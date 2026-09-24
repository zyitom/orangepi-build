# E902 在 A733 里到底是什么、能做什么

日期：2026-09-22 · 证据来源：A733 手册 v0.91、内核 6.6.98-sun60iw2 在树源码、厂商 `scp.fex` 反汇编、
以及本项目 2026-09-22 的全部实测（见 `FINDINGS-LEDGER.md` T1–T21）。

---

## 1. 一句话

**E902 不是"一颗给你跑应用的小核"，它是 A733 的 SCP（System Control Processor，系统控制协处理器）。**
它的本职工作是替大核（CPUX，4×Cortex-A55）干那些"不能停、必须一直在、大核睡着了也得活着"的活：
**DRAM 初始化与变频、PMIC 供电、待机/关机、晶振/24M hosc 电源广播（本板晶振为 26 MHz，见 T34）、I2C、看门狗、USB 隔离**。

这一点有硬证据——厂商固件 `scp.fex` 自己打印的服务清单：

```
DRAM Type =%d (8:LPDDR4,9:LPDDR5)     DRAM CLK =%d MHZ
DRAM DFS VERSION INFO: %s              set dram crc paras request
DRAM SIZE =%d MBytes, para1 = %x ...   the counter for dfi_freq is = %d
axp515 exist                           close axp515 batfet
reset axp515                           clear axp515 irq
pmu & bmu driver ok                    init twi succeeded
standby service ok                     poweroff system
broadcast 24mhosc power-on ready       24m hosc will power-off notify
clk_suspend                            iosc_freq: %d
watchdog ok                            usb_iso_suspend / usb_iso_resume
timer driver ok                        twi driver ok
hwmsgbox driver ok                     message manager ok
cpucfg driver ok                       debugger system ok
daemon service setup...                startup feedback ok
```

## 2. 硬件关系

```
        ┌──────────────── A733 SoC ────────────────┐
        │                                           │
  CPUX 域│  4× Cortex-A55        CPUS 域  玄铁 E902   │
        │  GIC 中断控制器         CLIC 中断控制器      │
        │  ├ Linux 跑在这           ├ 厂商 SCP 固件跑在这│
        │  │                        │                │
        │  └─ 共享:  SRAM_A2 208K  ─┘   （同一片，两侧地址不同）
        │            DRAM            （E902 有可 remap 的 DRAM 窗口）
        │     MSGBOX 两套单向 FIFO + hwspinlock  ← 跨域通信
        │     还有一颗 OR100 核 + S_SPC 块（手册未公开寄存器）
        └───────────────────────────────────────────┘
```

| 项 | 数值 / 事实 | 证据 |
|---|---|---|
| E902 ISA | **RV32EMC**（E=只 16 个通用寄存器、M=乘除、C=压缩）+ Zicsr | 工具链 `-march=rv32emc_zicsr` 可编；`scp.fex` 里 87 条乘除指令 |
| E902 中断 | CLIC @ 核内 `0xE0800000`，不是 GIC | E902 手册 10.2；`scp.fex` 反汇编 `lui a4,0xe0801` |
| 共享 SRAM | `SRAM_A2` 208K，**E902 视角 `0x40000000`，ARM 视角 `0x00040000`** | 手册 ch.2；`cpu_autogen.h:5` |
| DRAM | 大核与 E902 **数值相同、含义不同**（都从 `0x40000000` 起） | 本项目设计笔记 §2.3.1（早期踩过这个坑） |
| 跨域通信 | `MBOX_CPUX 0x03004000`（E902 写/ARM 读）、`S_MBOX 0x07094000`（ARM 写/E902 读）；各 4 通道 × 8×32bit | 手册 6.1；双方实测可读写 |
| 互斥 | `hwspinlock 0x03005000` | 手册；DT 里 `okay` |
| 时钟 | E902 子系统只有 `RISCV_24M`（`0x07010210`）+ `RISCV_BGR`（`0x0701021C`）**两个**寄存器，**没有 CPU 变频** | 手册 4.2.5.23/24；`ccu-sun60iw2-r.c` |
| 内核复位 | **不存在于任何软件可见寄存器**；只能由 bl31 释放 | 手册 5.2.3 / 4.2.5.24 / R-CCU 复位表；T5/T6/T9 实测 |

## 3. 软件关系

