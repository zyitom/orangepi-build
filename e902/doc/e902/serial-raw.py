#!/usr/bin/env python3
"""Send raw lines to the serial console and dump everything that comes back.

Used for debugging the console itself (sudo prompts, etc.). Each argument is
sent as one line followed by CR. Use --sleep to wait between lines.
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lines", nargs="+")
    ap.add_argument("-p", "--port", default="/dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--sleep", type=float, default=1.0)
    ap.add_argument("-t", "--tail", type=float, default=4.0,
                    help="seconds to keep reading after the last line")
    args = ap.parse_args()

    out = b""
    with serial.Serial(args.port, args.baud, timeout=0.2) as ser:
        ser.reset_input_buffer()
        ser.write(b"\r")
        time.sleep(0.5)
        out += ser.read(ser.in_waiting or 1)
        for ln in args.lines:
            ser.write(ln.encode() + b"\r")
            end = time.time() + args.sleep
            while time.time() < end:
                if ser.in_waiting:
                    out += ser.read(ser.in_waiting)
                else:
                    time.sleep(0.05)
        end = time.time() + args.tail
        while time.time() < end:
            if ser.in_waiting:
                out += ser.read(ser.in_waiting)
                end = time.time() + args.tail
            else:
                time.sleep(0.05)

    sys.stdout.write(out.decode("utf-8", "replace").replace("\r\n", "\n"))


if __name__ == "__main__":
    main()
