#!/bin/sh
# s5: (a) what happens after the sensor module is reloaded alone (HANDOFF 3.17),
#     (b) the real 1080p frame-rate ceiling with the driver's own fll numbers.
V=/home/orangepi/ar0234test/vfr
SUDO="printf ' \n' | sudo -S -p ''"
echo "########## STATE now: ar0234_mipi unloaded"
lsmod | grep -E 'vin_v4l2|vin_io|ar0234' | sed 's/^/  /'
ls -l /dev/video0 /dev/video4 /dev/v4l-subdev0 2>&1 | sed 's/^/  /'

echo "########## A. reload the sensor module alone, then try to stream (HANDOFF 3.17)"
$SUDO modprobe ar0234_mipi 2>&1 | sed 's/^/  modprobe: /'
sleep 2
lsmod | grep ar0234 | sed 's/^/  /'
v4l2-ctl -d /dev/video0 --list-inputs 2>&1 | sed 's/^/  list-inputs: /'
echo "  --- attempt S_INPUT + short stream ---"
$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -t 4 -p 1/120 2>&1 | grep -E 'RESULT|Invalid|failed|input ' | head -5 | sed 's/^/  /'
echo "  rc=$?"
echo "  --- dmesg tail ---"
dmesg | tail -15 | sed 's/^/  /'
echo "  --- nodes after ---"
ls -l /dev/video0 /dev/video4 2>&1 | sed 's/^/  /'

echo "########## B. fll/vts actually programmed for 1080p at 132/133/134/135"
$SUDO sh -c 'echo 8 > /proc/sys/kernel/printk' 2>/dev/null
for fps in 131 132 133 134 135; do
	M="S5-FLL-$fps-$$"
	echo $M > /dev/kmsg
	echo "  --- request 1080p @ $fps ---"
	$V -d /dev/video0 -w 1920 -h 1080 -f NV12 -t 3 -p 1/$fps 2>&1 | grep -E 'RESULT|requested' | sed 's/^/    /'
	dmesg | awk -v m="$M" '$0 ~ m {f=1} f' | grep -Ei 'fll|frame lost|reset' | head -4 | sed 's/^/    /'
done
echo "########## C. final health (before reboot)"
$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -t 5 -p 1/120 2>&1 | grep RESULT | sed 's/^/  /'
echo "########## DONE5"
