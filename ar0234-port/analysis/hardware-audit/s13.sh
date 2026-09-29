#!/bin/sh
# s13: H.265 condition matrix, G2D, OpenCL, NPU.
s() { printf ' \n' | sudo -S -p '' "$@"; }
REC=/usr/local/bin/ar0234-rec
T=/home/orangepi/ar0234test

echo "########## A. H.265 vs H.264 condition matrix (zero-copy, 120 frames each)"
for spec in "h264 1920 1200 120" "h265 1920 1200 120" "h265 1920 1200 60" "h265 1920 1200 30" "h265 1920 1080 30" "h265 1280 720 120" "h264 1920 1200 120"; do
	set -- $spec
	CO=$1; W=$2; H=$3; F=$4
	M="S13-$CO-$W-$H-$F-$$"; echo $M > /dev/kmsg
	O=/tmp/m_$CO_${W}x${H}_$F.h26${CO#h26}
	s $REC -w $W -h $H -f $F -c $CO -b 20000000 -n 120 -o $O 2>&1 \
		| grep -E 'input path|done:|FAIL' | sed 's/^/    /'
	printf '  %-26s size=%s  dmesg-new=%s\n' "$CO ${W}x${H}@$F" "$(stat -c%s $O 2>/dev/null || echo NONE)" \
		"$(dmesg | awk -v m="$M" '$0 ~ m {f=1} f' | grep -viE 'ar0234_mipi\]|s_fmt|sensor_s_stream|3DNR|enable_cedar|S13-|V4L2_IDENT|find the onsemi|vblank' | head -2 | tr '\n' '|')"
	sleep 1
done

echo "########## B. G2D"
echo "--- module load ---"
s modprobe g2d_sunxi 2>&1 | sed 's/^/  /'
sleep 1
lsmod | grep g2d | sed 's/^/  /'
ls -l /dev/g2d 2>&1 | sed 's/^/  /'
echo "--- g2d_test (opens /dev/video0 1920x1080@30, BITBLT NV12 fd->fd, then 2x scale) ---"
M="S13-g2d-$$"; echo $M > /dev/kmsg
s $T/g2d_test 2>&1 | sed 's/^/  /'
echo "  dmesg-new: $(dmesg | awk -v m="$M" '$0 ~ m {f=1} f' | grep -viE 'ar0234_mipi\]|s_fmt|sensor_s_stream|3DNR|S13-' | head -4 | tr '\n' '|')"

echo "########## C. GPU / OpenCL"
echo "--- pvr debugfs ---"
cat /sys/kernel/debug/pvr/version 2>&1 | sed 's/^/  /'
head -c 400 /sys/kernel/debug/pvr/status 2>&1 | sed 's/^/  /'; echo
echo "--- cltest2 ---"
$T/cltest2 2>&1 | sed 's/^/  /'
echo "--- dma-buf import extensions present? ---"
$T/cltest2 2>&1 | grep -iE 'dma_buf|import_memory|yuv_image|fp16|integer_dot' | head -10 | sed 's/^/  /'
echo "--- headers available? ---"
ls -d /usr/include/CL 2>&1 | sed 's/^/  /'
ls /usr/include/CL/*.h 2>/dev/null | head -3 | sed 's/^/    /'

echo "########## D. NPU"
echo "--- nodes / modules ---"
ls -l /dev/vipcore 2>&1 | sed 's/^/  /'; lsmod | grep vipcore | sed 's/^/  /'
cat /sys/kernel/debug/viplite/vip_info 2>&1 | sed 's/^/  /'
echo "--- user-space completeness ---"
for l in libVIPlite.so libVIPhal.so libNBGlinker.so; do
	f=$(find /usr/lib /lib -name "$l" 2>/dev/null | head -1)
	printf '  %-18s %s\n' "$l" "${f:-MISSING}"
done
echo "--- can a program actually init the NPU? ---"
cat > /tmp/nputry.c <<'EOF'
#include <stdio.h>
#include <dlfcn.h>
int main(void){
  const char *libs[]={"libVIPlite.so","/usr/lib/libVIPlite.so","/usr/lib/aarch64-linux-gnu/libVIPlite.so",0};
  for(int i=0;libs[i];i++){ void*h=dlopen(libs[i],RTLD_NOW);
    printf("dlopen(%s) = %s\n", libs[i], h?"OK":dlerror()); if(h) return 0; }
  void*h=dlopen("libVIPhal.so",RTLD_NOW);
  printf("dlopen(libVIPhal.so) = %s\n", h?"OK":dlerror());
  if(h){ void*f=dlsym(h,"viphal_init"); printf("  viphal_init: %s\n", f?"present":"absent"); }
  return 1;
}
EOF
gcc -O2 -o /tmp/nputry /tmp/nputry.c -ldl 2>&1 | sed 's/^/  /'
/tmp/nputry 2>&1 | sed 's/^/  /'
echo "  --- headers for the API ---"; ls -l /usr/include/vip_lite.h 2>&1 | sed 's/^/  /'
echo "  --- model conversion tool (.nb) present? ---"
ls /usr/bin/*nb* /usr/bin/*NBG* /usr/bin/*vip* 2>&1 | sed 's/^/  /'
find / -maxdepth 4 -name '*.nb' -o -maxdepth 4 -name 'nbg*' 2>/dev/null | head -5 | sed 's/^/  /'

echo "########## E. health"
s $T/vfr -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 6 2>&1 | grep RESULT | sed 's/^/  /'
echo "########## DONE13"
