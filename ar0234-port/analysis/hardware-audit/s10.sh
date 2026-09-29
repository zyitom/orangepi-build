#!/bin/sh
# s10: quantify the T16b claim (open+close video4 freezes video0) with repeats,
# while excluding the T6 bad-start runs (where video0 is already stalled at t=8s).
V=/home/orangepi/ar0234test/vfr
OC=/tmp/openclose
vi() { awk '/^vi0:/{f=1} f&&/frame =>/{gsub(/,/,"",$0); print $4; exit}' /sys/kernel/debug/mpp/vi; }

echo "########## CONTROLS: 20 s runs, read vi0 at t=8 s and t=16 s, NO touch"
c=1
while [ $c -le 2 ]; do
	$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 20 >/tmp/s10c 2>&1 &
	P=$!; sleep 8; C1=$(vi); sleep 8; C2=$(vi); wait $P
	printf '  control%s: vi0 %s -> %s (delta %s) | %s\n' "$c" "$C1" "$C2" "$((C2-C1))" \
		"$(grep -o 'frames=[0-9]* wall=[0-9.]*s fps=[0-9.]* timeouts=[0-9]*' /tmp/s10c)"
	c=$((c+1))
done

echo "########## 10 x [20 s stream + open/close video4 at t=8 s]"
froze=0; skipped=0; ok=0; i=1
while [ $i -le 10 ]; do
	$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 20 >/tmp/s10 2>&1 &
	P=$!
	sleep 8; C1=$(vi)
	if [ "$C1" -lt 800 ]; then
		wait $P
		printf '  %-2s SKIP (T6 bad start: vi0=%s) | %s\n' "$i" "$C1" \
			"$(grep -o 'frames=[0-9]* wall=[0-9.]*s fps=[0-9.]* timeouts=[0-9]*' /tmp/s10)"
		skipped=$((skipped+1)); i=$((i+1)); continue
	fi
	$OC /dev/video4 1 >/dev/null
	sleep 8; C2=$(vi); wait $P
	D=$((C2-C1))
	R=$(grep -o 'frames=[0-9]* wall=[0-9.]*s fps=[0-9.]* timeouts=[0-9]*' /tmp/s10)
	if [ "$D" -lt 500 ]; then V="FREEZE"; froze=$((froze+1)); else V="ok"; ok=$((ok+1)); fi
	printf '  %-2s %-6s vi0 %s -> %s (delta %s in 8 s) | %s\n' "$i" "$V" "$C1" "$C2" "$D" "$R"
	i=$((i+1))
done
echo "########## SUMMARY: ok=$ok freeze=$froze skipped(T6)=$skipped"
echo "--- current vi0 ---"; sed -n '/^vi0:/,/^\*\*\*/p' /sys/kernel/debug/mpp/vi
echo "--- dmesg underflow/sensor-not-used since boot ---"
dmesg | grep -c 'usage count underflow' | sed 's/^/  underflow: /'
dmesg | grep -c 'sensor is not used' | sed 's/^/  sensor-not-used: /'
rm -f /tmp/s10 /tmp/s10c
echo "########## DONE10"
