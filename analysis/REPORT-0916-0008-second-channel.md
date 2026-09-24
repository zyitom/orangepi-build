# 2026-09-16 第四轮报告：`/dev/video4` 打通（T14 收官）+ 第二路相机的正确路线 + 五道保护

模块 `vin_v4l2` = `patches/0008`（md5 `9899a15c89ebd6ee0126228fa9714d35`，srcversion
`659707E6FD376A25E1E38CB`）；DT 改动全部走 fdtput 且可回退。
**第一路有 60 s 零丢帧证据，第二路有"单路 120 fps"和"与第一路同时跑各自 118 fps、两者 lost_cnt 全 0"的证据。**

> ⚠️ **本轮末尾板子需要断电重启**（§7）：为验证"第二个相机节点"我打开了 `sensor@5812020`，
> 内核在 probe 阶段 panic；随后我用 U-Boot 的 `init=/bin/sh` 试图救回，结果 shell 落在 HDMI 控制台
> （`console=tty1` 是最后一个 `console=`），串口看不到也进不去；`init=/bin/sh` 没有 systemd ⇒
> 16 s 看门狗没被打开 ⇒ 不会自动复位。**必须断电重启**，之后按 §7.3 把 DTB 换回备份。
> 所有结论都是板子活着的时候测的，证据都已落盘。

---

## 0. 三态结论总表

| # | 目标一 5 项 | 结论 | 位置 |
|---|---|---|---|
| 1 | T14 `video4` 永远 0 帧 | **已修复（实测达标）** | §1 |
| 2 | IOMMU 空指针 DMA | **已定性 + 已消除触发条件；fault 本身未再复现** | §2 |
| 3 | `S_FMT` 改坏共享上行 | **已修复（显式报错，不再静默改坏）** | §3 |
| 4 | `scaler get_selection error` 噪声 | **已修复** | §4 |
| 5 | T15 其余噪声 | **3 条已降级；`Runtime PM underflow` 已复现+定性未修；新发现"开第二个节点不 stream 会把第一路拖到 18 fps"** | §5 |

| # | 目标二 双相机路线 | 状态 | 位置 |
|---|---|---|---|
| 1 | 走 `vinc20`(device 8)+`sensor2`，**不走 isp01** | 路线确定；**isp01 的破坏机制本轮更正** | §6.1 / §6.4 |
| 2 | `ar0234_mipi.c` 多实例安全审计 | **审计完成：无多实例缺陷** | §6.2 |
| 3 | DT 改动走 fdtput + 备份 | `tools/dt_second_cam.sh` | §6.3 |
| 4 | 无模组状态下的验收 | 第一路零回归 ✅；第二路节点 ⚠️（开节点会 panic，必须模组先到位）；失败路径 ✅ 变响亮 | §6.5 |
| 5 | 加保护：误配置不再静默 | ✅ 5 处 | §1.5 / §3 / §4 / §6.4 / §6.5 |
| 6 | `PE6` pwdn 冲突 | **结论修正：只有 sensor1 冲突，sensor2 是 PE10**，走 vinc20 天然避开 | §6.6 |
| 7 | 接线后一页验证清单 | `analysis/t14/CHECKLIST-second-camera.md` | §6.7 |

---

## 1. T14：`/dev/video4` 0 帧 —— **根因确认并修复**

### 1.1 拓扑（实测，非推测）

```
ar0234_mipi ─ sunxi_mipi.0 ─ sunxi_csi.0 ─ sunxi_tdm_rx.0 ─ sunxi_isp.0
                                                            pad2 ─┬─ sunxi_scaler.0 ─ vin_cap.0 ─ /dev/video0
                                                                  └─ sunxi_scaler.4 ─ vin_cap.4 ─ /dev/video4
vinc@5830000 device_id 0: csi0 mipi0 isp0 tdm_rx0 isp_tx_ch 0 -> vipp_sel 0 -> 物理 VIPP0
vinc@5831000 device_id 4: csi0 mipi0 isp0 tdm_rx0 isp_tx_ch 1 -> vipp_sel 4 -> 物理 VIPP1
```
`media-ctl -p` 里 `sunxi_isp.0:2` 同时 `[ENABLED]` 到 `sunxi_scaler.0` 和 `sunxi_scaler.4`；
`/sys/kernel/debug/mpp/vi` 里 `vi0: ... isp0 => vipp0`、`vi4: ... isp0 => vipp4`。

### 1.2 寄存器级证据（新增工具 `tools/vinreg.c`，经 `/dev/mem`）

关键坑：**空闲时读 VIN 寄存器全是 0**（时钟被门控），必须在出流期间读。

`/dev/video4` 单独出流时（模块 0007，`vinc4_isp_tx_ch = <1>`）：

