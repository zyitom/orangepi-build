#!/bin/sh
# 首启把 rootfs 分区扩到整卡并在线扩文件系统（只跑一次）
set -e
FLAG=/var/lib/zero3w/expanded
[ -e "$FLAG" ] && exit 0
# 根设备：取 / 挂载点的 major:minor 再查 sysfs（内核按 PARTUUID 挂根时
# /proc/mounts 只写 /dev/root；也不依赖 util-linux 的 findmnt，镜像里没有）
MAJMIN=$(awk '$5 == "/" { print $3; exit }' /proc/self/mountinfo)
PARTNAME=$(basename "$(readlink -f "/sys/dev/block/$MAJMIN")")
ROOTDEV=/dev/$PARTNAME
case "$ROOTDEV" in
  /dev/mmcblk[0-9]p[0-9]*) DISK=${ROOTDEV%p[0-9]*}; PART=${ROOTDEV##*p} ;;
  /dev/sd[a-z][0-9]*)      DISK=${ROOTDEV%%[0-9]*}; PART=${ROOTDEV##*sd?} ;;
  *) echo "zero3w-expand: unsupported root device '$ROOTDEV'" >&2; exit 0 ;;
esac
[ -b "$DISK" ] || exit 0
# 分区后剩余空间 > 1G 才值得扩（parted 单位 MB）
FREE=$(parted -s "$DISK" unit MB print free 2>/dev/null | awk '/Free Space/ {gsub("MB","",$3); print int($3)}' | tail -1)
[ -n "$FREE" ] && [ "$FREE" -gt 1024 ] || { mkdir -p "$(dirname "$FLAG")"; touch "$FLAG"; exit 0; }
parted -s "$DISK" resizepart "$PART" 100%
resize2fs "$ROOTDEV"
mkdir -p "$(dirname "$FLAG")"
touch "$FLAG"
echo "zero3w-expand: rootfs expanded to full card" >&2
