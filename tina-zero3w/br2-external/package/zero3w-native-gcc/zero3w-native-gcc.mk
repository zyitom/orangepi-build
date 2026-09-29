################################################################################
#
# zero3w-native-gcc — target-hosted binutils + gcc (C only) for the board
#
# 复用内部工具链已下载的 gcc/binutils 源码包（host-gcc-final / host-binutils
# 保证 DL_DIR 里就绪），以 --host=aarch64-buildroot-linux-gnu 交叉编译出
# "跑在板子上的" 原生编译器。要点：
#   * --disable-bootstrap（单阶段，避免 3 倍编译时间）
#   * --with-sysroot=/ （板上运行时找 /usr/include、/usr/lib）
#   * --with-build-sysroot=私有副本（fixincludes 不得改写共享的 staging）
#   * --with-gmp/mpfr/mpc=staging（编译期链接 aarch64 版 gmp/mpfr/mpc，
#     板上 /usr/lib 同路径命中）
#   * 安装后再把 staging 的头文件/链接脚本/静态库铺进 target，使板上
#     gcc 可直接编译链接
#
################################################################################

ZERO3W_NATIVE_GCC_VERSION = 1.0
ZERO3W_NATIVE_GCC_SITE = $(ZERO3W_NATIVE_GCC_PKGDIR)/files
ZERO3W_NATIVE_GCC_SITE_METHOD = local
ZERO3W_NATIVE_GCC_LICENSE = GPL-2.0 (binutils), GPL-3.0+ with GCC runtime library exception (gcc)
ZERO3W_NATIVE_GCC_DEPENDENCIES = host-gcc-final host-binutils gmp mpfr mpc

# gcc-10.3.0.tar.xz / binutils-2.37.tar.xz（与内部工具链同源；2022.05 的
# DL_DIR 按 dl/<包名>/ 存放，不是平铺）
ZERO3W_NATIVE_GCC_GCC_DIR = $(@D)/gcc-$(GCC_VERSION)
ZERO3W_NATIVE_GCC_BNU_DIR = $(@D)/binutils-$(BINUTILS_VERSION)

# local 站点包没有 extract 步骤，源码解包放 CONFIGURE_CMDS。
define ZERO3W_NATIVE_GCC_CONFIGURE_CMDS
	tar -C $(@D) -axf $(DL_DIR)/gcc/$(GCC_SOURCE)
	tar -C $(@D) -axf $(DL_DIR)/binutils/$(BINUTILS_SOURCE)
endef

define ZERO3W_NATIVE_GCC_BUILD_CMDS
	$(ZERO3W_NATIVE_GCC_BUILD_BINUTILS)
	$(ZERO3W_NATIVE_GCC_BUILD_GCC)
endef

define ZERO3W_NATIVE_GCC_INSTALL_TARGET_CMDS
	$(ZERO3W_NATIVE_GCC_INSTALL_CMDS)
endef

define ZERO3W_NATIVE_GCC_BUILD_BINUTILS
	mkdir -p $(@D)/bnu-build && cd $(@D)/bnu-build && \
	$(TARGET_CONFIGURE_OPTS) $(ZERO3W_NATIVE_GCC_BNU_DIR)/configure \
		--build=$(GNU_HOST_NAME) --host=$(GNU_TARGET_NAME) --target=$(GNU_TARGET_NAME) \
		--prefix=/usr --exec-prefix=/usr --sysconfdir=/etc --localstatedir=/var \
		--program-prefix="" --program-suffix="" \
		--without-zlib --disable-nls --disable-werror \
		--disable-gdb --disable-sim --disable-libdecnumber --disable-readline \
		--disable-gprofng && \
	$(MAKE) -j$(PARALLEL_JOBS) && \
	$(MAKE) DESTDIR=$(TARGET_DIR) install
endef

define ZERO3W_NATIVE_GCC_BUILD_GCC
	# PATH 必须整个块内有效：`PATH=xx cmd && make` 的前缀只作用于 cmd，
	# 而生成的 gcc Makefile 里 GCC_FOR_TARGET 是不带路径的
	# aarch64-buildroot-linux-gnu-gcc（cross-native 构建，编 target 库要用
	# 外部交叉 gcc），找不到就 Error 127。
	export PATH="$(HOST_DIR)/bin:$$PATH" && \
	rm -rf $(@D)/sysroot && mkdir -p $(@D)/sysroot && \
	cp -a $(STAGING_DIR)/. $(@D)/sysroot/ && \
	mkdir -p $(@D)/gcc-build && cd $(@D)/gcc-build && \
	$(TARGET_CONFIGURE_OPTS) $(ZERO3W_NATIVE_GCC_GCC_DIR)/configure \
		--build=$(GNU_HOST_NAME) --host=$(GNU_TARGET_NAME) --target=$(GNU_TARGET_NAME) \
		--prefix=/usr --exec-prefix=/usr --bindir=/usr/bin \
		--libdir=/usr/lib --libexecdir=/usr/lib \
		--with-sysroot=/ --with-build-sysroot=$(@D)/sysroot \
		--with-gmp=$(STAGING_DIR)/usr --with-mpfr=$(STAGING_DIR)/usr --with-mpc=$(STAGING_DIR)/usr \
		--without-isl --enable-languages=c --disable-bootstrap --disable-multilib \
		--disable-nls --disable-libgomp --enable-threads=posix --enable-__cxa_atexit \
		--enable-default-pie --enable-default-ssp \
		GCC_FOR_TARGET=$(TARGET_CC) && \
	$(MAKE) -j$(PARALLEL_JOBS) GCC_FOR_TARGET=$(TARGET_CC) all-gcc && \
	$(MAKE) -j$(PARALLEL_JOBS) all-target-libgcc
endef

define ZERO3W_NATIVE_GCC_INSTALL_CMDS
	$(MAKE1) -C $(@D)/gcc-build DESTDIR=$(TARGET_DIR) install-gcc
	# libgcc 手动安装：本配置下 libgcc.mvars 没有 SHLIB_*（install-shared 里
	# SHLIB_INSTALL 为空），install-target-libgcc 会报
	# "cannot stat '/libgcc_s.so.1'"。产物本身已在构建目录里，直接铺。
	$(INSTALL) -D -m 0644 $(@D)/gcc-build/$(GNU_TARGET_NAME)/libgcc/libgcc.a \
		$(TARGET_DIR)/usr/lib/
	$(INSTALL) -D -m 0644 $(@D)/gcc-build/$(GNU_TARGET_NAME)/libgcc/libgcc_eh.a \
		$(TARGET_DIR)/usr/lib/
	$(INSTALL) -D -m 0755 $(@D)/gcc-build/$(GNU_TARGET_NAME)/libgcc/libgcc_s.so.1 \
		$(TARGET_DIR)/usr/lib/
	cp -a $(@D)/gcc-build/$(GNU_TARGET_NAME)/libgcc/libgcc_s.so $(TARGET_DIR)/usr/lib/
	ln -sf gcc $(TARGET_DIR)/usr/bin/cc
	# 把 target 变成板上编译可用的 sysroot：头文件 + .so 链接脚本 + .a/.o
	cp -a $(STAGING_DIR)/usr/include $(TARGET_DIR)/usr/
	for f in $(STAGING_DIR)/usr/lib/*.so $(STAGING_DIR)/usr/lib/*.a $(STAGING_DIR)/usr/lib/*.o; do \
		if [ -e "$$f" ]; then cp -aP "$$f" $(TARGET_DIR)/usr/lib/; fi; \
	done
	rm -rf $(@D)/sysroot
endef

$(eval $(generic-package))
