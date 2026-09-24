// SPDX-License-Identifier: GPL-2.0
/*
 * ispclk_scan.c - temporary out-of-tree probe for the sun60iw2 ISP clock.
 *
 * NOT part of the kernel tree and NOT part of patches/.  Purpose: find how far
 * the ISP602 clock can actually be moved on this board, without touching the
 * device tree.
 *
 * Clock layout (bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c):
 *
 *   pll-video0 (ccu_nm, min_rate 1272 MHz, max_rate 2520 MHz, ref 24 MHz)
 *     +-- pll-video0-4x   SUNXI_CCU_M, div bits 20..23, CLK_SET_RATE_PARENT
 *     +-- pll-video0-3x   SUNXI_CCU_M, div bits 16..19, CLK_SET_RATE_PARENT
 *   isp = SUNXI_CCU_M_WITH_MUX_GATE, reg 0x1860, M = bits 0..4, mux = 24..26
 *         parents = { pll-video2-4x, pll-peri0-480m, pll-peri0-400m,
 *                     pll-peri0-600m, pll-video0-4x, pll-video1-4x }
 *         flags  = CLK_SET_RATE_PARENT | CLK_SET_RATE_NO_REPARENT
 *
 * The VIN driver asks for the device tree rate (csi_isp = <540000000>) via
 * __vin_set_isp_clk_rate() -> clk_set_rate(isp, 540000000); the measured result
 * is 324 MHz, so the request is silently clamped.  This module first prints the
 * achievable set with clk_round_rate() (read-only), then optionally really sets
 * rates and reports everything that moved - in particular whether pll-video0
 * itself was retuned, which is the side effect to watch for: pll-video0 also
 * feeds pll-video0-3x (csi_mclk*_pll) and csi-master*.
 *
 * Clocks are fetched the same way the driver does it, of_clk_get() by index
 * into the vind node's `clocks` list:
 *   0 csi_top   1 csi_top_src   2 csi_mclk0  3 .._24m  4 .._pll
 *   11 csi_isp  12 csi_isp_src  13 csi_bus   14 csi_mbus  15 csi_isp_mbus
 *
 * Usage:
 *   insmod ispclk_scan.ko                    # table only, no clock is written
 *   insmod ispclk_scan.ko apply=1            # walk set_list[]
 *   insmod ispclk_scan.ko apply=1 rate=216000000
 *   rmmod ispclk_scan                        # restores the original rate
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/io.h>

#define MAXCHAIN	8
#define ISP_ORIG_DEFAULT 324000000UL

static unsigned int apply;
module_param(apply, uint, 0444);
MODULE_PARM_DESC(apply, "1 = really call clk_set_rate (0 = read-only table)");

static unsigned long rate;
module_param(rate, ulong, 0444);
MODULE_PARM_DESC(rate, "extra single rate to set, 0 = none");

static unsigned int hold;
module_param(hold, uint, 0644);
MODULE_PARM_DESC(hold, "1 = leave the clock where it is instead of restoring at the end of init");

static unsigned long restore_rate = 324000000UL;
module_param(restore_rate, ulong, 0644);
MODULE_PARM_DESC(restore_rate, "rate put back on rmmod");

static unsigned int target;
module_param(target, uint, 0444);
MODULE_PARM_DESC(target, "chain index to act on: 0 = isp, 1 = mux, 2 = PLL branch");

/* the ISP clock is the one that actually gates the ISP pipeline */
static unsigned int clk_idx = 11;
module_param(clk_idx, uint, 0444);
MODULE_PARM_DESC(clk_idx, "index into vind's clocks list (11 = csi_isp)");

static const unsigned long set_list[] = {
	600000000UL, 540000000UL, 486000000UL, 432000000UL, 400000000UL,
	360000000UL, 324000000UL, 300000000UL, 270000000UL, 243000000UL,
	216000000UL, 200000000UL, 180000000UL, 162000000UL, 150000000UL,
	135000000UL, 120000000UL, 108000000UL, 100000000UL, 90000000UL,
	81000000UL, 72000000UL, 54000000UL,
};

