#!/bin/bash
# flash-scp.sh -- ON TL101. Write an SCP image into the scp item of the
# board's own SD card (board running Linux), reboot it, and record both
# serial consoles across the reboot.
#
#   bash e902/tools/flash-scp.sh --dry-run                  # checks + backup, no write
#   bash e902/tools/flash-scp.sh                            # fw-out/vendor-scp.bin
#   bash e902/tools/flash-scp.sh e902/e902-fw/build/fw-scp-padded.bin   # our own firmware
#   bash e902/tools/flash-scp.sh external/packages/pack-uboot/sun60iw2/bin/scp.fex  # shipped image
#
# Any payload size works: tools/bootpkg.py resizes the item and fixes the
# boot-package add_sum (without that fix boot0 drops to FEL on next power-on).
# A 24 MiB backup of the card head is pulled to e902/backup/ before writing.
# Boards without python3 (the Buildroot image): the card head is patched on
# the host instead and written back with dd, then read back and compared.
# If the board does not come back: tools/flash-scp-reader.sh with the card in
# the USB reader, or dd the backup image back (see FLASHING.md).
set -eu

E=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(dirname "$E")
S=${SSH_BOARD:-$ROOT/ar0234-port/tools/ssh_board.sh}
: "${BOARD_PASS:=orangepi}"
export BOARD_PASS
DEV=/dev/mmcblk1
RDIR=/home/orangepi/e902v2
TS=$(date +%Y%m%d-%H%M%S)
LOGS=$E/verify-logs/flash-$TS

DRY=0
[ "${1:-}" = "--dry-run" ] && { DRY=1; shift; }
PAYLOAD=$(realpath "${1:-$E/fw-out/vendor-scp.bin}")

B() { "$S" "$@"; }
BSUDO() { B "printf '%s\n' '$BOARD_PASS' | sudo -S -p '' $*"; }

echo "=== 0. payload ==="
[ -f "$PAYLOAD" ] || { echo "missing $PAYLOAD"; exit 1; }
SZ=$(stat -c%s "$PAYLOAD"); SHA=$(sha256sum "$PAYLOAD" | cut -d' ' -f1)
echo "$PAYLOAD: $SZ bytes, sha256 $SHA"
REC=$(awk -v f="$(basename "$PAYLOAD")" '$2==f{print $1}' "$E/fw-out/SHA256SUMS" 2>/dev/null || true)
if [ -n "$REC" ] && [ "$REC" != "$SHA" ]; then
	echo "ERROR: sha256 differs from fw-out/SHA256SUMS ($REC)"; exit 1
fi

echo; echo "=== 1. board + boot package ==="
B 'uname -r; uptime'
B "mkdir -p $RDIR"
HOSTMODE=0
if B 'command -v python3' >/dev/null 2>&1; then
	B "cat > $RDIR/bootpkg.py" < "$E/tools/bootpkg.py"
	BSUDO python3 $RDIR/bootpkg.py info $DEV || { echo "ERROR: package check failed on board"; exit 1; }
else
	HOSTMODE=1
	echo "(no python3 on the board: patching the card head on the host)"
fi

echo; echo "=== 2. backup card head (24 MiB) ==="
BK=$E/backup/sd-head-before-flash-$TS.img
mkdir -p "$E/backup"
BSUDO dd if=$DEV bs=1M count=24 status=none > "$BK"
[ "$(stat -c%s "$BK")" = $((24 * 1024 * 1024)) ] || { echo "ERROR: short backup $BK"; exit 1; }
python3 "$E/tools/bootpkg.py" info "$BK" | head -1
echo "-> $BK"

