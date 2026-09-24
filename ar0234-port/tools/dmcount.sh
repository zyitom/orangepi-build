#!/bin/sh
# On the board: count kernel log lines matching PATTERN, starting at MARKER.
# dmesg is a ring buffer, so absolute counts go down when it wraps - counting
# after a marker written into /dev/kmsg at the start of a test is the only
# reliable way.
#   dmcount.sh <marker> <pattern>
dmesg | awk -v m="$1" '$0 ~ m { f = 1 } f' | grep -c "$2" || true
