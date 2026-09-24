#!/usr/bin/env python3
"""Fetch a file from the board over the serial console: get_file.py <remote> <local>"""
import base64, hashlib, re, sys, time, serial

remote, local = sys.argv[1], sys.argv[2]
s = serial.Serial("/dev/ttyUSB0", 115200, timeout=0.2)
s.write(b"stty -echo\r"); time.sleep(0.4); s.read(65536)
s.write(f"md5sum {remote}; echo __B64S__; base64 {remote}; echo __B64E__\r".encode())
buf, t = b"", time.time()
while time.time() - t < 900:
    d = s.read(65536)
    if d:
        buf += d
        if b"__B64E__\r\n" in buf or buf.rstrip().endswith(b"__B64E__"):
            if buf.count(b"__B64E__") >= 1 and re.search(rb"__B64E__\s*\r?\n", buf):
                break
txt = buf.decode("utf-8", "replace")
s.write(b"stty echo\r"); time.sleep(0.2); s.read(65536)
m = re.search(r"([0-9a-f]{32})\s+\S+", txt)
body = txt.split("__B64S__", 1)[1].split("__B64E__", 1)[0]
data = base64.b64decode("".join(body.split()))
open(local, "wb").write(data)
md5 = hashlib.md5(data).hexdigest()
print("remote md5", m.group(1) if m else None, "local md5", md5, len(data), "bytes")
sys.exit(0 if m and m.group(1) == md5 else 1)
