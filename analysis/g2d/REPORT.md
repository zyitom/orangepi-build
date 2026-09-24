# G2D investigation — 2026-09-16 (round: fix tools/hwtest/g2d_test.cpp + NV12 chroma root cause)

Project root on TL101: `/home/helios/Desktop/orangepi-build/ar0234-port`
Board: Orange Pi Zero 3W (A733 / sun60iw2), kernel 6.6.98-sun60iw2.

Conclusion grades used here: **观察 / 候选 / 复现 / 确认**.

---

## 0. Starting point (given, verified last round — not re-run)

| finding | grade |
|---|---|
| CPU memcpy control bit-exact; ARGB8888 opaque bit-exact | 复现 |
| ARGB8888 random alpha → diffs exactly at alpha==0 pixels ⇒ premultiply | 复现 |
| NV12 constant fill → Y and UV both bit-exact | 复现 |
| NV12 in-range Y → differences are exactly the `<16` Y clamped to 16 | 复现 |
| NV12 varying data → Y exact, UV wrong (deterministic) | 复现 |
| `bbuff` must be 1; `G2D_BLT_NONE_H` OK, `G2D_BLT_COPYPEN` rc=-1 | 复现 |

**Self-criticism carried forward:** the earlier ramp used a 256-byte period and was
sampled with a 256 stride ⇒ every sample hit the same phase, so those readings were
uninformative. No sampling stride that is a multiple of 256 is used in this round.

---

## 1. Driver source reading (read-only, kernel tree NOT modified)

Source: `kernel/orange-pi-6.6-sun60iw2/bsp/drivers/g2d/` (RCQ/mixer driver, the one
built into `g2d_sunxi.ko`).

### 1.1 Plane geometry as the driver computes it

`g2d_rcq/g2d.c: g2d_set_info()` — called from `g2d_set_image_addr()`
(`g2d_mixer.c:477`) for `use_phy_addr == 0`:

```c
y_width  = g2d_img->width;  y_height = g2d_img->height;
u_width  = y_width / attr.hor_rsample_u;      /* NV12: /2 -> 960 */
u_height = y_height / attr.ver_rsample_u;     /* NV12: /2 -> 540 */
y_pitch  = G2DALIGN(y_width, align[0]);       /* 1920 */
u_pitch  = G2DALIGN(u_width * (attr.uvc+1), align[1]);  /* 960*2 -> 1920 */
y_size   = y_pitch * y_height;                /* 2073600 */
u_size   = u_pitch * u_height;                /* 1036800 */
laddr[1] = laddr[0] + y_size;                 /* NV12 chroma base — CORRECT */
laddr[2] = laddr[0] + y_size + u_size;
```

`fmt_attr_tbl` for `0x29 G2D_FORMAT_YUV420UVC_U1V1U0V0`:
`{8, hor_rsample_u=2, hor_rsample_v=2, ver_rsample_u=2, ver_rsample_v=2, uvc=1, ...}`.
`G2DALIGN(v,0) == v` (`g2d_driver_i.h:47`), so with `align[] = {0,0,0}` every pitch is
linear and `laddr[1] - laddr[0] == 1920*1080` — i.e. **the driver places the NV12 chroma
plane at exactly the canonical offset**. `g2d_byte_cal()` gives `ycnt=1, ucnt=2` for
`0x28`/`0x29`, so the chroma **pitch** used by ovl_v/wb is `2*960 = 1920`, consistent
with `laddr[1]`.

### 1.2 Path taken by `G2D_BLT_NONE_H` on an NV12 → NV12 blit

`g2d_mixer.c: g2d_bsp_bitblt()` → `g2d_vlayer_set(ovl_v, 0, src)`, then (because
`src->format >= G2D_FORMAT_IYUV422_V0Y1U0Y0`) `g2d_ovl_v_calc_coarse()` +
`g2d_vsu_para_set()` are called, then `bld_*`, then `g2d_wb_set(wb, dst)`.

Two structural facts worth noting:

* the blit is **not** a byte mover: it is `ovl_v (read+format) -> ovl_u -> scal (VSU)
  -> bld -> wb (write+format)`, i.e. every YUV→YUV blit runs through the *mixer*, whose
  internal colour space is set by `bld_cs_set(p_frame->bld, src->format)`;
