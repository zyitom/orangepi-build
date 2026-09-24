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

/* msgbox.c */
void msgbox_init(void);
int  msgbox_try_send(u32 msg);
int  msgbox_send(u32 msg);
unsigned int msgbox_rx_pending(void);
u32  msgbox_recv(void);
void msgbox_ack_irq(void);

/* timer.c */
void delay_us(unsigned int us);
void delay_ms(unsigned int ms);
void timer_periodic_start(unsigned int period_ms);
void timer_periodic_stop(void);
unsigned int timer_ticks(void);
u32 timer_raw_count(void);

/* clic.c */
void clic_set_handler(unsigned int irq, void (*fn)(void));
void clic_enable(unsigned int irq, int edge_triggered);
void clic_disable(unsigned int irq);

#endif /* __FW_H__ */