```
--- CSIC VIPP IN 0xa0: 0x00000000 0x00000001 0x00000000 0x00000000
     (0x58008a0 VIPP0_IN = 0 ; 0x58008a4 VIPP1_IN = 1)
--- VIPP1 top 0x00-0x3c (nonzero):
0x05910400 = 0x00000003     TOP_EN = CLK_GATING_EN | CAP_EN     <- VIPP1 已经使能
0x05910420 = 0xfbefbe03
0x05910430 = 0x00000100     CHN0_REG_LOAD 发生过
0x05910438 = 0x00000020
--- VIPP1 load 0x200-0x22c:
0x05910600 = 0x00000001     module_en: sc_en = 1
0x05910604 = 0x08400100     scaler cfg
0x05910608 = 0x04b00780     sc_size = 1920x1200
0x05910620 = 0x00000040     output fmt
```

对照第一路（`/dev/video0` 单独出流）VIPP0 的 load 寄存器：
`module_en = 0x00000001, sc_cfg = 0x08400100, sc_size = 0x04b00780, out = 0x40` —— **逐位相同**。

⇒ **scaler 与 VIPP1 的配置、使能状态与工作路完全一致；唯一不同是 `CSIC_VIPP1_IN` 的值 = 1。**

### 1.3 单变量在位实验（决定性）

`/dev/video4` 出流期间用 `vinreg w 0x58008a4 0` 把 VIPP1 的输入源从 1 改回 0：

```
-- reg before write: 0x058008a4 = 0x00000001
0x058008a4 <- 0x00000000 (readback 0x00000000)
-- vi4 after write (+8s): prs_in => x: 0, y: 0, hb: 0, hs: 0
                          frame => cnt: 963, lost_cnt: 2, error_cnt: 0
 t=  5.7s frames=      1      <- 改写之前 5 s 一帧都没有
 t=  6.7s frames=    122
 ...
RESULT dev=/dev/video4 1920x1200 NV12 frames=2924 wall=30.007s fps=97.44 timeouts=5
GAPS n=2923 median=0.008330s (120.04 fps) min=0.008119 max=0.008602 >1.5x=0 >3x=0
```

**写一个寄存器就把 0 帧变成 120 fps**，且立刻能与 `video0` 并存（§1.5 C）。

### 1.4 根因（确认级）

`CSIC_VIPPx_IN` 的值 = `vipp_input[vipp_virtual_find_sel[vipp_sel]][isp_virtual_find_sel[isp_sel]][isp_tx_ch]`
（`top_reg.c`；sun60iw2 走恒等表 ⇒ 值 = `isp*4 + ch`），语义就是"喂这个 VIPP 的 **ISP 输出通道号**"。

* `vinc4_isp_tx_ch = <1>` ⇒ 写 `CSIC_VIPP1_IN = 1` ⇒ 让 VIPP1 去取 **ISP0 输出通道 1**；
* 这块 SoC 的 ISP602 **只有一个输出通道**：`/sys/kernel/debug/mpp/vi` 自报 `isp 1`，而且全驱动里
  `isp_tx_ch` 只出现在 `vin_core.c` 的 DT 解析和 `vin.c:1458` 那一次 `csic_vipp_input_select()`，
  **`sunxi_isp.c` / `isp600_reg_cfg.c` 里完全没有"输出通道"这个概念**，没有任何代码会去使能通道 1；
* 于是 VIPP1 的输入源永远不来数据：**scaler 配好了、VIPP 使能了、DMA 起来了、内核一条错不报、
  `video4` 就是不出帧。**

⇒ 上一轮 `patches/0005` 的说明"第二路需要不同的 `isp_tx_ch`"**是错的**，它正是 0 帧的直接原因；
厂商给 `vinc8`（第二台相机）的默认值也是 `<0>`，与第一路一致。

### 1.5 修复 + 验证

修复（`patches/0008`，`vin_core.c`）：解析到 `vinc*_isp_tx_ch != 0` 时**强制回 0 并打 ERR**：

```
[    5.653975] sunxi:vin:[ERR]: vinc4_isp_tx_ch = 1, but isp0 has a single output
channel on this SoC; forcing 0 (video4 would otherwise silently never receive a frame)
```

换上 0008（`updates/vin_v4l2.ko` = `9899a15c…`）+ 重启后实测：

| 场景 | 命令 | 结果 |
|---|---|---|
| A 回归 `video0` 单路 60 s | `~/ar0234test/vfr -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 60` | rc=0 **frames=7203 / 60.003 s / 120.04 fps / timeouts=0**；间隔 median 8.330 ms，`>1.5x=0 >3x=0`；`vi0 frame cnt 7204, lost_cnt 0, error_cnt 0` |
| B 修复 `video4` 单路 20 s | 同参数 `-d /dev/video4` | rc=0 **frames=2360 / 20.005 s / 117.97 fps / timeouts=0**；`vi4 frame cnt 2361, lost_cnt 0`；`prs_in x:1920 y:1200` |
| C 双路同跑（先 v0，3 s 后 v4） | 两个 vfr | v0 **3068 帧 / 26 s / 117.98 fps / lost_cnt 0**；v4 **2361 帧 / 20 s / 118.01 fps / lost_cnt 0**；两边间隔统计 `>1.5x=0 >3x=0`；dmesg `frame lost 0 / sunxi_isp_reset 0 / Oops 0 / not mapped 0` |

