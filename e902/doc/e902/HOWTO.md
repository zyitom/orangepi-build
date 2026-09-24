> ⚠️ **2026-09-22 更正**：本文档中"可以从 Linux 停核/换固件/启动 E902"的说法已被上板实测推翻，
> 详见 `doc/e902/CORRECTION-2026-09-22.md` 与 `doc/e902/FINDINGS-LEDGER.md`。
> 小核复位只能由 bl31 释放。

# E902 开发操作手册

从零开始：怎么写代码、怎么烧、怎么调、怎么跟 ARM 通信。

背景和为什么这么做，见 `DESIGN-NOTES.md`。这份文档只讲操作。

---

## 0. 先理解一件事：E902 不是外挂的单片机

它和 ARM 在**同一块芯片里**，共享内存和总线。所以：

- **没有 JTAG 可用**。E902 的调试口是 PL4~PL7，但这几个脚在这块板子上被
  红外(PL4)、I2C(PL5)、LED(PL7) 占了，没引出。**调试全靠串口 printf**。
- **不用"烧写"到 flash**。固件是拷进 SRAM 的，写完立刻能跑，断电就没。
  改一行代码到看到效果，大概 10 秒。
- **不用重启板子**。Linux 跑着的时候就能停 E902、换固件、再启动。
- **它现在正在跑厂商固件**（`scp.fex`，管 DRAM 变频/待机/电源）。你是
  "借用"这颗核，用完可以还回去。

---

## 1. 一次性准备

### 1.1 电脑上：装工具链

已经装好了：

```
/home/zyi/3rd_party/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0-20250627/
    Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0/bin/riscv64-unknown-elf-*
```

设个环境变量省事，加到 `~/.bashrc`：

```bash
export E902_CROSS=/home/zyi/3rd_party/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0-20250627/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0/bin/riscv64-unknown-elf-
```

之后编译就只要 `make CROSS=$E902_CROSS`。

### 1.2 硬件：接串口线

USB-TTL 模块（CH340/CP2102 之类，**3.3V** 电平，别用 5V 的）接到 40-pin 排针：

```
排针 pin 16  (PL2, 网络名 SCPU-TX)  ──→  USB-TTL 的 RX
排针 pin 18  (PL3, 网络名 SCPU-RX)  ──→  USB-TTL 的 TX
排针 pin 6/9/14/20/25/30 (GND)      ──→  USB-TTL 的 GND
```

TX/RX 是交叉的：板子的 TX 接模块的 RX。

⚠️ 这跟你平时用的**调试串口不是同一个**。调试串口（`console=ttyS0`）走
PB4/PB5，网络名 `CPU-TX`/`CPU-RX`，那个继续正常用。E902 这个是板厂另外
预留的，原理图上标 `SCPU-TX`/`SCPU-RX`。

电脑上开串口终端：

```bash
sudo apt install picocom          # 或者 minicom / screen
sudo picocom -b 115200 /dev/ttyUSB0
# 退出：Ctrl-A 然后 Ctrl-X
```

### 1.3 板子上：装依赖 + 拷文件

不用装 `busybox` / `devmem2` —— 官方镜像里两个都没有，而且它们也读不了 SRAM_A2
（见下）。需要的 `python3` 和 `gcc` 镜像自带，实测：

```bash
# 板子上执行，确认一下就行
for t in python3 gcc; do printf '%-8s %s\n' $t "$(command -v $t || echo 缺)"; done
```

```bash
# 从电脑拷过去。awdevmem.py 是必需的：它负责 E902↔ARM 地址换算
scp e902-fw/build/fw.bin \
    e902-fw/scripts/awdevmem.py \
    e902-fw/scripts/e902-load.sh \
    e902-fw/scripts/e902-restore.sh \
    e902-fw/scripts/arm-msgbox-test.c \
    doc/e902/check-e902.sh \
    u-boot/v2018.05-sun60iw2/scp.fex \
    orangepi@<板子IP>:~/e902/
```

只有串口、没有网络时，用 `doc/e902/serial-put.py` 走控制台传（base64 + sha256 校验）：

```bash
# 电脑上执行
python3 doc/e902/serial-put.py e902-fw/scripts/awdevmem.py /tmp/awdevmem.py
python3 doc/e902/serial-run.py -s 'python3 /tmp/awdevmem.py read 0x07032204'
```

### 1.3.1 ⚠️ 千万别用 dd 往 0x40014000 写

`0x40014000` 是 **E902 视角**的地址。ARM 侧同一片 SRAM_A2 在 `0x00040000`，
而 ARM 的 `0x40000000` 是 DRAM（`/proc/iomem`：`40000000-13fffffff System RAM`）。
所以：

