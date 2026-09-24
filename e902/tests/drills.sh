#!/bin/bash
# drills.sh -- ON TL101. Exception drills E2 (E902 freeze via clock gate) and
# E3 (mailbox flood). Run only AFTER post-poweron.sh has PASSED.
# Usage: bash e902/tests/drills.sh
set -u
ROOT=/home/helios/Desktop/orangepi-build
E=$ROOT/e902
S=$ROOT/ar0234-port/tools/ssh_board.sh
export BOARD_PASS=orangepi
TS=$(date +%Y%m%d-%H%M%S)
LOGD=$E/verify-logs/drills-$TS; mkdir -p "$LOGD"
SU() { $S "printf 'orangepi\n' | sudo -S $1"; }
hbseq() { SU 'python3 /home/orangepi/e902v2/awdevmem.py dump --e902 0x4001E000 --count 2' 2>/dev/null | tail -1; }

echo "== drill E2: E902 freeze/resume via RISCV_BGR clock gate =="
echo "seq before : $(hbseq)" | tee "$LOGD/e2.txt"
SU 'python3 /home/orangepi/e902v2/awdevmem.py write 0x0701021C 0x00010002' >> "$LOGD/e2.txt" 2>&1
echo "gated (RISCV_BGR=0x00010002), sleep 3"
sleep 3
echo "seq frozen : $(hbseq)" | tee -a "$LOGD/e2.txt"
sleep 2
echo "seq frozen : $(hbseq)" | tee -a "$LOGD/e2.txt"
SU 'python3 /home/orangepi/e902v2/awdevmem.py write 0x0701021C 0x00010003' >> "$LOGD/e2.txt" 2>&1
sleep 2
echo "seq resumed: $(hbseq)" | tee -a "$LOGD/e2.txt"
grep -q 'RISCV_BGR' /dev/null 2>&1 || true

echo "== drill E3: mailbox flood (16 ECHO words) =="
for i in $(seq 0 15); do
  SU "python3 /home/orangepi/e902v2/awdevmem.py write 0x0709407C $((0x20000000 | i))" >/dev/null 2>&1
done
sleep 3
SU 'python3 /home/orangepi/e902v2/awdevmem.py dump --e902 0x4001E000 --count 14' > "$LOGD/e3-hb.txt" 2>&1
B() { $S "$@"; }
B 'dmesg | tail -5; uptime' > "$LOGD/e3-arm.txt" 2>&1
echo "heartbeat after flood:"; tail -3 "$LOGD/e3-hb.txt"
echo "ARM side after flood:";  cat "$LOGD/e3-arm.txt"
echo "drill logs: $LOGD"
