/*
 * A733 E902 firmware -- v2 (SPI + interrupt + SRAM heartbeat).
 *
 * What it does:
 *   - writes a status block to SRAM 0x4001E000 that the ARM can read via
 *     /dev/mem (see heartbeat.c) -- this is the primary bring-up observable,
 *     because /dev/ttyUSB0 on the build host is the *Linux* console, not the
 *     E902's S_UART0
 *   - brings up S_UART0, prints a banner, echoes input, and (new) routes
 *     received bytes through a real CLIC interrupt on IRQ 29
 *   - brings up S_SPI (r_spi @ 0x07092000, currently status="disabled" in
 *     Linux's device tree, so nobody else owns it) and can clock out a fixed
 *     pattern, reporting what came back
 *   - receives vendor-format packets from bl31 over MSGBOX channel 3
 *     (polled, see msgbox.c) and answers them the way the vendor SCP does
 *   - carries out PSCI SYSTEM_OFF / SYSTEM_RESET for bl31 (power.c)
 *
 * v63 robustness rules (see FINDINGS-LEDGER T29):
 *   - nothing on the main loop may block for long: the mailbox is polled
 *     every pass, UART output is buffered (uart_tx_pump), sync requests are
 *     answered BEFORE anything is logged;
 *   - no unsolicited word ever goes to the ARM after the boot handshake;
 *   - an exception is logged and survived (trap_handler), not parked on;
 *   - interrupts are an optimisation only: the periodic tick has a polled
 *     fallback, UART RX and the mailbox are polled.
 */
#include "fw.h"
#include "spi.h"

/* vendor packet header bytes: [b0 attr][b1 flags][b2 type][b3 result] */
#define PKT_TYPE(h)		(((h) >> 16) & 0xFFu)
#define PKT_WANTS_REPLY(h)	((((h) >> 8) & 3u) != 0u)
#define PKT_ACK_HDR		0x00900200u	/* bl31's ack of our startup feedback */
#define PKT_TYPE_SYSOP		0x24u		/* PSCI SYSTEM_OFF/RESET */
#define PKT_RESULT_EINVAL	0xEAu		/* vendor's -22 */

static u8 spi_rx[SPI_MAX_BURST];
static unsigned int spi_rx_len;
static u32 spi_rx_sum;

static void spi_do_read(unsigned int len)
{
	unsigned int i;

	if (len == 0 || len > SPI_MAX_BURST)
		len = 8;

	if (spi_selftest(spi_rx, &spi_rx_len) < 0) {
		spi_rx_len = 0;
		spi_rx_sum = 0xDEADu;
		return;
	}
	spi_rx_sum = 0;
	for (i = 0; i < SPI_MAX_BURST; i++)
		spi_rx_sum ^= ((u32)spi_rx[i] << ((i & 3) * 8));
}

static unsigned int rpc_replies;
static unsigned int bench_replies;
static int tick_print = 1;

/* vendor message ids start at MESSAGE_BASE 0x10 (lichee/arisc messages.h);
 * lower types never come from bl31 and are used for loopback benchmarks
 * (tests/board/xcore_bench.c): answered without a console line, because
 * formatting ~60 characters per packet cost ~13 us of the ~17 us RTT */
#define PKT_TYPE_VENDOR_MIN	0x10u
static unsigned int rpc_reply_fail;

static void log_packet(u32 hdr, const u32 *data, unsigned int count,
		       const char *what)
{
	uart_puts("[pkt] hdr=");
	uart_put_hex(hdr);
	uart_puts(" count=");
	uart_put_dec(count);
	if (count > 0) {
		uart_puts(" d0=");
		uart_put_hex(data[0]);
	}
	uart_puts(" -> ");
	uart_puts(what);
	uart_puts("\n");
}

