# Tina (tina-ng / Buildroot) 适配 Orange Pi Zero 3W —— 方案

状态：实施中。日期：2026-09-25。
对应任务书：`docs/ORIGINAL-BRIEF.md`（原仓库根目录 TINA-BUILDROOT-PROMPT.md）（验收标准 1–9 逐条对照见 §8）。

实施修订（相对下文调研稿）：(1) 构建基座用**上游 Buildroot 2022.05**——Tina 的
buildroot-202205 与上游同版本包数相当（2637 vs 2621；早先记的 "~2900" 是拿更新的
上游误比），真正的问题是它**不是独立可用的树**：121 个厂商包配方全部
`SITE_METHOD = local` 指向 SDK 内部 blob，基树靠 `package/allwinner/allwinner.mk`
里 `include ../config/buildroot/*.mk` 相对路径硬挂 SDK 目录布局，rootfs defconfig
也不在树里（lichee/build 现场组装）；缺 ocl-icd、system/mkusers 属实但属小缺口。
板级差异全由我们的 br2-external 提供，Tina 树留作配方/布局参考；(2) 内核 tarball 文件名
固定为 `linux-6.6.98-rt58-a733.tar.gz`（defconfig 引用路径不能带 hash），可复现性由
`.hash` sidecar 保证；(3) 增补 zero3w-vulkaninfo / zero3w-firmware 两个包
（deb 自带 libvulkan 1.3.280，vulkaninfo 用 Vulkan-Tools vulkan-sdk-1.3.280.0 交叉编译；
板载 WiFi 芯片实为 AIC8800D80，固件取自 orangepi-firmware-git/aic8800d80）；
(4) **内核 tarball = 厂家树工作区的确定性快照**，不是"commit × 补丁重放"——实测补丁
对纯净 2ac08e8c7 重放在 vin 子系统 3 处上下文漂移失败，且工作区里还有 8 个因 root
属主从未进过 git index 的合法文件（补丁新增的 amp_timestamp/ar0234_mipi、RT 的
nbcon/localversion-rt/trng、GPU 模块 2 个源文件）；快照经临时 index + 私有 object
目录完成（厂家 .git 只读），连跑两次 hash 一致（1a78e1b2…，95819 文件）。

---

## 1. 调研结论

### 1.1 Tina SDK 的组织方式

- 全志公开 GitLab 组 `tina5.0_aiot`，发布版本 **Tina AIOT Linux v1.5.0**（tag `aiot-linux-v1.5.0`，
  分支 `product-aiot-stable`）。用 Google `repo` 拉取，manifest 在 `manifest` 项目的
  **`master` 分支**（不是 product-aiot-stable），文件 `tina5.0_aiot-linux-v1.5.0.xml`，共 98 个项目。
- SDK 顶层布局（manifest 里的 path）：`brandy/`（bootloader：spl-pub、u-boot-2018、dramlib、arisc）、
  `kernel/linux-6.6`、`bsp/`（SoC BSP 增量树）、`buildroot/buildroot-202205`、
  `buildroot/config/`（Tina 的 BR2_EXTERNAL 包定义）、`target/a733`、`device/config/chips/a733`、
  `build/`（顶层构建脚本）、`platform/`（闭源用户态包：固件、多媒体库等）。
- **重要限制**：官方 README 说明 11 个仓库的大文件（共约 14 GB）被排除在 git 之外，只发布在 MEGA
  网盘，其中包括 `buildroot-202205`、`prebuilt/kernel-built/aarch64`（交叉工具链）、
  `lichee/brandy-2.0/tools`（pack 工具）、`lichee/arisc` 等。也就是说：**纯 git 拉不下能直接编的
  官方 SDK**，官方 demo_aiot（带多媒体/GPU 包）必须补 MEGA 大文件包才能编。

### 1.2 Tina 的 A733 构建模型

- Buildroot（2022.05）只负责 **rootfs**：`make O=out defconfig sun60iw2p1_aiot_defconfig && make`。
  自定义包通过 BR2_EXTERNAL（kunos 的 `buildroot/config`）注入；板级差异通过
  `LICHEE_IC` / `LICHEE_BOARD` 变量展开到 `BR2_ROOTFS_OVERLAY`。
