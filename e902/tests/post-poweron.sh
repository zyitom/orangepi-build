#!/bin/bash
# post-poweron.sh -- ON TL101. One-shot bring-up verification + demos, run
# AFTER the SD card (carrying our scp firmware) is back in the board and the
# board is powered on.  Usage: bash e902/tests/post-poweron.sh
set -u
ROOT=/home/helios/Desktop/orangepi-build
E=$ROOT/e902
S=$ROOT/ar0234-port/tools/ssh_board.sh
export BOARD_PASS=orangepi
TS=$(date +%Y%m%d-%H%M%S)
LOGD=$E/verify-logs/$TS; mkdir -p "$LOGD"
B()  { $S "$@"; }
SU() { $S "printf 'orangepi\n' | sudo -S $1"; }

echo "[0] log dir: $LOGD"
echo "[1] waiting for board ssh (max 240 s)"
up=0
for i in $(seq 1 48); do
  if B 'uname -r' >/dev/null 2>&1; then up=1; echo "    board up after ~$((i*5)) s"; break; fi
  sleep 5
done
if [ "$up" != 1 ]; then
  echo "VERDICT: NO-BOOT -- bl31 likely rejected the startup-feedback packet."
  echo "RECOVER: move the SD to the TL101 reader, then: sudo bash e902/recover-sd.sh"
  echo "NO-BOOT $(date -Is)" > "$LOGD/verdict.txt"
  exit 2
fi
B 'uname -a && uptime' | tee "$LOGD/board-info.txt"

echo "[2] push awdevmem.py to board (idempotent)"
cat "$E/e902-fw/scripts/awdevmem.py" | B 'mkdir -p ~/e902v2 && cat > ~/e902v2/awdevmem.py' || true

echo "[3] E902 console (FT232H on TL101): start 50 s background capture"
DEV=""
for n in /sys/bus/usb-serial/devices/ttyUSB*; do
  [ -e "$n" ] || continue
  readlink -f "$n/driver" 2>/dev/null | grep -q ftdi_sio && DEV=/dev/$(basename "$n")
done
echo "    console tty: ${DEV:-NOT FOUND}"
CAP=""
if [ -n "$DEV" ]; then
  stty -F "$DEV" 115200 cs8 -cstopb -parenb -crtscts raw -echo 2>/dev/null
  timeout 50 cat "$DEV" > "$LOGD/e902-console.txt" 2>/dev/null &
  CAP=$!
fi

echo "[4] heartbeat x2 (10 s apart) + key registers"
SU 'python3 /home/orangepi/e902v2/awdevmem.py hb' > "$LOGD/heartbeat-1.txt" 2>&1
sleep 10
SU 'python3 /home/orangepi/e902v2/awdevmem.py hb' > "$LOGD/heartbeat-2.txt" 2>&1
SU 'python3 /home/orangepi/e902v2/awdevmem.py read 0x07032204' > "$LOGD/regs.txt" 2>&1
SU 'python3 /home/orangepi/e902v2/awdevmem.py read 0x0701021C' >> "$LOGD/regs.txt" 2>&1
SU 'python3 /home/orangepi/e902v2/awdevmem.py read 0x0300406C' >> "$LOGD/regs.txt" 2>&1
SU 'python3 /home/orangepi/e902v2/awdevmem.py read 0x0709406C' >> "$LOGD/regs.txt" 2>&1
echo "--- heartbeat-2:";  tail -6 "$LOGD/heartbeat-2.txt"
echo "--- regs (RST_START / RISCV_BGR / CPUX ch3 status / CPUS ch3 status):"; tail -4 "$LOGD/regs.txt"

echo "[5] mailbox demos: PING / ECHO / SPI_READ / UART_STAT"
echo "--- drain stale replies (HELLO) up to 4 reads"
for i in 1 2 3 4; do
  r=$(SU 'python3 /home/orangepi/e902v2/awdevmem.py read 0x0300407C' 2>/dev/null | tail -1)
  echo "    drain$i: $r" | tee -a "$LOGD/demos.txt"
