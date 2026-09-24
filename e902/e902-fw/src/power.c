/*
 * power.c -- PSCI SYSTEM_OFF / SYSTEM_RESET for the A733, executed here on
 * the E902.  Our firmware replaces the vendor scp.fex, and on this SoC the
 * little core is the only agent that can cut or cycle the PMU rails.
 *
 * PROTOCOL (reversed from monitor.fex/bl31 + scp.fex, see
 * doc/e902/FINDINGS-LEDGER.md for the disassembly references):
 *
 * ARM side (monitor.fex, vaddr = file offset):
 *   plat_aw_system_off  (0x80bc) -> sender 0x2158, arg 0
 *   plat_aw_system_reset (0x8110)-> sender 0x2158, arg 1
 *   sender 0x2158: builds {byte0=0x02, byte1=0x00, byte2=0x24, byte3=dc}
 *   + count=1 + payload[0]=op (0 = off, 1 = reset), pushes it over msgbox
 *   ch3 (ARM-side regs 0x0309406c/7c) and then simply `wfi`s inside PSCI.
 *   No reply is expected: flags byte1 = 0, and bl31 never polls anything
 *   afterwards.  If the SCP does not act, the board just stays up.
 *
 * SCP side (scp.fex, vaddr = file + 0x40004000):
 *   dispatcher 0x4000abe0: hdr byte2 = type; type 0x24 -> handler
 *   0x4000aa54; payload[0] = op:
 *
 *     op 0 (system off), 0x40007a0e -> cb 0x4000766a:
 *       GP3=0xA101; twi init + wait bus idle; GP3=0xA203;
 *       pmu-inited gate (0x4001dddc, skipped here, see deviations);
 *       vbus check (GP3=0xA501, AXP515(0x34) reg 0x00 bit1);
 *       log "axp8191 exist"; AXP8191(0x36) reg 0x04 = 0x08;
 *       axp515 reset prep; exit(1)                    (0x400075ee)
 *
 *     op 1/2 (system reset), 0x400079c8 -> cb 0x400076ce:
 *       GP3=0xA102; twi idle; GP3=0xA202; axp515 reset prep;
 *       AXP8191 reg 0x04 = 0x01; exit(1)
 *
 *     op 3, 0x40007974 -> cb 0x40007706:
 *       GP3=0xA103; twi idle (only if twi was ever inited); GP3=0xA201;
 *       exit(0)
 *
 *   exit(arg) 0x400075ee: GP3 = 0xA300|arg, AXP8191 reg 0x55 |=
 *   1<<(7-arg) (bit6 for op0/1/2, bit7 for op3), prints
 *   "reset system"/"poweroff system", dead loop.  The GP3 codes
 *   (0xA1xx/0xA2xx/0xA3xx/0xA4xx/0xA5xx written to RTC 0x0709010C) drive
 *   the always-on power FSM, which decides from the history whether
 *   power returns after the plug-pull (off vs. reset).
 *
 *   The reply rule (dispatcher tail 0x4000acd8) is implemented in
 *   main.c: result byte = handler return, echo only when flags&3 != 0.
 *   For ops 0..3 the handler below never returns, exactly like the
 *   vendor: no echo goes out, bl31 is long gone into wfi anyway.
 *
 * Deviations from the vendor firmware (deliberate):
 *   - the pmu-inited gate (0x4001dddc) is ignored: our firmware has no
 *     vendor PMU framework, so honoring it would make poweroff a
 *     permanent no-op.  TWI/PMU errors are logged and the sequence
 *     still runs to the exit write.
 *   - the op0 vbus gate cannot abort the power action here (vendor
 *     returns silently, i.e. poweroff fails); we log and continue.
 *   - the bus-idle retry loop is bounded (vendor retries forever).
 *   - op 3 skips the vendor PMU-framework hook 0x40007828.
 *
 * The TWI engine below follows the vendor byte-for-byte (S_TWI0 = same IP
 * as the sun6i "Twin Fruit" controller): CNTR 0x44 = BUS_EN|AACK, INT flag
 * bit3 is set by hardware on byte completion and cleared by us writing 1,
 * START bit5, STOP bit4, last-byte NACK by clearing AACK, idle STAT 0xF8.
 */
#include "fw.h"

