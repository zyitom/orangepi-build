#!/usr/bin/env python3
"""Run a shell command on the board over the serial console, print its output.

With -s the command runs under `sudo -S`, fed the password from $BOARD_PASS
(default: the Orange Pi image default, `orangepi`).

  ./serial-run.py 'cat /proc/iomem'
  ./serial-run.py -s 'busybox devmem 0x0701021C 32'

Framing note: the markers are written on the wire with an embedded '' quote
pair, so the shell's echo of the command line does not itself match the
marker we scan for -- only the shell's output of `echo` does.
"""
import argparse
import os
import re
import sys
import time

import serial

BEG = "KIROBEG9"
END = "KIROEND9"
# what we type: quote pair splits the literal so the echoed line won't match
BEG_WIRE = "KIROB''EG9"
END_WIRE = "KIROE''ND9"

ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")


def sh_quote(s):
    return "'" + s.replace("'", "'\\''") + "'"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command")
    ap.add_argument("-p", "--port", default="/dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-t", "--timeout", type=float, default=30.0)
    ap.add_argument("-s", "--sudo", action="store_true",
                    help="run under sudo -S, feeding $BOARD_PASS")
    args = ap.parse_args()

    cmd = args.command
    if args.sudo:
        pw = os.environ.get("BOARD_PASS", "orangepi")
        cmd = "printf '%%s\\n' %s | sudo -S -p '' sh -c %s" % (sh_quote(pw), sh_quote(cmd))

    line = "echo %s; %s; echo %s" % (BEG_WIRE, cmd, END_WIRE)

    with serial.Serial(args.port, args.baud, timeout=0.2) as ser:
        ser.reset_input_buffer()
        ser.write(b"\r")
        time.sleep(0.5)
        if ser.in_waiting:
            ser.read(ser.in_waiting)
        ser.write(line.encode() + b"\r")

        buf = b""
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            if ser.in_waiting:
                buf += ser.read(ser.in_waiting)
                if END.encode() in buf:
                    break
            else:
                time.sleep(0.05)

    text = ANSI.sub("", buf.decode("utf-8", "replace")).replace("\r\n", "\n")
    complete = END in text
    if BEG in text:
        text = text.split(BEG, 1)[1].lstrip("\n")
    if END in text:
        text = text.rsplit(END, 1)[0]
    print(text.rstrip("\n"))
    if not complete:
        print("[serial-run: no end marker; output truncated or still running]",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
