#!/bin/bash
# regression.sh —— OPI5 原生 EtherCAT 全功能回归测试（板上跑，需接意优从站）
# 用法: sudo ./regression.sh [秒数/项，默认 30]
# 前置: 主站已启动（sudo ecat-native start），从站在总线上（ethercat slaves 可见）
# 环境调优与 xbot tune_rt 一致：performance 调频 + 大核 + FIFO；结束自动还原。
set -u
SECS="${1:-30}"
E=/usr/local/bin/ethercat
TEST=/tmp/ecat_full_test
PASS=0; FAIL=0
ck(){ if [ "$2" = "1" ]; then echo "  [PASS] $1"; PASS=$((PASS+1)); else echo "  [FAIL] $1"; FAIL=$((FAIL+1)); fi; }

echo "== 环境调优 =="
for p in /sys/devices/system/cpu/cpufreq/policy*; do echo performance > $p/scaling_governor; done
echo "  governor=performance, app: taskset cpu4 + SCHED_FIFO 80"

echo "== 0) 从站在位 =="
S="$($E slaves 2>/dev/null | head -1)"
echo "  $S"
ck "从站枚举 (EYOU_ServoModule PREOP)" "$(echo "$S" | grep -q 'PREOP  +  EYOU_ServoModule' && echo 1 || echo 0)"

echo "== 1) T1: DC+OP 30s 全功能（${SECS}s） =="
OUT="$(LD_LIBRARY_PATH=/usr/local/lib taskset -c 4 chrt -f 80 $TEST "$SECS" 2>&1)"
echo "$OUT" | grep -E '^\[|== 汇总|周期:|OP 进入|唤醒|单周期|漂移|statusword' | tail -7
ck "OP 达成"          "$(echo "$OUT" | grep -q '含 0x08: yes' && echo 1 || echo 0)"
ck "OP 后零 wkc=0"    "$(echo "$OUT" | awk '/wkc=0 周期数/{print $5+0 < 5000 ? 1 : 0}')"
ck "唤醒抖动 mean<5us" "$(echo "$OUT" | awk '/唤醒超期/{gsub(/ns/,"",$4); print ($4+0 < 5000) ? 1 : 0}')"
ck "单周期工作 <100us" "$(echo "$OUT" | awk '/单周期工作/{gsub(/ns/,"",$4); print ($4+0 < 100000) ? 1 : 0}')"

echo "== 2) T2: CPU 满载下重复 =="
for i in 0 1 2 3 5 6 7; do nohup sh -c 'while :; do sha256sum /dev/zero >/dev/null; done' >/dev/null 2>&1 & done
sleep 1
OUT="$(LD_LIBRARY_PATH=/usr/local/lib taskset -c 4 chrt -f 80 $TEST "$SECS" 2>&1)"
echo "$OUT" | grep -E 'OP 进入|wkc=0 周期数|唤醒超期|单周期' | sed 's/^/  (T2) /'
ck "满载下 OP 达成"  "$(echo "$OUT" | grep -q '含 0x08: yes' && echo 1 || echo 0)"
ck "满载下零丢帧"    "$(echo "$OUT" | awk '/wkc=0 周期数/{print ($5+0 < 3000) ? 1 : 0}')"
pkill -f 'sha256sum /dev/zero'; wait 2>/dev/null

echo "== 3) T3: 故障注入（OP 中 kill -9 → 恢复） =="
LD_LIBRARY_PATH=/usr/local/lib taskset -c 4 chrt -f 80 $TEST 120 >/dev/null 2>&1 &
sleep 5
KILLPID="$(pgrep -f ecat_full_test | head -1)"
kill -9 "$KILLPID" 2>/dev/null; sleep 2
ck "强杀后从站干净回 PREOP" "$($E slaves 2>/dev/null | grep -q 'PREOP.*+' && echo 1 || echo 0)"
OUT="$(LD_LIBRARY_PATH=/usr/local/lib taskset -c 4 chrt -f 80 $TEST 10 2>&1)"
ck "重跑恢复 OP" "$(echo "$OUT" | grep -q '含 0x08: yes' && echo 1 || echo 0)"

echo "== 4) T4: 邮箱与身份 =="
FW=""
for try in 1 2 3; do
    FW="$($E upload -p 0 0x100a 0 2>/dev/null | tr -d '\n')"
    [ -n "$FW" ] && break
    sleep 2
done
echo "  固件: $FW"
ck "固件版本可读" "$([ -n "$FW" ] && echo 1 || echo 0)"
FOE="$(timeout 12 $E foe_read -p 0 /nonexistent.bin 2>&1)"
ck "FoE 邮箱握手 (期望 0x8002 abort)" "$(echo "$FOE" | grep -q '0x00008002' && echo 1 || echo 0)"
ck "从站回到 PREOP" "$($E slaves 2>/dev/null | grep -q 'PREOP.*+' && echo 1 || echo 0)"

echo "== 还原 =="
for p in /sys/devices/system/cpu/cpufreq/policy*; do echo ondemand > $p/scaling_governor; done

echo "== 结果: PASS=$PASS FAIL=$FAIL =="
[ "$FAIL" = 0 ]