原始日志：板上 `/home/orangepi/verify0008.out`，开机快照 `analysis/dmesg-boot-0008b.txt`。

**这正是用户要的"单 ISP 双输出"**：同一颗 sensor、同一时刻，两个 video 节点各自缩放/格式化、各自 DMA，
各约 118 fps 且零丢帧。

---

## 2. IOMMU 空指针 DMA（`0x0 is not mapped` / `Bug is in CSI module, id:0x2`）

### 2.1 受控复现实验（超时 + 看门狗兜底）

脚本 `analysis/scripts/t14iommu.sh`：先挂 150 s 的 `systemd-run --on-active=150 /bin/systemctl reboot` 兜底，
再按两个顺序跑，全程 `dmesg -n 8`。

* **X1（`video0` 出流中给 `video4` 做 S_FMT）**：`rc=0`，随后 dmesg 出现
  `isp0 sensor 960x600 vts 620 fps 235` —— **共享上行（sensor）在流运行中被改掉**，
  即"静默改坏"的机制，本轮复现。
* **X2（两个 S_FMT 都先做，再只出流 `video0`）**：
  `vi0: input => w: 1920, h: 1080` 而 `prs_in => x: 960, y: 600` ⇒ scaler 以为有 1920x1080，
  CSI 只给 960x600：**`frame cnt 648, lost_cnt 647`**（648 帧只出 1 帧），
  `v4l2-ctl --stream-mmap --stream-count=100` 25 s 后被 KILL（rc=137）—— 与 HANDOFF 记的
  "双进程卡死 DQBUF" 同一状态。**但这次没有触发 IOMMU fault**：
  `is not mapped 0 / Bug is in 0 / sunxi_iommu 0 / WARNING: 0 / Oops 0`。

### 2.2 旧证据定性（复现级 → 定性级）

`analysis/t14/B_dual.dmesg`（上一轮抓到的那一次）：

```
[   41.980001] L1 PageTable Invalid
[   41.980187] sunxi:vin:[INFO]: [ar0234_mipi]sensor_s_stream on = 1, 1920*1080 300a
[   41.989142] 0x0x0000000000000000 is not mapped!
[   41.989154] Bug is in CSI module, invalid address: 0x0, data:0x0, id:0x2
[   41.989319] ------------[ cut here ]------------
[   42.023220] WARNING: CPU: 0 PID: 0 at bsp/drivers/iommu/sunxi-iommu-v2.c:405 sunxi_iommu_irq+0x5bc/0x5c4
```

`bsp/drivers/vin/platform/sun60iw2_vin_cfg.h` 里
`#define ISP_IOMMU_MASTER 2`、`#define CSI_IOMMU_MASTER 1`
⇒ **`id:0x2` 就是 ISP 主设备**：ISP 的 DMA 被要求写到地址 0，发生在
`[ar0234_mipi]sensor_s_stream on = 1`（开流那一刻），前一行是 `L1 PageTable Invalid`。
与 §2.1 的错配状态属于同一个坑的下游后果（ISP 侧 load image / 地址状态与上游尺寸不一致）。
**本轮未再复现**（窗口很窄）。

### 2.3 修复思路：让错配状态**进不来**（而不是事后救）

`patches/0008` 在 `vin_video.c` 加了两道闸门、一处预检，共三处：

1. `vin_pipeline_try_format(set=true)` 中，凡 **sensor / mipi / csi / tdm_rx / isp** 这些
   *共享上行* 子设备正被另一个在跑的 capture 节点使用时，尺寸不一致就 **`-EBUSY` + 明确报错**，
   绝不改它的 ACTIVE 格式。（只看尺寸不看 mbus code —— 第一版比了 code，把正常的"同尺寸 S_FMT"
   也拒了，实测发现后改掉。）
2. `vidioc_s_fmt_vid_cap_mplane()` 开头、**`vin_auto_s_input()` 之前**，用
   `vin_check_shared_uplink()` 再预检一次，保证被拒的 S_FMT **不留下半开的 video 节点**。
3. `vidioc_streamon` 用 `GET_CURRENT_WIN_CFG` 比较"本节点配置的尺寸"与"sensor 真正在跑的尺寸"，
   不一致就拒绝开流：

```
[  146.432001] sunxi:vin:[ERR]: video0 was configured for 1920x1080 but the shared sensor is
now running 960x600 (another video node did an S_FMT in between); refusing to start - run
VIDIOC_S_FMT on this node again
```

即 §2.1 X2 那个"647/648 丢帧"的状态现在**开不起来**。

---

## 3. `S_FMT` 改坏共享上行 —— 已修（显式报错）

