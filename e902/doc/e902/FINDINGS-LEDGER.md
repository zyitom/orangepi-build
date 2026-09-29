# E902 上板实测记录（原始输出 + 判定）

日期：2026-09-22（TL101 本地时间 04:19–05:00）· 板子 `orangepi@172.16.0.193`
（板子自身时钟显示 `Sep 21 20:4x`，比 TL101 慢 8 小时，两者都用同一物理时刻描述）
登录：`ssh helios@TL101`，经 `ar0234-port/tools/ssh_board.sh` 上板。
**sudo 口令修正**：板子上 sudo 口令是 `orangepi`（与登录口令相同），**不是**任务描述里给的那个（那是 TL101 的）。
这是本次开工第一个被实测纠正的假设。

复现用的固件：`e902-fw/build/fw.bin`（5896 B，`sha256 504a13b5…96fac`），加载址 `0x40020000`。
探针固件：`e902-fw/build/fw_min2.bin`（768 B），只写 SRAM，不初始化任何外设。

---

## 结论状态一览

| 编号 | 命题 | 状态 | 依据 |
|---|---|---|---|
| F1 | 板子出厂跑厂商 `scp.fex`，入口 `0x40004000` | **observed** | T1、T3 原始读值 |
| F2 | `SRAM_A2` 双地址换算正确，写入可读回校验 | **observed** | T3 `verify ok (5896 bytes read back identical)` |
| F3 | 往 `0x40020000` 写固件不会破坏厂商镜像 | **observed** | T3 加载前后 `0x40004000` 前 16 字节一致 |
| F4 | 只改 `RISCV_BGR` 时钟门控**无法**让 E902 从新向量开始执行 | **reproduced** | T4/T5/T6：探针固件 scratch 恒为 0 |
| F5 | R-CCU / E902_CFG 里**不存在**软件可见的 E902 内核复位位 | **observed**（手册+代码） | 手册 5.2.3、4.2.5.24；`ccu-sun60iw2-r.c:221-234` |
| F6 | 因此 Linux 侧"借用核"（stop→load→set vector→release）路线不成立 | **confirmed**（F4 复现 + F5 反证：没有复位源就不可能重定位 PC） | F4 + F5 |
| F7 | MSGBOX 收中断号 48 是手册正确值（旧笔记怀疑的 39 是错的） | **observed**（手册文本） | 手册表 12-2 文本 33904 |
| F8 | 固件能在硬件上收发 MSGBOX | **not reproduced**（因 F6，固件根本没跑） | T7 无任何消息 |
| F9 | S_SPI 上板读取 | **not reproduced**（同上） | T3/T6 中 `0x07092004` 恒为 `0x00000000` |

---

## T1 · 基线（厂商 SCP 运行中，只读）

命令（板子上，sudo 口令 `orangepi`）：

```
python3 awdevmem.py read 0x0701021C   # RISCV_BGR
python3 awdevmem.py read 0x07032204   # E902_RST_START_ADDR
python3 awdevmem.py read 0x07025000   # PL_CFG0
python3 awdevmem.py read 0x07010010   # APBS1_CLK
python3 awdevmem.py read 0x0701018C   # S_UART_BGR
python3 awdevmem.py read 0x07092004   # r_spi GC
python3 awdevmem.py dump --e902 0x40004000 --count 16
```

原始输出：

```
0x0701021C: 0x00010003
0x07032204: 0x40004000
0x07025000: 0x1FFF1F22
0x07010010: 0x04000000
0x0701018C: 0x00000000
0x07092004: 0x00000000
40004000  81 40 01 41 81 41 01 42 81 42 01 43 81 43 01 44  |.@.A.A.B.B.C.C.D|
```

判定：
- `RISCV_BGR = 0x00010003` → cfg 块在、内核时钟在（bit0=1）。
- `RST_START_ADDR = 0x40004000` → **厂商 scp.fex 正在跑**（F1 成立）。
- `PL_CFG0 = 0x1FFF1F22`：PL2/PL3 两个 nibble 是 `2`（S_UART1-TX/RX），**不是** S_UART0 需要的 `3`。
- `0x40004000` 前 16 字节 `81 40 01 41 …` 与 `scp.fex` 一致。
- `r_spi GC = 0` → SPI 控制器未使能（与 DTS `disabled` 一致）。

---

## T2 · 构建（一条命令，可复现）

```
cd /home/helios/Desktop/orangepi-build/e902/e902-fw
make            # 默认工具链 e902/toolchains/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0
```

工具链版本：`Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0`（`riscv64-unknown-elf-gcc`），
路径 `/home/helios/Desktop/orangepi-build/e902/toolchains/...`（HOWTO 里写的 `/home/zyi/3rd_party/...`
在 TL101 上不存在，已纠正）。

产物结构（`riscv64-unknown-elf-readelf` / `objdump`）：

```
There are no relocations in this file.
  0 .text      0000101e  40020000  40020000
  1 .vectors   00000180  40021040  40021040
  2 .rodata    00000548  400211c0  400211c0
  3 .data      00000000  40021708  40021708
  4 .bss       000000c0  40021708  40021708
  5 .stack     00001000  400217d0  400217d0
入口前 16 字节：81 40 01 41 81 41 01 42 81 42 01 43 81 43 01 44
符号：__start=0x40020000  trap_entry=0x400200c0  vector_table=0x40021040
```

判定：无重定位、段数与厂商入口序列一致（前 16 字节与 `scp.fex` 相同，是入口序列正确的独立印证）。

---

## T3 · 加载 v2 固件（非破坏性地址）

```
sudo sh e902-load.sh fw.bin
```

原始输出（截取关键行）：

```
firmware : fw.bin (5896 bytes)
load addr: 0x40020000 (E902 view)
before: RISCV_BGR=0x00010003  RST_START=0x40004000
1/4 asserting reset ...
2/4 copying image into SRAM_A2 (verified read-back) ...
fw.bin: 5896 bytes
E902 0x40020000  ->  ARM 0x00060000
verify ok (5896 bytes read back identical)
3/4 setting reset vector ...
4/4 releasing reset ...
after : RISCV_BGR=0x00010003  RST_START=0x40020000
```

随后读心跳块与寄存器：

```
# 加载后 2s 与 4s 各读一次，0x4001E000 心跳块
4001e000  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
4001e010  00 00 00 00 00 00 00 00

0x07032204: 0x40020000     # 向量已改
0x07092004: 0x00000000     # r_spi GC 仍为 0 —— 固件里的 spi_init 没跑
0x07092008: 0x00000000
0x07092024: 0x00000000
0x0709201C: 0x00000000
0x07092014: 0x00000000

# 厂商镜像未被破坏
40004000  81 40 01 41 81 41 01 42 81 42 01 43 81 43 01 44
```

判定：**写入成功（verify ok）但固件一行都没执行**——心跳 magic `0xE902C0DE` 没出现。
F3 成立（厂商镜像完好），但 F4 初现。

---

## T4 · min2 探针（排除"固件自己跑挂"）

`fw_min2.bin`（768 B）：`soc_early_init()` 空实现，`main()` 只写 `0x4001E000` 然后自增计数，
**完全不碰任何外设寄存器**。清空 scratch 后按同样流程加载：

```
4001e000  00 00 00 00 00 00 00 00      # 加载前
...
verify ok (768 bytes read back identical)
...
4001e000  00 00 00 00 00 00 00 00      # 加载后仍全 0
```

判定：**不是固件 bug**——连"只写一个 SRAM 字"的探针都没执行。核心没到我们的向量。

---

## T5 · `RISCV_BGR` 取值扫描

对 `0x0701021C` 依次写 7 个值（覆盖 cfg rst / cfg gate / core gate 的全部组合），每次等待后读心跳：

| 写入 `RISCV_BGR` | 读回 | 心跳块 |
|---|---|---|
| `0x00010003` | `0x00010003` | 全 0 |
| `0x00010002` | `0x00010002` | 全 0 |
| `0x00010000` | `0x00010000` | 全 0 |
| `0x00010001` | `0x00010001` | 全 0 |
| `0x00000003` | `0x00000003` | 全 0 |
| `0x00000002` | `0x00000002` | 全 0 |
| `0x00000000` | `0x00000000` | 全 0 |

判定：写进去都读得回来（寄存器可写），但**没有任何组合能让内核重新取指**。
与手册一致：这些位只是时钟门控/CFG 块复位，不是内核复位。

---

## T6 · 显式"复位脉冲"尝试

```
写 BGR=0x00010002（cfg 在，内核时钟关）
写 RST_START=0x40020000  → 读回 0x40020000
写 BGR=0x00000002（bit16 清零，尝试 assert cfg 复位）
写 BGR=0x00010002；再写 RST_START=0x40020000
写 BGR=0x00010003（释放）
心跳块：全 0
—— 再单独脉冲 bit0（内核时钟门控）——
心跳块：全 0
```

判定：**F4 复现**。连同 T5，共 9 种序列，全部无效。

---

## T7 · ARM 侧 MSGBOX 监听

```
sudo /tmp/amt -t 5          # 只监听，期望收到 HELLO 0x10000000
rx block 0x03004000: msg_status=0x0 fifo=0x0
tx block 0x07094000: msg_status=0x0 fifo=0x0
listening 5.0s ...
0 message(s) received

sudo /tmp/amt -t 4 ping
  -> 0x01000abc  cmd=0x01 (PING)
listening 4.0s ...
0 message(s) received
```

判定：无任何消息（F8 not reproduced）。这与 T4/T6 一致：小核没在跑我们的代码。
两个 mailbox 块本身可 map、可读写（T1/T7 都读到 0），硬件通路在。

---

## T8 · 回滚验证（成功）

```
sudo sh e902-restore.sh scp.fex
restoring: scp.fex (105912 bytes) to 0x40004000 (E902 view)
before   : RISCV_BGR=0x00010003  RST_START=0x40020000
2/4 writing vendor image (verified read-back) ...
verify ok (105912 bytes read back identical)
after    : RISCV_BGR=0x00010003  RST_START=0x40004000
厂商镜像头：40004000  81 40 01 41 81 41 01 42 81 42 01 43
```

判定：**回滚可用**。`RST_START_ADDR` 已复位为 `0x40004000`，厂商镜像字节级还原。
注意：`e902-restore.sh` 只是冷启动厂商固件（拿不到 bl31 的 `dts_cfg_64` 参数块），
若要彻底干净，重启一次让 bl31 正常流程接管。

---

## 证据缺口（明确列出）

1. **没看到小核串口**：TL101 的 `/dev/ttyUSB0`（CH340）接的是**大核 Linux 控制台**
   （PB4/PB5，实测敲回车出现 `orangepi@orangepizero3w:~$`）。小核的 `S_UART0`（PL2=pin16 / PL3=pin18）
   没有第二个 USB-TTL 接着（`lsusb` 只有一个 CH340 + 一个 DM-USB2FDCAN）。因此小核 UART 只能靠
   SRAM 心跳 / mailbox 间接观测（已实现）。
2. **中断从未在硬件上真实触发过**：IRQ 29/38/48 的编号来自手册，但固件没跑到注册中断那一步，
   所以"中断计数/延迟"没有实测数据。
3. **SPI 从没在硬件上跑过**：`r_spi` 寄存器读值恒为 0（固件没执行）。
   另外板子上**没有任何 SPI 从设备**（`spi0/spi3/r_spi` 全 `disabled`，无 `/dev/spidev*`、无 MTD，
   `CONFIG_MTD_SPI_NOR is not set`），所以真要做"SPI 读取结果可校验"，需要在排针上把
   MOSI↔MISO 短接做回环，或外接一个从设备。
4. **bl31 复位释放路径未验证**：唯一能让小核从我们的向量启动的入口是 bl31（启动时载入 `scp.fex`
   并释放复位）。要让 bl31 载入我们的固件，必须替换 `scp.fex` 并满足它与 bl31 的握手；
   这涉及改写启动介质，未在本轮执行（见 `RECOVERY-AND-HANDOFF.md` 的安全边界）。

---

# 补充记录 T9（2026-09-22，同日晚些时候）：连"第一条指令"都没执行

## 为什么必须补这一刀

前面 T4 用的 `fw_min2.bin` 与主固件**共用同一个 `start.S`**。如果 `start.S` 前几条指令就 trap
（例如 `csrw 0x307` / mtvt，或 `mstatus` 写入），两个镜像都会在写心跳**之前**死掉，
于是 "内核没从我们的向量取指" 与 "内核取了指但立刻 trap" 无法区分——证据链有缺口。

## 探针

新增 `src/min3.S` + `make min3` → `build/fw_min3.bin`（**156 字节**，无重定位）。
它在**第一条指令**就往 SRAM 写标记，之后每完成一步再写一个不同的标记：

| 偏移 | 标记 | 含义 |
|---|---|---|
| +0x00 | `0xE902C0DE` | 内核执行了**我们的第一条指令** |
| +0x04 | `0x11111111` | 寄存器堆清零完成 |
| +0x08 | `0x22222222` | 栈指针装载完成 |
| +0x0C | `0x33333333` | `mtvec` 写入完成 |
| +0x10 | `0x44444444` | `mtvt`(CSR 0x307) 写入完成 |
| +0x14 | `0x55555555` | CLIC CFG/MINTTHRESH 配置完成 |
| +0x18 | `0x66666666` | 进入空转循环 |

全程不碰任何外设、不开任何中断，所以除了"指令/CSR 不被支持"之外没有别的出错途径。

## 原始输出

```
##### A. clear the scratch block
4001e000  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
4001e010  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00

##### C. load min3 at 0x40020000
firmware : fw_min3.bin (156 bytes)
verify ok (156 bytes read back identical)
after : RISCV_BGR=0x00010003  RST_START=0x40020000
--- scratch after min3 load:
4001e000  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
4001e010  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00

##### E. restore vendor
final RST_START=0x40004000
```

（另：同时刻采样厂商运行期 scratch `0x40010730` 两次、间隔 3 s，内容完全一致；
这说明该处不是厂商核的高频更新区，因此**不能**据此判断厂商核是否仍在运行——已如实记录，未作推断。）

## 判定

- `+0x00` 仍是 0 ⇒ **内核连我们的第一条指令都没有执行**。
- 结合 F5（软件侧无内核复位位），F6 由 `confirmed`（复现＋反证）**加强为无争议**：
  反证是"第一条指令不可能失败"——既然最简单的探针都不生效，失败原因只可能在"内核没有被重定位"这一层，
  与固件无关。
- 由此 **F8/F9（通信、SPI 实测）保持 not reproduced**，且原因明确归因于启动机制，而非驱动实现。

## 连带修正

- 旧文档"可以从 Linux 停核/换固件/启动"的说法**彻底作废**，见 `CORRECTION-2026-09-22.md`。
- 唯一可行路径与逐步运行手册：`BL31-PATH-RUNBOOK.md`。

---

# 补充记录 T10/T11（2026-09-22）：小核控制台通道打通 + 三通道一致的最终取证

## T10 · 控制台通道验证（线路）

设备：板子上 `lsusb` → `0403:6014 FT232H Single HS USB-UART/FIFO IC`，即**板子自己的 `/dev/ttyUSB0`**
（一块 USB2TTY 插在板子的 USB 口，串口侧接排针 pin16/pin18 = E902 `S_UART0`）。

线路验证（把 PL2 临时当 GPIO 输出翻转，同时抓板子 `/dev/ttyUSB0`）：

```
baseline bytes: 0
toggle-phase bytes: 10
PL_CFG0 restored = 0x1FFF1F22   PL_DAT = 0x0000020C
```

判定：基线 0 字节、有信号时 10 字节 ⇒ **该串口确实挂在 PL2(pin16) 上，是 E902 的控制台**。
`PL_CFG0` 测试后已还原。

## T11 · 加载固件 + 同时抓控制台（三通道一致）

```
##### 0. state before
PL_CFG0 = 0x1FFF1F22
RST_START = 0x40004000
##### 1. start console capture (10 s) on the E902 UART
##### 2. load the v2 firmware
##### 3. bytes received from the E902 console during load: 0
      (silent -- the core never reached our firmware)
##### 4. state after
PL_CFG0  = 0x1FFF1F22   (would be ...33.. if our pinmux had run)
RST_START= 0x40020000
r_spi GC = 0x00000000   (0x5 would mean our spi_init ran)
4001e000  00 00 00 00 00 00 00 00
##### 5. restore vendor
final RST_START = 0x40004000
```

