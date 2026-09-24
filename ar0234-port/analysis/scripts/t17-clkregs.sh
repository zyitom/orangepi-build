#!/bin/sh
# runs on the board as root: clock-register evidence for the ISP/CSI clock.
#   PLL_VIDEO0_CTRL 0x0120 : N bits8..15, /3x div bits16..18, /4x div bits20..22
#   CSI_CLK_REG     0x1840 : M bits0..4, mux bits24..26, gate bit31
#   ISP_CLK_REG     0x1860 : M bits0..4, mux bits24..26, gate bit31
# parents(index): isp = {v2-4x, p480, p400, p600, v0-4x, v1-4x}
#                 csi = {v2-4x, de-4x, p480, p400, p600, v0-4x, v1-4x}
echo "=== module params ==="
for p in /sys/module/vin_v4l2/parameters/*; do
	printf '  %-28s = %s\n' "$(basename $p)" "$(cat $p 2>/dev/null)"
done
echo
R=""
[ -x /tmp/vinreg ] && R=/tmp/vinreg
[ -z "$R" ] && [ -x /home/orangepi/ar0234test/vinreg ] && R=/home/orangepi/ar0234test/vinreg
if [ -z "$R" ]; then
	echo "!! no vinreg binary found"
	exit 1
fi
echo "using $R"
echo
decode() {
	p=$(printf '%d' "$1"); m=$(printf '%d' "$2"); c=$(printf '%d' "$3")
	echo "     PLL 0x0120 = 0x$p"
	echo "     CSI 0x1840 = 0x$c"
	echo "     ISP 0x1860 = 0x$m"
	pv0=$(( (0x$p >> 8) & 0xff ))
	d3=$(( ((0x$p >> 16) & 0x7) + 1 ))
	d4=$(( ((0x$p >> 20) & 0x7) + 1 ))
	echo "     => pll-video0 N=$pv0 ($((24 * pv0)) MHz) d3x=$d3 d4x=$d4 -> v0-4x=$((24 * pv0 / d4)) MHz v0-3x=$((24 * pv0 / d3)) MHz"
	im=$(( (0x$m & 0x1f) + 1 )); imux=$(( (0x$m >> 24) & 0x7 )); ig=$(( (0x$m >> 31) & 1 ))
	cm=$(( (0x$c & 0x1f) + 1 )); cmux=$(( (0x$c >> 24) & 0x7 )); cg=$(( (0x$c >> 31) & 1 ))
	echo "     => isp: M=$im mux=$imux gate=$ig   csi: M=$cm mux=$cmux gate=$cg"
	echo "     => isp = $((24 * pv0 / d4 / im)) MHz   csi = $((24 * pv0 / d4 / cm)) MHz"
}
echo "### idle (no stream) ###"
decode "$($R r 0x0120 | sed 's/.*= //')" "$($R r 0x1860 | sed 's/.*= //')" "$($R r 0x1840 | sed 's/.*= //')"
echo
echo "### dmesg: vin clock messages from boot ###"
dmesg | grep -iE "clk rate|isp clk|get core clk|set parent failed|clk prepare" | head -20
