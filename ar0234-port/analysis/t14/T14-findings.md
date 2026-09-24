# T14 复核：单 ISP 双路输出（2026-09-16 第二轮实测）

**结论一句话**：`patches/0005` 已经装到板上并生效（模块 srcversion `E5BC56FB49738345BEFA770`），
但它**没有**让双路输出跑起来 —— 第二个 vinc 不会 panic 了，可是第二路**永远收不到帧**（0 帧），
而且按「两个 S_FMT 都先做」的顺序还会把第一路搞坏。用户说的「卡死」（内核 Oops + 整机冻结）
在本轮**两种模块下都没能复现**（见下）。所以 0005 是「必要条件、未经证实充分」，双路输出的真正
阻塞点在别处。

---

## 1. 现场实测：板子与文档不一致

| 项 | docs/HANDOFF.md 说 | 实测（本轮） |
|---|---|---|
| `/lib/modules/…/updates/vin_v4l2.ko` | 修复「待验证」，DTB 里第二路已禁用 | md5 `f263f4d30ff43608ab854f825d99ac25` = **修复版**，且 `/sys/module/vin_v4l2/srcversion` = `E5BC56FB49738345BEFA770` ⇒ **加载的就是修复版** |
| DTB `vinc@5831000`（label `vinc10`, device_id 4 → `/dev/video4`） | status disabled（稳定态） | `status = "okay"`，`vinc4_isp_tx_ch = <1>` ⇒ **T14 的部署步骤 ①②已经做完** |
| 运行中的进程 | — | **上一轮的双流测试进程还挂在那里 7.8 小时**：两个 `v4l2-ctl --stream-mmap --stream-count=20` 卡在 `vb2_core_dqbuf`（D 状态），一个 libisp 子进程烧了 169 min CPU。两者都占着 `/dev/video0` / `/dev/video4` |

也就是说：**上一轮 agent 在提交 `34dcf75` 之后又部署并重启了（提交时间 04:01，DTB 改动 20:26 UTC，
重启 20:28 UTC），测试没有跑完就结束了**，HANDOFF 停留在部署前的说法。
→ 教训：接手先看 `md5sum` / `srcversion` / `/proc/uptime`，不要只信文档。

## 2. 拓扑（实测确认）

`/dev/media0` + `/proc/device-tree` + aliases：

```
sensor0 (ar0234_mipi, i2c 0x20)
  └─ sunxi_mipi.0 ─ sunxi_csi.0 ─ sunxi_tdm_rx.0 ─ sunxi_isp.0
        pad2 ─┬─ sunxi_scaler.0 ─ vin_cap.0 ─ /dev/video0   (vinc@5830000, device_id 0, tx_ch 0)
              └─ sunxi_scaler.4 ─ vin_cap.4 ─ /dev/video4   (vinc@5831000, label vinc10, device_id 4,
                                                             vinc4_csi_sel=0 mipi_sel=0 isp_sel=0
                                                             tdm_rx_sel=0 isp_tx_ch=1)
```

- **本任务的「双路」= 一个 ISP、两个输出通道（T14）**，不是 T9b 的两个物理相机。
  `sunxi_mipi.1/2`、`sunxi_csi.1/2`、`isp1`、`tdm_rx1..3` 全都没链接（0 link）。
- `vinc4_isp_tx_ch = <1>`：`top_reg.c: vipp_input[8][4][4]`（本 SoC 走 `#else` 全等表）把
  `(isp0, ch1)` 映射到 VI-PP 输入 1，即 `csic_vipp_input_select()` 写 1。设计上确实支持一个 ISP
  的 4 个输出通道各接一个 VIPP。

## 3. 单路：**必须限帧率**，否则看起来「全坏」

| 测试 | 结果 |
|---|---|
| `video0` 1920x1080 NV12，sensor 默认帧率（133fps, FLL=1096） | ❌ 8 s 只有 7 帧，`isp0 frame lost` 505 次 + `sunxi_isp_reset:isp0 reset!!!,ISP frame number is 0` 505 次 |
| `video0` 1920x1080 NV12，`frame_rate=30` | ✅ 120/120 帧（8 s 235 帧 ≈ 29.4fps），0 error |
| `video0` 1920x1080 NV12，`frame_rate=60` | ✅ 463 帧/8 s ≈ 58fps，0 error |
| `video4` 640x400 单独开（`frame_rate=30`） | ❌ **0 帧**（8 s，无任何内核报错，sensor 走 960x600） |

133fps 失败和 README 里「1920x1200@120 消隐只剩 16 行，3DNR 每帧丢帧」是同一类问题：
1080p 默认 133fps 的消隐同样只有 16 行（FLL 1096 = 1080+16）。**开 ISP 一定先限帧率**。

