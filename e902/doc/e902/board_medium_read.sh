#!/bin/sh
# Runs ON the board. READ-ONLY: dump the head of the boot medium and hash it,
# so the TL101-side rollback backup can be checked against reality.
# Nothing is written to the medium.
S() { printf 'orangepi\n' | sudo -S "$@" 2>&1; }

echo "##### boot medium identity"
S lsblk -o NAME,SIZE,TYPE,MOUNTPOINT
echo "--- which device is root / which is the boot device ---"
S 'cat /proc/cmdline; echo; lsblk -o NAME,SIZE,TYPE,MOUNTPOINT,PARTUUID'

echo
echo "##### read-only dump of the first 24 MiB of mmcblk1 (bootloader area)"
S dd if=/dev/mmcblk1 of=/tmp/sdhead_read.img bs=1M count=24 status=none
ls -la /tmp/sdhead_read.img
sha256sum /tmp/sdhead_read.img

echo
echo "##### also hash the first 1 MiB and the first 8 KiB for a coarse location map"
S dd if=/dev/mmcblk1 of=/tmp/h1.img bs=1M count=1 status=none
sha256sum /tmp/h1.img
S dd if=/dev/mmcblk1 of=/tmp/h8k.img bs=1024 count=8 status=none
sha256sum /tmp/h8k.img
echo "--- first 64 bytes of sector 16 (boot0 area) ---"
S od -A d -t x1 -j 8192 -N 64 /dev/mmcblk1

echo
echo "##### anything that looks like the scp.fex magic in the first 24 MiB?"
S 'grep -abo -m3 "$(printf "\x81\x40\x01\x41")" /tmp/sdhead_read.img 2>/dev/null || echo "(grep -P not usable; skipping)"'
