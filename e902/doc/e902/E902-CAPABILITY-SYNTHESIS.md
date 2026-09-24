# 深度归纳：E902 到底能做什么（基于全部文档 + 实测）

日期：2026-09-22 · 来源：`doc/e902/` 全部文档 + `verify-logs/` 现场日志 + T1–T21 实测

---

## 第 0 部分：板子现在为什么开不了机（逐行读 bootloop 日志）

`verify-logs/arm-console-v2-bootloop-20260922.log` 关键行：

```
 line 7    [123]boot param - magic error            ← boot0 阶段的告警（之后仍继续）
 line 19   NOTICE Error initializing runtime serv…   ← ★ bl31：SCP 运行时服务初始化失败
 line 20   amp fw boot after!!!                      ← ★ bl31：选择"跳过 SCP，继续启动"
 line 57   [11.499]Starting kernel ...               ← 内核镜像已加载、校验 OK、开始执行
 line 59   [4][mmc]: MMC Device 2 not found  so not exit
 line 60   by_device fail!
 line 61   [12.143]Failed                            ← 0.6 秒后失败
 line 63   starting!                                 ← 重启 → 循环
```

### 两个必须纠正的点

| 我之前说 | 实际 |
|---|---|
| "握手失败 → bl31 硬卡死" | ❌ **它会跳过**：打 `Error initializing runtime serv…` + `amp fw boot after!!!`，然后**继续启动**（对应字符串 `[SCP WARNING] :arisc startup waiting ignore message`）。所以板子能走到 kernel |
| "bootloop 一定是 E902 引起的" | ⚠️ **不确定**。日志里还有 `MMC Device 2 not found` / `by_device fail!` / `Loading Environment from EXT4…Failed (-5)` —— 这可能是**根文件系统/环境问题**，与 E902 无关 |

### 一个测试就能区分（便宜、安全）

**把厂商 `scp.fex` 写回去 → 重启**：
- 能正常进 kernel → 问题确定是 E902 造成的
- **还是 bootloop** → 问题与 E902 无关（SD 环境/根分区），要另查

---

## 第 1 部分：E902 能做什么 —— 分三档

### ✅ 第一档：**已经实测证明能做**（有原始日志）

| 能力 | 证据 |
|---|---|
| **执行我们的裸机代码** | 18:06 `e902-console-first-success` 日志里的 banner |
| **完整驱动 S_UART0（收发）** | banner + 一直用它做控制台 |
| **初始化并编程 `S_SPI`（`0x07092000`）为主机** | `spi_init(500kHz)=ok`；`GC=0x00000083`（EN+MODE master+TP_EN）、`CLK_CTL=0x00001017`（CDR2=23 → 24 MHz/48 = 500 kHz〔更正，T31：当时 SPI mux 在 dcxo 上，实际 26 MHz/48 = 541 kHz；v65 起 mux 改 pll-ref 后该公式才成立〕）、`INT_STA=0x32`（TC 位已置） |
| **跨域读 CPUX 域寄存器** | 日志里 `TSTAMP_CTRL=0x00000001`、`t0/t1` 递增 —— 那是 CPUX 域 `0x08010000` |
| **往邮箱写入**（能被对端看到） | T16：`MSG_STATUS` 0→3→6、通道 3 读中断挂起位被置起 |
| 写 SRAM 心跳、S_TIMER 计数 | 固件里的 `hb_*` / `[tick]` 机制 |
| **被 bl31 载入并放复位** | `RST_START=0x40004000` + 我们的代码真的跑了 |

### ⚠️ 第二档：**有依据、但还没实测**

| 能力 | 依据 | 缺什么 |
|---|---|---|
| **CLIC 中断真的触发** | 固件里已注册 4 个（UART29/SPI38/MBOX48/TIMER21）；手册 §12.3.3 给了寄存器 | 日志里**没有中断计数的变化记录** → 没验证过 |
| **PL/PM 引脚外部中断（DRDY）** | CLIC IRQ 25/26；PL2–PL5 都能设 `eint` | EINT 寄存器偏移未读出；未实测 |
| **跨域驱动 CPUX 域外设**（SPI3/I2C/UART2,6/PWM） | E902 能跨域读写（已实测）；中断可经 GINTC 桥接（手册 Table 12-2 IRQ 54–77） | SPI3 本身没试过；GINTC 基址未找到 |
| **DMA** | CLIC IRQ 50–53 = DMAC0/1 的 CPUS 通道 | 未试过 |
| **S_TWI / S_PWM / S_IRRX** | PL2–PL5 的 mux 里有；`soc_early_init` 已开时钟 | 未试过 |

### ❌ 第三档：**做不到 / 代价大**

| 事项 | 原因 |
|---|---|
| **软件侧重启/重定位 E902** | 无内核复位位（手册 4.2.5.24 / 5.2.3 / R-CCU 表，三方一致）→ 只能靠 bl31 启动 |
| **原生 `S_SPI` 从 40-pin 引出** | 需要 PL4+PL5+**PL6**+**PL7**，排针只有前两个 |
| **与厂商 SCP 共存** | 只有一颗核 |
| **CPU 变频** | `RISCV_BGR` 里没有分频位 |
| **原生 40-pin 外设数量** | 只有 4 根 PL 脚（UART×1 / I2C×1 / PWM×4 / IR×1） |

---

## 第 2 部分：所以对 BMI088 + 高精度时间戳，能到什么程度

| 需求 | E902 能给什么 | 状态 |
|---|---|---|
| 读 SPI | ✅ 能编程 SPI 控制器（已实测） | 控制器可以是自己的 `S_SPI`（PL 脚不够）**或跨域的 SPI3（40-pin 在 19/21/23/24/26）** |
| 硬件中断 | ⚠️ CLIC 已配好、中断号已知 | **触发尚未实测** |
| 高精度时间戳 | ✅ `S_TIMER` 24 MHz → **41.7 ns** 分辨率；核心时钟独立（不受大核变频影响） | 已实测读到时间戳寄存器 |
| 与 Linux 通信 | ✅ MSGBOX 通道 3，协议已解（`[头][字数][数据…]`） | 写入已实测；**读取/中断未实测** |

**⇒ 结论：技术路径成立，但三道门必须依次过：**
1. **握手（当前卡点）** —— 等待判据 bug 已定位待修
2. **中断** —— 需要实测一次真实触发
3. **跨域驱动 SPI3** —— 需要实测

---

## 第 3 部分：建议的推进方式（关键：用最小固件隔离变量）

现在的痛苦在于"一次刷写 = 一次开机失败 = 要拔卡恢复"。所以**不要一次堆太多功能**：

| 步骤 | 固件内容 | 判定 |
|---|---|---|
| **S1** | **只做握手**（`hb_init` + 修好的邮箱发包 + 死循环），不碰 UART/SPI | 板子能进 kernel ⇒ **握手正确**（这是最关键的一步） |
| S2 | S1 + S_UART0 输出 | 小核串口能出 banner |
| S3 | S2 + CLIC 中断（选一个能自触发的源，如 S_TIMER） | 中断计数开始增长 |
| S4 | S3 + SPI（先 `S_SPI` 自测，再跨域 SPI3） | 读回数据一致 |
| S5 | S4 + BMI088 真实读写 + DRDY 中断 + 时间戳 | 采样数据流 |

**每一步都有明确的通过/失败判据**，失败时只回退一步，而不是全盘重来。
