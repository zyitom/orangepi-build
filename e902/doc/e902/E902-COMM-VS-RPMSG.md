# 小核通信方案与 rpmsg 对比：速率、层级、缺口

> 回答三个问题：**小核（E902/CPUS）用什么通信？速率和 rpmsg 比如何？缺的是底层 SDK 还是上层 app？**

## 0. 一句话结论

**rpmsg 不提供额外的物理带宽。** 它本身就是"共享内存 + 邮箱中断"之上包的一层协议；
所以"MSGBOX + 共享 SRAM"和"rpmsg"的**物理上限完全相同**，rpmsg 反而每包多一层软件开销。
缺的不是底层 SDK，也不是上层 app，而是**中间那层协议栈（E902 侧 virtio/vring + rpmsg 状态机）**——那是"自己新写"，不是"打开一个开关"。

---

## 1. 小核现在实际可用的通信通道（均已上板实测）

| 机制 | 位置/寄存器 | 单次粒度 | 用途 | 实测状态 |
|---|---|---|---|---|
| **MSGBOX** 硬件邮箱 | TX `0x0300407C` / RX `0x0709407C` / STATUS `0x0300406C` / `0x0709406C`，ch3 | 4 字节，FIFO 8 深 | 控制/通知/小包 | ✅ 双向收发通过 |
| **共享 SRAM** | ARM 侧 = `0x00040000 + (E902_addr - 0x40000000)` | 任意长度 memcpy | 大数据/环形缓冲 | ✅ 读写通过 |
| **共享时间戳** | STA `0x08010000`，CTRL `0x08020000` | 64 位只读 | 跨核统一时基 | ✅ 24 MHz，与 ARM 同源、零漂移 |
| **MSGBOX 中断** | 小核 CLIC IRQ 48（收）；GPIO/其它见 CLIC 25–28/50–53 | — | 事件驱动 | ✅ CLIC 已定位 |

报文格式（厂商约定，已逆向）：

```
[u32 头][u32 字数][数据…]
头 = byte0 | flags<<8 | cmd<<16 | result<<24
启动反馈头 = 0x00900200
```

关键 **pacing 规则**（此前踩过的坑）：**每个字写入前**都要等 `MSG_STATUS != 8`（8 = FIFO 满）；
只在开头等一次会导致反馈包（15 字 > FIFO 深度 8）溢出 → `startup feedback : FAILED`。
RX 侧判据是 `MSG_STATUS != 0`。

---

## 2. 速率对比

### 2.1 物理层（决定上限）

| | MSGBOX + 共享 SRAM（自建） | rpmsg |
|---|---|---|
| 物理通道 | MSGBOX + SRAM | **同一个** MSGBOX + SRAM |
| 单次搬运 | 4 B（邮箱）/ 批量 memcpy（SRAM） | 同样走 SRAM（vring buffer） |
| 上限 | 总线带宽 | **总线带宽（相同）** |

**rpmsg 的层次**，从下往上：

```
物理：MSGBOX 寄存器 + 共享 SRAM + 中断
   ↑
virtio：vring（描述符表/avail 环/used 环）+ 内存屏障 + kick/notify
   ↑
rpmsg：endpoint 管理 + name service(NS) + 地址路由
   ↑
用户态：/dev/rpmsgX 、rpmsg-ctrl
```

自建方案**只取最底下那层**，上面三层全都省掉。

### 2.2 软件开销（rpmsg 多出来的部分）

| 环节 | 自建 | rpmsg |
|---|---|---|
| 发送 | 1 次 memcpy + 1 次 MSGBOX 写 | vring 描述符填充 + avail 环推进 + `kick` 通知 + 屏障 |
| 接收 | 读 MSGBOX → 取 SRAM 数据 | used 环检查 + 描述符回收 + endpoint 查找 + 回调派发 |
| 每次额外次数 | ~0 | 若干次指针推进 / 屏障 / 查表 |
| 量级 | — | 每包**多几百 ns ~ µs 级**（与主频相关） |

**大数据吞吐**：两者都被 SRAM 的 memcpy 带宽支配，rpmsg 只多几个百分点的协议开销。
**小包/控制消息**：自建明显更快，因为差异全在协议开销上。

> 注：具体绝对值未在本板逐项标定（板子当前离线）。上表为结构化量级判断；
> 上板后可用 CLIC/PMU 或共享时间戳对两者做实测对比，见 §5。

### 2.3 结论

**rpmsg 是"更标准的通道"，不是"更快的通道"。** 它换来的是通用性：
标准设备模型、动态 endpoint、用户态 `/dev/rpmsgX`、与 remoteproc 生态兼容。
对 BMI088 这种**固定格式 + 小数据 + 要时间戳**的场景，这些通用性用不上，
却要为此付出额外协议开销，并在小核侧背上一个协议栈。

---

## 3. 缺的是底层 SDK 还是上层 app？