/* ---- S_TWI0 (S_BUS) 0x07083000, pins PL0/PL1 func 2 ---- */
#define TWI_BASE		0x07083000
#define TWI_DATA_REG		(TWI_BASE + 0x08)
#define TWI_CNTR_REG		(TWI_BASE + 0x0C)
#define TWI_STAT_REG		(TWI_BASE + 0x10)
#define TWI_SRST_REG		(TWI_BASE + 0x18)
#define TWI_LCR_REG		(TWI_BASE + 0x20)

#define TWI_CNTR_AACK		(1u << 2)
#define TWI_CNTR_INT		(1u << 3)	/* hw sets on byte done, W1C */
#define TWI_CNTR_STOP		(1u << 4)
#define TWI_CNTR_START		(1u << 5)
#define TWI_CNTR_BUS_EN		(1u << 6)
#define TWI_CNTR_RUN		(TWI_CNTR_BUS_EN | TWI_CNTR_AACK)

#define TWI_STAT_IDLE		0xF8
#define TWI_STAT_START		0x08
#define TWI_STAT_ADDR_W_ACK	0x18
#define TWI_STAT_ADDR_R_ACK	0x40
#define TWI_STAT_RESTART	0x10
#define TWI_STAT_DATA_ACK	0x28
#define TWI_STAT_DATA_ACK_R	0x50
#define TWI_STAT_DATA_NACK_R	0x58
#define TWI_LCR_READY		0x3A	/* both lines released, SDA high */

/* vendor spin counts are 2047; ours is roomier because our core clock is
 * not the one the vendor tuned this for (~1ms at worst per byte) */
#define TWI_SPIN		50000

/* ---- RTC (0x07090000): GP3 carries the always-on FSM protocol ---- */
#define RTC_GP3_REG		0x0709010C

/* ---- R_TWI gate/reset, ccu-sun60iw2-r.c + scp.fex 0x40005936/0x40005ddc */
#define R_TWI_BGR_REG		0x0701019C
#define R_TWI_BGR_GATE		(1u << 0)
#define R_TWI_BGR_RST		(1u << 16)

/* ---- PL pinctrl (same block pinmux.c uses) ---- */
#define S_GPIO_BASE		0x07025000
#define PL_CFG0			(S_GPIO_BASE + 0x0000)
#define PL_PUL0			(S_GPIO_BASE + 0x0024)

/* ---- PMU I2C addresses (scp.fex: buf[1] = 54/52) ---- */
#define AXP8191_ADDR		0x36
#define AXP515_ADDR		0x34

static int twi_up;

static void rtc_gp3(u32 v)
{
	writel(v, RTC_GP3_REG);
}

/* ------------------------------------------------------------------ */
/* TWI byte engine (vendor 0x40006c44 state machine)                   */
/* ------------------------------------------------------------------ */

static int twi_wait_int(void)
{
	unsigned int spin = TWI_SPIN;

	while (!(readl(TWI_CNTR_REG) & TWI_CNTR_INT)) {
		if (--spin == 0)
			return -1;
	}
	return 0;
}

/* W1C: write 1 to clear, then let hardware re-set it on completion */
static void twi_flag_clear(void)
{
	writel(readl(TWI_CNTR_REG) | TWI_CNTR_INT, TWI_CNTR_REG);
}

static int twi_wait_stop(void)
{
	unsigned int spin = TWI_SPIN;

	while (readl(TWI_CNTR_REG) & TWI_CNTR_STOP) {
		if (--spin == 0)
			return -1;
	}
	return 0;
}

static int twi_wait_stat(u8 want, const char *what)
{
	unsigned int spin = TWI_SPIN;

	while ((u8)readl(TWI_STAT_REG) != want) {
		if (--spin == 0) {
			uart_puts("[pwr] twi stat wait ");
			uart_puts(what);
			uart_puts(" got ");
			uart_put_hex(readl(TWI_STAT_REG) & 0xFF);
			uart_puts("\n");
			return -1;
		}
	}
	return 0;
}

static void twi_stop(void)
{
	writel((readl(TWI_CNTR_REG) & ~TWI_CNTR_INT) | TWI_CNTR_STOP,
	       TWI_CNTR_REG);
	twi_flag_clear();
	twi_wait_stop();
	(void)twi_wait_stat(TWI_STAT_IDLE, "idle");
	writel(TWI_CNTR_RUN, TWI_CNTR_REG);
}

