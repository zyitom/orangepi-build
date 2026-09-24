#!/usr/bin/env python3
"""Recovery: from the U-Boot prompt, boot once with extraargs=init=/bin/sh
(no saveenv - RAM only), repair the device tree from the root shell and reboot.
"""
import time, serial

LOG = '/home/helios/Desktop/orangepi-build/ar0234-port/analysis/t14/serial-recovery.log'
DTB = '/boot/dtb-6.6.98-sun60iw2/allwinner/sun60i-a733-orangepi-zero3w.dtb'
GOOD = DTB + '.pre-0008'

logf = open(LOG, 'ab', buffering=0)
logf.write(b"\n===== shell rescue start =====\n")
ser = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.3)


def drain(secs):
    end = time.time() + secs
    while time.time() < end:
        d = ser.read(65536)
        if d:
            logf.write(d)


def send(cmd, settle=2.5):
    logf.write(("\n>>> " + cmd + "\n").encode())
    ser.write((cmd + '\r').encode())
    drain(settle)


drain(2)
send('setenv extraargs init=/bin/sh')
send('run bootcmd', settle=3)
# let it boot; the shell prompt appears once the kernel has started init
drain(45)
send('mount -o remount,rw /')
send('echo SHELLUP-$(id -u)')
send('ls -la %s | tail -8' % DTB)
send('cp -f %s %s' % (GOOD, DTB))
send('md5sum %s %s' % (DTB, GOOD))
send('sync')
send('reboot -f', settle=3)
drain(20)
logf.write(b"\n===== shell rescue end =====\n")
