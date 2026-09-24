/*
 * clocks.c -- CPUS-domain 24MHz oscillator broadcast, power-ON sequence.
 *
 * Transcribed verbatim from the vendor scp.fex function at 0x40005c44
 * (called from 0x40009dce) -- the power-ON mirror of the power-off flow at
 * 0x40005ba8 that v3/v3.1 mistakenly replicated (they ran the DISABLE path:
 * bit0 cleared, which kills the 24M the CPUS peripherals need, hence the
 * historic "toxic" freeze).
 *
 * Without this sequence the CPUS-domain peripherals clocked from
 * SYS_CLK24M do not run (hardware-proven 2026-09-23, v5.3):
 *   - S_TIMER: RELOAD never self-clears, CUR_VALUE static
 *     => no timer IRQ => no [tick], delay_ms() returns instantly
 *   - S_SPI: transfers never complete (TC timeout, tc_count stays 0)
 *
 * Register dance (addresses; keys in bits[31:16]):
 *   0x07090000  RTC LOSC_CTRL  key 0x16AA; bit14 LOSC auto-switch enable,
 *                              bit4 external 32k enable, bit0 osc32k-sys
 *                              parent = external 32k
 *   0x07090004  RTC LOSC_AUTO_SWT_STA, bit2 must poll clear
 *   0x0709015C  RTC key register for XO_CTRL, write 0x16AA
 *   0x07090160  RTC XO_CTRL, bit1 = DCXO enable
 *   0x07010244  R-CCU     : key 0xA700, bit0 = CPUS 24M broadcast enable
 *
 * OWNERSHIP (v63 correction): 0x0709xxxx is the RTC block, not a watchdog
 * (the R_WDT_* names below are historical). Linux's rtc_ccu driver
 * (bsp/drivers/clk/sunxi-ng/ccu-sun60iw2-rtc.c, rtc_ccu@7090000 "okay")
 * programs exactly the same bits to exactly the same values in its probe
 * (steps 1-4 of its LOSC init), so the two writers converge. This runs a
 * few microseconds after the ready packet, i.e. before the kernel is even
 * entered, so the read-modify-writes cannot interleave with Linux's.
 *
 * The vendor log-helper calls (0x40006676) between steps are diagnostics
 * only and are omitted. The busy poll is bounded here so an unexpected
 * state degrades to a failure code instead of a hang.
 */
#include "fw.h"

#define R_WDT_CFG       0x07090000u
#define R_WDT_BUSY      0x07090004u
#define R_WDT_KEY_REG   0x0709015Cu
#define R_WDT_CTRL2     0x07090160u
#define OSC24M_BCAST    0x07010244u

#define BCAST_KEY       0xA7000000u
#define WDT_KEY_HI      0x16AA0000u
#define WDT_KEY_WORD    0x16AAu

int cpus_24m_broadcast_on(void)
{
	u32 orig, v;
	int spin;

	/* v5.8: full vendor sequence with the CORRECT writel(v, a) argument
	 * order -- v5.4-v5.7 all froze because the key was OR-ed into the
	 * ADDRESS (0xA7010244) instead of the VALUE, causing an access fault
	 * (mcause=0x30000006, mepc=the faulting sw). Heartbeat markers
	 * HB_RST_START: 0xB1..0xB6 = vendor R_WDT dance, 0xC1..0xC4 = R-CCU
	 * broadcast. */
	orig = readl(R_WDT_CFG);
	hb_set(HB_RST_START, 0xB1u);

	/* W1 */ v = (orig & 0xFFFFu) | (WDT_KEY_HI | 0x4000u);
	writel(v, R_WDT_CFG);
	/* W2 */ v = (orig & 0xFFFFu) | (WDT_KEY_HI | 0x4010u);
	writel(v, R_WDT_CFG);
	/* W3 */ v = (orig & 0x1FFFFu) | (WDT_KEY_HI | 0x4010u);
	writel(v, R_WDT_CFG);
	hb_set(HB_RST_START, 0xB2u);

	/* poll: wait for the switch busy flag (bit2) to clear (bounded) */
	for (spin = 1000000; spin > 0; spin--)
		if ((readl(R_WDT_BUSY) & 4u) == 0u)
			break;
	if (spin == 0)
		return -1;
	hb_set(HB_RST_START, 0xB3u);

	/* W4: config |= bit14|bit4|bit0 (the broadcast enable itself) */
	v = (orig & 0x1FFFFu) | (WDT_KEY_HI | 0x4011u);
	writel(v, R_WDT_CFG);
	hb_set(HB_RST_START, 0xB4u);

	/* W5/W6: unlock + set bit1 of the second control register */
	writel(WDT_KEY_WORD, R_WDT_KEY_REG);
	writel(readl(R_WDT_CTRL2) | 0x2u, R_WDT_CTRL2);
	hb_set(HB_RST_START, 0xB6u);

	/* R-CCU broadcast: key phase, then key|enable, written twice */
	v = readl(OSC24M_BCAST);
	writel(v | BCAST_KEY, OSC24M_BCAST);
	hb_set(HB_RST_START, 0xC2u);

	v = readl(OSC24M_BCAST);
	writel(v | (BCAST_KEY | 1u), OSC24M_BCAST);
	writel(v | (BCAST_KEY | 1u), OSC24M_BCAST);
	hb_set(HB_RST_START, 0xC4u);

	/* v5.9: S_TIMER_BGR (0x011C) -- the vendor SCP footprint includes this
	 * register (RESOURCE-MAP section 8) and timer.c never opened it. Gate
	 * bit0 + reset release bit16 for S_TIMER0..3. */
	v = readl(S_TIMER_BGR_REG);
	v |= 0x1u | 0x10000u;
	writel(v, S_TIMER_BGR_REG);
	hb_set(HB_RST_START, 0xB7u);

	return 0;
}