/*
 * One complete packet from bl31 (called from msgbox_poll_rx).
 *
 * Reply rule from the vendor dispatcher (scp.fex 0x4000acd8): echo the
 * packet with only the result byte (hdr byte3) changed, and only when the
 * sender asked for a reply (flags & 3 != 0). bl31's synchronous sender
 * (monitor.fex 0x1bf4) spins in EL3 with no timeout until that echo arrives,
 * so the echo goes out FIRST and the console line is queued afterwards.
 * Synchronous requests bl31 can send: 0x11 (suspend-finish), 0x25, 0x26 set
 * wakeup src, 0x60 set debug level, 0x62 set dram crc paras. None needs
 * real work from us to keep the system running; result 0 (OK) keeps bl31
 * on its normal path. Async ones (0x22 cpu op, 0x61 baud, 0x64, 0x96) are
 * only logged.
 */
void handle_arm_packet(u32 hdr, const u32 *data, unsigned int count)
{
	u32 copy[16];
	unsigned int i;

	if (count > 16)
		count = 16;
	for (i = 0; i < count; i++)
		copy[i] = data[i];

	if ((hdr & 0x00FFFFFFu) == PKT_ACK_HDR) {
		/* bl31's ACK to our startup feedback -- never echo a
		 * reply back to a reply (T26). */
		log_packet(hdr, copy, count, "ack, no echo");
		return;
	}

	if (PKT_TYPE(hdr) == PKT_TYPE_SYSOP) {
		/* PSCI SYSTEM_OFF / SYSTEM_RESET from bl31 (sender reversed
		 * at monitor.fex 0x2158): {attr=2, flags=0, type=0x24,
		 * result, count=1, payload[0]=op}, op 0 = off, 1 = reset.
		 * bl31 has already gone into wfi; ops 0..3 never return.
		 * From here on nothing else runs, so the console may block. */
		u32 op = (count >= 1) ? copy[0] : 0xFFFFFFFFu;

		uart_set_blocking(1);
		log_packet(hdr, copy, count, "sys-op");
		hb_stage(0x50);
		if (power_sys_op(op) != 0) {
			/* unknown op: vendor returns -22 and echoes it back
			 * when asked to */
			if (PKT_WANTS_REPLY(hdr))
				msgbox_send_packet((hdr & 0x00FFFFFFu) |
						   (PKT_RESULT_EINVAL << 24),
						   copy, count);
			hb_stage(0x51);
			uart_puts("[pkt] sys-op: unknown op, ignored\n");
		}
		uart_set_blocking(0);
		return;
	}

	if (!PKT_WANTS_REPLY(hdr)) {
		log_packet(hdr, copy, count, "async, no reply");
		return;
	}

	/* result byte 0 = OK; same count, same data (bl31 copies `count`
	 * words back into a buffer sized for its own request) */
	if (PKT_TYPE(hdr) < PKT_TYPE_VENDOR_MIN) {
		if (msgbox_send_packet(hdr & 0x00FFFFFFu, copy, count) == 0)
			bench_replies++;
		else
			rpc_reply_fail++;
		return;
	}
	if (msgbox_send_packet(hdr & 0x00FFFFFFu, copy, count) == 0) {
		rpc_replies++;
		log_packet(hdr, copy, count, "replied");
	} else {
		rpc_reply_fail++;
		log_packet(hdr, copy, count, "REPLY FAILED");
	}
}

/*
 * Synchronous exception handler, called from start.S trap_entry with the
 * caller-saved registers already preserved. Returns the address to resume.
 *
 * Before v63 any exception parked the core for good, and a parked SCP
 * freezes the whole board the next time bl31 talks to it. Now: log it in
 * the heartbeat block and step over the faulting instruction. If the same
 * PC keeps faulting, or the fault is on the instruction fetch itself (so
 * there is no instruction to step over), restart the main loop instead.
 */
extern u32 last_mcause, last_mepc;
static void __attribute__((noreturn)) main_loop(void);

static volatile unsigned int trap_count;