**都不是。缺的是中间那层协议栈。**

| 层级 | 状态 | 说明 |
|---|---|---|
| **底层（寄存器/中断/时钟）** | ✅ **不缺** | MSGBOX 寄存器语义、SRAM 地址映射、CLIC/GIC 中断、时钟树、玄铁工具链全部在手；连 `opene902` 的 RTL 都有。已用**裸机寄存器**把 MSGBOX 收发 + SPI + 中断真跑起来过。厂商提供的"SDK"本质就是"寄存器定义 + 参考固件"，没有更高级的东西，也不需要更多。 |
| **上层（app/业务）** | ⚪ **不是主要矛盾** | BMI088 业务逻辑（配量程、读 XYZ、打时间戳、组包）几百行。 |
| **中间（协议栈）** | ❌ **真正的缺口** | E902 侧 `virtio/vring` + `rpmsg` 状态机：vring 描述符表、avail/used 环、内存屏障、队列中断、name service 端点管理。外加 DTS 里给 CPUS 建 remoteproc/vdev 对端节点。**量级：几千行新固件 + 板级联调。** |

补充说明"底层 SDK 为什么不缺"：

- E902 是 RV32E 裸机（`-march=rv32emc_zicsr -mabi=ilp32e`），本来就没有 Linux/RTOS 的
  "SDK 概念"；能给的只有寄存器手册 + 启动约定。这些我们已经完整掌握。
- 真正的启动约束也不在"SDK"，而在 **boot0 启动包校验（`add_sum`）** 与
  **bl31 两阶段握手**（`wait arisc ready` → 启动包查询/回显 → `hard syn`）——
  这些都已查明并有工具（`fix-bootpkg-sum.py`）。

---

## 4. 推荐方案（BMI088）

```
E902（小核）                          ARM（大核 Linux）
  ├ SPI3 读 BMI088（硬件中断触发）
  ├ 打共享时间戳（0x08010000，24 MHz）
  ├ 写入共享 SRAM 环形缓冲
  └ MSGBOX 通知（4 字节：序号/类型）  ──中断──▶ 内核侧读取环形缓冲
                                                  └▶ IIO / SPI 驱动消费
```

理由：

1. **物理通道与 rpmsg 相同**，速率不损失；
2. **零新内核驱动**（MSGBOX 已有 `AW_MSGBOX=y`，SRAM 走 `awdevmem.py`/devmem）；
3. **零 E902 协议栈**（只需自写 4 字节头 + 环形缓冲，几十行）；
4. 逻辑全在自己手里，寄存器级可观测，好调；
5. 时间戳**天然双核同源**，不需要任何同步协议。

---

## 5. 后续可选：rpmsg 里程碑（若确有通用消息需求）

前置条件（本仓库现状）：

| # | 前置 | 现状 |
|---|---|---|
| 1 | 内核侧 rpmsg-virtio 能编译 | ✅ **本轮已修**（`rpmsg_chrdev_register_device`→`rpmsg_ctrldev_register_device`；`class_create` 去掉 `THIS_MODULE`）。`aw_virtio_rpmsg_bus.o` / `rpmsg_master.o` / `rpmsg_client.o` / `rpmsg_heartbeat.o` 均可编译 |
| 2 | `rpmsg_notify.o` | ❌ 独立缺陷：`notify_callback` 类型未定义 |
| 3 | E902 侧 virtio/rpmsg 栈 | ❌ 完全没有（需新写） |
| 4 | DTS remoteproc/vdev 对端节点 | ❌ 没有 |
| 5 | 全树 `class_create(THIS_MODULE,…)` 同类缺口 | ⚠️ 20+ 处（smartcard/spinand/wifi/npu/dram_info…），**哪个驱动被打开哪个就编不过** |

**建议**：先让小核固件握手稳定 + BMI088 数据链路跑通（S1–S5 分步），
rpmsg 作为**独立里程碑**评估，不要与 BMI088 交付耦合。

---

## 附：本轮实测数据（构建证据）

```text
全量内核编译  EXIT=0   3637 个目标文件   0 真实错误
LD [M]  bsp/drivers/ce/sunxi_trng/sunxi_trng.ko
LD [M]  bsp/drivers/gpadc/sunxi_gpadc.ko
LD [M]  bsp/drivers/lradc/sunxi_lradc.ko
        （AW_AMP_TIMESTAMP=y 内建进 Image）
rpmsg 移植后：
  ✅ aw_virtio_rpmsg_bus.o  390720 B
  ✅ rpmsg_master.o         331712 B
  ✅ rpmsg_client.o         441584 B
  ✅ rpmsg_heartbeat.o      214672 B
  ✅ rpmsg_perf.o           172256 B  (需 CONFIG_AW_RPMSG_PERF_TRACE=y)
  ❌ rpmsg_notify.o         (独立缺陷，需 AW_RPMSG_NOTIFY=y + 补类型)
```
