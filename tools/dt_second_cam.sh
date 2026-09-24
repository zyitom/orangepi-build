#!/bin/sh
# dt_second_cam.sh <cmd>
#
# All device tree changes needed to bring up the *second* AR0234 path, done with
# fdtput only (no kernel/dts rebuild) and always with a timestamped backup.
#
# Commands
#   show                 print the interesting properties
#   isp01-on             status = okay on isp@58ffffc   (NOT needed, and the
#                        driver now refuses to register it - kept as a test)
#   isp01-off            revert
#   sensor2-on           point sensor slot 2 at the ar0234_mipi driver
#   sensor2-off          put the vendor name back (imx219_2)
#   save <tag>           copy the dtb to <dtb>.<tag>
#   restore <file>       copy <file> over the dtb
#
# The second camera must be wired to the MIPI-B connector (mipi1 / csi1) and to
# the I2C bus of cci 9; the vendor already routes vinc@5832000 (vinc20,
# device_id 8) at mipi1/csi1/isp0 with rear_sensor_sel = 2, and sensor slot 2
# already has pwdn = PE10 (sensor slot 1 shares PE6 with camera 0 - do not use
# it while camera 0 is attached).
S() { printf ' \n' | sudo -S -p '' "$@"; }

DTB=/boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb
V=/soc@3000000/vind@5800800
ISP01=$V/isp@58ffffc
SENS2=$V/sensor@5812020
VINC20=$V/vinc@5832000

backup() {
	B=$DTB.bak-$(date +%s)
	S cp "$DTB" "$B" && echo "backup: $B"
}

case "$1" in
	save)    [ -n "$2" ] || { echo "usage: save <tag>"; exit 2; }; S cp "$DTB" "$DTB.$2"; echo "saved $DTB.$2";;
	restore) [ -n "$2" ] || { echo "usage: restore <file>"; exit 2; }; S cp "$2" "$DTB"; echo "restored $2";;
	isp01-on)
		backup
		S fdtput -t s "$DTB" "$ISP01" status okay
		echo "isp@58ffffc status -> okay (driver should now REFUSE to register it)"
		;;
	isp01-off)
		backup
		S fdtput -t s "$DTB" "$ISP01" status disabled
		echo "isp@58ffffc status -> disabled"
		;;
	sensor2-on)
		backup
		S fdtput -t s "$DTB" "$SENS2" sensor2_mname ar0234_mipi
		S fdtput -t i "$DTB" "$SENS2" sensor2_twi_cci_id 9
		S fdtput -t i "$DTB" "$SENS2" sensor2_twi_addr 32
		S fdtput -t i "$DTB" "$SENS2" sensor2_mclk_id 2
		S fdtput -t i "$DTB" "$SENS2" sensor2_isp_used 1
		S fdtput -t i "$DTB" "$VINC20" vinc8_rear_sensor_sel 2
		S fdtput -t i "$DTB" "$VINC20" vinc8_front_sensor_sel 2
		S fdtput -t s "$DTB" "$VINC20" status okay
		# sensor@5812010 and sensor@5812020 ship as status = "disabled" in the
		# vendor dtsi, so without this the platform device is never created
		# and the AR0234 driver is never even asked to probe on the second
		# I2C bus (the failure is completely invisible).
		S fdtput -t s "$DTB" "$SENS2" status okay
		echo "sensor slot 2 -> ar0234_mipi (cci 9, addr 0x20, mclk2, pwdn PE10), status okay, vinc20 okay"
		;;
	sensor2-off)
		backup
		S fdtput -t s "$DTB" "$SENS2" sensor2_mname imx219_2
		S fdtput -t s "$DTB" "$SENS2" status disabled
		echo "sensor slot 2 -> imx219_2 (status disabled)"
		;;
esac

echo "--- file: $DTB ($(S md5sum "$DTB" 2>/dev/null | cut -d' ' -f1)) ---"
for p in status sensor2_mname sensor2_twi_cci_id sensor2_mclk_id; do
	printf '%-22s ' "$p"
	S fdtget "$DTB" "$SENS2" "$p" 2>&1 | tr -d '\r'; echo
done
printf '%-22s ' "isp01 status";  S fdtget "$DTB" "$ISP01"  status 2>&1 | tr -d '\r'; echo
for p in status device_id vinc8_rear_sensor_sel vinc8_mipi_sel vinc8_csi_sel vinc8_isp_sel vinc8_tdm_rx_sel vinc8_isp_tx_ch; do
	printf '%-22s ' "vinc20 $p"; S fdtget "$DTB" "$VINC20" "$p" 2>&1 | tr -d '\r'; echo
done
printf '%-22s ' "sensor0_pwdn"; S fdtget -t i "$DTB" "$V/sensor@5812000" sensor0_pwdn 2>&1 | tr -d '\r'; echo
printf '%-22s ' "sensor1_pwdn"; S fdtget -t i "$DTB" "$V/sensor@5812010" sensor1_pwdn 2>&1 | tr -d '\r'; echo
printf '%-22s ' "sensor2_pwdn"; S fdtget -t i "$DTB" "$SENS2" sensor2_pwdn 2>&1 | tr -d '\r'; echo
