#!/bin/sh
# post-build.sh — 在 rootfs 打包前做最后处理（buildroot 传入 TARGET_DIR=$1，
# 环境变量有 HOST_DIR/STAGING_DIR/BINARIES_DIR/BR2_CONFIG）。
set -e

TARGET_DIR="$1"
BOARD_DIR="$(cd "$(dirname "$0")" && pwd)"

# ---- /boot：uImage 包装 + boot.scr（内核 Image/dtb 由 BR2_LINUX_KERNEL_INSTALL_TARGET 装好）----
# -A 必须用 arm（arch=0x02）而不是 arm64（0x16）：vendor U-Boot 的 legacy bootm
# 不认 0x16，报 "Unsupported Architecture 0x16"（与 vendor 1.0.2 镜像的
# uImage 头逐字节对比确认）。
# ⚠️ 不能用 -C gzip：vendor U-Boot 2018.07 的 legacy bootm 对 gzip 压缩内核
# 会卡死在看门狗复位（150 秒重启 13 次的启动循环，#51 实测）。保持不压缩。
rm -f "${TARGET_DIR}/boot/uImage"
"${HOST_DIR}/bin/mkimage" -A arm -O linux -T kernel -C none \
	-a 0x41000000 -e 0x41000000 -n "Linux kernel" \
	-d "${BINARIES_DIR}/Image" "${TARGET_DIR}/boot/uImage"

# dtb 统一放 /boot 根目录（boot.cmd 从 ${prefix} 加载）
for dtb in "${BINARIES_DIR}"/*.dtb; do
	[ -e "$dtb" ] && cp -f "$dtb" "${TARGET_DIR}/boot/"
done

"${HOST_DIR}/bin/mkimage" -C none -A arm -T script \
	-d "${BOARD_DIR}/boot.cmd" "${TARGET_DIR}/boot/boot.scr"

# uImage 尺寸红线：fdt 在 0x44000000，即 uImage 不得超过 48 MiB（R4）
uimg_size=$(stat -c %s "${TARGET_DIR}/boot/uImage")
if [ "${uimg_size}" -gt $((48*1024*1024)) ]; then
	echo "ERROR: uImage is ${uimg_size} bytes (> 48 MiB), dtb would be overwritten" >&2
	exit 1
fi

# ---- sshd：允许 root 密码登录（板卡验证用，用户 orangepi 也在）----
if [ -f "${TARGET_DIR}/etc/ssh/sshd_config" ]; then
	grep -q '^PermitRootLogin' "${TARGET_DIR}/etc/ssh/sshd_config" || \
		echo 'PermitRootLogin yes' >> "${TARGET_DIR}/etc/ssh/sshd_config"
fi

# ---- sudoers 权限（overlay 无法表达 0440）----
# 属主不用在这里 chown：非特权构建机 chown 会 EPERM 直接打断
# target-finalize；fs/common.mk 的 fakeroot 脚本最后会 chown -R 0:0 全树。
if [ -f "${TARGET_DIR}/etc/sudoers.d/zero3w" ]; then
	chmod 0440 "${TARGET_DIR}/etc/sudoers.d/zero3w"
fi

# ---- systemd：networkd + resolved + wpa_supplicant 使能 ----
wants="${TARGET_DIR}/etc/systemd/system/multi-user.target.wants"
mkdir -p "${wants}"
for unit in systemd-networkd.service systemd-resolved.service wpa_supplicant.service; do
	ln -sf "/lib/systemd/system/${unit}" "${wants}/${unit}"
done
rm -f "${TARGET_DIR}/etc/resolv.conf"
ln -sf /run/systemd/resolve/stub-resolv.conf "${TARGET_DIR}/etc/resolv.conf"

# ---- systemd：mask 掉拖慢冷启动且板上用不到的单元 ----
# pstore 是 eBPF prog-id LOAD/UNLOAD audit 日志的来源之一；remote-fs 无网络文件系统。
for unit in systemd-pstore.service remote-fs.target; do
	ln -sf /dev/null "${TARGET_DIR}/etc/systemd/system/${unit}"
done

# ---- WiFi 凭据注入（来自 tina-zero3w/local.env 或环境变量；凭据不进 git）----
conf="${TARGET_DIR}/etc/wpa_supplicant/wpa_supplicant.conf"
if [ -f "${conf}" ]; then
	if [ -n "${WIFI_SSID:-}" ]; then
		sed -i "s|@WIFI_SSID@|${WIFI_SSID}|; s|@WIFI_PSK@|${WIFI_PSK:-}|" "${conf}"
	else
		# 没给凭据：去掉 network 块，镜像照常产出，WiFi 上板后再配
		sed -i '/^network={/,/^}/d' "${conf}"
		echo "post-build.sh: WIFI_SSID 未设置，镜像不含 WiFi 凭据" >&2
	fi
fi
# 兼容把配置放 /etc/wpa_supplicant.conf 的调用方式
cp -f "${conf}" "${TARGET_DIR}/etc/wpa_supplicant.conf" 2>/dev/null || true

echo "post-build.sh: uImage ${uimg_size} bytes, boot.scr + units ready"
