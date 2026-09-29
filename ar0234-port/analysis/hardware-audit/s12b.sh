#!/bin/sh
# s12b: VE encode (H.264/H.265, zero-copy + copy), hardware JPEG, decode round-trip.
s() { printf ' \n' | sudo -S -p '' "$@"; }
REC=/usr/local/bin/ar0234-rec
T=/home/orangepi/ar0234test
tmpf() { awk -v t="$1" -v s="$2" 'BEGIN{printf "%.2f", t-s}'; }

run() { # label codec frames extra_args outfile
	L=$1; C=$2; N=$3; X=$4; O=$5
	M="S12b-$L-$$"; echo $M > /dev/kmsg
	S=$(date +%s.%N)
	s $REC -w 1920 -h 1200 -f 120 -c $C -b 20000000 -n $N $X -o $O 2>&1 | grep -E 'input path|done:|FAIL|error|Error' | sed 's/^/    /'
	E=$(date +%s.%N)
	echo "  [$L] wall=$(tmpf $E $S)s size=$(stat -c%s $O 2>/dev/null || echo NONE)"
	echo "    dmesg-new: $(dmesg | awk -v m="$M" '$0 ~ m {f=1} f' | grep -viE 'ar0234_mipi\]|s_fmt|sensor_s_stream|3DNR|^\[.*\] S12b' | head -3 | tr '\n' '|')"
}

echo "########## 0. modules / nodes"
lsmod | grep -E 'sunxi_ve|cedar' | sed 's/^/  /'
ls -l /dev/cedar_dev /dev/cedar_dev_ve2 | sed 's/^/  /'

echo "########## 1. H.264 1920x1200@120 zero-copy, 240 frames"
run h264zc h264 240 "" /tmp/rec_zc.h264
echo "########## 2. H.265 1920x1200@120 zero-copy, 240 frames"
run h265zc h265 240 "" /tmp/rec_zc.h265
echo "########## 3. H.264 copy mode (-C), 120 frames"
run h264cp h264 120 "-C" /tmp/rec_cp.h264
echo "########## 4. H.264 1080p, 300 frames (reference point)"
run h264_1080 h264 300 "-h 1080" /tmp/rec_1080.h264

echo "########## 5. bitstream sanity (NAL start codes)"
for f in /tmp/rec_zc.h264 /tmp/rec_zc.h265 /tmp/rec_1080.h264; do
	printf '  %-22s size=%-9s first8=%s\n' "$(basename $f)" "$(stat -c%s $f 2>/dev/null)" \
		"$(od -A n -t x1 -N 8 $f 2>/dev/null | tr -s ' ')"
done

echo "########## 6. hardware JPEG (VE2, zero-copy) 1920x1080"
M="S12b-jpeg-$$"; echo $M > /dev/kmsg
s $T/jpeg_test /tmp/snap.jpg 90 2>&1 | grep -E 'frame seq|OK|FAIL|size|jpeg|JPEG|bytes' | tail -8 | sed 's/^/    /'
echo "  file size=$(stat -c%s /tmp/snap.jpg 2>/dev/null || echo NONE)"
printf '  SOI=%s  EOI=%s\n' "$(od -A n -t x1 -N 2 /tmp/snap.jpg 2>/dev/null | tr -d ' ')" "$(tail -c 2 /tmp/snap.jpg 2>/dev/null | od -A n -t x1 | tr -d ' ')"
echo "  parse JPEG SOF (dimensions/comp):"
python3 - <<'PY' 2>&1 | sed 's/^/    /'
import struct
d=open('/tmp/snap.jpg','rb').read()
i=2
while i < len(d)-1:
    if d[i]!=0xFF: i+=1; continue
    m=d[i+1]
    if m in (0xD8,0xD9) or 0xD0<=m<=0xD7: i+=2; continue
    ln=struct.unpack('>H', d[i+2:i+4])[0]
    if 0xC0<=m<=0xCF and m not in (0xC4,0xC8,0xCC):
        prec,h,w,nc = d[i+4],*struct.unpack('>HH',d[i+5:i+9]),d[i+9]
        print(f"SOF{m-0xC0} marker=0xFF{m:02X} precision={prec} {w}x{h} components={nc}")
        break
    if m==0xDA: break
    i+=2+ln
print("total", len(d), "bytes")
PY
echo "    dmesg-new: $(dmesg | awk -v m="$M" '$0 ~ m {f=1} f' | grep -viE 'ar0234_mipi\]|s_fmt|sensor_s_stream|3DNR|^\[.*\] S12b' | head -3 | tr '\n' '|')"

echo "########## 7. decode round-trip with the vendor demo"
echo "--- 7a. our own H.264 ---"
s /usr/bin/vdecoderdemo -i /tmp/rec_zc.h264 -codFmat 1 -o /tmp/dec_own.out -n 5 -sn 5 -outFmat 1 2>&1 | tail -10 | sed 's/^/    /'
echo "    rc=$? out=$(stat -c%s /tmp/dec_own.out 2>/dev/null || echo NONE)"
echo "--- 7b. our own H.265 (-codFmat 2) ---"
s /usr/bin/vdecoderdemo -i /tmp/rec_zc.h265 -codFmat 2 -o /tmp/dec_own265.out -n 5 -sn 5 -outFmat 1 2>&1 | tail -6 | sed 's/^/    /'
echo "    rc=$? out=$(stat -c%s /tmp/dec_own265.out 2>/dev/null || echo NONE)"
echo "--- 7c. vendor reference t30.h264 ---"
s /usr/bin/vdecoderdemo -i $T/t30.h264 -codFmat 1 -o /tmp/dec_t30.out -n 5 -sn 5 -outFmat 1 2>&1 | tail -6 | sed 's/^/    /'
echo "    rc=$? out=$(stat -c%s /tmp/dec_t30.out 2>/dev/null || echo NONE)"
echo "--- 7d. structure of the demo (why the custom wrapper hung the kernel) ---"
echo "    threads used by demo:"; strings /usr/bin/vdecoderdemo | grep -icE 'pthread_create' | sed 's/^/      pthread_create refs: /'
strings /usr/bin/vdecoderdemo | grep -iE 'AddVDPlugin|libawh|libvdecoder' | head -6 | sed 's/^/      /'
echo "    plugins:"; ls -l /usr/lib/aarch64-linux-gnu/libawh26*.so 2>&1 | sed 's/^/      /'
echo "    dec_test links:"; ldd $T/dec_test 2>/dev/null | grep -iE 'vdecoder|videoengine|awh' | sed 's/^/      /'

echo "########## 8. final health"
s /home/orangepi/ar0234test/vfr -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 6 2>&1 | grep RESULT | sed 's/^/  /'
ls -l /tmp/rec_* /tmp/snap.jpg /tmp/dec_* 2>&1 | sed 's/^/  /'
echo "########## DONE12B"
