#!/bin/sh
# runs on the board as root
echo "=== kprobe messages in dmesg ==="
dmesg | grep -iE "ispclk|kprobe" | tail -20
echo
echo "=== symbols in kallsyms ==="
grep -E "__vin_set_isp_clk_rate|__vin_set_top_clk_rate" /proc/kallsyms
echo "(none above means the symbols are not in kallsyms)"
echo
echo "=== kprobe support ==="
if [ -f /proc/config.gz ]; then zcat /proc/config.gz | grep -E "KPROBE|KALLSYMS"; else
	grep -E "KPROBE|KALLSYMS" /boot/config-6.6.98-sun60iw2 2>/dev/null || echo "no config available"
fi
echo
echo "=== tracefs kprobe_events available? ==="
ls /sys/kernel/debug/tracing/kprobe_events 2>&1
