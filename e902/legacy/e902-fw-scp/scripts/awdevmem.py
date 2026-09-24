#!/usr/bin/env python3
"""mmap-based peek/poke and SRAM image loader for the A733, in stdlib python3.

Replaces the devmem2 / busybox-devmem dependency: neither ships on the
Orange Pi Zero 3W image, and neither can safely write SRAM_A2 anyway --
`dd` on /dev/mem uses lseek+write, which the kernel refuses for
non-System-RAM ("Bad address"), and *accepts* for System RAM, which is
exactly the wrong outcome (see ADDRESS TRANSLATION below).

  awdevmem.py read  0x0701021C
  awdevmem.py write 0x0701021C 0x00010003
  awdevmem.py load  fw.bin --e902 0x40014000
  awdevmem.py dump  --e902 0x40014000 --count 64

ADDRESS TRANSLATION
-------------------
SRAM_A2 is visible at two different addresses, and the number 0x40000000
means something different on each side of the SoC:

  E902 view : SRAM_A2 at 0x40000000..0x40033FFF   (A733 manual ch.2)
  ARM  view : SRAM_A2 at 0x00040000               (u-boot
              arch/arm/include/asm/arch-sunxi/plat-sun60iw2p1/cpu_autogen.h:5
              SUNXI_SRAM_A2_BASE)
  ARM  view : 0x40000000 is the base of DRAM      (/proc/iomem: System RAM)

So an ARM-side write to 0x40014000 does not reach the E902 at all -- it
lands in live, unreserved kernel memory. With CONFIG_STRICT_DEVMEM off
(it is off on this image) the kernel permits it, so the write silently
corrupts whatever pages happen to be there. This tool always converts:

  ARM addr = 0x00040000 + (E902 addr - 0x40000000)

Pass SRAM addresses as --e902 (E902 view, matching fw.ld and the manual);
raw MMIO register addresses are the same from both sides and need no
conversion.
"""
import argparse
import mmap
import os
import sys

SRAM_A2_E902 = 0x40000000       # E902 view (manual ch.2)
SRAM_A2_ARM = 0x00040000        # ARM view (SUNXI_SRAM_A2_BASE)
SRAM_A2_SIZE = 0x00034000       # 208K

PAGE = os.sysconf("SC_PAGE_SIZE")


def e902_to_arm(addr):
    """Translate an E902-view SRAM_A2 address to the ARM-side alias."""
    if not (SRAM_A2_E902 <= addr < SRAM_A2_E902 + SRAM_A2_SIZE):
        raise SystemExit(
            "0x%08X is not in SRAM_A2 in the E902 view (0x%08X..0x%08X).\n"
            "Refusing to translate: if you meant a DRAM address, this tool "
            "is the wrong one." % (addr, SRAM_A2_E902,
                                   SRAM_A2_E902 + SRAM_A2_SIZE - 1))
    return SRAM_A2_ARM + (addr - SRAM_A2_E902)


