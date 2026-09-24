#!/usr/bin/env python3
"""Catch the U-Boot autoboot window on the board's serial console and run a
script of U-Boot commands, logging everything.  Needed when the device tree
makes the kernel panic before userspace (and therefore before ssh) exists.

  uboot_rescue.py --log FILE --script CMDFILE [--wait 180]
"""
import argparse, time
import serial

ap = argparse.ArgumentParser()
ap.add_argument('--port', default='/dev/ttyUSB0')
ap.add_argument('--log', required=True)
ap.add_argument('--script', required=True)
ap.add_argument('--wait', type=float, default=180)
args = ap.parse_args()

logf = open(args.log, 'ab', buffering=0)
logf.write(b"\n===== uboot_rescue start =====\n")
ser = serial.Serial(args.port, 115200, timeout=0.2)
buf = b''
done = False
deadline = time.time() + args.wait
while time.time() < deadline:
    d = ser.read(65536)
    if not d:
        continue
    logf.write(d)
    buf = (buf + d)[-6000:]
    t = buf.decode('utf-8', 'replace')
    if done or 'U-Boot 2018' not in t and 'Hit any key' not in t:
        continue
    done = True
    logf.write(b"\n>>> hammering console to stop autoboot\n")
    end = time.time() + 3.0
    while time.time() < end:
        ser.write(b'\r')
        time.sleep(0.05)
    time.sleep(1.0)
    for line in open(args.script):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        logf.write(("\n>>> " + line + "\n").encode())
        ser.write((line + '\r').encode())
        time.sleep(2.0)
    logf.write(b"\n>>> script done, draining 20 s\n")
    end = time.time() + 20
    while time.time() < end:
        d = ser.read(65536)
        if d:
            logf.write(d)
    break
logf.write(b"\n===== uboot_rescue end =====\n")
