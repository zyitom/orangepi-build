/*
 * MSGBOX transport to the ARM side.
 *
 * Two separate one-way FIFO blocks (manual 6.1.6):
 *   MBOX_CPUX 0x03004000  CPUS writes, CPUX reads  -> we TX here
 *   MBOX_CPUS 0x07094000  CPUX writes, CPUS reads  -> we RX here
 *
 * 4 channels each, 8 x 32-bit deep. A write to MSG_REG raises the peer's
 * interrupt automatically; no doorbell register to poke.
 *
 * CHANNEL NUMBER -- CHANGED TO 3 (2026-09-22)
 * -------------------------------------------
 * This firmware originally used channel 0. Disassembling the vendor scp.fex
 * showed the real A733 convention is channel 3:
 *
 *   TX: MBOX_MSG(CPUX_MSGBOX,3)        = 0x0300407C  (scp.fex 0x40007250)
 *   TX status: MBOX_MSG_STATUS(..., 3) = 0x0300406C  (scp.fex 0x400071b6-1c0)
 *   RX: MBOX_MSG(MBOX_CPUS,3)          = 0x0709407C  (scp.fex 0x40007278)
 *   RX status: MBOX_MSG_STATUS(..., 3) = 0x0709406C  (scp.fex 0x400071ee-f8)
 *
 * Vendor wire format for a packet (scp.fex 0x4000724a onwards):
 *   [u32 header] [u32 word_count] [word_count x u32 data]
 * and the boot handshake bl31 waits for is a "startup feedback" packet with
 * header 0 and count 13 (scp.fex 0x4000b1d2..0x4000b202).
 *
 * RX is polled from the main loop (msgbox_poll_rx), like the vendor's main
 * loop at scp.fex 0x4000abe0. There is deliberately no RX interrupt handler:
 * a second consumer would race the packet reassembler below.
 *
 * bl31 is the only ARM-side user of channel 3 and has NO timeouts anywhere
 * on this path (monitor.fex 0x1bf4): it spins while our RX FIFO is full,
 * and for a synchronous request (flags bit1) it spins until reply words
 * appear in the CPUX FIFO, then copies `count` words into its stack buffer.
 * Hence the rules this file and handle_arm_packet() keep:
 *   - drain RX continuously, never block the main loop for long;
 *   - answer every synchronous request, with the same count, promptly;
 *   - never put an unsolicited word into the CPUX FIFO after the boot
 *     handshake (bl31 would take it as the start of its next reply).
 */
#include "fw.h"

#define MBOX_CH		3	/* vendor convention, see above */
#define MBOX_CH_LEGACY	0	/* the old firmware's channel, kept for reference */

/* Packet reassembly (vendor wire format on ch3: [hdr][count][data...]).
 * bl31 sends a query whose data[0] low byte is 0x90 and waits for the SCP to
 * echo the packet back with the result byte (hdr byte3) = 0 -- that echo is
 * what arm_svc_arisc_wait_ready() consumes (monitor.fex 0xb054..0xb128). */
volatile u32 mb_pkt_hdr;            /* header of last complete packet */
volatile u32 mb_pkt_data[16];       /* data words of last complete packet */
volatile unsigned int mb_pkt_words; /* data word count of last complete packet */
volatile int mb_pkt_ready;          /* a complete packet is available */
volatile unsigned int mb_pkt_count; /* number of complete packets received */

/* raw word-level stats (kept for the heartbeat block) */
volatile u32 mb_last_rx;
volatile unsigned int mb_rx_count;
volatile int mb_rx_flag;
volatile unsigned int mb_irq_count;

static volatile u32 pkt_hdr;
static volatile u32 pkt_data[16];
static volatile unsigned int pkt_state;   /* 0=hdr 1=count 2=data */
static volatile unsigned int pkt_need, pkt_got;

static void finish_pkt(void)
{
	unsigned int i;

	mb_pkt_hdr = pkt_hdr;
	mb_pkt_words = pkt_need;
	for (i = 0; i < pkt_need && i < 16; i++)
		mb_pkt_data[i] = pkt_data[i];
	mb_pkt_ready = 1;
	mb_pkt_count++;
	pkt_state = 0;
	pkt_got = 0;
}

/* words drained by msgbox_init() -- reported by main() once the UART is up
 * (msgbox_init runs before uart_init, so it must not print) */
unsigned int mb_stale_drained;

