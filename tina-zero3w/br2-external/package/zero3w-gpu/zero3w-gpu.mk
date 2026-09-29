################################################################################
#
# zero3w-gpu — PowerVR BXM userland (vendor deb) + pvrsrvkm kernel module
#
# Source: orangepi-build/external/cache/sources/sun60iw2_packages/bullseye/
#         xserver/xserver-xorg-img-bxm_1.21.1-2_arm64.deb  (not in git)
# Kernel module: $(LINUX_DIR)/bsp/modules/gpu  (img-bxm / rogue_km, nulldrmws)
#
################################################################################

ZERO3W_GPU_VERSION = 1.0
ZERO3W_GPU_SITE = $(ZERO3W_GPU_PKGDIR)/files
ZERO3W_GPU_SITE_METHOD = local
ZERO3W_GPU_LICENSE = PROPRIETARY
# 只定义 INSTALL_STAGING_CMDS 不够——没有这个开关 buildroot 根本不执行它
ZERO3W_GPU_INSTALL_STAGING = YES
ZERO3W_GPU_DEPENDENCIES = linux

ZERO3W_GPU_DEB = $(ZERO3W_REPO_ROOT)/external/cache/sources/sun60iw2_packages/bullseye/xserver/xserver-xorg-img-bxm_1.21.1-2_arm64.deb

# rogue_km 输出目录（Makefile: GPU_TYPE=bxm, CONFIG_OS_TYPE=linux, aarch64）
ZERO3W_GPU_KO_DIR = $(LINUX_DIR)/bsp/modules/gpu/img-bxm/linux/rogue_km/binary_sunxi_linux_nulldrmws_release/target_aarch64/kbuild

# 注意：local 站点包 buildroot 会跳过 extract 步骤（只 rsync files/），
# deb 解包必须放在 CONFIGURE_CMDS 里做。
define ZERO3W_GPU_CONFIGURE_CMDS
	mkdir -p $(@D)/data
	cd $(@D)/data && ar x $(ZERO3W_GPU_DEB)
	tar -C $(@D) -xJf $(@D)/data/data.tar.xz
endef

# vulkaninfo（zero3w-vulkaninfo 包）要链接 deb 自带的 libvulkan 1.3.280
define ZERO3W_GPU_INSTALL_STAGING_CMDS
	cp -a $(@D)/usr/local/lib/libvulkan.so* $(STAGING_DIR)/usr/lib/
endef

define ZERO3W_GPU_INSTALL_TARGET_CMDS
	# PVR 后端 / OpenCL / Vulkan ICD 库 → /usr/lib（ICD 文本指向这里）
	$(INSTALL) -d $(TARGET_DIR)/usr/lib
	cp -a $(@D)/usr/lib/lib*.so* $(TARGET_DIR)/usr/lib/
	# GL 前端（mesa 式 EGL/GLES/gbm/glapi）+ dri → /usr/local/lib（厂家布局）
	$(INSTALL) -d $(TARGET_DIR)/usr/local/lib/dri
	cp -a $(@D)/usr/local/lib/lib*.so* $(TARGET_DIR)/usr/local/lib/
	cp -a $(@D)/usr/local/lib/dri/*.so $(TARGET_DIR)/usr/local/lib/dri/
	# rgx GPU 固件
	cp -a $(@D)/lib/firmware/rgx.* $(TARGET_DIR)/lib/firmware/
endef

# rogue_km 源码里的绝对 symlink（DDK 生成，曾指向构建机的内核树；本树已被
# compilation.sh repoint 成 /home/.../kernel/<树名>/...）。在 buildroot 解包位置
# 必须重指到 $(LINUX_DIR)。不能只抓断链（-xtype l）：构建机上 /home/... 链是
# "好的"，会静默漏掉、让模块编译混用外面那棵树。凡绝对路径且含 kernel/ 组件
# 的都重写：剥到 kernel/<树名>/ 为止。
define ZERO3W_GPU_FIX_SYMLINKS
	find $(LINUX_DIR)/bsp/modules/gpu -type l | while read -r l; do \
		t=$$(readlink "$$l"); \
		case "$$t" in /*kernel/*) \
			ln -sfn "$$(printf '%s' "$$t" | sed 's|^.*kernel/[^/]*/|$(LINUX_DIR)/|')" "$$l";; \
		esac; \
	done
endef

# rogue_km 按 CROSS_COMPILE 前缀找 config/compilers/<前缀>.mk；buildroot 工具链
# 前缀 aarch64-buildroot-linux-gnu 不在厂商清单里，会直接报
# "Compiler not recognised. Stop."。aarch64-linux-gnu.mk 只做一件事：声明
# TARGET_PRIMARY_ARCH=target_aarch64——对同一 gcc 直接复用即可。
define ZERO3W_GPU_ADD_COMPILER
	find $(LINUX_DIR)/bsp/modules/gpu -type d -path '*build/linux/config/compilers' | while read -r d; do \
		[ -f "$$d/aarch64-buildroot-linux-gnu.mk" ] || \
		[ -f "$$d/aarch64-linux-gnu.mk" ] || continue; \
		[ -f "$$d/aarch64-buildroot-linux-gnu.mk" ] || \
		cp "$$d/aarch64-linux-gnu.mk" "$$d/aarch64-buildroot-linux-gnu.mk"; \
	done
endef

# 复刻 compilation.sh 的调用（scripts/compilation.sh:537）：sunxi_linux/Makefile
# 用 KERNEL_CC := $(LICHEE_TOOLCHAIN_PATH)/$(LICHEE_CROSS_COMPILER)gcc 拼编译器，
# 这两个变量缺一不可，否则 CC 变成 "/gcc" 直接 not found（Error 127）。
# LICHEE_PLATFORM=linux 也必须传（缺省 android，路径会错）。
define ZERO3W_GPU_BUILD_CMDS
	$(ZERO3W_GPU_FIX_SYMLINKS)
	$(ZERO3W_GPU_ADD_COMPILER)
	$(MAKE) -C $(LINUX_DIR)/bsp/modules/gpu \
		LICHEE_KERN_DIR=$(LINUX_DIR) \
		O=$(LINUX_DIR) \
		LICHEE_PLATFORM=linux \
		LICHEE_TOOLCHAIN_PATH=$(HOST_DIR)/bin \
		LICHEE_CROSS_COMPILER=$(notdir $(TARGET_CROSS)) \
		ARCH=arm64 \
		CROSS_COMPILE=$(TARGET_CROSS) \
		GPU_BUILD_TYPE=release \
		BUILD=release
endef

define ZERO3W_GPU_INSTALL_MODULE
	$(INSTALL) -D -m 0755 $(ZERO3W_GPU_KO_DIR)/pvrsrvkm.ko \
		$(TARGET_DIR)/lib/modules/$(LINUX_VERSION_PROBED)/extra/pvrsrvkm.ko
endef
ZERO3W_GPU_POST_INSTALL_TARGET_HOOKS += ZERO3W_GPU_INSTALL_MODULE

$(eval $(generic-package))
