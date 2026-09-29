#!/usr/bin/env bash
# deploy.sh — 把编译产物/文件推到运行中的 Zero 3W（开发迭代用，秒级）
#
# 用法:
#   bash tina-zero3w/deploy.sh 文件 [文件...] [目录]     # 默认推到 /tmp/acc
#   FILE=板载路径 bash tina-zero3w/deploy.sh 本地文件     # 指定板载目标路径
#
# 认证走 ar0234-port/tools/ssh_board.sh（BOARD/BOARD_PASS 环境变量同其约定）。
# 例: 改了 cltest.c → 板上重编 → 立刻跑:
#   bash tina-zero3w/deploy.sh cltest.c
#   BOARD=... tools/ssh_board.sh "gcc -O2 -o /tmp/acc/cltest /tmp/acc/cltest.c -lOpenCL && /tmp/acc/cltest"
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SSH="$REPO_ROOT/ar0234-port/tools/ssh_board.sh"
DEST="${FILE:-/tmp/acc}"

[[ $# -ge 1 ]] || { echo "用法: deploy.sh 文件 [文件...] [板载目录]"; exit 1; }
for f in "$@"; do
	[[ -f "$f" ]] || { echo "跳过（不是文件）: $f"; continue; }
	b64=$(mktemp)
	base64 -w0 "$f" > "$b64"
	bn=$(basename "$f")
	BOARD="${BOARD:-172.16.0.193}" bash "$SSH" "mkdir -p '$DEST'" >/dev/null
	base64 -w0 "$f" | BOARD="${BOARD:-172.16.0.193}" bash "$SSH" "base64 -d > '$DEST/$bn'" >/dev/null
	echo "[deploy] $f -> $DEST/$bn"
	rm -f "$b64"
done
