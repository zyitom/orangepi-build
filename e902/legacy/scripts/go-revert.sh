#!/bin/bash
# go-revert.sh -- ON TL101. Put the vendor scp.fex back on the board.
#   bash e902/go-revert.sh
# Only works if the board still boots to Linux. Otherwise: card reader +
# backup/sd-boot-head-20260922.img.
set -eu
ROOT=/home/helios/Desktop/orangepi-build
E=$ROOT/e902
S=$ROOT/ar0234-port/tools/ssh_board.sh
export BOARD_PASS=orangepi
B() { $S "$@"; }

echo "=== pushing the vendor image and restoring the scp slot ==="
cat "$E/backup/scp.fex.on-medium.bin"   | B 'cat > ~/e902v2/scp.fex'
cat "$E/e902-fw/scripts/restore-scp.sh" | B 'cat > ~/e902v2/restore-scp.sh'
cat "$E/tests/fix-bootpkg-sum.py" | B 'cat > ~/e902v2/fix-bootpkg-sum.py'
B 'chmod +x ~/e902v2/restore-scp.sh'
B "printf 'orangepi\n' | sudo -S sh ~/e902v2/restore-scp.sh /home/orangepi/e902v2/scp.fex /dev/mmcblk1"

echo
echo "=== verify the slot now matches the vendor hash 07e6b976... ==="
B "printf 'orangepi\n' | sudo -S sh -c 'dd if=/dev/mmcblk1 bs=1 skip=\$((0x113BC00)) count=105912 status=none | sha256sum'"

echo
echo "=== reboot for a clean bl31-managed start ==="
B 'printf "orangepi\n" | sudo -S reboot' || true
