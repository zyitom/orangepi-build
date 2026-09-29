#!/usr/bin/env bash
# fetch-sdk.sh — 拉取/更新全志 Tina (tina-ng) SDK 子集到 ~/tina5
#
# 约束（见 PLAN.md §2.6）：全志源码一律不进 orangepi-build 仓库，全部落在仓库外的 ~/tina5。
# 这里只浅克隆构建/比对所需的最小子集（约 5 GB），不做完整 repo sync（98 个项目 ~30 GB，
# 且 11 个仓库的大文件只发布在 MEGA，git 里本来就不完整——见 docs/TINA-NOTES.md §大文件）。
#
# 用法:  bash tina-zero3w/fetch-sdk.sh [--update]
#   默认已存在的仓库原样保留；--update 时对已克隆的仓库做 git fetch + fast-forward。
set -euo pipefail

SDK="${TINA_SDK_DIR:-$HOME/tina5}"
BASE="https://gitlab.com/tina5.0_aiot"
BRANCH="product-aiot-stable"
UPDATE=0
[[ "${1:-}" == "--update" ]] && UPDATE=1

# manifest path -> 本地 path（对应 tina5.0_aiot-linux-v1.5.0.xml 的子集）
REPOS=(
  "product/tina/tina-ng/buildroot-202205|buildroot/buildroot-202205|构建基座: Buildroot 2022.05（必需）"
  "kunos/platform/config|buildroot/config|Tina 的 BR2_EXTERNAL 包定义（必需，作参考）"
  "product/tina/tina-ng/target/a733|target/a733|A733 rootfs overlay + swupdate 模板（必需）"
  "product/tina/tina-ng/target/common|target/common|公共 target（空仓库，占位）"
  "lichee/device/config/a733|device/config/chips/a733|A733 板级配置/预编译 boot0、bl31（必需）"
  "lichee/device/config/common|device/config/common|公共板级配置"
  "lichee/device/config/rootfs_tar|device/config/rootfs_tar|debian 流程的 base rootfs（参考）"
  "lichee/build|build|顶层构建脚本（参考/复用其内核参数）"
  "lichee/bsp|bsp|sun60iw2 BSP 增量树（比对参考）"
  "lichee/linux-6.6|kernel/linux-6.6|Tina 内核基树 6.6.98（比对参考）"
)

mkdir -p "$SDK"

# 构建基座：上游 Buildroot 2022.05（Tina tina-ng 用的就是这一版）。
# 说明：同版本上游 2621 个包目录 vs Tina 快照 2637，差距不在包数，而在 Tina 离了
# SDK 布局不能用（121 个厂商配方全 local blob；基树 include ../config/buildroot/*.mk
# 硬挂相对路径，见 tina-zero3w/README.md）；板级差异全部由我们的 br2-external 提供。
UBR="$SDK/buildroot/upstream-2022.05"
if [[ -d "$UBR/.git" ]]; then
  if [[ $UPDATE -eq 1 ]]; then
    echo "[update] buildroot/upstream-2022.05"
    git -C "$UBR" fetch --depth 1 origin tag 2022.05 >/dev/null 2>&1 || true
    git -C "$UBR" checkout -q 2022.05 2>/dev/null || true
  else
    echo "[skip]   buildroot/upstream-2022.05 (上游 Buildroot 2022.05，构建基座)"
  fi
else
  echo "[clone]  buildroot/upstream-2022.05 — 上游 Buildroot 2022.05（构建基座）"
  git clone --depth 1 --branch 2022.05 https://gitlab.com/buildroot.org/buildroot.git "$UBR" 2>&1 | tail -1
fi

# 我们对 Buildroot 树本身的改动（br2-external 覆盖不了的，如 package/strace 升版本）：
# 已打过的跳过，没打过的打上，两者都不是则报错停下（上游树被别的改动弄脏了）
for p in "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"/buildroot-patches/*.patch; do
  [[ -f "$p" ]] || continue
  if git -C "$UBR" apply --reverse --check "$p" 2>/dev/null; then
    echo "[ok]     buildroot-patches/$(basename "$p")（已打）"
  elif git -C "$UBR" apply --check "$p" 2>/dev/null; then
    git -C "$UBR" apply "$p"
    echo "[patch]  buildroot-patches/$(basename "$p")"
  else
    echo "错误: buildroot-patches/$(basename "$p") 打不上 $UBR（树里有别的改动？）" >&2
    exit 1
  fi
done

for entry in "${REPOS[@]}"; do
  IFS='|' read -r proj path desc <<<"$entry"
  dest="$SDK/$path"
  if [[ -d "$dest/.git" ]]; then
    if [[ $UPDATE -eq 1 ]]; then
      echo "[update] $path"
      git -C "$dest" fetch --depth 1 origin "$BRANCH" >/dev/null 2>&1 || true
      git -C "$dest" reset --hard FETCH_HEAD >/dev/null 2>&1 || true
    else
      echo "[skip]   $path ($desc)"
    fi
  else
    echo "[clone]  $path — $desc"
    git clone --depth 1 --single-branch --branch "$BRANCH" "$BASE/$proj" "$dest" 2>&1 | tail -1
  fi
done

echo
echo "SDK 子集就绪: $SDK"
du -sh "$SDK" 2>/dev/null | awk '{print "总大小: "$1}'
echo "注意: 官方 SDK 的 11 个仓库有大文件被排除在 git 外（工具链/pack 工具/媒体 blob，共 ~14 GB，在 MEGA）。"
echo "     本方案不依赖它们（见 PLAN.md §2.3/§2.2）；如需官方完整构建请另行下载 MEGA 包。"
