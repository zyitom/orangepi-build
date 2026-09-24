/*
 * CLIC setup. The CLIC lives in the E902's tightly-coupled IP window
 * (0xE0000000..0xEFFFFFFF, handled inside the core -- integration manual
 * ch.4), so it is not part of the SoC address map and is identical on any
 * E902 integration. Base 0xE0800000 per E902 manual 10.2, confirmed by
 * disassembling the vendor scp.fex (lui a4,0xe0801 -> CLICINTIE[0]).
 *
 * Per-interrupt control is byte-wide:
 *   INTIP   [i]  pending
 *   INTIE   [i]  enable
 *   INTATTR [i]  bit0 = vectored, bit2:1 = trigger type
 *   INTCTL  [i]  priority/level, high bits significant
 */
#include "fw.h"

extern unsigned int vector_table[];

/*
 * E902 wakeup masks (manual 5.2.4.5/6): one bit per CLIC source >= 16,
 * MASK0 bit0 = irq 16, MASK1 bit0 = irq 48. They gate whether a pending
 * source can wake the core out of wfi. The vendor sets the bit for every
 * interrupt it enables (scp.fex 0x40005360, called from its irq_enable at
 * 0x40009340) and clears it on disable; mirror that.
 */
static void wakeup_mask(unsigned int irq, int on)
{
	u32 reg, bit;

	if (irq < 16 || irq >= 80)
		return;
	if (irq < 48) {
		reg = E902_WAKEUP_MASK0_REG;
		bit = 1u << (irq - 16);
	} else {
		reg = E902_WAKEUP_MASK1_REG;
		bit = 1u << (irq - 48);
	}
	if (on)
		writel(readl(reg) | bit, reg);
	else
		writel(readl(reg) & ~bit, reg);
}

static volatile unsigned int spurious_count;
static volatile unsigned int spurious_last;

/*
 * Vector-table default. A level-triggered source that is enabled without a
 * handler would otherwise re-enter forever (an "mret only" stub never
 * clears the cause) and starve the main loop -- which then stops draining
 * the mailbox and wedges bl31. Mask the source and count it instead.
 */
void __attribute__((interrupt("machine"))) default_isr(void)
{
	u32 cause;
	unsigned int irq;

	__asm__ __volatile__("csrr %0, mcause" : "=r"(cause));
	irq = cause & 0xFFFu;
	if (irq < 256) {
		writeb(0, CLIC_INTIE(irq));
		wakeup_mask(irq, 0);
	}
	spurious_last = irq;
	spurious_count++;
	hb_set(HB_SPURIOUS, (irq << 16) | (spurious_count & 0xFFFFu));
}

void clic_set_handler(unsigned int irq, void (*fn)(void))
{
	vector_table[irq] = (unsigned int)fn;
}

void clic_enable(unsigned int irq, int edge_triggered)
{
	u8 attr = CLIC_INTATTR_VECTORED;

	if (edge_triggered)
		attr |= CLIC_INTATTR_TRIG_EDGE;

	writeb(attr, CLIC_INTATTR(irq));

	/* Mid-range priority. INTCTL is left-justified: with 8 implemented
	 * bits, 0x80 sits in the middle of the range. */
	writeb(0x80, CLIC_INTCTL(irq));

	/* Clear any stale pending state, then enable */
	writeb(0, CLIC_INTIP(irq));
	wakeup_mask(irq, 1);
	writeb(1, CLIC_INTIE(irq));
}

void clic_disable(unsigned int irq)
{
	writeb(0, CLIC_INTIE(irq));
	wakeup_mask(irq, 0);
}
