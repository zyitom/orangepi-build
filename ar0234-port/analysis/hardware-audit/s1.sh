#!/bin/sh
# READ-ONLY board baseline audit. Sends nothing, changes nothing.
echo "########## 0. uptime / kernel / cmdline"
uname -a; cut -d' ' -f1 /proc/uptime; cat /proc/cmdline
echo "panic_on_oops=$(cat /proc/sys/kernel/panic_on_oops) watchdog_handle=$(cat /sys/module/sunxi_wdt/parameters/* 2>/dev/null | tr '\n' ' ')"
echo "RuntimeWatchdogSec=$(grep -i '^RuntimeWatchdogSec' /etc/systemd/system.conf)"

echo "########## 1. devices"
ls -l /dev/video* /dev/v4l-subdev* /dev/media* /dev/cedar* /dev/g2d /dev/vipcore /dev/dma_heap/ /dev/watchdog /dev/rfkill 2>&1
ls -l /dev/dri/ 2>&1
echo "--- video nodes by sysfs"
for n in /sys/class/video4linux/*; do echo "$n -> $(cat $n/name 2>/dev/null)"; done

echo "########## 2. v4l2-ctl --list-devices"
v4l2-ctl --list-devices 2>&1

echo "########## 3. media-ctl -p"
media-ctl -p 2>&1

echo "########## 4. video0 --all"
v4l2-ctl -d /dev/video0 --all 2>&1
echo "########## 4b. video0 formats"
v4l2-ctl -d /dev/video0 --list-formats-ext 2>&1
echo "########## 4c. video4 --all"
v4l2-ctl -d /dev/video4 --all 2>&1
echo "########## 4d. video4 formats"
v4l2-ctl -d /dev/video4 --list-formats-ext 2>&1

echo "########## 5. sensor subdev"
v4l2-ctl -d /dev/v4l-subdev0 --list-ctrls 2>&1
echo "--- subdev names"
for n in /dev/v4l-subdev*; do printf '%s %s\n' "$n" "$(v4l2-ctl -d $n -D 2>/dev/null | grep -i 'card type' )"; done

echo "########## 6. debugfs mpp"
ls /sys/kernel/debug/mpp/ 2>&1
echo "--- mpp/vi"
cat /sys/kernel/debug/mpp/vi 2>&1
echo "--- mpp/isp"
cat /sys/kernel/debug/mpp/isp 2>&1
echo "--- mpp/ve"
cat /sys/kernel/debug/mpp/ve 2>&1
echo "--- mpp others"
for f in /sys/kernel/debug/mpp/*; do [ -f "$f" ] || continue; case "$f" in */vi|*/isp|*/ve) continue;; esac; echo "== $f"; cat "$f" 2>&1 | head -30; done

echo "########## 7. clocks (camera/ve/g2d/npu related)"
cat /sys/kernel/debug/clk/clk_summary 2>&1 | grep -Ei 'isp|csi|mipi|tdm|vipp|video|ve|g2d|npu|vip|gpu|pll' | head -80

echo "########## 8. iommu groups"
for g in /sys/kernel/iommu_groups/*; do echo "== $g"; ls -l $g/devices/ 2>/dev/null | tail -n +2 | sed 's#.*/##'; done

echo "########## 9. dma-buf"
cat /sys/kernel/debug/dma_buf/bufinfo 2>&1 | head -40
ls -l /dev/dma_heap/ 2>&1

echo "########## 10. thermal"
for z in /sys/class/thermal/thermal_zone*; do printf '%s %s %s\n' "$z" "$(cat $z/type)" "$(cat $z/temp)"; done
echo "--- cooling devices"; ls /sys/class/thermal/ | head

echo "########## 11. lsmod"
lsmod

echo "########## 12. module params"
for m in ar0234_mipi vin_v4l2 vin_io; do echo "== $m"; for p in /sys/module/$m/parameters/*; do printf '  %s = %s\n' "$(basename $p)" "$(cat $p 2>/dev/null)"; done; done

echo "########## 13. services"
systemctl is-active ar0234-3ad 2>&1; systemctl status ar0234-3ad --no-pager 2>&1 | head -20
echo "--- modules-load.d"; ls /etc/modules-load.d/ 2>&1; cat /etc/modules-load.d/*.conf 2>&1
echo "--- udev rules"; ls /etc/udev/rules.d/ 2>&1; grep -rl 'g2d\|video' /etc/udev/rules.d/ 2>/dev/null
echo "--- isp params"; ls -l /mnt/extsd/ 2>&1; ls -l /etc/ar0234.conf 2>&1

echo "########## 14. PWM / GPIO"
ls -l /sys/class/pwm/ 2>&1; for c in /sys/class/pwm/pwmchip*; do echo "$c npwm=$(cat $c/npwm 2>/dev/null)"; done
ls -l /sys/class/gpio/ 2>&1; ls /dev/gpiochip* 2>&1
echo "--- gpiochip labels"; for g in /sys/class/gpio/gpiochip*; do printf '%s %s\n' "$g" "$(cat $g/label 2>/dev/null)"; done

echo "########## 15. NPU / GPU"
ls -l /sys/kernel/debug/viplite/ 2>&1
for f in /sys/kernel/debug/viplite/*; do [ -f "$f" ] && { echo "== $f"; cat "$f" 2>&1 | head -10; }; done
ls -l /sys/kernel/debug/pvr/ 2>&1
ls -l /usr/lib/libVIPlite.so /usr/lib/libVIPhal.so /usr/lib/libNBGlinker.so /usr/lib/libOpenCL.so* /usr/lib/libvencoder.so /usr/lib/libvdecoder.so /usr/lib/libAWIspApi.so /usr/lib/libisp.so 2>&1
ls -d /usr/include/CL /usr/include/EGL /usr/include/GLES2 /usr/include/vulkan /usr/include/libdrm /usr/include/vip_lite.h 2>&1
ls -l /usr/bin/vdecoderdemo /usr/bin/*demo* 2>&1

echo "########## 16. userspace test binaries"
ls -l /home/orangepi/ar0234test/ 2>&1 | head -40
ls -l /home/orangepi/ar0234test/userspace/apps 2>&1 | head
echo "--- who holds video nodes"; fuser -v /dev/video0 /dev/video4 2>&1
echo "--- stray procs"; ps ax | grep -E 'vfr|v4l2-ctl|ar0234|gst|vdeo|g2d|cltest' | grep -v grep

echo "########## 17. dmesg (full)"
dmesg 2>&1
echo "########## DONE"
