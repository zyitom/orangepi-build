> ⚠️ **2026-09-22 更正**：本文档中"可以从 Linux 停核/换固件/启动 E902"的说法已被上板实测推翻，
> 详见 `doc/e902/CORRECTION-2026-09-22.md` 与 `doc/e902/FINDINGS-LEDGER.md`。
> 小核复位只能由 bl31 释放。

# A733 E902 接管记录

调研与实现过程的事实记录。每条结论都标了出处，方便日后复核。
配套代码在 `e902-fw/`，体检脚本在本目录 `check-e902.sh`。

日期：2026-07（基于 orangepi-build `next` 分支，commit bdba421 之后）

---

## 1. 结论摘要

A733（sun60iw2）的 CPUS 域里有一颗玄铁 **E902**（RV32EMC），出厂跑着
`u-boot/v2018.05-sun60iw2/scp.fex`。这颗核不是空闲资源，它承担 DRAM 变频、
suspend、PMIC 看护、时钟管理。

**当前内核对 E902 没有 remoteproc 支持**，但**通过 SMC → bl31 在用它的服务**。

自己的固件可以跑起来，且**不必改 u-boot** —— Linux 侧没有任何驱动持有 E902
的时钟/复位引用，可以从 `/dev/mem` 停核、换固件、启动，测完再写回 `scp.fex`。

---

## 2. 芯片侧事实

### 2.1 E902 存在且有完整文档

`doc/A733_User Manual_V0.91.pdf` 第 5.2 节 "RISC-V E902 Subsystem"：

| 寄存器 | 地址 | 手册 | 用途 |
|---|---|---|---|
`E902_AUTO_GATING` | `0x07032004` | 5.2.4.1 | 自动门控 |
`E902_DDR_REMAP` | `0x07032020` | 5.2.4.2 | 1G 窗口重映射到 16G 内任意段 |
`E902_TS_TMODE_SEL` | `0x07032040` | 5.2.4.3 | TIMESTAMP 测试模式 |
`E902_WAKEUP_IRQ_NUM` | `0x07032060` | 5.2.4.4 | 首个唤醒中断号 |
`E902_WAKEUP_MASK0` | `0x07032064` | 5.2.4.5 | bit0 = 中断 16 |
`E902_WAKEUP_MASK1` | `0x07032068` | 5.2.4.6 | bit0 = 中断 48 |
`E902_PAD_LPMD` | `0x07032080` | 5.2.4.7 | WFI 后的睡眠深度 |
`E902_RST_START_ADDR` | `0x07032204` | 5.2.4.8 | **复位后 PC。手册写默认 `0x40014000`，但跑着厂商固件时实测读出 `0x40004000`（bl31 会改写它）** |

`0x0204` 手册明确写了：**必须在放复位之前配好，不支持动态修改**。

时钟/复位在 R-CCU（`0x07010000`，手册叫 S_PRCM）：

| 寄存器 | 地址 | 手册 | 位 |
|---|---|---|---|
`RISCV_24M_CLK` | `0x07010210` | 4.2.5.23 | bit31 gate，bit[25:24] mux |
`RISCV_BGR` | `0x0701021C` | 4.2.5.24 | bit16 rst，bit1 cfg gate，bit0 core gate |
`S_MBOX_BGR` | `0x0701017C` | 4.2.5.14 | bit16 rst，bit0 gate |
`S_UART_BGR` | `0x0701018C` | 4.2.5.15 | bit17/16 rst，bit1/0 gate |
`APBS1_CLK` | `0x07010010` | 4.2.5.3 | S_UART 的父时钟，mux + 5 位分频 |

内核里这些时钟已经实现好了：`bsp/drivers/clk/sunxi-ng/ccu-sun60iw2-r.c:173-185`
定义了 `riscv-24m`/`riscv-cfg`/`riscv`，offset 跟手册逐位对得上；
`RST_BUS_RISCV_CFG` 在同文件 line 233。

### 2.2 CPUS 域其实有两颗核

`u-boot/.../plat-sun60iw2p1/cpu_autogen.h:156-157` 并列写着：

```c
#define SUNXI_CPUS_OR100_CFG_BASE      0x07031000
#define SUNXI_CPUS_RV_CFG_BASE         0x07032000   // E902
```

手册 12.1.2 的框图也画了两条中断链路：`RV_INT_CTRL → E902` 和
`CPUS_INT_CTRL → OR100`。OR100 是 OpenRISC，全志历史上叫 AR100 —— 这也是
"ARISC" 这个名字的来源。我们只动 E902。

### 2.3 E902 视角的内存映射（手册第 2 章）

