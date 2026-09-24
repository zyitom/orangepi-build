/*
 * S_SPI (r_spi @ 0x07092000) master driver for the A733's E902 core.
 *
 * Scope: enough of the sunxi SPI v1.3 controller to (a) prove the E902 can
 * own and program the controller at all, and (b) complete a real 8-bit SPI
 * read whose result can be checked against a known reference.
 *
 * Register offsets and bit names come from the in-tree kernel driver header
 * (bsp/drivers/spi/spi-sunxi.h); the base address, clock and reset bits come
 * from sun60iw2p1.dtsi and ccu-sun60iw2-r.c. See spi.h for the citations.
 *
 * Two test modes are supported, because the board ships with no SPI slave
 * (spi0/spi3/r_spi are all status="disabled" and nothing is wired to the
 * r_spi pins):
 *
 *   LOOPBACK  - short r_spi MOSI to MISO on the header. The bytes the E902
 *               clocks out come straight back, so the received data has a
 *               known reference value: the transmitted pattern.
 *   OPEN BUS  - no jumper: the controller still runs, TC still fires, and the
 *               receive FIFO returns whatever the floating/biased MISO pin
 *               presents. That validates the controller mechanics (clock,
 *               FIFO, counters, interrupt) but not data correctness, and the
 *               firmware reports it as such.
 */
#include "spi.h"
#include "fw.h"

static volatile unsigned int irq_count;
static volatile unsigned int tc_count;
static volatile unsigned int err_count;
static unsigned int last_cdr2;

/* The same Allwinner SPI IP sits in the CPUS domain (S_SPI) and the CPUX
 * domain (SPI0..4); every register access goes through spi_base so the
 * code below drives either one (spi_use()). */
static u32 spi_base = R_SPI_BASE;

void spi_use(u32 base)
{
	spi_base = base;
}

u32 spi_read_reg(unsigned int off)
{
	return readl(spi_base + off);
}

static void spi_wr(unsigned int off, u32 v)
{
	writel(v, spi_base + off);
}

static void r_spi_clocks_on(void)
{
	u32 v;

	/* Reset then clock, the same order every R-CCU BGR block wants.
	 * 0x015C holds bus-gate in bit0 and reset in bit16. */
	v = readl(R_SPI_BGR_REG);
	v |= (1u << R_SPI_BGR_RST_BIT);
	writel(v, R_SPI_BGR_REG);
	v |= (1u << R_SPI_BGR_GATE_BIT);
	writel(v, R_SPI_BGR_REG);

	/* Module clock: put the mux on pll-ref (24 MHz) with the /1 dividers
	 * so the input clock is a known 24 MHz regardless of what boot0 left
	 * behind, then open the gate (bit31). NOT dcxo: the Zero 3W crystal
	 * is 26 MHz (Linux clk_summary: dcxo 26000000, pll-ref 24000000), so
	 * dcxo made every SPI rate 8.3 % fast (T31). */
	v = readl(R_SPI_CLK_REG);
	v &= ~(R_SPI_CLK_MUX_MASK << R_SPI_CLK_MUX_SHIFT);
	v |= (R_SPI_MUX_PLL_REF << R_SPI_CLK_MUX_SHIFT);
	v &= ~(R_SPI_CLK_M_MASK << R_SPI_CLK_M_SHIFT);	/* M = 0 -> /1 */
	v &= ~(R_SPI_CLK_N_MASK << R_SPI_CLK_N_SHIFT);	/* N = 0 -> /1 */
	v |= R_SPI_CLK_GATE;
	writel(v, R_SPI_CLK_REG);
}

/*
 * Program the divider for sclk_hz, given a 24 MHz module clock.
 * CDR2 (DRS=1): sclk = 24 MHz / (2 * (CDR2 + 1)).
 */
static void spi_set_sclk(unsigned int sclk_hz)
{
	unsigned int div = 24000000u / (2u * sclk_hz);
	u32 reg;

	if (div == 0)
		div = 1;
	if (div > 256)
		div = 256;
	div -= 1;			/* register holds CDR2 = div - 1 */
	last_cdr2 = div;

	reg = readl(spi_base + SPI_CLK_CTL_REG);
	reg &= ~(SPI_CLK_CTL_CDR2 | SPI_CLK_CTL_CDR1 | SPI_CLK_CTL_DRS);
	reg |= (div & 0xFF) << 0;	/* CDR2 */
	reg |= SPI_CLK_CTL_DRS;		/* rate 2 */
	spi_wr(SPI_CLK_CTL_REG, reg);
}

/*
 * Master, mode 0 (CPOL=0/CPHA=0), CS0 asserted low, MSB first.
 * Mode/PHA/POL/CS are the "关键参数" the acceptance record asks for.
 */
