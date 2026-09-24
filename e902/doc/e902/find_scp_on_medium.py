#!/usr/bin/env python3
# Runs ON the board. Locate scp.fex (and u-boot/monitor) inside the 24 MiB head
# dump of the boot medium, by searching for known magic bytes.
# READ-ONLY: only reads /tmp/sdhead_read.img.
IMG = "/tmp/sdhead_read.img"
SCP = "/home/orangepi/e902v2/scp.fex"

d = open(IMG, "rb").read()
print("head image: %d bytes" % len(d))

# scp.fex entry: first 16 bytes are the distinctive prologue
magic = bytes.fromhex("81400141" + "81410142" + "81420143" + "81430144")
i = d.find(magic)
print("scp.fex-style prologue (81 40 01 41 ...):", "0x%X" % i if i >= 0 else "not found")

try:
    s = open(SCP, "rb").read()
    j = d.find(s[:64])
    print("scp.fex first-64 match:", "0x%X" % j if j >= 0 else "not found")
    # full match
    k = d.find(s[:4096])
    print("scp.fex first-4K match:", "0x%X" % k if k >= 0 else "not found")
except Exception as e:
    print("scp.fex read failed:", e)

# u-boot / monitor: look for "U-Boot" and "monitor" strings
for pat in (b"U-Boot 20", b"eGON.BT0", b"sunxi", b"monitor"):
    p = d.find(pat)
    print("%-12s first at 0x%X" % (pat.decode(), p) if p >= 0 else "%-12s not found" % pat.decode())

# 256-byte aligned scan for other eGON TOC entries
import struct
for off in range(0, 0x40000, 0x200):
    if d[off:off+8] == b"eGON.BT0":
        print("eGON.BT0 header at 0x%X" % off)