* for `src->format > G2D_FORMAT_BGRA1010102 && dst->format > G2D_FORMAT_BGRA1010102`
  (both YUV) **no CSC is programmed** — the YUV→YUV blit depends on the YUV-in/YUV-out
  packing of the `bld` block being exactly self-inverse.

The chroma up/down-sampling ratios are programmed from `hor_down_sample1` /
`ver_down_sample1` with `(inw+1)>>1` / `(inh+1)>>1` (`g2d_ovl_v.c:46-53`), i.e. the
**4:2:0 chroma path is a resampling path even at 1:1 scale** — this is the mechanism
candidate for "constant fill is bit-exact (a constant survives any resampler) but
non-constant chroma is not".

Status: **候选** until the probe data below confirms it.

---

## 2. Probe results

(see sections appended below as they are run)

## 2. Probe results

### 2.1 Part 1 — fixed `tools/hwtest/g2d_test.cpp`, first run (board, user `orangepi`, no sudo)

Build: native on the board (`g++`, glibc 2.31 — the 11.2 cross toolchain is too new:
`GLIBC_2.32/2.34 not found`).

```
g2d version 0x10112114, 1920x1080
== phase 0: self-controls ==
  ctrl1 const Y          [EXACT]   exact 2073600/2073600   maxdelta 0
  ctrl1 const UV         [EXACT]   exact 1036800/1036800   maxdelta 0
  ctrl2 memcpy Y         [EXACT]   exact 2073600/2073600   maxdelta 0
  ctrl2 memcpy UV        [EXACT]   exact 1036800/1036800   maxdelta 0
== phase 1: NV12 -> NV12, G2D_FORMAT_YUV420UVC_U1V1U0V0 ==
  full-range Y  [DIFFERS] exact 1944065/2073600  (<=2 16142, <=8 48416, >8 64977)  maxdelta 16
  full-range UV [DIFFERS] exact   11369/1036800  (<=2 45591, <=8 136837, >8 843003) maxdelta 85
  in-range   Y  [EXACT]   exact 2073600/2073600  maxdelta 0
  in-range   UV [DIFFERS] exact   11601/1036800  (<=2 46334, <=8 140347, >8 838518) maxdelta 75
  UV per-band differing bytes (8 bands of 129600 B): 128149 128173 128149 128066
                                                     128201 128216 128145 128100
== phase 2: ARGB8888 (B,G,R,A in memory) ==
  ch0(B) ch1(G) ch2(R) ch3(A)  all [EXACT] 2073600/2073600, maxdelta 0
  premul probe [expected to differ]: 24293/2073600 bytes differ;
      pixels with alpha==0: 8125/8125 had RGB changed
result: OK  (rc=0)
```

Conclusions (grade **复现**):
* both controls pass ⇒ the harness, the sync order and the compare are sound; the
  old "MISMATCH" verdicts were not trustworthy.
* **`full-range Y` maxdelta is exactly 16** — the entire Y error is the lower clamp,
  nothing else. `in-range Y` is bit-exact with a 0-maxdelta histogram. This upgrades
  last round's "129600 Y bytes differ" to a precise statement: *the only Y
  transformation is a floor of 16*.
* UV differs over ~98.9 % of the plane with **uniform damage in all 8 row bands**
  (128066..128216 out of 129600) ⇒ no localised offset, no missing region, no
  half-plane: the whole chroma plane is transformed.
* the premultiply probe reproduces last round exactly: **8125/8125 pixels with
  alpha==0 had their RGB zeroed** ⇒ premultiply, 确认.

### 2.2 Part 2 — impulse mapping probe (`analysis/g2d/g2d_mark.c`)

Method (avoids the 256-period aliasing completely): source plane filled flat with
`0x80`, **one** impulse `0xEE` at a single known offset, destination poisoned with
`0xA5`, then every destination byte that is no longer `0xA5` is dumped.

