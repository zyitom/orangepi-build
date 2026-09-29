################################################################################
#
# zero3w-npu — A733 VIP NPU userland (vendor libs) + npurun
#
################################################################################

ZERO3W_NPU_VERSION = 1.0
ZERO3W_NPU_SITE = $(ZERO3W_NPU_PKGDIR)/files
ZERO3W_NPU_SITE_METHOD = local
ZERO3W_NPU_LICENSE = PROPRIETARY (vendor libs), GPL-2.0+ (npurun)
ZERO3W_NPU_INSTALL_STAGING = YES

ZERO3W_NPU_SRC = $(ZERO3W_REPO_ROOT)/external/cache/sources/sun60iw2_packages/npu

# npurun links only against the two vendor libs; build it here so the
# binary always matches this rootfs
define ZERO3W_NPU_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall \
		-I$(ZERO3W_NPU_SRC)/usr/include -o $(@D)/npurun $(ZERO3W_REPO_ROOT)/npu-run/npurun.c \
		-L$(ZERO3W_NPU_SRC)/usr/lib -lNBGlinker -lVIPhal
endef

define ZERO3W_NPU_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0755 $(ZERO3W_NPU_SRC)/usr/lib/libNBGlinker.so $(STAGING_DIR)/usr/lib/libNBGlinker.so
	$(INSTALL) -D -m 0755 $(ZERO3W_NPU_SRC)/usr/lib/libVIPhal.so $(STAGING_DIR)/usr/lib/libVIPhal.so
	$(INSTALL) -D -m 0644 $(ZERO3W_NPU_SRC)/usr/include/vip_lite.h $(STAGING_DIR)/usr/include/vip_lite.h
	$(INSTALL) -D -m 0644 $(ZERO3W_NPU_SRC)/usr/include/vip_lite_common.h $(STAGING_DIR)/usr/include/vip_lite_common.h
endef

define ZERO3W_NPU_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(ZERO3W_NPU_SRC)/usr/lib/libNBGlinker.so $(TARGET_DIR)/usr/lib/libNBGlinker.so
	$(INSTALL) -D -m 0755 $(ZERO3W_NPU_SRC)/usr/lib/libVIPhal.so $(TARGET_DIR)/usr/lib/libVIPhal.so
	# headers on the target too: the image ships gcc for on-board builds
	$(INSTALL) -D -m 0644 $(ZERO3W_NPU_SRC)/usr/include/vip_lite.h $(TARGET_DIR)/usr/include/vip_lite.h
	$(INSTALL) -D -m 0644 $(ZERO3W_NPU_SRC)/usr/include/vip_lite_common.h $(TARGET_DIR)/usr/include/vip_lite_common.h
	$(INSTALL) -d $(TARGET_DIR)/opt/vpm_run
	cp -a $(ZERO3W_NPU_SRC)/opt/vpm_run/. $(TARGET_DIR)/opt/vpm_run/
	$(INSTALL) -D -m 0755 $(@D)/npurun $(TARGET_DIR)/usr/bin/npurun
endef

$(eval $(generic-package))
