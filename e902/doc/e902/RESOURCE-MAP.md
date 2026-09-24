# A733 / Orange Pi Zero 3W — E902 与小核共享资源归属表

生成时间：2026-09-22 · 主机 `ssh helios@TL101` · 板子 `orangepi@172.16.0.193`
工程根：`/home/helios/Desktop/orangepi-build`

每一条都给出「归属 + 证据」。证据只有三类：**手册**（`e902/doc/A733_User Manual_V0.91.pdf`，已用 pdftotext
转文本，给出章节号与文本行号）、**代码**（`文件:行号`）、**实测**（2026-09-22 在板子上用
`e902-fw/scripts/awdevmem.py` 读回，原始值见 `FINDINGS-LEDGER.md`）。没有来源的条目不写。

术语：**大核 = ARM Cortex-A（CPUX 域）**，**小核 = 玄铁 E902（CPUS 域）**。

---

## 1. 内存区域

| 区域 | 地址（E902 视角） | 地址（ARM 视角） | 大小 | 归属 | 证据 |
|---|---|---|---|---|---|
| SRAM_A2 | `0x40000000..0x40033FFF` | `0x00040000..0x00073FFF` | 208K | **共享**（同一片 SRAM，两侧地址不同） | 手册 ch.2（文本 1477 起）；`u-boot/.../plat-sun60iw2p1/cpu_autogen.h:5 SUNXI_SRAM_A2_BASE` |
| 厂商 SCP 镜像占用 | `0x40004000..0x4001DDB8` | `0x00044000..0x0005DDB8` | 105912 B | 小核独占（bl31 载入 `scp.fex`） | 实测 `0x07032204` 读出 `0x40004000`；该处 105912 B 与 `u-boot/v2018.05-sun60iw2/scp.fex` 逐字节一致 |
| 厂商 SCP 栈顶 | `0x4002F000`（向下增长） | `0x0006F000` | — | 小核独占 | `scp.fex` 入口反汇编（`doc/e902/DESIGN-NOTES.md` §2.3） |
| DRAM | `0x40000000..0x7FFFFFFF` | `0x40000000..0x7FFFFFFF` ← **同一数值含义完全不同** | ~1G | 共享（小核可访问，需 `E902_DDR_REMAP`） | 手册 ch.2；`/proc/iomem`：`40000000-13fffffff System RAM` |
| DRAM 窗口 | `0x80000000..0xBFFFFFFF` | — | 1G | 小核独占视图，可 remap 到 16G 内任意段 | 手册 5.2.4.2 `E902_DDR_REMAP_REG` |
| CPUS_CFG 外设 | `0x07000000..0x0709FFFF` | 同址 | 640K | 共享（大核也在用，如 `msgbox@7094000`） | 手册内存表（文本 1484） |
| CPUX 子系统外设 | `0x00080000..0x06FFFFFF` | 同址 | — | 大核独占，**小核也可访问**（可读写主 CCU） | 手册 ch.2 |

> ⚠️ **最容易踩的坑**：`0x40000000` 在 E902 眼里是 SRAM_A2 起点，在 ARM 眼里是 DRAM 起点。
> 换算 `ARM = 0x00040000 + (E902 - 0x40000000)`。`e902-fw/scripts/awdevmem.py` 强制做这个换算并拒绝越界地址，
> 早期脚本直接拿 E902 地址当 ARM `/dev/mem` 偏移，会静默破坏内核内存（DESIGN-NOTES §2.3.1）。

---

## 2. 时钟

