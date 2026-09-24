#!/bin/sh
# 板上运行：40-pin 启用后的验收（只读；i2cdetect 需 sudo）
S() { sudo "$@"; }
echo "=== applied overlays (dmesg) ==="
S dmesg | grep -i 'Applying kernel provided DT overlay' | tail
echo "=== i2c adapters ==="
ls /dev/i2c-* 2>/dev/null
for b in 0 1 2 3 5 7 8 9; do
	echo "--- i2c-$b ---"
	S i2cdetect -y "$b" 2>/dev/null | sed -n '2,9p'
done
echo "=== spidev ==="
ls -l /dev/spidev* 2>&1
echo "=== pwm ==="
for c in 0 10 20; do echo "pwmchip$c npwm=$(cat /sys/class/pwm/pwmchip$c/npwm 2>/dev/null)"; done
echo "=== uart ==="
ls /dev/ttyS* 2>/dev/null
echo "=== gpio chips ==="
ls -l /dev/gpiochip* 2>/dev/null
which gpioinfo >/dev/null 2>&1 && S gpioinfo 2>/dev/null | head -20 || echo "(gpiod not installed: apt install gpiod)"
echo "=== new errors ==="
S dmesg | grep -iE 'error|fail' | tail -20
