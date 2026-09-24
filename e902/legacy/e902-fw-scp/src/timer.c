/*
 * S_TIMER driver. Registers per manual 8.2.6/8.2.7, base from ch.2.
 *
 * Four down-counters. We drive them from SYS_CLK24M with no prescaler, so
 * one tick is 1/24 us and the interval register holds (microseconds * 24).
 * That gives a max single interval of 2^32/24 us ~= 178 seconds.
 *
 * timer0 is used for blocking delays (polled, no interrupt).
 * timer1 is used for the periodic tick (interrupt, CLIC irq 21).
 */
#include "fw.h"

#define TICK_HZ		24000000u	/* SYS_CLK24M, prescaler /1 */
#define TICKS_PER_US	(TICK_HZ / 1000000u)

#define DELAY_TIMER	0
#define PERIODIC_TIMER	1

/* Incremented by the periodic ISR. volatile: the main loop polls it. */
static volatile unsigned int tick_count;

static void timer_stop(unsigned int n)
{
	writel(readl(TMR_CTRL_REG(n)) & ~TMR_CTRL_EN, TMR_CTRL_REG(n));
}

/*
 * Load an interval and start counting.
 *
 * Ordering matters (manual 8.2.7.3): the interval register may only be
 * modified while the timer is paused, and RELOAD must be pulsed to copy it
 * into the internal counter. RELOAD self-clears, so poll it before starting.
 */
static void timer_start(unsigned int n, u32 ticks, u32 mode)
{
	u32 ctrl;

	/* counter clock: pll-ref 24 MHz, /1, enabled (see a733.h). Without
	 * this the reset default is clock-OFF, and mux 0 would feed the
	 * 26 MHz crystal -- 8.3% fast (T31). */
	writel(R_TIMER_CLK_PLL_REF | R_TIMER_CLK_EN, R_TIMER_CLK_REG(n));

	timer_stop(n);

	writel(ticks, TMR_INTV_VALUE_REG(n));

	ctrl = TMR_CTRL_SRC_24M | (0u << TMR_CTRL_PRES_SHIFT) | mode;
	writel(ctrl | TMR_CTRL_RELOAD, TMR_CTRL_REG(n));

	/* RELOAD is cleared by hardware once the copy is done. Bounded wait --
	 * if the timer's clock is not running this would otherwise hang. */
	{
		int spin = 10000;

		while ((readl(TMR_CTRL_REG(n)) & TMR_CTRL_RELOAD) && spin--)
			;
	}

	writel(ctrl | TMR_CTRL_EN, TMR_CTRL_REG(n));
}

/*
 * Blocking delay. Polls the counter, does not need an interrupt, so it is
 * safe to call before interrupts are enabled.
 */
void delay_us(unsigned int us)
{
	u32 ticks = us * TICKS_PER_US;

	if (ticks == 0)
		return;

	timer_start(DELAY_TIMER, ticks, TMR_CTRL_MODE_SINGLE);

	/* Counter runs interval -> 0. In single mode the hardware clears EN
	 * when it reaches zero, so wait for that rather than for CUR == 0
	 * (which is ambiguous with "not yet loaded"). */
	while (readl(TMR_CTRL_REG(DELAY_TIMER)) & TMR_CTRL_EN)
		;
}

void delay_ms(unsigned int ms)
{
	/* Split the wait so ms * 1000 * 24 cannot overflow 32 bits.
	 * 1000 ms at a time keeps the product at 24e6, well inside range. */
	while (ms >= 1000) {
		delay_us(1000000);
		ms -= 1000;
	}
	if (ms)
		delay_us(ms * 1000);
}

/*
 * Periodic tick ISR.
 *
 * The interrupt attribute is mandatory: CLIC hardware vectoring jumps here
 * without saving anything, so the compiler has to emit the save/restore
 * prologue and return with mret. Without it this would silently corrupt
 * whatever the main loop had in registers.
 *
 * Keep it short and do not print from here -- uart_putc() busy-waits on the
 * FIFO, which would stretch the interrupt long enough to drop msgbox
 * messages. Set a flag, let the main loop do the talking.
 */
static void __attribute__((interrupt("machine"))) timer_isr(void)
{
	tick_count++;

	/* Acknowledge: write 1 to the pending bit (manual 8.2.7.2). Do this
	 * last, after the work, or a fast re-trigger could be lost. */
	writel(TMR_IRQ_PEND(PERIODIC_TIMER), TMR_IRQ_STA_REG);
}

/*
 * Start a periodic interrupt every period_ms milliseconds.
 * Must be called before mstatus.MIE is set, or with interrupts masked.
 */
void timer_periodic_start(unsigned int period_ms)
{
	u32 ticks = period_ms * 1000u * TICKS_PER_US;

	tick_count = 0;

	/* Clear any stale pending bit before enabling, so we do not take an
	 * immediate spurious interrupt. */
	writel(TMR_IRQ_PEND(PERIODIC_TIMER), TMR_IRQ_STA_REG);
	writel(readl(TMR_IRQ_EN_REG) | TMR_IRQ_EN(PERIODIC_TIMER),
	       TMR_IRQ_EN_REG);

	clic_set_handler(IRQ_S_TIMER1, timer_isr);
	clic_enable(IRQ_S_TIMER1, 0 /* level triggered */);

	timer_start(PERIODIC_TIMER, ticks, TMR_CTRL_MODE_CONT);
}

void timer_periodic_stop(void)
{
	timer_stop(PERIODIC_TIMER);
	clic_disable(IRQ_S_TIMER1);
	writel(readl(TMR_IRQ_EN_REG) & ~TMR_IRQ_EN(PERIODIC_TIMER),
	       TMR_IRQ_EN_REG);
}

unsigned int timer_ticks(void)
{
	return tick_count;
}

/* Free-running microsecond counter for rough timing measurements.
 * Counts DOWN, so callers should diff in the right direction. */
u32 timer_raw_count(void)
{
	return readl(TMR_CUR_VALUE_REG(DELAY_TIMER));
}
