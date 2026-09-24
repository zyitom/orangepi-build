#!/bin/bash
D=/tmp/scp.dis
BIN=/home/helios/Desktop/orangepi-build/u-boot/v2018.05-sun60iw2/scp.fex

echo "===== the two helpers the SCP calls with a0=3 (channel 3) ====="
awk '/^400071b2:/,/^400071ea:/' "$D" | head -30
echo "--- 0x400071ea ---"
awk '/^400071ea:/,/^40007230:/' "$D" | head -30

echo
echo "===== who calls the packet-sender at 0x400072xx (search jal targets) ====="
grep -nE 'jal[[:space:]]+0x400072(0|1|2|3|4|5|6|7|8|9|a|b|c|d|e|f)' "$D" | head -20
echo "--- callers of 0x400071b2 / 0x400071ea ---"
grep -nE 'jal[[:space:]]+0x400071(b2|ea)' "$D" | head -20

echo
echo "===== string at 0x4000f8f4 (log format) ====="
python3 - "$BIN" <<'PY'
import sys
b=open(sys.argv[1],'rb').read()
BASE=0x40004000
for a in (0x4000f8f4, 0x4000f300, 0x4000f2ec, 0x4000f310):
    off=a-BASE
    if 0<=off<len(b):
        end=b.find(b'\x00', off)
        s=b[off:end if 0<end<off+200 else off+120]
        print("0x%08X: %r" % (a, s))
PY

echo
echo "===== any obviously 'ready/hello/handshake' strings in scp.fex ====="
strings -n 6 "$BIN" | grep -iE 'ready|hello|handshake|ack|sync|start|init ok|ok$' | head -30