```
0x00074000..0x000F3FFF  SHARED_SRAM  512K（需 NPU 上电）
0x00080000..0x06FFFFFF  L4_TOP_CONN  CPUX 子系统外设  ← 能访问主 CCU
0x07000000..0x0709FFFF  CPUS_CFG     CPUS 域外设
0x40000000..0x40033FFF  SRAM_A2      208K
0x40034000..0x7FFFFFFF  DRAM         ~1G
0x80000000..0xBFFFFFFF  DRAM 窗口    1G，可 remap
```

✅ **原“未解矛盾”已在板子上解决（2026-07-29 实测）**：之前按 `0x40014000`
反汇编 `scp.fex`，算出栈是 `0x4003f000`，落在 SRAM_A2 之外，看着像手册表格不全。
真正的原因是**加载基址搞错了**：bl31 把 `scp.fex` 放在 `0x40004000`，不是
`0x40014000`。运行中的板子上 `E902_RST_START_ADDR`（`0x07032204`）读出来就是
`0x40004000`，而且该地址起 105912 字节与 `u-boot/v2018.05-sun60iw2/scp.fex`
逐字节一致（仅 `0x40010738..0x40010767` 有 10 字节运行期数据不同）。

按正确基址重算：

```
+0x26  auipc sp,0x2b   → pc=0x40004026, sp=0x4002F026
+0x2a  addi  sp,sp,-38 → sp=0x4002F000   ← 在 SRAM_A2 内，且高于镜像尾 0x4001DDB8
```

栈是 `0x4002F000`，完全落在手册记录的 `0x40000000..0x40033FFF` 内。手册表格没问题。

⚠️ 连带结论：`0x40014000` 落在**厂商镜像内部**（偏移 `0x10000`，镜像共
`0x19DB8` 字节）。往那里加载自己的固件会覆盖厂商 SCP 的中段，所以两者互斥这点
仍然成立，但恢复时必须写回**整个** `scp.fex` 到 `0x40004000`。

### 2.3.1 ⚠️ SRAM_A2 的两个地址（最容易踩的坑）

同一片 SRAM，两个核看到的地址不一样，而 `0x40000000` 这个数在两边**含义完全不同**：

```
E902 视角：SRAM_A2  0x40000000..0x40033FFF   （手册 ch.2，也是 fw.ld 里的 ORIGIN）
ARM  视角：SRAM_A2  0x00040000               （u-boot cpu_autogen.h:5 SUNXI_SRAM_A2_BASE）
ARM  视角：0x40000000 是 DRAM 起点            （/proc/iomem: 40000000-13fffffff System RAM）
```

换算：`ARM 地址 = 0x00040000 + (E902 地址 - 0x40000000)`，
即 E902 `0x40014000` ↔ ARM `0x00054000`。

**为什么危险**：拿 E902 视角的 `0x40014000` 直接当 ARM 侧 `/dev/mem` 偏移，写的
是活着的、未保留的内核内存（内核代码从 `0x41010000` 起，`0x40014000` 不在任何
reserved 区里）。这台板子 `CONFIG_STRICT_DEVMEM` 没开启（实测
`# CONFIG_STRICT_DEVMEM is not set`），内核不会拦，于是：

- `dd` 返回 0、脚本继续往下跑，看起来一切正常
- E902 那边什么都没收到
- 被覆盖的页归谁就谁出事，而且是延迟发作：段错误、读到乱码、脏页把垃圾写回文件系统

`e902-load.sh` / `e902-restore.sh` / `check-e902.sh` 早期版本正是这么写的。现在
统一走 `e902-fw/scripts/awdevmem.py`，它只接受 E902 视角地址、内部换算、
范围外直接拒绝，并且写完读回校验。

另外 SRAM_A2 只能走 `mmap`，不能用 `dd`：`/dev/mem` 的 `lseek+read/write` 对非
System RAM 会返回 `Bad address`（实测）。而且这块映射是设备内存，按字节
`memcpy` 在长度非 16 字节对齐时会 `SIGBUS`（实测 `0x19DB8`/`0x19DB4` 崩、
`0x19DB0`/`0x19DC0` 不崩），所以 `awdevmem.py` 按 32 位字访问，尾巴逐字节补。

### 2.4 中断（手册表 12-2，E902 视角）

```
20~23  S_TIMER0~3        25/26  GPIOL_S/NS       29  S_UART0
30     S_UART1           31~33  S_TWI0/1/2       35  S_PWM
38     S_SPI             39     CPUS_MBOX_READ   48  CPUX_MSGBOX_W_R  ← 收 ARM 消息
54~69  riscv_sys_irq_i[6..21]，由 GINTC_CONFIG_REG0~4 从 GIC irq 32~159 映射
```

