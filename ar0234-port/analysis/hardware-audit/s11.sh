#!/bin/sh
# s11: discriminating arms for the "second node open/close" issue.
#   arm1: 12 s stream, NO gap before next run, touch video4 at t=5 s
#   arm2: 12 s stream, 5 s gap before next run, touch video4 at t=5 s
#   arm3: 12 s stream, NO gap, NO touch (control)
V=/home/orangepi/ar0234test/vfr
OC=/tmp/openclose
vi() { awk '/^vi0:/{f=1} f&&/frame =>/{gsub(/,/,"",$0); print $4; exit}' /sys/kernel/debug/mpp/vi; }

arm() { # armname n gap touch
	A=$1; N=$2; G=$3; T=$4
	echo "########## $A (n=$N gap=$G touch=$T)"
	i=1
	while [ $i -le $N ]; do
		LOG=/tmp/s11.$A.$i
		M="S11-$A-$i-$$"; echo $M > /dev/kmsg
		$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 12 >$LOG 2>&1 &
		P=$!
		sleep 5; C1=$(vi)
		if [ "$T" = touch ]; then $OC /dev/video4 1 >/dev/null 2>&1; TAG="touched"; else TAG="untouched"; fi
		sleep 5; C2=$(vi)
		wait $P; RC=$?
		D=$((C2-C1))
		R=$(grep -o 'frames=[0-9]* wall=[0-9.]*s fps=[0-9.]* timeouts=[0-9]*' $LOG | head -1)
		[ -z "$R" ] && R="NO-RESULT(rc=$RC) $(tail -1 $LOG | cut -c1-60)"
		if [ "$D" -lt 300 ]; then Vv="FROZEN"; else Vv="ok"; fi
		printf '  %s run%-2s %-7s vi0 %s -> %s (d=%s) | %s\n' "$A" "$i" "$Vv" "$C1" "$C2" "$D" "$R"
		printf '      frame_lost=%s sensor_not_used=%s underflow=%s\n' \
			"$(dmesg | awk -v m="$M" '$0 ~ m {f=1} f' | grep -c 'frame lost')" \
			"$(dmesg | awk -v m="$M" '$0 ~ m {f=1} f' | grep -c 'sensor is not used')" \
			"$(dmesg | awk -v m="$M" '$0 ~ m {f=1} f' | grep -c 'underflow')"
		i=$((i+1))
		[ "$G" -gt 0 ] && sleep $G
	done
}

arm arm1 4 0 touch
arm arm2 4 5 touch
arm arm3 4 0 none
echo "########## recovery check (3 s gap, untouched)"
sleep 3
$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 8 2>&1 | grep RESULT | sed 's/^/  /'
echo "########## DONE11"
