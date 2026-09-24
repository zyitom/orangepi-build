/* Cross-module prototypes for the A733 E902 firmware. */
#ifndef __FW_H__
#define __FW_H__

#include "a733.h"

/* soc.c */
void soc_early_init(void);
unsigned int apbs1_rate(void);

/* pinmux.c */
int pinmux_s_uart0(void);
u32 pinmux_read_pl_cfg0(void);

/* uart.c */
void uart_init(unsigned int baud);
void uart_putc(char c);
void uart_puts(const char *s);
int  uart_getc_nb(void);
void uart_flush(void);
void uart_put_hex(u32 v);
void uart_put_dec(unsigned int v);
void uart_irq_enable(void);
void uart_isr(void);
unsigned int uart_rx_count(void);
int  uart_rx_pop(void);
void uart_rx_poll(void);
void uart_tx_pump(void);
void uart_tx_flush(void);
void uart_set_blocking(int on);
unsigned int uart_tx_dropped(void);

/* msgbox.c */
void msgbox_init(void);
int  msgbox_try_send(u32 msg);
int  msgbox_send(u32 msg);
unsigned int msgbox_rx_pending(void);
u32  msgbox_recv(void);
void msgbox_ack_irq(void);
unsigned int msgbox_irq_count(void);
unsigned int msgbox_poll_rx(void);
void handle_arm_packet(u32 hdr, const u32 *data, unsigned int count);
int  msgbox_wait_tx_empty(int spin);
int  msgbox_send_packet(u32 hdr, const u32 *data, unsigned int count);
int  msgbox_send_startup_feedback(void);
int  cpus_24m_broadcast_on(void);

/* globals owned by msgbox.c, read by the main loop */
extern volatile u32 mb_last_rx;
extern volatile unsigned int mb_rx_count;
extern volatile int mb_rx_flag;
extern volatile unsigned int mb_irq_count;
extern volatile u32 mb_pkt_hdr;
extern volatile u32 mb_pkt_data[16];
extern volatile unsigned int mb_pkt_words;
extern volatile int mb_pkt_ready;
extern volatile unsigned int mb_pkt_count;
extern unsigned int mb_stale_drained;
extern unsigned int mb_resyncs;

/* timer.c */
void delay_us(unsigned int us);
void delay_ms(unsigned int ms);
void timer_periodic_start(unsigned int period_ms);
void timer_periodic_stop(void);
unsigned int timer_ticks(void);
void timer_poll(void);
u32  timer_tick_sources(void);
u32  timer_irq_latency(void);
u32  timer_irq_latency_min(void);
u32 timer_raw_count(void);

/* ts_test.c */
void ts_test_run(void);

/* clic.c */
void clic_set_handler(unsigned int irq, void (*fn)(void));
void clic_enable(unsigned int irq, int edge_triggered);
void clic_disable(unsigned int irq);
void default_isr(void);

/* main.c -- called from start.S trap_entry, returns the resume address */
u32  trap_handler(u32 mcause, u32 mepc);

/* wdt.c */
void wdt_cpus_service(void);

/* power.c -- PSCI SYSTEM_OFF / SYSTEM_RESET on behalf of bl31 */
int twi_write(u8 dev, u8 reg, u8 val);
int twi_read(u8 dev, u8 reg, u8 *val);
int power_sys_op(u32 op);

/* heartbeat.c - SRAM status block, readable from the ARM via /dev/mem */
void hb_init(void);
void hb_set(unsigned int idx, u32 v);
u32  hb_get(unsigned int idx);
void hb_stage(u32 s);
void hb_tick(void);

/* heartbeat word indices (mirror of the enum in heartbeat.c) */
#define HB_MAGIC_IDX	0
#define HB_SEQ		1
#define HB_STAGE	2
#define HB_PL_CFG0	3
#define HB_APBS1	4
#define HB_RISCV_BGR	5
#define HB_RST_START	6
#define HB_SPI_INIT	7
#define HB_SPI_TXCNT	8
#define HB_SPI_RXCNT	9
#define HB_SPI_IRQ	10
#define HB_UART_RX	11
#define HB_MBOX_IRQ	12
#define HB_MBOX_RXCOUNT	13
#define HB_TIMER_TICKS	14
#define HB_SPI_RX0	15
#define HB_SPI_RX1	16
/* v63 diagnostics */
#define HB_TRAPS	17	/* synchronous exceptions survived */
#define HB_LAST_MCAUSE	18	/* mcause of the latest exception */
#define HB_LAST_MEPC	19	/* its mepc */
#define HB_SPURIOUS	20	/* [31:16] last irq id, [15:0] count */
#define HB_TICK_SRC	21	/* [31:16] ticks via IRQ, [15:0] ticks via poll */
#define HB_CLIC_DIAG	22	/* timer1: [31:24] INTIP [23:16] INTIE,
				 * [15:0] S_TIMER IRQ_STA */
#define HB_RPC		23	/* [31:16] packets rx'd, [15:0] replies sent */
#define HB_IRQ_LAT	24	/* timer irq latency, 24 MHz ticks:
				 * [31:16] max, [15:0] last */
#define HB_IRQ_LAT_MIN	25
#define HB_WORDS	26
#define HB_BASE_ADDR	0x4001E000u
#define HB_MAGIC_VALUE	0xE902C0DEu

#endif /* __FW_H__ */
