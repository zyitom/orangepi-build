mem=$(cat /sys/power/mem_sleep | grep -o '\[.*\]' | tr -d '[]')
echo "mem_sleep current: $mem"
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo +20 > /sys/class/rtc/rtc0/wakealarm
echo "alarm armed for $(cat /sys/class/rtc/rtc0/wakealarm), now $(date +%s)"
sync
echo deep > /sys/power/mem_sleep
echo mem > /sys/power/state
rc=$?
echo "resume rc=$rc  uptime=$(cut -d' ' -f1 /proc/uptime)"
dmesg | grep -E "PM:|suspend|resume|Restarting" | tail -8
echo 0 > /sys/class/rtc/rtc0/wakealarm
