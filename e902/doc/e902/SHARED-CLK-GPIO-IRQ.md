# E902 与大核：底层时钟 / GPIO / 中断 的共享归属表

日期：2026-09-22 · 证据：A733 手册 v0.91 Table 12-2、`ccu-sun60iw2-r.c`、`sun60iw2p1.dtsi`、
板级 `sun60i-a733-orangepi-zero3w.dts`、以及本项目实测（T1/T10/T13/T16/T21）

> ⚠️ **2026-09-23 更正（FINDINGS-LEDGER T31/T34）**：本板晶振（DCXO）是 **26 MHz**，不是 24 MHz
> （原理图 Y1 = 26M、datasheet V1.01 §3.9.1、XO_CTRL[15:14]=2、clk_summary `dcxo 26000000`）。
> 文中所有真正 24 MHz 的时钟都来自 **pll-ref / SYS_CLK24M（R-CCU mux 4，实测 23.99–24.00 MHz）**，
> 它是从 26 MHz 晶振再生的 PLL。"24 MHz 晶振"是旧平台遗留说法，已就地修正。

---

## 一、时钟

**结论先说：E902 的核心时钟是独立的（DCXO 晶振直出，不经过大核的 PLL；本板晶振 26 MHz，见 T34）；但 CPUS 域的外设总线时钟可以配置成来自大核 PLL —— 那才是需要提防的共享点。**

| 时钟 | 父时钟表 | 共享? | 证据 |
|---|---|---|---|
| **`riscv`**（E902 核心门控，`RISCV_BGR` bit0） | **`"dcxo"`** | ❌ **独占** | `ccu-sun60iw2-r.c:183-185`（`SUNXI_CCU_GATE(riscv, "riscv", "dcxo", 0x021C, BIT(0))`） |
| `riscv-cfg`（`RISCV_BGR` bit1） | `"dcxo"` | ❌ 独占 | `:179-181` |
| `riscv-24m`（`0x07010210`） | `{dcxo, osc32k, iosc}` | ❌ 独占 | `:173-177` |
| `r-ahb`（CPUS 总线 `0x07010000`） | `{dcxo, rtc32k, iosc, **pll-peri0-200m**, **pll-peri0-300m**}` | ⚠️ **可选共享** | `:26-28` |
| `r-apbs0` / `r-apbs1` | `{dcxo, rtc32k, iosc, **pll-peri0-200m**, pll-ref}` | ⚠️ 可选共享 | `:30-35` |
| `r-spi`（**S_SPI**，`0x070150`） | `{dcxo, **pll-peri0-200m**, **pll-peri0-300m**, **pll-peri1-300m**, pll-ref}` | ⚠️ **可选共享** | `:107` |
| `r-timer0..3` | `{dcxo, rtc32k, iosc, **pll-peri0-200m**, pll-ref}` | ⚠️ 可选共享 | `:38` |
| **DCXO 晶振（物理源）** | 全芯片共用 | ✅ **共享** | **26 MHz**（原理图 Y1 / datasheet V1.01 §3.9.1 / XO_CTRL 实测，T34）；SCP 里有 `broadcast 24mhosc power-on ready` / `24m hosc will power-off notify` → 说明 **SCP 管着这个晶振（及 24M hosc 派生时钟）的开关** |

**实测**：`APBS1_CLK(0x07010010) = 0x04000000` → mux 位 `[26:24] = 4` → 查 `r_apbs_parents` 得 **`"pll-ref"`**。
`pll-ref` 在 `ccu-sun60iw2.c:39-51` 是一个 **NM PLL**（`CLK_HW_INIT("pll-ref", "dcxo", ...)`）：它从 26 MHz 晶振
**再生出 24 MHz**（不是直通晶振），实测 `clk_summary` 显示 24.000000 MHz。分频位 `[4:0] = 0` → /1。
所以 **S_UART0 的父时钟 = 24 MHz**（与固件里 `apbs1_rate()` 算出的 24 MHz 一致；
S_TIMER1 实测 23.993 MHz、共享时间戳实测 23.9999 MHz，T31）。

> **对 BMI088 的直接影响**：`r-spi` 的父表里有 **大核的 `pll-peri0/1`**。
> 如果 S_SPI 的 mux 落在它们上面，**大核一改那个 PLL（比如变频），SPI 时钟就会跟着漂 → BMI088 波特率变**。
> **必须把 `r-spi` 的 mux 固定到 `pll-ref`（24 MHz，R-CCU mux 4）**。
> **不能选 `dcxo`** —— 本板 dcxo 是 26 MHz，选它 SPI 会快 8.3%
> （v64 实测踩过：标称"500 kHz"实为 541 kHz，T31），并每次上电后读回校验。

---

## 二、中断

**结论：中断控制器是两套独立硬件（GIC ↔ CPUX；CLIC ↔ E902），但 CPUS 域每个外设的中断线在两边都可见。**

