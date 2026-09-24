#!/bin/bash
# Watch for the A733 boot card appearing in the USB reader; flash the canary
# payload automatically (once). Run as root.
CAN=/home/helios/Desktop/orangepi-build/e902/fw-out/v58-padded-105912.bin
WANT=$(sha256sum $CAN | cut -d' ' -f1)
FLASH=/home/helios/Desktop/orangepi-build/e902/flash-scp-from-reader.sh
LOG=/home/helios/Desktop/orangepi-build/e902/verify-logs/autoflash-20260922.log
echo "$(date +%F\ %T) watcher start, want sha $WANT" >> $LOG
DONE=0
while [ $DONE -lt 3 ]; do
  for d in /dev/sd?; do
    [ -b "$d" ] || continue
    rem=$(cat /sys/block/$(basename $d)/removable 2>/dev/null || echo 0)
    [ "$rem" = "1" ] || continue
    m=$(dd if=$d bs=1 skip=$((0x1004000)) count=13 status=none 2>/dev/null)
    [ "$m" = "sunxi-package" ] || continue
    cur=$(dd if=$d bs=1 skip=$((0x113BC00)) count=105912 status=none 2>/dev/null | sha256sum | cut -d' ' -f1)
    if [ "$cur" = "$WANT" ]; then
      echo "$(date +%T) $d: card present, already canary" >> $LOG
      DONE=$((DONE+1))
    else
      echo "$(date +%T) $d: A733 card detected, flashing canary..." >> $LOG
      bash $FLASH $d flash $CAN >> $LOG 2>&1
      echo "$(date +%T) $d: flash cycle finished (rc=$?)" >> $LOG
      DONE=3
    fi
  done
  sleep 2
done
echo "$(date +%F\ %T) watcher exit" >> $LOG
