#!/bin/sh
# isp-div-scan.sh "2 3 4 6" - ISP-core-only clock ladder using raw dividers.
#
# Why raw dividers: clk_set_rate() may retune pll-video0 to hit a request, and a
# mid-stream PLL retune was measured to trip a spurious "hblank short" reset
# (486 MHz failed while 648 MHz passed).  Writing the divider field keeps
# pll-video0 and its /4 divider exactly where they are.
#
# Setup for every point (after the driver has programmed its own 324/324):
#   1. set_rate    = 648000000  -> pll-video0-4x 324 -> 648 (divider only, PLL
#                                  stays 1296); isp M=1 -> 648, csi M=1 -> 648
#   2. csi_div     = 2          -> csi = 324 again  (parser clock unchanged)
#   3. isp_div     = D          -> isp core = 648/D  (the variable under test)
#
# D: 1->648  2->324  3->216  4->162  5->129.6  6->108  8->81  12->54 MHz
set -u

SECS=${SECS:-14}
SETTLE=${SETTLE:-4}
MEAS=${MEAS:-8}
VFR=/home/orangepi/ar0234test/vfr
KISP=/sys/module/ispclk_scan/parameters/set_rate
KDIV=/sys/module/ispclk_scan/parameters/isp_div
KCDIV=/sys/module/ispclk_scan/parameters/csi_div
OUT=${OUT:-/tmp/isp-div.out}

R=/tmp/vinreg
[ -x "$R" ] || R=/home/orangepi/ar0234test/vinreg

exec >"$OUT" 2>&1

reg() { $R r "$1" 2>/dev/null | sed 's/.*= 0x//'; }
isp_mhz() {	# isp divider field of 0x2003860 -> MHz
	v=$(reg 0x2003860); n=$(printf '%d' "0x$v")
	d=$(( (n & 0x1f) + 1 )); p=$(( (($(printf '%d' "0x$(reg 0x2002120)")) >> 20 & 0x7) + 1 ))
	echo $(( (24 * ((($(printf '%d' "0x$(reg 0x2002120)")) >> 8 & 0xff) + 1)) / p / d ))
}
vi() { awk '/^vi0:/{f=1} f && /frame =>|internal =>|prs_in/{print "      " $0} f && /^\*\*\*/{exit}' /sys/kernel/debug/mpp/vi; }
cnt() { awk '/^vi0:/{f=1} f && /frame =>/{gsub(/,/,"");print $4+0; exit}' /sys/kernel/debug/mpp/vi; }

echo "### isp-div-scan start=$(date -Is) secs=$SECS meas=$MEAS"
echo "### uptime=$(cut -d' ' -f1 /proc/uptime) load=$(cut -d' ' -f1-3 /proc/loadavg)"
echo "### frame-lost in dmesg at start: $(dmesg | grep -c 'frame lost')"

echo
echo "### warm-up (throwaway)"
$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 10 2>&1 | grep RESULT | sed 's/^/###   /'
sleep 3
echo "### frame-lost after warm-up: $(dmesg | grep -c 'frame lost')"

for D in "$@"; do
	MARK="ISPDIV-${D}-$(date +%s)"
	echo
	echo "=============================================================="
	echo "=== POINT isp_div=$D  (isp core = 648/$D)"
	echo "=============================================================="
	TARGET=$(( 648 / D ))
	echo "  target isp ~= ${TARGET} MHz  -> $(( 276500000 / (TARGET * 1000000) * 100 ))/100 cycle/pixel for one 1200p120 stream"
	echo "$MARK" > /dev/kmsg
	F0=$(cnt)

	$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t "$SECS" >/tmp/div.log 2>&1 &
	VP=$!
	sleep "$SETTLE"

	echo "  step1 pll-video0-4x <- 648 (divider change only)"
	echo 648000000 > "$KISP" 2>/dev/null || echo "  !! KISP failed"
	sleep 1
	echo "  step2 csi_div <- 2"
	echo 2 > "$KCDIV" 2>/dev/null || echo "  !! KCDIV failed"
	sleep 1
	echo "  step3 isp_div <- $D"
	echo "$D" > "$KDIV" 2>/dev/null || echo "  !! KDIV failed"
	sleep 1

	echo "  regs: pll-ctrl=0x$(reg 0x2002120) csi=0x$(reg 0x2003840) isp=0x$(reg 0x2003860)"
	echo "  >>> MEASURED: isp core ~= $(isp_mhz) MHz, debugfs isp=$(cat /sys/kernel/debug/clk/isp/clk_rate) csi=$(cat /sys/kernel/debug/clk/csi/clk_rate)"

	F1=$(cnt)
	vi
	sleep "$MEAS"
	F2=$(cnt)
	echo "  vi0 frames: $F0 -> $F1 -> $F2 (delta $((F2-F1)) in ${MEAS}s)"
	vi
	FL=$(dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "frame lost")
	HB=$(dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "hblank short")
	RX=$(dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "reset!!!")
	echo "  counts: frame_lost=$FL hblank_short=$HB resets=$RX"
	dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -vE "$MARK" | head -5 | sed 's/^/    /'

	wait $VP
	echo "  vfr: $(grep RESULT /tmp/div.log || echo '<no RESULT>')"

	# restore: dividers back to 1, parent back to 324
	echo 1 > "$KDIV" 2>/dev/null
	echo 1 > "$KCDIV" 2>/dev/null
	echo 324000000 > "$KISP" 2>/dev/null
	sleep 2
	echo "  restored: regs pll=0x$(reg 0x2002120) csi=0x$(reg 0x2003840) isp=0x$(reg 0x2003860)"

	if [ "$FL" -gt 0 ] || [ "$HB" -gt 0 ] || [ "$RX" -gt 0 ]; then
		echo
		echo "  *** POINT FAILED (frame_lost=$FL hblank_short=$HB resets=$RX) - stopping."
		break
	fi
done

echo
echo "### done $(date -Is)"
