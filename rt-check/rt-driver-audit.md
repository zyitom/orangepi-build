# sun60iw2 BSP 驱动 RT(PREEMPT_RT) 内核兼容性审计

日期：2026-09-16　内核树：`kernel/orange-pi-6.6-sun60iw2`（6.6.98，`CONFIG_AW_BSP=y`，当前 `CONFIG_PREEMPT=y` 未打 RT 补丁）

## 一、结论

1. **补丁层面**：`patch-6.6.97-rt57` 与 `patch-6.6.99-rt58` 对当前 6.6.98 树 dry-run 全部通过（163 个文件，无 FAILED/reject；`rt-check/` 里两份日志 byte 级相同）。本次用真实树重新验证了 rt58，结果一致。
2. **驱动层面**：对当前配置**启用**的厂商驱动（bsp/ 下 5097 个 .c 静态扫描 + 人工核验），**未发现 RT 内核下的致命模式**（硬中断上下文持有可睡眠锁等）。发现的问题全部集中在**未启用**的驱动里（mali、xr819/xr829、atbm、loopback 声卡、gt9xx）。
3. **厂商已有 RT 适配痕迹**：`sunxi-uart-ng` 内置 `CONFIG_PREEMPT_RT` 分支（给 IRQ 线程设 SCHED_FIFO 优先级，DTS 属性 `irq-priority-for-rt`）；`sunxi-dump2pc` 也有 RT 分支。说明 Allwinner 对该 BSP 做过部分 RT 工作。
4. **编译级验证已实测通过**（见第六节）：真打 rt58 补丁 + `CONFIG_PREEMPT_RT=y` 全量构建成功，内核 + 全部 956 个模块 + img-bxm GPU 模块 0 错误，vmlinux 版本串 `Linux version 6.6.98-rt58+ ... # SMP PREEMPT_RT`。剩余未验证项：实机运行时验证（cyclictest、外设功能）。

## 二、扫描范围与方法

范围：`bsp/drivers`、`bsp/modules`（含 GPU 模块）、`bsp/platform`，以及 `drivers/soc/sunxi`、`drivers/clk/sunxi-ng`、`sound/soc/sunxi`、`drivers/media/platform/sunxi`。

在 6.6-rt 上 `spinlock_t` 变为可睡眠锁，真正致命的只有四类模式（脚本 `rt_scan.py` 按"函数级上下文"检测）：

| 类别 | 模式 |
|---|---|
| irq_chip 回调（RT 上仍跑硬中断） | 使用非 raw `spin_lock` 或睡眠调用 |
| `IRQF_NO_THREAD`/`IRQF_PERCPU`/chained handler（硬中断） | 同上 |
| hrtimer 回调（硬中断） | 同上 |
| `raw_spin_lock` / `preempt_disable` / `local_irq_save` 区域 | 内部使用非 raw `spin_lock` 或睡眠调用 |

## 三、已启用驱动的核验结果

| 驱动 | 结果 | 依据 |
|---|---|---|
| pinctrl-sunxi（GPIO 链式中断） | ✅ 安全 | chip 回调（ack/mask/unmask/set_type）全部 `raw_spin_lock(&pctl->lock)`；非 raw 的 `sun55iw3_pinctrl_lock` 只用于 pinconf/io-bias 进程上下文 |
| irq-sun8i-nmi（NMI 控制器） | ✅ 安全 | generic irq chip（raw 锁）+ 标准链式 demux |
| rtc sunxi_timer_alarm（`IRQF_NO_THREAD`） | ✅ 安全 | handler 只 `writel`+`pr_debug`，无锁 |
| GPU img-bxm（`AW_GPU_TYPE="bxm"`，compilation.sh 对 sun60iw2 无条件编译 `bsp/modules/gpu`） | ✅ 安全 | 无 hrtimer；所有 `request_irq` 无 `IRQF_NO_THREAD` → RT 上 IRQ 线程化，LISR 持 spinlock_t 合法 |
| AIC8800 WiFi/BT（=m，唯一启用的厂商无线） | ✅ 安全 | tasklet（RT 上线程化）+ 线程化 IRQ；源码无 hrtimer |
| sunxi-uart-ng | ✅ 有官方 RT 适配 | `#ifdef CONFIG_PREEMPT_RT`：为 IRQ 线程设实时优先级 |
| ve / g2d / di / vin / npu(AW_NNA_VIP) / dmc-devfreq / stmmac 等大模块（=m） | ✅ 无危险模式 | 无 NO_THREAD/PERCPU/hrtimer + 非 raw 锁组合 |
| vendor_hooks（android vh：mm/dmabuf/sched/usb） | ✅ 与 RT 补丁无文本冲突 | dry-run 已覆盖其挂接的核心文件 |
| standby / timer / watchdog | ✅ 干净 | 无自旋锁/关中断+睡眠模式 |

## 四、未启用驱动中的 RT 隐患（将来启用前必须先修）

