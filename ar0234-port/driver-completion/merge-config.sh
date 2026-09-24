#!/bin/sh
# 幂等合并 config.fragment 到 userpatches/linux-sun60iw2-current-a733.config
# 在主机 TL101 上运行（不需要 sudo；该 config 属主 helios）。
set -e
ROOT="/home/helios/Desktop/orangepi-build"
CFG="$ROOT/userpatches/linux-sun60iw2-current-a733.config"
FRAG="$(cd "$(dirname "$0")" && pwd)/config.fragment"
[ -f "$CFG" ] || { echo "missing: $CFG"; exit 1; }
[ -f "$FRAG" ] || { echo "missing: $FRAG"; exit 1; }

BK="$CFG.bak-$(date +%Y%m%d-%H%M%S)"
cp -a "$CFG" "$BK"
echo "backup -> $BK"

# 1) 删掉这些符号的现有行（配置项 / is not set 两种写法）
for s in $(grep -oE '^CONFIG_[A-Z0-9_]+' "$FRAG" | sort -u); do
  sed -i "/^${s}=/d" "$CFG"
  sed -i "/^# ${s} is not set/d" "$CFG"
done

# 2) 追加片段里的 CONFIG_ 行
{
  echo ""
  echo "# ==== driver-completion (added $(date +%Y-%m-%d)) ===="
  grep -E '^CONFIG_' "$FRAG"
} >> "$CFG"

echo "--- merged, now in config: ---"
grep -E 'AW_CE_SOCKET|AW_CE_IOCTL|AW_HWRNG_DRIVER|AW_TRNG|AW_GPADC|AW_LRADC|AW_SPI=|SPI_SPIDEV|AW_RTC' "$CFG"
echo
echo "回退：cp -a $BK $CFG"
