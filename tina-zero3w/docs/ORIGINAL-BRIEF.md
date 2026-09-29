<!-- Tina 移植开工时交给实施会话的原始任务书，原为仓库根目录 TINA-BUILDROOT-PROMPT.md；
     2026-09-29 移入此处，去掉了 WiFi 与主机 sudo 凭据，其余原样保留。 -->

# 任务：把全志官方 Tina（tina-ng / Buildroot）适配到 Orange Pi Zero 3W（A733）

## 目标
用全志官方的 Tina Linux（tina-ng，基于 Buildroot 2022.05）给 Orange Pi Zero 3W 做一套**可复现构建、能从 SD 卡直接启动的产品级 Buildroot 系统**，并保留我们在 orangepi-build 上已经做好的能力：PREEMPT_RT 内核、AR0234 相机驱动、E902 协处理器固件和 amp_timestamp、GPU（OpenCL/Vulkan）、WiFi。

## 验收标准（按优先级）
1. 用一条命令（或一个脚本）从头构建出 SD 卡镜像；构建步骤和依赖写进文档。
2. 镜像烧进 SD 卡后，上电**不需要任何手动干预**就能启动到串口登录（/dev/ttyUSB0，115200）。
3. 内核是 PREEMPT_RT（`uname -v` 里有 `PREEMPT_RT`），并且带上我们的补丁（见下文"现有资产"）。
4. WiFi（aic8800）能连上网络，能 ssh 登录。
5. GPU：`ar0234-port/tools/rt/cltest.c` 在板上编译运行，`mismatches=0`；vulkaninfo 能列出 PowerVR BXM-4-64。
6. E902：boot_package 里的 SCP 是 `e902/fw-out/vendor-scp.bin`（sha256 前缀 302deda8）；dmesg 里 amp-timestamp 驱动报告 24 MHz。
7. 实时性：在独占的 A55 核上跑 `ar0234-port/tools/rt/waitlat.c`，混合模式（hybrid）最坏 < 2 µs，结果要和 Ubuntu/Debian 镜像的数据对照（见"已有测试数据"）。
8. AR0234 驱动模块和 vin/ISP 驱动在镜像里；libisp/libAWIspApi、libcedarc 装进 rootfs（相机端到端出图作为加分项，不强制）。
9. 加分项：swupdate A/B OTA（Tina 的 A733 板级目录里已经有 sw-description 模板）、只读 rootfs、启动时间测量。

## 用户已经确认的约束
- **实时任务只能用 A55 小核（cpu0–5）**，A76（cpu6–7）要留给别的工作。实时相关的配置和测试都按独占 A55 来做（之前验证用的是 cpu5）。
- 用户的 GitHub fork `zyitom/orangepi-build` 是**公开仓库**。全志的源码（Tina SDK、arisc 等）很多没有 LICENSE，**不要把全志源码提交进这个仓库**。SDK 放在仓库外面（比如 `~/tina5`），仓库里只放我们自己写的板级配置、补丁、脚本和文档。

## 第一步：先调研，写方案，再动手
Tina 在全志公开 GitLab 上，不需要 NDA：https://gitlab.com/tina5.0_aiot ，分支 `product-aiot-stable`。可以用 GitLab API 列项目：`/api/v4/groups/tina5.0_aiot/projects?include_subgroups=true`。已经确认存在的项目：
- `product/tina/tina-ng/buildroot-202205`：全志维护的 Buildroot 2022.05
- `product/tina/tina-ng/target/a733`：A733 的 target，下面有 `buildroot/{common,demo_aiot,pro3}` 和 `debian/...`。其中 demo_aiot 带 swupdate、gstreamer 全志插件、cedarc 和 aic8800 的配置
- `lichee/device/config/a733`：A733 的板级配置（sys_config、分区、boot 相关）
- tina-ng 下还有 `package`、`prebuilt`、`target` 这几个子组
- 另外已经克隆过 `lichee/arisc`（SCP 源码），在 `e902/vendor-ref/arisc`

需要先弄清楚、写进方案的问题：
1. Tina SDK 怎么拉取（repo manifest 在哪），怎么选 A733 的板级配置，怎么编。先按官方方式把 demo_aiot 编出来，确认工具链能跑通。
2. Tina 的 A733 内核是哪个版本、哪个分支，和我们用的 orangepi 厂家树 `orange-pi-6.6-sun60iw2 @ 2ac08e8c7` 差多少。在下面两条路里选一条并说明理由：
   - (a) 让 Tina 直接编我们的内核树，加上我们的补丁和配置；
   - (b) 把我们的补丁移植到 Tina 自己的内核上。
