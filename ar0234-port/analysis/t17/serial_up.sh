#!/bin/sh
# serial_up.sh [logfile] - start the continuous serial logger as a background
# systemd unit (per HANDOFF 3.13: never nohup; and sc.py is unusable while it
# runs, so remember serial_down.sh).
set -e

# Host sudo: feed HOST_SUDO_PASS on stdin when set (unattended), else plain sudo.
sudo_() { if [ -n "${HOST_SUDO_PASS+x}" ]; then printf '%s\n' "$HOST_SUDO_PASS" | sudo -S -p '' "$@"; else sudo "$@"; fi; }
LOG=${1:-/home/helios/Desktop/orangepi-build/ar0234-port/analysis/t17/serial.log}
P=/home/helios/Desktop/orangepi-build/ar0234-port
CMD=${2:-/tmp/t17.cmd}

sudo_ systemctl stop t17serial 2>/dev/null || true
sudo_ systemd-run --unit=t17serial --collect \
	/usr/bin/python3 "$P/tools/serial_log.py" --log "$LOG" --cmd "$CMD"
sleep 2
sudo_ systemctl is-active t17serial
echo "log=$LOG cmd=$CMD"
