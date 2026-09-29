#!/bin/sh
# s9: how often does a stream come up broken (HANDOFF T6), and does a broken
# start poison the following ones (i.e. is a reboot really needed)?
V=/home/orangepi/ar0234test/vfr
SUDO="printf ' \n' | sudo -S -p ''"
vif() { awk '/^vi0:/{f=1} f&&/frame =>/{gsub(/,/,"",$0); print $4,$6,$8; exit}' /sys/kernel/debug/mpp/vi; }
bad=0; n=0
echo "########## 40 x 4 s runs at 1920x1200@120 (expect ~478 frames each)"
i=1
while [ $i -le 40 ]; do
	M="S9-$i-$$"; echo $M > /dev/kmsg
	B=$(vif)
	$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 4 >/tmp/s9 2>&1
	R=$(grep -o 'frames=[0-9]* wall=[0-9.]*s fps=[0-9.]* timeouts=[0-9]*' /tmp/s9)
	D=$(vif)
	F=$(echo "$R" | sed 's/frames=\([0-9]*\).*/\1/')
	if [ "${F:-0}" -lt 400 ]; then
		bad=$((bad+1)); FLAG=" <== BAD START"
	else
		FLAG=""
	fi
	n=$((n+1))
	printf '  run%-3s %-58s vi0[%s] -> [%s]%s\n' "$i" "$R" "$B" "$D" "$FLAG"
	if [ -n "$FLAG" ]; then
		echo "    --- recovery attempt: 3 more runs right after the bad one ---"
		j=1
		while [ $j -le 3 ]; do
			$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 4 >/tmp/s9r 2>&1
			printf '      retry%s: %s | dmesg frame_lost=%s\n' "$j" \
				"$(grep -o 'frames=[0-9]* wall=[0-9.]*s fps=[0-9.]* timeouts=[0-9]*' /tmp/s9r)" \
				"$(dmesg | awk -v m="$M" '$0 ~ m {f=1} f' | grep -c 'frame lost')"
			j=$((j+1))
		done
	fi
	i=$((i+1))
done
echo "########## SUMMARY: $bad bad starts out of $n runs"
echo "--- dmesg pattern totals over the whole loop ---"
dmesg | tail -400 | grep -c 'frame lost' | sed 's/^/  frame lost: /'
echo "--- vi0 now ---"; sed -n '/^vi0:/,/^\*\*\*/p' /sys/kernel/debug/mpp/vi
echo "--- final health run ---"
$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 6 2>&1 | grep RESULT | sed 's/^/  /'
rm -f /tmp/s9 /tmp/s9r
echo "########## DONE9"
