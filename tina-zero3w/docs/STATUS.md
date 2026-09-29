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
| 5b | vulkaninfo | ❌ **vendor-blocked** | 见「四、Vulkan 终局」 |
| 6 | boot_package SCP + amp_timestamp | ✅ | vendor-scp.bin sha `302deda8…`、add_sum `265fe798`、checksum/readback PASS；dmesg `freqid=24000000 (24.000 MHz)` |
| 7 | waitlat 独占 cpu5 | ✅ | hybrid **max=0.25µs**（目标 <2µs）|
| 8 | AR0234 + ISP/cedarc | ✅ | 传感器 ID 0xa56 + 实拍 **1920x1200 NV12 @120.4fps、602 帧/5s、0 超时**；libisp/libAWIspApi/cedarc 全套 |

**合计 7.5 / 8**——5b 为全志 DDK 交付缺口（对 Debian/Ubuntu/Buildroot 一视同仁），非移植缺陷。

## 三、已修复的工程缺陷（全部沉淀于 docs/PITFALLS.md，40+ 条）

### 构建系统层
- FORTIFY_SOURCE=3 × systemd 250.4 三处 abort（0001/0002 补丁，gdb 反推定位）
- target-finalize 三陷阱：`rm -rf usr/include`、`*.a` 全删、install-gcc EXTRA_PARTS 缺失、glibc `/usr/lib64` 绝对路径 → post-fakeroot 统一恢复
- `/etc/ld.so.conf{,.d}` 禁令 vs PVR 搜索路径 → post-fakeroot 写入 + libvulkan/lib64 符号链接
- strace 5.17 对 6.6 内核 UAPI 三连不兼容 → SDK 内升级 strace 6.6
- kconfig 静默丢弃三例（XORG7/DTS_SUPPORT/SCHEDUTILS）
- 并发构建互删 → flock；补丁文件必须 apply 进工作区（快照式 tarball）；换 tarball 需 linux-dirclean
- GPU 用户态库 target 截断损坏（并发战争残骸）→ dirclean 重装 + 大小比对诊断法

### 启动链
- uImage `-A arm`（vendor U-Boot 不认 arm64 legacy）、**禁用 gzip**（bootm 卡死→看门狗循环）
- boot.scr 缺 `fdt resize 65536` → FDT_ERR_NOSPACE 挂死
- 模块早载（WiFi SDIO 竞态 / vin 无 of-modalias）

### 产品化
- WiFi regulatory.db、rootfs 首启自动扩容（parted+resize2fs）、zram 2G zstd
- 板上工具链完整化：cross-native gcc + CL/cedarc 头文件 + taskset

## 四、Vulkan 终局（调查 3 天、8+ 对照实验、三层仪器化）

**定性：全志 24.2 DDK 的 Vulkan 用户态从未在此 SoC 上支持设备枚举——对 Debian/Ubuntu/Buildroot 一视同仁。**

关键证据：
1. **vendor 官方 noble 镜像本板实测**：系统自带 vulkaninfo `Devices` 为空（零设备）——原栈同样失败；
2. 内核侧零失败：0021/0022 日志补丁实测，ICD 的每个 bridge 调用内核都返回 PVRSRV_OK；
3. 数据全对：Connect BVNC=36.56.104.183 正确、KernelArch=64、能力标志正常；GetMultiCoreInfo eError=OK；
4. 全部假设排除：模块三种构建（我们/原厂预编译/Radxa dkms 源码）、ICD 两种构建（逐字节同）、PRIME-import 补丁、强制核数、X 环境、库版本、API 版本；
5. noble 镜像确有 libVK_IMG（此前误判"vendor 没发布"仅对 bookworm 成立），但"库存在"≠"枚举可用"。

**如需真 Vulkan**：Mesa `pvr` 驱动对该 BVNC 有 Vulkan 1.2 一致性认证，但需主线内核 6.17+ 的 pvr DRM（与厂家 rogue_km UAPI 互斥）→ 独立启动介质（Radxa Cubie A7A 镜像可直接跑）或等主线成熟后栈迁移。社区参考：github.com/ayiejosh/a733-powervr-fex。

## 五、当前状态与待办

| 项 | 状态 |
|---|---|
| 卡上系统 | vendor noble 镜像（Vulkan 对照实验用，可随时 `flash-image.sh` 换回）|
| 最终交付镜像 | **#59 已构建待烧**（含 pristine GPU 库 + regdb + 首启扩容 + zram + 0021/0022 诊断日志）|
| 卡头备份 | `~/tina5/sd-backup/sd-head-sdb-*.img`（5 份，含恢复说明）|
| swupdate A/B OTA | 设计完成（README「三条路径」节），未实施——需改分区布局 + u-boot bootcount |
| 只读 rootfs | 未实施（同上，可选迭代）|
| git | `next` 分支，tina-zero3w/ 与 userpatches 全部未提交——**等用户明确要求才 commit/push** |

## 六、复现速查

```sh
bash tina-zero3w/build-image.sh                 # 构建镜像
bash tina-zero3w/flash-image.sh /dev/sdb        # 烧卡（备份+确认）
bash tina-zero3w/test-board.sh                  # 板上验收采集
BOARD=<IP> bash tina-zero3w/vulkan-debug.sh     # Vulkan 诊断三连
```
