#!/bin/sh
# Roll the "scp" item in the boot package back to the vendor image.
#
# Usage (ON THE BOARD, as root):
#   sh restore-scp.sh [vendor-scp.bin] [/dev/mmcblk1]
#
# Defaults to the hash-verified on-medium copy taken 2026-09-22:
#   e902/backup/scp.fex.on-medium.bin
#   sha256 07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749
set -eu

SRC="${1:-/home/orangepi/e902v2/scp.fex}"
DEV="${2:-/dev/mmcblk1}"
OFF=$((0x113BC00))
SIZE=105912
EXPECT=07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749

die() { echo "error: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "must run as root"
[ -f "$SRC" ] || die "vendor image not found: $SRC (pass it as arg 1)"
[ -b "$DEV" ] || die "$DEV is not a block device"

N=$(stat -c%s "$SRC")
[ "$N" = "$SIZE" ] || die "$SRC is $N bytes, expected $SIZE"

got=$(sha256sum "$SRC" | cut -d' ' -f1)
echo "source : $SRC"
echo "sha256 : $got"
if [ "$got" = "$EXPECT" ]; then
    echo "         (matches the known-good vendor image)"
else
    echo "         WARNING: does not match the recorded vendor hash $EXPECT"
    read -r -p "Continue anyway? Type YES: " a
    [ "$a" = YES ] || die "aborted"
fi

dd if="$SRC" of="$DEV" bs=1 seek="$OFF" count="$SIZE" conv=notrunc status=none

have=$(dd if="$DEV" bs=1 skip="$OFF" count="$SIZE" status=none | sha256sum | cut -d' ' -f1)
echo "read  : $have"
[ "$have" = "$got" ] && echo "OK - vendor scp restored and verified." || die "READ-BACK MISMATCH"
# put back the vendor add_sum too, or boot0 rejects the package -> FEL
FIX="$(dirname "$0")/fix-bootpkg-sum.py"
[ -f "$FIX" ] || die "missing $FIX -- add_sum NOT restored, DO NOT reboot"
python3 "$FIX" "$DEV" --write --vendor || die "add_sum restore failed -- DO NOT reboot"
python3 "$FIX" "$DEV" | grep -q 'PASS' || die "add_sum still invalid -- DO NOT reboot"
echo "OK - vendor add_sum restored and verified."
