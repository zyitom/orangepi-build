#!/bin/sh
# runs on the board: who is using the camera right now?
echo "=== 3A service ==="
systemctl is-active ar0234-3ad
echo "=== processes holding /dev/video* ==="
fuser -v /dev/video0 /dev/video4 2>&1
echo "=== any vfr / v4l2-ctl / capture processes ==="
ps ax | grep -E 'vfr|v4l2-ctl|ar0234|gst' | grep -v grep
echo "=== module refs ==="
lsmod | grep -E 'vin_v4l2|vin_io|ar0234|ispclk'
echo "=== uptime ==="
cut -d' ' -f1 /proc/uptime
echo "=== isp clk ==="
cat /sys/kernel/debug/clk/isp/clk_rate
echo "=== vi frame counters ==="
grep -E '^vi[0-9]+:|frame =>' /sys/kernel/debug/mpp/vi