static struct clk *chain[MAXCHAIN];
static int chain_len;
static unsigned long chain_orig[MAXCHAIN];

struct obs { const char *label; unsigned int idx; struct clk *clk; };
static struct obs observed[] = {
	{ "csi_top  ", 0,  NULL },
	{ "csi_top_s", 1,  NULL },
	{ "csi_mclk0", 2,  NULL },
	{ "mclk0_pll", 4,  NULL },
	{ "csi_isp  ", 11, NULL },
	{ "isp_src  ", 12, NULL },
	{ "isp_mbus ", 15, NULL },
};
static int n_observed;

static struct device_node *vin_np;

static void dump_state(const char *tag)
{
	int i;

	pr_info("ispclk[%s] chain :", tag);
	for (i = 0; i < chain_len; i++)
		pr_cont(" [%d]=%lu", i, clk_get_rate(chain[i]));
	pr_cont("\n");

	pr_info("ispclk[%s] others:", tag);
	for (i = 0; i < n_observed; i++)
		pr_cont(" %s=%lu", observed[i].label, clk_get_rate(observed[i].clk));
	pr_cont("\n");
}

static int resolve(void)
{
	struct clk *c;
	int i;

	vin_np = of_find_compatible_node(NULL, NULL, "allwinner,sunxi-vin-media");
	if (!vin_np) {
		pr_err("ispclk: no sunxi-vin-media node\n");
		return -ENODEV;
	}

	for (i = 0; i < (int)ARRAY_SIZE(observed); i++) {
		c = of_clk_get(vin_np, observed[i].idx);
		if (IS_ERR_OR_NULL(c)) {
			pr_warn("ispclk: of_clk_get(idx %u) for %s failed: %ld\n",
				observed[i].idx, observed[i].label,
				PTR_ERR_OR_ZERO(c));
			observed[i].clk = NULL;
			continue;
		}
		observed[i].clk = c;
		n_observed++;
	}

	c = of_clk_get(vin_np, clk_idx);
	if (IS_ERR_OR_NULL(c)) {
		pr_err("ispclk: of_clk_get(idx %u) failed: %ld\n", clk_idx,
		       PTR_ERR_OR_ZERO(c));
		return -ENODEV;
	}
	chain[0] = c;

	c = chain[0];
	for (i = 0; i < MAXCHAIN; i++) {
		chain[i] = c;
		chain_orig[i] = clk_get_rate(c);
		chain_len = i + 1;
		c = clk_get_parent(c);
		if (!c)
			break;
	}
	return 0;
}

static void dump_round_table(void)
{
	unsigned long l;
	unsigned long last = 0;
	int first = 1;

	pr_info("ispclk: ---- clk_round_rate(chain[%u], R), read-only ----\n", target);
	for (l = 10000000UL; l <= 800000000UL; l += 20000000UL) {
		unsigned long r = clk_round_rate(chain[target], l);

		if (IS_ERR_VALUE(r)) {
			if (first || last != 0) {
				pr_info("ispclk:   req %9lu -> ERR %ld\n", l, (long)r);
				last = 0;
				first = 0;
			}
			continue;
		}
		if (first || r != last) {
			pr_info("ispclk:   req %9lu -> round %9lu\n", l, r);
			last = r;
			first = 0;
		}
	}
}

