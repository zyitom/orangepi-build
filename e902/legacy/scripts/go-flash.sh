#!/bin/bash
# go-flash.sh -- ON TL101. Push the payload, write the scp slot, reboot, then
# read back what the little core is doing.
#
#   bash e902/go-flash.sh --yes       # just do it
#   bash e902/go-flash.sh --dry-run   # everything except write + reboot
#
# NOTE ON OBSERVATION: the FT232H that carries the E902's S_UART0 is plugged
# into the BOARD's USB, so its tty lives on the board. While the board reboots,
# that Linux is down and no capture can run. Therefore the verdict is taken
# AFTER the board is back, from three things that survive:
#   * the E902's ongoing UART output ("[tick] N" once per second)
#   * the SRAM heartbeat block at 0x4001E000 (magic 0xE902C0DE + rising seq)
#   * mailbox channel 3 MSG_STATUS (our HELLO sitting unconsumed)
set -eu

ROOT=/home/helios/Desktop/orangepi-build
E=$ROOT/e902
S=$ROOT/ar0234-port/tools/ssh_board.sh
export BOARD_PASS=orangepi
PAYLOAD=$E/fw-out/scp-ours-padded-105912.bin
EXPECT_SIZE=105912
EXPECT_SHA=$(awk '/scp-ours-padded/{print $1}' "$E/fw-out/SHA256SUMS")

DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1

B() { $S "$@"; }

echo "=== 0. preconditions ==="
[ -f "$PAYLOAD" ] || { echo "missing $PAYLOAD"; exit 1; }
sz=$(stat -c%s "$PAYLOAD"); sha=$(sha256sum "$PAYLOAD" | cut -d' ' -f1)
echo "payload : $PAYLOAD ($sz bytes, sha256 $sha)"
[ "$sz" = "$EXPECT_SIZE" ] || { echo "ERROR: wrong size"; exit 1; }
[ "$sha" = "$EXPECT_SHA" ] || { echo "ERROR: sha mismatch vs $EXPECT_SHA"; exit 1; }

echo
echo "=== 1. baseline ==="
B 'uname -r'
B 'printf "orangepi\n" | sudo -S python3 /home/orangepi/e902v2/awdevmem.py read 0x07032204' 2>&1 | tail -1
B 'printf "orangepi\n" | sudo -S python3 /home/orangepi/e902v2/awdevmem.py hb' 2>&1 | tail -2

echo
echo "=== 2. push payload + helper scripts ==="
cat "$PAYLOAD"                          | B 'cat > ~/e902v2/scp-ours-padded.bin'
cat "$E/backup/scp.fex.on-medium.bin"   | B 'cat > ~/e902v2/scp.fex'
cat "$E/e902-fw/scripts/flash-scp.sh"   | B 'cat > ~/e902v2/flash-scp.sh'
cat "$E/e902-fw/scripts/restore-scp.sh" | B 'cat > ~/e902v2/restore-scp.sh'
cat "$E/tests/fix-bootpkg-sum.py"       | B 'cat > ~/e902v2/fix-bootpkg-sum.py'
B 'chmod +x ~/e902v2/*.sh; sha256sum ~/e902v2/scp-ours-padded.bin ~/e902v2/scp.fex'

if [ "$DRY" = 1 ]; then
    echo
    echo "=== DRY RUN: skipping write and reboot ==="
    echo "would run: dd if=~/e902v2/scp-ours-padded.bin of=/dev/mmcblk1 bs=1 seek=0x113BC00 count=$EXPECT_SIZE conv=notrunc"
    exit 0
fi

echo
echo "=== 3. WRITE the scp slot ==="
B "printf 'orangepi\n' | sudo -S sh -c 'dd if=/home/orangepi/e902v2/scp-ours-padded.bin of=/dev/mmcblk1 bs=1 seek=\$((0x113BC00)) count=$EXPECT_SIZE conv=notrunc status=none; sync'"
# boot0 checks the package add_sum -- without this the next power-on lands in FEL
B "printf 'orangepi\n' | sudo -S python3 /home/orangepi/e902v2/fix-bootpkg-sum.py /dev/mmcblk1 --write"
B "printf 'orangepi\n' | sudo -S python3 /home/orangepi/e902v2/fix-bootpkg-sum.py /dev/mmcblk1" | grep -q PASS \
  || { echo "FATAL: add_sum invalid after flash -- DO NOT reboot"; exit 1; }
echo "--- read back (expect $EXPECT_SHA) ---"
got=$(B "printf 'orangepi\n' | sudo -S sh -c 'dd if=/dev/mmcblk1 bs=1 skip=\$((0x113BC00)) count=$EXPECT_SIZE status=none | sha256sum'" 2>/dev/null | awk '{print $1}' | tail -1)
echo "    got $got"
[ "$got" = "$EXPECT_SHA" ] || { echo "FATAL: slot readback mismatch -- DO NOT reboot (rollback: bash e902/go-revert.sh)"; exit 1; }

echo
echo "=== 4. reboot ==="
B 'printf "orangepi\n" | sudo -S reboot' || true
for i in $(seq 1 12); do
    sleep 10
    if B 'uname -r' >/dev/null 2>&1; then
        echo "board is back after ~${i}0 s"; break
    fi
    echo "  ... waiting (${i}0 s)"
done

echo
echo "=== 5. VERDICT: is the little core running our firmware? ==="
echo "--- (a) live E902 console, 8 s (expect '[tick] N' lines if our fw runs)"
B 'printf "orangepi\n" | sudo -S sh -c "stty -F /dev/ttyUSB0 115200 cs8 -cstopb -parenb -crtscts raw -echo 2>/dev/null; timeout 8 cat /dev/ttyUSB0"' 2>&1 | head -25
echo "--- (b) SRAM heartbeat at 0x4001E000 (expect 0xE902C0DE + rising seq)"
B 'printf "orangepi\n" | sudo -S python3 /home/orangepi/e902v2/awdevmem.py hb' 2>&1 | tail -4
echo "--- (c) mailbox channel 3 (our HELLO would sit here unconsumed)"
B "printf 'orangepi\n' | sudo -S sh -c 'python3 /home/orangepi/e902v2/awdevmem.py read 0x0709406c; python3 /home/orangepi/e902v2/awdevmem.py read 0x0300406c'" 2>&1 | tail -2
echo "--- (d) RST_START + riscv bgr"
B 'printf "orangepi\n" | sudo -S python3 /home/orangepi/e902v2/awdevmem.py read 0x07032204' 2>&1 | tail -1
B 'printf "orangepi\n" | sudo -S python3 /home/orangepi/e902v2/awdevmem.py read 0x0701021C' 2>&1 | tail -1

echo
echo "If (a) shows [tick] lines and (b) shows 0xE902C0DE, the E902 is OURS."
echo "Rollback: bash e902/go-revert.sh"
