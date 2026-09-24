/*
 * wdt.c -- WDT_CPUS service, transcribed verbatim from the vendor scp.fex
 * boot path: the function at 0x40007c18 runs IMMEDIATELY BEFORE the startup
 * feedback (caller 0x4000b198 -> feedback at 0x4000b202) and services the
 * WDT_CPUS (0x07021000) which boot0/bl31 leave armed with a ~10s timeout.
 *
 * History (FINDINGS-LEDGER T23/T24/T25):
 *   v2      - no WDT service            -> watchdog bites ~12s, boot loop
 *   v3/v3.1 - replicated the SUSPEND-path function at 0x40005ba8 (R-CCU
 *             0x07010244 key 0xA700 + R_WDT 0x07090000 key 0x16AA): that code
 *             belongs to the "power off 24mhz osc broadcast" flow and froze
 *             the E902 before the banner; v3.1 additionally proved that the
 *             freeze disappears the moment the toxic writes are removed.
 *   v3.2    - THIS file: only the vendor boot-path sequence below.
 */
#include "fw.h"

#define WDT_CPUS_BASE   0x07021000u   /* WDT_CPUS (manual 8.3.5) */
#define WDT_MODE_REG    (WDT_CPUS_BASE + 0x14)
#define WDT_CTRL_REG    (WDT_CPUS_BASE + 0x18)

void wdt_cpus_service(void)
{
	u32 v;

	/* WDT_MODE (0x14): clock source bit8 = 0 (RTC_32K), mode bits[1:0] = 01,
	 * each write followed by a read-back exactly like the vendor. */
	v = readl(WDT_MODE_REG);
	v &= ~0x100u;
	writel(WDT_MODE_REG, v);
	v = readl(WDT_MODE_REG);
	writel(WDT_MODE_REG, v);
	v = readl(WDT_MODE_REG);
	v &= ~0x3u;
	writel(WDT_MODE_REG, v);
	v = readl(WDT_MODE_REG);
	v |= 0x1u;
	writel(WDT_MODE_REG, v);

	/* WDT_CTRL (0x18): interval field bits[7:4] = 0x4. */
	v = readl(WDT_CTRL_REG);
	v &= ~0xF0u;
	writel(WDT_CTRL_REG, v);
	v = readl(WDT_CTRL_REG);
	v |= 0x40u;
	writel(WDT_CTRL_REG, v);
}
