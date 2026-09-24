# 本轮修复与内核选项处理（2026-09-22 晚）

## 一、先回答：IMU 解算要精准时间戳吗？—— **要，而且不止"准"**

姿态解算本质是**积分**：角度 = Σ(角速度 × Δt)。所以 **Δt 的误差直接变成角度误差**。

| 采样率 | 周期 | 若 Δt 抖动 1% | 若抖动 10% |
|---|---|---|---|
| 1 kHz | 1 ms | 10 µs | 100 µs |
| 2 kHz | 500 µs | 5 µs | 50 µs |

- 抖动不走零（有偏）时，会**累积成漂移**；随机抖动则表现为噪声抬升
- 所以通常要求 **Δt 抖动 < 周期的 1%**

**但还有第二个、更容易被忽略的要求：时间戳必须"可比"。**
小核打的时间戳要和大核（滤波/发布）的时间戳对得上，否则融合出来是错的。
两条路：
1. **同源**：两边读**同一个硬件计数器**（A733 的 `TIMESTAMP_STA 0x08010000` / Linux `CLOCK_MONOTONIC` 同源，只差常量偏置）← **推荐**
2. **换算**：不同源就必须做偏移+漂移拟合（还得定期重标定）

**⇒ 结论：A733 已经给了"共享时基"，所以这一块不用额外协议。**

---

## 二、本轮实际修了什么

### 修复 1：`msgbox.c` —— **逐字 pacing**（真正导致握手失败的原因）

厂商发送函数**每个字之前**都等一次"FIFO 不满"（`scp.fex 0x40007240/4e/aa`），
而我们只在开头等一次，然后连发 15 个字（1 头 + 1 计数 + 13 数据）——
**FIFO 只有 8 深，必然溢出**，`msgbox_try_send()` 中途返回 -1 → `startup feedback : FAILED`。

已改为：**每一个字（含头字与计数字）发送前都调一次 `msgbox_wait_tx_empty()`**。

> 注：`msgbox_wait_tx_empty()` 的判据（`!= 8` 才算可发）先前已被另一轮修正为正确版本，
> 并附了更准确的解释（FIFO 里残留消息导致"等空"死锁，T23）。本轮补的是 **pacing** 这一半。

### 修复 2：`ts_test.c` —— 加 **rdcycle 交叉验证**

日志里 `delta_1s = 58254 ticks` 与"24 MHz"不符。但那个数字只有在 `delay_ms(1000)` 真的是
1 秒、且 `S_TIMER` 输入时钟如 `timer.c` 所假设时才有意义。**为排除 `delay_ms()` 的干扰**，
现在同一段延时同时也用 **E902 自己的 `rdcycle`（CSR，不可能配错）** 计量：

```
rdcycle over the same 1 s delay = <N> (E902 cycles)
```

另外把 `TIMESTAMP_CTRL` 块的计数器（`CNT_CTRL_LOW/HI @+0x08/+0x0C`）也一起读出来对照。

### ⚠️ 纠正我自己：**时间戳计数器地址本来就是对的**

上一轮我说"`TS_STA = 0x08010000` 读错了"——**错了**。手册 §5.1.5.2 明确：

```
TIMESTAMP_STA  (0x08010000): CNT_LOW_REG @+0x00, CNT_HI_REG @+0x04   ← 计数器就在这
TIMESTAMP_CTRL (0x08020000): TSTAMP_CTRL @+0x00, CNT_CTRL_LOW @+0x08,
                             CNT_CTRL_HI @+0x0C, CNT_FREQID @+0x20
```

**⇒ 计数器在 STA 块里，固件读的是对的。** 真正的异常是另外两点：
① `CNT_FREQID`（0x08020020）从 **E902 读回 0**，而 ARM 侧读是 `24000000`；
② `58254 ticks/s` 与 24 MHz 不符 —— 待 rdcycle 数据判定是"计数器不是 24 MHz"还是"`delay_ms` 不准"。