| 中断源 | ARM/GIC 视角 | E902/CLIC 视角 | 共享? |
|---|---|---|---|
| **PL/PM GPIO（GPIOL/GPIOM）** | `r_pio` 节点：**GIC_SPI 198 (GPIOL) + GIC_SPI 200 (GPIOM)**，且 `interrupt-controller` | **IRQ 25 GPIOL_S / 26 GPIOL_NS / 27 GPIOM_S / 28 GPIOM_NS** | ✅ **真共享**（两个域都能收同一条 GPIO 中断） |
| `S_UART0` | GIC_SPI **201**（`uart7`，status=**disabled**） | IRQ **29** | 共享线（Linux 侧未启用） |
| `S_UART1` | GIC_SPI 202（`uart8`，disabled） | IRQ 30 | 同上 |
| `S_TWI0/1/2` | GIC_SPI **203/204/205** | IRQ **31/32/33** | 共享线（s_twi0 被 PMIC 用） |
| `S_IRRX` | GIC_SPI 206（disabled） | IRQ 34 | 共享线 |
| `S_PWM` | GIC_SPI 207（`s_pwm0` status=**okay**） | IRQ 35 | 共享线 |
| **`S_SPI`** | **GIC_SPI 210**（`r_spi`，disabled） | **IRQ 38** | 共享线（同一外设两条中断线） |
| `S_TIMER0..3` | —（GIC 里的 `timer@3009000`=SPI 89 是 CPUX 域） | IRQ **20..23** | E902 侧用 |
| `S_WDT` | —（Linux 用 `watchdog@2050000` CPUX 域） | IRQ **19** | E902 侧另有 |
| `MSGBOX` | GIC_SPI 0（`msgbox@3004000`，disabled） | 39 / 48 / 49 … | 共享线 |
| `RTC` | GIC_SPI 196（`rtc@7090000`） | IRQ 24（RTC_ALARM） | 共享线 |

**⇒ 这是真正需要"约定"的共享点**：同一条外设中断线接到 GIC 与 CLIC 两边，
**谁使能、谁负责清中断**必须明确；两边都开会互相抢。

---

## 三、GPIO

| 项 | 事实 |
|---|---|
| 谁管 PL/PM | **Linux 的 `r_pio`**：`sun60iw2p1.dtsi:1374` `r_pio: pinctrl@7025000`，`reg=0x07025000..0x07025410`，`interrupts=<198>,<200>`，`gpio-controller` + `interrupt-controller` |
| 寄存器层 | **同一个 `PL_CFG0(0x07025000)`，大小核都能写** → **读改写竞争是真实存在的**（本项目 T10 实测踩过：把 PL2 当 GPIO 写后必须显式还原） |
| 板级占用（板 DTS 实测） | `PL0/PL1 = s_twi0`（**PMIC AXP515**）· `PL2/PL3 = uart7/s_uart0`（**E902 控制台**，Linux 侧 `uart7` disabled 故未生效）· `PL4 = s_irrx` 或 `s_pwm0_2` · **`PL7` 被某设备当 GPIO 用**（`gpios=<&r_pio PL 7>`）· `PL8/PL9 = uart8/s_uart1` + `husb311_int` · `PL10/PL11 = s_twi2` · `PL12/PL13 = s_twi1` |
| **空闲** | **`PL5`、`PL6` 在板级 DTS 里没有任何组引用** ← 对 E902 的 `s_spi0` 最友好 |

> **对 BMI088 的直接影响**：`s_spi0` 的四个信号在 **PL4/PL5/PL6/PL7** 上。
> - **PL5、PL6 空闲** → 可以直接用
> - **PL4** 在板级 DTS 里被 `s_irrx` / `s_pwm0_2` 两组引用（`s_irrx` 设备 disabled，但 `s_pwm0` 是 okay）→ **需确认 s_pwm0 是否真在用 PL4**
> - **PL7** 已被某个设备当 GPIO（`&r_pio PL 7`）→ **有冲突**
> - ⇒ 四个脚要凑齐 `s_spi0`，**PL4 与 PL7 的归属必须先与原理图/实际设备核对**

---

## 四、给 BMI088 方案的直接建议

1. **SPI 时钟**：把 `r-spi` 的 mux 定为 `pll-ref`（24 MHz，mux 4），**不要落在 `dcxo`（本板 26 MHz，会快 8.3%，T31），
   也不要落在大核 `pll-peri*`**，并在固件启动时读回 `R_SPI_CLK(0x070150)` 校验 mux 值。
2. **引脚**：优先 **PL5 / PL6**；PL4 与 PL7 的占用要先确认（板 DTS 里有引用）。
   若要 CS，另找空闲 GPIO，别和 PL7 抢。
3. **中断归属**：PL/PM 的 GPIO 中断在 **CLIC IRQ 25/26（GPIOL_S/NS）** 可用；
   但 Linux 的 `r_pio` 也在监听 **GIC_SPI 198/200**。**两边别同时对同一根脚开中断**，
   否则谁清谁不清会打架。若 DRDY 走 PL/PM，建议在 Linux 侧把该脚的 r_pio 中断关掉。
4. **E902 核心时钟独立**（26 MHz 晶振直出，非大核 PLL）→ 大核变频不影响小核时序，**这是好消息**。
