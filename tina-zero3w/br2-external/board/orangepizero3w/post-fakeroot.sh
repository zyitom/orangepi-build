#!/bin/sh
# post-fakeroot：在 fakeroot 环境里做需要 root 身份的 target 修补。
#
# 不能放在 overlay：rootfs overlay 在 target-finalize 里拷贝，而
# target-finalize 末尾的 sanity check 禁止 /etc/ld.so.conf{,.d}
# （buildroot 假设交叉构建不需要 ld.so 缓存配置），放 overlay 会直接
# 打断构建。post-fakeroot 在该检查之后、镜像生成之前运行，合法。
set -e

TARGET_DIR="${1}"
# $0 = .../br2-external/board/orangepizero3w/post-fakeroot.sh，三层 dirname 到 br2-external
BR2_EXTERNAL_DIR="$(dirname "$(dirname "$(dirname "$0")")")"

# ---- PVR 用户态搜索顺序（/usr/local/lib 优先，厂家同款）----
mkdir -p "${TARGET_DIR}/etc/ld.so.conf.d"
install -m 0644 \
	"${BR2_EXTERNAL_DIR}/board/orangepizero3w/00-pvr-priority.conf" \
	"${TARGET_DIR}/etc/ld.so.conf.d/00-pvr-priority.conf"
chown 0:0 "${TARGET_DIR}/etc/ld.so.conf.d" "${TARGET_DIR}/etc/ld.so.conf.d/00-pvr-priority.conf"

# ---- sudoers 属主（overlay/post-build 无法表达）----
if [ -f "${TARGET_DIR}/etc/sudoers.d/zero3w" ]; then
	chown 0:0 "${TARGET_DIR}/etc/sudoers.d/zero3w"
fi

# ---- 恢复 /usr/include ----
# buildroot target-finalize 无条件 `rm -rf $(TARGET_DIR)/usr/include`
# （Makefile:733），会把板上编译要用的 glibc / OpenCL / cedarc 头文件全清掉。
# post-fakeroot 在 finalize 之后运行，从 staging sysroot 拷回来。
# CONFIG_DIR 由 buildroot EXTRA_ENV 传给 post-fakeroot 脚本（= $(O)）。
SYSROOT="${CONFIG_DIR:-/home/helios/tina5/buildroot-out}/host/aarch64-buildroot-linux-gnu/sysroot"
if [ -d "${SYSROOT}/usr/include" ]; then
	rm -rf "${TARGET_DIR}/usr/include"
	cp -a "${SYSROOT}/usr/include" "${TARGET_DIR}/usr/include"
fi

# ---- 板上 gcc 的静态链接环境 ----
# target-finalize 会 `rm -rf` target 里所有 *.a（libgcc.a/libc_nonshared.a 等
# 全灭），crtbegin*.o/crtend*.o 则是 install-gcc 的 EXTRA_PARTS 在本配置下没有
# 安装；另外 glibc 的 libc.so 链接脚本用绝对路径 /usr/lib64/...（aarch64 布局），
# target 里连 lib64 符号链接都没有。三者不齐板上 gcc 必挂：
# "cannot find crtbeginS.o / -lgcc / /usr/lib64/libc_nonshared.a"。
SYSROOT_LIB="${SYSROOT}/usr/lib"
TGTGCC="${TARGET_DIR}/usr/lib/gcc/aarch64-buildroot-linux-gnu/10.3.0"
# 1) /usr/lib64 -> lib（libc.so 脚本里的绝对路径依赖它）
[ -e "${TARGET_DIR}/usr/lib64" ] || ln -s lib "${TARGET_DIR}/usr/lib64"
# 2) 全部静态库（glibc/libgcc 的 .a，vendor 闭源库不带 .a 不受影响）
cp -aP "${SYSROOT_LIB}"/*.a "${TARGET_DIR}/usr/lib/" 2>/dev/null || true
# 3) gcc 启动文件 + libgcc
GCCDIR="${CONFIG_DIR:-/home/helios/tina5/buildroot-out}/build/zero3w-native-gcc-1.0/gcc-build/aarch64-buildroot-linux-gnu"
if [ -d "${GCCDIR}/libgcc" ]; then
	mkdir -p "${TGTGCC}"
	install -m 0644 "${GCCDIR}/libgcc/libgcc.a" "${GCCDIR}/libgcc/libgcc_eh.a" "${TGTGCC}/"
	install -m 0644 "${GCCDIR}"/libgcc/crtbegin.o "${GCCDIR}/libgcc/crtbeginS.o" \
		"${GCCDIR}/libgcc/crtbeginT.o" "${GCCDIR}/libgcc/crtend.o" \
		"${GCCDIR}/libgcc/crtendS.o" "${TGTGCC}/" 2>/dev/null || true
fi

# ---- libvulkan 进默认搜索路径 ----
# /etc/ld.so.conf{,.d} 被 buildroot sanity check 禁止，glibc 不会读 conf.d，
# 而 vendor 布局把 libvulkan 放在 /usr/local/lib → vulkaninfo 报
# "libvulkan.so.1: cannot open shared object file"。用符号链接补进 /usr/lib。
for v in libvulkan.so libvulkan.so.1 libvulkan.so.1.3.280; do
	[ -e "${TARGET_DIR}/usr/local/lib/${v}" ] && \
		ln -sf "/usr/local/lib/${v}" "${TARGET_DIR}/usr/lib/${v}"
done
