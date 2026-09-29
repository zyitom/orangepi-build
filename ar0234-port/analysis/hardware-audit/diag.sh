#!/bin/sh
SUDO="printf ' \n' | sudo -S -p ''"
echo "=== uptime/health ==="; cut -d' ' -f1 /proc/uptime
echo "=== full vfr output, 6 s, 1200p120 ==="
/home/orangepi/ar0234test/vfr -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 6; echo "  vfr rc=$?"
echo "=== vi0/vi4 ==="; sed -n '/^vi0:/,/^\*\*\*/p' /sys/kernel/debug/mpp/vi
echo "=== dmesg tail 40 ==="; dmesg | tail -40
echo "=== 3A service ==="; systemctl status ar0234-3ad --no-pager | head -12
echo "=== procs holding video ==="; fuser -v /dev/video0 /dev/video4 2>&1
echo "=== retry after a 5 s pause ==="; sleep 5
/home/orangepi/ar0234test/vfr -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 6 2>&1 | grep -E 'RESULT|fmt|Invalid|failed'
echo "=== DONE diag ==="
