#!/bin/bash
# Board-side dual-stream test (run as root). Order Y:
#   v0 S_FMT -> v0 stream on -> DELAY s -> v4 S_FMT -> v4 stream on
# rc==0 for a stream means all --stream-count frames were dequeued within its timeout.
#   dualtest.sh <fr> <count0> <count4> <delay> <timeout0> <timeout4>
D=/tmp/t14; mkdir -p $D
export PATH=$PATH:/usr/sbin:/sbin
FR=${1:-30}; C0=${2:-120}; C4=${3:-90}; DELAY=${4:-4}; T0=${5:-30}; T4=${6:-15}

dmesg -n 8
echo "printk=$(cat /proc/sys/kernel/printk)"
v4l2-ctl -d /dev/v4l-subdev0 -c frame_rate=$FR >/dev/null 2>&1
echo "sensor frame_rate=$(v4l2-ctl -d /dev/v4l-subdev0 -C frame_rate 2>&1 | tail -1)"
dmesg -c > /dev/null

echo "--- v0 S_FMT 1920x1080 NV12:"; v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=NV12 2>&1 | head -2
echo "T_V0STREAM=$(date +%s.%N)"
( timeout $T0 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=$C0 >/dev/null 2>&1
  echo "RESULT v0 rc=$? want=$C0 frames  t=$(date +%s.%N)" ) > $D/dt.v0 &
P0=$!
sleep $DELAY
echo "T_V4FMT=$(date +%s.%N)  last_dmesg=[$(dmesg | tail -1)]"
echo "--- v4 S_FMT 640x400 NV12:"; v4l2-ctl -d /dev/video4 --set-fmt-video=width=640,height=400,pixelformat=NV12 2>&1 | head -2
echo "T_V4STREAM=$(date +%s.%N)  last_dmesg=[$(dmesg | tail -1)]"
( timeout $T4 v4l2-ctl -d /dev/video4 --stream-mmap --stream-count=$C4 >/dev/null 2>&1
  echo "RESULT v4 rc=$? want=$C4 frames  t=$(date +%s.%N)" ) > $D/dt.v4 &
P4=$!
wait $P0 $P4
cat $D/dt.v0; cat $D/dt.v4
echo "T_END=$(date +%s.%N)  last_dmesg=[$(dmesg | tail -1)]"
dmesg > $D/dt.dmesg
echo "--- dmesg after ($(wc -l < $D/dt.dmesg) lines):"
cat $D/dt.dmesg
echo "SUMMARY oops=$(grep -cE 'Oops|panic|Call trace' $D/dt.dmesg) iommu_fault=$(grep -cE 'is not mapped|Bug is in' $D/dt.dmesg) isp_frame_lost=$(grep -cE 'isp0 frame lost' $D/dt.dmesg) vinc_frame_lost=$(grep -cE 'input lines in one frame' $D/dt.dmesg) scaler_frame_lost=$(grep -cE 'scaler. frame lost' $D/dt.dmesg)"
echo "MEDIAGRAPH:"
media-ctl -p 2>/dev/null | grep -E "entity [0-9]+: (ar0234|sunxi_isp|sunxi_scaler|vin_video|sunxi_mipi|sunxi_csi|sunxi_tdm_rx)|fmt:" | head -40
