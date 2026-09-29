#!/bin/sh
# E902/SCP related feature check + noble card fixes, run as root on the board:
#   tools/ssh_board.sh "cat > /tmp/e902t.sh" < e902/tests/board/e902-noble-check.sh
#   tools/ssh_board.sh -s "sh /tmp/e902t.sh"
# Sections marked FIX change card state; the script ends with a reboot.
echo ===AMPTS===
python3 - <<'PYEOF'
import time
def rd(p):
    with open(p) as f: return f.read().strip()
base='/sys/devices/platform/soc@3000000/8010000.amp-timestamp/'
print("freqid:", rd(base+'freqid'))
c0,u0=int(rd(base+'counter')), int(rd(base+'usec'))
mr0=time.clock_gettime(time.CLOCK_MONOTONIC_RAW)
time.sleep(10)
c1,u1=int(rd(base+'counter')), int(rd(base+'usec'))
mr1=time.clock_gettime(time.CLOCK_MONOTONIC_RAW)
print(f"counter delta={c1-c0} over wall={mr1-mr0:.4f}s -> {((c1-c0)/(mr1-mr0))/1e6:.6f} MHz")
print(f"usec delta={u1-u0} vs wall={int((mr1-mr0)*1e6)} -> drift {u1-u0-int((mr1-mr0)*1e6)} us per 10s")
print(f"usec-vs-counter/24: {u1-u0} vs {(c1-c0)//24}")
PYEOF
echo ===VULKAN-WSI===
timeout 30 vulkaninfo 2>/dev/null | grep -oE 'VK_KHR_(display|swapchain|wayland_surface|xcb_surface|external_[a-z]+)' | sort -u | head
echo ===FIX-DNSMASQ===
systemctl disable --now dnsmasq 2>&1 | tail -1
systemctl --failed --no-legend
echo ===FIX-PAM===
cp -n /etc/pam.d/login /etc/pam.d/login.bak-lastlog
sed -i '/pam_lastlog/d' /etc/pam.d/login && echo pam_lastlog removed
echo ===HWRNG===
cat /sys/class/misc/hw_random/rng_current /sys/class/misc/hw_random/rng_available
echo sunxi_trng > /sys/class/misc/hw_random/rng_current 2>/dev/null; cat /sys/class/misc/hw_random/rng_current
timeout 60 dd if=/dev/hwrng of=/dev/null bs=4096 count=64 2>&1 | tail -1
timeout 10 dd if=/dev/hwrng bs=64 count=1 2>/dev/null | od -A x -t x1 | head -3
echo ===IIO===
for d in /sys/bus/iio/devices/iio:device*; do echo "$d: $(cat $d/name)"; done
cat /sys/bus/iio/devices/iio:device0/in_voltage_raw 2>/dev/null
echo ===IRQ-RATE===
grep -E 'gpadc|msgbox|lradc' /proc/interrupts
sleep 5
grep -E 'gpadc|msgbox|lradc' /proc/interrupts
echo ===CPUFREQ===
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver /sys/devices/system/cpu/cpu4/cpufreq/scaling_driver 2>/dev/null
ls /sys/class/devfreq/ 2>/dev/null
echo ===TOOLS-RTLA===
apt-get -y -qq install linux-tools-common >/dev/null 2>&1
dpkg -L linux-tools-common 2>/dev/null | grep -i rtla
ls /usr/lib/linux-tools* 2>/dev/null
echo ===DMESG-E902===
dmesg | grep -iE 'msgbox|rpmsg|trng|lradc|gpadc|amp|timestamp' | grep -viE 'example' | head -15
echo ===RTARGS===
cp -n /boot/orangepiEnv.txt /boot/orangepiEnv.txt.bak-noble-rt
grep -q '^extraargs=' /boot/orangepiEnv.txt || printf '\n# RT test config: isolate A55 cpu5 (A76 unusable for RT, see latency tests)\nextraargs=isolcpus=5 nohz_full=5 rcu_nocbs=5\n' >> /boot/orangepiEnv.txt
grep -A1 'RT test config' /boot/orangepiEnv.txt
cat > /root/postboot.sh <<'PBEOF'
#!/bin/sh
echo ===CMDLINE===
cat /proc/cmdline
echo ===ISOLATED===
cat /sys/devices/system/cpu/isolated
cat /sys/devices/system/cpu/nohz_full
echo ===FAILED===
systemctl --failed --no-legend
echo ===RTLA===
which rtla && rtla --version 2>&1 | head -1
echo ===PAM===
grep -c pam_lastlog /etc/pam.d/login
echo ===CYC-5===
cyclictest -m -p95 -i1000 -d0 -t1 -a5 -D 10s 2>&1 | tail -4
echo ===LOAD===
cat /proc/loadavg
PBEOF
chmod +x /root/postboot.sh
echo ===REBOOTING===
sync
systemctl reboot
