#!/usr/bin/env python3
# (on TL101) Decode the three package TOC entries precisely.
import struct
IMG = "/home/helios/Desktop/orangepi-build/e902/backup/sd-boot-head-20260922.img"
d = open(IMG, "rb").read()
BASE = 0x1004000

def show(off, n, label):
    print("--- %s @0x%X ---" % (label, off))
    for o in range(off, off + n, 16):
        print("  %08X  %s  |%s|" % (o,
            " ".join("%02x" % b for b in d[o:o+16]),
            "".join(chr(b) if 32 <= b < 127 else "." for b in d[o:o+16])))

show(BASE, 0x40, "package header")
# entries: marker "MIE;" at +0x3C, name at +0x40, then 0x40 bytes of fields at +0x80
for idx, (marker_off, name) in enumerate([(0x3C, "u-boot"), (0x1AC, "monitor"), (0x31C, "scp")]):
    name_off = BASE + 0x40 + idx * 0x170
    field_off = name_off + 0x40
    show(BASE + marker_off, 0x08, "marker %d" % idx)
    nm = d[name_off:name_off+16].split(b"\x00")[0]
    fields = struct.unpack_from("<IIIII", d, field_off)
    print("  entry%d name=%-10s fields(off,size,?,type,?) = 0x%X 0x%X 0x%X 0x%X 0x%X"
          % (idx, nm.decode(errors="replace"), *fields))
    # both interpretations
    print("     if REL: abs=0x%X .. 0x%X" % (BASE + fields[0], BASE + fields[0] + fields[1]))
    print("     if ABS:     0x%X .. 0x%X" % (fields[0], fields[0] + fields[1]))

print()
print("=== sanity: what is at the two candidate scp positions? ===")
for pos, label in ((0x113BC00, "found by search"), (BASE + 0x13BC00, "BASE+rel"), (0x13BC00, "abs 0x13BC00")):
    print("  %-22s 0x%08X : %s" % (label, pos, " ".join("%02x" % b for b in d[pos:pos+8])))
