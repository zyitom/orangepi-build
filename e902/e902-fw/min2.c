/*
 * min2.c - minimal "did the core start at OUR vector?" probe.
 *
 * Deliberately does nothing except write a magic and a free-running counter
 * to the SRAM scratch block, so it cannot be confused with a firmware bug in
 * peripheral bring-up. soc_early_init() is intentionally empty: start.S calls
 * it before main(), and if it touched a gated peripheral it could trap before
 * the scratch was ever written.
 *
 * Scratch: E902 view 0x4001E000 (same window heartbeat.c uses).
 *   word0 = 0xE902C0DE
 *   word1 = incrementing counter
 *
 * If word0 stays 0 after a load, the core did not begin executing at the
 * address we programmed into E902_RST_START_ADDR -- which separates
 * "reset/start mechanism does not work" from "our firmware crashed".
 */
#define SCRATCH 0x4001E000u

void soc_early_init(void)
{
	/* intentionally empty */
}

void main(void)
{
	volatile unsigned int *p = (volatile unsigned int *)SCRATCH;
	unsigned int n = 0;

	p[0] = 0xE902C0DEu;
	p[1] = 0;
	for (;;)
		p[1] = ++n;
}
