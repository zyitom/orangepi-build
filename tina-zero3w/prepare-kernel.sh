#!/usr/bin/env bash
# prepare-kernel.sh — 把 orangepi-build 的厂家内核树工作区内容打包成确定性 tarball
# 供 buildroot 使用（BR2_LINUX_KERNEL_CUSTOM_TARBALL_LOCATION 直接指向它）。
#
# 为什么是工作区：RT/AR0234 验证内核是从厂家树**工作区**构建的（工作区 = commit
# 2ac08e8c7 + userpatches 补丁就地应用）。更正（2026-09-29 核对）：补丁系列用
# patch(1) 对纯净 commit 可以完整重放（orangepi-build 同样用 patch，接受 fuzz/偏移；
# 先前"不可重放"是 git apply 不接受 fuzz 的误判）。重放结果与工作区只差
# userpatches/kernel/sun60iw2-current/experimental/ 的 Vulkan 诊断改动和 0019——
# 也就是说当前 tarball 会带上这些实验改动。后续可改为"纯净 commit + 补丁"生成。
#
# 导出方式（对 root 属主的厂家仓库完全只读）：
#   GIT_INDEX_FILE/GIT_OBJECT_DIRECTORY 指到 ~/tina5 下的临时文件，
#   `git add -u` 只暂存**被跟踪文件**的工作区内容（未跟踪的 debian/tmp、*.orig、
#   *.o 等构建垃圾天然排除），write-tree → 固定日期 commit-tree → git archive。
#   symlink/权限/mtime 全确定性；--prefix 不改 symlink target。
#
# 输出（~/tina5/dl/）:
#   linux-6.6.98-rt58-a733.tar.gz        顶层目录 linux-6.6.98/，gzip -n
#   linux-6.6.98-rt58-a733.config        配套 .config（buildroot 引用）
#   linux-6.6.98-rt58-a733.tar.gz.hash   gzip 前 tar 流 sha256（内容寻址）
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERN_TREE="${KERN_TREE:-$REPO_ROOT/kernel/orange-pi-6.6-sun60iw2}"
KCONF="$REPO_ROOT/userpatches/linux-sun60iw2-current-a733.config"
PATCH_DIR="$REPO_ROOT/userpatches/kernel/sun60iw2-current"
SDK="${TINA_SDK_DIR:-$HOME/tina5}"
DL="$SDK/dl"
WORK="$SDK/kernel-src"

mkdir -p "$DL" "$WORK"

GIT() { git -c safe.directory="$KERN_TREE" -C "$KERN_TREE" "$@"; }
echo "厂家树: $KERN_TREE ($(GIT log --oneline -1))"

# 1) 信息：工作区里已删除的被跟踪文件数（git add -u 会自动从 index 去掉）
DELETED=$(GIT diff --name-only --diff-filter=D HEAD | wc -l)
[[ $DELETED -gt 0 ]] && echo "工作区已删除 $DELETED 个被跟踪文件（不含入 tarball）"
FILES_TRACKED=$(GIT ls-files | wc -l)

# 2) 补丁系列来源校验（反向 apply --check：补丁内容应已在工作区中；只读）
echo "补丁系列来源校验（userpatches/kernel/sun60iw2-current）:"
for p in $(ls "$PATCH_DIR"/[0-9]*.patch | sort); do
  if GIT apply --check --reverse "$p" 2>/dev/null; then
    echo "  [ok] $(basename "$p")"
  else
    echo "  [warn] $(basename "$p") 不能反向应用（工作区在其上有后续微调）"
  fi
done

# 3) 打包：临时 index + 私有 object 目录（不写厂家 .git）
TARBALL="$DL/linux-6.6.98-rt58-a733.tar.gz"
CONFIG_OUT="$DL/linux-6.6.98-rt58-a733.config"
HASH_OUT="$TARBALL.hash"

export GIT_INDEX_FILE="$WORK/tmp-index"
export GIT_OBJECT_DIRECTORY="$WORK/tmp-objects"
export GIT_ALTERNATE_OBJECT_DIRECTORIES="$KERN_TREE/.git/objects"
rm -rf "$WORK/tmp-objects"      # 残留的旧 object 目录会让 add -u 误判对象已存在
mkdir -p "$WORK/tmp-objects"
rm -f "$WORK/tmp-index"

