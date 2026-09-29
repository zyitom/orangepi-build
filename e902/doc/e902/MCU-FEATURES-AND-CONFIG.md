# E902/CPUS 域的 MCU 类功能：有什么、怎么测、怎么配（2026-09-25）

A733 的 CPUS 域里有一颗玄铁 E902（SCP），出厂固件（Tina arisc）负责电源、休眠、DRAM 调频、
定时器、时钟；Linux 侧能感知到的"MCU 类"功能是下面这几个。本文只讲**当前板上实际可用的东西**
（厂商固件 + RT 内核 6.6.98-rt58），自研固件阶段的原始测量见 `FINDINGS-LEDGER.md` T31–T37。

## 1. 共享时间戳（CONFIG_AW_AMP_TIMESTAMP，补丁 0014）

CPUX 和 E902 读同一个 64 位自由运行计数器（`TIMESTAMP_STA @ 0x08010000`，24 MHz，
分辨率 41.7 ns）。两边时间戳直接可比，不需要对时协议。E902 固件给事件打的就是这个时间。

```bash
cat /sys/bus/platform/devices/8010000.amp-timestamp/counter   # 原始计数
cat /sys/bus/platform/devices/8010000.amp-timestamp/usec      # 微秒
cat /sys/bus/platform/devices/8010000.amp-timestamp/freqid    # 0 → 用 DT 里的 clock-frequency
```

内核驱动（AR0234 帧时间戳就是这么打的）：

```c
#include <linux/amp_timestamp.h>
void *ts; u64 now;
if (!amp_ts_get_dev(0, &ts))
        amp_ts_get_timestamp(ts, &now);
```

配置来源：补丁 `userpatches/kernel/sun60iw2-current/0014`（驱动 + `sun60iw2p1.dtsi` 的
`amp-timestamp@8010000` 节点 + Kconfig），a733 内核配置 `CONFIG_AW_AMP_TIMESTAMP=y`。
注意 `CNT_FREQID_REG`（0x08020020）两个核读都是 0，频率以 DT 的 `clock-frequency = <24000000>`
为准（实测）。跨域读取本身约 1 µs（T35：ARM 973–1128 ns，E902 1083 ns）。

## 2. TRNG 硬件随机数（CONFIG_AW_TRNG=m，补丁 0015）

sunxi_trng 是 CPUS 域的真随机数块，通过标准 hwrng 框架暴露：

```bash
cat /sys/class/misc/hw_random/rng_available     # 应列出 sunxi_trng
echo sunxi_trng > /sys/class/misc/hw_random/rng_current
dd if=/dev/hwrng bs=4096 count=64 | od -x | head  # 读数 + 看吞吐
```

要长期喂给内核熵池就跑 `rngd`（`apt install rng-tools5`，`-r /dev/hwrng`）。

## 3. LRADC 按键（CONFIG_AW_LRADC=m，补丁 0015）

LRADC 在 CPUS 域（手册 S_ 外设）。BSP 驱动把它同时暴露成 IIO 设备和 input 按键：

```bash
cat /sys/bus/iio/devices/iio:device*/name        # sunxi-lradc
cat /sys/bus/iio/devices/iio:device0/in_voltage_raw   # 0–63 的原始键值
evtest                                           # 有按键接 LRADC0 时看 input 事件
```

按键阈值/去抖在 DT 的 lradc 节点（`2524000.lradc`）里配；设备树默认模板在
内核 `sun60iw2p1.dtsi`。本板没接按键，读数应悬空在满量程附近。

## 4. GPADC（CONFIG_AW_GPADC=m）

通用 ADC，IIO 暴露，用法同上（`gpadc` 名字的 iio:device）。注意：厂商 BSP 的 GPADC
中断线程空闲时也会周期跑（`ps` 里 `[irq/N-sunxi-gpadc]` 有累计 CPU 时间），是轮询式的，
不是故障。

## 5. mailbox / 传输

- **厂商固件路径**：mailbox ch3 由 bl31 独占（SCPI 式 RPC：休眠、DRAM 参数等），**Linux
  应用层没有直接的 mailbox 接口**，也不该碰 ch3（bl31 无超时，乱发包会挂死，台账 T29/T30）。
- **Linux rpmsg**：A733 BSP 没走 rpmsg；`AW_RPMSG_*` 在 a733 配置里全关，打开也要有固件侧
  配合（厂商 arisc 无 rpmsg 服务端）。补丁 0015 只是让这些驱动在 6.6 下**能编译**。
- **实测能力（自研固件 v68，T35）**：mailbox 往返 1 字中位 4.1 µs（24 万次/s，p99.9 5.6 µs）、
  16 字中位 18.1 µs、单向 3.4 MB/s；共享 SRAM A2 ARM 侧写 141–148 MB/s、读 28–53 MB/s；
  E902 SRAM 拷贝 ≈95 MB/s、经 0x80000000 窗口读 DRAM ≈12.5 MB/s、外设寄存器读 70 ns。
  要复现这些数字需要换回自研固件（`e902-fw/`，见 `FLASHING.md`），厂商固件没有测试键。
- **E902 中断延迟（自研固件）**：定时器到期→ISR 体 0.04–0.38 µs，Linux 满载不变。

## 6. 硬件中断（E902 侧视角，手册 V1.00 Table 12-2 + irqnum_config.h）

