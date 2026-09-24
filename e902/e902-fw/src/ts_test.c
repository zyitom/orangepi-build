/*
 * ts_test.c - 一次性验证：E902（CPUS 域）能不能读到 CPUX 域的
 * SoC 时间戳计数器 0x08010000。
 *
 * ARM 侧已实测：该计数器使能位=1、FREQID=24000000、速率 24.000 MHz，
 * 且与 CLOCK_MONOTONIC 只有恒定偏置（零漂移）。本测试回答的是
 * "同一地址能不能从 CPUS 域读到" —— 能，则双核时间戳可零偏置共用。
 */
#include "fw.h"

#define TS_STA   0x08010000u	/* CNT_LOW(+0) / CNT_HI(+4)，只读 */
#define TS_CTRL  0x08020000u	/* bit0 = enable */
#define TS_FREQID 0x08020020u	/* 时基频率，应为 0x016E3600 */

/*
 * v5.3: the rdcycle cross-check is REMOVED. Hardware evidence (2026-09-23):
 * `rdcycle` raises an illegal-instruction exception on this E902
 * (last_mcause=0x30000002, last_mepc = the rdcycle site in this function) --
 * the cycle CSR is not accessible here, so keep only the delay-based delta.
 *
 * The previous run reported delta_1s = 58254 ticks. That number is only
 * meaningful if delay_ms(1000) really is 1000 ms -- and if S_TIMER's input
 * clock is what timer.c assumes.
 */

static void put64(u32 hi, u32 lo)
{
	uart_put_hex(hi);
	uart_putc(':');
	uart_put_hex(lo);
}

void ts_test_run(void)
{
	u32 ctrl, freq, lo0, hi0, lo1, hi1, d;

	uart_puts("\n--- TIMESTAMP probe (CPUX domain 0x08010000) ---\n");

	ctrl = readl(TS_CTRL);
	freq = readl(TS_FREQID);
	uart_puts("TSTAMP_CTRL = ");
	uart_put_hex(ctrl);
	uart_puts("  FREQID = ");
	uart_put_hex(freq);
	uart_puts(freq == 24000000u ? "  (24 MHz)\n" : "  (NOT 24 MHz)\n");

	lo0 = readl(TS_STA);
	hi0 = readl(TS_STA + 4);
	uart_puts("t0 = ");
	put64(hi0, lo0);
	uart_puts("  (also STA+0x08/0x0C for cross-check: ");
	put64(readl(TS_CTRL + 0x0C), readl(TS_CTRL + 0x08));
	uart_puts(")\n");

	delay_ms(1000);		/* v5.3: straight delay, no rdcycle */

	lo1 = readl(TS_STA);
	hi1 = readl(TS_STA + 4);
	uart_puts("t1 = ");
	put64(hi1, lo1);
	uart_puts("  (also STA+0x08/0x0C: ");
	put64(readl(TS_CTRL + 0x0C), readl(TS_CTRL + 0x08));
	uart_puts(")\n");

	if (hi0 == 0u && lo0 == 0u && hi1 == 0u && lo1 == 0u) {
		uart_puts("=> 读到全 0：该地址在 CPUS 域不可读（或计数器未使能）\n");
	} else if (hi1 == hi0) {
		d = lo1 - lo0;
		uart_puts("delta_1s = ");
		uart_put_dec(d);
		uart_puts(" ticks\n");
		if (d > 23000000u && d < 25000000u)
			uart_puts("=> OK: 与 ARM 侧同一个 24 MHz 计数器，可零偏置共用\n");
		else if (d == 0u)
			uart_puts("=> 计数器没动：地址可达但计数器未运行\n");
		else
			uart_puts("=> 速率异常，需要进一步判断\n");
	} else {
		uart_puts("=> 1 秒内高 32 位已进位（正常，说明计数器在跑）\n");
	}

	uart_puts("--- end probe ---\n");
}
