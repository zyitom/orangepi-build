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

# two vinc on one CSI: bind the pipeline before sunxi_isp_sensor_type()
patch -p1 -d "$K/bsp/drivers/vin" --forward < "$DIR/patches/0005-vin-fix-second-vinc-pipeline-binding.patch" \
	|| echo "0005 already applied?"

# ISP 3DNR vs sensor blanking interlock (1920x1200@120 frame loss).
# 0005/0006 use paths relative to bsp/drivers/vin - that directory is also
# what the external build/vin-d3d-lbc mirror is made of, so both build paths
# stay in sync. 0001/0003 use kernel root relative paths.
patch -p1 -d "$K/bsp/drivers/vin" --forward < "$DIR/patches/0006-vin-isp-3dnr-blanking-interlock.patch" \
	|| echo "0006 already applied?"
# sensor-module reload + S_INPUT: NULL deref panic in __vin_sensor_setup_link()
patch -p1 -d "$K/bsp/drivers/vin" --forward < "$DIR/patches/0007-vin-fix-sensor-setup-link-null-deref.patch" \
	|| echo "0007 already applied?"

# second VI channel on the same ISP (vinc4/vipp1 = /dev/video4): clamp
# vinc*_isp_tx_ch to the single ISP output channel, refuse to rewrite a
# streaming uplink from another capture node, report a capture node whose
# sensor slot is empty, refuse the virtual isp@58ffffc instances and stop
# the scaler get_selection ERR noise.  Paths are relative to
# bsp/drivers/vin, like 0005/0006/0007.
patch -p1 -d "$K/bsp/drivers/vin" --forward < "$DIR/patches/0008-vin-second-channel-and-pipeline-guards.patch" \
	|| echo "0008 already applied?"

# a capture node opened and closed *without* streaming must still run the
# teardown tail of vin_close(), otherwise the streaming sibling node on the
# shared mipi0/csi0/tdm_rx0/isp0 pipe dies: 120 fps -> 29 fps plus
# "5831000.vinc: Runtime PM usage count underflow" (HANDOFF 3.27).  The
# vin_pipeline_call(vinc, close, ...) in that tail is skipped for a node
# that never did S_INPUT, whose pipeline was never prepared.
patch -p1 -d "$K/bsp/drivers/vin" --forward < "$DIR/patches/0009-vin-close-complete-rollback.patch" \
	|| echo "0009 already applied?"

# LBC output formats (LC21/LBC_2X/...) are listed in the capture node's format
# table and accepted by S_FMT, but the VIPP scaler of this SoC has no LBC output
# (vipp100/vipp200 have no LBC register writes at all): the stream then runs and
# silently produces 0 frames with nothing in dmesg.  Refuse them at S_FMT.
patch -p1 -d "$K/bsp/drivers/vin" --forward < "$DIR/patches/0010-lbc-output-refused-in-scaler.patch" \
	|| echo "0010 already applied?"

# "CSI Bandwidth" in /sys/kernel/debug/mpp/vi was always 0: the old
# buf_size * (1000/frame_internal/1000) is integer-truncated to 0 below
# 1000 fps.  Compute bytes/second instead.
patch -p1 -d "$K/bsp/drivers/vin" --forward < "$DIR/patches/0011-vin-csi-bandwidth-fix.patch" \
	|| echo "0011 already applied?"

# __vin_pipeline_close() undoes three operations that __vin_pipeline_open() does,
# but only vin_md_set_power() is reference counted (vind->use_count).  A close
# without a partner open - open("/dev/video0") + close() with no ioctl, which is
# what any device enumerator does - therefore disabled the vind_mclkpin regulator
# and the capture node's runtime PM one extra time:
#   "Runtime PM usage count underflow!", a _regulator_disable WARNING and
#   "vin_pin_disable: ... fail to disable regulator!".  Gate all three on the
#   same reference count.
patch -p1 -d "$K/bsp/drivers/vin" --forward < "$DIR/patches/0012-vin-pipeline-close-refcount.patch" \
	|| echo "0012 already applied?"

# read-only tripwire in vin_pin_disable(): an already-disabled regulator there
# means an enable-count imbalance (a disable without a partner enable) and is
# reported as an explicit vin_err instead of a generic regulator WARNING.
# kernel-root-relative paths, like 0001/0003/0008.
patch -p1 -d "$K" --forward < "$DIR/patches/0013-vin-pin-disable-imbalance-diagnostic.patch" \
	|| echo "0013 already applied?"


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
