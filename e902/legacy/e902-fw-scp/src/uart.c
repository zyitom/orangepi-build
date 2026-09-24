/*
 * S_UART0 driver. The A733 UART is a DesignWare APB UART (16550-compatible);
 * register map is manual 18.10.8, offsets are byte offsets from the base.
 *
 * Pin mux is not done here -- see pinmux.c, which main() calls first.
 */
#include "fw.h"

static u32 uart_base = S_UART0_BASE;

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

void uart_putc(char c)
{
	while (!(readl(uart_base + UART_LSR) & LSR_THRE))
		;
	writel((u8)c, uart_base + UART_THR);
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
	while (!(readl(uart_base + UART_LSR) & LSR_TEMT))
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
