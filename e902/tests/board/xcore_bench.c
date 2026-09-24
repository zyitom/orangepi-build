/*
 * Cross-core benchmark, ARM (Linux) side. Run on the board as root while the
 * system is idle (it talks to E902 over mailbox channel 3, which bl31 also
 * uses for its RPCs).
 *   gcc -O2 -o xcore_bench xcore_bench.c && sudo ./xcore_bench
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static volatile uint32_t *map(int fd, off_t pa, size_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pa);

	if (p == MAP_FAILED) { perror("mmap"); exit(1); }
	return p;
}

static uint64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

static int cmp64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

static volatile uint32_t *ts, *sram, *cpus, *cpux;

static uint64_t ts_read(void)
{
	uint32_t h, l;

	do { h = ts[1]; l = ts[0]; } while (ts[1] != h);
	return ((uint64_t)h << 32) | l;
}

/* one synchronous ping: [hdr][n][n words] -> wait for the same back */
static int ping(unsigned int n, uint32_t tag)
{
	uint32_t hdr = 0x00010202u | ((tag & 0x7F) << 24), got;
	unsigned int i, spins = 0;

	cpus[0x7C / 4] = hdr;
	cpus[0x7C / 4] = n;
	for (i = 0; i < n; i++) {
		while ((cpus[0x6C / 4] & 0xF) >= 8)	/* E902 RX FIFO full */
			;
		cpus[0x7C / 4] = tag + i;
	}
	for (i = 0; i < n + 2; i++) {
		while ((cpux[0x6C / 4] & 0xF) == 0)
			if (++spins > 50000000) return -1;
		got = cpux[0x7C / 4];
		if (i == 0 && got != (hdr & 0x00FFFFFF)) return -2;
		if (i == 1 && got != n) return -3;
		if (i >= 2 && got != tag + i - 2) return -4;
	}
	return 0;
}

int main(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	enum { N = 200000, P = 20000 };
	uint64_t t0, t1, *lat;
	unsigned int i, k;
	volatile uint32_t sink = 0;

	if (fd < 0) { perror("/dev/mem"); return 1; }
	ts = map(fd, 0x08010000, 4096);
	sram = map(fd, 0x00060000, 0x8000);	/* E902 0x40020000..0x40027FFF: unused */
	cpus = map(fd, 0x07094000, 4096);	/* ARM -> E902 */
	cpux = map(fd, 0x03004000, 4096);	/* E902 -> ARM */

	/* 1. shared timestamp read cost */
	t0 = now_ns();
	for (i = 0; i < N; i++) sink += (uint32_t)ts_read();
	t1 = now_ns();
	printf("timestamp read (64-bit, tear-free): %.0f ns/read\n", (double)(t1 - t0) / N);

	/* 2. shared SRAM A2 bandwidth, 32-bit accesses (device memory) */
	t0 = now_ns();
	for (k = 0; k < 64; k++) for (i = 0; i < 0x2000; i++) sram[i] = i ^ k;
	t1 = now_ns();
	printf("SRAM A2 write: %.1f MB/s\n", 64.0 * 0x8000 / ((t1 - t0) / 1e3));
	t0 = now_ns();
	for (k = 0; k < 64; k++) for (i = 0; i < 0x2000; i++) sink += sram[i];
	t1 = now_ns();
	printf("SRAM A2 read : %.1f MB/s\n", 64.0 * 0x8000 / ((t1 - t0) / 1e3));

	/* 3. mailbox round trips through the E902 firmware */
	if ((cpux[0x6C / 4] & 0xF) || (cpus[0x6C / 4] & 0xF)) {
		printf("mailbox ch3 not idle, skipping\n");
		return 0;
	}
	lat = calloc(P, sizeof(*lat));
	for (unsigned int n = 1; n <= 16; n += 15) {
		for (i = 0; i < P; i++) {
			t0 = now_ns();
			int r = ping(n, 0x1000 + i * 32);
			t1 = now_ns();
			if (r) { printf("ping %u failed (%d)\n", i, r); return 1; }
			lat[i] = t1 - t0;
		}
		qsort(lat, P, sizeof(*lat), cmp64);
		uint64_t sum = 0;
		for (i = 0; i < P; i++) sum += lat[i];
		printf("mailbox ping %2u data words: rtt min %.1f  median %.1f  p99 %.1f  p99.9 %.1f  max %.1f us"
		       "  | %.0f pings/s, payload %.1f KB/s each way\n",
		       n, lat[0] / 1e3, lat[P / 2] / 1e3, lat[P * 99 / 100] / 1e3,
		       lat[P * 999 / 1000] / 1e3, lat[P - 1] / 1e3,
		       P / (sum / 1e9), P * n * 4.0 / (sum / 1e9) / 1024);
	}
	printf("residue cpux=%u cpus=%u (sink %u)\n", cpux[0x6C / 4] & 0xF, cpus[0x6C / 4] & 0xF, sink);
	return 0;
}
