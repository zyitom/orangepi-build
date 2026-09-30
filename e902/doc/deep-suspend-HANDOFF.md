# Deep 休眠修复交接（2026-10-01）

给下一个接手的 agent / 工程师。前置阅读：`tina-zero3w/docs/STATUS.md` 第六、七节。

## 现状一句话

deep 休眠的 SCP 侧全链路已在板上跑通（DRAM 保存→恢复→cpu on），卡在两件事：
**假唤醒**（进入后 ~0.13 s 被不明源唤醒，已排除多个嫌疑但身份未定）和
**内核不恢复**（`wait ac327 resume...` 之后再无下文）。两者可能是同一个问题：
假唤醒让恢复流程与挂起入口竞争。

## 已修复且板上验证 ✅

| 问题 | 修复 | 证据 |
|---|---|---|
| SCP 的 FDT 里 `/dram` 无参数（全零），闭源 dramlib 带零参数 save 即死 | `e902/vendor-scp/patches/0004`（全零时用内置表：sys_config + boot0 实测 dram_clk=2400/para1=0xa0fa/para2=0x10001001/tpr13=0x65）；内核 `userpatches/.../0030` 补 dram_para00..31 + standby_param | 板上日志 `WRN:dram para all zero -- using built-in zero3w values` → `dram save done` → `dram up enter/done`（DRAM banner 参数与表一致）→ `cpu on` |
| dram_clk 用错（1200 vs boot0 实测 2400） | 表里改 2400 | 恢复日志 `DRAM CLK =2400 MHZ` |

注意：恢复时 DRAM 控制器按参数重配——**dram_clk/para1/para2/tpr13 必须与 boot0 实际
初始化用的值一致**，否则起来的 A55 撞上错频 DRAM。bootparam 原始数组可从
`0x4A1FF400`（CONFIG_SYS_TEXT_BASE 0x4A000000 + SUNXI_BOOTPARAM_OFFSET 0x1FF000，
ddr_info 在 +0x400，96 字）核对——前提是 U-Boot 没被覆盖。

## 未解决 ❌

### 1. 假唤醒（进入后 ~0.13 s，5/5 次复现）

- 已排除：USB1 EHCI/OHCI（SPI 159/160，解绑后仍瞬醒）；R-PIO GPIOM（root irq
  200→intno 28，SCP 已拒绝登记仍瞬醒，`patches-debug/0003`）。
- 同秒去毛刺（RTC_HMS 比较）没拦住 → 假唤醒落在参考秒之后，或 HMS 读回不可靠。
- 当前固件 7aa98c86（`patches-debug/0004`）加了 **5 s 硬黑窗**：进入后 5 s 内所有
  唤醒忽略，记入 RTC_GPR0(0xED000000|src)/GPR1(age)，`standby_init` 打印
  `last wake record`。**此版本还没跑过一次完整测试**。
- 假唤醒源候选（按可能性）：
  1. **PMU NMI 线**：AXP8191 中断 → SoC NMI → E907 异常 mcause 24
     （`arch/riscv/cpu/e907/e907_com.c exception_entry` 硬编码）→
     `driver/pmu/pmu-sun60iw2p1/pmu.c platform_nmi_handler` 无条件
     `wakeup_source = 188`。若 PRCM 的 NMI_INT_EN(0x07091324) bit1 被
     BL31/boot0 打开且 PMU 有 pending 事件，必现瞬醒。查法：读 0x07091324。
  2. 时钟切换毛刺（GINTC 在 clk_suspend 总线切换窗口内的伪中断）。
  3. RTC 行毛刺。
- 6 次测试的日志在 `e902/verify-logs/deep-retest-*`（e902.log = SCP 控制台）。
  唤醒发生在 clk_suspend/clk_resume 的 UART 分频切换窗口内，打印全是乱码，
  所以要靠 GPR0 记录而不是串口。

### 2. 内核不恢复（6/6 次）

SCP 完成 `dram up done → cpu on → SMC: non-secure chip → wait ac327 resume...`
后，A55 没有回来（entrypoint 0x48000228 = BL31 的恢复入口）。两种可能的路径：
- (a) 假唤醒竞态：0.13 s 就退出 standby，恢复与入口竞争。**黑窗固件还没验证过
  这种情况是否消失** —— 下一个 agent 的第一件事。
- (b) 若黑窗后"RTC 20 s 准点唤醒"仍不恢复 → BL31 恢复路径问题。对比对象：
  `~/tina5` 的 SDK bl31（我们的 monitor.fex 来自 orangepi 的 deb）。
- 每次失败后板子假死（SCP 卡 wait_cpu0_resume，串口/网络全静默），需要断电重启；
  有时 ~5-8 分钟看门狗**热**复位自恢复（RTC 记录保留），断电则 RTC 清零。

## 板子与工具现状

- 板子在线 `172.16.0.193`（DHCP 会变，扫网段），Tina 镜像 #65，
  ssh `ar0234-port/tools/ssh_board.sh`（orangepi/orangepi，sudo 密码同）。
- 卡上是 debug 固件 7aa98c86（S_UART0=PL2/PL3 57600，FT232=0403:6014 在
  /dev/serial/by-id，功能与正式版一致 + 探针）。恢复正式版：
  `BOARD=<ip> bash e902/tools/flash-scp.sh`（默认 vendor-scp.bin）。
- 一键测试：`BOARD=<ip> bash e902/tests/board/deep-suspend-retest.sh`
  （会烧 debug 固件+重启+挂起 20s RTC 唤醒）。**ssh 在挂起时会断**，结果看
  `e902/verify-logs/deep-retest-*/`（e902.log=SCP 控制台、arm.log=大核串口、
  board.log=板内输出）。挂起脚本建议 nohup 落盘跑（ssh 断连会丢 rc）。
- RTC 记录读取：板上 `sudo devmem 0x07090100`（GPR0）、`0x07090104`（GPR1）、
  `0x0709010C`（save_state_flag：0xf3f3XXXX=deep 流程，0x5001=wait_wakeup，
  0x7xxx=exit 步骤；断电清零）。

## 下一任的第一小时

1. `BOARD=<ip> bash e902/tests/board/deep-suspend-retest.sh`（黑窗固件已烧在卡上）。
2. 看 e902.log：若出现 `ignore early wake`（黑窗生效）且 ~20 s 后 `wake src: 196`
   → 假唤醒被压掉；看内核是否恢复。
3. 若仍不恢复：板上 `sudo devmem 0x07091324 32` 查 NMI 使能位；假唤醒若为 188
   → 在 `platform_nmi_handler` 里不置 wakeup_source（debug 补丁），或内核侧
   suspend 前清 AXP8191 的 pending 中断。
4. 若 RTC 准点唤醒仍不恢复 → 抓大核串口（arm.log）看 BL31/内核 resume 打印到
   哪一步，对比 ~/tina5 的 bl31。

## 其他任务状态（非 deep）

- noble 镜像已构建+镜像内验证（内核 0024–0030、三个 zero3w 服务启用、pvrsrvkm
  正常），SCP 槽位已换修复版：`output/images/Orangepizero3w_1.0.2_ubuntu_noble_*`。
- 文档已更新（ZERO3W.md、tina-zero3w/docs/STATUS.md）。全部提交已推送
  （fork/zero3w，最新 3d0ae44）。
- 待做：WiFi 关省电后 30 分钟空闲观察（`e902/tests/board/wifi-idle-observe.sh`
  已备好）；deep 收尾后把正式版 vendor-scp.bin 烧回。