static void spi_set_mode0(void)
{
	u32 tc = 0;

	tc |= SPI_TC_SS0;		/* chip select 0 */
	tc |= SPI_TC_SPOL;		/* active low (SPI_TC_SPOL default 1) */
	/* PHA=0, POL=0 -> SPI mode 0 */
	spi_wr(SPI_TC_REG, tc);
}

/* controller setup shared by S_SPI and the CPUX SPIs; clocks must be on */
static int spi_ctrl_init(unsigned int sclk_hz)
{
	u32 v;

	spi_set_sclk(sclk_hz);

	/* Soft reset, then enable as master with transmit-stop enabled. */
	spi_wr(SPI_GC_REG, SPI_GC_SRST);
	v = SPI_GC_EN | SPI_GC_MODE | SPI_GC_TP_EN;
	spi_wr(SPI_GC_REG, v);

	/* No interrupts by default; clear any stale status. */
	spi_wr(SPI_INT_CTL_REG, 0);
	spi_wr(SPI_INT_STA_REG, 0xFFFFFFFF);

	/* Reset both FIFOs, no DMA requests, default trigger levels. */
	spi_wr(SPI_FIFO_CTL_REG,
	       SPI_FIFO_CTL_RX_RST | SPI_FIFO_CTL_TX_RST |
	       (1u << 0) | (0x40u << 16));

	spi_set_mode0();

	/* Sanity: the controller must report itself enabled and in master
	 * mode, otherwise the access was dropped (clock gate still closed). */
	v = spi_read_reg(SPI_GC_REG);
	if (!(v & SPI_GC_EN) || !(v & SPI_GC_MODE))
		return -1;
	return 0;
}

int spi_init(unsigned int sclk_hz)
{
	spi_use(R_SPI_BASE);
	r_spi_clocks_on();
	return spi_ctrl_init(sclk_hz);
}

/*
 * CPUX-domain SPI3 on the 40-pin header, driven cross-domain by the E902.
 * Nothing on the Linux side may own it: spi@2543000 is "disabled" in the
 * board DT and PE0..PE3 are UNCLAIMED in pinctrl (checked 2026-09-23).
 *
 *   40-pin 24 = PE0 SPI3-CS0     40-pin 23 = PE1 SPI3-CLK
 *   40-pin 19 = PE2 SPI3-MOSI    40-pin 21 = PE3 SPI3-MISO
 *
 * Steps are exactly what the Linux driver would do, done by hand:
 *   1. main CCU: SPI3 bus gate + reset release (0x0F24), module clock
 *      sys24M / 1 + gate (0x0F20)             (ccu-sun60iw2.c)
 *   2. CPUX PIO: PE0..PE3 -> function 5        (pinctrl-sun60iw2.c)
 *   3. controller: identical to S_SPI         (spi_ctrl_init)
 * No interrupt: GIC->E902 forwarding (GINTC) is undocumented, so the
 * transfer is polled (spi_transfer already is).
 */
int cx_spi3_init(unsigned int sclk_hz)
{
	u32 v;

	/* 1. clocks: reset first, then bus gate, then module clock */
	v = readl(CCU_SPI3_BGR_REG);
	writel(v | (1u << 16), CCU_SPI3_BGR_REG);
	writel(v | (1u << 16) | 1u, CCU_SPI3_BGR_REG);
	writel(CCU_SPI3_CLK_GATE | (0u << 24), CCU_SPI3_CLK_REG); /* sys24M, M=N=0 */

	/* 2. pins: read-modify-write, PE4..PE7 belong to others (PE7 is
	 *    claimed by Linux' twi@251b000) */
	v = readl(CX_PIO_PE_CFG0);
	v &= ~0xFFFFu;
	v |= (PE_FUNC_SPI3 << 0) | (PE_FUNC_SPI3 << 4) |
	     (PE_FUNC_SPI3 << 8) | (PE_FUNC_SPI3 << 12);
	writel(v, CX_PIO_PE_CFG0);

	/* 3. controller */
	spi_use(CX_SPI3_BASE);
	return spi_ctrl_init(sclk_hz);
}

void spi_dump_regs(void)
{
	uart_puts("SPI VER=");    uart_put_hex(spi_read_reg(SPI_VER_REG));
	uart_puts(" GC=");        uart_put_hex(spi_read_reg(SPI_GC_REG));
	uart_puts(" TC=");        uart_put_hex(spi_read_reg(SPI_TC_REG));
	uart_puts("\nSPI CLK_CTL="); uart_put_hex(spi_read_reg(SPI_CLK_CTL_REG));
	uart_puts(" FIFO_STA=");  uart_put_hex(spi_read_reg(SPI_FIFO_STA_REG));
	uart_puts(" INT_STA=");   uart_put_hex(spi_read_reg(SPI_INT_STA_REG));
	uart_puts("\n");
}

