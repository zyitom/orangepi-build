#!/bin/sh
# dmfilter.sh <marker>
#
# Print the kernel messages logged after <marker> (a unique string previously
# written to /dev/kmsg), dropping the per-frame noise and the ar0234_mipi
# debug chatter, so a run's log tail stays readable.
#   dmfilter.sh VFRMARK-a1-1234-567
dmesg | awk -v m="$1" '$0 ~ m { f = 1 } f' |
	grep -Ev 'frame lost|sunxi_isp_reset|\[ar0234_mipi\]' |
	tail -20
