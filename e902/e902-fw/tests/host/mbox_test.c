/*
 * Host-side test of the bl31 <-> E902 mailbox protocol handling.
 *
 * Builds the real src/msgbox.c and src/main.c against a mock of the two
 * MSGBOX FIFOs (channel 3) and plays the request patterns bl31 actually
 * sends (monitor.fex 0x1bf4 callers, see FINDINGS-LEDGER T29). Everything
 * else main.c links against is stubbed below.
 *
 *   make hosttest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fw.h"	/* build/host/a733.h (mock shim) is force-included first */

/* ------------------------------------------------------------------ */
/* mock FIFOs                                                          */
/* ------------------------------------------------------------------ */

#define DEPTH 8

struct fifo { u32 w[64]; unsigned int n; };
static struct fifo rxq;		/* ARM -> E902 (MBOX_CPUS, we read) */
static struct fifo txq;		/* E902 -> ARM (MBOX_CPUX, we write) */
static int tx_overrun;

#define CH 3
#define RX_STATUS	(0x07094000u + 0x60 + 4 * CH)
#define RX_MSG		(0x07094000u + 0x70 + 4 * CH)
#define TX_FIFO_STA	(0x03004000u + 0x50 + 4 * CH)
#define TX_STATUS	(0x03004000u + 0x60 + 4 * CH)
#define TX_MSG		(0x03004000u + 0x70 + 4 * CH)

static u32 pop(struct fifo *f)
{
	u32 v = f->w[0];

	memmove(f->w, f->w + 1, (f->n - 1) * sizeof(u32));
	f->n--;
	return v;
}

u32 mock_readl(unsigned long a)
{
	switch (a) {
	case RX_STATUS:   return rxq.n < DEPTH ? rxq.n : DEPTH;
	case RX_MSG:      return rxq.n ? pop(&rxq) : 0;
	case TX_FIFO_STA: return txq.n >= DEPTH ? 1u : 0u;
	case TX_STATUS:   return txq.n;
	default:          return 0;
	}
}

void mock_writel(u32 v, unsigned long a)
{
	if (a == TX_MSG) {
		if (txq.n >= DEPTH)
			tx_overrun++;
		else
			txq.w[txq.n++] = v;
	}
}

u8 mock_readb(unsigned long a) { (void)a; return 0; }
void mock_writeb(u8 v, unsigned long a) { (void)v; (void)a; }

/* ------------------------------------------------------------------ */
/* stubs for everything main.c / msgbox.c link against                 */
/* ------------------------------------------------------------------ */

u32 last_mcause, last_mepc;
static int sysop_called = -1;

void uart_puts(const char *s) { (void)s; }
void uart_putc(char c) { (void)c; }
void uart_put_hex(u32 v) { (void)v; }
void uart_put_dec(unsigned int v) { (void)v; }
void uart_init(unsigned int b) { (void)b; }
void uart_set_blocking(int on) { (void)on; }
void uart_tx_pump(void) {}
void uart_tx_flush(void) {}
void uart_rx_poll(void) {}
int uart_rx_pop(void) { return -1; }
unsigned int uart_rx_count(void) { return 0; }
unsigned int uart_tx_dropped(void) { return 0; }
void hb_init(void) {}
void hb_set(unsigned int i, u32 v) { (void)i; (void)v; }
u32 hb_get(unsigned int i) { (void)i; return 0; }
void hb_stage(u32 s) { (void)s; }
void hb_tick(void) {}
void clic_disable(unsigned int irq) { (void)irq; }
static unsigned int fake_ticks;
unsigned int timer_ticks(void) { return fake_ticks; }
void timer_poll(void) {}
u32 timer_tick_sources(void) { return 0; }
u32 timer_irq_latency(void) { return 0; }
u32 timer_irq_latency_min(void) { return 0; }
void timer_periodic_start(unsigned int ms) { (void)ms; }
int spi_init(unsigned int hz) { (void)hz; return 0; }
void spi_dump_regs(void) {}
int spi_selftest(u8 *rx, unsigned int *len) { (void)rx; *len = 0; return 0; }
unsigned int spi_irq_count(void) { return 0; }
unsigned int spi_tc_count(void) { return 0; }
void spi_use(u32 base) { (void)base; }
int cx_spi3_init(unsigned int hz) { (void)hz; return 0; }
int pinmux_s_uart0(void) { return 0; }
u32 pinmux_read_pl_cfg0(void) { return 0; }
unsigned int apbs1_rate(void) { return 24000000; }
int cpus_24m_broadcast_on(void) { return 0; }
void ts_test_run(void) {}
unsigned int gintc_count(void) { return 0; }
void gintc_sweep(void) {}
void gintc_off(void) {}
void gintc_uart_toggle(void) {}
void gintc_lradc_toggle(void) {}
void gintc_report(void) {}

