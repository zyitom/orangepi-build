sleep 3		# let the launching ssh session close first
mem=$(cat /sys/power/mem_sleep | grep -o '\[.*\]' | tr -d '[]')
echo "mem_sleep current: $mem"
# wake-source census: which IRQs are wakeup-enabled
for f in /sys/kernel/irq/*/wakeup; do
	[ "$(cat $f)" = enabled ] && echo "wake-enabled $(dirname $f | xargs basename) $(cat $(dirname $f)/actions 2>/dev/null)"
done
# keep the kernel console alive through suspend/resume and make it talk:
# the image boots with "quiet loglevel=3", which hides every PM message
echo 8 > /proc/sys/kernel/printk
echo N > /sys/module/printk/parameters/console_suspend
echo 1 > /sys/power/pm_debug_messages
echo 1 > /sys/power/pm_print_times
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo +20 > /sys/class/rtc/rtc0/wakealarm
echo "alarm armed for $(cat /sys/class/rtc/rtc0/wakealarm), now $(date +%s)"
sync
echo deep > /sys/power/mem_sleep
echo mem > /sys/power/state
rc=$?
echo "resume rc=$rc  now $(date +%s) uptime=$(cut -d' ' -f1 /proc/uptime)"
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo s2idle > /sys/power/mem_sleep
dmesg | grep -aE "PM:|suspend|resume|Restarting|psci|CPU[0-9]|Enabling|Disabling" | tail -40
sync