u32 trap_handler(u32 mcause, u32 mepc)
{
	static u32 prev_pc;
	static unsigned int same_pc;
	u32 code = mcause & 0xFFFu;

	if (mcause & 0x80000000u) {
		/* an interrupt without shv: only happens if a source was
		 * enabled non-vectored by mistake -- mask it */
		if (code < 256)
			writeb(0, CLIC_INTIE(code));
		hb_set(HB_SPURIOUS, (code << 16) | 0xFFFFu);
		return mepc;
	}

	trap_count++;
	last_mcause = mcause;
	last_mepc = mepc;
	hb_set(HB_TRAPS, trap_count);
	hb_set(HB_LAST_MCAUSE, mcause);
	hb_set(HB_LAST_MEPC, mepc);

	same_pc = (mepc == prev_pc) ? same_pc + 1 : 0;
	prev_pc = mepc;

	/* 0 = fetch misaligned, 1 = fetch access fault: mepc itself is
	 * not readable/executable, there is nothing to step over */
	if (code <= 1 || same_pc >= 16) {
		same_pc = 0;
		return (u32)main_loop;
	}
	/* 16-bit (C extension) if the low two bits are not 0b11 */
	return mepc + (((*(volatile unsigned short *)mepc) & 3u) == 3u ? 4u : 2u);
}

static void publish_diag(unsigned int t)
{
	hb_set(HB_SPI_IRQ, spi_irq_count());
	hb_set(HB_SPI_TXCNT, spi_tc_count());
	hb_set(HB_UART_RX, uart_rx_count());
	hb_set(HB_MBOX_IRQ, mb_pkt_count);
	hb_set(HB_MBOX_RXCOUNT, mb_rx_count);
	hb_set(HB_TIMER_TICKS, t);
	hb_set(HB_TICK_SRC, timer_tick_sources());
	hb_set(HB_CLIC_DIAG,
	       ((u32)readb(CLIC_INTIP(IRQ_S_TIMER1)) << 24) |
	       ((u32)readb(CLIC_INTIE(IRQ_S_TIMER1)) << 16) |
	       (readl(TMR_IRQ_STA_REG) & 0xFFFFu));
	hb_set(HB_IRQ_LAT, timer_irq_latency());
	hb_set(HB_IRQ_LAT_MIN, timer_irq_latency_min());
	hb_set(HB_RPC, ((mb_pkt_count & 0xFFFFu) << 16) |
		       (rpc_replies & 0xFFFFu));
}

static void print_status(void)
{
	u32 src = timer_tick_sources();

	uart_puts("[status] ticks=");
	uart_put_dec(timer_ticks());
	uart_puts(" (irq ");
	uart_put_dec(src >> 16);
	uart_puts(", polled ");
	uart_put_dec(src & 0xFFFFu);
	uart_puts(")\n[status] mbox words=");
	uart_put_dec(mb_rx_count);
	uart_puts(" pkts=");
	uart_put_dec(mb_pkt_count);
	uart_puts(" replies=");
	uart_put_dec(rpc_replies);
	uart_puts(" reply-fail=");
	uart_put_dec(rpc_reply_fail);
	uart_puts(" resyncs=");
	uart_put_dec(mb_resyncs);
	uart_puts(" bench=");
	uart_put_dec(bench_replies);
	uart_puts("\n[status] traps=");
	uart_put_dec(trap_count);
	uart_puts(" last mcause=");
	uart_put_hex(last_mcause);
	uart_puts(" mepc=");
	uart_put_hex(last_mepc);
	uart_puts(" spurious=");
	uart_put_hex(hb_get(HB_SPURIOUS));
	uart_puts(" uart-drop=");
	uart_put_dec(uart_tx_dropped());
	uart_puts("\n");
}


/*
 * 'B' key: E902-side speed test, timed with the shared 24 MHz timestamp
 * (0x08010000, the same counter Linux reads). Blocks the main loop for a
 * few ms -- idle system only.
 */
#define TS_LO	0x08010000u
#define TS_HI	0x08010004u

static u32 ts_lo(void)
{
	return readl(TS_LO);
}

static void put_rate(const char *what, unsigned int bytes, u32 ticks)
{
	/* 32-bit only (no libgcc): bytes <= 4 MB, us = ticks / 24 */
	unsigned int us = ticks / 24u;
	unsigned int bytes_per_ms = us ? bytes * 1000u / us : 0;

	uart_puts(what);
	uart_put_dec(bytes_per_ms * 1000u / 1024u);
	uart_puts(" KB/s\n");
}

