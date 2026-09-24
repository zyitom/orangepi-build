# 让 E902 真正跑起来的唯一路径：bl31 / u-boot 启动链（运行手册）

日期：2026-09-22 · 依据：`FINDINGS-LEDGER.md`（F1–F9、T1–T9）、u-boot 源码逐处引用

## 1. 为什么必须走这条路

| 证据 | 结论 |
|---|---|
| `FINDINGS-LEDGER.md` T4/T5/T6/T9：连"第一条指令"的标记都写不进 SRAM | E902 **从未从我们设置的向量取指**，不是固件 bug |
| 手册 5.2.3（`E902_CFG` 仅 8 个寄存器）、4.2.5.24（`RISCV_BGR` 只有 `RISCV_CFG_RST` + 两个门控）、`ccu-sun60iw2-r.c:221-234` | **软件侧不存在 E902 内核复位位** |
| 手册 5.2.4.8 原文："It is the running PC address when E902 reset **is released**" | 释放复位这件事只发生在启动流程里 |

`RISCV_BGR` bit0 只是**时钟门控**：写 0 停时钟、写 1 恢复时钟，内核**从原地继续跑**，PC 不会被重定位。
所以"停核 → 换固件 → 设向量 → 放复位"这个链条缺了关键一环，且该环节软件不可达。

## 2. 官方启动链（源码实证）

```
boot0(SPL) → u-boot → bl31(monitor.fex) → Linux
                        ▲
u-boot arch/arm/lib/bootm.c:409-412
    u32 ARM_SVC_ARISC_STARTUP = 0x8000ff10;
    sunxi_smc_call_atf(ARM_SVC_ARISC_STARTUP, (ulong)r2, 0, 0);   // bootm 路径里调用
u-boot drivers/arisc/arisc.c:274
    arm_svc_arisc_startup((ulong)&dts_cfg_64)                     // 板级路径
u-boot drivers/arisc/arisc.c:290
    arm_svc_arisc_wait_ready();                                   // 等小核握手
```

- `dts_cfg_64`（`arisc_i.h:260+`）由 u-boot 从 FDT 组装，包含：
  - `struct space_cfg_64 space`：`sram_dst / sram_offset / sram_size / dram_dst / dram_offset / dram_size / para_* / msgpool_* / standby_*`
    —— **`sram_offset` 就是 bl31 从哪里取小核镜像、`sram_dst` 是放到哪里**
  - `struct image_cfg_64 image { u64 base; u64 size; }`
  - `struct dram_para dram_para`（DRAM 时序全套）、`arisc_freq_voltage vf[16]`（DVFS 表）、
    `dev_cfg_64 msgbox/hwspinlock/s_uart/s_twi/s_rsb/s_jtag`、`cir_cfg_64 s_cir`、`pmu_cfg pmu`、`power_cfg power`
- 配置来源：`sunxi_arisc_parse_cfg()` 从 FDT 的 `/soc/arisc_space`、`/dvfs_table` 等节点解析
  （`drivers/arisc/arisc.c`）。
- u-boot 里还有命令 `smc <fid> [args...]`（`cmd/smccc.c`，需 `CONFIG_CMD_SMC`）。

## 3. 两条可选执行路线

### 路线 A（推荐，不碰启动介质）：u-boot 控制台 + `smc`
1. 从 TL101 用 `/dev/ttyUSB0`（115200 8N1）盯住板子，触发重启，在 autoboot 倒计时里按键进 u-boot 提示符。
2. 确认能力：`help`（看有没有 `smc`）、`printenv`、`bdinfo`。
3. 把我们的固件写进 SRAM_A2 的安全区（ARM 视角 `0x00060000`，E902 视角 `0x40020000`）——
   用 u-boot 的 `mw` 逐字写，或用 `loady` 从串口灌（156 B / 5.9 KB 量级，串口完全够）。
4. 构造/复用 `dts_cfg_64`：最小可行做法是把 `space.sram_offset` 指向 u-boot 已加载到 DRAM 的镜像，
   `sram_dst = 0x40020000`，然后 `smc 0x8000ff10 <cfg_addr>`。
5. 用我们的 `heartbeat` 块（E902 视角 `0x4001E000`）确认内核是否真的跑了我们第一条指令。

**代价/风险**：bl31 会等小核握手（`arm_svc_arisc_wait_ready`）；若我们的固件不握手，
bl31 可能挂住 → 只能断电重启。**因此第 3 步之前必须先把握手实现了。**

### 路线 B（长期，需要能物理恢复）：替换启动介质里的 `scp.fex`
把 `u-boot/v2018.05-sun60iw2/scp.fex` 换成实现握手的我们的镜像，重新打包 bootloader 并刷写。
**只有在具备 SD 恢复条件时才做**（`e902/backup/sd-boot-head.img` + `restore-sd-auto.sh`）。

