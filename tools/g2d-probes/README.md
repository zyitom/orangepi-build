# G2D probes

Throwaway, single-purpose test programs used to pin down how `g2d_sunxi` (A733 /
sun60iw2, `g2d version 0x10112114`) actually behaves. The supported, self-checking
tool is `../g2d_test.cpp`; these are the probes that produced the evidence behind
`../../analysis/g2d/REPORT.md` and they are kept so the claims can be re-derived.

All of them use two dedicated `/dev/dma_heap/system` buffers, `G2D_BLT_NONE_H`,
`bbuff = 1`, `use_phy_addr = 0`, and the correct DMA-BUF sync order. The sync
itself lives in the shared header `dmabuf_sync.h`: it negotiates the
`DMA_BUF_IOCTL_SYNC` flag encoding at runtime (this vendor kernel uses the
*legacy* layout, mainline 6.12+ the other one — hard-coding the wrong one made
every sync fail with EINVAL and, with the return value ignored, silently do
nothing; see `../../analysis/round9/REPORT.md`) and treats every failure as
fatal. They run as a plain user in the `video` group (no sudo).

## How to build and run

The board is glibc 2.31 while the aarch64 cross toolchain on TL101 is glibc 2.34,
so build **natively on the board**; the headers come from `/tmp/g2dbuild`:

    tools/put_board.sh tools/g2d-probes/dmabuf_sync.h /tmp/g2dbuild/
    tools/put_board.sh tools/g2d-probes/g2d_mark.c /tmp/g2dbuild/
    tools/ssh_board.sh "cd /tmp/g2dbuild && gcc -O2 -o /tmp/g2d_mark g2d_mark.c && /tmp/g2d_mark"

(`g2d_test.cpp` needs `g++ -O2 -std=c++20 -I. -o /tmp/g2d_test g2d_test.cpp v4l2.cpp -lpthread`.)

## This round (2026-09-16, round 8) — the ones that settled it

| file | question | answer |
|---|---|---|
| `g2d_mark.c` | plant **one** impulse per run and read back every destination byte that changed: is the chroma plane an address remap, a filter, or a swap? | a **normalised multi-tap filter** (footprint -2..+4 bytes = 2 chroma pairs, negative lobe, phase 0, exact on constant chroma). Also proves the chroma row pitch really is 1920 B: an impulse at plane offset 1920 lands at destination offset 0. Y comes back as a single exact byte. |
| `g2d_mech.c` | D: where exactly does the filter change bytes? E: is a `G2D_FORMAT_Y8` blit bit-exact? F/G: do the two documented ARGB↔NV12 directions work? | D: **every** differing byte is within 4 B of a chroma transition, none in a flat region, maxdelta scales with the local step. E: Y8 is **bit-exact (0/2073600)** and applies no range clamp. F/G: conversion works, see `g2d_order.c` for which enum is which. |
| `g2d_order.c` | is `0x28 G2D_FORMAT_YUV420UVC_V1U1V0U0` or `0x29 ..._U1V1U0V0` the real NV12? | **0x28 is NV12 (U first); 0x29 is NV21 (V first)** — proved with red/blue constants in both directions. The enum does not affect how much the chroma is filtered. |
| `g2d_work.c` | K: is there a bit-exact way to move NV12 with G2D? L: how do you fill a rectangle on NV12? | K: **yes — two `G2D_FORMAT_Y8` blits** over a 1920x1620 view of the same buffer, `clip_rect.y = 0/h = 1080` for luma and `y = 1080/h = 540` for chroma: **0/3110400 on full-range random data**. L: `G2D_CMD_FILLRECT_H` with `dst_image_h.color = (Y<<16)|(U<<8)|V`, written verbatim, clipped correctly. |

## Round 7 (kept for the record)

These produced the numbers that round 8 then had to reinterpret (`g2d_map.c`'s result
in particular was misleading: the pattern had a 256-byte period and was sampled with a
256-byte stride, so every sample hit the same phase).

| file | what it did |
|---|---|
| `g2d_selftest.c` | version query, constant fill, plain copy, rotation |
| `g2d_diag.c` | CPU-memcpy control plus per-plane localisation of the differences |
| `g2d_hyp.c` | alpha / range-clamp hypothesis tests |
| `g2d_chroma.c` | constant fill with flag comparison (`bbuff`, `G2D_BLT_COPYPEN`) |
| `g2d_map.c` | self-encoding ramp (`src[i] = i & 0xff`) to localise a chroma offset — **superseded by `g2d_mark.c`**, its ramp aliases at 256 |
| `g2d_fmt.c` | the three 4:2:0 constants (`0x28` / `0x29` / `0x2a`) side by side |
| `g2d_scan.c` | flag/format scan |
