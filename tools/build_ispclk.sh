#!/bin/sh
# build_ispclk.sh - external build of build/t17-ispclk/ispclk_scan.ko
# Deliberately a separate module dir: build/vin-d3d-lbc/ must stay byte-identical
# to "BSP + patches 0003/0005/0006/0007/0008" so its rebuild can be verified.
set -e
cd "$(dirname "$0")/.."

SRC=/home/helios/Desktop/orangepi-build
K=$SRC/kernel/orange-pi-6.6-sun60iw2
TC=$SRC/toolchains/gcc-arm-11.2-2022.02-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-
B=$(pwd)/build/t17-ispclk

mkdir -p "$B"
cp -f "$(pwd)/tools/t17-ispclk/ispclk_scan.c" "$B/"
cp -f "$(pwd)/tools/t17-ispclk/Makefile" "$B/"

make -C "$K" M="$B" ARCH=arm64 CROSS_COMPILE="$TC" -j6 modules

"${TC}strip" --strip-debug -o "$B/out/ispclk_scan.ko" "$B/ispclk_scan.ko" 2>/dev/null || {
	mkdir -p "$B/out"
	"${TC}strip" --strip-debug -o "$B/out/ispclk_scan.ko" "$B/ispclk_scan.ko"
}
echo "built: $B/out/ispclk_scan.ko"
md5sum "$B/out/ispclk_scan.ko"
modinfo "$B/out/ispclk_scan.ko" | head -8
