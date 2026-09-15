#!/usr/bin/env python3
"""Build /mnt/extsd/isp_param_config.bin for the Allwinner ISP602 libisp.

The file is [u32 size][20B time][50B notes][struct isp_param_config], where
the struct (116284 bytes) is the concatenation of a built-in sensor's
test/3a/tuning/iso settings. Offsets below come from the libisp_ini.so
set_* accessors (ISP602 layout, differs from the kernel isp_tuning_priv.h).

make_isp_bin.py <template_blob> <kurokesu ar0234.json> <out.bin> [options]
  --awb off|on          --msc off|on       --black N (10-bit, default 42)
  --ccm kurokesu|template   --awb-table kurokesu|template
"""
import argparse
import json
import struct
import time

SIZE = 116284

EN = {n: 88 + i for i, n in enumerate(
    "manual afs ae af awb hist wdr_split wdr_stitch otf_dpc ctc gca nrp denoise "
    "tdf blc wb dig_gain lsc msc pltm cfa lca sharp ccm defog cnr drc gtm gamma "
    "cem encpp enc_3dnr enc_2dnr".split())}
AWB_CT_LOW, AWB_CT_HIGH = 1088, 1090
AWB_LIGHT_NUM, AWB_LIGHT_INFO, AWB_LIGHT_FIELDS = 1098, 1104, 10
CCM0, CCM_TRIG = 87210, 87282          # 3 x (s16 matrix[3][3] + offset[3]), u16[3]
DYN_CFG0, DYN_CFG_SIZE, DYN_BLACK = 102480, 986, 121   # s16 index of black_level[4]

# LSC/MSC tables, reverse engineered from libisp_ini.so set_* accessors
# (2026-09-16, see analysis/libisp-offsets/). Payload offsets; the kernel
# isp_tuning_priv.h layout matches with +2848 bytes inserted before bayer_gain.
LSC_MODE, LSC_TBL, LSC_TRIG = 3124, 3130, 21562   # u8, u16[12][768], u16[6]
LSC_TBL_ROWS, LSC_ROW_LEN = 12, 768               # 768 = 3ch x 256 radial, Q10 (1.0=1024)
MSC_MODE, MSC_BLW, MSC_BLH, MSC_TRIG, MSC_TBL = 21574, 21576, 21598, 21620, 21632
MSC_TBL_ROWS, MSC_ROW_LEN = 12, 1452              # 1452 = 3ch x 484 (22x22 mesh)

ap = argparse.ArgumentParser()
ap.add_argument("template")
ap.add_argument("tuning")
ap.add_argument("out")
ap.add_argument("--awb", default="on")
ap.add_argument("--msc", default="off")
ap.add_argument("--lsc", default="off")
ap.add_argument("--black", type=int, default=42)
ap.add_argument("--ccm", default="kurokesu")
ap.add_argument("--awb-table", default="kurokesu")
ap.add_argument("--note", default="ar0234 from kurokesu libcamera tuning")
ap.add_argument("--set", action="append", default=[], help="module enable override, e.g. cem=0")
ap.add_argument("--lsc-json", metavar="FILE", help="inject LSC table from calibrate_lsc.py output")
ap.add_argument("--dump-lsc", metavar="FILE", help="dump the template's LSC/MSC tables to JSON and exit")
a = ap.parse_args()

b = bytearray(open(a.template, "rb").read())
assert len(b) == SIZE, len(b)

if a.dump_lsc:
    import json as _j
    _d = {
        "lsc_mode": b[LSC_MODE], "lsc_center": list(struct.unpack_from("<2h", b, LSC_MODE + 2)),
        "lsc_trig_cfg": list(u16_ := struct.unpack_from("<6H", b, LSC_TRIG)),
        "lsc_tbl": [list(struct.unpack_from("<768H", b, LSC_TBL + 768 * 2 * i)) for i in range(LSC_TBL_ROWS)],
        "msc_mode": b[MSC_MODE], "msc_trig_cfg": list(struct.unpack_from("<6H", b, MSC_TRIG)),
        "msc_tbl": [list(struct.unpack_from("<1452H", b, MSC_TBL + 1452 * 2 * i)) for i in range(MSC_TBL_ROWS)],
    }
    _j.dump(_d, open(a.dump_lsc, "w"))
    print("wrote", a.dump_lsc)
    raise SystemExit
t = {list(x)[0]: x[list(x)[0]] for x in json.load(open(a.tuning))["algorithms"]}
ct = t["rpi.awb"]["ct_curve"]
ct = [ct[i:i + 3] for i in range(0, len(ct), 3)]


