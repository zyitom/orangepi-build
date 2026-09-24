# 系统构建可开启的优化项（基于真实配置审计）

> 审计对象：`kernel/orange-pi-6.6-sun60iw2/.config`（8686 行），内核 **6.6.98**，
> Image 27,093,000 B，近 2 小时产出 321 个 `.ko`。
> 原则：本工程坚持**"按需开"而非"能开就开"**（已有 5 个真实反例），下列按"收益/代价/是否需要补丁"分级。

---

## ⚠️ Tier 0：先纠一条错误记录 —— 当前内核**完全非抢占**

```text
CONFIG_PREEMPT_NONE_BUILD=y
CONFIG_PREEMPT_NONE=y          ← 实际值
```

**不是** `PREEMPT_RT`，也**不是** `PREEMPT`。此前会话记录的"PREEMPT_RT=y 已落实"
**是错的**，本次实测配置否认了它。另：`kernel/Makefile` 中无 `-rt`，**RT 补丁从未打入**。

**影响**：`PREEMPT_NONE` 意味着内核态长路径（syscall、中断下半部、驱动循环）可以不被抢占，
最坏调度/中断延迟可达**毫秒级**。对"硬件中断 + 高精度时间戳"场景，这是**最大的单点误差源**。

**重要限定**：BMI088 的采集在小核 E902（裸机）上做，**小核侧延迟是确定性的**（纯硬件，无 OS）。
所以抢占模型只影响**大核侧**——即消费共享 SRAM 的内核驱动、以及任何 ARM 侧中断处理。
仍然值得做，但不要指望它改善小核的采样抖动。

---

## Tier 1：建议开（收益大、风险可控）

### 1.1 抢占模型 —— 先上"无需打补丁"的档

| 方案 | 成本 | 配置 |
|---|---|---|
| **A. `PREEMPT_DYNAMIC`（推荐先做）** | **零补丁**，主线已有 | `CONFIG_PREEMPT_DYNAMIC=y` + 启动参数 `preempt=full` |
| B. `PREEMPT`（全抢占） | 零补丁，编译期固定 | `CONFIG_PREEMPT=y` |
| C. `PREEMPT_RT` | **需先打 `6.6.98-rt*` 补丁** + 评估厂商 BSP 兼容性 | `CONFIG_PREEMPT_RT=y` |

选 A 的理由：无需动源码树，既能立刻拿到全抢占，又保留"启动时切换"的灵活性，
出问题可以退回去，风险最低。若实测仍不够，再上 C。

### 1.2 `PSTORE` + ramoops —— 跨复位保留日志

```text
CONFIG_PSTORE=y
CONFIG_PSTORE_RAM=y
CONFIG_PSTORE_CONSOLE=y
CONFIG_PSTORE_DEFLATE_COMPRESS=y     # 可选，压缩 ramoops
（+ DTS 里加 reserved-memory 节点给 ramoops）
```

现状：`fs/pstore/ram.c`、`ram_core.c` **源码都在**，`CONFIG_OF_RESERVED_MEM=y` 已开，
但 `CONFIG_PSTORE` 未开 → **现在完全没有**。

**为什么对本工程是刚需**：我们此前排查 bootloop 全靠串口抓（还依赖 TL101 的 CH340），
一旦板子在外面、或串口没接，黑箱就再也看不到。ramoops 能把**上一次启动的 console**
留在内存里，重启后从 `/sys/fs/pstore/` 读出来——这正好解决本项目反复遇到的排障困境。

代价：需要预留一小块物理内存（几 MB 量级），显存/CMA 若紧张需权衡。

### 1.3 ftrace —— 量中断延迟

```text
CONFIG_FTRACE=y
CONFIG_FUNCTION_TRACER=y
CONFIG_IRQSOFF_TRACER=y       # 最大关中断时长
CONFIG_PREEMPT_TRACER=y       # 最大关抢占时长（若上 PREEMPT）
CONFIG_SCHED_TRACER=y         # 最长唤醒延迟
```

现状：`kernel/trace/` 源码在、`CONFIG_TRACING_SUPPORT=y`，但 `CONFIG_FTRACE` 未开 →
**现在没有任何 tracer**。未启用时几乎零开销，是拿"中断延迟到底是多少"的唯一硬手段。

---

## Tier 2：运行时即可（**无需重编**）

| 项 | 做法 | 目的 |
|---|---|---|
| 禁深度 idle | 启动参数 `cpuidle.off=1` | 消掉 idle 唤醒延迟（`ARM_PSCI_CPUIDLE=y` 已开） |
| 固定高频 | `echo performance > /sys/devices/system/cpu/cpufreq/policy*/scaling_governor` | 消调频导致的延迟抖动（当前默认 `schedutil`） |
| 关 slab 调试 | 启动参数 `slub_debug=-` | `CONFIG_SLUB_DEBUG=y` 有运行时开销 |

这三项**不需要重新编译**，建议先加进 bootargs 实测，再决定是否固化为默认配置。

---

## Tier 3：体积 / 构建时间

