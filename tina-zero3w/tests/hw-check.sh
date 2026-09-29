#!/bin/sh
# hw-check.sh — on-board check of the 2026-09-29 hardware fixes (run as root).
# Prints one PASS/FAIL line per item plus the evidence; exit status = number of FAILs.
F=0
ok()   { echo "[PASS] $*"; }
bad()  { echo "[FAIL] $*"; F=$((F+1)); }
irqs() { grep -E "$1" /proc/interrupts | awk '{s=0; for (i=2; i<=9; i++) s+=$i; t+=s} END {print t+0}'; }
rate() { a=$(irqs "$1"); sleep 5; b=$(irqs "$1"); echo $(( (b-a)/5 )); }

echo "== kernel $(uname -r) $(uname -v | cut -c1-40)"

# 1. GPADC: no free-running data IRQs; IIO read still works
r=$(rate 'sunxi-gpadc')
[ "$r" -lt 5 ] && ok "gpadc idle IRQ rate $r/s" || bad "gpadc idle IRQ rate $r/s (was ~800)"
v=$(cat /sys/bus/iio/devices/iio:device0/in_voltage0_raw 2>/dev/null)
[ -n "$v" ] && ok "gpadc in_voltage0_raw = $v (on-demand read)" || bad "gpadc IIO read failed"

# 2. DVFS on the RT cluster, PMIC I2C traffic
g=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor)
[ "$g" = performance ] && ok "policy0 (A55/RT) governor $g" || bad "policy0 governor $g"
r=$(rate '7083000.twi')
[ "$r" -lt 200 ] && ok "PMIC I2C IRQ rate $r/s" || bad "PMIC I2C IRQ rate $r/s (was ~600)"

# 3. phantom devices gone (0016)
n=$(dmesg | grep -cE 'probe of 13-0034|probe of 15-0051|probe of 12-0014|9th SCL')
[ "$n" = 0 ] && ok "no phantom PMU/RTC/touch probes" || bad "$n phantom-device probe errors"

# 4. USB OTG manager (0024/0025)
U=/sys/devices/platform/soc@3000000/10.usbc0
dmesg | grep -q 'otg manager: probe of 10.usbc0 failed' && bad "otg manager probe failed" || ok "otg manager probed"
role=$(cat $U/otg_role 2>/dev/null)
[ "$role" = usb_device ] && ok "USB0 role at boot: $role" || bad "USB0 role at boot: $role"
w=$(dmesg | grep -c 'invalid sysfs_emit')
echo usb_host > $U/otg_role; sleep 2; echo usb_device > $U/otg_role; sleep 2
w2=$(dmesg | grep -c 'invalid sysfs_emit')
[ "$w2" = "$w" ] && ok "otg_role switch host->device without WARN (now $(cat $U/otg_role))" || bad "otg_role switch still WARNs"

# 5. RTC (0029)
if [ -e /dev/rtc0 ]; then
	ok "rtc0 = $(cat /sys/class/rtc/rtc0/name) $(cat /sys/class/rtc/rtc0/date) $(cat /sys/class/rtc/rtc0/time)"
else
	bad "no /dev/rtc0"
fi

# 6. G2D autoload (0028)
[ -e /dev/g2d ] && ok "/dev/g2d present at boot" || bad "/dev/g2d missing"

# 7. WiFi power save off
ps=$(iw dev wlan0 get power_save 2>/dev/null)
[ "$ps" = "Power save: off" ] && ok "wlan0 $ps" || bad "wlan0 $ps"

# 8. Bluetooth
systemctl is-active -q zero3w-bluetooth && ok "zero3w-bluetooth.service active" || bad "zero3w-bluetooth.service $(systemctl is-active zero3w-bluetooth)"
a=$(bluetoothctl show 2>/dev/null | awk '/^Controller/ {print tolower($2); exit}')   # hci sysfs has no address file
exp=$(cat /sys/class/addr_mgt/addr_bt 2>/dev/null | tr 'A-F' 'a-f')
[ -n "$a" ] && [ "$a" = "$exp" ] && ok "hci0 address $a (= addr_mgt)" || bad "hci0 address '$a' (want '$exp')"
if command -v bluetoothctl >/dev/null; then
	bluetoothctl power on >/dev/null 2>&1
	n=$(bluetoothctl --timeout 12 scan on 2>/dev/null | grep -c 'NEW.*Device')   # no timeout(1) in busybox
	[ "$n" -gt 0 ] && ok "bluetoothctl scan found $n devices" || echo "[INFO] bluetoothctl scan found no devices (none nearby?)"
fi

# 9. rootfs expanded on first boot
rp=$(awk '$5 == "/" {print $3; exit}' /proc/self/mountinfo)
sz=$(($(cat /sys/dev/block/$rp/size) / 2 / 1024 / 1024))
[ "$sz" -gt 4 ] && ok "rootfs partition ${sz} GiB (expanded)" || bad "rootfs partition ${sz} GiB (not expanded)"

# 10. NPU
if [ -x /usr/bin/npurun ] && [ -f /opt/vpm_run/network_binary.nb ]; then
	out=$(npurun /opt/vpm_run/network_binary.nb /opt/vpm_run/input_0.dat 3 2>&1 | tail -2 | tr '\n' ' ')
	echo "$out" | grep -qiE 'error|fail' && bad "npurun: $out" || ok "npurun: $out"
else
	bad "NPU userland missing"
fi

# 11. failed units
fu=$(systemctl --failed --no-legend 2>/dev/null | wc -l)
[ "$fu" = 0 ] && ok "no failed systemd units" || bad "$fu failed units: $(systemctl --failed --no-legend | awk '{print $2}' | tr '\n' ' ')"

# 12. suspend path with WiFi up (0026): pm_test "devices" x3 must pass
if [ "${SKIP_SUSPEND:-0}" != 1 ]; then
	pass=0
	for i in 1 2 3; do
		echo devices > /sys/power/pm_test
		echo mem > /sys/power/state 2>/dev/null && pass=$((pass+1))
		sleep 3
	done
	echo none > /sys/power/pm_test
	[ "$pass" = 3 ] && ok "pm_test devices 3/3 with WiFi up" || bad "pm_test devices $pass/3"
fi

echo "== $F failure(s)"
exit $F
