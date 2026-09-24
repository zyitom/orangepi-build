#!/bin/sh
# Surgical write of the "scp" item inside the sunxi boot package.
#
# Layout proven by e902/doc/e902/FLASH-SPEC.md (decoded from a read-only dump of
# the medium):
#   package @ 0x1004000 "sunxi-package", TOC entry "scp":
#     relative offset 0x137C00 -> absolute 0x113BC00, size 0x19DB8 (105912)
#
# The payload MUST be exactly 0x19DB8 bytes or the TOC offsets after it break.
#
# Usage (ON THE BOARD, as root):
#   sh flash-scp.sh <padded-firmware.bin> [/dev/mmcblk1]
#
# It refuses to write unless the size matches, reads back, and compares hashes.
set -eu

FW="${1:-}"
DEV="${2:-/dev/mmcblk1}"
OFF=$((0x113BC00))
SIZE=105912

die() { echo "error: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "must run as root"
[ -n "$FW" ] && [ -f "$FW" ] || die "usage: $0 <padded-firmware.bin> [$DEV]"
[ -b "$DEV" ] || die "$DEV is not a block device"

N=$(stat -c%s "$FW")
[ "$N" = "$SIZE" ] || die "firmware is $N bytes; must be exactly $SIZE (0x19DB8). Pad with 0x00."

echo "device     : $DEV"
echo "offset     : $OFF (0x113BC00)"
echo "size       : $SIZE"
echo "source     : $FW"
echo "src sha256 : $(sha256sum "$FW" | cut -d' ' -f1)"
echo
read -r -p "This WRITES the boot medium. Type YES to continue: " ans
[ "$ans" = YES ] || die "aborted by user"

echo "1/3 writing ..."
dd if="$FW" of="$DEV" bs=1 seek="$OFF" count="$SIZE" conv=notrunc status=none

echo "2/3 reading back ..."
have=$(dd if="$DEV" bs=1 skip="$OFF" count="$SIZE" status=none | sha256sum | cut -d' ' -f1)
want=$(sha256sum "$FW" | cut -d' ' -f1)
echo "    wrote : $want"
echo "    read  : $have"

echo "3/3 result"
if [ "$have" = "$want" ]; then
    echo "OK - scp item written and verified."
    # The boot package carries an add_sum over its whole content; boot0
    # rejects a package whose sum no longer matches and the board drops to
    # FEL (2026-09-23 17:26: v61 written without this step -> FEL at next
    # power-on). fix-bootpkg-sum.py must sit next to this script.
    FIX="$(dirname "$0")/fix-bootpkg-sum.py"
    [ -f "$FIX" ] || die "missing $FIX -- add_sum NOT fixed, DO NOT reboot; copy it over and run: python3 $FIX $DEV --write"
    python3 "$FIX" "$DEV" --write || die "add_sum fix failed -- DO NOT reboot"
    python3 "$FIX" "$DEV" | grep -q 'PASS' || die "add_sum still invalid -- DO NOT reboot"
    echo "OK - boot package add_sum fixed and verified."
    echo "Reboot the board and watch the E902 console on /dev/ttyUSB0 (115200 8N1)."
else
    die "READ-BACK MISMATCH -- restore immediately with restore-scp.sh"
fi
