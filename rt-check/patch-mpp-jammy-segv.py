#!/usr/bin/env python3
"""Patch the Xunlong jammy librockchip-mpp1 1.5.0-2 SIGSEGV (RT hard-decode hang).

Root cause (full chain, verified on the board 2026-10-18):

1. MppBufferService constructor (librockchip_mpp.so.0 @0x56920) creates one
   allocator per buffer type at first use.  Its loop sets only the `type`
   argument (mov w2, w20); the `flags` argument (w3) is never initialised and
   holds whatever the preceding memset() call left in x3.  That value varies
   per process (heap ASLR), so on some processes the flags carry the
   CONTIG/CMA bit, the dma-heap allocator then opens a heap node that does
   not exist on the vendor kernel (only system / system-uncached / reserved
   exist under /dev/dma_heap), and the ION allocator creation fails silently.
2. mAllocator[ION] / mAllocatorApi[ION] stay NULL forever (no retry).
3. MppBufferService::get_group (@0x57590) copies the NULLs into the new
   group, hits mpp_assert("p->allocator") / mpp_assert("p->alloc_api") --
   which only logs (suppressed at the default log level) and CONTINUES,
   returning the half-initialised group.
4. vdpu34x_h264d_init -> mpp_buffer_get_with_tag -> mpp_buffer_create
   dereferences group->alloc_api->alloc -> SIGSEGV.
5. gst-launch-1.0's SIGSEGV handler prints "Spinning.  Please run 'gdb ...'"
   (stderr, usually discarded) and spins in g_usleep forever: the pipeline
   looks hung (this was mis-diagnosed as a kernel rkvdec INT_EN race).

Fix (same semantics as the upstream fix that landed later in
rockchip-linux/mpp, see mpp/base/mpp_buffer_impl.cpp "failed to get
allocater"): get_group must fail cleanly instead of returning a group with a
NULL allocator.  Three instructions:

  0x576c0  cbz x0, 579d8  ->  cbz x0, 5798c   (route alloc_api==NULL to the block below)
  0x5798c  mov x25, xzr                       (p = NULL)
  0x57990  b   577a8                          (function epilogue: canary check, ret x0=x25)

mpp_buffer_group_init then returns an error, hal init fails, mpp_init fails
and the pipeline errors out visibly instead of spinning forever.  The noble
images are not affected: the jjriek PPA mpp (git20240717) already contains
the upstream fix.

Usage: patch-mpp-jammy-segv.py <librockchip_mpp.so.0>   (patched in place)
       pass --check to only verify the expected bytes.
"""
import struct
import sys

PATCHES = [
    # (offset, expected old, new, description)
    (0x576C0, 0xB40018C0, 0xB4001660,
     "get_group: cbz x0, 579d8 -> cbz x0, 5798c"),
    (0x5798C, 0x52800040, 0xAA1F03F9,
     "get_group: mov x25, xzr (p = NULL)"),
    (0x57990, 0x900008A5, 0x17FFFF86,
     "get_group: b 577a8 (clean NULL return)"),
]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    path = sys.argv[1]
    check_only = "--check" in sys.argv
    with open(path, "rb") as f:
        data = bytearray(f.read())

    bad = False
    for off, old, new, desc in PATCHES:
        cur = struct.unpack_from("<I", data, off)[0]
        if cur == new:
            print(f"0x{off:x}: already patched ({desc})")
        elif cur != old:
            print(f"0x{off:x}: UNEXPECTED 0x{cur:08x}, expected 0x{old:08x} ({desc})")
            bad = True
        else:
            print(f"0x{off:x}: 0x{old:08x} -> 0x{new:08x} ({desc})")
    if bad:
        sys.exit(1)
    if check_only:
        return
    for off, old, new, _ in PATCHES:
        struct.pack_into("<I", data, off, new)
    with open(path, "wb") as f:
        f.write(data)
    print(f"patched {path}")


if __name__ == "__main__":
    main()
