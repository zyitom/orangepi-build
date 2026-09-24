#!/bin/sh
# 在【板子】上以 root 运行：对板载 DTB 做“可回退”的属性修改。
# 只做纯 status 翻转（fdtput 足够），不新增节点。
# 重新编译内核不是必须的：本脚本只动当前生效的 DTB；要真正启用驱动仍需内核开对应 CONFIG。
set -e
DTB="/boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb"
[ -f "$DTB" ] || { echo "DTB not found: $DTB"; exit 1; }
BK="$DTB.bak-$(date +%Y%m%d-%H%M%S)"
cp -a "$DTB" "$BK"
echo "backup -> $BK"
P="/soc@3000000"

# 1) 启用 SoC 内部 RTC（配合 CONFIG_AW_RTC）
fdtput -t s "$DTB" "$P/rtc@7090000" status okay && echo "rtc@7090000 -> okay"

# 2) 关闭板上未装配的“幽灵”外设节点（现在只会每 boot 刷 dmesg 报错）
#    已验证：i2c-15(hym8563) 与 i2c-12(gt9271) 总线扫描无器件应答。
for n in "twi@251C000/touchscreen@14" "twi@7085000/rtc@51" "twi@7085000/ac101@1a"; do
  if fdtput -t s "$DTB" "$P/$n" status disabled 2>/dev/null; then
    echo "$n -> disabled"
  else
    echo "$n (skip: node not found)"
  fi
done

echo
echo "生效需要重启：printf ' \\n' | sudo -S -p '' reboot"
echo "回退：cp -a $BK $DTB && reboot"