| 时钟 | R-CCU 偏移 | 位 | 供谁 | 归属 | 证据 |
|---|---|---|---|---|---|
| `RISCV_24M_CLK` | `0x0210` | mux[25:24]，gate bit31 | E902 子系统 TIMESTAMP 时基 | 小核可配；boot0/厂商固件也配 | 手册 4.2.5.23（文本 13583）；`ccu-sun60iw2-r.c:174` `riscv_24m_clk` |
| `RISCV_BGR` | `0x021C` | bit16 `RISCV_CFG_RST`、bit1 `RISCV_CFG_GATING`、bit0 `RISCV_GATING` | RISCV_CFG 寄存器块 + E902 内核总线时钟 | 共享（无内核复位位，见 §3） | 手册 4.2.5.24（文本 13613）；`ccu-sun60iw2-r.c:179,183` |
| `S_SPI_CLK` | `0x0150` | M[4:0]、N[12:8]、mux[26:24]、gate bit31 | CPUS 域 S_SPI | 小核独占（大核 DTS 里 `disabled`） | 手册 4.2.5.12（文本 13303）；`ccu-sun60iw2-r.c:109`；DT `r_spi` `status="disabled"` |
| `S_SPI_BGR` | `0x015C` | bit16 `S_SPI_RST`、bit0 `S_SPI_GATING` | 同上 | 小核独占 | 手册 4.2.5.13（文本 13344）；`ccu-sun60iw2-r.c:116,224` |
| `S_MBOX_BGR`（CPUS 读侧） | `0x017C` | bit16 rst、bit0 gate | `S_MBOX 0x07094000` | 小核独占 | 手册 4.2.5.14（文本 13363）；`ccu-sun60iw2-r.c:128` |
| `MSGBOX0_BGR`（CPUX 写侧） | `0x0744`（主 CCU） | bit16 rst、bit0 gate | `CPUX_MSGBOX 0x03004000` | **共享**（大核的 `sunxi-msgbox` 驱动 probe 时无条件打开） | 手册 4.1.6.88（文本 6877）；`ccu-sun60iw2.c:751,2087`；`sunxi-msgbox.c:825` |
| `S_UART_BGR` | `0x018C` | bit17/16 rst、bit1/0 gate | `S_UART0/1` | 小核独占（大核 DTS 里 `uart7` `disabled`） | 手册 4.2.5.15（文本 13363 附近）；`ccu-sun60iw2-r.c:135,136` |
| `S_TIMER_BGR`（4 个共享一个） | `0x011C` | bit16 rst、bit0 gate | `S_TIMER0..3` | 小核可配；未见大核占用 | 手册 4.2.5.x；`ccu-sun60iw2-r.c` rst 表 `RST_BUS_R_TIME = {0x011c, BIT(16)}` |
| `R_TIMERn_CLK` | `0x0100+4n` | bit0 使能（复位值 0=关）、[3:1] 分频、[6:4] mux | 每个 S_TIMER 的计数时钟 | 小核独占（Linux 无消费者，CLK_IGNORE_UNUSED） | `ccu-sun60iw2-r.c:40-84`；T30 实测：不开则计数器不动 |
| `APBS1_CLK` | `0x0010` | mux[26:24] + 5 位分频 | `S_UART0/1` 的父时钟 | **只读**（boot0/u-boot/Linux 已配好） | 手册 4.2.5.3；`ccu-sun60iw2-r.c:29 r_apbs_parents` |

CPU 变频不走小核：`ccu-sun60iw2-cpupll.c` 无任何 SMC 调用（大核自己写 PLL）。DRAM 变频**走**小核
（`ccu-ddr.c:77` 的 `ARM_SVC_SUNXI_DDRFREQ`）。

---

## 3. 复位