```bash
# 错的，会破坏内核内存，而且 dd 还会报成功
sudo dd if=fw.bin of=/dev/mem seek=$((0x40014000)) ...
```

这台板子 `CONFIG_STRICT_DEVMEM` 没开启，内核不拦；那 128K 落在活着的页上，
后果延迟发作（段错误 / 读到乱码 / 脏页把垃圾写回文件系统）。
换算是 `ARM = 0x00040000 + (E902 - 0x40000000)`，即 `0x40014000 → 0x00054000`。
另外 SRAM_A2 只能 `mmap`，`dd` 的 lseek 会被拒（`Bad address`）。
`awdevmem.py` 把这些都处理了，并且写完读回校验，所以统一用它。

`scp.fex` 一定要拷 —— 那是厂商固件，用完还回去要用它。

### 1.4 板子上：内核要开一个选项

`CONFIG_AW_MSGBOX=y`。不开的话 ARM 侧收不到消息（DTS 节点是 `okay` 的，但
编进内核的驱动 compatible 对不上，节点悬空）。

已经在 `external/config/kernel/linux-sun60iw2-current-a733.config` 改好了，
重新编内核装上：

```bash
# 电脑上
./build.sh BOARD=orangepizero3w BRANCH=current BUILD_OPT=kernel
# 拷 output/debs/linux-image-*.deb 和 linux-dtb-*.deb 到板子
sudo dpkg -i linux-dtb-*.deb linux-image-*.deb
sudo reboot
```

**u-boot 不用动。** 检查一下有没有生效：

```bash
ls /sys/bus/platform/drivers/ | grep msgbox        # 应该有 sunxi-msgbox
dmesg | grep -i msgbox
```

---

## 2. 日常开发循环

写完代码之后的完整一轮，大概 10 秒：

```bash
# ── 电脑上 ──
cd e902-fw
vim src/main.c                      # 改代码
make CROSS=$E902_CROSS              # 编译，2 秒
scp build/fw.bin orangepi@<板子IP>:~/e902/

# ── 板子上（另一个 ssh 窗口）──
cd ~/e902 && sudo ./e902-load.sh fw.bin

# ── 串口终端里 ──
# 立刻能看到新固件的输出
```

不用重启、不用重新烧卡、不用改 u-boot。

第一次跑之前，先存个基线好对比：

```bash
sudo sh check-e902.sh | tee before.txt
```

---

## 3. 第一次运行会看到什么

`sudo ./e902-load.sh fw.bin` 的输出：

```
firmware : fw.bin (2896 bytes)
load addr: 0x40014000 (E902 view)

before: RISCV_BGR=0x00010003  RST_START=0x40004000
1/4 asserting reset ...
2/4 copying image into SRAM_A2 (verified read-back) ...
fw.bin: 2896 bytes
E902 0x40014000  ->  ARM 0x00054000
verify ok (2896 bytes read back identical)
3/4 setting reset vector ...
4/4 releasing reset ...

after : RISCV_BGR=0x00010003  RST_START=0x40014000
```

注意 `before` 那行的 `RST_START=0x40004000` —— 那是厂商 `scp.fex` 的入口，
说明加载前跑的是厂商固件。`after` 变成 `0x40014000` 才是我们的固件。
`verify ok` 那行是关键：它证明字节真的进了 SRAM，而不是被写进了 DRAM。

串口终端里：

```
=== A733 E902 firmware ===
PL_CFG0     = 0x33333333  (PL2/PL3 mux ok)
APBS1 (S_UART clock) = 200000000 Hz
RISCV_BGR   = 0x00010003
RST_START   = 0x40014000
DDR_REMAP   = 0x00000000
PAD_LPMD    = 0x00000003
msgbox ready (CLIC irq 48). UART echo on; 's' sends to ARM.
sent HELLO to ARM
```

（`PL_CFG0` 和 `APBS1` 的具体数值会不一样，那是读回来的实际值）

然后键盘敲字符会回显，敲 `s` 会给 ARM 发消息，敲 `r` 显示收到多少条。

---

## 4. 跟 ARM 通信

### 4.1 硬件机制

两块独立的单向 FIFO，各 4 通道，每通道 8 条 × 32 位：

```
E902 → ARM :  MBOX_CPUX  0x03004000    E902 写这里，ARM 读这里
ARM → E902 :  MBOX_CPUS  0x07094000    ARM 写这里，E902 读这里
```

写进去硬件自动给对方拉中断，没有额外的通知步骤。E902 侧的接收中断是
CLIC IRQ 48。

**一条消息就是 32 位整数**，含义你自己定。现在的约定：

