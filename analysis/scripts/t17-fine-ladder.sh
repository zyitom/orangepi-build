#!/bin/sh
# fine-ladder.sh - the ISP-core ladder the measurement really wants.
#
# Trick: the VIN driver re-writes clk_set_rate(isp, 324 MHz) on every stream-on,
# and that request drags pll-video0-4x back to 324 MHz.  If the parent is already
# 648 MHz *and* the ISP divider is already 2, then 648/2 = 324 and the driver's
# write becomes a no-op - the parent stays at 648 and the CSI clock (parent 648,
# divider 2) stays at 324 too.  After that, ONLY the ISP divider has to move to
# put the ISP core anywhere on 648/D:
#
#   D=2 -> 324   D=3 -> 216   D=4 -> 162   D=6 -> 108   D=8 -> 81 MHz
#
# All setup happens with no stream open, so nothing is retuned mid-frame except
# the single ISP divider per measured point.
set -u

SECS=${SECS:-16}
MEAS=${MEAS:-8}
VFR=/home/orangepi/ar0234test/vfr
KISP=/sys/module/ispclk_scan/parameters/set_rate
KDIV=/sys/module/ispclk_scan/parameters/isp_div
KCDIV=/sys/module/ispclk_scan/parameters/csi_div
OUT=${OUT:-/tmp/fine-ladder.out}
R=/tmp/vinreg

exec >"$OUT" 2>&1

reg() { $R r $1 2>/dev/null | sed 's/.*= //'; }
fcount() { awk '/^vi0:/{f=1} f && /frame =>/{gsub(/,/,"");print $4+0; exit}' /sys/kernel/debug/mpp/vi; }
lcount() { awk '/^vi0:/{f=1} f && /frame =>/{gsub(/,/,"");print $6+0; exit}' /sys/kernel/debug/mpp/vi; }
vi() { awk '/^vi0:/{f=1} f && /frame =>|internal =>|prs_in/{print "      " $0} f && /^\*\*\*/{exit}' /sys/kernel/debug/mpp/vi; }

echo "### fine-ladder start=$(date -Is) secs=$SECS meas=$MEAS"
echo "### uptime=$(cut -d' ' -f1 /proc/uptime) load=$(cut -d' ' -f1-3 /proc/loadavg)"
echo "### frame-lost in dmesg at start: $(dmesg | grep -c 'frame lost')"

echo
echo "### warm-up (throwaway, also lets the driver do its one-time clock setup)"
$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 10 2>&1 | grep RESULT | sed 's/^/###   /'
sleep 3
echo "### after warm-up: pll=$(reg 0x2002120) csi=$(reg 0x2003840) isp=$(reg 0x2003860) frame_lost=$(dmesg | grep -c 'frame lost')"

echo
echo "### SETUP (no stream open): parent -> 648, csi_div -> 2, isp_div -> 2"
echo 648000000 > "$KISP" 2>/dev/null
sleep 1
echo 2 > "$KCDIV" 2>/dev/null
sleep 1
echo 2 > "$KDIV" 2>/dev/null
sleep 1
echo "### setup result: pll=$(reg 0x2002120) csi=$(reg 0x2003840) isp=$(reg 0x2003860)"
echo "###   (expect pll d4x=2 -> 648 MHz parent, csi M=2 -> 324, isp M=2 -> 324)"

for D in "$@"; do
	MARK="FINE-${D}-$(date +%s)"
	REQ=$(( 648 / D ))
	echo
	echo "=============================================================="
	echo "=== POINT isp_div=$D -> isp core ~= ${REQ} MHz (parent 648, csi 324) ==="
	echo "=============================================================="
	echo "$MARK" > /dev/kmsg
	F0=$(fcount); L0=$(lcount)

	$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t "$SECS" >/tmp/fine.log 2>&1 &
	VP=$!
	sleep 5

	# first stream of the run: check the driver did NOT undo the setup
	PRE_PLL=$(reg 0x2002120); PRE_CSI=$(reg 0x2003840); PRE_ISP=$(reg 0x2003860)
	echo "  after stream-on: pll=$PRE_PLL csi=$PRE_CSI isp=$PRE_ISP"
	if [ "$PRE_PLL" != "0xfd123500" ] || [ "$PRE_CSI" != "0x85000001" ]; then
		echo "  *** driver undid the setup (pll/csi) -> point not isolated, still reporting"
	fi

	echo "  isp_div <- $D"
	echo "$D" > "$KDIV" 2>/dev/null || echo "  !! KDIV failed"
	sleep 1
	echo "  after write:    pll=$(reg 0x2002120) csi=$(reg 0x2003840) isp=$(reg 0x2003860)"

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
	dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -vE "$MARK" | head -4 | sed 's/^/    /'

	wait $VP
	echo "  vfr: $(grep RESULT /tmp/fine.log || echo '<no RESULT>')"

	echo 2 > "$KDIV" 2>/dev/null
	sleep 2

	if [ "$FL" -gt 0 ] || [ "$HB" -gt 0 ] || [ "$RX" -gt 0 ]; then
		echo
		echo "  *** POINT FAILED (frame_lost=$FL hblank_short=$HB resets=$RX) - stopping."
		break
	fi
done

echo
echo "### final: pll=$(reg 0x2002120) csi=$(reg 0x2003840) isp=$(reg 0x2003860)"
echo "### done $(date -Is)"