if [ "$HOSTMODE" = 1 ]; then
	HEAD=$E/backup/sd-head-new-$TS.img
	cp "$BK" "$HEAD"
	python3 "$E/tools/bootpkg.py" set-scp "$HEAD" "$PAYLOAD"
	if [ "$DRY" = 1 ]; then
		echo; echo "=== DRY RUN: nothing written ==="; rm -f "$HEAD"; exit 0
	fi
	python3 "$E/tools/bootpkg.py" set-scp "$HEAD" "$PAYLOAD" --write
	python3 "$E/tools/bootpkg.py" info "$HEAD" | grep -q 'checksum PASS' \
		|| { echo "FATAL: checksum invalid in the patched head -- nothing written"; exit 1; }
	echo; echo "=== 3/4. WRITE (host-patched head, 24 MiB) ==="
	HSHA=$(sha256sum "$HEAD" | cut -d' ' -f1)
	B "cat > $RDIR/sd-head.img" < "$HEAD"
	got=$(B "sha256sum $RDIR/sd-head.img" | cut -d' ' -f1)
	[ "$got" = "$HSHA" ] || { echo "ERROR: push corrupted ($got)"; exit 1; }
	BSUDO dd if=$RDIR/sd-head.img of=$DEV bs=1M conv=notrunc,fsync status=none
	B sync
	back_sha=$(BSUDO dd if=$DEV bs=1M count=24 status=none | sha256sum | cut -d' ' -f1)
	[ "$back_sha" = "$HSHA" ] || { echo "FATAL: read-back differs -- DO NOT reboot; dd $BK back"; exit 1; }
	B "rm -f $RDIR/sd-head.img"
	rm -f "$HEAD"
	echo "written and read back OK"
else

echo; echo "=== 3. push payload ==="
B "cat > $RDIR/scp-payload.bin" < "$PAYLOAD"
got=$(B "sha256sum $RDIR/scp-payload.bin" | cut -d' ' -f1)
[ "$got" = "$SHA" ] || { echo "ERROR: push corrupted ($got)"; exit 1; }
BSUDO python3 $RDIR/bootpkg.py set-scp $DEV $RDIR/scp-payload.bin

if [ "$DRY" = 1 ]; then
	echo; echo "=== DRY RUN: nothing written ==="; exit 0
fi

echo; echo "=== 4. WRITE ==="
BSUDO python3 $RDIR/bootpkg.py set-scp $DEV $RDIR/scp-payload.bin --write
B sync
BSUDO python3 $RDIR/bootpkg.py info $DEV | grep -q 'checksum PASS' \
	|| { echo "FATAL: checksum invalid after write -- DO NOT reboot; restore with bootpkg.py set-scp <backup scp>"; exit 1; }
fi

echo; echo "=== 5. reboot, recording both consoles -> $LOGS ==="
mkdir -p "$LOGS"
cap() {	# cap <by-id glob> <log>
	local d
	d=$(ls /dev/serial/by-id/$1 2>/dev/null | head -1)
	[ -n "$d" ] || { echo "  (no console matching $1)"; return; }
	if fuser "$d" >/dev/null 2>&1; then echo "  WARNING: $d is busy (another capture?)"; fi
	stty -F "$d" 115200 cs8 -cstopb -parenb -crtscts raw -echo
	timeout 240 cat "$d" > "$2" &
	CAP_PIDS="$CAP_PIDS $!"
}
CAP_PIDS=
cap 'usb-1a86_USB_Serial*' "$LOGS/arm-console.log"
cap 'usb-FTDI_*' "$LOGS/e902-console.log"
BOOT_ID=$(B 'cat /proc/sys/kernel/random/boot_id')
BSUDO reboot || true
# the board may still answer for a few seconds -- a changed boot_id is the proof
back=0
for i in $(seq 1 18); do
	sleep 10
	id=$(B 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null || true)
	if [ -n "$id" ] && [ "$id" != "$BOOT_ID" ]; then back=1; echo "board is back after ~${i}0 s"; break; fi
	echo "  ... waiting (${i}0 s)"
done
sleep 5
[ -n "$CAP_PIDS" ] && kill $CAP_PIDS 2>/dev/null || true

echo; echo "=== 6. verdict ==="
echo "--- ARM console (boot0 / u-boot / bl31 lines about the SCP):"
grep -aiE 'scp|arisc|fel|checksum|bad magic' "$LOGS/arm-console.log" 2>/dev/null | head -20 || true
echo "--- E902 console (first lines):"
head -c 3000 "$LOGS/e902-console.log" 2>/dev/null | tr -d '\r' | head -30 || true
if [ "$back" = 1 ]; then
	B 'uname -r; uptime'
	echo "OK: board booted with the new SCP ($SHA)"
else
	echo "Board NOT back. Check $LOGS/arm-console.log; recovery: FLASHING.md section 3."
	exit 1
fi
