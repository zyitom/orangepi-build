#!/bin/sh
# isp-iso2.sh "s162" "f486:324:972" ... - measure what the ISP core can carry.
#
# Background (all measured, see analysis/t17/REPORT*):
#   * the running DTB has csi_isp = <324000000> and csi_top = <324000000>, and
#     the VIN driver re-applies exactly that on every stream-on, so the ISP core
#     clock and the CSI/parser clock both sit at 324 MHz in normal operation;
#   * isp (0x1860) and csi (0x1840) are two M dividers on the same parent
#     pll-video0-4x, so a rate the current parent divides exactly is satisfied
#     with the ISP's own divider and leaves the CSI clock alone -> that is the
#     only way to attribute a failure to the ISP core rather than the parser.
#
# Point spec:
#   s<N>            isp core <- N MHz via its own divider only, CSI must stay 324
#   f<I>:<C>:<P>    set parent <- P, csi <- C, then isp <- I  (for I that the
#                   current parent cannot reach with a divider)
#
# Every point gets a fresh stream (the driver reprograms the clocks at stream-on,
# so each transition is 324 -> target, identical in character across points).
# Stops at the first failure: the driver cannot reset the ISP without a reboot.
set -u

SECS=${SECS:-14}
SETTLE=${SETTLE:-4}
MEAS=${MEAS:-8}
VFR=/home/orangepi/ar0234test/vfr
KISP=/sys/module/ispclk_scan/parameters/set_rate
KCSI=/sys/module/ispclk_scan/parameters/set_rate_csi
OUT=${OUT:-/tmp/isp-iso2.out}
WANT_CSI=324000000

exec >"$OUT" 2>&1

R=/tmp/vinreg
[ -x "$R" ] || R=/home/orangepi/ar0234test/vinreg

isp_clk() { cat /sys/kernel/debug/clk/isp/clk_rate; }
csi_clk() { cat /sys/kernel/debug/clk/csi/clk_rate; }
vi() { awk '/^vi0:/{f=1} f && /frame =>|internal =>|prs_in/{print "      " $0} f && /^\*\*\*/{exit}' /sys/kernel/debug/mpp/vi; }
cnt() { awk '/^vi0:/{f=1} f && /frame =>/{gsub(/,/,"");print $4+0; exit}' /sys/kernel/debug/mpp/vi; }

echo "### isp-iso2 start=$(date -Is) secs=$SECS settle=$SETTLE meas=$MEAS"
echo "### uptime=$(cut -d' ' -f1 /proc/uptime) load=$(cut -d' ' -f1-3 /proc/loadavg) 3A=$(systemctl is-active ar0234-3ad)"
echo "### frame-lost lines in dmesg at start: $(dmesg | grep -c 'frame lost')"

echo
echo "### warm-up stream (throwaway; the first 1200p120 stream after a fresh"
echo "### boot was measured to fail while later ones are clean)"
$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 10 2>&1 | grep RESULT | sed 's/^/###   /'
sleep 3
echo "### after warm-up: isp=$(isp_clk) csi=$(csi_clk) frame_lost=$(dmesg | grep -c 'frame lost')"

for POINT in "$@"; do
	MARK="ISPO2-${POINT}-$(date +%s)"
	KIND=$(echo "$POINT" | cut -c1)

	echo
	echo "=============================================================="
	echo "=== POINT $POINT (kind=$KIND) ==="
	echo "=============================================================="
	echo "$MARK" > /dev/kmsg
	F0=$(cnt)

	$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t "$SECS" >/tmp/iso2.log 2>&1 &
	VP=$!
	sleep "$SETTLE"

	if [ "$KIND" = s ]; then
		TARGET=$(echo "$POINT" | cut -c2-)
		echo "  isp <- ${TARGET} (divider only)"
		echo "$TARGET"000000 > "$KISP" 2>/dev/null || echo "  !! KISP write failed"
		sleep 1
	else
		I=$(echo "$POINT" | cut -c2- | cut -d: -f1)
		C=$(echo "$POINT" | cut -d: -f2)
		P=$(echo "$POINT" | cut -c2- | cut -d: -f3)
		echo "  parent <- $P"
		echo "$P"000000 > "$KISP" 2>/dev/null || echo "  !! KISP(P) write failed"
		sleep 1
		echo "  csi <- $C"
		echo "$C"000000 > "$KCSI" 2>/dev/null || echo "  !! KCSI write failed"
		sleep 1
		echo "  isp <- $I"
		echo "$I"000000 > "$KISP" 2>/dev/null || echo "  !! KISP(I) write failed"
		sleep 1
	fi

	AISP=$(isp_clk); ACSI=$(csi_clk)
	echo "  >>> MEASURED AT: isp=$AISP csi=$ACSI"
	if [ "$ACSI" != "$WANT_CSI" ]; then
		echo "  *** WARNING: csi moved off $WANT_CSI -> this point is NOT ISP-isolated"
	fi
	$R r 0x2002120 2>/dev/null | sed 's/^/    /'
	$R r 0x2003860 2>/dev/null | sed 's/^/    /'

	F1=$(cnt)
	vi
	sleep "$MEAS"
	F2=$(cnt)
	echo "  vi0 frames: $F0 -> $F1 -> $F2  (delta in ${MEAS}s+: $((F2-F1)))"
	vi
	FL=$(dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "frame lost")
	HB=$(dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "hblank short")
	RX=$(dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "reset!!!")
	echo "  counts: frame_lost=$FL hblank_short=$HB resets=$RX"
	dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -vE "$MARK" | head -6 | sed 's/^/    /'

	wait $VP
	echo "  vfr: $(grep RESULT /tmp/iso2.log || echo '<no RESULT>')"

	echo 324000000 > "$KCSI" 2>/dev/null
	echo 324000000 > "$KISP" 2>/dev/null
	sleep 2
	echo "  restored: isp=$(isp_clk) csi=$(csi_clk)"

	if [ "$FL" -gt 0 ] || [ "$HB" -gt 0 ] || [ "$RX" -gt 0 ]; then
		echo
		echo "  *** POINT FAILED (frame_lost=$FL hblank_short=$HB resets=$RX) - stopping."
		break
	fi
done

echo
echo "### final: isp=$(isp_clk) csi=$(csi_clk)"
echo "### done $(date -Is)"