实测（模块 0008，`video0` 正在出流时对 `video4` 做 960x600 S_FMT）：

```
VIDIOC_S_FMT: failed: Device or resource busy          (rc=255)
[  150.471264] sunxi:vin:[ERR]: video0 is streaming 1920x1200 from the same sensor, refusing
the 960x600 S_FMT on video4 (the uplink cannot serve two sizes at once)
```

`vi0` 保持 `w:1920 h:1200, prs_in x:1920 y:1200`，第一路零影响。

**限制（必须写清楚）**：同一颗 sensor 的两个 video 节点**只能请求同一尺寸**。
`vin_pipeline_try_format()` 是把整条链按同一个尺寸配的（scaler 的 sink/source 都会被写成该尺寸），
这套代码本来就表达不了"1920x1200 输入 → 640x400 输出"。想在小尺寸拿第二路要走
`VIDIOC_S_SELECTION`（`sunxi_scaler` 的 CROP→缩放），**本轮未验证**，列为下一步。

---

## 4. `scaler get_selection error` —— 已修

`sunxi_scaler_subdev_get_selection()` 原来对 CROP/CROP_BOUNDS 以外的 target 一律 `-EINVAL`，
`vin_video.c:1425` 把它打成 `vin_err`。现在 default 返回 sink 当前矩形，`vin_video.c` 这条降为
`vin_warn`。实测：**模块 0008 的开机 dmesg 里 0 次**（旧模块每次 probe 4 次）。

## 5. T15 其余噪声

| 消息 | 结论 | 处理 |
|---|---|---|
| `videoN has already stream off` | 重复 STREAMOFF 的幂等提示 | 降为 `vin_warn` |
| `%s is not used, videoN cannot be close!` | 传感器未绑定时 close（节点被开过、管线没绑） | 降为 `vin_warn`（原 `vin_err`，且原实现会 `return -1`） |
| `Runtime PM usage count underflow` | **已复现**，见 §5.1 | 未修（定性） |
| `sensor_read/write error! sensor is not used!` | 上一条的伴生：3A/libisp 在传感器已关闭后仍去读写 | 解释性说明 |

### 5.1 新发现（重要）：**"打开再关闭第二个 video 节点"就会把第一路拖到 ~18 fps**

对照实验 `analysis/scripts/eprobe.sh`（每轮 `video0` 单独出流 30 s，中途对 `video4` 做一次操作）：

| 轮次 | 条件 | video0 结果 | timeouts |
|---|---|---|---|
| E1 | 3A 运行，`video4` S_FMT 960x600（被拒） | 550 帧 / 30.7 s = **17.93 fps** | 26 |
| E2 | **3A 停止**，同上 | 556 帧 = **18.13 fps** | 26 |
| E3 | 3A 停止，`video4` S_FMT **1920x1200（成功）** | 553 帧 = **18.04 fps** | 26 |
| E4 | 3A 停止，**完全不碰 video4** | **3599 帧 / 30 s = 119.96 fps** | **0** |
| E5 | 3A 运行，完全不碰 video4 | **3600 帧 = 119.98 fps** | **0** |

⇒ 与 3A 服务无关（E2 停掉 3A 一样坏），与 S_FMT 成功与否无关（E3 成功也坏）：
**只要"打开 video4 → 关闭"而没出流，第一路就从 120 fps 掉到 18 fps。** 伴生 dmesg：

```
sunxi:vin:[ERR]: [VIN_DEV_I2C_ERR]sensor_write error! sensor is not used!
sunxi:vin:[WARN]: ar0234_mipi is not used, video0 cannot be close!
sunxi-vin-core 5831000.vinc: Runtime PM usage count underflow!     <- video4 的 pdev
```

定性（候选）：关节点路径在"传感器没被真正绑定"时**提前 return**，跳过了管线下电 / PM 计数归还，
把共享的 sensor/ISP 留在半开状态（`5831000.vinc` 的 PM 计数不平衡是直接证据）。
**未修**：该状态只在"开了不出流"时出现；正常双路（都出流，§1.5 C）不受影响。
真修需要给 close 的提前返回路径补完整回滚，改动面超出本轮预算。

---

## 6. 目标二：两台 AR0234 的正确路线

### 6.1 路线（无歧义）

**走 `vinc@5832000`（label `vinc20`，device_id 8）+ `sensor@5812020`（sensor 槽 2）。**
厂商已把它配成 `mipi1 / csi1 / tdm_rx0 / isp0 / rear_sensor_sel = 2`，且
`vinc8_isp_tx_ch = 0`（与第一路一致 ⇒ 不会再踩 §1.4 的坑）。**不要**走 `vinc01`/`isp01`。

### 6.2 `ar0234_mipi.c` 多实例安全审计（逐项）