| 项 | 现状 | 建议 |
|---|---|---|
| `CONFIG_DEBUG_INFO` | **=y**（Image 27 MB，321 个 .ko 全带调试信息） | 交付/量产镜像改 `CONFIG_DEBUG_INFO_NONE=y`：体积与构建时间大幅下降。**开发期建议保留**（崩溃分析要用）。 |
| 内核压缩 | 配置里**找不到** `KERNEL_GZIP/ZSTD/LZ4` 任何一项 | **待确认**：压缩算法实际值未落盘，需查 `arch/arm64/Kconfig` 的 choice 是否被厂商改写。若可换 zstd/lz4，可缩短解压时间。 |
| `CONFIG_TRIM_UNUSED_KSYMS` | 未开 | 可缩体积，但**会破坏 out-of-tree 模块**。我们刚加的 `amp_timestamp` 是内置的没问题，但以后若要编外部模块就麻烦——**建议暂不开**。 |

---

## Tier 4：建议关掉（省开销）

| 项 | 现状 | 说明 |
|---|---|---|
| `CONFIG_SECURITY_SELINUX` | **=y**（还带 `SELINUX_DEVELOP=y`） | 开发板不用 SELinux 策略的话是纯开销 |
| `CONFIG_SECURITY_APPARMOR` | **=y** | 同上；两套 LSM 同时启用开销更明显 |
| `CONFIG_SLUB_DEBUG` | =y | 编进内核的部分可关（或运行时 `slub_debug=-`） |

---

## Tier 5：保留，别动（都是排障刚需，开销可忽略）

| 项 | 现状 | 为什么留 |
|---|---|---|
| `CONFIG_FRAME_POINTER` | =y | 栈回溯可用，代价很小 |
| `CONFIG_STACKPROTECTOR_PER_TASK` | =y | 栈溢出检测 |
| `CONFIG_SCHED_DEBUG` | =y | `/proc/sched_debug` |
| `CONFIG_MAGIC_SYSRQ` | =y | SysRq 救命键 |
| `CONFIG_IKCONFIG_PROC` | =y | `/proc/config.gz` 可自证配置 |
| `CONFIG_KALLSYMS_ALL` | =y | 符号完整 |
| `CONFIG_IRQ_FORCED_THREADING` | =y | RT 前置，已对 |
| `CONFIG_STRICT_DEVMEM` | **未开** ✅ | `/dev/mem` 访问共享 SRAM 必需 |
| `CONFIG_MODULE_SIG` | **未开** ✅ | 我们能自由加载自编模块 |
| `CONFIG_DEBUG_FS` | =y | 各种调试接口 |
| `CONFIG_HWSPINLOCK` / `CONFIG_AW_MSGBOX` | =y | 跨核通信前置 |

---

## Tier 6：明确**不要**开

`class_create(THIS_MODULE, …)` 这棵厂商树里散落 **20+ 处**（smartcard、spinand、
rawnand、bcmhd/uwe5622 wifi、NPU、dram_info、ctp…）。**任何**这些驱动被打开都会编译失败。
若确需其中之一，必须先做同样的 API 移植（本轮已为 rpmsg 处理）。

另：`AW_RPMSG_*` / `AW_RPBUF*` 目前保持 `n`（理由见 `E902-COMM-VS-RPMSG.md`）。

---

## 建议执行顺序

1. **先做零风险的**：`PREEMPT_DYNAMIC=y` + bootargs(`preempt=full cpuidle.off=1 slub_debug=-`) + governor=performance → 重编一次，实测中断延迟。
2. **再加可观测性**：`PSTORE*`(+DTS ramoops) + `FTRACE/IRQSOFF_TRACER` → 以后排障不再依赖串口。
3. **视实测结果决定是否上 RT 补丁**（Tier 1.1-C）。
4. 交付镜像再考虑 `DEBUG_INFO_NONE` 与关闭 SELinux/AppArmor。

---

## 附：本轮审计原始数据（关键行）

```text
CONFIG_PREEMPT_NONE=y                       ← 非抢占
CONFIG_HZ=250 / CONFIG_HZ_250=y
CONFIG_NO_HZ_IDLE=y                          ← ✅ 无滴答（idle）
CONFIG_HIGH_RES_TIMERS=y                     ← ✅ 高精度定时器
CONFIG_IRQ_FORCED_THREADING=y                ← ✅
CONFIG_CPU_FREQ=y / GOV_SCHEDUTIL(默认) / GOV_PERFORMANCE=y
CONFIG_CPU_IDLE=y / ARM_PSCI_CPUIDLE=y / CPU_IDLE_GOV_MENU=y
CONFIG_DEBUG_FS=y / KALLSYMS_ALL=y / MAGIC_SYSRQ=y / IKCONFIG_PROC=y
CONFIG_FTRACE 未开 / 无任何 tracer
CONFIG_PSTORE 未开（但 fs/pstore/ram.c 源码在，OF_RESERVED_MEM=y）
CONFIG_DEBUG_INFO=y（DEBUG_INFO_NONE 未开）→ Image 27,093,000 B
CONFIG_LTO_NONE=y / CC_OPTIMIZE_FOR_PERFORMANCE=y
CONFIG_ZRAM=y ✅ / ZSWAP 未开 / CMA_SIZE_MBYTES=16
CONFIG_SECURITY_SELINUX=y / APPARMOR=y / AUDIT=y
CONFIG_MODULE_SIG 未开 ✅ / SECURITY_LOCKDOWN_LSM 未开 ✅ / STRICT_DEVMEM 未开 ✅
CONFIG_SLUB_DEBUG=y / SCHED_DEBUG=y / FRAME_POINTER=y
CONFIG_PROVE_LOCKING 未开 ✅ / KASAN 未开 ✅ / LOCKDEP 未开 ✅
```
