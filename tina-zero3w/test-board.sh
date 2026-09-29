#!/usr/bin/env bash
# test-board.sh — 上板验收脚本（对照 docs/ORIGINAL-BRIEF.md §验收 1-8）
# 前提: 镜像已烧卡、板子起来并连上 WiFi（172.16.0.193，或 BOARD=... 指定）。
# 串口（验收 2）人工核对: /dev/ttyUSB0 115200 出登录提示且无人工干预。
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SSH="$REPO_ROOT/ar0234-port/tools/ssh_board.sh"
RT="$REPO_ROOT/ar0234-port/tools/rt"
TS=$(date +%Y%m%d-%H%M%S)
REPORT="${1:-$REPO_ROOT/tina-zero3w/test-report-$TS.txt}"
FAIL=0

say()  { printf '\n######## %s\n' "$*" | tee -a "$REPORT"; }
run()  { printf '$ %s\n' "$1" | tee -a "$REPORT"; "$SSH" "$1" 2>&1 | tee -a "$REPORT"; }
runS() { printf '$ (sudo) %s\n' "$1" | tee -a "$REPORT"; "$SSH" -s "$1" 2>&1 | tee -a "$REPORT"; }
check() { # check <名称> <上一命令是否期望通过>
  if "$SSH" "$1" >/dev/null 2>&1; then
    printf '[PASS] %s\n' "$2" | tee -a "$REPORT"
  else
    printf '[FAIL] %s\n' "$2" | tee -a "$REPORT"; FAIL=1
  fi
}

mkdir -p "$(dirname "$REPORT")"; : > "$REPORT"
echo "Zero 3W Tina/Buildroot 验收报告 $TS" | tee -a "$REPORT"

say "1/2  系统与串口登录（免干预性靠烧卡后直接上电验证，本脚本核对系统标识）"
run "uname -a"
check "uname -a | grep -q 'PREEMPT_RT'" "3. 内核为 PREEMPT_RT (uname -v 含 PREEMPT_RT)"
check "uname -r | grep -q '^6.6.98-rt58'" "3. 内核版本 6.6.98-rt58（我们的树）"

say "4. WiFi (aic8800) + ssh（ssh 本身就在用）"
run "dmesg | grep -i aic8800 | head -5"
run "ip -4 addr show wlan0"
run "ip route"

say "6. E902 SCP + amp_timestamp"
# dmesg 需要 root（非 root klogctl 被拒）；探测信息在内核 dmesg 里
check "echo orangepi | sudo -S dmesg 2>/dev/null | grep -qi 'amp.timestamp'" "6. amp-timestamp 驱动已探测"
runS "dmesg | grep -i 'amp.timestamp'"
run "cat /sys/kernel/*/amp_timestamp/freqid 2>/dev/null; cat /sys/class/misc/amp_timestamp/freqid 2>/dev/null" || true
runS "dmesg | grep -iE 'arisc|scp ' | head -5"

say "5a. 板上编译 cltest + OpenCL (PowerVR)"
run "gcc --version | head -1"
$SSH "mkdir -p /tmp/acc" </dev/null >/dev/null 2>&1
$SSH "cat > /tmp/acc/cltest.c" < "$RT/cltest.c"
run "gcc -O2 -o /tmp/acc/cltest /tmp/acc/cltest.c -lOpenCL && /tmp/acc/cltest"

say "5b. Vulkan (vulkaninfo → PowerVR BXM-4-64)"
run "vulkaninfo --summary 2>/dev/null | grep -iE 'powervr|bxm' || vulkaninfo 2>/dev/null | grep -iE 'powervr|bxm' | head -5"

say "7. RT 延迟 (waitlat, 独占 cpu5; 目标 hybrid max < 2 µs)"
$SSH "cat > /tmp/acc/waitlat.c" < "$RT/waitlat.c"
run "gcc -O2 -o /tmp/acc/waitlat /tmp/acc/waitlat.c"
runS "taskset -c 5 /tmp/acc/waitlat 5 hybrid 20"
runS "taskset -c 5 /tmp/acc/waitlat 5 spin 10"

say "8. AR0234/vin 驱动 + ISP/cedarc 用户态"
runS "dmesg | grep -iE 'vin|ar0234' | head -10"
run "ls /dev/video* 2>/dev/null"
check "ls /usr/lib/libisp.so /usr/lib/libAWIspApi.so >/dev/null" "8. libisp/libAWIspApi 在 rootfs"
check "ls /usr/lib/libvdecoder.so /etc/cedarc.conf >/dev/null" "8. libcedarc 在 rootfs"

say "GPU 内核模块与库落位"
run "ls /lib/modules/\$(uname -r)/extra/ 2>/dev/null"
run "lsmod | grep -E 'pvrsrvkm|aic8800' | head -5"
check "test -e /usr/lib/libsrv_um.so.1 -o -e /usr/lib/libsrv_um.so" "GPU 用户态库在位"

say "结论"
if [[ $FAIL -eq 0 ]]; then
  echo "全部 [PASS] 项通过；逐项对照见上。" | tee -a "$REPORT"
else
  echo "存在 [FAIL] 项 —— 见报告 $REPORT" | tee -a "$REPORT"
fi
echo "报告: $REPORT"
exit $FAIL