## 4. 必须先完成的前置：小核握手

bl31 放复位前的等待（`arm_svc_arisc_wait_ready`）说明存在一个"小核就绪"信号。

### 4.0 本轮已做的反汇编分析（`scp.fex`，vma 0x40004000）

```
40004000  li ra..a5,0            清 15 个 GPR（RV32E）
4000401e  auipc gp,0x1a           gp = 0x4001e430   （GP 相对寻址基址）
40004026  auipc sp,0x2b           sp = 0x4002f000
40004030  auipc a4,0x2b ; a4 = 0x4002ec00
40004038  sw a3,0(a4) / +4 / 循环  → 清零 0x4002EC00..0x4002F000（1KB）
40004048  csrw mtvec, 0x400040c0
4000404c  csrw mtvt , 0x400040c0   ← 确实用了 mtvt(0x307)，我们的 min3 与它一致
40004052  csrs mstatus, 8
40004056  jal 0x4000415c           → 清零 0x4001DDB8..0x4002AF3C（.bss）
4000405a  jal 0x4000b080           → 主初始化链（下面那一串外设初始化）
```

已确认的外部访问点：
- `40007334 lui a4,0x3004` + 后续 `addi a5,a5,1` → **写 `0x03004000`（CPUX_MSGBOX）**，即小核→大核的发送路径。
- `4000536e/5384/539e/b6 lui a5,0x7032` → 访问 **`0x07032000`（E902_CFG）**。
- `40005722 lui s1,0x7010` → 访问 **`0x07010000`（R-CCU）**。

**重要副产品（修正 `fw.ld` 的一处说法）**：厂商 `.bss` 区间是 `0x4001DDB8..0x4002AF3C`，
我们新的加载址 `0x40020000` 与心跳块 `0x4001E000` **都落在这段 bss 里**（不是"空闲间隙"）。
这对我们无害（厂商核被停掉后才加载我们），但**如果厂商核在跑，它会把这 1KB+ 清成 0**——
所以不能依赖这两段内存跨厂商重启保留内容。

**未找到**：反汇编里没有发现"向某个固定跨核地址写 magic"的握手动作（grep `5a5a/dead/beef/1234`
无命中）。因此 bl31 的 `wait_ready` 很可能走 **mailbox 消息**（`0x03004000`）而不是一个 SRAM magic 字。
这仍属**未证实**，需要下一步继续挖。

### 4.0b 已解出具体规格 —— 见 `FINDINGS-LEDGER.md` 的 T15

握手 = 通过 **mailbox 通道 3** 发送「启动反馈」报文，格式 `[u32 头=0][u32 字数=13][13 个 u32 数据]`，
超时 100000；对应寄存器 `MBOX_MSG(CPUX_MSGBOX,3)=0x0300407C`、`MSG_STATUS=0x0300406C`，
RX 中断使能位 `1<<6`。**注意我们现有固件用的是通道 0，必须改到通道 3。**

### 4.1 下一步三件事（按顺序）

1. 从 `scp.fex` 反汇编找它启动后**最先写**的非栈/非BSS地址（很可能是 SRAM_A2 里的一个 magic 字，
   或向 `CPUX_MSGBOX 0x03004000` 写一条消息）。
2. 用 `awdevmem.py` 在厂商运行的板子上读该地址的实时值。
3. 在我们的固件 `main()` 最前面复刻同样的写操作。

> 已有素材：`e902/e902-fw-scp/`（为 SCP 替换准备的固件副本）、`e902/scp-ours.bin/elf`、
> `e902/apply-scp.sh`、`e902/preflight-image.sh`。这些是上一位同事留下的起点。

## 5. 成功判据（可检查）

1. u-boot 里 `smc 0x8000ff10 <cfg>` 返回后，`awdevmem.py read 0x07032204` 读出我们设的向量。
2. 心跳块 `0x4001E000`：`+0x00 == 0xE902C0DE`（出现即可证明"内核执行了我们的第一条指令"，本次最有价值的一步）。
3. `+0x18 == 0x66666666`（`min3`）或主固件 `+0x00 == 0xE902C0DE` 且 `+0x01` 持续自增。
4. 心跳 `+0x0A`（`mbox_irq`）/`+0x0D`（`mbox_rx`）出现非零 ⇒ 中断与 MSGBOX 通路实测成立。

## 6. 回滚

- 路线 A：不写任何持久介质；`reset` 或断电重启即回到厂商流程。
- 路线 B：`e902/restore-sd-auto.sh` 或 `dd if=backup/sd-boot-head.img of=/dev/sdX`。
