#!/bin/sh
# ispclk-scan.sh <mode> <secs> - step the ISP clock while streaming 1920x1200@120
# on /dev/video0 and record where frames start to get lost.
#
# Runs on the board as root (via systemd-run).  Writes to /tmp/ispclk-<mode>.out.
#   mode = up    : 324 -> 360 -> 405 -> 432 -> 486 -> 540 -> 600   (headroom test)
#   mode = down  : 324 -> 300 -> 270 -> 243 -> 216 -> 194 -> 162   (threshold test)
#
# One variable at a time: only the ISP clock source changes, the stream is
# closed while the rate is written, everything else (3DNR off, 1200p120, NV12)
# stays exactly as in the known-good baseline.
set -u

MODE=${1:-down}
SECS=${2:-12}
VFR=/home/orangepi/ar0234test/vfr
PARAM=/sys/module/ispclk_scan/parameters/set_rate
OUT=/tmp/ispclk-$MODE.out

case "$MODE" in
up)   RATES="324000000 360000000 405000000 432000000 486000000 540000000 600000000" ;;
down) RATES="324000000 300000000 270000000 243000000 216000000 194000000 162000000" ;;
test) RATES="324000000" ;;
*)    echo "usage: $0 up|down [secs]"; exit 2 ;;
esac

exec >"$OUT" 2>&1

clk() { cat /sys/kernel/debug/clk/isp/clk_rate; }
vi() { awk -v n="$1" '$0 ~ "^vi" n ":" {f=1} f && /frame =>|prs_in|internal/ {print "      " $0} f && /^\*\*\*/ {exit}' /sys/kernel/debug/mpp/vi; }
lost() { dmesg | awk -v m="$1" '$0 ~ m {f=1} f' | grep -c "frame lost"; }
reset() { dmesg | awk -v m="$1" '$0 ~ m {f=1} f' | grep -c "sunxi_isp_reset\|reset!!!"; }
errs() { dmesg | awk -v m="$1" '$0 ~ m {f=1} f' | grep -cE "configuration error|height error|FIFO|IOMMU|Oops|BUG:"; }

echo "### ispclk-scan mode=$MODE secs=$SECS start=$(date -Is)"
echo "### uptime=$(cut -d' ' -f1 /proc/uptime) 3A=$(systemctl is-active ar0234-3ad)"
echo "### module: $(cat /sys/module/vin_v4l2/srcversion)"
echo "### knob: $(cat $PARAM 2>/dev/null || echo MISSING)"

for R in $RATES; do
	echo
	echo "================= rate request $R ================="
	if [ -w "$PARAM" ]; then
		echo "$R" > "$PARAM"
	else
		echo "   !! $PARAM not writable, skipping"
		continue
	fi
	sleep 1
	ACLK=$(clk)
	echo "   clock after set : $ACLK"
	MARK="ISPC-${MODE}-${R}-$(date +%s)"
	echo "$MARK" > /dev/kmsg
	echo "   --- before ---"
	vi 0
	$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t "$SECS" 2>&1 | grep -E "RESULT|STREAMON|S_FMT|poll:" | sed 's/^/   /'
	echo "   clock after vfr : $(clk)"
	echo "   --- after ---"
	vi 0
	echo "   frame lost since marker : $(lost "$MARK")"
	echo "   isp resets since marker : $(reset "$MARK")"
	echo "   other errors            : $(errs "$MARK")"
	dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -E "frame lost|reset|error" | head -5 | sed 's/^/   /'
done

echo
echo "### final clock: $(clk)"
echo "### done $(date -Is)"