class Mem:
    """A window onto /dev/mem, page-aligned under the hood."""

    def __init__(self, addr, length, write=False):
        self.base = addr & ~(PAGE - 1)
        self.off = addr - self.base
        self.len = ((self.off + length + PAGE - 1) // PAGE) * PAGE
        flags = os.O_RDWR | os.O_SYNC if write else os.O_RDONLY | os.O_SYNC
        self.fd = os.open("/dev/mem", flags)
        prot = mmap.PROT_READ | (mmap.PROT_WRITE if write else 0)
        try:
            self.m = mmap.mmap(self.fd, self.len, mmap.MAP_SHARED, prot,
                               offset=self.base)
        except OSError as e:
            os.close(self.fd)
            raise SystemExit("mmap 0x%08X (+0x%X) failed: %s"
                             % (self.base, self.len, e))

    def r32(self, delta=0):
        return int.from_bytes(
            self.m[self.off + delta:self.off + delta + 4], "little")

    def w32(self, value, delta=0):
        self.m[self.off + delta:self.off + delta + 4] = \
            (value & 0xFFFFFFFF).to_bytes(4, "little")

    # SRAM_A2 is mapped as device memory, which does not tolerate every
    # access width/alignment memcpy might pick: a plain byte-slice copy
    # SIGBUSes unless the length happens to be 16-byte aligned (measured on
    # this board -- 0x19DB8 and 0x19DB4 bus-error, 0x19DB0 and 0x19DC0 do
    # not). Word-at-a-time access always works, so do that and handle the
    # sub-word tail one byte at a time.
    def write(self, data):
        mv = memoryview(self.m)
        n4 = len(data) & ~3
        for i in range(0, n4, 4):
            mv[self.off + i:self.off + i + 4] = data[i:i + 4]
        for i in range(n4, len(data)):
            mv[self.off + i:self.off + i + 1] = data[i:i + 1]

    def read(self, n):
        mv = memoryview(self.m)
        out = bytearray(n)
        n4 = n & ~3
        for i in range(0, n4, 4):
            out[i:i + 4] = mv[self.off + i:self.off + i + 4]
        for i in range(n4, n):
            out[i:i + 1] = mv[self.off + i:self.off + i + 1]
        return bytes(out)

    def close(self):
        # msync() on a /dev/mem mapping returns EINVAL -- there is no
        # page cache behind it to write back. The mapping is uncached
        # device memory (opened O_SYNC), so stores are already visible;
        # nothing is lost by skipping the flush.
        try:
            self.m.flush()
        except OSError:
            pass
        self.m.close()
        os.close(self.fd)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


def anyint(s):
    return int(s, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("read", help="read one 32-bit register")
    p.add_argument("addr", type=anyint)

    p = sub.add_parser("write", help="write one 32-bit register")
    p.add_argument("addr", type=anyint)
    p.add_argument("value", type=anyint)

    p = sub.add_parser("load", help="copy an image into SRAM_A2")
    p.add_argument("image")
    p.add_argument("--e902", type=anyint, required=True,
                   help="destination in the E902 address view, e.g. 0x40014000")
    p.add_argument("--verify", action="store_true",
                   help="read the region back and compare")

    p = sub.add_parser("dump", help="hexdump from SRAM_A2")
    p.add_argument("--e902", type=anyint, required=True)
    p.add_argument("--count", type=anyint, default=64)

    p = sub.add_parser("save", help="copy SRAM_A2 contents out to a file")
    p.add_argument("--e902", type=anyint, required=True)
    p.add_argument("--count", type=anyint, required=True)
    p.add_argument("-o", "--out", required=True)

    a = ap.parse_args()

    if os.geteuid() != 0:
        raise SystemExit("must run as root")

    if a.cmd == "read":
        with Mem(a.addr, 4) as m:
            print("0x%08X: 0x%08X" % (a.addr, m.r32()))

    elif a.cmd == "write":
        with Mem(a.addr, 4, write=True) as m:
            m.w32(a.value)
            print("0x%08X: 0x%08X" % (a.addr, m.r32()))

    elif a.cmd == "load":
        arm = e902_to_arm(a.e902)
        with open(a.image, "rb") as f:
            data = f.read()
        room = SRAM_A2_E902 + SRAM_A2_SIZE - a.e902
        if len(data) > room:
            raise SystemExit("image is %d bytes, only %d bytes of SRAM_A2 "
                             "above 0x%08X" % (len(data), room, a.e902))
        print("%s: %d bytes" % (a.image, len(data)))
        print("E902 0x%08X  ->  ARM 0x%08X" % (a.e902, arm))
        with Mem(arm, len(data), write=True) as m:
            m.write(data)
            back = m.read(len(data))
        if back != data:
            bad = next(i for i in range(len(data)) if back[i] != data[i])
            raise SystemExit("verify failed at +0x%X: wrote 0x%02X read 0x%02X"
                             % (bad, data[bad], back[bad]))
        print("verify ok (%d bytes read back identical)" % len(data))

    elif a.cmd == "dump":
        arm = e902_to_arm(a.e902)
        with Mem(arm, a.count) as m:
            data = m.read(a.count)
        for i in range(0, len(data), 16):
            row = data[i:i + 16]
            print("%08x  %-47s  |%s|" % (
                a.e902 + i,
                " ".join("%02x" % b for b in row),
                "".join(chr(b) if 32 <= b < 127 else "." for b in row)))

    elif a.cmd == "save":
        arm = e902_to_arm(a.e902)
        with Mem(arm, a.count) as m:
            data = m.read(a.count)
        with open(a.out, "wb") as f:
            f.write(data)
        print("saved %d bytes from E902 0x%08X (ARM 0x%08X) to %s"
              % (len(data), a.e902, arm, a.out))

    return 0


if __name__ == "__main__":
    sys.exit(main())
