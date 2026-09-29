#!/bin/sh
# s15: (A) NPU demos that ship on the board, (B) ISP param enable-map diff,
# (C) read-only test of the isp01 "register window alias" hypothesis,
# (D) misc leftovers.
s() { printf ' \n' | sudo -S -p '' "$@"; }
T=/home/orangepi/ar0234test

echo "########## A. NPU: the vendor demos that are actually on the board"
echo "--- readme ---"; cat /opt/vpm_run/readme.txt 2>&1 | sed 's/^/  /'
echo "--- sample.txt ---"; cat /opt/vpm_run/sample.txt 2>&1 | sed 's/^/  /'
echo "--- vpm_run libs needed ---"; ldd /opt/vpm_run/vpm_run 2>&1 | sed 's/^/  /'
echo "--- yolov5 libs needed ---"; ldd /opt/yolov5/yolov5 2>&1 | sed 's/^/  /'
echo "--- run vpm_run ---"
cd /opt/vpm_run 2>/dev/null && timeout 60 ./vpm_run 2>&1 | head -40 | sed 's/^/  /'; echo "  rc=$?"
echo "--- run yolov5 ---"
cd /opt/yolov5 2>/dev/null && timeout 90 ./yolov5 2>&1 | head -30 | sed 's/^/  /'; echo "  rc=$?"
cd /
echo "--- npu state after ---"; cat /sys/kernel/debug/viplite/core_loading 2>&1 | sed 's/^/  /'
cat /sys/kernel/debug/viplite/mem_profile 2>&1 | sed 's/^/  /'
grep -iE 'npu|vip' /sys/kernel/debug/pm_genpd/pd_npu/current_state 2>/dev/null | sed 's/^/  /'
dmesg | tail -6 | sed 's/^/  /'

echo "########## B. ISP parameter enable map"
echo "--- 3dnr vs no3dnr variant diff (both on the board) ---"
cmp -l /mnt/extsd/ar0234/isp_param_3dnr.bin /mnt/extsd/ar0234/isp_param_no3dnr.bin 2>&1 | sed 's/^/  byte-diff: /'
echo "--- deployed file vs both variants ---"
for v in 3dnr no3dnr; do
	printf '  vs %-8s : %s\n' "$v" "$(cmp -s /mnt/extsd/isp_param_config.bin /mnt/extsd/ar0234/isp_param_$v.bin && echo IDENTICAL || echo differs)"
done
echo "--- enable bytes 88..120 of each variant ---"
for f in /mnt/extsd/ar0234/isp_param_3dnr.bin /mnt/extsd/ar0234/isp_param_no3dnr.bin; do
	printf '  %-45s %s\n' "$(basename $f)" "$(od -A n -j 88 -N 32 -t x1 $f | tr -s ' ')"
done
echo "  index map: 0 manual 1 afs 2 ae 3 af 4 awb 5 hist 6 wdr_split 7 wdr_stitch 8 otf_dpc 9 ctc 10 gca 11 nrp 12 denoise 13 tdf 14 blc 15 wb 16 dig_gain 17 lsc 18 msc 19 pltm 20 cfa 21 lca 22 sharp 23 ccm 24 defog 25 cnr 26 drc 27 gtm 28 gamma 29 cem 30 encpp 31 enc_3dnr"

echo "########## C. isp01 register-alias hypothesis, read-only"
s $T/vfr -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 10 >/tmp/s15.v 2>&1 &
P=$!
sleep 3
for a in 0x5900000 0x58ffffc 0x5901300 0x5901304 0x58ffff8 0x58ffff4 0x5901860 0x5901840; do
	printf '  read %-10s = %s\n' "$a" "$(s $T/tools/vinreg r $a 1 2>&1 | tail -1)"
done
echo "  --- 8 words starting at each isp00/isp01 base ---"
printf '  0x5900000: %s\n' "$(s $T/tools/vinreg s 0x5900000 32 2>&1 | tr '\n' ' ')"
printf '  0x58ffffc: %s\n' "$(s $T/tools/vinreg s 0x58ffffc 32 2>&1 | tr '\n' ' ')"
wait $P
grep RESULT /tmp/s15.v | sed 's/^/  /'

echo "########## D. leftovers"
echo "--- module search path: which .ko wins ---"
modinfo -n vin_v4l2 2>&1 | sed 's/^/  /'; modinfo -n ar0234_mipi 2>&1 | sed 's/^/  /'
echo "--- panic/watchdog settings ---"
printf '  panic_on_oops=%s\n' "$(cat /proc/sys/kernel/panic_on_oops)"
grep -i RuntimeWatchdogSec /etc/systemd/system.conf | sed 's/^/  /'
echo "--- dtb backups present ---"
ls -l /boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb* 2>&1 | sed 's/^/  /'
echo "--- vin_warn/vin_err knobs ---"
ls /sys/module/vin_v4l2/parameters/ | sed 's/^/  /'
echo "########## DONE15"
