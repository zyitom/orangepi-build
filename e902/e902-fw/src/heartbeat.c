/*
 * heartbeat.c - "is the E902 actually executing our code?" probe.
 *
 * The board's USB-TTL on /dev/ttyUSB0 is wired to the *Linux* console
 * (uart0, PB4/PB5), NOT to the E902's S_UART0 (PL2/PL3). So during bring-up
 * there is usually no console on the little core's UART. This module gives a
 * second, independent observation channel that needs no extra hardware:
 * a small status block at a fixed SRAM address the ARM can read through
 * /dev/mem.
 *
 * Where: E902 view 0x4001E000. That sits above the vendor scp.fex image
 * (0x40004000 + 0x19DB8 = 0x4001DDB8) and below both this firmware's own
 * load region (0x40020000) and the vendor's stack top (0x4002F000), so it
 * overlaps neither.
 *
 * ARM side reads it with the project's own tool:
 *   sudo python3 awdevmem.py dump --e902 0x4001E000 --count 32
 *
 * Word layout is declared in fw.h (HB_MAGIC_IDX .. HB_SPI_RX1).
 */
#include "a733.h"
#include "fw.h"

#define HB_BASE		0x4001E000u
#define HB_MAGIC	0xE902C0DEu
#define HB_N		HB_WORDS

static volatile u32 *const hb = (volatile u32 *)HB_BASE;

void hb_init(void)
{
	unsigned int i;

	for (i = 0; i < HB_N; i++)
		hb[i] = 0;
	hb[HB_MAGIC_IDX] = HB_MAGIC;
}

void hb_set(unsigned int idx, u32 v)
{
	if (idx < HB_N)
		hb[idx] = v;
}

u32 hb_get(unsigned int idx)
{
	return (idx < HB_N) ? hb[idx] : 0;
}

void hb_stage(u32 s)
{
	hb[HB_STAGE] = s;
}

void hb_tick(void)
{
	hb[HB_SEQ]++;
}
