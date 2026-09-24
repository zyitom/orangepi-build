#!/bin/bash
# wait-and-verify.sh -- ON TL101. Poll for board ssh up to N minutes; when it
# comes up, run the full post-poweron verification automatically.
#   setsid nohup bash e902/tests/wait-and-verify.sh 600 > /tmp/wait-verify.log 2>&1 &
N=${1:-600}
S=/home/helios/Desktop/orangepi-build/ar0234-port/tools/ssh_board.sh
export BOARD_PASS=orangepi
echo "[wait] polling board ssh for ${N}s ..."
t0=$(date +%s)
while :; do
  if $S 'uname -r' >/dev/null 2>&1; then
    echo "[wait] board is UP after $(( $(date +%s) - t0 ))s -> running verification"
    bash /home/helios/Desktop/orangepi-build/e902/tests/post-poweron.sh
    exit $?
  fi
  now=$(date +%s); [ $((now - t0)) -ge "$N" ] && { echo "[wait] timeout (${N}s) -- board never came up"; exit 2; }
  sleep 5
done
