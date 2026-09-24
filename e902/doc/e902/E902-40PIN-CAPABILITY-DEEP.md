# 深度分析：40-pin 上「对 E902 能配多少外设」

日期：2026-09-22 · 证据：官方手册 §3.14/§3.16（40pin 引脚表，OCR 取得）、手册 Table 12-2、
`pinctrl-sun60iw2-r.c`、`ccu-sun60iw2-r.c`、厂商 SCP 反汇编足迹

---

## 核心结论（一句话）

**40-pin 上，E902 用「自己原生的外设」只能配到很少几个（4 根 PL 脚，且没有 SPI）；
但如果走「跨域驱动」——去操作大核（CPUX 域）的控制器——那 40-pin 上的外设理论上**全部**可用，
而且连中断都能送给它（靠 **GINTC** 桥）。**

---

## 路径 1：E902 原生 CPUS 域外设（必须落在 PL 脚上）

40-pin 全表里，**r 域（PL）只有 4 根脚**：**PL2=16 / PL3=18 / PL4=28 / PL5=27**。

它们的**全部**复用功能（逐条抄自 `pinctrl-sun60iw2-r.c` 的 `SUNXI_PIN` 定义）：

| 引脚 | 针脚 | 可复用为 |
|---|---|---|
| **PL2** | 16 | gpio_in / gpio_out / s_uart1 / **s_uart0** / s_twi1 / s_pwm0_0 / **eint** / io_disabled |
| **PL3** | 18 | gpio_in / gpio_out / s_uart1 / **s_uart0** / s_twi1 / s_ir_rx / s_pwm0_1 / **eint** / io_disabled |
| **PL4** | 28 | gpio_in / gpio_out / s_jtag_ms / s_twi2 / **s_spi0** / s_ir_rx / s_pwm0_2 / **eint** / io_disabled |
| **PL5** | 27 | gpio_in / gpio_out / s_jtag_ck / s_twi2 / **s_spi0** / s_pwm0_3 / **eint** / io_disabled |
| *(PL6)* | **不在排针** | … / s_uart0 / **s_spi0** / s_ir_rx / s_pwm0_4 / eint |
| *(PL7)* | **不在排针** | … / s_uart0 / **s_spi0** / s_pwm0_5 / eint |

### ⇒ 路径 1 实际能配出的外设

| 外设 | 能否 | 说明 |
|---|---|---|
| **S_UART0** | ✅ 1 路（2 线） | 占 PL2+PL3 —— **就是现在我们的 E902 控制台** |
| **S_TWI** | ✅ 1 路 | `s_twi1` 在 PL2/PL3；`s_twi2` 在 PL4/PL5（二选一，因为同脚） |
| **S_PWM0** | ✅ 最多 4 路 | pwm0_0(PL2) / 0_1(PL3) / 0_2(PL4) / 0_3(PL5) |
| **S_IRRX** | ✅ 1 路 | PL3 或 PL4 |
| **S_SPI0** | ❌ **凑不齐** | 需要 PL4+PL5+**PL6**+**PL7**，而 PL6/PL7 **不在排针上** |
| **外部中断 (eint)** | ✅ **4 路** | PL2/PL3/PL4/PL5 都能当 eint → E902 的 CLIC **IRQ 25 GPIOL_S / 26 GPIOL_NS** |

**⇒ 原生路径：只有 UART×1 + I2C×1 + PWM×4 + IR×1 + 4 个外部中断；没有 SPI。**

---

## 路径 2：E902 跨域驱动 CPUX 域外设（**这才是关键**）

### 2.1 E902 能不能访问 CPUX 域？

**能** —— 实锤来自厂商 SCP 自己的足迹（T14 静态提取）：它在访问
`0x03004000`（**CPUX_MSGBOX**）、`0x02002744`/`0x02003300`（**主 CCU**）。
即 **CPUS 核可以读写 CPUX 域的寄存器和控制器**。

### 2.2 CPUX 域外设的中断能不能送给 E902？

**能 —— 靠 GINTC**。手册 Table 12-2 的尾部（IRQ 54–77）写得非常明确：

```
54   riscv_sys_irq_i[6]    <- GINTC_CONFIG_REG0[7:0]    can mask gic irq [39:32]
55   riscv_sys_irq_i[7]    <- GINTC_CONFIG_REG0[15:8]   can mask gic irq [47:40]
56   riscv_sys_irq_i[8]    <- GINTC_CONFIG_REG0[23:16]  can mask gic irq [55:48]
...
77   riscv_sys_irq_i[29]   <- GINTC_CONFIG_REG5[31:24]  can mask gic irq [223:216]
```

