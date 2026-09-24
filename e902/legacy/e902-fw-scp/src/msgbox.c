/*
 * MSGBOX transport to the ARM side.
 *
 * Two separate one-way FIFO blocks (manual 6.1.6):
 *   MBOX_CPUX 0x03004000  CPUS writes, CPUX reads  -> we TX here
 *   MBOX_CPUS 0x07094000  CPUX writes, CPUS reads  -> we RX here
 *
 * 4 channels each, 8 x 32-bit deep. A write to MSG_REG raises the peer's
 * interrupt automatically; no doorbell register to poke.
 *
 * Our RX interrupt is CLIC IRQ 48 (CPUX_MSGBOX_W_R, manual Table 12-2).
 */
#include "fw.h"

#define MBOX_CH		0	/* channel 0 for both directions */

void msgbox_init(void)
{
	/* Clear any stale RX pending bit from before we booted */
	writel(MBOX_RD_IRQ_PEND_BIT(MBOX_CH), MBOX_RD_IRQ_STAT(MBOX_CPUS_BASE));

	/* Enable "message received" interrupt on our RX block */
	writel(MBOX_RD_IRQ_EN_BIT(MBOX_CH), MBOX_RD_IRQ_EN(MBOX_CPUS_BASE));
}

/* Non-blocking send. Returns 0 on success, -1 if the FIFO is full. */
int msgbox_try_send(u32 msg)
{
	if (readl(MBOX_FIFO_STATUS(MBOX_CPUX_BASE, MBOX_CH)) & MBOX_FIFO_FULL)
		return -1;

	writel(msg, MBOX_MSG(MBOX_CPUX_BASE, MBOX_CH));
	return 0;
}

/* Blocking send, bounded so a dead peer cannot wedge us forever. */
int msgbox_send(u32 msg)
{
	int spin = 100000;

	while (spin--) {
		if (msgbox_try_send(msg) == 0)
			return 0;
	}
	return -1;
}

/* How many messages are waiting for us. */
unsigned int msgbox_rx_pending(void)
{
	return readl(MBOX_MSG_STATUS(MBOX_CPUS_BASE, MBOX_CH))
		& MBOX_MSG_NUM_MASK;
}

/* Pop one message. Only valid when msgbox_rx_pending() > 0. */
u32 msgbox_recv(void)
{
	return readl(MBOX_MSG(MBOX_CPUS_BASE, MBOX_CH));
}

void msgbox_ack_irq(void)
{
	writel(MBOX_RD_IRQ_PEND_BIT(MBOX_CH), MBOX_RD_IRQ_STAT(MBOX_CPUS_BASE));
}
