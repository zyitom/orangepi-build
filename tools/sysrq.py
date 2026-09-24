#!/usr/bin/env python3
"""Send a SysRq sequence on the board's serial console (break + key)."""
import time, serial
logf = open('/home/helios/Desktop/orangepi-build/ar0234-port/analysis/t14/sysrq.log', 'ab', buffering=0)
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.3)
s.break_condition = True
time.sleep(0.35)
s.break_condition = False
time.sleep(0.15)
s.write(b's')                       # sync
time.sleep(1.0)
s.break_condition = True
time.sleep(0.35)
s.break_condition = False
time.sleep(0.15)
s.write(b'u')                       # umount
time.sleep(1.0)
s.break_condition = True
time.sleep(0.35)
s.break_condition = False
time.sleep(0.15)
s.write(b'b')                       # reboot
end = time.time() + 25
while time.time() < end:
    d = s.read(65536)
    if d:
        logf.write(d)
