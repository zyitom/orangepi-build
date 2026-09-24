/*
 * Clock/reset bring-up for the peripherals this firmware owns.
 *
 * Called from start.S before main(), with interrupts still masked.
 */
#include "fw.h"

/* Ungate and de-assert reset for a BGR-style register.
 * Manual convention (4.2.5.x): bit16+ = reset (0 assert, 1 de-assert),
 * bit0+ = clock gating (0 mask, 1 pass). Reset must be released before
 * the gate is opened, or the block latches a bad state. */
static void bgr_enable(u32 reg, u32 rst_bit, u32 gate_bit)
{
	u32 v = readl(reg);

	v |= (1u << rst_bit);
	writel(v, reg);

	v |= (1u << gate_bit);
	writel(v, reg);
}

/*
 * Work out the actual APBS1 rate, which clocks S_UART0/1.
 *
 * We cannot hard-code this: the source mux and divider are set by boot0 /
 * u-boot / Linux and we have no guarantee which. Read it back instead.
 * Register layout is manual 4.2.5.3; the parent list matches
 * bsp/drivers/clk/sunxi-ng/ccu-sun60iw2-r.c:29 ("r_apbs_parents").
 */
unsigned int apbs1_rate(void)
{
	u32 reg = readl(R_APBS1_CLK_REG);
	u32 src = (reg >> APBS_SRC_SHIFT) & APBS_SRC_MASK;
	u32 m   = ((reg >> APBS_M_SHIFT) & APBS_M_MASK) + 1;
	unsigned int parent;

	switch (src) {
	case APBS_SRC_DCXO:		parent = FREQ_DCXO;		break;	/* 26 MHz on this board */
	case APBS_SRC_RTC32K:		parent = FREQ_RTC32K;		break;
	case APBS_SRC_RC16M:		parent = FREQ_RC16M;		break;
	case APBS_SRC_PERIPLL_DIV:	parent = FREQ_PERIPLL_DIV;	break;
	case APBS_SRC_SYS_CLK24M:	parent = FREQ_SYS_CLK24M;	break;	/* 24 MHz, what the board actually uses */
	default:
		/* Unknown mux setting -- assume the reset default (mux 0, DCXO),
		 * 26 MHz on this board. Still possibly wrong, but the best guess. */
		parent = FREQ_DCXO;
		break;
	}

	return parent / m;
}

void soc_early_init(void)
{
	u32 v;

	/*
	 * RISCV_24M: feeds the E902 subsystem's TIMESTAMP counter (manual 5.2.2
	 * shows "ts_clk: 24M" going into the block alongside GRAYENC/GRAYDEC).
	 * The vendor scp.fex programs this register too, so mirror it: select
	 * DCXO (mux 00, the reset default) and open the gate at bit31.
	 *
	 * Not strictly required for UART or msgbox, but the counter is the only
	 * timebase available to us and manual 5.2.1 notes it "supports counting
	 * immediately after reset is released" -- so it should be running.
	 *
	 * Measured (T31/T35): the shared counter at 0x08010000 ticks at a fixed
	 * 24.000 MHz (ts_clk is a dedicated 24 MHz input) -- do NOT assume this
	 * mux couples it to the 26 MHz crystal.
	 */
	v = readl(RISCV_24M_CLK_REG);
	v &= ~(APBS_SRC_MASK << APBS_SRC_SHIFT);	/* mux 00 = DCXO */
	v |= (1u << 31);			/* gate on */
	writel(v, RISCV_24M_CLK_REG);

	/* S_UART0: bit16 = S_UART0_RST, bit0 = S_UART0_GATING (manual 4.2.5.15) */
	bgr_enable(S_UART_BGR_REG, 16, 0);

	/* S_MBOX (MBOX_CPUS, the side ARM writes to): manual 4.2.5.14 */
	bgr_enable(S_MBOX_BGR_REG, 16, 0);

	/* S_TIMER: bit16 = S_TIMER_RST, bit0 = S_TIMER_GATING (manual 4.2.5.8).
	 * All four timers share this one gate. Forgetting it is the classic
	 * mistake: reads of TMR_* would take an access fault (mcause 0x5/0x7)
	 * and park the firmware in trap_entry. */
	bgr_enable(S_TIMER_BGR_REG, 16, 0);

	/*
	 * MBOX_CPUX at 0x03004000 sits in the CPUX domain and its gate lives in
	 * the main CCU (0x02002000 + 0x0744, = CLK_MSGBOX0 / RST_BUS_MSGBOX0).
	 *
	 * Linux already does this for us: sunxi-msgbox.c's probe path calls
	 * sunxi_msgbox_hw_init() (line 884), which unconditionally does
	 * reset_control_deassert() + clk_prepare_enable() on exactly this
	 * register -- no mailbox client required. So with CONFIG_AW_MSGBOX=y the
	 * gate is open from boot.
	 *
	 * We still poke it, for the case where the firmware is started before
	 * Linux has probed (or with the driver absent). Writing an already-set
	 * bit is harmless. If this SoC's security config rejects E902 writes to
	 * the main CCU the store is dropped silently, and we fall back on Linux
	 * having done it -- which is why the banner reports the msgbox send
	 * result rather than assuming success.
	 */
	bgr_enable(CCU_MSGBOX_BGR_REG, 16, 0);
}
