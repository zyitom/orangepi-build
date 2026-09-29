#!/bin/bash
# suspend-diagnose.sh -- staged suspend diagnosis with serial evidence.
#
#   bash e902/tests/board/suspend-diagnose.sh freezer          # one stage
#   bash e902/tests/board/suspend-diagnose.sh ladder           # all stages
#
# pm_test stages (CONFIG_PM_DEBUG=y, already in the a733 kernel):
#   freezer -> devices -> platform -> processors -> core
# Each stage makes `echo mem` run suspend up to that layer, wait 5 s,
# and resume -- without actually powering anything off. The stage that
# hangs or errors is where real suspend dies; with loglevel raised and
# the serial console captured, the last driver named in the log is the
# culprit.
#
# Evidence lands in e902/backup/suspend-diag-<ts>/ (serial log + dmesg).
set -u

# Host sudo: feed HOST_SUDO_PASS on stdin when set (unattended), else plain sudo.
sudo_() { if [ -n "${HOST_SUDO_PASS+x}" ]; then printf '%s\n' "$HOST_SUDO_PASS" | sudo -S -p '' "$@"; else sudo "$@"; fi; }
E=$(cd "$(dirname "$0")/../.." && pwd)
S=$E/ar0234-port/tools/ssh_board.sh
MODE=${1:-ladder}
TTY=${TTY:-/dev/ttyUSB0}
TS=$(date +%m%d-%H%M%S)
OUT=$E/backup/suspend-diag-$TS
mkdir -p "$OUT"

sts() { timeout 15 "$S" -s "$1" 2>/dev/null; }

echo "== 0. preconditions"
[ -w "$TTY" ] || { echo "  serial $TTY missing"; }
if ! sts "echo up" | grep -q up; then
    echo "  board offline -- waiting (max 90 s)"
    for i in $(seq 18); do
        sts "echo up" | grep -q up && break
        sleep 5
    done
fi
sts "echo up" | grep -q up || { echo "  board still offline, abort"; exit 1; }

echo "== 1. raise console loglevel + marker check"
sts "dmesg -n 7"
sts "echo SUSPEND-DIAG-MARKER > /dev/kmsg"
sleep 1
sudo_ bash -c "stty -F $TTY 115200 raw -echo" 2>/dev/null
MARK=$(sudo_ timeout 3 cat "$TTY" 2>/dev/null | grep -c SUSPEND-DIAG-MARKER)
echo "  serial path: $([ "${MARK:-0}" -ge 1 ] && echo ok || echo DEGRADED)"

echo "== 2. serial capture -> $OUT/serial.log"
sudo_ bash -c "nohup cat $TTY > $OUT/serial.log 2>/dev/null & echo \$! > /tmp/sd-cat.pid"

diagnose_stage() {  # $1 = stage
    local stage=$1 rc
    echo "== stage: $stage"
    sts "echo $stage > /sys/power/pm_test"
    rc=$(sts "timeout 30 sh -c 'echo mem > /sys/power/state'; echo \${PIPESTATUS[0]}" | tail -1)
    echo "  rc=$rc $([ "$rc" = "0" ] && echo PASS || echo FAIL/HANG)"
    sts "dmesg | grep -E 'PM:' | tail -3" | sed 's/^/  | /'
    if [ "$rc" != "0" ]; then
        echo "  !! board hung at '$stage' -- serial log tail:"
        sudo_ tail -25 "$OUT/serial.log" 2>/dev/null | sed 's/^/  | /'
        echo "  !! power-cycle the board, then rerun to skip past this stage"
        sts "dmesg" > "$OUT/dmesg-hung-$stage.txt" 2>/dev/null
        return 1
    fi
    return 0
}

if [ "$MODE" = "ladder" ]; then
    for st in freezer devices platform processors core; do
        diagnose_stage "$st" || break
    done
else
    diagnose_stage "$MODE" || true
fi

echo "== 3. collect evidence"
sudo_ kill "$(cat /tmp/sd-cat.pid)" 2>/dev/null
sts "dmesg" > "$OUT/dmesg-final.txt" 2>/dev/null
[ -s "$OUT/serial.log" ] && grep -aE 'PM:|Suspending|calling|call back|failed|Error' \
    "$OUT/serial.log" | tail -30 | tee "$OUT/suspect-drivers.txt"
echo "== done: $OUT"
