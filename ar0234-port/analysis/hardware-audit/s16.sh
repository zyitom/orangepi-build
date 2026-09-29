#!/bin/sh
s() { printf ' \n' | sudo -S -p '' "$@"; }
echo "########## A. the kernel trace seen around uptime 472 s"
dmesg | awk '/Internal error|Unable to handle|BUG:|WARNING:|Call trace|end trace|pc :|lr :/{print} /vin_pin_disable|underflow/{print "  *** "$0}' | tail -40 | sed 's/^/  /'
echo "--- raw window (lines with el0t_64_sync) ---"
dmesg | grep -n 'el0t_64_sync' | sed 's/^/  /'
dmesg | awk 'NR>=1{print NR": "$0}' | sed -n "$(dmesg | grep -n 'el0t_64_sync' | head -1 | cut -d: -f1 | awk '{print $1-40}'),+45p" | sed 's/^/  /'
echo "--- counts since boot ---"
for p in 'Unable to handle' 'Internal error' 'Call trace' 'end trace' 'WARNING:' 'vin_pin_disable' 'usage count underflow' 'is not mapped' 'frame lost'; do
	printf '  %-24s %s\n' "$p" "$(dmesg | grep -c "$p")"
done

echo "########## B. ISP param: locate the real tdf/enable bytes"
for f in /mnt/extsd/ar0234/isp_param_3dnr.bin /mnt/extsd/ar0234/isp_param_no3dnr.bin; do
	echo "  == $(basename $f)"
	printf '   0-based 88..127 : %s\n' "$(od -A n -j 88 -N 40 -t x1 $f | tr -s ' ')"
	printf '   0-based 160..199: %s\n' "$(od -A n -j 160 -N 40 -t x1 $f | tr -s ' ')"
done
echo "  --- byte 175 (0-based 175) in both ---"
for f in /mnt/extsd/ar0234/isp_param_3dnr.bin /mnt/extsd/ar0234/isp_param_no3dnr.bin; do
	printf '   %-28s [175]=%s [101]=%s\n' "$(basename $f)" "$(od -A n -j 175 -N 1 -t u1 $f | tr -d ' ')" "$(od -A n -j 101 -N 1 -t u1 $f | tr -d ' ')"
done

echo "########## C. NPU: actually run the shipped framework"
cd /opt/vpm_run
echo "--- vpm_run -s sample.txt -l 1 ---"
timeout 120 ./vpm_run -s sample.txt -l 1 2>&1 | head -30 | sed 's/^/  /'
echo "  rc=$?"
echo "--- npu counters after ---"; cat /sys/kernel/debug/viplite/mem_profile | sed 's/^/  /'
cat /sys/kernel/debug/viplite/core_loading | sed 's/^/  /'
echo "--- dmesg ---"; dmesg | tail -8 | sed 's/^/  /'
cd /

echo "########## D. yolov5 demo with its own model"
cd /opt/yolov5
echo "--- ./yolov5 model/yolov5.nb input_data/dog_640_640.jpg ---"
timeout 180 ./yolov5 model/yolov5.nb input_data/dog_640_640.jpg 2>&1 | tail -25 | sed 's/^/  /'
echo "  rc=$?"
ls -l result*.png 2>/dev/null | sed 's/^/  /'
cd /

echo "########## E. vinreg (compile + the isp01 alias read) ---"
gcc -O2 -w -o /tmp/vinreg /tmp/vinreg.c 2>&1 | sed 's/^/  /'
/home/orangepi/ar0234test/vfr -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 12 >/tmp/s16.v 2>&1 &
P=$!; sleep 3
for a in 0x5900000 0x58ffffc 0x5901300 0x58ffff8 0x5901860 0x5901840; do
	printf '  %-10s = %s\n' "$a" "$(s /tmp/vinreg r $a 1 2>&1 | tail -1)"
done
echo "  --- words at isp00 base and isp01 base (8 words each) ---"
s /tmp/vinreg s 0x5900000 32 2>&1 | sed 's/^/    /'
s /tmp/vinreg s 0x58ffffc 32 2>&1 | sed 's/^/    /'
wait $P; grep RESULT /tmp/s16.v | sed 's/^/  /'
echo "########## DONE16"