3. Zero 3W 和官方板子（demo_aiot/pro3）的差异，包括设备树、DRAM 参数、PMIC（Zero 3W 是 AXP8191，**没有 AXP515**）、晶振（**DCXO 是 26 MHz**）、存储（SD 卡启动）、WiFi 型号、串口。bootloader 这一段（boot0、U-Boot、bl31、SCP）用 Tina 自己编的，还是复用 orangepi-build 已经验证能启动的那一套，要做出决定。
4. 闭源用户态库（ISP、cedarc、GPU、NPU）在 Tina 里是怎么打包的；Tina 的 C 库是 glibc 还是别的。我们手上的库都是针对 glibc 编的，GPU 库要求 glibc ≥ 2.30。

方案写成 `tina-zero3w/PLAN.md`（目录名自定），交给用户确认后再做大规模改动。

## 现有资产（全部在 /home/helios/Desktop/orangepi-build）
- **构建系统**：orangepi-build，用 `sudo ./build.sh a733` 出 Debian bookworm 镜像；加上 `RELEASE=noble NO_APT_CACHER=yes` 出 Ubuntu 24.04。板级配置在 `userpatches/config-a733.conf`。
- **内核**：`kernel/orange-pi-6.6-sun60iw2`（厂家树，@ 2ac08e8c7）。**不要直接改这棵树**：orangepi-build 每次构建前都会 `git checkout -f` 加 `git clean`，改动会丢。我们的改动全部以补丁形式放在 `userpatches/kernel/sun60iw2-current/`：
  - 0000：rt58
  - 0001–0013：AR0234 和 vin 修复
  - 0014：amp_timestamp
  - 0015：lradc/rpmsg/trng
  - 补丁清单和说明见该目录的 README.md
- **内核配置**：`userpatches/linux-sun60iw2-current-a733.config`，已经用 olddefconfig 规整过，包含 PREEMPT_RT、ftrace/timerlat/osnoise、lockup 检测、NO_HZ_FULL、RCU_NOCB_CPU、D3D LBC 模式、AR0234。
- **启动**：
  - 卡上布局：boot0 在 8 KiB 处，boot_package（u-boot + monitor/bl31 + scp）在 16400 KiB 处（2050×8K），rootfs 在第 1 个分区。
  - U-Boot 2018.07 通过 `/boot/boot.scr` 启动 uImage、uInitrd 和 `dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb`。
  - 启动脚本源文件：`external/config/bootscripts/boot-sun60iw2.cmd`。
  - 打包：`scripts/pack-uboot.sh`、`external/packages/pack-uboot/sun60iw2/`。
- **E902 SCP**：`e902/fw-out/vendor-scp.bin`。用 `e902/tools/flash-scp-reader.sh /dev/sdX` 写进读卡器里卡上的 boot_package；它会自动改 scp 条目长度、valid_len 和 add_sum，并先备份卡头。说明在 `e902/README.md` 和 `e902/FLASHING.md`。**只要改了 boot_package，就必须修正 add_sum**，否则下次上电会掉进 FEL 模式。
- **GPU**：
  - 内核模块在 `bsp/modules/gpu`，由 `scripts/compilation.sh` 编译。
  - 用户态库从 `external/cache/sources/sun60iw2_packages/bullseye/xserver/xserver-xorg-img-bxm_*.deb` 里抽取：`libsrv_um`、`libusc`、`libglslcompiler`、`libufwriter`、`libPVROCL`、`libVK_IMG`、`libPVRScopeServices`，外加 `usr/local/lib/libpvr_mesa_wsi.so`（**Vulkan 缺了它会在 CreateInstance 失败**）和固件 `lib/firmware/rgx.*`。
  - ICD 配置文件在 `external/packages/bsp/sun60iw2/etc/OpenCL/vendors/pvr.icd` 和 `usr/share/vulkan/icd.d/img_icd.json`。
  - 参考实现在 `external/config/sources/families/sun60iw2.conf` 的 family_tweaks_s 里。
- **ISP 和视频编解码**：`external/cache/sources/sun60iw2_packages/bullseye/{libAWIspApi,libcedarc,gst-omx-generic1.0}/*.deb`。AR0234 相关资料在 `ar0234-port/`，入口文档是 `ar0234-port/docs/HANDOFF.md`。
- **测试工具**：`ar0234-port/tools/rt/`
  - `cltest.c`：OpenCL 正确性和性能测试
  - `waitlat.c`：睡眠/混合/忙等三种等待方式的延迟测试，用法：`waitlat <cpu> sleep|hybrid|spin <秒> [guard_us]`
  - `rt-test.sh`：cyclictest 测试套件
  - `ab.sh`：stress-ng 版本 A/B 对比
  - `ser.sh`：通过串口执行命令
