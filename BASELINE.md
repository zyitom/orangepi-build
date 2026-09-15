# 基线与复现

全部功能实测通过时（2026-09-15）的环境版本。**外部模块编译对内核版本严格敏感**，
升级内核必须重新验证。

| 组件 | 版本 |
|---|---|
| orangepi-build | `bdba421`（branch `next`） |
| 内核 | github.com/orangepi-xunlong/linux-orangepi，branch `orange-pi-6.6-sun60iw2`，commit `8a9be72`（6.6.98） |
| 交叉工具链 | gcc-arm-11.2-2022.02（aarch64-none-linux-gnu），`../toolchains/` |
| 用户态编译 | 板上 g++ 10.2（Debian），C++20，无 CMake |
| 硬件 | Orange Pi Zero 3W（A733/sun60iw2）+ AR0234 彩色模组（chip id 0x0A56），MIPI-A 4-lane，24MHz MCLK |

## 复现步骤

### 1. 传感器驱动（外部模块，不改内核树，约 9 秒）

```bash
K=/home/helios/Desktop/orangepi-build/kernel/orange-pi-6.6-sun60iw2
TC=/home/helios/Desktop/orangepi-build/toolchains/gcc-arm-11.2-2022.02-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-
make -C $K M=$PWD ARCH=arm64 CROSS_COMPILE=$TC modules    # M=本仓库根目录
```

### 2. vin 模块（含 0003 补丁 + D3D LBC，ISP 硬件 3DNR）

`build/vin-d3d-lbc/` 是 vin 源码副本 + 0003 补丁 + `board_compat.h`（定义
`CONFIG_D3D`/`CONFIG_D3D_LBC_MODE`，undef `CONFIG_AW_DMC_DEVFREQ`）：

```bash
make -C $K M=/path/to/ar0234-port/build/vin-d3d-lbc \
    ARCH=arm64 CROSS_COMPILE=$TC CONFIG_CSI_VIN=m CONFIG_AW_VIDEO_SUNXI_VIN=m -j6 modules
```

产物 strip 后即 `prebuilt/ar0234_mipi.ko`、`prebuilt/vin_v4l2.ko`。
`vin_io.ko` 必须用系统原装的，重编的出不了图。

### 3. 内核树补丁（可选，需要 root，改内核树前先征得同意）

```bash
sudo bash apply.sh    # 打入 0001（驱动+DTS）/ 0002（配置）
```

### 4. 板上部署（当前板上已是此状态）

模块与 dtb 的安装方式、`/etc/modules-load.d/ar0234.conf`、`ar0234-3ad.service`、
备份文件清单见 README「板上使用 / 测试」。正式打包见 `ARCHITECTURE.md` 的 deb 路线。