| 位置 | 问题 | 状态 |
|---|---|---|
| `bsp/modules/gpu/mali-valhall|mali-bifrost .../mali_kbase_js_backend.c:105`、`mali_kbase_pm_metrics.c` | hrtimer 回调里 `spin_lock_irqsave(&kbdev->hwaccess_lock)` → RT 上必触发 sleeping-in-atomic | `AW_GPU_TYPE=bxm` 时不编译 |
| `mali-valhall .../mali_kbase_mem_migrate.c:347` `kbase_page_isolate()` | raw/抢占禁用区内 `spin_trylock`（非 raw） | 同上 |
| `bsp/drivers/sound/misc/snd_sunxi_loopback.c:135,213` | 两个 hrtimer 回调里 `spin_lock_irqsave(&buffer_lock)` | `CONFIG_SND_SOC_SUNXI_LOOPBACK` 未开 |
| `bsp/drivers/net/wireless/xr819|xr829|atbm6023is`（queue gc、mesh_plink hrtimer） | hrtimer 回调持非 raw 锁 | 均未启用 |
| `bsp/drivers/input/ctp/gt9xxnew/gt9xx_update.c:2524` `gup_output_pulse()` | `local_irq_save` 区间内 `msleep` —— **不开 RT 也是 bug**（固件升级路径，唤醒定时器无法触发） | 未启用 |
| `bsp/drivers/net/wireless/uwe5622`（`IRQF_PERCPU\|IRQF_NO_THREAD` 的 pcie MSI handler） | 硬中断 handler，若启用需审查 `msi_irq_handle`/`legacy_irq_handle` | 未启用 |

## 五、建议的下一步

1. ~~编译级验证~~ **已完成，见第六节**。
2. **运行时验证**：cyclictest（`-m -p95`）、GPU 满载 + 网络压测下看最大延迟；串口 console（uart-ng）用 `irq-priority-for-rt` 提优先级。
3. 若以后切 Mali GPU：先按第四节修 `hwaccess_lock`（改 raw 或把 dvfs/jit 定时器移到 kthread/workqueue）。

## 六、编译级验证（2026-09-16 实测通过）

原内核树 root 属主不可写，测试在**复制树**上进行，原树未动：

- 测试树：`/home/helios/rt-kernel-test/linux-6.6.98-sun60iw2-rt`（含编译产物，约 6GB，可删除或直接用于烧写测试）
- 工具链：`toolchains/gcc-arm-11.2-2022.02-x86_64-aarch64-none-linux-gnu`

| 步骤 | 结果 |
|---|---|
| `patch -p1 < rt-check/patch-6.6.99-rt58.patch` | ✅ 163 文件，0 reject / 0 fuzz / 0 offset |
| `scripts/config --enable PREEMPT_RT` + `make ARCH=arm64 CROSS_COMPILE=… olddefconfig` | ✅ config delta 仅 RT 预期项：`PREEMPT→PREEMPT_RT`、`RCU_BOOST=y`、`ARCH_SUPPORTS_RT=y`、去掉 `QUEUED_RWLOCKS`/`SOFTIRQ_ON_OWN_STACK` 等 |
| `make ARCH=arm64 CROSS_COMPILE=… -j6 Image modules` | ✅ exit 0，3620 个编译单元（含 259 个 bsp/ 厂商文件），产出 956 个 .ko，**0 error**；仅 6 条告警且全是厂商代码原有的风格问题（缩进/未用变量），与 RT 无关 |
| GPU 模块 `make -C bsp/modules/gpu LICHEE_KERN_DIR=<测试树> …`（img-bxm，`AW_GPU_TYPE="bxm"`） | ✅ exit 0，`pvrsrvkm.ko` 编出 |
| 关键厂商模块抽查 | ✅ aic8800_fdrv/btlpm/bsp、vin_io/vin_v4l2、g2d、di、sunxi-ve、vipcore(NPU)、sunxi-stmmac、sun55iw3-devfreq 等全部产出 |
| vmlinux 版本串 | `Linux version 6.6.98-rt58+ (…aarch64-none-linux-gnu-gcc 11.2…) # SMP PREEMPT_RT` |
| **自检内核变体**（同一棵测试树追加 `DEBUG_ATOMIC_SLEEP` / `PROVE_LOCKING`+`PROVE_RAW_LOCK_NESTING` / `DEBUG_PREEMPT` / `DEBUG_OBJECTS_TIMERS` / `WQ_WATCHDOG` / 全套 ftrace 延迟跟踪器后重编） | ✅ exit 0，0 error，Image 34M。上板跑负载矩阵即可自动暴露"自旋锁类"运行时问题，操作见 `rt-check/rt-runtime-guide.md` |

注意事项：