| 复位 | 位置 | 位 | 归属 | 证据 |
|---|---|---|---|---|
| `RST_BUS_RISCV_CFG` | R-CCU `0x021C` | bit16 | **只复位 RISCV_CFG 寄存器块**，不是 E902 内核 | 手册 4.2.5.24 原文 `RISCV_CFG_RST / RISCV_CFG Reset`；`ccu-sun60iw2-r.c:233` |
| `RST_BUS_R_SPI` | R-CCU `0x015C` | bit16 | S_SPI | 手册 4.2.5.13；`ccu-sun60iw2-r.c:224` |
| `RST_BUS_R_MBOX` | R-CCU `0x017C` | bit16 | S_MBOX | `ccu-sun60iw2-r.c:225` |
| `RST_BUS_R_UART0/1` | R-CCU `0x018C` | bit16/17 | S_UART0/1 | `ccu-sun60iw2-r.c:226,227` |
| `RST_BUS_SPI0..4` | 主 CCU `0x0F04..0x0F2C` | bit16 | CPUX 域 5 个 SPI | `ccu-sun60iw2.c:2136-2140` |
| **E902 内核复位** | **不存在于任何软件可见寄存器** | — | 只能由 bl31 在启动流程中释放 | 手册 5.2.3 `E902_CFG` 寄存器清单（仅 8 个，无复位寄存器，文本 18371-18385）；R-CCU 复位表全表（`ccu-sun60iw2-r.c:221-234`）无 `RST_BUS_RISCV` |

`E902_RST_START_ADDR`（`E902_CFG + 0x0204`）：手册原文 —— *"It is the running PC address when E902 reset
is released. Before this, configure the register as dynamic configuration is not supported."*（文本 18550-18560）。
即：**该寄存器是"复位释放时的 PC"，复位本身由启动流程释放，软件无法自己再释放一次**。
这一点是本次任务的核心结论，2026-09-22 已实测复现，见 `FINDINGS-LEDGER.md`。

---

## 4. 中断（E902 视角，手册 Table 12-2，文本 33860-33980）

| IRQ | 名称（手册原文） | 用途 | 本工程用法 | 证据 |
|---|---|---|---|---|
| 20–23 | `S_TIMER0..3`（寄存器组在 0x07091000+0x20+0x20n，**不是**手册 8.2.6 的 0x10 起，T30） | CPUS 域定时器 | 固件用 21 做 100 ms tick（v63：兼作中断投递探针，另有轮询兜底） | 文本 33871-33874 |
| 25 / 26 | `GPIOL_S / GPIOL_NS` | PL 组 GPIO | 未用 | 文本 33876-33879 |
| 29 | `S_UART0` | 小核自己的串口 | v63 起 RX 轮询，中断不使能 | 文本 33882 |
| 30 | `S_UART1` | 第二路 S_UART | 未用 | 文本 33883 |
| 31–33 | `S_TWI0..2` | S_TWI（PL0/PL1 = PMIC 总线） | ⚠️ 绝对不要碰 PL0/PL1 | 文本 33884-33886 |
| 35 | `S_PWM` | S_PWM | 未用 | 文本 33891 |
| **38** | **`S_SPI`** | "STBY SPI interrupt" | v63 起不使能（spi_isr 会抢轮询驱动的 RX FIFO/TC） | 文本 33897 |
| **39** | `CPUS_MBOX_READ_IRQ` | "CPUS MSGBOX READ IRQ" | 未用 | 文本 33899 |
| **48** | `CPUX_MSGBOX_W_R` | 手册原文 **"CPUX MSGBOX WRITE IRQ FOR CPUS/RISCV"** | 手册证实 48 正确；但 v62 起 mailbox 改为主循环轮询（与厂商一致），48 不使能 | 文本 33904 |
| 49 | `SPINLOCK(CPUX)` | 硬件自旋锁 | 预留（大数据载荷互斥） | 文本 33906 |
| 54–69 | `riscv_sys_irq_i[6..21]` | 由 `GINTC_CONFIG_REG0~4` 从 GIC irq 32–159 映射 | 未用 | 文本 33907+ |

> **旧笔记的疑点已解决**：`HANDOFF-RT-IMAGE.md` 曾怀疑"固件用 48 是 bug，应该是 39"。
> 手册表 12-2 明确写着 48 是 *"CPUX MSGBOX WRITE IRQ FOR CPUS/RISCV"* —— 正是"CPUX 写了消息、
> 通知给 CPUS/RISCV 的中断"。**48 是对的，39 不是。**
> 注：这只是"手册与代码一致"，尚未在硬件上看到中断真实触发（见 §6 与 FINDINGS-LEDGER）。

