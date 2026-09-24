#!/bin/sh
# board side: install the new vin module + back up things
set -e
U=/lib/modules/6.6.98-sun60iw2/updates
D=/boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb
rm -f /vin_v4l2.ko
echo "--- before ---"
md5sum $U/vin_v4l2.ko
ls $U | tr '\n' ' '; echo
[ -f $U/vin_v4l2.ko.bak-0007 ] || cp $U/vin_v4l2.ko $U/vin_v4l2.ko.bak-0007
cp /tmp/vin_v4l2-0008.ko $U/vin_v4l2.ko
depmod -a
[ -f $D.pre-0008 ] || cp $D $D.pre-0008
echo "--- after ---"
md5sum $U/vin_v4l2.ko $U/vin_v4l2.ko.bak-0007 $D $D.pre-0008
modinfo -F srcversion $U/vin_v4l2.ko
