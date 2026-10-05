# Deep 休眠修复交接（2026-10-01）

给下一个接手的 agent / 工程师。前置阅读：`tina-zero3w/docs/STATUS.md` 第六、七节。

## 现状一句话

两个"未解决"问题在 10-01 03:02 那次测试（`verify-logs/deep-retest-20261001-030125/`）
里都查清了：**"假唤醒"不存在**（是 SCP 时间戳冻结造成的错觉，唤醒其实是 +20 s 的
RTC 闹钟），**内核不恢复的根因是 DRAM 类型错**——板子是 4 GiB **LPDDR5**，SCP 兜底
参数表却是通用 sys_config 的 **LPDDR4**（type 8），恢复后 DRAM 读出全零，CPU0 从
BL31 入口起来无代码可跑。兜底表已换成 boot0 自己用的 LPDDR5 参数集。
上板验证结果见文末「验证」一节。

## 已修复且板上验证 ✅

| 问题 | 修复 | 证据 |
|---|---|---|
| SCP 的 FDT 里 `/dram` 无参数（全零），闭源 dramlib 带零参数 save 即死 | `e902/vendor-scp/patches/0004`（全零时用内置表）；内核 `userpatches/.../0030` 补 dram_para00..31 + standby_param | 板上日志 `WRN:dram para all zero -- using built-in zero3w values` → `dram save done` → `dram up enter/done` → `cpu on` |
| dram_clk 用错（1200 vs boot0 实测 2400） | 表里改 2400 | 恢复日志 `DRAM CLK =2400 MHZ` |
| "进入后 ~0.13 s 假唤醒" | **不是 bug**：SCP 的 tick 来自 S_TIMER 的 SYS_CLK24M（mux 4，patches/0002），在 26 MHz DCXO 的板子上它由 PLL_REF 产生，而 `clk_suspend` 关掉了 PLL_REF——睡眠期间 tick 停走，`[xxxx.xx]` 时间戳完全不反映真实睡了多久 | debug 固件用 RTC 记时：`wake src 196 after 13 s real (rtc 68549 -> 68562), 0 ticks`（196 = RTC 闹钟；0 ticks = tick 冻结）。之前的"同秒去毛刺"放行了唤醒（跨了 RTC 秒）、"5 s 黑窗"按冻结的 tick 计时，把真正的 RTC 唤醒也忽略并屏蔽了→卡死在 wait_wakeup |
| **内核不恢复** | 兜底表（`patches/0004`）和 DTS（`0030`）改为 boot0 的 LPDDR5 参数集，见下 | 旧表恢复后 `dram sum bl31 899B65D3 -> 0 CHANGED, kernel 4FBDBA83 -> 0 CHANGED`，恢复横幅 `DRAM Type =8`；boot0 横幅是 `DRAM Type =9 (8:LPDDR4,9:LPDDR5)`、`DRAM_VCC set to 560 mv`、`vaild para:4  select dram para0` |

### DRAM 参数的来源（以后换板/换内存必须照此重取）

卡上 boot0 = `boot0_sdcard_orangepizero3w.fex`（md5 0ec43908…，与 pack-uboot 里同名文件一致）。
它有 4 组参数（"vaild para:4"）：头部 0x38 起 32 字 = para0，另有 `DRAM.ext` 表里三组
（0x374 起，2400/2040 MHz 变体）。boot0 选 para0；运行时打印的 para1=a0fa、tpr13=0x65
与头部完全一致，只有 para2 被自动扫描填入容量字段：头部 0x1001 → 运行时 0x10001001。
所以表 = boot0 头部 para0 逐字拷贝 + para2=0x10001001。取数方法：

```sh
dd if=<卡头备份 e902/backup/sd-head-*.img> bs=1024 skip=8 count=240 of=boot0.bin
python3 -c "import struct;print([hex(x) for x in struct.unpack_from('<32I',open('boot0.bin','rb').read(),0x38)])"
```

**不要再用** `u-boot/v2018.05-sun60iw2/sys_config.fex` 的 `[dram_para]`——那是通用 LPDDR4/1200 MHz 配置，
不是这块板子的内存。

## 工具改进（10-01）

- `e902/tests/board/deep-suspend-retest.sh` 重写：
  - 串口改用 `e902/tools/serial-cap.py`（每行带主机时间戳）；开始前清理残留读者、退出时一定收尾。
    **10-01 02:10 那次测试的日志是坏的**：01:03 那次测试的脚本卡在 ssh 上一直没退，它的两个
    `cat` 还占着串口，把 02:10 那次的数据分走了一半（`deep-retest-20261001-021022/` 与
    `-010337/` 里的日志互相交错，别再拿来分析）。
  - 挂起脚本 nohup 落盘在板上跑（`/var/tmp/deep-retest.log`），主机轮询，ssh 断开不丢结果；
  - 挂起前 `printk=8`、`console_suspend=N`、`pm_debug_messages`、`pm_print_times`
    （镜像是 `quiet loglevel=3` 启动的，之前所有 PM 打印都被吞了）；
  - 判定项：兜底表、dram save、真实睡眠秒数、DRAM 校验和、BL31 restore notify、内核恢复、
    是否被 RTC 闹钟唤醒（resume 时刻 vs 闹钟时刻）；
  - `SCP_FW=e902/fw-out/vendor-scp.bin` 可测正式版；
  - 恢复失败时自动在 SCP 控制台发 `RST` 重启板子（见下），不用再人工断电。
