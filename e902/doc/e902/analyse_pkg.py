#!/usr/bin/env python3
# (runs on TL101) Analyse the 24 MiB boot-medium head to characterise the boot
# package around scp.fex: is there a TOC/checksum, and can scp.fex be swapped
# in place?
IMG = "/home/helios/Desktop/orangepi-build/e902/backup/sd-boot-head-20260922.img"
SCP_ON_MEDIA = "/home/helios/Desktop/orangepi-build/e902/backup/scp.fex.on-medium.bin"
SCP_OFF = 0x113BC00

d = open(IMG, "rb").read()
print("image size: %d bytes (0x%X)" % (len(d), len(d)))

print("\n=== bytes immediately BEFORE scp.fex (0x113BC00) ===")
for off in range(SCP_OFF - 0x80, SCP_OFF, 16):
    print("  %08X  %s  |%s|" % (off,
        " ".join("%02x" % b for b in d[off:off+16]),
        "".join(chr(b) if 32 <= b < 127 else "." for b in d[off:off+16])))

print("\n=== bytes immediately AFTER scp.fex end (0x113BC00+105912) ===")
end = SCP_OFF + 105912
for off in range(end, end + 0x80, 16):
    print("  %08X  %s  |%s|" % (off,
        " ".join("%02x" % b for b in d[off:off+16]),
        "".join(chr(b) if 32 <= b < 127 else "." for b in d[off:off+16])))

print("\n=== scan the whole head for TOC-ish headers ===")
for pat in (b"eGON.BT0", b"eGON.TOC", b"sunxi-package", b"boot_package",
            b"u-boot.fex", b"monitor.fex", b"scp.fex", b"optee.fex"):
    offs = []
    i = 0
    while len(offs) < 6:
        i = d.find(pat, i)
        if i < 0: break
        offs.append(i); i += 1
    print("  %-16s %s" % (pat.decode(), " ".join("0x%X" % o for o in offs) if offs else "not found"))

print("\n=== where does the scp payload actually start vs the file? ===")
s = open(SCP_ON_MEDIA, "rb").read()
print("  on-medium scp region size: %d" % len(s))
print("  first 16: %s" % " ".join("%02x" % b for b in s[:16]))

print("\n=== how far do the 24 MiB and the last u-boot string sit? ===")
for pat in (b"U-Boot 20", b"monitor", b"scp.fex"):
    i = d.rfind(pat)
    print("  last %-10s at 0x%X" % (pat.decode(), i))
