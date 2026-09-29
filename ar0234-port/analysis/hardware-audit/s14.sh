#!/bin/sh
# s14: NPU runtime hunt, clock/power/thermal, memory + bandwidth counters,
# ISP parameter state on the board, trigger/flash surface.
s() { printf ' \n' | sudo -S -p '' "$@"; }
T=/home/orangepi/ar0234test
vi() { sed -n "/^$1:/,/^\*\*\*/p" /sys/kernel/debug/mpp/vi; }

echo "########## A. NPU: hunt for the runtime (libVIPlite.so / simulators)"
echo "--- whole-fs search for the VIPLite runtime and friends ---"
find / -xdev -name 'libVIP*.so*' -o -xdev -name 'libNBG*.so*' -o -xdev -name 'libOpenVX*' 2>/dev/null | head -20 | sed 's/^/  /'
echo "--- /opt ---"
ls -lR /opt 2>/dev/null | head -60 | sed 's/^/  /'
echo "--- /opt/vpm_run contents ---"
ls -la /opt/vpm_run 2>/dev/null | sed 's/^/  /'
file /opt/vpm_run/* 2>/dev/null | head -20 | sed 's/^/  /'
echo "--- any vip/npu test binaries ---"
find / -xdev -iname '*vip*' -not -path '/proc/*' 2>/dev/null | grep -vE '^/sys|^/usr/include/vip_lite|networks' | head -20 | sed 's/^/  /'
echo "--- dpkg: who owns /opt/vpm_run? ---"
dpkg -S /opt/vpm_run 2>&1 | head -3 | sed 's/^/  /'
dpkg -l 2>/dev/null | grep -iE 'vip|npu|nna|nn ' | sed 's/^/  /'
echo "--- vip_lite.h version / API surface ---"
grep -m5 -nE 'VIPLITE_VERSION|VERSION_MAJOR|vip_init' /usr/include/vip_lite.h | sed 's/^/  /'

echo "########## B. clocks"
echo "--- camera/encoder clocks now ---"
for c in isp csi csi-master0 ve-enc0 ve-dec g2d npu gpu0; do
	[ -e /sys/kernel/debug/clk/$c/clk_rate ] && printf '  %-14s %s Hz (parent %s)\n' "$c" "$(cat /sys/kernel/debug/clk/$c/clk_rate)" "$(cat /sys/kernel/debug/clk/$c/clk_parent 2>/dev/null)"
done
echo "--- clk_summary: video/isp branch ---"
grep -A2 -E '^\s+(pll-video0|pll-video0-4x|pll-video0-3x|csi|csi-bus|csi-mclk|isp|isp-mclk|ve-enc0|ve-dec|g2d|npu|gpu0)\s' /sys/kernel/debug/clk/clk_summary 2>/dev/null | head -40 | sed 's/^/  /'
echo "--- can the ISP clock even be changed by userspace? ---"
ls -l /sys/kernel/debug/clk/isp/clk_rate | sed 's/^/  /'

echo "########## C. power / runtime PM / thermal"
echo "--- genpd states ---"
for d in /sys/kernel/debug/pm_genpd/pd_*; do printf '  %-16s %s\n' "$(basename $d)" "$(cat $d/current_state 2>/dev/null)"; done
echo "--- runtime PM (idle) ---"
for p in $(find /sys/devices -name runtime_status 2>/dev/null | grep -E 'vinc|vind\.|cedar|g2d|isp|scaler|tdm'); do printf '  %-70s %s\n' "$(dirname $p | sed 's#/sys/devices/##')" "$(cat $p)"; done
echo "--- thermal now ---"
for z in /sys/class/thermal/thermal_zone*; do printf '  %-14s %s C\n' "$(cat $z/type)" "$(awk -v t=$(cat $z/temp) 'BEGIN{printf "%.1f", t/1000}')"; done
echo "--- devfreq (DDR / VE / GPU opp) ---"
for d in /sys/class/devfreq/*; do printf '  %-40s %s\n' "$(basename $d)" "$(cat $d/cur_freq 2>/dev/null)"; done

echo "########## D. memory / DMA-BUF / IOMMU counters DURING a stream"
s $T/vfr -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 12 >/tmp/s14.v 2>&1 &
P=$!
sleep 4
echo "--- mpp/vi (live) ---"; vi vi0 | sed 's/^/  /'
echo "--- CSI/ISP Bandwidth fields ---"; grep -E 'Bandwidth' /sys/kernel/debug/mpp/vi | sed 's/^/  /'
echo "--- dma_buf/bufinfo (live) ---"; head -30 /sys/kernel/debug/dma_buf/bufinfo | sed 's/^/  /'
echo "--- iommu groups (live) ---"; for g in /sys/kernel/iommu_groups/*; do echo "  $g: $(ls $g/devices/ | tr '\n' ' ')"; done
echo "--- mem ---"; free -m | sed 's/^/  /'
echo "--- slab/ion ---"; grep -E 'ion|dma_buf' /proc/slabinfo 2>/dev/null | head -5 | sed 's/^/  /'
wait $P
grep RESULT /tmp/s14.v | sed 's/^/  /'

echo "########## E. ISP parameter state on the board"
echo "--- files ---"
ls -la /mnt/extsd/ /mnt/extsd/ar0234/ 2>&1 | sed 's/^/  /'
echo "--- libisp config lookup name / fps_fixed ---"
grep -o 'isp_param_config[^"]*' /usr/lib/aarch64-linux-gnu/libisp.so 2>/dev/null | head -3 | sed 's/^/  /'
echo "--- enable-map bytes (offset 88..120) of the deployed bin ---"
od -A d -j 88 -N 32 -t x1 /mnt/extsd/isp_param_config.bin 2>&1 | sed 's/^/  /'
echo "--- header ---"
od -A d -N 80 -t x1z /mnt/extsd/isp_param_config.bin 2>&1 | sed 's/^/  /'

echo "########## F. trigger / flash surface (no hardware attached)"
echo "--- driver params ---"
for p in /sys/module/ar0234_mipi/parameters/*; do printf '  %-16s %s\n' "$(basename $p)" "$(cat $p)"; done
echo "--- trigger support visible in the module? ---"
strings /lib/modules/6.6.98-sun60iw2/extra/ar0234_mipi.ko 2>/dev/null | grep -iE 'trigger|flash' | head -8 | sed 's/^/  /'
strings /lib/modules/6.6.98-sun60iw2/updates/ar0234_mipi.ko 2>/dev/null | grep -iE 'trigger|flash' | head -8 | sed 's/^/  /'
echo "--- trigger/flash pins in the DT ---"
D=/boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb
fdtget -p $D /soc/vind@5800800/sensor@5812000 2>/dev/null | tr ' ' '\n' | grep -iE 'trig|flash|pwdn|reset|mclk' | sed 's/^/  /'
for a in sensor0_pwdn sensor0_reset sensor0_mclk_id sensor0_twi_addr; do printf '  %-18s %s\n' "$a" "$(fdtget -t i $D /soc/vind@5800800/sensor@5812000 $a 2>&1)"; done
echo "--- flash control on video0 ---"
v4l2-ctl -d /dev/video0 --list-ctrls 2>/dev/null | grep -iE 'flash|led' | sed 's/^/  /'
echo "--- PWM available ---"
for c in /sys/class/pwm/pwmchip*; do printf '  %-14s npwm=%s\n' "$(basename $c)" "$(cat $c/npwm)"; done
echo "########## DONE14"