- 内核由 `lichee/build` 单独编：源 = `kernel/linux-6.6`，把 `lichee/bsp` 以**符号链接
  `$KERNEL_SRC/bsp`** 的方式并进去（bsp 自带 Kconfig/Makefile/drivers/configs），
  defconfig 是 `LICHEE_KERN_DEFCONF=bsp_defconfig`（每个板级一份，位于
  `device/config/chips/a733/configs/<板>/buildroot/linux-6.6/`），板级 dts 是
  `configs/<板>/linux-6.6/board.dts`（引用 bsp 里的 `sun60iw2p1.dtsi`）。
- Tina 的 A733 内核：**linux-6.6 @ 44a2934864（6.6.98，"aiot-linux-v1.5.0 release"，2026-07-15）**，
  基础树是接近主线的 6.6.98，sun60iw2 的 dtsi/driver 全在 `lichee/bsp` 里。
- 工具链：`BR2_TOOLCHAIN_EXTERNAL`，装在 `out/<chip>/external-toolchain/gcc-arm`
  （来源就是被排除的 `prebuilt/kernel-built/aarch64` MEGA 包）。

### 1.3 我们厂家树与 Tina 的关系

- orangepi 厂家树 `kernel/orange-pi-6.6-sun60iw2`（@ 2ac08e8c7，6.6.98）与 Tina 的
  kernel+bsp 结构**同源同构**：树里就有 `bsp/`（含 modules/gpu、configs/linux-6.6 等），
  aic8800 驱动与 Tina bsp 的**逐文件一致**（diff 为空）。同一套 Allwinner 6.6 BSP 的不同 fork。
- 差异在 orangepi 的板级 dts（`sun60i-a733-orangepi-zero3w.dts`）和我们的补丁系列
  0000–0015（RT58、AR0234/vin、amp_timestamp、lradc/rpmsg/trng）。
- 我们手上的 16 个补丁在 2ac08e8c7 上已验证零失败零 fuzz 套用并编出 RT 内核（见记忆
  a733-rt-kernel-assessment）。

### 1.4 Zero 3W 与官方 A733 板的差异（问题 3 的回答）

| 项 | 官方 pro3/demo_aiot | Zero 3W | 结论 |
|---|---|---|---|
| PMIC | sys_config 用 axp2202 | **AXP8191**（无 AXP515） | 内核 dts 用我们验证过的 zero3w.dts |
| DRAM | pro3: LPDDR4 2400，boot0 通用 | 专用 boot0 变体 | **Tina 的通用 boot0 ≠ orangepi 的 zero3w boot0**（sha256 不同：4e10d669… vs 829ea8e3…），DRAM 初始化以 orangepi 的为准 |
| DCXO | A733 标准 26 MHz | 26 MHz（Y1） | SCP 用我们修正过时钟源的 vendor-scp.bin |
| WiFi | aic8800d80（pro3 overlay 带固件） | aic8800 SDIO | 驱动两树同源；固件用板上已验证的一套 |
| 启动介质 | eMMC 为主，也支持 SD | **SD 卡** | 沿用已验证的 SD 布局 |
| 串口 | 板级不同 | ttyS0 115200 | bootargs 用我们的 |

### 1.5 闭源用户态库与 C 库（问题 4 的回答）

- Tina 里闭源库走两条路：MEGA 补回的包内 blob（libcedarc `library/<prefix>/*.so` 等）或
  `platform/` 下的仓库（如 wifi firmware，在 git 里）。demo_aiot 的多媒体/GPU blob 大多缺失。
- 我们不依赖 Tina 的 blob：**ISP（libisp/libAWIspApi）、cedarc、GPU 用户态（libsrv_um、
  libVK_IMG、libPVROCL、libpvr_mesa_wsi.so、rgx.* 固件）全部复用 orangepi-build 已验证的
  deb 抽取产物**（`external/cache/sources/sun60iw2_packages/bullseye/…`，见
  `sun60iw2.conf` 的 family_tweaks_s 实现）。这些都是 glibc 编译产物。
