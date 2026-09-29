#!/bin/bash
# A733 / Orange Pi Zero 3W 构建预检。
#
#   bash e902/tools/preflight-image.sh
#
# 只读检查，不修改任何东西；退出码 = FAIL 数（warn 不计）。
# 第 2 项（内核树 .config）分编前/编后两种情形：
#   - 还没按当前 userpatches 配置编过内核 -> 只 warn（编译前必然是红的）
#   - 已经按当前配置编过（deb 比配置新）   -> 才是 FAIL
set -u

SRC=$(cd "$(dirname "$0")/../.." && pwd)
CFG="$SRC/userpatches/linux-sun60iw2-current-a733.config"
KCFG="$SRC/kernel/orange-pi-6.6-sun60iw2/.config"
DTSI="$SRC/kernel/orange-pi-6.6-sun60iw2/arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi"
SCP="$SRC/external/packages/pack-uboot/sun60iw2/bin/scp.fex"
UBOOT_SCP="$SRC/u-boot/v2018.05-sun60iw2/scp.fex"

# 期望的 scp.fex（2026-09-25 起打包源 = vendor-scp.bin，构建自
# e902/vendor-scp/build.sh；出厂件备份在 e902/backup/scp.fex.factory-20260925）
VENDOR_SCP_MD5="b9904524a554fc6537c7b11093e72c83"

fail=0
ok()   { printf '  [ok]   %s\n' "$1"; }
bad()  { printf '  [FAIL] %s\n' "$1"; fail=$((fail + 1)); }
warn() { printf '  [warn] %s\n' "$1"; }

node_status() {	# $1=节点标签 $2=文件
	grep -A14 "$1" "$2" 2>/dev/null | grep -m1 'status' | tr -d ' \t'
}

echo "== 1. 内核配置：$CFG"
if [ ! -f "$CFG" ]; then
	bad "文件不存在"
else
	grep -q '^CONFIG_PREEMPT_RT=y' "$CFG" \
		&& ok "CONFIG_PREEMPT_RT=y" \
		|| bad "缺少 CONFIG_PREEMPT_RT=y"
	if grep -q '^CONFIG_PREEMPT=y' "$CFG"; then
		bad "还存在 CONFIG_PREEMPT=y —— 它和 PREEMPT_RT 同一个 kconfig choice，会把 RT 顶掉"
	else
		ok "没有 CONFIG_PREEMPT=y（不会顶掉 RT）"
	fi
	grep -q '^CONFIG_AW_MSGBOX=y' "$CFG" \
		&& ok "CONFIG_AW_MSGBOX=y（ARM 侧收 E902 消息必需）" \
		|| bad "缺少 CONFIG_AW_MSGBOX=y"
	grep -q '^CONFIG_AW_HWSPINLOCK=y' "$CFG" \
		&& ok "CONFIG_AW_HWSPINLOCK=y" \
		|| warn "缺少 CONFIG_AW_HWSPINLOCK=y"
	grep -q '^# CONFIG_STRICT_DEVMEM is not set' "$CFG" \
		&& ok "CONFIG_STRICT_DEVMEM 关闭（/dev/mem 工具要用）" \
		|| warn "STRICT_DEVMEM 不是关闭状态，/dev/mem 访问可能被拦"
fi

# 判断内核树 .config 是否已经由当前配置产出：内核 deb 比配置新就算编过
KDEB=$(ls -1t "$SRC"/output/debs/linux-image-current-sun60iw2_*.deb 2>/dev/null | head -1)
built_with_current=no
[ -n "$KDEB" ] && [ "$KDEB" -nt "$CFG" ] && built_with_current=yes

echo "== 2. 内核树 .config：$KCFG"
if [ ! -f "$KCFG" ]; then
	warn "还没有 .config（内核未编译过），先编再回来看这里"
else
	if grep -q '^CONFIG_PREEMPT_RT=y' "$KCFG" && ! grep -q '^CONFIG_PREEMPT=y' "$KCFG"; then
		ok "CONFIG_PREEMPT_RT=y 且无 CONFIG_PREEMPT=y —— RT 真的生效了"
	else
		if [ "$built_with_current" = yes ]; then
			bad "已按当前配置编过内核，但 .config 里 RT 没生效 —— 别刷这个镜像"
		else
			warn "这份 .config 早于当前 userpatches 配置（还是上一次非 RT 的那份）：编译前必然如此，直接编即可，编完再跑一次本脚本"
		fi
	fi
fi

echo "== 3. E902 固件（会打进 boot_package.fex）"
if [ -f "$SCP" ]; then
	m=$(md5sum "$SCP" | cut -d' ' -f1)
	if [ "$m" = "$VENDOR_SCP_MD5" ]; then
		ok "scp.fex = 厂商件 ($m)"
	else
		warn "scp.fex 不是厂商件 ($m) —— 换过固件？上一轮实测换掉会导致 bl31 握不上手、板子起不来，刷之前先备好 SD"
	fi
else
	bad "$SCP 不存在（u-boot 打包会失败）"
fi
if [ -f "$UBOOT_SCP" ]; then
	m2=$(md5sum "$UBOOT_SCP" | cut -d' ' -f1)
	[ "$m2" = "$(md5sum "$SCP" 2>/dev/null | cut -d' ' -f1)" ] \
		&& ok "u-boot 树里的 scp.fex 与打包源一致" \
		|| warn "u-boot 树里的 scp.fex 与打包源不同（构建时会被覆盖，一般无害）"
fi

echo "== 4. CPUS 域外设必须留给 E902（保持 disabled）"
for n in 'r_spi: spi@7092000' 'uart7: uart@7080000'; do
	st=$(node_status "$n" "$DTSI")
	case "$st" in
		*disabled*) ok "$n -> $st" ;;
		'')         warn "$n 没找到 status" ;;
		*)          bad "$n -> $st（Linux 会和 E902 抢这个控制器）" ;;
	esac
done

echo "== 5. 磁盘与产物"
df -h "$SRC" | tail -1 | awk '{printf "  /home: 共 %s, 已用 %s, 可用 %s (%s)\n", $2, $3, $4, $5}'
avail=$(df -Pk "$SRC" | awk 'NR==2 {print int($4/1024/1024)}')
[ "$avail" -ge 25 ] && ok "可用空间 ${avail}G（整机镜像建议 >=25G）" || warn "可用空间只有 ${avail}G，整机镜像可能不够"
IMGS=$(find "$SRC"/output/images -name '*.img' -o -name '*.img.gz' 2>/dev/null | sort)
if [ -n "$IMGS" ]; then
	ok "已有镜像产物："
	while read -r i; do
		[ -n "$i" ] && ls -lh "$i" | awk '{printf "         %s  %s %s %s\n", $5, $6, $7, $9}'
	done <<< "$IMGS"
else
	warn "output/images/ 下还没有 .img"
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "预检通过（warn 不影响构建）。构建命令："
	echo "  cd $SRC"
	echo "  sudo ./build.sh a733                      # 整机镜像（配置见 userpatches/config-a733.conf）"
	echo "  sudo ./build.sh a733 BUILD_OPT=kernel     # 只重编内核"
	echo "  sudo ./build.sh a733 BUILD_OPT=u-boot     # 只重打 u-boot"
	echo "  sudo ./build.sh a733 CLEAN_LEVEL=oldcache # 复用已有 deb/rootfs 缓存，最快"
else
	echo "有 $fail 项 FAIL，先修掉再编。"
fi
exit "$fail"
