#!/bin/sh
# t14probe.sh <label> [secs]
# Board side: collect register + media + counter evidence for the second VI
# channel (/dev/video4, vinc4 -> vipp1) on the A733.
#   A) /dev/video4 alone
#   B) /dev/video0 alone (control)
#   C) /dev/video0 in the background + /dev/video4
# needs /tmp/vinreg (tools/vinreg.c).
LABEL=${1:-t14}
SECS=${2:-12}
VFR=/home/orangepi/ar0234test/vfr
OUT=/tmp/$LABEL.out
exec >"$OUT" 2>&1

CSIC=0x5800800
V0=0x5910000
V1=0x5910400

vin0() {   # viN counters
	awk -v n="$1" '$0 ~ "^vi" n ":" {f=1} f {print} f && /^\*\*\*/ {exit}' \
		/sys/kernel/debug/mpp/vi | grep -E "frame =>|input =>|output =>|prs_in|=> mipi|interface"
}
regs() {
	echo "--- CSIC TOP gen:      $(/tmp/vinreg r $CSIC 1)"
	echo "--- CSIC VIPP IN 0xa0: $(/tmp/vinreg r 0x58008a0 1 | sed 's/.*= //') $(/tmp/vinreg r 0x58008a4 1 | sed 's/.*= //') $(/tmp/vinreg r 0x58008a8 1 | sed 's/.*= //') $(/tmp/vinreg r 0x58008ac 1 | sed 's/.*= //')"
	echo "--- VIPP0 top 0x00-0x3c (nonzero):"; /tmp/vinreg s $V0 0x40
	echo "--- VIPP0 load 0x200-0x22c:";      /tmp/vinreg r 0x5910200 12
	echo "--- VIPP1 top 0x00-0x3c (nonzero):"; /tmp/vinreg s $V1 0x40
	echo "--- VIPP1 load 0x200-0x22c:";      /tmp/vinreg r 0x5910600 12
}
scalers() {
	media-ctl -d /dev/media0 -p | awk '/entity .*sunxi_isp.0|entity .*sunxi_scaler|pad[0-9]:|fmt:|<-|->/' |
		sed -n '/sunxi_isp.0/,$p'
}

echo "########## $LABEL  uptime=$(cut -d' ' -f1 /proc/uptime)"
MARK="$LABEL-$(date +%s)"
echo "$MARK" > /dev/kmsg
kcount() { dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "$1"; }

echo "===== step A: /dev/video4 ALONE ====="
echo "-- vi0 before: $(vin0 0 | tr '\n' '|')"
$VFR -d /dev/video4 -w 1920 -h 1200 -f NV12 -p 1/120 -t $SECS > /tmp/${LABEL}.a4.log 2>&1 &
P=$!
sleep 5
regs
wait $P
echo "-- A video4: $(cat /tmp/${LABEL}.a4.log | grep RESULT)"
echo "-- vi4 : $(vin0 4 | tr '\n' '|')"

echo "===== step B: /dev/video0 ALONE (control) ====="
$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t $SECS > /tmp/${LABEL}.b0.log 2>&1 &
P=$!
sleep 5
regs
wait $P
echo "-- B video0: $(cat /tmp/${LABEL}.b0.log | grep RESULT)"
echo "-- vi0 : $(vin0 0 | tr '\n' '|')"

echo "===== step C: video0 background + video4 ====="
$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t $((SECS*2)) > /tmp/${LABEL}.c0.log 2>&1 &
P0=$!
sleep 3
$VFR -d /dev/video4 -w 1920 -h 1200 -f NV12 -p 1/120 -t $SECS > /tmp/${LABEL}.c4.log 2>&1 &
P4=$!
sleep 5
regs
scalers
wait $P4; wait $P0
echo "-- C video0: $(cat /tmp/${LABEL}.c0.log | grep RESULT)"
echo "-- C video4: $(cat /tmp/${LABEL}.c4.log | grep RESULT)"
echo "-- vi0 : $(vin0 0 | tr '\n' '|')"
echo "-- vi4 : $(vin0 4 | tr '\n' '|')"

echo "===== dmesg since $MARK ====="
for p in "frame lost" "sunxi_isp_reset" "configuration error" "width error" "not mapped" "CSI module" "sunxi_iommu" "Oops" "BUG:" "WARNING:" "Call trace" "get_selection" "already stream off" "underflow" "cannot be close"; do
	printf '%-22s %s\n' "$p" "$(kcount "$p")"
done
dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -Ev '\[ar0234_mipi\]|frame lost|sunxi_isp_reset' | tail -25
echo "########## done $MARK"
