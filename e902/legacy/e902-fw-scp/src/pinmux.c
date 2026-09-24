/*
 * Pin mux for S_UART0 on PL2 (TX) / PL3 (RX).
 *
 * This has to be done here, not on the Linux side. The board DTS looks like
 * it hands the pins over -- there is an "arisc_config { s_uart_config { ... } }"
 * node naming PL2/PL3 with function 3 -- but nothing in this kernel or u-boot
 * tree parses that node; it is a leftover from the sys_config.fex era. The
 * commented-out "&s_uart0" blocks in the board DTS are dead too: the label
 * does not exist, S_UART0 is called "uart7" in sun60iw2p1.dtsi (uart@7080000).
 * So if we do not set the mux, nobody does.
 *
 * RACE WARNING -- reduced, but not zero
 * PL_CFG0 packs eight pins into one 32-bit word, four bits each:
 *
 *   [ 3: 0] PL0   s_twi0  <- AXP515(0x34)/AXP8191(0x36) PMIC bus
 *   [ 7: 4] PL1   s_twi0  <- same
 *   [11: 8] PL2   ours (0x3 = S-UART0-TX, schematic net "SCPU-TX")
 *   [15:12] PL3   ours (0x3 = S-UART0-RX, schematic net "SCPU-RX")
 *   [19:16] PL4   s_irrx (IR receiver, schematic confirms)
 *   [23:20] PL5   s_twi2 SDA, wired to 40-pin header pin 27
 *   [27:24] PL6   unused
 *   [31:28] PL7   status LED
 *
 * Good news, verified in this tree: the ONLY DTS pinctrl groups naming PL2/PL3
 * are uart7_pins_active/sleep, and uart7 (which IS S_UART0, 0x07080000) is
 * status="disabled". A disabled node's pinctrl groups are never applied, so
 * nothing on the Linux side ever programs the PL2/PL3 nibbles. The board
 * schematic agrees these pins are the SCP's own console.
 *
 * Remaining risk: Linux still read-modify-writes the same *word* when it
 * configures PL0/PL1 (PMIC I2C), PL4 (IR) or PL7 (LED). If its write lands
 * between our read and our write we could clobber those nibbles -- and PL0/PL1
 * carry the PMIC bus, which powers the CPU cores.
 *
 * What keeps this survivable:
 *   - Linux writes PL_CFG0 only when a pin's *function* changes: at driver
 *     probe and across suspend/resume, not at runtime. The LED on PL7 blinks
 *     via PL_DAT (0x0010), a different register.
 *   - We touch it exactly once, during firmware init, and only the two
 *     nibbles nobody else owns.
 *
 * So: load the firmware after Linux has finished booting. set_pl_function()
 * reads back and reports, and main() prints the whole word so you can eyeball
 * whether PL0/PL1 still look sane.
 */
#include "fw.h"

#define S_GPIO_BASE	0x07025000
#define PL_CFG0		(S_GPIO_BASE + 0x0000)	/* PL0..PL7  */
#define PL_CFG1		(S_GPIO_BASE + 0x0004)	/* PL8..PL13 */
#define PL_DAT		(S_GPIO_BASE + 0x0010)
#define PL_PUL0		(S_GPIO_BASE + 0x0024)

#define PL_FUNC_INPUT		0x0
#define PL_FUNC_OUTPUT		0x1
#define PL_FUNC_S_UART0		0x3	/* TX on PL2, RX on PL3 */
#define PL_FUNC_IO_DISABLE	0xF

#define PULL_NONE	0x0
#define PULL_UP		0x1

/* Returns 1 if the read-back matches what we asked for. */
static int set_pl_function(unsigned int pin, u32 func)
{
	u32 shift = (pin & 7) * 4;
	u32 mask = 0xFu << shift;
	u32 v;

	v = readl(PL_CFG0);
	v = (v & ~mask) | ((func & 0xF) << shift);
	writel(v, PL_CFG0);

	v = readl(PL_CFG0);
	return ((v & mask) >> shift) == (func & 0xF);
}

static void set_pl_pull(unsigned int pin, u32 pull)
{
	u32 shift = (pin & 0xF) * 2;
	u32 mask = 0x3u << shift;
	u32 v = readl(PL_PUL0);

	writel((v & ~mask) | ((pull & 0x3) << shift), PL_PUL0);
}

/*
 * Returns 0 on success, -1 if the mux did not stick (someone else is
 * fighting us for PL_CFG0, or the register is secured against us).
 */
int pinmux_s_uart0(void)
{
	int ok = 1;

	/* RX needs a pull-up so an unconnected pin idles high instead of
	 * generating a stream of framing errors. */
	set_pl_pull(3, PULL_UP);

	ok &= set_pl_function(2, PL_FUNC_S_UART0);	/* TX */
	ok &= set_pl_function(3, PL_FUNC_S_UART0);	/* RX */

	return ok ? 0 : -1;
}

/* For reporting: the raw PL_CFG0 word, so the banner can show whether
 * PL0/PL1 (the PMIC bus) still look sane after we touched the register. */
u32 pinmux_read_pl_cfg0(void)
{
	return readl(PL_CFG0);
}