- **项目记忆**：`~/.claude/projects/-home-helios-Desktop-orangepi-build/memory/` 里有这个项目的历史结论，**开工前先读**：
  - `MEMORY.md`
  - `sun60iw2-image-build-pitfalls.md`
  - `e902-scp-firmware.md`
  - `a733-rt-kernel-assessment.md`
  - `ar0234-a733-bringup.md`
  - `allwinner-tina5-gitlab.md`

## 硬件和访问方式
- 主机串口：/dev/ttyUSB0 是 ARM 控制台（115200），/dev/ttyUSB1 是 E902 控制台。开始之前先用 `fuser /dev/ttyUSB*` 确认没有别的进程占着串口。
- 板子连实验室 WiFi（凭据见本机 `tina-zero3w/local.env`，不进 git），一般会拿到 172.16.0.193。ssh 用 `ar0234-port/tools/ssh_board.sh "cmd"`，加 `-s` 表示用 sudo 执行。板上的用户名和密码都是 orangepi。
- 主机 sudo 需要非交互时，把密码放进本机 `tina-zero3w/local.env` 的 `HOST_SUDO_PASS`（不进 git），脚本经 stdin 喂给 `sudo -S`。注意不要再把 stdin 重定向到 /dev/null，否则密码也会被顶掉。
- SD 读卡器通常是 /dev/sdb（可移动设备）。**卡上现在装的是用户正在用的 Ubuntu 24.04 系统。第一次烧卡前先问用户**，并先备份卡头（前 24 MiB）；烧之前要检查 `/sys/block/sdX/removable` 是 1，而且确认这张卡就是要烧的那张。

## 已经踩过的坑
- 内核 uImage 超过 32 MiB 时，U-Boot 默认地址下会被 dtb 覆盖，报 "Bad Data CRC"。现在的地址：kernel 0x41000000、fdt 0x44000000、overlay 暂存 0x44800000、ramdisk 0x45000000；bl31 在 0x48000000，这些区域都不能碰。
- 厂家内核树里把 GPU 构建目录（`rogue_km/binary_sunxi_linux_nulldrmws_release`）提交进了 git，里面的源码符号链接全部指向 `/root/orangepi/...`，编之前要先改指到本地源码。修复写在 `scripts/compilation.sh` 里。
- 主机上的 apt-cacher-ng 访问华为云 ubuntu-ports 会返回 503，Ubuntu 相关构建要绕过它。主机上 `find` 被 alias 成了 bfs，要读 /root 下的东西请用 `/usr/bin/find`，并以 root 身份执行。
- `pkill -f` 的匹配模式不要写得让它连自己所在的 shell 一起杀掉。板上的 `/tmp` 重启后会清空。
- A55 的深度 idle 状态唤醒要 105 µs 和 121 µs，实时程序必须打开 `/dev/cpu_dma_latency` 写入 0 并一直保持打开。纯忙等要把 `sched_rt_runtime_us` 设成 -1。
- 比较不同系统的延迟时要固定 stress-ng 版本：0.17 比 0.15 负载更重，同一套系统上 A55 最大延迟会从 75 µs 变成 110 µs。

## 已有测试数据（用来对照）
内核 6.6.98-rt58；负载为 stress-ng 0.15/0.17，参数 `--cpu --io --vm 2x256M --hdd --timer`；调频策略 performance：
- 不隔离：A55 最大 70–91 µs（Debian，0.15）、74–76 µs（Ubuntu，0.15），A76 最大 34–58 µs。
- 独占 cpu5（A55）：睡眠等待 35–47 µs，混合等待（提前 60 µs 醒来）0.24–0.30 µs，纯忙等 0.38–1.54 µs。
- GPU：OpenCL 3.0，saxpy 约 16.5 GFLOPS，0 处不一致；Vulkan 1.3.277。

## 工作方式
- 每做完一个阶段，都要在真板上验证：拉取和官方构建、Zero 3W 板级适配、启动、内核、WiFi、GPU、E902、RT。报告时附上串口或 ssh 的原始输出，失败就如实写失败。
- 仓库当前在 `next` 分支，有一批用户还没提交的改动（构建脚本修复、内核配置、启动脚本等）。**不要回退或覆盖这些改动**；需要改同一批文件时，先告诉用户。
- 只有用户明确要求时才 commit 或 push。
- 新的踩坑经验写进 `tina-zero3w/` 里的文档，重要结论同时写进上面的记忆目录。
