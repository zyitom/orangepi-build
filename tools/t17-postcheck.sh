#!/bin/sh
# runs on the board as root: post-experiment clock + health check
echo "=== clk_rate isp ==="
cat /sys/kernel/debug/clk/isp/clk_rate
echo "=== clk_summary (isp / pll-video0 subtree) ==="
awk '/pll-video0 /{f=1} f{print} /isp-mclk/{if(f)exit}' /sys/kernel/debug/clk/clk_summary | head -30
echo "=== upstream rate of the isp chain ==="
grep -E "^ +(isp|pll-video0-4x|pll-video0-3x|pll-video0|csi) " /sys/kernel/debug/clk/clk_summary
echo "=== uptime / load ==="
uptime
echo "=== dmesg tail ==="
dmesg | tail -22
echo "=== vi counters ==="
grep -E '^vi[0-9]+:|frame =>' /sys/kernel/debug/mpp/vi