判定：**三个独立通道同时指向同一结论**——
① 小核自己的串口 0 字节；② SRAM 心跳块全 0；③ 外设寄存器未被编程（`r_spi GC=0`、`PL_CFG0` 未变）。
"内核从未从我们设置的向量取指"由此成为**多源交叉证实**的结论，不再依赖单一证据。

## 对 F8/F9 的状态更新

- F8（MSGBOX 双向）、F9（SPI 读取）**仍为 not reproduced**，原因不变（核未运行）。
- 但它们**现在具备了直接取证的通道**：控制台可读 → 一旦核跑起来，banner / 中断计数 / SPI 结果都能直接看到。

## 更正

- `RISKS.md` 风险 A6（"小核 S_UART0 没有引出、只能间接观测"）**已解决**，见 `CONSOLE-CHANNEL.md`。
- `RECOVERY-AND-HANDOFF.md` §7.3 与 `BL31-PATH-RUNBOOK.md` §3 里"先接小核串口"的前置条件**已完成**。

---

# 补充记录 T12（2026-09-22）："还有没有别的复位/启动入口"——系统性排查结论

按"软件侧还能不能启动 E902"逐条排查，全部为**否定或不可安全使用**，逐条留证：

| 候选入口 | 结论 | 证据 |
|---|---|---|
| R-CCU 复位表 | **无 E902 内核复位位** | `ccu-sun60iw2-r.c:221-234` 全表；只有 `RST_BUS_RISCV_CFG`(0x021C bit16) |
| `E902_CFG`(0x07032000) | **8 个寄存器，无复位** | 手册 5.2.3 寄存器清单（文本 18371-18385） |
| `RISCV_BGR`(0x0701021C) bit0 | **只是时钟门控**，写 0 停时钟、写 1 原 PC 续跑 | 手册 4.2.5.24；T5/T6/T9 实测 9 组序列全无效 |
| u-boot `smc` 命令（路线 A 的关键） | **未编译进这版 u-boot** | `sun60iw2p1_t736_defconfig` 无 `CONFIG_CMD_SMC`；`u-boot.bin`/`u-boot` 里 `strings` 搜不到 `smc`/`hvc`；`cmd/smccc.c` 里 `#ifdef CONFIG_CMD_SMC` |
| Linux 侧通用 SMC 接口 | **不存在** | 板子 `/sys/firmware/` 只有 `devicetree`/`fdt`；`psci/method = smc`（仅内核 PSCI 内部用）；无 `/dev/tee*`、无 OP-TEE |
| `/arisc_config` DT 节点 | 存在但是**死节点**，无代码解析；不过它**印证了 PL2/PL3 是 SCP 控制台** | 板上实读：`pins = "PL2" "PL3"`、`status = "disabled"`；u-boot/kernel 全树无解析代码 |
| u-boot `arisc_space` 节点 | **不存在** | 活 DT 的 `/proc/device-tree/` 下无 `arisc_space`；只在 `arisc.c` 源码里出现（解析失败则 `dts_cfg_64` 基本为 0，bl31 用自己的默认） |
| CPUS 电源域复位（VDD_CPUS） | **控制寄存器未公开** | 手册只在 RTC 章提到"VDD_CPUS 电源域里有一批记录 BROM 信息的寄存器"；CPUS 域另有 `S_SPC`(0x07002000) 块，手册**没有寄存器清单**（只有内存映射一行）。盲写未公开寄存器不安全，未执行 |
| `PRCM_SEC_SWITCH_REG`(0x0290) | 只是**安全位**，不是复位本身 | 手册 4.2.5.26：bit2 `POWER_SEC` 等描述的是"这些寄存器是否安全"，其正文提到存在 "VDD_SYS power domain reset register"，但该寄存器本体未在文档中给出 |
| JTAG | `JTAG_PAD_SEL_REG`(0x0701037C) 存在（bit0 `RISCV_JTAG_SEL`、bit1 `CPUS_JTAG_SEL`），板上实读 **0x00000000**；本轮用户已澄清所接为 FT232H USB2TTY、**非调试探针**，故不走此路 | 手册 4.2.5.35；板上实读 |

## 结论

**在"不写启动介质、不盲写未公开寄存器"的前提下，软件侧没有可用的 E902 启动入口。**
可行的只剩两条，都要动启动介质或接受断电恢复风险，已整理成 `OPTIONS-TO-START-E902.md`。

---

# 补充记录 T13（2026-09-22）：SRAM_A2 只读扫描 —— 厂商 SCP 是"活程序"的直接证据

方法（零风险、只读，脚本 `scan_sram.py` 走 `awdevmem.py` 的 mmap 通道）：
厂商 SCP 启动时会**主动清零自己的 `.bss`**（`0x4001DDB8..0x4002AF3C`，见 `scp.fex` 反汇编
`0x4000415c` 处的清循环）和另一段 `0x4002EC00..0x4002F000`。
因此：**在这两段"被清零过"的区域里出现的非 0 内容，必然是启动之后才写进去的运行时数据。**

## 结果

```
=== SRAM_A2 below vendor image : E902 0x40000000..0x40003FFF (16384 bytes) ===
  non-zero runs: 32, total 32 bytes
  +0x00007 (40000007) len=1 : 15
  +0x00107 (40000107) len=1 : 15
  ... （每 0x100 一个，共 32 个，值都是 0x15） ...

=== SRAM_A2 upper half : E902 0x4002A000..0x40033FFF (40960 bytes) ===
  non-zero runs: 3032, total 29023 bytes
  +0x00C99 (4002AC99) len=9 : 10 02 07 cc de 01 40 4e 60
  +0x00CA8 (4002ACA8) len=2 : 4e 60
  +0x00CB0 (4002ACB0) len=2 : 4e 60
  ... （0x4002AC99 起密集的 8/16 字节步长表项，形如 ... 40 | 4e 60 ...） ...
```

## 判定

1. **厂商 SCP 是在运行的活程序**（`observed`）：`0x4002AC99` 等处落在它启动时清零过的
   `.bss` 里，却含 29023 字节非 0 的、有明显表结构的运行期数据。这从侧面印证了
   F4/F6 的机制解释——**我们做的只是把它的时钟关掉再打开，它从原地继续跑，PC 从未被重定位**。
   （补充：u-boot 自己的 `dts_cfg_64` 也可能往 SRAM 写，但 `.bss` 区间归属厂商固件无疑。）
2. **`0x40000000..0x40004000` 这 16K 未解释**（`candidate`）：32 个非 0 字节，位于
   `+0x007`、`+0x107`、`+0x207` … 每 `0x100` 一个，值恒为 `0x15`。
   它不是厂商镜像（镜像从 `0x40004000` 开始），也不在 SCP 的清零范围内。
   可能是 SRAM 初始化pattern、ECC/奇偶位，或 bl31 预置的标志。**未证实，列为待查项 B8**。
   注意：这 16K 目前**没有任何已知使用者**，若将来要做"放一个跨核共享小结构"的实验，这里是候选位置。

## 未做了的事（明确说明）

本次扫描**没有**给出 bl31 握手标志的确切位置。结论仍是 `BL31-PATH-RUNBOOK.md` §4.1 列出的那三件事，
只是现在多了一条判据：**握手标志若存在，应当出现在"被清零过又是非 0"的区间里**（即
`0x4001DDB8..0x4002AF3C` 或 `0x4002EC00..0x4002F000`），可用同一方法继续缩小范围。

---

# 补充记录 T14（2026-09-22）：从 `scp.fex` 反汇编提取厂商固件的**真实资源足迹**

方法（零风险、纯静态）：`riscv64-unknown-elf-objdump -D -b binary -m riscv:rv32 --adjust-vma=0x40004000`
反汇编 105912 字节的 `scp.fex`，抽取 objdump 为 `auipc+addi` / `lui+addi` 解析出的全部有效地址注释
（脚本 `scp_addrs.py`），按区域归类。

## 关键结论 1：SCP **不碰自己内存之外的任何 SRAM**

```
--- SRAM-OTHER (1 distinct) ---
    0x4002F000  refs=2
```

唯一一个落在"自己的镜像+bss+栈"之外、又在 SRAM 区间内的地址是 **`0x4002F000`——那正是它自己的栈顶**
（`sp` 由 `auipc sp,0x2b` 得到 `0x4002F000`）。

⇒ **厂商固件没有向任何外部 SRAM 位置写"握手标志"。** 因此与 bl31 的握手只可能是：
① 走 **mailbox**（它的足迹里有 `0x03004001` = CPUX_MSGBOX）；或
② 写在**它自己的 bss 里**（bl31 可以读 SRAM，但那需要一个 bl31 已知的固定地址）；或
③ 根本不是内存标志。
这条把 `BL31-PATH-RUNBOOK.md` §4.1 的搜索范围**大幅缩小**了（原来要在 208K SRAM 里找）。

## 关键结论 2：SCP 实际访问的 CPUS 域寄存器（42 个，按手册名对照）

| 地址 | 名称（手册） | refs |
|---|---|---|
| `0x07010000` | STBY_PRCM / AHBS_CLK_REG | 2 |
| `0x0701000C` | APBS0_CLK_REG | 1 |
| `0x07010100` | S_TIMER0_CLK_REG | 3 |
| `0x0701011C` | S_TIMER_BGR_REG | 2 |
| `0x0701013C` | S_PWM_BGR_REG | 1 |
| `0x0701017C` | **S_MBOX_BGR_REG**（CPUS 读侧邮箱） | 2 |
| `0x0701018C` | **S_UART_BGR_REG**（S_UART0/1） | 2 |
| `0x0701019C` | S_TWI_BGR_REG | 2 |
| `0x070101C0` | S_IRRX_CLK_REG | **6** |
| `0x070101CC` | S_IRRX_BGR_REG | 2 |
| `0x0701020C` | RTC_BGR_REG | 2 |
| `0x07010210` | **RISCV_24M_CLK_REG** | 1 |
| `0x07010244` / `0x07010250` / `0x07010254` / `0x07010260` | PRCM 内四个寄存器（手册该章未列出，**待查**） | 2 / **12** / 1 / 2 |
| `0x07010324` | NMI_INT_EN_REG | 3 |
| `0x0701033C` | BUS_ACG_REG | 1 |
| `0x070240C0` | 0x07024000 块内 | 1 |
| `0x07024FE0` | S_GPIO(PL) 附近 | 2 |
| `0x07032001` | **E902_CFG 区**（`0x07032000`+1） | 1 |
| `0x07050010` / `0x07050014` / `0x07050220` | 0x07050000 块 | 1/1/1 |
| `0x07080080` / `0x07080084` / `0x070800A4` | **S_UART0**（TFL/RFL/HALT） | 1/1/3 |
| `0x07082FFF` / `0x07083001/04/0C/10/1C/20` | **S_TWI0**（PMIC 总线） | 1/… |
| `0x0709010C` / `0x0709015C` / `0x07090160` / `0x070901F4` / `0x07090310` / `0x07090FD0` | 0x07090000 块（**待查**） | 3/1/1/2/1/1 |

CPUX 域（跨域访问）：`0x03004001`（**CPUX_MSGBOX**，2 refs）、`0x030060A0`、`0x03000200`、`0x03000160`、`0x03000000`。
主 CCU（跨域写）：`0x02002744`（**MSGBOX0_BGR**）、`0x02003300/04/08/0C`（各 6 refs）、`0x02003340`、`0x02002588`、`0x02002724`、`0x02002C00/0C` 等。

## 对"共享资源"的补充意义

以前资源归属表主要依据**手册 + 内核源码**。现在多了一类证据：**厂商固件自己实际碰了哪些寄存器**。
凡是它碰过、而大核 Linux 也在用的，就是**真正的共享点**（例如 `RISCV_24M_CLK`、`S_MBOX_BGR`、
`MSGBOX0_BGR`、`S_TWI0`（PMIC 总线）、`S_UART_BGR`）。反过来，它**完全没碰**的（例如 `S_SPI` = `0x07092000`
区、`RISCV_BGR` = `0x0701021C`）就可以确认是"小核可以放心独占"的位置。

> 注意：`RISCV_BGR(0x0701021C)` 出现在 `0x070101C0` 之外——上表里**没有** `0x0701021C`，
> 与 DESIGN-NOTES §3 的结论一致（厂商固件不碰 `RISCV_BGR`，那是启动方的事）。

---

# 补充记录 T15（2026-09-22）：**解出厂商 SCP 的 mailbox 协议与启动握手（含具体规格）**

这是"让 bl31 接受我们的固件"所缺的最后一块拼图。全部来自对 `scp.fex` 的反汇编（静态、零风险），
脚本 `scp_mbox*.sh`；**未在硬件上验证**，标注为 inferred（强推断），需在真正刷写时用控制台确认。

## 1. 通道：**用 CPUX_MSGBOX 的通道 3**（不是通道 0）

| 用途 | 访问的寄存器 | 地址 | 依据 |
|---|---|---|---|
| SCP → ARM 发送 | `MBOX_MSG(CPUX_MSGBOX, 3)` | `0x03004000 + 0x70 + 4*3` = **`0x0300407C`** | `4000724a lui a5,0x3004` + `40007250 sw a4,124(a5)` |
| 检查 TX 空闲 | `MBOX_MSG_STATUS(CPUX_MSGBOX, 3)` | **`0x0300406C`** | `400071b6 lui s0,0xc01; addi s0,s0,24; add s0,s0,a0(a0=3); slli s0,2` → `(0xC01018+3)*4 = 0x300406C` |
| SCP 接收（ARM → SCP） | `MBOX_MSG(MBOX_CPUS, 3)` | **`0x0709407C`** | `40007274 lui a5,0x7094` + `40007278 lw a4,124(a5)` |
| 检查 RX 就绪 | `MBOX_MSG_STATUS(MBOX_CPUS, 3)` | **`0x0709406C`** | `400071ee lui s0,0x1c25; addi s0,s0,24; add s0,s0,a0; slli s0,2` → `(0x1C25018+3)*4 = 0x709406C` |

两个等待原语（都以 `a0=通道号` 传入，超时用 `jal 0x40006676` 延时一个 tick，
耗尽后返回 `-35`）：

- **`0x400071b2`** = `mbox_wait_tx_empty(ch, spin)`：等 `MSG_STATUS(CPUX_MSGBOX,ch) == 8`（8 = FIFO 深度 ⇒ 空）。成功返回 0。
- **`0x400071ea`** = `mbox_wait_rx_ready(ch, spin)`：等 `MSG_STATUS(MBOX_CPUS,ch) != 0`。成功返回 0。

> ⚠️ 对本次目标的影响：我们现有固件用的是**通道 0**（`MBOX_CH 0`、`MQB_RD_IRQ_EN_BIT(0)`）。
> 厂商与 ARM 侧约定是**通道 3**。要兼容 bl31/既有协议必须改用通道 3；
> 对应 RX 中断使能位是 `MBOX_RD_IRQ_EN_BIT(3) = 1 << 6`（手册 `MBOX_RD_IRQ_EN = b+0x20`）。

## 2. 报文格式：**`[u32 头][u32 字数][u32 数据...]`**

发送函数 `0x4000721e`（`a0=报文结构指针, a1=超时`），核心序列（`0x4000724a` 起）：

```
lw  a4, 0(s1)        ; word0  = 报文头        -> 写 MBOX_MSG(CPUX_MSGBOX,3)
lui a5, 0x3004 ; sw a4, 124(a5)
lbu a4, 4(s1)        ; count  = 结构偏移 +4 的字节 -> 写 MBOX_MSG(CPUX_MSGBOX,3)
lui a5, 0x3004 ; sw a4, 124(a5)
; 然后循环 count 次：
lw  a3, 0(a4_data)   ; 从 *(结构+0x1C) + i*4 取 u32 -> 写 MBOX_MSG(CPUX_MSGBOX,3)
lui a4, 0x3004 ; sw a3, 124(a4)
```
即**先发一个字（头），再发一个字（字数），再发字数个数据字**。
报文结构布局（从 `0x4000b1d2` 附近的初始化看出）：`[+0]u32 头`、`[+1]u8`、`[+2]u16=0x0090`、
`[+4]u8=count`、`[+0x1C]u32 数据指针`，整体 `memset` 52 字节。