- `patches-debug/0004` 换成「RTC 记时唤醒报告 + DRAM 校验和 + 恢复心跳 + RST 救援」：
  - `wake src N after S s real (rtc a -> b), T ticks`（clk_resume 之后打印，波特率正常）；
  - save 前/恢复后对 BL31(0x48000000) 和内核代码(0x41000000) 各 64 KiB 求和：`dram sum ... ok/CHANGED`；
  - 启动 CPU0 前打开 A55 的 UART0（115200），BL31/内核早期恢复打印和崩溃信息能出来；
  - `wait ac327 resume` 期间每 5 个 RTC 秒打印心跳（PPU/RVBAR/CPU PLL/msgbox/UART0 LSR）；
  - 这期间在 SCP 控制台输入 `RST` → `pmu_reset()`（正常 reboot 走的同一路径）重启整板。
- `e902/vendor-scp/build.sh`：debug 补丁按逆序回退（新 0004 以 0002 为上下文，正序回退会失败留下脏树）。
- 旧的 RTC_GPR0「last wake record」方案作废：启动链会清 GPR0（每次都读到 0）。

## 固件

| 文件 | sha256 | 内容 |
|---|---|---|
| `e902/fw-out/vendor-scp.bin` | bd6c58e3… | 正式版，LPDDR5 兜底表 |
| `e902/fw-out/vendor-scp-debug.bin` | 9fe80d4c… | 同上 + 调试控制台(S_UART0 PL2/PL3 57600) + probes + 忽略 R-PIO(200) + 上述 0004 |

## 板子与工具现状

- 板子 `172.16.0.193`（DHCP 会变：主机没有 nmap，可用并行 `/dev/tcp/…/22` 扫 172.16.0.0/21），
  Tina 镜像 #65，ssh `ar0234-port/tools/ssh_board.sh`（orangepi/orangepi，sudo 密码同）。
- 串口：`/dev/serial/by-id/usb-FTDI_*`（E902，57600）和 `usb-1a86_*`（A55，115200），
  ttyUSB 编号会随主机重启互换，脚本按 by-id 找。
- RTC 记录：`sudo devmem 0x0709010C`（save_state_flag：0xf3f3XXXX=deep 流程，0x5001=wait_wakeup，
  0x7xxx=exit 步骤；断电清零）。
- 一键测试：`BOARD=<ip> bash e902/tests/board/deep-suspend-retest.sh`（烧 debug 固件+重启+挂起 20 s）。

## 验证（10-01 08:10，`verify-logs/deep-retest-20261001-081013/`，debug 923ec2a9）

| 步骤 | 结果 |
|---|---|
| 兜底表 | ✅ `dram para all zero -- using built-in zero3w values` |
| DRAM save | ✅ `dram save done` |
| 唤醒 | ✅ `wake src 196 after 19 s real`（RTC 闹钟，+20 s 准点） |
| DRAM 恢复 | ✅ `DRAM Type =9`，`dram sum bl31 899B65D3 -> 899B65D3 ok, kernel 4FBDBA83 -> 4FBDBA83 ok` |
| CPU0 上电 | ✅ PPU c0=0x108(ON)，RVBAR=0x48000228，CPU PLL 锁定 |
| BL31 → SCP restore notify | ❌ 没有；msgbox ARM→SCP 全程空，UART0 无输出 |
| RST 救援 | ✅ 发 `RST` 后 20 s 板子回来 |

**剩下的问题（唯一一个）**：DRAM 已完好，CPU0 也被拉起，但 BL31 的 warm boot
（0x48000228 → reset handler → MMU → psci_warmboot_entrypoint）没走到给 SCP 发
restore notify 那一步，也没有任何打印。注意 923ec2a9 那次 UART0 引脚没恢复
（PB9/PB10 function 2 由 boot0 设置、内核不管，VDD_SYS 复位后失效），所以"没打印"
还不能说明 BL31 没跑——**9fe80d4c 已补上引脚复用 + CPU0 运行/WFI 状态
（C0_CPU0_STATUS0 0x08860004：bit12 INSTRRUN、bit8 INSTRRET、bit4 WFI）**，
08:18 那次测试开始后主机两个 USB 串口都掉了（/dev/ttyUSB* 消失），没拿到数据。

下一步（板子断电重启、串口重新插好后）：
1. `BOARD=<ip> bash e902/tests/board/deep-suspend-retest.sh`（debug 9fe80d4c 已在 fw-out，会自动烧）；
2. 看心跳 `hb: c0 ctrl .. st ..`：
   - st 有 bit4（WFI）→ BL31 panic 了，ARM 串口（arm.log）应能看到 crash dump；
   - st 有 bit12/bit8 在变 → BL31 在跑但卡在轮询（看 arm.log 的打印定位）；
   - st 全 0 且 ctrl bit0=0 → 核没出复位，查 C0_RST_CTRL/CPU_SYS_RST；
3. 若 arm.log 能看到内核打印但卡在某个驱动 resume → `pm_print_times` 已开，最后一行就是凶手；
4. 通过后：`SCP_FW=e902/fw-out/vendor-scp.bin` 测正式版（无 R-PIO 过滤），再把
   `external/packages/pack-uboot/sun60iw2/bin/scp.fex` 同步为 vendor-scp.bin（bd6c58e3）。

当前卡上：debug 9fe80d4c（可用，不影响 s2idle）；板子停在 deep 恢复失败状态，需断电。
