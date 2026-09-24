#!/usr/bin/env python3
"""Compare two libisp parameter blobs and localise every difference.

Used to answer "do isp_param_3dnr.bin and isp_param_no3dnr.bin really differ in
the ISP module enables, or only in a description string?".
"""
import sys

a = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()

print(f"{sys.argv[1]}: {len(a)} bytes")
print(f"{sys.argv[2]}: {len(b)} bytes")
if len(a) != len(b):
    print("different lengths -> cannot align")
    sys.exit(1)

diffs = [i for i in range(len(a)) if a[i] != b[i]]
print(f"differing bytes: {len(diffs)}")
if diffs:
    print(f"offsets: {diffs[0]}..{diffs[-1]}")
    for i in diffs:
        print(f"  0x{i:06x} ({i:7d})  {a[i]:02x} -> {b[i]:02x}")

    # printable context
    lo, hi = max(0, diffs[0] - 16), min(len(a), diffs[-1] + 17)
    print(f"\ncontext 0x{lo:x}..0x{hi:x}")
    print("  A:", a[lo:hi])
    print("  B:", b[lo:hi])
else:
    print("identical payload")

# where does the well known MODULE_BYPASS1 value live?
for name, data in (("A", a), ("B", b)):
    for pat in (bytes.fromhex("fef9073d"), bytes.fromhex("fef9073d".upper())):
        off = data.find(pat)
        if off >= 0:
            print(f"{name}: little-endian 0x3d07f9fe at offset {off}")
