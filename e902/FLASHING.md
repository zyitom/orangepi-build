# E902 SCP 固件烧录指南

适用于 Orange Pi Zero 3W（A733）。命令都在 **TL101**（本机）上、仓库根目录下执行。
板子默认地址 `172.16.0.193`，登录和 sudo 口令都是 Orange Pi 镜像默认的 `orangepi`（可以用 `BOARD_PASS=` 覆盖）。

## 0. 必须知道的一件事

SCP 固件放在 SD 卡启动包（`sunxi-package`，偏移 `0x1004000`）的 `scp` 条目里（数据从 `0x113BC00` 开始）。
启动包头里有一个覆盖整个包的 **`add_sum` 校验和**：**改了条目却没重算 `add_sum`，下次上电 boot0 就拒绝启动包，
板子直接进 FEL，串口上什么都没有。**
所有写卡操作都要走 `tools/bootpkg.py`，它会调整条目长度和 `valid_len`、重算 `add_sum` 并读回校验。
**不要**直接用 `dd` 写槽位。

```bash
python3 e902/tools/bootpkg.py info <镜像或设备>     # 只读：看条目和校验和 PASS/FAIL
```

## 1. 编译

默认固件是厂商源码构建（见 `vendor-scp/README.md`）：

```bash
bash e902/vendor-scp/build.sh        # -> e902/fw-out/vendor-scp.bin
```

自研固件（`e902-fw/`，可选）：

```bash
make -C e902/e902-fw scpfw hosttest  # -> e902/e902-fw/build/fw-scp-padded.bin，hosttest 必须 all passed
```

## 2. 在线烧录（板子能正常启动时，推荐）

```bash
bash e902/tools/flash-scp.sh --dry-run          # 检查 + 备份卡头 + 推送，不写
bash e902/tools/flash-scp.sh                    # 默认写 fw-out/vendor-scp.bin
bash e902/tools/flash-scp.sh <其他镜像>         # 例如 e902/e902-fw/build/fw-scp-padded.bin
```

脚本依次做这些事：
1. 核对镜像的 sha256（如果 `fw-out/SHA256SUMS` 里有记录）；
2. 在板上检查启动包，把卡头 24 MiB 备份到 `e902/backup/sd-head-before-flash-<时间>.img`；
3. 写入，校验和 PASS 才继续；
4. 在 TL101 上同时录两路串口（CH340 = 大核，FTDI = 小核），然后重启；
5. 等板子回来，打印串口里和 SCP 相关的行。

录下来的日志在 `e902/verify-logs/flash-<时间>/`。

重启这一步本身由**当前正在运行**的固件完成。如果板子停在关机尾部，断电再上电即可，新固件已经写进卡里了。

## 3. 离线烧录（板子起不来 / 进了 FEL）

把 SD 卡插进 TL101 的读卡器，先用 `lsblk` 确认设备名（59.7G 的可移动盘，通常是 `/dev/sdb`）：

```bash
sudo bash e902/tools/flash-scp-reader.sh /dev/sdb                    # 写 vendor-scp.bin
sudo bash e902/tools/flash-scp-reader.sh /dev/sdb <镜像>             # 写指定镜像
sync; sudo eject /dev/sdb
```

脚本同样会先备份卡头 24 MiB 再写。

## 4. 回滚到出厂 scp

出厂镜像就是本仓库的 `external/packages/pack-uboot/sun60iw2/bin/scp.fex`（sha256 `07e6b976…`，
与卡上原物逐字节相同）：

```bash
bash e902/tools/flash-scp.sh external/packages/pack-uboot/sun60iw2/bin/scp.fex              # 板子能启动
sudo bash e902/tools/flash-scp-reader.sh /dev/sdb external/packages/pack-uboot/sun60iw2/bin/scp.fex  # 起不来
```

最后手段：把某份 `e902/backup/sd-head-before-flash-*.img` 整段写回卡头
（`sudo dd if=<img> of=/dev/sdb bs=1M count=24 conv=fsync`）。只动卡头 24 MiB，不影响 rootfs 分区。

## 5. 刷完怎么验证

- 大核串口日志里 boot0 打出 `[SCP] :load arisc image finish`，u-boot / bl31 没卡在 `wait arisc ready`，Linux 能正常启动。
- 大核串口里 bl31 打出 `[SCP] :arisc version: [<源码提交>-dirty]`、`arisc startup ready`，说明握手完成；
  版本串就是 `vendor-scp` 编进去的上游提交号。
- 小核串口：厂商固件**不输出**（boot0 报 `dtb not found for scp`，没有给它串口配置；出厂固件也一样）。自研固件会打印 `[tick]`。
- 26 MHz 修正是否生效：在板上读 `0x07010100`，应为 `0x00000041`（mux 4 + 使能）。
- 再重启一次、`poweroff` 后上电一次，都应该正常。
