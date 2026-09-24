#!/bin/sh
# pure-isp-div.sh "1 2 3 4" - change ONLY the ISP602 core divider, mid-stream.
#
# Parent pll-video0-4x and the CSI/parser clock (csi, 0x1840) are left exactly
# where the VIN driver put them (both 324 MHz, M=1).  This is the only clock
# movement that is simultaneously (a) ISP-core-only and (b) free of any PLL or
# CSI re-programme, which is what a clean measurement of the ISP's own limit
# needs.  The price is that the ladder is only 324/D:
#   D=1 -> 324   D=2 -> 162   D=3 -> 108   D=4 -> 81   D=6 -> 54 MHz
set -u

SECS=${SECS:-14}
SETTLE=${SETTLE:-4}
MEAS=${MEAS:-8}
VFR=/home/orangepi/ar0234test/vfr
KISP=/sys/module/ispclk_scan/parameters/set_rate
KDIV=/sys/module/ispclk_scan/parameters/isp_div
OUT=${OUT:-/tmp/pure-div.out}

R=/tmp/vinreg
exec >"$OUT" 2>&1

reg() { $R r $1 2>/dev/null | sed 's/.*= //'; }
vi() { awk '/^vi0:/{f=1} f && /frame =>|internal =>|prs_in/{print "      " $0} f && /^\*\*\*/{exit}' /sys/kernel/debug/mpp/vi; }
vfield() { awk '/^vi0:/{f=1} f && /frame =>/{gsub(/,/,"");print $'"$1"'+0; exit}' /sys/kernel/debug/mpp/vi; }
fcount() { awk '/^vi0:/{f=1} f && /frame =>/{gsub(/,/,"");print $4+0; exit}' /sys/kernel/debug/mpp/vi; }
lcount() { awk '/^vi0:/{f=1} f && /frame =>/{gsub(/,/,"");print $6+0; exit}' /sys/kernel/debug/mpp/vi; }

echo "### pure-isp-div start=$(date -Is) secs=$SECS meas=$MEAS"
echo "### uptime=$(cut -d' ' -f1 /proc/uptime) load=$(cut -d' ' -f1-3 /proc/loadavg)"
echo "### frame-lost in dmesg at start: $(dmesg | grep -c 'frame lost')"
echo
echo "### warm-up (throwaway)"
$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 10 2>&1 | grep RESULT | sed 's/^/###   /'
sleep 3

for D in "$@"; do
	MARK="PUREDIV-${D}-$(date +%s)"
	REQ=$(( 324 / D ))
	echo
	echo "=============================================================="
	echo "=== POINT isp_div=$D -> isp core ~= ${REQ} MHz ==="
	echo "=============================================================="
	echo "$MARK" > /dev/kmsg
	F0=$(fcount); L0=$(lcount)

	$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t "$SECS" >/tmp/pure.log 2>&1 &
	VP=$!
	sleep "$SETTLE"

	echo "  before write: pll=$(reg 0x2002120) csi=$(reg 0x2003840) isp=$(reg 0x2003860)"
	echo "  isp_div <- $D"
	echo "$D" > "$KDIV" 2>/dev/null || echo "  !! KDIV write failed"
	sleep 1
	echo "  after  write: pll=$(reg 0x2002120) csi=$(reg 0x2003840) isp=$(reg 0x2003860)"
	echo "  debugfs: isp=$(cat /sys/kernel/debug/clk/isp/clk_rate) csi=$(cat /sys/kernel/debug/clk/csi/clk_rate)"

	F1=$(fcount); L1=$(lcount)
	vi
	sleep "$MEAS"
	F2=$(fcount); L2=$(lcount)
	echo "  vi0 frames $F0 -> $F1 -> $F2 (delta $((F2-F1)) in ${MEAS}s = $(( (F2-F1) / MEAS )) fps)"
	echo "  vi0 lost   $L0 -> $L1 -> $L2 (delta $((L2-L1)))"
	vi
	FL=$(dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "frame lost")
	HB=$(dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "hblank short")
	RX=$(dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "reset!!!")
	echo "  dmesg counts: frame_lost=$FL hblank_short=$HB resets=$RX"
	dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -vE "$MARK" | head -5 | sed 's/^/    /'

	wait $VP
	echo "  vfr: $(grep RESULT /tmp/pure.log || echo '<no RESULT>')"

	echo 1 > "$KDIV" 2>/dev/null
	sleep 2
	echo "  restored: pll=$(reg 0x2002120) csi=$(reg 0x2003840) isp=$(reg 0x2003860)"

	if [ "$FL" -gt 0 ] || [ "$HB" -gt 0 ] || [ "$RX" -gt 0 ]; then
		echo
		echo "  *** POINT FAILED (frame_lost=$FL hblank_short=$HB resets=$RX) - stopping."
		break
	fi
done

echo
echo "### done $(date -Is)"
