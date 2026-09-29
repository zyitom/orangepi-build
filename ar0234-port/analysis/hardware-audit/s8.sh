#!/bin/sh
# s8: is there a stream-restart race? Back-to-back vfr runs vs gap-separated runs,
# with and without the 3A service. One variable at a time.
V=/home/orangepi/ar0234test/vfr
r() { # runs seconds gap  -> prints one line per run
	N=$1; S=$2; G=$3
	i=1
	while [ $i -le $N ]; do
		M="S8-${4}-$i-$$"
		echo $M > /dev/kmsg
		B=$(awk '/^vi0:/{f=1} f&&/frame =>/{print $4}' /sys/kernel/debug/mpp/vi)
		$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t $S >/tmp/s8.$i 2>&1
		R=$(grep -o 'frames=[0-9]* wall=[0-9.]*s fps=[0-9.]* timeouts=[0-9]*' /tmp/s8.$i)
		A=$(awk '/^vi0:/{f=1} f&&/frame =>/{print $4}' /sys/kernel/debug/mpp/vi)
		E=$(dmesg | awk -v m="$M" '$0 ~ m {f=1} f' | grep -c 'frame lost')
		printf '   %s run%-2s %s | vi0 cnt %s -> %s | frame_lost=%s\n' "$4" "$i" "$R" "$B" "$A" "$E"
		sleep $G
		i=$((i+1))
	done
}

echo "########## P1: 3A ACTIVE, 8 runs x 6 s, NO gap"
r 8 6 0 nogap
echo "########## P2: 3A ACTIVE, 8 runs x 6 s, 3 s gap"
r 8 6 3 gap
echo "########## P3: 3A STOPPED, 6 runs x 6 s, NO gap"
printf ' \n' | sudo -S -p '' systemctl stop ar0234-3ad
sleep 2
r 6 6 0 no3a
echo "########## P4: 3A STOPPED, 6 runs x 6 s, 3 s gap"
r 6 6 3 gap3a
echo "########## restore 3A"
printf ' \n' | sudo -S -p '' systemctl start ar0234-3ad
sleep 3
systemctl is-active ar0234-3ad
echo "########## final health"
$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 10 2>&1 | grep RESULT | sed 's/^/  /'
rm -f /tmp/s8.*
echo "########## DONE8"