CLIC 在 `0xE0800000`（E902 手册 10.2）。集成手册第 4 章说 TCIP（含 CLIC/CLINT）
地址空间**固定 `0xE0000000..0xEFFFFFFF`，核内直接处理，不走 AHB** —— 所以
A733 手册不提 CLIC 是正常的，它不在 SoC 地址空间里。

---

## 3. 从 scp.fex 反汇编得到的（文档拿不到的）

这些是"集成时可配置"的选项，任何文档都不会写 A733 具体选了什么。

```bash
# 注意 vma 是 0x40004000。早期版本这里写 0x40014000，导致所有绝对地址都偏了
# 0x10000，也就是上面 2.3 节那个“栈在 SRAM 外”的假矛盾的来源。
riscv64-linux-gnu objdump -D -b binary -m riscv:rv32 \
  --adjust-vma=0x40004000 u-boot/v2018.05-sun60iw2/scp.fex
```

| 发现 | 依据 |
|---|---|
**RV32EMC**（带 M 扩展） | 87 条 `mul`/`div`/`rem` 指令 |
**CLIC 基址 `0xE0800000`** | `lui a4,0xe0801` 构造 `0xE0801001` = CLICINTIE[0] |
**有 I-Cache** | 操作 `mhcr`（CSR `0x7c1`） |
**加载基址 `0x40004000`** | 板子上实测：`0x07032204` 读出 `0x40004000`，且该处 105912 字节与 `scp.fex` 逐字节一致（仅 10 字节运行期数据不同） |
用到玄铁自定义 CSR | `0x7c0`(mxstatus)、`0x7e1`、`0x676`/`0x6c6`/`0x746` 等 |

启动序列（可作为自己固件的参考）：

```
40004000: 4081  c.li ra,0        ← 清 16 个 GPR（RV32E 只有 16 个）
4000401e: auipc gp,0x1a          → gp = 0x4001e430
40004026: auipc sp,0x2b          ┐ sp = 0x4002f026
4000402a: addi  sp,sp,-38        ┘ sp = 0x4002F000  ← 在 SRAM_A2 内
40004048: csrrw zero,mtvec,a3
4000404c: csrrw zero,0x307,a3    ← mtvt，CLIC 矢量表
40004052: csrrs zero,mstatus,a3  ← 开 MIE
```

**我们的固件前 16 字节与 `scp.fex` 逐字节相同**（`8140 0141 8141 0142 ...`），
这是入口序列正确的独立印证。

厂商固件配的 R-CCU 寄存器（用来核对我们的配置清单）：

| 寄存器 | scp.fex 访问次数 | 我们 |
|---|---|---|
`S_UART_BGR` `0x701018c` | 2 | ✅ 配 |
`S_MBOX_BGR` `0x701017c` | 2 | ✅ 配 |
`RISCV_24M` `0x7010210` | 1 | ✅ 配（一开始漏了，据此补上） |
`RISCV_BGR` `0x701021c` | **0** | ✅ 不配（那是启动方的事） |
`APBS0_CLK` `0x701000c` | 1 | ❌ 不配（我们只用 APBS1，且只读不写） |

字符串印证它在干什么：

```
DRAM Type =%d (8:LPDDR4,9:LPDDR5)   DRAM DFS VERSION INFO: %s
standby service ok                   wait ac327 resume...
pmu & bmu driver ok                  axp515 exist / reset axp515
clk_suspend                          _axp8191_vbus_check
hwmsgbox driver ok                   ERR:dram crc error...
```

占用 `0x40004000..0x4001DDB8`（105912 字节，103.4KB），栈另占到 `0x4002F000`。
恢复厂商固件时要把整个 `scp.fex` 写回 `0x40004000`。

---

## 4. Linux 侧现状

### 4.1 remoteproc：完全没有 E902 支持

`bsp/drivers/remoteproc/sunxi_rproc.c:1412-1418` 的 compatible 表：

```c
{ "allwinner,hifi4-rproc" }, { "allwinner,c906-rproc" },
{ "allwinner,e906-rproc" },  { "allwinner,e907-rproc" },
{ "allwinner,arm-rtos-rproc" }, { "allwinner,arm-barematal-rproc" },
```

**没有 e902**，也没有 `sunxi_rproc_e902_boot.c`。

DTS 里 `sun60iw2p1.dtsi` 搜不到任何 riscv/e902/rproc 节点，`0x07032000`
完全没被占用。唯一的 rproc 节点是 `a55_rproc@0`（`allwinner,arm64-rproc`）且
`status = "disabled"`。

