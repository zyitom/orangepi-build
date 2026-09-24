#!/usr/bin/env python3
# Runs ON the board (root). Read-only scan of SRAM_A2.
#
# Rationale: the vendor SCP zeroes its own .bss (0x4001DDB8..0x4002AF3C) and a
# stack-adjacent area (0x4002EC00..0x4002F000) at startup. So any NON-ZERO
# content in SRAM_A2 that is not part of the loaded image (0x40004000..0x4001DDB8)
# was written at runtime by either bl31 or the SCP -- which is exactly where a
# handshake flag / parameter block would live.
import sys, os
sys.path.insert(0, "/home/orangepi/e902v2")
import importlib.util
spec = importlib.util.spec_from_file_location("awdevmem", "/home/orangepi/e902v2/awdevmem.py")
aw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(aw)

IMG_START, IMG_END = 0x40004000, 0x4001DDB8

def scan(e902_addr, length, label):
    arm = aw.e902_to_arm(e902_addr)
    with aw.Mem(arm, length) as m:
        data = m.read(length)
    print("=== %s : E902 0x%08X..0x%08X (%d bytes) ===" % (label, e902_addr, e902_addr + length - 1, length))
    runs = []
    i = 0
    while i < len(data):
        if data[i] != 0:
            j = i
            while j < len(data) and data[j] != 0:
                j += 1
            runs.append((i, j))
            i = j
        else:
            i += 1
    if not runs:
        print("  (all zero)")
        return
    print("  non-zero runs: %d, total %d bytes" % (len(runs), sum(b - a for a, b in runs)))
    for a, b in runs[:60]:
        chunk = data[a:min(b, a + 32)]
        print("  +0x%05X (%08X) len=%4d : %s" % (a, e902_addr + a, b - a,
              " ".join("%02x" % c for c in chunk)))

def main():
    # 1. below the vendor image (16K, unused by the image)
    scan(0x40000000, 0x4000, "SRAM_A2 below vendor image")
    # 2. from just under the vendor bss end up to the top of SRAM_A2
    scan(0x4002A000, 0x40034000 - 0x4002A000, "SRAM_A2 upper half")
    # 3. the vendor image tail region check (should look like image data)
    print()
    print("=== sanity: image head at 0x40004000 ===")
    with aw.Mem(aw.e902_to_arm(0x40004000), 16) as m:
        print("  " + " ".join("%02x" % c for c in m.read(16)))

main()