| 检查项 | 结论 |
|---|---|
| 全局/静态单实例状态 | `frame_rate`/`trigger_mode`/`flash_enable`/`flash_delay` 是 **module param**，只作"新 probe 的默认值"（被 `sensor->fps` 覆盖），**按设计共享**，不是 bug |
| per-stream 状态 | 全在 `struct ar0234`（`streaming/mono/mfr_30ba/test_pattern/fps/isp_fps/fll`），随 i2c client 走 ✅ |
| mode/win 表 | `sensor_formats[]` / `sensor_win_sizes[]` 只读 ✅ |
| i2c client 缓存 | 无 ✅ |
| `of_find_node_by_path` 取 sensor0 路径 | 无 ✅（`vin_set_mclk*` / `vin_gpio_*` 都吃 `sd`，走 per-instance 的 `sensorN_*` DT 属性） |
| `cci_drv[]` 选择 | probe/remove 都按 `client->name` **名字查找**，两个 driver 名（`ar0234_mipi` / `ar0234_mipi_2`）各占一格 ✅ |
| `static int sensor_dev_id` | 只在 `client == NULL` 分支用（真实 i2c probe 不走），probe/remove 都 `++`；**目前无害**，但真用到会错位 —— 建议删（未改） |
| `SENSOR_NUM = 2`、`I2C_ADDR = 0x20` | 两个实例同地址 ⇒ **不能挂同一条 i2c 总线**；两路必须各走一条 CCI/TWI（见 §6.6） |

⇒ **结论：`ar0234_mipi.c` 不需要为多实例改动。** 双相机真正的坑都在 VIN 框架侧（§6.4 / §6.5）。

### 6.3 DT 改动（fdtput，全部可回退）

工具 `tools/dt_second_cam.sh`（板上 `/tmp/dt_second_cam.sh`）：

```
sh dt_second_cam.sh show            # 打印相关属性
sh dt_second_cam.sh sensor2-on      # mname/cci_id 9/addr 0x20/mclk2/status okay + vinc20 okay
sh dt_second_cam.sh sensor2-off     # 回 imx219_2 + status disabled
sh dt_second_cam.sh isp01-on|off    # 只开/关 isp@58ffffc（验证驱动保护）
```
每次改动前自动 `cp <dtb> <dtb>.bak-<epoch>`。

第二路要改的**完整清单**：

| 节点 | 属性 | 现在（厂商） | 第二路到位后 |
|---|---|---|---|
| `sensor@5812020` | `sensor2_mname` | `imx219_2` | `ar0234_mipi`（或 `ar0234_mipi_2`） |
| | `status` | `disabled` | **`okay`（⚠️ 见 §6.5：相机必须先插上并上电）** |
| | `sensor2_twi_cci_id` | `9` | 保持（确认第二路接在 cci 9 那条总线） |
| | `sensor2_twi_addr` | `32` (0x20) | 保持 |
| | `sensor2_mclk_id` | `2` | 保持（确认 mipi1 的 MCLK 能出 24 MHz） |
| | `sensor2_pwdn` | `PE10` | 保持（**与相机 0 的 PE6 不冲突**） |
| | `sensor2_reset` | 空 | 模组若有 reset 脚，指到空闲 GPIO |
| `vinc@5832000` | `status` | `okay` | 保持 |
| | `vinc8_*_sel` | mipi1 / csi1 / isp0 / tdm0 / sensor2 | 保持 |

### 6.4 `isp01` 保护：**机制更正**（本轮最重要的认知修正）

上一轮结论是"`isp01 = okay` 会让第一路静默 0 帧"。本轮加保护后复现并进一步隔离：

* `isp@58ffffc` 的 probe 被**明确拒绝**，板上**没有** `sunxi_isp.1`，
  media graph 21 个实体与正常启动**完全一致**（无多余实体/链接），而：
  ```
  [    5.348310] sunxi:vin:[ERR]: isp1@/soc@3000000/vind@5800800/isp@58ffffc: refusing to
  register, it is a virtual instance of isp0 which already works in online mode; keep this
  node disabled in the device tree, enabling it makes the running camera stop delivering
  frames with no error at all
  ```
* **但第一路仍然是 0 帧**：`vfr -d /dev/video0 -t 30` → `frames=0 wall=30.034s timeouts=30`，
  `input => w: 0, h: 0, fmt: NULL`、`prs_in => x: 1920, y: 0`（有行时序、没有帧），
  除我那条 ERR 外内核**无任何其它消息**（`Oops/BUG/WARNING/not mapped` 全 0）。
  启动日志里唯一的新差异是 `vin_isp 58ffffc.isp: Adding to iommu group 0`。

⇒ 结论修正为：**"坏"不是由子设备注册/媒体实体造成的，而是"这个 DT 节点被 enable"本身**；
机制不在 `sunxi_isp.c` 内（候选：IOMMU 组 / platform device 层面），**本轮未能定位**。
驱动保护解决的是"**不再静默**"（有明确 ERR 指引你把节点关掉），**不解决"不会坏"**。
所以 §6.1 的路线（不碰 isp01）是必须项，不是偏好。

