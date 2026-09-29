#!/bin/bash
# A/B the load generator: stress-ng 0.17.06 (noble) vs 0.15.06 (= Debian bookworm)
R=/home/orangepi/ab; mkdir -p $R; cd $R; rm -f *.txt
for p in /sys/devices/system/cpu/cpufreq/policy*; do echo performance > $p/scaling_governor; done
declare -A BIN=( [new]=/usr/bin/stress-ng [old]=/home/orangepi/stress-ng-0.15.06/stress-ng )
for round in 1 2 3; do for v in new old; do
  ${BIN[$v]} --cpu 8 --io 2 --vm 2 --vm-bytes 256M --hdd 1 --hdd-bytes 64M --timer 2 -t 65 --temp-path /tmp >/dev/null 2>&1 &
  sleep 2
  cyclictest -m -p95 -S -i200 -D60 -q | grep -E 'T: *[0-9]' | sed 's/ *(.*)//' | \
    awk -v v=$v -v r=$round '{m=$NF+0; a=$(NF-2)+0; if ($2<6) {if (m>s) s=m} else {if (m>b) b=m}; sa+=a} END {printf "%s round%d  A55max=%3d  A76max=%3d  avg=%.1f\n", v, r, s, b, sa/8}' >> result.txt
  wait
done; done
for p in /sys/devices/system/cpu/cpufreq/policy*; do echo ondemand > $p/scaling_governor; done
echo end >> result.txt
