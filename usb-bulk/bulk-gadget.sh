#!/bin/bash
# bulk-gadget.sh -- bring up the A733 as a FunctionFS BULK USB device.
#
#   bash usb-bulk/bulk-gadget.sh        # configfs + ffs mount + start daemon
#
# Switches usbc0 to device role first (the otg manager owns the port),
# builds the gadget through configfs, binds the UDC and starts
# usb-bulk/ffs-bulk (echo mode by default).
set -e
D=$(cd "$(dirname "$0")" && pwd)
SOC=/sys/devices/platform/soc@3000000
ROLE_FILE=$SOC/10.usbc0/otg_role
CFG=/sys/kernel/config/usb_gadget/bulk

# device role (ignored if already bound)
[ -f "$ROLE_FILE" ] && echo usb_device > "$ROLE_FILE" 2>/dev/null || true

# configfs
[ -d /sys/kernel/config/usb_gadget ] || modprobe libcomposite || {
    mount -t configfs none /sys/kernel/config 2>/dev/null || true
}
mkdir -p "$CFG"
cd "$CFG"
echo 0x1f3a > idVendor		# Allwinner
echo 0xa733 > idProduct
mkdir -p strings/0x409
echo "Allwinner" > strings/0x409/manufacturer
echo "A733 bulk pipe" > strings/0x409/product

mkdir -p functions/ffs.usb0
mkdir -p configs/c.1
ln -sf functions/ffs.usb0 configs/c.1/f1

# functionfs instance
mkdir -p /dev/ffs-bulk
mountpoint -q /dev/ffs-bulk || mount -t functionfs usb0 /dev/ffs-bulk

# bind UDC (auto-detect the single one)
UDC=$(ls /sys/class/udc | head -1)
[ -n "$UDC" ] || { echo "no UDC available (otg role? VBUS?)"; exit 1; }
echo "$UDC" > UDC
echo "gadget bound to $UDC"

# the daemon claims ep0 and runs
exec "$D/ffs-bulk" ${1:-}