static void do_one(unsigned long asked)
{
	unsigned long before[MAXCHAIN];
	unsigned long b_obs[16];
	int i;
	int moved = 0;

	for (i = 0; i < chain_len; i++)
		before[i] = clk_get_rate(chain[i]);
	for (i = 0; i < n_observed; i++)
		b_obs[i] = clk_get_rate(observed[i].clk);

	if (clk_set_rate(chain[target], asked)) {
		pr_err("ispclk: clk_set_rate(chain[%u], %lu) FAILED\n", target, asked);
		return;
	}
	udelay(200);

	for (i = 0; i < chain_len; i++)
		if (before[i] != clk_get_rate(chain[i]))
			moved = 1;

	pr_info("ispclk: set chain[%u] req=%lu isp %lu -> %lu%s\n",
		target, asked, b_obs[4], clk_get_rate(observed[4].clk),
		moved ? "   [chain moved]" : "");
	for (i = 1; i < chain_len; i++)
		if (before[i] != clk_get_rate(chain[i]))
			pr_info("ispclk:     chain[%d] %lu -> %lu  <== MOVED\n", i,
				before[i], clk_get_rate(chain[i]));
	for (i = 0; i < n_observed; i++)
		if (i != 4 && observed[i].clk &&
		    b_obs[i] != clk_get_rate(observed[i].clk))
			pr_warn("ispclk:     !!! %s %lu -> %lu\n", observed[i].label,
				b_obs[i], clk_get_rate(observed[i].clk));
}

/*
 * Live knob: echo a rate into /sys/module/ispclk_scan/parameters/set_rate and
 * the ISP clock is moved right away.  Used by the frame-loss threshold scan
 * (step the clock while vfr keeps streaming, then read lost_cnt).
 */
static int set_rate_set(const char *val, const struct kernel_param *kp)
{
	unsigned long want, got;
	int ret;

	ret = kstrtoul(val, 0, &want);
	if (ret)
		return ret;
	if (!chain[0])
		return -ENODEV;

	pr_info("ispclk: knob set_rate <- %lu (was %lu)\n", want, clk_get_rate(chain[0]));
	ret = clk_set_rate(chain[target], want);
	if (ret) {
		pr_err("ispclk: knob clk_set_rate failed %d\n", ret);
		return ret;
	}
	udelay(200);
	got = clk_get_rate(chain[0]);
	pr_info("ispclk: knob result isp=%lu isp_parent=%lu pll-video0=%lu csi_top=%lu%s\n",
		got, chain[1] ? clk_get_rate(chain[1]) : 0,
		chain[2] ? clk_get_rate(chain[2]) : 0,
		observed[0].clk ? clk_get_rate(observed[0].clk) : 0,
		got == want ? "" : "  (not exact)");
	*(unsigned long *)kp->arg = want;
	return 0;
}

static const struct kernel_param_ops set_rate_ops = {
	.set = set_rate_set,
	.get = param_get_ulong,
};
module_param_cb(set_rate, &set_rate_ops, &rate, 0644);
MODULE_PARM_DESC(set_rate, "write a rate here to move the ISP clock immediately");

/*
 * Second knob for the CSI ("csi_top") clock.  isp and csi are two M dividers on
 * the *same* parent pll-video0-4x, so moving the ISP clock by retuning the
 * parent drags the CSI clock along.  Requests that the current parent divides
 * exactly are satisfied with the ISP's own M divider alone, so the sequence
 *   set_rate=648000000 ; set_rate_csi=324000000
 * pins CSI at 324 MHz while the ISP core runs at 648 MHz - which is what makes
 * it possible to attribute a failure to the ISP core rather than to the parser.
 */
static unsigned long csi_rate;

static int set_rate_csi_set(const char *val, const struct kernel_param *kp)
{
	unsigned long want;
	int ret;

	ret = kstrtoul(val, 0, &want);
	if (ret)
		return ret;
	if (!chain[1] || !observed[0].clk)
		return -ENODEV;

	pr_info("ispclk: knob set_rate_csi <- %lu (was %lu)\n", want,
		clk_get_rate(observed[0].clk));
	ret = clk_set_rate(chain[1], want);	/* chain[1] = pll-video0-4x */
	if (ret) {
		pr_err("ispclk: clk_set_rate(pll-video0-4x, %lu) failed %d\n", want, ret);
		return ret;
	}
	ret = clk_set_rate(observed[0].clk, want);	/* csi_top */
	if (ret) {
		pr_err("ispclk: clk_set_rate(csi_top, %lu) failed %d\n", want, ret);
		return ret;
	}
	udelay(200);
	pr_info("ispclk: knob csi -> csi_top=%lu csi_src=%lu isp=%lu isp_parent=%lu pll-video0=%lu\n",
		clk_get_rate(observed[0].clk), clk_get_rate(observed[1].clk),
		clk_get_rate(chain[0]), clk_get_rate(chain[1]),
		chain[2] ? clk_get_rate(chain[2]) : 0);
	*(unsigned long *)kp->arg = want;
	return 0;
}

