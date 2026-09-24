/*
 * amt_pkt.c -- ARM-side packet poker for the A733 mailbox, channel 3.
 *
 * Derived from the disassembly of the vendor scp.fex (see FINDINGS-LEDGER T15):
 *
 *   from the ARM (CPUX) point of view
 *     TX  -> MBOX_CPUS 0x07094000, channel 3  = 0x0709407C   (CPUS reads it)
 *     RX  <- MBOX_CPUX 0x03004000, channel 3  = 0x0300407C   (CPUS writes it)
 *
 *   wire packet format:  [u32 header] [u32 word_count] [word_count x u32 data]
 *
 *   header word bytes (little endian u32):
 *     byte0 = 0
 *     byte1 = flags        (bit0|bit1 != 0  => the SCP sends a response back)
 *     byte2 = command id   (SCP dispatch table at scp.fex 0x4000ac32..0x4000ad74)
 *     byte3 = result       (written by the SCP in its response)
 *
 *   known command ids from the dispatch table:
 *     0x19 0x22 0x24 0x25 0x26 0x60 0x61 0x62 0x64 0x96
 *     0x61 == "loopback message request"  (scp.fex 0x4000acb6 -> 0x400101a8)
 *
 * Usage:
 *   ./amt_pkt                 listen forever on channel 3
 *   ./amt_pkt -t 10           listen 10 s
 *   ./amt_pkt loop [data]     send cmd=0x61, flags=1, one data word
 *   ./amt_pkt send <cmd> <flags> <data0> [data1 ...]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>

#define MBOX_CPUX	0x03004000UL	/* ARM reads  (CPUS -> CPUX) */
#define MBOX_CPUS	0x07094000UL	/* ARM writes (CPUX -> CPUS) */
#define MAP_LEN		0x1000
#define CH		3

#define OFF_RD_IRQ_EN	0x20
#define OFF_RD_IRQ_STAT	0x24
#define OFF_FIFO_STATUS	(0x50 + 4 * CH)
#define OFF_MSG_STATUS	(0x60 + 4 * CH)
#define OFF_MSG		(0x70 + 4 * CH)

static volatile uint8_t *map_block(int fd, unsigned long pa)
{
	void *p = mmap(NULL, MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)pa);
	if (p == MAP_FAILED) { perror("mmap"); exit(1); }
	return (volatile uint8_t *)p;
}
static uint32_t rd(volatile uint8_t *b, unsigned off) { return *(volatile uint32_t *)(b + off); }
static void wr(volatile uint8_t *b, unsigned off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }

static uint32_t make_hdr(unsigned cmd, unsigned flags) { return (flags & 0xFFu) << 8 | (cmd & 0xFFu) << 16; }

static void dump_packet(volatile uint8_t *rx)
{
	unsigned n = 0;
	while ((rd(rx, OFF_MSG_STATUS) & 0xF) && n < 64) {
		uint32_t m = rd(rx, OFF_MSG);
		printf("  <- word%-2u 0x%08x   [byte0=0x%02x flags=0x%02x cmd=0x%02x result=0x%02x]\n",
		       n, m, m & 0xFF, (m >> 8) & 0xFF, (m >> 16) & 0xFF, (m >> 24) & 0xFF);
		n++;
	}
	if (n) wr(rx, OFF_RD_IRQ_STAT, 1u << (CH * 2));
}

int main(int argc, char **argv)
{
	int fd;
	volatile uint8_t *rx, *tx;
	double timeout = 0.0;
	unsigned got = 0;

	if (argc > 2 && !strcmp(argv[1], "-t")) { timeout = strtod(argv[2], NULL); argv += 2; argc -= 2; }

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (!fd) { perror("/dev/mem"); return 1; }
	rx = map_block(fd, MBOX_CPUX);
	tx = map_block(fd, MBOX_CPUS);

	printf("chan %d  rx(ARM reads) 0x%08lx+0x%02x  tx(ARM writes) 0x%08lx+0x%02x\n",
	       CH, MBOX_CPUX, OFF_MSG, MBOX_CPUS, OFF_MSG);
	printf("before: rx_msg_status=0x%x rx_fifo=0x%x  tx_msg_status=0x%x tx_fifo=0x%x\n",
	       rd(rx, OFF_MSG_STATUS), rd(rx, OFF_FIFO_STATUS),
	       rd(tx, OFF_MSG_STATUS), rd(tx, OFF_FIFO_STATUS));

	if (argc > 1) {
		unsigned cmd = 0x61, flags = 1;
		uint32_t data[32];
		unsigned nd = 0, i;
		uint32_t hdr;

		if (!strcmp(argv[1], "loop")) {
			cmd = 0x61;
			data[nd++] = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 0xDEADBEEFu;
		} else if (!strcmp(argv[1], "send")) {
			if (argc < 5) { fprintf(stderr, "usage: send <cmd> <flags> <data0> [data1..]\n"); return 2; }
			cmd   = strtoul(argv[2], NULL, 0);
			flags = strtoul(argv[3], NULL, 0);
			for (i = 4; i < (unsigned)argc && nd < 32; i++)
				data[nd++] = (uint32_t)strtoul(argv[i], NULL, 0);
		} else {
			fprintf(stderr, "unknown mode\n"); return 2;
		}

		hdr = make_hdr(cmd, flags);
		printf("sending packet: hdr=0x%08x (cmd=0x%02x flags=0x%02x) count=%u\n", hdr, cmd, flags, nd);
		if (rd(tx, OFF_FIFO_STATUS) & 1) { fprintf(stderr, "tx fifo full\n"); return 1; }
		wr(tx, OFF_MSG, hdr);
		wr(tx, OFF_MSG, nd);
		for (i = 0; i < nd; i++) {
			printf("  -> data[%u] = 0x%08x\n", i, data[i]);
			wr(tx, OFF_MSG, data[i]);
		}
		printf("after send: tx_msg_status=0x%x tx_fifo=0x%x\n",
		       rd(tx, OFF_MSG_STATUS), rd(tx, OFF_FIFO_STATUS));
	}

	if (timeout > 0.0) printf("listening %.1fs ...\n", timeout);
	else printf("listening (ctrl-c) ...\n");

	{
		struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
		for (;;) {
			if (rd(rx, OFF_MSG_STATUS) & 0xF) got += 1, dump_packet(rx);
			if (timeout > 0.0) {
				struct timespec now; double dt;
				clock_gettime(CLOCK_MONOTONIC, &now);
				dt = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
				if (dt >= timeout) break;
			}
			usleep(20000);
		}
	}
	if (timeout > 0.0) { printf("%u packet(s) seen\n", got); return got ? 0 : 2; }
	return 0;
}
