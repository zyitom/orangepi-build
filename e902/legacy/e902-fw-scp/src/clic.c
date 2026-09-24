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
	writeb(1, CLIC_INTIE(irq));
}

void clic_disable(unsigned int irq)
{
	writeb(0, CLIC_INTIE(irq));
}
