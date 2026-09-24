#!/bin/sh
S() { sudo -p '' "$@"; }
IMG=/home/helios/Desktop/orangepi-build/e902/backup/sd-boot-head.img
[ -f "$IMG" ] || { echo "备份不存在: $IMG"; exit 1; }

echo "=== 当前块设备 ==="
lsblk -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINT,MODEL 2>/dev/null | grep -vE '^loop'

REF=$(dd if="$IMG" bs=512 skip=16 count=1 status=none | md5sum | awk '{print $1}')
echo
echo "备份 boot0 扇区 md5 = $REF"
ROOTSRC=$(findmnt -n -o SOURCE / 2>/dev/null)
echo "系统根设备 = $ROOTSRC"
echo

CAND=""
for d in /dev/sd[a-z] /dev/mmcblk[0-9]; do
  [ -b "$d" ] || continue
  case "$ROOTSRC" in "$d"*) echo "跳过系统盘 $d"; continue;; esac
  sz=$(blockdev --getsize64 "$d" 2>/dev/null || echo 0)
  [ "$sz" -gt 8000000000 ] || continue
  m=$(S dd if="$d" bs=512 skip=16 count=1 status=none 2>/dev/null | md5sum | awk '{print $1}')
  printf '候选 %-12s %sGB  扇区16 md5=%s\n' "$d" "$((sz/1000000000))" "$m"
  [ "$m" = "$REF" ] && CAND="$d"
done

echo
if [ -z "$CAND" ]; then
  echo ">>> 没有匹配的 SD（还没插上？或卡不是这块）——未做任何写入"
  exit 1
fi

echo ">>> 匹配到 $CAND —— 写回前 24MB（原 boot0 + boot_package）"
S sh -c "dd if='$IMG' of='$CAND' bs=1M count=24 conv=fsync status=none; sync; echo 写回完成"
echo "回读扇区 32800（应为 sunxi-package）:"
S dd if="$CAND" bs=512 skip=32800 count=1 status=none 2>/dev/null | od -An -tx1 -N16
echo "现在可以把 SD 插回板子、上电。"
