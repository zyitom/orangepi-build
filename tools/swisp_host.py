#!/usr/bin/env python3
"""Host software ISP for AR0234 RAW10 sequences using the Kurokesu libcamera tuning.

swisp_host.py <seq.raw> <tuning.json> <out.mp4> [--w 1920 --h 1200 --fps 30]
RAW: 16-bit LE, 10-bit data, GRBG. Pipeline: black level -> full-res demosaic
-> AWB (grey world snapped to calibrated ct curve) -> CCM(ct) -> gamma.
"""
import argparse
import json
import sys

import cv2
import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("raw")
ap.add_argument("tuning")
ap.add_argument("out")
ap.add_argument("--w", type=int, default=1920)
ap.add_argument("--h", type=int, default=1200)
ap.add_argument("--fps", type=float, default=30)
ap.add_argument("--bayer", default="GB", help="OpenCV Bayer code suffix giving GRBG")
ap.add_argument("--still", default=None, help="save frame N as PNG: N:path")
a = ap.parse_args()

t = {list(x)[0]: x[list(x)[0]] for x in json.load(open(a.tuning))["algorithms"]}
black = t["rpi.black_level"]["black_level"] / 64.0
ct = np.array(t["rpi.awb"]["ct_curve"]).reshape(-1, 3)
ccms = t["rpi.ccm"]["ccms"]
ccm_ct = np.array([c["ct"] for c in ccms])
ccm_m = np.array([c["ccm"] for c in ccms]).reshape(-1, 3, 3)
g = np.array(t["rpi.contrast"]["gamma_curve"]).reshape(-1, 2)
gamma_lut = np.interp(np.arange(65536), g[:, 0], g[:, 1]).astype(np.float32) / 65535.0
lo, hi = t["rpi.awb"]["modes"]["auto"]["lo"], t["rpi.awb"]["modes"]["auto"]["hi"]
ts = np.arange(lo, hi + 1, 10)
r_t = np.interp(ts, ct[:, 0], ct[:, 1])
b_t = np.interp(ts, ct[:, 0], ct[:, 2])
code = getattr(cv2, "COLOR_Bayer%s2RGB_EA" % a.bayer)

fsize = a.w * a.h * 2
nfr = __import__("os").path.getsize(a.raw) // fsize
fourcc = cv2.VideoWriter_fourcc(*"avc1")
vw = cv2.VideoWriter(a.out, fourcc, a.fps, (a.w, a.h))
if not vw.isOpened():
    print("avc1 unavailable, falling back to mp4v", file=sys.stderr)
    vw = cv2.VideoWriter(a.out, cv2.VideoWriter_fourcc(*"mp4v"), a.fps, (a.w, a.h))
still_n, still_path = (int(a.still.split(":")[0]), a.still.split(":", 1)[1]) if a.still else (-1, None)

ct_s = dg_s = None
with open(a.raw, "rb") as f:
    for n in range(nfr):
        raw = np.frombuffer(f.read(fsize), dtype="<u2").reshape(a.h, a.w)
        rgb = cv2.cvtColor(raw, code).astype(np.float32)
        rgb = np.clip((rgb - black) / (1023.0 - black), 0, 1)
        # grey world on mid-tones, snapped to the calibrated ct curve
        sub = rgb[::8, ::8].reshape(-1, 3)
        m = (sub.max(1) < 0.95) & (sub[:, 1] > 0.02)
        mr, mg, mb = sub[m].mean(0) if m.any() else (1, 1, 1)
        i = np.argmin((r_t - mr / mg) ** 2 + (b_t - mb / mg) ** 2)
        ct_s = ts[i] if ct_s is None else 0.8 * ct_s + 0.2 * ts[i]
        gr, gb = 1 / np.interp(ct_s, ct[:, 0], ct[:, 1]), 1 / np.interp(ct_s, ct[:, 0], ct[:, 2])
        k = np.clip(np.searchsorted(ccm_ct, ct_s), 1, len(ccm_ct) - 1)
        w = np.clip((ct_s - ccm_ct[k - 1]) / (ccm_ct[k] - ccm_ct[k - 1]), 0, 1)
        M = ccm_m[k - 1] * (1 - w) + ccm_m[k] * w
        rgb *= np.array([gr, 1.0, gb], dtype=np.float32)
        y = (rgb[::8, ::8] @ np.array([0.299, 0.587, 0.114], dtype=np.float32)).mean()
        dg = float(np.clip(0.18 / max(y, 1e-4), 1, 8))
        dg_s = dg if dg_s is None else 0.8 * dg_s + 0.2 * dg
        rgb = np.clip((rgb * dg_s) @ M.T.astype(np.float32), 0, 1)
        out = gamma_lut[(rgb * 65535).astype(np.uint16)]
        bgr = cv2.cvtColor((out * 255 + 0.5).astype(np.uint8), cv2.COLOR_RGB2BGR)
        vw.write(bgr)
        if n == still_n:
            cv2.imwrite(still_path, bgr)
        if n % 30 == 0:
            print("frame %d/%d ct=%.0fK gains R=%.2f B=%.2f dgain=%.2f" % (n, nfr, ct_s, gr, gb, dg_s))
vw.release()
print("wrote", a.out, nfr, "frames")
