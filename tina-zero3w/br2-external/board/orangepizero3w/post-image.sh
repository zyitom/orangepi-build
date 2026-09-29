#!/bin/sh
# post-image.sh — genimage 合成 sdcard.img（boot0 / boot_package 由 build-image.sh
# 预先放进 BINARIES_DIR：boot0_sdcard.fex + 已换 SCP 的 boot_package.fex）。
set -e

BOARD_DIR="$(cd "$(dirname "$0")" && pwd)"

for f in boot0_sdcard.fex boot_package.fex; do
	if [ ! -f "${BINARIES_DIR}/${f}" ]; then
		echo "ERROR: ${BINARIES_DIR}/${f} missing — build-image.sh should stage it" >&2
		exit 1
	fi
done

rm -rf "${BUILD_DIR}/genimage.tmp"
"${HOST_DIR}/bin/genimage" \
	--rootpath "${TARGET_DIR}" \
	--tmppath "${BUILD_DIR}/genimage.tmp" \
	--inputpath "${BINARIES_DIR}" \
	--outputpath "${BINARIES_DIR}" \
	--config "${BOARD_DIR}/genimage.cfg"

echo "post-image.sh: sdcard.img ready in ${BINARIES_DIR}"