⇒ **有 24 条桥（CLIC 54–77），每条可选一路 GIC 中断（raw GIC ID 32–223）转发给 E902。**
换言之：**任何 GIC 中断号落在 32..223 的 CPUX 域外设，都能被路由成 E902 的中断。**

对照 40-pin 上的外设（GIC 中断号来自 dtsi）：

| 40-pin 外设 | GIC_SPI | raw GIC ID (=+32) | 可桥接给 E902？ |
|---|---|---|---|
| **SPI3** | 27 | **59** | ✅ |
| I2C0 (twi0) | ? | ? | ✅（区间内） |
| UART2 / UART6 | ? | ? | ✅ |
| PWM0 | ? | ? | ✅ |
| PL/PM GPIO | 198 / 200 | 230 / 232 | ❌ 超出 223（但**本来就有专属 CLIC 线 25–28**） |

### 2.3 E902 还有 DMA！

CLIC **IRQ 50–53 = `DMAC0_CPUS_NS/S`、`DMAC1_CPUS_NS/S`** ⇒ **E902 有两路 DMA，可做无 CPU 参与的批量搬运**
（对高频 IMU 采样非常有用）。

### ⇒ 路径 2 实际能配出的外设（40-pin 全清单）

| 类别 | 可用数量 | 针脚 |
|---|---|---|
| **SPI** | 1 路（SPI3） | 19(MOSI) / 21(MISO) / 23(CLK) / 24(CS0) / 26(CS1) |
| **I2C** | 最多 4 路（但引脚与 SPI3/UART6 冲突） | I2C0:3/5 · I2C1:38/40 · I2C2:19/23 · I2C3:26/21 |
| **UART** | 3 路（UART2/6/7；**UART7 已被 E902 控制台占用**） | uart2:13/11 · uart6:23/24 · uart7:18/16 |
| **PWM** | 7 路 | 7/32/36/33/37/35/40 |
| **GPIO** | **28 个**（3.3V） | 全表 |
| **外部中断** | 4 路（PL2/3/4/5 的 eint） | 16/18/28/27 |
| **DMA** | 2 路（DMAC0/1 的 CPUS 通道） | 内部资源，不占引脚 |

---

## 三条硬约束（必须说清）

1. **互斥**：跨域驱动某个控制器，就意味着 **Linux 不能同时启用它**（否则两边抢寄存器/中断）。
   例如用 SPI3 → `/boot/orangepiEnv.txt` 里**不要**加 `overlays=spi3`。
2. **谁配 pinctrl**：CPUX 域引脚的 mux 在 `2000000.pinctrl` 里。E902 要驱动 SPI3，就得**自己去写那块的寄存器**
   （或让 Linux 先把脚 mux 好并保持不驱动控制器）。
3. **时钟可能共享**：SPI3 的时钟来自**主 CCU 的 `pll-peri*`**，而主 CCU 是大核在管
   （`CLK_SPI3`/`RST_BUS_SPI3`）。**大核改 PLL 会影响 SPI3 波特率** → 需要把 mux 定到稳定源并开机校验。

---

## 对你 BMI088 的最终建议

| 方案 | 外设能力 | 评价 |
|---|---|---|
| **A. 大核 SPI3 + spidev** | 1 路 SPI + 任意 GPIO 做 DRDY | 最稳、今天可做；先量 p99 抖动 |
| **B. E902 跨域驱动 SPI3 + DRDY 走 PL4/PL5** | 同 A，但实时性由小核保证 | ⭐ 满足你的诉求；**待验证**（E902 能否驱动 CPUX SPI） |
| C. E902 原生 S_SPI | — | ❌ PL6/PL7 不在排针 |

**验证方案 B 的最小实验（板子恢复后第一件事）**：
1. 从 E902 读 `0x02543000`（SPI3）的 `VER` 寄存器 → 非 0 即证明"能访问"
2. 关掉 Linux 的 spi3 覆层，从 E902 配好 `GC/TC/CLK_CTL` 并发一次 1 字节传输
3. DRDY 接 PL4(28)，E902 用 CLIC IRQ 25/26 收中断，读 S_TIMER 打时间戳
