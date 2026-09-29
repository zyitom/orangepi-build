#!/bin/sh
# Controlled failure + recovery experiment.
# Tests (a) the documented 1080p133 limit, (b) whether an ISP error survives a
# plain stream restart or needs a reboot, (c) rmmod viability.
# One variable at a time; nothing is written to the DT or the clock tree.
V=/home/orangepi/ar0234test/vfr
vic() { sed -n "/^$1:/,/^\*\*\*/p" /sys/kernel/debug/mpp/vi | grep -E 'frame =>|prs_in|output =>' | tr -s ' ' | tr '\n' '|'; }
dm() { dmesg | awk -v m="$1" '$0 ~ m {f=1} f'; }
run() { # label dev fmt w h fps secs
	MARK="S4-$1-$$"
	echo "------------------------------------------------------------------"
	echo "RUN $1 : $2 $3 ${4}x${5} fps=$6 t=$7"
	echo "  BEFORE $(echo ${2#/dev/video} | sed 's/^/vi/'): $(vic $(echo ${2#/dev/video} | sed 's/^/vi/'))"
	echo $MARK > /dev/kmsg
	$V -d $2 -w $4 -h $5 -f $3 -t $7 -p 1/$6 >/tmp/s4.$$ 2>&1
	echo "  vfr rc=$?"
	grep -E 'RESULT|GAPS|Invalid|failed|requested|no frame|fll' /tmp/s4.$$ | head -5 | sed 's/^/  | /'
	echo "  AFTER  $(echo ${2#/dev/video} | sed 's/^/vi/'): $(vic $(echo ${2#/dev/video} | sed 's/^/vi/'))"
	E=$(dm "$MARK")
	printf '  dmesg: frame_lost=%s hblank=%s reset=%s not_mapped=%s oops=%s warn=%s | lines=%s\n' \
		"$(echo "$E" | grep -c 'frame lost')" "$(echo "$E" | grep -ci 'hblank short')" \
		"$(echo "$E" | grep -ci 'sunxi_isp_reset\|isp reset')" "$(echo "$E" | grep -c 'is not mapped')" \
		"$(echo "$E" | grep -ci 'oops\|panic')" "$(echo "$E" | grep -ci 'WARNING:')" "$(echo "$E" | grep -c .)"
	echo "$E" | grep -Ei 'err|warn|lost|reset|short' | head -6 | sed 's/^/  ! /'
}

echo "########## PHASE 0: health check"
run health0 /dev/video0 NV12 1920 1200 120 5

echo "########## PHASE 1: smallest advertised size"
run minsize /dev/video0 BGR3 192 128 120 5
run weird-1000x150 /dev/video0 NV12 1000 150 120 5

echo "########## PHASE 2: the documented 1080p limit (132 ok / 133 broken)"
run 1080p132 /dev/video0 NV12 1920 1080 132 5
run 1080p133 /dev/video0 NV12 1920 1080 133 8
run 1080p136 /dev/video0 NV12 1920 1080 136 6

echo "########## PHASE 3: can a broken ISP be recovered WITHOUT a reboot?"
run recover-1200p120 /dev/video0 NV12 1920 1200 120 8
run recover-1200p30  /dev/video0 NV12 1920 1200 30  5

echo "########## PHASE 4: rmmod viability"
printf ' \n' | sudo -S -p '' rmmod vin_v4l2 2>&1 | sed 's/^/  rmmod vin_v4l2: /'
printf ' \n' | sudo -S -p '' rmmod ar0234_mipi 2>&1 | sed 's/^/  rmmod ar0234_mipi: /'
lsmod | grep -E 'vin_v4l2|vin_io|ar0234' | sed 's/^/  /'

echo "########## PHASE 5: state after the whole experiment"
cat /sys/kernel/debug/mpp/vi
echo "--- isp clk"; cat /sys/kernel/debug/clk/isp/clk_rate
echo "--- dmesg tail"; dmesg | tail -20
rm -f /tmp/s4.$$
echo "########## DONE4"
