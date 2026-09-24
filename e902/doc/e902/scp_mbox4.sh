#!/bin/bash
D=/tmp/scp.dis
BIN=/home/helios/Desktop/orangepi-build/u-boot/v2018.05-sun60iw2/scp.fex

echo "===== the startup-feedback function (0x4000b1e0 .. 0x4000b2e0) ====="
awk '/^4000b1e0:/,/^4000b2e0:/' "$D"
echo
echo "===== strings used there ====="
python3 - "$BIN" <<'PY'
import sys
b=open(sys.argv[1],'rb').read(); BASE=0x40004000
for a in (0x40010314,0x4001067c,0x4001032c,0x40010340,0x40010360,0x400101a8,0x4000f908,0x4000f920,0x400102c8):
    off=a-BASE
    if 0<=off<len(b):
        e=b.find(b'\x00',off); print("0x%08X: %r" % (a, b[off:e if 0<e<off+160 else off+120]))
PY
echo
echo "===== who calls 0x4000b1xx (the whole init function) ====="
grep -nE 'jal[[:space:]]+0x4000b1[0-9a-f]{2}' "$D" | head -10
