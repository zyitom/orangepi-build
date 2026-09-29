#!/bin/bash
# ser.sh <wait-seconds> "<command>"  -- run a shell command on the board's serial console
W=$1; shift
O=$(mktemp)
timeout $((W+1)) cat /dev/ttyUSB0 > $O &
sleep 0.3
printf '%s\r' "$*" > /dev/ttyUSB0
sleep $W
wait
tr -d '\000\r' < $O | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g'
rm -f $O
