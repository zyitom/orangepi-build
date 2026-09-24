#!/bin/sh
# verify0008.sh - regression + fix verification for module 0008 (run on board as root)
VFR=/home/orangepi/ar0234test/vfr
OUT=/home/orangepi/verify0008.out
exec >"$OUT" 2>&1
MARK="V0008-$(date +%s)"
echo "$MARK" > /dev/kmsg
echo "### $MARK uptime=$(cut -d' ' -f1 /proc/uptime) module=$(cat /sys/module/vin_v4l2/srcversion)"
systemd-run --on-active=400 --unit=v8guard /bin/systemctl reboot >/dev/null 2>&1
K() { dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f'; }
vi() { awk -v n="$1" '$0 ~ "^vi" n ":" {f=1} f {print} f && /^\*\*\*/ {exit}' /sys/kernel/debug/mpp/vi | grep -E 'frame =>|prs_in|input =>|output =>'; }
res() { grep -E 'RESULT|GAPS' "$1"; }

echo; echo "=========== A: /dev/video0 alone, 1200p120, 60 s (regression) ==========="
$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 60 > /tmp/a.log 2>&1
echo "rc=$?"; res /tmp/a.log; echo "vi0: $(vi 0 | tr '\n' '|')"

echo; echo "=========== B: /dev/video4 alone, 1200p120, 20 s (was 0 frames) ==========="
$VFR -d /dev/video4 -w 1920 -h 1200 -f NV12 -p 1/120 -t 20 > /tmp/b.log 2>&1
echo "rc=$?"; res /tmp/b.log; echo "vi4: $(vi 4 | tr '\n' '|')"

echo; echo "=========== C: video0 + video4 together, 20 s ==========="
$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 26 > /tmp/c0.log 2>&1 &
P0=$!
sleep 3
$VFR -d /dev/video4 -w 1920 -h 1200 -f NV12 -p 1/120 -t 20 > /tmp/c4.log 2>&1
echo "v4 rc=$?"; res /tmp/c4.log
wait $P0; echo "v0 rc=$?"; res /tmp/c0.log
echo "vi0: $(vi 0 | tr '\n' '|')"
echo "vi4: $(vi 4 | tr '\n' '|')"

echo; echo "=========== D: mismatched pipeline must now fail loudly ==========="
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=NV12 >/dev/null
echo "v0 S_FMT 1080 rc=$?"
v4l2-ctl -d /dev/video4 --set-fmt-video=width=960,height=600,pixelformat=NV12 >/dev/null
echo "v4 S_FMT 960x600 rc=$?"
timeout -s KILL 20 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=100 --stream-to=/dev/null > /tmp/d.log 2>&1
echo "streamon/stream rc=$? (137=hung, non-zero=refused)"
echo "vi0: $(vi 0 | tr '\n' '|')"

echo; echo "=========== E: S_FMT on video4 while video0 streams ==========="
$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 40 > /tmp/e0.log 2>&1 &
PE=$!
sleep 4
v4l2-ctl -d /dev/video4 --set-fmt-video=width=960,height=600,pixelformat=NV12; echo "live v4 S_FMT rc=$?"
sleep 3
echo "vi0 still: $(vi 0 | tr '\n' '|')"
wait $PE; res /tmp/e0.log

echo; echo "=========== dmesg since $MARK ==========="
dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -Ev '\[ar0234_mipi\]' | tail -40
echo "--- counters ---"
for p in "is not mapped" "Bug is in" "sunxi_iommu" "WARNING:" "Oops" "BUG:" "frame lost" "less than execpted" "sunxi_isp_reset" "get_selection error" "already stream off" "cannot be close" "refusing"; do
	printf '%-22s %s\n' "$p" "$(K | grep -c "$p")"
done
dmesg > /home/orangepi/verify0008.dmesg
systemctl stop v8guard.timer >/dev/null 2>&1
echo "### done $MARK"
