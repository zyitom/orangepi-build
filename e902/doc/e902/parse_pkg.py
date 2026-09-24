#!/usr/bin/env python3
# (on TL101) Parse the sunxi boot-package TOC at 0x1004000.
import struct
IMG = "/home/helios/Desktop/orangepi-build/e902/backup/sd-boot-head-20260922.img"
d = open(IMG, "rb").read()
BASE = 0x1004000

print("=== raw 0x200 bytes at the package header 0x%X ===" % BASE)
for off in range(BASE, BASE + 0x200, 16):
    print("  %08X  %s  |%s|" % (off,
        " ".join("%02x" % b for b in d[off:off+16]),
        "".join(chr(b) if 32 <= b < 127 else "." for b in d[off:off+16])))

print()
print("=== try to walk the TOC (item entries of 16/32 bytes with name+offsets) ===")
# entries of the eGON package TOC are typically: char name[16]; u32 offset; u32 size; u32 reserved/checksum
for rec in range(0, 8):
    for step in (16, 32, 64):
        off = BASE + 0x20 + rec * step
        rec_d = d[off:off+step]
        name = rec_d[:16].split(b"\x00")[0]
        if not name:
            continue
        try:
            vals = struct.unpack("<" + "I" * ((step - 16) // 4), rec_d[16:])
            print("  off 0x%X step %2d  name=%-16s vals=%s" % (off, step, name.decode(errors="replace"),
                  " ".join("0x%X" % v for v in vals)))
        except Exception as e:
            pass

print()
print("=== TOC-ish search inside the first 4 KiB of the package ===")
for pat in (b"u-boot", b"monitor", b"scp", b"optee", b"dtb", b"name"):
    i = d.find(pat, BASE, BASE + 0x1000)
    print("  %-10s at %s" % (pat.decode(), ("0x%X" % i) if i >= 0 else "not found"))

print()
print("=== absolute offsets seen in that window (little endian sanity) ===")
for off in range(BASE, BASE + 0x100, 4):
    v = struct.unpack_from("<I", d, off)[0]
    if 0x1000000 < v < 0x1800000 or 0x1000 < v < 0x1200000:
        print("  0x%X -> 0x%X" % (off, v))