static void run_bench(void)
{
	static u32 buf_a[1024];
	static volatile u32 buf_b[1024];	/* 4 KB each, SRAM */
	volatile u32 sink = 0;
	unsigned int i, k;
	u32 t0, t1, h;

	uart_set_blocking(1);
	uart_puts("[bench] E902 side, timed by the shared 24 MHz timestamp\n");

	t0 = ts_lo();
	for (i = 0; i < 10000; i++) {
		do { h = readl(TS_HI); sink += readl(TS_LO); } while (readl(TS_HI) != h);
	}
	t1 = ts_lo();
	uart_puts("[bench] timestamp read (64-bit): ");
	uart_put_dec((t1 - t0) * 1000u / 24u / 10000u);
	uart_puts(" ns\n");

	t0 = ts_lo();
	for (i = 0; i < 10000; i++)
		sink += readl(TMR_CUR_VALUE_REG(1));
	t1 = ts_lo();
	uart_puts("[bench] CPUS peripheral read (S_TIMER): ");
	uart_put_dec((t1 - t0) * 1000u / 24u / 10000u);
	uart_puts(" ns\n");

	t0 = ts_lo();
	for (k = 0; k < 64; k++)
		for (i = 0; i < 1024; i++)
			buf_b[i] = buf_a[i] + k;
	t1 = ts_lo();
	sink += buf_b[1023];
	put_rate("[bench] SRAM copy (32-bit loop): ", 64u * 4096u, t1 - t0);

	t0 = ts_lo();
	for (k = 0; k < 16; k++)
		for (i = 0; i < 4096; i++)
			sink += *(volatile u32 *)(0x80000000u + 4u * i);
	t1 = ts_lo();
	put_rate("[bench] DRAM read via 0x80000000 window: ", 16u * 16384u, t1 - t0);

	uart_puts("[bench] done (sink ");
	uart_put_hex(sink);
	uart_puts(")\n");
	uart_set_blocking(0);
}

static void handle_key(int c)
{
	switch (c) {
	case 's':
		/* v63: used to push a raw single word to the ARM -- that
		 * breaks bl31's packet framing exactly like the old HELLO
		 * (T26). Now prints local status only. */
		print_status();
		break;
	case 'r':
		uart_puts("[rx count] ");
		uart_put_dec(mb_rx_count);
		uart_puts(" words (uart rx bytes ");
		uart_put_dec(uart_rx_count());
		uart_puts(")\n");
		break;
	case 't':
		uart_puts("[ticks] ");
		uart_put_dec(timer_ticks());
		uart_puts(" (x100ms)\n");
		break;
	case 'p':
		spi_do_read(8);
		uart_puts("[spi] ");
		uart_put_dec(spi_rx_len);
		uart_puts(" bytes sum=");
		uart_put_hex(spi_rx_sum);
		uart_puts("\n");
		break;
	case 'T':
		/* 1 s busy delay: the mailbox is not serviced meanwhile */
		uart_puts("[ts] timestamp probe, mailbox paused ~1 s\n");
		uart_tx_flush();
		ts_test_run();
		break;
	case 'B':
		run_bench();
		break;
	case 'G':
		/* GINTC routing sweep: input 70 (GPADC). Needs
		 * 'modprobe sunxi_gpadc' on the host for a live storm. */
		gintc_sweep();
		break;
	case 'g':
		gintc_off();
		break;
	case 'U':
		/* UART0 RX (input 34) routing test, edge-triggered */
		gintc_uart_toggle();
		break;
	case 'L':
		/* LRADC (input 69) routing test, edge-triggered */
		gintc_lradc_toggle();
		break;
	case 'I':
		gintc_report();
		break;
	case 'X': {
		/* CPUX SPI3 on the 40-pin header (24 CS0, 23 CLK, 19 MOSI,
		 * 21 MISO), driven cross-domain; jumper 19<->21 to loop back */
		u8 rx[8];
		unsigned int n = 0, i;
		int r = cx_spi3_init(1000000);

		uart_puts("[spi3] init ");
		uart_puts(r == 0 ? "ok" : "FAILED");
		uart_puts(", 1 MHz, 40-pin 24/23/19/21\n");
		spi_dump_regs();
		r = spi_selftest(rx, &n);
		uart_puts("[spi3] transfer ");
		uart_puts(r == 0 ? "complete" : "TIMEOUT");
		uart_puts(", rx:");
		for (i = 0; i < 8; i++) {
			uart_putc(' ');
			uart_put_hex(rx[i]);
		}
		uart_puts("\n[spi3] tx was a5 5a 01 fe 00 ff 3c c3 "
			  "(equal only with MOSI-MISO jumpered)\n");
		spi_use(R_SPI_BASE);		/* 'p' keeps using S_SPI */
		break;
	}
	case 'h':
		tick_print = !tick_print;
		uart_puts(tick_print ? "[tick print on]\n"
				     : "[tick print off]\n");
		break;
	case '\r':
	case '\n':
		break;
	default:
		uart_putc((char)c);
		break;
	}
	hb_set(HB_UART_RX, uart_rx_count());
}

