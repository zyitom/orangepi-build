#!/bin/sh
# Full hardware inventory + osnoise errno diagnostic (run as root on board).
echo ===OSNOISE-ERRNO===
python3 - <<'PYEOF'
import errno
try:
    f = open('/sys/kernel/tracing/current_tracer','w')
    f.write('osnoise'); f.close()
    print('osnoise enabled OK')
except OSError as e:
    print(f'errno={e.errno} {errno.errorcode.get(e.errno)}: {e.strerror}')
PYEOF
dmesg | grep -iE 'osnoise|timerlat' | tail -5
echo ===NPU===
ls /dev/ | grep -iE 'vip|npu'
ls /usr/lib/ | grep -iE 'VIPlite|VIPhal|NBGlink' | head -5
echo ===G2D===
grep g2d /proc/modules; ls /dev/g2d 2>&1
echo ===BT===
rfkill list 2>/dev/null | head -6; ls /sys/class/bluetooth/ 2>&1
echo ===USB===
ls /sys/bus/usb/devices/ | head -8
echo ===IR===
ls /sys/bus/platform/devices/ | grep -iE 'ir|lradc' ; ls /sys/class/rc/ 2>/dev/null
echo ===PWM===
ls /sys/class/pwm/ 2>&1
echo ===LEDS===
ls /sys/class/leds/ 2>&1
echo ===STORAGE===
cat /proc/partitions
echo ===SPI-UART===
ls /dev/spidev* 2>&1; ls /dev/ttyS* 2>/dev/null | head -6
echo ===AUDIO===
cat /proc/asound/cards; aplay -l 2>/dev/null | head -6
echo ===I2C===
ls /dev/i2c-* | tr '\n' ' '; echo
echo ===VIDEO-CAM===
ls /dev/video* 2>&1 | head -2
echo ===GPIO-CHIP===
grep -c . /sys/class/gpio/export 2>/dev/null; ls /dev/gpiochip* 2>&1
echo ===FREQS===
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq /sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq 2>/dev/null
cat /sys/class/devfreq/1800000.gpu/cur_freq /sys/class/devfreq/3600000.npu/cur_freq 2>/dev/null
