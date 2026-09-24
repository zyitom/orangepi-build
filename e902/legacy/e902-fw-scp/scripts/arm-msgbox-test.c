/*
 * ARM-side MSGBOX poker, for testing the E902 firmware without needing a
 * kernel driver. Talks to the hardware directly through /dev/mem.
 *
 *   gcc -O2 -o arm-msgbox-test arm-msgbox-test.c
 *   sudo ./arm-msgbox-test            # listen forever
 *   sudo ./arm-msgbox-test ping       # send CMD_PING, expect CMD_PONG
 *   sudo ./arm-msgbox-test echo 1234  # send CMD_ECHO with payload
 *   sudo ./arm-msgbox-test -t 5 ping  # send, then listen 5s and exit
 *
 * -t <seconds> bounds the listen loop so this can be scripted. Exit code is
 * 0 if at least one message arrived, 2 if the wait timed out empty.
 *
 * Coexistence with the kernel driver: safe in practice. sunxi-msgbox.c only
 * enables the RX interrupt in sunxi_msgbox_startup() (line 346), which runs
 * when a mailbox *client* claims the channel. With CONFIG_AW_MSGBOX=y but no
 * client bound, the driver probes and sits idle -- its ISR checks
 * READ_IRQ_ENABLE before touching the FIFO, so it will not race us. Once you
 * add a real kernel-side client, retire this tool.
 *
 * Direction reminder (manual 6.1.6), from the ARM's point of view:
 *   MBOX_CPUX 0x03004000  E902 -> ARM   we READ here
 *   MBOX_CPUS 0x07094000  ARM  -> E902  we WRITE here
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

#define MBOX_CPUX	0x03004000UL	/* read side  */
#define MBOX_CPUS	0x07094000UL	/* write side */
#define MAP_LEN		0x1000

#define CH		0
#define OFF_RD_IRQ_EN	0x20
#define OFF_RD_IRQ_STAT	0x24
#define OFF_FIFO_STATUS	(0x50 + 4 * CH)
#define OFF_MSG_STATUS	(0x60 + 4 * CH)
#define OFF_MSG		(0x70 + 4 * CH)

#define CMD_PING	0x01
#define CMD_PONG	0x02
#define CMD_HELLO	0x10
#define CMD_UART_KEY	0x11
#define CMD_ECHO	0x20

#define MSG(c, p)	((((uint32_t)(c)) << 24) | ((p) & 0xFFFFFF))

static volatile uint8_t *map_block(int fd, unsigned long pa)
{
	void *p = mmap(NULL, MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fd, (off_t)pa);
	if (p == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	return (volatile uint8_t *)p;
}

static uint32_t rd(volatile uint8_t *b, unsigned off)
{
	return *(volatile uint32_t *)(b + off);
}

static void wr(volatile uint8_t *b, unsigned off, uint32_t v)
{
	*(volatile uint32_t *)(b + off) = v;
}

static const char *cmd_name(unsigned c)
{
	switch (c) {
	case CMD_PING:     return "PING";
	case CMD_PONG:     return "PONG";
	case CMD_HELLO:    return "HELLO";
	case CMD_UART_KEY: return "UART_KEY";
	case CMD_ECHO:     return "ECHO";
	default:           return "?";
	}
}

static unsigned drain(volatile uint8_t *rx)
{
	unsigned n = 0;

	while (rd(rx, OFF_MSG_STATUS) & 0xF) {
		uint32_t m = rd(rx, OFF_MSG);

		printf("  <- 0x%08x  cmd=0x%02x (%-8s) payload=0x%06x\n",
		       m, m >> 24, cmd_name(m >> 24), m & 0xFFFFFF);
		n++;
	}
	wr(rx, OFF_RD_IRQ_STAT, 1u << (CH * 2));
	return n;
}

int main(int argc, char **argv)
{
	int fd;
	volatile uint8_t *rx, *tx;
	double timeout = 0.0;		/* 0 = listen forever */
	unsigned got = 0;

	/* optional leading -t <seconds> */
	if (argc > 2 && !strcmp(argv[1], "-t")) {
		timeout = strtod(argv[2], NULL);
		argv += 2;
		argc -= 2;
	}

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	rx = map_block(fd, MBOX_CPUX);
	tx = map_block(fd, MBOX_CPUS);

	printf("rx block 0x%08lx: msg_status=0x%x fifo=0x%x\n",
	       MBOX_CPUX, rd(rx, OFF_MSG_STATUS), rd(rx, OFF_FIFO_STATUS));
	printf("tx block 0x%08lx: msg_status=0x%x fifo=0x%x\n",
	       MBOX_CPUS, rd(tx, OFF_MSG_STATUS), rd(tx, OFF_FIFO_STATUS));

	if (argc > 1) {
		uint32_t msg;

		if (!strcmp(argv[1], "ping")) {
			msg = MSG(CMD_PING, 0xABC);
		} else if (!strcmp(argv[1], "echo")) {
			uint32_t p = (argc > 2) ? strtoul(argv[2], NULL, 0) : 0x123;
			msg = MSG(CMD_ECHO, p);
		} else {
			msg = strtoul(argv[1], NULL, 0);
		}

		if (rd(tx, OFF_FIFO_STATUS) & 1) {
			fprintf(stderr, "tx fifo full -- is the E902 running?\n");
			return 1;
		}
		printf("  -> 0x%08x  cmd=0x%02x (%s)\n",
		       msg, msg >> 24, cmd_name(msg >> 24));
		wr(tx, OFF_MSG, msg);
	}

	if (timeout > 0.0)
		printf("listening %.1fs ...\n", timeout);
	else
		printf("listening (ctrl-c to stop) ...\n");

	{
		struct timespec t0;
		clock_gettime(CLOCK_MONOTONIC, &t0);

		for (;;) {
			if (rd(rx, OFF_MSG_STATUS) & 0xF)
				got += drain(rx);

			if (timeout > 0.0) {
				struct timespec now;
				double dt;

				clock_gettime(CLOCK_MONOTONIC, &now);
				dt = (now.tv_sec - t0.tv_sec)
				   + (now.tv_nsec - t0.tv_nsec) / 1e9;
				if (dt >= timeout)
					break;
			}
			usleep(20000);
		}
	}

	if (timeout > 0.0) {
		printf("%u message(s) received\n", got);
		return got ? 0 : 2;
	}
	return 0;
}
