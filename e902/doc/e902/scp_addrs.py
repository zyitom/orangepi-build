#!/usr/bin/env python3
# Static analysis of the vendor scp.fex disassembly.
#
# objdump emits the resolved target address as a "# 0x...." comment for
# auipc+addi / lui+addi pairs. Collect every such address, classify by region,
# and print the ones that are NOT (a) inside the SCP's own image+ram nor
# (b) a CPUS-domain peripheral -- those are its "outside world" accesses, the
# set that must contain the parameter block and any handshake flag.
import re, collections, sys

DIS = "/tmp/scp.dis"
IMG_LO, IMG_HI = 0x40004000, 0x4002F000          # own image + bss + stack
text = open(DIS, encoding="utf-8", errors="replace").read()

addr_re = re.compile(r"#\s*0x([0-9a-fA-F]+)")
found = collections.Counter()
for m in addr_re.finditer(text):
    found[int(m.group(1), 16)] += 1

def region(a):
    if IMG_LO <= a < IMG_HI:            return "SELF"
    if 0x07000000 <= a < 0x070A0000:    return "CPUS-REGS"
    if 0x03000000 <= a < 0x03100000:    return "CPUX-REGS"
    if 0x02000000 <= a < 0x02030000:    return "MAIN-CCU"
    if 0x00000000 <= a < 0x00080000:    return "LOW/DRAM"
    if 0x40000000 <= a < 0x40040000:    return "SRAM-OTHER"
    if a < 0x00010000:                  return "TINY"
    return "OTHER"

groups = collections.defaultdict(list)
for a, n in found.items():
    groups[region(a)].append((a, n))

print("=== resolved-address comment histogram by region ===")
for r in sorted(groups, key=lambda k: -sum(n for _, n in groups[k])):
    tot = sum(n for _, n in groups[r])
    print("  %-12s distinct=%-5d refs=%-6d" % (r, len(groups[r]), tot))

print()
print("=== SELF range detail (what part of its own memory it touches) ===")
selfs = sorted(groups.get("SELF", []))
def bucket(a):
    if a < 0x40004000: return "?"
    if a < 0x4001DDB8: return "image (code/rodata/data)"
    if a < 0x4002AF3C: return "bss"
    if a < 0x4002EC00: return "bss..stack gap"
    if a < 0x4002F000: return "stack-adjacent"
    return "above stack"
b = collections.Counter()
for a, n in selfs:
    b[bucket(a)] += n
for k, v in b.most_common():
    print("  %-26s refs=%d" % (k, v))

print()
print("=== NON-SELF addresses (its outside-world accesses) ===")
for r in ("CPUS-REGS", "CPUX-REGS", "MAIN-CCU", "LOW/DRAM", "SRAM-OTHER", "TINY", "OTHER"):
    lst = sorted(groups.get(r, []))
    if not lst:
        continue
    print("--- %s (%d distinct) ---" % (r, len(lst)))
    for a, n in lst[:40]:
        print("    0x%08X  refs=%d" % (a, n))
