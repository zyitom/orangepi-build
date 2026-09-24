#!/usr/bin/env python3
"""Copy a local file to the board over the serial console, as base64 chunks.

  ./serial-put.py awdevmem.py /tmp/awdevmem.py

Verifies with sha256 on both ends and exits non-zero on mismatch.
"""
import argparse
import base64
import hashlib
import os
import re
import sys
import time

import serial

ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")
CHUNK = 512


def wait_prompt(ser, timeout=15.0):
    """Read until the shell prompt trailer appears; return what we saw."""
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if ser.in_waiting:
            buf += ser.read(ser.in_waiting)
            if buf.rstrip().endswith(b"$") or buf.rstrip().endswith(b"#"):
                return buf
        else:
            time.sleep(0.02)
    return buf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("local")
    ap.add_argument("remote")
    ap.add_argument("-p", "--port", default="/dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    args = ap.parse_args()

    with open(args.local, "rb") as f:
        data = f.read()
    want = hashlib.sha256(data).hexdigest()
    b64 = base64.b64encode(data).decode()
    chunks = [b64[i:i + CHUNK] for i in range(0, len(b64), CHUNK)]
    tmp = args.remote + ".b64"

    with serial.Serial(args.port, args.baud, timeout=0.2) as ser:
        ser.reset_input_buffer()
        ser.write(b"\r")
        wait_prompt(ser, 3)

        ser.write((": > %s\r" % tmp).encode())
        wait_prompt(ser, 5)

        for i, c in enumerate(chunks):
            ser.write(("printf %%s %s >> %s\r" % (c, tmp)).encode())
            wait_prompt(ser, 10)
            sys.stderr.write("\r  %d/%d chunks" % (i + 1, len(chunks)))
        sys.stderr.write("\n")

        ser.write(("base64 -d %s > %s && rm -f %s && sha256sum %s\r"
                   % (tmp, args.remote, tmp, args.remote)).encode())
        out = ANSI.sub("", wait_prompt(ser, 20).decode("utf-8", "replace"))

    got = None
    for tok in out.replace("\r\n", "\n").split():
        if len(tok) == 64 and all(ch in "0123456789abcdef" for ch in tok):
            got = tok
    if got != want:
        print("sha256 mismatch: board=%s local=%s" % (got, want), file=sys.stderr)
        print(out, file=sys.stderr)
        return 1
    print("%s -> %s  (%d bytes, sha256 ok)"
          % (args.local, args.remote, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
