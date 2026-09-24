// SPDX-License-Identifier: GPL-2.0
/*
 * Allwinner A733 AMP shared timestamp driver.
 *
 * Why this exists: the kernel tree shipped by the vendor references
 * <linux/amp_timestamp.h> from bsp/drivers/rpmsg/{rpmsg_perf,aw_virtio_rpmsg_bus}.c
 * and has a Kconfig symbol AW_AMP_TIMESTAMP selected by AW_RPMSG_PERF_TRACE, but
 * neither the header nor any implementation is present in this tree. That is why
 * AW_RPMSG_PERF_TRACE cannot be enabled (it fails to compile).
 *
 * This provides the missing piece, and it is useful well beyond rpmsg: the
 * counter it exposes is the SoC's cross-core time base, so a driver (or
 * userspace via the sysfs attributes below) can timestamp events on the big core
 * with the same clock the E902 uses for its IMU samples.
 *
 * Register layout: see the user manual section 5.1.5 (also documented in
 * <linux/amp_timestamp.h>). The live counter is TIMESTAMP_STA.CNT_LOW/HI; the
 * CTRL block carries the enable bit and the base-frequency id.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/amp_timestamp.h>

#define AMP_TS_STA_CNT_LOW	0x00
#define AMP_TS_STA_CNT_HI	0x04

#define AMP_TS_CTRL_CTRL	0x00	/* bit0 = enable */
#define AMP_TS_CTRL_CNT_LOW	0x08
#define AMP_TS_CTRL_CNT_HI	0x0C
#define AMP_TS_CTRL_FREQID	0x20

#define AMP_TS_ENABLE_BIT	BIT(0)

struct amp_ts_dev {
	struct device *dev;
	void __iomem *sta;
	void __iomem *ctrl;
	u32 freqid;
	bool claimed;
};

static struct amp_ts_dev *g_ts[AMP_TS_MAX_DEV];
static DEFINE_MUTEX(g_ts_lock);

/*
 * 64-bit read that cannot tear: read HI, LO, HI and retry while the high word
 * changes. (A raw single-shot read of the two halves can straddle a carry.)
 */
static u64 amp_ts_read64(struct amp_ts_dev *t)
{
	u32 lo, hi, hi2;

	do {
		hi = readl(t->sta + AMP_TS_STA_CNT_HI);
		lo = readl(t->sta + AMP_TS_STA_CNT_LOW);
		hi2 = readl(t->sta + AMP_TS_STA_CNT_HI);
	} while (hi != hi2);

	return ((u64)hi << 32) | lo;
}

int amp_ts_get_dev(int dev_id, void **dev)
{
	if (!dev)
		return -EINVAL;
	*dev = NULL;
	if (dev_id < 0 || dev_id >= AMP_TS_MAX_DEV)
		return -EINVAL;

	mutex_lock(&g_ts_lock);
	if (!g_ts[dev_id]) {
		mutex_unlock(&g_ts_lock);
		return -ENODEV;
	}
	g_ts[dev_id]->claimed = true;
	*dev = g_ts[dev_id];
	mutex_unlock(&g_ts_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(amp_ts_get_dev);

int amp_ts_get_timestamp(void *dev, u64 *ts)
{
	struct amp_ts_dev *t = dev;

	if (!t || !ts)
		return -EINVAL;
	*ts = amp_ts_read64(t);
	return 0;
}
EXPORT_SYMBOL_GPL(amp_ts_get_timestamp);

int amp_ts_get_freqid(void *dev, u32 *freqid)
{
	struct amp_ts_dev *t = dev;

	if (!t || !freqid)
		return -EINVAL;
	 *freqid = t->freqid;
	return 0;
}
EXPORT_SYMBOL_GPL(amp_ts_get_freqid);

/* ---- sysfs: let userspace read the shared counter too ---- */

static ssize_t counter_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct amp_ts_dev *t = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%llu\n", amp_ts_read64(t));
}
static DEVICE_ATTR_RO(counter);

static ssize_t freqid_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct amp_ts_dev *t = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", t->freqid);
}
static DEVICE_ATTR_RO(freqid);

/*
 * Microseconds since the counter started. Integer-only: the kernel is built
 * with -mgeneral-regs-only, so no floating point is allowed here.
 * us = counter / (freqid / 1e6); requires freqid to be a multiple of 1e6
 * (24 MHz on this SoC), otherwise we fall back to reporting the raw counter.
 */
