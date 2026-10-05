mem=$(cat /sys/power/mem_sleep | grep -o '\[.*\]' | tr -d '[]')
echo "mem_sleep current: $mem"
# wake-source census: which IRQs are wakeup-enabled + what's on the USB bus
for f in /sys/kernel/irq/*/wakeup; do
	[ -e "$f" ] && echo "wake-enabled $(dirname $f | xargs basename) = $(cat $f) $(cat $(dirname $f)/actions 2>/dev/null)"
done | head -30
[ -x /bin/lsusb ] && lsusb 2>/dev/null | sed 's/^/usb-device /'
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo +20 > /sys/class/rtc/rtc0/wakealarm
echo "alarm armed for $(cat /sys/class/rtc/rtc0/wakealarm), now $(date +%s)"
UNBOUND=""
for dev in 4200000.ehci1-controller 4200400.ohci1-controller; do
	D=/sys/bus/platform/devices/$dev
	if [ -e $D/driver/unbind ]; then
		echo $dev > $D/driver/unbind 2>/dev/null && UNBOUND="$UNBOUND $dev"
	fi
done
echo "unbound:$UNBOUND"
sync
echo deep > /sys/power/mem_sleep
echo mem > /sys/power/state
rc=$?
echo "resume rc=$rc  uptime=$(cut -d' ' -f1 /proc/uptime)"
dmesg | grep -E "PM:|suspend|resume|Restarting|usb" | tail -12
echo 0 > /sys/class/rtc/rtc0/wakealarm
