#!/bin/bash
# flash-scp-from-reader.sh -- flash fw-out/scp-ours-padded-105912.bin into the
# scp slot of the A733 SD card via a USB card reader ON TL101.
# Use when the board cannot boot (bl31 stuck at "wait arisc ready") so the
# on-board flash path (go-flash.sh) is unavailable.
#
# Usage (sudo, card in reader):
#   sudo bash e902/flash-scp-from-reader.sh /dev/sdX            # flash ours
#   sudo bash e902/flash-scp-from-reader.sh /dev/sdX restore    # restore vendor scp + add_sum
set -euo pipefail

MODE=${2:-flash}
DEV=${1:?usage: flash-scp-from-reader.sh /dev/sdX [restore]}
ROOT=/home/helios/Desktop/orangepi-build
E=$ROOT/e902
FIX=$E/tests/fix-bootpkg-sum.py
SLOT=$((0x113BC00)); SIZE=105912; PKG=$((0x1004000))
TS=$(date +%Y%m%d-%H%M%S)

if [ "$MODE" = restore ]; then
    PAYLOAD=$E/backup/scp.fex.on-medium.bin          # vendor scp (105912 B)
    SUMARGS=--vendor
else
    PAYLOAD=${3:-$E/fw-out/scp-ours-padded-105912.bin}  # default: latest ours; $3 overrides (e.g. canary)
    SUMARGS=
fi

[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -b "$DEV" ] || { echo "$DEV is not a block device"; exit 1; }
case "$DEV" in
    /dev/nvme*|/dev/mmcblk0) echo "refusing $DEV (host disk)"; exit 1;;
esac
# /dev/sdX from the USB reader is fine; the removable + boot-package-magic
# checks below are the real guards (reader LUNs swap sda/sdb on renum).
REM=$(cat "/sys/block/$(basename "$DEV")/removable" 2>/dev/null || echo 0)
[ "$REM" = 1 ] || { echo "$DEV is not a removable device - refusing"; exit 1; }
[ -f "$PAYLOAD" ] || { echo "missing $PAYLOAD"; exit 1; }

# unmount if automounted
umount ${DEV}?* 2>/dev/null || true; umount "$DEV" 2>/dev/null || true

# sanity: sunxi boot package magic at 0x1004000
python3 - "$DEV" <<'PY'
import sys, struct
with open(sys.argv[1],'rb') as f:
    f.seek(0x1004000); head = f.read(0x18)
sig = head[:14]
magic, add_sum = struct.unpack_from('<II', head, 0x10)
assert sig == b'sunxi-package\x00', 'sig=%r - not the A733 boot card?' % sig
print('boot package OK: sig=sunxi-package magic=%08x add_sum=%08x' % (magic, add_sum))
PY

# 1. always backup the 24 MiB head first
BK=$E/backup/sd-head-before-$MODE-$TS.img
dd if="$DEV" of="$BK" bs=1M count=24 status=none; sync
echo "backup 24MiB head -> $BK"
sha256sum "$BK" | awk '{print "  sha256", substr($1,1,16)"..."}'

# 2. write the scp slot
dd if="$PAYLOAD" of="$DEV" bs=1 skip=0 seek=$SLOT count=$SIZE conv=notrunc status=none
sync

# 3. read back and compare
sha=$(dd if="$DEV" bs=1 skip=$SLOT count=$SIZE status=none | sha256sum | cut -d' ' -f1)
want=$(sha256sum "$PAYLOAD" | cut -d' ' -f1)
[ "$sha" = "$want" ] || { echo "FATAL: slot readback mismatch"; exit 1; }
echo "slot readback OK: $sha"

# 4. recompute + write the boot package add_sum, with readback verify
python3 "$FIX" "$DEV" --write $SUMARGS

# 5. final dry check
python3 "$FIX" "$DEV"
echo
echo "=== DONE ($MODE) ==="
echo "next: eject card, put it back into the board, power on."
