#!/usr/bin/env python3
"""serial-cap.py -- capture a serial console with host timestamps.

    serial-cap.py <tty> <baud> <out.log> [seconds]

Every line is prefixed with the host time since start, "[+  12.345] ", so
events on different consoles (and the SCP's frozen-in-standby tick
timestamps) can be put on one real-time axis. Bytes are written as
received (non-UTF-8 garble from baud switches is kept, lines are split
on \\n only). Stops after [seconds] (default: forever) or on SIGTERM.
"""
import signal
import sys
import time

import serial

tty, baud, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
limit = float(sys.argv[4]) if len(sys.argv) > 4 else None

stop = False


def on_term(*_):
    global stop
    stop = True


signal.signal(signal.SIGTERM, on_term)
signal.signal(signal.SIGINT, on_term)

t0 = time.monotonic()
port = serial.Serial(tty, baud, timeout=0.2)
port.reset_input_buffer()
at_line_start = True
with open(out, "wb", buffering=0) as f:
    f.write(b"# %s @%d, host start %s\n" % (tty.encode(), baud,
            time.strftime("%Y-%m-%d %H:%M:%S").encode()))
    while not stop and (limit is None or time.monotonic() - t0 < limit):
        data = port.read(4096)
        if not data:
            continue
        stamp = b"[+%8.3f] " % (time.monotonic() - t0)
        buf = bytearray()
        for b in data:
            if at_line_start:
                buf += stamp
                at_line_start = False
            buf.append(b)
            if b == 0x0A:
                at_line_start = True
        f.write(bytes(buf))
port.close()
