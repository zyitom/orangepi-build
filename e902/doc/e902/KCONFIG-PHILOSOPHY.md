# 内核配置不是"能开就开" —— 判断标准与本工程的反例

日期：2026-09-22 · 全部反例都来自本项目实际踩过的坑

---

## 一、先纠正那个常见误解

**`CONFIG_xxx=y` 不等于"更好"。** 每个选项都有代价：

| 形态 | 代价 |
|---|---|
| `=y`（编进内核） | **永久占内存**（代码 + 数据结构）、内核镜像变大、启动变慢 |
| `=m`（模块） | 不占内存直到加载，但占 rootfs、多一次加载动作 |
| 任意 | **编译时间变长**；驱动 probe 会跑代码 → **可能与别的模块抢资源** |

**我们这板子还跑着 `PREEMPT_RT`，更要克制**：RT 追求的是「**最坏情况延迟可控**」。
多一个驱动、多一个中断源、多一个内核线程，就多一个**抖动源**。
工业上的做法恰恰相反 —— **把配置压到最小**。

---

## 二、本项目实际踩过的"开了就出事"（这是最好的论据）

| 选项 | 开了会怎样 | 证据 |
|---|---|---|
| `CONFIG_AW_TRNG` | **编译不过** —— `bsp/drivers/ce/sunxi_trng/` **只有 .c 没有 .h**（头文件缺失） | `HANDOFF-RT-IMAGE.md:51` |
| `CONFIG_AW_LRADC` / `AW_GPADC` | **编译不过** —— 驱动里用了 **6.6 已删除的 `iio_dev->mlock`**（`sunxi-lradc.c:753,763`） | 同上 |
| `CONFIG_AW_RPMSG_PERF_TRACE` | **编译不过** —— 它 `select AW_AMP_TIMESTAMP`，而该符号**在这棵树里没有 Kconfig 定义**、`include/linux/amp_timestamp.h` **文件不存在** | 本轮核实 |
| `CONFIG_PREEMPT` **+** `CONFIG_PREEMPT_RT` | **RT 被顶掉** —— 两者同属一个 kconfig `choice`，同时写会取 `PREEMPT`、丢掉 `RT`（版本串还显示 rt58，是"装饰"） | `HANDOFF-RT-IMAGE.md:13-15` |
| **`CONFIG_STRICT_DEVMEM=y`** | **`/dev/mem` 只能访问 PCI 区** → 我们的 `awdevmem.py` 全废、读不了寄存器/SRAM | `DESIGN-NOTES.md:119` |

**⇒ 最后一行尤其说明问题：对我们来说 `STRICT_DEVMEM` 是「必须关」而不是「必须开」。**

---

## 三、那正确的判断标准是什么？

> **不是"这个选项存不存在"，而是"这个选项对应的硬件/功能我确实要用吗？"**

- **要用** → 开（**优先 `=m`**：不用就别加载）
- **不用** → 关掉（省内存、少抖动源、启动快、编译快）

---

## 四、按这个标准，本工程的逐项结论

| 选项 | 结论 | 理由 |
|---|---|---|
| `CONFIG_PREEMPT_RT` | ✅ **开** | 你要实时性（且必须确认 `CONFIG_PREEMPT` 不在） |
| `CONFIG_AW_MSGBOX` | ✅ 开 | 小核通信的物理通道 |
| `CONFIG_AW_HWSPINLOCK` | ✅ 开 | 共享内存互斥 |
| `CONFIG_AW_SPI` / `SPI_SPIDEV` | ✅ 开 | **BMI088 要用** |
| `CONFIG_IIO` 系列 | ✅ 开 | **IMU 走 IIO 框架**（工业标配） |
| `CONFIG_STRICT_DEVMEM` | ❌ **必须关** | 否则 `/dev/mem` 废掉 |
| `CONFIG_AW_DMC_DEVFREQ` | ⚠️ **建议关**（现为 `=m`） | 避免每 100 ms 一次 SMC 失败 + 400 ms 超时卡顿（`DESIGN-NOTES.md:533`） |
| `CONFIG_AW_RPMSG_*` / `AW_RPBUF` | ⚠️ 本轮开了，但**目前 probe 不到东西** | 备着；等真写 rpmsg 客户端再起作用 |
| `CONFIG_SUN6I_MSGBOX` | ⚠️ 可关 | compatible 不匹配 A733，纯占位 |
| `CONFIG_AW_TRNG` / `LRADC` / `GPADC` | ❌ 关（现状就是关） | 开着编译不过 |
| `CONFIG_AW_REMOTEPROC` | ❌ 关（现状） | 无 rproc 节点 |

---

## 五、一句话

> **内核配置的目标不是"功能最多"，而是"刚好够用且可预测"。**
> 对本工程：**该开的一个不少（RT / msgbox / SPI / IIO），该关的一个不留（TRNG / LRADC / STRICT_DEVMEM）**，
> 而"看着有用但暂时用不到"的（rpmsg/rpbuf），**先备着、别指望它自动生效**。
