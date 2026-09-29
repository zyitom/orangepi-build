#!/bin/sh
s() { printf ' \n' | sudo -S -p '' "$@"; }
echo "########## A. does libNBGlinker.so export the documented vip_lite API?"
for l in /usr/lib/libNBGlinker.so /usr/lib/libVIPhal.so; do
	echo "  == $l"
	nm -D --defined-only $l 2>/dev/null | awk '{print "    "$3}' | grep -E '^    vip_(init|destroy|create_network|create_buffer|query_network|run_network|prepare_network|set_input|set_output|map_buffer|finish_network)' | head -20
	printf '    total exported: %s\n' "$(nm -D --defined-only $l 2>/dev/null | wc -l)"
done
echo "--- can we link a tiny inference-less program against VIPLite API? ---"
cat > /tmp/npulink.c <<'C'
#include <stdio.h>
#include <vip_lite.h>
int main(void){ vip_uint32_t v=0; if(vip_query_driver_version(&v)==VIP_SUCCESS) printf("driver 0x%x\n", v); else printf("query failed\n"); return 0; }
C
gcc -O2 -o /tmp/npulink /tmp/npulink.c -lNBGlinker -lVIPhal 2>&1 | head -8 | sed 's/^/    /'
[ -x /tmp/npulink ] && /tmp/npulink 2>&1 | sed 's/^/    /' || echo "    not built (missing libVIPlite symbols or header mismatch)"

echo "########## B. is the ISP actually live / what does its register window show in-stream"
/home/orangepi/ar0234test/vfr -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 10 >/tmp/s17.v 2>&1 &
P=$!; sleep 3
printf '  ISP_TOP_CFG0 0x5900000 = %s\n' "$(s /tmp/vinreg r 0x5900000 1 2>&1 | tail -1)"
printf '  CSIC VIPP IN 0x58008a0/4 = %s\n' "$(s /tmp/vinreg r 0x58008a0 1 2>&1 | tail -1)"
printf '  VIPP0 top  0x5910400 = %s\n' "$(s /tmp/vinreg r 0x5910400 1 2>&1 | tail -1)"
printf '  VIPP0 load 0x5910600 = %s\n' "$(s /tmp/vinreg r 0x5910600 1 2>&1 | tail -1)"
printf '  TDM 0x5908000 = %s\n' "$(s /tmp/vinreg r 0x5908000 1 2>&1 | tail -1)"
printf '  CSI 0x5800000 = %s\n' "$(s /tmp/vinreg r 0x5800000 1 2>&1 | tail -1)"
wait $P; grep RESULT /tmp/s17.v | sed 's/^/  /'

echo "########## C. 3A service: fixed/industrial mode?"
ls -l /etc/ar0234.conf 2>&1 | sed 's/^/  /'
systemctl is-active ar0234-3ad | sed 's/^/  active: /'
ls -l /usr/local/bin/ar0234-* | sed 's/^/  /'
echo "  --- manual exposure/gain accepted? (should be, via libisp) ---"
s /usr/local/bin/ar0234-rec -w 1920 -h 1200 -f 60 -c h264 -n 30 -e 8000 -g 2.0 -o /tmp/manual.h264 2>&1 | grep -E 'done:|FAIL|gain|exposure' | tail -3 | sed 's/^/    /'
v4l2-ctl -d /dev/video0 -C exposure_time_absolute,gain 2>/dev/null | sed 's/^/    after: /'

echo "########## D. cleanup: g2d module was not loaded at boot -> put it back"
s rmmod g2d_sunxi 2>&1 | sed 's/^/  /'
ls /dev/g2d 2>&1 | sed 's/^/  /'
lsmod | grep -c g2d | sed 's/^/  g2d refs: /'

echo "########## E. final state"
printf '  uptime=%s\n  dtb=%s\n  module=%s\n' "$(cut -d' ' -f1 /proc/uptime)" \
  "$(md5sum /boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb)" "$(cat /sys/module/vin_v4l2/srcversion)"
printf '  isp clk=%s csi clk=%s\n' "$(cat /sys/kernel/debug/clk/isp/clk_rate)" "$(cat /sys/kernel/debug/clk/csi/clk_rate)"
printf '  d3d_min_vblank_us=%s\n' "$(cat /sys/module/vin_v4l2/parameters/d3d_min_vblank_us)"
printf '  ar0234_mipi params: trigger_mode=%s flash_enable=%s frame_rate=%s\n' "$(cat /sys/module/ar0234_mipi/parameters/trigger_mode)" "$(cat /sys/module/ar0234_mipi/parameters/flash_enable)" "$(cat /sys/module/ar0234_mipi/parameters/frame_rate)"
echo "  stray procs:"; ps ax | grep -E 'vfr|v4l2-ctl|ar0234-rec|vpm_run|yolov5|vdecoder|g2d_test|jpeg_test' | grep -v grep | sed 's/^/    /'
echo "  health:"; /home/orangepi/ar0234test/vfr -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 6 2>&1 | grep RESULT | sed 's/^/    /'
echo "########## DONE17"
