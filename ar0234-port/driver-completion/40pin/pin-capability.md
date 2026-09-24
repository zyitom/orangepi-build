# A733 引脚能力图（40-pin 能引出什么 / 在芯片哪里看）

## 0. 「在芯片的哪里看」——三个权威来源

| 来源 | 位置 | 说明 |
|---|---|---|
| **① pinctrl 驱动源码** | `kernel/orange-pi-6.6-sun60iw2/bsp/drivers/pinctrl/pinctrl-sun60iw2.c`（2722 行）+ `pinctrl-sun60iw2-r.c`（r 域 PL/PM） | 每个 `SUNXI_PIN(...)` 列出该引脚**所有复用功能**（`SUNXI_FUNCTION(mux值, "名字")`）。这是芯片侧的定义，最权威。 |
| **② 运行时 pinctrl 表** | 板上（root）：`/sys/kernel/debug/pinctrl/2000000.pinctrl/pinmux-functions` 与 `…/7025000.pinctrl/pinmux-functions` | 与①同源的活表：`function N: 名字, groups = [ 引脚… ]`。本文件 §2 就是把它按引脚反转得到的。 |
| **③ 板级原理图 / DTS** | `…/dts/allwinner/sun60i-a733-orangepi-zero3w.dts`（+ 官方 Zero 3W 原理图） | 把「SoC 引脚（PB7…）」对应到「40-pin 物理针脚号」。**注意该 dts 是从 Orange Pi 4 Pro 派生的**（头部 `compatible="xunlong,orangepi-4-pro"`），其注释里的 `PIN_xx` 不一定等于 Zero 3W 实际排针，物理针脚号务必以官方原理图为准。 |

⚠️ 芯片级能力（①/②）是**确定**的；物理针脚映射（③）需要原理图**核对**——这是本项目目前唯一缺的一环。

## 1. SoC 速览

- **主 pinctrl** `2000000.pinctrl`：gpiochip0，**GPIO 0–351**，bank **PB/PC/PD/PE/PF/PG/PH/PI/PJ/PK**（共 181 个注册引脚）。
- **r 域 pinctrl** `7025000.pinctrl`：gpiochip1，**GPIO 352–415**，bank **PL/PM**（常电域，含 RTC/PMIC/唤醒）。
- 每个普通引脚至少可作 `gpio_in` / `gpio_out` / `irq`（外部中断）；其余为复用功能。

## 2. 引脚 → 可用功能（反转自 ③ 的 live 表；只列非 gpio 复用）

### PB（40-pin 主力 bank）
```
PB0 : uart2 uart0 spi2 dsi lcd0 jtag
PB1 : uart2 uart0 spi2 lcd0 jtag
PB2 : uart2 spi2 lcd0 jtag hdmi twi0
PB3 : uart2 spi2 lcd0 jtag hdmi twi0
PB4 : spi2 lcd0 hdmi pwm0_0 i2s0_mclk trace twi1
PB5 : spi2 lcd0 trace twi1 i2s0_bclk pwm0_1
PB6 : spi2 trace clk i2s0_lrck pwm0_2 pwm0_8
PB7 : trace twi1 clk i2s0_dout0 i2s0_din1 pwm0_9 owa0
PB8 : trace twi1 clk owa0 i2s0_din0 i2s0_dout1 pwm1_0
PB9 : uart0 lcd0 twi0 i2s0_din2 i2s0_dout2 pwm1_1 watchdog twi8
PB10: uart0 lcd0 twi0 twi8 test i2s0_din3 i2s0_dout3 pwm1_2 pll
```

### PD
```
PD0-2 : lcd0 pwm0_0/1/2 lvds0 dsi0 eink
PD3-7 : lcd0 lvds0 dsi0 eink pwm0_3/4/5/6/7
PD8-9 : lcd0 pwm0_8/9 lvds0 dsi0 eink
PD10-12: lcd0 pwm1_0/1/2 eink lvds1 dsi1 spi1
PD13 : lcd0 eink lvds1 dsi1 spi1 pwm1_3
PD14-15: lcd0 eink lvds1 dsi1 spi1 uart3
PD16-17: lcd0 eink lvds1 dsi1 uart3 twi2
PD18 : lcd0 eink lvds1 dsi1 spi1 uart4
PD19 : lcd0 eink lvds1 dsi1 uart4 pwm1_5
PD20 : dsi lcd0 twi0 pwm0_2 eink uart4 pcie twi3
PD21 : lcd0 twi0 eink pwm0_3 uart4 pcie twi3
PD22 : twi0 eink pwm0_4 twi2 pcie pwm1_4
PD23 : twi0 pwm0_5 twi2 pwm1_5 pcie
```

### PE
```
PE0 : mcsi0 csi0 spi3 lpc ncsi0 uart6
PE1 : twi2 spi3 lpc ncsi0 uart6 csi1
PE2 : twi2 csi0 spi3 lpc ncsi0 uart6
PE3 : pwm0_0 twi3 spi3 lpc ncsi0 uart6 csi1
PE4 : pwm0_1 twi3 spi3 lpc ncsi0 uart6 ledc
PE5 : pwm0_2 pll lpc ncsi0 uart6 mcsi1
PE6 : hdmi clk lpc ncsi0 uart6 i2s3_din0 i2s3_dout1
PE7 : hdmi clk lpc ncsi0 i2s3_bclk twi11
PE8 : hdmi clk lpc ncsi0 twi11 i2s3_lrck
PE9 : lpc ncsi0 uart6 mcsi2 tcon i2s3_mclk
PE10: lpc ncsi0 tcon twi4 i2s3_din3 i2s3_dout3 uart1
PE11: pcie spi3 ncsi0 twi4 uart1 i2s3_din2 i2s3_dout2
PE12-13: pcie spi3 uart1 (+PE12: i2s3_dout0/din1)
PE14: pcie spi3 pwm1_8 ir twi9
PE15: spi3 ir twi9 pwm1_9
```

