#!/usr/bin/env bash
# build-image.sh — 一条命令构建 Zero 3W (A733) Tina/Buildroot SD 镜像
#
# 流程: fetch-sdk → prepare-kernel(内容寻址 tarball) → 离线换 SCP 的 boot 组件
#       → buildroot(内核+模块+rootfs+板上工具链) → genimage → images/sdcard.img
#
# 用法:
#   bash tina-zero3w/build-image.sh
#   WIFI_SSID=myap WIFI_PSK=mypass bash tina-zero3w/build-image.sh   # WiFi 凭据注入
#   （也可写进 tina-zero3w/local.env，模板见 local.env.example；不设则镜像不带 WiFi 凭据）
#
# 首次构建约 2-3 小时（工具链 + 内核 + 板上 gcc）；输入不变时增量构建只重跑变化部分。
# 输出: ~/tina5/buildroot-out/images/sdcard.img（可用 BUILD_OUT=... 覆盖）。
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TINA="$REPO_ROOT/tina-zero3w"
# 本机私有设置（WIFI_SSID / WIFI_PSK / HOST_SUDO_PASS），不进 git
[[ -f "$TINA/local.env" ]] && source "$TINA/local.env"
SDK="${TINA_SDK_DIR:-$HOME/tina5}"
OUT="${BUILD_OUT:-$SDK/buildroot-out}"
BR="$SDK/buildroot/upstream-2022.05"
EXT="$TINA/br2-external"
WORK="$SDK/boot-work"
IMAGES="$OUT/images"

UB_DEB="$REPO_ROOT/output/debs/u-boot/linux-u-boot-current-orangepizero3w_1.0.2_arm64.deb"
# = e902/vendor-scp/build.sh 的产物 e902/fw-out/vendor-scp.bin（sha256 302deda8…），
# 同一文件已作为 orangepi-build 的打包件随仓库跟踪，干净 clone 即可用
SCP_BIN="$REPO_ROOT/external/packages/pack-uboot/sun60iw2/bin/scp.fex"
GPU_DEB="$REPO_ROOT/external/cache/sources/sun60iw2_packages/bullseye/xserver/xserver-xorg-img-bxm_1.21.1-2_arm64.deb"
FW_SRC="$REPO_ROOT/external/cache/sources/orangepi-firmware-git/aic8800d80"
SCP_SHA=$(sha256sum "$SCP_BIN" | cut -d' ' -f1)

log() { printf '\n=== %s ===\n' "$*"; }

# 全程互斥锁：SDK 快照、buildroot O= 目录都不能并发读写（两个会话同时跑
# build-image.sh 会在 prepare-kernel 的 tmp-objects 和包 build 目录上互删）。
# 已持锁者直接继续；后来者阻塞等待并提示。
exec 9>"$SDK/.build-image.lock"
if ! flock -n 9; then
  echo "[wait] 另一个 build-image.sh 正在运行（锁 $SDK/.build-image.lock），等待其完成…"
  flock 9
fi

log "0. 输入检查"
for f in "$UB_DEB" "$SCP_BIN" "$GPU_DEB"; do
  [[ -f "$f" ]] || { echo "缺少 $f"; exit 1; }
done
[[ -d "$FW_SRC" ]] || { echo "缺少 $FW_SRC"; exit 1; }
echo "SCP payload: $SCP_BIN (sha256 ${SCP_SHA:0:16}…)"

log "1. SDK 子集 (~/tina5)"
bash "$TINA/fetch-sdk.sh"

log "2. 内核 tarball (厂家树 @2ac08e8c7 × 0000-0015 × 调优 config)"
bash "$TINA/prepare-kernel.sh"
KHASH=$(cat "$SDK/dl/linux-6.6.98-rt58-a733.tar.gz.hash")
# tarball 内容变了 → 强制 buildroot 重新解包/重编内核与模块
if [[ -f "$OUT/.zero3w-kernel-hash" && "$(cat "$OUT/.zero3w-kernel-hash")" != "$KHASH" ]]; then
  echo "内核输入变化: 强制重建 buildroot 内核/模块"
  rm -rf "$OUT"/build/linux-6.6.98* "$OUT"/build/linux-headers-*
fi
echo "$KHASH" > "$OUT/.zero3w-kernel-hash"

