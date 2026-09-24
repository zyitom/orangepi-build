# 启动介质布局与精确刷写/回滚规格

日期：2026-09-22 · 全部数据来自对**板子启动介质前 24 MiB 的只读 dump**
（`e902/backup/sd-boot-head-20260922.img`，sha256 `114cd1f3a792b346acd87c8a550d4b3a09b5bfa7cce7cf0c406fa602b7bd4a54`）

---

## 1. 启动介质识别

| 项 | 值 | 依据 |
|---|---|---|
| 启动设备 | `/dev/mmcblk1`（59.7 G，根分区 `mmcblk1p1`） | 板上 `lsblk`；无 `mmcblk0`；无 MTD |
| boot0 | 偏移 `0x2000`（sector 16），magic `eGON.BT0` | `od -j 8192`：`be 02 00 ea 65 47 4f 4e 2e 42 54 30` |
| 启动包容器 | 偏移 **`0x1004000`**，magic `sunxi-package` | dump 内字符串定位 |

## 2. 启动包 TOC（已完整解出）

容器头（`0x1004000`）：`"sunxi-package"` + 条目数 `3` + 总大小 `0x154000`。

| # | 条目标记 | 名称 | 相对偏移 | 大小 | 绝对地址 | 条目内 type |
|---|---|---|---|---|---|---|
| 0 | `MIE;` | `u-boot` | `0x800` | `0x124000` | `0x1004800` .. `0x1128800` | 3 |
| 1 | `IIE;` | `monitor`（bl31） | `0x124800` | `0x13311` | `0x1128800` .. `0x113BB11` | 3 |
| 2 | `IIE;` | **`scp`** | `0x137C00` | **`0x19DB8`** (=105912) | **`0x113BC00` .. `0x11559B8`** | 3 |

**交叉验证**：独立搜索 dump 里 `81 40 01 41 …` 入口序列，得到 **`0x113BC00`**，与表里 `0x1004000 + 0x137C00` 完全一致；
且该处 105912 字节的 sha256 = `07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749`
= 工程里 `u-boot/v2018.05-sun60iw2/scp.fex` 的 sha256。**位置与内容双重确认。**

条目字段为 `(offset, size, 0, type=3, 0)`；**未发现非零的条目校验和字段**（另注：这是"未发现"，非"确认没有"）。

## 3. 精确刷写规格

我们的固件 `fw.bin` 只有 6232 字节，而条目大小是固定的 `0x19DB8`。因此：

1. 把固件**补齐到恰好 105912 字节**（`0x00` 填充）——**大小必须一致**，否则 TOC 与后续条目偏移全错。
   > ⚠️ 注意：我们的固件入口在 `0x40020000`，若由 bl31 在 `0x40004000` 载入，需要改回
   > 让入口等于 bl31 设置的 `RST_START_ADDR`。**这一点必须在刷写前决定并写进固件**
   > （见 `OPTIONS-TO-START-E902.md` 与 `BL31-PATH-RUNBOOK.md` §4）。
2. **外科式写入（推荐）**：只写这 105912 字节。
   ```bash
   # 板子上，root
   dd if=scp-ours-padded.bin of=/dev/mmcblk1 bs=1 seek=$((0x113BC00)) count=105912 conv=notrunc
   # 读回校验
   dd if=/dev/mmcblk1 bs=1 skip=$((0x113BC00)) count=105912 status=none | sha256sum
   ```
   等价脚本：`e902-fw/scripts/flash-scp.sh`。
3. 备选：用 `dragonsecboot -pack boot_package.cfg` 重新生成整个启动包再整包写入 `0x1004000`。

## 4. 回滚（已具备经校验的产物）

| 产物 | 路径 | sha256 | 用途 |
|---|---|---|---|
| 24 MiB 引导头全量 | `e902/backup/sd-boot-head-20260922.img` | `114cd1f3a792b346acd87c8a550d4b3a09b5bfa7cce7cf0c406fa602b7bd4a54` | 整块还原引导区 |
| `scp` 那 105912 字节 | `e902/backup/scp.fex.on-medium.bin` | `07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749` | 只回退 scp 条目 |

```bash
dd if=scp.fex.on-medium.bin of=/dev/mmcblk1 bs=1 seek=$((0x113BC00)) count=105912 conv=notrunc
```
等价脚本：`e902-fw/scripts/restore-scp.sh`。

> ⚠️ **前提**：以上回滚都要求板子**还能启动到 Linux**。如果刷入的 scp 让 bl31 卡住，
> 板子起不来 ⇒ 只能**用读卡器在另一台机器上**写回（这就是需要现场条件的原因）。

## 5. 旧备份的问题（如实记录）

`e902/backup/sd-boot-head.img`（2026-09-22 00:18，sha256 `665cd428…`）与**当前介质的实际内容不一致**
（当前 `114cd1f3…`）。原因未查明（可能是其后又刷过镜像/换过卡）。**在任何刷写前请以
`sd-boot-head-20260922.img` 为准**（本次现场读取、板子侧与 TL101 侧哈希双向一致）。