- Tina buildroot 2022.05 的 glibc 是 **2.35 ≥ 2.30** ✓（GPU 库要求满足）。
- 库依赖（libstdc++、openssl、zlib 等）由 buildroot 按 `readelf -d` 需要开启对应包。

---

## 2. 决策

### 2.1 内核路线：选 (a)——Tina 的 buildroot 直接编我们的内核树

理由：
1. 16 个补丁 + 调优 config 只在 2ac08e8c7 上验证过；Tina 的 linux-6.6 + bsp 组合是另一棵
   树，把 AR0234/RT/amp_timestamp 全套重新移植、重验证，风险和工作量都大，且其 bsp_defconfig
   不含 PREEMPT_RT（6.6.98 需要 rt 补丁整套打入，0000 就是为厂家树准备的）。
2. 两树同构（§1.3），buildroot 用 `BR2_LINUX_KERNEL` + 本地源码 + 我们的 config 编内核
   没有任何移植成本；Tina 的 bsp_defconfig 只做参考比对。
3. 验收 3/6/7/8 全部直接复用已验证资产，可复现性由"固定 commit + 固定补丁 + 固定 config"
   保证（见 §4.1 的内容寻址 tarball）。

### 2.2 Bootloader：复用 orangepi-build 已验证的那一套（不做 Tina pack）

理由：boot0 是 DRAM 初始化的载体，Tina 通用 boot0 与 zero3w 专用 boot0 二进制不同、未在
本板验证；U-Boot 2018.07 + bl31 + boot.scr 的地址布局（kernel 0x41000000 / fdt 0x44000000 /
ramdisk 0x45000000 / bl31 0x48000000）是踩过 "Bad Data CRC" 坑调出来的。SCP 必须是
`e902/fw-out/vendor-scp.bin`（sha256 302deda8…，验收 6），Tina 的 scp.fex 不是。
产线：`scripts/pack-uboot.sh`（orangepi-build 机制，工具在
`external/packages/pack-uboot/tools/`）打出 boot0 + boot_package，再用
`e902/tools/bootpkg.py` 把 vendor-scp.bin 换进 scp 槽并修 add_sum（e902 已验证流程）。
Tina 的 brandy/spl-pub 完全不碰。**镜像组装用我们自己的脚本 + genimage**（host 工具，
buildroot 自带），不依赖 Tina 被排除的 pack 工具链。

### 2.3 工具链：buildroot 内部工具链（glibc 2.35）

理由：官方外部工具链在 MEGA 大文件包里（下载 14 GB 不可控、不确定链接），内部工具链
（`BR2_TOOLCHAIN_BUILDROOT_GLIBC`）一次构建、完全可复现、无外部依赖；内核也用同一
工具链编（buildroot 的 `BR2_LINUX_KERNEL`），整条链一个 `make` 出齐。gcc-arm-11.2
（orangepi-build 本地那套）只作为 SCP 构建沿用 e902 流程时使用。代价：首次构建多
30–60 分钟。官方工具链路线作为可选验证（§6 风险 R6），不进产品链。

### 2.4 rootfs 形态：systemd + 最小化

- init 用 **systemd**（与现网 Ubuntu/Debian 镜像行为一致；Tina demo 也是 systemd），
  getty 挂 ttyS0 115200；用户 orangepi（sudo）。
- WiFi：内核 aic8800 模块（我们的树）+ wpa_supplicant + 固件从已验证产物抽取进 overlay；
  开机自动连 OpenWrt（预置 wpa_supplicant.conf）。
- ssh：openssh-server（与现网一致）。
- 验收 5 的"板上编译"：buildroot 2022.05 没有板上 gcc 选项（`BR2_PACKAGE_GCC_TARGET`
  已在上游移除）。方案：**加一个自定义 buildroot 包，把 gcc 以 target-hosted 方式编一次
  （--host=aarch64 --disable-bootstrap --enable-languages=c，只出 gcc+libgcc）**，源码直接用
  buildroot 内部工具链同款 gcc 源码包，装进 rootfs；板上即可 `gcc cltest.c -lOpenCL` 编译。
  若该包构建不顺（风险 R3），回退为"镜像内预装交叉编译好的二进制 + 源码"并在交付时注明
  偏差。

### 2.5 镜像布局：完全沿用已验证的 SD 布局

