# BMI088 + 高精度时间戳：能用 E902 做吗？

日期：2026-09-22 · 针对 A733 / Orange Pi Zero 3W · 证据来源：`pin-capability.md`、`RESOURCE-MAP.md`、
`FINDINGS-LEDGER.md`（T1–T21）、A733 手册

---

## 1. 先说 SCP 是什么

**SCP = System Control Processor（系统控制协处理器）**。A733 里的 E902 就是干这个的：
它不是给你跑应用的小核，而是替大核盯住那些"大核睡了也得活着"的活——
DRAM 初始化/变频、PMIC 供电、待机/关机、晶振/24M hosc 电源广播（本板晶振为 26 MHz，见 T34）、I2C、看门狗、USB 隔离。

所以**把它拿来当实时 IO 前端，在物理上完全可行，但意味着厂商 SCP 的活没人干了**（下面 §4 详述）。

## 2. BMI088 需要什么

| 需求 | BMI088 的情况 |
|---|---|
| 总线 | SPI（4 线，CS 独立控制 accel / gyro 两个片选） |
| 数据率 | accel ODR 最高 ~1.6 kHz，gyro ~2 kHz；姿态解算常取 400 Hz–2 kHz |
| 关键信号 | **DRDY（数据就绪）中断输出** —— 拿它触发读取，才能把"采样时刻"钉准 |
| 时间精度 | 姿态积分对**采样间隔抖动**敏感：2 kHz 时周期 500 µs，如果抖动 50 µs 就是 10% 误差 |
| 每次事务量 | 一次读 6–12 字节（accel xyz + gyro xyz + 温度），SPI 10 MHz 下约 5–10 µs |

## 3. 三种架构（含实测判据）

### 方案 A：大核 Linux + spidev + DRDY GPIO 中断

- **做法**：用 CPUX 域的 `spi2`（PB0–PB5）或 `spi3`（PE0–PE4），设备树开 `spidev` + 一个 `interrupts-extended` 的 DRDY；
  用户态跑高优先级实时线程（`SCHED_FIFO`，绑核，`PREEMPT_RT` 内核已经装好）。
- **优点**：一天能出东西；spidev 生态成熟；不用动启动介质、不用碰 SCP。
- **缺点**：**抖动来自内核调度 + SPI 子系统 + 中断路径**。PREEMPT_RT 下典型 20–100 µs 最坏抖动。
  对 1–2 kHz 采样，这个抖动会直接进积分误差。
- **实测判据**：读 DRDY 中断时间戳（`CLOCK_MONOTONIC_RAW`）与 SPI 读完成时间，统计 10 万个样本的
  **最小/中位/p99/max 间隔**。**如果 p99 抖动 < 5% 周期，你根本不需要 E902。**

### 方案 B：E902 独占 S_SPI（**你的想法**）

- **做法**：E902 上跑裸机固件：DRDY 接一个 GPIO → **CLIC 外部中断**；
  中断里立刻读 `S_TIMER` 打时间戳，再用 `S_SPI` 突发读 BMI088。
- **硬件可行性**：**成立，已确认** —— `s_spi0` 就在 `PL4/PL5/PL6/PL7`（也能走 PM0–PM5）（见 `pin-capability.md` §2）。
- **时间精度**：E902 是裸机、只干这一件事，CLIC 中断延迟是"几个到几十个周期"量级；
  `S_TIMER` 24 MHz（R-CCU mux 4 = pll-ref/SYS_CLK24M，从 26 MHz 晶振再生；**勿选 dcxo mux 0 = 26 MHz**，T31）→ **41.7 ns 分辨率**。相比方案 A 是数量级的改善。
- **代价**：**厂商 SCP 服务全丢**（DRAM 变频/PMIC/待机/看门狗/USB 隔离）；`echo mem` 会醒不过来。
  而且必须改启动介质里的 `scp.fex`（就是现在这块卡住的地方）。