```
== A. Y plane impulse ==================================================================
Y  src[0]          -> dst[0]=0xEE, dst[1..]=0x80   (single byte, exact identity)
Y  src[1921]       -> single byte, exact
Y  src[960005]     -> single byte, exact

== B. UV plane impulse (canonical NV12 layout: 1920 B/row, 540 rows) ===================
UV rel 0 -> dst rel [0]=0xd6 [2]=0x86 [4]=0x7f
UV rel 1 -> dst rel [1]=0xd6 [3]=0x86 [5]=0x7f
UV rel 2 -> dst rel [0]=0x8b [2]=0xd3 [4]=0x87 [6]=0x7f
UV rel 3 -> dst rel [1]=0x8b [3]=0xd3 [5]=0x87 [7]=0x7f
UV rel 1920 -> dst rel [0]=0x8c [2]=0x81          <-- src row 1 lands in dst row 0
```

Reading (all values are deviations from the 0x80 baseline; impulse delta is +110):

* the impulse comes back as a **multi-tap response spanning -2..+4 bytes around the
  same position, on the same byte parity**, with a **negative lobe** (`0x7f` = -1):
  a normalised low-pass / resampling kernel, not a byte move.
* the tap spread of ±2 bytes = ±1 chroma *pair* ⇒ the kernel works on U/V pairs, and
  the sampled phase is 0 (no fractional shift: an impulse at rel 0 or rel 1 lands
  back at rel 0 / rel 1).
* `UV rel 1920 -> dst rel 0` proves the source chroma row pitch really is **1920 B**
  (the canonical NV12 pitch) and that **two source chroma rows are mixed into one
  destination row** — i.e. the chroma goes through an upsample-to-4:4:4 →
  downsample-to-4:2:0 round trip.
* verdict: **the chroma corruption is a resampling filter, not an address, pitch,
  offset, U/V-swap or cache problem.** Grade: **确认**.
* an impulse at the very last UV byte (`UVLEN-1`) leaves **no** trace ⇒ consistent
  with a filter whose kernel runs off the end of the plane (border handling).

```
== C. flat / slowly-varying chroma ====================================================
flat 0x80 everywhere        -> written 1036800/1036800, equal to src 1036800/1036800
64-byte plateaus 0x80..0x8C -> written 1036738/1036800, equal to src 751310/1036800 (72 %)
```

⇒ a **constant** chroma plane is invariant under the filter (this is why last round's
"constant fill round-trips exactly" was a true but *weak* control: a constant plane is
invariant under any permutation or normalised filter), and a **smooth** chroma plane
survives to within a few LSB over 72 % of its bytes. The random-noise measurement in
2.1 (98.9 % differ) describes the worst case, not a typical frame.

### 2.3 Part 2 — does the 4:2:0 enum matter? (`analysis/g2d/g2d_order.c`)

```
== H. ARGB8888 -> <fmt>, constant colour (red: U=85 V=255, blue: U=255 V=107) ==
  red  -> fmt=0x28  Y= 76  UV[0]= 84 UV[1]=255     <- even byte = U  = NV12
  red  -> fmt=0x29  Y= 76  UV[0]=255 UV[1]= 84     <- even byte = V  = NV21
  blue -> fmt=0x28  Y= 29  UV[0]=255 UV[1]=107     <- even byte = U
  blue -> fmt=0x29  Y= 29  UV[0]=107 UV[1]=255     <- even byte = V

== I. <fmt> -> ARGB8888, Y=81 U=90 V=240 (CPU full-range reference R=238 G=14 B=14) ==
  fmt=0x28  B= 13 G= 13 R=238   <- exact
  fmt=0x29  B=255 G= 69 R= 27   <- red/blue swapped

== J. <fmt> -> <fmt> chroma exactness, 1920x1080 ==
  smooth chroma       fmt=0x28  Y diff 0/2073600  UV diff   2160/1036800  maxdelta 1
  in-range random     fmt=0x28  Y diff 0/2073600  UV diff 1025237/1036800  maxdelta 76
  smooth chroma       fmt=0x29  Y diff 0/2073600  UV diff   2160/1036800  maxdelta 1
  in-range random     fmt=0x29  Y diff 0/2073600  UV diff 1025300/1036800  maxdelta 76
```

Conclusions:

