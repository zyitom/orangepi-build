/* vinreg - read/write SoC registers through /dev/mem (evidence tool for T14)
 *   vinreg r <addr> [n]           read n words
 *   vinreg s <addr> <len>         scan len bytes, print only non-zero words
 *   vinreg w <addr> <value>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>

static int fd = -1;
static unsigned long win_base; static volatile uint8_t *map; static unsigned long win_len;
static const unsigned long PS = 4096;

static void *mapto(unsigned long addr)
{
	unsigned long base = addr & ~(PS - 1);
	if (map && addr >= win_base && addr + 4 <= win_base + win_len)
		return (void *)(map + (addr - win_base));
	if (map)
		munmap((void *)map, win_len);
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("open /dev/mem"); exit(2); }
	win_base = base;
	win_len = 0x10000;                       /* 64 KB window */
	map = mmap(NULL, win_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, win_base);
	if (map == MAP_FAILED) { perror("mmap"); exit(2); }
	return (void *)(map + (addr - win_base));
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: vinreg r <addr> [n] | vinreg s <addr> <len> | vinreg w <addr> <val>\n"); return 1; }
	unsigned long addr = strtoul(argv[2], NULL, 0);
	if (!strcmp(argv[1], "r")) {
		int n = argc > 3 ? atoi(argv[3]) : 1, i;
		for (i = 0; i < n; i++) {
			unsigned long a = addr + i * 4;
			printf("0x%08lx = 0x%08x\n", a, *(volatile uint32_t *)mapto(a));
		}
		return 0;
	}
	if (!strcmp(argv[1], "s")) {
		unsigned long len = argc > 3 ? strtoul(argv[3], NULL, 0) : 0x1000, a;
		int nz = 0;
		for (a = addr; a < addr + len; a += 4) {
			uint32_t v = *(volatile uint32_t *)mapto(a);
			if (v) { printf("0x%08lx = 0x%08x\n", a, v); nz++; }
		}
		printf("-- nonzero words: %d --\n", nz);
		return 0;
	}
	if (!strcmp(argv[1], "w")) {
		unsigned long v = strtoul(argv[3], NULL, 0);
		*(volatile uint32_t *)mapto(addr) = (uint32_t)v;
		printf("0x%08lx <- 0x%08x (readback 0x%08x)\n", addr, (unsigned)v,
		       *(volatile uint32_t *)mapto(addr));
		return 0;
	}
	fprintf(stderr, "bad mode\n");
	return 1;
}