int power_sys_op(u32 op)
{
	sysop_called = (int)op;
	return op <= 3 ? 0 /* real one never returns */ : -22;
}

/* ------------------------------------------------------------------ */
/* tests                                                               */
/* ------------------------------------------------------------------ */

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); printf("\n"); } } while (0)

static void reset(void)
{
	memset(&rxq, 0, sizeof(rxq));
	memset(&txq, 0, sizeof(txq));
	tx_overrun = 0;
	sysop_called = -1;
}

/* bl31 header: [b0 attr][b1 flags][b2 type][b3 result] */
static u32 hdr(u32 attr, u32 flags, u32 type)
{
	return attr | (flags << 8) | (type << 16);
}

static void arm_send(u32 h, const u32 *d, unsigned int n)
{
	unsigned int i;

	rxq.w[rxq.n++] = h;
	rxq.w[rxq.n++] = n;
	for (i = 0; i < n; i++)
		rxq.w[rxq.n++] = d[i];
}

/* bl31 sync receive (0x1cb8..): hdr, count, count data words */
static int arm_recv(u32 *h, u32 *d, unsigned int *n)
{
	unsigned int i;

	if (txq.n < 2)
		return -1;
	*h = pop(&txq);
	*n = pop(&txq);
	if (txq.n < *n)
		return -1;
	for (i = 0; i < *n; i++)
		d[i] = pop(&txq);
	return 0;
}

static void test_sync_requests(void)
{
	/* every synchronous type bl31 can send, with its real word count */
	static const struct { u32 type; unsigned int n; } req[] = {
		{ 0x25, 1 }, { 0x11, 0 }, { 0x60, 1 }, { 0x62, 1 }, { 0x26, 1 },
	};
	unsigned int k, i;

	for (k = 0; k < sizeof(req) / sizeof(req[0]); k++) {
		u32 d[4] = { 0x1234 + k, 2, 3, 4 }, rh, rd[16];
		unsigned int rn;
		u32 h = hdr(2, 2, req[k].type) | (0x77u << 24);

		reset();
		arm_send(h, d, req[k].n);
		msgbox_poll_rx();
		CHECK(arm_recv(&rh, rd, &rn) == 0, "type %#x: no reply", req[k].type);
		CHECK(rh == (h & 0x00FFFFFFu), "type %#x: reply hdr %#x", req[k].type, rh);
		CHECK(rn == req[k].n, "type %#x: reply count %u", req[k].type, rn);
		for (i = 0; i < rn && i < req[k].n; i++)
			CHECK(rd[i] == d[i], "type %#x: data[%u]", req[k].type, i);
		CHECK(txq.n == 0, "type %#x: %u extra words", req[k].type, txq.n);
		CHECK(rxq.n == 0, "type %#x: rx not drained", req[k].type);
	}
}

