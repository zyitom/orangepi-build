#!/usr/bin/env bash
# gen-port.sh —— 在 IgH stable-1.6 源码树内生成 RK35xx 原生 stmmac/dwmac-rk
# EtherCAT 驱动（针对 OPI5 的 6.1.99 BSP 内核）。
#
# 设计（对应 2026-10-18 的调研结论）：
#   * core  = 官方 stable-1.6 的 *-6.1-ethercat.c（mainline 6.1.99 + ecdev 变换，
#             已验证与 BSP 核心逐字节等价），再 backport BSP 相对 v6.1.99 的
#             厂商增量（合计 ~140 行：get_eth_addr/integrated_phy_power/
#             flow_ctrl/dma-size/mdio_idle/STMMAC_FULL 开关等）。
#   * glue  = BSP 的 dwmac-rk.c / dwmac-rk-tool.c / stmmac_platform.c 原样拷入，
#             只做机械变换：include 指向 ethercat 头、dvr_probe→ec 变体、
#             驱动名 rk_gmac-dwmac-ec（避免与内置 =y 驱动同名注册失败）、
#             module_platform_driver → 自定义 init/exit（调 stmmac_init/exit）。
#   * 单模块 ec_dwmac_rockchip.ko = core + platform + rk glue + tool，
#     全部内部链接，零 EXPORT_SYMBOL —— 内置驱动导出的同名符号
#     （stmmac_dvr_probe 等 15 个）不会遮蔽本模块的调用。
#   * ecdev_open 延迟 100ms（VJW 0005/0008 的结论：platform probe 上下文里
#     不能直接 open；RK 胶水无 runtime PM，时钟不会被门控）。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC_TARBALL="$ROOT/igh-stable-1.6.tar.gz"
KERNEL_STMMAC="$ROOT/../kernel/orange-pi-6.1-rk35xx/drivers/net/ethernet/stmicro/stmmac"
REF6199=/tmp/v6199
DEV="$ROOT/src/devices/stmmac"

if [ ! -f "$SRC_TARBALL" ]; then
    echo "ERROR: $SRC_TARBALL 不存在（先从 gitlab stable-1.6 archive 下载）" >&2
    exit 1
fi
if [ ! -d "$REF6199" ]; then
    echo "ERROR: $REF6199 不存在（mainline v6.1.99 基准文件）" >&2
    exit 1
fi

echo "== 0) 恢复 pristine IgH stable-1.6 =="
rm -rf "$ROOT/src"
tar xzf "$SRC_TARBALL" -C "$ROOT"
mv "$ROOT"/ethercat-stable-1.6-* "$ROOT/src"
mkdir -p "$ROOT/patches"

echo "== 1) 拷贝 BSP 胶水文件 =="
cp "$KERNEL_STMMAC/dwmac-rk.c"        "$DEV/dwmac-rk-6.1-ethercat.c"
cp "$KERNEL_STMMAC/dwmac-rk-tool.c"   "$DEV/dwmac-rk-tool-6.1-ethercat.c"
cp "$KERNEL_STMMAC/dwmac-rk-tool.h"   "$DEV/dwmac-rk-tool-6.1-ethercat.h"
cp "$KERNEL_STMMAC/stmmac_platform.c" "$DEV/stmmac_platform-6.1-ethercat.c"
cp "$KERNEL_STMMAC/stmmac_platform.h" "$DEV/stmmac_platform-6.1-ethercat.h"

echo "== 2) include 指向 ethercat 头 =="
sed -i \
    -e 's|#include "stmmac.h"|#include "stmmac-6.1-ethercat.h"|g' \
    -e 's|#include "stmmac_platform.h"|#include "stmmac_platform-6.1-ethercat.h"|g' \
    -e 's|#include "dwmac-rk-tool.h"|#include "dwmac-rk-tool-6.1-ethercat.h"|g' \
    -e 's|#include "dwmac1000.h"|#include "dwmac1000-6.1-ethercat.h"|g' \
    -e 's|#include "dwmac_dma.h"|#include "dwmac_dma-6.1-ethercat.h"|g' \
    "$DEV/dwmac-rk-6.1-ethercat.c" \
    "$DEV/dwmac-rk-tool-6.1-ethercat.c" \
    "$DEV/dwmac-rk-tool-6.1-ethercat.h" \
    "$DEV/stmmac_platform-6.1-ethercat.c" \
    "$DEV/stmmac_platform-6.1-ethercat.h"

