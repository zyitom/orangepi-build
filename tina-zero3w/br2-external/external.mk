# orangepi-build 仓库根（br2-external 在 <repo>/tina-zero3w/br2-external）。
# 闭源 blob（GPU/ISP deb、WiFi 固件）构建时从这里抽取，不进 git。
ZERO3W_REPO_ROOT := $(abspath $(BR2_EXTERNAL_ZERO3W_PATH)/../..)

include $(sort $(wildcard $(BR2_EXTERNAL_ZERO3W_PATH)/package/*/*.mk))