static void test_async_and_ack(void)
{
	u32 d[5] = { 1, 2, 3, 3, 3 };

	reset();
	arm_send(hdr(2, 0, 0x22), d, 5);		/* cpu op, async */
	arm_send(hdr(2, 0, 0x61), d, 1);		/* uart baud, async */
	arm_send(0x00900200u, d, 1);			/* ack of our ready pkt */
	arm_send(0x00900200u | (5u << 24), d, 0);	/* ack, other result */
	msgbox_poll_rx();
	CHECK(txq.n == 0, "async/ack produced %u reply words", txq.n);
	CHECK(rxq.n == 0, "rx not drained");
}

static void test_sysop(void)
{
	u32 op, rh, rd[16];
	unsigned int rn;

	reset();
	op = 1;					/* SYSTEM_RESET */
	arm_send(hdr(2, 0, 0x24), &op, 1);
	msgbox_poll_rx();
	CHECK(sysop_called == 1, "sys-op reset not dispatched (%d)", sysop_called);
	CHECK(txq.n == 0, "sys-op produced a reply");

	reset();
	op = 7;					/* unknown op, sync */
	arm_send(hdr(2, 2, 0x24), &op, 1);
	msgbox_poll_rx();
	CHECK(arm_recv(&rh, rd, &rn) == 0, "unknown sys-op: no reply");
	CHECK((rh >> 24) == 0xEA, "unknown sys-op: result %#x", rh >> 24);
	CHECK(rn == 1 && rd[0] == 7, "unknown sys-op: payload");
}

/* words dribble in one at a time across polls, like a slow sender */
static void test_split_delivery(void)
{
	u32 d[1] = { 0xCAFE }, words[3], rh, rd[16];
	unsigned int i, rn;
	u32 h = hdr(2, 2, 0x60);

	reset();
	words[0] = h; words[1] = 1; words[2] = d[0];
	for (i = 0; i < 3; i++) {
		rxq.w[rxq.n++] = words[i];
		msgbox_poll_rx();
		if (i < 2)
			CHECK(txq.n == 0, "replied before packet complete");
	}
	CHECK(arm_recv(&rh, rd, &rn) == 0 && rn == 1 && rd[0] == 0xCAFE,
	      "split packet reply wrong");
}

/* a burst larger than the 8-deep FIFO: bl31 blocks while RX is full, so
 * model it as a writer that refills as fast as we drain */
static void test_back_to_back(void)
{
	u32 d[1] = { 9 }, rh, rd[16];
	unsigned int k, rn, replies = 0;

	reset();
	for (k = 0; k < 20; k++) {
		arm_send(hdr(2, 2, 0x60), d, 1);
		msgbox_poll_rx();
		while (arm_recv(&rh, rd, &rn) == 0)
			replies++;
	}
	CHECK(replies == 20, "back-to-back: %u/20 replies", replies);
	CHECK(tx_overrun == 0, "tx overrun %d", tx_overrun);
}

/* a stray word desyncs framing; after >= 2 quiet ticks the next real
 * request must still be answered correctly */
static void test_resync(void)
{
	u32 d[1] = { 0x5A5A }, rh, rd[16];
	unsigned int rn;
	u32 h = hdr(2, 2, 0x60);

	reset();
	rxq.w[rxq.n++] = 0xDEADBEEF;		/* lone stray word */
	msgbox_poll_rx();
	fake_ticks += 3;			/* 300 ms of silence */
	msgbox_poll_rx();
	arm_send(h, d, 1);
	msgbox_poll_rx();
	CHECK(arm_recv(&rh, rd, &rn) == 0 && rh == (h & 0x00FFFFFFu) &&
	      rn == 1 && rd[0] == 0x5A5A, "no correct reply after resync");
	CHECK(mb_resyncs >= 1, "resync not counted");
}

int main(void)
{
	test_sync_requests();
	test_async_and_ack();
	test_sysop();
	test_split_delivery();
	test_back_to_back();
	test_resync();
	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("mbox host test: all passed\n");
	return 0;
}
