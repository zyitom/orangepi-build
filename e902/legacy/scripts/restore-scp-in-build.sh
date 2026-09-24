#!/bin/bash
# restore-scp-in-build.sh -- ON TL101. Put the vendor scp.fex back into the build tree.
set -eu
ROOT=/home/helios/Desktop/orangepi-build
UB=$ROOT/u-boot/v2018.05-sun60iw2
BAK=$ROOT/e902/backup/scp.fex.vendor-bak

if [ ! -f "$BAK" ]; then
    echo "no vendor backup at $BAK -- nothing to restore from"
    echo "(the original vendor file is also kept as e902/backup/scp.fex.on-medium.bin)"
    exit 1
fi
cp -f "$BAK" "$UB/scp.fex"
echo "restored vendor scp.fex into the build tree"
echo "now: $(sha256sum "$UB/scp.fex" | cut -d' ' -f1)  ($(stat -c%s "$UB/scp.fex") bytes)"
echo "expected vendor hash: 07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749"
