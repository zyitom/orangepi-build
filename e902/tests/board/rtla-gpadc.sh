#!/bin/sh
# rtla build + GPADC interrupt-storm fix, run as root on the board:
#   ar0234-port/tools/ssh_board.sh "cat > /tmp/rt.sh" < e902/tests/board/rtla-gpadc.sh
#   ar0234-port/tools/ssh_board.sh -s "sh /tmp/rt.sh"
echo ===RTLA-BUILD===
apt-get -y -qq install libtraceevent-dev libtracefs-dev >/dev/null 2>&1
cd ~/rtla-src && make >/dev/null 2>&1
install -m755 rtla /usr/local/bin/rtla
which rtla
rtla timerlat -h 2>&1 | head -2
echo ===RTLA-SMOKE===
rtla osnoise top -q -D 3s 2>&1 | tail -4
echo ===GPADC-BEFORE===
grep gpadc /proc/interrupts
cyclictest -m -p95 -i1000 -d0 -t1 -a0 -D 10s 2>&1 | tail -2
echo 2521000.gpadc > /sys/bus/platform/drivers/sunxi-gpadc/unbind && echo gpadc unbound
printf '# CPUS-domain GPADC polls ~4 kHz on the housekeeping core with nothing\n# using it; modprobe it back if you ever need an ADC pin.\nblacklist sunxi_gpadc\n' > /etc/modprobe.d/blacklist-sunxi-gpadc.conf
echo ===GPADC-AFTER===
grep gpadc /proc/interrupts
sleep 3
grep gpadc /proc/interrupts
cyclictest -m -p95 -i1000 -d0 -t1 -a0 -D 10s 2>&1 | tail -2
echo ===LRADC===
for e in /sys/class/input/event*; do echo "$e: $(cat $e/device/name)"; done
ls /dev/iio:device* 2>/dev/null
