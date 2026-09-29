# Tina SDK 结构速查（tina5.0_aiot / Tina AIOT Linux v1.5.0）

## 拉取方式

- GitLab 组 `tina5.0_aiot`，发布 tag `aiot-linux-v1.5.0`，分支 `product-aiot-stable`
  （唯一公开分支；kunos/target 仓库没有 tina-dev）。
- Google `repo` manifest：`manifest` 项目的 **master 分支**（注意：不是
  product-aiot-stable），文件 `tina5.0_aiot-linux-v1.5.0.xml`，98 个项目。
- 浅克隆子集脚本：`tina-zero3w/fetch-sdk.sh`（~5 GB）；完整 repo sync ~30 GB。

## 大文件（MEGA-only，git 里没有）

官方 README：11 个仓库的大文件（共 ~14 GB）被排除在 git 外，只发 MEGA 网盘，包括：

- `buildroot-202205`（部分预编译包）
- `prebuilt/kernel-built/{aarch64,arm,riscv}`（官方外部交叉工具链）
- `lichee/brandy-2.0/tools`（pack 工具，产 .img 用）
- `lichee/arisc`（SCP 源码的另一份发行版，我们用 e902/vendor-scp 自建的那套）

结论：纯 git 拉不下"能直接编"的官方 SDK；本方案（PLAN.md §2）不依赖它们。
Tina 的 buildroot 快照包数与上游同版本相当（2637 vs 2621；旧记 "~2900" 是拿更新
上游误比），真问题是 121 个厂商配方全部 `SITE_METHOD = local` 指向 SDK 内部 blob、
基树 `include ../config/buildroot/*.mk` 硬挂 SDK 目录布局 → 构建基座改用**上游
Buildroot 2022.05**（注意 ocl-icd/
opencl-headers 是上游 2022.11 才加入的，2022.05 无论哪个树都没有，OpenCL loader
由我们的 zero3w-ocl-icd 包提供 —— KhronosGroup/OpenCL-ICD-Loader）。

## 构建模型

- Buildroot 只出 rootfs：`make O=out defconfig sun60iw2p1_aiot_defconfig`；
  自定义包经 BR2_EXTERNAL（kunos `buildroot/config`，含 aic8800 固件包、GPU_UM_PUB、
  swupdate 等，WIFI_FIRMWARE_SITE 指向 platform 仓库）。
- 内核由 `lichee/build` 单独编：源 = `kernel/linux-6.6`（@44a2934864，6.6.98），
  `lichee/bsp` 以 `$KERNEL_SRC/bsp` 符号链接并入（bsp 自带 Kconfig/Makefile/drivers）；
  defconfig = `device/config/chips/a733/configs/<板>/buildroot/linux-6.6/bsp_defconfig`；
  板级 dts = `configs/<板>/linux-6.6/board.dts`（引用 bsp 的 sun60iw2p1.dtsi）。
- 工具链 = 被排除的外部工具链（MEGA），Tina 用 `BR2_TOOLCHAIN_EXTERNAL`。

## 与我们厂家树的关系

orangepi `kernel/orange-pi-6.6-sun60iw2` @2ac08e8c7 与 Tina 的 kernel+bsp 结构同源同构
（bsp/ 在树内，aic8800 驱动逐文件一致）——同一套 Allwinner 6.6 BSP 的不同 fork。
差异：orangepi 板级 dts（zero3w）+ 我们的补丁 0000-0015（RT58/AR0234+vin/
amp_timestamp/lradc+rpmsg+trng）。故选"buildroot 直接编我们的树"（PLAN §2.1）。

## A733 板级细节（移植用到的）

- PMIC：官方板 axp2202/sys_config；Zero 3W 是 AXP8191（内核 dts 用我们验证过的）。
- boot0：官方通用 boot0 与 zero3w 专用 boot0 sha 不同（DRAM 初始化不同）→ 复用
  orangepi u-boot deb 里的 `boot0_sdcard.fex`。
- GPU：`CONFIG_AW_GPU_TYPE="bxm"`；模块产物
  `bsp/modules/gpu/img-bxm/linux/rogue_km/binary_sunxi_linux_nulldrmws_release/target_aarch64/kbuild/pvrsrvkm.ko`
  （Makefile 需要 `LICHEE_PLATFORM=linux`，缺省 android）。
- WiFi：Zero 3W 板载芯片是 **AIC8800D80**（orangepi-firmware-git 只有 aic8800d80/
  目录且现网可用）；内核驱动 SDIO 路径构建 `aic8800_fdrv` + `aic8800_bsp`
  （`CONFIG_AIC8800_WLAN_SUPPORT=m`；PCIE 变体才是 aic8800p_fdrv）。
- ISP/cedarc/GPU 闭源库：glibc 编译产物，来自
  `external/cache/sources/sun60iw2_packages/bullseye/` 下的 deb
  （xserver-xorg-img-bxm / libAWIspApi / libcedarc），deb 自带 libvulkan 1.3.280。
