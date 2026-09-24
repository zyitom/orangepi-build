#!/bin/sh
# runs on the board as root: insmod the ISP clock probe and print its output.
# usage: run-ispclk.sh "<params>"      e.g. run-ispclk.sh "apply=0"
PARAMS="$1"
MARK="ISPCLK-$(date +%s)"
echo "$MARK" > /dev/kmsg
if [ -e /sys/module/ispclk_scan ]; then
	rmmod ispclk_scan 2>&1 || echo "rmmod failed"
fi
echo "--- insmod ispclk_scan.ko $PARAMS ---"
insmod /tmp/ispclk_scan.ko $PARAMS
echo "insmod rc=$?"
sleep 1
echo "--- dmesg since $MARK ---"
dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -v "^$MARK" 
echo "--- module state ---"
lsmod | grep ispclk || echo "(not loaded)"
