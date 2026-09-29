#!/usr/bin/env bash
# flash-image.sh — 烧 SD 卡（有备份 + 多重确认；烧卡是破坏性操作）
#
# 用法: bash tina-zero3w/flash-image.sh [镜像] <设备>
#   镜像默认 ~/tina5/buildroot-out/images/sdcard.img
#   设备必须显式给出，例如 /dev/sdb（只接受整盘块设备）
set -euo pipefail

TINA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 本机私有设置（WIFI_SSID / WIFI_PSK / HOST_SUDO_PASS），不进 git，见 local.env.example
[[ -f "$TINA_DIR/local.env" ]] && source "$TINA_DIR/local.env"
# 宿主机 sudo：设了 HOST_SUDO_PASS 就经 stdin 喂给 sudo -S（无人值守），否则普通 sudo（交互输入）
sudo_() { if [[ -n "${HOST_SUDO_PASS+x}" ]]; then printf '%s\n' "$HOST_SUDO_PASS" | sudo -S -p '' "$@"; else sudo "$@"; fi; }

SDK="${TINA_SDK_DIR:-$HOME/tina5}"
IMG="${1:-$SDK/buildroot-out/images/sdcard.img}"
DEV="${2:?用法: flash-image.sh [镜像] <设备>，例如 /dev/sdb}"

[[ -f "$IMG" ]] || { echo "镜像不存在: $IMG"; exit 1; }
[[ -b "$DEV" ]] || { echo "不是块设备: $DEV"; exit 1; }

DEVBASE=$(basename "$DEV")
[[ "$DEVBASE" =~ ^(sd[a-z]+|mmcblk[0-9]+|nvme[0-9]+n[0-9]+)$ ]] || { echo "只接受整盘设备名 (sdX/mmcblkX/nvmeXnY)，拒绝: $DEV"; exit 1; }

# removable 检查（任务书要求）
REM=$(cat "/sys/block/$DEVBASE/removable" 2>/dev/null || echo 0)
if [[ "$REM" != "1" ]]; then
  echo "拒绝: /sys/block/$DEVBASE/removable = $REM（不是可移动介质）"
  echo "如果是 USB 读卡器被识别成固定盘，先确认设备号无误后再人工干预。"
  exit 1
fi

echo "=== 写入目标 ==="
lsblk -dno NAME,SIZE,MODEL,VENDOR "/dev/$DEVBASE" || true
echo "镜像: $IMG ($(du -h "$IMG" | cut -f1))"

# 已挂载分区先卸载
MP=$(lsblk -rno MOUNTPOINT "/dev/$DEVBASE" 2>/dev/null | grep -v '^$' || true)
if [[ -n "$MP" ]]; then
  echo "卸载已挂载分区: $MP"
  sudo_ bash -c "umount $(lsblk -rno MOUNTPOINT "/dev/$DEVBASE" | grep -v '^$' | tr '\n' ' ')" || true
fi

# 备份卡头 24 MiB（任务书要求）
TS=$(date +%Y%m%d-%H%M%S)
BK="$SDK/sd-backup/sd-head-$DEVBASE-$TS.img"
mkdir -p "$(dirname "$BK")"
echo "备份卡头 24 MiB -> $BK"
sudo_ dd if="/dev/$DEVBASE" bs=1M count=24 status=none > "$BK"
[[ "$(stat -c%s "$BK")" = $((24*1024*1024)) ]] || { echo "备份异常，中止"; exit 1; }

echo
echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
echo "即将把镜像写入 $DEV，目标盘上现有数据将全部丢失！"
echo "确认 $DEV 是插在电脑上的 SD 卡读卡器。"
echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
read -r -p "输入 yes 确认写入: " ans
[[ "$ans" == "yes" ]] || { echo "已取消"; exit 1; }

sudo_ dd if="$IMG" of="$DEV" bs=4M conv=fsync status=progress
sudo_ sync
echo
echo "烧录完成。写入前卡头备份: $BK"
echo "把卡插回 Zero 3W，接串口 /dev/ttyUSB0 (115200)，上电即应自动进入登录提示。"
echo "恢复旧系统: dd if=$BK of=$DEV bs=1M conv=fsync（仅恢复 24 MiB 头部）。"
