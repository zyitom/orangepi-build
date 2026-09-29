################################################################################
#
# zero3w-vulkaninfo — vulkaninfo linked against the vendor's libvulkan
#
################################################################################

ZERO3W_VULKANINFO_VERSION = 1.3.280
ZERO3W_VULKANINFO_SITE = https://github.com/KhronosGroup/Vulkan-Tools/archive/refs/tags
ZERO3W_VULKANINFO_SOURCE = vulkan-sdk-$(ZERO3W_VULKANINFO_VERSION).0.tar.gz
# 与 loader 同版本的头文件
ZERO3W_VULKANINFO_EXTRA_DOWNLOADS = \
	https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v$(ZERO3W_VULKANINFO_VERSION).tar.gz \
	https://raw.githubusercontent.com/zeux/volk/vulkan-sdk-$(ZERO3W_VULKANINFO_VERSION).0/volk.h \
	https://raw.githubusercontent.com/zeux/volk/vulkan-sdk-$(ZERO3W_VULKANINFO_VERSION).0/volk.c
ZERO3W_VULKANINFO_LICENSE = Apache-2.0
ZERO3W_VULKANINFO_LICENSE_FILES = LICENSE.txt
ZERO3W_VULKANINFO_DEPENDENCIES = zero3w-gpu

define ZERO3W_VULKANINFO_EXTRACT_CMDS
	# 2022.05 的下载按包名分子目录；$(@D) 已存在，mv 会挪进它而不是替代，
	# 用 cp -a src/. dst/。
	tar -C $(BUILD_DIR) -xf $(ZERO3W_VULKANINFO_DL_DIR)/$(ZERO3W_VULKANINFO_SOURCE)
	cp -a $(BUILD_DIR)/Vulkan-Tools-vulkan-sdk-$(ZERO3W_VULKANINFO_VERSION).0/. $(@D)/
	tar -C $(@D) -xf $(ZERO3W_VULKANINFO_DL_DIR)/v$(ZERO3W_VULKANINFO_VERSION).tar.gz
	mv $(@D)/Vulkan-Headers-$(ZERO3W_VULKANINFO_VERSION) $(@D)/headers
	# vulkaninfo.h 以 VOLK_IMPLEMENTATION 包 volk.h，后者会 #include "volk.c"，
	# 两个文件都要进编译目录
	cp $(ZERO3W_VULKANINFO_DL_DIR)/volk.h $(ZERO3W_VULKANINFO_DL_DIR)/volk.c $(@D)/vulkaninfo/
endef

# vulkaninfo 是单翻译单元（sdk 分支已是 vulkaninfo.cpp），直接用 buildroot
# 交叉工具链编，链接 deb 自带的 libvulkan.so.1.3.280（zero3w-gpu 已装进 staging）
define ZERO3W_VULKANINFO_BUILD_CMDS
	# deb 的 libvulkan 在 target 的 /usr/local/lib（staging 副本在多次重建中
	# 会被 sysroot 清理吃掉，直接 -L target 目录最稳）
	$(TARGET_CXX) $(TARGET_CXXFLAGS) $(TARGET_LDFLAGS) -std=c++17 -DVK_NO_PROTOTYPES \
		-L$(TARGET_DIR)/usr/local/lib \
		-I$(@D)/headers/include -I$(@D)/vulkaninfo -I$(@D)/vulkaninfo/generated \
		$(@D)/vulkaninfo/vulkaninfo.cpp -lvulkan -lm -lpthread -ldl \
		-o $(@D)/vulkaninfo-bin
endef

define ZERO3W_VULKANINFO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/vulkaninfo-bin $(TARGET_DIR)/usr/bin/vulkaninfo
endef

$(eval $(generic-package))
