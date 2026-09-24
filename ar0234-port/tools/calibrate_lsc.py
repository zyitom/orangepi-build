#!/usr/bin/env python3
"""Calibrate the AR0234 LSC (lens shading) table without Allwinner's tool.

Shoot several flat-field RAW frames (evenly lit white target, fixed exposure
well below saturation, same mode you will use), then:

  tools/calibrate_lsc.py -o lsc.json /tmp/flat1.raw /tmp/flat2.raw ...
  tools/make_isp_bin.py template tuning out.bin --lsc on --lsc-json lsc.json

cap(1) captures RAW:  sudo ./cap 1920 1200 BA10 8 800 400 /tmp/flat
Table format (reverse engineered, see analysis/libisp-offsets/): 12 rows of
768 u16 = [R 256][G 256][B 256] radial gain curve, Q10 (1.0 = 1024), 6 color
temperature slots x 2 identical halves. We generate one temp slot; the same
table is written to all 6.
"""
import argparse
import json

import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("raw", nargs="+", help="flat-field RAW frames (BA10, 16-bit LE)")
ap.add_argument("-o", "--out", default="lsc.json")
ap.add_argument("--width", type=int, default=1920)
ap.add_argument("--height", type=int, default=1200)
ap.add_argument("--black", type=int, default=42, help="sensor black level (10-bit)")
ap.add_argument("--clip", type=float, default=0.95, help="ignore pixels above this * 1023")
a = ap.parse_args()

acc, n = None, 0
for p in a.raw:
    d = np.fromfile(p, dtype="<u2")
    assert d.size == a.width * a.height, f"{p}: {d.size} != {a.width}x{a.height}"
    img = d.reshape(a.height, a.width).astype(np.float64)
    img[img > a.clip * 1023] = np.nan                     # drop hot/saturated pixels
    acc = img if acc is None else acc + img
    n += 1
img = (acc / n) - a.black

ch = {                                                   # GRBG bayer
    "R": img[0::2, 1::2],
    "G": (img[0::2, 0::2] + img[1::2, 1::2]) / 2,
    "B": img[1::2, 0::2],
}

h, w = a.height // 2, a.width // 2
cy, cx = (h - 1) / 2, (w - 1) / 2
yy, xx = np.mgrid[0:h, 0:w]
rad = np.sqrt((yy - cy) ** 2 + (xx - cx) ** 2) / np.hypot(cy, cx)
bins = np.minimum((rad * 255).astype(int), 255)          # 256 radial bins, 0 = center

curves = {}
for name, m in ch.items():
    m = np.nan_to_num(m, nan=0.0)
    cnt = np.bincount(bins.ravel(), minlength=256)
    tot = np.bincount(bins.ravel(), weights=m.ravel(), minlength=256)
    mean = tot / np.maximum(cnt, 1)
    center = mean[0:3].mean()                            # innermost bins
    gains = center / np.maximum(mean, 1e-6)
    q = np.clip(np.round(gains * 1024), 1024, 8191).astype(int)   # Q10
    curves[name] = q.tolist()
    print("%s: center %.1f, edge gain %.2fx -> %.2fx" %
          (name, center, gains[255], q[255] / 1024))

rows = [curves["R"] + curves["G"] + curves["B"]]         # one color-temp slot
json.dump({"lsc_center": [2048, 2048], "lsc_tbl": rows}, open(a.out, "w"))
print("wrote", a.out, "- feed to make_isp_bin.py --lsc on --lsc-json", a.out)
