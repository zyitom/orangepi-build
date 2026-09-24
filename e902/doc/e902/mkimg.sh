#!/bin/bash
# Make a copy of the existing full image with OUR fixed E902 firmware written
# into the scp slot, so one flash both recovers the board and tests the fix.
set -eu
cd /home/helios/Desktop/orangepi-build/output/images/Orangepizero3w_1.0.0_debian_bookworm_minimal_linux6.6.98/
SRC=Orangepizero3w_1.0.0_debian_bookworm_minimal_linux6.6.98.img
DST=Orangepizero3w_1.0.0_debian_bookworm_minimal_linux6.6.98-e902fw.img
FW=/home/helios/Desktop/orangepi-build/e902/fw-out/scp-ours-padded-105912.bin
OFF=$((0x113BC00))
SIZE=105912

echo "=== free space ==="
df -h . | tail -1
echo "=== source ==="
ls -la "$SRC"
echo "fw sha256 : $(sha256sum "$FW" | cut -d' ' -f1)"

echo
echo "=== copy ==="
cp -f "$SRC" "$DST"
ls -la "$DST"

echo
echo "=== write our firmware into the scp slot of the copy ==="
dd if="$FW" of="$DST" bs=1 seek=$OFF count=$SIZE conv=notrunc status=none
sync
echo -n "slot in copy : "; dd if="$DST" bs=1 skip=$OFF count=$SIZE status=none | sha256sum | awk '{print $1}'
echo    "expected     : fb1d809d1fd6b5e9ee7553050f1d0569e45c3a4ceb4186026758c20875a38c26"

echo
echo "=== sanity: everything else identical to the source (outside the slot) ==="
echo -n "head 0..0x113BC00 sha256 (src): "; dd if="$SRC" bs=1 count=$OFF status=none | sha256sum | awk '{print $1}'
echo -n "head 0..0x113BC00 sha256 (dst): "; dd if="$DST" bs=1 count=$OFF status=none | sha256sum | awk '{print $1}'

echo
echo "=== write the .sha file ==="
sha256sum "$DST" | sed 's# .*/# *#' > "$DST.sha"
cat "$DST.sha"
ls -la "$DST" "$DST.sha"
