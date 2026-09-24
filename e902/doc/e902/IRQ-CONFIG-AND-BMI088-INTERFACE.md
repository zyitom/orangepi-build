# E902 中断配置三条路径 + BMI088 接口选择

日期：2026-09-22 · 证据：A733 手册 Table 12-2 / §12.3.3、`pinctrl-sun60iw2-r.c`、`pinctrl-sunxi.h`、
本项目固件 `clic.c`、Bosch BMI088 数据手册（经网络检索确认接口能力）

---

## 0. 先划清"确定"与"未证实"

| 结论 | 状态 |
|---|---|
| 40-pin 的 40 个脚、可用功能（SPI3/I2C0-3/UART2,6,7/PWM0-x/28 GPIO） | ✅ **确定**（官方手册 §3.14/§3.16，OCR + 文本双来源） |
| 40-pin 上 PL 只有 4 根（PL2/3/4/5），`s_spi0` 要 PL4–PL7 ⇒ 引不出来 | ✅ **确定**（官方引脚表 + `pinctrl-sun60iw2-r.c` 逐条 mux） |
| E902 能访问 CPUX 域寄存器 | ✅ **确定**（厂商 SCP 实测足迹含 `0x03004000`、主 CCU `0x02003300`） |
| **E902 能驱动 CPUX 域的 SPI3** | ⚠️ **未实测**（有跨域先例、但 SPI3 没试过） |
| GINTC 能把 GIC 中断桥给 CLIC | ⚠️ **手册写了寄存器，但基址未找到**；未实测 |
| PL/PM GPIO 中断 → CLIC IRQ 25/26 | ✅ 中断号确定（手册 Table 12-2）；⚠️ EINT 寄存器确切偏移待读 |
| BMI088 支持 SPI **或** I2C | ✅ 数据手册原文 *"fitted with digital interfaces (SPI or I2C)"* |

---

## 1. 硬件中断配置 —— 三条路径

### 路径 A：E902 收**自己域**的中断（CLIC）—— 最简单

**CLIC 基址 `0xE0800000`**（手册 §12.3.3 明确列出）。寄存器：

| 寄存器 | 偏移 | 说明 |
|---|---|---|
| `CLICCFG` | `0x0000` | nlbits（优先级有效位） |
| `CLICMINTTHRESH` | `0x0008` | 优先级门限 |
| `CLIC_INT_REGn` | `0x1000 + 4*N`（N=0..63） | 每条中断 4 字节 |

每条中断 4 字节：`INTIP(+0)` 挂起 / `INTIE(+1)` 使能 / `INTATTR(+2)` 向量与触发方式 / `INTCTL(+3)` 优先级。

**配置步骤**（我们固件的 `e902-fw/src/clic.c` 已实现）：
1. `CLICCFG` 设 nlbits
2. `CLICMINTTHRESH = 0`
3. 目标 IRQ：`INTATTR` 写「向量位 + 触发方式(bit2:1)」→ `INTCTL` 写优先级 → 清 `INTIP` → `INTIE = 1`
4. 向量表：`csrw mtvt(0x307), <向量表基址>`（向量模式）；非向量模式走 `mtvec`

**中断号从哪来**：手册 Table 12-2 —— `S_TIMER0-3`=20–23、`GPIOL_S/NS`=25/26、`GPIOM_S/NS`=27/28、
`S_UART0`=29、`S_TWI0-2`=31–33、`S_PWM`=35、`S_SPI`=38、`S_MBOX 收`=48、`DMAC0/1_CPUS`=50–53。

### 路径 B：E902 收 **PL/PM 引脚**的外部中断

1. **引脚 mux**：把 PLx 设成 `eint`（mux 值 `0xE`）。PL 的 mux 在 `PL_CFG0/PL_CFG1`（`0x07025000`/`0x07025004`），**每脚 4 位**。
   例：**PL4 在 `PL_CFG0` 的 bit[19:16]**、**PL5 在 bit[23:20]**、PL2 在 bit[11:8]、PL3 在 bit[15:12]。
   （依据：官方 40-pin 表 + `PL_CFG0 = 0x1FFF1F22` 时 PL2 的 nibble 恰为 `2`，与实测一致）
