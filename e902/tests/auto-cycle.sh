#!/bin/bash
# auto-cycle.sh -- ON TL101. Full unattended cycle:
#   wait card in reader -> flash v5 + add_sum -> wait board ssh -> verify+demos
cd /home/helios/Desktop/orangepi-build
echo "[1] waiting for card in reader (max 30 min)..."
t0=$(date +%s)
while [ ! -b /dev/sdb ]; do
  [ $(( $(date +%s) - t0 )) -ge 1800 ] && { echo "TIMEOUT waiting for card"; exit 2; }
  sleep 5
done
echo "[2] card detected - fingerprint check"
ok=$(sudo dd if=/dev/sdb bs=1 skip=$((0x2004)) count=8 status=none 2>/dev/null)
[ "$ok" = "eGON.BT0" ] || { echo "not our board SD ($ok)"; exit 3; }
echo "[3] flashing v5 + add_sum"
sudo dd if=e902/fw-out/scp-ours-padded-105912.bin of=/dev/sdb bs=1 seek=$((0x113BC00)) count=105912 conv=notrunc status=none || exit 3
sync
sudo python3 e902/tests/fix-bootpkg-sum.py /dev/sdb --write || exit 3
sudo python3 e902/tests/fix-bootpkg-sum.py /dev/sdb || exit 3
echo "[4] DONE flashing - NOW: pull card, insert into BOARD, power on"
echo "[5] waiting for board ssh (max 25 min)..."
export BOARD_PASS=orangepi
t0=$(date +%s)
while :; do
  if ar0234-port/tools/ssh_board.sh 'uname -r' >/dev/null 2>&1; then
    echo "[6] board UP after $(( $(date +%s) - t0 ))s - running full verification"
    bash e902/tests/post-poweron.sh
    exit $?
  fi
  [ $(( $(date +%s) - t0 )) -ge 1500 ] && { echo "board never came up (25 min)"; exit 4; }
  sleep 5
done
