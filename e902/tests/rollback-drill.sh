#!/bin/bash
# rollback-drill.sh -- ON TL101. Proves the one-click rollback AND redeploy:
#   vendor scp -> reboot -> verify vendor (no heartbeat, board healthy)
#   ours again -> reboot -> verify ours (heartbeat back)
# Usage: bash e902/tests/rollback-drill.sh
set -u
ROOT=/home/helios/Desktop/orangepi-build
E=$ROOT/e902
S=$ROOT/ar0234-port/tools/ssh_board.sh
export BOARD_PASS=orangepi
TS=$(date +%Y%m%d-%H%M%S)
LOGD=$E/verify-logs/rollback-$TS; mkdir -p "$LOGD"
B()  { $S "$@"; }
SU() { $S "printf 'orangepi\n' | sudo -S $1"; }
VENDOR=07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749

echo "== stage A: restore vendor scp on the live board =="
SU 'sh /home/orangepi/e902v2/restore-scp.sh /home/orangepi/e902v2/scp.fex /dev/mmcblk1' 2>&1 | tee "$LOGD/restore.txt"
grep -q "$VENDOR" "$LOGD/restore.txt" && echo "  restore: slot back to vendor (hash match)" || echo "  WARNING: vendor hash not confirmed"
echo "== stage B: reboot and verify vendor behaviour =="
SU 'reboot' || true
sleep 20
for i in $(seq 1 48); do B 'uname -r' >/dev/null 2>&1 && break; sleep 5; done
SU 'python3 /home/orangepi/e902v2/awdevmem.py dump --e902 0x4001E000 --count 4' > "$LOGD/vendor-hb.txt" 2>&1
SU 'python3 /home/orangepi/e902v2/awdevmem.py read 0x07032204' >> "$LOGD/vendor-hb.txt" 2>&1
echo "--- vendor heartbeat (expect NO e902c0de magic):"; tail -3 "$LOGD/vendor-hb.txt"
grep -ic 'e902c0de' "$LOGD/vendor-hb.txt" | grep -q '^0$' && echo "  vendor confirmed (no magic)" || echo "  magic still present?!"
echo "== stage C: redeploy ours =="
printf 'orangepi\nYES\n' | $S "sudo -S sh /home/orangepi/e902v2/flash-scp.sh /home/orangepi/e902v2/scp-ours-padded.bin /dev/mmcblk1" 2>&1 | tee "$LOGD/reflash.txt" | tail -4
echo "== stage D: reboot and verify ours =="
SU 'reboot' || true
sleep 20
for i in $(seq 1 48); do B 'uname -r' >/dev/null 2>&1 && break; sleep 5; done
SU 'python3 /home/orangepi/e902v2/awdevmem.py dump --e902 0x4001E000 --count 4' > "$LOGD/ours-hb.txt" 2>&1
grep -ic 'e902c0de' "$LOGD/ours-hb.txt" | grep -q '^[1-9]' && echo "  ours confirmed (heartbeat magic back)" || echo "  FAIL: magic missing after redeploy"
echo "rollback drill logs: $LOGD"
