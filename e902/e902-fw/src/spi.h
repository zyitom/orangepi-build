/*
 * CPUS-domain SPI controller for the A733 ("r_spi" in the device tree,
 * compatible allwinner,sunxi-spi-v1.3), from the E902's point of view.
 *
 * Every register offset and bit field below is copied from the in-tree
 * kernel driver, which is the authoritative description of this exact IP:
 *
 *   kernel/orange-pi-6.6-sun60iw2/bsp/drivers/spi/spi-sunxi.h
 *     SPI_VER_REG / SPI_GC_REG / SPI_TC_REG / ... and the SPI_GC_x, SPI_TC_x,
 *     SPI_INT_x, SPI_FIFO_x and SPI_CLK_x bit macros.
 *
 * Where the controller lives, and what clocks it needs, comes from
 *   arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi  (r_spi: spi@7092000)
 *   bsp/drivers/clk/sunxi-ng/ccu-sun60iw2-r.c      (r_spi_clk, r_spi_bus_clk,
 *                                                   RST_BUS_R_SPI)
 */
#ifndef __SPI_H__
#define __SPI_H__

#include "a733.h"

/* ---- node (dtsi: r_spi: spi@7092000) ---- */
#define R_SPI_BASE		0x07092000

/* CPUX-domain SPI3 (40-pin header), driven cross-domain -- see spi.c */
#define CX_SPI3_BASE		0x02543000
#define CCU_SPI3_CLK_REG	(CCU_BASE + 0x0F20)	/* gate31 mux[26:24] N[12:8] M[4:0] */
#define CCU_SPI3_CLK_GATE	(1u << 31)
#define CCU_SPI3_BGR_REG	(CCU_BASE + 0x0F24)	/* bit0 gate, bit16 reset */
#define CX_PIO_BASE		0x02000000
#define CX_PIO_PE_CFG0		(CX_PIO_BASE + 0x80 + 4 * 0x80)	/* bank E, PE0..PE7 */
#define PE_FUNC_SPI3		5u

/* ---- R-CCU bits for r_spi ----
 * ccu-sun60iw2-r.c definitions, transcribed without the C comment markers:
 *
 *   SUNXI_CCU_MP_WITH_MUX_GATE_NO_INDEX(r_spi_clk, "r-spi", ..., 0x0150,
 *       M field at bit 0 width 5, N field at bit 8 width 5,
 *       mux field at bit 24 width 3, gate = BIT(31));
 *   SUNXI_CCU_GATE(r_spi_bus_clk, "r-spi-bus", "dcxo", 0x015C, BIT(0), ...);
 *   [RST_BUS_R_SPI] = { 0x015C, BIT(16) };
 *
 * Note 0x015C carries both the bus gate (bit0) and the reset (bit16) --
 * the same BGR layout every other R-CCU block uses.
 */
#define R_SPI_CLK_REG		(R_CCU_BASE + 0x0150)
#define R_SPI_CLK_GATE		(1u << 31)
#define R_SPI_CLK_MUX_SHIFT	24
#define R_SPI_CLK_MUX_MASK	0x7u
#define R_SPI_CLK_N_SHIFT	8
#define R_SPI_CLK_N_MASK	0x1Fu
#define R_SPI_CLK_M_SHIFT	0
#define R_SPI_CLK_M_MASK	0x1Fu

#define R_SPI_BGR_REG		(R_CCU_BASE + 0x015C)
#define R_SPI_BGR_RST_BIT	16
#define R_SPI_BGR_GATE_BIT	0

/* Parent mux values, from r_spi_parents[] = { "dcxo", "pll-peri0-200m",
 * "pll-peri0-300m", "pll-peri1-300m", "pll-ref" }. */
#define R_SPI_MUX_DCXO		0	/* crystal: 26 MHz on the Zero 3W, not 24 */
#define R_SPI_MUX_PLL_REF	4u	/* r_spi_parents[4]: 24 MHz (dcxo is 26 MHz on this board, T31) */
#define R_SPI_MUX_PERI0_200M	1
#define R_SPI_MUX_PERI0_300M	2

/* ---- interrupt (E902 view) ----
 * A733 manual Table 12-2 lists S_SPI as CLIC IRQ 38; the Linux side sees the
 * same line as GIC SPI 210 (sun60iw2p1.dtsi: interrupts = GIC_SPI 210 ...).
 * The 38 value is the one already recorded in doc/e902/DESIGN-NOTES.md; it is
 * the number this firmware programs into the CLIC.
 */
#define IRQ_S_SPI		38