```
[31:24] 命令   [23:0] 数据
```

| 命令 | 方向 | 含义 |
|---|---|---|
`0x01` PING | ARM → E902 | E902 回 PONG |
`0x02` PONG | E902 → ARM | |
`0x10` HELLO | E902 → ARM | 固件启动时发一次 |
`0x11` UART_KEY | E902 → ARM | E902 串口收到按键 |
`0x20` ECHO | 双向 | 原样返回 |

### 4.2 先用测试工具验证

板子上编译一次：

```bash
gcc -O2 -o amt arm-msgbox-test.c
```

**测 ARM → E902**：

```bash
sudo ./amt ping
```

ARM 侧输出：
```
  -> 0x01000abc  cmd=0x01 (PING)
listening ...
  <- 0x02000abc  cmd=0x02 (PONG    ) payload=0x000abc
```

同时 E902 串口输出：
```
[rx] 0x01000abc  cmd=0x00000001 (ping) -> pong
```

**测 E902 → ARM**：

```bash
sudo ./amt          # 只监听
```

然后在 E902 串口终端敲 `s`：

```
# E902 串口：
[tx] seq=1 sent

# ARM 侧：
  <- 0x11000001  cmd=0x11 (UART_KEY) payload=0x000001
```

两个方向都通了，说明整条链路没问题。

### 4.3 在 E902 固件里收发

```c
#include "fw.h"

/* 发（非阻塞版本 msgbox_try_send 返回 -1 表示 FIFO 满） */
msgbox_send(MSG(0x30, 12345));

/* 收：中断里已经处理好了，主循环检查标志 */
if (rx_flag) {
    u32 m = last_rx;
    rx_flag = 0;
    if (MSG_CMD(m) == 0x30) {
        /* 你的逻辑 */
    }
}
```

`main.c` 里的 `msgbox_isr()` 已经把 FIFO 收干净并置了 `rx_flag`，你只要在
主循环里读。中断处理函数要短，别在里面 printf。

### 4.4 ARM 侧写真程序

测试工具用 `/dev/mem` 直接怼寄存器，方便但不正规。正规做法是写内核
mailbox 客户端：

```c
#include <linux/mailbox_client.h>

static struct mbox_client cl;
static struct mbox_chan *chan;

static void rx_callback(struct mbox_client *cl, void *msg)
{
    u32 m = *(u32 *)msg;
    pr_info("from E902: 0x%08x\n", m);
}

/* probe 里 */
cl.dev = dev;
cl.rx_callback = rx_callback;
cl.tx_block = true;
chan = mbox_request_channel(&cl, 0);

/* 发送 */
u32 msg = 0x30003039;
mbox_send_message(chan, &msg);
```

DTS 里要给你的节点加：

```
mboxes = <&msgbox 0>;      /* 通道 0 */
mbox-names = "e902";
```

⚠️ 一旦有了内核客户端，`arm-msgbox-test.c` 就不能再用了 —— 两边会抢同一个
FIFO。在那之前测试工具是安全的（驱动没客户端时不开接收中断）。

大数据传不动怎么办：32 位一条塞不下就用共享内存，msgbox 只传个地址/偏移，
互斥用 hwspinlock（`0x03005000`，DTS 已经 `okay`）。

---

## 5. 调试：只有 printf

没有 JTAG，所以就三招。

### 5.1 串口打印

```c
uart_puts("到这里了\n");
uart_put_hex(some_value);          // 0x1234abcd
uart_put_dec(count);               // 12345
uart_putc('x');
```

⚠️ **中断处理函数里不要 printf**。`uart_putc()` 是忙等 FIFO 的，在中断里
调用会拖长中断时间，可能丢消息。要打印就置个标志，让主循环打。

### 5.2 卡死了怎么定位

固件如果跑飞（非法指令、访问越界地址），会跳到 `start.S` 的 `trap_entry`，
把 `mcause` 和 `mepc` 存进 `last_mcause` / `last_mepc` 然后停住。

从 ARM 侧读出来（这俩符号在 `.bss` 里，用 nm 查地址）：

```bash
# 电脑上查地址（${E902_CROSS} 结尾已含 riscv64-unknown-elf-）
${E902_CROSS}nm build/fw.elf | grep last_

# 板子上读（当前这版固件的地址，改代码后要重新查）
# 这两个是 SRAM_A2 里的变量，必须走 awdevmem.py（会换算到 ARM 侧）
sudo python3 awdevmem.py dump --e902 0x400149a0 --count 8   # last_mcause, last_mepc
```

`mepc` 是出错的指令地址，拿去查反汇编：

