#!/bin/sh
# Runs on the board as root.  Streams once with the probe module loaded so the
# kprobes catch what __vin_set_isp_clk_rate / __vin_set_top_clk_rate ask for.
VFR=/home/orangepi/ar0234test/vfr
MARK="KPROBE-$(date +%s)"
echo "$MARK" > /dev/kmsg
sleep 1
$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 8 2>&1 | grep RESULT
sleep 1
echo "=== dmesg since $MARK ==="
dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -E "ispclk" | head -30
echo "=== final clocks ==="
echo "  isp=$(cat /sys/kernel/debug/clk/isp/clk_rate) csi=$(cat /sys/kernel/debug/clk/csi/clk_rate)"