CLIC：基址 `0xE0800000`（E902 手册 10.2；厂商 `scp.fex` 反汇编 `lui a4,0xe0801` 印证）。
`INTIP/INTIE/INTATTR/INTCTL` 每中断一个字节，位于 `+0x1000 + 4*i + {0,1,2,3}`；`CLICCFG` 在 `+0x0000`。
CLIC/CLINT 位于核内 TCIP 窗口 `0xE0000000..0xEFFFFFFF`，不占 SoC 地址空间（集成手册 ch.4），
所以 A733 手册查不到 CLIC 是正常的。

---

## 5. 引脚复用

| 引脚 | 排针 | 复用 | 归属 | 证据 |
|---|---|---|---|---|
| PL0 / PL1 | — | `s_twi0` → AXP515/AXP8191 PMIC | **大核独占，绝对不要动** | 手册复用表；原理图 Sheet18；`pinmux.c` 注释 |
| PL2 | pin 16 | 功能 3 = `S_UART0-TX`，网络名 `SCPU-TX` | **小核独占**（大核 `uart7` disabled，其 pinctrl 组永不应用） | 原理图 `OPI ZERO 3W V1_2` Sheet18；`pinctrl-sun60iw2-r.c:38-56` |
| PL3 | pin 18 | 功能 3 = `S_UART0-RX`，网络名 `SCPU-RX` | 小核独占（同上） | 同上 |
| PL4 | — | `s_irrx` 红外 | 大核（DTS `s_irrx` disabled，但引脚仍是 IR 用途） | `sun60i-a733-orangepi-zero3w.dts` |
| PL5 | pin 27 | `s_twi2` SDA | 大核 | 原理图 |
| PL6 | — | 空闲 | **可给 E902 用** | 原理图 |
| PL7 | — | status LED | 大核（闪烁走 `PL_DAT 0x0010`，不写 `PL_CFG0`） | 原理图 |
| PM0–PM4 | — | WiFi/蓝牙 | 大核 | 原理图 |

`PL_CFG0`（`0x07025000`）一个 32 位字装 8 个脚，每脚 4 bit。改 PL2/PL3 必须读-改-写整字，
存在与 Linux 抢同一个字的窗口（Linux 只在 probe/suspend 改功能，LED 闪烁走 `PL_DAT`）。
实测 `PL_CFG0 = 0x1FFF1F22`（2026-09-22，见 LEDGER），其中 PL2/PL3 两个 nibble 为 `2`（不是我们固件会写的 `3`），
说明当时没有任何一方把它们设成 S_UART0。

---

## 6. 总线 / 通信

| 资源 | 地址 | 方向 | 归属 | 证据 |
|---|---|---|---|---|
| `CPUX_MSGBOX` | `0x03004000` | CPUS 写，CPUX 读 | E902→ARM | 手册 6.1.1（文本 18310 附近）；实测可读 |
| `S_MBOX`(CPUS_MSGBOX) | `0x07094000` | CPUX 写，CPUS 读 | ARM→E902 | 手册 6.1.1；实测可读 |
| 通道/FIFO | 各 4 通道 × 8×32bit | — | — | 手册 6.1.1 原文 |
| 读中断频率 | — | — | — | 手册 6.1.4.1："当通道非空时产生读中断" |
| `hwspinlock` | `0x03005000` | — | 共享（DTS `okay`，无客户端） | `check-e902.sh` 输出；`sunxi-hwspinlock` 已绑定 |
| GIC 映射 | `r_spi` → GIC SPI 210；`uart7` → ？ | — | 大核侧 | `sun60iw2p1.dtsi` r_spi node |

**通道 3 的 ARM 侧只属于 bl31（2026-09-23，T29）**：bl31 的同步 RPC 在 EL3 无超时自旋等待 CPUX FIFO
中的应答，且不校验应答与请求是否对应。因此：Linux 不得有任何 mbox 客户端 claim 通道 3；
`msgbox@3004000` 节点必须保持 okay（它持有 CLK_MSGBOX0）；E902 在握手之后绝不主动向 ARM 发送任何字；
用 /dev/mem 读写 0x0300407C/0x0709407C 做调试只能在系统空闲时进行。

