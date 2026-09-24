#!/bin/sh
# dual_dtb.sh <enable|disable|show>
#
# Patch the board's device tree so that the *second* physical camera path
# (MIPI-B) exists, using fdtput only - no kernel/dts rebuild:
#
#   vin node (soc@3000000/vind@5800800)
#     isp@58ffffc      status                   disabled -> okay   (isp01)
#     vinc@582fffc     status                   disabled -> okay   (vinc01, device_id 1)
#     vinc@582fffc     vinc1_rear_sensor_sel    0 -> 1             (use sensor1, not sensor0)
#     vinc@582fffc     vinc1_front_sensor_sel   0 -> 1
#     sensor@5812010   sensor1_mname            ov13850_mipi -> ar0234_mipi
#
# NB fdtput takes the node and the property as two separate arguments
# (`fdtput <dtb> <node> <prop> <value>`); giving it "<node>/<prop>" fails with
# FDT_ERR_NOTFOUND.
#
# Run it ON the board (it sudos by itself), then reboot.  The previous dtb is
# copied to <dtb>.dual-bak-<timestamp>, `disable` restores the most recent one.
# The serial console stays usable either way, so a bad dtb can always be undone
# over the console.
S() { printf ' \n' | sudo -S -p '' "$@"; }

DTB=/boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb
V=/soc@3000000/vind@5800800
ISP=$V/isp@58ffffc
VINC=$V/vinc@582fffc
SENS=$V/sensor@5812010

show() {
	printf '%-26s ' "$1"
	S fdtget "$DTB" "$2" "$1" 2>&1 | tr -d '\r'
	echo
}

case "$1" in
enable)
	[ -f "$DTB" ] || { echo "no $DTB" >&2; exit 1; }
	BAK=$DTB.dual-bak-$(date +%s)
	S cp "$DTB" "$BAK"
	echo "backup: $BAK"
	S fdtput -t s "$DTB" "$ISP"  status             okay
	S fdtput -t s "$DTB" "$VINC" status             okay
	S fdtput -t i "$DTB" "$VINC" vinc1_rear_sensor_sel  1
	S fdtput -t i "$DTB" "$VINC" vinc1_front_sensor_sel 1
	S fdtput -t s "$DTB" "$SENS" sensor1_mname      ar0234_mipi
	echo "--- values after enable (file) ---"
	S fdtget "$DTB" "$ISP"  status
	S fdtget "$DTB" "$VINC" status
	S fdtget "$DTB" "$VINC" vinc1_rear_sensor_sel
	S fdtget "$DTB" "$VINC" vinc1_front_sensor_sel
	S fdtget "$DTB" "$SENS" sensor1_mname
	;;
disable)
	LAST=$(ls -1t $DTB.dual-bak-* 2>/dev/null | head -1)
	[ -n "$LAST" ] || { echo "no backup to restore" >&2; exit 1; }
	S cp "$LAST" "$DTB"
	echo "restored: $LAST"
	;;
esac

echo "--- file: $DTB ($(S md5sum "$DTB" 2>/dev/null | cut -d' ' -f1)) ---"
show status            $ISP
show status            $VINC
show vinc1_rear_sensor_sel  $VINC
show vinc1_front_sensor_sel $VINC
show sensor1_mname     $SENS
