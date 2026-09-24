#!/bin/sh
# repro-close.sh [label] - reproduce HANDOFF 3.27:
# a plain open()+close() of /dev/video4 (no streaming) while /dev/video0 streams.
#   E1: video0 alone
#   E2: video0 + one open/close of video4 in the middle
set -u
VFR=/home/orangepi/ar0234test/vfr
OC=/tmp/openclose
OUT=/tmp/repro-close.out
exec >"$OUT" 2>&1

MARK="REPRO-$(date +%s)"
echo "$MARK" > /dev/kmsg
echo "### repro-close start=$(date -Is) uptime=$(cut -d' ' -f1 /proc/uptime)"
echo "### srcversion=$(cat /sys/module/vin_v4l2/srcversion)"
echo "### 3A=$(systemctl is-active ar0234-3ad)"

echo
echo "=== E1: video0 alone, 30 s ==="
$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 30 >/tmp/e1.log 2>&1
grep RESULT /tmp/e1.log | sed 's/^/  /'

echo
echo "=== E2: video0 30 s, open+close video4 once at t=8 s ==="
$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 30 >/tmp/e2.log 2>&1 &
P=$!
sleep 8
echo "  --- counters before touching video4 ---"
awk '/^vi0:/{f=1} f && /frame =>/{print "    vi0 " $0} f && /^\*\*\*/{exit}' /sys/kernel/debug/mpp/vi
$OC /dev/video4 1 | sed 's/^/  /'
sleep 1
echo "  --- counters right after ---"
awk '/^vi0:/{f=1} f && /frame =>/{print "    vi0 " $0} f && /^\*\*\*/{exit}' /sys/kernel/debug/mpp/vi
sleep 8
echo "  --- counters 8 s later ---"
awk '/^vi0:/{f=1} f && /frame =>/{print "    vi0 " $0} f && /^\*\*\*/{exit}' /sys/kernel/debug/mpp/vi
awk '/^vi4:/{f=1} f && /frame =>/{print "    vi4 " $0} f && /^\*\*\*/{exit}' /sys/kernel/debug/mpp/vi
wait $P
grep RESULT /tmp/e2.log | sed 's/^/  /'

echo
echo "=== dmesg since $MARK ==="
dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -vE "$MARK" | grep -viE "ar0234_mipi\]" | tail -30 | sed 's/^/  /'
echo
echo "=== dmesg underflow count since marker ==="
dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "usage count underflow"
echo "### done $(date -Is)"
