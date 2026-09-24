# 小核控制台通道已打通并验证（2026-09-22）

## 结论

**E902 的 `S_UART0` 控制台是可以直接看到的**，方式是**在板子上读它自己的 `/dev/ttyUSB0`**。

接线形态（本轮实测确认）：一块 **FT232H USB2TTY** 插在 **OrangePi 3W 板子自己的 USB 口**上，
它的串口侧接到排针 **pin16(PL2, SCPU-TX) / pin18(PL3, SCPU-RX)**。
于是"板子上的 Linux"就是小核控制台的观测终端——不需要额外主机。

设备身份（板子上实测）：

```
Bus 005 Device 010: ID 0403:6014 Future Technology Devices International, Ltd FT232H Single HS USB-UART/FIFO IC
crw-rw---- 1 root dialout 188, 0 /dev/ttyUSB0
```

> 注意：TL101 上的 `/dev/ttyUSB0` 是**另一回事**——那是 CH340，接的是板子的大核 Linux 控制台
> （PB4/PB5，敲回车会出 `orangepi@orangepizero3w:~$`）。两个 ttyUSB0 不是同一个东西，别混。

## 验证方法（不是推断，是实测）

把 PL2（= 排针 pin16）临时当 GPIO 输出、以约 2 Hz 翻转，同时抓板子的 `/dev/ttyUSB0`：

| 阶段 | 抓到字节数 |
|---|---|
| 基线（板子空闲，什么都不做） | **0** |
| 翻转 PL2 期间（10 次高/低） | **10** |

基线 0 字节 + 有信号时收到 10 字节 ⇒ **FT232H 的 RX 确实挂在 PL2 上**。
测试后 `PL_CFG0` 已还原为 `0x1FFF1F22`（PL2 由临时输出位改回原值）。

同一时刻读回 `PL_CFG0 = 0x1FFF1F22`：PL2 nibble = `0xF`、PL3 nibble = `0x1`
—— 说明**当前没有任何一方把这两根脚复用成 UART**（`S_UART0` 的复用本来就由我们的固件在
`pinmux.c` 里写功能 3 时才建立；厂商 SCP 与 Linux 都不碰这两个 nibble）。

## 加载固件时的控制台取证（决定性）

开着控制台抓取，同时用 `e902-load.sh` 加载 v2 固件（固件若执行会先把 PL2/PL3 复用到
`S_UART0` 并打印 banner）：

```
bytes received from the E902 console during load: 0
(silent -- the core never reached our firmware)

PL_CFG0  = 0x1FFF1F22   (would be ...33.. if our pinmux had run)
RST_START= 0x40020000
r_spi GC = 0x00000000   (0x5 would mean our spi_init ran)
4001e000  00 00 00 00 00 00 00 00     <- SRAM 心跳块全 0
```

**三个完全独立的观测通道同时给出同一结论**：
① E902 自己的串口（0 字节）、② SRAM 心跳块（全 0）、③ 外设寄存器（`r_spi` 未被编程、`PL_CFG0` 未被改）。
内核**没有**从我们设置的向量取指。这条结论从此不再依赖任何单一证据。

## 对后续的意义

以前"小核有没有跑"只能靠 SRAM 心跳间接判断；现在**有真正的控制台**：

- 一旦 bl31 路线（见 `BL31-PATH-RUNBOOK.md`）成功，banner `=== A733 E902 firmware v2 ===` 会直接出现在
  板子的 `/dev/ttyUSB0` 上，肉眼可读；
- 小核固件跑飞时 `trap_entry` 的行为、`mcause/mepc` 的串口打印也能直接看到；
- 中断计数、SPI 结果等后续验收项都可以用这条通道直接取证。

## 观测命令（在板子上执行）

```bash
# 板子上
sudo stty -F /dev/ttyUSB0 115200 cs8 -cstopb -parenb -crtscts raw -echo
sudo cat /dev/ttyUSB0            # 或 sudo picocom -b 115200 /dev/ttyUSB0
```
