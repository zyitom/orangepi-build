#!/usr/bin/env python3
"""Continuous serial logger + command injector for the Orange Pi board.

Owns /dev/ttyUSB0 so it can both (a) dump everything the board prints to a log
file, and (b) send commands into the board's console shell, which is the only
channel that survives a kernel panic.

  serial_log.py [--port /dev/ttyUSB0] [--log FILE] [--cmd FILE] [--no-login]

Command injection: append lines to the file given by --cmd (default /tmp/t14.cmd);
each appended line is sent to the board as a shell command. Example:

  echo 'uptime' >> /tmp/t14.cmd

Log file gets a timestamped marker line whenever output resumes after a gap,
so a panic/last-words moment is easy to locate.
"""
import argparse
import os
import re
import sys
import time

import serial

ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")


def stamp():
    return time.strftime("%H:%M:%S")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--log", default="/tmp/t14-serial.log")
    ap.add_argument("--cmd", default="/tmp/t14.cmd")
    ap.add_argument("--no-login", action="store_true")
    args = ap.parse_args()

    log = open(args.log, "ab", buffering=0)
    log.write(("\n===== serial_log start %s (%s) =====\n"
               % (time.strftime("%Y-%m-%d %H:%M:%S"), args.port)).encode())

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    cmd_pos = os.path.getsize(args.cmd) if os.path.exists(args.cmd) else 0
    last_rx = 0.0
    login_sent = args.no_login
    probe_deadline = time.time() + 8.0

    # Nudge the console so we learn whether a shell is already there.
    ser.write(b"\r")

    while True:
        try:
            data = ser.read(65536)
        except Exception as exc:  # device replug / transient error
            log.write(("[%s] SERIAL ERROR %r\n" % (stamp(), exc)).encode())
            time.sleep(1)
            continue

        if data:
            now = time.time()
            if now - last_rx > 3.0:
                log.write(("\n[%s] ---------- output ----------\n" % stamp()).encode())
            last_rx = now
            log.write(data)
            text = ANSI.sub("", data.decode("utf-8", "replace"))
            if not login_sent and not args.no_login and "login:" in text:
                time.sleep(0.3)
                ser.write(b"orangepi\r")
                time.sleep(2.0)
                ser.write(os.environ.get("BOARD_PASS", "orangepi").encode() + b"\r")
                time.sleep(1.5)
                ser.write(b"export PS1='T14> '\r")
                login_sent = True
                log.write(("\n[%s] (auto-login sent)\n" % stamp()).encode())

        # After boot, getty may not be up yet; keep nudging until we're in.
        if not login_sent and time.time() > probe_deadline:
            if "T14>" in open(args.log, "rb").read()[-4096:].decode("utf-8", "replace"):
                login_sent = True
            else:
                probe_deadline = time.time() + 8.0
                ser.write(b"\r")

        # Command injection.
        try:
            size = os.path.getsize(args.cmd)
        except OSError:
            size = cmd_pos
        if size > cmd_pos:
            with open(args.cmd, "rb") as cf:
                cf.seek(cmd_pos)
                chunk = cf.read()
                cmd_pos = cf.tell()
            for line in chunk.decode("utf-8", "replace").splitlines():
                line = line.strip()
                if not line:
                    continue
                log.write(("[%s] >>> %s\n" % (stamp(), line)).encode())
                ser.write(line.encode() + b"\r")
                time.sleep(0.15)
        elif size < cmd_pos:  # file was truncated/rotated
            cmd_pos = size


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