static const struct kernel_param_ops set_rate_csi_ops = {
	.set = set_rate_csi_set,
	.get = param_get_ulong,
};
module_param_cb(set_rate_csi, &set_rate_csi_ops, &csi_rate, 0644);
MODULE_PARM_DESC(set_rate_csi, "write a rate here to move the CSI (csi_top) clock immediately");

/*
 * Raw divider knobs.  clk_set_rate() decides for itself which divider to use and
 * whether to retune the shared PLL first; retuning pll-video0 while the ISP is
 * running was measured to trip spurious "hblank short" resets (486 MHz failed
 * while 648 MHz was fine), so a measurement of the ISP's own limit needs the
 * divider changed with nothing else moving.
 *
 *   0x2003840 CSI_CLK_REG : M = bits 0..4 (value = div-1), mux 24..26, gate 31
 *   0x2003860 ISP_CLK_REG : same layout
 *
 * Writing only the M field keeps mux and gate untouched.  Plausible ladder with
 * pll-video0-4x at 648 MHz (isp_csi parent 648 = pll-video0 1296 / 2):
 *   csi_div=2 -> csi 324 MHz, isp_div 2/3/4/6 -> isp 324/216/162/108 MHz.
 */
#define CCU_BASE	0x2002000
#define CCU_LEN		0x2000
#define REG_CSI_CLK	0x1840
#define REG_ISP_CLK	0x1860

static void __iomem *ccu;
static unsigned int isp_div;
static unsigned int csi_div;

static void set_div(void __iomem *reg, unsigned int div, const char *what)
{
	u32 v = readl(reg);

	if (div < 1 || div > 32) {
		pr_err("ispclk: %s: bad divider %u\n", what, div);
		return;
	}
	v = (v & ~0x1fUL) | (div - 1);
	writel(v, reg);
	pr_info("ispclk: %s <- div %u (reg 0x%08x = 0x%08x)\n", what, div,
		(u32)(uintptr_t)reg, readl(reg));
}

static int isp_div_set(const char *val, const struct kernel_param *kp)
{
	unsigned int d;
	int ret = kstrtouint(val, 0, &d);
	unsigned long parent;

	if (ret || !ccu)
		return ret ? ret : -ENODEV;
	set_div(ccu + REG_ISP_CLK, d, "isp");
	parent = chain[1] ? clk_get_rate(chain[1]) : 0;
	pr_info("ispclk: isp div=%u -> isp ~= %lu MHz (parent reported %lu MHz, csi %lu MHz)\n",
		d, parent / d / 1000000, parent / 1000000,
		observed[0].clk ? clk_get_rate(observed[0].clk) / 1000000 : 0);
	*(unsigned int *)kp->arg = d;
	return 0;
}

static int csi_div_set(const char *val, const struct kernel_param *kp)
{
	unsigned int d;
	int ret = kstrtouint(val, 0, &d);

	if (ret || !ccu)
		return ret ? ret : -ENODEV;
	set_div(ccu + REG_CSI_CLK, d, "csi");
	*(unsigned int *)kp->arg = d;
	return 0;
}

static const struct kernel_param_ops isp_div_ops = { .set = isp_div_set, .get = param_get_uint };
static const struct kernel_param_ops csi_div_ops = { .set = csi_div_set, .get = param_get_uint };
module_param_cb(isp_div, &isp_div_ops, &isp_div, 0644);
MODULE_PARM_DESC(isp_div, "raw ISP_CLK_REG divider (1..32), bypasses clk_set_rate");
module_param_cb(csi_div, &csi_div_ops, &csi_div, 0644);
MODULE_PARM_DESC(csi_div, "raw CSI_CLK_REG divider (1..32), bypasses clk_set_rate");

