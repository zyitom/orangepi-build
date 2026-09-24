# A733 底层驱动补全（driver-completion）

日期：2026-09-21 ｜ 基线：orangepi-build `bdba421` + `ar0234-port` 第 11 轮
目标：把「SoC 有、但当前内核/DT 没让用户态用上」的底层外设补齐到可用并给出验收方法。
**不碰相机栈**（VIN / ISP / AR0234 / VE / G2D / NPU 已打通，保持不动）。

## 0. 先说一个纠正：失败项里有一半是「幽灵外设」，不是驱动缺失

本轮在板上用 `i2cdetect` 做了交叉验证（对照组：能应答的 bus）：

| bus | 器件 | 结果 |
|---|---|---|
| 13 | axp515@34 / axp8191@36（PMIC） | `UU`（已绑定，正常） |
| 11 | ar0234 @0x10 | `UU`（正常） |
| 20 | HDMI DDC | 0x30（有 EDID 器件） |
| **15** | **hym8563 RTC@51 / ac101@1a** | **无器件应答** |
| **12** | **gt9271 触摸@14** | **无器件应答** |

⇒ `rtc-hym8563`、`goodix_ts` 的 probe `-22` 与声卡 `simple_dai_link_of failed`，
根因是 **DT 声明了这些器件、但 Zero 3W 板上没有装**（DT 来自参考设计）。
**这不是「驱动没写完」，写驱动也修不好。** 处理方式：把对应 DT 节点 `disabled`（去 dmesg 噪声），
不要在这上面投工时。

（证据等级：确认——总线扫描 + 对照组。）

## 1. 真正的「驱动补全」清单（SoC 有、驱动源码有、只是没启用）

| 外设 | 现状 | 缺什么 | 修法 |
|---|---|---|---|
| **Crypto Engine（CE）** + TRNG | `/proc/crypto` 里 0 个 sunxi 算法 | `bsp/drivers/ce/` 驱动在，但对应 CONFIG 未开 | `CONFIG_AW_CE_SOCKET/IOCTL`、`AW_HWRNG_DRIVER`、`AW_TRNG` |
| **GPADC**（6 通道通用 ADC） | 节点 okay，无驱动绑定 | `CONFIG_AW_GPADC` 未开 | 开 CONFIG |
| **LRADC**（按键 ADC） | 节点 okay，无驱动绑定 | `CONFIG_AW_LRADC` 未开 | 开 CONFIG |
| **SPI**（6 路控制器） | 全部 `disabled`，无 `/dev/spidev` | `CONFIG_AW_SPI` 未开 + DT 未启控制器 | 开 CONFIG + DT（spidev 子节点需 DTS 补丁） |
| **SoC 内部 RTC**（`rtc@7090000`） | DT `disabled` | `CONFIG_AW_RTC` + DT `okay` | 开 CONFIG + DT status flip |
| **USB UVC gadget** | UDC 在位、`configfs/usb_gadget` 可用 | 只缺用户态 gadget 配置（**不用重编内核**） | configfs 脚本 |

来源：`bsp/drivers/{ce,gpadc,lradc,spi,rtc}/Kconfig`（符号名），`/proc/device-tree`（DT 状态）。

## 2. 怎么用（两步）

### 第一步：内核配置（主机 TL101）
```sh
cd ~/Desktop/orangepi-build/ar0234-port/driver-completion
bash merge-config.sh          # 幂等，自动备份 config
```
然后重编内核（需 sudo，约 10–30 分钟；**改内核树前请确认可回退**）：
```sh
cd ~/Desktop/orangepi-build
sudo ./build.sh BOARD=orangepizero3w BRANCH=current BUILD_OPT=kernel REVISION=1.0.1
```
产物：`output/debs/linux-image-current-sun60iw2_*.deb` 等（其中 image db 里已含 CE/GPADC/LRADC/SPI/RTC 驱动）。

### 第二步：DT 翻转（板子上，root）
```sh
# 把 dt-enable.sh 传到板上再跑
sudo sh /tmp/dt-enable.sh
sudo reboot
```
（脚本会先备份 DTB，并打印回退命令。）

### 验收
```sh
sh verify.sh
```
期望：`/dev/rtc0` 出现、`/proc/crypto` 有 sunxi 算法、`/dev/hwrng` 出现、
gpadc/lradc 有驱动绑定、`/dev/spidev*` 出现（若做了 SPI 那步）、幽灵外设报错消失。

## 3. 风险与回退

- **配置**：`merge-config.sh` 先备份 `userpatches/linux-sun60iw2-current-a733.config`；回退 = 覆盖回 `.bak-*`。
- **DTB**：`dt-enable.sh` 先备份 `/boot/dtb/.../sun60i-a733-orangepi-zero3w.dtb`；回退 = `cp -a *.bak-* <dtb> && reboot`。
- **内部 RTC**：需要 32.768kHz 时钟，厂商默认关它可能是因为没有外部晶振/电池——**先试开，按 verify 判活**；不活就回退。
- **SPI 的 spidev 子节点**：`fdtput` 不能新增节点，需要在 DTS 层加（`spi@2540000 { spidev@0 { compatible="rohm,dh2228fv"; reg=<0>; spi-max-frequency=...; }; }`），
  归入 `userpatches/kernel/sun60iw2-current/` 的新补丁；本轮先不做，避免动内核树。
- 全程**不 rmmod vin/ar0234、不碰 isp01、不改 bootargs**。

## 4. 本轮未做（边界）
- 未执行内核重编与刷机（属有风险操作，需要一次专门、有人看着板子的构建-上板循环）。
- 未新增 DTS 补丁（SPI spidev 子节点）。
- 未做 USB UVC gadget（需要另一个 USB 主机在设备口对端；等你确认接线后可做）。
