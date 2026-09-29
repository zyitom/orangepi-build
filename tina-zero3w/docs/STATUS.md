# Tina/Buildroot 移植 Zero 3W（A733）— 完成状态记录

日期：2026-09-28 ｜ 依据：`docs/ORIGINAL-BRIEF.md` ｜ 构建：`~/tina5/buildroot-out/images/sdcard.img`（#59 起）

## 一、交付物总览（全部可一条命令复现）

| 脚本 | 功能 | 状态 |
|---|---|---|
| `fetch-sdk.sh` | SDK 拉取（上游 Buildroot 2022.05 + Tina SDK 参考子集）| ✅ |
| `prepare-kernel.sh` | 厂家树工作区确定性快照 → 内核 tarball（95819 文件，内容寻址 hash，两次构建一致）| ✅ |
| `build-image.sh` | 一条命令全流程：SDK → 内核 → 换 SCP boot 组件 → buildroot → genimage（flock 串行化防并发）| ✅ |
| `flash-image.sh` | 整卡烧录（removable 检查 + 24MiB 卡头备份 + yes 确认）| ✅ |
| `flash-rootfs.sh` | 仅重烧 rootfs 分区（boot 组件不动，迭代加速）| ✅ |
| `deploy.sh` | 推文件到运行中的板子（开发迭代，秒级）| ✅ |
| `test-board.sh` | 验收 1-8 自动采集（报告 test-report-*.txt）| ✅ |
| `tests/` | vktest.c（Vulkan 探针）、ioctl_shim.c（PVR bridge 协议全量解码器）| ✅ |
| `vulkan-debug.sh` | Vulkan 诊断三连（PVRDebugLevel/strace/dmesg）| ✅ |

## 二、验收 8 项终态

| # | 项 | 结果 | 证据 |
|---|---|---|---|
| 1 | 一条命令构建 + 文档 | ✅ | build-image.sh 全自动 + README/PLAN/PITFALLS/STATUS |
| 2 | 上电串口登录零干预 | ✅ | 多次实测；用户态 20.5s→**6.5s**（journal 单调时间戳）|
| 3 | PREEMPT_RT 内核 | ✅ | 6.6.98-rt58 `#1 SMP PREEMPT_RT`，补丁系列 **0000-0022**（含 3 个诊断补丁），每次构建反向校验 |
| 4 | WiFi + ssh | ✅ | aic8800d80 自动连（modules-load 早载修复 SDIO 竞态）|
| 5a | cltest 板上编译 + OpenCL | ✅ | 板上 gcc 10.3 编译，**mismatches=0**，GPU 48ms / 16.8 GFLOPS |
| 5b | vulkaninfo | ✅（2026-09-29 修复） | 根因 rootfs 缺 libxshmfence；#61 板上 vulkaninfo 列出 **BXM-4-64 MC1**（API 1.3.277），compute 256/256 |
| 6 | boot_package SCP + amp_timestamp | ✅ | vendor-scp.bin sha `302deda8…`、add_sum `265fe798`、checksum/readback PASS；dmesg `freqid=24000000 (24.000 MHz)` |
| 7 | waitlat 独占 cpu5 | ✅ | hybrid **max=0.25µs**（目标 <2µs）|
| 8 | AR0234 + ISP/cedarc | ✅ | 传感器 ID 0xa56 + 实拍 **1920x1200 NV12 @120.4fps、602 帧/5s、0 超时**；libisp/libAWIspApi/cedarc 全套 |

**合计 8 / 8**。（2026-09-29：5b 不是全志 DDK 交付缺口，是本移植 rootfs 缺库，修复后板上通过，见第四节。）

## 三、已修复的工程缺陷（全部沉淀于 docs/PITFALLS.md，40+ 条）