## 3. **启动反馈（= bl31 等的握手）** `0x4000b1b0..0x4000b220`

```
4000b1b8 jal 0x40008060        ; memset(pkt, 0, 52)
4000b1c8 jal 0x4000ae0a        ; log "feedback startup result [%d]" , 0
4000b1d2 sw  s1, 28(sp)        ; pkt.data_ptr = s1
4000b1d6 sb  a5(=2), 1(sp)     ; pkt[1]  = 2
4000b1de sh  a5(=0x90), 2(sp)  ; pkt[2..3] = 0x0090
4000b1f2 sb  a5(=13), 4(sp)    ; pkt[4]  = 13   (count)
4000b1f6 jal 0x40007ecc        ; 用 51 字节填充数据区(源 0x4001067C)
4000b1fa li  a1, 0x186a0       ; 超时 = 100000
4000b202 jal 0x4000721e        ; *** 发送启动反馈报文 ***
4000b21a jal 0x4000ae0a        ; log 成功/失败
```
随后打印 `"startup feedback ok"`（字符串在 `0x40010314`）。

**⇒ 握手动作 = 通过 mailbox 通道 3 发出一个「启动反馈」报文（头 0 / 计数 13 / 13 个数据字），超时 100000。**
这就是 bl31 `arm_svc_arisc_wait_ready()` 在等的东西（强推断：这些字符串只在这一处被引用，
且位于启动初始化链里；仍未在硬件上确认）。

## 4. 旁证字符串（`scp.fex` 内，均在镜像里）

```
0x4000F8F4  send asyn message
0x4000F908  send feedback message
0x4000F920  feedback hard syn message : %x
0x400101A8  loopback message request
0x400102C8  feedback startup result [%d]
0x40010314  startup feedback ok
0x4001026C  message manager ok
0x40010244  hwmsgbox driver ok
0x40010208  debugger system ok
0x4001032C  ar100 firmware version : %s
0x40010???  broadcast 24mhosc power-on ready / 24m hosc power-on ready notify
```

## 5. 对 BSP 代码的具体改动清单

1. `msgbox.c`：`MBOX_CH` 从 `0` 改为 **`3`**；RX 中断使能位随之变 `1<<6`。
2. 新增 `mbox_send_packet(hdr, data, count)`：按 `[hdr][count][data...]` 发送（含 `wait_tx_empty`）。
3. 启动时先发**启动反馈报文**（头 0、计数 13、13 个字，超时 100000），再进入正常协议；
   若 bl31 路线成立，这是它继续启动的条件。
4. 保留通道 0 的旧协议作为兼容模式（`#define MBOX_CH_LEGACY 0`）。

## 6. 状态与下一步

- 上述规格：**inferred（强推断）**，来源是反汇编 + 字符串交叉印证，**未上机验证**。
- 下一步（零风险）：把 `e902-fw-scp/` 的固件按本规格改造，用 `arm-msgbox-test`（需改到通道 3）
  在**厂商 SCP 不运行时**先本地自发自收验证格式。
- 再下一步（需现场条件）：按 `OPTIONS-TO-START-E902.md` 执行刷写，并用**板子的 `/dev/ttyUSB0`**
  观察横幅与握手结果。

---

# 补充记录 T15b（2026-09-22）：握手规格已落成代码并通过**指令级**验证

按 T15 的规格改了固件，重新编译并反汇编核验。

## 改动

| 文件 | 改动 |
|---|---|
| `src/msgbox.c` | `MBOX_CH` 0 → **3**；RX 中断位改 `1<<6`；新增 `msgbox_wait_tx_empty()`、`msgbox_send_packet()`、`msgbox_send_startup_feedback()`；ISR 增加"包首字"记录（`mb_last_pkt_hdr`/`mb_pkt_count`） |
| `src/fw.h` | 新增上述三个原型 + 两个全局 |
| `src/main.c` | `msgbox_init()` 之后**先发启动反馈报文**（头 0 / 13 个字），再发 HELLO |
| `scripts/arm-msgbox-test.c` | `#define CH` 0 → **3**（与固件同通道） |

## 构建结果

```
--> build/fw.bin  (6232 bytes, loads at 0x40020000)
   text 6208  data 0  bss 4296
sha256 bba6d9ab5ef3afe45b97149b5cca04420d754b71eb5d95f5e3e4c5f5f39240ca
```

## 指令级核验（`objdump -d build/fw.elf`）

| 期望 | 反汇编实际 | 判定 |
|---|---|---|
| TX 写 = `MBOX_MSG(CPUX_MSGBOX,3)` = `0x0300407C` | `40020522 lui a4,0x3004` … `4002052c sw a0,124(a4)` → `0x0300407C` | ✅ |
| RX 状态 = `MBOX_MSG_STATUS(MBOX_CPUS,3)` = `0x0709406C` | `4002061e addi a4,a4,108` → `709406c` | ✅ |
| RX 数据 = `MBOX_MSG(MBOX_CPUS,3)` = `0x0709407C` | `40020622 addi a3,a3,124` → `709407c` | ✅ |
| RX 中断 pending 位 = `1<<6` = 64 = `MBOX_RD_IRQ_STAT` (`0x07094024`) | `40020632 lui a5,0x7094`；`40020636 li a4,64`；`4002063a sw a4,36(a5)` → `0x07094024 = 64` | ✅ |
| 新符号存在 | `msgbox_send_packet`@`0x40020580`、`msgbox_send_startup_feedback`@`0x400205d4`、`msgbox_wait_tx_empty`@`0x40020564`、`msgbox_isr`@`0x400205f8` | ✅ |

## 状态

- **代码与规格一致（observed，指令级）**；但它**仍未在硬件上运行过**——内核依旧无法被重定位（T9/T11/T12）。
- 这套改动正是 `OPTIONS-TO-START-E902.md` 里选项 1（让 bl31 载入我们的镜像）所必需的前置。
- 仍未验证：启动反馈报文是否**确实是** bl31 `arm_svc_arisc_wait_ready()` 所等待的东西。
  这条只有在真正刷写并用板子的 `/dev/ttyUSB0` 观察时才能确认。

---

# 补充记录 T16（2026-09-22）：真机验证 mailbox 通道 3 的**写入通路**（并证伪"厂商 SCP 可当活对手方"）

目的：T15 解出的报文规格只有静态证据。想用**此刻正在运行的厂商 SCP** 当活的对手方来验证，
于是写了一个按规格发报文的工具（`e902-fw/scripts/amt_pkt.c`，通道 3、`[hdr][count][data...]`），
向它的 RX 队列发 `cmd=0x61`（"loopback message request"）并监听回应。

## 原始输出

```
##### 0. 发送/接收寄存器快照（通道 3）
CPUX_MSGBOX 0x03004000 (ARM 读): RD_IRQ_EN=00000000 RD_IRQ_STAT=00000040
  ch3 MSG_STATUS=00000000 FIFO_STATUS=00000000
S_MBOX 0x07094000 (ARM 写):      RD_IRQ_EN=00000000 RD_IRQ_STAT=00000001
  ch0 MSG_STATUS=00000001 FIFO_STATUS=00000000
  ch3 MSG_STATUS=00000000 FIFO_STATUS=00000000

##### 1. loopback 请求（hdr=0x00610100 flags=1 cmd=0x61, 1 个字 0xDEADBEEF）
before: rx_msg_status=0x0  tx_msg_status=0x0
sending packet: hdr=0x00610100 (cmd=0x61 flags=0x01) count=1
after send: tx_msg_status=0x3 tx_fifo=0x0        <-- 3 个字进了 SCP 的 RX 队列
listening 6.0s ... 0 packet(s) seen

##### 2. 再来一次
before: tx_msg_status=0x3
after send: tx_msg_status=0x6                    <-- 累加到 6，无人取走
listening 8.0s ... 0 packet(s) seen

##### 3. 之后
S_MBOX RD_IRQ_STAT=00000041   (bit0=ch0, bit6=ch3 挂起)
  ch3 MSG_STATUS=00000006
```

## 判定

1. **ARM → SCP 的写入通路是真实有效的**（`observed`）：
   写 `MBOX_MSG(MBOX_CPUS,3)=0x0709407C` 后，`MSG_STATUS(0x0709406C)` 从 0→3→6，
   且 `S_MBOX RD_IRQ_STAT` 的 **bit6（通道 3 的读中断挂起位）被置起**。
   这从硬件上证实了 T15 的通道/寄存器结论。
2. **运行中的厂商 SCP 不服务邮箱**（`observed`）：`S_MBOX RD_IRQ_EN=0`（它根本没开收中断），
   消息只入队不被取走，8 秒无回应。⇒ **不能拿它当活的协议对手方**，
   关于 `cmd=0x61` 回环响应的行为**仍未在硬件上验证**（`not reproduced`）。
3. 附带观察（`observed`，未解释）：`S_MBOX ch0 MSG_STATUS=1` —— **在我们动手之前**，
   通道 0 里就已经有 1 条没人取走的消息（很可能是启动阶段 bl31↔SCP 的遗留）。列为待查项 **B9**。

## 残留状态与清理方式（如实记录）

本次实验在 SCP 的 RX 队列里留下 **6 个字**（两个包）：
`[0x00610100][0x00000001][0xdeadbeef]` 与 `[0x00610100][0x00000001][0x12345678]`。

- **ARM 无法清掉它**：从 ARM 读 `MBOX_MSG(MBOX_CPUS,3)` 返回 0 且不减少计数
  —— 该 FIFO 的读端口属于 CPUS 侧，符合手册"CPUX 写、CPUS 读"的定义。
- 清理方式（二选一，均未执行）：① 写 `S_MBOX_BGR(0x0701017C)` 的 bit16 = 0→1 复位该块（会清 FIFO）；
  ② 重启板子。
- **风险评估**：这 6 个字构成两个"格式合法"的 `cmd=0x61` 包；即使 SCP 将来读到，
  分发表里 0x61 的处理器只是打印 "loopback message request" 并返回 0，无副作用。
  因此判定为**惰性残留**，未采取复位动作（宁可留下可解释的残留，也不去动 SCP 的外设块）。

---

# 补充记录 T17（2026-09-22）：启动介质布局完全解出，刷写与回滚变成精确可控

## 做了什么（全部**只读**）

在板子上 `dd if=/dev/mmcblk1 bs=1M count=24` 取启动介质前 24 MiB，
把它带回 TL101（两侧哈希一致），然后离线解析。

## 结果

1. **启动介质识别**：`/dev/mmcblk1`（59.7 G，根 `mmcblk1p1`）；无 MTD；
   boot0 在偏移 `0x2000`（`eGON.BT0` magic 实测）。
2. **启动包容器**：偏移 **`0x1004000`**，magic `sunxi-package`，条目数 `3`。
3. **TOC 完整解出**（相对偏移 + 0x1004000）：

   | 名称 | rel offset | size | 绝对地址 |
   |---|---|---|---|
   | `u-boot` | `0x800` | `0x124000` | `0x1004800` |
   | `monitor`(bl31) | `0x124800` | `0x13311` | `0x1128800` |
   | **`scp`** | `0x137C00` | **`0x19DB8`** | **`0x113BC00`** |

4. **双重交叉验证**：独立搜索 dump 里 `81 40 01 41 …` 得到 `0x113BC00`，与 TOC 一致；
   该处 105912 字节 sha256 = `07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749`
   = 工程里 `scp.fex` 的 sha256。**位置与内容都确认。**
5. **条目字段** `(offset, size, 0, type=3, 0)`：**没有发现非零的条目校验和**（"未发现"，非"确认没有"）。

## 产出的可用产物

| 产物 | 说明 |
|---|---|
| `doc/e902/FLASH-SPEC.md` | 布局、精确刷写/回滚规格、注意事项 |
| `e902-fw/scripts/flash-scp.sh` | 外科式写入 scp 条目（尺寸强校验 + 读回哈希校验 + 需输入 YES） |
| `e902-fw/scripts/restore-scp.sh` | 回滚 scp 条目（校验厂商哈希 + 读回校验） |
| `e902/backup/sd-boot-head-20260922.img` | **经校验**的 24 MiB 引导头（sha256 `114cd1f3…`，板子侧/TL101 侧一致） |
| `e902/backup/scp.fex.on-medium.bin` | 介质上那 105912 字节的副本（sha256 `07e6b976…`） |
| `doc/e902/{analyse_pkg,decode_toc,parse_pkg}.py`、`board_medium_read.sh`、`find_scp_on_medium.py` | 本次解析所用的可复现工具 |

## 需要点明的一个前提

外科式刷写要求固件**补齐到恰好 105912 字节**；且**入口地址必须与 bl31 设置的
`RST_START_ADDR` 一致**。我们当前固件的 `ORIGIN = 0x40020000` 是为"从 Linux 侧手工加载"设计的；
若改由 bl31 在 `0x40004000` 载入，必须把 `fw.ld` 改回 `0x40004000` 再编。
这条已写入 `FLASH-SPEC.md` 与 `BL31-PATH-RUNBOOK.md`，**刷写前必须先改**。

## 旧备份不一致（如实记录）

`e902/backup/sd-boot-head.img`（00:18，`665cd428…`）与当前介质（`114cd1f3…`）**不一致**，
原因未查明。刷写前请以 `sd-boot-head-20260922.img` 为准。

---

# 补充记录 T19（2026-09-22）：**失败安全设计** —— 握手包前移到 `main()` 最前

## 为什么改

bl31 的流程是：载入镜像 → 设 `RST_START_ADDR` → 放复位 → **`arm_svc_arisc_wait_ready()` 等小核报就绪** → 继续启动 Linux。
如果这个"就绪"信号没到，**挂起发生在 bl31 里，板子根本起不来**。

原先把握手包放在 `main()` 中段（引脚复用、UART、SPI 初始化之后）。这意味着：
只要中途任何一步出错（引脚写失败、UART 初始化卡住、SPI 时钟没开导致访问 fault），
握手就发不出去 → bl31 挂住 → 需要读卡器才能恢复。

## 改法

把 `msgbox_init()` + `msgbox_send_startup_feedback()` **提到 `main()` 的开头**，
在任何外设初始化之前（只保留 `hb_init()` 写 SRAM 标志）。

## 指令级确认（`objdump -d build/fw-scp.elf`，`main()` 的调用序列）

```
40004be4: jal 400049be <hb_init>
40004be8: jal 40004a06 <hb_stage>
40004bea: jal 40004514 <msgbox_init>
40004bec: jal 40004532 <msgbox_send_startup_feedback>   ← 握手最先
40004bf0: jal 400041bc <pinmux_s_uart0>
40004bf8: jal 40004a06 <hb_stage>
40004c00: jal 40004216 <uart_init>
40004c04: jal 4000420e <pinmux_read_pl_cfg0>
...
```

## 效果（这是本轮最重要的风险削减）

| | 改前 | 改后 |
|---|---|---|
| 握手失败/初始化中途挂掉 | bl31 等不到就绪 → **板子起不来，需读卡器** | 握手已先发出 → **板子照常启动**，最坏只是小核后续死掉（丢 DRAM 变频/suspend） |

也就是说：**唯一的"砖"风险被压到只剩"握手包格式猜错"这一种**；而握手包的格式不是猜的——
它来自 `scp.fex` 反汇编（`0x4000b1d2..0x4000b202`：memset 52B → count=13 → 通道 3 发包 → 打印 `"startup feedback ok"`）。

## 产物更新

| 文件 | 大小 | sha256 |
|---|---|---|
| `fw-out/scp-ours-padded-105912.bin`（待刷） | 105912 | `063f4130c8ec96e524199cd909f39134e17377879c3b30d8129d9332665ddc15` |
| `fw-out/scp-ours-6208.bin` | 6208 | `711f604a5f0245e76235ae142d25bd67139c677bd9301832aa45df139db88e34` |

（旧版 6232/`94f9d088…` 已被本次覆盖，请以本页 sha256 为准。）

## 仍未验证

`msgbox_send_startup_feedback()` 是否**真的**能喂饱 bl31 的 `wait_ready`，只有实际刷写并用板子
`/dev/ttyUSB0` 观察才能确认。但**即使猜错，最坏后果也已从"砖"降级为"小核不可用"**——
因为我们的固件不写任何持久介质，回滚 `restore-scp.sh` 在板子正常启动后随时可执行。

---

