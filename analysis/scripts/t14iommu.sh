#!/bin/sh
# t14iommu.sh - controlled reproduction of the IOMMU null-address DMA
# ("0x0 is not mapped" / "Bug is in CSI module, invalid address: 0x0, id:0x2").
#
# Run it on the board as root.  It is written to be recoverable: a 150 s
# systemd timer reboots the board if a capture node gets stuck in D state,
# which is the failure mode this is trying to provoke.
OUT=/home/orangepi/t14iommu.out
exec >"$OUT" 2>&1
MARK="T14IOMMU-$(date +%s)"
echo "$MARK" > /dev/kmsg
echo "### $MARK  uptime=$(cut -d' ' -f1 /proc/uptime)  module=$(cat /sys/module/vin_v4l2/srcversion)"
systemd-run --on-active=150 --unit=t14guard /bin/systemctl reboot >/dev/null 2>&1
K() { dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f'; }
vi() { awk -v n="$1" '$0 ~ "^vi" n ":" {f=1} f {print} f && /^\*\*\*/ {exit}' /sys/kernel/debug/mpp/vi | grep -E 'frame =>|prs_in|input =>'; }

echo
echo "================ X1: S_FMT on video4 while video0 is streaming ================"
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1200,pixelformat=NV12
echo "-- v0 fmt after S_FMT: $(v4l2-ctl -d /dev/video0 --get-fmt-video | tr '\n' ' ')"
timeout -s KILL 30 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=500 --stream-to=/dev/null >/tmp/x1.log 2>&1 &
BG=$!
sleep 5
echo "-- vi0 while streaming: $(vi 0 | tr '\n' '|')"
echo "-- now S_FMT 960x600 on video4 (shares the uplink)"
v4l2-ctl -d /dev/video4 --set-fmt-video=width=960,height=600,pixelformat=NV12
echo "   rc=$?"
sleep 8
echo "-- vi0 after : $(vi 0 | tr '\n' '|')"
echo "-- x1 log: $(tail -2 /tmp/x1.log | tr '\n' ' ')"
wait $BG 2>/dev/null
echo "-- dmesg (X1):"; K | grep -Ev '\[ar0234_mipi\]' | tail -15

echo
echo "================ X2: both S_FMT first, then stream video0 ================"
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=NV12
v4l2-ctl -d /dev/video4 --set-fmt-video=width=960,height=600,pixelformat=NV12
echo "-- sensor win now: $(v4l2-ctl -d /dev/v4l-subdev0 --get-fmt-video 2>&1 | tr '\n' ' ')"
echo "-- v0 thinks it is configured for: $(v4l2-ctl -d /dev/video0 --get-fmt-video | tr '\n' ' ')"
timeout -s KILL 25 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=100 --stream-to=/dev/null >/tmp/x2.log 2>&1
echo "-- x2 rc=$? log: $(tail -3 /tmp/x2.log | tr '\n' ' ')"
echo "-- vi0: $(vi 0 | tr '\n' '|')"
echo "-- dmesg (X2):"; K | grep -Ev '\[ar0234_mipi\]' | tail -25
echo
echo "================ counters ================"
for p in "is not mapped" "Bug is in" "sunxi_iommu" "WARNING:" "Oops" "BUG:" "frame lost" "less than execpted" "sunxi_isp_reset"; do
	printf '%-22s %s\n' "$p" "$(K | grep -c "$p")"
done
dmesg > /home/orangepi/t14iommu.dmesg
systemctl stop t14guard.timer >/dev/null 2>&1
echo "### done $MARK"
