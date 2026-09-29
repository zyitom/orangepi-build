# A733 (BXM-4-64) Vulkan 故障交接文档

日期：2026-09-28 ｜ 板：Orange Pi Zero 3W（全志 A733/T527 family）
GPU：PowerVR BXM-4-64，BVNC **36.56.104.183** ｜ DDK：**24.2@6603887**（内核模块 pvrsrvkm + 用户态 libVK_IMG/libsrv_um，全部来自厂商 deb `xserver-xorg-img-bxm_1.21.1-2_arm64.deb`）

## 一、现象

**三个系统上 Vulkan 均无法枚举设备，OpenCL 完全正常：**

| 系统 | 内核 | 现象 |
|---|---|---|
| vendor Ubuntu noble 1.0.2 原版镜像 | 6.6.98-rt58-sun60iw2（原厂）| 系统自带 `vulkaninfo`（1.3.275）：`Devices:` **空** |
| vendor Debian bookworm 1.0.2 | 同上 | 无 libVK_IMG（未交付）|
| 我们的 Buildroot（同一内核源码自建）| 6.6.98-rt58 | `vkCreateInstance` 返回 **VK_ERROR_INCOMPATIBLE_DRIVER(-9)**（API 1.0/1.3.277/1.3.280、root/非 root、有无 X 全部一致）|

**同时 OpenCL 完全正常**（同一连接、同一固件）：libPVROCL 枚举设备成功，GPU 计算
`mismatches=0`、16.8 GFLOPS → 内核驱动、固件（rgx.fw.36.56.104.183）、硬件均正常。

## 二、已排除项（全部有对照实验）

| 排除项 | 方法 |
|---|---|
| 内核模块构建 | 三种模块实测一致：我们源码构建 / **原厂预编译二进制**（vermagic 补丁热替换）/ Radxa img-bxm-dkms 0.1.0-3 源码编译 |
| ICD 构建 | Ubuntu noble 镜像的 libVK_IMG 与我们 deb 的**逐字节相同**（md5 22302f1e，unstripped）|
| libsrv_um | 同上（md5 1bb80d24）|
| 固件 | rgx.fw/rgx.sh md5 相同 |
| 内核配置 | 与 noble 镜像 `config-6.6.98-rt58-sun60iw2` **diff = 0** |
| 设备树 | 反编译 diff，仅 pinmux 差异（GPU/reserved-memory 节点一致）|
| PRIME-import | 套用社区补丁（a733-powervr-fex）重编模块——枚举无关 |
| 多核拓扑 | 强制 NumCores=4 依旧 -9；RGX_CR_MULTICORE_SYSTEM 寄存器实读 1 为硬件真值；caps[0]=0x78 含 PRIMARY\|GEOMETRY\|COMPUTE，拓扑合法 |
| X11 环境 | Xvfb 经 ssh 转发连通后依旧失败（noble 上有 X、buildroot 无头——不是差异点）|
| API 版本 | 1.0 / 1.3.277 / 1.3.280 全部 -9 |
| 用户态库损坏 | target/lib 截断文件已修复（libpvr_mesa_wsi 曾差 667KB）——修复后不变 |

## 三、关键测量（ioctl_shim + 内核日志补丁）

**bridge 调用序列**（LD_PRELOAD 解码 PVRSRV_BRIDGE_PACKAGE，cmd 0xc0206440）：

```
Connect(func0)        → eError=OK, BVNC=36.56.104.183 ✓, CapabilityFlags=0x20000, KernelArch=64
AcquireGlobalEventObject(func2) → handle=1, OK
AcquireInfoPage(func15)         → hPMR=1, OK
AlignmentCheck(func10)          → OK
GetMultiCoreInfo(func12)        → eError=OK, NumCores=1   ← capsSize=0
ReleaseGlobalEO(func3) / ReleaseInfoPage(func16) → OK
Disconnect(func1)               → 随后 vkCreateInstance 返回 -9
```

**内核侧**（诊断补丁 0021/0022，已入内核补丁系列）：
- `BridgedDispatchKM` 失败日志：**零记录**——ICD 发出的每个 bridge 内核都返回 OK；
- `PVRSRVGetMultiCoreInfoKM`：**从未被调用**（LD_DEBUG 的符号绑定顺序 ≠ 调用顺序）；
- 固件正常加载：`RGX Device registered BVNC 36.56.104.183` + `rgx.fw/rgx.sh loaded`；
- 无 build options mismatch 告警（Connect 的 client/KM options 校验通过）。

**结论**：ICD 在收到**全部成功应答**（含正确 BVNC）后，在闭源用户态内部判定
"无兼容设备"并断开——判定点不可从外部观测。

## 四、复现步骤（Buildroot 镜像，5 分钟）

```sh
bash tina-zero3w/build-image.sh && bash tina-zero3w/flash-image.sh /dev/sdb
# 上电后（WiFi 自动连，板 IP 见路由）：
BOARD=<IP> tools/ssh_board.sh "gcc -O2 -I/usr/include -o /tmp/vktest /tmp/vktest.c -lvulkan && /tmp/vktest"
# 期望复现：apiVersion=0x400000 -> VkResult=-9
```

noble 镜像复现：烧 `Orangepizero3w_1.0.2_ubuntu_nole...img` → 上电自动登录 → `vulkaninfo` → Devices 空。

## 五、需要厂商回答的问题

1. `libVK_IMG 24.2.6603887` 的设备支持表是否包含 **BVNC 36.56.104.183**（A733/T527 BXM-4-64）？若有，Connect 应答需要满足什么条件？
2. Connect 应答中 `CapabilityFlags=0x00020000`（bit17，超出我们内核树 device_connection.h 定义范围）是什么含义？ICD 对它有何检查？
3. `GetMultiCoreInfo` 返回 `NumCores=1`（RGX_CR_MULTICORE_SYSTEM 寄存器直读）是否符合 BXM-4-64 预期？ICD 是否要求特定核数/拓扑？
4. Ubuntu noble 镜像带 vkcube + libVK_IMG，Debian bookworm 镜像的 icd.d 指向不存在的库——**Vulkan 在 A733 BSP 中是否为已完成交付项**？
5. 是否存在已修复此问题的新版 DDK 用户态（对应哪个内核模块版本）？

## 六、工具与补丁（仓库内）

- `tina-zero3w/tests/vktest.c` —— 最小复现探针（多 API 版本 + 设备枚举打印）
- `tina-zero3w/tests/ioctl_shim.c` —— bridge 协议全量解码 LD_PRELOAD
- `userpatches/kernel/sun60iw2-current/0020/0021/0022` —— 内核侧诊断日志补丁
- `tina-zero3w/docs/PITFALLS.md` —— 全部实验记录
- 核对基准：Radxa Cubie A7A（同 SoC 同 BVNC 同 DDK）社区栈 github.com/ayiejosh/a733-powervr-fex 报告 DXVK/Zink 可用——其内核为 Radxa BSP 非本板环境，差异未定位