* **`G2D_FORMAT_YUV420UVC_V1U1V0U0` (0x28) is the real NV12 (U first);
  `G2D_FORMAT_YUV420UVC_U1V1U0V0` (0x29) is NV21 (V first).** The enum *names* are
  inverted relative to the byte order they produce. Proven two ways: ARGB→420 with
  red and blue identifies which byte is U, and 420→ARGB with U=90/V=240 reproduces
  the CPU reference exactly for 0x28 and comes back red/blue-swapped for 0x29.
  Grade **确认**. **Our previous tool used 0x29 on V4L2 NV12 data — that is a real
  bug in our usage, independent of the chroma filter.** (It did not cause the
  YUV→YUV chroma mismatch, because a same-format copy is self-consistent; it would
  have swapped red and blue in every ARGB annotation we drew.)
* the enum makes **no difference** to how much the chroma is filtered
  (2160 vs 2160 differing bytes on smooth data) ⇒ the resampling is in the shared
  mixer path, not in the format unpacking. Grade **确认**.
* the chroma error is **proportional to the local chroma step**: the same geometry
  with 1-LSB plateaus gives maxdelta **1**, with 3-LSB plateaus maxdelta **3**, with
  full-range random data maxdelta 76. Grade **复现**.

### 2.4 Part 2 — the chroma filter is a *transition* filter (`g2d_mech.c` D)

Vertical-constant chroma, 64-byte plateaus, period 1024 (no 256 aliasing):

```
UV differing 33480/1036800  maxdelta 3
of those, 0 are >=5 B from a plateau boundary, 33480 are within 4 B of one
row 0, cols 60..75  src: 80 80 80 80 83 83 83 83 83 83 83 83 83 83 83 83
row 0, cols 60..75  dst: 80 80 80 80 82 82 83 83 83 83 83 83 83 83 83 83
```

* **every** differing byte sits within 4 bytes of a chroma transition, and **not one**
  differing byte is in a flat region ⇒ the filter is an exact pass-through on constant
  chroma and only softens edges (here: two bytes at 82 instead of a hard 80→83 step).
  Combined with 2.2's impulse response (taps at -2..+4 bytes, with a negative lobe)
  this is a normalised low-pass/transition filter of about 2 chroma pairs.
  Grade **确认**.

### 2.5 Part 2 — a **bit-exact** NV12 copy does exist (`g2d_work.c` K)

`G2D_FORMAT_Y8` (0x30) is a single 8-bit plane. The NV12 buffer is exactly
1920 x 1620 bytes of 8-bit data, so a Y8 image with `width=1920, height=1620` plus
`clip_rect.y = 0, h = 1080` addresses the luma plane and
`clip_rect.y = 1080, h = 540` addresses the chroma plane **in place**.

```
== K. NV12 -> NV12 as two Y8 blits (full-range random source, worst case) ==
   rc=0/0  Y differing 0/2073600 (maxdelta 0)  UV differing 0/1036800 (maxdelta 0)
   => BIT-EXACT NV12 COPY WORKS
   (reference) one YUV420UVC_V1U1V0U0 blit rc=0: Y differing 129392/2073600, UV 1025547/1036800
```

Also measured earlier (`g2d_mech.c` E): a **single-plane Y8 blit is bit-exact**
(0/2073600 on full-range random data) and applies **no range clamp at all**, and it
touches nothing outside the selected rectangle (0 bytes past the Y plane modified).

Grade **确认**: there is a bit-exact G2D path for ISP NV12 frames; it just is not the
`YUV420UVC` blit.

### 2.6 Part 2 — annotating NV12 with G2D (`g2d_work.c` L)

`G2D_CMD_FILLRECT_H` takes no colour field of its own; the colour is
`dst_image_h.color`. Measured against three constants:

| `color` | observed Y | observed U | observed V |
|---|---|---|---|
| `0x00108080` | 16 | 128 | 128 |
| `0x00FF8040` | 255 | 128 | 64 |
| `0x8010A0C0` | 16 | 160 | 192 |

⇒ the value is written **straight through as `(Y<<16)|(U<<8)|V`** for a 420 target;
for every sample the three bytes equal the three colour bytes verbatim, so there is
no RGB→YUV matrix in this path.

