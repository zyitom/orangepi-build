#!/bin/sh
# t17-bw.sh - capture /sys/kernel/debug/mpp/vi in the two scenarios that matter:
#   1. single stream on /dev/video0
#   2. both /dev/video0 and /dev/video4 streaming at once
# plus the raw driver-side numbers the "Bandwidth" lines are built from.
set -u
VFR=/home/orangepi/ar0234test/vfr
OUT=/tmp/t17-bw.out
exec >"$OUT" 2>&1

echo "### t17-bw start=$(date -Is) uptime=$(cut -d' ' -f1 /proc/uptime)"
echo "### srcversion=$(cat /sys/module/vin_v4l2/srcversion)"

echo
echo "################ 1. IDLE (nothing streaming) ################"
cat /sys/kernel/debug/mpp/vi

echo
echo "################ 2. SINGLE stream /dev/video0 ################"
$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 20 >/tmp/bw0.log 2>&1 &
P0=$!
sleep 8
cat /sys/kernel/debug/mpp/vi
echo "--- vi0 vfr so far ---"; cat /tmp/bw0.log
wait $P0
echo "--- video0 RESULT ---"; grep RESULT /tmp/bw0.log

echo
echo "################ 3. DUAL streams video0 + video4 ################"
$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 24 >/tmp/bw0.log 2>&1 &
P0=$!
sleep 1
$VFR -d /dev/video4 -w 1920 -h 1200 -f NV12 -p 1/120 -t 22 >/tmp/bw4.log 2>&1 &
P4=$!
sleep 10
cat /sys/kernel/debug/mpp/vi
wait $P0; wait $P4
echo "--- video0 RESULT ---"; grep RESULT /tmp/bw0.log
echo "--- video4 RESULT ---"; grep RESULT /tmp/bw4.log

echo
echo "################ 4. AFTER both stopped ################"
cat /sys/kernel/debug/mpp/vi
echo "### done $(date -Is)"