对比 `bsp/configs/linux-6.6/sun55iw3p1.dtsi:2983`，A523 有完整的
`e906_rproc@7130000` 节点（clocks/resets/`firmware-name = "amp_rv0.bin"`/
power-domains 全齐）—— A733 这边一个都没有。

配置里整条 AMP 链路全关：

```
# CONFIG_AW_REMOTEPROC is not set
# CONFIG_AW_MSGBOX is not set        ← 我们改成 y
# CONFIG_AW_HWSPINLOCK is not set    ← 我们改成 y
# CONFIG_AW_RPMSG_VIRTIO / AW_RPBUF_* is not set
CONFIG_REMOTEPROC=y                  ← 只是框架
CONFIG_SUN6I_MSGBOX=y                ← mainline 驱动，compatible 不匹配
```

### 4.2 但 Linux 一直在通过 SMC 用 E902 的服务

这条我第一轮漏了。`bsp/include/sunxi-sip.h` 定义了服务号，
`invoke_scp_fn_smc()` 是入口（`ARM_SVC_BASE = 0xc0000000` on arm64）：

| 服务号 | 名字 | 调用点 | 编译状态 |
|---|---|---|---|
`0xc0000025/26` | `CLEAR/SET_WAKEUP_SRC` | `irq-sunxi-wakeupgen.c:49,59`；`pinctrl-sunxi.c:2125` | ✅ `AW_WAKEUPGEN=y`、`AW_PINCTRL=y` |
`0xc0000027/28` | `CLEAR/SET_DRAM_KEEP_RUNNING` | `thermal/sunxi_critical_handler.c:297,306` | ❌ 未编 |
`0xc0000064` | `SET_DEBUG_DRAM_CRC_PARAS` | `standby/standby_debug.c:113` | ✅ `AW_STANDBY_DEBUG=y` |
`0xc0000095` | `CRASHDUMP_START` | `crashdump/sunxi-dump2pc.c:276` | ❌ 未编 |
`0xc0000096` | `ARM_SVC_SUNXI_DDRFREQ` | `clk/sunxi-ng/ccu-ddr.c:77` | ✅ `AW_DMC_DEVFREQ=m` |

**DRAM 变频真的走 E902**，`ccu-ddr.c:77`：

```c
static inline int set_ddrfreq(struct sunxi_ddrclk *ddrclk, unsigned int freq_id)
{
	return invoke_scp_fn_smc(ARM_SVC_SUNXI_DDRFREQ, freq_id, 0, 0);
}
```

Linux **不自己写 DRAM 控制器寄存器**，只发一个 freq_id。原因是变频时 DRAM 要短暂
停止响应，跑在 DRAM 里的 ARM 会取不到指令 —— 必须由跑在 SRAM 里的核做。

⚠️ **变频跟 suspend 无关**：`devfreq` profile 是 `.polling_ms = 100`
（`sunxi-dmc.c:281`），simple_ondemand governor 每 100ms 采样带宽利用率自动升降频。
`upthreshold = <50>`、`downdifferential = <20>` 在 dtsi:835-837。
suspend 只是在上面加了个开关（`sunxi-dmc.c:449` 的 `devfreq_suspend_device()`）。

**CPU 变频不走 E902**：`ccu-sun60iw2-cpupll.c` 里 `invoke_scp_fn_smc`/`arm_smccc`
出现 **0 次**，Linux 自己写 PLL；电压走 `cpu-supply = <&reg_dcdc5>` →
AXP8191 → 标准 regulator 框架。

### 4.3 E902 的启动路径

```
boot0 → u-boot → bl31(monitor.fex)
u-boot bootm.c:409  SMC 0x8000ff10 (ARM_SVC_ARISC_STARTUP)
    └→ bl31（闭源）：拷 scp.fex 到 0x40004000 → 配 0x07032204 → 放 0x0701021C
       （0x40004000 是实测值：跑起来后 0x07032204 读出 0x40004000）
```

`configs/sun60iw2p1_t736_defconfig:97-98`：

```
CONFIG_SUNXI_ARISC_EXIST=y
CONFIG_ARISC_DEASSERT_BEFORE_KERNEL=y
```

⚠️ 两个宏是**嵌套互斥**关系，不是并列（`board/sunxi/board_common.c:767-771`）：

```c
#ifdef CONFIG_SUNXI_ARISC_EXIST
#ifndef CONFIG_ARISC_DEASSERT_BEFORE_KERNEL
        sunxi_arisc_probe();
#endif
#endif
```