/* start + addr byte; returns 0 or -1 */
static int twi_start_addr(u8 addr, u8 want_stat)
{
	writel((readl(TWI_CNTR_REG) & ~(TWI_CNTR_INT | 1u)) | TWI_CNTR_START,
	       TWI_CNTR_REG);
	if (twi_wait_int() != 0)
		return -1;
	if (twi_wait_stat(TWI_STAT_START, "start") != 0)
		return -1;
	writel(addr, TWI_DATA_REG);
	twi_flag_clear();
	if (twi_wait_int() != 0)
		return -1;
	return twi_wait_stat(want_stat, "addr");
}

static int twi_send_byte(u8 v, u8 want_stat)
{
	writel(v, TWI_DATA_REG);
	twi_flag_clear();
	if (twi_wait_int() != 0)
		return -1;
	return twi_wait_stat(want_stat, "data");
}

int twi_write(u8 dev, u8 reg, u8 val)
{
	int r;

	if (!twi_up)
		return -13;
	r = twi_start_addr(dev << 1, TWI_STAT_ADDR_W_ACK);
	if (r == 0)
		r = twi_send_byte(reg, TWI_STAT_DATA_ACK);
	if (r == 0)
		r = twi_send_byte(val, TWI_STAT_DATA_ACK);
	twi_stop();
	if (r != 0) {
		uart_puts("[pwr] twi_wr dev=");
		uart_put_hex(dev);
		uart_puts(" reg=");
		uart_put_hex(reg);
		uart_puts(" FAIL\n");
	}
	return r;
}

int twi_read(u8 dev, u8 reg, u8 *val)
{
	int r;

	if (!twi_up)
		return -13;
	r = twi_start_addr(dev << 1, TWI_STAT_ADDR_W_ACK);
	if (r == 0)
		r = twi_send_byte(reg, TWI_STAT_DATA_ACK);
	if (r == 0) {
		/* restart into read direction */
		writel((readl(TWI_CNTR_REG) & ~(TWI_CNTR_INT | 1u)) |
		       TWI_CNTR_START, TWI_CNTR_REG);
		twi_flag_clear();
		if (twi_wait_int() != 0)
			r = -1;
		else if (twi_wait_stat(TWI_STAT_RESTART, "restart") != 0)
			r = -1;
	}
	if (r == 0) {
		writel((dev << 1) | 1, TWI_DATA_REG);
		twi_flag_clear();
		if (twi_wait_int() != 0)
			r = -1;
		else if (twi_wait_stat(TWI_STAT_ADDR_R_ACK, "addr-r") != 0)
			r = -1;
	}
	if (r == 0) {
		/* single byte: NACK it */
		writel(readl(TWI_CNTR_REG) & ~(TWI_CNTR_AACK | TWI_CNTR_INT),
		       TWI_CNTR_REG);
		twi_flag_clear();
		if (twi_wait_int() != 0)
			r = -1;
		else if (twi_wait_stat(TWI_STAT_DATA_NACK_R, "data-r") != 0)
			r = -1;
		else
			*val = (u8)readl(TWI_DATA_REG);
	}
	twi_stop();
	return r;
}

/* ------------------------------------------------------------------ */
/* Lazy bus bring-up (vendor 0x400070d4, runs at poweroff/reset time   */
/* when Linux's r_i2c driver is already down)                          */
/* ------------------------------------------------------------------ */

static void twi_soft_reset(void)
{
	unsigned int spin = TWI_SPIN;

	writel(readl(TWI_SRST_REG) | 1u, TWI_SRST_REG);
	while ((readl(TWI_SRST_REG) & 1u) && --spin)
		;
}

static void twi_bus_clear(void)
{
	/* vendor 0x4000702c: release SCL, poll SDA up to 9 times */
	unsigned int i;

	writel(readl(TWI_LCR_REG) | 4u, TWI_LCR_REG);
	for (i = 0; i < 9; i++) {
		if (readl(TWI_LCR_REG) & 0x10)
			break;
	}
	writel(readl(TWI_LCR_REG) & ~4u, TWI_LCR_REG);
}