void msgbox_init(void)
{
	unsigned int stale = 0;

	/* v62 (T28): the RX interrupt path has NEVER delivered a packet in
	 * the field (no "[pkt]" line ever captured on any boot), and bl31's
	 * shutdown sender spins forever on MSG_STATUS == 8 (FIFO full) with
	 * no timeout -- an unconsumed backlog wedges the whole poweroff/
	 * reset flow. The vendor firmware does not use an RX interrupt
	 * either: its main loop (scp.fex 0x4000abe0) polls. Do the same:
	 * IRQ stays off, the main loop calls msgbox_poll_rx(). */

	/*
	 * v64 (T30): reset BOTH message-box blocks first, like the vendor
	 * (scp.fex 0x40005e56/0x40005e80: bit16 of CCU 0x02002744 and R-CCU
	 * 0x0701017C asserted together, then released). Nothing else ever
	 * empties the E902->ARM FIFO across a warm reboot: measured on the
	 * board, it held the previous boots' ready packet and 0x26 reply, so
	 * bl31's wait_ready matched a STALE ready packet and its 0x26 RPC took
	 * a stale reply, leaving the fresh ones queued one round behind. The
	 * ARM side cannot be drained by us (only the ARM reads it), so reset is
	 * the only way. bl31 is only polling MSG_STATUS at this point (waiting
	 * for our feedback) and Linux has not started, so nobody is mid-packet.
	 */
	{
		volatile unsigned int d;

		writel(readl(CCU_MSGBOX_BGR_REG) & ~(1u << 16), CCU_MSGBOX_BGR_REG);
		writel(readl(S_MBOX_BGR_REG) & ~(1u << 16), S_MBOX_BGR_REG);
		for (d = 0; d < 100; d++)
			;
		writel(readl(CCU_MSGBOX_BGR_REG) | (1u << 16) | 1u,
		       CCU_MSGBOX_BGR_REG);
		writel(readl(S_MBOX_BGR_REG) | (1u << 16) | 1u, S_MBOX_BGR_REG);
	}

	/* make sure the RX interrupt is really off, on both sides */
	writel(0, MBOX_RD_IRQ_EN(MBOX_CPUS_BASE));
	writel(1u << (MBOX_CH * 2), MBOX_RD_IRQ_STAT(MBOX_CPUS_BASE));
	clic_disable(IRQ_CPUX_MBOX_W_R);
	clic_disable(IRQ_CPUS_MBOX_READ);

	/* drain anything stale left by SPL/U-Boot/bl31 (or a previous
	 * wedged shutdown cycle) so the FIFO can never start out full */
	while (msgbox_rx_pending() && stale < 32) {
		u32 m = msgbox_recv();

		mb_rx_count++;
		mb_last_rx = m;
		stale++;
	}
	mb_stale_drained += stale;
}

/* Poll-driven receive: consume every pending word, reassemble packets
 * (wire format [hdr][count][data...]) and hand complete packets to the
 * main loop via handle_arm_packet(). Safe to call from the main loop only
 * (single consumer, like the vendor). Returns number of complete packets. */
/* framing resync (v67, T31): bl31 pushes a packet's words back to back, so
 * a packet that stays incomplete with nothing more arriving for >= 2 ticks
 * (100-200 ms) can only be a stray/partial word; dropping it re-aligns the
 * reassembler instead of mis-splitting every later packet. Seen on the
 * board when a Linux /dev/mem test wrote the FIFO byte-wise. */
static unsigned int last_word_tick;
unsigned int mb_resyncs;

unsigned int msgbox_poll_rx(void)
{
	unsigned int packets = 0;

	if (pkt_state != 0 && !msgbox_rx_pending() &&
	    timer_ticks() - last_word_tick >= 2) {
		pkt_state = 0;
		pkt_got = 0;
		mb_resyncs++;
	}

	while (msgbox_rx_pending()) {
		u32 m = msgbox_recv();

		last_word_tick = timer_ticks();

		mb_rx_count++;
		mb_last_rx = m;
		switch (pkt_state) {
		case 0:
			pkt_hdr = m;
			pkt_state = 1;
			break;
		case 1: {
			u32 n = m & 0xFFu;

			if (n > 16) {           /* bogus length: resync */
				pkt_state = 0;
				break;
			}
			pkt_need = n;
			pkt_got = 0;
			pkt_state = 2;
			if (n == 0)
				finish_pkt();
			break;
		}
		default:
			if (pkt_got < 16)
				pkt_data[pkt_got] = m;
			pkt_got++;
			if (pkt_got >= pkt_need)
				finish_pkt();
			break;
		}
		while (mb_pkt_ready) {
			handle_arm_packet(mb_pkt_hdr,
					  (const u32 *)mb_pkt_data,
					  mb_pkt_words);
			mb_pkt_ready = 0;
			packets++;
		}
	}
	return packets;
}

/* Non-blocking send. Returns 0 on success, -1 if the FIFO is full. */
int msgbox_try_send(u32 msg)
{
	if (readl(MBOX_FIFO_STATUS(MBOX_CPUX_BASE, MBOX_CH)) & MBOX_FIFO_FULL)
		return -1;

	writel(msg, MBOX_MSG(MBOX_CPUX_BASE, MBOX_CH));
	return 0;
}

