#!/bin/sh
# s6: post-reboot evidence. Boot snapshot, health, 1080p ceiling, dual stream,
# open/close regression (0009), same-size limitation, live register views.
V=/home/orangepi/ar0234test/vfr
SUDO="printf ' \n' | sudo -S -p ''"
vic() { sed -n "/^$1:/,/^\*\*\*/p" /sys/kernel/debug/mpp/vi | grep -E 'frame =>|prs_in|output =>|bkuf' | tr -s ' ' | tr '\n' '|'; }
dm() { dmesg | awk -v m="$1" '$0 ~ m {f=1} f'; }
cnt() { E=$(dm "$1"); printf 'frame_lost=%s hblank=%s reset=%s not_mapped=%s oops=%s warn=%s csi_iommu=%s lines=%s' \
	"$(echo "$E" | grep -c 'frame lost')" "$(echo "$E" | grep -ci 'hblank short')" \
	"$(echo "$E" | grep -ci 'sunxi_isp_reset')" "$(echo "$E" | grep -c 'is not mapped')" \
	"$(echo "$E" | grep -ci 'oops\|panic')" "$(echo "$E" | grep -ci 'WARNING:')" \
	"$(echo "$E" | grep -ci 'sunxi_iommu')" "$(echo "$E" | grep -c .)"; }
mark() { echo "S6-$1-$$" > /dev/kmsg; echo "S6-$1-$$"; }

echo "########## 0. boot dmesg snapshot -> /tmp/dmesg-boot-audit.txt"
dmesg > /tmp/dmesg-boot-audit.txt
wc -l /tmp/dmesg-boot-audit.txt
grep -Ei 'isp0|vin|sensor|not used|warn|err|reset' /tmp/dmesg-boot-audit.txt | grep -vi 'mmc\|uart\|usb' | head -40

echo "########## 1. health: 1200p120 30 s"
M=$(mark health)
$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -t 30 -p 1/120 2>&1 | grep -E 'RESULT|GAPS' | sed 's/^/  /'
echo "  counters: $(cnt $M)"
echo "  vi0: $(vic vi0)"

echo "########## 2. 1080p ceiling ladder (driver's own fll)"
for fps in 130 132 133 134 135 136; do
	M=$(mark fll$fps)
	R=$($V -d /dev/video0 -w 1920 -h 1080 -f NV12 -t 3 -p 1/$fps 2>&1 | grep -E 'RESULT' | head -1)
	FLL=$(dm "$M" | grep -o 'fll = [0-9]* ([0-9]* fps)' | head -1)
	printf '  fps=%-4s %s | %s\n' "$fps" "$FLL" "$R"
	printf '      %s\n' "$(cnt $M)"
done

echo "########## 3. dual stream video0 + video4, 20 s"
M=$(mark dual)
$V -d /dev/video4 -w 1920 -h 1200 -f NV12 -t 20 -p 1/120 >/tmp/s6.v4 2>&1 &
P4=$!
sleep 2
$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -t 20 -p 1/120 >/tmp/s6.v0 2>&1
wait $P4
echo "  --- video0 ---"; grep -E 'RESULT|GAPS' /tmp/s6.v0 | sed 's/^/  /'
echo "  --- video4 ---"; grep -E 'RESULT|GAPS' /tmp/s6.v4 | sed 's/^/  /'
echo "  counters since dual marker: $(cnt $M)"
echo "  vi0: $(vic vi0)"
echo "  vi4: $(vic vi4)"
echo "  --- mpp/vi full during idle-after ---"; cat /sys/kernel/debug/mpp/vi

echo "########## 4. live MIPI PHY + VIN registers during a stream"
$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -t 12 -p 1/120 >/tmp/s6.live 2>&1 &
PL=$!
sleep 4
echo "  --- /sys/kernel/debug/mpp/mipi while streaming ---"
cat /sys/kernel/debug/mpp/mipi | sed 's/^/  /'
echo "  --- vi0 while streaming ---"; vic vi0
wait $PL
grep RESULT /tmp/s6.live | sed 's/^/  /'

echo "########## 5. T16b regression: open+close video4 (no stream) while video0 streams"
M=$(mark openclose)
$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -t 30 -p 1/120 >/tmp/s6.oc 2>&1 &
PO=$!
sleep 8
B=$(vic vi0); echo "  vi0 before open/close video4: $B"
python3 -c "
import os,time
fd=os.open('/dev/video4',os.O_RDWR); time.sleep(0.2); os.close(fd)
print('  opened+closed /dev/video4 once')
"
sleep 8
echo "  vi0 8s later: $(vic vi0)"
wait $PO
grep -E 'RESULT' /tmp/s6.oc | sed 's/^/  /'
echo "  counters since openclose marker: $(cnt $M)"
echo "  dmesg new lines:"; dm "$M" | grep -v '^$' | head -10 | sed 's/^/  ! /'

echo "########## 6. same-size limitation: S_FMT 960x600 on video4 while video0 streams"
M=$(mark sizelock)
$V -d /dev/video0 -w 1920 -h 1200 -f NV12 -t 14 -p 1/120 >/tmp/s6.sz 2>&1 &
PS=$!
sleep 5
printf ' \n' | sudo -S -p '' v4l2-ctl -d /dev/video4 --set-fmt-video=width=960,height=600,pixelformat=NV12 2>&1 | sed 's/^/  S_FMT video4 960x600: /'
echo "  rc=$?"
echo "  vi0 during: $(vic vi0)"
wait $PS
grep RESULT /tmp/s6.sz | sed 's/^/  /'
echo "  dmesg:"; dm "$M" | grep -Ei 'refus|busy|err' | head -5 | sed 's/^/  ! /'

echo "########## 7. state"
echo "  isp clk: $(cat /sys/kernel/debug/clk/isp/clk_rate)"
echo "  dtb: $(md5sum /boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb)"
rm -f /tmp/s6.v0 /tmp/s6.v4 /tmp/s6.live /tmp/s6.oc /tmp/s6.sz
echo "########## DONE6"