E902 可用的中断：16 USB 待机、18 S_TWD、19 S_WDT、20–23 S_TIMER0–3、24 RTC 闹钟、
25–28 PL/PM GPIO、29–30 S_UART0/1、31–33 S_TWI0–2、34 S_IRRX、35 S_PWM、36 S_TZMA、
37 AHBS 超时、38 S_SPI、39 CPUS MSGBOX 读、48 CPUX MSGBOX 写、49 硬件自旋锁、
54–77 GINTC 转发的 GIC 中断。**GINTC 基址已锁定 0x07090000**（手册 12.1 章
"Interrupt Controller"：CONFIG 寄存器从 offset 0x0010 起、E902 中断号 N 的选择寄存器在
offset 4*N；地址映射里 RTC 与 S_TIMER 之间的无名 4K 块，T38）—— 还差 E902 侧路由一个
真实 GIC 中断做闭环验证。

## 7. 电源/休眠/DRAM 调频（厂商固件的主体功能）

- 休眠：`echo deep > /sys/power/mem_sleep && echo mem > /sys/power/state`。
  **还没回归过**，而且唤醒源要现场接（无 RTC 闹钟、无网口），需要有人在板边。
- poweroff 后上电：同样需要现场，还没回归（`tests/post-poweroff.sh` 类演练脚本在 `e902/tests/`）。
- DRAM 调频：在 E902 里跑，Linux 侧**没有 devfreq 节点**，改频走 bl31 RPC，应用层不可见。
- 换/重编固件：`e902/FLASHING.md`；构建 `bash e902/vendor-scp/build.sh`（arisc `0170020e`
  + dramlib `7142734a` + 3 补丁，Xuantie GCC V3.2.0，产物 `fw-out/vendor-scp.bin`，
  `tools/flash-scp.sh` 在线刷）。**改 scp 条目必须重算 add_sum（bootpkg.py 自动做）**。
  注意 orangepi-build 打镜像用的还是出厂 `external/packages/pack-uboot/sun60iw2/bin/scp.fex`，
  新镜像刷完要再刷一次 vendor-scp（README「还没做的」挂着）。

## 8. 内核配置速查（userpatches/linux-sun60iw2-current-a733.config）

| 配置 | 值 | 说明 |
|---|---|---|
| AW_AMP_TIMESTAMP | y | 共享时间戳（0014） |
| AW_TRNG / AW_LRADC / AW_GPADC | m | hwrng / IIO（0015 修的 6.6 编译） |
| AW_MSGBOX | y | mailbox 驱动（无用户态接口） |
| AW_RPMSG_* | n | BSP 未用 rpmsg |
| HW_RANDOM | y | hwrng 框架 |

## 9. 测试脚本

`e902/tests/board/e902-noble-check.sh`：一键跑 §1–§4 的实测 + 顺手修 dnsmasq/pam_lastlog/
RT 启动参数（改卡状态，结尾重启）。运行方式见文件头注释。
`e902/tests/board/rtla-gpadc.sh`：rtla 构建 + GPADC 中断风暴处置。

## 10. 实测（2026-09-25，noble 卡，1.0.2 镜像 + RT 内核）

| 项 | 结果 |
|---|---|
| amp_timestamp 频率 | 实测 23.999808 MHz（DT 标称 24 MHz） |
| usec vs CLOCK_MONOTONIC_RAW | 漂移 −13 µs / 10 s（≈1.3 ppm，晶振容差级） |
| TRNG 吞吐 | 142 kB/s（/dev/hwrng，256 KB 连续读） |
| mailbox 中断（厂商固件） | /proc/interrupts 两个 msgbox 号均为 0 —— 厂商固件不主动发，印证 §5 |
| GPADC | 空闲时 ~4 kHz 中断打在 cpu0（42.5M 次累计）→ 已 unbind + `blacklist sunxi_gpadc`（/etc/modprobe.d/blacklist-sunxi-gpadc.conf），要用 ADC 口 `modprobe sunxi_gpadc` |
| LRADC | 以 input 设备 `sunxi-keyboard` 暴露（event9），不是 IIO；按键走 /dev/input |
| RT 启动参数 | `isolcpus=5 nohz_full=5 rcu_nocbs=5` 已写进 orangepiEnv.txt（备份 .bak-noble-rt），/proc/cmdline 与 /sys/.../isolated 均确认；隔离 cpu5 cyclictest 空载 Avg 25 / Max 71 µs（1 kHz） |
| rtla | /usr/bin/rtla（linux-tools-common 的壳）在自编内核上不可用（"rtla not found for kernel 6.6.98-rt58"）；已用 6.6 rtla 源码在板上编出真二进制装到 /usr/local/bin，二进制留档 userpatches/overlay/rtla/rtla 供镜像自带 |
| rtla tracer | timerlat / hwlat / function_graph 正常；**rtla osnoise top 挂死**（ppoll 等 trace 数据），但内核 osnoise tracer 本体正常（echo osnoise > current_tracer + cat trace 可用）；同次开机先跑过 timerlat 再 enable osnoise 会 EBUSY（trace_osnoise.c:3021）—— 用 timerlat 即可 |
| 幻影外设 | 0x34 "AXP515"（启动 I2C 毛刺假象，运行时不应答，真 PMU 是 0x36 AXP8191）和 hym8563@0x51（未焊）—— 补丁 0016 已在板级 dts 禁用 |
| 开机日志残留 | `unknown pin`×4 = uart0/uart5 pinmux 空引脚，补丁 0017 已清；`ccu_ddr failed to find dram_clk` = **有意**（/dram 无 dram_para，Linux 侧 DRAM 驱动惰性退出，DRAM 调频归 SCP，不要修）；`twi-251c000` 早期探测超时一次，无功能影响 |
| Vulkan WSI | VK_KHR_xcb_surface + swapchain + external memory/fence/semaphore；**无 VK_KHR_display** → 上屏必须经 X11，不能直连 KMS |
