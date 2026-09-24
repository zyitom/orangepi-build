# 待刷镜像（scp 槽位 payload）——已就绪并全项校验

生成：2026-09-22 · 由 `cd e902-fw && make scpfw` 一条命令产出

## 产物

| 文件 | 大小 | sha256 |
|---|---|---|
| `e902/fw-out/scp-ours-padded-105912.bin` | **105912** | `94f9d088431a6e2926901d5dd4af0c6d9dbf355b18c28d2ca463c8044b29c6e2` |
| `e902/fw-out/scp-ours-6232.bin` | 6232 | `d21cae10768c6349e2b485286cf6109c241ba73af388989068df32782a35d821` |

105912 = `0x19DB8`，正是启动包 TOC 里 `scp` 条目的 size（见 `FLASH-SPEC.md`）。
补齐方式：先 `dd` 出全 0 的 105912 字节，再把 6232 字节的固件 `conv=notrunc` 写进去。

## 校验结果（`verify_scpfw.sh`，全部通过）

```
=== sizes and hashes ===
  fw-scp.bin         6232
  fw-scp-padded.bin 105912   sha256 94f9d088431a6e2926901d5dd4af0c6d9dbf355b18c28d2ca463c8044b29c6e2

=== sections (必须落在 0x40004000..0x4002F000) ===
  0 .text     00001128  40004000
  1 .vectors  00000180  40005140
  2 .rodata   00000598  400052c0
  3 .data     00000000  40005858
  4 .bss      000000c8  40005858
  5 .stack    00001000  40005920
  __start=0x40004000  trap_entry=0x400040c0  vector_table=0x40005140  main=0x40004bdc
  msgbox_send_startup_feedback=0x400045d4

=== relocations ===
  There are no relocations in this file.

=== 前 16 字节 ===
  待刷镜像 : 81 40 01 41 81 41 01 42 81 42 01 43 81 43 01 44
  厂商 scp.fex: 81 40 01 41 81 41 01 42 81 42 01 43 81 43 01 44   ← 逐字节相同

=== 握手代码在位 ===
  lui 0x3004 (CPUX_MSGBOX) : 2
  store +124 (=0x7C, 通道3) : 1
  709406c / 709407c         : 2
  li a4,64  (1<<6 中断位)    : 2

=== 尾部补齐区 ===
  全 0
```

## 刷写（需现场恢复条件）

```bash
# 1) 把待刷镜像放到板子上
cat e902/fw-out/scp-ours-padded-105912.bin | <ssh_board.sh> 'cat > ~/e902v2/scp-ours-padded.bin'
# 2) 写入（脚本自带尺寸校验 + 读回哈希校验 + 需输入 YES）
printf 'orangepi\n' | sudo -S sh ~/e902v2/flash-scp.sh ~/e902v2/scp-ours-padded.bin /dev/mmcblk1
# 3) 重启，同时盯板子自己的 /dev/ttyUSB0（115200 8N1）看 banner
sudo reboot
```

## 回滚

```bash
printf 'orangepi\n' | sudo -S sh ~/e902v2/restore-scp.sh ~/e902v2/scp.fex /dev/mmcblk1
```
（`scp.fex` = `e902/backup/scp.fex.on-medium.bin`，sha256 `07e6b976…`）

> ⚠️ 若刷入后板子起不来，**回滚只能在另一台机器上用读卡器写回**——这是唯一需要人现场的部分。

## 期望现象（判据）

板子自己的 `/dev/ttyUSB0`（FT232H = E902 控制台）应出现：

```
=== A733 E902 firmware v2 ===
PL_CFG0     = 0x...   (PL2/PL3 mux ok)
...
sent startup feedback (ch3, hdr=0, 13 words)
sent HELLO to ARM
```

同时大核侧读 `0x07032204` 应为 `0x40004000`，SRAM `0x4001E000` 出现 `0xE902C0DE`。



---

## 更新（2026-09-22 T21，以此为准）

- hdr=0 版本（063f4130）已被实测否定：刷入重启后 bl31 拒绝、板子 no-boot（见 FINDINGS-LEDGER T21）。
- 最终固件：banner 更正后重编，fw-scp.bin 6216B（bc3848f7…）、padded 105912B（**8aaf9486…**）。
  握手 = ch3，[0x00900200][13][0×13]，与厂商反汇编逐指令一致（两轮独立复核）。
- 介质状态：SD 卡在读卡器（/dev/sdb）已刷入 8aaf9486…（读回校验通过），24MiB 头 a84c5c63…；
  恢复厂商后头应回到 114cd1f3…。
- 期望串口现象第 4 行同步更正为：`sent startup feedback (ch3 hdr=0x00900200 13 words)`。




## 更新 2（2026-09-22 T22，必须读）

两次"刷入后板子不启动"的真因 = **boot0 对启动包整包 add_sum 校验失败**
（串口实录：bad checksum / bad magic / Loading boot-pkg fail），与 bl31 无关。
修复工具：`e902/tests/fix-bootpkg-sum.py <sdX> --write`——把包头 0x1004014 处的
add_sum 从 0xa91ddf89 改为当前内容的正确值 **0x83f86015**（4 字节）。
自测：厂商备份 PASS / 模拟件修复后 PASS / 槽位内容不变。
**流程变更：今后任何 scp 槽位内容改动后，必须跑一次 fix-bootpkg-sum.py。**


## 更新 3（2026-09-22 T24，以此为准）

- v3（afcc86f4）实测：boot0 过、bl31 载入放复位、E902 运行；但 v3 的"保险"WDT 写
  （0x02050014/0x07021000，厂商从不访问）跨域挂死小核；且 bl31 卡 "wait arisc ready"。
- **bl31 wait_ready 判据（T24 逆向）**：轮询 CPUX ch3，等 data[0] 低字节==0x90 的包；
  feedback 全零数据不满足。需 SCP 对 bl31 查询回显（result=0）。
- **v3.1（当前 fw-out）**：fw-scp.bin 6400B（08e67528…）、padded 105912B（**40b005f0…**）；
  删除 WDT 步骤(3)；ISR 包重组；主循环按厂商规则回显（result=0，flags&3 门控）。
- 预期 add_sum（厂商头+本内容）= **a6e7e22a**；实际以 fix-bootpkg-sum.py 对卡计算为准。