## 4. 双路：两种顺序、两种模块，全都不出帧

模块 A/B 身份用反汇编确认（`__vin_s_input` 里的调用顺序）：

```
vin_v4l2.ko      (md5 512ec92e…) : __vin_sensor_setup_link → __csi_isp_setup_link → sunxi_isp_sensor_type → mutex_lock(管线 open) → …
vin_v4l2-fix.ko  (md5 f263f4d3…) : __vin_sensor_setup_link → __csi_isp_setup_link → mutex_lock(管线 open) → … → sunxi_isp_sensor_type
```

### 顺序 X：两个 `--set-fmt-video` 都先做，再各自出流（= 上一轮脚本的顺序）
- `video4` 的 S_FMT 走 `vin_pipeline_try_format(vinc, set=true)`，会把整条**共享上行**（isp/tdm/csi/mipi/sensor）
  的 ACTIVE format 一起改掉 → sensor 从 1920x1080 变成 **960x600**，而 `scaler.0` 还是 1920x1080。
- 结果：`vinc0 input pixel in one line is less than expected` / `vinc0 input lines in one frame is
  less than execpted, frame lost!` 无限刷（就是我在板上接手时看到的 7.8 小时死状态），两个
  `v4l2-ctl` 都卡死在 DQBUF。
- 这个顺序下还出过一次 IOMMU 空指针 DMA：
  `0x0x0000000000000000 is not mapped!` / `Bug is in CSI module, invalid address: 0x0, data:0x0, id:0x2`
  → `WARNING: CPU:0 PID:0 at bsp/drivers/iommu/sunxi-iommu-v2.c:405 sunxi_iommu_irq+0x5bc/0x5c4`。

### 顺序 Y：`v0 S_FMT → v0 出流 → (4 s) → v4 S_FMT → v4 出流`（T14 想要的顺序）
| 模块 | video0 | video4 | Oops | IOMMU fault |
|---|---|---|---|---|
| fix（E5BC56…） | ✅ 120/120 帧 | ❌ **0 帧**（15 s 超时） | 0 | 0（这一轮） |
| pre-fix（6285A5…） | ✅ 120/120 帧 | ❌ **0 帧**（15 s 超时） | 0 | 0 |

这个顺序下**管线配置是对的**（media-ctl 实测）：

```
ar0234_mipi   pad0  SGRBG10/1920x1080
sunxi_isp.0   pad0/1/2 1920x1080
sunxi_scaler.0 pad0 sink 1920x1080 → pad1 source 1920x1080 → vin_video0
sunxi_scaler.4 pad0 sink 1920x1080 → pad1 source 640x400    → vin_video4
```

`sensor` 窗口保持 1080p、`scaler.4` 做 1080p→640x400 缩放，全链路 link 都 `[ENABLED]`，
内核却**一条报错都没有**（除了 `scaler get_selection error` ×6 这种每次都有的噪声），
`video4` 就是不出帧、DQBUF 永不返回。**所以瓶颈在 ISP 第二个输出通道 / VIPP1 的实际使能，
不在绑定和格式协商。**

### 关于「卡死 = Oops」：本轮没能复现
- 试过的触发组合（都用 pre-fix 模块、串口全程抓日志、`panic_on_oops=1`、console loglevel 8）：
  1. `video0`/`video4` 两个 S_FMT 都先做 → 再各自出流；
  2. `video0` 出流 4 s 后 `video4` 单独 `--set-fmt-video`（正是 `vidioc_s_fmt_vid_cap_mplane` 路径）；
  3. 上面两种各自跟一个 30 s 的双流出流窗口。
- **一次 Oops 都没有**，串口只有干净的重启（`systemd-shutdown[1]: Rebooting.`）。
- 读代码的解释：`cap->pipe.sd[]` 由 `vin_md_prepare_pipeline()`（`vin.c`，由 `pipeline_ops->open` →
  `__vin_pipeline_open` 调用）填充，而 `__csi_isp_setup_link()` 里那个
  `if (csi_dev->stream_count >= 1) return 0;` 只是**跳过 link 再使能一次**，不负责填 `pipe.sd[]`；
  并且第一路已经把 `csi0→tdm_rx0` 等 link 设成 `[ENABLED]` 了，而
  `media_pad_remote_pad_first()` 只走 enabled link，所以第二路 walk 依然能找到 ISP。
  ⇒ 0005 描述的那条「第二路 `pipe.sd[VIN_IND_ISP]` 一定为 NULL」的链条，在本板当前 DTB/顺序下
    **我没能构造出来**。要么原始 panic 需要另外一个前提（例如 K5.15 以下的 `csi->entity.stream_count`
    分支、或当时 DTB/内核不同），要么当时触发它的序列跟文档记的不一样。
  **这条要请用户确认**：原始 Oops 的复现步骤（哪个命令、哪个 DTB、module md5）有没有留档。