只关掉 `DEASSERT_BEFORE_KERNEL` 反而会**启用** `sunxi_arisc_probe()`，在 board init
阶段就启动 E902。要关就两个都关。

`drivers/arisc/arisc.c` 只负责从 DTS 抠参数打包成 `dts_cfg_64` 交给 bl31，真正的
加载和放复位它不做。`arisc_i.h` 的结构体能看出 SCP 需要什么：

```c
struct dts_cfg_64 {
    struct dram_para dram_para;        // DRAM 时序全套
    struct arisc_freq_voltage vf[16];  // DVFS 电压频率表
    struct space_cfg_64 space;         // sram/dram/para/msgpool/standby 五段
    struct dev_cfg_64 msgbox, hwspinlock, s_uart, s_rsb, s_jtag;
    struct cir_cfg_64 s_cir;
    struct pmu_cfg pmu;  struct power_cfg power;
};
```

注意有 `s_uart` —— SCP 固件自己带串口驱动。

---

## 5. 通信机制

### 5.1 三条路径

| 路径 | 机制 | 现状 |
|---|---|---|
① SMC → bl31 → E902 | 同步阻塞，像 RPC | ✅ Linux 一直在用 |
② MSGBOX | 异步双向硬件 FIFO | 硬件在，驱动没编（我们开了） |
③ 共享内存 + hwspinlock | 大数据载荷 | `space_cfg.msgpool` |

**自己的固件走 ②**，不依赖 bl31。

### 5.2 MSGBOX 细节（手册 6.1.6）

**两块独立的单向 FIFO**，不是一块双向的：

```
MBOX_CPUX @ 0x03004000   CPUS 写，CPUX 读   E902 → ARM
MBOX_CPUS @ 0x07094000   CPUX 写，CPUS 读   ARM → E902
```

各 4 通道，每通道 8×32bit 硬件 FIFO。寄存器（`N`=对端编号，A733 只有一个所以
N=0；`P`=通道 0~3）：

```
0x20 + N*0x100            READ_IRQ_ENABLE
0x24 + N*0x100            READ_IRQ_STATUS
0x30 / 0x34               WRITE_IRQ_EN/STATUS
0x50 + N*0x100 + P*4      FIFO_STATUS      bit0 = full
0x60 + N*0x100 + P*4      MSG_STATUS       bit[3:0] = 队列里几条
0x70 + N*0x100 + P*4      MSG_FIFO    ★    读/写消息本体，32bit
0x80 + N*0x100 + P*4      WRITE_IRQ_THRESHOLD
```

写 MSG_REG 自动给对方拉中断，没有单独的 doorbell。

中断号两侧不同：ARM 收是 GIC SPI 0（dtsi `msgbox` 节点第一个 interrupts），
E902 收是 CLIC IRQ **48**。

驱动实现与手册完全一致：`bsp/drivers/msgbox/sunxi-msgbox.c:29-38`。
compatible 在 line 717：`{ "allwinner,sun60iw2-msgbox", .data = &sun55iw5_hwdata }`
（注释说 sun60iw2 和 sun55iw5 的 msgbox 资源相同）。

### 5.3 是"上下位机"吗

结构上像，但方向反了：

| | 传统（STM32 + 树莓派） | A733 ARM + E902 |
|---|---|---|
连接 | UART/SPI 线缆 | 同一块硅片，共享总线和内存 |
地址空间 | 各自独立 | **重叠**（E902 能看到 ARM 的外设） |
延迟 | 毫秒级 | 纳秒级（写寄存器） |
谁是"上位" | 上位机发指令 | **E902 掌握 ARM 的生存条件** |

suspend 时 ARM 断电，是 E902 决定何时叫醒它；DRAM 变频时 E902 操作，ARM 只能请求。

---

## 6. 踩过的坑（重要）

### 6.1 `arisc_config` 节点是死的

板级 dts 第 62-68 行：

```c
arisc_config: arisc_config {
	s_uart_config{
		pins = "PL2", "PL3";
		function = <3>, <3>;
		status = "disabled";
	};
};
```

看起来是把引脚交给 SCP 的机制。**但内核和 u-boot 里都没有任何代码解析这个节点**
（grep 过 `bsp/`、`drivers/`、u-boot 全树）—— 是 `sys_config.fex` 时代的遗留。
设成 `okay` 没有任何效果。

### 6.2 `s_uart0` 这个 label 不存在

板级 dts 第 1350-1355 行那些注释掉的 `&s_uart0 { }` 引用的 label 在 dtsi 里
**不存在**，取消注释编不过。

