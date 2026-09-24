#!/usr/bin/env python3
"""Passively listen on a serial port and dump everything received.

For watching the E902's S_UART0 console (PL2/PL3) while the firmware is
loaded from the other port. Start this BEFORE releasing reset, or the banner
is gone before you are attached.

  ./serial-listen.py -p /dev/ttyUSB1 -t 20

Prints raw bytes as hex too, so a wrong baud rate is recognisable (garbage
with the right framing) versus a dead line (nothing at all).
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="/dev/ttyUSB1")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-t", "--seconds", type=float, default=15.0)
    ap.add_argument("--hex", action="store_true", help="also dump hex")
    a = ap.parse_args()

    try:
        ser = serial.Serial(a.port, a.baud, timeout=0.2)
    except OSError as e:
        print("cannot open %s: %s" % (a.port, e), file=sys.stderr)
        return 1

    buf = b""
    with ser:
        ser.reset_input_buffer()
        end = time.time() + a.seconds
        while time.time() < end:
            if ser.in_waiting:
                chunk = ser.read(ser.in_waiting)
                buf += chunk
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
            else:
                time.sleep(0.02)

    print("\n--- %d bytes received on %s @ %d ---"
          % (len(buf), a.port, a.baud), file=sys.stderr)
    if a.hex and buf:
        for i in range(0, min(len(buf), 256), 16):
            row = buf[i:i + 16]
            print("%04x  %-47s  |%s|" % (
                i, " ".join("%02x" % b for b in row),
                "".join(chr(c) if 32 <= c < 127 else "." for c in row)),
                file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
