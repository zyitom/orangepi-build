# ⚠️ 更正（2026-09-22）：从 Linux "借用 E902 核"的路线在硬件上不成立

本目录（`doc/e902/`）与 `../e902-fw/` 里早期的若干说法**已被 2026-09-22 的上板实测推翻**。
在按旧文档操作之前，先读这一页。原始证据见 `FINDINGS-LEDGER.md`。

## 被推翻的说法

| 旧说法（出处） | 实测结果 |
|---|---|
| "可以从 `/dev/mem` 停核、换固件、启动，Linux 完全不知情"（`e902-fw/README.md`「Borrow the core」） | ❌ **不成立**。`RISCV_BGR` 的 bit0 只是**时钟门控**，不是内核复位；手册 4.2.5.24 里该寄存器只有 `RISCV_CFG_RST`（复位 CFG 寄存器块）。 |
| "加载后 E902 就会从 `0x40014000` / `0x40020000` 开始执行"（`e902-load.sh` 结尾提示） | ❌ **不成立**。设置 `E902_RST_START_ADDR` 后**没有任何机制释放复位**，内核继续执行原来的代码。 |
| "固件可能 bug：CLIC irq 48 应为 39"（`HANDOFF-RT-IMAGE.md`） | ❌ **48 是对的**。手册表 12-2 原文：48 = "CPUX MSGBOX WRITE IRQ FOR **CPUS/RISCV**"，正是 CPUX 写消息时通知 CPUS 的中断；39 是 `CPUS_MBOX_READ_IRQ`。 |
| HOWTO 里工具链在 `/home/zyi/3rd_party/...` | ❌ 该路径在 TL101 上不存在。实际在 `e902/toolchains/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0`。 |
| 任务描述里给的板子 sudo 口令 | ❌ 板子 sudo 口令是 `orangepi`；任务描述给的是 **TL101** 的 sudo 口令。 |

## 实测过的 9 组尝试（全部无效）

写 `RISCV_BGR(0x0701021C)` = `0x00010003 / 0x00010002 / 0x00010000 / 0x00010001 /
0x00000003 / 0x00000002 / 0x00000000`，以及"设向量后脉冲 bit16 / 脉冲 bit0"两组序列 ——
用只写 SRAM、不碰任何外设的 `fw_min2.bin`(768 B) 探针，心跳 scratch **恒为 0**。
即：不是固件跑挂，而是**核心根本没有从我们设置的向量取指**。

## 结论

E902 的复位只能由 **bl31** 在启动流程中释放（`E902_RST_START_ADDR` 的定义就是
"复位释放时的 PC"）。因此要让小核跑自己的代码，唯一路径是让 bl31 载入我们的镜像，
并完成它与 bl31 的握手。详见 `RECOVERY-AND-HANDOFF.md` §7。

## 本轮新增（仍然有效的成果）

- `RESOURCE-MAP.md` —— 时钟/复位/中断/引脚/内存的归属表，逐项带证据。
- `FINDINGS-LEDGER.md` —— 全部原始读值与判定（observed / reproduced / confirmed）。
- `RISKS.md` —— 风险与未决问题。
- `RECOVERY-AND-HANDOFF.md` —— 恢复手册与交接。
- `../e902-fw/` —— **新增/重写的 BSP 代码**（编译通过、未上机运行）：
  `src/spi.c` + `src/spi.h`（CPUS 域 `r_spi @ 0x07092000` 主机驱动，含 CLIC IRQ 38 中断路径）、
  `src/heartbeat.c`（SRAM 心跳块，弥补小核没有串口的观测缺口）、
  `src/uart.c`（新增 S_UART0 RX 中断，CLIC IRQ 29）、
  `src/main.c`（新协议：SPI 读取 / UART 统计 / 心跳读数）、
  `src/fw.ld`（加载址改 `0x40020000`，避免覆盖厂商镜像）、`Makefile`（`make` / `make min2`）。