static ssize_t usec_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct amp_ts_dev *t = dev_get_drvdata(dev);
	u64 c = amp_ts_read64(t);
	u64 us = 0;

	if (t->freqid && (t->freqid % 1000000u) == 0)
		us = div_u64(c, t->freqid / 1000000u);

	return sysfs_emit(buf, "%llu\n", us);
}
static DEVICE_ATTR_RO(usec);

static struct attribute *amp_ts_attrs[] = {
	&dev_attr_counter.attr,
	&dev_attr_freqid.attr,
	&dev_attr_usec.attr,
	NULL,
};
ATTRIBUTE_GROUPS(amp_ts);

static int amp_ts_probe(struct platform_device *pdev)
{
	struct amp_ts_dev *t;
	void __iomem *sta, *ctrl;
	int ret;
	int id = pdev->id < 0 ? 0 : pdev->id;

	if (id >= AMP_TS_MAX_DEV)
		return -EINVAL;

	sta = devm_platform_ioremap_resource_byname(pdev, "sta");
	if (IS_ERR(sta)) {
		dev_err(&pdev->dev, "ioremap 'sta' failed: %ld\n", PTR_ERR(sta));
		return PTR_ERR(sta);
	}
	ctrl = devm_platform_ioremap_resource_byname(pdev, "ctrl");
	if (IS_ERR(ctrl)) {
		dev_err(&pdev->dev, "ioremap 'ctrl' failed: %ld\n", PTR_ERR(ctrl));
		return PTR_ERR(ctrl);
	}

	t = devm_kzalloc(&pdev->dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	t->dev = &pdev->dev;
	t->sta = sta;
	t->ctrl = ctrl;

	/* Make sure the counter runs (bl31/the SCP normally leaves it enabled). */
	writel(readl(ctrl + AMP_TS_CTRL_CTRL) | AMP_TS_ENABLE_BIT,
	       ctrl + AMP_TS_CTRL_CTRL);

	t->freqid = readl(ctrl + AMP_TS_CTRL_FREQID);
	if (!t->freqid) {
		u32 hz;

		/* FREQID reads 0 on the A733 from both the ARM and the E902;
		 * the counter itself runs at 24.000 MHz (measured against
		 * CLOCK_MONOTONIC), so take the rate from the DT instead. */
		if (!of_property_read_u32(pdev->dev.of_node, "clock-frequency",
					  &hz))
			t->freqid = hz;
	}

	/* Touch it once so a dead/unclocked block fails loudly here. */
	(void)amp_ts_read64(t);

	platform_set_drvdata(pdev, t);
	mutex_lock(&g_ts_lock);
	g_ts[id] = t;
	mutex_unlock(&g_ts_lock);

	ret = device_add_groups(&pdev->dev, amp_ts_groups);
	if (ret)
		dev_warn(&pdev->dev, "sysfs attributes not added: %d\n", ret);

	dev_info(&pdev->dev,
		 "A733 AMP timestamp ready: counter=%llu freqid=%u (%u.%03u MHz)\n",
		 amp_ts_read64(t), t->freqid,
		 t->freqid / 1000000u, (t->freqid % 1000000u) / 1000u);
	return 0;
}

static int amp_ts_remove(struct platform_device *pdev)
{
	struct amp_ts_dev *t = platform_get_drvdata(pdev);
	int i;

	mutex_lock(&g_ts_lock);
	for (i = 0; i < AMP_TS_MAX_DEV; i++)
		if (g_ts[i] == t)
			g_ts[i] = NULL;
	mutex_unlock(&g_ts_lock);
	return 0;
}

static const struct of_device_id amp_ts_of_match[] = {
	{ .compatible = "allwinner,amp-timestamp" },
	{ }
};
MODULE_DEVICE_TABLE(of, amp_ts_of_match);

static struct platform_driver amp_ts_driver = {
	.probe	= amp_ts_probe,
	.remove	= amp_ts_remove,
	.driver	= {
		.name		= "amp-timestamp",
		.of_match_table	= amp_ts_of_match,
	},
};
module_platform_driver(amp_ts_driver);

MODULE_DESCRIPTION("Allwinner A733 AMP shared timestamp");
MODULE_LICENSE("GPL v2");
