#!/bin/sh
# 恢复脚本：把备份的 SD 引导头写回（用于启动挂死时救板）
# 用法：  sudo sh restore-sd.sh /dev/sdX          （X 是 TL101 上插入的 SD 卡设备）
# 先确认设备：lsblk
set -e
IMG=/home/helios/Desktop/orangepi-build/e902/backup/sd-boot-head.img
DEV="${1:-}"
[ -n "$DEV" ] || { echo "用法: sudo sh $0 /dev/sdX"; echo "先 lsblk 找到 SD"; exit 1; }
[ -b "$DEV" ] || { echo "$DEV 不是块设备"; exit 1; }

echo "=== 备份文件核对 ==="
ls -la "$IMG"
md5sum "$IMG"
echo "（应为 04ed3a3aebab3f07b1e35e65e8ca6ea2）"
echo
echo "=== 目标设备 ==="
lsblk "$DEV" 2>/dev/null || true
echo
echo "!!! 即将把 $IMG 的前 24MB 写到 $DEV （这会覆盖 SD 的 boot0+boot_package）"
echo "3 秒后开始... Ctrl-C 取消"
sleep 3
sudo -p '' sh -c "dd if='$IMG' of='$DEV' bs=1M count=24 conv=fsync status=none; sync; echo 写回完成"
echo "现在可以把 SD 插回板子、上电。"