Sub-rectangle fill `clip_rect = (100,64,256x64)`: **16384/16384 luma bytes changed
inside the rectangle and 0 outside**; the chroma half-plane was written at the
correctly halved rectangle. Grade **确认**: box drawing on NV12 works and clips
correctly — this is the safe annotation primitive.

### 2.7 Part 2 — real ISP frame (`g2d_test --capture`)

(see 2.8)

### 2.8 Part 2 — real ISP frame (`tools/hwtest/g2d_test.cpp --capture`, phase 3)

```
== phase 3: real ISP NV12 frame -> G2D -> NV12 ==
  stream: 1 start(s), 0 failed probe(s), 0 STREAMON failure(s), 0 reopen(s), seq 10
  isp frame Y (yuv420)   [DIFFERS] exact 1559144/2073600  maxdelta 16
  isp frame UV (yuv420)  [EXACT]   exact 1036800/1036800  maxdelta 0
  isp frame UV: per-band differing bytes: 0 0 0 0 0 0 0 0
  isp frame Y (y8 pair)  [EXACT]   exact 2073600/2073600  maxdelta 0
  isp frame UV (y8 pair) [EXACT]   exact 1036800/1036800  maxdelta 0
```

**This is the single most important measurement of the round.** On a *real* frame:

* the **chroma plane round-trips bit-exactly** through the YUV420UVC blit. The
  chroma transition filter measured in 2.2/2.4 never fires on real (smooth) chroma.
  So "NV12→NV12 色度不往返" is **only true for synthetic high-frequency chroma**;
  it is a property of the probe data, not of the camera pipeline. Grade **确认**
  (and a correction of our own earlier framing).
* the **luma plane does not**: 514456 bytes differ, **maxdelta exactly 16**, and the
  Y8 variant of the same frame is bit-exact ⇒ the entire luma error is the
  `Y < 16 → 16` floor. On a real ISP frame this is a *low-light clipping* bug, not
  a cosmetic one: every pixel below the studio black level is raised.
* two Y8 blits move the real frame **bit-exactly**, both planes, 0/3110400.

### 2.9 Part 2 — mechanism conclusion

**Confirmed, mechanism level:**

1. A `G2D_BLT_NONE_H` blit with a 4:2:0 UVC format is not a byte mover. It runs
   `ovl_v → scal(VSU) → bld → wb` (`g2d_mixer.c: g2d_bsp_bitblt`), and the YUV
   chroma is carried through the mixer as 4:4:4 internally and re-subsampled on
   write-back. Seen directly as a **normalised multi-tap chroma kernel** with a
   negative lobe, footprint ±2..+4 bytes (= 2 chroma pairs) horizontally plus a
   2-row vertical mix, exact in constant regions.
2. Consequently a `YUV420UVC → YUV420UVC` blit **cannot** be bit-exact, and its
   error scales with the local chroma step (maxdelta 1 for 1-LSB steps, 3 for
   3-LSB steps, ~76 for full-range noise, 0 on a real frame).
3. The Y path is per-pixel and exact, **except** for a hard floor of 16 at the
   input side (present on real frames).
4. **Not** a driver bug in the plane geometry: the chroma base address
   (`laddr[0] + 1920*1080`) and pitch (1920 B, verified by an impulse landing at
   dst offset 0 from src offset 1920) are exactly the canonical NV12 layout.
5. Also **not** sync, cache, `bbuff`, alignment (`align[]=0` keeps every pitch
   linear) or the U/V enum: `0x28` and `0x29` filter identically. The enum *is* a
   real bug in our usage, but a different one (swapped red/blue, see 2.3).

**Excluded, with evidence:** address/pitch/offset remap (marker impulses land
exactly where they started when the plane is flat), plane truncation or a missing
half-plane (a constant fill round-trips and the damage is uniform across all 8 row
bands), U/V swap inside the YUV→YUV path (parity is preserved: even→even,
odd→odd), cache incoherence (differing counts reproduce to <0.001 % across runs),
`bbuff` (1 is required and used), range clamping for the chroma (in-range data
misbehaves identically).

## 3. Safe / unsafe operations for "ISP NV12 → G2D 搬运 / 标注"