```
偏移 0           保留（含 8 KiB 处 boot0，16400 KiB 处 boot_package：u-boot+bl31+vendor-scp）
分区 1  @ 20 MiB  rootfs（ext4，buildroot 产物，烧后在线扩容）
/boot 在 rootfs 内：boot.scr + uImage + uInitrd(可选) + dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb
bootargs：console=ttyS0,115200 isolcpus/nohz_full/rcu_nocbs=5 …（与现网验证参数一致）
```

### 2.6 仓库边界（约束遵守）

- 全志源码/binaries 一律在 `~/tina5`（SDK 子集拉取脚本 `fetch-sdk.sh` 落盘在仓库里，拉取动作
  可复现）；`orangepi-build` 仓库里只进：脚本、buildroot external 配置、overlay 的文本部分、
  补丁、文档。
- 闭源二进制（GPU/ISP/cedarc 库、固件）构建时从 orangepi-build 本地已有的 deb/固件目录
  抽取，不进 git。
- 不回退/覆盖当前 `next` 分支上未提交的 6 个文件改动；`tina-zero3w/` 全部是新文件。

---

## 3. 构建架构

```
tina-zero3w/
├── PLAN.md                  本文件
├── README.md                一条命令构建 + 烧卡 + 上板验收文档
├── fetch-sdk.sh             拉取/更新 ~/tina5 里的 SDK 子集（浅克隆，~5 GB）
├── prepare-kernel.sh        厂家树 @2ac08e8c7 导出 + 0000-0015 补丁 + 调优 config
│                            → 内容寻址 tarball 进 ~/tina5/dl/（输入不变则跳过）
├── build-image.sh           一条命令：sdk → kernel tarball → buildroot（内核+模块+rootfs）
│                            → GPU 模块 → uImage/boot.scr → genimage 出 SD 镜像
├── flash-image.sh           烧卡（检查 removable、备份前 24 MiB、确认后写入）
├── test-board.sh            上板验收脚本（对照 §8 清单，串口/ssh 采集原始输出）
├── br2-external/            buildroot external（我们唯一的 BR2_EXTERNAL，不用 kunos 的）
│   ├── external.desc / Config.in / external.mk
│   ├── configs/sun60iw2p1_zero3w_defconfig
│   ├── package/zero3w-gpu/  pvrsrvkm 模块 + 用户态库/固件安装（从 deb 抽取）
│   ├── package/zero3w-isp/  libisp/libAWIspApi + libcedarc 安装
│   ├── package/zero3w-native-gcc/  板上编译器（§2.4）
│   ├── overlay/             etc/wpa_supplicant、systemd 单元、ICD json、测试工具源码等文本
│   └── board/orangepizero3w/
│       ├── genimage.cfg     SD 布局（§2.5）
│       ├── boot.cmd         基于 external/config/bootscripts/boot-sun60iw2.cmd + RT cmdline
│       └── post-image.sh    uImage 包装(mkimage)、boot.scr、genimage
└── docs/
    ├── PITFALLS.md          新踩坑记录（滚动更新）
    └── TINA-NOTES.md        Tina SDK 结构速查（本轮调研沉淀）
```

数据流（`sudo ./tina-zero3w/build-image.sh` 或普通用户跑，涉及 root 的步骤内部处理）：

1. `fetch-sdk.sh`：检查/补齐 `~/tina5`：buildroot-202205、buildroot/config、target/a733、
   target/common、device/config/{a733,common,rootfs_tar}、build、lichee/bsp、lichee/linux-6.6
   （后两者用于比对，不进构建）、kernel/linux-6.6（同前）。
2. `prepare-kernel.sh`：`git -c safe.directory=… archive 2ac08e8c7` 导出厂家树（不动
   orangepi-build 的工作树）→ `git apply userpatches/kernel/sun60iw2-current/0000..0015` →
   覆写 `userpatches/linux-sun60iw2-current-a733.config` → 打 tarball
   `linux-6.6.98-rt58-a733-<hash>.tar.xz`（hash = commit+补丁+config 摘要）。
