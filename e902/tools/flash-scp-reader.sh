#!/bin/bash
# flash-scp-reader.sh -- ON TL101. Write an SCP image into the scp item of
# the A733 boot SD card sitting in a USB card reader. Use when the board does
# not boot (e.g. FEL after a bad write), otherwise prefer flash-scp.sh.
#
#   sudo bash e902/tools/flash-scp-reader.sh /dev/sdX [payload]
#   sudo bash e902/tools/flash-scp-reader.sh /dev/sdX external/packages/pack-uboot/sun60iw2/bin/scp.fex  # restore shipped
#
# Default payload: e902/fw-out/vendor-scp.bin. The 24 MiB card head is backed
# up to e902/backup/ first.
set -euo pipefail

E=$(cd "$(dirname "$0")/.." && pwd)
DEV=${1:?usage: flash-scp-reader.sh /dev/sdX [payload]}
PAYLOAD=${2:-$E/fw-out/vendor-scp.bin}
TS=$(date +%Y%m%d-%H%M%S)

[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -b "$DEV" ] || { echo "$DEV is not a block device"; exit 1; }
case "$DEV" in
	/dev/nvme*|/dev/mmcblk0) echo "refusing $DEV (host disk)"; exit 1;;
esac
# the removable flag + boot-package magic (checked by bootpkg.py) are the guards;
# reader LUNs swap sda/sdb on re-enumeration
[ "$(cat "/sys/block/$(basename "$DEV")/removable" 2>/dev/null || echo 0)" = 1 ] \
	|| { echo "$DEV is not removable - refusing"; exit 1; }
[ -f "$PAYLOAD" ] || { echo "missing $PAYLOAD"; exit 1; }

umount "${DEV}"?* 2>/dev/null || true

python3 "$E/tools/bootpkg.py" info "$DEV"

BK=$E/backup/sd-head-before-flash-$TS.img
mkdir -p "$E/backup"
dd if="$DEV" of="$BK" bs=1M count=24 status=none
sync
echo "backup -> $BK"

python3 "$E/tools/bootpkg.py" set-scp "$DEV" "$PAYLOAD" --write
sync
python3 "$E/tools/bootpkg.py" info "$DEV"
echo "done: sync; eject $DEV, put the card back and power on"
