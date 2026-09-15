#!/bin/sh
# Install the AR0234 camera stack on an Orange Pi Zero 3W (A733) running the
# 6.6.98-sun60iw2 image. Run on the board from a copy of ar0234-port:
#   sudo sh board/install.sh            (then reboot)
# Undo: restore the *.orig backups it prints, apt-mark unhold the kernel packages.
set -e
DIR=$(cd "$(dirname "$0")/.." && pwd)
KV=6.6.98-sun60iw2
M=/lib/modules/$KV/kernel/bsp/drivers/vin
DTB=/boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb
VIND=/soc@3000000/vind@5800800

[ "$(uname -r)" = "$KV" ] || { echo "kernel $(uname -r) != $KV, rebuild the modules"; exit 1; }

backup() { [ -f "$1.orig" ] || cp "$1" "$1.orig"; echo "backup: $1.orig"; }

# modules: sensor driver + vin framework with the auto S_INPUT fixes and
# CONFIG_D3D (LBC mode) for the ISP 3DNR; vin_io.ko stays the packaged one
backup $M/vin_v4l2.ko
[ -f $M/modules/sensor/ar0234_mipi.ko ] && backup $M/modules/sensor/ar0234_mipi.ko
install -m 644 "$DIR/prebuilt/vin_v4l2.ko" $M/vin_v4l2.ko
install -m 644 "$DIR/prebuilt/ar0234_mipi.ko" $M/modules/sensor/ar0234_mipi.ko
depmod -a $KV

# device tree: AR0234 on MIPI-A through the ISP, unused MIPI-B sensor off
backup $DTB
fdtput -t s $DTB $VIND/sensor@5812000 sensor0_mname ar0234_mipi
fdtput -t i $DTB $VIND/sensor@5812000 sensor0_isp_used 1
fdtput -t s $DTB $VIND/sensor@5812020 status disabled

# load at boot (vin_v4l2 pulls vin_io and requests ar0234_mipi)
printf '# AR0234 camera on MIPI-A\nvin_v4l2\n' > /etc/modules-load.d/ar0234.conf

# libisp 3A parameters (first-pass AR0234 tuning, see README): all usable ISP
# modules on incl. hardware 3DNR; ar0234-3ad picks the no-3DNR set for 1200p120
mkdir -p /mnt/extsd/ar0234
install -m 644 "$DIR/isp/isp_param_3dnr.bin" "$DIR/isp/isp_param_no3dnr.bin" /mnt/extsd/ar0234/
install -m 644 "$DIR/isp/isp_param_3dnr.bin" /mnt/extsd/isp_param_config.bin
rm -f /mnt/isp0_*_ar0234_mipi_ctx_saved.bin

# keep apt from replacing the modules/DTB above
apt-mark hold linux-image-current-sun60iw2 linux-dtb-current-sun60iw2

# C++ userspace: ar0234-3ad (3A for every stream) + ar0234-rec (VE recorder)
make -C "$DIR/userspace" -j"$(nproc)"
make -C "$DIR/userspace" install
systemctl daemon-reload
systemctl enable ar0234-3ad.service
echo "done, reboot to use the new device tree and start ar0234-3ad"