```bash
make CROSS=$E902_CROSS dis
grep -A3 "40014abc:" build/fw.dis
```

`mcause` 常见值：

| 值 | 含义 |
|---|---|
`0x2` | 非法指令（多半用了 RV32E 没有的指令，或者 x16+ 寄存器） |
`0x5` | load 访问错误（地址不对，或者那块外设时钟没开） |
`0x7` | store 访问错误（同上） |
`0xb` | 环境调用 |

### 5.3 完全没输出：查寄存器

```bash
sudo python3 awdevmem.py read 0x0701021C   # RISCV_BGR，应该 0x00010003
sudo python3 awdevmem.py read 0x07032204   # 起始 PC，加载后应该 0x40014000
sudo python3 awdevmem.py read 0x0701018C   # S_UART_BGR，应该 0x00010001

# 看 SRAM 里到底装了什么（地址给 E902 视角，工具内部换算）
sudo python3 awdevmem.py dump --e902 0x40014000 --count 16
# 我们的固件和厂商 scp.fex 入口序列相同，前 8 字节都是 8140 0141 8141 0142
# 想确认是谁在跑，就比 0x07032204 的值：0x40014000=我们的，0x40004000=厂商的
```

寄存器地址（`0x07...`）两边视角一致，不用换算；SRAM 地址必须用 `--e902`。

如果内存里的字节对、寄存器也对，但串口没输出 —— 检查接线（TX/RX 有没有
交叉）、波特率、GND 有没有接。

---

## 6. 加自己的功能

### 6.1 加一个新外设

以 S_TIMER0 为例（`0x07091000`，中断号 20）：

**第一步**，在 `src/a733.h` 加地址定义。地址从手册第 2 章的表里查，
时钟/复位位从 4.2.5 节查：

```c
#define S_TIMER_BASE      0x07091000
#define S_TIMER_BGR_REG   (R_CCU_BASE + 0x011C)   /* 手册 4.2.5.x */
```

**第二步**，在 `src/soc.c` 的 `soc_early_init()` 里开时钟：

```c
bgr_enable(S_TIMER_BGR_REG, 16, 0);    /* bit16=复位 bit0=时钟 */
```

⚠️ **忘了开时钟的后果**：读写那个外设的寄存器会触发 access fault
（`mcause` = 0x5 或 0x7），固件停在 `trap_entry`。这是最常见的错误。

**第三步**，写驱动，加到 `Makefile` 的 `SRCS`。

### 6.2 加一个中断

```c
static void __attribute__((interrupt("machine"))) my_isr(void)
{
    /* 处理 */
    /* 清外设的中断标志 */
}

/* main() 里 */
clic_set_handler(IRQ_S_TIMER0, my_isr);
clic_enable(IRQ_S_TIMER0, 0);      /* 0=电平触发 1=边沿触发 */
```

⚠️ `__attribute__((interrupt("machine")))` **必须写**。CLIC 硬件矢量跳转
不保存寄存器，这个属性让编译器生成保存/恢复代码并用 `mret` 返回。少了它
会把主程序的寄存器搞坏，症状是随机跑飞。

中断号查手册表 12-2（`DESIGN-NOTES.md` 第 2.4 节抄了常用的）。

### 6.3 控制 GPIO

**先确认引脚没被 Linux 占用。** PL 组的占用情况：

| 脚 | 用途 | 能用吗 |
|---|---|---|
PL0/PL1 | s_twi0 → **PMIC** | ❌ 绝对别动 |
PL2/PL3 | 我们的串口 | 已用 |
PL4 | 红外接收 | ❌ |
PL5 | s_twi2 SDA (pin 27) | ❌ |
PL6 | 空闲 | ✅ |
PL7 | status LED | ⚠️ 会跟 Linux 抢 |
PM0~PM4 | WiFi/蓝牙 | ❌ |

只有 PL6 是真干净的。

```c
/* pinmux.c 里已有 set_pl_function()，改成 non-static 就能用 */
set_pl_function(6, PL_FUNC_OUTPUT);

/* 拉高/拉低 */
u32 v = readl(PL_DAT);
writel(v | (1u << 6), PL_DAT);      /* 高 */
writel(v & ~(1u << 6), PL_DAT);     /* 低 */
```

⚠️ **`PL_CFG0` 一个 32 位寄存器装 8 个脚**，PL0/PL1 是 PMIC 的 I2C。改
PL6 要读-改-写整个寄存器，如果 Linux 同时也在改（配 LED、红外时会），
可能互相覆盖 —— 最坏情况把 PMIC 的 I2C 配置搞坏。所以：

