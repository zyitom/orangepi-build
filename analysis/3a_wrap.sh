#!/bin/sh
# pass only the "ready" line to ar0234_rec, log everything else
stdbuf -oL -eL /home/orangepi/ar0234test/ar0234_3a "$@" 2>&1 | while IFS= read -r l; do
	echo "$l" >> /tmp/an/3a.log
	case "$l" in ready*) echo "$l" ;; esac
done
