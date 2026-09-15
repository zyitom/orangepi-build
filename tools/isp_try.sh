#!/bin/sh
# isp_try.sh <param.bin> <tag> [frames]  -- run libisp 3A with a parameter file,
# print colour stats of the last NV12 frame and save /tmp/isp/<tag>.jpg (960x540)
BIN=$1; TAG=$2; N=${3:-90}
S() { printf ' \n' | sudo -S -p '' "$@"; }
S mkdir -p /mnt/extsd
S cp "$BIN" /mnt/extsd/isp_param_config.bin
S rm -f /mnt/isp0_*_ar0234_mipi_ctx_saved.bin
rm -rf /tmp/isp; mkdir -p /tmp/isp; cd /tmp/isp || exit 1
S timeout 40 stdbuf -oL AWISPdemo 0 0 1920 1080 /tmp/isp 4 "$N" 30 > /tmp/isp/log.txt 2>&1
grep -E "load bin|Read seccess|ERR|cannot find" /tmp/isp/log.txt | grep -v "debug_info\|tdm event\|stats error" | head -5
LAST=$(ls /tmp/isp/fb0_y4_1920_1080_*.bin 2>/dev/null | sort -t_ -k5 -n | tail -1)
[ -n "$LAST" ] || { echo "no frames"; exit 1; }
python3 - "$LAST" "/tmp/isp/$TAG.jpg" <<'EOF'
import sys
from PIL import Image
W, H = 1920, 1080
d = open(sys.argv[1], "rb").read()
y = Image.frombytes("L", (W, H), d[:W * H])
uv = d[W * H:W * H + W * H // 2]
u = Image.frombytes("L", (W // 2, H // 2), uv[0::2]).resize((W, H))
v = Image.frombytes("L", (W // 2, H // 2), uv[1::2]).resize((W, H))
img = Image.merge("YCbCr", (y, u, v)).convert("RGB").resize((960, 540))
img.save(sys.argv[2], quality=88)
r, g, b = [sum(c.getdata()) / (960 * 540) for c in img.split()]
print("mean RGB %.0f %.0f %.0f" % (r, g, b))
EOF
