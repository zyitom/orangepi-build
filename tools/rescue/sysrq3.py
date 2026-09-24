#!/usr/bin/env python3
"""Send a real long BREAK through the CH340 by dropping the baud rate and
writing a NUL frame, then deliver the sysrq key at 115200."""
import time, serial

LOG = '/home/helios/Desktop/orangepi-build/ar0234-port/analysis/t14/sysrq3.log'
logf = open(LOG, 'ab', buffering=0)
logf.write(b"\n===== sysrq3 =====\n")
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.3)


def watch(secs):
    end = time.time() + secs
    hdr = False
    while time.time() < end:
        d = s.read(65536)
        if d:
            if not hdr:
                logf.write(b"\n--- output ---\n")
                hdr = True
            logf.write(d)
    return hdr


def longbreak():
    s.baudrate = 300          # 30 ms per bit -> a 0x00 byte is ~0.3 s of low line
    time.sleep(0.1)
    s.write(b'\x00\x00\x00')
    s.flush()
    time.sleep(0.4)
    s.baudrate = 115200
    time.sleep(0.1)


watch(1)
for key, name in ((b'b', 'reboot'), (b'h', 'help'), (b'b', 'reboot2')):
    logf.write(("\n>>> longbreak + '%s'\n" % key.decode()).encode())
    longbreak()
    s.write(key)
    if watch(15):
        break
logf.write(b"\n===== sysrq3 end =====\n")
