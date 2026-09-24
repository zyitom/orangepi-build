# 资料溯源表：本项目的每个结论分别来自哪份资料

日期：2026-09-22 · 全部路径相对 `/home/helios/Desktop/orangepi-build/e902/`

---

## A. 你手上其实有 **4 份不同的手册**（来自 3 个不同的出版方）

| # | 文件 | 是什么 | 出版方 | 页数 / 大小 | 生成时间 |
|---|---|---|---|---|---|
| 1 | `doc/A733_User Manual_V0.91.pdf` | **A733 芯片用户手册 v0.91** —— SoC 级：内存映射、R-CCU 寄存器、MSGBOX、**Table 12-2 中断源**、CLIC、E902_CFG | **Allwinner（全志）** —— 芯片厂商 | **1990 页** / 12.7 MB | 2024-12-13 |
| 2 | `doc/玄铁E902R3S0用户手册_Rev.12_20260104.pdf` | **玄铁 E902 R3S0 CPU 用户手册 Rev.12** —— 核级：CLIC 寄存器、CSR、指令集、时序 | **平头哥 T-Head / XuanTie** —— CPU IP 厂商 | 137 页 / 1.9 MB | 2026-01-04 |
| 3 | `doc/OrangePi_Zero3W_A733_用户手册_v1.0.pdf` | **Orange Pi Zero 3W 板级用户手册 v1.0** —— **§3.14「40 pin 接口引脚说明」**、§3.16 SPI/I2C/UART/PWM 测试步骤 | **香橙派 / 迅龙软件** —— 板厂 | 262 页 / 16.7 MB | 2026-04-07（作者 leeboby） |
| 4 | `doc/OPI ZERO 3W V1_2_原理图.pdf` | **Orange Pi Zero 3W V1.2 原理图** —— 18 页 A3 | 香橙派 / 迅龙软件 | 18 页 / 1.1 MB | 2026-04-20 |
| 5 | `opene902/doc/玄铁E902集成手册(opene902).pdf` | **E902 集成手册** —— CLIC 在核内 TCIP 窗口、集成约束 | 平头哥（openE902 仓） | 46 页 | 2024-06-27 |
| 6 | `opene902/doc/玄铁E902用户手册(opene902).pdf` | E902 用户手册（openE902 版，比 #2 早） | 平头哥 | 135 页 | 2024-06-27 |
| 7 | `opene902/doc/opene902_datasheet.pdf` | openE902 数据手册（简版） | 平头哥 | 3 页 / 23 MB | 2024-06-27 |

> **`opene902/` 是个 git 仓**：`https://github.com/XUANTIE-RV/opene902.git`（平头哥官方，最后提交 2024-06-28）。
> 它里面**不只有文档，还有 RTL 源码**：`E902_RTL_FACTORY/gen_rtl/`（Verilog）+ `smart_run/`（RTL 仿真环境，含链接脚本、boot code、测试用例）。
> **这比手册更底层** —— 以后如果手册写得含糊（比如某个 CSR 行为），可以直接查 RTL。

## B. 软件基线（不是手册，是源码）

| 项 | 位置 | 来源 | 版本 |
|---|---|---|---|
| **内核** | `kernel/orange-pi-6.6-sun60iw2/` | **Allwinner BSP 内核**（不是主线！），隶属 `github.com/orangepi-xunlong/orangepi-build` | 6.6.98 |
| **U-Boot** | `u-boot/v2018.05-sun60iw2/` | Allwinner BSP U-Boot | 2018.07 |
| **厂商 SCP 固件** | `u-boot/v2018.05-sun60iw2/scp.fex` | **Allwinner 闭源二进制 blob**（我只做反汇编，没有源码） | 105912 B |
| orangepi-build 整体 | `/home/helios/Desktop/orangepi-build` | `github.com/orangepi-xunlong/orangepi-build` | 最后提交 2026-06-09 |

## C. **不是**厂商手册，而是之前会话/你自己的分析

| 文件 | 性质 |
|---|---|
| `ar0234-port/driver-completion/40pin/pin-capability.md` | **前一个会话写的**引脚能力笔记（它自己就标了"物理针脚映射需原理图核对"） |
| `ar0234-port/driver-completion/40pin/readme.md` | 同上，覆层启用方法笔记 |
| `ar0234-port/`（整个） | **你自己的项目**：AR0234 摄像头驱动移植到这块板子（已有上板实测记录） |
| `doc/e902/DESIGN-NOTES.md`、`HOWTO.md`、`HANDOFF-RT-IMAGE.md` | 之前会话写的分析笔记 |
| `doc/e902/*.md`（含本表） | **本次会话（2026-09-22）产出** |

## D. 本次会话新增的**实测**证据（不是任何手册）

| 证据 | 怎么来的 |
|---|---|
| T1–T21 全部实测 | 在板子上用 `/dev/mem`（`awdevmem.py`）读寄存器、读 SRAM、抓串口 |
| `scp.fex` 的反汇编（T14/T15/T16） | `objdump -D -b binary -m riscv:rv32` |
| 40-pin 引脚表的完整内容（§3.14） | **OCR**（那张表在原 PDF 里是图片，普通抽取拿不到） |
| 启动包 TOC 布局（T17） | 只读 `dd` 出介质前 24 MiB 后离线解析 |
| BMI088 支持 SPI 或 I2C | **网络检索** Bosch 数据手册的条目（*"fitted with digital interfaces (SPI or I2C)"*） |

---

## ⚠️ 一个必须记住的坑

板级 DTS `sun60i-a733-orangepi-zero3w.dts` 的头部是：

```
compatible = "xunlong,orangepi-4-pro", "arm,sun60iw2p1";
```

**它是从 Orange Pi 4 Pro 派生的**，所以：
- 里面的 `PIN_xx` 注释**不代表 Zero 3W 的实际排针编号**
- **物理针脚号必须以「手册 #3（§3.14）」或「原理图 #4」为准**

本次会话的 40-pin 结论正是改用 #3 之后才修正过来的（PL4/PL5 在 27/28 脚、PL6/PL7 根本没引出）。

---

## 引用哪份资料查什么（速查）

| 想知道什么 | 查哪份 |
|---|---|
| 中断号 / 内存映射 / R-CCU 寄存器 / MSGBOX / E902_CFG | **#1 A733 芯片手册**（Table 12-2 在第 616 页，E902_CFG 在 §5.2） |
| CLIC 寄存器怎么配、CSR 行为 | **#2 E902 CPU 手册**（§10.2 / §12.3）+ **#5 集成手册**（TCIP 窗口） |
| **40pin 哪一脚本、能配什么功能** | **#3 Zero 3W 用户手册 §3.14/§3.16**（唯一权威） |
| 排针到底连到哪个 SoC 焊盘 | **#4 原理图** |
| 某个寄存器在 BSP 里怎么用 | **内核/U-Boot 源码**（B 部分） |
| 厂商 SCP 到底在干什么 | **`scp.fex` 反汇编**（D 部分） |
| 某个 CSR 行为手册没说清 | **`opene902/E902_RTL_FACTORY/gen_rtl/` 的 Verilog** |