log "3. boot 组件 (orangepi boot0/boot_package + vendor-scp 换槽)"
mkdir -p "$WORK" "$IMAGES"
UBDIR="$WORK/uboot/usr/lib/linux-u-boot-current-orangepizero3w_1.0.2_arm64"
rm -rf "$WORK/uboot"; dpkg-deb -x "$UB_DEB" "$WORK/uboot"
cp "$UBDIR/boot0_sdcard.fex" "$IMAGES/boot0_sdcard.fex"

# bootpkg.py 以 SD 视角工作（包头固定在 0x1004000）：构造空白头镜像 → 换 SCP → 取回
HEAD="$WORK/boot-head.img"
rm -f "$HEAD"; truncate -s $((16#1004000 + 16#400000)) "$HEAD"
dd if="$UBDIR/boot_package.fex" of="$HEAD" bs=4096 seek=$((16#1004000/4096)) conv=notrunc status=none
python3 "$REPO_ROOT/e902/tools/bootpkg.py" info "$HEAD"
python3 "$REPO_ROOT/e902/tools/bootpkg.py" set-scp "$HEAD" "$SCP_BIN"
python3 "$REPO_ROOT/e902/tools/bootpkg.py" set-scp "$HEAD" "$SCP_BIN" --write
# 不用 grep -q：匹配即关管道 + pipefail 会让 python3 吃 SIGPIPE 造成假 FATAL
python3 "$REPO_ROOT/e902/tools/bootpkg.py" info "$HEAD" | grep 'checksum PASS' >/dev/null \
  || { echo "FATAL: 换槽后 checksum 不通过"; exit 1; }
# 新 valid_len = 0x158000（vendor-scp 120856 字节；bootpkg.py 文档与 e902 已验证一致）
dd if="$HEAD" of="$IMAGES/boot_package.fex" bs=4096 skip=$((16#1004000/4096)) count=$((16#158000/4096)) status=none
echo "boot0_sdcard.fex  $(sha256sum "$IMAGES/boot0_sdcard.fex" | cut -c1-16)…"
echo "boot_package.fex  $(sha256sum "$IMAGES/boot_package.fex" | cut -c1-16)… (SCP=$SCP_SHA)"

log "4. buildroot (内部工具链 + 内核 + rootfs + zero3w 包)"
export WIFI_SSID="${WIFI_SSID:-}"
export WIFI_PSK="${WIFI_PSK:-}"
make -C "$BR" O="$OUT" BR2_EXTERNAL="$EXT" sun60iw2p1_zero3w_defconfig
make -C "$OUT" source    # 提前把下载做完，失败更快暴露
make -C "$OUT" -j"$(nproc)"

log "5. 产物检查"
IMG="$IMAGES/sdcard.img"
[[ -f "$IMG" ]] || { echo "FATAL: 没有 $IMG"; exit 1; }
echo "sdcard.img        $(du -h "$IMG" | cut -f1)"
echo "uImage            $(du -h "$OUT/images/Image" 2>/dev/null | cut -f1 || true)"

# 闭源库缺依赖扫描（staging 的交叉 readelf）
READELF="$OUT/host/bin/aarch64-buildroot-linux-gnu-readelf"
if [[ -x "$READELF" ]]; then
  missing=0
  for lib in "$OUT"/target/usr/lib/libsrv_um.so* "$OUT"/target/usr/lib/libPVROCL.so* \
             "$OUT"/target/usr/lib/libVK_IMG.so* "$OUT"/target/usr/lib/libisp.so \
             "$OUT"/target/usr/lib/libAWIspApi.so "$OUT"/target/usr/lib/libvdecoder.so; do
    [[ -e "$lib" ]] || continue
    for dep in $("$READELF" -d "$lib" | awk '/NEEDED/{print $NF}' | tr -d '[]'); do
      base="${dep%.so*}"; found=$(find "$OUT/target/usr/lib" "$OUT/target/lib" -name "${dep}" -o -name "${dep}.*" 2>/dev/null | head -1)
      if [[ -z "$found" ]]; then echo "警告: $(basename "$lib") 缺依赖 $dep"; missing=1; fi
    done
  done
  [[ $missing -eq 0 ]] && echo "闭源库依赖检查: 无缺失"
fi

log "完成"
echo "镜像: $IMG"
echo "下一步: bash tina-zero3w/flash-image.sh <镜像> <设备>  （烧卡前会备份卡头并二次确认）"
