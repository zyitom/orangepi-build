#!/bin/bash
# Build the ar0234-camera .deb from the repo on the host. One command:
#   bash packaging/build-deb.sh
# Output: ar0234-camera_<ver>_arm64.deb next to this script.
set -euo pipefail
cd "$(dirname "$0")/.."

VER=0.2.0
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

# --- payload in target filesystem layout ------------------------------------
# kernel side (ar0234_mipi.ko, patched vin, DTB) comes from the kernel/dtb
# packages built with userpatches/kernel/sun60iw2-current -- not shipped here.
# The binaries below are built on the board: make -C userspace, then copy
# build/ar0234-{3ad,rec,npu-zerocopy} to packaging/payload/usr/local/bin/.
for b in ar0234-3ad ar0234-rec ar0234-npu-zerocopy; do
	[ -x packaging/payload/usr/local/bin/$b ] || { echo "missing packaging/payload/usr/local/bin/$b (see above)"; exit 1; }
done
install -D -m 755 packaging/payload/usr/local/bin/ar0234-3ad "$STAGE/usr/local/bin/ar0234-3ad"
install -D -m 755 packaging/payload/usr/local/bin/ar0234-rec "$STAGE/usr/local/bin/ar0234-rec"
install -D -m 755 packaging/payload/usr/local/bin/ar0234-npu-zerocopy "$STAGE/usr/local/bin/ar0234-npu-zerocopy"
install -D -m 644 userspace/systemd/ar0234-3ad.service "$STAGE/etc/systemd/system/ar0234-3ad.service"
install -D -m 644 board/99-ar0234-camera.rules "$STAGE/etc/udev/rules.d/99-ar0234-camera.rules"
# fixed-mode config; a conffile so dpkg never clobbers operator edits
install -D -m 644 board/ar0234.conf "$STAGE/etc/ar0234.conf"
install -d "$STAGE/etc/modules-load.d" "$STAGE/mnt/extsd/ar0234" "$STAGE/DEBIAN"
printf '/etc/ar0234.conf\n' > "$STAGE/DEBIAN/conffiles"
printf '# AR0234 camera on MIPI-A\nvin_v4l2\n' > "$STAGE/etc/modules-load.d/ar0234.conf"
# G2D is not requested by anything at boot -> list it explicitly
install -D -m 644 board/g2d.conf "$STAGE/etc/modules-load.d/g2d.conf"
install -m 644 isp/isp_param_3dnr.bin isp/isp_param_no3dnr.bin "$STAGE/mnt/extsd/ar0234/"

# --- control + maintainer scripts -------------------------------------------
install -m 644 packaging/control  "$STAGE/DEBIAN/control"
install -m 755 packaging/postinst "$STAGE/DEBIAN/postinst"
install -m 755 packaging/prerm    "$STAGE/DEBIAN/prerm"
install -m 755 packaging/postrm   "$STAGE/DEBIAN/postrm"

# -Zxz: the board's dpkg predates zstd support in control.tar
dpkg-deb --build --root-owner-group -Zxz "$STAGE" "packaging/ar0234-camera_${VER}_arm64.deb" >/dev/null
echo "== built packaging/ar0234-camera_${VER}_arm64.deb =="
dpkg-deb --contents "packaging/ar0234-camera_${VER}_arm64.deb"
