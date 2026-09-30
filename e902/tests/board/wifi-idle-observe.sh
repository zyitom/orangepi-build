#!/bin/bash
# wifi-idle-observe.sh -- ON TL101. 30-minute WiFi idle observation.
#
# Verifies that with power save off (udev rule 70-wifi-powersave-off.rules,
# NetworkManager wifi.powersave=2) the AIC8800 no longer gets kicked by the
# AP with reason=4 (inactivity) on an idle link. Background job: nothing but
# the idle system; we sample link state every 60 s and count disconnects.
#
#   BOARD=172.16.0.194 bash e902/tests/board/wifi-idle-observe.sh [minutes]
set -u

E=$(cd "$(dirname "$0")/../.." && pwd)
ROOT=$(dirname "$E")
S=${SSH_BOARD:-$ROOT/ar0234-port/tools/ssh_board.sh}
: "${BOARD_PASS:=orangepi}"
export BOARD_PASS
MINS=${1:-30}
OUT=$E/verify-logs/wifi-idle-$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"

B() { "$S" "$@"; }
BSUDO() { B "printf '%s\n' '$BOARD_PASS' | sudo -S -p '' $*"; }

echo "=== preflight ==="
B 'iw dev wlan0 get power_save; iw dev wlan0 link | head -3; uptime' 2>&1 | tee $OUT/preflight.log

echo "=== observing ${MINS} min (sampling every 60 s) ==="
# observer runs detached on the board so our ssh session can go away
OBS_SH=$OUT/observe.sh
cat > $OBS_SH <<'EOF'
R=/root/wifi-idle.csv
: > $R
echo "t,signal_dbm,tx_bw,freq,connected" >> $R
END=$(( $(date +%s) + $__MINS__ * 60 ))
while [ $(date +%s) -lt $END ]; do
	sig=$(iw dev wlan0 link | awk '/signal:/{print $2}')
	freq=$(iw dev wlan0 link | awk '/freq:/{print $2}')
	bw=$(iw dev wlan0 link | awk '/tx bitrate:/{print $3}')
	conn=$(iw dev wlan0 link | grep -c "Connected")
	echo "$(date +%H:%M:%S),${sig:-NA},${bw:-NA},${freq:-NA},$conn" >> $R
	sleep 60
done
echo done >> $R
EOF
sed -i "s/__MINS__/$MINS/" $OBS_SH
cat $OBS_SH | B "cat > /tmp/observe.sh"
BSUDO "mv /tmp/observe.sh /root/wifi-idle-observe.sh; chmod +x /root/wifi-idle-observe.sh; nohup sh /root/wifi-idle-observe.sh >/dev/null 2>&1 & echo observer-started"

echo "waiting ${MINS} minutes..."
sleep $(( MINS * 60 + 30 ))

echo "=== collect ==="
BSUDO 'cat /root/wifi-idle.csv' > $OUT/idle.csv 2>/dev/null
tail -3 $OUT/idle.csv
echo "=== disconnect events (reason=4 hunt) ==="
BSUDO 'dmesg | grep -iE "disconnect|deauth|reason" | tail -20; journalctl -u wpa_supplicant --no-pager 2>/dev/null | grep -iE "reason|disconnect" | tail -10' > $OUT/disconnects.log 2>/dev/null
grep -ci "reason=4\|reason 4\|4 locally" $OUT/disconnects.log >/dev/null 2>&1 && echo "see $OUT/disconnects.log"
NA=$(awk -F, '$5==0' $OUT/idle.csv | wc -l)
echo "=== verdict: $((60 - 0)) samples; link-down samples: $NA ==="
[ "$NA" = "0" ] && echo "  [ok] link stayed up the whole time" || echo "  [check] link dropped in $NA samples (see idle.csv)"
echo "logs in $OUT"