## 5. 下一步该查什么（按性价比排序）

1. **ISP 第二个输出通道到底有没有被使能**。`sunxi_isp_reset()`（`vin-isp/sunxi_isp.c:1550+`）会遍历
   所有 `isp_sel == isp->id` 的 vinc 并 `vipp_*_enable(vinc->vipp_sel)`，而 `vinc->vipp_sel = pdev->id`
   = **4**（vinc10 的 device_id 是 4，VIPP 物理实例看起来只有 `scaler.0/.4/.8/.16` 这一组
   虚拟编号）。`vin.c:1458` 用 `vipp_virtual_find_sel[vinc->vipp_sel]` 做虚拟→物理映射，
   但 ISP/scaler 路径里大量 `vipp_*(vinc->vipp_sel)` 是**直接用 vipp_sel**。
   怀疑点：vinc10 的 `vipp_sel=4` 没有被正确映射到物理 VIPP1，导致第二路 DMA 从未启动。
   建议用 `analysis/ispreg_spy.c` 或直接 dump `CSIC_VIPP0_IN_REG_OFF` / VIPP 使能位对比两路。
2. **先做最小验证**：把第二路换到 `vinc01`/`vinc02`（device_id 1/2，`vinc1_isp_sel=1`）不行——它们
   指向 isp1（不存在）。更现实的验证是**固定 `vinc4` 的 vipp 映射**或直接给 `vinc10` 设
   `device_id`/`vipp_sel` 让 `vipp_input[]` 落到物理 1。这需要改 `vin_core.c` 外部模块 + DTS 试探，
   建议做成一次性的实验模块，别直接改 0005。
3. 顺序问题：**双路必须在第一路出流之后再 S_FMT 第二路**（否则共享上行被改坏）；这条即使将来
   双路能跑通也要写进文档/用户态（相当于给 vinculum 加一个「先起主路、再起副路」的约束）。
4. 校验 `vinc4_isp_tx_ch=1` 是否真的被驱动读到：`vin_core.c:2163`
   `sprintf(property_name, "vinc%d_isp_tx_ch", pdev->id)`。当前 DTB 属性名是 `vinc4_isp_tx_ch` 而
   label 是 `vinc10`，需要确认 `pdev->id` 确实是 4（把 `VIN_LOG_VIDEO` 打开看 `isp_tx_ch = %d` 日志）。

## 6. 本轮改动清单（都可回退）

板上：
- `analysis/scripts/boardtest.sh`、`analysis/scripts/dualtest.sh`、`tools/serial_log.py`（新增，主机侧/板上测试工具）
- `/lib/modules/6.6.98-sun60iw2/updates/` 增加：`vin_v4l2.ko.fix-t14`（= f263…，修复版）、
  `vin_v4l2.ko.orig-bsp`（= 512ec…，pre-fix，和已有的 `.bak-pre-fix` 内容相同）。
  **`vin_v4l2.ko` 当前 = fix-t14**（和接手时一致）。
- `/boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb.t14-vinc4on-txch1`（现在的 DTB 副本）
- `/etc/systemd/system.conf`：`RuntimeWatchdogSec=16s`（**新增**，见下）
- 杀掉了上一轮遗留的两个卡死 `v4l2-ctl` 和 libisp 子进程
- `analysis/t14/*.dmesg`、`analysis/dmesg-boot-dual-*.txt`：原始 dmesg 证据

**看门狗（重要）**：`sunxi-wdt` 绑定在 `2050000.watchdog`，实测 open `/dev/watchdog` 后不喂狗
**16 s 整机复位**（`wdt_reset_val` 触发 SoC reset）。开启 `RuntimeWatchdogSec=16s` 后，
panic / 硬挂都能自动重启，不必手动断电。回退：把该值改回 `0` 再 `systemctl daemon-reexec`。

## 7. 复现命令

```bash
# 单路基线（必须限帧率）
tools/ssh_board.sh -s "sh -c '
  v4l2-ctl -d /dev/v4l-subdev0 -c frame_rate=30
  v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=NV12
  timeout 20 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=120'"
# 期望：rc=0，dmesg 无 Oops/frame lost

# 双路（顺序 Y）
tools/ssh_board.sh -s "/tmp/dualtest.sh 30 120 90 4 30 15"
# 期望（本轮实际）：v0 rc=0、v4 rc=124（0 帧）

# 换模块（必须装完重启，HANDOFF §3.1）
sudo cp /lib/modules/6.6.98-sun60iw2/updates/vin_v4l2.ko.{orig-bsp,} && sudo depmod -a && sudo reboot
```
