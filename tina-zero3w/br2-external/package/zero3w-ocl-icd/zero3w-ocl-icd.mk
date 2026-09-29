################################################################################
#
# zero3w-ocl-icd — Khronos OpenCL ICD loader (libOpenCL) + headers
#
################################################################################

ZERO3W_OCL_ICD_VERSION = 2023.04.17
# headers 用相邻 tag：主源码和 headers 的 github 归档 basename 都是 vX.tar.gz，
# 同版本会在 DL_DIR 里互相覆盖，必须错开
ZERO3W_OCL_ICD_HEADERS_VERSION = 2023.02.06
ZERO3W_OCL_ICD_SITE = https://github.com/KhronosGroup/OpenCL-ICD-Loader/archive/refs/tags
ZERO3W_OCL_ICD_SOURCE = v$(ZERO3W_OCL_ICD_VERSION).tar.gz
ZERO3W_OCL_ICD_EXTRA_DOWNLOADS = \
	https://github.com/KhronosGroup/OpenCL-Headers/archive/refs/tags/v$(ZERO3W_OCL_ICD_HEADERS_VERSION).tar.gz
ZERO3W_OCL_ICD_LICENSE = Apache-2.0
# headers 仓库解包后 CL/ 就在顶层，指向仓库根（CMakeLists 只检查
# <dir>/CL/cl.h 是否存在）；指向 include/ 会让它退回 find_package(OpenCLHeaders)
# 从而 configure 失败。注意注释不能放进续行赋值里（会吞掉后面的行）。
ZERO3W_OCL_ICD_CONF_OPTS = \
	-DOPENCL_ICD_LOADER_HEADERS_DIR=$(@D)/headers \
	-DBUILD_TESTING=OFF

define ZERO3W_OCL_ICD_EXTRACT_CMDS
	# 2022.05 的下载按包名分子目录（$(PKG)_DL_DIR），不是平铺 DL_DIR。
	# $(@D) 已存在（rsync 建的），mv 会把源码目录挪进它里面而不是替代它，
	# 所以用 cp -a src/. dst/。
	tar -C $(BUILD_DIR) -xf $(ZERO3W_OCL_ICD_DL_DIR)/$(ZERO3W_OCL_ICD_SOURCE)
	cp -a $(BUILD_DIR)/OpenCL-ICD-Loader-$(ZERO3W_OCL_ICD_VERSION)/. $(@D)/
	tar -C $(@D) -xf $(ZERO3W_OCL_ICD_DL_DIR)/v$(ZERO3W_OCL_ICD_HEADERS_VERSION).tar.gz
	mv $(@D)/OpenCL-Headers-$(ZERO3W_OCL_ICD_HEADERS_VERSION) $(@D)/headers
endef

# 头文件进 staging + target（板上 gcc 编 cltest 用）。
# 注意：headers 归档解包后 CL/ 在顶层（headers/CL），没有 include/ 层。
define ZERO3W_OCL_ICD_INSTALL_HEADERS
	cp -a $(@D)/headers/CL $(STAGING_DIR)/usr/include/
	cp -a $(@D)/headers/CL $(TARGET_DIR)/usr/include/
endef
ZERO3W_OCL_ICD_POST_INSTALL_TARGET_HOOKS += ZERO3W_OCL_ICD_INSTALL_HEADERS

$(eval $(cmake-package))