# 补充记录 T20（2026-09-22）：**原样写回测试** —— 最后一道机械环节证实，且介质零改变

## 动机

写入是唯一没做过的动作，之前没人验证过三件"机械前提"：
① `/dev/mmcblk1` 那个裸区到底**能不能写**（不是只读、不被内核拒绝）；
② `dd` 的 `seek/count/conv=notrunc` 到底**落点对不对**；
③ 读回校验链路**真的能比较到正确的字节**吗。

## 做法（零内容改变）

读出 `0x113BC00` 处那 105912 字节，**把完全相同的字节写回去**，再读回比较三个哈希。
因为写入内容与原有内容一致，**介质最终逐字节不变**；不改变固件加载的任何东西，故不影响启动行为。

## 原始输出

```
##### 0. device read-only flags
/sys/block/mmcblk1/ro = 0
blockdev --getro      = 0

##### 1. read BEFORE
before sha256: 07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749

##### 2. write the SAME bytes back (content-preserving)
write command issued

##### 3. sync + read AFTER
after  sha256: 07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749

##### 4. verdict
before=07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749
after =07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749
RESULT: IDENTICAL -> raw region is WRITABLE and the write path is correct

##### 5. whole-head integrity
24MiB  sha256: 114cd1f3a792b346acd87c8a550d4b3a09b5bfa7cce7cf0c406fa602b7bd4a54
（与经校验备份 sd-boot-head-20260922.img 完全一致）

##### 6. board still healthy
0x07032204: 0x40004000
up 9:26
```

## 判定

- 裸区**可写**（`ro=0` 且写入成功）—— `observed`
- `dd` 落点**精确**（写入后读回与写入源一致）—— `observed`
- 读回校验**有效**（能正确比较字节）—— `observed`
- **介质零改变**：写入前后 24 MiB 引导头哈希相同（`114cd1f3…`）—— `observed`
- 板子健康：`RST_START` 仍为 `0x40004000`，`up 9:26`

## 意义

至此，刷写链条上**每一个机械环节**都被验证过：
偏移正确（T17）· 长度匹配槽位（T17/T18）· 内容格式正确（T18）· 介质可写（T20）·
落点正确（T20）· 读回校验有效（T20）· 回滚产物经校验（T17）· 握手包格式来自反汇编（T15）·
且握手已前置（T19，失败安全）。

**唯一仍未验证的，只剩下"bl31 是否真的接受我们的握手包"这一条**——它只能在真正刷写并重启后才能知道。

---

# 补充记录 T21（2026-09-22）：首次真机刷写 → bl31 拒绝当前握手（失败模式已确认），并已修正 + 做好可烧镜像

## T21a · 首次真机刷写（已执行）

- 写入 `0x113BC00` 那 105912 字节：**成功**，读回 sha256 `063f4130…` 与源一致。
- 重启后：**板子起不来**（呼吸灯灭）。TL101 侧抓到板子的大核控制台为
  `[35048.426152] systemd-shutdown[`，即**正常关机后再无输出**。
- 判定：**bl31 在 `arm_svc_arisc_wait_ready` 处卡住** —— 我们发的握手包没被接受。
  这是唯一一个此前无法离线验证的环节，现在有了答案：**不接受**。

## T21b · 找到根因并修正

回看厂商反汇编，启动反馈包的**头字不是 0**：

```
4000b1d6  sb a5(=2),   1(sp)   → pkt[1]    = 2
4000b1de  sh a5(=0x90),2(sp)   → pkt[2..3] = 0x0090（消息号）
4000b1f2  sb a5(=13),  4(sp)   → pkt[4]    = 13（字数）
40007248  lw a4,0(pkt) ; sw a4,124(a5)   → 头字 = 小端 [00 02 90 00] = 0x00900200
```

第一次发的是 `hdr = 0`。已改为 `0x00900200`（`e902-fw/src/msgbox.c` 的 `MBOX_FEEDBACK_HDR`），
重建后 `fw-scp-padded.bin` sha256 = `fb1d809d1fd6b5e9ee7553050f1d0569e45c3a4ceb4186026758c20875a38c26`。

## T21c · 现成镜像与改造镜像（可烧）

| 文件 | scp 槽 | sha256 |
|---|---|---|
| `output/images/.../...minimal_linux6.6.98.img` | 厂商 `07e6b976…` | `3b47847a78ecca9fd3f4d36edc967e601e3f0f2a7e0b5b73775a1e8014b4ec49` |
| `output/images/.../...minimal_linux6.6.98-e902fw.img` | 我们的 `fb1d809d…` | `82cf7fd9e46bc55ded3cf2dc495a6e82178e48a631cea3955fb1853f4e47aff9` |

两图**除 scp 那 105912 字节外完全相同**（前 `0x113BC00` 字节哈希一致 `9bcf95a9…`，已校验）。

## T21d · 确切构建命令与板名

```
BOARD=orangepizero3w   BRANCH=current   BUILD_OPT=image   BUILD_MINIMAL=yes
cd /home/helios/Desktop/orangepi-build && sudo ./build.sh userpatches/config-a733.conf
```

要让构建直接带我们的固件：`bash e902/install-scp-into-build.sh`（备份并替换
`u-boot/v2018.05-sun60iw2/scp.fex`）→ build。**在握手被证实接受前不要固化。**


---

# 补充记录 T21（2026-09-22 15:24–16:40）：hdr=0 失败事件定案 + 握手包 0x00900200 双轮独立复核 + 卡内重刷

## 1. 上一轮实弹事件（补录）

2026-09-22 13:55–14:04：以 12:42 构建的镜像（063f4130，startup feedback 头字 = 0）执行了槽位刷写并重启。
结果：bl31 未放行 —— 板子 ssh 无路由（ARP INCOMPLETE）、大核串口静默。SD 卡移入 TL101 读卡器（Realtek
0bda:0301，板上 /dev/sdb）。recover-sd.sh（14:04 建好）实为恢复预案，本轮确认卡内槽位当时仍为 063f4130
（即恢复脚本建好后未执行、会话中断）。本轮直接在读卡器侧恢复链路上继续推进。

## 2. 握手包逐指令独立复核（主线重做，不依赖上轮笔记）

反汇编 u-boot/v2018.05-sun60iw2/scp.fex（--adjust-vma=0x40004000）：

- builder（0x4000b1b0..0x4000b202）：memset(s1,0,52) → `sb 2,1(sp)` → `li a5,144; sh a5,2(sp)`
  → `li a5,13; sb a5,4(sp)` → `sw s1,28(sp)`（data_ptr 存 +28）→ 0x4000721e(a0=sp, a1=0x186a0)。
  即栈上描述符：b0=0（memset 零）、b1=2（flags）、b2..b3=0x0090（半字小端）、b4=13（count）、+28=数据指针。
- sender（0x4000721e..0x4000733c）：先查 `pkt[1]&2`；0x400071b2=wait_tx_empty(ch3)；
  `lw a4,0(s1)` → `sw a4,124(0x3004)`（**先发头字 = 0x00900200**）→ wait → `lbu a4,4(s1)`
  → `sw a4,124`（**发 count=13**）→ 循环 `lw a3,0(data_ptr+i*4)` → 发 data[i]。
- 结论：线上序 = [0x00900200][13][0×13]，通道 3，与 e902-fw/src/msgbox.c 的
  msgbox_send_packet(MBOX_FEEDBACK_HDR=0x00900200, payload[13]=0, 13) 完全一致。
  hdr=0 的失败反例与「bl31 校验包头」解释自洽。

## 3. 三处手册抽查（本轮、TL101 本机 pdftotext 重跑）

- 表 12-2：`48 / 0x00C0 / CPUX_MSGBOX_W_R / CPUX MSGBOX WRITE IRQ FOR CPUS-RISCV`（/tmp/a733.txt 33897–33903）✓
- 5.2.3：`Note: It is the running PC address when E902 reset is released.`（行 18556）✓
- 4.2.5.24：`RISCV_CFG_RST`(13619) / `RISCV_GATING`(13630) 位定义 ✓

## 4. 产物与介质状态（本轮终态）

- 固件：banner 字符串更正为 hdr=0x00900200 后重编 → fw-scp.bin 6216B sha256 bc3848f73ccd…；
  padded 105912B sha256 8aaf9486151a…（= fw-out/scp-ours-padded-105912.bin，SHA256SUMS 已更新）。
- verify_scpfw.sh / verify_failsafe.sh：全项通过（无重定位、厂商序言一致、握手最前、尾补零）。
- SD 卡（TL101 读卡器 /dev/sdb，指纹 eGON.BT0 + sunxi-package 已验）：
  槽位写入 + 读回 = 8aaf9486…；24MiB 头哈希 a84c5c63…（厂商态为 114cd1f3…，恢复后应回到该值）。
- 板子当前关机、卡在读卡器：下一步 = 插卡上电 → bash e902/tests/post-poweron.sh。

## 5. 置信度

- 握手包格式：High（两轮独立反汇编 + hdr=0 反例实测）。
- bl31 是否接受 0x00900200：Pending（只能上电证实；失败后果 = T21 同款 no-boot，恢复链已演练）。



---

# 补充记录 T22（2026-09-22 17:30 前后）：启动失败真因定案 —— boot0 对启动包整包 add_sum 校验失败（此前"bl31 拒绝握手"为误诊）

## 1. 决定性证据（CH340 大核串口，2026-09-22 17:0x 实抓）

```
[4657]error:bad checksum.
[4661]error:bad magic.
[4663]Loading boot-pkg fail(error=4)
[4666]Failed preparing the second boot
```
板子卡死在 **boot0 加载启动包阶段**，u-boot/bl31 从未运行 —— 与 mailbox 握手无关。
E902 串口同窗 0 字节（小核未被放行）与此吻合。

## 2. 校验机制（源码级，u-boot v2018.05-sun60iw2）

- 头结构 `sbrom_toc1_head_info_t`（include/private_toc.h）：name[16]="sunxi-package"、
  magic@+0x10 = TOC_MAIN_INFO_MAGIC **0x89119800**、**add_sum@+0x14**、items_nr@+0x20、valid_len@+0x24。
  （此前把 0x89119800 当"待解释字段"是错的，它是 magic；真正要修的只有 add_sum。）
- 算法 `sunxi_generate_checksum`（board/sunxi/board_common.c:846）：
  `sum = Σ(LE u32 words over buf[0..valid_len)) - src_sum + STAMP_VALUE`，STAMP_VALUE=0x5F0A6C39（tools/mksunxiboot.c:19）。
  boot0 校验条件：`generate_checksum(pkg, valid_len, stored) == stored`。
- 打包器同源：`sprite/sprite_download.c:177`（下载固件时重算 add_sum）。

## 3. 数学闭合（厂商包实测）

W(厂商包, 0x154000B LE word sum) = f33152d9；
f33152d9 − a91ddf89 + 5f0a6c39 = **a91ddf89** = 卡内存储值 ✓。
推论：add_sum 字段在求和区间内 ⇒ 固定 W0（字段置 0 的和）后，
**正确 add_sum = W0 + STAMP = boot0_verify(当前状态) 的输出值**（唯一解）。

## 4. 修复工具与自测

`e902/tests/fix-bootpkg-sum.py`（--write/--vendor）：
- 厂商备份自检 → PASS（还原 a91ddf89）✓
- 模拟件（厂商头+我们的槽位内容）→ FAIL → --write → add_sum=**83f86015** → PASS ✓
- 槽位内容不受影响（8aaf9486… 保持）✓
真实卡修复 = 在读卡器上对 /dev/sdX 执行 `--write`（预计写入 0x83f86015，4 字节 @0x1004014）。

## 5. 对旧结论的更正

- T21 中"hdr=0 被 bl31 拒绝"不成立：当时板子同样死在 boot0 校验（hdr 值从未被硬件检验过）。
  0x00900200 仍是最优假设（与厂商逐字节一致），其硬件验证状态 = Pending。
- msgbox.c 内"bl31 rejected it"注释已更正。

## 6. 置信度

- add_sum 算法与修复值：**High**（源码 + 厂商包数值闭合 + 修包自测三重）。
- 修复后能否过 boot0 并进入 bl31：Pending（等插卡上电）。


---

# 补充记录 T23（2026-09-22 17:45–18:00）：v2 上板实证 + 新双根因（握手语义/看门狗）+ v3

## 1. E902 首次真实运行（里程碑）

add_sum 修复后 boot0 放行，BL31 载入并放复位，**我们的固件在 E902 上真实运行**，
板载 FT232H 控制台抓到（verify-logs/e902-console-first-success-20260922.log）：

```
=== A733 E902 firmware v2 ===
PL_CFG0 = 0xffff3322 (PL2/PL3 mux ok)   APBS1 = 24000000 Hz
RISCV_BGR = 0x00010003   RST_START = 0x40004000
startup feedback (ch3 hdr=0x00900200 13w) : FAILED
spi_init(500kHz) = ok (VER=0x00010003)
msgbox irq 48, timer irq 21, uart irq 29, spi irq 38.
sent HELLO to ARM
```

## 2. 新根因 ①：握手发送周期性 FAILED

- 我们 v2 的 wait_tx_empty 等"**FIFO 全空**"（MSG_STATUS & 0xF == 0）；
- 厂商 0x400071b2 等"**不满**"（MSG_STATUS != 8，手册 6.1 MSG_NUM bits[3:0]，深度 8）。
- 复位循环中上一周期的 HELLO/反馈残留使 FIFO 非空 → 我们的等待必然超时 → FAILED。
  （首个干净周期是否发送成功无法回看，但复位循环已把它掩盖。）

## 3. 新根因 ②：~12s 复位 = R_WDT 无人安抚（实锤）

- 大核串口：`Starting kernel ...` 后 ~0.6s 复位，内核零输出，12~13s 周期循环；
- 厂商 scp.fex 启动路径（0x40005bd4..0x40005c42，反汇编）对 **R-CCU 0x07010244**
  （写键 0xA700<<16，清 bit1）和 **R_WDT 0x07090000**（写键 0x16AA<<16；0x15C=0x16AA；
  0x160 清 bit1；0x00C 清 bit0；0x000 低16保留|键，再清 bit2/bit4）执行"去复位使能"；
- 我们不做 → 看门狗到点咬 → 复位循环。0x07050000 实为 CPUIDLE 块（手册 ch.2），此前的
  "0x07050010/14=WDT"猜测作废；0x016AA 键与手册 8.3（WDT 写键 0x16AA、CTRL 键 0xA57）吻合。

## 4. v3 变更（fw-out/scp-ours-padded-105912.bin = afcc86f4…）

1. msgbox_wait_tx_empty 改厂商语义（!=8 即可发）；
2. 新增 wdt.c：wdt_neutralize() 逐条复刻厂商序列（R-CCU 0x0244 键 0xA700 清 bit1；
   R_WDT 0x07090000 六步；另对文档 WDT0_CPUX/WDT_CPUS 的 MODE 写键禁用做保险）；
3. main() 顺序：hb → msgbox_init → **feedback** → **wdt_neutralize** → pinmux/uart/…（失败安全不变）；
4. 横幅升 v3。
结构校验 verify_scpfw/verify_failsafe 全过；无重定位；厂商序言一致。

---

# 补充记录 T24（2026-09-22 18:30–19:00）：bl31 wait_ready 判据逆向完成 + v3.1

## 1. bl31 (monitor.fex, aarch64) 关键函数

- 0x19f0 = bl31 的 ch3 发送器：flags.bit1 未置 → 返回 -22；等 0x0709406C != 8（不满）→
  发 [hdr][count][data]，与厂商 SCP 发送器完全镜像。
- 0x1b18 = bl31 的 ch3 接收器：0x0300406C == 0 → 返回 -22；否则收 hdr(0x0300407C)、
  count、data → 写 0x03004024 = 64（清 ch3 RX pending）→ 返回 0。
