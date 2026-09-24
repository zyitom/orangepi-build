#!/usr/bin/env python3
"""Try harder to deliver a SysRq over the CH340 serial line."""
import time, termios, serial, fcntl

LOG = '/home/helios/Desktop/orangepi-build/ar0234-port/analysis/t14/sysrq2.log'
logf = open(LOG, 'ab', buffering=0)
logf.write(b"\n===== sysrq2 =====\n")
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.3)
fd = s.fileno()


def watch(secs, tag):
    end = time.time() + secs
    got = b''
    while time.time() < end:
        d = s.read(65536)
        if d:
            got += d
    if got:
        logf.write(("\n--- output after %s ---\n" % tag).encode() + got)
    return bool(got)


for tag in ('break_cond_0.5', 'tcsendbreak0', 'tcsendbreak1'):
    logf.write(("\n>>> method %s then 'b'\n" % tag).encode())
    if tag == 'break_cond_0.5':
        s.break_condition = True
        time.sleep(0.5)
        s.break_condition = False
    else:
        termios.tcsendbreak(fd, 0 if tag == 'tcsendbreak0' else 1)
    time.sleep(0.3)
    s.write(b'b')
    if watch(20, tag):
        break
logf.write(b"\n===== sysrq2 end =====\n")
