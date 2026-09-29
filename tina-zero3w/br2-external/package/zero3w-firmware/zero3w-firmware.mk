################################################################################
#
# zero3w-firmware — aic8800d80 WiFi firmware from the orangepi firmware cache
#
################################################################################

ZERO3W_FIRMWARE_VERSION = 1.0
ZERO3W_FIRMWARE_SITE = $(ZERO3W_FIRMWARE_PKGDIR)/files
ZERO3W_FIRMWARE_SITE_METHOD = local
ZERO3W_FIRMWARE_LICENSE = PROPRIETARY

ZERO3W_FIRMWARE_SRC = $(ZERO3W_REPO_ROOT)/external/cache/sources/orangepi-firmware-git/aic8800d80

define ZERO3W_FIRMWARE_INSTALL_TARGET_CMDS
	$(INSTALL) -d $(TARGET_DIR)/lib/firmware
	cp -a $(ZERO3W_FIRMWARE_SRC) $(TARGET_DIR)/lib/firmware/
endef

$(eval $(generic-package))
