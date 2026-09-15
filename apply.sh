#!/bin/bash
# Apply the AR0234 BSP sensor driver into the Orange Pi sun60iw2 kernel tree.
# Run from the orangepi-build root (needs root because the kernel tree is
# owned by root):   sudo bash ar0234-port/apply.sh
set -e
SRC="${SRC:-/home/helios/Desktop/orangepi-build}"
K="$SRC/kernel/orange-pi-6.6-sun60iw2"
DIR="$(cd "$(dirname "$0")" && pwd)"

if [ ! -d "$K/bsp/drivers/vin" ]; then
	echo "ERROR: $K not found" >&2
	exit 1
fi

if [ -f "$K/bsp/drivers/vin/modules/sensor/ar0234_mipi.c" ]; then
	echo "kernel patch already applied, skipping"
else
	patch -p1 -d "$K" --forward < "$DIR/patches/0001-vin-sun60iw2-add-ar0234-sensor.patch"
fi

patch -p1 -d "$SRC" --forward < "$DIR/patches/0002-configs-enable-sensor-ar0234.patch"
# ISP hardware 3DNR: D3D in LBC mode (PKG mode gives "isp0 width error" on A733)
patch -p1 -d "$SRC" --forward < "$DIR/patches/0004-configs-enable-isp-3dnr-d3d-lbc.patch" \
	|| echo "0004 already applied?"

# vin framework: auto S_INPUT for generic V4L2 apps, streamon rollback, S_PARM type
patch -p1 -d "$K" --forward < "$DIR/patches/0003-vin-auto-s_input-streamon-rollback-parm-type.patch" \
	|| echo "0003 already applied?"


echo
echo "OK. AR0234 driver applied."
echo "Fast path (incremental, see ar0234-port/README.md):"
echo "  cd $K && sudo make ARCH=arm64 CROSS_COMPILE=<tc> modules dtbs"

# kernel/.config is needed for incremental in-tree module builds
if ! grep -q "^CONFIG_SENSOR_AR0234=m" "$K/.config" 2>/dev/null; then
	sed -i 's/^CONFIG_SENSOR_IMX219=m$/CONFIG_SENSOR_IMX219=m\nCONFIG_SENSOR_AR0234=m/' "$K/.config" \
		|| echo "CONFIG_SENSOR_AR0234=m" >> "$K/.config"
	echo "CONFIG_SENSOR_AR0234=m added to $K/.config"
fi
if grep -q "^# CONFIG_D3D is not set" "$K/.config" 2>/dev/null; then
	sed -i 's/^# CONFIG_D3D is not set$/CONFIG_D3D=y\n# CONFIG_D3D_LTF_EN is not set\n# CONFIG_D3D_COMPRESS_EN is not set\n# CONFIG_D3D_PKG_MODE is not set\nCONFIG_D3D_LBC_MODE=y/' "$K/.config"
	echo "CONFIG_D3D=y (LBC mode) set in $K/.config"
fi
