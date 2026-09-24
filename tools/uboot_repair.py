#!/usr/bin/env python3
"""Catch the U-Boot prompt of a board that is panic-looping and repair the DTB
from U-Boot itself (no kernel/userspace needed), then boot.

  tools/uboot_repair.py [--minutes 12] [--cmds FILE]

The board panics ~6 s into every boot (bad device tree), systemd's 16 s watchdog
reboots it, so U-Boot's 1 s autoboot window comes back every ~35-50 s.  This
script watches the serial console, stops autoboot on the next cycle, verifies it
really has a prompt (echo PING) and then runs the command file.
"""
import argparse
import time

import serial

ap = argparse.ArgumentParser()
ap.add_argument('--port', default='/dev/ttyUSB0')
ap.add_argument('--log', default='/home/helios/Desktop/orangepi-build/ar0234-port/analysis/t14/uboot-repair.log')
ap.add_argument('--cmds', default='/home/helios/Desktop/orangepi-build/ar0234-port/tools/uboot_fix_dtb.txt')
ap.add_argument('--minutes', type=float, default=12)
args = ap.parse_args()

logf = open(args.log, 'ab', buffering=0)
logf.write(b"\n===== uboot_repair start =====\n")
ser = serial.Serial(args.port, 115200, timeout=0.3)

buf = b''
deadline = time.time() + args.minutes * 60
caught = False

while time.time() < deadline and not caught:
    d = ser.read(65536)
    if not d:
        continue
    logf.write(d)
    buf = (buf + d)[-8000:]
    t = buf.decode('utf-8', 'replace')
    if 'U-Boot 2018' not in t and 'Hit any key' not in t:
        continue
    logf.write(b"\n>>> banner seen, stopping autoboot\n")
    end = time.time() + 4.0
    while time.time() < end:
        ser.write(b'\r')
        time.sleep(0.05)
    time.sleep(1.0)
    # verify we are at a prompt
    ser.write(b'echo PING\r')
    end = time.time() + 3
    got = b''
    while time.time() < end:
        got += ser.read(65536)
    logf.write(b'\n--- prompt probe ---\n' + got)
    if b'PING' in got:
        caught = True
        break
    logf.write(b">>> not at a prompt, waiting for the next boot cycle\n")
    buf = b''

if not caught:
    logf.write(b"\n!!! never got a U-Boot prompt !!!\n")
    raise SystemExit(1)

logf.write(b"\n>>> at U-Boot prompt, running repair commands\n")
for line in open(args.cmds):
    line = line.rstrip('\n')
    if not line or line.startswith('#'):
        continue
    logf.write(("\n>>> " + line + "\n").encode())
    ser.write((line + '\r').encode())
    end = time.time() + 2.0
    while time.time() < end:
        d = ser.read(65536)
        if d:
            logf.write(d)
            end = time.time() + 2.0

logf.write(b"\n>>> draining 60 s (boot)\n")
end = time.time() + 60
while time.time() < end:
    d = ser.read(65536)
    if d:
        logf.write(d)
logf.write(b"\n===== uboot_repair end =====\n")
