#!/bin/sh
# t14live.sh - live single-variable test: flip CSIC_VIPP1_IN (0x58008a4) from
# 1 (isp0 tx_ch1) to 0 (isp0 tx_ch0) while /dev/video4 is streaming, and watch
# whether frames start arriving.  Purely reversible (register written back).
VFR=/home/orangepi/ar0234test/vfr
OUT=/tmp/t14live.out
exec >"$OUT" 2>&1
echo "===== before ====="
/tmp/vinreg r 0x58008a0 4
echo "===== start video4 (30 s) ====="
$VFR -d /dev/video4 -w 1920 -h 1200 -f NV12 -p 1/120 -t 30 &
P=$!
sleep 6
echo "-- t=6s frames so far: $(grep -c 'frames=' /dev/null 2>/dev/null)"
echo "-- vi4: $(sed -n '/^vi4:/,/^\*\*\*/p' /sys/kernel/debug/mpp/vi ->/dev/null 2>/dev/null; awk '/^vi4:/{f=1} f{print} f&&/^\*\*\*/{exit}' /sys/kernel/debug/mpp/vi | grep -E 'frame =>|prs_in')"
echo "-- reg before write: $(/tmp/vinreg r 0x58008a4)"
/tmp/vinreg w 0x58008a4 0
echo "-- reg after  write: $(/tmp/vinreg r 0x58008a4)"
sleep 8
echo "-- vi4 after write (+8s): $(awk '/^vi4:/{f=1} f{print} f&&/^\*\*\*/{exit}' /sys/kernel/debug/mpp/vi | grep -E 'frame =>|prs_in')"
/tmp/vinreg r 0x58008a0 4
wait $P
echo "===== result ====="
grep -E 'RESULT|GAPS|\[.*\] (frames|no frame)' /tmp/t14live.out 2>/dev/null | tail -5
