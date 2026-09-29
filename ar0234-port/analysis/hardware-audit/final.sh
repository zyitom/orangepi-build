#!/bin/sh
echo "=== 1. DTB unchanged? ==="
md5sum /boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb
echo "=== 2. module ==="
cat /sys/module/vin_v4l2/srcversion; md5sum /lib/modules/6.6.98-sun60iw2/updates/vin_v4l2.ko
echo "=== 3. clocks ==="
printf 'isp=%s csi=%s  d3d_min_vblank_us=%s\n' "$(cat /sys/kernel/debug/clk/isp/clk_rate)" "$(cat /sys/kernel/debug/clk/csi/clk_rate)" "$(cat /sys/module/vin_v4l2/parameters/d3d_min_vblank_us)"
echo "=== 4. ar0234_mipi params (unchanged from boot) ==="
for p in /sys/module/ar0234_mipi/parameters/*; do printf '%s=%s ' "$(basename $p)" "$(cat $p)"; done; echo
echo "=== 5. g2d back to boot state ==="
ls /dev/g2d 2>&1; lsmod | grep g2d || echo "  g2d_sunxi not loaded (OK)"
echo "=== 6. stray processes ==="
ps ax | grep -E 'vfr|v4l2-ctl|ar0234-rec|ar0234-3a|vpm_run|yolov5|vdecoder|g2d_test|jpeg_test|cltest|dec_test' | grep -v grep || echo "  none"
echo "=== 7. services ==="
systemctl is-active ar0234-3ad
echo "=== 8. /tmp leftovers (tmpfs, wiped on reboot) ==="
ls /tmp | head -20 | tr '\n' ' '; echo
echo "=== 9. final 60 s regression 1920x1200 NV12 @120 ==="
/home/orangepi/ar0234test/vfr -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 60 2>&1 | grep -E 'RESULT|GAPS'
echo "=== vi0 after ==="
sed -n '/^vi0:/,/^\*\*\*/p' /sys/kernel/debug/mpp/vi | grep -E 'frame =>|prs_in|lost|bkuf'
echo "=== 10. dmesg errors since boot (summary) ==="
for p in 'frame lost' 'sunxi_isp_reset' 'is not mapped' 'Internal error' 'Unable to handle' 'usage count underflow' 'vin_pin_disable'; do
	printf '  %-24s %s\n' "$p" "$(dmesg | grep -c "$p")"
done
echo "=== DONE FINAL ==="