snapshot_once() {
        GIT read-tree HEAD
        GIT add -u              # 被跟踪文件：暂存工作区内容
        # 未跟踪但属于验证树的新文件（补丁系列新增、RT 组件等——厂家仓库 root 属主，
        # 当年无法 git add）：收进 tarball；.gitignore 已过滤构建垃圾，再挡一层明显垃圾。
        UNTRACKED=0
        while IFS= read -r f; do
                [[ -f "$KERN_TREE/$f" ]] || continue
                GIT add --force -- "$f"
                UNTRACKED=$((UNTRACKED+1))
        done < <(GIT ls-files --others --exclude-standard \
                 | grep -vE '\.(orig|rej)$|(^|/)\.git|(^|/)debian/' || true)
        [[ $UNTRACKED -gt 0 ]] && echo "收进 $UNTRACKED 个未跟踪源文件（补丁新增/RT 组件等）" >&2
        echo "$UNTRACKED" > "$WORK/.untracked"  # 子 shell 里跑，用文件带回给父进程
        GIT write-tree
}

# add -u/write-tree 偶发丢对象（2026-09-25 build #3：fault.c blob 缺失，重跑即好）。
# write-tree 失败时清掉临时 index/object 目录整体重试一次；hash 自检兜底正确性。
if ! TREE=$(snapshot_once); then
        echo "[warn] write-tree 失败（index 引用了缺失对象），清理后重试一次"
        rm -rf "$WORK/tmp-objects" "$WORK/tmp-index"
        mkdir -p "$WORK/tmp-objects"
        TREE=$(snapshot_once)
fi
UNTRACKED=$(cat "$WORK/.untracked")
rm -f "$WORK/.untracked"
GITLINKS=$(GIT ls-files -s | awk '$1=="160000"' | wc -l)

# 固定作者/日期 → commit 对象 id 确定 → archive 头部 mtime 确定
export GIT_AUTHOR_NAME=zero3w GIT_AUTHOR_EMAIL=zero3w@localhost
export GIT_COMMITTER_NAME=zero3w GIT_COMMITTER_EMAIL=zero3w@localhost
export GIT_AUTHOR_DATE="2026-01-01T00:00:00+00:00" GIT_COMMITTER_DATE="2026-01-01T00:00:00+00:00"
COMMIT=$(GIT commit-tree "$TREE" -m "zero3w kernel export (worktree snapshot)")

git archive --format=tar --prefix=linux-6.6.98/ "$COMMIT" > "$WORK/kernel.tar"
unset GIT_INDEX_FILE GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES

HASH=$(sha256sum "$WORK/kernel.tar" | cut -d' ' -f1)
gzip -n < "$WORK/kernel.tar" > "$TARBALL.part"
mv "$TARBALL.part" "$TARBALL"
rm -f "$WORK/kernel.tar" "$WORK/tmp-index"
rm -rf "$WORK/tmp-objects"

# 4) config
cp "$KCONF" "$CONFIG_OUT"
echo "$HASH" > "$HASH_OUT"

# 5) 自检：文件数一致、关键文件（补丁新增/RT 组件）都在
TARLIST="$WORK/tarlist.txt"
tar -tzf "$TARBALL" > "$TARLIST"
FILES_IN_TAR=$(grep -vc '/$' "$TARLIST")
FILES_EXPECT=$((FILES_TRACKED - GITLINKS + UNTRACKED))
if [[ $FILES_IN_TAR -ne $FILES_EXPECT ]]; then
  echo "错误: tar 条目数 $FILES_IN_TAR != 预期 $FILES_EXPECT（tracked $FILES_TRACKED - gitlinks $GITLINKS + untracked $UNTRACKED）" >&2
  exit 1
fi
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
rm -f "$TARLIST"
# 注意不用 grep -q：匹配即关管道会让 307MB 的 tar 流吃 SIGPIPE（pipefail 下致命）
tar -xzf "$TARBALL" -O linux-6.6.98/Makefile | grep '^VERSION = 6$' >/dev/null
grep -q '^CONFIG_PREEMPT_RT=y' "$CONFIG_OUT"

echo "完成: $TARBALL ($(du -h "$TARBALL" | cut -f1))"
echo "config: $CONFIG_OUT"
echo "hash:   $HASH ($FILES_IN_TAR 个文件)"
