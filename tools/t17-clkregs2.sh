#!/bin/sh
# runs on the board as root: what does the driver actually program for the ISP
# and CSI clocks?  CCU base = 0x2002000 (DT: ccu@2002000).
#   0x2002120 PLL_VIDEO0_CTRL : N bits8..15, /3x bits16..18, /4x bits20..22
#   0x2003840 CSI_CLK_REG     : M bits0..4, mux bits24..26, gate bit31
#   0x2003860 ISP_CLK_REG     : M bits0..4, mux bits24..26, gate bit31
R=/tmp/vinreg
[ -x "$R" ] || R=/home/orangepi/ar0234test/vinreg
VFR=/home/orangepi/ar0234test/vfr

reg() { $R r $1 | sed 's/.*= //'; }

show() {
	echo "--- $1 ---"
	p=$(reg 0x2002120); c=$(reg 0x2003840); m=$(reg 0x2003860)
	echo "  PLL_VIDEO0_CTRL 0x2002120 = $p"
	echo "  CSI_CLK_REG     0x2003840 = $c"
	echo "  ISP_CLK_REG     0x2003860 = $m"
	pv0=$(( (0x$p >> 8) & 0xff )); d3=$(( ((0x$p >> 16) & 0x7) + 1 )); d4=$(( ((0x$p >> 20) & 0x7) + 1 ))
	echo "  decode: pll-video0 N=$pv0 -> $((24*pv0)) MHz, d3x=$d3 d4x=$d4 -> v0-4x=$((24*pv0/d4)) MHz"
	im=$(( (0x$m & 0x1f) + 1 )); imux=$(( (0x$m >> 24) & 0x7 )); ig=$(( (0x$m >> 31) & 1 ))
	cm=$(( (0x$c & 0x1f) + 1 )); cmux=$(( (0x$c >> 24) & 0x7 )); cg=$(( (0x$c >> 31) & 1 ))
	echo "  decode: isp M=$im mux=$imux gate=$ig -> $((24*pv0/d4/im)) MHz | csi M=$cm mux=$cmux gate=$cg -> $((24*pv0/d4/cm)) MHz"
	echo "  debugfs: isp=$(cat /sys/kernel/debug/clk/isp/clk_rate) csi_top=$(cat /sys/kernel/debug/clk/csi/clk_rate 2>/dev/null || echo n/a)"
}

show "IDLE (no stream)"

echo
echo "### starting a 1200p120 stream and re-reading ###"
$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 18 >/tmp/regrun.log 2>&1 &
P=$!
sleep 5
show "STREAMING"
echo
echo "### vfr result ###"
wait $P
cat /tmp/regrun.log

show "AFTER stream off"
