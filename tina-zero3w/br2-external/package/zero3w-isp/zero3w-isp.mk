################################################################################
#
# zero3w-isp — vendor camera ISP + cedarc codec userland (from debs)
#
################################################################################

ZERO3W_ISP_VERSION = 1.0
ZERO3W_ISP_SITE = $(ZERO3W_ISP_PKGDIR)/files
ZERO3W_ISP_SITE_METHOD = local
ZERO3W_ISP_LICENSE = PROPRIETARY

ZERO3W_ISP_PKG_BASE = $(ZERO3W_REPO_ROOT)/external/cache/sources/sun60iw2_packages/bullseye

# 注意：local 站点包 buildroot 会跳过 extract 步骤（只 rsync files/），
# deb 解包必须放在 CONFIGURE_CMDS 里做。
define ZERO3W_ISP_CONFIGURE_CMDS
	mkdir -p $(@D)/awi $(@D)/cedarc
	cd $(@D)/awi && ar x $(ZERO3W_ISP_PKG_BASE)/libAWIspApi/libAWIspApi_602_1.0.0_arm64.deb
	tar -C $(@D) -xJf $(@D)/awi/data.tar.xz
	mv $(@D)/usr $(@D)/awi-root 2>/dev/null || true
	cd $(@D)/cedarc && ar x $(ZERO3W_ISP_PKG_BASE)/libcedarc/libcedarc-dev_2.0.0_arm64.deb
	tar -C $(@D) -xJf $(@D)/cedarc/data.tar.xz
	mv $(@D)/usr $(@D)/cedarc-root 2>/dev/null || true
endef

define ZERO3W_ISP_INSTALL_TARGET_CMDS
	# deb 的 data.tar 根就是 usr/{lib,include,bin}，mv 成 *-root 后不再有 usr 层
	$(INSTALL) -d $(TARGET_DIR)/usr/lib
	cp -a $(@D)/awi-root/lib/aarch64-linux-gnu/*.so $(TARGET_DIR)/usr/lib/
	cp -a $(@D)/cedarc-root/lib/aarch64-linux-gnu/*.so $(TARGET_DIR)/usr/lib/
	$(INSTALL) -D -m 0644 $(@D)/etc/cedarc.conf $(TARGET_DIR)/etc/cedarc.conf
	$(INSTALL) -D -m 0755 $(@D)/awi-root/bin/AWISPdemo $(TARGET_DIR)/usr/bin/AWISPdemo
	# 头文件也给 target（板上 gcc 编 camera/codec 试验程序用）
	$(INSTALL) -d $(TARGET_DIR)/usr/include
	cp -a $(@D)/awi-root/include/*.h $(TARGET_DIR)/usr/include/
	cp -a $(@D)/cedarc-root/include/*.h $(TARGET_DIR)/usr/include/
endef

$(eval $(generic-package))