### 6.5 失败路径：**无模组时打开 sensor2 节点会 panic（务必写进接线步骤）**

`tools/dt_second_cam.sh sensor2-on`（含 `sensor@5812020 status=okay`）+ 重启后（模组未插），
串口原文（`analysis/t14/serial-recover.log`）：

```
[    5.967238] Internal error: Oops: 0000000096000005 [#1] PREEMPT SMP
[    5.974271] Modules linked in: ar0234_mipi(O) vin_v4l2(O+) videobuf2_dma_contig vin_io
[    5.974300] pc : device_del+0x48/0x410
[    6.120705]  device_unregister+0x18/0x34
[    6.120711]  cci_dev_remove_helper+0x70/0x168 [vin_io]      <- 传感器驱动 remove
[    6.120739]  sensor_remove+0x68/0xc8 [ar0234_mipi]
[    6.120752]  i2c_device_remove+0x2c/0x9c
[    6.156579]  i2c_unregister_device.part.0+0x3c/0x6c
[    6.156592]  v4l2_i2c_new_subdev_board+0x88/0x124           <- V4L2 的失败回滚
[    6.156603]  __vin_subdev_register+0x80/0x120 [vin_v4l2]
[    6.207983]  vin_probe+0x730/0x13a0 [vin_v4l2]
[    6.342558] Kernel panic - not syncing: Oops: Fatal exception
```

机制：`v4l2_i2c_new_subdev()` 建好 i2c client 后认为驱动没绑上（`client->dev.driver == NULL`），
走**失败回滚** `i2c_unregister_device()` → 触发 `ar0234_mipi` 的 `remove` →
`cci_dev_remove_helper()` → `v4l2_device_unregister_subdev()` 里对一个没建成的 device 做
`device_unregister()` → NULL 解引用。**这是厂商 probe 失败回滚路径的 bug，也正是厂商把
sensor1/2/3 全部 `disabled` 的原因。**

⇒ **接线顺序（硬性）**：
1. 先把第二颗模组插到 MIPI-B（`mipi1`）并接好 i2c（cci 9）；
2. 再 `dt_second_cam.sh sensor2-on`；
3. 重启。**反了就是 boot loop**（panic → 16 s 看门狗 → panic …），而那时**没有 ssh**。

失败路径的"干净"部分（无模组、节点保持 `disabled`）是达成的：

```
[    5.731573] sunxi:vin:[ERR]: vinc8 (device_id 8) needs sensor2 (vinc8_rear_sensor_sel = 2)
but no sensor is bound to that slot, so no /dev/video8 will be created
```

（这条以前**完全没有** —— 厂商直接 `vind->vinc[i] = NULL` 静默丢掉，"第二路没节点"一直没人知道。）

### 6.6 `PE6` pwdn 冲突：结论修正

```
$ fdtget -t i <dtb> .../sensor@5812000 sensor0_pwdn -> 54 4 6 0    (&pio PE 6)
$ fdtget -t i <dtb> .../sensor@5812010 sensor1_pwdn -> 54 4 6 0    (&pio PE 6)  <-- 与相机 0 同一根
$ fdtget -t i <dtb> .../sensor@5812020 sensor2_pwdn -> 54 4 10 0   (&pio PE 10) <-- 不同
```

⇒ **冲突只存在于厂商的 `sensor1` 槽（即 `vinc01`/`isp01` 那条不推荐的路）。**
推荐的 `sensor2`（`vinc20`）用 **PE10**，天然避开。**建议**：
(a) 第二颗模组接 MIPI-B / cci9（sensor2），硬件上不要把两颗模组的 PWDN 并到 PE6；
(b) 若模组只有 PWDN 且必须共线，则把 DT 里两路的 pwdn 指到同一根，并接受"两路同时上电"；
(c) 接线前用**模组原理图**确认 PWDN/RESET/MCLK 实际引脚（HANDOFF T23 仍缺这张图）。

### 6.7 接线后验证清单

见 `analysis/t14/CHECKLIST-second-camera.md`（一页，每步有确切命令与判据）。

---

## 7. 本轮事故：板子现在需要断电重启

### 7.1 经过

1. 为验证"第二路节点"打开了 `sensor@5812020` ⇒ §6.5 的 panic。
2. `panic_on_oops=1` + 没有 systemd ⇒ 没人喂狗；靠**上一次 boot 里 systemd 打开的**
   `RuntimeWatchdogSec=16s` 才自动复位了几次（日志里 3 次 BOOT0），直到某次 U-Boot 被我的串口按键停住。
3. 停在了 U-Boot `=>`，用 `setenv extraargs init=/bin/sh` + `run bootcmd`
   （**内存内改，没 saveenv**）想拿 root shell 去 `cp` 回备份 DTB。
