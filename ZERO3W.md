# Orange Pi Zero 3W (A733) —— 本 fork 的工作索引

这是 [orangepi-xunlong/orangepi-build](https://github.com/orangepi-xunlong/orangepi-build)
的 fork，在官方构建系统之上放了 Zero 3W（全志 A733）的全部工作：PREEMPT_RT 内核、
AR0234 工业相机、E902 小核（SCP）固件、Buildroot 镜像，以及 NPU/USB 小工具。

分支：`next` 只跟官方同步；`zero3w`（默认分支）= `next` + 这里的全部工作。
`git diff next..zero3w` 就是对官方的全部改动。

## 从哪里入手

| 想做什么 | 入口 |
|---|---|
| 编 Debian/Ubuntu 镜像（RT 内核 + AR0234 + E902 SCP + GPU/NPU 用户态） | `sudo ./build.sh a733`，配置在 `userpatches/config-a733.conf` |
| 看/改内核（RT、相机驱动、amp_timestamp、DTS 清理） | [`userpatches/kernel/sun60iw2-current/README.md`](userpatches/kernel/sun60iw2-current/README.md)——内核改动只在这里 |
| 编 Buildroot 镜像（Tina 同版本基线，一条命令出 SD 卡） | [`tina-zero3w/README.md`](tina-zero3w/README.md) |
| E902 小核、SCP 固件、烧录与回滚 | [`e902/README.md`](e902/README.md)、[`e902/FLASHING.md`](e902/FLASHING.md) |
| AR0234 相机（ISP、编码、用户态、调试记录） | [`ar0234-port/README.md`](ar0234-port/README.md)、[`ar0234-port/docs/HANDOFF.md`](ar0234-port/docs/HANDOFF.md) |
| 实时性评估（rt58 补丁、驱动风险扫描、运行时调优） | [`rt-check/rt-runtime-guide.md`](rt-check/rt-runtime-guide.md)、[`rt-check/rt-driver-audit.md`](rt-check/rt-driver-audit.md) |
| NPU 推理 | [`npu-run/README.md`](npu-run/README.md) |
| 板子当 USB 设备走 BULK 传数据 | [`usb-bulk/README.md`](usb-bulk/README.md) |

## 对官方构建系统的改动

| 文件 | 改了什么 |
|---|---|
| `userpatches/config-a733.conf`、`linux-sun60iw2-current-a733.config`、`customize-image.sh` | 板级构建配置、RT 调优内核配置（ftrace/timerlat 等）、镜像定制（rt-tests、rtla 等） |
| `userpatches/overlay/rtla/rtla` | 从内核 6.6 tools 编的 rtla（Ubuntu 没有独立包），customize-image.sh 拷进镜像 |
| `external/config/bootscripts/boot-sun60iw2.cmd` | 加载地址：内核超过 32 MiB 时 dtb 会盖掉它（"Bad Data CRC"） |
| `external/config/sources/families/sun60iw2.conf` | 厂家包路径缺 `bullseye/` 回退（镜像里没有 libisp/cedarc）、pvrsrvkm 装进每个内核、GPU/GL 用户态进所有镜像、noble 上 dnsmasq 冲突、NPU demo 的 OpenCV soname |
| `external/config/boards/orangepizero3w.conf` | 允许 noble |
| `scripts/compilation.sh` | 厂家内核树里 GPU 构建目录的符号链接指向官方构建机路径，编 pvrsrvkm 前改指本树 |
| `external/packages/pack-uboot/sun60iw2/bin/scp.fex` | 换成从源码编的厂家 SCP（`e902/vendor-scp/build.sh`，sha256 302deda8…） |

## 不在仓库里的东西（体积大、可再生成，或不能再分发）

| 东西 | 怎么得到 |
|---|---|
| 全志 Tina SDK 子集 + Buildroot 输出（`~/tina5`） | `bash tina-zero3w/fetch-sdk.sh`，然后 `build-image.sh` |
| 全志 arisc/dramlib 源码（无 LICENSE） | `bash e902/vendor-scp/build.sh` 自动拉取到 `e902/vendor-ref/` |
| E902 工具链、玄铁 RTL、各类手册 PDF | 见 `e902/README.md`「本地目录」 |
| 厂家内核树、官方工具链、镜像 | `./build.sh` 自动下载/产出（`kernel/`、`toolchains/`、`output/`） |
| 本机私有设置（WiFi 凭据、宿主机 sudo 密码） | `tina-zero3w/local.env`，模板 `local.env.example` |

## 未完成 / 已知限制

- Vulkan：compute 可用（noble 镜像已在板上跑通）；Buildroot 镜像之前的 -9 是 rootfs 缺 libxshmfence，2026-09-29 修复并上板通过（枚举 + compute）。上屏只能经支持 DRI3 的 X11（驱动没有 VK_KHR_display）。见 `tina-zero3w/docs/VULKAN-HANDOFF.md`。
- E902 侧接收 GIC 外设中断（GINTC）：基址和路由表已找到（09-29 更正过基址），使能序列未找到（`e902/tests/board/GINTC-TEST.md`）。
- 休眠：s2idle + RTC 闹钟唤醒已在 Tina 镜像上验证可用；deep 休眠 2026-09-30 定位到根因——SCP 读到的 FDT 里 `/dram` 无参数（全零），DRAM 库带空参数做 save 后整机假死。固件侧已加"参数全零则用内置实测参数表"兜底（`e902/vendor-scp/patches/0004`）+ 调试探针（`patches-debug/0002`），内核 DTS 补上 `standby_param` 节点和 `dram_para00..31`（`userpatches/kernel/sun60iw2-current/0030`），**待上板验证**。
- Buildroot 内核 tarball 取自"厂家提交 + userpatches 补丁"（`tina-zero3w/prepare-kernel.sh`，09-29 起为确定性导出；当日发现 Tina 内核一直停在 9 月 25 日旧源码，缓存的旧 tarball 已删除重建）。

## 2026-09-29/30 整机检查（Tina 镜像 64/65，提交 978b108）

全项检查脚本 `tina-zero3w/tests/hw-check.sh` 18/18 通过：Vulkan/OpenCL/实时延迟正常、RTC 掉电后时间正确、s2idle 可被 RTC 闹钟唤醒。内核补丁 0024–0029：USB0 device 口、otg_role sysfs 告警、aic8800 休眠唤醒等待、GPADC 按需采样（修中断风暴）、G2D 自动加载、SoC RTC 启用。A55 固定 performance 档（DVFS I²C 风暴随之消失）；WiFi 关省电（reason=4 踢线下长时空闲观察未做）；蓝牙经 hciattach_opi + aic-btaddr 可用且每板地址唯一；Tina 补 NPU 库与分区扩容。