echo "== 3) backport BSP 厂商增量到官方 ethercat core 副本 =="
apply_delta() {  # apply_delta <v6.1.99 file> <target file>
    local src="$1" dst="$2" base
    base="$(basename "$src")"
    diff -u "$REF6199/$src" "$KERNEL_STMMAC/$src" > "$ROOT/patches/backport-$base.patch" || true
    if ! patch --quiet "$dst" < "$ROOT/patches/backport-$base.patch"; then
        echo "ERROR: vendor delta 应用失败: $dst（见 .rej）" >&2
        exit 1
    fi
}
apply_delta stmmac_main.c      "$DEV/stmmac_main-6.1-ethercat.c"
apply_delta stmmac_mdio.c      "$DEV/stmmac_mdio-6.1-ethercat.c"
apply_delta hwif.c             "$DEV/hwif-6.1-ethercat.c"
apply_delta stmmac.h           "$DEV/stmmac-6.1-ethercat.h"
apply_delta dwmac4_core.c      "$DEV/dwmac4_core-6.1-ethercat.c"
apply_delta common.h           "$DEV/common-6.1-ethercat.h"
apply_delta stmmac_ethtool.c   "$DEV/stmmac_ethtool-6.1-ethercat.c"
apply_delta dwmac4_lib.c       "$DEV/dwmac4_lib-6.1-ethercat.c"

echo "== 4) 胶水文件变换 =="
# 4a) 调用官方 ethercat 探测入口（core 侧改了名：probe/remove 带 ec_，
#     bus_clks_config 变成 ec_stmmac_bus_clks_config；suspend/resume/init/exit 同名）
sed -i \
    -e 's/\bstmmac_dvr_probe\b/stmmac_ec_dvr_probe/g' \
    -e 's/\bstmmac_dvr_remove\b/stmmac_ec_dvr_remove/g' \
    -e 's/\bstmmac_bus_clks_config\b/ec_stmmac_bus_clks_config/g' \
    "$DEV/dwmac-rk-6.1-ethercat.c" \
    "$DEV/stmmac_platform-6.1-ethercat.c"
# 4b) 驱动名避开内置 rk_gmac-dwmac（当前内核 STMMAC=y，同名会注册失败）
sed -i 's/\.name[[:space:]]*= *"rk_gmac-dwmac"/.name           = "rk_gmac-dwmac-ec"/' \
    "$DEV/dwmac-rk-6.1-ethercat.c"
# 4b2) 官方头把 STMMAC_RESOURCE_NAME 改名成 "ec_stmmaceth"（PCI 变体没有 DT
#      时钟，无所谓）；平台设备的 DT clock/reset con_id 是 "stmmaceth"，必须
#      还原——否则 CSR 时钟 get 失败，MAC 时钟不开，寄存器读 0，open 必败。
sed -i 's/\bSTMMAC_RESOURCE_NAME\b/"stmmaceth"/g' \
    "$DEV/stmmac_platform-6.1-ethercat.c"
# 4c) 单模块：去掉全部 EXPORT_SYMBOL（避免成为内核同名导出的第二提供者）
sed -i '/^EXPORT_SYMBOL/d' \
    "$DEV/dwmac-rk-6.1-ethercat.c" \
    "$DEV/dwmac-rk-tool-6.1-ethercat.c" \
    "$DEV/stmmac_platform-6.1-ethercat.c" \
    "$DEV/stmmac_mdio-6.1-ethercat.c"

echo "== 5) module init/exit（注册前先 stmmac_init）+ ecdev_open 延迟化 =="
python3 - "$DEV" <<'PYEOF'
import sys, pathlib

dev = pathlib.Path(sys.argv[1])

# ---- dwmac-rk-6.1-ethercat.c: module_platform_driver -> init/exit ----
p = dev / "dwmac-rk-6.1-ethercat.c"
t = p.read_text()
old = "module_platform_driver(rk_gmac_dwmac_driver);"
new = """static int __init ec_dwmac_rk_init_module(void)
{
	int ret = stmmac_init();

	if (ret)
		return ret;

	ret = platform_driver_register(&rk_gmac_dwmac_driver);
	if (ret)
		stmmac_exit();

	return ret;
}

static void __exit ec_dwmac_rk_exit_module(void)
{
	platform_driver_unregister(&rk_gmac_dwmac_driver);
	stmmac_exit();
}

module_init(ec_dwmac_rk_init_module);
module_exit(ec_dwmac_rk_exit_module);"""
assert old in t, "anchor module_platform_driver not found"
t = t.replace(old, new, 1)
t = t.replace('MODULE_DESCRIPTION("Rockchip RK3288 DWMAC specific glue layer");',
              'MODULE_DESCRIPTION("Rockchip DWMAC glue layer (IgH EtherCAT native)");', 1)
p.write_text(t)