**S_UART0（`0x07080000`）在 dtsi 里叫 `uart7`**（`sun60iw2p1.dtsi:1729`
`uart7: uart@7080000`）。

### 6.3 uart0 ≠ uart7，调试口安全

| DTS 名 | 地址 | 手册名 | 域 | 引脚 | 现状 |
|---|---|---|---|---|---|
`uart0` | `0x02500000` | UART0 | CPUX | PB4/PB5 | **`okay`，`console=ttyS0`** |
`uart7` | `0x07080000` | **S_UART0** | CPUS | PL2/PL3 | `disabled` |

`sun60iw2p1.dtsi:90`：`earlyprintk=sunxi-uart,0x2500000 ... console=ttyS0`
→ 指的是 `uart0`。**固件只碰 uart7，调试口完全不受影响。**

板子上 `okay` 的串口只有 `uart0` 和 `uart1`。`uart7` 本来就闲置，拿走零成本。

### 6.4 PL_CFG0 竞态：比初判的轻，但没消除

`PL_CFG0`（`0x07025000`）一个 32 位字装 8 个脚，每脚 4 bit：

```
[ 3: 0] PL0   s_twi0 ← AXP515(0x34)/AXP8191(0x36) PMIC 总线
[ 7: 4] PL1   s_twi0 ← 同上
[11: 8] PL2   ours (0x3 = S-UART0-TX)
[15:12] PL3   ours (0x3 = S-UART0-RX)
[19:16] PL4   s_irrx 红外
[23:20] PL5   s_twi2 SDA → 排针 pin 27
[31:28] PL7   status LED
```

**好消息（已核实）**：整个板级 dts 里提到 PL2/PL3 的 pinctrl 组**只有**
`uart7_pins_active`/`uart7_pins_sleep`，而 `uart7` 是 `disabled` ——
**disabled 节点的 pinctrl 组永远不会被应用**。所以 Linux 侧从不写 PL2/PL3 那两个
nibble。

**剩余风险**：Linux 配 PL0/PL1/PL4/PL7 时读改写同一个**字**。如果它的写落在我们
读和写之间，我们会覆盖掉那些 nibble —— PL0/PL1 是 PMIC 总线，最坏情况伤硬件。

缓解：只在初始化时写一次、只写自己那两个 nibble、读回校验（`pinmux.c` 的
`set_pl_function()`）、banner 打印整个字。**等 Linux 启动完再加载固件**
（`r_pio` 只在 probe 和 suspend/resume 写 `PL_CFG0`；LED 闪烁走 `PL_DAT` `0x0010`
另一个寄存器）。

hwspinlock 硬件在（`0x03005000`）但 Linux 的 pinctrl 不用它，所以没有跨核锁可用。

### 6.5 原理图确认（`doc/OPI ZERO 3W V1_2_原理图.pdf` 第 18 页）

40-pin 排针上，PL2/PL3 的网络名是 **`SCPU-TX` / `SCPU-RX`** —— 板厂本来就把
E902 的串口引出来了，跟 `CPU-TX`/`CPU-RX`（PB4/PB5，各串 1K 电阻，标 `CPU DEBUG`）
并列两套。

**物理针脚：PL2 = pin 16，PL3 = pin 18**（用户目视确认）。

芯片复用表原理图上也印着，与手册、`pinctrl-sun60iw2-r.c:38-56` 三方一致：

```
PL2/S-UART1-TX/S-UART0-TX/S-TWI1-SDA/S-PWM0-0/PL-EINT2      ← function 3
PL3/S-UART1-RX/S-UART0-RX/S-TWI1-SCK/S-IR-RX/S-PWM0-1/PL-EINT3
```

另外确认 PL4 = 红外、PL5 = s_twi2 SDA（pin 27），固件都避开。

### 6.6 MBOX_CPUX 时钟 —— 已解决

`0x03004000` 在 CPUX 域，gate 在主 CCU（`0x02002000+0x0744`，
= `CLK_MSGBOX0`/`RST_BUS_MSGBOX0`，见 `ccu-sun60iw2.c:751,2087`）。
曾担心 E902 写主 CCU 被安全限制。

**Linux 已经替我们开好了**：`sunxi_msgbox_hw_init()`（`sunxi-msgbox.c:825`）在
**probe** 路径调用（line 884），无条件执行 `reset_control_deassert()` +
`clk_prepare_enable()`，**不需要 mailbox 客户端**。`CONFIG_AW_MSGBOX=y` 一开机
就生效。

固件仍然会去写（为了"Linux 未 probe 时也能跑"的场景），写已置位的位无害。

