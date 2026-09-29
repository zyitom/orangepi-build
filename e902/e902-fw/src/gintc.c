/*
 * GINTC: the GIC -> E902 interrupt router at 0x07090000 (A733 manual
 * ch.12.1 "Interrupt Controller"; the unnamed 4K block between RTC
 * 0x07085000 and S_TIMER 0x07091000 in the ch.2 address map -- T38).
 *
 * Two register families (manual 12.1.5):
 *   - group-mask regs at 0x0010+4k, byte per 8 GIC ids: REG0[7:0] ->
 *     gic [39:32], REG1[7:0] -> [71:64]...
 *   - per-input regs at base + 4*N (input 70 = 0x0118 GPADC).
 * Input numbers match GIC ids, so the router can hand ANY SoC SPI to
 * the E902 -- the vendor firmware just never does. The exact enable
 * semantics are not documented; 'G' sweeps both families against a
 * live GPADC storm (sunxi_gpadc on the host fires SPI 38 at ~4 kHz).
 */
#include "fw.h"

#define GINTC_BASE		0x07090000
#define GINTC_CFG(n)		(GINTC_BASE + 4u * (n))
#define GINTC_GROUP(k)		(GINTC_BASE + 0x10u + 4u * (k))
/* GIC id 121 (timer@3009000, fires ~70/s) -> REG2 (96-127) bit 25 */
#define GINTC_TEST_GROUP	3			/* REG3 = GIC ids 96-127 */
#define GINTC_TEST_MASK_BIT	(1u << 25)	/* gic id 121 */
#define GINTC_TEST_INPUT	121	/* TIMER0 */
#define GINTC_INPUT_GPADC	70
#define GINTC_INPUT_LRADC	69
#define GINTC_INPUT_UART0	34	/* big-core console RX */
#define GINTC_MAX_INPUT		128

static volatile unsigned int gintc_hits[GINTC_MAX_INPUT];
static unsigned int gintc_armed[GINTC_MAX_INPUT];
static unsigned int gintc_cfg_val[GINTC_MAX_INPUT];

static void __attribute__((interrupt("machine"))) gintc_isr(void)
{
	u32 mcause;
	unsigned int irq;

	__asm__ __volatile__("csrr %0, mcause" : "=r"(mcause));
	irq = mcause & 0xFFFu;
	if (irq < GINTC_MAX_INPUT)
		gintc_hits[irq]++;
}

unsigned int gintc_count(void)
{
	unsigned int i, sum = 0;

	for (i = 0; i < GINTC_MAX_INPUT; i++)
		sum += gintc_hits[i];
	return sum;
}

static void gintc_arm(unsigned int input, unsigned int cfg, int edge)
{
	gintc_hits[input] = 0;
	writel(cfg, GINTC_CFG(input));
	gintc_cfg_val[input] = cfg;
	gintc_armed[input] = 1;
	clic_set_handler(input, gintc_isr);
	clic_enable(input, edge);
}

static void gintc_disarm(unsigned int input)
{
	writeb(0, CLIC_INTIE(input));
	writel(0, GINTC_CFG(input));
	gintc_cfg_val[input] = 0;
	gintc_armed[input] = 0;
}

void gintc_off(void)
{
	unsigned int i;

	for (i = 16; i < GINTC_MAX_INPUT; i++)
		if (gintc_armed[i])
			gintc_disarm(i);
	uart_puts("[gintc] all inputs disarmed\n");
}

/* 'G': route GIC id 121 (TIMER0, ~70/s) through the GINTC group
 * enable (REG3 bit25) and count arrivals on the group lines
 * riscv_sys_irq_i -> E902 inputs 72/73. */
void gintc_sweep(void)
{
	unsigned int before72, before73, hits72, hits73;
	u32 t0, r3;

	uart_puts("[gintc] forward gic121 (TIMER0) via REG3 bit25; "
		  "count on inputs 72/73\n");
	r3 = readl(GINTC_GROUP(GINTC_TEST_GROUP));
	writel(r3 | GINTC_TEST_MASK_BIT, GINTC_GROUP(GINTC_TEST_GROUP));

	gintc_hits[72] = 0;
	gintc_hits[73] = 0;
	clic_set_handler(72, gintc_isr);
	clic_enable(72, 0);
	clic_set_handler(73, gintc_isr);
	clic_enable(73, 0);

	t0 = timer_ticks();
	while (timer_ticks() - t0 < 30) {	/* 3 s */
		msgbox_poll_rx();
		timer_poll();
	}
	hits72 = gintc_hits[72];
	hits73 = gintc_hits[73];
	writel(r3, GINTC_GROUP(GINTC_TEST_GROUP));	/* restore */
	writeb(0, CLIC_INTIE(72));
	writeb(0, CLIC_INTIE(73));

	uart_puts("[gintc] in72=");
	uart_put_dec(hits72);
	uart_puts(" in73=");
	uart_put_dec(hits73);
	uart_puts(hits72 || hits73 ?
		  "  -> ROUTED (gic121 reached the E902)\n" :
		  "  -> nothing arrived on the group lines\n");
}

/* 'U': arm/disarm UART0 RX (input 34, edge). Host types on the ARM
 * serial console (ttyS0) -- each char should count once here. */
void gintc_uart_toggle(void)
{
	if (gintc_armed[GINTC_INPUT_UART0]) {
		unsigned int h = gintc_hits[GINTC_INPUT_UART0];

		gintc_disarm(GINTC_INPUT_UART0);
		uart_puts("[gintc] uart0 rx count=");
		uart_put_dec(h);
		uart_puts(" (now disarmed)\n");
	} else {
		gintc_arm(GINTC_INPUT_UART0, 1, 1);
		uart_puts("[gintc] uart0 rx armed (edge); type on ttyS0, "
			  "'U' again to read\n");
	}
}

/* 'L': arm/disarm LRADC (input 69, edge). */
void gintc_lradc_toggle(void)
{
	if (gintc_armed[GINTC_INPUT_LRADC]) {
		unsigned int h = gintc_hits[GINTC_INPUT_LRADC];

		gintc_disarm(GINTC_INPUT_LRADC);
		uart_puts("[gintc] lradc count=");
		uart_put_dec(h);
		uart_puts(" (now disarmed)\n");
	} else {
		gintc_arm(GINTC_INPUT_LRADC, 1, 1);
		uart_puts("[gintc] lradc armed (edge); 'L' again to read\n");
	}
}

/* 'I': report per-input counters for everything still armed. */
void gintc_report(void)
{
	unsigned int i;

	uart_puts("[gintc] counts:");
	for (i = 16; i < GINTC_MAX_INPUT; i++)
		if (gintc_armed[i]) {
			uart_puts(" in");
			uart_put_dec(i);
			uart_putc('=');
			uart_put_dec(gintc_hits[i]);
			uart_puts("/cfg=");
			uart_put_hex(gintc_cfg_val[i]);
		}
	uart_puts("\n");
}
