#!/bin/sh
# runs on the board as root.  CCU base = 0x2002000 (DT ccu@2002000).
#   0x2002120 PLL_VIDEO0_CTRL : N bits8..15 (=24MHz*(N+1)), /3x bits16..18, /4x bits20..22
#   0x2003840 CSI_CLK_REG     : M bits0..4, mux bits24..26, gate bit31
#   0x2003860 ISP_CLK_REG     : M bits0..4, mux bits24..26, gate bit31
#   parents(index): isp = {v2-4x, p480, p400, p600, v0-4x, v1-4x}
#                   csi = {v2-4x, de-4x, p480, p400, p600, v0-4x, v1-4x}
R=/tmp/vinreg
[ -x "$R" ] || R=/home/orangepi/ar0234test/vinreg
VFR=/home/orangepi/ar0234test/vfr
KNOB=/sys/module/ispclk_scan/parameters/set_rate

reg() { $R r $1 | sed "s/.*= 0x//" | tr -d ' '; }

show() {
	ph=$(reg 0x2002120); ch=$(reg 0x2003840); mh=$(reg 0x2003860)
	P=$(printf '%d' "0x$ph"); C=$(printf '%d' "0x$ch"); M=$(printf '%d' "0x$mh")
	pv0=$(( (P >> 8 & 0xff) + 1 )); d3=$(( ((P >> 16) & 0x7) + 1 )); d4=$(( ((P >> 20) & 0x7) + 1 ))
	im=$(( (M & 0x1f) + 1 )); imux=$(( (M >> 24) & 0x7 )); ig=$(( (M >> 31) & 1 ))
	cm=$(( (C & 0x1f) + 1 )); cmux=$(( (C >> 24) & 0x7 )); cg=$(( (C >> 31) & 1 ))
	echo "  [$1] pll-video0=$((24*pv0/1000))MHz($((24*pv0))) d3x=$d3 d4x=$d4 | isp: M=$im mux=$imux g=$ig =$((24*pv0/d4/im))MHz | csi: M=$cm mux=$cmux g=$cg =$((24*pv0/d4/cm))MHz"
	echo "        regs: pll=$ph csi=$ch isp=$mh  debugfs isp=$(cat /sys/kernel/debug/clk/isp/clk_rate)"
}

echo "=== 1. idle ==="
show idle

echo
echo "=== 2. knob -> 600000000 (no stream) ==="
echo 600000000 > $KNOB
sleep 1
show knob600

echo
echo "=== 3. knob -> 540000000 (no stream) ==="
echo 540000000 > $KNOB
sleep 1
show knob540

echo
echo "=== 4. start vfr (stream on) ==="
$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 16 >/tmp/regrun.log 2>&1 &
P=$!
sleep 5
show streaming
echo
echo "=== 5. knob -> 162000000 WHILE streaming ==="
echo 162000000 > $KNOB
sleep 1
show stream162
sleep 3
show stream162b
echo
echo "=== 6. knob back to 324000000 WHILE streaming ==="
echo 324000000 > $KNOB
sleep 3
show stream324
wait $P
echo
echo "=== vfr result ==="
cat /tmp/regrun.log
echo
echo "=== 7. after stream off ==="
show after
echo
echo "=== dmesg since start ==="
dmesg | tail -20