done
# NOTE: these demos poke channel 3 from Linux via /dev/mem. That channel's
# ARM side belongs to bl31, whose RPCs spin without timeout on the CPUX FIFO:
# reading 0x0300407C while bl31 waits for a reply would steal it. Only run on
# an idle system (no suspend/resume, no reboot in flight).
pktdemo() { # $1 name  $2 hdr  $3 count  $4 d0  (v3.1 packet protocol)
  echo "--- $1: hdr=$2 count=$3 d0=$4" | tee -a "$LOGD/demos.txt"
  for w in "$2" "$3" "$4"; do
    SU "python3 /home/orangepi/e902v2/awdevmem.py write 0x0709407C $w" >> "$LOGD/demos.txt" 2>&1
  done
  sleep 1
  for w in 1 2 3; do
    r=$(SU 'python3 /home/orangepi/e902v2/awdevmem.py read 0x0300407C' 2>/dev/null | tail -1)
    echo "    reply$w: $r" | tee -a "$LOGD/demos.txt"
  done
}
pktdemo PKT-PING 0x00010200 0x00000001 0x000000C8
pktdemo PKT-ECHO 0x00022000 0x00000001 0x00ABCDEF

echo "[6] UART RX-interrupt proof: push bytes from TL101 into S_UART0 RX, recount"
if [ -n "$DEV" ]; then
  printf 'xyz\n' > "$DEV" 2>/dev/null || true
  sleep 1
  sleep 1
  SU 'python3 /home/orangepi/e902v2/awdevmem.py hb' >> "$LOGD/heartbeat-3.txt" 2>&1
  echo "    heartbeat-3 word11 (uart_rx) should now count the injected bytes" | tee -a "$LOGD/demos.txt"
  echo "    uart-rx-count reply: $r   (expect 0x3500000N with N>=4)" | tee -a "$LOGD/demos.txt"
fi
[ -n "$CAP" ] && wait $CAP 2>/dev/null
echo "    console bytes: $(wc -c < "$LOGD/e902-console.txt" 2>/dev/null || echo 0)"

echo "[7] main-core side effects (A3): devfreq noise, cpufreq, scp dmesg"
B 'dmesg | grep -icE "ddrfreq|devfreq" || true' > "$LOGD/devfreq-count.txt" 2>&1
B 'cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null; dmesg | grep -iE "arisc|scp|ddr" | tail -8' > "$LOGD/dmesg-scp.txt" 2>&1
echo "    devfreq-related dmesg lines: $(cat "$LOGD/devfreq-count.txt")"

echo "[8] verdict"
MAGIC=$(grep -ic 'e902c0de' "$LOGD/heartbeat-2.txt" || true)
BANNER=$(grep -c 'A733 E902 firmware' "$LOGD/e902-console.txt" 2>/dev/null || true)
TICKS=$(grep -c '\[tick\]' "$LOGD/e902-console.txt" 2>/dev/null || true)
PONG=$(grep -c '0x02000001' "$LOGD/demos.txt" 2>/dev/null || true)
if [ "$MAGIC" -ge 1 ]; then echo "  heartbeat magic 0xE902C0DE : FOUND (E902 executes our firmware)"; else echo "  heartbeat magic : MISSING"; V=FAIL; fi
if [ "$BANNER" -ge 1 ]; then echo "  console banner  : FOUND"; else echo "  console banner  : not captured (check FT232H)"; fi
echo "  console [tick] lines: $TICKS ; PONG replies: $PONG"
if [ "${V:-}" = FAIL ]; then echo "VERDICT: FAIL -- see $LOGD"; exit 3; fi
echo "VERDICT: PASS -- E902 is ours. logs: $LOGD"
echo "PASS $(date -Is)" > "$LOGD/verdict.txt"
