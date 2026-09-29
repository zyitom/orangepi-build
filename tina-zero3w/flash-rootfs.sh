#!/usr/bin/env bash
# flash-rootfs.sh — 只重烧 rootfs 分区（迭代用，跳过 boot 组件与 dd 全卡）
#
# 用法:
#   bash tina-zero3w/flash-rootfs.sh <设备>            # 用最近一次构建的 rootfs.ext4
#   bash tina-zero3w/flash-rootfs.sh <设备> <rootfs.img>
#
# 前提: build-image.sh 至少完整跑过一次（rootfs.ext4 在 ~/tina5/buildroot-out/images）。
# 安全: 与 flash-image.sh 同款防护 —— removable 检查、24 MiB 卡头备份、输入 yes。
# 注意: rootfs 分区起点 49152 扇区来自 genimage.cfg；改过分区布局的话这里要同步。
set -euo pipefail

TINA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 本机私有设置（WIFI_SSID / WIFI_PSK / HOST_SUDO_PASS），不进 git，见 local.env.example
[[ -f "$TINA_DIR/local.env" ]] && source "$TINA_DIR/local.env"
# 宿主机 sudo：设了 HOST_SUDO_PASS 就经 stdin 喂给 sudo -S（无人值守），否则普通 sudo（交互输入）
sudo_() { if [[ -n "${HOST_SUDO_PASS+x}" ]]; then printf '%s\n' "$HOST_SUDO_PASS" | sudo -S -p '' "$@"; else sudo "$@"; fi; }

SDK="${TINA_SDK_DIR:-$HOME/tina5}"
DEV="${1:?用法: flash-rootfs.sh <设备> 例如 /dev/sdb}"
IMG="${2:-$SDK/buildroot-out/images/rootfs.ext4}"

[[ -b "$DEV" ]] || { echo "不是块设备: $DEV"; exit 1; }
[[ -f "$IMG" ]] || { echo "镜像不存在: $IMG（先跑 build-image.sh）"; exit 1; }
DEVBASE=$(basename "$DEV")
[[ "$DEVBASE" =~ ^(sd[a-z]+|mmcblk[0-9]+)$ ]] || { echo "只接受 sdX/mmcblkX 整盘"; exit 1; }
REM=$(cat "/sys/block/$DEVBASE/removable" 2>/dev/null || echo 0)
[[ "$REM" == "1" ]] || { echo "拒绝: 不是可移动介质"; exit 1; }

echo "=== 写入目标 ==="
lsblk -dno NAME,SIZE,MODEL "/dev/$DEVBASE" || true
echo "rootfs: $IMG ($(du -h "$IMG" | cut -f1))"

# 已挂载分区先卸载
MP=$(lsblk -rno MOUNTPOINT "/dev/$DEVBASE" 2>/dev/null | grep -v '^$' || true)
if [[ -n "$MP" ]]; then
	echo "卸载已挂载分区: $MP"
	sudo_ bash -c "umount $(lsblk -rno MOUNTPOINT "/dev/$DEVBASE" | grep -v '^$' | tr '\n' ' ')" || true
fi

# 卡头备份（与 flash-image.sh 同规格，可互换恢复）
TS=$(date +%Y%m%d-%H%M%S)
BK="$SDK/sd-backup/sd-head-$DEVBASE-$TS.img"
mkdir -p "$(dirname "$BK")"
sudo_ dd if="/dev/$DEVBASE" bs=1M count=24 status=none > "$BK"
[[ "$(stat -c%s "$BK")" = $((24*1024*1024)) ]] || { echo "备份异常，中止"; exit 1; }

echo
echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
echo "即将只重写 $DEV 的 rootfs 分区（扇区 49152 起），boot 组件不动。"
echo "确认 $DEV 是插在电脑上的 SD 卡读卡器。输入 yes 继续。"
echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
read -r -p "输入 yes 确认写入: " ans
[[ "$ans" == "yes" ]] || { echo "已取消"; exit 1; }

# rootfs.ext4 是 sparse 文件时 dd 会写洞，先确保整卡分区表已存在（整盘烧过一次）
sudo_ dd if="$IMG" of="$DEV" bs=4M seek=$((49152*512/4194304)) conv=fsync status=progress
sudo_ sync
echo
echo "完成。备份: $BK   （boot 组件未被触碰，无需重写）"
