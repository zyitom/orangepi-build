#!/usr/bin/env python3
"""Copy a file off the board over the serial console, as base64.

  ./serial-get.py /tmp/sram.bin ./sram.bin
"""
import argparse
import base64
import hashlib
import re
import sys
import time

import serial

ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")
BEG, END = "KIROB64BEG", "KIROB64END"
BEG_W, END_W = "KIROB''64BEG", "KIROB''64END"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("remote")
    ap.add_argument("local")
    ap.add_argument("-p", "--port", default="/dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-t", "--timeout", type=float, default=600.0)
    a = ap.parse_args()

    cmd = ("echo %s; base64 -w 0 %s; echo; sha256sum %s | cut -d' ' -f1; echo %s"
           % (BEG_W, a.remote, a.remote, END_W))

    with serial.Serial(a.port, a.baud, timeout=0.2) as ser:
        ser.reset_input_buffer()
        ser.write(b"\r")
        time.sleep(0.5)
        if ser.in_waiting:
            ser.read(ser.in_waiting)
        ser.write(cmd.encode() + b"\r")
        buf = b""
        deadline = time.time() + a.timeout
        while time.time() < deadline:
            if ser.in_waiting:
                buf += ser.read(ser.in_waiting)
                if END.encode() in buf:
                    break
                deadline = time.time() + a.timeout
            else:
                time.sleep(0.05)

    text = ANSI.sub("", buf.decode("utf-8", "replace")).replace("\r\n", "\n")
    if BEG not in text or END not in text:
        print("framing markers missing", file=sys.stderr)
        return 1
    body = text.split(BEG, 1)[1].rsplit(END, 1)[0].strip().split("\n")
    body = [l.strip() for l in body if l.strip()]
    want = body[-1]
    data = base64.b64decode("".join(body[:-1]))
    got = hashlib.sha256(data).hexdigest()
    if got != want:
        print("sha256 mismatch: board=%s local=%s" % (want, got), file=sys.stderr)
        return 1
    with open(a.local, "wb") as f:
        f.write(data)
    print("%s -> %s (%d bytes, sha256 ok)" % (a.remote, a.local, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
