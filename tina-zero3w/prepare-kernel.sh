#!/usr/bin/env bash
# prepare-kernel.sh — 厂家内核提交 + userpatches 补丁系列 → 确定性 tarball，供 buildroot 使用
# （BR2_LINUX_KERNEL_CUSTOM_TARBALL_LOCATION 直接指向它）。
#
# 输入与 orangepi-build 编内核完全相同：厂家树 $BASE 提交的纯净导出，再按文件名顺序用
# patch -p1 -N 打 userpatches/kernel/sun60iw2-current/ 顶层的 *.patch（与
# scripts/compilation.sh 的 advanced_patch 同一规则；experimental/ 子目录不打）。
#
# 以前这里打包的是厂家树**工作区**，工作区随 orangepi-build 的编译状态变化，曾把
# Vulkan 诊断补丁和 PRIME 实验代码混进镜像、又漏掉新补丁（DTS 清理、0019）。现在
# 内核树只作为只读的 git 对象来源（root 属主的 .git 不写任何东西）。
#
# 输出（~/tina5/dl/）:
#   linux-6.6.98-rt58-a733.tar.gz        顶层目录 linux-6.6.98/，gzip -n
#   linux-6.6.98-rt58-a733.config        配套 .config（buildroot 引用）
#   linux-6.6.98-rt58-a733.tar.gz.hash   gzip 前 tar 流 sha256（内容寻址，build-image.sh 据此决定是否重编内核）
set -euo pipefail
umask 022

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERN_TREE="${KERN_TREE:-$REPO_ROOT/kernel/orange-pi-6.6-sun60iw2}"
BASE="${KERNEL_BASE:-2ac08e8c7}"
KCONF="$REPO_ROOT/userpatches/linux-sun60iw2-current-a733.config"
PATCH_DIR="$REPO_ROOT/userpatches/kernel/sun60iw2-current"
SDK="${TINA_SDK_DIR:-$HOME/tina5}"
DL="$SDK/dl"
WORK="$SDK/kernel-src"
SRC="$WORK/linux-6.6.98"

TARBALL="$DL/linux-6.6.98-rt58-a733.tar.gz"
CONFIG_OUT="$DL/linux-6.6.98-rt58-a733.config"
HASH_OUT="$TARBALL.hash"

mkdir -p "$DL" "$WORK"
GIT() { git -c safe.directory="$KERN_TREE" -C "$KERN_TREE" "$@"; }

GIT cat-file -e "$BASE^{commit}" 2>/dev/null || {
  echo "错误: $KERN_TREE 里没有提交 $BASE（先用 orangepi-build 拉一次内核：./build.sh a733 BUILD_OPT=kernel）" >&2
  exit 1
}
echo "基线: $(GIT log --oneline -1 "$BASE")"

# 1) 纯净导出
rm -rf "$SRC"
mkdir -p "$SRC"
GIT archive "$BASE" | tar -x -C "$SRC"

# 2) 打补丁（名字按 C locale 排序，与 advanced_patch 一致）
echo "补丁系列（$PATCH_DIR）:"
n=0
while IFS= read -r p; do
  if ! out=$(patch --batch -p1 -N -d "$SRC" < "$p" 2>&1); then
    echo "$out" >&2
    echo "错误: $(basename "$p") 打不上" >&2
    exit 1
  fi
  fuzz=$(grep -c 'with fuzz' <<<"$out" || true)
  if [[ $fuzz -gt 0 ]]; then echo "  [ok] $(basename "$p") (fuzz x$fuzz)"; else echo "  [ok] $(basename "$p")"; fi
  n=$((n+1))
done < <(find "$PATCH_DIR" -maxdepth 1 -name '*.patch' -type f | LC_ALL=C sort)
find "$SRC" \( -name '*.orig' -o -name '*.rej' \) -delete

# 3) 确定性打包：固定排序/时间/属主，内容相同则 tar 流逐字节相同
tar --sort=name --mtime='2026-01-01 00:00:00 UTC' --owner=0 --group=0 --numeric-owner \
    --format=gnu -C "$WORK" -cf "$WORK/kernel.tar" linux-6.6.98
HASH=$(sha256sum "$WORK/kernel.tar" | cut -d' ' -f1)
gzip -n < "$WORK/kernel.tar" > "$TARBALL.part"
mv "$TARBALL.part" "$TARBALL"
rm -rf "$WORK/kernel.tar" "$SRC"

# 4) config
cp "$KCONF" "$CONFIG_OUT"
echo "$HASH" > "$HASH_OUT"

# 5) 自检：关键文件（补丁新增/RT 组件）都在
TARLIST="$WORK/tarlist.txt"
tar -tzf "$TARBALL" > "$TARLIST"
for f in Makefile \
         bsp/drivers/misc/amp_timestamp.c \
         include/linux/amp_timestamp.h \
         bsp/drivers/vin/vin.c \
         bsp/drivers/vin/modules/sensor/ar0234_mipi.c \
         localversion-rt \
         kernel/printk/nbcon.c \
         include/sunxi-trng.h; do
  grep -qF "linux-6.6.98/$f" "$TARLIST" || { echo "错误: 缺 $f" >&2; exit 1; }
done
FILES_IN_TAR=$(grep -vc '/$' "$TARLIST")
rm -f "$TARLIST"
# 注意不用 grep -q：匹配即关管道会让大 tar 流吃 SIGPIPE（pipefail 下致命）
tar -xzf "$TARBALL" -O linux-6.6.98/Makefile | grep '^VERSION = 6$' >/dev/null
grep -q '^CONFIG_PREEMPT_RT=y' "$CONFIG_OUT"

echo "完成: $TARBALL ($(du -h "$TARBALL" | cut -f1))，$n 个补丁"
echo "config: $CONFIG_OUT"
echo "hash:   $HASH ($FILES_IN_TAR 个文件)"
