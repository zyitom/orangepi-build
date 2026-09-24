#!/usr/bin/env python3
# fix-bootpkg-sum.py -- fix the toc1 add_sum field of a sunxi boot package
# after the "scp" item content has been replaced.
#
# Root cause (2026-09-22): boot0 validates the whole boot package with
#   sunxi_generate_checksum(buf, valid_len, 1, stored)
#   = LE-u32-word-sum(buf[0..valid_len)) - stored + STAMP_VALUE(0x5F0A6C39)
# and requires the result to equal `stored`. Replacing item content without
# updating add_sum => "error:bad checksum / error:bad magic" at boot0.
# Evidence: u-boot board/sunxi/board_common.c:846, tools/mksunxiboot.c:19
# (STAMP_VALUE), sprite/sprite_download.c:177 (packer recomputes add_sum);
# algorithm re-derived and verified against the vendor package on this card.
#
# Usage:
#   python3 fix-bootpkg-sum.py <device-or-image>            # check only
#   python3 fix-bootpkg-sum.py <device-or-image> --write    # apply fix
#   python3 fix-bootpkg-sum.py <device-or-image> --write --vendor  # restore vendor add_sum value
import sys, struct

STAMP = 0x5F0A6C39
PKG_OFF = 0x1004000
MAGIC = 0x89119800
VENDOR_SUM = 0xA91DDF89

def words_sum(data):
    n = len(data) // 4
    return sum(struct.unpack_from('<%dI' % n, data, 0)) & 0xFFFFFFFF

def boot0_verify(buf, valid_len, stored):
    return (words_sum(buf[:valid_len]) - stored + STAMP) & 0xFFFFFFFF

def main():
    path = sys.argv[1]
    write = '--write' in sys.argv
    with open(path, 'r+b' if write else 'rb') as f:
        f.seek(PKG_OFF)
        head = f.read(0x40)
        magic, add_sum = struct.unpack_from('<II', head, 0x10)
        items_nr, valid_len = struct.unpack_from('<II', head, 0x20)
        print(f'pkg head : magic={magic:08x} add_sum={add_sum:08x} items={items_nr} valid_len={valid_len:#x}')
        assert magic == MAGIC, 'not a sunxi-package (magic mismatch)'
        f.seek(PKG_OFF)
        pkg = f.read(valid_len)
        W = words_sum(pkg)
        v = boot0_verify(pkg, valid_len, add_sum)
        ok = (v == add_sum)
        print(f'words_sum W = {W:08x}   boot0_verify(W, stored) = {v:08x}   -> {"PASS (checksum valid)" if ok else "FAIL (this is why boot0 refuses the package)"}')
        # The add_sum field is INSIDE the summed region, so W(s) = W0 + s and
        # boot0_verify(s) = W0 + STAMP is independent of s. The unique fix is
        # therefore s = boot0_verify(current state) -- i.e. the value boot0
        # would compute right now.
        s = v
        print(f'required add_sum for current content = {s:08x}')
        if ok:
            print('nothing to do: checksum already valid.')
            return 0
        if not write:
            print('DRY RUN: re-run with --write to patch 4 bytes at 0x%08x' % (PKG_OFF + 0x14))
            return 1
        if '--vendor' in sys.argv:
            s = VENDOR_SUM
            print(f'--vendor: writing vendor add_sum {s:08x}')
        f.seek(PKG_OFF + 0x14)
        f.write(struct.pack('<I', s))
        f.flush()
        # read back & verify
        f.seek(PKG_OFF)
        pkg2 = f.read(valid_len)
        v2 = boot0_verify(pkg2, valid_len, s)
        print(f'write done: add_sum={s:08x}, readback verify = {"PASS" if v2 == s else "FAIL"}')
        return 0 if v2 == s else 2

if __name__ == '__main__':
    sys.exit(main())
