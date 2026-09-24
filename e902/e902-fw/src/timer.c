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

/* Periodic ticks, split by who noticed them: the ISR (interrupt delivery
 * works) or timer_poll() in the main loop (it does not). Their sum is the
 * tick count; HB_TICK_SRC publishes both halves. */
static volatile unsigned int tick_irq;
static volatile unsigned int tick_polled;
static volatile u32 tick_interval;		/* reload value, 24 MHz ticks */

/* interrupt latency = counter expiry -> first instruction of timer_isr's
 * body, in 24 MHz ticks (41.7 ns). The counter reloads at expiry and keeps
 * counting down, so (interval - CUR) read in the ISR is the time since. */
static volatile u32 lat_last, lat_min = 0xFFFFFFFFu, lat_max;

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

	/* counter clock: pll-ref 24 MHz, /1, enabled (see a733.h) */
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
	 * (which is ambiguous with "not yet loaded").
	 *
	 * Bounded (v63): if S_TIMER has no input clock (24M broadcast off),
	 * EN never clears and this used to spin forever. The E902 runs at up
	 * to 200 MHz and one poll is a slow peripheral read, so us * 64 polls
	 * is comfortably longer than the requested delay. */
	{
		unsigned int spin = us * 64u + 1024u;

		while ((readl(TMR_CTRL_REG(DELAY_TIMER)) & TMR_CTRL_EN) &&
		       --spin)
			;
	}
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
	u32 el = tick_interval - readl(TMR_CUR_VALUE_REG(PERIODIC_TIMER));

	lat_last = el;
	if (el < lat_min)
		lat_min = el;
	if (el > lat_max)
		lat_max = el;
	tick_irq++;

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

	tick_irq = 0;
	tick_polled = 0;
	tick_interval = ticks;

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
	return tick_irq + tick_polled;
}

/*
 * Main-loop fallback for the periodic tick: if the timer1 pending bit is set
 * the ISR has not run (interrupt not delivered), so count and acknowledge it
 * here. Interrupts are masked around the check so the ISR and this path can
 * never both count the same period.
 */
void timer_poll(void)
{
	u32 mstatus;

	__asm__ __volatile__("csrrci %0, mstatus, 8" : "=r"(mstatus));
	/* only a real fallback: claim the period if it has been pending for
	 * over 1 ms, i.e. the interrupt is clearly not coming. Claiming it
	 * right away would steal ticks from the ISR (~20 % measured, T31)
	 * and hide its latency. */
	if ((readl(TMR_IRQ_STA_REG) & TMR_IRQ_PEND(PERIODIC_TIMER)) &&
	    tick_interval - readl(TMR_CUR_VALUE_REG(PERIODIC_TIMER)) >
	    1000u * TICKS_PER_US) {
		writel(TMR_IRQ_PEND(PERIODIC_TIMER), TMR_IRQ_STA_REG);
		tick_polled++;
	}
	if (mstatus & 8)
		__asm__ __volatile__("csrsi mstatus, 8");
}

/* [31:16] max, [15:0] last latency, 24 MHz ticks, saturated */
u32 timer_irq_latency(void)
{
	u32 mx = lat_max > 0xFFFFu ? 0xFFFFu : lat_max;
	u32 la = lat_last > 0xFFFFu ? 0xFFFFu : lat_last;

	return (mx << 16) | la;
}

u32 timer_irq_latency_min(void)
{
	return lat_min;
}

u32 timer_tick_sources(void)
{
	return ((tick_irq & 0xFFFFu) << 16) | (tick_polled & 0xFFFFu);
}

/* Free-running microsecond counter for rough timing measurements.
 * Counts DOWN, so callers should diff in the right direction. */
u32 timer_raw_count(void)
{
	return readl(TMR_CUR_VALUE_REG(DELAY_TIMER));
}