- 只在初始化时配一次功能，别在循环里改
- 循环里只写 `PL_DAT`（`0x0010`），那个寄存器 Linux 用得少
- **等 Linux 完全启动完再加载固件**

---

## 7. 用完还回去

```bash
sudo ./e902-restore.sh scp.fex
```

厂商固件回来，DRAM 变频/待机/电源看护恢复。

如果不确定状态对不对，**重启一次最干净** —— 开机时 bl31 会按正常流程加载
`scp.fex` 并传给它参数块（DRAM 时序、DVFS 电压表、PMIC 配置）。
`e902-restore.sh` 是冷启动，拿不到那个参数块。

---

## 8. 你固件跑着的时候，会失去什么

厂商固件在管四件事，你借用这颗核期间它们就没了：

| 失去 | 后果 | 严重度 |
|---|---|---|
DRAM 变频 | 内存频率固定，掉点性能/功耗 | 能忍 |
**suspend** | **`echo mem` 醒不过来，要硬复位** | ⚠️ 别试 |
PMIC 额外看护 | Linux 自己的 AXP 驱动还在，基本电源没问题 | 低 |
时钟低功耗切换 | 边缘情况可能异常 | 低 |

**CPU 变频不受影响**（Linux 自己写 PLL，不经过 E902）。
**调试串口不受影响**（`uart0` 跟 `uart7` 是两套硬件）。

**唯一硬规矩：你固件跑着的时候不要 suspend。**

---

## 9. 常见问题

**Q: 要不要重新烧 rootfs？**
不用。内核和 DTB 通过 deb 包装（`BUILD_OPT=kernel`），E902 固件是运行时
拷进 SRAM 的，u-boot 完全不用改。

**Q: 断电之后固件还在吗？**
不在。SRAM 断电就没了，开机跑的还是厂商 `scp.fex`。想开机自动加载，写个
systemd service 调 `e902-load.sh`。

**Q: 能同时用 uart0 和 uart7 吗？**
能，两套独立硬件。`uart0`（PB4/PB5）是 Linux 的 `console=ttyS0`，
`uart7`（PL2/PL3）给 E902。互不干扰。

**Q: 固件最大能多大？**
128KB。装在 `0x40014000`，SRAM_A2 到 `0x40034000` 结束。链接脚本里有
`ASSERT` 检查，超了会编译报错。现在用了 2.4KB。

**Q: 能用 printf/malloc 吗？**
不能，是 `-nostdlib` 裸机环境。用 `uart_puts`/`uart_put_hex`/`uart_put_dec`。
要浮点或复杂库的话得链 newlib（工具链自带），但 RV32E 没硬件浮点，软浮点
会很占空间。

**Q: RV32E 有什么坑？**
只有 16 个通用寄存器（x0~x15）。用到 x16 以上会触发非法指令异常
（`mcause`=0x2）。C 代码不用担心，编译器知道；写汇编要注意。

**Q: 改了地址定义之后跑飞了？**
八成是忘了在 `soc_early_init()` 里开那个外设的时钟。访问未开时钟的外设会
access fault。按 5.2 节读 `mcause`/`mepc` 定位。

---

## 10. 命令速查

```bash
# 编译
make CROSS=$E902_CROSS              # 出 build/fw.bin
make CROSS=$E902_CROSS dis          # 出 build/fw.dis 反汇编
make CROSS=$E902_CROSS size         # 看大小
make clean

# 板子上
sudo ./e902-load.sh fw.bin          # 加载启动
sudo ./e902-restore.sh scp.fex      # 还回去
sudo sh check-e902.sh               # 体检
gcc -O2 -o amt arm-msgbox-test.c    # 编测试工具
sudo ./amt ping                     # 发 PING
sudo ./amt                          # 只监听

# 串口
sudo picocom -b 115200 /dev/ttyUSB0     # Ctrl-A Ctrl-X 退出

# 关键寄存器（寄存器地址两边视角一致，直接读）
sudo python3 awdevmem.py read 0x0701021C   # RISCV_BGR (0x00010003 = 在跑)
sudo python3 awdevmem.py read 0x07032204   # 起始 PC：0x40014000=我们的 / 0x40004000=厂商
sudo python3 awdevmem.py read 0x07025000   # PL_CFG0 (引脚复用)

# SRAM 内容（--e902 是 E902 视角，工具换算到 ARM 侧 0x00040000+）
sudo python3 awdevmem.py dump --e902 0x40014000 --count 64
```

## 11. 串口终端里的按键

| 键 | 作用 |
|---|---|
`s` | 给 ARM 发一条 UART_KEY 消息 |
`r` | 显示收到多少条 ARM 消息 |
其他 | 原样回显（验证 RX 通路） |
