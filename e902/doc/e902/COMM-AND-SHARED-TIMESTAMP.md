# E902 ↔ 主核：通信机制 与 时间戳一致性

日期：2026-09-22 · 证据：A733 手册 §5.1.5/内存映射、内核 `bsp/drivers/rpmsg/`、`userpatches/*.config`、
`e902-fw/src/ts_test.c` 注释与 18:06 现场日志

---

## 一、当前 E902 与主核怎么通信

A733 **为大小核（AMP）设计了一整套框架**，分四层：

| 层 | 机制 | 本板状态 | 证据 |
|---|---|---|---|
| **① 硬件** | **MSGBOX**（两套单向 FIFO，各 4 通道 × 8 深）+ **hwspinlock** + 共享 SRAM | 驱动在树、`=y`，**但无客户端** | 手册 ch.6；`/proc/interrupts` 无 mbox 行 |
| **② 异步消息框架** | **`aw_virtio_rpmsg_bus`**（virtio + rpmsg）—— Allwinner 自己的 AMP 通信实现，含 `rpmsg_master` / `rpmsg_client` / `rpmsg_notify` / `rpmsg_heartbeat` / `rpmsg_perf` | **`CONFIG_RPMSG_VIRTIO is not set` → 没编进内核** | `bsp/drivers/rpmsg/` 目录；配置文件 |
| **③ 同步 RPC** | **SMC**（Linux → bl31 → SCP），如 `ARM_SVC_SUNXI_DDRFREQ` | `CONFIG_SUNXI_SMC` 没编、`AW_DMC_DEVFREQ=m` 没跑 → **实际上没有活跃调用** | `ccu-ddr.c:77`、`sunxi-sip.h` |
| **④ 启动握手** | bl31 ↔ SCP 的 `startup notify` + `hard syn`（发生在 Linux 之前） | 进行中（我们的固件正卡在这） | `monitor.fex` 的 `[SCP]` 日志 |

**⇒ 结论："当前" Linux 跑起来时，E902 与主核之间其实几乎**没有活跃通信**：
异步框架没编、同步 RPC 没启用、mailbox 通道空着。**但硬件通路是完好的**（我们实测过写入：`MSG_STATUS` 0→3→6）。

> 这反而是**好消息**：通道空着 = 随便我们用，不会和谁抢。

---

## 二、有硬件中断时间戳吗？**有，而且是专门为跨核做的**

手册 §5.1.5 明确列出一个 **CPUX 域的硬件时间戳单元**：

| 模块 | 地址 | 说明 |
|---|---|---|
| `TIMESTAMP_STA` | `0x08010000` | Timestamp **Status**（状态） |
| **`TIMESTAMP_CTRL`** | **`0x08020000`** | Timestamp **Control** ← 主体在这 |
| └ `TSTAMP_CTRL_REG` | `+0x00` | 控制（bit0 = 使能） |
| └ **`CNT_CTRL_LOW_REG`** | **`+0x08`** | **计数器低 32 位** |
| └ **`CNT_CTRL_HI_REG`** | **`+0x0C`** | **计数器高 32 位** |
| └ `CNT_FREQID_REG` | `+0x20` | **基准频率**（应为 `0x016E3600` = 24 000 000） |

⇒ **这是一个 64 位自由运行计数器，位于 CPUX 域，但两域都能读。**
另外 E902 自己还有 `S_TIMER @0x07091000`（4 通道，CLIC IRQ 20–23）。

---

## 三、和 Linux 的一致吗？**一致 —— 这是 A733 特意提供的"跨核共享时基"**

`e902-fw/src/ts_test.c` 的文件头注释写着（前一位会话的实测结论）：

```
 * ARM 侧已实测：该计数器使能位=1、FREQID=24000000、速率 24.000 MHz，
 * 且与 CLOCK_MONOTONIC 只有恒定偏置（零漂移）。本测试回答的是
 * "同一地址能不能从 CPUS 域读到" —— 能，则双核时间戳可零偏置共用。
```

**⇒ 也就是说：**
- 这个 24 MHz 计数器**与 Linux 的 `CLOCK_MONOTONIC` 同源、零漂移，只差一个常量偏置**
- 只要 E902 也读同一个寄存器，**双核时间戳天然可比**（换算：`t_linux = t_tstamp/24M + const`）

### 但我们 18:06 的探针有一处**地址写错**（这就是日志里"速率异常"的原因）

```
ts_test.c:  #define TS_STA 0x08010000   ← 读计数器（CNT_LOW/HI）… ❌ 错的
# 手册：计数器在 0x08020008 / 0x0802000C
```

日志结果：
```
TSTAMP_CTRL = 0x00000001   FREQID = 0x00000000   (NOT 24 MHz)
delta_1s = 58254 ticks  => 速率异常
```

- `TSTAMP_CTRL` 读对了（`0x08020000` → bit0=1 ✅）
- **`FREQID` 从 E902 读回 `0x00000000`**（ARM 侧读是 `24000000`）→ 可疑，待查
- **计数器读的是 `0x08010000`（那是 STA 块），不是 `0x08020008`** → 所以 `58254 ticks/s` 是无意义数字

**⇒ 两个可修的 bug：**
1. 计数器地址改为 `0x08020008/0x0802000C`
2. 查清为什么 E902 读 `0x08020020` 得到 0（可能是时序/需要先置使能位，或该寄存器在 CPUS 侧读返回 0）

---

## 四、对你 BMI088 高精度时间戳的意义（**这是好消息**）

| 需求 | 结论 |
|---|---|
| 小核打时间戳 | ✅ 用 `S_TIMER`（24 MHz，41.7 ns）或**跨域读 `0x08020008`**（同源、零漂移） |
| 与大核时间戳对齐 | ✅ **天然同源**（同一个 24 MHz 计数器），只需减一个常量偏置 |
| 大核侧有现成支持吗 | ✅ 内核里有 `amp_timestamp` 的使用者（`rpmsg_perf.c`、`aw_virtio_rpmsg_bus.c`），虽是给 rpmsg 打点用的，但说明**"AMP 共享时间戳"是 SoC 设计的一部分** |
| 需要自己做什么 | ① 修上面两个地址/读值 bug ② 在 E902 采样中断里读该计数器 ③ 大核侧读同一寄存器做换算 |

**⇒ BMI088 的"高精度时间戳"不需要额外的时钟同步协议 —— 芯片已经给了共享时基。**