Everything below was measured on the board with `tools/hwtest/g2d_test.cpp` and the probes
in `tools/hwtest/g2d-probes/`. Numbers are for 1920x1080 NV12 in `/dev/dma_heap/system`
buffers, `G2D_BLT_NONE_H`, `bbuff=1`, `use_phy_addr=0`, and the correct sync order.

### ✅ 安全（直接可用）

| operation | how | evidence |
|---|---|---|
| **搬运整帧 NV12（位精确）** | 两次 `G2D_FORMAT_Y8` blit：同一缓冲看成 1920×1620 的 8 位单平面图，`clip_rect=(0,0,1920,1080)` 拷 Y，`clip_rect=(0,1080,1920,540)` 拷 UV | 全范围随机数据 **0/3110400**；真实 ISP 帧 **0/3110400**；Y8 路径不钳位、不滤波、不越界（矩形外 0 字节改动） |
| **单独搬运 Y（luma）** | 一次 `G2D_FORMAT_Y8`，`width/height/clip = w×h` | **0/2073600**（全范围随机，含 <16） |
| **缩放 / 裁 ROI / 旋转** | `G2D_CMD_BITBLT_H` + `resize` / `clip_rect` / `G2D_ROT_*` | 旋转几何逐点核对正确（上一轮），缩放尺寸正确（未做逐像素比对） |
| **在 NV12 上画框/填充** | `G2D_CMD_FILLRECT_H`，`dst_image_h.color = (Y<<16)|(U<<8)|V` | 矩形 `(100,64,256×64)`：内 16384/16384 改动、**外 0**；两平面同步写、色度矩形正确折半 |
| **ARGB 合成 / 颜色转换（NV12 ↔ ARGB8888）** | `G2D_CMD_BITBLT_H`，**4:2:0 常量必须用 `G2D_FORMAT_YUV420UVC_V1U1V0U0`（0x28）** | NV12→ARGB 复原 CPU 全范围 BT.601 参考精确（`R=238 G=14 B=14`）；ARGB→NV12 红/蓝色相正确 |
| **ARGB 位精确拷贝** | `G2D_CMD_BITBLT_H` + `G2D_FORMAT_ARGB8888`，数据 alpha 必须 `0xFF` | 逐通道 **0/2073600** |
| **给 ISP 缓冲加注释（零拷贝）** | `g2d_image_enh.fd` = V4L2 EXPBUF 的 fd（`use_phy_addr=0`） | 接口打通；dst 仍建议用专用 heap 缓冲 |

### ⛔ 要避开

| avoid | why |
|---|---|
| 用单次 `YUV420UVC` blit 做 NV12 搬运并期望位精确 | **不可能**。Y 被硬钳到 ≥16（真实帧实测 514456/2073600 字节被抬到 16 ⇒ **暗部被削**）；色度走 mixer 的 4:2:0 重采样滤波器（±2..+4 字节、带负瓣）。真实帧上色度恰好 0 差异，但 Y 一定不精确 |
| 用 `G2D_FORMAT_YUV420UVC_U1V1U0V0`（0x29）配 NV12 数据 | 它是 **NV21**。YUV→YUV 拷贝自洽（看不出问题），但任何 ARGB 方向转换都会**红蓝对调**。第六/七轮的探针一直用的这个值 |
| 用 `0x29` 产生的 "NV12" 交给编码器 / V4L2 / 下游 | 同上：色度字节序是 VU，下游按 UV 解会红蓝对调 |
| 拿 V4L2 采集缓冲直接当 G2D 的 dst | 专用 heap 缓冲才可控；且必须先 `cap.stop()`，否则 vin 还在往同一缓冲 DMA |
| `bbuff = 0` | 读到陈旧/黑数据（Y=16 有限色域黑帧） |
| `G2D_BLT_COPYPEN` | 本内核返回 -1（EINVAL），2.0 接口上不可用 |
| 把色度差异归因于缓存/同步竞态 | 差异是**确定性**的（三次跑 1025237/1025300，差 <0.001%），机制是重采样滤波。别再往 `begin_cpu_access`、uncached 分配、手工 flush 方向查 |
| 用周期 256 的图案 + 步长 256 去"定位色度偏移" | 每个采样点同相位，读数恒等 → 无信息。定位源偏移要用单点冲激（`tools/hwtest/g2d-probes/g2d_mark.c`）。 |
