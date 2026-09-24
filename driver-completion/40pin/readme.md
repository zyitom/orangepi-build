# 40-pin 头启用（Orange Pi Zero 3W / A733）

## 0. 机制：厂家已把 40-pin 功能做成「设备树覆层」，由 u-boot 施加

板上 `/boot/boot.cmd` 里：
```
for overlay_file in ${overlays}; do
  load ... dtb/allwinner/overlay/${overlay_prefix}-${overlay_file}.dtbo
  fdt apply ${load_addr} ...
done
```
`/boot/orangepiEnv.txt` 里有 `overlay_prefix=sun60i-a733`。

⇒ **要启用某个 40-pin 功能，只要在 `/boot/orangepiEnv.txt` 加一行 `overlays=...`，重启即可。**
不需要重编内核、不需要 `fdtput`、完全可回退（备份这一行就行）。

> 结论先行：**40-pin 这块「不用写驱动」**——pinctrl/gpio/i2c/pwm/uart 驱动都在内核里，
> 40-pin 的复用是「覆层把某个控制器/通道 `status=okay` + 用该功能已定义的 pinctrl 组」。
> 需要写/改代码的只有 SPI 那条（见 §4）以及 ADC/CE（见上级目录的 driver-completion）。

## 1. 可用覆层（板上 `/boot/dtb/allwinner/overlay/`，前缀 = `sun60i-a733-`）

| 覆层名 | 目标节点 | SoC 引脚（来自 DTS pinctrl） |
|---|---|---|
| `i2c0` | `twi0` | PB2 / PB3 |
| `i2c1` | `twi1` | PB7 / PB8 |
| `i2c2` | `twi2` | PE1 / PE2 |
| `i2c3` | `twi3` | PE3 / PE4 |
| `spi3` | `spi3` | PE0–PE4（含 spidev，CS0/CS1） |
| `uart2` | `uart2` | PB0 / PB1 |
| `uart6` | `uart6` | PE0 / PE1 |
| `uart7` | `uart7` | PL2 / PL3（s_uart0） |
| `uart8` | `uart8` | PL8 / PL9（s_uart1） |
| `pwm0`…`pwm9` | `pwm0_0`…`pwm0_9` | 见下表 |
| `spwm2` | `s_pwm0_2` | PL4 |
| `lcd` / `opizero3w-lcd` | 显示屏（DSI/LCD 面板） | — |

PWM 通道 → 引脚（来自 board DTS 的 pinctrl 定义）：
`pwm0→PB4`、`pwm1→PD1`、`pwm2→PD2`、`pwm3→PD3`、`pwm4→PD4`、`pwm5→PD5(风扇占用)`、
`pwm6→PD6`、`pwm7→PD7`、`pwm8→PB6`、`pwm9→PB7`。

## 2. 怎么用

```sh
# 板上（root）。先看有哪些可用，再启用（会自动备份 orangepiEnv.txt）
sh enable-overlay.sh                                   # 列出可用覆层
sh enable-overlay.sh i2c0 pwm3 uart2                    # 启用三个
printf ' \n' | sudo -S -p '' reboot
sh test-40pin.sh                                        # 验收
```
回退：脚本会打印 `cp -a /boot/orangepiEnv.txt.bak-… /boot/orangepiEnv.txt && reboot`。

## 3. 验收判据

| 启用 | 期望 |
|---|---|
| `i2cN` | 出现新 `/dev/i2c-X`（`i2cdetect -y X` 能扫到总线上挂的器件） |
| `uartN` | 出现新的 `/dev/ttyS<N>`，可用 `stty`/`cat` 收发 |
| `pwmN` | `/sys/class/pwm/pwmchip{0}/pwmN` 可 `export`、设 `period/duty_cycle` |
| `spi3` | 出现 `/dev/spidev3.0`、`/dev/spidev3.1` |
| 纯 GPIO | 任意未占用引脚：`/dev/gpiochip0`（=pio, 356 线）+ libgpiod `gpioset/gpioget` |

## 4. 各功能的额外前提

- **i2c / uart / pwm**：驱动已在内核（`twi`/`uart-ng`/`sunxi-pwm` 已在跑，`pwmchip0/10/20` 已在），覆层直接可用。
- **spi3**：需要厂商 SPI 驱动 `CONFIG_AW_SPI=y`（见上级 `config.fragment`）。若当前 `.config` 未开，
  则先合配置 + 重编内核，再启用 `spi3` 覆层，才会出 `/dev/spidev3.x`。
- **纯 GPIO**：无需任何覆层；`/dev/gpiochip0/1` 已在。建议装 `gpiod`（`apt install gpiod`）拿 `gpioinfo/gpioget/gpioset`。

## 5. 引脚名映射（重要提醒）

board DTS（`sun60i-a733-orangepi-zero3w.dts`）里有一段**被注释掉的 `gpio-line-names`**，
把物理排针号写进了 pio/r_pio 各 bank（例如 PB0→PIN_7、PB1→PIN_11、PB9→PIN_8、PB10→PIN_10、
PD10→PIN_24、PJ22→PIN_5、PJ25→PIN_18、PK0→PIN_12、PL6→PIN_13 …）。

⚠️ **但该文件头部写的是 `compatible = "xunlong,orangepi-4-pro"`** —— 这份 DTS 是从 **Orange Pi 4 Pro**
派生的，那段 PIN_xx 映射**可能对应 4 Pro 的排针布局、不一定等于 Zero 3W**。
**要拿到 Zero 3W 40-pin 的权威物理引脚图，请以 Orange Pi 官方 Zero 3W 原理图/引脚说明为准**，
再用 §1 的「覆层 → SoC 引脚」表把它对起来。（本文件只保证 §1 的 SoC 引脚是 DTS 里真实的。）

## 6. 风险

- 只改 `/boot/orangepiEnv.txt` 一行，重启生效；脚本自动备份并给回退命令。
- 覆层之间可能争用同一组引脚（例如 `uart2`=PB0/PB1 与 `i2c0`=PB2/PB3 不同组，安全；
  但 `spi3`=PE0–PE4 与 `uart6`=PE0/PE1、`i2c2`=PE1/PE2 **互相冲突**，不要同时启用）。
  下表是已知冲突，启用前先看：
  - `spi3`(PE0-PE4) ⚔ `uart6`(PE0/PE1) ⚔ `i2c2`(PE1/PE2) → 三选一。
  - `pwm9`(PB7) ⚔ `i2c1`(PB7/PB8) → 二选一。
  - `pwm8`(PB6) 与 i2s0 组相关，注意是否被占用。