### PJ
```
PJ0-9  : lcd1 lvds2 rgmii1
PJ10-15: lcd1 rgmii1 lvds3
PJ16-17: uart5 twi7 lcd1 lvds3 (+i2s4_bclk/mclk)
PJ18   : pwm0_0 twi4 uart5 lcd1 lvds3 i2s4_lrck
PJ19   : twi4 uart5 lcd1 lvds3 i2s4_din0 i2s4_dout1
PJ20   : lcd0 uart3 twi12 lcd1 i2s4_dout0 i2s4_din1 twi10
PJ21   : lcd0 pwm1_3 uart3 spi3 twi12 lcd1 twi10
PJ22   : uart2 lcd0 uart3 twi3 pwm1_4 twi11 twi7 lcd1
PJ23   : uart2 lcd0 uart3 pwm1_5 twi3 twi11 twi7 lcd1
PJ24-25: uart4 spi3 twi4 pwm1_6/1_7 lcd1
PJ26   : uart2 lcd0 uart4 spi3 pwm1_8 twi5 lcd1
PJ27   : uart2 dsi lcd0 uart4 spi3 pwm1_9 twi5 lcd1
```

### PK
```
PK0-3 : hdmi twi1 uart6 sgpio i2s4_* (mcsia ncsi1)
PK4-5 : pcie spi3 jtag i2s4_* / pwm1_8 pwm1_9 (mcsia ncsi1)
PK6-9 : uart2 jtag twi2 uart4 spi3 uart4 mcsi* (mcsia ncsi1)
PK10-15: uart6 pwm0_3/4/5/6/7/8 ncsi1 mcsib (+PK14: pcie)
PK16-19: uart2 pwm0_9/pwm1_0/1_1/1_2 twi3 mcsi* twi9 ncsi1 mcsib
PK20-23: uart3 twi2 uart1 pwm0_1/0_2/pwm1_6/1_7 ncsi1 mcsic
PK24-25: pwm0_6/0_7 mcsi0/1 twi12 twi10 mcsic
```

### PL / PM（r 域，常电；含 RTC/PMIC/唤醒）
```
PL0-3 : s_twi0/1/2 (+PL2/3: s_uart0/1, s_pwm0_0/1, PL1/3: s_ir_rx)
PL4   : s_twi2 s_ir_rx s_jtag_ms s_spi0 s_pwm0_2
PL5   : s_twi2 s_spi0 s_jtag_ck s_pwm0_3
PL6   : s_ir_rx s_uart0 s_spi0 s_jtag_do s_pwm0_4
PL7   : s_uart0 s_spi0 s_jtag_di s_pwm0_5
PL8-9 : s_twi0/1/2 s_uart1 s_pwm0_6/0_7
PL10-13: s_twi2/s_twi1 s_uart1 s_pwm0_8/0_9 s_ir_rx
PM0-5 : s_twi1/2 s_spi0 s_jtag_* s_uart0/1 s_pwm0_0…0_5 s_ir_rx s_rjtag_*
```

### 其余 bank（一般不在 40-pin，作为参考）
- `PC`：`nand / sdc2 / sdc3 / spi0 / spif0`（eMMC/SD/SPI-NOR 用途）
- `PF`：`sdc0 / uart5 / twi2 / trace / jtag`（SD 卡）
- `PG`：`sdc1 / lcd0 / lpc / i2s1 / dmic / twi0/6/9/12 / sgpio / hdmi / pwm1_*`
- `PH`：`rgmii0 / spi1 / spi2 / uart3/5 / twi0/1/3/5/6/7 / i2s2 / dmic / ledc / ir`
- `PI`：`uart0/3/4/5/6 / spi4 / twi2/3/4/5/8/11 / pwm0_*/1_* / owa0 / ir / pcie`

## 3. 怎么用这张表（配合 40-pin）

1. 用官方 **Zero 3W 原理图**把「物理针脚号 → SoC 引脚（PB7 等）」对上。（本仓库 dts 里的 `PIN_xx` 注释来自 4 Pro，仅供参考。）
2. 从 §2 查出该 SoC 引脚**能当什么**（例如 PB7：`twi1/i2s0/pwm0_9/owa0/gpio`）。
3. 选一种功能 → 用对应**覆层**（`enable-overlay.sh <name>`）启用，或直接当 GPIO（`/dev/gpiochip0`）。
4. 注意**同引脚只能选一种功能**；多引脚功能（如 `spi3` 占 PE0–PE4）要整组看。

## 4. GPIO 编号换算

- 主域：`GPIO# = bank_index*32 + pin`，bank 顺序 **B=1,C=2,D=3,E=4,F=5,G=6,H=7,I=8,J=9,K=10**（A=0 未用）。
  例：PB7 → `1*32+7 = 39`；PE0 → `4*32+0 = 128`。
- r 域（gpiochip1，352 起）：PL0 = 352，PM0 = 384（bank 顺序 L=0, M=1 → 11*32=352, 12*32=384）。
- 命令行：`gpiodetect` / `gpioinfo`（需 `apt install gpiod`），或 sysfs `/sys/class/gpio`。
