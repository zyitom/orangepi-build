#!/bin/bash
# install-scp-into-build.sh -- ON TL101.
#
# Make the A733 build produce a bootloader package that contains OUR E902
# firmware, so that any future build/flash of the bootloader (or a fresh image)
# brings the little core up as ours -- instead of silently reverting to the
# vendor scp.fex.
#
# Where it plugs in (see scripts/pack-uboot.sh):
#     update_scp scp.fex sunxi.fex          # scp.fex is taken from the u-boot tree
#     dragonsecboot -pack boot_package.cfg  # items: u-boot / monitor / scp
# so replacing  u-boot/v2018.05-sun60iw2/scp.fex  is exactly the right lever.
set -eu

ROOT=/home/helios/Desktop/orangepi-build
E=$ROOT/e902
UB=$ROOT/u-boot/v2018.05-sun60iw2
SRC=$E/fw-out/scp-ours-padded-105912.bin
BAK=$E/backup/scp.fex.vendor-bak

[ -f "$SRC" ] || { echo "missing $SRC (run: cd e902-fw && make scpfw)"; exit 1; }
sz=$(stat -c%s "$SRC"); [ "$sz" = 105912 ] || { echo "payload must be 105912 bytes (is $sz)"; exit 1; }

if [ ! -f "$BAK" ]; then
    cp -a "$UB/scp.fex" "$BAK"
    echo "backed up vendor scp.fex -> $BAK"
else
    echo "vendor backup already present: $BAK"
fi

echo "before : $(sha256sum "$UB/scp.fex" | cut -d' ' -f1)  ($(stat -c%s "$UB/scp.fex") bytes)"
cp -f "$SRC" "$UB/scp.fex"
echo "after  : $(sha256sum "$UB/scp.fex" | cut -d' ' -f1)  ($(stat -c%s "$UB/scp.fex") bytes)"

echo
echo "Now any bootloader build will package OUR scp. To produce and install it:"
echo "  cd $ROOT"
echo "  sudo ./build.sh BOARD=orangepizero3w BRANCH=current BUILD_OPT=u-boot"
echo "  # then flash the resulting bootloader package, or write the package"
echo "  # region directly (see doc/e902/FLASH-SPEC.md)."
echo
echo "Revert with: bash e902/restore-scp-in-build.sh"
