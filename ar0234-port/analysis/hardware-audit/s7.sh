#!/bin/sh
# s7: isolate whether the T16b regression is caused by (a) open+close of the
# second node, or (b) simply two back-to-back streams. Control series.
V=/home/orangepi/ar0234test/vfr
OC=/tmp/openclose
vicf() { awk '/^vi[0-9]+:/{n=$1} n=="vi0:"&&/frame =>/{print "vi0 "$0} n=="vi4:"&&/frame =>/{print "vi4 "$0}' /sys/kernel/debug/mpp/vi; }
one() { # label seconds touch|none
	L=$1; S=$2; T=$3
	M="S7-$L-$$"
	echo $M > /dev/kmsg
	echo "----- $L : ${S}s touch=$T"
	echo "  vi before: $(vicf | tr '\n' ' ')"
	if [ "$T" = "touch" ]; then
		$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t $S >/tmp/s7.$L 2>&1 &
		P=$!; sleep 8
		echo "  vi @8s: $(vicf | tr '\n' ' ')"
		$OC /dev/video4 1 | sed 's/^/    /'
		sleep 1; echo "  vi right after touch: $(vicf | tr '\n' ' ')"
		sleep 8; echo "  vi 8s later:          $(vicf | tr '\n' ' ')"
		wait $P
	else
		$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t $S >/tmp/s7.$L 2>&1
	fi
	grep -E 'RESULT' /tmp/s7.$L | sed 's/^/  /'
	echo "  dmesg:"; dmesg | awk -v m="$M" '$0 ~ m {f=1} f' | grep -viE '\[ar0234_mipi\]' | grep -v "^\[.*\] $M" | head -6 | sed 's/^/   ! /'
	sleep 4
}

one A1 20 none
one A2 20 none
one A3 20 none
one B1 20 touch
one B2 20 touch
one C1 20 none
one D1 20 touch
echo "=========== 3A service journal (last 20) ==========="
journalctl -u ar0234-3ad -n 20 --no-pager 2>&1 | sed 's/^/  /'
echo "########## DONE7"