3. buildroot：`make BR2_EXTERNAL=tina-zero3w/br2-external O=… sun60iw2p1_zero3w_defconfig`
   （linux 包用 tarball + `BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE`；编出 Image/dtb/全部 in-tree
   模块含 aic8800、vin、amp_timestamp；出 rootfs.ext4）。
4. `zero3w-gpu` 包：解 `xserver-xorg-img-bxm_*.deb` 抽用户态库+固件+ICD；用 buildroot 的
   kernel 构建目录编 `bsp/modules/gpu/img-bxm`（含 compilation.sh 同款 symlink 修复），
   装 `pvrsrvkm.ko` 到 `/lib/modules/<ver>/extra`。
5. `zero3w-isp` 包：抽 libAWIspApi/libcedarc/gst-omx deb 装库。
6. boot 组件：`pack-uboot.sh` + `bootpkg.py` 换 SCP → `boot0.bin`/`boot_package.bin`；
   post-image：mkimage 包 uImage、编 boot.scr、genimage 合成 `sdcard.img`。
7. `flash-image.sh` → 上板 → `test-board.sh`。

---

## 4. 关键技术点

### 4.1 内核可复现性
`prepare-kernel.sh` 对 (厂家 commit 2ac08e8c7 × 16 补丁 × config) 做摘要，tarball 文件名携带
摘要；输入不变则直接复用 `~/tina5/dl/` 里现成的 tarball。构建日志打印内核 `uname` 版本串
期望值 `6.6.98-rt58-…` 供上板比对。

### 4.2 buildroot 里编内核的注入方式
- `BR2_LINUX_KERNEL=y`、`BR2_LINUX_KERNEL_CUSTOM_TARBALL_LOCATION=file://~/tina5/dl/<tarball>`
  （普通 extract 流程，不经 OVERRIDE_SRCDIR——override 会跳过 buildroot 打补丁，但我们补丁
  已打进 tarball，无所谓；选 tarball 是为了下载缓存与校验语义）。
- `BR2_LINUX_KERNEL_USE_CUSTOM_CONFIG` + 我们的完整 .config。
- `BR2_LINUX_KERNEL_INTREE_DTS_NAME="allwinner/sun60i-a733-orangepi-zero3w"`。
- cmdline 不进内核（CMDLINE_ARGS 留空），全部由 boot.scr 传。

### 4.3 GPU 模块的坑（已知，直接带上）
rogue_km 构建目录里 145 个指向 `/root/orangepi/…` 的 symlink 要先重指到本地源码
（compilation.sh 的修复逻辑复制到 `zero3w-gpu` 包里）；pvrsrvkm.ko 装到
`/lib/modules/<ver>/extra`（ver 含 -rt58 后缀）；Vulkan 必须有 `libpvr_mesa_wsi.so`；
OpenCL/Vulkan ICD 文本进 overlay。

### 4.4 E902 / amp_timestamp
- SCP：打包时 `bootpkg.py` 换入 `e902/fw-out/vendor-scp.bin` 并修 add_sum（该流程已在板上
  3 次重启验证）。也可以先跑 `e902/vendor-scp/build.sh` 重编验证 sha 一致。
- amp_timestamp：补丁 0014 已含驱动+DTS 节点；若 zero3w.dts 默认未启用该节点，则在
  `tina-zero3w/br2-external` 里加一个小补丁（0016）启用——上板 dmesg 必须出现 24 MHz 报告
  （验收 6）。

### 4.5 RT 参数
bootargs 与现网验证一致：`isolcpus=5 nohz_full=5 rcu_nocbs=5`（独占 A55 cpu5，A76 留给
别的负载）；测试工具 `waitlat.c`/`rt-test.sh`/`ab.sh` 源码进 overlay，`cpu_dma_latency`
由测试脚本打开保持。对照数据：混合等待 0.24–0.30 µs、纯忙等 0.38–1.54 µs（验收 7 的目标
线 < 2 µs）。

### 4.6 swupdate A/B（加分项，第二阶段）
`target/a733/buildroot/pro3/swupdate/` 有现成 sw-description 模板。第一步先不开 A/B（单
rootfs + 只读 rootfs 选项），验收 1–8 全过后再加：分区表换 A/B 布局 + swupdate 包 + sw-
description（bundle 里放 rootfs + 内核）。

