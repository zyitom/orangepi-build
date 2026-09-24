#!/usr/bin/env python3
"""Transfer a file to the board over the serial console (base64, paced).

Usage: tools/send_file.py <local_file> <remote_path>
"""
import base64
import hashlib
import re
import sys
import time

import serial

PORT = "/dev/ttyUSB0"
BAUD = 115200


def drain(s, timeout=1.0):
    buf = b""
    t0 = time.time()
    while time.time() - t0 < timeout:
        d = s.read(65536)
        if d:
            buf += d
            t0 = time.time() + 0.3
    return buf.decode("utf-8", "replace")


def main():
    local, remote = sys.argv[1], sys.argv[2]
    data = open(local, "rb").read()
    md5 = hashlib.md5(data).hexdigest()
    b64 = base64.b64encode(data).decode()
    lines = [b64[i : i + 76] for i in range(0, len(b64), 76)]

    s = serial.Serial(PORT, BAUD, timeout=0.2)
    s.reset_input_buffer()
    s.write(b"stty -echo\r")
    time.sleep(0.3)
    drain(s)

    s.write(f"cat > {remote}.b64 <<'XEOF'\r".encode())
    time.sleep(0.4)
    drain(s)

    sent = 0
    chunk = b""
    for ln in lines:
        chunk += (ln + "\n").encode()
        if len(chunk) >= 512:
            s.write(chunk)
            sent += len(chunk)
            chunk = b""
            time.sleep(0.06)
            if sent % 32768 < 600:
                print(f"  {sent/1024:.0f} KiB / {len(b64)/1024:.0f} KiB", flush=True)
    if chunk:
        s.write(chunk)
        time.sleep(0.1)
    s.write(b"XEOF\r")
    time.sleep(0.6)
    drain(s)

    s.write(f"base64 -d {remote}.b64 > {remote} && md5sum {remote} && ls -la {remote}\r".encode())
    out = drain(s, 6)
    s.write(b"stty echo\r")
    time.sleep(0.2)
    drain(s)
    s.close()

    m = re.search(r"([0-9a-f]{32})\s+" + re.escape(remote), out)
    if m and m.group(1) == md5:
        print(f"OK {remote}: md5 {md5} verified, {len(data)} bytes")
        return 0
    print(f"MISMATCH/FAIL: expected {md5}, got:\n{out[-800:]}")
    return 1


sys.exit(main())