/* Blocking send, bounded so a dead peer cannot wedge us forever. */
int msgbox_send(u32 msg)
{
	int spin = 100000;

	while (spin--) {
		if (msgbox_try_send(msg) == 0)
			return 0;
	}
	return -1;
}

/*
 * Wait until the TX FIFO has a free slot.  The vendor helper at scp.fex
 * 0x400071b2 proceeds as soon as MSG_STATUS != 8 (8 = FIFO full, manual
 * 6.1: MSG_NUM bits[3:0], 8 messages deep) and only spins while it IS full.
 * v2 waited for "completely empty" instead, which deadlocks on stale
 * messages left in the FIFO by a previous reset cycle (T23, banner printed
 * "startup feedback : FAILED" on every cycle >= 2).
 */
int msgbox_wait_tx_empty(int spin)
{
	while (spin-- > 0) {
		if (readl(MBOX_MSG_STATUS(MBOX_CPUX_BASE, MBOX_CH)) != 8)
			return 0;
	}
	return -1;
}

/*
 * Vendor-compatible packet send: [u32 hdr][u32 count][count x u32 data].
 * Matches the sequence at scp.fex 0x4000724a..0x4000733c.
 */
int msgbox_send_packet(u32 hdr, const u32 *data, unsigned int count)
{
	unsigned int i;

	/*
	 * Pacing: the vendor sender (scp.fex 0x40007240/0x4000724e/0x400072aa)
	 * calls its "not full" wait BEFORE EVERY SINGLE WORD, not once at the
	 * start. A packet can be longer than the 8-deep FIFO (the startup
	 * feedback is 15 words: hdr + count + 13 data), so pacing per word is
	 * mandatory -- writing 15 words back-to-back overruns the FIFO and
	 * msgbox_try_send() starts returning -1 halfway through.
	 */
	if (msgbox_wait_tx_empty(100000) != 0)
		return -1;
	if (msgbox_try_send(hdr) != 0)
		return -1;
	if (msgbox_wait_tx_empty(100000) != 0)
		return -1;
	if (msgbox_try_send(count) != 0)
		return -1;
	for (i = 0; i < count; i++) {
		if (msgbox_wait_tx_empty(100000) != 0)
			return -1;
		if (msgbox_try_send(data ? data[i] : 0u) != 0)
			return -1;
	}
	return 0;
}

/*
 * The boot handshake: the vendor firmware sends a "startup feedback" packet
 * before entering its main loop, and bl31's arm_svc_arisc_wait_ready() is what
 * consumes it.
 *
 * scp.fex 0x4000b1b0..0x4000b202 builds the packet like this:
 *     memset(data_buf, 0, 52)
 *     pkt[1]      = 2              (0x4000b1d6)  -- sender only transmits when bit1 is set
 *     pkt[2..3]   = 0x0090         (0x4000b1de)  -- message id 0x90
 *     pkt[4]      = 13             (0x4000b1f2)  -- word count
 *     pkt[0x1C]   = data_buf       (0x4000b1d2)
 * and the sender (0x40007248) puts *(u32*)pkt on the wire first, i.e. the
 * little-endian bytes [b0=0][b1=2][b2=0x90][b3=0], which is 0x00900200.
 *
 * NOTE (T22, 2026-09-22): the earlier "hdr=0 was rejected by bl31" note was a
 * misdiagnosis -- those boot failures were boot0 rejecting the boot package
 * add_sum checksum, before bl31 ever ran. The header value has therefore never
 * been hardware-validated; 0x00900200 is kept because it is byte-identical to
 * what the vendor firmware transmits (see fix-bootpkg-sum.py for the actual
 * root cause and its 4-byte fix).
 */
#define MBOX_FEEDBACK_HDR	0x00900200u	/* [0]=0, [1]=2, [2]=0x90, [3]=0 */
#define MBOX_FEEDBACK_WORDS	13u

int msgbox_send_startup_feedback(void)
{
	u32 payload[13];
	unsigned int i;

	for (i = 0; i < 13; i++)
		payload[i] = 0;

	return msgbox_send_packet(MBOX_FEEDBACK_HDR, payload, MBOX_FEEDBACK_WORDS);
}

/* How many messages are waiting for us. */
unsigned int msgbox_rx_pending(void)
{
	return readl(MBOX_MSG_STATUS(MBOX_CPUS_BASE, MBOX_CH))
		& MBOX_MSG_NUM_MASK;
}

/* Pop one message. Only valid when msgbox_rx_pending() > 0. */
u32 msgbox_recv(void)
{
	return readl(MBOX_MSG(MBOX_CPUS_BASE, MBOX_CH));
}

void msgbox_ack_irq(void)
{
	writel(1u << (MBOX_CH * 2), MBOX_RD_IRQ_STAT(MBOX_CPUS_BASE));
}

unsigned int msgbox_irq_count(void)
{
	return mb_irq_count;
}
