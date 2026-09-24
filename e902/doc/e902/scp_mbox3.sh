#!/bin/bash
D=/tmp/scp.dis
BIN=/home/helios/Desktop/orangepi-build/u-boot/v2018.05-sun60iw2/scp.fex

python3 - "$BIN" > /tmp/scp_strtab.txt <<'PY'
import sys, re
b=open(sys.argv[1],'rb').read()
BASE=0x40004000
want = [b"startup feedback ok", b"feedback startup result", b"send feedback message",
        b"loopback message request", b"send asyn message", b"feedback hard syn message",
        b"message manager ok", b"hwmsgbox driver ok", b"debugger system ok"]
for w in want:
    i = 0
    while True:
        i = b.find(w, i)
        if i < 0: break
        print("0x%08X  %s" % (BASE + i, w.decode()))
        i += 1
PY
cat /tmp/scp_strtab.txt

echo
echo "===== code references to those string addresses ====="
for a in $(awk '{print $1}' /tmp/scp_strtab.txt | sort -u); do
    hits=$(grep -nE "# $a\$|# 0x0*${a#0x}\$" "$D" | head -3)
    if [ -n "$hits" ]; then
        echo "--- refs to $a ---"
        echo "$hits"
    fi
done

echo
echo "===== dump the function around 'startup feedback ok' refs ====="
for n in $(grep -nE 'startup|feedback' /tmp/scp_strtab.txt | awk '{print $1}' | head -3); do :; done
REF=$(grep -nE "# 0x4000[Ff][0-9a-fA-F]{3}" "$D" | grep -iE 'addi' | head -0)
grep -n -B12 -A20 'feedback' /dev/null 2>/dev/null
# print the region that logs 'startup feedback ok'
python3 - "$BIN" <<'PY' > /tmp/scp_sf_addr.txt
b=open('/home/helios/Desktop/orangepi-build/u-boot/v2018.05-sun60iw2/scp.fex','rb').read()
BASE=0x40004000
w=b"startup feedback ok"
i=b.find(w)
print("0x%08X" % (BASE+i))
PY
SF=$(cat /tmp/scp_sf_addr.txt)
echo "string addr: $SF"
grep -n -B6 -A6 "$SF" "$D" | head -40
