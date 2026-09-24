#!/bin/sh
# build_vin.sh [tag]
#
# Build the sunxi-vin module externally out of build/vin-d3d-lbc (a copy of the
# BSP driver sources with patches 0003/0005/0006/0007 applied - the kernel tree
# itself is root owned and must not be touched) and drop the stripped module in
# build/vin-d3d-lbc/out/vin_v4l2[-<tag>].ko.
#
#   tools/build_vin.sh            -> out/vin_v4l2.ko
#   tools/build_vin.sh 0007       -> out/vin_v4l2-0007.ko
#
# CONFIG_D3D / CONFIG_D3D_LBC_MODE come from board_compat.h (see the Makefile's
# -include), which is what makes the ISP 3DNR (D3D/LBC) code compile in.
set -e
cd "$(dirname "$0")/.."

TAG=$1
SRC=/home/helios/Desktop/orangepi-build
K=$SRC/kernel/orange-pi-6.6-sun60iw2
TC=$SRC/toolchains/gcc-arm-11.2-2022.02-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-
B=$(pwd)/build/vin-d3d-lbc

[ -d "$K" ] || { echo "kernel tree $K missing" >&2; exit 1; }

make -C "$K" M="$B" ARCH=arm64 CROSS_COMPILE="$TC" \
	CONFIG_CSI_VIN=m CONFIG_AW_VIDEO_SUNXI_VIN=m -j6 modules

if [ -n "$TAG" ]; then
	OUT="$B/out/vin_v4l2-$TAG.ko"
else
	OUT="$B/out/vin_v4l2.ko"
fi
"${TC}strip" --strip-debug -o "$OUT" "$B/vin_v4l2.ko"
echo "built: $OUT"
md5sum "$OUT"
modinfo -F srcversion "$OUT"
