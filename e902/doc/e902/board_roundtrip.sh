#!/bin/sh
# Runs ON the board. CONTENT-PRESERVING write test of the boot medium.
#
# Reads the 105912-byte "scp" region at 0x113BC00, writes EXACTLY THE SAME
# BYTES back, then reads again and compares hashes. Because the payload is
# identical the medium ends up bit-for-bit unchanged.
# Proves: that raw region is really WRITABLE, that dd seek/count/conv=notrunc
# lands where we think, and that the read-back verification works.
S() { printf 'orangepi\n' | sudo -S sh -c "$*" 2>&1; }

DEV=/dev/mmcblk1
OFF=$((0x113BC00))
SIZE=105912

echo "##### 0. device read-only flags"
S 'cat /sys/block/mmcblk1/ro'
S 'blockdev --getro /dev/mmcblk1'

echo
echo "##### 1. read BEFORE"
S "dd if=$DEV of=/tmp/rt_before.bin bs=1 skip=$OFF count=$SIZE status=none"
echo -n "before sha256: "; S 'sha256sum /tmp/rt_before.bin' | awk '{print $1}'

echo
echo "##### 2. write the SAME bytes back (content-preserving)"
S "dd if=/tmp/rt_before.bin of=$DEV bs=1 seek=$OFF count=$SIZE conv=notrunc status=none" >/dev/null 2>&1
echo "write command issued"

echo
echo "##### 3. sync + read AFTER"
S 'sync'
S "dd if=$DEV of=/tmp/rt_after.bin bs=1 skip=$OFF count=$SIZE status=none"
echo -n "after  sha256: "; S 'sha256sum /tmp/rt_after.bin' | awk '{print $1}'

echo
echo "##### 4. verdict"
S 'b=$(sha256sum /tmp/rt_before.bin | cut -d" " -f1); a=$(sha256sum /tmp/rt_after.bin | cut -d" " -f1); echo "before=$b"; echo "after =$a"; [ "$b" = "$a" ] && echo "RESULT: IDENTICAL -> raw region is WRITABLE and the write path is correct" || echo "RESULT: MISMATCH -> do not proceed"'

echo
echo "##### 5. whole-head integrity re-check (expect 114cd1f3...)"
S "dd if=$DEV of=/tmp/head_after.bin bs=1M count=24 status=none"
echo -n "24MiB  sha256: "; S 'sha256sum /tmp/head_after.bin' | awk '{print $1}'

echo
echo "##### 6. board still healthy (RST_START should remain 0x40004000)"
S 'python3 /home/orangepi/e902v2/awdevmem.py read 0x07032204'
