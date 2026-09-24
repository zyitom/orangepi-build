# E902 异常恢复手册与交接文档

工程根：`/home/helios/Desktop/orangepi-build` · 构建机 `ssh helios@TL101`
· 板子 `orangepi@172.16.0.193`（登录与 sudo 口令都是 `orangepi`）· 板子调试串口在 TL101 上是 `/dev/ttyUSB0`（115200 8N1）

---

## 0. 一句话现状

**小核（E902）目前无法从 Linux 侧被"借来"跑自己的代码**：软件层没有内核复位位，
只有 bl31 在启动流程中释放复位。详见 `FINDINGS-LEDGER.md`（F4/F5/F6，已实测复现）。
本手册给出：如何安全地做实验、如何回滚、以及唯一可行的"真正让小核跑我们代码"的下一步。

## 1. 安全边界（改动前必读）

| 动作 | 风险 | 是否已具备回滚 |
|---|---|---|
| 往 SRAM_A2 写数据（`awdevmem.py load`） | 低（掉电即失） | ✅ 无需回滚 |
| 改 `RISCV_BGR` / 设 `E902_RST_START_ADDR` | 低（关电重启即恢复；`RST_START` 会被 bl31 重写） | ✅ 重启即可 |
| 改 `userpatches/` 重新编内核 deb | 中（覆盖 `/boot/uImage`，无回退项） | ✅ 有 `board-boot-backup.tgz` |
| **替换 `scp.fex` / 重写启动介质裸扇区** | **高（板子可能起不来，需物理介入）** | ⚠️ 有 `backup/sd-boot-head.img` + `restore-sd-auto.sh`，但当前**不具备物理介入条件** |

**本轮只在"低风险"区内操作，未触碰启动介质。**

## 2. 环境准备（接手者照做）

```bash
# 1) 构建机
ssh helios@TL101           # 免密钥已配好
cd ~/Desktop/orangepi-build/e902/e902-fw

# 2) 工具链（已在位，无需下载）
ls toolchains/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0/bin/riscv64-unknown-elf-gcc
# 或重新获取：./scripts/get-toolchain.sh

# 3) 上板通道（TL101 上）
S=~/Desktop/orangepi-build/ar0234-port/tools/ssh_board.sh
BOARD_PASS=orangepi $S 'uname -a'        # 口令是 orangepi，不是空格
```

### 板子 sudo 的正确用法

```bash
printf 'orangepi\n' | sudo -S <命令>      # 板子
sudo <命令>                            # TL101 本机
```

## 3. 构建（一条命令）

```bash
cd ~/Desktop/orangepi-build/e902/e902-fw
make                 # 出 build/fw.bin（v2 主固件，5896 B）
make min2            # 出 build/fw_min2.bin（768 B 探针，只写 SRAM）
```

记录：工具链 `Xuantie-900 V3.2.0`，`-march=rv32emc_zicsr -mabi=ilp32e`，
源码版本标识 = 本目录内容（非 git 仓；如需可 `git -C ~/Desktop/orangepi-build log -1`）。

## 4. 上板实验（低风险区）

```bash
# 推送到板子（TL101 上执行）
cd ~/Desktop/orangepi-build/e902/e902-fw
S=~/Desktop/orangepi-build/ar0234-port/tools/ssh_board.sh
export BOARD_PASS=orangepi
cat build/fw.bin            | $S 'cat > ~/e902v2/fw.bin'
cat scripts/awdevmem.py     | $S 'cat > ~/e902v2/awdevmem.py'
cat scripts/e902-load.sh    | $S 'cat > ~/e902v2/e902-load.sh'
cat scripts/e902-restore.sh | $S 'cat > ~/e902v2/e902-restore.sh'
cat ../../u-boot/v2018.05-sun60iw2/scp.fex | $S 'cat > ~/e902v2/scp.fex'
$S 'chmod +x ~/e902v2/*.sh'

# 板子上：基线 → 加载 → 读心跳 → 回滚
$S 'printf "orangepi\n" | sudo -S sh ~/e902v2/e902-load.sh ~/e902v2/fw.bin'
$S 'printf "orangepi\n" | sudo -S python3 ~/e902v2/awdevmem.py dump --e902 0x4001E000 --count 16'
$S 'printf "orangepi\n" | sudo -S sh ~/e902v2/e902-restore.sh ~/e902v2/scp.fex'
```

心跳块 `0x4001E000`（E902 视角）字段：
`[0]=magic(0xE902C0DE) [1]=seq [2]=stage [3]=PL_CFG0 [4]=APBS1 [5]=RISCV_BGR [6]=RST_START
[7]=spi_init结果 [8]=spi_tc次数 [9]=spi_rx长度 [10]=spi_irq次数 [11]=uart_rx字节 [12]=mbox_irq次数
[13]=mbox_rx条数 [14]=timer ticks [15][16]=SPI RX 前 8 字节`

## 5. 异常场景与恢复

### 5.1 小核"挂死"（我们的固件跑飞）
现象：心跳 `seq` 不再增长。
定位：读 `last_mcause` / `last_mepc`（`trap_entry` 会存）：
```bash
# TL101 上查符号地址
riscv64-unknown-elf-nm build/fw.elf | grep last_
# 板子上读（SRAM 必须走 --e902）
printf 'orangepi\n' | sudo -S python3 ~/e902v2/awdevmem.py dump --e902 <符号地址> --count 8
```
`mcause=0x2` 非法指令（用了 RV32E 没有的指令/寄存器）、`0x5/0x7` 访问错误（多半忘了开该外设时钟）。
恢复：`e902-restore.sh scp.fex`，或直接断电重启。