/*
 * The main loop. Every pass services, in order of urgency: the mailbox
 * (bl31 may be spinning on us), the periodic tick, console RX, console TX.
 * Also the re-entry point trap_handler() uses after an unrecoverable fault,
 * so all its state is static.
 */
static void __attribute__((noreturn)) main_loop(void)
{
	static unsigned int last_reported;
	static unsigned int last_traps;
	int c;

	hb_stage(4);
	for (;;) {
		hb_tick();
		msgbox_poll_rx();
		timer_poll();
		uart_rx_poll();

		{
			unsigned int t = timer_ticks();

			/* diagnostics also refresh without ticks, so a dead
			 * timer is visible from Linux (hb seq ~1M/s) */
			if ((hb_get(HB_SEQ) & 0xFFFFFu) == 0)
				publish_diag(t);
			if (t - last_reported >= 10) {	/* 10 x 100 ms */
				last_reported = t;
				publish_diag(t);
				if (tick_print) {
					uart_puts("[tick] ");
					uart_put_dec(t);
					uart_puts("\n");
				}
			}
		}
		if (trap_count != last_traps) {
			last_traps = trap_count;
			uart_puts("[trap] #");
			uart_put_dec(trap_count);
			uart_puts(" mcause=");
			uart_put_hex(last_mcause);
			uart_puts(" mepc=");
			uart_put_hex(last_mepc);
			uart_puts(" (survived)\n");
		}

		while ((c = uart_rx_pop()) >= 0)
			handle_key(c);

		uart_tx_pump();
	}
}

