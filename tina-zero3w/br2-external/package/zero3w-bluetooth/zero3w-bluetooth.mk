################################################################################
#
# zero3w-bluetooth — AIC8800 Bluetooth UART bring-up (hciattach + address fix)
#
################################################################################

ZERO3W_BLUETOOTH_VERSION = 1.0
ZERO3W_BLUETOOTH_SITE = $(ZERO3W_BLUETOOTH_PKGDIR)/files
ZERO3W_BLUETOOTH_SITE_METHOD = local
ZERO3W_BLUETOOTH_LICENSE = PROPRIETARY (hciattach_opi), GPL-2.0+ (aic-btaddr)

ZERO3W_BLUETOOTH_HCIATTACH = $(ZERO3W_REPO_ROOT)/external/packages/blobs/bt/hciattach/hciattach_opi_arm64

# aic-btaddr.c is shared with the Debian/Ubuntu images: userpatches/overlay/zero3w/
define ZERO3W_BLUETOOTH_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -o $(@D)/aic-btaddr \
		$(ZERO3W_REPO_ROOT)/userpatches/overlay/zero3w/aic-btaddr.c
endef

define ZERO3W_BLUETOOTH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(ZERO3W_BLUETOOTH_HCIATTACH) $(TARGET_DIR)/usr/bin/hciattach_opi
	$(INSTALL) -D -m 0755 $(@D)/aic-btaddr $(TARGET_DIR)/usr/bin/aic-btaddr
endef

define ZERO3W_BLUETOOTH_INSTALL_INIT_SYSTEMD
	$(INSTALL) -D -m 0644 $(ZERO3W_BLUETOOTH_PKGDIR)/zero3w-bluetooth.service \
		$(TARGET_DIR)/usr/lib/systemd/system/zero3w-bluetooth.service
	$(INSTALL) -D -m 0644 $(ZERO3W_BLUETOOTH_PKGDIR)/zero3w-bluetooth-tmpfiles.conf \
		$(TARGET_DIR)/usr/lib/tmpfiles.d/zero3w-bluetooth.conf
	$(INSTALL) -D -m 0644 $(ZERO3W_BLUETOOTH_PKGDIR)/80-zero3w-bluetooth.preset \
		$(TARGET_DIR)/usr/lib/systemd/system-preset/80-zero3w-bluetooth.preset
endef

$(eval $(generic-package))