### 5.2 小核时钟被关、寄存器写不进去
现象：写 `0x07032204` 读回没变。
原因：`RISCV_BGR` 被写成 `0x00000000` 会把 `RISCV_CFG` 寄存器块一起复位/门控（DESIGN-NOTES §6 实测）。
恢复：先写 `RISCV_BGR = 0x00010002`（cfg 在、内核时钟关），再写 `RST_START`，最后 `0x00010003`。

### 5.3 通信超时（大核发了、小核不回）
现象：`arm-msgbox-test` 发出后 4 s 无回应。
判定顺序：① 心跳 `seq` 是否在涨（不涨＝小核没跑，见 5.1）；② `mbox_irq` 是否在涨
（不涨但 mbox_rx 在涨＝中断号配错）；③ 大核 `ls /sys/bus/platform/drivers | grep msgbox`。
恢复：小核侧 `msgbox_ack_irq()` 清 pending；大核侧确认 `CONFIG_AW_MSGBOX=y`。

### 5.4 启动失败（改过 boot 介质后起不来）
现象：串口无输出 / 停在 u-boot。
恢复（需物理介入）：
1. 断电，插入写有 `e902/backup/sd-boot-head.img` 的卡，用 `e902/restore-sd-auto.sh` 自动写回；
2. 或用 `e902/backup/board-boot-backup.tgz` 还原 `/boot` 与 `/lib/modules`；
3. 最坏情况：读卡器 + `dd if=sd-boot-head.img of=/dev/sdX` 重写引导头。

### 5.5 资源冲突（大核抢了引脚/时钟）
现象：`PL_CFG0` 读回与我们写的不符（banner/heartbeat 里的 `MISMATCH`）。
处理：只在初始化时写一次、只写 PL2/PL3 两个 nibble、等 Linux 启动完成后再加载固件；
`PL0/PL1` 是 PMIC 的 I2C，任何情况下不要动。

## 6. 回滚（已验证）

```bash
printf 'orangepi\n' | sudo -S sh ~/e902v2/e902-restore.sh ~/e902v2/scp.fex
```
实测输出（2026-09-22，见 LEDGER T8）：`verify ok (105912 bytes read back identical)`，
`RST_START_ADDR` 复位 `0x40004000`。**已实际演练通过。**

要彻底干净：`sudo reboot`（让 bl31 按正常流程重新载入 `scp.fex` 并传参数块）。

## 7. 已知限制与"真正跑起来"的下一步

### 7.1 为什么现在跑不起来
手册 5.2.3 的 `E902_CFG` 只有 8 个寄存器，**没有内核复位**；手册 4.2.5.24 的 `RISCV_BGR`
只有 `RISCV_CFG_RST`（复位 CFG 寄存器块，不是内核）；`RST_START_ADDR` 的定义是
"复位释放时的 PC"，而复位由启动流程（bl31）释放。实测 9 种寄存器序列均无法让内核重新取指。

### 7.2 唯一可行路径：让 bl31 载入我们的固件
bl31 在启动时把 `scp.fex` 拷到 `0x40004000`、设 `RST_START_ADDR`、释放复位，然后**等待小核握手**。
所以：

1. 把我们的固件做成能被 bl31 接受的镜像（`e902/e902-fw-scp/` 已有一份为此准备的副本，
   `e902/apply-scp.sh`、`e902/scp-ours.bin`）；
2. **实现握手**——这是之前失败的原因（旧笔记：换 `scp.fex` 后"bl31 挂死、板子起不来（无握手）"）；
3. 用 `userpatches/u-boot/.../0001-...patch` 那条路线或直接替换 boot 介质里的 `scp.fex` 段；
4. 刷写前必须有可用的 SD 恢复手段，且**需要现场能插卡**。

代价：自己固件接管后，DRAM 变频、suspend、PMIC 看护全丢（全是软失败，见 `RISCV-IMPACT`）。

### 7.3 建议的推进顺序（每步独立可回滚）
1. 先做**只读**：把小核串口（PL2/PL3）再接一个 USB-TTL，这样任何后续实验都能直接看到 banner——
   这是当前最大的观测短板。
2. 研究 bl31 与小核的握手协议（从 `scp.fex` 反汇编 + `arisc_i.h` 的 `dts_cfg_64` 入手），
   在 `e902-fw-scp/` 里实现最小握手。
3. 在**有物理恢复条件**时，做一次 `scp.fex` 替换实验，并当场验证回滚。

## 8. 交接清单

- [x] 资源归属表：`doc/e902/RESOURCE-MAP.md`
- [x] 实测记录与结论状态：`doc/e902/FINDINGS-LEDGER.md`
- [x] 风险与未决问题：`doc/e902/RISKS.md`
- [x] BSP 源码：`e902-fw/src/{spi.c,spi.h,heartbeat.c,uart.c,msgbox.c,main.c,a733.h,fw.h,fw.ld}`
- [x] 构建：`e902-fw/Makefile`（`make` / `make min2`）
- [x] 加载/回滚脚本：`e902-fw/scripts/{e902-load.sh,e902-restore.sh,awdevmem.py,arm-msgbox-test.c}`
- [x] 本手册：`doc/e902/RECOVERY-AND-HANDOFF.md`
- [ ] 小核串口物理接入（PL2=pin16 TX，PL3=pin18 RX，GND）— 待现场
- [ ] bl31 握手实现 + `scp.fex` 替换实验 — 待现场可恢复条件