### 构建系统层
- FORTIFY_SOURCE=3 × systemd 250.4 三处 abort（0001/0002 补丁，gdb 反推定位）
- target-finalize 三陷阱：`rm -rf usr/include`、`*.a` 全删、install-gcc EXTRA_PARTS 缺失、glibc `/usr/lib64` 绝对路径 → post-fakeroot 统一恢复
- `/etc/ld.so.conf{,.d}` 禁令 vs PVR 搜索路径 → post-fakeroot 写入 + libvulkan/lib64 符号链接
- strace 5.17 对 6.6 内核 UAPI 三连不兼容 → SDK 内升级 strace 6.6
- kconfig 静默丢弃三例（XORG7/DTS_SUPPORT/SCHEDUTILS）
- 并发构建互删 → flock；补丁文件必须 apply 进工作区（快照式 tarball）；换 tarball 需 linux-dirclean
- ~~GPU 用户态库 target 截断损坏~~（2026-09-29 更正：target 与 deb 的大小差异是 Buildroot 正常 strip，对 deb 原件做同样 strip 后 md5 完全一致，并无损坏）

### 启动链
- uImage `-A arm`（vendor U-Boot 不认 arm64 legacy）、**禁用 gzip**（bootm 卡死→看门狗循环）
- boot.scr 缺 `fdt resize 65536` → FDT_ERR_NOSPACE 挂死
- 模块早载（WiFi SDIO 竞态 / vin 无 of-modalias）

### 产品化
- WiFi regulatory.db、rootfs 首启自动扩容（parted+resize2fs）、zram 2G zstd
- 板上工具链完整化：cross-native gcc + CL/cedarc 头文件 + taskset

## 四、Vulkan（2026-09-29 更正）

**根因：Buildroot rootfs 缺 `libxshmfence.so.1`。** libVK_IMG 创建 instance 时
`dlopen("libpvr_mesa_wsi.so", RTLD_NOW)`，该库依赖 libxshmfence → 加载失败 → ICD 返回 -3
→ loader 报 -9。反汇编定位 + qemu 离线复现（缺库时 dlopen 失败、补上即成功）已证实；
修复为 zero3w-gpu 包 `select BR2_PACKAGE_XLIB_LIBXSHMFENCE`，构建脚本的依赖扫描同时
补上 `/usr/local/lib`。板上实测（2026-09-29，镜像 #61，6.6.98-rt58 PREEMPT_RT）：`vkCreateInstance` → VkResult=0，枚举出
PowerVR B-Series BXM-4-64 MC1（API 1.3.277，driver "PowerVR B-Series Vulkan Driver"）；
vkcomp compute 256/256 正确（连跑 3 次）；`ld.so --list libpvr_mesa_wsi.so` 0 个 not found；dmesg 无 GPU 报错。详见 `docs/VULKAN-HANDOFF.md`。

此前"厂家 DDK 不支持设备枚举、平台级限制、需非 RT 内核 + Mesa pvr"的定性作废：
我们的 noble 镜像（装有 libxshmfence1）09-28 已在板上跑通 Vulkan compute。
真实限制只有一条：上屏只能经支持 DRI3 的 X11（ICD 无 VK_KHR_display），无头计算不受影响。

## 五、当前状态与待办

| 项 | 状态 |
|---|---|
| 卡上系统 | vendor noble 镜像（Vulkan 对照实验用，可随时 `flash-image.sh` 换回）|
| 最终交付镜像 | **#61，已烧卡并上板验证**（#59 + libxshmfence 修复；内核同 #59，仍含 0020–0022 诊断日志与 PRIME 实验代码）|
| 卡头备份 | `~/tina5/sd-backup/sd-head-sdb-*.img`（5 份，含恢复说明）|
| swupdate A/B OTA | 设计完成（README「三条路径」节），未实施——需改分区布局 + u-boot bootcount |
| 只读 rootfs | 未实施（同上，可选迭代）|
| git | 已提交并推送：zyitom/orangepi-build 的 `zero3w` 分支（默认分支）|

## 六、复现速查

```sh
bash tina-zero3w/build-image.sh                 # 构建镜像
bash tina-zero3w/flash-image.sh /dev/sdb        # 烧卡（备份+确认）
bash tina-zero3w/test-board.sh                  # 板上验收采集
BOARD=<IP> bash tina-zero3w/vulkan-debug.sh     # Vulkan 诊断三连
```
