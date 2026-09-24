#!/bin/sh
# eprobe.sh - who degrades video0 when a second video node is touched?
#   E1: with the 3A service RUNNING,  S_FMT 960x600 on video4 (now refused)
#   E2: with the 3A service STOPPED,  same
#   E3: with the 3A service RUNNING,  same-size S_FMT on video4 (allowed)
VFR=/home/orangepi/ar0234test/vfr
exec >/home/orangepi/eprobe.out 2>&1
MARK="EPROBE-$(date +%s)"; echo "$MARK" > /dev/kmsg
K() { dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f'; }
vi() { awk -v n="$1" '$0 ~ "^vi" n ":" {f=1} f {print} f && /^\*\*\*/ {exit}' /sys/kernel/debug/mpp/vi | grep -E 'frame =>|prs_in'; }
three_d() { systemctl "$1" ar0234-3ad; sleep 3; systemctl is-active ar0234-3ad; }
run() {				# $1 = label, $2 = "yes|no" touch video4, $3 = size
	$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 30 > /tmp/ep0.log 2>&1 &
	P=$!
	sleep 5
	if [ "$2" = yes ]; then
		v4l2-ctl -d /dev/video4 --set-fmt-video=width=$3,height=$4,pixelformat=NV12 >/dev/null 2>&1
		echo "  [$1] video4 S_FMT $3x$4 rc=$?"
	fi
	sleep 4
	echo "  [$1] vi0 mid: $(vi 0 | tr '\n' '|')"
	wait $P
	echo "  [$1] $(grep RESULT /tmp/ep0.log)"
	echo "  [$1] dmesg: $(K | grep -cE 'sensor_read error|sensor_write error|underflow|refusing|streaming 1920')  err-lines"
}
echo "### $MARK 3A=$(systemctl is-active ar0234-3ad)"
echo "=== E1: 3A running, refused 960x600 S_FMT on video4 ==="; run E1 yes 960 600
echo "=== E2: 3A stopped, same ==="; three_d stop; run E2 yes 960 600
echo "=== E3: 3A stopped, allowed same-size S_FMT on video4 ==="; run E3 yes 1920 1200
echo "=== E4: 3A stopped, no touch at all ==="; run E4 no 0 0
echo "=== E5: 3A running again, no touch ==="; three_d start; run E5 no 0 0
echo "### done 3A=$(systemctl is-active ar0234-3ad)"
echo "--- dmesg since MARK ---"; K | grep -Ev '\[ar0234_mipi\]' | tail -25