static void twi_pin_setup(void)
{
	/* PL0/PL1 -> func 2 (s_twi0), pull-up; read-modify-write so the
	 * PL2/PL3 console nibbles and PL4/PL7 survive (pinmux.c race note) */
	u32 shift0 = 0 * 4, shift1 = 1 * 4;
	u32 v;

	v = readl(PL_CFG0);
	v &= ~((0xFu << shift0) | (0xFu << shift1));
	v |= (2u << shift0) | (2u << shift1);
	writel(v, PL_CFG0);

	v = readl(PL_PUL0);
	v &= ~((0x3u << (0 * 2)) | (0x3u << (1 * 2)));
	v |= (0x1u << (0 * 2)) | (0x1u << (1 * 2));
	writel(v, PL_PUL0);
}

static void twi_init(void)
{
	writel(readl(R_TWI_BGR_REG) | R_TWI_BGR_GATE | R_TWI_BGR_RST,
	       R_TWI_BGR_REG);
	twi_pin_setup();
	twi_soft_reset();
	writel(TWI_CNTR_RUN, TWI_CNTR_REG);
	twi_bus_clear();
	delay_ms(10);
	if ((readl(TWI_LCR_REG) & 0xFF) != TWI_LCR_READY)
		uart_puts("[pwr] ERR:SDA is still low level!\n");
	twi_up = 1;
}

/* vendor 0x400070ac: idle means STAT 0xF8 AND both line-state bits set
 * (LCR bit4 = SDA high, bit5 = SCL high). v60-v64 had the LCR test
 * inverted, so an idle bus always failed it; harmless while delay_ms() was
 * a no-op, but once the timer worked (T30) it added 10 x 2 s to every
 * reboot/poweroff. */
static int twi_idle(void)
{
	if ((u8)readl(TWI_STAT_REG) != TWI_STAT_IDLE)
		return -1;
	if ((readl(TWI_LCR_REG) & 0x30) != 0x30)
		return -1;
	return 0;
}

