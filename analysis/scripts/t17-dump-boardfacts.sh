#!/bin/sh
# dump-boardfacts.sh : pushed to the board and run as root by broot.sh
echo "=== clk: isp ==="
for f in clk_rate clk_parent clk_possible_parents clk_flags clk_prepare_count clk_enable_count; do
	printf '%-22s: %s\n' "$f" "$(cat /sys/kernel/debug/clk/isp/$f 2>&1 | tr '\n' ' ')"
done
echo
echo "=== clk_summary: isp + video plls ==="
grep -Ei "isp|pll-video|mipi|_csi|tdm|vipp|_tclk" /sys/kernel/debug/clk/clk_summary 2>/dev/null
echo
echo "=== /sys/kernel/debug/mpp/vi ==="
cat /sys/kernel/debug/mpp/vi