# ---- stmmac-6.1-ethercat.h: 延迟 open 的 priv 字段 ----
p = dev / "stmmac-6.1-ethercat.h"
t = p.read_text()
old = "\tbool ecdev_initialized;"
new = "\tbool ecdev_initialized;\n\n\t/* 延迟 native open：platform probe 上下文不能直接 ecdev_open。 */\n\tstruct delayed_work ec_open_work;\n\tbool ecdev_opened;"
assert old in t, "anchor ecdev_initialized not found"
t = t.replace(old, new, 1)
p.write_text(t)

# ---- stmmac_main-6.1-ethercat.c: deferred open ----
p = dev / "stmmac_main-6.1-ethercat.c"
t = p.read_text()

# 5a) probe 里 ecdev_initialized 置位处补 opened 标志
old = "\tpriv->ecdev_ = ecdev_offer(ndev, ec_poll, THIS_MODULE);\n\tpriv->ecdev_initialized = true;"
new = "\tpriv->ecdev_ = ecdev_offer(ndev, ec_poll, THIS_MODULE);\n\tpriv->ecdev_initialized = true;\n\tpriv->ecdev_opened = false;"
assert old in t, "anchor ecdev_offer not found"
t = t.replace(old, new, 1)

# 5b) probe 末尾：立即 open -> 延迟 100ms
old = """	pm_runtime_put(device);
	if (get_ecdev(priv)) {
		init_irq_work(&priv->ec_watchdog_kicker, ec_kick_watchdog);
		ret = ecdev_open(get_ecdev(priv));
		if (ret) {
			ecdev_withdraw(get_ecdev(priv));
			goto error_netdev_register;
		}
	}
"""
new = """	pm_runtime_put(device);
	if (get_ecdev(priv)) {
		init_irq_work(&priv->ec_watchdog_kicker, ec_kick_watchdog);
		/* RK 胶水要在 probe 完成后才算就绪（时钟/GRF/powerup），且
		 * 不能在 probe 上下文里拉起主站栈：延迟到 work 里 open。 */
		INIT_DELAYED_WORK(&priv->ec_open_work, stmmac_ec_open_work);
		schedule_delayed_work(&priv->ec_open_work, msecs_to_jiffies(100));
	}
"""
assert old in t, "anchor probe-end ecdev_open not found"
t = t.replace(old, new, 1)

# 5c) 延迟 open 的 work 函数（放在 netdev_ops 前）
old = "static const struct net_device_ops stmmac_netdev_ops = {"
new = """static void stmmac_ec_open_work(struct work_struct *work)
{
	struct stmmac_priv *priv = container_of(to_delayed_work(work),
			struct stmmac_priv, ec_open_work);
	int ret;

	ret = ecdev_open(get_ecdev(priv));
	if (ret) {
		dev_err(priv->device, "deferred EtherCAT device open failed: %d\\n",
			ret);
		return;
	}

	priv->ecdev_opened = true;
	dev_info(priv->device, "deferred EtherCAT device open completed\\n");
}

static const struct net_device_ops stmmac_netdev_ops = {"""
assert old in t, "anchor netdev_ops not found"
t = t.replace(old, new, 1)

# 5d) remove：先取消延迟 work，只对已 open 的设备 ecdev_close
old = """	if (get_ecdev(priv)) {
		ecdev_close(get_ecdev(priv));
		irq_work_sync(&priv->ec_watchdog_kicker);
		ecdev_withdraw(get_ecdev(priv));
		priv->ecdev_ = NULL;
		priv->ecdev_initialized = false;
	} else {"""
new = """	if (get_ecdev(priv)) {
		cancel_delayed_work_sync(&priv->ec_open_work);
		if (priv->ecdev_opened) {
			ecdev_close(get_ecdev(priv));
			irq_work_sync(&priv->ec_watchdog_kicker);
		}
		ecdev_withdraw(get_ecdev(priv));
		priv->ecdev_ = NULL;
		priv->ecdev_initialized = false;
	} else {"""
assert old in t, "anchor remove ecdev_close not found"
t = t.replace(old, new, 1)
p.write_text(t)
print("   python transforms OK")