def interp(x, pts, col):
    """linear in 1/CT (mired) on log(ratio), extrapolating from the end segments"""
    import math
    m = [(1e6 / p[0], math.log(p[col])) for p in pts]
    m.sort()
    xm = 1e6 / x
    if xm <= m[0][0]:
        (x0, y0), (x1, y1) = m[0], m[1]
    elif xm >= m[-1][0]:
        (x0, y0), (x1, y1) = m[-2], m[-1]
    else:
        for (x0, y0), (x1, y1) in zip(m, m[1:]):
            if x0 <= xm <= x1:
                break
    return math.exp(y0 + (y1 - y0) * (xm - x0) / (x1 - x0))


for name, val in (("awb", a.awb), ("msc", a.msc), ("lsc", a.lsc)):
    b[EN[name]] = 1 if val == "on" else 0
b[EN["manual"]] = 0
for kv in a.set:
    name, val = kv.split("=")
    b[EN[name]] = int(val)
print("enables:", " ".join("%s=%d" % (n, b[o]) for n, o in EN.items()))

# white balance light sources: [R/G*256, 256, B/G*256, 256, 256, 256, weight, CT, ...]
if a.awb_table == "kurokesu":
    n = b[AWB_LIGHT_NUM]
    for i in range(n):
        o = AWB_LIGHT_INFO + 4 * AWB_LIGHT_FIELDS * i
        f = list(struct.unpack_from("<10i", b, o))
        cct = f[7]
        f[0] = round(256 * interp(cct, ct, 1))
        f[2] = round(256 * interp(cct, ct, 2))
        struct.pack_into("<10i", b, o, *f)
        print("awb light %d: CT %d -> R/G %.3f B/G %.3f" % (i, cct, f[0] / 256, f[2] / 256))

# colour matrices at the template's trigger temperatures, Q8
if a.ccm == "kurokesu":
    ccms = t["rpi.ccm"]["ccms"]
    trig = struct.unpack_from("<3H", b, CCM_TRIG)
    for k, cct in enumerate(trig):
        lo = max([c for c in ccms if c["ct"] <= cct] or [ccms[0]], key=lambda c: c["ct"])
        hi = min([c for c in ccms if c["ct"] >= cct] or [ccms[-1]], key=lambda c: c["ct"])
        w = 0 if hi["ct"] == lo["ct"] else (cct - lo["ct"]) / (hi["ct"] - lo["ct"])
        m = [lo["ccm"][i] * (1 - w) + hi["ccm"][i] * w for i in range(9)]
        q = [round(v * 256) for v in m]
        for r in range(3):          # keep each row summing to exactly 256
            q[r * 3 + r] += 256 - sum(q[r * 3:r * 3 + 3])
        struct.pack_into("<12h", b, CCM0 + 24 * k, *q, 0, 0, 0)
        print("ccm %dK: %s" % (cct, q))

# black level, negative offset in 10-bit units, for all 14 ISO levels
for i in range(14):
    struct.pack_into("<4h", b, DYN_CFG0 + DYN_CFG_SIZE * i + 2 * DYN_BLACK, *([-a.black] * 4))

# LSC table from calibrate_lsc.py: rows of 768 u16 (3ch x 256 radial, Q10),
# duplicated into both halves (6 color temps x 2), triggers from the template
if a.lsc_json:
    d = json.load(open(a.lsc_json))
    rows = d["lsc_tbl"]
    assert len(rows) in (6, LSC_TBL_ROWS) and len(rows[0]) == LSC_ROW_LEN
    if len(rows) == 6:
        rows = rows + rows
    for i, r in enumerate(rows):
        struct.pack_into("<768H", b, LSC_TBL + LSC_ROW_LEN * 2 * i, *r)
    if d.get("lsc_center"):
        struct.pack_into("<2h", b, LSC_MODE + 2, *d["lsc_center"])
    b[LSC_MODE] = 1                      # lsc mode on (module enable is the EN bit)
    print("lsc: injected %d rows, center %s, trig %s" %
          (len(rows), d.get("lsc_center"), struct.unpack_from("<6H", b, LSC_TRIG)))

hdr = struct.pack("<I", SIZE) + time.strftime("%Y-%m-%d %H:%M").encode().ljust(20, b"\0")[:20] \
    + a.note.encode().ljust(50, b"\0")[:50]
open(a.out, "wb").write(hdr + bytes(b))
print("wrote", a.out)
