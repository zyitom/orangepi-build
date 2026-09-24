#!/bin/sh
# 板上以 root 运行：启用/查看 40-pin 设备树覆层（写 /boot/orangepiEnv.txt 的 overlays= 行）
# 用法：  sh enable-overlay.sh                 # 只列出可用
#         sh enable-overlay.sh i2c0 pwm3 uart2  # 启用这些
set -u
ENV=/boot/orangepiEnv.txt
[ -f "$ENV" ] || { echo "no $ENV"; exit 1; }
PREFIX=$(grep -E '^overlay_prefix=' "$ENV" | cut -d= -f2)
[ -n "$PREFIX" ] || PREFIX=sun60i-a733
OVDIR=/boot/dtb/allwinner/overlay

echo "overlay_prefix = $PREFIX"
echo "available overlays:"
ls "$OVDIR" 2>/dev/null | grep "^${PREFIX}-" | sed "s/^${PREFIX}-//; s/\.dtbo$//" | tr '\n' ' '
echo

if [ "$#" -eq 0 ]; then
	echo "usage: sh $0 <name> [name ...]"
	exit 0
fi

# 校验
for n in "$@"; do
	if [ ! -f "$OVDIR/${PREFIX}-${n}.dtbo" ]; then
		echo "ERROR: unknown overlay '$n'"
		exit 2
	fi
done

NEW="$*"
BK="$ENV.bak-$(date +%Y%m%d-%H%M%S)"
cp -a "$ENV" "$BK"
echo "backup -> $BK"

if grep -qE '^overlays=' "$ENV"; then
	sed -i "s|^overlays=.*|overlays=${NEW}|" "$ENV"
else
	echo "overlays=${NEW}" >> "$ENV"
fi

echo "--- $ENV now: ---"
grep -E '^(overlays|overlay_prefix|fdtfile)=' "$ENV"
echo
echo "生效需重启：printf ' \\n' | sudo -S -p '' reboot"
echo "回退：cp -a $BK $ENV && printf ' \\n' | sudo -S -p '' reboot"