**启动链**：
```
boot0(SPL) → u-boot → bl31(monitor.fex) → Linux
                          │
                          ├─ 从启动包取出 scp.fex，拷到 SRAM_A2 @0x40004000
                          ├─ 写 E902_RST_START_ADDR = 0x40004000
                          ├─ 释放复位 → E902 开始跑厂商 SCP
                          └─ 调 arm_svc_arisc_wait_ready() 等它"报就绪"
```
证据：`u-boot/arch/arm/lib/bootm.c:409`（`ARM_SVC_ARISC_STARTUP = 0x8000ff10`）、
`u-boot/drivers/arisc/arisc.c:274,290`；`scp.fex` 在介质上的位置实测 `0x113BC00`（T17）。

**Linux → E902 的调用**：走 **SMC（同步 RPC）**，只有极少数驱动在用：

| SMC 服务 | ID | 谁在用 |
|---|---|---|
| `ARM_SVC_SUNXI_DDRFREQ` | `0xc0000086` | `bsp/drivers/clk/sunxi-ng/**ccu-ddr.c**` → **DRAM 变频必须经过 E902** |
| `ARM_SVC_SUNXI_CRASHDUMP_START` | `0xc0000085` | `bsp/drivers/crashdump/sunxi-dump2pc.c` |
| `SET_DEBUG_DRAM_CRC_PARAS` | `0xc0000054` | `sunxi-smc.c` |
| `SET/CLEAR_WAKEUP_SRC`、`SET/CLEAR_DRAM_KEEP_RUNNING` | `0xc0000016/15/18/17` | 同上 |

（`bsp/include/sunxi-sip.h`；`awlink` 那套是给 **T527** 的，sun60iw2 上没启用。）

**反过来 E902 → Linux**：走 **MSGBOX 通道 3** 的"分包"协议
（报文 `[u32 头][u32 字数][数据…]`，头字 = `byte0 | flags<<8 | cmd<<16 | result<<24`；T15/T16 实测）。
启动时它会先发一条**启动反馈**（头字 `0x00900200`、13 个字），这就是 bl31 等的东西。

**Linux 侧驱动现状**：`sunxi-msgbox`、`sunxi-hwspinlock` 都在树且 `CONFIG_*=y`，
但 `/proc/interrupts` 里没有 mbox 行 —— **没有任何客户端在用它**。也就是说
"ARM↔E902 的异步通道"目前是空跑着的，随时可以拿来用。

## 4. 它硬件上"能"做什么

| 资源 | 状况 | 小核能用吗 |
|---|---|---|
| `S_UART0 / S_UART1` | 排针 pin16/pin18 接的就是 S_UART0（**本轮已实测打通**） | ✅ |
| `S_TWI0 / 1 / 2` | `S_TWI0` 是 **PMIC 总线**（AXP515/AXP8191），厂商 SCP 在用 | ⚠️ 别抢 |
| `S_SPI` | `0x07092000`，DT 里 `disabled`，**厂商与 Linux 都不碰** | ✅ 免冲突（驱动已写好） |
| `S_PWM` | DT 未启用 | ✅ |
| `S_IRRX` | 红外，厂商 SCP 高频访问（Clk 6 次引用） | ⚠️ |
| `S_TIMER0..3` | 4 个，CPUS 域时钟 | ✅ |
| `GPIOL / GPIOM`（PL/PM） | PL0/PL1 是 PMIC 的 I2C；PL2/PL3 是 S_UART0；PL7 是 LED | ⚠️ 部分占用 |
| `CLIC` | 核内中断控制器，IRQ 编号见手册 Table 12-2（S_SPI=38、S_UART0=29、MSGBOX 收=48） | ✅ |
| `MSGBOX` ×2 + `hwspinlock` | 双向通道 | ✅ |
| 内存 | SRAM_A2 208K（自身）；DRAM 需 `E902_DDR_REMAP` 配置窗口 | ✅ 有限 |
| **CPU 变频** | **没有**（`RISCV_BGR` 里没有分频位） | ❌ 固定频率 |

## 5. 所以你能拿它做什么 —— 以及代价

### 能做到的（由易到难）

1. **跑自己的裸机固件**：SPI 读取、串口中断、定时器、GPIO 翻转 —— 这些硬件资源它都有，
   BSP 代码本项目已写好（`e902-fw/src/{spi.c,uart.c,msgbox.c,timer.c,...}`）。
2. **与 Linux 双向通信**：MSGBOX 通道 3 + 共享 SRAM，协议已解出，Linux 侧驱动在树（只是没客户端）。
3. **当"协处理器"用**：高频采样、实时性要求高的 IO 轮询/中断响应，不受 Linux 调度抖动影响。

### 代价（**这是最关键的一条**）

**E902 只有一颗。它跑了你的固件，厂商 SCP 就没了。** 于是这些服务同时消失：

