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

## 五、2026-09-29/30 整机硬件检查（镜像 64/65）

| 项 | 结果 |
|---|---|
| 全项检查脚本 `tests/hw-check.sh` | **18/18 通过**（Vulkan 枚举+compute、OpenCL mismatches=0、cyclictest/rtla、RTC、BT、USB roles 等）|
| 内核补丁 0024–0029 | USB0 device 口、otg_role store 的 sysfs WARN、aic8800 休眠唤醒等待、GPADC 按需采样（cpu0 ~4 kHz 轮询中断风暴消失）、G2D 自动加载（MODULE_DEVICE_TABLE）、SoC RTC 启用（rtc0，掉电重启时间正确）|
| A55 小核簇 | policy0 固定 performance（DVFS 期间的 I²C 中断风暴随之消失）|
| WiFi | power_save off（reason=4 踢线；**30 分钟以上空闲观察未做**）|
| 蓝牙 | hciattach_opi + aic-btaddr(0xFC70) 每板唯一地址；/var/lib/bluetooth 进 tmpfiles |
| s2idle | 可被 RTC 闹钟唤醒（挂起 20 s 定时唤醒实测）|
| 深睡眠（deep） | 见下节，根因已定位，修复待上板 |
| Tina 构建缺陷 | 内核一直停在 9 月 25 日旧源码（buildroot 的 DL 缓存 tarball 未随补丁系列失效）；prepare-kernel.sh 改为内容寻址 hash 并删除旧缓存后重建 |

## 六、深睡眠根因（2026-09-30，修复待上板验证）

症状：deep 进入后，调试版 SCP（`vendor-scp-debug.bin`，57600 on ttyUSB1）打出
`WRN:no standby_param` → `cpu off` → `the first time ddr standby` 后静音，
RTC 闹钟到点也不醒，必须断电。

根因链：
1. SCP 的 FDT 来自 `RTC_DTB_BASE_STORE_REG`(0x0709010C) 指向的 0x44000000；
2. 本板启动链没有厂商那种把 `dram_para` 合并进 FDT 的通道（boot0 报
   `error: dtb not found for scp`，U-Boot 的 bootparam→FDT 修整发生在 SCP
   解析之后），`/dram` 下 160 个参数全为 0（SCP 启动日志可见）；
3. 闭源 dramlib（`dram_power_save_process`）带全零参数做 save，SCP 卡死在
   init 序列中途（日志里连 `wait wakeup` 都没到），唤醒自然无从谈起。

已做修复（2026-09-30 板上首测：SCP 侧全链路已通，还差最后一步）：
- `e902/vendor-scp/patches/0004-dram-para-zero-fallback.patch`：参数全零时用内置表
  （sys_config 值 + boot0 运行时打印的 dram_clk=2400/para1=0xa0fa/para2=0x10001001/tpr13=0x65）；
- `e902/vendor-scp/patches-debug/0002-suspend-path-probes.patch`：`dram save done` /
  `ppu on` / `dram up enter/done` 探针；
- `userpatches/kernel/sun60iw2-current/0030`：板级 DTS 补 `standby_param` 节点
  （先不放电源位图，全零=休眠时不动任何路，保守）+ `dram_para00..31`。

**板上实测（debug SCP，E902 控制台）**：`dram save done`（库返回，兜底表生效）→
唤醒触发 → `dram up enter` → DRAM 完整 banner（4096MB，para 与表一致）→
`dram up done` → `cpu on` → `wait ac327 resume...` —— SCP 侧修复链确认打通。
新发现两件事（e5066e1）：
1. `dram_clk` 必须是 2400（boot0 实测 "DRAM CLK =2400 MHZ"），不是 sys_config 的
   1200——恢复按参数重配控制器，写 1200 会让起来的 A55 撞上错频的 DRAM。
   已改表重编（vendor-scp c98bdbca / debug 79cd0eec，scp.fex 与 noble 镜像已同步）。
2. 进入后 ~0.2s 即被唤醒（不是 +20s RTC）：内核把 ehci1/ohci1(SPI 159/160) 和
   r_pio(SPI 200) 登记为唤醒源，电平中断在挂起时已有效就会立刻唤醒。
   `deep-suspend-retest.sh` 现在会先解绑 USB1 控制器并打印唤醒源清单。
- 待办：断电重启后重跑 `e902/tests/board/deep-suspend-retest.sh`；若 USB 解绑后
  能撑满 20s 并被 RTC 唤醒 → 换回正式版固件收尾；若仍瞬醒 → 排查 r_pio(200)。

## 七、当前状态与待办

| 项 | 状态 |
|---|---|
| 卡上系统 | vendor noble 镜像（Vulkan 对照实验用，可随时 `flash-image.sh` 换回）|
| 最终交付镜像 | **#61，已烧卡并上板验证**（#59 + libxshmfence 修复；内核同 #59，仍含 0020–0022 诊断日志与 PRIME 实验代码）|
| 卡头备份 | `~/tina5/sd-backup/sd-head-sdb-*.img`（5 份，含恢复说明）|
| swupdate A/B OTA | 设计完成（README「三条路径」节），未实施——需改分区布局 + u-boot bootcount |
| 只读 rootfs | 未实施（同上，可选迭代）|
| git | 已提交并推送：zyitom/orangepi-build 的 `zero3w` 分支（默认分支）|

## 八、复现速查

```sh
bash tina-zero3w/build-image.sh                 # 构建镜像
bash tina-zero3w/flash-image.sh /dev/sdb        # 烧卡（备份+确认）
bash tina-zero3w/test-board.sh                  # 板上验收采集
BOARD=<IP> bash tina-zero3w/tests/hw-check.sh   # 整机 18 项检查
BOARD=<IP> bash tina-zero3w/vulkan-debug.sh     # Vulkan 诊断三连
```