# ---- Kbuild.in: 增加 --enable-dwmac-rk 的单模块目标 ----
p = dev / "Kbuild.in"
t = p.read_text()
old = """ifeq (@ENABLE_STMMACPCI@,1)
	obj-m += ec_stmmac-pci.o
	ec_stmmac-pci-objs := \\
		$(EC_STMMAC_OBJS) \\
		stmmac_pci-@KERNEL_STMMAC@-ethercat.o
endif
endif"""
new = """ifeq (@ENABLE_STMMACPCI@,1)
	obj-m += ec_stmmac-pci.o
	ec_stmmac-pci-objs := \\
		$(EC_STMMAC_OBJS) \\
		stmmac_pci-@KERNEL_STMMAC@-ethercat.o
endif

# Rockchip 平台入口：core + platform(DT 解析) + dwmac-rk 胶水 + tool
# 链成单个模块，全部内部链接，不新增任何 EXPORT_SYMBOL。
ifeq (@ENABLE_DWMACROCKCHIP@,1)
	obj-m += ec_dwmac_rockchip.o
	ec_dwmac_rockchip-objs := \\
		$(EC_STMMAC_OBJS) \\
		stmmac_platform-@KERNEL_STMMAC@-ethercat.o \\
		dwmac-rk-@KERNEL_STMMAC@-ethercat.o \\
		dwmac-rk-tool-@KERNEL_STMMAC@-ethercat.o
endif
endif"""
assert old in t, "anchor stmmac-pci block not found in Kbuild.in"
t = t.replace(old, new, 1)
p.write_text(t)
print("   Kbuild.in OK")

# ---- Makefile.am: 把新文件加进 dist 列表 ----
p = dev / "Makefile.am"
t = p.read_text()
old = "\tstmmac_platform-6.1-orig.c \\\n" if "\tstmmac_platform-6.1-orig.c \\\n" in t else None
add = ""
for f in ["dwmac-rk-6.1-ethercat.c", "dwmac-rk-tool-6.1-ethercat.c",
          "dwmac-rk-tool-6.1-ethercat.h", "stmmac_platform-6.1-ethercat.c",
          "stmmac_platform-6.1-ethercat.h"]:
    add += "\t%s \\\n" % f
# 简单起见：追加到统一 dist 列表末尾（update.sh 是最后一项）
anchor = "update.sh"
assert anchor in t, "Makefile.am anchor not found"
t = t.replace(anchor, anchor + " \\\n" + add.rstrip(" \\\n"), 1)
p.write_text(t)
print("   Makefile.am OK")
PYEOF

echo "== 6) configure.ac 增加 --enable-dwmac-rk =="
python3 - "$ROOT/src" <<'PYEOF'
import sys, pathlib
p = pathlib.Path(sys.argv[1]) / "configure.ac"
t = p.read_text()
anchor = """AM_CONDITIONAL(ENABLE_DWMACINTEL, test "x$enabledwmacintel" = "x1")
AC_SUBST(ENABLE_DWMACINTEL, [$enabledwmacintel])
"""
new = anchor + """
AC_ARG_ENABLE([dwmac-rk],
    AS_HELP_STRING([--enable-dwmac-rk],
                   [Build the Rockchip dwmac-rk native driver [default=no]]),
    [
        case "${enableval}" in
            yes) enabledwmacrockchip=1
                 enablestmmac=1
                ;;
            no) enabledwmacrockchip=0
                ;;
            *) AC_MSG_ERROR([Invalid value for --enable-dwmac-rk])
                ;;
        esac
    ],
    [enabledwmacrockchip=0]
)
AM_CONDITIONAL(ENABLE_DWMACROCKCHIP, test "x$enabledwmacrockchip" = "x1")
AC_SUBST(ENABLE_DWMACROCKCHIP, [$enabledwmacrockchip])
"""
assert anchor in t, "configure.ac anchor not found"
t = t.replace(anchor, new, 1)
p.write_text(t)
print("   configure.ac OK")
PYEOF

echo "== 7) 自检 =="
fail=0
grep -q 'stmmac_ec_dvr_probe' "$DEV/dwmac-rk-6.1-ethercat.c" || { echo "FAIL: ec probe 未接线"; fail=1; }
grep -q 'rk_gmac-dwmac-ec' "$DEV/dwmac-rk-6.1-ethercat.c" || { echo "FAIL: 驱动名未改"; fail=1; }
grep -q 'stmmac_ec_open_work' "$DEV/stmmac_main-6.1-ethercat.c" || { echo "FAIL: 延迟 open 未加"; fail=1; }
grep -q 'ENABLE_DWMACROCKCHIP' "$DEV/Kbuild.in" || { echo "FAIL: Kbuild 未加目标"; fail=1; }
grep -q 'get_eth_addr' "$DEV/stmmac_main-6.1-ethercat.c" || { echo "FAIL: vendor MAC 钩子未 backport"; fail=1; }
! grep -rlq '#include "stmmac.h"' $(ls "$DEV"/*-ethercat.c "$DEV"/*-ethercat.h 2>/dev/null) || { echo "FAIL: 仍有裸 stmmac.h include"; fail=1; }
! find "$DEV" -name '*.rej' | grep -q . || { echo "FAIL: 存在 patch .rej"; fail=1; }
[ "$fail" = 0 ] && echo "   全部自检通过"
exit "$fail"