- 0xae88 = **wait_ready 本体**：0xaf54 写 RISCV_BGR=0x00010003 → 0xaf68 写
  RST_START=0x40004000 → 0xaf78 置 CPUIDLE 0x07050008 bit0 → 0xaf8c 开
  RISCV_24M gate(bit31) → 0xafa0 清 0x05005120 bit0 → 循环 { bl 0x1b18;
  **ldrb w0,[buf+0x20]; cmp w0,#0x90; b.ne 重收** } → 匹配后清全局、置 buf[0x28]
  字节=1、bl 0x19f0 回发（回发受 buf[0x1F]&3 门控，冷栈为 0 → 实际不回发）→ 返回。

## 2. 判据

**bl31 等的是"数据区第一个字节 == 0x90"的包**。厂商 feedback 的数据区是
strncpy(空字符串) 的 13 个全零字 —— **feedback 本身不满足 wait_ready**！
满足它的是厂商主循环对 bl31 查询包的**回显**（dispatcher 0x4000acd8：handler
返回值写入 hdr byte3(result)，flags&3 != 0 时经 0x4000733e 原路回发；
bl31 的查询 data[0].byte0=0x90 → 回显保留 → 匹配 → 就绪）。

## 3. 命令名（字符串证据）

0x19=fake poweroff req, 0x22=cpu op req, 0x24=sys op req, 0x26=set wakeup src req,
0x60=set debug level request, 0x61=set uart baudrate request, 0x62=set dram crc
paras request, 0x96/0x97=...；收到未知命令 → result=0xFF 且照常回显。

## 4. v3.1 变更（fw-out = 40b005f0… / 08e67528…）

1. wdt.c 删除"保险"步骤(3)（写 0x02050014/0x07021000 —— 厂商从不访问，v3 实测
   跨域写挂死小核总线，横幅都无法打印）；
2. msgbox ISR 改为**包重组**（hdr→count→data 状态机，长度上限 16）；
3. main 收到完整包后按厂商规则回显（result=0，flags&3 门控），控制台同步打印；
4. post-poweron.sh 演示改包协议（PKT-PING/PKT-ECHO）。

## 5. 置信度

- wait_ready 判据（data[0].byte0==0x90 + 回显机制）：High（bl31 反汇编 + 厂商
  dispatcher 反汇编 + v2/v3 现象三方一致）。
- v3.1 能否让 bl31 放行：Pending（刷卡上电验证）。

---

# 补充记录 T25（2026-09-22 21:20 前后）：T23 的"WDT 序列"实为挂起路径；真正的启动路径喂狗 = 0x40007c18（WDT_CPUS）

## 1. v3.1 实测（21:1x）

- boot0 过 → bl31 → "[SCP] :wait arisc ready...." 卡死（分钟级无复位）；
- E902 串口 0 字节 —— 小核在横幅前冻结；
- 结论：v3.1 的 wdt_neutralize 步骤(3)删除后仍冻结 → 冻结点在步骤(1)(2)内部
  （0x07010244 键 0xA700 / 0x07090000 键 0x16AA 序列 —— 该序列属于
  "broadcast 24mhosc will power-off" 的挂起准备路径，放到启动路径会关掉
  小核自己的时钟/触发跨域冻结）；但看门狗确实被解除（分钟级无 12s 复位）。

## 2. 真正的启动路径喂狗（monitor.fex + scp.fex 双向印证）

- 厂商 feedback 构造前（0x4000b198）调用 0x40007c18：
  对 **WDT_CPUS 0x07021000** 执行——
  MODE(0x14)：清 bit8（时钟源 RTC_32K）、清 bit[1:0] 后置 01（双写）；
  CTRL(0x18)：清 bits[7:4] 后置 bit6（interval=4）。
- 即：boot0/bl31 以 ~10s 武装 WDT_CPUS，SCP 启动路径第一步就是按上述
  参数收敛它；v2 没做 → 12.1s 咬人；v3/v3.1 的错误复刻 → 自己冻死
  （顺带把看门狗解除了，所以表现为"卡住而非循环"）。

## 3. v4 变更（fw-out = 245d9a7c… / 762c21a9…）

- wdt.c 重写：只保留 0x40007c18 的 WDT_CPUS 序列（逐指令转写）；
- main 顺序改为 hb → wdt_cpus_service → msgbox_init → feedback（与厂商一致）；
- 横幅升 v4；其余（包重组/应答器/失败安全）不变。

## 4. 置信度

- WDT_CPUS 服务序列：High（厂商反汇编逐指令 + 调用位置在 feedback 前 + v2 12s
  咬人时间线吻合）。
- v4 能否过 wait_ready：Pending（待刷卡上电）。

---

# 补充记录 T26（2026-09-22 22:30 前后）：wait_ready 卡死主因 = 单字 HELLO 毒化 bl31 接收器 + v5

## 1. v4 两轮实测复盘

- 第 1 轮（21:27）：boot0 DRAM 训练阶段偶发冻结（先于固件运行，与 v4 无关，疑似上电/插卡毛刺）；
- 第 2 轮（22:22）：串口采集晚挂错过横幅（ races），大核仍卡 "wait arisc ready"，
  bl31 接收器 0x1b38 的 0x1b38 自旋环无超时。

## 2. 主因（反汇编实锤）

bl31 接收函数 0x1b18：弹出 hdr 后在 0x1b38 处 `ldr w1,[x0]; cbz w1, 0x1b38`
**无超时自旋等待 count 字**。v2 固件在 feedback 之后发送的"单字 HELLO"
（0x10000000，非包格式）被 bl31 当作包头弹出 → 永等 count → wait_ready
永不完结。v2 的 12s 复位 = WDT 咬；v3.x 因看门狗被解除 → 表现为永远挂起。
所有历史现象（v2 循环复位 / v3.x 永挂 / 横幅出现与否）全部闭合。

## 3. v5 变更（fw-out = 623f4db0… / 046a2ac0…）

1. **删除单字 HELLO**（毒源）；
2. feedback 之后发送 **ready 通知包**：hdr=0x00900200、count=1、data[0]=0x00000090
   （满足 bl31 的 data[0].byte0==0x90 判据，T24）；
3. 应答器增加 ACK 抑制：hdr 低 24 位 == 0x00900200 的来包（bl31 对我们
   feedback/ready 的 ACK）不回显，避免应答风暴；
4. 横幅升 v5；包重组/回显/WDT_CPUS 服务（T25）不变。

## 4. 置信度

- HELLO 毒化机理：High（bl31 接收器反汇编 + 各版本现象闭合）。
- v5 能否过 wait_ready：Pending（刷卡上电）。

---

# 补充记录 T27（2026-09-23）：reboot/poweroff mailbox 协议逆向完成并在固件中实现（v60）

## 1. ARM 侧发送方（monitor.fex/bl31，vaddr=文件偏移）

- PSCI SYSTEM_OFF  -> plat_aw_system_off (0x80bc)，SYSTEM_RESET -> plat_aw_system_reset (0x8110)；
  两者都调公共发送器 **0x2158(arg)**，arg: 0=off, 1=reset。
- 0x2158 构造包：hdr 字节 {b0=0x02(attr), b1=0x00(flags, **不要求回显**), b2=0x24(type=sys-op),
  b3=无关} + count=1 + payload[0]=op（u32: **0=poweroff, 1=reset**），经 ch3 推出后 **直接 wfi**，
  不等待、不轮询任何回包 → 整个电源动作完全由 E902 完成。
- 失败模式：SCP 不动作 = 板子永远不断电（即 v59 及之前的现状：v59 盲回显无人收）。

## 2. SCP 侧接收方（scp.fex，vaddr=文件偏移+0x40004000）

- 分发器 0x4000abe0 按 hdr b2 分派；type 0x24 -> 0x4000aa54，payload[0]=op：
  - **op0 (off)**: GP3=0xA101 -> twi_init+bus-idle(0x40007a0e) -> GP3=0xA203 -> cb 0x4000766a:
    pmu_inited 门(0x4001dddc) -> vbus 检查(0x40007b82->0x4000771a: GP3=0xA501, 读 AXP515(0x34)
    reg0 bit1) -> 打印 "axp8191 exist" -> **AXP8191(0x36) reg 0x04 = 0x08** -> axp515 复位准备
    (0x40007b62->0x400078f8->0x4000775a) -> exit(1) (0x400075ee)。
  - **op1/2 (reset)**: GP3=0xA102 -> twi init+idle(0x400079c8) -> GP3=0xA202 -> cb 0x400076ce:
    axp515 复位准备 -> **AXP8191 reg 0x04 = 0x01** -> exit(1)。
  - **op3**: GP3=0xA103 -> twi gate(0x4001ddd8)+idle(0x40007974) -> GP3=0xA201 -> cb 0x40007706:
    PMU 框架钩子 0x40007828 -> exit(0)。
- **exit(arg) 0x400075ee**（不归点）: GP3 = 0xA300|arg；AXP8191 reg 0x55 读改写
  |= 1<<(7-arg)（op0/1/2=bit6, op3=bit7）；打印 "reset system"/"poweroff system"；死循环。
- **axp515 复位准备**（0x400078f8+0x4000775a）: GP3=0xA401, GP3=0xA400；AXP515 写 0xFF 到
  reg 0x40..0x45、0x48..0x4D（关全部 IRQ 使能）；0xC0->0x42；0xF0->0x43；打印 "reset axp515"。
- **GP3** = RTC 0x0709010C，0xA1xx/0xA2xx/0xA3xx/0xA4xx/0xA5xx 是 always-on 电源 FSM 协议码；
  FSM 依据序列历史决定断电后是否复电（off vs reset）。
- 回显规则（0x4000acd8 尾部）：result 字节 = handler 返回值，仅当 flags&3 != 0 才回显；
  op0..3 handler 不返回 → 不回显（bl31 已 wfi）。

## 3. TWI（S_TWI0 0x07083000, PL0/PL1 func2, 门控 0x0701019C bit0/16）

- 初始化 0x400070d4：BGR 门+复位解除 → 引脚 func2/上拉 → 软复位(SRST bit0 自清) →
  CNTR=0x44(BUS_EN|AACK) → bus_clear(0x4000702c: LCR|=4 轮询 LCR bit4=SDA 电平, 9 次) →
  校验 LCR==0x3A（失败仅打印 "ERR:SDA is still low level!" 不中止）。
- 字节机 0x40006c44：CNTR bit3=INT 标志（**写 1 清**，硬件完成字节后置位）；bit5=START,
  bit4=STOP, bit2=AACK（最后读字节 NACK 用 CNTR&=~0x0C）；STAT 期望序列
  0xF8(idle)->0x08(START)->0x18(ADDR_W ACK)->0x28(DATA ACK)->0x10(RESTART)->0x40(ADDR_R)->0x58(DATA NACK)；
  超时 2047 自旋（v60 放宽到 50000，时钟不同）。
- v60 惰性初始化：仅在 poweroff/reset 时初始化 TWI（运行期总线归 Linux r_i2c）。

## 4. v60 实现（e902-fw/src/power.c + main.c 0x24 分派；fw-out=170bc50f…）

- power_sys_op(op) 逐条复刻上述序列；main.c handle_arm_packet 拦截 type 0x24（b2），
  hb_stage=0x50 进入；未知 op 回 result=0xEA（仅当 flags&3）。
- **有意偏差**（power.c 头注释有完整说明）：
  1. 忽略 pmu_inited 门（无厂商 PMU 框架，遵循它会令 poweroff 永远无效）；
  2. op0 vbus 门只记录不中止（厂商失败即静默放弃 = poweroff 坏）；
  3. bus-idle 重试有界（10 次×2s + bus_clear）；op3 跳过 0x40007828 钩子。
- 可观测性：进入 0x24 处理时 hb_stage=0x50；exit 前 0x60(op3)/0x61(op0/1/2)；
  S_UART0 全程打印 [pwr] 行。

## 5. 状态

- 反汇编三方一致（bl31 发送器 / scp.fex 分发器+PMU 序列 / 手册 S_TWI0+RTC 寄存器布局）：High。
- v60 已写入读卡器中的启动卡（0x113BC00 槽位，add_sum=88dae8ea，boot0 校验 PASS）。
- **Pending：给板上电验证 `reboot` / `poweroff`。**

---

# 补充记录 T28（2026-09-23 17:30 前后，补记）：v60 的 reboot 卡死 = 中断从未投递 + v62 改轮询

- 17:08 启动跑的是 v60（横幅仍写 v5.9）：全程 **零 `[tick]`、零 `[pkt]`**；17:27 `reboot` 后大核停在
  `npu unregister cooling` 之后，E902 串口 0 字节（verify-logs/arm-reboot-test.log / e902-reboot-test.log）。
- v60 的收包只走 mailbox RX 中断（IRQ 48）→ 中断不投递 = bl31 的 sys-op(0x24) 包永远没人读 → 不断电、不复位。
- v62：RX 改为主循环轮询（与厂商主循环 0x4000abe0 一致），IRQ 48 关闭；启动时排空残留 RX。
- 中断为什么不投递：**仍未定位**（CLIC 配置与手册一致：mtvec.mode 硬连 3、shv=1、INTCTL=0x80>阈值 0、MIE=1）。
  v63 加了投递探针（见 T29 §4），下一次上电即可判定。

---

# 补充记录 T29（2026-09-23 19:00–20:00）：卡死与资源冲突的系统性排查 + v63

## 1. bl31 与 E902 之间的全部 RPC（monitor.fex 反汇编，vaddr = 文件偏移）

通用发送器 **0x1bf4**：`hdr.b1(flags) bit1 = 1` → **同步**：逐字发 [hdr][count][data]，然后在
0x1cc4 / 0x1d34 `cbz` **无超时自旋**等 CPUX FIFO（0x0300406C）出现应答，读 hdr、count，再把 **count 个字
拷进调用者栈上的缓冲区**。不检查应答是否对应本请求。发送侧每字前等"RX FIFO 不满"，同样无超时。

| 调用点 | type | flags | 性质 | 触发路径 |
|---|---|---|---|---|
| 0x1904 | 0x25 | 2 | **同步** | SIP SMC 分发 0x2504 |
| 0x1948 | 0x22 cpu op | 0 | 异步，5 字 | system suspend（0x8780，保存 CCU 后） |
| 0x1994 | 0x11 | 2 | **同步**，count 0，应答 data[0] 存全局 0x20798 | suspend finish（0x87fc） |
| 0x1f34 | 0x61 uart baud | 0 | 异步，11 字 | SIP 0x2530 |
| 0x1fb4 | 0x96 | 0 | 异步 | SIP 0x24f8 |
| 0x1ff0 | 0x60 debug level | 2 | **同步** | SIP 0x251c |
| 0x203c | 0x64 | 0 | 异步，3 字 | SIP 0x2540 |
| 0x20c0 | 0x62 dram crc | 2 | **同步** | SIP 0x2528 |
| 0x210c | 0x26 wakeup src | 2 | **同步** | SIP 0x2510 |
| 0x2158 / 0x2194 | 0x24 sys op | 0 | 异步，发完即 wfi | PSCI SYSTEM_OFF/RESET（0x80d0/0x8124） |

**推论（所有"卡死"的共同机理）**：E902 只要 ① 不排空 RX、② 不应答同步请求、③ 往 CPUX FIFO 塞了一个
非请求的字，bl31 就在 EL3、关中断状态下永久自旋 → 该 ARM 核死锁 → 整机表现为卡死。
v60 同时满足 ①②；v62 的 `s` 键仍满足 ③（单字 `MSG(CMD_UART_KEY)`，与 T26 的 HELLO 同一毒性）。

## 2. 固件缺陷清单（v62 → v63 已修）

| # | 缺陷 | 后果 | v63 修复 |
|---|---|---|---|
| 1 | `trap_entry` 记录后 `wfi` 永久停车 | 任一次总线错误/非法指令 → SCP 死 → 下一个 bl31 RPC/reboot 卡死 | 保存现场、记入心跳、**跳过故障指令**后 `mret`；取指故障或同一 PC 连续 16 次 → 重新进入主循环 |
| 2 | `uart_isr`、`spi_isr` 缺 `interrupt("machine")` | CLIC 硬件向量直接跳入，`ret` 返回到垃圾 ra → 中断一旦能投递，首次按键/SPI 中断即崩溃 | 加属性（目标文件中确认以 `mret` 结尾） |
| 3 | `default_isr` 仅 `mret` | 无处理函数的电平中断无限重入，主循环饿死 | 屏蔽该源（INTIE=0 + wakeup mask）并计数 |
| 4 | `spi_isr` 与轮询 `spi_transfer` 争抢 RX FIFO / TC 标志 | 中断一旦投递，所有 SPI 传输超时或读到 0 | SPI 中断不再使能（驱动是轮询的） |
| 5 | main 里第二次 `msgbox_init` 后又挂 `msgbox_isr` 并使能 CLIC 48 | 两个消费者竞争同一个包重组状态机 | 删除 `msgbox_isr`；RX 只有主循环一个消费者 |
| 6 | 回包前先打印 `[pkt] …`（阻塞串口，约 50 字 ≈ 4 ms） | 每个同步 RPC 让 ARM 核在 EL3 关中断多等 ~4 ms（RT 内核不可接受） | **先回包后记录**；串口 TX 改 2 KB 环形缓冲、主循环 `uart_tx_pump()` 非阻塞排空，满则丢弃计数 |
| 7 | `s` 键向 ARM 推送单字 | 毁掉 bl31 下一次 RPC 的帧 | 改为只在本地打印状态 |
| 8 | `msgbox_init` 在 `uart_init` 之前调用 `uart_puts` | 握手之前可能卡在未初始化的 UART 上 → 板子不启动 | `msgbox_init` 不再打印，由 main 在 UART 就绪后汇报 |
| 9 | `ts_test_run()`（1 s 忙等）放在 ready 包之后、主循环之前 | bl31 放行后的第一批同步 RPC 至少被拖 1 s | 移到 `T` 键 |
| 10 | `delay_us` 等 EN 清零无上限 | S_TIMER 无时钟时永久挂死 | 有上限（us×64+1024 次轮询） |
| 11 | `uart_putc` / `uart_flush` 等 THRE/TEMT 无上限 | UART 无时钟时永久挂死 | 有上限 |
| 12 | 未设置 E902_WAKEUP_MASK0/1（0x07032064/68） | 使能的中断不能把核从 wfi 唤醒（厂商 0x40005360 每次 irq_enable 都置位） | `clic_enable/disable` 同步置/清 |

## 3. 资源归属核对（Linux DTS/驱动 ↔ E902 固件）

| 资源 | Linux 侧 | E902 侧 | 结论 |
|---|---|---|---|
| MSGBOX ch3（0x03004000 / 0x07094000） | `msgbox@3004000` okay，reg 同时覆盖两块；probe 只解复位、开时钟、关全部中断；DT 无任何客户端 | 唯一的 CPUS 侧读者 | ARM 侧 ch3 **只属于 bl31**。任何 Linux mbox 客户端若 claim ch3 会偷走 bl31 的应答 → bl31 永久自旋。节点必须保持 okay（它持有 CLK_MSGBOX0，E902→ARM 方向靠它）。post-poweron.sh 的 /dev/mem 演示同样会抢 ch3，已加警告 |
| R-CCU 门控（r-mbox/r-uart0/r-timer/r-spi/r-twi0…） | 全部 `CLK_IGNORE_UNUSED` | 使用 | Linux 的 clk_disable_unused 不会关掉它们 ✓ |
| RTC 块 0x07090000（LOSC_CTRL/0x04/0x15C/0x160） | `rtc_ccu@7090000` okay，probe 写同样的位（LOSC 外部 32k、auto-switch、XO_CTRL DCXO_EN） | clocks.c（旧注释误称 R_WDT） | 两方写入值相同且 E902 在内核入口之前完成 → 无冲突；clocks.c 注释已更正 |
| S_TWI0 0x07083000 + PL0/PL1（PMIC） | 板级 DTS okay，运行期独占 | 仅在 sys-op（Linux 已停）时惰性初始化 | 时序上互斥 ✓ |
| S_UART0（uart7）、S_SPI（r_spi） | disabled | 独占 | ✓ |
| SRAM A2 0x4001E000（ARM 0x5E000）心跳块 | 无节点占用（`dump_reg@44000` 只占 4 字节） | 独占 | ✓ |
| TIMESTAMP 0x08010000/0x08020000 | `amp_timestamp` 驱动置 CTRL.enable | ts_test 只读 | ✓ |

## 4. v63 的可观测性（下一次上电直接判定中断问题）

`awdevmem.py hb` 解码心跳块全部 24 字（旧 post-poweron.sh 的 `dump --count 17` 实际只读了 17 **字节**）：
- `tick_src` = [IRQ 计数 << 16 | 轮询兜底计数]：IRQ>0 → 中断已投递；只有轮询在涨 → 未投递；
- `clic_diag` = timer1 的 CLIC INTIP / INTIE + S_TIMER IRQ_STA：INTIP=1 且不进 ISR → 核侧问题；
  IRQ_STA 置位但 INTIP=0 → 片上路由问题；
- `traps / last_mcause / last_mepc`：被"跳过"的异常；`spurious`：被屏蔽的无主中断；
- `rpc` = [收包数 << 16 | 回包数]。
控制台 `s` 键打印同样的状态。

## 5. 验证

- 交叉编译零警告；verify_scpfw.sh / verify_failsafe.sh 全过（厂商序言一致、无重定位、ch3 握手在、尾部全零）；
- 新增 `make hosttest`（e902-fw/tests/host/mbox_test.c）：真实 msgbox.c + main.c 链接到模拟 FIFO，
  覆盖全部 5 种同步请求（逐字核对应答 hdr/count/data）、异步与 ACK 不回包、sys-op 分派与未知 op 回 0xEA、
  逐字分批到达、20 个背靠背请求无溢出 —— **全过**；把"回包"改坏的变体 19 项失败（测试有效）。
- fw-out/v63-padded-105912.bin = 188f4d81…（scp-ours-padded-105912.bin 同步）。
- **Pending（需要人手给板子断电重上电）**：板子自 17:28 起卡在 reboot，网络不通。

---

# 补充记录 T30（2026-09-23 19:30–20:10）：上板验证 v63/v64 —— 软件重启打通、中断其实可投递、三个新根因

## 0. 上电进 FEL 的原因

19:29 上电直接进 FEL（USB 1f3a:efe8，sunxi-fel 认出 soc=0x1903）。卡插读卡器只读比对：boot0 完好，
启动包 add_sum 仍是 v60 的 88dae8ea，而槽位里是 v61 —— 17:26 板上刷 v61 时没重算 add_sum。
读卡器写入 v63 + 修 add_sum 后正常启动。**flash-scp.sh / go-flash.sh / restore-scp.sh / go-revert.sh
原先都不修 add_sum**，已全部补上（写完即修、校验不 PASS 则拒绝重启），go-flash 另加槽位读回比对。

## 1. 中断"从未投递"的真相：S_TIMER 驱动两处错误（CLIC 一直是好的）

- CPUS 域 S_TIMER（0x07091000）实测布局：**每个定时器 0x20 字节，从 0x20 起**；0x10..0x1C 读零写忽略。
  0x20 那组置 IRQ_STA bit0，0x40 那组置 bit1 并触发 CLIC 21。手册 8.2.6 的表是 CPUX 域定时器（0x10 起、步长 0x10）。
  厂商 scp.fex 印证：timer0 在 0x07091020（0x4000645c），间隔寄存器 +0x24/+0x44。
- 每个定时器的计数时钟在 R-CCU 0x07010100+4n（bit0 使能，复位值 0 = 关）；固件只开了总线门控 0x011C。
- 修正后：心跳 tick_src = IRQ 计数为主（例 irq=276 / polled=4）—— **CLIC 硬件向量中断工作正常**。
- 副作用链：`delay_us` 用的 0x10 组不存在 → 以往所有 delay 都是空转；T5.x 的"时间戳速率异常
  delta_1s=185125"源于此。现在 delta_1s=22154452（E902 的 1 s ≈ 0.92 s，按 TS=24 MHz），**遗留待查**。

## 2. 启动残包：多余的 ready 包使每个同步 RPC 错位一包

- 实测每次启动后 CPUX ch3 FIFO 残留 3 字；排空后看到 ready 包与 0x26 回包成对堆积（跨多次热启动，FIFO 不会被复位清空）。
- bl31 wait_ready（0xb054..0xb068）把包收进 sp+0x38 的结构，比较 `ldrb [sp,#58]` = **包头 byte2（type）== 0x90**，
  不是 data[0]（T24/T26 读错偏移）。startup feedback（hdr 0x00900200）本身就满足 → T26 加的 ready 包是多余的，
  留在 FIFO 里被下一次同步 RPC（0x26）当作回包读走，真回包滞留 —— 每次同步 RPC 都拿到上一个包。
- 修复：删除 ready 包；msgbox_init 按厂商（scp.fex 0x40005e56/0x40005e80）对 CCU 0x02002744 与 R-CCU 0x0701017C
  的 bit16 做复位脉冲，清掉历史残包。验证：启动后 0x0300406C = 0x0709406C = 0；rpc = 2 包收 / 1 回（0x26）。

## 3. 关机/复位路径

- 软件 `reboot` 共验证 5 次（v63 处理 1 次、v64 各版本 4 次），全部成功：`[pkt] … sys-op` → `[pwr] axp8191 55: 0x3b -> 0x7b`
  → 复位 → 重新启动。
- 本板 0x34（AXP515）地址无应答：reset prep 改为先探测一次，缺失则跳过（原来 14 次写超时）。
- `twi_idle()` 的 LCR 判据写反（厂商 0x400070ac：STAT==0xF8 且 (LCR&0x30)==0x30 才空闲）；delay 修好后它会让
  每次复位白等 10×2 s。已改正，reboot 全程 39 s（含内核关机与启动）。
- poweroff（op0）未测：会让板子断电，需要人手再开机。

## 4. 运行期观测

- 运行期 bl31 的同步请求：type 0x26（set wakeup src，d0=0x400024c6），启动约 1 s 时到达，E902 正确应答并被 bl31 读走。
- 1 分多钟浸泡：traps=0、spurious=0，无 RCU stall / soft lockup。
- 固件：fw-out/v64-padded-105912.bin = 5fa1c4b2…（已刷入并运行）。

---

# 补充记录 T31（2026-09-23 20:10–20:35）：中断 / 时间戳 / 串口 / mailbox 全面测试 + v65–v67

## 1. 时钟：这块板的 DCXO 是 26 MHz

Linux `clk_summary`：`dcxo 26000000`，`pll-ref 24000000`（APBS1/UART 走 pll-ref，所以串口一直正常）。
v64 给 S_TIMER 选的是 dcxo → tick 10.857/s、E902 的"1 s" = 0.92 s；S_SPI 也在 dcxo 上（"500 kHz"实为 541 kHz）。
v65：R_TIMERn_CLK 与 R_SPI_CLK 的 mux 都改为 pll-ref（index 4）。

## 2. 实测结果（v66/v67）

| 项 | 结果 |
|---|---|
| S_TIMER1 计数速率 | 23.993 MHz（Linux 侧 20 ms 窗口采样中位数） |
| 共享时间戳 0x08010000 | 23.9999 MHz（对 CLOCK_MONOTONIC） |
| E902 tick | 10.000 /s（21 s 边沿法） |
| 中断投递 | 100 % 由 ISR 计数（polled 兜底改为"挂起超过 1 ms 才接手"后 = 0） |
| 定时器中断延迟（到期 → ISR 函数体） | min 1 / max 9 个 24 MHz 计数 = 0.04 / 0.38 µs；Linux 8 核 CPU+内存满载下不变 |
| E902 测时间戳（T 键） | 1 s 延时内 24,000,606 计数 → 两核同一 24 MHz 计数器、25 ppm 内 |
| E902 串口 | 1000 字节连发回显逐字节一致；RX 计数与发送总数一致；uart-drop = 0 |
| mailbox 往返（Linux→E902→Linux，200 个同步 ping） | 0 错；RTT min 8 / 中位 16 / p99 21–22 / max 31–72 µs（含 Python 开销）；两方向零残留 |
| 软件 reboot | 累计 9 次全部成功（v67 自身处理：38 s 回到系统） |

## 3. 本轮修掉的问题

- **Makefile 无头文件依赖**：改 fw.h 后 heartbeat.o 未重编（HB_N 仍为 24，新心跳字被丢弃）。
  已加 `$(OBJS): src/a733.h src/fw.h src/spi.h`；clean 重建后镜像哈希改变，证实之前混有旧目标文件。
- **组包状态机不能自愈**：一个错位的字会让之后所有包被错拆（由按字节访问 FIFO 的测试脚本触发，1000+ 个错字）。
  v67：包未收完且 ≥2 tick（100–200 ms）无新字 → 丢弃重来，计数 `resyncs`。真机验证：塞入 0xDEADBEEF 后
  resyncs=1，随后 200 个 ping 全对。主机测试加 test_resync（去掉修复则 2 项失败）。
- 轮询兜底过早接手 tick（约 20 %），掩盖 ISR；改为挂起 >1 ms 才接手。
- 新增心跳字 24/25：定时器中断延迟 max/last、min（`awdevmem.py hb` 解码）。

## 4. 遗留

- 当前 RT 内核（~/rt-kernel-test 树）没有 `amp_timestamp` 驱动，Linux 侧只能经 /dev/mem 读共享时间戳；
  驱动在厂商树 bsp/drivers/misc/amp_timestamp.c，需要移植。
- E902 读 TIMESTAMP FREQID（0x08020020）得 0，计数本身正确。
- poweroff、休眠/唤醒未测。
- 固件：fw-out/v67-padded-105912.bin = 2e314c22…（运行中）。烧录流程见 e902/FLASHING.md。

---

# 补充记录 T32（2026-09-23 20:40–21:00）：amp_timestamp 驱动移植到运行中的 RT 内核

- 运行内核 6.6.98-rt58-sun60iw2 由另一份配置编出：RT 树 `.config` 是调试版（PROVE_LOCKING 等），且缺少
  SENSOR_AR0234 等符号 → 在 RT 树里编的模块 `struct module` 大小不符（`Exec format error`）。
  改为在板上用已安装的 linux-headers 包原生编译，vermagic 与运行内核一致，加载正常。
- DT 节点用用户 overlay 提供（`/boot/overlay-user/amp-timestamp.dtbo` + `user_overlays=amp-timestamp`），
  内核镜像与主 DTB 不动；装之前在 TL101 上用 fdtoverlay 套在板上那份 DTB 的拷贝上验证（boot.cmd 失败不回退）。
- CNT_FREQID_REG 从 ARM 读也是 0（之前头文件注释写的"FREQID=24000000"不成立）；驱动改为 FREQID=0 时读 DT
  `clock-frequency = <24000000>`。
- 验证：u-boot 打印 "Applying user provided DT overlay amp-timestamp.dtbo"；开机自动加载并 probe
  （`freqid=24000000 (24.000 MHz)`）；sysfs `usec` 相对 CLOCK_MONOTONIC 走速 1.000000；sysfs 读数与寄存器一致；
  amp_ts_get_dev/get_timestamp/get_freqid 已导出；E902 在三次重启后仍正常（中断延迟 ≤0.38 µs、无残包）。
- RT 树同步加入：驱动、头文件、Kconfig/Makefile、dtsi 节点（以后整树出包时开 CONFIG_AW_AMP_TIMESTAMP 即可）。
- 说明与安装/撤销步骤：e902/linux/README.md。

---

# 补充记录 T33（2026-09-23 21:10）：公开厂商源码对照 —— 反汇编结论核对

全志公开 GitLab（无 NDA）`gitlab.com/tina5.0_aiot` 的 `lichee/arisc` 就是 scp.fex 的源码
（本地：e902/vendor-ref/arisc，A733 = sun60iw2p1；无 LICENSE，勿公开分发）。手册 V1.00：e902/doc/vendor-v1/。

| 项 | 我们（反汇编/实测） | 源码 / V1.00 手册 | 结论 |
|---|---|---|---|
| CPUS S_TIMER 布局 | 0x20 起、步长 0x20，CUR +0x08 | `EXT_TIMER_CTRL_REG(n) = base + 0x20*(n+1)`，IVL +4，CVL +8，另有 IVH +0xC、CVH +0x10 | 一致；源码多出高位寄存器（更宽计数器） |
| 定时器时钟 | R-CCU 0x100+4n，需开 bit0 | `EXT_TIMER_CLK_REG(n) = R_TIMER0_CLK + 4n`，bit0 使能 | 一致 |
| 定时器时钟源 | mux 4（24 MHz） | 源码 mux 0 "DCXO=24M"；V1.00 4.2.5.4：000 DCXO，100 SYS_CLK24M | **源码假设 24 MHz 晶振，本板是 26 MHz，按源码会快 8.3 %**；我们的选择对 |
| 握手 | 只需 type 0x90 的 feedback | `AR100_STARTUP_NOTIFY = 0x90`，attr HARDSYN，13 字版本字符串，只发一个包 | 一致（v64 删 ready 包正确） |
| 消息类型 | 0x11/0x22/0x24/0x25/0x26/0x60… | MESSAGE_BASE 0x10：0x11 SSTANDBY_RESTORE_NOTIFY、0x22 CPU_OP、0x24 SYS_OP、0x25/0x26 CLEAR/SET_WAKEUP_SRC、0x60 SET_DEBUG_LEVEL、0x61 LOOPBACK、0x62 SET_UART_BAUDRATE | 一致；0x61/0x62 名称此前推断错位（仅影响注释） |

结论：以后先查这份源码与 V1.00 手册，再决定是否需要反汇编；但源码是面向"24 MHz 晶振"的通用实现，
与本板实测冲突时以实测为准。

---

# 补充记录 T34（2026-09-23 21:20）：晶振定论（原理图 + Datasheet）；ISP 源码现状

## 晶振：26 MHz（定论）

| 证据 | 内容 |
|---|---|
| 原理图 doc/OPI ZERO 3W V1_2_原理图.pdf 第 8 页 | **Y1 = 26M-12pF-10ppm，TSX-2016**，接 DXIN(Y36)/DXOUT(AA36)，C150/C157 = 12 pF C0G；Y2 = 32.768K-12pF-20ppm（SX-3215） |
| 同页 strap | CK-SEL1（1K26）经 R13 470K 上拉 VCC-RTC，CK-SEL0（1K25）经 R15 0R 接地 → CK-SEL[1:0] = 10b |
| Datasheet V1.01 | 3.9.1 "external 26 MHz DCXO"；4.x CK-SEL0/1 = "Crystal Frequency Indicator"；5.8.1 整节为 26 MHz 晶振要求（推荐 20 ppm，本板 10 ppm） |
| 寄存器实测 | RTC XO_CTRL(0x07090160)[15:14] = 2（dcxo26M 档，与 CK-SEL=10b 一致）；Linux clk_summary dcxo 26000000 |
| 设计原因（推断） | REFCLK-OUT 经 R1134 输出 AP-UFS-CLKOUT 给 UFS；UFS 参考时钟只有 19.2/26/38.4 MHz 档，不含 24 MHz |

→ 厂商 SCP 源码里 "DCXO = 24M" 是旧平台遗留注释；需要 24 MHz 的地方用 pll-ref / SYS_CLK24M（R-CCU mux 4）。

## ISP（sunxi ISP602）开源情况

- 内核侧 vin/ISP 驱动：GPL 源码，在 BSP 内核树里。
- 用户态：gitlab.com/tina5.0_aiot/product/linux/external/libAWIspApi，`isp6xx` 对应 A733 的 ISP602：
  框架是源码（isp.c、isp_manage、isp_dev、isp_tuning、ini 解析、**tuning_app 调参服务端完整源码**）；
  3A 算法只有预编译 `libisp_algo.a/.so`（含 isp602_debian 的 gcc-10.2 / 14.2 版本），接口头文件 `include/isp_3a_ae.h`、`isp_3a_awb.h`。
- 板上现为 2025-09-08 的整包闭源 libisp.so / libisp_ini.so。

---

# 补充记录 T35（2026-09-23 21:40）：实时性 / 时间戳 / 跨核速率实测（v68）

| 项 | 结果 | 方法 |
|---|---|---|
| E902 定时器中断延迟（到期→ISR 体） | 0.04–0.38 µs，Linux 满载不变 | 心跳字 24/25 |
| A733 RT 内核 cyclictest（1 kHz，P95，8 核） | 空载 A55 平均 19 / 最大 36–49 µs，A76 平均 7–8 / 最大 23 µs；满载（hackbench+dd+内存，load 30）平均 23–34 / 最大 86–135 µs | cyclictest -m -S -p95 -i1000 |
| 共享时间戳读取（64 位防撕裂 = 3 次跨域读） | ARM 973–1128 ns，E902 1083 ns；分辨率 41.7 ns | xcore_bench.c / 固件 B 键 |
| E902 读 CPUS 外设寄存器 | 70 ns | B 键 |
| E902 SRAM 拷贝（32 位循环） | ≈ 95 MB/s | B 键 |
| E902 经 0x80000000 窗口读 DRAM（单字非缓存） | ≈ 12.5 MB/s | B 键 |
| ARM 读写共享 SRAM A2（/dev/mem，32 位） | 写 141–148 MB/s，读 28–53 MB/s | xcore_bench.c |
| mailbox 往返（ARM→E902→ARM，同步包） | 1 字：min 3.2 / 中位 4.1 / p99.9 5.6 / max 8.1 µs，24 万次/s；16 字：中位 18.1 µs，单向 3.4 MB/s | xcore_bench.c（taskset + chrt 90，rt_runtime=-1） |

- v68：类型 < 0x10（非厂商消息，MESSAGE_BASE=0x10）的回环包不打印日志——此前每包约 60 字符格式化占掉约 13 µs（中位 16.7 → 4.1 µs）。
- 测量陷阱：普通优先级进程会被调度出去（1.1 ms 毛刺）；SCHED_FIFO 忙等会被 RT 配额节流（48 ms 毛刺，sched_rt_runtime_us=950000）。
- E902 可用中断（手册 V1.00 Table 12-2 + 厂商源码 irqnum_config.h）：16 USB 待机、18 S_TWD、19 S_WDT、20–23 S_TIMER0–3、
  24 RTC 闹钟、25–28 PL/PM GPIO（安全/非安全）、29–30 S_UART0/1、31–33 S_TWI0–2、34 S_IRRX、35 S_PWM、36 S_TZMA、
  37 AHBS 超时、38 S_SPI、39 CPUS MSGBOX 读、48 CPUX MSGBOX 写、49 硬件自旋锁、54–77 GINTC 转发的 GIC 中断
  （GINTC 基址手册与源码均未给出，厂商固件也不用 → 目前不可用）。

---

# 补充记录 T36（2026-09-23 23:20）：dramlib 下载 + OEM DRAM 函数链接/反汇编/与板上固件字节对比

## 背景
T33 的公开 arisc 源码缺 3 个 OEM 私有 DRAM 函数（`dram_power_save_process`/`dram_power_up_process`/
`mctl_mdfs_software`），来自 `LICHEE_DRAMLIB_PATH` 预编译库。用户要求下载全量对比逆向。

## 1. 找到并下载官方 dramlib 仓库
- manifest 仓库 `tina5.0_aiot/manifest`（`tina5.0_aiot-linux-v1.5.0.xml`）列出全部 SDK 子仓库，
  其中 **`lichee/dramlib`** 即 `LICHEE_DRAMLIB_PATH`；已浅克隆到 `e902/vendor-ref/dramlib/`（1.9 MB）。
- 目标库路径与 ar100s Makefile 约定完全一致：`dramlib/sun60iw2p1/arisc_liboem/libar100s.a`。
- `nm` 确认 3 个符号齐全（T）；无公开源码（各芯片均只有预编译 `.a`）。

## 2. 完整厂商 SCP 链接成功
- ar100s 全树用 Xuantie GCC 14.1.1 编译通过；对 vendor 树打了 3 个构建兼容补丁
  （`Makefile:178`：`_zicsr` 走 `MARCH_FLAGS` 命令行覆盖 + `-Wno-error=deprecated` + `-fcommon`）。
  注意 vendor 的 march 就是 `-march=rv32emc -mabi=ilp32e`（cpu 目录名叫 e907，指令集与 E902 相同）。
- `make LICHEE_DRAMLIB_PATH=.../dramlib` → **scp.elf 完整链接**（text 57016 + data 64600，
  加载地址 0x40004000 = bl31 用的地址）。3 个 OEM 函数带符号：
  `dram_power_save_process@0x4000e16c`、`dram_power_up_process@0x4000e6b2`、`mctl_mdfs_software@0x4000f570`。

## 3. 反汇编产物
`e902/vendor-ref/oem-disasm/{dram_power_save_process,dram_power_up_process,mctl_mdfs_software}.dis`
（438/1524/318 行）。函数体可见 DRAM 控制器基址 `0x0a110xxx`/`0x0a510xxx`、`standby_flag`、
`time_udelay` 调用等，结构清晰可读。

## 4. 与板上运行固件（scp.fex.on-medium.bin）字节对比
对每函数每 0x40 取 24 B 窗口在板上固件中搜索：
- `dram_power_save_process`：9 窗命中（偏移平移恒定 0xc6a）
- `dram_power_up_process`：8 窗命中（平移 0xc62）
- `mctl_mdfs_software`：3 窗命中（平移 0xc7e）
- DRAM 特征常量 `lui 0xa110`/`lui 0xa510` 在板上固件各 ≥8 处命中
**结论：dramlib 预编译库与板上 scp.fex 同源**——直线代码段逐字节一致，函数首部不等只是
`auipc` 取址随链接布局变化 + 两版 .a 填补/编译细节微差（相邻函数间距 0x546 vs 0x54e）。
⇒ 三个函数不再"未逆向"：官方编译 + 带符号反汇编 + 板上固件定位齐备。

## 5. 关键警告（T33 重申，vendor 源码实测）
`timer-extended.c:64-67` 注释写 "source clock = 24M" 但 mux 写 `0x0`（DCXO）——**本板 DCXO=26 MHz**，
直接编出的厂商固件所有 ms 定时快 8.3%。烧 vendor 构建前必须打 mux 4（SYS_CLK24M）补丁（同 v65）。

## 6. 现状
- vendor 重构建产物 `vendor-ref/arisc/ar100s/scp.bin` **未打包未烧录**（121616 B raw > 板上原版
  105912 B，config 与出货版不完全相同）；是否以 vendor 为基底由用户决定。
- 供烧录还需：26M mux 补丁 → padding → 走 e902/FLASHING.md 流程 + 读卡器烧写校验。

---

# 补充记录 T37（2026-09-24 18:47）：改用厂商源码构建的 SCP，上板验证

## 构建
- `vendor-scp/build.sh`：arisc `0170020e` + dramlib `7142734a` + 3 个补丁（GCC 14 编译修复、S_TIMER 时钟改用 SYS_CLK24M mux 4、
  板级 defconfig 去掉 AXP517 与出厂一致）。产物 120856 B，sha256 `302deda8…`；bss 结束于 `0x4002D084`，低于栈底 `0x4002EC00`。
- 反汇编确认 `timer_init` 里有 `andi -113` / `ori 64`（3 位字段，mux=4）。
- 比出厂的 105912 B 大，是因为新版 dramlib 的 `.data` 里带了 LPDDR4/5 保存表（约 60 KB）。

## 启动包
- scp 是启动包最后一个条目（off `0x137C00`），卡上启动包之后（`0x1158000` 起，到 24 MiB）全是 0。
- `tools/bootpkg.py set-scp`：条目长度 `0x19DB8→0x1D818`，valid_len `0x154000→0x158000`（条目末尾按 16 KiB 向上取整，
  对出厂镜像正好还原成 0x154000），并重算 add_sum。
- 离线验证：出厂卡头 → 写厂商构建 → 再写回出厂 scp.fex，结果与原卡头**逐字节相同**。
- 9 份 `sd-head-before-flash-*` 备份与出厂卡头相比，只有 scp 槽位和 add_sum 不同（逐字节比对），已移出仓库。
  其中 20260923-193806 那份的 add_sum 本身无效，对应 09-23 那次进 FEL。

## 上板（observed）
- 在线刷写 `tools/flash-scp.sh`：写入后 checksum PASS、读回 PASS；boot0 接受了变大的启动包，约 20 s 后回到 Linux。
- 大核串口（3 次启动都一样）：`[SCP] :wait arisc ready....` → `arisc version: [0170020e421772d892d569dad5ef8b0faffc120a-dirty]`
  → `arisc startup ready` → `startup notify message feedback` → `sunxi-arisc driver is starting`。
- 厂商固件自己处理的 reboot 连续 2 次成功（每次约 37 s，boot_id 都变了）。
- R-CCU `0x07010100`/`0x07010104` = `0x41`（S_TIMER0/1 时钟 mux 4 + 使能）；`0x07091040`（TMR1 CTRL）= 1。
- `boot0: error: dtb not found for scp`：09-22 用出厂固件时也有（arm-console-live-20260922.log 里 24 次），不是新问题。
  厂商固件因此拿不到 s_uart 配置，小核串口没有输出。
- 未测：poweroff 后上电、休眠/唤醒（mem_sleep = deep）、DRAM 调频。

---

# 补充记录 T38（2026-09-25）：GINTC 基址锁定 0x07090000 + 幻影外设 + scp 打包集成

## GINTC 基址：0x07090000（高置信，待 E902 侧实测闭环）
- A733_User Manual V1.00 第 12.1 章 "Interrupt Controller"（RV_INT_CTRL=E902 侧）描述了
  该模块，pdftotext 后可检索：寄存器从 offset 0x0010 起（CPUS_INTC_CONFIG_REGN，组中断掩码，
  CONFIG_REG0[7:0] 对应 GIC IRQ [39:32]，以此类推），另有 E902 中断源选择表：
  **E902 IRQ 号 N 的配置寄存器在 offset 4*N**（IRQ16→0x40, IRQ20 S_TIMER0→0x50,
  IRQ66→0x108，全部吻合，含 12.1.5.10-15 的 System Interrupt1-6 State 0x0104-0x0118）。
- 手册第 2 章地址映射里，CPUS APB 区间 RTC(0x07085000) 与 S_TIMER(0x07091000) 之间有一个
  **无名的 4K 块 0x07090000-0x07090FFF** —— 上述全部 offset 落入 4K，判定为 GINTC 基址。
- 待闭环：E902 侧写 CONFIG 寄存器把一个真实 GIC IRQ 转进来并验证（下次改 e902-fw 时做）。

## 幻影外设（本次定性，补丁 0016 已禁用）
- **0x34 的 "AXP515" 是启动毛刺假象**：运行时 i2c-13 对 0x34 任何读写都失败（i2cget 0x03 也
  失败），而 0x36 的 AXP8191 正常（i2cdetect 显示 UU）；开机日志里同时刻有
  "Timeout when sending 9th SCL clk"/"TWI BUS error state" —— axp2101 驱动把总线毛刺读出的
  垃圾当成了 AXP515 的 ID，随后写 IRQ mask 0x40 撞上总线错误返回 -22，"failed to add irq
  chip: -22" 每次开机必现。原理图（OPI ZERO 3W）只有一个 PMU。
- **hym8563 (15-0051) 板上没有焊**：i2c-15 全地址无应答，init 失败 -22。
- 补丁 `userpatches/kernel/sun60iw2-current/0016-dts-zero3w-disable-phantom-pmu1-and-hym8563.patch`
  在板级 dts 里把两个节点 status 改 disabled。

## osnoise 定性（修正之前"厂商内核怪癖"的说法）
- 内核侧 osnoise tracer 正常（echo osnoise > current_tracer 成功，trace 有输出）。
- `rtla osnoise top` 在 ppoll 里挂死（task stack: do_sys_poll）；同一次开机里被杀过的
  timerlat 会让随后的 enable 返回 EBUSY（trace_osnoise.c:3021 timerlat_enabled() 检查）。
  结论：cyclictest / hwlatdetect / rtla timerlat / 裸 tracefs osnoise 都可用，rtla osnoise
  top 单独不可用，属 rtla↔厂商内核交互问题，不再追。

## scp.fex 打包集成（完成）
- `external/packages/pack-uboot/sun60iw2/bin/scp.fex` 已换成 `e902/fw-out/vendor-scp.bin`
  （md5 b9904524…；sun60iw2 不走 update_scp，boot_package.cfg item=scp 直接打包）。
- BUILD_OPT=u-boot 重打包后，boot_package.fex 偏移 0x138000 处逐字节确认 vendor 固件存在。
- `tools/preflight-image.sh` 的期望 md5 已同步更新；出厂原件备份在 `e902/backup/scp.fex.factory-20260925`。

## 全硬件盘点（noble 卡，2026-09-25）
可用：CPU 双簇 cpufreq-dt、GPU（OCL/Vulkan/GLES 库齐）、DE+HDMI、VE/libcedarc、ISP 驱动模块
（未启用）、WiFi aic8800、蓝牙 hci0（rfkill 解锁后 UP，UART 总线）、G2D（modprobe g2d_sunxi →
/dev/g2d）、3×PWM、status_led、gpiochip、TRNG、LRADC（input sunxi-keyboard）、7×i2c、ttyS0/1、
音频 HDMI、zram 交换。
不可用/缺件：RTC（硬件缺失）、NPU（缺 libVIPlite，厂商公开包只有 VIPhal/NBGlinker）、
无 eMMC（这颗板）、无网口、E902 收 GIC 中断（等 GINTC 闭环）。

## T38 追记（2026-09-25 深夜）
- `pin-2000000 "unknown pin"` ×4：uart0/uart5 pinmux 节点 `pins = "", ""`（厂商模板残留）。
  补丁 0017 删掉四行空引脚（节点保留，uart0 沿用 boot0 配好的 mux，uart5 未用）。
- `ccu_ddr "failed to find dram_clk"`：ccu-ddr.c 读 /dram 节点 dram_para[00]，本板没有 →
  驱动惰性 -ENODEV。**判定为设计意图**：DRAM 调频归 SCP（dramlib），激活 Linux 侧会造成
  双主管，不修。
- rtla osnoise top：strace 定位未完成（板子交付离线），定性维持 T38 原结论。
- 交板前快照：e902/backup/CARD-RUNTIME-STATE-20260925.md（含新镜像内含项与按卡手配项清单）。

## T38 追记 2（2026-09-26）：twi12 超时定性 + rtla strace 铁证 + 交板归档
- `twi-251C000 9th SCL timeout`：twi12 上挂的 **goodix,gt9271 触摸屏**（0x14，PK22/23）——
  Zero 3W 没有面板，又一处 A733 模板幻影设备。补丁 0018 禁用（`status = "disabled"`）。
- rtla osnoise top 挂死 strace 铁证：反复 openat `/proc/sys/fs/pipe-max-size`（fd 持续增长
  = 循环建管道），随后停在 tracing_on 上直到 SIGTERM —— rtla 前端与厂商内核的管道交互
  bug，永不到达采样阶段。结论不变：用 timerlat。
- 交板归档（e902/backup/card-20260925/）：orangepiEnv.txt（RT 参数）、GPADC blacklist、
  login.pam（无 pam_lastlog ✓）、dmesg-final.txt（axp515/hym8563 0 条 ✓）、final-check.txt。
- 最终镜像 = 0016+0017+0018，开机 ERR 级应只剩 pinctrl "unknown pin"（0017 修）之前的
  良性杂音；poweroff 后板子重启过一次（/tmp 清空），DTB 修复跨断电保持。

## T38 终记（2026-09-26 15:02）：闭环
- 卡上 DTB = 终版（cmp 确认），开机实测：TARGETED 错误（axp515/hym8563/irq-chip/9th SCL/
  unknown pin/missing pins）**0 条**，failed 服务 **0 个**。
- err 级总量 128 条全部为厂商驱动探测期噪音（mmc-4022000 ×36、sunxi-ufs ×33、sound-mach ×25、
  host_regs ×10、mmc-4021000 ×9 等），出厂镜像同样存在，不处理。
- 最终交接镜像：Orangepizero3w_1.0.2_ubuntu_noble_minimal_linux6.6.98.img（0016+0017v2+0018）。
- 交板归档完成：e902/backup/card-20260925/ + CARD-RUNTIME-STATE-20260925.md。

## T38 追记 3（2026-09-26）：NPU 翻案 —— 可用，无需 libVIPlite
- 之前"NPU 缺 libVIPlite 不可用"的结论**错误**。检查 npu 包演示二进制的 NEEDED：
  yolov5/vpm_run 只依赖 libNBGlinker + libVIPhal（+OpenCV），推理链就是
  NBGlinker（加载 .nb）→ VIPhal → vipcore（内核），libVIPlite 只属于旧 vip_lite
  API/模型编译侧。
- 板上实测 `cd /opt/vpm_run && ./vpm_run -s sample.txt -b 1`：加载 224×224×3 网络，
  **inference time=2890us, cycle=2444411，ret=0** —— NPU 硬件与运行时全通。
- yolov5 演示按 T736 编译（异常路径里的 build 路径可见），在 A733 上崩在
  OpenCV resize（参数解析不合），且需 OpenCV 4.5 soname——镜像配方已补
  libopencv_{core,imgproc,imgcodecs}.so.4.5 → .407 软链（4.x ABI 兼容）。
- 遗留（演示级，非 NPU 级）：yolov5 演示的参数/尺寸处理不适配，要用 yolov5.nb
  得自己写调用 NBGlinker 的小程序（头文件在 npu/usr/include）。
- 20 轮基准：sample 网 224×224×3，avg inference **2903 µs**（2875–2934，±1%），2.44M cycles/次。

## T38 终记补(2026-09-26):rtla 使用矩阵(实测钉死)
| 工具/用法 | 可靠性 |
|---|---|
| cyclictest | ✅ 永远可用;隔离 cpu5 实测 Avg 7 / Max 15 µs(1kHz) |
| hwlatdetect | ✅ 可用 |
| tracefs 裸用(echo osnoise > current_tracer + cat trace) | ✅ 可用 |
| rtla osnoise top(不带 -q) | ⚠️ 时好时坏 —— 成功过一次(打出完整统计表),新开机也可能 ppoll 挂死 |
| rtla timerlat top | ⚠️ 同样抖动;osnoise 开着时会 EBUSY(双向互斥),失败的运行会把 tracer 留在开启态 |
| rtla 任意命令带 -q | ❌ 静默路径在无初始数据时阻塞 ppoll,必挂 |
| rtla 失败/被杀之后 | 同次开机内后续 tracing 不可靠,重启恢复 |
结论:延迟测量以 **cyclictest(主)+ hwlatdetect + tracefs 裸用** 为准;rtla 属 best-effort。

## T38 追记 4（2026-09-26）：电源回归结论（休眠/poweroff/关机键）
- **关机键已确认**：板上 PWRON 测试点短接 GND = 电源键。行为实测：短按产生 KEY_POWER
  事件（axp8191-pek）、长按 ~6s 强制关机（用户短接过长，板子应声下电，重上电正常）。
  三个焊盘 = BOOT（勿动）/ PWRON / GND。
- **poweroff/冷启动回归**：✅ 当日多次冷启动（插拔电源）均正常恢复，无坏卡。
- **休眠（mem_sleep=deep）**：进入成功；**唤醒不可用** —— WoWLAN magic-packet（驱动支持、
  已 enable）实测唤不醒；PWRON 键唤醒未完成验证（用户定位焊盘时触发过长按关机）。
  连续第二次休眠尝试内核立即退出（"PM: suspend exit" + 服务 exit 1），需重启才能重试。
  **结论：当前 BSP 休眠不可靠，工业用法不要使用 mem 挂起**；journal 易失、无 pstore，
  崩溃现场取证需串口（本次串口捕获未生效，stty/cat 偶发失败，原因未查）。
- 板子"每隔几分钟重启"为虚惊：是用户插拔电源的冷启动；15:01 开机实际稳定运行 1h07m。
- 自发重启监视：watchdog 挂 30 分钟无再重启事件。

## T38 追记 5（2026-09-26）：休眠修复攻坚 —— 未闭环，下一步已定义
- 排除项：WiFi 的 sdc1 节点 **已有** keep-power-in-suspend/cap-sdio-irq/ignore-pm-notify
  （DT 不缺电源保持）；WoWLAN 可 enable 但魔术包唤不醒 → 问题在驱动/固件层，非 DT。
- 观测：第二次休眠尝试内核立即放弃（"PM: suspend exit"，无 entry，服务 exit 1）→
  每次开机只有第一次休眠机会；首次进入 deep 后不醒。
- 下一步诊断（板子可用时按序执行）：
  1. `cat /sys/power/pm_test` —— 若存在（CONFIG_PM_DEBUG），依次 echo
     freezer/devices/platform/processors/core 后 `echo mem`，找出失败层：
     devices 层失败 → dmesg 点名驱动，可 DT 修复；core 层失败 → bl31/SCP 的
     DRAM 自刷新路径，属厂商固件问题。
  2. 若无 pm_test：重编内核开 CONFIG_PM_DEBUG（+CONFIG_PM_ADVANCED_DEBUG）再诊断。
  3. 若定位到 core 层：对照 vendor arisc 源码的 suspend/DRAM self-refresh 路径
     （dram_power_save_process 已在 dramlib），需固件级开发。
- 结论：休眠唤醒是 bl31/SCP/驱动 层问题，Linux 用户态与 DT 均无法修复；
  工业用法以 poweroff 替代 mem。

## T38 追记 6（2026-09-26）：GINTC 固件代码就绪，等板闭环
- 从手册 12.1 章抽出的完整表（修正解析后 64 行）证实**输入号 = GIC id**：
  input 15=SGI15、16-31=PPI0-15、32=CPUX_MBOX_R、34-40=UART0-6、43-55=TWI0-12、
  56-59=SPI0-3、67/68=PWM0/1、69=LRADC、70=GPADC、71=THS、80=VE_ENC，
  每个输入 N 的配置寄存器在 0x07090000+4N —— E902 可路由的中断远超之前
  认知的 54-77 一段，vendor 固件只是从不用。
- e902-fw 新增 src/gintc.c（'G' 键 = 对 input 70/GPADC 做 24 值配置扫描，
  0.3 s 窗口计数，mailbox 保持轮询；'g' 键关闭）。fw-scp-padded.bin 已编出
  （sha256 2c782dbd…，105912 B，槽位兼容）。
- 上板验证手册：e902/tests/board/GINTC-TEST.md（flash → capture → modprobe
  sunxi_gpadc → 按 G → 判读；全流程 ~5 分钟，回滚成熟）。
- pm_test 诊断：CONFIG_PM_DEBUG=y 已在 a733 配置，/sys/power/pm_test 无需重编
  内核即可用 —— 休眠分层诊断随时可执行。

## T38 追记 7（2026-09-26）：休眠分层定位 —— devices 阶段硬挂死
- pm_test 阶梯实测：freezer 层通过（entry deep → 5 s → exit 干净）；devices 层
  `echo mem` 后**整机硬挂死**（ssh 失联、ping 无响应）—— 与真实休眠不唤醒吻合。
- 定位工具链：cmdline loglevel=1 吞掉了挂死前的驱动打印 → 复现时先 `dmesg -n 7`
  再串口录像，即可看到最后一个被调用的 suspend 回调 = 元凶驱动。
- 下一步：上电后带串口复现 devices 层 → 定位驱动 → DT 修复（0019）。

## T38 追记 7（2026-09-26 晚）：板外完成的四项（等板验证）
1. **rtla 挂死根因破解（纯源码分析）**：libtracefs 1.8 的 trace_pipe_raw 读取用
   子缓冲批量路径，阻塞回退走 `ring_buffer_poll_wait(..., buffer_percent)`——
   唤醒条件是**缓冲区填充 ≥ buffer_percent（默认 50%）**。安静系统上 osnoise
   事件稀疏永远到不了 50% → select() 挂几分钟 → "挂死"。开机后噪音多很快达标
   → 偶尔成功。补丁 `userpatches/kernel/sun60iw2-current/0019-rtla-poll-on-
   every-commit.patch`：rtla 建 instance 后写 buffer_percent=0（每次提交即唤醒）。
   下次内核构建生效；临时 workaround = 对 rtla 的 instance 目录手工写 0。
2. **GINTC 固件 v2**：gintc.c 重构为多输入计数矩阵（ISR 读 mcause 分流），
   新键 U（UART0 RX=34 边沿，ttyS0 打字计数）、L（LRADC=69 边沿）、I（计数报告）、
   g（全关）。fw-scp-padded.bin 已编出（13912 B 内核 + pad，sha 见 GINTC-TEST.md）。
3. **npurun**：npu-run/npurun.c + 交叉编译好的 aarch64 二进制——纯 C 的 vip_lite
   API 调用器（无 OpenCV），跑任意 .nb 并报耗时；yolov5.nb 可直接喂。
4. **suspend-diagnose.sh**：pm_test 分层 + 串口录像 + loglevel 控制的一键诊断
   （freezer/devices/platform/processors/core 阶梯，挂死层自动取证串口尾部）。
   注意 CONFIG_PM_DEBUG 已在配置中，无需重编内核。

## T38 追记 8（2026-09-26）：USB Device/BULK —— 无需新驱动,FunctionFS 方案就绪
- 内核配置已全：CONFIG_USB_F_FS=y + CONFIGFS_F_FS=y + CONFIGFS/ACM/NCM/UVC/
  MASS_STORAGE/ADB;DT usbc0 已是 usb_port_type=2(双角色)+ VBUS 检测。
- 角色切换现成接口:`echo usb_device|usb_host > .../10.usbc0/otg_role`
  (adb_conf.sh 在用);Type-C 方向自动切换由 TCPM 处理。
- 新增 usb-bulk/(FunctionFS BULK 方案):ffs-bulk.c + 交叉编译二进制 +
  bulk-gadget.sh(configfs 建 gadget → 绑 UDC → 起守护)+ host/bulk-test.py
  (pyusb 回环压测)。FFS 描述符按板上 6.6 ABI 写(magic=3,flags
  FFS_HAS_FS_DESC|HS_DESC;主机 UAPI 头的 struct 布局不同,已自定义结构绕开)。
- 注意:OTG 口是 USB2 高速,实测回环预期 ~30-40 MB/s;USB3 Type-C 是纯 HOST。
- libusb 定位澄清:libusb 是 PC 侧的库;板端 device 模式标准功能(ACM/NCM/
  UVC/MSD)零代码,自定义协议用 FunctionFS(板端 read/write 端点文件)。

## T38 追记 9（2026-09-28）：GINTC 实验矩阵 + Vulkan 定性 + 交付态恢复
- **GINTC 使能序列未破解**。已系统排除：per-input cfg 24 值平扫（input 70/121）、
  组寄存器 REG1/REG2/REG3 位操作、组线假设（input 72/73 = riscv_sys_irq_i）——
  全部零命中，测试源真实在跑（GPADC 风暴/TIMER0 ~70/s）。寄存器模型已确认：
  输入号=GIC id、组寄存器=转发使能位（1=forward）、组线=input 72/73。
  疑点：缺全局使能/触发类型/轮询周期等未文档化步骤。固件扫描框架（gintc.c v5）
  与手册解析已就绪，后续可继续（备选：反汇编 uboot/bl31 找 GINTC 写序列；
  对比 vendor 固件启动前后的 0x07090000 区转储）。
- 板子已恢复交付态：vendor 固件（sha 302deda8）+ 终版镜像。
- **Vulkan 定性**：驱动正常（vulkaninfo 枚举 BXM-4-64 MC1、1.3.277、conformance
  1.3.8.1、ICD 正常）；"不工作"= 无显示环境（DISPLAY/XDG_RUNTIME_DIR 缺失 +
  Xvfb 下 present 队列选不出）—— present 需要 vendor X 栈（桌面镜像方案）。
  计算路线用 OpenCL（已验证）或 headless surface（扩展存在）。
- npurun 调试记录：SIZES_OF_DIMENSION 的 value 参数 = 调用者预分配数组
  （传 &ptr 会被驱动写数组内容导致段错误）。

## T38 追记 10（2026-09-28）：Vulkan compute 实测通过
- 最小 compute 冒烟测试（vkcomp: SPIR-V 着色器分派 256 线程写 42.0）在板上
  **端到端跑通**：instance/device/queue family 0/存储缓冲/描述符/管线/分派/
  结果校验全链路 OK（bad=0），PVR 电源状态正常轮转。测试程序与 SPIR-V 在
  /tmp/vktest（交叉编译：aarch64 gcc + 主机 vulkan 头 + 镜像内 libvulkan.so.1，
  链接用 -l:libvulkan.so.1）。
- 结论修正：**Vulkan compute 可用**；不可用的是 (a) 无显示环境时的 WSI 程序
  （vkcube 报 DISPLAY/XDG_RUNTIME_DIR/present-queue 错误，Xvfb 也不够——present
  需要厂商 X 栈+DRM），(b) -D 时长停止等 rtla 式周边问题。
- 与 hwapi 对照：GPU dev headers 未装（CL/EGL/GLES/vulkan 头文件）——开发时
  需在板上补 vulkan 头或用交叉编译（本次即交叉编译方案）。
