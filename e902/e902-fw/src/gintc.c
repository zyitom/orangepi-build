/*
 * GINTC: the GIC -> E902 interrupt router.
 *
 * 2026-09-29 (ledger T39): the base used to be 0x07090000, which is the
 * RTC (manual address map: RTC 0x07090000, S_TIMER 0x07091000, nothing in
 * between; DT rtc@7090000 / rtc_ccu@7090000). Every T38 experiment read
 * and wrote RTC / RTC-CCU registers. Manual 12.1.4 lists the real
 * instances: CPUS_INTERRUPT_CTRL 0x02055000 and RV_INTERRUPT_CTRL
 * 0x02056000 (RV = this E902), with INTC_CONFIG_REG0..7 at 0x10-0x2C (one
 * bit per interrupt, 1 = forward) and SYS_INT_STATE0..6 from 0x100.
 *
 * The "per-input register at base + 4*N" family is not in the manual; its
 * writes are compiled out unless GINTC_PER_INPUT_REGS is set. Untested on
 * the new base.
 */
#include "fw.h"

#define GINTC_BASE		0x02056000	/* RV_INTERRUPT_CTRL, manual 12.1.4 */
#ifndef GINTC_PER_INPUT_REGS
#define GINTC_PER_INPUT_REGS	0
#endif
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
	if (GINTC_PER_INPUT_REGS)
		writel(cfg, GINTC_CFG(input));
	gintc_cfg_val[input] = cfg;
	gintc_armed[input] = 1;
	clic_set_handler(input, gintc_isr);
	clic_enable(input, edge);
}

static void gintc_disarm(unsigned int input)
{
	writeb(0, CLIC_INTIE(input));
	if (GINTC_PER_INPUT_REGS)
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
