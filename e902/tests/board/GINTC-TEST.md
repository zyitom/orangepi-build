# GINTC 闭环验证手册（T38 → 待上板执行）

> **2026-09-29 更正（台账 T39）**：本文用的基址 0x07090000 是 **RTC**，不是 GINTC；下面的结果都是
> 读写 RTC/RTC-CCU 寄存器得到的，作废。手册 12.1.4 的实例是 `RV_INTERRUPT_CTRL = 0x02056000`
> （`INTC_CONFIG_REG0..7` 在 0x10–0x2C，每位一个中断）。`e902-fw/src/gintc.c` 已改用新基址，
> "每输入寄存器"写操作默认编译掉；新基址上的实验尚未做。


目的：证实"基址 0x07090000 的 GINTC 能把大核 GIC 中断路由给 E902"。
测试源 = **GPADC（E902 输入 70 = GIC id 70 = SPI 38）**：在 Linux 上
`modprobe sunxi_gpadc` 就有 ~4 kHz 中断风暴，无需任何接线。

固件已备好：`e902/e902-fw/build/fw-scp-padded.bin`（v2 sha256 6a175a4a…,含 gintc.c 多输入计数矩阵）。
按键：**G** = GPADC(70) 配置值扫描；**U** = UART0 RX(34) 边沿计数（在 ttyS0 上打字）；**L** = LRADC(69) 边沿计数；**I** = 打印各输入计数；**g** = 全部关闭。
原理：每个 E902 输入 N 的配置寄存器在 0x07090000 + 4N（手册 12.1 章 +
ch.2 地址映射，T38）；配置位语义未文档化，用扫描法在硬件上找出有效值。

## 步骤（板子回来后 ~5 分钟）

```bash
cd /home/helios/Desktop/orangepi-build

# 1. 刷自研固件（在线，自动备份卡头 + 重算 add_sum + 重启）
bash e902/tools/flash-scp.sh e902/e902-fw/build/fw-scp-padded.bin

# 2. 等 ~40 s 板子回线，开 E902 串口监听（另一个终端）
bash e902/tools/capture-e902-console.sh

# 3. Linux 侧装回 GPADC 风暴源
ar0234-port/tools/ssh_board.sh -s "modprobe sunxi_gpadc"

# 4. 在 E902 串口控制台按 'G' —— 固件扫描 24 个候选配置值，
#    每个 0.3 s 窗口统计中断命中数，自动留下命中的那个

# 5. 看输出：
#    "cfg=XXXXXXXX hits=~1200"  → 该值把 GPADC 路由进来了（闭环！）
#    全部 hits=0                → 该输入不通，换输入号重试（改 gintc.c 的
#                                 GINTC_TEST_INPUT，比如 LRADC=69、UART0=34）

# 6. 恢复厂商固件（交付状态）
bash e902/tools/flash-scp.sh
```

## 判读

- **命中 ~1200/0.3 s（≈4 kHz）**：GINTC 闭环证实，把命中的 cfg 值 + 输入号
  记入台账 T38，即完成"大核中断可路由给 E902"的验证。
- 扫描期间 mailbox 每 0.3 s 窗口内有轮询，bl31 握手不受影响（T29 不变量保持）。
- 风险与回滚：任一环节卡死 → 断电重启；固件槽位改写由 bootpkg.py 自动重算
  add_sum，最坏用 `e902/tools/flash-scp-reader.sh` 读卡器恢复（卡头备份自动生成）。

## 后续（GINTC 闭环之后）

- 换输入号验证 2–3 个不同外设（如 LRADC=69、UART0=34），确认"输入号=GIC id"
  的映射规律普遍成立；
- 在 e902-fw 里实现"接管"演示：E902 独占处理某外设中断、大核休眠时工作
  （与休眠修复联动：PEK/GPADC 由 E902 处理后唤醒大核）。
