#!/bin/bash
cd /home/helios/Desktop/orangepi-build || exit 1
IMG=output/images/Orangepizero3w_1.0.0_debian_bookworm_minimal_linux6.6.98/Orangepizero3w_1.0.0_debian_bookworm_minimal_linux6.6.98.img

echo "=== the existing image ==="
ls -la "$IMG" "$IMG.sha" 2>/dev/null
head -3 "$IMG.sha" 2>/dev/null
echo "--- its own checksum line vs actual ---"
sha256sum "$IMG" 2>/dev/null

echo
echo "=== scp slot inside that image (offset 0x113BC00, 105912 bytes) ==="
dd if="$IMG" bs=1 skip=$((0x113BC00)) count=105912 status=none | sha256sum
echo "vendor scp : 07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749"
echo "our fw     : fb1d809d1fd6b5e9ee7553050f1d0569e45c3a4ceb4186026758c20875a38c26"

echo
echo "=== image head sanity (eGON.BT0 + sunxi-package) ==="
dd if="$IMG" bs=1 skip=$((0x2004)) count=8 status=none | od -c | head -2
dd if="$IMG" bs=1 skip=$((0x1004000)) count=16 status=none | od -c | head -2

echo
echo "=== partition table of the image ==="
fdisk -l "$IMG" 2>/dev/null | head -20 || sfdisk -d "$IMG" 2>/dev/null | head -20

echo
echo "=== the build config used ==="
echo "--- userpatches/config-a733.conf ---"
grep -vE '^\s*#|^\s*$' userpatches/config-a733.conf 2>/dev/null | head -25
echo "--- userpatches/config-default.conf ---"
grep -vE '^\s*#|^\s*$' userpatches/config-default.conf 2>/dev/null | head -25
echo "--- board conf ---"
grep -vE '^\s*#|^\s*$' external/config/boards/orangepizero3w.conf 2>/dev/null | head -30
