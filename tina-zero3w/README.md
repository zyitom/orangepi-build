# Tina/Buildroot (tina-ng) 移植到 Orange Pi Zero 3W (A733)

把全志官方 Tina Linux 的构建体系（Buildroot 2022.05，tina-ng 同版本基线）移植到
Orange Pi Zero 3W，产出**可复现、可一条命令构建**的 SD 卡系统，保留既有全部能力：
PREEMPT_RT 内核（补丁 0000-0015）、AR0234 相机驱动栈、E902 SCP（vendor-scp.bin）+
amp_timestamp、PowerVR BXM GPU（OpenCL/Vulkan）、aic8800 WiFi。

方案与调研结论见 [PLAN.md](PLAN.md)；构建/上板踩坑滚动记录见 [docs/PITFALLS.md](docs/PITFALLS.md)；
**验收完成状态见 [docs/STATUS.md](docs/STATUS.md)**（7.5/8 达标；5b Vulkan 的根因是 rootfs 缺 libxshmfence，2026-09-29 已修复待上板确认，见 [docs/VULKAN-HANDOFF.md](docs/VULKAN-HANDOFF.md)）。

## 一条命令

```sh
bash tina-zero3w/build-image.sh          # 首次约 2-3 小时；之后增量
WIFI_SSID=myap WIFI_PSK=mypass bash tina-zero3w/build-image.sh   # 带 WiFi 凭据
```

本机私有设置（WiFi 凭据、宿主机 sudo 密码）写在 `tina-zero3w/local.env`（不进 git，
模板 `local.env.example`），build/flash 脚本启动时自动读取；不设 WiFi 凭据时镜像照常
产出，只是不带 network 配置。

产物：`~/tina5/buildroot-out/images/sdcard.img`（构建输出全在仓库外的 `~/tina5`）。

烧卡（破坏性操作，脚本会先备份卡头 24 MiB 并要求输入 yes）：

```sh
bash tina-zero3w/flash-image.sh [镜像] /dev/sdX     # 设备必须显式给出
```

上电即自动进入串口登录（/dev/ttyUSB0 @ 115200，root/orangepi 或 orangepi/orangepi），
板子连上 WiFi 后（默认按 172.16.0.193 找板子，`BOARD=<IP>` 覆盖）：

```sh
bash tina-zero3w/test-board.sh      # 验收 1-8 自动采集原始输出到 test-report-*.txt
```

## 程序部署与更新（三条路径，从快到慢）

| 方式 | 命令 | 适用 |
|---|---|---|
| 推文件到在跑的系统（秒级） | `bash tina-zero3w/deploy.sh cltest.c` → 板上 gcc 重编 | 开发迭代；文件落 `/tmp/acc`（`FILE=` 指定其他路径） |
| 只重烧 rootfs 分区（~1 分钟） | `bash tina-zero3w/flash-rootfs.sh /dev/sdb` | rootfs 变了但 boot 组件没动；跳过全卡 dd |
| 整卡重烧（~1.5 分钟） | `bash tina-zero3w/flash-image.sh [镜像] /dev/sdb` | boot0/boot_package/内核也变了；同样带卡头备份与 yes 确认 |

板上编译：rootfs 自带 gcc 10.3 + OpenCL/vulkan 头文件，`deploy.sh` 推源码即可在板上
重编（验收 5a 的 cltest 就是这样跑的）。

**A/B OTA（swupdate）设计要点（未实施，后续迭代）**：genimage.cfg 拆 rootfs 为
A/B 两个分区（各 1 GiB，够装当前 rootfs 的 2 倍），boot_package 里 u-boot 环境
加 bootcount/upgrade_available 双槽标记；swupdate 生成 .swu bundle（rootfs.ext4
+ 描述符 + sha256），A/B 双分区原子切换 + 失败自动回滚（u-boot bootcount 阈值）。
sdcard.img 布局变更时 `flash-rootfs.sh` 的 49152 扇区偏移需同步。

## 文件

| 文件 | 作用 |
|---|---|
| `fetch-sdk.sh` | 拉取/更新 `~/tina5`：上游 Buildroot 2022.05（构建基座）+ Tina SDK 参考子集；给上游 Buildroot 打 `buildroot-patches/` |
| `buildroot-patches/` | 对上游 Buildroot 树本身的改动（br2-external 覆盖不了的）：strace 升 6.6 |
| `local.env.example` | 本机私有设置模板（WiFi 凭据、宿主机 sudo），复制为 `local.env` |
| `prepare-kernel.sh` | 厂家树工作区确定性快照 → `~/tina5/dl/linux-6.6.98-rt58-a733.tar.gz`（已含 0000-0015 补丁内容与 untracked 的 RT/相机组件；hash sidecar 判定输入变化，两次构建 hash 一致） |
| `build-image.sh` | 一条命令：SDK → 内核 tarball → 离线换 SCP 的 boot 组件 → buildroot → genimage |
| `flash-image.sh` | 烧卡（removable 检查、卡头备份、二次确认） |
| `flash-rootfs.sh` | 只重烧 rootfs 分区（boot 组件不动，迭代加速） |
| `deploy.sh` | 推文件到运行中的板子（开发迭代） |
| `test-board.sh` | 上板验收采集（uname/amp-timestamp/cltest/vulkaninfo/waitlat/相机） |
| `br2-external/` | 唯一的 BR2_EXTERNAL：defconfig、zero3w-gpu/isp/firmware/vulkaninfo/native-gcc 五个包、overlay、board 脚本 |

