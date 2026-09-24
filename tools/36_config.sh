#!/bin/bash
# 36_config.sh <module: fix|old> <vinc4: on|off> <tag> [devspec] [secs]
set -u
cd /home/helios/Desktop/orangepi-build/ar0234-port || exit 1
B() { tools/ssh_board.sh "$1" < /dev/null; }
R() { B "printf ' \n' | sudo -S -p '' /tmp/boardtest.sh $*"; }
DTB=/boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb
UPD=/lib/modules/6.6.98-sun60iw2/updates

MOD=$1; V4=$2; TAG=$3; SPEC=${4:-0:1920:1080}; SECS=${5:-8}

case "$MOD" in
  fix) SRC=$UPD/vin_v4l2.ko.fix-t14 ;;
  old) SRC=$UPD/vin_v4l2.ko.orig-bsp ;;
  *) echo "bad module $MOD"; exit 2 ;;
esac

echo "=== DTB: vinc4 status -> $V4"
B "printf ' \n' | sudo -S -p '' sh -c 'fdtput -t s $DTB /soc@3000000/vind@5800800/vinc@5831000 status $V4; fdtget -t s $DTB /soc@3000000/vind@5800800/vinc@5831000 status'"

echo "=== module -> $MOD"
B "printf ' \n' | sudo -S -p '' sh -c 'cp -a $SRC $UPD/vin_v4l2.ko && depmod -a && md5sum $SRC $UPD/vin_v4l2.ko'"

echo "=== reboot"
B "printf ' \n' | sudo -S -p '' systemd-run --on-active=2 /bin/systemctl reboot"
for i in $(seq 1 40); do
  sleep 5
  up=$(B 'cut -d. -f1 /proc/uptime' 2>/dev/null | tr -d '\r\n ')
  case "$up" in ''|*[!0-9]*) continue;; esac
  if [ "$up" -lt 120 ] && [ "$i" -gt 2 ]; then echo "BOARD BACK (uptime ${up}s)"; break; fi
done
sleep 8
echo "=== state: srcversion / videos / oops count in dmesg"
B 'cat /sys/module/vin_v4l2/srcversion; echo; ls /dev/video*; printf " \n" | sudo -S -p "" dmesg | grep -cE "Oops|panic|Call trace"'
cp tools/boardtest.sh /tmp/boardtest.sh
b64=$(base64 -w0 /tmp/boardtest.sh)
B "printf ' \n' | sudo -S -p '' sh -c 'echo $b64 | base64 -d > /tmp/boardtest.sh; chmod +x /tmp/boardtest.sh'"

echo
echo "############ $TAG : stream $SPEC for ${SECS}s   (module=$MOD vinc4=$V4)"
R "stream $TAG $SPEC $SECS"