/* Polled 8-bit master transfer. tx may be NULL (send 0xFF), rx may be NULL. */
int spi_transfer(const u8 *tx, u8 *rx, unsigned int len)
{
	unsigned int i, got, spin;
	u32 v;

	if (len == 0 || len > SPI_MAX_BURST)
		return -1;

	/* Clear FIFOs and stale status before every transfer. */
	spi_wr(SPI_FIFO_CTL_REG, SPI_FIFO_CTL_RX_RST | SPI_FIFO_CTL_TX_RST |
	       (1u << 0) | (0x40u << 16));
	spi_wr(SPI_INT_STA_REG, 0xFFFFFFFF);

	/* Push the transmit bytes. The FIFO is 32-bit wide; byte mode uses
	 * the low 8 bits of each word (same as the kernel driver). */
	for (i = 0; i < len; i++) {
		u32 b = tx ? tx[i] : 0xFFu;

		spi_wr(SPI_TXDATA_REG, b & 0xFFu);
	}

	/* Total burst = tx (no dummy bytes) -> BC = len; tx count = len. */
	spi_wr(SPI_BURST_CNT_REG, len & SPI_BC_CNT_MASK);
	spi_wr(SPI_TRANSMIT_CNT_REG, len & SPI_TC_CNT_MASK);

	/* Start. */
	v = spi_read_reg(SPI_TC_REG);
	spi_wr(SPI_TC_REG, v | SPI_TC_XCH);

	/* Wait for transfer-complete. Bounded so a wedged controller (wrong
	 * clock, no gate) cannot hang the firmware. */
	for (spin = 0; spin < 2000000; spin++) {
		v = spi_read_reg(SPI_INT_STA_REG);
		if (v & SPI_INT_TC)
			break;
	}
	if (!(v & SPI_INT_TC)) {
		err_count++;
		spi_wr(SPI_TC_REG, spi_read_reg(SPI_TC_REG) & ~SPI_TC_XCH);
		return -2;		/* timeout */
	}

	spi_wr(SPI_INT_STA_REG, SPI_INT_TC);

	/* Drain the receive FIFO. */
	got = (spi_read_reg(SPI_FIFO_STA_REG) & SPI_FIFO_STA_RX_CNT) & 0xFF;
	if (rx) {
		for (i = 0; i < len; i++) {
			if (i < got)
				rx[i] = (u8)(spi_read_reg(SPI_RXDATA_REG) & 0xFF);
			else
				rx[i] = 0;
		}
	} else {
		for (i = 0; i < got; i++)
			(void)spi_read_reg(SPI_RXDATA_REG);
	}

	tc_count++;
	return (int)got;
}

/*
 * Self-test: clock out a fixed, recognisable pattern and capture what came
 * back. With MOSI->MISO shorted the two must match; on an open bus they will
 * not, and the caller reports the raw bytes either way.
 */
int spi_selftest(u8 *rx_out, unsigned int *rx_len)
{
	static const u8 pattern[8] = { 0xA5, 0x5A, 0x01, 0xFE, 0x00, 0xFF, 0x3C, 0xC3 };
	u8 rx[8];
	int n;

	n = spi_transfer(pattern, rx, sizeof(pattern));
	if (n < 0)
		return n;

	if (rx_out) {
		unsigned int i;

		for (i = 0; i < sizeof(pattern); i++)
			rx_out[i] = rx[i];
	}
	if (rx_len)
		*rx_len = (n > 0) ? (unsigned int)n : 0;
	return 0;
}

/* ---- interrupt path ---- */

void spi_irq_enable(void)
{
	spi_wr(SPI_INT_STA_REG, 0xFFFFFFFF);		/* clear stale */
	spi_wr(SPI_INT_CTL_REG, SPI_INT_RX_RDY | SPI_INT_TC | SPI_INT_ERR);
}

/*
 * CLIC IRQ 38. Keep it minimal: acknowledge status, bump counters, let the
 * main loop poll them.
 *
 * NOT enabled by main() (v63): spi_transfer() is a polled driver that
 * waits for INT_TC and then reads the RX FIFO itself. This ISR clears
 * INT_TC and drains the RX FIFO, so with it live every polled transfer
 * would lose its data and time out. Only turn it on together with an
 * interrupt-driven transfer path.
 */
void __attribute__((interrupt("machine"))) spi_isr(void)
{
	u32 sta = spi_read_reg(SPI_INT_STA_REG);

	if (sta & SPI_INT_ERR)
		err_count++;
	if (sta & SPI_INT_TC)
		tc_count++;
	if (sta & SPI_INT_RX_RDY) {
		while ((spi_read_reg(SPI_FIFO_STA_REG) & SPI_FIFO_STA_RX_CNT) & 0xFF)
			(void)spi_read_reg(SPI_RXDATA_REG);
	}
	spi_wr(SPI_INT_STA_REG, sta);
	irq_count++;
}

unsigned int spi_irq_count(void) { return irq_count; }
unsigned int spi_tc_count(void)  { return tc_count; }
unsigned int spi_err_count(void) { return err_count; }