## 关键决定（与 PLAN.md §2 对照）

- **内核**：不用 Tina 的 kernel+bsp 两棵树重组，而是让 buildroot 直接编我们已验证的
  厂家树导出（**工作区确定性快照**——RT/AR0234 验证内核就是从它编的。内容寻址
  tarball）。2026-09-29 更正：补丁系列用 patch(1) 对纯净 commit 可完整重放，见
  prepare-kernel.sh 开头注释）。
  零移植风险。
- **Bootloader**：复用 orangepi-build 已验证的 boot0/boot_package（u-boot deb 里提取）；
  用 `e902/tools/bootpkg.py` 在空白头镜像上离线把 SCP 槽换成 vendor-scp.bin
  （`e902/vendor-scp/build.sh` 的产物，随仓库跟踪为 `external/packages/pack-uboot/sun60iw2/bin/scp.fex`；
  sha256 302deda8…，自动重算 add_sum）。不用 Tina 被排除在 git 外的 pack 工具。
- **Buildroot 基座 = 上游 2022.05**：同版本上游 2621 个包目录 vs Tina 快照 2637，
  Tina 不是"剪包"（早先 "~2900" 的对比是拿更新的上游误比），而是**离了 SDK 布局
  不能用**：121 个厂商包配方全部 `SITE_METHOD = local` 指向 SDK 内部 blob，基树用
  `package/allwinner/allwinner.mk` 的 `include ../config/buildroot/*.mk` 相对路径
  挂包，rootfs defconfig 由 lichee/build 现场组装。板级差异全部由本
  br2-external 提供。Tina 树（`~/tina5/buildroot/buildroot-202205` + 配方层
  `~/tina5/buildroot/config`）作布局/配方参考。
  注意 ocl-icd 是上游 2022.11 才有的包，2022.05 没有 —— OpenCL loader 由自建的
  `zero3w-ocl-icd` 包（Khronos OpenCL-ICD-Loader v2023.04.17）提供。
- **glibc 内部工具链**（2.35 ≥ 闭源 GPU 库要求的 2.30），C++ 打开（板上 gcc 包需要）。
- **板上编译器**：buildroot 2022.05 无 target gcc（上游已删 GCC_TARGET），自建
  `zero3w-native-gcc` 包：binutils+gcc 以 `--host=aarch64` 交叉编译（C only、
  --disable-bootstrap、私有 sysroot 副本防 fixincludes 污染 staging），装进 rootfs；
  cltest/waitlat 在板上现场编译（验收 5/7 的"板上编译"）。
- **闭源 blob**（GPU/ISP/cedarc deb、aic8800d80 固件）构建时从 orangepi-build 本地缓存
  抽取，**不进 git**；全志源码一律在 `~/tina5`（公开仓库约束）。
- **WiFi 芯片实为 AIC8800D80**（orangepi-firmware-git 只有该目录而现网可用）；
  固件目录 `aic8800d80/` 整体拷入。

## SD 布局（genimage.cfg）

```
8 KiB      boot0_sdcard.fex        （Zero 3W 专用 DRAM 初始化，来自 orangepi u-boot deb）
16400 KiB  boot_package.fex        （u-boot + bl31 + vendor-scp.bin，bootpkg.py 已换槽）
24 MiB     rootfs (ext4 2G)        （/boot 含 boot.scr + uImage + dtb）
```

bootargs（boot.cmd）：`console=ttyS0,115200 root=PARTUUID=<运行时解析> rootwait rw
isolcpus=5 nohz_full=5 rcu_nocbs=5`（RT 任务只跑 A55 cpu0-5，A76 cpu6-7 留给别的负载）。

## 验收对照（原始任务书：docs/ORIGINAL-BRIEF.md）

| # | 标准 | 达成路径 |
|---|---|---|
| 1 | 一条命令出镜像 + 文档 | 本 README + build-image.sh |
| 2 | 上电免干预到串口登录 | boot.scr 全自动 + systemd getty ttyS0 |
| 3 | PREEMPT_RT + 我们的补丁 | 内核 tarball = 厂家树 × 0000-0015；`uname -v` 含 PREEMPT_RT |
| 4 | aic8800 WiFi + ssh | 内核模块(m) + aic8800d80 固件 + wpa_supplicant + openssh |
| 5 | cltest mismatches=0；vulkaninfo 列出 BXM-4-64 | ocl-icd + pvr.icd → libPVROCL；deb 自带 libvulkan + img_icd.json；板上 gcc |
| 6 | SCP=vendor-scp.bin(302deda8)；amp-timestamp 24 MHz | bootpkg.py 换槽 + 补丁 0014（dtsi status=okay，zero3w 直接继承） |
| 7 | 独占 A55 hybrid < 2 µs | 同款内核/config/cmdline + waitlat（板上编译） |
| 8 | AR0234/vin + libisp/libAWIspApi + libcedarc | 补丁系列内核侧 + zero3w-isp 包 |
| 9 | 加分项：swupdate A/B / 只读 rootfs / 启动时间 | 后续阶段（PLAN §4.6） |
