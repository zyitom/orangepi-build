#!/bin/sh
# start.sh <outfile> <points...> - launch isp-iso-scan.sh detached via systemd-run
OUT=$1; shift
exec systemd-run --unit=t17iso --collect --setenv=OUT="$OUT" \
	/bin/sh /tmp/isp-iso-scan.sh "$@"