### 6.7 msgbox 驱动不会抢 /dev/mem 测试工具的消息

RX 中断只在 `sunxi_msgbox_startup()`（line 346）打开，那只在 mailbox **客户端**
申请通道时跑。没客户端时驱动 probe 完就闲着，ISR 会先检查 `READ_IRQ_ENABLE`
再碰 FIFO。所以 `AW_MSGBOX=y` 和 `arm-msgbox-test.c` 可以共存。

### 6.8 编译需要 `_zicsr`

binutils 2.36 之后 CSR 指令从基础 ISA 分离成独立扩展。`start.S` 用了
`csrw mtvec`/`csrw 0x307`/`csrsi mstatus`/`csrr mcause`/`csrr mepc`，所以要
`-march=rv32emc_zicsr`，光 `rv32emc` 会报 "extension `zicsr' required"。

### 6.9 必须禁 PIE

主机 gcc 默认 PIE，会往镜像里塞 `.interp`/`.dynamic`/`.got` 和
`R_RISCV_RELATIVE` 重定位 —— 落在 flat binary 里没人处理，镜像不可用。
需要 `-fno-pic -fno-pie` + `-static -no-pie -Wl,--build-id=none`。

验证方法：`readelf -r` 应该报"没有重定位信息"，`objdump -h` 应该只有 6 个段。

---

## 7. 最终方案

### 7.1 u-boot 不用改（推荐）

DTS 里**没有任何节点引用 `CLK_RISCV`/`RST_BUS_RISCV`** → Linux 侧没人持有 E902
的时钟/复位引用 → 可以从 `/dev/mem` 停核、换固件、启动，Linux 完全不知情。

```bash
sudo ./scripts/e902-load.sh build/fw.bin   # 借用
sudo ./scripts/e902-restore.sh scp.fex     # 还回去
```

代价只在"自己固件在跑"的那段时间。**不碰裸扇区、不用重启、随时可逆。**

⚠️ 恢复时 SCP 是冷启动，拿不到 bl31 正常传给它的参数块（`dts_cfg_64`：DRAM 时序、
DVFS 电压表、PMIC 配置）。基本服务应该回来；如果 DFS 或 suspend 还是不对，重启
一次让厂商流程接管。

### 7.2 改 u-boot 的方案（保留，长期用）

`userpatches/u-boot/u-boot-sunxi/0001-sun60iw2-do-not-start-E902-SCP.patch`
关掉两个宏，bl31 从不加载 `scp.fex`。适合固定用自己固件的场景。
代价是永久失去 DFS/suspend，且回滚要重刷裸扇区（**必须先有备份镜像**）。

### 7.3 内核改动（唯一必需的一处）

`external/config/kernel/linux-sun60iw2-current-a733.config`：

```
CONFIG_AW_MSGBOX=y          ← 必需，否则 ARM 侧收不到消息（也顺带开好 MBOX_CPUX 时钟）
CONFIG_AW_HWSPINLOCK=y      ← 建议
# CONFIG_AW_DMC_DEVFREQ is not set    ← 建议，避免每 100ms SMC 失败 + 400ms 超时卡顿
```

原值备份在 `.config.bak-e902`。**内核源码不用改**，DTS 也不用改
（那个 patch 只加注释）。

为什么 `AW_MSGBOX` 必需：DTS 节点已经 `okay`，但编进内核的是 mainline
`sun6i-msgbox.c`，只认 `allwinner,sun6i-a31-msgbox`，跟节点的
`allwinner,sun60iw2-msgbox` 对不上 —— 节点悬空，没有驱动 probe。

### 7.4 固件配置清单（审计结果）

固件实际写的寄存器，反汇编 `soc_early_init` 确认：

| 地址 | 寄存器 | 为什么 |
|---|---|---|
`0x07010210` | `RISCV_24M_CLK` | TIMESTAMP 时基（照 `scp.fex` 补的） |
`0x0701018C` | `S_UART_BGR` | S_UART0 复位+时钟 |
`0x0701017C` | `S_MBOX_BGR` | MBOX_CPUS（RX 侧） |
`0x02002744` | `CCU_MSGBOX_BGR` | MBOX_CPUX（TX 侧），Linux 也会开 |
`0x07025000` | `PL_CFG0` | PL2/PL3 复用（读改写，只动两个 nibble） |
`0x07025024` | `PL_PUL0` | PL3 上拉 |
CLIC `0xE0800000` | CFG/MINTTHRESH/INTATTR/INTCTL/INTIE/INTIP | 矢量模式 + IRQ 48 |
CSR | `mtvec`/`mtvt(0x307)`/`mstatus.MIE` | 异常与中断入口 |