/* ---- register offsets (spi-sunxi.h) ---- */
#define SPI_VER_REG		0x00
#define SPI_GC_REG		0x04
#define SPI_TC_REG		0x08
#define SPI_INT_CTL_REG		0x10
#define SPI_INT_STA_REG		0x14
#define SPI_FIFO_CTL_REG	0x18
#define SPI_FIFO_STA_REG	0x1C
#define SPI_WAIT_CNT_REG	0x20
#define SPI_CLK_CTL_REG		0x24
#define SPI_SAMPLE_DELAY_REG	0x28
#define SPI_BURST_CNT_REG	0x30
#define SPI_TRANSMIT_CNT_REG	0x34
#define SPI_BCC_REG		0x38
#define SPI_TXDATA_REG		0x200
#define SPI_RXDATA_REG		0x300

/* Global control (spi-sunxi.h SPI_GC_x) */
#define SPI_GC_EN		(1u << 0)
#define SPI_GC_MODE		(1u << 1)	/* 1 = master */
#define SPI_GC_TP_EN		(1u << 7)
#define SPI_GC_SRST		(1u << 31)

/* Transfer control (SPI_TC_x) */
#define SPI_TC_PHA		(1u << 0)
#define SPI_TC_POL		(1u << 1)
#define SPI_TC_SPOL		(1u << 2)
#define SPI_TC_SSCTL		(1u << 3)
#define SPI_TC_SS_MASK		(0x3u << 4)
#define SPI_TC_SS0		(0u << 4)
#define SPI_TC_SS_OWNER		(1u << 6)
#define SPI_TC_SS_LEVEL		(1u << 7)
#define SPI_TC_DHB		(1u << 8)
#define SPI_TC_DDB		(1u << 9)
#define SPI_TC_RPSM		(1u << 10)
#define SPI_TC_SDC		(1u << 11)
#define SPI_TC_FBS		(1u << 12)
#define SPI_TC_SDM		(1u << 13)
#define SPI_TC_SDC1		(1u << 15)
#define SPI_TC_XCH		(1u << 31)

/* Interrupt control/status (SPI_INTEN_x / SPI_INT_STA_x) */
#define SPI_INT_RX_RDY		(1u << 0)
#define SPI_INT_RX_EMP		(1u << 1)
#define SPI_INT_RX_FULL		(1u << 2)
#define SPI_INT_TX_ERQ		(1u << 4)
#define SPI_INT_TX_EMP		(1u << 5)
#define SPI_INT_TX_FULL		(1u << 6)
#define SPI_INT_RX_OVF		(1u << 8)
#define SPI_INT_RX_UDR		(1u << 9)
#define SPI_INT_TX_OVF		(1u << 10)
#define SPI_INT_TX_UDR		(1u << 11)
#define SPI_INT_TC		(1u << 12)
#define SPI_INT_SSI		(1u << 13)
#define SPI_INT_ERR		(SPI_INT_TX_OVF | SPI_INT_RX_UDR | SPI_INT_RX_OVF)

/* FIFO control/status (SPI_FIFO_CTL_x / SPI_FIFO_STA_x) */
#define SPI_FIFO_CTL_RX_LEVEL	(0xFFu << 0)
#define SPI_FIFO_CTL_RX_DRQEN	(1u << 8)
#define SPI_FIFO_CTL_RX_RST	(1u << 15)
#define SPI_FIFO_CTL_TX_LEVEL	(0xFFu << 16)
#define SPI_FIFO_CTL_TX_DRQEN	(1u << 24)
#define SPI_FIFO_CTL_TX_RST	(1u << 31)

#define SPI_FIFO_STA_RX_CNT	(0xFFu << 0)
#define SPI_FIFO_STA_TX_CNT	(0xFFu << 16)

/* Clock control (SPI_CLK_CTL_x) : SPI_CLK = mod_clk / (2*(CDR2+1)) when
 * DRS=1, or mod_clk / 2^CDR1 when DRS=0. */
#define SPI_CLK_CTL_CDR2	(0xFFu << 0)
#define SPI_CLK_CTL_CDR1	(0xFu << 8)
#define SPI_CLK_CTL_DRS		(1u << 12)

/* Burst / transmit counters */
#define SPI_BC_CNT_MASK		(0xFFFFFFu << 0)	/* total burst bytes */
#define SPI_TC_CNT_MASK		(0xFFFFFFu << 0)	/* tx bytes */

/* FIFO depth, spi-sunxi.h SPI_FIFO_DEPTH / MAX_FIFU */
#define SPI_FIFO_DEPTH		128
#define SPI_MAX_BURST		64	/* what we push per transfer in this driver */

/* spi.c */
int  spi_init(unsigned int sclk_hz);
u32  spi_read_reg(unsigned int off);
void spi_dump_regs(void);
int  spi_transfer(const u8 *tx, u8 *rx, unsigned int len);
int  spi_selftest(u8 *rx_out, unsigned int *rx_len);
void spi_irq_enable(void);
void spi_use(u32 base);
int cx_spi3_init(unsigned int sclk_hz);
void spi_isr(void);
unsigned int spi_irq_count(void);
unsigned int spi_tc_count(void);
unsigned int spi_err_count(void);

#endif /* __SPI_H__ */
