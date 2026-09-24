#!/bin/sh
# t16b.sh -- HANDOFF T16 with patch 0007 in place.
#
# The old reproducer was:
#   stream once, then rmmod/modprobe ar0234_mipi, then run any app that calls
#   VIDIOC_S_INPUT  ->  NULL deref panic in __vin_sensor_setup_link()
# 0007 turns that into a rejected ioctl.  This script re-runs exactly that
# sequence and then checks whether the pipeline recovered on its own.
S() { printf ' \n' | sudo -S -p '' "$@"; }
V=$HOME/ar0234test/vfr
M="T16B-$(date +%s)"

echo "=== 1. stream once (ISP has run) ==="
$V -t 4 -q -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 | grep -E 'RESULT|GAPS'

S sh -c "echo $M > /dev/kmsg"
echo "=== 2. rmmod vin_v4l2 (expected to be impossible: circular refcount) ==="
S rmmod vin_v4l2; echo "   rmmod vin_v4l2 rc=$?"
S sh -c "lsmod | grep -E 'vin_v4l2|vin_io|ar0234_mipi'"

echo "=== 3. reload the sensor module only ==="
S rmmod ar0234_mipi;    echo "   rmmod ar0234_mipi rc=$?"
S modprobe ar0234_mipi; echo "   modprobe ar0234_mipi rc=$?"

echo "=== 4. S_INPUT on the reloaded module (used to panic) ==="
$V -t 6 -q -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120
echo "   vfr rc=$?"

echo "=== 5. does the pipeline still work? ==="
$V -t 6 -q -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120
echo "   vfr rc=$?"

echo "=== 6. dmesg since $M ==="
S sh -c "dmesg | awk -v m=$M '\$0 ~ m {f=1} f' | sed 's/^/   /'"
echo "=== 7. board still up? ==="
cat /proc/uptime