**故意不配的**：

| 寄存器 | 理由 |
|---|---|
`RISCV_BGR` `0x0701021C` | 启动方的事（`scp.fex` 也不碰，访问 0 次） |
`E902_RST_START_ADDR` `0x07032204` | 同上，由 `e902-load.sh` 设 |
`E902_DDR_REMAP` `0x07032020` | 不访问 DRAM，只用 SRAM_A2 |
`E902_AUTO_GATING` `0x07032004` | 手册说 bit0=1 是 Reserved，默认 0 就对 |
`E902_WAKEUP_MASK0/1` | 不参与 suspend；将来做 standby 才需要 |
`E902_TS_TMODE_SEL` | 测试模式，正常运行不用 |
`APBS1_CLK` `0x07010010` | **只读不写** —— boot0/u-boot/Linux 已经设好，我们读回来算波特率分频，不去改（改了会影响 Linux 侧其他 CPUS 外设） |
`PL_CFG1`/`PL_DAT` | 第一版不用 GPIO |

所以答案是：**E902 侧该配的都配了。** 剩下没配的都是有意为之，理由在上表。

---

## 8. 代价（当自己固件在跑时）

全是软失败，不是崩溃 —— Linux 通过 SMC 访问 SCP，失败就是返回错误码。

| 丢失 | 机制 | 严重度 |
|---|---|---|
DRAM 变频 | `ccu-ddr.c:77` SMC 无人应答 | 只掉性能；已关 `AW_DMC_DEVFREQ` 免得每 100ms 重试 |
suspend-to-RAM | `irq-sunxi-wakeupgen.c:49` + E902 是 STR 期间唯一醒着的核 | **`echo mem` 很可能醒不过来，要硬复位** |
PMIC 额外看护 | E902 轮询 AXP515/AXP8191 | Linux 自己的 AXP 驱动仍管 regulator |

**CPU 变频不受影响**（`ccu-sun60iw2-cpupll.c` 无 SMC 调用）。
**调试口不受影响**（`uart0` 独立于 `uart7`）。

⚠️ 自己固件在跑时**不要 suspend**。

---

## 9. 待验证（没有板子，全部未上机）

1. **PL_CFG0 mux 是否生效** —— banner 会打印 `PL_CFG0` 值和 ok/MISMATCH
2. **APBS1 频率探测是否正确** —— banner 打印算出的 Hz，跟 `0x07010010` 实际内容对
3. **msgbox TX** —— 如果"能收不能发"，就是主 CCU 写被安全限制（但 6.6 应该已解决）
4. **`scp.fex` 栈地址矛盾**（见 2.3）—— 顺便验证 bl31 的真实加载地址
5. **`e902-restore.sh` 后 SCP 冷启动是否可用** —— 拿不到参数块，可能要重启

---

## 10. 文件索引

```
e902-fw/
  Makefile                     -march=rv32emc_zicsr -mabi=ilp32e
  README.md                    使用说明、消息协议、验证步骤
  src/a733.h                   SoC 寄存器定义（E902 视角）
  src/fw.h                     跨模块原型
  src/start.S                  复位入口、.data/.bss、mtvec/mtvt、CLIC 表
  src/fw.ld                    链接脚本，镜像定位 0x40014000
  src/soc.c                    时钟/复位、APBS1 频率运行时探测
  src/pinmux.c                 PL2/PL3 复用（含竞态分析）
  src/uart.c                   S_UART0 驱动（16550 兼容）
  src/msgbox.c                 MSGBOX 传输
  src/clic.c                   CLIC 中断配置
  src/main.c                   banner、UART 回显、消息协议
  scripts/get-toolchain.sh     下载玄铁工具链（含 sha256）
  scripts/e902-load.sh         加载并启动，不用重启
  scripts/e902-restore.sh      写回厂商 scp.fex
  scripts/arm-msgbox-test.c    ARM 侧 /dev/mem 测试工具

doc/e902/
  check-e902.sh                上板体检脚本（只读）
  DESIGN-NOTES.md              本文件

userpatches/
  u-boot/u-boot-sunxi/0001-*.patch          可选：永久禁用 SCP 启动
  kernel/sun60iw2-current/0001-*.patch      仅注释，记录 uart7 = S_UART0

external/config/kernel/linux-sun60iw2-current-a733.config       已改
external/config/kernel/linux-sun60iw2-current-a733.config.bak-e902   原值
```

构建产物：`e902-fw/build/fw.bin`，2464 字节，加载到 `0x40014000`。