| 失去的能力 | 后果 |
|---|---|
| DRAM 变频（`ARM_SVC_SUNXI_DDRFREQ`） | DRAM 频率锁死；`ccu-ddr.c` 的 SMC 调用失败（软失败，返回错误码） |
| PMIC 看护（AXP515/AXP8191） | 供电异常/中断没人管 |
| 待机 / 关机（`standby`/`poweroff`） | **`echo mem` 会醒不过来**（要硬复位）；关机要靠 ACPI/其他途径 |
| 晶振/24M hosc 电源广播（晶振本身是 26 MHz，T34） | 时钟域切换行为可能异常 |
| 看门狗 | `watchdog ok` 那套没了 |
| USB 隔离（suspend/resume 用） | USB 在 suspend 恢复时可能不稳 |

所以现实的用法是二选一：
- **A：完全接管**（我们现在的路线）——E902 是我们的，厂商服务全丢，需要时自己重实现。
- **B：不动厂商 SCP，用它的"消息接口"**——见下节，这是更省事但还没验证的一条路。

## 6. 有一条"不替换"的路，值得试（本轮新增想法）

厂商 SCP 里有完整的 **message manager**（`message manager ok`），支持这些 ARM→SCP 命令：
`0x19 0x22 0x24 0x25 0x26 0x60 0x61 0x62 0x64 0x96`，其中 `0x61` 是 "loopback message request"。

本轮实测：往通道 3 发命令，**消息只入队不被取走**，因为 `S_MBOX RD_IRQ_EN = 0`
——**SCP 没打开自己的收中断**。

**所以只要把那个位打开（从大核侧写 `S_MBOX + 0x20` 的 bit6），厂商 SCP 也许就会开始收我们的消息。**
如果它响应了，就等于**在不替换 scp.fex 的前提下拿到了一个可用的双向通道**，能直接让现成的 SCP 帮你干活
（比如 0x96 / 0x60 这类命令，或它自己实现的回环）。

这条路**零风险**（只写一个中断使能位 + 发消息），而且不需要刷写任何介质。
板子恢复后第一件事就该试这个。

## 7. 当前状态

| 项 | 状态 |
|---|---|
| 资源归属 / 手册与实测对照 | ✅ 完成（`RESOURCE-MAP.md`） |
| 小核串口观测 | ✅ 打通（FT232H 已在 TL101，`/dev/ttyUSB1`） |
| 握手协议 | ✅ 解出并修正头字（`0x00900200`），**尚未验证被 bl31 接受** |
| writer / 回滚 / 镜像 | ✅ 全部就绪（含现成可烧镜像两张） |
| 板子 | ⚠️ 当前因第一次刷写（头字错误）起不来，**待烧录恢复** |

---

## 8. 证据等级声明（**重要修正**，2026-09-22 补）

第 1 节把"固件里有这些代码"写成了"它正在干这些活"——**这是越界**。分开说：

### 8.1 实测到的（observed，可复现）

| 事实 | 证据 |
|---|---|
| 厂商固件被载入 `0x40004000` 且**正在运行** | T13：它自己启动时会清零的 `.bss`（`0x4001DDB8..0x4002AF3C`）里存在 **29023 字节**运行期写入的数据、有明显表结构 |
| 它**不理会** mailbox | T16：`S_MBOX RD_IRQ_EN = 0`；往里发的消息只入队不被取走，8 秒无回应 |
| bl31 **确实在等它** | T21a：把它的启动反馈偷换成错误头字后，**整块板子起不来**（卡在 bl31）→ 它在启动关键路径上 |
| 内核**把它当 arisc 对待** | 板上 DT 有 `/arisc_config` 节点（虽为死节点，但记录了 `pins = "PL2" "PL3"`） |

### 8.2 只在固件镜像里存在（代码路径存在，≠ 正在执行）

第 1 节列的那些字符串（`DRAM CLK`、`axp515`、`standby service ok`、`watchdog ok`、`usb_iso_*`…）
证明**这些代码路径被编进了这个固件**。但**没有任何一条被观测到真的执行**。

### 8.3 接口层证据（设计上会调它，≠ 真的调了）

- `bsp/include/sunxi-sip.h` 定义服务号，`ccu-ddr.c` 里 `set_ddrfreq()` 真的写
  `invoke_scp_fn_smc(ARM_SVC_SUNXI_DDRFREQ, freq_id, 0, 0)` — 这是**设计意图**。
- 但 **本板实测/配置显示这些路径大多没开**：

