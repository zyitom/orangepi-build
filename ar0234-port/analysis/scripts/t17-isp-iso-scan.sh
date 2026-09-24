#!/bin/sh
# isp-iso-scan.sh "I:C:P ..." - per point: pin the CSI clock at C MHz, run the ISP
# core at I MHz, stream 1920x1200@120 for a few seconds and see whether the ISP
# keeps up.
#
# P is the intermediate rate used to establish the shared parent pll-video0-4x,
# chosen so that both C and I are exact integer divisors of it (so the clock
# framework uses each clock's own M divider and never has to retune the parent
# again - that is what keeps CSI and ISP independent).
#
# Runs on the board as root.  Each point gets its own vfr run, so every
# measurement starts from a freshly opened stream (the driver reprograms the
# clocks at stream-on, so the knobs are written after the stream is up).
#
#   MODE: use "top" points first (safe), then "low" points, because a point that
#   fails leaves the ISP dead until a reboot (HANDOFF 3.1/3.2).
set -u

SECS=${SECS:-14}		# vfr run length per point
SETTLE=${SETTLE:-4}		# seconds to let the stream stabilise before stepping
MEAS=${MEAS:-8}			# seconds of measurement at the stepped clock
VFR=/home/orangepi/ar0234test/vfr
KISP=/sys/module/ispclk_scan/parameters/set_rate
KCSI=/sys/module/ispclk_scan/parameters/set_rate_csi
OUT=${OUT:-/tmp/isp-iso.out}

R=/tmp/vinreg
[ -x "$R" ] || R=/home/orangepi/ar0234test/vinreg

exec >"$OUT" 2>&1

isp_clk() { cat /sys/kernel/debug/clk/isp/clk_rate; }
csi_clk() { cat /sys/kernel/debug/clk/csi/clk_rate; }
vi() { awk -v n="$1" '$0 ~ "^vi" n ":" {f=1} f && /frame =>|internal/ {print "      " $0} f && /^\*\*\*/ {exit}' /sys/kernel/debug/mpp/vi; }
cnt() { awk -v n="$1" '/^vi'"$1"':/{f=1} f && /frame =>/{gsub(/,/,"");print $4+0; exit}' /sys/kernel/debug/mpp/vi; }
lcnt() { awk -v n="$1" '/^vi'"$1"':/{f=1} f && /frame =>/{gsub(/,/,"");print $6+0; exit}' /sys/kernel/debug/mpp/vi; }

echo "### isp-iso-scan start=$(date -Is) secs=$SECS settle=$SETTLE meas=$MEAS"
echo "### uptime=$(cut -d' ' -f1 /proc/uptime) 3A=$(systemctl is-active ar0234-3ad)"
echo "### module srcversion=$(cat /sys/module/vin_v4l2/srcversion) ispclk=$(lsmod | awk '/^ispclk/{print $3}')"
echo "### baseline clocks: isp=$(isp_clk) csi=$(csi_clk)"
echo "### load: $(cut -d' ' -f1-3 /proc/loadavg)"

# Warm-up: the first 1200p120 stream after a fresh boot was measured to fail
# with a frame-lost storm at uptime 51 s while every run at uptime > 140 s was
# clean, so burn one throwaway stream first and report it.
if [ "${WARMUP:-1}" = 1 ]; then
	echo "### warm-up stream (throwaway)"
	$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 8 2>&1 | grep RESULT | sed 's/^/###   /'
	sleep 2
	echo "### after warm-up: isp=$(isp_clk) csi=$(csi_clk)"
	WFL=$(dmesg | grep -c "frame lost")
	echo "### frame-lost lines in dmesg so far: $WFL"
fi

