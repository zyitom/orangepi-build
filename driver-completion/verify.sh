#!/bin/sh
# 在【板子】上运行：补全后逐项验收（只读）。
echo "=== RTC ==="
ls -l /dev/rtc* 2>&1
cat /sys/class/rtc/rtc0/name 2>/dev/null || echo "(no rtc0)"
echo "=== Crypto CE (sunxi 算法) ==="
grep -c -i sunxi /proc/crypto 2>/dev/null
echo "=== hwrng ==="
ls -l /dev/hwrng 2>&1
echo "=== GPADC / LRADC ==="
ls -d /sys/bus/platform/devices/*gpadc* /sys/bus/platform/devices/*lradc* 2>/dev/null
for d in /sys/bus/platform/devices/*gpadc* /sys/bus/platform/devices/*lradc*; do
  echo "$(basename $d) -> driver: $(basename $(readlink -f $d/driver 2>/dev/null) 2>/dev/null || echo NONE)"
done
echo "=== SPI / spidev ==="
ls -l /dev/spidev* 2>&1
echo "=== 幽灵外设是否已安静（应无 hym8563/goodix 报错）==="
printf ' \n' | sudo -S -p '' dmesg | grep -iE 'hym8563|Goodix|ac101' | tail -10
echo "(以上为空 = 幽灵节点已关闭)"
echo "=== 其它错误汇总 ==="
printf ' \n' | sudo -S -p '' dmesg | grep -iE 'error|fail' | tail -15