---

## 5. 实施顺序

| 阶段 | 内容 | 产出/验收点 |
|---|---|---|
| P1 | fetch-sdk + prepare-kernel 跑通，tarball 生成 | tarball 可解、含 -rt58 config |
| P2 | buildroot defconfig + 内核编出 Image/dtb/模块（不跑完整 rootfs） | `Image` + `sun60i-a733-orangepi-zero3w.dtb` + `aic8800_fdrv.ko` 等 |
| P3 | 完整 rootfs（systemd、wpa、ssh、overlay）+ 各 zero3w 包 | rootfs.ext4，loop-mount 检查 |
| P4 | boot 组件 + genimage 镜像 | `sdcard.img`；loop-mount 核对布局/文件 |
| P5 | 烧卡上板（**先问用户** + 备份卡头），test-board.sh 全绿 | §8 全表 |
| P6 | 文档收尾 +（可选）swupdate/只读 rootfs/启动时间 | 验收 9 |

---

## 6. 风险与对策

- **R1 buildroot 编内核与厂家树的 Makefile/工具链兼容性**：buildroot 2022.05 gcc 12.x 编
  6.6.98 没问题（主线支持）。vendor Makefile 若有硬编码路径，在 prepare 阶段打补丁修。
- **R2 内部工具链 + 闭源库 ABI**：库是 gcc 9/10 (bullseye) 编的，glibc 2.35 向后兼容 ✓；
  libgcc_s.so.1 版本差由 buildroot 提供 ✓。装完用 `readelf -d` 扫缺库。
- **R3 板上 gcc 包构建失败**：回退预装二进制方案，验收 5 注明偏差（§2.4）。
- **R4 uImage 大小**：config 已带 ftrace 全家，uImage >32 MiB 的坑已在 boot.cmd 地址布局里
  解决；构建后检查 `uImage` < 0x3000000（48 MiB，到 fdt 的间距）。
- **R5 Tina 源码许可**：SDK 全部在 `~/tina5`（repo 外），`tina-zero3w/` 只进自产文件；
  buildroot 产物镜像不进 git。
- **R6 官方 SDK 能否"原样"编 demo_aiot**：受 MEGA 大文件限制不可纯 git 完成；方案不依赖
  它。若用户想验证官方路径，另行下载 MEGA 包（fetch-sdk.sh 预留 `--official` 开关的文档
  说明），不在本方案关键路径上。

---

## 7. 明确不做的事

- 不改 `orangepi-build` 现有构建行为（`sudo ./build.sh a733` 继续可用）；不动未提交改动。
- 不把任何全志源码/二进制提交进 `zyitom/orangepi-build`。
- 不在第一遍就把 swupdate/只读 rootfs 塞进来（先过 1–8，再加 9）。
- 不用 Tina 的 pack/phoenix 工具链产 .img（依赖 MEGA；SD 镜像用 genimage）。

---

## 8. 验收对照

| # | 标准 | 本方案的达成路径 |
|---|---|---|
| 1 | 一条命令出镜像 + 文档 | `tina-zero3w/build-image.sh` + README |
| 2 | 上电免干预到串口登录 | systemd getty ttyS0；boot.scr 全自动；P5 串口原始输出为证 |
| 3 | PREEMPT_RT + 我们的补丁 | §2.1 路线，`uname -v` 含 PREEMPT_RT |
| 4 | aic8800 WiFi + ssh | 内核模块 + wpa_supplicant + openssh；板上 `ssh` 实测 |
| 5 | cltest mismatches=0；vulkaninfo BXM-4-64 | GPU 库/固件/ICD + 板上 gcc（§2.4） |
| 6 | SCP=vendor-scp.bin(302deda8)；amp-timestamp 24 MHz | bootpkg.py 注入 + 补丁 0014 + DTS 启用 |
| 7 | 独占 A55 hybrid < 2 µs | 同款内核/config/cmdline + waitlat，对照 §已有测试数据 |
| 8 | AR0234/vin 驱动 + libisp/libcedarc | 内核补丁系列 + zero3w-isp 包 |
| 9 | 加分：swupdate A/B、只读 rootfs、启动时间 | P6 |
