#!/bin/bash
# Board-side test harness for the T14 dual-stream investigation. Run as root.
# All results go to /tmp/t14/ so the host can cat them afterwards.
D=/tmp/t14; mkdir -p $D
export PATH=$PATH:/usr/sbin:/sbin

dm() { dmesg -c; }

save() { # save <tag>
  dmesg > $D/$tag.dmesg 2>&1
  echo "OOPSCOUNT=$(grep -cE 'Oops|panic|Call trace|Unable to handle|Internal error' $D/$tag.dmesg)"
  echo "isp_frame_lost=$(grep -cE 'isp0 frame lost' $D/$tag.dmesg)"
  echo "isp_reset=$(grep -cE 'sunxi_isp_reset' $D/$tag.dmesg)"
  echo "vinc_frame_lost=$(grep -cE 'input lines in one frame' $D/$tag.dmesg)"
  echo "--- unique error/warn lines:"
  grep -E "ERR|WARN" $D/$tag.dmesg | sed -E 's/^\[[ 0-9.]+\] //' | sort | uniq -c | sort -rn | head -15
}

fmt_of() { v4l2-ctl -d $1 --get-fmt-video 2>&1 | tr '\n' ' ' | sed 's/  */ /g'; }

stream() { # stream <tag> <devspec> <seconds> [sensor-frame-rate]
  local tag=$1 spec=$2 secs=$3 fr=${4:-}
  echo "===== STREAM $tag : $spec for ${secs}s fr=${fr:-default}"
  dm
  if [ -n "$fr" ]; then
    v4l2-ctl -d /dev/v4l-subdev0 -c frame_rate=$fr 2>&1
    echo "sensor frame_rate set to $fr"
    v4l2-ctl -d /dev/v4l-subdev0 -l 2>&1 | grep -E "frame_rate|exposure|gain " || true
  fi
  local pids=""
  local i=0
  for one in $(echo $spec | tr '+' ' '); do
    local id w h
    id=$(echo $one|cut -d: -f1); w=$(echo $one|cut -d: -f2); h=$(echo $one|cut -d: -f3)
    v4l2-ctl -d /dev/video$id --set-fmt-video=width=$w,height=$h,pixelformat=NV12 > $D/$tag.v$id.fmt 2>&1
    echo "--- video$id set fmt rc=$?:"; cat $D/$tag.v$id.fmt
    if [ $i -eq 1 ]; then sleep 1; fi
    ( s=$(date +%s%N)
      timeout -s INT $secs v4l2-ctl -d /dev/video$id --stream-mmap --stream-count=2000000 > $D/$tag.v$id.raw 2>&1
      rc=$?
      e=$(date +%s%N)
      printf 'rc=%s elapsed_ms=%s\n' "$rc" "$(( (e-s)/1000000 ))" > $D/$tag.v$id.sum ) &
    pids="$pids $!"
    i=$((i+1))
  done
  wait $pids
  for one in $(echo $spec | tr '+' ' '); do
    local id
    id=$(echo $one|cut -d: -f1)
    echo "--- video$id summary: $(cat $D/$tag.v$id.sum)"
    echo "    frames_printed=$(tr -cd '<.' < $D/$tag.v$id.raw | wc -c)"
    echo "    formats: $(fmt_of /dev/video$id)"
  done
  save $tag
  echo "--- media graph fmt (sensor/mipi/csi/tdm/isp/scalers):"
  media-ctl -p 2>/dev/null | grep -E "entity [0-9]+: (ar0234|sunxi_isp|sunxi_scaler|vin_video|sunxi_mipi|sunxi_csi|sunxi_tdm_rx)|fmt:" | head -40
}

case "$1" in
  stream) shift; stream "$@" ;;
  save)   shift; save "$@" ;;
  fmt)    fmt_of /dev/video${2:-0} ;;
  graph)  media-ctl -p -d /dev/media0 ;;
  *) echo "usage: $0 stream <tag> <devspec> <secs> | save <tag> | fmt <id> | graph" ;;
esac