1. 补丁基线是 6.6.99、树是 6.6.98——文本层面零冲突，但应用后内核为 `6.6.98-rt58`（不含 6.6.98→6.6.99 之间的少量上游修复）。正式启用建议二选一：把树升到 6.6.99 再套 rt58，或先降 sublevel 用 6.6.97-rt57。
2. `olddefconfig` 必须带 `ARCH=arm64 CROSS_COMPILE=…`，否则 Kconfig 会探测主机 x86 gcc，把工具链相关的 config 项写坏。
3. 正式启用方式（在原构建流程里）：给 `external/config/kernel/linux-sun60iw2-current.config` 加 `CONFIG_PREEMPT_RT=y`，并在 kernel 打补丁步骤套用 `rt-check/patch-6.6.99-rt58.patch`（可用 userpatches 或直接改 `compilation.sh` 的补丁序列）。
4. 复现命令：

```bash
TC=/home/helios/Desktop/orangepi-build/toolchains/gcc-arm-11.2-2022.02-x86_64-aarch64-none-linux-gnu/bin
cp -r <kernel tree> /home/helios/rt-kernel-test/linux-6.6.98-sun60iw2-rt
cd /home/helios/rt-kernel-test/linux-6.6.98-sun60iw2-rt
patch -p1 --batch < /home/helios/Desktop/orangepi-build/rt-check/patch-6.6.99-rt58.patch
./scripts/config --enable PREEMPT_RT
make ARCH=arm64 CROSS_COMPILE=$TC/aarch64-none-linux-gnu- olddefconfig
make ARCH=arm64 CROSS_COMPILE=$TC/aarch64-none-linux-gnu- -j6 Image modules
make -C bsp/modules/gpu LICHEE_TOOLCHAIN_PATH=$TC LICHEE_CROSS_COMPILER=aarch64-none-linux-gnu- \
  LICHEE_PLATFORM=linux LICHEE_MOD_DIR=/home/helios/rt-kernel-test/gpu_modules \
  LICHEE_KERN_DIR=$PWD CROSS_COMPILE=$TC/aarch64-none-linux-gnu- ARCH=arm64
```

## 七、deb 打包（bindeb-pkg，对齐官方流水线）

按 `compilation.sh` 同款方式打包（`make bindeb-pkg KDEB_PKGVERSION=1.0.0 KDEB_COMPRESS=xz KBUILD_DEBARCH=arm64 BRANCH=current ARCH=arm64 LOCALVERSION=-sun60iw2`），产物在 `/home/helios/rt-kernel-test/`：

| 包 | 大小 | 内容 |
|---|---|---|
| `linux-image-current-sun60iw2_1.0.0_arm64.deb` | 12M | `/boot/vmlinux-6.6.98-rt58-sun60iw2`（=Image）、System.map、config、318 个 .ko |
| `linux-dtb-current-sun60iw2_1.0.0_arm64.deb` | 98K | 99 个 .dtb/.dtbo（含 overlay） |
| `linux-headers-current-sun60iw2_1.0.0_arm64.deb` | 13M | 板上编外部模块用（含 Module.symvers） |
| `linux-image-current-sun60iw2-dbg_1.0.0_arm64.deb` | 123M | vmlinux 调试符号，验证用可装可不装 |

一致性已验证：release 字符串 `6.6.98-rt58-sun60iw2`（无 git "+" 后缀）；`/lib/modules/` 目录名与 vermagic（`6.6.98-rt58-sun60iw2 SMP preempt_rt mod_unload aarch64`）与内核三方一致。GPU 模块不随 deb 打包（与官方流程一致，官方用 `LICHEE_MOD_DIR` 单独投放）：`pvrsrvkm.ko` 在 `…/img-bxm/linux/rogue_km/binary_sunxi_linux_nulldrmws_release/target_aarch64/kbuild/`，vermagic 已对齐，装完拷到 `/lib/modules/6.6.98-rt58-sun60iw2/kernel/` 下再 `depmod -a`。

版本串细节（踩过的坑）：树里 `localversion-rt`（`-rt58`）永远排在 `CONFIG_LOCALVERSION` 之后；"+" 后缀来自 `setlocalversion`——`CONFIG_LOCALVERSION_AUTO` 关闭时若环境变量 `LOCALVERSION` 未设置就会补 "+"，所以**编译和打包都必须带 `LOCALVERSION=-sun60iw2`**（官方流水线正是如此）。当前这套 deb 是**自检变体**（含 lockdep/DEBUG_ATOMIC_SLEEP 等），用于上板验证；验证通过后把第六节列的 debug 选项关掉重跑 `make Image modules` + `bindeb-pkg` 即得正式版。

## 审计产物

- `rt_scan.py` — 可重跑的扫描脚本（函数级上下文检测）
- `rt_scan_findings.json` / `rt_scan_output.txt` — 原始结果（17 条，全部人工核验过）
- `/tmp/rt-apply.log`、`/tmp/rt-build.log`、`/tmp/rt-gpu-build.log` — 编译级测试日志
- `/home/helios/rt-kernel-test/linux-6.6.98-sun60iw2-rt` — 已打补丁的 RT 测试树（自检变体）
- `/home/helios/rt-kernel-test/*.deb` — 可直接 `dpkg -i` 的 RT 内核 deb 包（image/dtb/headers/dbg）
