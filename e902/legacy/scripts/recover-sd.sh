#!/bin/bash
# recover-sd.sh -- ON TL101. Recover the board by writing the VENDOR scp.fex
# back into the scp slot of the SD card (the one that failed to boot).
#
#   sudo bash e902/recover-sd.sh            # auto-detect the card, show it, ask
#   sudo bash e902/recover-sd.sh /dev/sdX   # force a device
#
# Safety: it refuses to write to a device unless that device actually looks like
# this board's boot medium (eGON.BT0 at 0x2004 AND a 24 MiB head that either
# matches our known backup or at least parses as a sunxi boot package).
set -eu

ROOT=/home/helios/Desktop/orangepi-build
E=$ROOT/e902
BAK24=$E/backup/sd-boot-head-20260922.img
SCP=$E/backup/scp.fex.on-medium.bin
OFF=$((0x113BC00))
SIZE=105912
KNOWN_HEAD=114cd1f3a792b346acd87c8a550d4b3a09b5bfa7cce7cf0c406fa602b7bd4a54
VENDOR_SCP=07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749

die() { echo "error: $*" >&2; exit 1; }

looks_like_board_sd() {
    dev=$1
    # eGON.BT0 magic at 0x2004
    if ! dd if="$dev" bs=1 skip=$((0x2004)) count=8 status=none 2>/dev/null | grep -q 'eGON.BT0'; then
        return 1
    fi
    # sunxi boot package header somewhere in the first 24 MiB
    if ! dd if="$dev" bs=1 skip=$((0x1004000)) count=16 status=none 2>/dev/null | grep -q 'sunxi-package'; then
        return 1
    fi
    return 0
}

echo "=== candidate removable disks ==="
lsblk -o NAME,SIZE,TYPE,TRAN,MOUNTPOINT | grep -E 'disk|part' | head -30

DEVS=""
if [ "${1:-}" != "" ]; then
    DEVS="$1"
else
    for d in /dev/sd? /dev/mmcblk?; do
        [ -b "$d" ] || continue
        if looks_like_board_sd "$d"; then DEVS="$DEVS $d"; fi
    done
fi
[ -n "$DEVS" ] || die "no device found that looks like this board's SD (eGON.BT0 + sunxi-package). Pass it explicitly if you are sure."

for DEV in $DEVS; do
    echo
    echo "=== candidate: $DEV ==="
    lsblk -o NAME,SIZE,TYPE,MOUNTPOINT "$DEV" 2>/dev/null | head -8
    echo -n "current scp-slot sha256 : "
    dd if="$DEV" bs=1 skip=$OFF count=$SIZE status=none | sha256sum | awk '{print $1}'
    echo    "vendor scp sha256       : $VENDOR_SCP"
    echo -n "first 24 MiB sha256     : "
    dd if="$DEV" bs=1M count=24 status=none | sha256sum | awk '{print $1}'
    echo    "our known backup head   : $KNOWN_HEAD"
    echo
    echo "Writing the vendor scp back to $DEV at offset $OFF ($SIZE bytes) ..."
    dd if="$SCP" of="$DEV" bs=1 seek="$OFF" count=$SIZE conv=notrunc status=none
    sync
    echo -n "read back               : "
    dd if="$DEV" bs=1 skip=$OFF count=$SIZE status=none | sha256sum | awk '{print $1}'
    echo
    echo "DONE -- pull the card, put it back in the board, power on."
    echo "Expect the normal Orange Pi boot (and the vendor SCP running again)."
done