4. 结果：`boot.scr` 生成的 `consoleargs` = `"console=ttyS0,115200 console=tty1"`，
   **`/dev/console` 落在 `tty1`（HDMI）**；`init=/bin/sh` 的 shell 挂在 HDMI 上，
   串口只能看到 loglevel=1 的寥寥几行。没有 systemd ⇒ 看门狗没被打开 ⇒ **不会自动复位**。
5. 串口 SysRq 无效：`CONFIG_MAGIC_SYSRQ=y` / `MAGIC_SYSRQ_SERIAL=y` 都在，但板子的控制台驱动是
   厂商 `uart-ng`（`/proc/iomem` 里写的就是 `uart-ng`）。BREAK、`tcsendbreak`、
   降波特率造长 BREAK 三种方式实测都无反应。

### 7.2 恢复：**断电重启**（唯一手段）

断电 5 s 再上电，板子回到"单相机可用"状态（DTB 里的坏属性仍在，见 §7.3）。

### 7.3 断电后第一件事（把 DTB 换回备份）

```sh
tools/ssh_board.sh -s "sh -c '
  cp /boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb.pre-0008 \
     /boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb; sync; \
  md5sum /boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb'"
#   期望 md5 = d4ee5b6869e7b7cd39ce3762d0e078f9
tools/ssh_board.sh -s "systemd-run --on-active=2 /bin/systemctl reboot" < /dev/null
#   然后跑一次 120 s 回归：tools/hwtest/vfrrun.sh final-0008 120 -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120
```

`/boot/dtb/allwinner/` 里的备份都在（U-Boot 的 `ls` 可见）：
`.pre-0008`（= 原始 `d4ee5b68…`）、`.bak-1789547410`、`.bak-1789547036`、`.dual-bak-*`、`.orig-ar0234-camera`。

### 7.4 教训（已写进 HANDOFF）

* **不要在这块板上用 `init=/bin/sh`**：console 顺序把 `/dev/console` 给了 tty1，串口进不去；
  又没有 systemd ⇒ 看门狗不开 ⇒ 卡住只能断电。要救 DTB 请显式写
  `setenv extraargs "console=ttyS0,115200 init=/bin/sh"`（或把 ttyS0 放到最后）。
* **16 s 看门狗只在 systemd 起来后有效**；probe 阶段 panic 依赖的是*上一次 boot* 的看门狗。
  想在无 systemd 时也安全，需要在 U-Boot / cmdline 里打开硬件看门狗（本轮未做，列为建议）。
* 串口 SysRq 在 `uart-ng` 上不可用 ⇒ **不要把它当兜底手段**。

---

## 8. 改动清单

### 8.1 TL101（`/home/helios/Desktop/orangepi-build/ar0234-port`，**未 git commit**）

| 路径 | 动作 |
|---|---|
| `patches/0008-vin-second-channel-and-pipeline-guards.patch` | 新增（411 行，含根因/证据的提交信息） |
| `apply.sh` | 加 0008（相对 `bsp/drivers/vin`）；`bash -n` 通过；备份 `apply.sh.bak-pre-0008` |
| `build/vin-d3d-lbc/{vin.c, vin-video/vin_core.c, vin-video/vin_video.c, vin-vipp/sunxi_scaler.c, vin-isp/sunxi_isp.c}` | 改（= pristine+0003/0005/0006/0007 + 0008） |
| `build/vin-d3d-lbc/out/vin_v4l2-0008.ko` | md5 `9899a15c89ebd6ee0126228fa9714d35`，srcversion `659707E6FD376A25E1E38CB` |
| `tools/vinreg.c` | 新增：/dev/mem 读写 SoC 寄存器（`r/s/w`），T14 证据工具 |
| `analysis/scripts/t14probe.sh` `t14live.sh` `t14dual.sh` `t14iommu.sh` `verify0008.sh` `eprobe.sh` | 新增：T14 三场景 / 在位改写寄存器 / 双路 / IOMMU 受控复现 / 0008 回归 / 3A 对照 |
| `tools/dt_second_cam.sh` | 新增：第二路 DT 一键改/回退（fdtput + 时间戳备份） |
| `tools/rescue/uboot_rescue.py` `uboot_cmd.py` `uboot_shell_rescue.py` `sysrq.py` `sysrq2.py` `sysrq3.py` | 新增：U-Boot 抢救/命令注入（事故中产生） |
| `analysis/REPORT-0916-0008-second-channel.md` | 本报告 |
| `analysis/dmesg-boot-0008.txt` / `-0008b.txt` / `-isp01-on-0008.txt` / `-sensor2-0008.txt` / `-sensor2b-0008.txt` | 开机快照 |
| `analysis/t14/uboot*.log` `serial-recover.log` `serial-recovery.log` `sysrq*.log` | 事故现场与恢复过程 |
| `analysis/t14/CHECKLIST-second-camera.md` | 接线后一页清单 |
| `docs/HANDOFF.md` | 更新 |

### 8.2 板子