---

## 三、内核选项：**能开的开了，但要说清"开了也没用"的部分**

### 已写入 `userpatches/linux-sun60iw2-current-a733.config`（已备份原文件）

| 选项 | 值 | 作用 |
|---|---|---|
| `CONFIG_RPMSG` / `RPMSG_NS` / `RPMSG_VIRTIO` | `=y` | 主线的 rpmsg/virtio 栈 |
| `CONFIG_VIRTIO` | `=y`（原本就是） | — |
| `CONFIG_AW_RPMSG_VIRTIO` | `=y` | **Allwinner 的 AMP rpmsg 总线**（`aw_virtio_rpmsg_bus.c`） |
| `CONFIG_AW_RPMSG_CTRL` | `=y` | 导出 `/dev/rpmsg-*`，让用户态能建 endpoint |
| `CONFIG_AW_RPMSG_CLASS` | `=y` | rpmsg class |
| `CONFIG_AW_RPBUF` / `RPBUF_DEV` / `RPBUF_SERVICE_RPMSG` | `=y` | rpmsg 之上的大块数据交换框架 |

### ❌ **故意没开**：`AW_RPMSG_PERF_TRACE`（也就是用 `amp_timestamp` 的那条）

原因：它 `select AW_AMP_TIMESTAMP`，而
- **`AW_AMP_TIMESTAMP` 在整棵树里没有 Kconfig 定义**（没有任何 `config AW_AMP_TIMESTAMP`）
- **`include/linux/amp_timestamp.h` 文件不存在**（`find` 全树无结果）

⇒ 这套 API 是**更新版 SDK 才有**的；这棵树里只有引用、没有实现。开了会**编译失败**。

### ⚠️ 而且：**这些选项开了也不会有效果**

`grep` 全部 DTS：**没有任何 `rpmsg` / `rpbuf` / `remoteproc` 节点**。
⇒ 驱动会 probe，但**找不到对端设备**，通道建立不起来。

要真用上，得再补：
1. 一个 **remoteproc 节点**（描述 E902 的固件载入方式）
2. **E902 侧的 virtio/rpmsg 实现**（现在只有裸机固件，没有 rpmsg 栈）

**⇒ 结论：这些选项是"把基础设施准备好"，不是"打开就能通信"。**

---

## 四、小核↔大核通信：**现在真正可用的两条路**

| 路径 | 需要做什么 | 工作量 |
|---|---|---|
| **裸 MSGBOX**（通道 3） | 大核侧写一个**小字符设备/模块**做 msgbox 客户端；协议已是现成的 `[头][字数][数据…]` | 小（一个 .c） |
| **共享 SRAM + `/dev/mem`** ⭐ | E902 把采样写进 SRAM 环形缓冲；大核用户态直接 `mmap /dev/mem` 读 | **最小（零内核工作）** |
| rpmsg/rpbuf | 补 remoteproc 节点 + E902 侧 rpmsg 栈 | 大 |

**⇒ 对 BMI088 建议走"共享 SRAM 环形缓冲"**：E902 采样 + 打时间戳 → 写环形缓冲 → 大核 `mmap` 读，
加一个 MSGBOX 通道做"有新数据"通知。**不需要任何新内核驱动。**

---

## 五、镜像更新

| 文件 | 大小 | sha256 |
|---|---|---|
| `e902/fw-out/scp-ours-padded-105912.bin`（待刷） | 105912 | `e9a361d27887b32c7aa8a149211ab89a6e6fa8c0755fee3a66f45e58d860ecd6` |
| `e902/fw-out/scp-ours-6656.bin` | 6656 | `45b92ff220c72f4d3971a421ca9a48f00276e4afca415b6ab89f59775abedd44` |

**刷写前记得**：内容变了要重算启动包校验和 → `python3 e902/tests/fix-bootpkg-sum.py <dev> --write`
