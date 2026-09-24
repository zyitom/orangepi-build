/*
 * A733 E902 firmware -- first version.
 *
 * What it does:
 *   - brings up S_UART0 and prints a banner with the clocks it detected
 *   - echoes UART input back, so you can confirm RX works
 *   - receives 32-bit messages from ARM over MSGBOX and prints them
 *   - answers each ARM message (simple ping/echo protocol)
 *   - types 's' on the UART to push an unsolicited message to ARM
 *
 * Message format (ours to define; 32 bits is all the hardware carries):
 *   [31:24] command
 *   [23:0]  payload
 */
#include "fw.h"

#define CMD_PING	0x01	/* ARM -> us, we reply CMD_PONG          */
#define CMD_PONG	0x02	/* us -> ARM                             */
#define CMD_HELLO	0x10	/* us -> ARM, sent once at startup       */
#define CMD_UART_KEY	0x11	/* us -> ARM, payload = keypress         */
#define CMD_ECHO	0x20	/* ARM -> us, we echo the payload back   */

#define MSG(cmd, payload)	((((u32)(cmd)) << 24) | ((payload) & 0xFFFFFF))
#define MSG_CMD(m)		(((m) >> 24) & 0xFF)
#define MSG_PAYLOAD(m)		((m) & 0xFFFFFF)

/* Written by the ISR, read by the main loop. */
static volatile unsigned int rx_count;
static volatile u32 last_rx;
static volatile int rx_flag;

/*
 * MSGBOX RX interrupt handler (CLIC IRQ 48).
 *
 * Kept short: drain the FIFO, stash the last message, let the main loop do
 * the printing. CLIC hardware vectoring jumps straight here but does not
 * save any registers, so the interrupt attribute is required -- it emits the
 * save/restore prologue and returns with mret instead of ret.
 */
static void __attribute__((interrupt("machine"))) msgbox_isr(void)
{
	while (msgbox_rx_pending()) {
		last_rx = msgbox_recv();
		rx_count++;
		rx_flag = 1;
	}
	msgbox_ack_irq();
}

static void handle_arm_message(u32 msg)
{
	uart_puts("[rx] ");
	uart_put_hex(msg);
	uart_puts("  cmd=");
	uart_put_hex(MSG_CMD(msg));

	switch (MSG_CMD(msg)) {
	case CMD_PING:
		uart_puts(" (ping) -> pong\n");
		msgbox_send(MSG(CMD_PONG, MSG_PAYLOAD(msg)));
		break;
	case CMD_ECHO:
		uart_puts(" (echo) -> back\n");
		msgbox_send(MSG(CMD_ECHO, MSG_PAYLOAD(msg)));
		break;
	default:
		uart_puts(" (unknown)\n");
		break;
	}
}

int main(void)
{
	unsigned int seq = 0;
	unsigned int last_reported = 0;
	int heartbeat = 1;
	int mux_ok;
	int c;

	/* Mux PL2/PL3 to S_UART0 before touching the UART. Nothing on the
	 * Linux side does this for us -- see the comment in pinmux.c. */
	mux_ok = pinmux_s_uart0();

	uart_init(115200);

	uart_puts("\n\n=== A733 E902 firmware ===\n");
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
	uart_puts("\nDDR_REMAP   = ");
	uart_put_hex(readl(E902_DDR_REMAP_REG));
	uart_puts("\nPAD_LPMD    = ");
	uart_put_hex(readl(E902_PAD_LPMD_REG));
	uart_puts("\n");

	msgbox_init();
	clic_set_handler(IRQ_CPUX_MBOX_W_R, msgbox_isr);
	clic_enable(IRQ_CPUX_MBOX_W_R, 0 /* level triggered */);

	/* 100 ms periodic tick on CLIC irq 21. Started before mstatus.MIE is
	 * set by start.S, so no need to mask around it. */
	timer_periodic_start(100);

	uart_puts("msgbox irq 48, timer irq 21 (100ms tick).\n");
	uart_puts("keys: s=send to ARM  r=rx count  t=ticks  h=heartbeat on/off\n");

	if (msgbox_send(MSG(CMD_HELLO, 0)) == 0)
		uart_puts("sent HELLO to ARM\n");
	else
		uart_puts("HELLO send FAILED (fifo full / clock gated?)\n");

	for (;;) {
		/* Report the tick every second, if the heartbeat is on. This is
		 * done here rather than in the ISR because uart_putc() busy-waits
		 * on the FIFO and would stretch the interrupt. */
		if (heartbeat) {
			unsigned int t = timer_ticks();

			if (t - last_reported >= 10) {	/* 10 x 100 ms */
				last_reported = t;
				uart_puts("[tick] ");
				uart_put_dec(t);
				uart_puts("\n");
			}
		}

		if (rx_flag) {
			u32 m = last_rx;

			rx_flag = 0;
			handle_arm_message(m);
		}

		c = uart_getc_nb();
		if (c >= 0) {
			if (c == 's') {
				seq++;
				uart_puts("[tx] seq=");
				uart_put_dec(seq);
				if (msgbox_send(MSG(CMD_UART_KEY, seq)) == 0)
					uart_puts(" sent\n");
				else
					uart_puts(" FAILED\n");
			} else if (c == 'r') {
				uart_puts("[rx count] ");
				uart_put_dec(rx_count);
				uart_puts("\n");
			} else if (c == 't') {
				uart_puts("[ticks] ");
				uart_put_dec(timer_ticks());
				uart_puts(" (x100ms)\n");
			} else if (c == 'h') {
				heartbeat = !heartbeat;
				uart_puts(heartbeat ? "[heartbeat on]\n"
						    : "[heartbeat off]\n");
			} else {
				/* plain echo, so you can see RX works at all */
				uart_putc((char)c);
			}
		}
	}

	return 0;
}
