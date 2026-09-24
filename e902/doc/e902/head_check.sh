#!/bin/sh
S() { printf 'orangepi\n' | sudo -S sh -c "$*" 2>&1; }
echo "-- 24MiB head hash after the round-trip test --"
S 'dd if=/dev/mmcblk1 of=/tmp/head_after2.bin bs=1M count=24 status=none'
S 'sha256sum /tmp/head_after2.bin'
echo "-- expected --"
echo "114cd1f3a792b346acd87c8a550d4b3a09b5bfa7cce7cf0c406fa602b7bd4a54  (sd-boot-head-20260922.img)"
echo "-- board health --"
S 'python3 /home/orangepi/e902v2/awdevmem.py read 0x07032204'
S 'uptime'