static int twi_wait_idle_retry(void)
{
	unsigned int tries;

	for (tries = 0; tries < 10; tries++) {
		twi_init();			/* vendor re-inits every attempt */
		if (twi_idle() == 0)
			return 0;
		uart_puts("[pwr] wait twi bus idle loop\n");
		delay_ms(2000);
		twi_bus_clear();
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* PMU helpers (vendor wrappers 0x40007ab6/0x40007abe)                 */
/* ------------------------------------------------------------------ */

static int axp_write(u8 dev, u8 reg, u8 val)
{
	return twi_write(dev, reg, val);
}

static int axp_read(u8 dev, u8 reg, u8 *val)
{
	return twi_read(dev, reg, val);
}

/* vendor 0x4000771a: GP3=0xA501, read AXP515 reg 0x00;
 * returns 0 when bit1 is set, -1 otherwise (or on TWI error) */
static int axp515_vbus_check(void)
{
	u8 v = 0;

	rtc_gp3(0xA501);
	if (axp_read(AXP515_ADDR, 0x00, &v) != 0)
		return -1;
	return (v & 0x02) ? 0 : -1;
}

/* vendor 0x400078f8 + 0x4000775a: AXP515 reset prep.
 *   GP3=0xA401, GP3=0xA400,
 *   0x00 -> regs 0x40..0x45   (disable all AXP515 IRQ enables),
 *   0xFF -> regs 0x48..0x4D   (clear all pending IRQ status, W1C),
 *   0xC0 -> reg 0x42, 0xF0 -> reg 0x43 (re-enable button/wake IRQs only)
 */
static void axp515_reset_prep(void)
{
	u8 reg, probe;

	rtc_gp3(0xA401);
	rtc_gp3(0xA400);
	/* Orange Pi Zero 3W: nothing answers at 0x34 (every write NACKed on
	 * the address byte, measured 2026-09-23 T30) -- probe once instead of
	 * timing out 14 times on the way to the reset */
	if (axp_read(AXP515_ADDR, 0x00, &probe) != 0) {
		uart_puts("[pwr] axp515 absent, skip\n");
		return;
	}
	for (reg = 0x40; reg <= 0x45; reg++)
		axp_write(AXP515_ADDR, reg, 0x00);
	for (reg = 0x48; reg <= 0x4D; reg++)
		axp_write(AXP515_ADDR, reg, 0xFF);
	axp_write(AXP515_ADDR, 0x42, 0xC0);
	axp_write(AXP515_ADDR, 0x43, 0xF0);
	uart_puts("[pwr] reset axp515\n");
}

/* vendor 0x400075ee: the point of no return.  arg 1 -> GP3=0xA301,
 * AXP8191 reg 0x55 |= bit6, prints "reset system"; arg 0 -> GP3=0xA300,
 * reg 0x55 |= bit7, prints "poweroff system".  Both dead-loop after. */
static void __attribute__((noreturn)) exit_now(int arg)
{
	u8 v = 0;

	uart_puts("[pwr] exit arg=");
	uart_put_dec((unsigned int)arg);
	uart_puts("\n");
	rtc_gp3(0xA300u | (u32)arg);
	if (axp_read(AXP8191_ADDR, 0x55, &v) == 0) {
		u8 nv = v | (u8)(1u << (7 - arg));

		uart_puts("[pwr] axp8191 55: ");
		uart_put_hex(v);
		uart_puts(" -> ");
		uart_put_hex(nv);
		uart_puts("\n");
		if (nv != v)
			axp_write(AXP8191_ADDR, 0x55, nv);
	} else {
		uart_puts("[pwr] axp8191 55 read FAIL, write ");
		uart_put_hex((u32)(1u << (7 - arg)));
		uart_puts("\n");
		axp_write(AXP8191_ADDR, 0x55, (u8)(1u << (7 - arg)));
	}
	uart_puts(arg ? "[pwr] reset system\n" : "[pwr] poweroff system\n");
	hb_stage(arg ? 0x61 : 0x60);
	for (;;)
		__asm__ __volatile__("wfi");
}

/* vendor 0x40007974 body: cb[0] = 0x40007706: GP3=0xA201, runtime hook
 * 0x40007b42 (gated on the vendor pmu framework -- skipped here), then
 * exit(0) = the real "poweroff system" terminal state. */
static void __attribute__((noreturn)) poweroff_tail(void)
{
	rtc_gp3(0xA201);
	exit_now(0);
	/* not reached */
}

/* ------------------------------------------------------------------ */
/* Sys-op handler (vendor 0x4000aa54); returns only for op != 0..3     */
/* ------------------------------------------------------------------ */

int power_sys_op(u32 op)
{
	switch (op) {
	case 0: {
		/* vendor: GP3=0xA101 (handler) -> 0x40007a0e (twi init +
		 * idle, gated on the vendor twi flag -- we always try) ->
		 * cb[8] 0x4000766a; on its fall-through -> 0x40007974 ->
		 * cb[0] 0x40007706. */
		int vbus;

		rtc_gp3(0xA101);
		hb_stage(0x52);
		twi_wait_idle_retry();
		rtc_gp3(0xA203);
		vbus = axp515_vbus_check();
		uart_puts("[pwr] vbus check -> ");
		uart_put_dec((unsigned int)(vbus ? 1 : 0));
		uart_puts("\n");
		if (vbus == 0) {
			/* vbus present: bmu charging path -- vendor ends
			 * this with exit(1) ("fake poweroff" into charging
			 * mode on charger-powered boards) */
			uart_puts("[pwr] bmu_charging_vbus_det\n");
			axp_write(AXP8191_ADDR, 0x04, 0x08);
			axp515_reset_prep();
			exit_now(1);
			/* not reached */
		}
		/* vbus absent / AXP515 silent: real poweroff */
		uart_puts("[pwr] axp8191 exist\n");
		poweroff_tail();
		/* not reached */
	}
	case 1:
	case 2:					/* PSCI SYSTEM_RESET */
		/* vendor: GP3=0xA102 (handler) -> 0x400079c8 (twi init +
		 * idle, no gate) -> cb[4] 0x400076ce */
		rtc_gp3(0xA102);
		hb_stage(0x53);
		twi_wait_idle_retry();
		rtc_gp3(0xA202);
		axp515_reset_prep();
		axp_write(AXP8191_ADDR, 0x04, 0x01);
		exit_now(1);
		/* not reached */
	case 3:
		/* vendor: GP3=0xA103 (handler) -> 0x40007974 -> cb[0] */
		rtc_gp3(0xA103);
		hb_stage(0x55);
		twi_wait_idle_retry();
		poweroff_tail();
		/* not reached */
	default:
		return -22;
	}
}
