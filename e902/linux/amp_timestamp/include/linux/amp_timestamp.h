/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Allwinner A733 AMP shared timestamp.
 *
 * The A733 exposes one 64-bit free-running counter that BOTH the big core
 * (CPUX/Linux) and the little core (CPUS/E902) can read, which is what makes
 * cross-core timestamps comparable without any clock-sync protocol.
 *
 * Register blocks (A733 user manual v0.91, section 5.1.5):
 *   TIMESTAMP_STA  @ 0x08010000  CNT_LOW_REG  +0x00
 *                                CNT_HI_REG    +0x04     <- the live counter
 *   TIMESTAMP_CTRL @ 0x08020000  TSTAMP_CTRL_REG +0x00    bit0 = enable
 *                                CNT_CTRL_LOW_REG +0x08  (second copy)
 *                                CNT_CTRL_HI_REG  +0x0C
 *                                CNT_FREQID_REG   +0x20  base frequency id
 *
 * Measured on this board (2026-09-23): enable bit set, but CNT_FREQID_REG
 * reads 0 from both cores; the counter runs at 24.000 MHz against
 * CLOCK_MONOTONIC (zero drift), so the driver takes the rate from the DT
 * "clock-frequency" property when FREQID is 0.
 */
#ifndef _LINUX_AMP_TIMESTAMP_H
#define _LINUX_AMP_TIMESTAMP_H

#include <linux/types.h>

#define AMP_TS_MAX_DEV		4

#if IS_ENABLED(CONFIG_AW_AMP_TIMESTAMP)

/* Get a handle to the shared timestamp unit (dev_id 0..AMP_TS_MAX_DEV-1). */
int amp_ts_get_dev(int dev_id, void **dev);

/* Read the raw 64-bit counter. */
int amp_ts_get_timestamp(void *dev, u64 *ts);

/* Read the base-frequency id (bytes/sec; 0x016E3600 == 24 MHz on this SoC). */
int amp_ts_get_freqid(void *dev, u32 *freqid);

#else /* stubs so callers link without the driver */

static inline int amp_ts_get_dev(int dev_id, void **dev)
{
	(void)dev_id;
	if (dev)
		*dev = NULL;
	return -ENODEV;
}

static inline int amp_ts_get_timestamp(void *dev, u64 *ts)
{
	(void)dev;
	if (ts)
		*ts = 0;
	return -ENODEV;
}

static inline int amp_ts_get_freqid(void *dev, u32 *freqid)
{
	(void)dev;
	if (freqid)
		*freqid = 0;
	return -ENODEV;
}

#endif /* CONFIG_AW_AMP_TIMESTAMP */

#endif /* _LINUX_AMP_TIMESTAMP_H */