int main(void)
{
	int mux_ok, spi_ok, hs_ok;

	/* First thing: publish the magic. If the ARM can read it, the core is
	 * executing our code even when nothing else works. */
	hb_init();
	hb_stage(1);

	/*
	 * FAIL-SAFE ORDERING -- read this before moving anything above it.
	 *
	 * bl31 loads this image, sets E902_RST_START_ADDR, releases reset and
	 * then waits in arm_svc_arisc_wait_ready() for the little core to
	 * report readiness before it carries on booting Linux. So the
	 * handshake goes FIRST; msgbox_init() must not touch the UART (it is
	 * not initialised yet) and only drains stale RX words.
	 *
	 * WDT service stays removed (v5.1 canary proved it freezes the core --
	 * WDT_CPUS bus clock is gated at cold start).
	 */
	msgbox_init();
	hs_ok = msgbox_send_startup_feedback();

	/* Mux PL2/PL3 to S_UART0 before touching the UART. */
	mux_ok = pinmux_s_uart0();
	hb_stage(2);

	/* Console is synchronous (bounded) during init so a hang here still
	 * leaves its last line on the wire; buffered once the loop runs. */
	uart_set_blocking(1);
	uart_init(115200);
	hb_set(HB_PL_CFG0, pinmux_read_pl_cfg0());
	hb_set(HB_APBS1, apbs1_rate());
	hb_set(HB_RISCV_BGR, readl(RISCV_BGR_REG));
	hb_set(HB_RST_START, readl(E902_RST_START_ADDR_REG));

	uart_puts("\n\n=== A733 E902 firmware v6.9 ===\n");
	uart_puts("PL_CFG0     = ");
	uart_put_hex(pinmux_read_pl_cfg0());
	uart_puts(mux_ok == 0 ? "  (PL2/PL3 mux ok)\n"
			      : "  (PL2/PL3 mux MISMATCH!)\n");
	uart_puts("APBS1 (S_UART clock) = ");
	uart_put_dec(apbs1_rate());
	uart_puts(" Hz\n");
	uart_puts("RISCV_BGR   = ");
	uart_put_hex(readl(RISCV_BGR_REG));
	uart_puts("\nRST_START   = ");
	uart_put_hex(readl(E902_RST_START_ADDR_REG));
	uart_puts("\nstartup feedback (ch3 hdr=0x00900200 13w) : ");
	uart_puts(hs_ok == 0 ? "sent\n" : "FAILED\n");
	if (mb_stale_drained) {
		uart_puts("[mbox] drained stale rx words: ");
		uart_put_dec(mb_stale_drained);
		uart_puts("\n");
	}

	/* S_SPI: the CPUS-domain controller, free because Linux has the node
	 * disabled. 500 kHz is slow and safe for bench wiring. Polled. */
	spi_ok = spi_init(500000);
	hb_set(HB_SPI_INIT, (spi_ok == 0) ? 0u : 0xFFFFFFFFu);
	uart_puts("spi_init(500kHz) = ");
	uart_puts(spi_ok == 0 ? "ok\n" : "FAILED (controller not enabled)\n");
	spi_dump_regs();
	spi_do_read(8);
	hb_set(HB_SPI_TXCNT, spi_tc_count());
	hb_set(HB_SPI_RXCNT, spi_rx_len);
	hb_set(HB_SPI_RX0, ((u32)spi_rx[0] << 24) | ((u32)spi_rx[1] << 16) |
			   ((u32)spi_rx[2] << 8) | spi_rx[3]);
	hb_set(HB_SPI_RX1, ((u32)spi_rx[4] << 24) | ((u32)spi_rx[5] << 16) |
			   ((u32)spi_rx[6] << 8) | spi_rx[7]);

	hb_stage(3);
	uart_puts("timer irq 21 (100ms, polled fallback) after 24m; mbox/uart/spi polled.\n");
	uart_puts("keys: s=status r=rx count t=ticks p=spi selftest T=ts probe B=bench X=spi3(40pin) h=tick print\n");
	uart_set_blocking(0);

	/*
	 * No separate "ready" packet (removed in v64, T30). bl31's
	 * wait_ready (monitor.fex 0xb054..0xb068) receives into a struct at
	 * sp+0x38 and compares `ldrb [sp,#58]` -- byte 2 of the HEADER, the
	 * type -- with 0x90. The startup feedback (hdr 0x00900200, type 0x90)
	 * already satisfies it, exactly as with the vendor firmware. The old
	 * T26 ready packet (0x00900200, 1, 0x90) was left in the FIFO and
	 * every later synchronous RPC then read the previous packet as its
	 * reply (measured: our 0x26 reply stayed queued after every boot).
	 * bl31 continues as soon as the feedback is consumed, so everything
	 * below must be quick before the main loop starts.
	 */

	/* CPUS 24M broadcast (clocks.c): S_TIMER/S_SPI need it. It runs
	 * right after the handshake completes, long before Linux's
	 * rtc_ccu driver can touch the same RTC-block registers. */
	{
		int clk_ok;

		hb_set(HB_RST_START, 0xC0u);
		clk_ok = cpus_24m_broadcast_on();
		uart_puts("[clk] 24m broadcast ");
		uart_puts(clk_ok == 0 ? "on\n" : "FAILED\n");
	}

	/* 100 ms periodic tick on CLIC irq 21 (S_TIMER1 at +0x40), started
	 * after the 24M broadcast so its RELOAD can complete. HB_TICK_SRC
	 * shows whether the ISR or the polled fallback counted the ticks
	 * (2026-09-23: ISR -- CLIC delivery works). UART RX (29), SPI (38)
	 * and the mailbox (48) stay polled. */
	timer_periodic_start(100);

	/* The timestamp probe (ts_test_run, 1 s busy delay) used to run here,
	 * i.e. exactly while bl31 might be waiting on its first reply. It is
	 * on the 'T' key now. */
	main_loop();
	return 0;
}
