#!/bin/bash
# RT latency suite for the 1.0.2 image. Output: /home/orangepi/rt-results/
R=/home/orangepi/rt-results; mkdir -p $R; cd $R
D=60
gov() { for p in /sys/devices/system/cpu/cpufreq/policy*; do echo $1 > $p/scaling_governor; done; }
load() { stress-ng --cpu 8 --io 2 --vm 2 --vm-bytes 256M --hdd 1 --hdd-bytes 64M --timer 2 -t $1 >/dev/null 2>&1 & }
cyc() { cyclictest -m -p95 -S -i200 -D$D -q > $1.txt 2>&1; }
echo "start $(date)" > done.txt
gov ondemand; cyc idle
gov ondemand; load $((D+5)); sleep 2; cyc load-ondemand; wait
gov performance; load $((D+5)); sleep 2; cyc load-performance; wait
gov performance; load 40; sleep 2
rtla timerlat top -q -d 30 -a 150 > timerlat-load.txt 2>&1; wait
gov ondemand
dmesg | grep -iE 'lockup|hung_task|BUG:|WARNING:|rcu.*stall' > dmesg-issues.txt
echo "end $(date)" >> done.txt