/*
 * The VIN driver reprograms the ISP/CSI clocks on every stream-on, so an
 * externally pre-set rate is silently undone.  These kprobes log the rate the
 * driver asks for, which is the difference between "the silicon cannot do it"
 * and "the driver never asks for more".
 */
static int kp_enter(struct kprobe *p, struct pt_regs *regs)
{
	unsigned long rate = regs_get_kernel_argument(regs, 1);

	pr_info("ispclk: %s(vind, rate=%lu Hz = %lu MHz)\n", p->symbol_name,
		rate, rate / 1000000UL);
	return 0;
}

static struct kprobe kps[] = {
	{ .symbol_name = "__vin_set_isp_clk_rate", .pre_handler = kp_enter },
	{ .symbol_name = "__vin_set_top_clk_rate", .pre_handler = kp_enter },
};

static void kprobe_probe(void)
{
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(kps); i++) {
		int ret = register_kprobe(&kps[i]);
		if (ret) {
			pr_warn("ispclk: kprobe on %s failed: %d (kprobes disabled?)\n",
				kps[i].symbol_name, ret);
			kps[i].symbol_name = NULL;
		} else {
			pr_info("ispclk: kprobe armed on %s\n", kps[i].symbol_name);
		}
	}
}

static int __init ispclk_scan_init(void)
{
	int i;

	if (resolve())
		return -ENODEV;

	ccu = ioremap(CCU_BASE, CCU_LEN);
	if (!ccu)
		pr_warn("ispclk: ioremap(0x%x) failed, raw divider knobs disabled\n",
			CCU_BASE);
	else
		pr_info("ispclk: CCU mapped at 0x%x: CSI_CLK_REG=0x%08x ISP_CLK_REG=0x%08x\n",
			CCU_BASE, readl(ccu + REG_CSI_CLK), readl(ccu + REG_ISP_CLK));

	/* find out what the VIN driver actually asks the clock framework for */
	kprobe_probe();

	pr_info("ispclk: ================ base (clk_idx %u, target %u) ================\n",
		clk_idx, target);
	dump_state("base");
	for (i = 0; i < n_observed; i++)
		pr_info("ispclk:   %s idx=%-2u rate=%lu\n", observed[i].label,
			observed[i].idx, clk_get_rate(observed[i].clk));
	dump_round_table();

	if (!apply) {
		pr_info("ispclk: read-only run done (apply=0); set_rate knob is live\n");
		return 0;
	}

	pr_info("ispclk: ================ apply to chain[%u] ================\n", target);
	for (i = 0; i < (int)ARRAY_SIZE(set_list); i++)
		do_one(set_list[i]);
	if (rate)
		do_one(rate);

	if (hold) {
		pr_info("ispclk: hold=1, leaving isp at %lu (rmmod restores %lu)\n",
			clk_get_rate(chain[0]), restore_rate);
		dump_state("held");
		return 0;
	}

	pr_info("ispclk: ================ restore ================\n");
	if (clk_set_rate(chain[target], chain_orig[target]))
		pr_err("ispclk: restore FAILED\n");
	udelay(200);
	dump_state("restored");

	return 0;
}

static void __exit ispclk_scan_exit(void)
{
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(kps); i++)
		if (kps[i].symbol_name)
			unregister_kprobe(&kps[i]);

	if (chain[0]) {
		clk_set_rate(chain[target], restore_rate);
		udelay(100);
		pr_info("ispclk: exit, chain[%u] set to %lu, now %lu\n", target,
			restore_rate, clk_get_rate(chain[target]));
		dump_state("exit");
	}
	for (i = 0; i < n_observed; i++)
		if (observed[i].clk)
			clk_put(observed[i].clk);
	if (ccu)
		iounmap(ccu);
	if (chain[0])
		clk_put(chain[0]);
	if (vin_np)
		of_node_put(vin_np);
}

module_init(ispclk_scan_init);
module_exit(ispclk_scan_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sun60iw2 ISP clock range probe (temporary, not in patches/)");
