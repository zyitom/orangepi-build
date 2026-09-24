#!/bin/sh
# Install the AR0234 camera stack on an Orange Pi Zero 3W (A733) running the
# kernel built by orangepi-build with userpatches/kernel/sun60iw2-current
# (that kernel package already carries ar0234_mipi.ko and the patched vin).
# Run on the board from a copy of ar0234-port:
#   sudo sh board/install.sh            (then reboot)
# Undo: restore the *.orig backups it prints, apt-mark unhold the kernel packages.
set -e
DIR=$(cd "$(dirname "$0")/.." && pwd)
M=/lib/modules/$(uname -r)/kernel/bsp/drivers/vin
DTB=/boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb
VIND=/soc@3000000/vind@5800800

[ -f $M/modules/sensor/ar0234_mipi.ko ] || {
	echo "no ar0234_mipi.ko under $M: install a kernel built with userpatches/kernel/sun60iw2-current"; exit 1; }

backup() {
	[ -f "$1" ] || { echo "backup: $1 (absent, nothing to save)"; return 0; }
	[ -f "$1.orig" ] || cp "$1" "$1.orig"
	echo "backup: $1.orig"
}

# device tree: AR0234 on MIPI-A through the ISP, unused MIPI-B sensor off
backup $DTB
fdtput -t s $DTB $VIND/sensor@5812000 sensor0_mname ar0234_mipi
fdtput -t i $DTB $VIND/sensor@5812000 sensor0_isp_used 1
fdtput -t s $DTB $VIND/sensor@5812020 status disabled

# load at boot (vin_v4l2 pulls vin_io and requests ar0234_mipi)
printf '# AR0234 camera on MIPI-A\nvin_v4l2\n' > /etc/modules-load.d/ar0234.conf

# G2D: nothing in this image requests it at boot (DRM/KMS does not pull it in)
# and its misc device comes up 0600 root:root, so it needs both a modules-load
# entry and a udev rule to be usable by the video group.
install -m 644 "$DIR/board/g2d.conf" /etc/modules-load.d/g2d.conf
backup /etc/udev/rules.d/99-ar0234-camera.rules
install -m 644 "$DIR/board/99-ar0234-camera.rules" /etc/udev/rules.d/99-ar0234-camera.rules
udevadm control --reload-rules || true

# libisp 3A parameters (first-pass AR0234 tuning, see README): all usable ISP
# modules on incl. hardware 3DNR; ar0234-3ad picks the no-3DNR set for 1200p120
mkdir -p /mnt/extsd/ar0234
install -m 644 "$DIR/isp/isp_param_3dnr.bin" "$DIR/isp/isp_param_no3dnr.bin" /mnt/extsd/ar0234/
install -m 644 "$DIR/isp/isp_param_3dnr.bin" /mnt/extsd/isp_param_config.bin
rm -f /mnt/isp0_*_ar0234_mipi_ctx_saved.bin

# fixed-mode config (/etc/ar0234.conf): never overwrite an existing one --
# editing that file is exactly how the operator pins exposure/gain/AWB
backup /etc/ar0234.conf
[ -f /etc/ar0234.conf ] || install -m 644 "$DIR/board/ar0234.conf" /etc/ar0234.conf

# keep apt from replacing the kernel/DTB this setup depends on
apt-mark hold linux-image-current-sun60iw2 linux-dtb-current-sun60iw2

# C++ userspace: ar0234-3ad (3A for every stream) + ar0234-rec (VE recorder)
make -C "$DIR/userspace" -j"$(nproc)"
make -C "$DIR/userspace" install
systemctl daemon-reload
systemctl enable ar0234-3ad.service
echo "done, reboot to use the new device tree and start ar0234-3ad"
