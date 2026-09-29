#!/usr/bin/env bash
# vulkan-debug.sh — 板上 Vulkan 失败点定位（按顺序执行，把输出发回）
# 前提: #53+ 镜像（带 strace + 0019 内核补丁），BOARD=板子IP
set -uo pipefail
SSH="ar0234-port/tools/ssh_board.sh"; BOARD="${BOARD:?BOARD=板子IP}"
run() { echo "### $1"; BOARD=$BOARD bash $SSH "$2" 2>&1 | tail -8; }
runS() { echo "### (sudo) $1"; BOARD=$BOARD bash $SSH -s "$2" 2>&1 | tail -8; }

# 1. vendor 库的调试开关：PVRDebugLevel 拉满，libsrv_um 会打印
#    "BridgeGetMultiCoreInfo: BridgeCall failed" + 具体错误码
run "PVRDebugLevel 全开跑 vktest" "PVRDebugLevel=-1 /tmp/vktest 2>&1 | head -20"

# 2. strace：看 PVRSRVGetMultiCoreInfo 的 ioctl 返回值
runS "strace 跟踪 ioctl" "strace -f -e trace=ioctl -o /tmp/vk.strace /tmp/vktest 2>&1 | tail -2; grep -c ENOTTY /tmp/vk.strace; tail -5 /tmp/vk.strace"

# 3. 0019 内核补丁：RGXInitMultiCoreInfo 是否在开机时报错
runS "dmesg 找 multicore 初始化错误" "dmesg | grep -iE 'MultiCore|InitMultiCore' | head -6"