| 配置项 | 本板值 | 含义 |
|---|---|---|
| `CONFIG_AW_DMC_DEVFREQ` | `=m`（模块） | 而板上 `/sys/class/devfreq` 只有 `3600000.npu` → **DMC 没跑** |
| `CONFIG_SUNXI_SMC` | 配置里**没有** | `sunxi-smc.c` 根本没编 |
| `CONFIG_AW_CRASHDUMP` | `# ... is not set` | crashdump 那条 SMC 路径没编 |
| `CONFIG_AW_MSGBOX` / `AW_HWSPINLOCK` | `=y` | 驱动在，但**没有客户端**（`/proc/interrupts` 无 mbox 行） |

**所以更准确的说法是**：E902 是 SoC 的 SCP，固件里带着一整套系统控制服务；
**但在本板上，Linux 侧把绝大部分相关路径都关掉了，所以我们现在无法从"行为上"证明它在具体做哪几件事。**

### 8.4 怎么把 8.2 变成硬证据（板子恢复后我可以逐项做）

| 要验证的 | 命令 | 判据 |
|---|---|---|
| DMC/DRAM DFS 是否在跑 | `lsmod \| grep dmc`；`ls /sys/class/devfreq`；`dmesg \| grep -i ddr` | 出现 dmc/devfreq 条目 = 在跑 |
| 待机是否由它服务 | `cat /sys/power/state`；**谨慎**试 `echo mem > /sys/power/state` | 能进能出 = 服务在 |
| mailbox 命令谁响应 | 打开 `S_MBOX+0x20` 的 bit6 后，逐个试 `0x19/0x22/0x24/0x25/0x26/0x60/0x61/0x62/0x64/0x96` | 有回应 = 它的命令接口是活的 |
| PMIC 是否由它看护 | 看它是否在轮询 `S_TWI0`；或制造一次 PMIC 中断 | 有定期访问 = 在看护 |

**结论**：第 4 节"硬件上能做什么"（手册 + 内核 headers）是硬的；
第 1 节"它正在做什么"**目前是弱的**，需按 8.4 逐项落实。

---

## 9. **重大修正**：那四项"会丢失的服务"，三项其实不经过 SCP（2026-09-22）

有读者追问"DRAM 变频/PMIC/待机/看门狗这些**现在真的有在做吗**？我完全看不出来"。
逐项查证后：**三项根本不经过 SCP，只有待机可能真的依赖它。**

| 服务 | 谁在做 | 证据 |
|---|---|---|
| **PMIC（AXP515/AXP8191）** | **Linux 自己在做** | 板级 DTS `sun60i-a733-orangepi-zero3w.dts`：`&s_twi0` 下挂 `compatible = "x-powers,axp515"`（行 1958）、`axp515-pek`(1971)、`axp515-usb-power-supply`(1982)、`axp515-bat-power-supply`(1997)、`axp515-regulator`(2085)、`axp8191`(2099)。即 **Linux 通过 `S_TWI0` 这条 I2C 直接管 PMIC**，与 SCP 无关 |
| **看门狗** | **Linux 硬件驱动** | `CONFIG_AW_WATCHDOG=y`（`CONFIG_SUNXI_WATCHDOG` 未设）；`sun60iw2p1.dtsi:1388` 有 `wdt: watchdog@2050000 { compatible = "allwinner,wdt-v103" }` —— 这是 **CPUX 域**的硬件看门狗，不是 SCP 的 |
| **DRAM 变频 / DFS** | **本板没有启用** | `CONFIG_AW_DMC_DEVFREQ=m`，但板级 DTS **没有任何 dmc/devfreq 节点**；板上 `/sys/class/devfreq` 只有 `3600000.npu`。唯一会调 `ARM_SVC_SUNXI_DDRFREQ` 的 `ccu-ddr.c` 路径没有设备在驱动它 |
| **待机 / 关机** | ⚠️ **可能真的依赖 SCP** | SCP 里有 `standby service ok` / `broadcast 24mhosc power-on ready` / `24m hosc will power-off notify` / `poweroff system`；suspend 走 PSCI → bl31 → SCP。**未验证** |

### 这意味着什么

1. **本板 `S_TWI0` 是 Linux 在用的**（PMIC）——所以"E902 独占 s_twi0"是错的，
   `S_TWI0` 属于**共享/大核在用**，我们的固件**不要碰它**。
2. 替换 `scp.fex` 的**真实代价比先前说的小得多**：
   - ~~丢 PMIC~~ → ❌ 不成立（Linux 自己管）
   - ~~丢看门狗~~ → ❌ 不成立（CPUX 域独立硬件）
   - ~~丢 DRAM 变频~~ → ❌ 本板没启用
   - **待机/关机** → 可能真会丢，需实测
3. 这也再次说明 **"从固件字符串推断它在做什么"极不可靠**。
   要确认它到底在干什么，只有一条可靠路子：**打开它的消息接口直接问它**（见 §6 的 B 方案）。
