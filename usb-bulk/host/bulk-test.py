#!/usr/bin/env python3
"""bulk-test.py -- PC-side test for the A733 FunctionFS BULK device.

    pip install pyusb        (libusb 1.0 needed on the PC)
    sudo python3 bulk-test.py [MB]

Finds the gadget by VID/PID (1f3a:a733), sends `MB` megabytes on the
BULK OUT endpoint and reads the echo on BULK IN, then prints the
measured throughput. This is the side where libusb-style bulk
transfers live.
"""
import sys
import time

import usb.core
import usb.util

VID, PID = 0x1F3A, 0xA733
MB = int(sys.argv[1]) if len(sys.argv) > 1 else 8
CHUNK = 512 * 1024

dev = usb.core.find(idVendor=VID, idProduct=PID)
if dev is None:
    sys.exit("device 1f3a:a733 not found (gadget up? cable on the OTG port?)")

if dev.is_kernel_driver_active(0):
    dev.detach_kernel_driver(0)
dev.set_configuration()

out_ep = dev[0][(0, 0)][1]      # first interface, endpoint OUT
in_ep = dev[0][(0, 0)][0]       # first interface, endpoint IN
print(f"found: bulk OUT {out_ep.bEndpointAddress:#04x}, "
      f"bulk IN {in_ep.bEndpointAddress:#04x}, "
      f"maxpacket {out_ep.wMaxPacketSize}")

payload = bytes(range(256)) * (CHUNK // 256)
total = MB * 1024 * 1024
sent = 0
t0 = time.monotonic()
while sent < total:
    n = min(CHUNK, total - sent)
    out_ep.write(payload[:n], timeout=5000)
    got = in_ep.read(len(payload), timeout=5000)
    assert bytes(got) == payload[:n], "echo mismatch"
    sent += n
dt = time.monotonic() - t0
print(f"echo {MB} MB in {dt:.2f} s = {MB / dt:.1f} MB/s "
      f"({MB * 8 / dt:.0f} Mbit/s)")
