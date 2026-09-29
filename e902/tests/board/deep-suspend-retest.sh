#!/bin/bash
# deep-suspend-retest.sh -- ON TL101. Retest deep suspend with the fixed SCP.
#
# Validates the 2026-09-30 deep-suspend fix chain (see
# tina-zero3w/docs/STATUS.md section 6):
#   1. flash the debug SCP (patches 0004 fallback + patches-debug 0002 probes)
#   2. reboot, confirm on the E902 console that the fallback fired
#      ("dram para all zero -- using built-in zero3w values")
#   3. deep suspend with an RTC alarm +20 s, both serials captured
#   4. decode: "dram save done" must appear after "the first time ddr standby",
#      then "wakeup: N" + "dram up enter/done" on resume, kernel must come back.
#
# Usage:
#   BOARD=172.16.0.194 bash e902/tests/board/deep-suspend-retest.sh [--no-flash]
# Needs the FT232 (0403:6014) on ttyUSB1 at 57600 for the SCP console.
set -u

E=$(cd "$(dirname "$0")/../.." && pwd)          # e902/
ROOT=$(dirname "$E")
S=${SSH_BOARD:-$ROOT/ar0234-port/tools/ssh_board.sh}
: "${BOARD_PASS:=orangepi}"
export BOARD_PASS
FW=$E/fw-out/vendor-scp-debug.bin
OUT=$E/verify-logs/deep-retest-$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
FTDI_TTY=${FTDI_TTY:-/dev/ttyUSB1}              # E902 S_UART0 via FT232H

NOFLASH=0
[ "${1:-}" = "--no-flash" ] && NOFLASH=1

# find the FT232's tty via /dev/serial/by-id (stable; the CH340 big-core
# console also shows up as ttyUSB*, so resolving by USB id matters)
find_ftdi() {
	readlink -f /dev/serial/by-id/usb-FTDI_*-if00-port0 2>/dev/null | head -1
}
FTDI_TTY=$(find_ftdi)
[ -n "$FTDI_TTY" ] && [ -c "$FTDI_TTY" ] || { echo "FT232 (0403:6014) not found"; exit 1; }
echo "E902 console on $FTDI_TTY"

B() { "$S" "$@"; }
BSUDO() { B "printf '%s\n' '$BOARD_PASS' | sudo -S -p '' $*"; }

if [ $NOFLASH = 0 ]; then
	echo "=== 1. flash debug SCP ==="
	bash "$E/tools/flash-scp.sh" "$FW" || { echo "flash failed"; exit 1; }
	# flash-scp.sh reboots the board; wait for ssh to come back
	for i in $(seq 60); do B true 2>/dev/null && break; sleep 5; done
	B true || { echo "board did not come back"; exit 1; }
fi

echo "=== 2. capture SCP console across the test ==="
stty -F $FTDI_TTY 57600 raw -echo </dev/null || exit 1
stty -F $FTDI_TTY 57600 raw -echo </dev/null || exit 1
cat $FTDI_TTY > $OUT/e902.log &
CATPID=$!
sleep 1
BSUDO "dmesg -c >/dev/null 2>&1" >/dev/null

echo "=== 3. deep suspend, RTC alarm +20 s ==="
# run the suspend script as root: pass it base64'd as ONE argument so the
# sudo password pipe and the script body cannot collide on stdin
SUSPEND_SH=$OUT/suspend.sh
cat > $SUSPEND_SH <<'EOF'
mem=$(cat /sys/power/mem_sleep | grep -o '\[.*\]' | tr -d '[]')
echo "mem_sleep current: $mem"
# wake-source census: what the kernel armed + USB devices that hold level IRQs
grep -q "" /proc/bus/input/devices 2>/dev/null && true
awk '$0~/enabled:/{print "wake-armed "$0}' /proc/interrupts 2>/dev/null | head
[ -x /bin/lsusb ] && lsusb 2>/dev/null | sed 's/^/usb-device /'
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo +20 > /sys/class/rtc/rtc0/wakealarm
echo "alarm armed for $(cat /sys/class/rtc/rtc0/wakealarm), now $(date +%s)"
UNBOUND=""
for dev in 4200000.ehci1-controller 4200400.ohci1-controller; do
	D=/sys/bus/platform/devices/$dev
	if [ -e $D/driver/unbind ]; then
		echo $dev > $D/driver/unbind 2>/dev/null && UNBOUND="$UNBOUND $dev"
	fi
done
echo "unbound:$UNBOUND"
sync
echo deep > /sys/power/mem_sleep
echo mem > /sys/power/state
rc=$?
echo "resume rc=$rc  uptime=$(cut -d' ' -f1 /proc/uptime)"
dmesg | grep -E "PM:|suspend|resume|Restarting|usb" | tail -12
echo 0 > /sys/class/rtc/rtc0/wakealarm
EOF
Q=$(base64 -w0 < $SUSPEND_SH)
B "printf '%s\n' '$BOARD_PASS' | sudo -S -p '' sh -c \"\$(echo $Q | base64 -d)\"" > $OUT/board.log 2>&1
RES=$?
sleep 2; kill $CATPID 2>/dev/null; wait $CATPID 2>/dev/null

echo "=== 4. decode ==="
echo "--- E902 console (key lines) ---"
tr -d '\r' < $OUT/e902.log | grep -aE "fallback|first time|dram save done|wait wakeup|wakeup:|ppu on|dram up|wait ac327|cpu0 restore|system tick" | tail -20
echo "--- verdict ---"
tr -d '\r' < $OUT/e902.log | grep -qa "dram para all zero" && echo "  [ok] fallback table active" || echo "  [??] fallback line not seen (FDT may already carry params)"
tr -d '\r' < $OUT/e902.log | grep -qa "dram save done" && echo "  [ok] dram_power_save_process returned" || echo "  [FAIL] no 'dram save done': hang inside the DRAM lib"
tr -d '\r' < $OUT/e902.log | grep -qa "dram up done" && echo "  [ok] DRAM restore ran" || echo "  [??] no 'dram up done' (wake never fired, or hang before it)"
grep -qa "resume rc=0" $OUT/board.log && echo "  [ok] kernel resumed" || echo "  [FAIL] kernel did not resume (rc log follows)"
echo "logs in $OUT"
exit $RES