- **未知量**：E902 实际主频（`RISCV_BGR` 没有分频位，源码看是固定，但**具体多少必须实测**）；
  中断延迟与 SPI 读耗时也要实测。

### 方案 C：混合（**推荐**）

E902 只做"采样 + 打时间戳"，把结果塞进 SRAM 环形缓冲，再通过 **MSGBOX 通道 3**（或共享内存 + hwspinlock）
批量通知大核；Linux 拿到的是**已经带 E902 时间戳的整批样本**，只负责解算/发布。
- 时间精度等同方案 B；对大核的实时性要求降到"能及时取走"即可。
- 协议已经解出来了（报文 `[u32 头][u32 字数][数据…]`，通道 3，`FINDINGS-LEDGER` T15/T16）。
- 大核侧 Linux 的 `sunxi-msgbox` 驱动**已在树、`=y`、但没有客户端** —— 正好留给你写。

## 4. 代价必须说清

**E902 只有一颗。跑了你的固件，厂商 SCP 就没了。** 失去：

| 原本以为会失去 | 逐项查证结果 |
|---|---|
| ~~DRAM 变频~~ | ❌ **不成立**：本板没启用（DTS 无 dmc 节点，`/sys/class/devfreq` 只有 NPU） |
| ~~PMIC 看护~~ | ❌ **不成立**：**Linux 自己在管**（板级 DTS `&s_twi0` 下挂 `x-powers,axp515`/`axp8191`） |
| ~~看门狗~~ | ❌ **不成立**：CPUX 域独立硬件（`watchdog@2050000`，`CONFIG_AW_WATCHDOG=y`） |
| **待机/关机** | ⚠️ **可能真会丢**（SCP 里的 `standby service ok` / `24m hosc will power-off notify`）；**未验证** |
| 晶振/24M hosc 电源广播（晶振 26 MHz，T34）/ USB 隔离 | 未验证 |

**结论：替换 `scp.fex` 的代价比先前描述的小得多，主要风险集中在"待机/关机"。**
（详见 `E902-ROLE-AND-CAPABILITIES.md` §9）

## 5. 建议的顺序（**先测量，再决定**）

1. **先修板**（烧 `output/images/…-e902fw.img` 或原版 `.img`）。
2. **先做方案 A 的抖动实测**（半天，零风险）。用 p99 抖动判断你到底需不需要 E902。
   —— 很多 AHRS 场景在 PREEMPT_RT 下把时间戳放在 ISR 里取就够用了。
3. 如果确实需要 <5 µs 量级：走方案 C。先把"握手 + 心跳 + S_UART0 控制台"跑通（现在卡在握手），
   再加 SPI 与 DRDY 中断。
4. 每一层都先在**小核自己的串口**（TL101 `/dev/ttyUSB1`）上打印时间戳序列，再做统计。

## 6. 引脚规划（待与官方原理图核对）

| 用途 | SoC 引脚 | 备注 |
|---|---|---|
| E902 `S_SPI`（CLK/MOSI/MISO） | `PL4` / `PL5` / `PL6`（`s_spi0` 复用） | PL4 与 `s_jtag_ms` 共用，别同时开 |
| BMI088 CS1 / CS2 | `PL7`（`s_spi0`）或另找一个 GPIO | 两个片选需独立 |
| BMI088 DRDY → 中断 | 任选一个空闲 GPIO（`gpio_in` + `irq`） | CLIC 外部中断（手册 Table 12-2 里 GPIOL_S/NS = IRQ 25/26） |
| **E902 控制台** | `PL2` / `PL3` = `S_UART0` | **已被占用**（现在接到 TL101 的 FT232H） |

> ⚠️ 物理针脚号（40pin 上第几脚）**必须以官方 Zero 3W 原理图为准**
> —— 仓库里那份 dts 是从 Orange Pi 4 Pro 派生的，注释里的 `PIN_xx` 不可信（`pin-capability.md` §0 已说明）。
