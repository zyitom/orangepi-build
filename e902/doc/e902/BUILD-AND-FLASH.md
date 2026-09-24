# 构建 / 烧录 / 回退 —— 完整步骤

日期：2026-09-22 · 工程根 `/home/helios/Desktop/orangepi-build`

---

## 0. 现在的状况（一句话）

板子因为我写入了**握手头字错误**的 E902 固件，bl31 卡住、起不来。**已修好并做好镜像，烧一次即可恢复（并顺带验证修正）。**

## 1. 现成的镜像（不用重新构建）

目录：`output/images/Orangepizero3w_1.0.0_debian_bookworm_minimal_linux6.6.98/`

| 文件 | scp 槽内容 | 用途 |
|---|---|---|
| `...minimal_linux6.6.98.img` | 厂商（`07e6b976…`） | **纯恢复**：烧它 → 板子必然能起来 |
| `...minimal_linux6.6.98-e902fw.img` | 我们的修正固件（`fb1d809d…`） | **恢复 + 验证**：烧它 → 若小核接受握手则板子正常启动且 E902 跑我们的代码 |

- `-e902fw.img` 的 sha256：`82cf7fd9e46bc55ded3cf2dc495a6e82178e48a631cea3955fb1853f4e47aff9`
- 原图 sha256：`3b47847a78ecca9fd3f4d36edc967e601e3f0f2a7e0b5b73775a1e8014b4ec49`
- 两张图**除 scp 那 105912 字节外完全相同**（已验证：前 `0x113BC00` 字节哈希一致 `9bcf95a9…`）

**建议**：先烧 `-e902fw.img`。若板子正常起来 → 目标达成；若还是起不来 → 再烧原版 `.img` 恢复，我们换思路。

## 2. 从零重新构建（一条命令）

```bash
cd /home/helios/Desktop/orangepi-build
sudo ./build.sh userpatches/config-a733.conf
```

`userpatches/config-a733.conf` 的内容（已存在，是上一位同事配好的）：

```
BOARD="orangepizero3w"
BRANCH="current"          # 6.6.x BSP 内核
RELEASE="bookworm"
INSTALL_HEADERS="yes"
BUILD_OPT="image"         # u-boot + kernel + rootfs + 组装 .img
BUILD_MINIMAL="yes"
COMPRESS_OUTPUTIMAGE="sha,img"
DOWNLOAD_MIRROR="china"
```

产物：`output/images/Orangepizero3w_1.0.0_debian_bookworm_minimal_linux6.6.98/*.img`

> **注意**：默认构建会把**厂商 `scp.fex`** 打进启动包——也就是 E902 又回到"调不动"。
> 想让构建直接带我们的固件：先执行 `bash e902/install-scp-into-build.sh`（它会备份厂商文件并替换
> `u-boot/v2018.05-sun60iw2/scp.fex`），再 build。回退用 `bash e902/restore-scp-in-build.sh`。
> **但在我们确认修正后的握手真被 bl31 接受之前，不要固化进构建。**

## 3. 烧录

- 用你的烧录器把 `-e902fw.img`（或原版 `.img`）写入 SD。
- 或者**把 SD 插到 TL101**，我直接用 `dd` 写（2.9 GB，约几分钟），你再说一声即可。

## 4. 起来之后怎么验证小核在跑我们的代码

小核串口现在接在 **TL101 的 `/dev/ttyUSB1`**（FT232H，`0403:6014`；`ttyUSB0` 是 CH340 大核控制台，别搞混）：

```bash
# TL101 上
sudo stty -F /dev/ttyUSB1 115200 cs8 -cstopb -parenb -crtscts raw -echo
sudo cat /dev/ttyUSB1
```

**期望看到**：

```
=== A733 E902 firmware v2 ===
PL_CFG0     = 0x...  (PL2/PL3 mux ok)
...
startup feedback (ch3 hdr=0 13w) : sent
sent HELLO to ARM
[tick] 1
[tick] 2
...
```

也可以从板子侧读 SRAM 心跳（间接但更硬）：

```bash
<ssh_board.sh> 'printf "orangepi\n" | sudo -S python3 ~/e902v2/awdevmem.py dump --e902 0x4001E000 --count 16'
# 期望 +0x00 = 0xE902C0DE，+0x01 持续自增
```

## 5. 回退

| 场景 | 做法 |
|---|---|
| 板子能起来 | `bash e902/go-revert.sh`（写回厂商 scp + 重启） |
| 板子起不来、SD 可拔 | 把 SD 插到 TL101 → `sudo bash e902/recover-sd.sh`（自动识别 + 写回 + 校验） |
| 有烧录器 | 直接烧原版 `...minimal_linux6.6.98.img` |

## 6. 本次失败的原因（已修正）

厂商启动反馈包的**头字不是 0**。反汇编 `scp.fex`：

```
4000b1d6  sb a5(=2),   1(sp)      pkt[1]    = 2
4000b1de  sh a5(=0x90),2(sp)      pkt[2..3] = 0x0090
4000b1f2  sb a5(=13),  4(sp)      pkt[4]    = 13
发送：lw a4,0(pkt) → 邮箱       头字 = 小端 [00 02 90 00] = 0x00900200
```

第一次我发的是 `hdr = 0` → bl31 不认 → 卡住。现已改为 `0x00900200`
（`e902-fw/src/msgbox.c` 的 `MBOX_FEEDBACK_HDR`），重建后的镜像哈希见上表。
