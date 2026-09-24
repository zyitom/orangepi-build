#!/usr/bin/env python3
"""Send U-Boot commands to the board serial console (assumes it is already at
the '=>' prompt) and log everything.  uboot_cmd.py --log FILE --script CMDFILE"""
import argparse, time
import serial

ap = argparse.ArgumentParser()
ap.add_argument('--port', default='/dev/ttyUSB0')
ap.add_argument('--log', required=True)
ap.add_argument('--script', required=True)
ap.add_argument('--settle', type=float, default=1.0)
args = ap.parse_args()

logf = open(args.log, 'ab', buffering=0)
logf.write(b"\n===== uboot_cmd start =====\n")
ser = serial.Serial(args.port, 115200, timeout=0.3)
# drain whatever is pending
end = time.time() + 2
while time.time() < end:
    d = ser.read(65536)
    if d:
        logf.write(d)
for line in open(args.script):
    line = line.rstrip('\n')
    if not line or line.startswith('#'):
        continue
    logf.write(("\n>>> " + line + "\n").encode())
    ser.write((line + '\r').encode())
    end = time.time() + args.settle
    while time.time() < end:
        d = ser.read(65536)
        if d:
            logf.write(d)
            end = time.time() + args.settle
logf.write(b"\n===== uboot_cmd end =====\n")
