#!/bin/bash
# capture-e902-console.sh -- ON TL101.
#
# The E902's S_UART0 (board header pin16/pin18) is carried by a USB2TTY
# (FT232H, USB id 0403:6014). TL101's own /dev/ttyUSB0 is a CH340 wired to the
# board's *big-core* Linux console, so we must find the FTDI one by USB id, not
# by guessing ttyUSB0.
#
#   bash e902/capture-e902-console.sh            # find it and print diagnostics
#   bash e902/capture-e902-console.sh 60         # capture 60 s to a file + show it
#
# With the adapter moved to TL101 this keeps working across board reboots --
# which is the whole point.
set -u

DUR=${1:-0}
OUT=/tmp/e902-console-$(date +%H%M%S).log

echo "=== USB serial adapters on TL101 ==="
for d in /sys/bus/usb/devices/*/idVendor; do
    p=$(dirname "$d"); v=$(cat "$d" 2>/dev/null); [ -n "$v" ] || continue
    pr=$(cat "$p/idProduct" 2>/dev/null)
    pd=$(cat "$p/product" 2>/dev/null)
    [ "$v:$pr" = "0403:6014" ] && echo "  FTDI  $v:$pr  $pd   (path $p)" || echo "  other $v:$pr  $pd"
done

echo
echo "=== tty devices ==="
ls -l /dev/ttyUSB* /dev/ttyACM* 2>&1

# find the FTDI interface number -> /dev/ttyUSBn mapping
echo
echo "=== mapping (via /sys) ==="
for n in /sys/bus/usb-serial/devices/*; do
    [ -e "$n" ] || continue
    t=$(basename "$n")
    dev=$(readlink -f "$n/device" 2>/dev/null)
    echo "  $t -> $dev"
done

DEV=""
for n in /sys/bus/usb-serial/devices/ttyUSB*; do
    [ -e "$n" ] || continue
    drv=$(readlink -f "$n/driver" 2>/dev/null)
    dev=$(readlink -f "$n/device" 2>/dev/null)
    if echo "$drv $dev" | grep -q ftdi_sio; then
        DEV=/dev/$(basename "$n")
    fi
done

if [ -z "$DEV" ]; then
    echo
    echo "No FTDI (ftdi_sio) tty found. If you have not moved the FT232H's USB"
    echo "from the board to TL101 yet, do that first -- then re-run this."
    exit 1
fi

echo
echo "E902 console device: $DEV"
stty -F "$DEV" 115200 cs8 -cstopb -parenb -crtscts raw -echo 2>/dev/null

if [ "$DUR" = "0" ]; then
    echo "Reading 5 s as a liveness check ..."
    timeout 5 cat "$DEV" | head -20
    echo "(done)"
else
    echo "Capturing $DUR s to $OUT ..."
    timeout "$DUR" cat "$DEV" > "$OUT"
    echo "bytes: $(wc -c < "$OUT")"
    cat "$OUT"
fi
