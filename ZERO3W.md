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

- Vulkan：厂家 DDK 的 Vulkan ICD 在这套 RT 内核栈上拒绝设备（官方 noble 镜像同样没有可用设备）；OpenCL/GLES 正常。定论与排查记录见 `tina-zero3w/docs/VULKAN-HANDOFF.md`。
- E902 侧接收 GIC 外设中断（GINTC）：基址和路由表已找到，使能序列未找到（`e902/tests/board/GINTC-TEST.md`）。
- 休眠唤醒（mem）不可用，工业场景用关机；诊断脚本 `e902/tests/board/suspend-diagnose.sh`。
- Buildroot 内核 tarball 目前取自厂家内核树工作区（`tina-zero3w/prepare-kernel.sh`），应改为"纯净提交 + 补丁"生成。
