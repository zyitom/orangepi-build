#!/bin/bash
# deep-suspend-retest.sh -- ON TL101. Retest deep suspend with the fixed SCP.
#
# Validates the deep-suspend fix chain (see e902/doc/deep-suspend-HANDOFF.md
# and tina-zero3w/docs/STATUS.md section 6):
#   1. flash the debug SCP (patches 0004 fallback + patches-debug probes)
#   2. reboot, confirm on the E902 console that the fallback fired
#      ("dram para all zero -- using built-in zero3w values")
#   3. deep suspend with an RTC alarm +20 s, both serials captured with
#      host timestamps (e902/tools/serial-cap.py)
#   4. decode: "dram save done" -> "wake src N after S s real" -> "dram up
#      done" + "dram sum ... ok" -> "cpu0 start" -> kernel must come back.
#
# Usage:
#   BOARD=172.16.0.193 bash e902/tests/board/deep-suspend-retest.sh [--no-flash]
#   SCP_FW=e902/fw-out/vendor-scp.bin BOARD=... bash ...   # production SCP:
#     silent console, so only the kernel side (resume rc, timing) is judged
# Needs the FT232 (0403:6014) for the SCP console at 57600 and the CH340
# (1a86) for the A55 console at 115200; both are found via /dev/serial/by-id.
#
# The suspend runs detached on the board (nohup, log in /var/tmp) so the ssh
# drop at suspend cannot lose the result; the host polls until the board
# answers again or WAIT_S (default 240) seconds pass. A board that does not
# resume stays dead (SCP parked in wait_cpu0_resume) until a power cycle or
# a warm reset several minutes later.
set -u

E=$(cd "$(dirname "$0")/../.." && pwd)          # e902/
ROOT=$(dirname "$E")
S=${SSH_BOARD:-$ROOT/ar0234-port/tools/ssh_board.sh}
: "${BOARD_PASS:=orangepi}"
export BOARD_PASS
FW=${SCP_FW:-$E/fw-out/vendor-scp-debug.bin}	# SCP_FW=.../vendor-scp.bin: production
OUT=$E/verify-logs/deep-retest-$(date +%Y%m%d-%H%M%S)
WAIT_S=${WAIT_S:-240}
mkdir -p "$OUT"

NOFLASH=0
[ "${1:-}" = "--no-flash" ] && NOFLASH=1

# find the consoles via /dev/serial/by-id (ttyUSB numbering swaps between
# host boots; the CH340 big-core console also shows up as ttyUSB*)
FTDI_TTY=$(readlink -f /dev/serial/by-id/usb-FTDI_*-if00-port0 2>/dev/null | head -1)
ARM_TTY=$(readlink -f /dev/serial/by-id/usb-1a86_*-if00-port0 2>/dev/null | head -1)
[ -n "$FTDI_TTY" ] && [ -c "$FTDI_TTY" ] || { echo "FT232 (0403:6014) not found"; exit 1; }
echo "E902 console on $FTDI_TTY, ARM console on ${ARM_TTY:-(none)}"

# A capture left over from an aborted run keeps reading the tty and steals
# half of every later capture (this garbled the 2026-10-01 02:10 run). Clear
# our own readers before starting, and never leave ours behind.
for t in $FTDI_TTY $ARM_TTY; do
	pkill -f "^cat $t\$" 2>/dev/null
	pkill -f "serial-cap.py $t " 2>/dev/null
