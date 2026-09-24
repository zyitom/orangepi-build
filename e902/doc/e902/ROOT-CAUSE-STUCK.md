# 定位：为什么卡住 —— 三层问题，全部找到根因

日期：2026-09-22 · 依据：`e902/verify-logs/` 里的现场日志 + `scp.fex` 反汇编逐条重读

---

## 先把**我之前的错误判断**纠正掉

| 我之前说的 | 实际 |
|---|---|
| "第一次刷写板子死掉 = bl31 握手失败" | ❌ **不完整**。真凶是 **boot0 的启动包校验和 `add_sum`** —— 板子死在 boot0，比 bl31 更早 |
| "固件从未在真机上跑起来过" | ❌ **已经跑起来过了**（18:06 的日志） |

---

## 层 ①：第一次刷写为什么把板子刷死 —— **启动包校验和**

启动包（`sunxi-package`）头部有一个 **`add_sum`** 字段，boot0 会用它校验**整个包**：

```
sunxi_generate_checksum(buf, valid_len, 1, stored)
   = LE-u32-word-sum(buf[0..valid_len)) - stored + STAMP_VALUE(0x5F0A6C39)
要求结果 == stored
```

**我只替换了 scp 那 105912 字节，没有重算 `add_sum`** → boot0 校验失败 → 拒绝启动包
（现象：`error:bad checksum / error:bad magic`）→ 板子死在**最早期**。

> 这一点是后来（T22，17:42）找到的，现场记录：`verify-logs/T22-fix-log.txt`
> *"T22 add_sum fix applied (2026-09-22 ~17:42, card in TL101 reader /dev/sdb)"*，
> 工具：`e902/tests/fix-bootpkg-sum.py`（算法从 `board_common.c:846` / `tools/mksunxiboot.c:19` 反推并验证过）。

**⇒ 这说明我当时的"握手失败"结论下早了。真正的第一道坎在 boot0。**

---

## 层 ②：校验和修好之后 —— **我们的固件真的跑起来了**

`verify-logs/e902-console-first-success-20260922.log`（18:06）：

```
=== A733 E902 firmware v2 ===
PL_CFG0     = 0xffff3322  (PL2/PL3 mux ok)
APBS1 (S_UART clock) = 24000000 Hz
RISCV_BGR   = 0x00010003
RST_START   = 0x40004000
startup feedback (ch3 hdr=0x00900200 13w) : FAILED     ← 握手失败
spi_init(500kHz) = ok                                   ← ★ S_SPI 编程成功
SPI VER=0x00010003 GC=0x00000083 TC=0x00000004
SPI CLK_CTL=0x00001017 FIFO_STA=0x00000000 INT_STA=0x00000032
sent HELLO to ARM
```

**这是实质成功**：E902 在执行我们的代码、初始化了 S_UART0、**成功编程了 S_SPI**
（`GC=0x83` = EN|MODE(master)|TP_EN；`CLK_CTL=0x1017` = CDR2=23 → 24 MHz/(2×24) = 500 kHz〔更正，T31：当时 mux 在 dcxo 上实际 26 MHz → 541 kHz；v65 起 mux 改 pll-ref 后公式成立〕）。

---

## 层 ③：现在仍然卡住的两个具体 bug

### bug #1 —— 握手失败：**邮箱等待判据写反了**（已逐条核对反汇编）

厂商发送函数（`scp.fex` `0x4000721e`）的实际逻辑，**每发一个字之前**都调一次等待：

```
40007240  mv a1,s0 ; li a0,3 ; jal 0x400071b2   ← 等一次
40007248  lw a4,0(s1) ; sw a4,124(a5)           ← 发第 1 个字（头）
4000724e  jal 0x400071b2                        ← 再等
40007258  lbu a4,4(s1) ; sw a4,124(a5)          ← 发第 2 个字（计数）
  循环 count 次：
400072aa  jal 0x400071b2                        ← ★ 每个数据字之前都等
400072b8  lw a3,0(a4) ; sw a3,124(a4)           ← 发数据字
```

而 `0x400071b2` 的判据（逐条读）：

```
lw  a4,0(s0)          ; 读 MSG_STATUS(CPUX_MSGBOX, ch)
li  a5,8
beq a4,a5,<wait>      ; ★ 等于 8  → 说明 FIFO 满 → 继续等（超时返回 -35）
         ; 不等于 8 → 返回 0（可以发）
```

**⇒ TX 可发条件 = `MSG_STATUS != 8`（8 = FIFO 深 = 满）**

**我固件里写的是：**

```c
if ((readl(MBOX_MSG_STATUS(MBOX_CPUX_BASE, MBOX_CH)) & MBOX_MSG_NUM_MASK) == 0)
    return 0;              /* ← 判据写反了 */
```

而且 `msgbox_send_packet()` **只在开头等一次**，然后连发 15 个字。

**致命点**：启动反馈包 = **1 头 + 1 计数 + 13 数据 = 15 个字**，而邮箱 FIFO **只有 8 深**
→ 不分批必然塞满 → `msgbox_try_send()` 返回 -1 → 整个包发送失败 → **`startup feedback : FAILED`** ✓

（对照：`0x400071ea` 的 RX 判据是 `status != 0` = 有消息 —— 这一条我写对了。）

### bug #2 —— ARM 侧 bootloop

`verify-logs/arm-console-v2-bootloop-20260922.log` 显示：boot0 → bl31 → u-boot → `Starting kernel ...`
→ `[12.143]Failed` → 重启 → 循环。

原因：小核没完成握手（bug #1）或其后行为异常 → Linux 起不来 → 重启。日志里也有
`NOTICE Error initializing runtime serv...` 与 `amp fw boot after!!!`，说明 bl31 的 SCP 运行时服务初始化是失败的。

---

## 修复清单（明确、可执行）

1. **改 `msgbox_wait_tx_not_full()` 的判据**：`(STATUS & 7) != 8` 才算可发，否则自旋等待（超时返回 -1）。
2. **每个字发送前都调用一次**（不是只在开头调一次）——`msgbox_send_packet()` 里逐字 pacing。
3. **握手失败要重试/报告**，不要静默继续。
4. 重建 → 重新 `fix-bootpkg-sum.py --write` → 刷 → 重启 → 看小核串口。

## 顺带确认的已有成果（这轮不是白干）

| 成果 | 证据 |
|---|---|
| E902 真的在跑我们的固件 | 18:06 日志的 banner |
| **E902 成功编程 S_SPI（`0x07092000`）** | `SPI VER=0x00010003 GC=0x83 CLK_CTL=0x1017` |
| E902 能跨域读写 CPUX 域时间戳寄存器 | 日志里的 `TSTAMP_CTRL = 0x00000001`、`t0/t1` 读数 |
| boot0 包校验和问题已定位并有工具 | `tests/fix-bootpkg-sum.py` |
| 回滚/演练脚本齐备 | `tests/{rollback-drill,drills,post-poweron}.sh` |