for POINT in "$@"; do
	I=$(echo "$POINT" | cut -d: -f1)
	C=$(echo "$POINT" | cut -d: -f2)
	P=$(echo "$POINT" | cut -d: -f3)

	# Simple spec ("162"): only move the ISP clock and rely on the framework
	# picking the ISP's own M divider.  That keeps pll-video0-4x (and therefore
	# the CSI clock) exactly where it is and avoids retuning the PLL while the
	# ISP is running, which is what a *clean* measurement needs.  Only rates
	# the current parent divides exactly qualify (324, 162, 108, 81, 54 ...).
	SIMPLE=0
	[ -z "$C" ] && SIMPLE=1

	echo
	echo "==============================================================="
	if [ "$SIMPLE" = 1 ]; then
		echo "=== POINT isp=${I}MHz  (divider-only, csi must stay put) ==="
	else
		echo "=== POINT isp=${I}MHz csi=${C}MHz parent-setup=${P} ==="
	fi
	echo "==============================================================="
	MARK="ISPO-${I}-${C}-$(date +%s)"
	echo "$MARK" > /dev/kmsg

	F0=$(cnt 0); L0=$(lcnt 0)
	echo "  counters before: frame=$F0 lost=$L0"

	$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t "$SECS" >/tmp/isop.log 2>&1 &
	VP=$!
	sleep "$SETTLE"

	if [ "$SIMPLE" = 1 ]; then
		echo "  isp <- $I (divider only, no PLL retune)"
		if ! echo "$I"000000 > "$KISP"; then echo "  !! KISP write failed (rc=$?)"; fi
		sleep 1
	else
		# 1) establish the shared parent, 2) pin CSI, 3) put the ISP core where we want
		echo "  step1 parent <- $P   (before: isp=$(isp_clk) csi=$(csi_clk))"
		if ! echo "$P"000000 > "$KISP"; then echo "  !! KISP write failed (rc=$?)"; fi
		sleep 1
		echo "  step2 csi    <- $C   (before: isp=$(isp_clk) csi=$(csi_clk))"
		if ! echo "$C"000000 > "$KCSI"; then echo "  !! KCSI write failed (rc=$?)"; fi
		sleep 1
		echo "  step3 isp    <- $I   (before: isp=$(isp_clk) csi=$(csi_clk))"
		if ! echo "$I"000000 > "$KISP"; then echo "  !! KISP write failed (rc=$?)"; fi
		sleep 1
	fi
	echo "  >>> MEASURED AT: isp=$(isp_clk) csi=$(csi_clk)"
	echo "  clocks now: isp=$(isp_clk) csi=$(csi_clk)   (idle-only regs:)"
	$R r 0x2002120 | sed 's/^/    /'
	$R r 0x2003840 | sed 's/^/    /'
	$R r 0x2003860 | sed 's/^/    /'

	F1=$(cnt 0); L1=$(lcnt 0)
	echo "  counters at step-in: frame=$F1 lost=$L1"
	vi 0

	sleep "$MEAS"

	F2=$(cnt 0); L2=$(lcnt 0)
	echo "  counters after ${MEAS}s: frame=$F2 lost=$L2  (delta frame=$((F2-F1)) lost=$((L2-L1)))"
	vi 0
	echo "  dmesg since marker:"
	dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -vE "^\[.*\] $MARK" | head -8 | sed 's/^/    /'
	echo "  counts: frame_lost=$(dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "frame lost")  hblank_short=$(dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "hblank short")  resets=$(dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "reset!!!")"

	wait $VP
	echo "  vfr: $(grep RESULT /tmp/isop.log || echo '<no RESULT line>')"

	FL=$(dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "frame lost")
	HB=$(dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "hblank short")
	RX=$(dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "reset!!!")

	# back to the known-good clock for the next point
	echo 324000000 > "$KCSI" 2>/dev/null
	echo 324000000 > "$KISP" 2>/dev/null
	sleep 2
	echo "  restored: isp=$(isp_clk) csi=$(csi_clk)"

	if [ "$FL" -gt 0 ] || [ "$HB" -gt 0 ] || [ "$RX" -gt 0 ]; then
		echo
		echo "  *** POINT FAILED (frame_lost=$FL hblank_short=$HB resets=$RX)"
		echo "  *** stopping here: the driver cannot reset the ISP without a"
		echo "  *** reboot (HANDOFF 3.1/3.16), further points would be invalid."
		break
	fi
done

echo
echo "### final: isp=$(isp_clk) csi=$(csi_clk)"
echo "### done $(date -Is)"