| 路径 | 内容 |
|---|---|
| `/lib/modules/6.6.98-sun60iw2/updates/vin_v4l2.ko` | md5 `9899a15c…` = 0008（断电后会加载它） |
| `…/updates/vin_v4l2.ko.bak-0007` | 0007 版 `f27bca0d…`（本轮新增备份） |
| `…/updates/vin_v4l2.ko.{bak-0006,bak-pre-fix,fix-t14,orig-bsp,pre-0006}` | 既有备份未动 |
| `/boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb` | ⚠️ **当前是坏配置**（`sensor@5812020 status=okay`），按 §7.3 恢复 |
| `/boot/dtb/allwinner/…dtb.pre-0008` | 原始 `d4ee5b68…`（本轮新增） |
| `/boot/dtb/allwinner/…dtb.bak-*` | 每次 fdtput 前的时间戳备份（可删） |
| `/home/orangepi/{t14iommu.sh,eprobe.sh,verify0008.sh,t14iommu.out,verify0008.out,…}` | 测试脚本与日志（非持久改动） |
| `/etc/systemd/system.conf` | 未动（`RuntimeWatchdogSec=16s` 沿用上一轮） |

### 8.3 回退

```sh
# 模块回 0007（或更早 .orig-bsp）
tools/ssh_board.sh -s "sh -c 'cp \
  /lib/modules/6.6.98-sun60iw2/updates/vin_v4l2.ko.bak-0007 \
  /lib/modules/6.6.98-sun60iw2/updates/vin_v4l2.ko; depmod -a'"   # 换模块必须重启
# DT 回原始
sh /tmp/dt_second_cam.sh restore /boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb.pre-0008
# 重编
tools/build_vin.sh <tag>
```

**补丁可复现性（已验证）**：把 `0008` 施到 `pristine + 0003 + 0005 + 0006 + 0007`，
5 个文件与 `build/vin-d3d-lbc` **逐字节一致**（`patch` rc=0，`diff -q` 全 OK）。

---

## 9. 没验证的部分（无模组 / 板子断电）

| 项 | 状态 |
|---|---|
| `/dev/video8` + `vin_cap.8` 真出现 | **未验证**（需模组；且无模组时开节点会 panic，§6.5） |
| `sensor2` 上真探到 AR0234（0xa56） | 未验证（同上） |
| 第二路独立出流 / 两路同时出流（两台相机） | 未验证（需要两颗模组） |
| `sunxi_tdm_rx.1 → sunxi_isp.0` link 变 `[ENABLED]` | 未验证 |
| MIPI-B 的 4 lane 与 MCLK 电气（能否 24 MHz、lane 映射） | 未验证（需要原理图 + 模组） |
| ISP602 双 1200p120 的总吞吐（553 Mpix/s vs 标称 4K30≈249 Mpix/s） | 未验证（单 ISP 双 VIPP 已在 118+118 fps 跑通，但那是**同源**；两路不同源要 ISP 时分，风险高） |
| `VIDIOC_S_SELECTION` 在第二路做小尺寸输出 | 未验证（§3 的限制） |
| `patches/0008` 的**最终**长时回归（120 s） | **未做**（板子断电，恢复后第一件事） |

---

## 10. "两台相机怎么才能跑起来"：还差什么（按优先级）

1. **硬件（阻塞）**：第二颗 AR0234 模组 + 排线插到 **MIPI-B**，i2c 接 **cci 9** 那条总线，
   PWDN 走 **PE10**（不要并到 PE6），确认 MCLK 与 4 lane 走线。没有模组无法进入下一步。
2. **DT（已备好）**：`dt_second_cam.sh sensor2-on` 一条命令
   （`sensor@5812020` 改 `ar0234_mipi` + `status=okay`；`vinc@5832000` 本来就 `okay`）。
3. **顺序（硬性）**：**先插模组再开节点**，否则 §6.5 的 probe 回滚 bug 会让板子 panic 起不来。
4. **验收顺序**：先只开第二路（`/dev/video8`）跑通 → 再两路同跑 → 记录两个 `viN` 的
   `lost_cnt/error_cnt` 与 dmesg 计数（清单见 §6.7）。
5. **吞吐**：先试 1200p60 + 1200p60，再往 120 加。若 ISP 时分撑不住，退路：
   (a) 其中一路降帧率；(b) 其中一路走 RAW bypass（不经 ISP）；(c) 两路都用
   "一颗 sensor 两个输出"那种已经验证过的形态。
6. **框架侧遗留**（不阻塞双相机，建议做）：
   - §5.1 "开第二个节点不 stream 会拖慢第一路"（close 提前 return 未回滚）；
   - §2 IOMMU fault 的根因仍在 ISP 侧地址状态机（闸门只让它进不来）；
   - §6.4 `isp01 = okay` 的破坏机制（不在 `sunxi_isp.c` 里）。
7. **需要用户决定**：两路要不要**不同尺寸/格式**（决定是否需要做 scaler 的 ROI/selection 路径）。
