#!/bin/sh
# t14dual.sh - both VI channels of the *same* sensor at once, with the VIPP1
# input-select workaround applied live (0x58008a4 := 0).
VFR=/home/orangepi/ar0234test/vfr
OUT=/tmp/t14dual.out
exec >"$OUT" 2>&1
MARK="t14dual-$(date +%s)"; echo "$MARK" > /dev/kmsg
A=/tmp/cam0.log; B=/tmp/cam4.log
$VFR -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 30 > $A 2>&1 & P0=$!
sleep 3
$VFR -d /dev/video4 -w 1920 -h 1200 -f NV12 -p 1/120 -t 24 > $B 2>&1 & P4=$!
sleep 4
echo "-- regs before fixup: $(/tmp/vinreg r 0x58008a0 4 | tr '\n' ' ')"
/tmp/vinreg w 0x58008a4 0
echo "-- regs after  fixup: $(/tmp/vinreg r 0x58008a0 4 | tr '\n' ' ')"
sleep 6
for n in 0 4; do
  echo "-- vi$n: $(awk -v n=$n '$0 ~ "^vi" n ":" {f=1} f {print} f && /^\*\*\*/ {exit}' /sys/kernel/debug/mpp/vi | grep -E 'frame =>|prs_in|input =>|output =>')"
done
wait $P4; wait $P0
echo "=== results ==="
echo "video0: $(grep RESULT $A)"; grep GAPS $A
echo "video4: $(grep RESULT $B)"; grep GAPS $B
echo "=== dmesg since $MARK ==="
for p in "frame lost" "sunxi_isp_reset" "not mapped" "CSI module" "sunxi_iommu" "Oops" "BUG:" "WARNING:" "Call trace"; do
  printf '%-18s %s\n' "$p" "$(dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -c "$p")"
done
dmesg | awk -v m="$MARK" '$0 ~ m {f=1} f' | grep -Ev '\[ar0234_mipi\]' | tail -12