2. **触发方式/使能/挂起**：r_pio 的 EINT 寄存器组，位于 `r_pio` 的 reg 窗口内
   （`0x07025000..0x07025410`；驱动里 `IRQ_CFG_IRQ_PER_REG = 8`、`IRQ_CFG_IRQ_BITS = 4`，
   即 **每 8 根脚一个 CFG 寄存器、每脚 4 位**；另有 enable 与 status 寄存器）。
   > ⚠️ **该 SoC 的确切偏移我还没读出来**（`sunxi_pinctrl_hw_info[SUNXI_PCTL_HW_TYPE_4]` 未定位到）。
   > 板子恢复后可从 `/sys/kernel/debug/pinctrl/7025000.pinctrl/` 或直接读寄存器确认。
3. **中断号**：CLIC **IRQ 25 = `GPIOL_S`** 或 **IRQ 26 = `GPIOL_NS`**
   （我们固件 `a733.h` 里已有 `IRQ_GPIOL_S 25` / `IRQ_GPIOL_NS 26`）
4. ⚠️ **必须与 Linux 互斥**：Linux 的 `r_pio` 也在监听 **GIC_SPI 198/200**。同一根脚两边都开中断会互相抢。

### 路径 C：E902 收**大核外设**的中断（GINTC 桥）

手册 Table 12-2 的 IRQ 54–77 = `riscv_sys_irq_i[6..29]`，由 `GINTC_CONFIG_REG0..5` 各 8 位选源，
可屏蔽/映射 **GIC 中断号 32..223**。

- 例：SPI3 的 `GIC_SPI 27` → raw ID **59** → 在 32..223 内 → **可桥接**
- > ⚠️ **GINTC 的基址我没找到**（手册摘录里只有寄存器名，没有寄存器列表）。这是明确的缺口。
- **备选方案**：**轮询**。E902 直接读 SPI3 的 `INT_STA(0x14)` 寄存器判断传输完成 —— 不依赖中断。
  对 BMI088 这种"DRDY 触发 → 固定长度突发读"的模式，**轮询完全够用**（而且延迟更可预测）。

---

## 2. BMI088 走 I2C 行不行？

| | SPI | I2C |
|---|---|---|
| BMI088 是否支持 | ✅ | ✅（数据手册："SPI or I2C"） |
| 需要几根线 | **CS×2** + CLK/MOSI/MISO = 5 | SDA/SCL = 2 |
| 40-pin 上有吗 | ✅ **SPI3 的 CS0=24 / CS1=26 正好两个片选** | ✅ 4 路 I2C |
| 速率 | 高（SPI 可达 MHz 量级） | 低（I2C 上限远低于 SPI，且有时钟拉伸） |
| 与 SPI3 共存 | — | ❌ **I2C2(19/23)/I2C3(26/21) 与 SPI3 共用引脚**，二选一 |
| 高频采样适用性 | ⭐ 更适合 | ⚠️ 2 kHz × 12 字节 ≈ 24 kB/s，I2C 会很吃紧 |

**关键点**：BMI088 的加速度计和陀螺仪是**两颗独立 die**，SPI 模式下**各有自己的 CS**。
**"只有一路 SPI"完全够用** —— 因为本来就是"**一条 SPI 总线 + 两个片选**"。
而 40-pin 的 SPI3 **恰好提供 CS0(24) 和 CS1(26)** → 天然匹配。

**⇒ 结论：用 SPI3 比 I2C 好**（一个控制器解决，两个 CS 现成，速率高得多）。
I2C 只有在"SPI3 引脚被别的用途占了"时才有必要，且要接受速率与抖动的代价。

---

## 3. BMI088 在 40-pin 上的推荐接法

| BMI088 信号 | 接到 | 说明 |
|---|---|---|
| SCLK | **针脚 23** | SPI3-CLK（PE1） |
| SDI/MOSI | **针脚 19** | SPI3-MOSI（PE2） |
| SDO/MISO | **针脚 21** | SPI3-MISO（PE3） |
| **CSB1（加速度）** | **针脚 24** | SPI3-CS0（PE0） |
| **CSB2（陀螺仪）** | **针脚 26** | SPI3-CS1（PE4） |
| **INT3/DRDY（加速度）** | **针脚 27 或 28**（PL5 / PL4） | 若走 E902：这两根能做 CLIC 外部中断 |
| GND | 6/9/14/20/25/30/34/39 | |
| VDD/VDDIO | 1 或 17（3.3V） | BMI088 VDDIO 支持 1.2–3.6V |
| （可选）INT4 | 15(PE11) / 29(PE12) / 31(PE13) / 12(PB5) | 另一个中断脚 |

**注意冲突**：19/21/23/24/26 同时也是 I2C2/I2C3/UART6 的脚 —— **一次只能启用一种功能**。
