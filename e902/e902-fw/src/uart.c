/*
 * S_UART0 driver. The A733 UART is a DesignWare APB UART (16550-compatible);
 * register map is manual 18.10.8, offsets are byte offsets from the base.
 *
 * Pin mux is not done here -- see pinmux.c, which main() calls first.
 *
 * Receive interrupts (CLIC IRQ 29, manual Table 12-2) are optional: the
 * polling path (uart_getc_nb) keeps working with the interrupt disabled, and
 * uart_irq_enable() additionally routes RX into a small ring buffer drained
 * by uart_rx_pop(). The ISR counts every byte, which is what the interrupt
 * acceptance record reports.
 */
#include "fw.h"

static u32 uart_base = S_UART0_BASE;

static volatile unsigned int rx_count;
static volatile unsigned int rx_drop;
#define RX_RING 64
static volatile unsigned char rx_ring[RX_RING];
static volatile unsigned int rx_head, rx_tail;

void uart_init(unsigned int baud)
{
	unsigned int clk = apbs1_rate();
	unsigned int div;

	/* 16x oversampling: divisor = clk / (16 * baud), rounded */
	div = (clk + 8 * baud) / (16 * baud);
	if (div == 0)
		div = 1;

	/* Disable interrupts while we reconfigure */
	writel(0, uart_base + UART_IER);

	/* Program the divisor latches (needs DLAB=1) */
	writel(LCR_DLAB, uart_base + UART_LCR);
	writel(div & 0xFF, uart_base + UART_DLL);
	writel((div >> 8) & 0xFF, uart_base + UART_DLH);

	/* 8N1, DLAB back to 0 */
	writel(LCR_WLS_8BIT, uart_base + UART_LCR);

	/* Enable and reset both FIFOs */
	writel(FCR_FIFOE | FCR_RFIFOR | FCR_XFIFOR, uart_base + UART_FCR);

	/* No modem control lines wired on this board */
	writel(0, uart_base + UART_MCR);
}

/*
 * TX path (v63). uart_putc() used to busy-wait on THRE for every byte: a
 * 60-character log line costs ~5 ms at 115200 baud, and every "[pkt]" line
 * was printed BEFORE the reply went out -- with bl31 spinning in EL3, IRQs
 * masked, waiting for that reply. Now bytes go into a ring that
 * uart_tx_pump() drains from the main loop whenever the hardware FIFO is
 * empty; when the ring is full new bytes are dropped (and counted), never
 * waited for. uart_set_blocking(1) restores the old synchronous behaviour
 * for the point-of-no-return power path, where nothing else runs anyway.
 */
#define TX_RING		2048
#define TX_BURST	16		/* THRE => whole 64-byte FIFO is empty */
#define TX_SPIN		200000		/* ~a few ms: a dead UART can't hang us */

static volatile char tx_ring[TX_RING];
static volatile unsigned int tx_head, tx_tail;
static unsigned int tx_drop;
static int tx_blocking;

static int thr_wait(void)
{
	unsigned int spin = TX_SPIN;

	while (!(readl(uart_base + UART_LSR) & LSR_THRE)) {
		if (--spin == 0)
			return -1;
	}
	return 0;
}

void uart_tx_pump(void)
{
	unsigned int n;

	if (tx_tail == tx_head)
		return;
	if (!(readl(uart_base + UART_LSR) & LSR_THRE))
		return;
	for (n = 0; n < TX_BURST && tx_tail != tx_head; n++) {
		writel((u8)tx_ring[tx_tail], uart_base + UART_THR);
		tx_tail = (tx_tail + 1) % TX_RING;
	}
}

/* Drain the ring synchronously (bounded). */
void uart_tx_flush(void)
{
	while (tx_tail != tx_head) {
		if (thr_wait() != 0)
			return;
		uart_tx_pump();
	}
}

void uart_set_blocking(int on)
{
	if (on)
		uart_tx_flush();
	tx_blocking = on;
}

unsigned int uart_tx_dropped(void)
{
	return tx_drop;
}

void uart_putc(char c)
{
	unsigned int next;

	if (tx_blocking) {
		if (thr_wait() == 0)
			writel((u8)c, uart_base + UART_THR);
		return;
	}
	next = (tx_head + 1) % TX_RING;
	if (next == tx_tail) {
		tx_drop++;
		return;
	}
	tx_ring[tx_head] = c;
	tx_head = next;
}

void uart_puts(const char *s)
{
	while (*s) {
		if (*s == '\n')
			uart_putc('\r');
		uart_putc(*s++);
	}
}

/* Returns -1 when no byte is waiting. */
int uart_getc_nb(void)
{
	if (!(readl(uart_base + UART_LSR) & LSR_DR))
		return -1;
	return (int)(readl(uart_base + UART_RBR) & 0xFF);
}

void uart_flush(void)
{
	unsigned int spin = TX_SPIN;

	uart_tx_flush();
	while (!(readl(uart_base + UART_LSR) & LSR_TEMT) && --spin)
		;
}

void uart_put_hex(u32 v)
{
	static const char d[] = "0123456789abcdef";
	int i;

	uart_puts("0x");
	for (i = 28; i >= 0; i -= 4)
		uart_putc(d[(v >> i) & 0xF]);
}

void uart_put_dec(unsigned int v)
{
	char buf[11];
	int i = 0;

	if (v == 0) {
		uart_putc('0');
		return;
	}
	while (v && i < (int)sizeof(buf)) {
		buf[i++] = '0' + (v % 10);
		v /= 10;
	}
	while (i--)
		uart_putc(buf[i]);
}

/* ---- receive interrupt path ---- */

void uart_irq_enable(void)
{
	/* ERBFI: interrupt when the receive FIFO has data. */
	writel(IER_ERBFI, uart_base + UART_IER);
}

static void rx_drain(void)
{
	while (readl(uart_base + UART_LSR) & LSR_DR) {
		unsigned char c = (unsigned char)(readl(uart_base + UART_RBR) & 0xFF);
		unsigned int next = (rx_head + 1) % RX_RING;

		if (next != rx_tail) {
			rx_ring[rx_head] = c;
			rx_head = next;
		} else {
			rx_drop++;
		}
		rx_count++;
	}
}

/*
 * CLIC IRQ 29. Drain the RX FIFO into the ring, count, return. The
 * interrupt attribute is mandatory: CLIC hardware vectoring jumps straight
 * here, and a plain function would return with `ret` into whatever ra held
 * (v62 and earlier: the first key press after interrupts start working
 * would have crashed the core).
 */
void __attribute__((interrupt("machine"))) uart_isr(void)
{
	rx_drain();
}

/* Polled RX: main loop. Safe alongside the ISR (interrupts are masked
 * around the drain so the two never interleave on RBR). */
void uart_rx_poll(void)
{
	u32 mstatus;

	__asm__ __volatile__("csrrci %0, mstatus, 8" : "=r"(mstatus));
	rx_drain();
	if (mstatus & 8)
		__asm__ __volatile__("csrsi mstatus, 8");
}

unsigned int uart_rx_count(void)
{
	return rx_count;
}

int uart_rx_pop(void)
{
	int c;

	if (rx_tail == rx_head)
		return -1;
	c = rx_ring[rx_tail];
	rx_tail = (rx_tail + 1) % RX_RING;
	return c;
}