done
CAP_PIDS=""
cleanup() { [ -n "$CAP_PIDS" ] && kill $CAP_PIDS 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

B() { timeout 60 "$S" "$@"; }
BSUDO() { B "printf '%s\n' '$BOARD_PASS' | sudo -S -p '' $*"; }

if [ $NOFLASH = 0 ]; then
	echo "=== 1. flash debug SCP ==="
	bash "$E/tools/flash-scp.sh" "$FW" || { echo "flash failed"; exit 1; }
	# flash-scp.sh reboots the board; wait for ssh to come back
	for i in $(seq 60); do B true 2>/dev/null && break; sleep 5; done
	B true || { echo "board did not come back"; exit 1; }
fi

echo "=== 2. capture SCP + ARM consoles across the test ==="
python3 "$E/tools/serial-cap.py" "$FTDI_TTY" 57600 "$OUT/e902.log" &
CAP_PIDS="$!"
if [ -n "$ARM_TTY" ] && [ -c "$ARM_TTY" ]; then
	python3 "$E/tools/serial-cap.py" "$ARM_TTY" 115200 "$OUT/arm.log" &
	CAP_PIDS="$CAP_PIDS $!"
fi
sleep 1
BSUDO "dmesg -c >/dev/null 2>&1" >/dev/null
BOOT_ID=$(B 'cat /proc/sys/kernel/random/boot_id')

echo "=== 3. deep suspend, RTC alarm +20 s ==="
SUSPEND_SH=$OUT/suspend.sh
cat > $SUSPEND_SH <<'EOF'
sleep 3		# let the launching ssh session close first
mem=$(cat /sys/power/mem_sleep | grep -o '\[.*\]' | tr -d '[]')
echo "mem_sleep current: $mem"
# wake-source census: which IRQs are wakeup-enabled
for f in /sys/kernel/irq/*/wakeup; do
	[ "$(cat $f)" = enabled ] && echo "wake-enabled $(dirname $f | xargs basename) $(cat $(dirname $f)/actions 2>/dev/null)"
done
# keep the kernel console alive through suspend/resume and make it talk:
# the image boots with "quiet loglevel=3", which hides every PM message
echo 8 > /proc/sys/kernel/printk
echo N > /sys/module/printk/parameters/console_suspend
echo 1 > /sys/power/pm_debug_messages
echo 1 > /sys/power/pm_print_times
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo +20 > /sys/class/rtc/rtc0/wakealarm
echo "alarm armed for $(cat /sys/class/rtc/rtc0/wakealarm), now $(date +%s)"
sync
echo deep > /sys/power/mem_sleep
echo mem > /sys/power/state
rc=$?
echo "resume rc=$rc  now $(date +%s) uptime=$(cut -d' ' -f1 /proc/uptime)"
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo s2idle > /sys/power/mem_sleep
dmesg | grep -aE "PM:|suspend|resume|Restarting|psci|CPU[0-9]|Enabling|Disabling" | tail -40
sync
EOF
# ship the script, then start it detached so the ssh drop cannot kill it
Q=$(base64 -w0 < $SUSPEND_SH)
B "echo $Q | base64 -d > /tmp/deep-retest.sh"
BSUDO "sh -c 'nohup sh /tmp/deep-retest.sh < /dev/null > /var/tmp/deep-retest.log 2>&1 &'"
T0=$(date +%s)
echo "suspend launched at $(date +%T); polling for up to ${WAIT_S}s"
RESUMED=0
sleep 15
while [ $(( $(date +%s) - T0 )) -lt $WAIT_S ]; do
	if B "grep -q 'resume rc=' /var/tmp/deep-retest.log" 2>/dev/null; then
		RESUMED=1; break
	fi
	sleep 5
done
echo "poll ended after $(( $(date +%s) - T0 ))s, resumed=$RESUMED"
if [ $RESUMED = 1 ]; then
	B "cat /var/tmp/deep-retest.log" > $OUT/board.log 2>&1
	NOW_ID=$(B 'cat /proc/sys/kernel/random/boot_id')
	[ "$NOW_ID" = "$BOOT_ID" ] || echo "WARNING: boot_id changed -- the board rebooted"
	BSUDO "dmesg" > $OUT/dmesg.log 2>&1
else
	# give the consoles a little longer for the SCP heartbeat / A55 prints
	sleep 20
	# debug SCP parked in wait_cpu0_resume offers a console reboot ("RST",
	# patches-debug/0004): use it so the next run needs no power cycle
	if [ "${RESCUE:-1}" = 1 ] && tr -d '\r' < $OUT/e902.log | grep -qa "type RST on this console"; then
		echo "board parked in wait_cpu0_resume: sending RST on the SCP console"
		printf 'RST' > $FTDI_TTY
		for i in $(seq 36); do sleep 5; B true 2>/dev/null && { echo "board back after $((i * 5)) s"; break; }; done
	fi
fi
cleanup; CAP_PIDS=""

echo "=== 4. decode ==="
echo "--- E902 console (key lines) ---"
tr -d '\r' < $OUT/e902.log | grep -aE "fallback|all zero|first time|dram save done|wakeup:|wake src|ppu on|dram up|dram sum|cpu0 start|wait ac327|cpu0 restore|hb|crc|ignore" | tail -40
echo "--- ARM console during/after suspend ---"
[ -f $OUT/arm.log ] && tr -d '\r' < $OUT/arm.log | grep -aE "PM:|BL31|NOTICE|ERROR|psci|CPU[0-9]|Enabling|Disabling|resume|panic|SError|Unhandled|Internal error|rcu|timeout" | tail -30 || echo "(no ARM capture)"
echo "--- verdict ---"
L=$(tr -d '\r' < $OUT/e902.log)
# the SCP parses /dram at its own startup, i.e. during the flash reboot
FL=$(ls -dt $E/verify-logs/flash-* 2>/dev/null | head -1)
{ echo "$L"; [ $NOFLASH = 0 ] && [ -n "$FL" ] && tr -d '\r' < $FL/e902-console.log; } | grep -qa "dram para all zero" \
	&& echo "  [ok] fallback table active" || echo "  [??] fallback line not seen (FDT may already carry params)"
echo "$L" | grep -qa "dram save done" && echo "  [ok] dram_power_save_process returned" || echo "  [FAIL] no 'dram save done': hang inside the DRAM lib"
echo "$L" | grep -aE "wake src [0-9]+ after" | sed 's/^/  [info] /'
echo "$L" | grep -qa "dram up done" && echo "  [ok] DRAM restore ran" || echo "  [??] no 'dram up done' (wake never fired, or hang before it)"
echo "$L" | grep -qa "dram sum.*CHANGED" && echo "  [FAIL] DRAM content changed across standby" || { echo "$L" | grep -qa "dram sum" && echo "  [ok] BL31 + kernel text survived self-refresh"; }
echo "$L" | grep -qa "cpu0 restore finished" && echo "  [ok] BL31 sent the restore notify" || echo "  [??] no restore notify from BL31"
[ $RESUMED = 1 ] && grep -qa "resume rc=0" $OUT/board.log && echo "  [ok] kernel resumed" || echo "  [FAIL] kernel did not resume"
if [ $RESUMED = 1 ]; then
	# alarm is +20 s from "now"; a resume well before that = another wake source
	a=$(sed -n 's/^alarm armed for \([0-9]*\), now \([0-9]*\).*/\1/p' $OUT/board.log)
	r=$(sed -n 's/^resume rc=[0-9-]*  now \([0-9]*\).*/\1/p' $OUT/board.log)
	[ -n "$a" ] && [ -n "$r" ] && { [ "$r" -ge "$a" ] \
		&& echo "  [ok] woke by the RTC alarm (resumed $((r - a)) s after it was due)" \
		|| echo "  [??] resumed $((a - r)) s BEFORE the alarm -- another wake source"; }
fi
echo "logs in $OUT"
[ $RESUMED = 1 ]