**驱动绑定现状（2026-09-22 实测）**：`sunxi-msgbox` 与 `sunxi-hwspinlock` 平台驱动均已存在，
但 `/proc/interrupts` 里**没有 mbox 行** —— 说明没有 mailbox 客户端申请通道，RX 中断未开
（`sunxi-msgbox.c` 只在 `sunxi_msgbox_startup()` 里开 RX 中断，即客户端 claim 通道时才跑）。
`arm-msgbox-test.c` 直接怼寄存器，与此共存安全（DESIGN-NOTES §6.7）。

---

## 7. 大核侧如何"用"小核

| 路径 | 机制 | 现状 | 证据 |
|---|---|---|---|
| SMC → bl31 → E902 | 同步 RPC | 一直在用（DRAM 变频、suspend、PMIC） | `sunxi-sip.h`；`ccu-ddr.c:77` |
| MSGBOX | 异步双向 FIFO | 硬件在，驱动 `=y`，无客户端 | 本表 §6 |
| 共享内存 + hwspinlock | 大数据载荷 | 预留 | 手册 `space_cfg.msgpool` |
| remoteproc | 标准 Linux 框架 | **A733 完全没有 E902 rproc** | `sunxi_rproc.c:1412-1418` compatible 表无 e902；dtsi 无 riscv/rproc 节点 |

---

## 8. 厂商 SCP 固件自身的资源足迹（静态反汇编提取，2026-09-22）

证据来源：对 `u-boot/v2018.05-sun60iw2/scp.fex`（105912 B）做
`objdump -D -b binary -m riscv:rv32 --adjust-vma=0x40004000`，抽取全部被解析出的地址注释，
脚本 `scp_addrs.py`。

**最重要的一条**：除自己的镜像 / bss / 栈（`0x4002F000` = 其栈顶）之外，
**SCP 不访问 SRAM 里的任何其他位置**（SRAM 区间内只有 1 个 distinct 地址，就是它自己的栈顶）。
⇒ 它**没有**往外部 SRAM 写握手标志；与 bl31 的握手只可能走 mailbox 或它自己的 bss。

**它实际使用的 CPUS 域寄存器**（按手册名）：STBY_PRCM/AHBS_CLK(0x0000)、APBS0_CLK(0x000C)、
S_TIMER0_CLK(0x0100)、S_TIMER_BGR(0x011C)、S_PWM_BGR(0x013C)、**S_MBOX_BGR(0x017C)**、
**S_UART_BGR(0x018C)**、S_TWI_BGR(0x019C)、S_IRRX_CLK(0x01C0,6 次)、S_IRRX_BGR(0x01CC)、
RTC_BGR(0x020C)、**RISCV_24M_CLK(0x0210)**、PRCM 0x0244/0x0250(12 次)/0x0254/0x0260（手册该章未列出，待查）、
NMI_INT_EN(0x0324)、BUS_ACG(0x033C)、0x070240C0、0x07024FE0、**E902_CFG 区 0x07032001**、
0x07050010/14/0220、**S_UART0 0x07080080/84/A4**、**S_TWI0 0x07083000 区**、0x07090000 区。

**跨域访问**：CPUX 域 `0x03004001`(**CPUX_MSGBOX**)、0x030060A0、0x03000200/0160/0000；
主 CCU `0x02002744`(**MSGBOX0_BGR**)、`0x02003300/04/08/0C`（各 6 次）、0x02003340、0x02002588 等。

**由此新增的"共享点"判断依据**：它碰过、而 Linux 也在用的即为真共享（RISCV_24M_CLK、S_MBOX_BGR、
MSGBOX0_BGR、S_TWI0→PMIC、S_UART_BGR）；它**完全没碰**的（`S_SPI 0x07092000` 区、`RISCV_BGR 0x0701021C`）
可确认适合小核独占。
