# A733（Orange Pi Zero 3W）底层硬件栈可用性审计 —— 单颗 AR0234 场景

日期：2026-09-16（第六轮）
作者：ZCode（agent）
板子：Orange Pi Zero 3W / Allwinner A733（sun60iw2），内核 `6.6.98-sun60iw2`
模块：`vin_v4l2` = `patches/0009`（`srcversion 843BC1A03606D35EC062D57`，md5 `2715c25c40085869f0573a43b52528ae`）
DTB：`/boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb`，
**md5 `d4ee5b6869e7b7cd39ce3762d0e078f9`，本轮全程未改动任何 DT**
传感器：**只有一颗 AR0234，MIPI-A（`sensor@5812000`）**

---

## 0. 一句话结论

**这颗 SoC 除"第二颗模组/NPU 端到端喂流/外部触发接线"三类缺硬件项之外，图像链路的每一级都能在本轮实测中端到端跑通**：
MIPI PHY 4-lane 844 Mbps 在流、CSI300_500、TDM online、ISP602（唯一一块）、双 VIPP、VE 编解码、硬件 JPEG、
G2D、PowerVR OpenCL 3.0、**NPU（用 `libNBGlinker.so` 跑通了真实推理）** 全部可用。

**但文档里的 ✅ 有若干条经不起复测**，其中三条是"状态被高估"：

1. **H.265 编码在 1920×1200@120 下直接失败**（只出 6 帧 + `isp0 configuration error/height error`）——
   而 HANDOFF 把"硬件编码 H.264/H.265 零拷贝"记成整条 ✅。
2. **G2D 的"字节级精确（0 差异）"说法两轮都错**（第八轮把机制查清了）：单次 4:2:0 `YUV420UVC` blit **不可能位精确**（Y 被钳到 ≥16、色度走 4:2:0 重采样滤波器），本次 fd→fd 的 213 834 字节差**不是缓存/漂移**；而 **NV12 位精确拷贝存在**——两次 `G2D_FORMAT_Y8` blit，实测 0/3 110 400（含真实 ISP 帧）。另外 NV12 的正确格式常量是 0x28（`..._V1U1V0U0`），用了两轮的 0x29 其实是 NV21。见 `../g2d/REPORT.md`。
3. **`patches/0009` 并不能把 §3.27 变成"已修复"**：同样手法 11 次干净、但另有两次独立发作（其中一次让 video0 连续 9 个 20 s 流一帧不出）。

反过来，**有两条被记成"必然/严重"的坑，本次证明被高估**：

4. **ISP 出错后并不一定要重启**：1080p@136 打出的 400 次 `frame_lost` + 400 次 `sunxi_isp_reset`，
   停流后下一次 1200p120 立刻 119.78 fps、0 丢帧（HANDOFF §3.2「ISP 出过错之后…先重启再测」过宽）。
5. **T6「十几次里有 1 次」被严重低估**：实测 **4/40 = 10%** 的流启动会"只出 4 帧就卡死"，
   而且**紧接着的下一次启动一定正常，不需要重启**（这是本轮最有操作价值的一条）。

以及一条**被记成 ❌ 但其实是 ✅**：

6. **NPU**：文档说"缺 `libVIPlite.so` ⇒ 用不了"。实际上 `/usr/lib/libNBGlinker.so`
   **导出了 `vip_lite.h` 的整套 API**（`vip_init` / `vip_create_network` / `vip_create_buffer_from_fd` /
   `vip_run_network` …），而且板上自带 `/opt/vpm_run`（含 `.nb` + 输入数据 + 运行框架）与 `/opt/yolov5`
   （含模型 + OpenCV 依赖）。实测 **推理跑通**（2747 µs），YOLOv5 demo 认出 dog 82%。

---

## 1. 硬件能力矩阵（本轮实测）

状态定义：✅ = 本次端到端跑通且有证据；⚠️ = 能跑但有前提/限制/偶发；❌ = 本该能用但当前不行；⬜ = 缺硬件/缺 SDK。

### 1.1 传感器链路（逐级）

| 块 | 状态 | 本次实测证据 | 可达条件/限制 | 遗留缺陷 |
|---|---|---|---|---|
| AR0234 驱动 `ar0234_mipi` | ✅ | `/dev/v4l-subdev0` = `ar0234_mipi`；开机 dmesg `[ar0234_mipi]V4L2_IDENT_SENSOR = 0xa56` + `find the onsemi AR0234`；控件齐（exposure/gain/frame_rate/test_pattern/hflip/vflip/temperature） | 保持 `extra` 与 `updates` 两份 `.ko` 都可加载：`modinfo -n` = `/lib/modules/6.6.98-sun60iw2/updates/ar0234_mipi.ko` | `temperature_approx_degc` 读到 **0**（控件在，值恒 0，本轮未深究）；触发/闪光灯代码在但未实测（见 1.8） |
| MCLK | ✅ | `clk_summary`：`pll-video0-3x = 432 MHz` 喂 `csi_mclk{0,1,2}_pll`；DT `sensor0_mclk_id = 1`；`isp-mclk`/`csi-mclk` = 26 MHz（`CLK_IS_CRITICAL`） | 由 DT 决定 | 无法从用户态改（`clk_rate` 只读） |
| MIPI PHY（MIPI-A / mipi0） | ✅ | 出流中 `cat /sys/kernel/debug/mpp/mipi`：`phy_en: enabled`、`data_lane_en: 0xf`、`clk_lane_en: enabled`、`deskew 0x2`、`settle_time 0x28`、**`mipi_bps: 844 Mbps`**、`data_lane0/1 + clk_lane = 0x5 (HS_S, ok)` | 空闲时 phy_en = disabled（时钟门控），**必须出流时读** | 无（`phy_link_mode: 0x0 unrecognized` 是空闲状态的正常表现） |
| MIPI 端口层 | ✅ | 出流中：`port_en: enabled`、`lane_num: 4 lane`、`channel_num: 1`、`out_data_num: 2`、`unpack_en: enabled`、**`cur_data_type: 0x2b (RAW10)`** | — | yuv_seq 显示 `YUYV`（对 RAW10 无意义） |
| CSI | ✅ | `CSI_VERSION: CSI300_500`；出流中寄存器 `0x05800000 = 0x80000003`；`mcsi 3, ncsi 2, parser 3` | — | 只有 csi0 被接出（csi1/csi2 无 sink link），第二颗模组要用 csi1 |
| TDM | ✅（online） | `mpp/vi` 两路都是 `tdm_rx0`；DT `tdm@5908000 work_mode = 0`（online）；`bkuf work_mode: online` | **online 模式驱动明确拒绝第二路 rx**（`tdm can not be open again!`），且 `rx->id != 0` 直接报错 | 多路进一块 ISP 需要 offline（`work_mode=1`）+ DDR 环形缓冲，未启用 |
| ISP（ISP602） | ✅ | `ISP_VERSION: ISP602_100`，`isp 1`（**全片只有一块**）；出流中 `0x05900000 = 0x00000005`；`CSI_CLK: 324000000, ISP_CLK: 324000000` | 输入是**逐行实时流**，行消隐 ≥128 个 ISP 周期；开流前必须限帧率（`vfr -p`） | 见 3.2（时钟 324 是配置值，不是硅片上限） |
| VIPP / scaler | ✅ | `vi0 → ... → vipp0`、`vi4 → ... → vipp4`；`sunxi_scaler.0/.4` 链路 `[ENABLED]` | 两个 video 节点共用同一条 mipi/csi/tdm/isp | **`vinc4_isp_tx_ch` 必须为 0**（0008 已强制并打 ERR）；`scaler.8/.16` 实体在但没接 video 节点 |
| 3A 服务 `ar0234-3ad` | ✅ | `systemctl is-active` = active；journal 每次开流都 "stream 1920x1200, 120 fps" → "3A running on isp0" → "stream stopped" | 靠 ISP 子设备事件（`V4L2_EVENT_FRAME_SYNC`）工作 | **固定/工业模式未实现**：`/etc/ar0234.conf` 不存在（T3 未做），只能靠 `-e/-g` 在开流后临时改 |

**链路计数证据**（单路 1200p120 30 s）：
```
RESULT dev=/dev/video0 1920x1200 NV12 frames=3601 wall=30.000s fps=120.03 timeouts=0
vi0: input w:1920 h:1200 fmt:GRBG10 | output 1920x1200 NV12 | prs_in x:1920 y:1200 hb:478 hs:1724
     bkuf cnt:4 size:3457024 rest:3 work_mode:online | frame cnt 3602 lost_cnt 0 error_cnt 0
     internal avg:8ms max:8ms min:7ms
```

### 1.2 ISP 功能面

| 项 | 状态 | 本次实测证据（`vfr ... -p 1/120`，8 s） | 条件/限制 | 遗留缺陷 |
|---|---|---|---|---|
| NV12 1920×1200@120 | ✅ | `frames=960 fps=119.90 timeouts=0`，`lost_cnt 0`，sizeimage 3456000 / bpl 2880 | 需要先限帧率 | — |
| BGR3 1920×1200@120 | ✅ | `frames=960 fps=119.90`，sizeimage 6912000 / bpl 5760 | — | — |
| RGB3 | ✅ | `frames=952 fps=118.93` | 与 BGR3 **同一条代码路径**：`sunxi_scaler.c:849-852` 里 `V4L2_PIX_FMT_RGB24` 与 `V4L2_PIX_FMT_BGR24` 都设 `out_fmt = YUV2RGB888`（源码级确认，故字节序必然相同） | 字节序本身（B,G,R）为上一轮结论，本轮未重测 |
| BA10（RAW，绕 ISP） | ✅ | `1920x1200@120 → 960 fps`，sizeimage 4608000 / bpl 3840（16 bit 小端）；`960x600@120 → 600 fps` | — | — |
| GREY | ✅ | `1920x1200@120 → 960 fps`（bpl 1920）；`640x400@120 → 118.86 fps` | 等价于 NV12 的 Y 平面 | — |
| NV21 / YU12 | ✅ | 各 300 帧 @60 fps 干净 | — | 未被 userspace 采集库使用 |
| RGBP(565) | ✅ | 300 帧 @60 fps，sizeimage 4608000（16bpp） | — | — |
| FC21（FBC NV12 2-plane） | ⚠️ | `300 帧 @60 fps timeouts=0`，`vi0 output fmt: FBC` | S_FMT 被接受、帧也出 | **载荷未验证**（bpl/sizeimage 与 NV12 完全相同，怀疑实际仍是 NV12 数据）；工业用途不需要，未深究 |
| LC21（LBC 2X） | ❌ | `frames=0 wall=5.006s timeouts=5`；`vi0 output fmt: LBC_2X`，`frame cnt 0`；**内核一条错都不报** | 先决条件未知 | **静默失败**：格式被接受、VIPP 配好、就是 0 帧 |
| YUYV / UYVY / RGB4 | ❌ | `S_FMT: Invalid argument` + `[ERR] vin is not support this pixelformat` + `vin_pipeline_try_format failed` | — | 驱动 `ENUM_FMT` 里列了它们，但 `S_FMT` 必失败（枚举表 ≠ 可用表） |
| 硬件缩放（ISP 直出小尺寸） | ✅ | 640×400 BGR @118.94、320×200 @119.81、960×600 @119.81、1600×1000 @119.81、192×128 @118.86、1000×150 @119.77、640×640 @118.86（**拉伸**） | 宽高比与传感器窗口不同时会被拉伸，不裁切 | — |
| 帧率 60/30/110 | ✅ | 1200p：120→119.90、110→109.94、60→59.91、30→29.94，全部 0 超时 | — | — |
| 1080p 帧率上限 | ⚠️ | 请求 130/132/133/134/135 **全部干净**（实测 131.6–131.8 fps）；请求 **136 → `frame_lost` 风暴**（3 s 内 201 次 + 201 次 `sunxi_isp_reset`） | 驱动把 ≥134 的请求都钳到 `fll = 1096 (133 fps)`，132→1109、133→1100 | **文档的"132 好 /133 坏"边界不成立**；134/135 与 136 编制了同一个 fll 但只有 136 崩，机制**未定位**（怀疑在 3A 的 `ispSetFpsRanage` 曝光上限，不在传感器） |

**ISP 内部模块实际开关**（`isp_param_config.bin` 的 74 字节文件头之后是结构体；
结构体偏移 88..119 = **文件偏移 162..193**，实测逐字节读出）：

| 模块 | 值 | 模块 | 值 | 模块 | 值 | 模块 | 值 |
|---|---|---|---|---|---|---|---|
| manual | 0 | otf_dpc | 1 | pltm | 1 | drc | 1 |
| afs | 1 | ctc | 1 | cfa | 1 | gtm | 1 |
| ae | 1 | gca | 1 | lca | 1 | gamma | 1 |
| af | 0 | nrp | 1 | sharp | 1 | cem | 1 |
| awb | 1 | denoise | 1 | ccm | 1 | encpp | 0 |
| hist | 1 | **tdf(3DNR)** | **1**（1200p120 时切 0） | defog | 1 | enc_3dnr | 0 |
| wdr_split | 0 | blc | 1 | cnr | 1 | **lsc** | **0** |
| wdr_stitch | 0 | wb | 1 | dig_gain | 1 | **msc** | **0** |

- **实际生效**：BLC、DPC、CTC、WB、CFA(去马赛克)、CCM、gamma、DRC、PLTM、CNR、CEM、GCA、LCA、sharp、defog、dig_gain、NRP、denoise。
- **LSC/MSC 关着**（暗角未校正）；**3DNR（tdf）按流切换**：1200p120 下驱动侧 `3DNR forced off, sensor vblank 136 us < 500 us`。
- **偏"美化"而非工业**：`defog`（去雾）、`lca/gca`（色差）、`sharp`、`pltm`(局部色调映射) 都开着——**T4「工业参数集」还没做**。
- 证据：3dnr / no3dnr 两个变体文件**只有一个功能字节不同**，文件偏移 175（= 结构体 101 = `tdf`）：
  `isp_param_3dnr.bin [175]=1`，`isp_param_no3dnr.bin [175]=0`；其余差异全在那 50 字节 note 文本里。

**参数注入工具链** | ✅ 可用 | `python3 tools/make_isp_bin.py isp/template_gc05a2_a733.blob tuning-ref/kurokesu_libcamera_vc4_ar0234.json /tmp/tune.bin --set tdf=0 --set lsc=0 --note audit-check` → 生成 116358 字节；回读文件 162..193：`tdf`=0、`lsc`=0、`msc`=0（按预期生效）。同时打印 CCM 三色温行（2800/4000/6500K）与 AWB 光源表。| 需要 `-Zxz` 无关 | ⚠️ **所有偏移都是「结构体偏移」**：文件偏移 = 结构体偏移 + 74（4B 长度 + 20B 日期 + 50B note）。文档/`setters.json` 没写清这一点，容易踩空 |

### 1.3 视频输出面

| 项 | 状态 | 本次实测证据 | 条件/限制 | 遗留缺陷 |
|---|---|---|---|---|
| 两个 video 节点 | ✅ | `/dev/video0`(vi0→vipp0)、`/dev/video4`(vi4→vipp4)；`media-ctl -p` 共 21 个实体 | — | — |
| 双路同跑（同一颗 sensor） | ✅ | video0 **2381 帧/20.004 s = 119.02 fps**、video4 **2381 帧/20.002 s = 119.04 fps**，`timeouts=0`，`lost_cnt` 0/1，dmesg 0 错 | 需要 4 s 量级间隔再开第二路更稳 | — |
| DMA 缓冲 | ✅ | 每路 `bkuf cnt: 4 size: 3457024 rest:3`（两路共 ~27.6 MB 常驻） | — | — |
| 同尺寸限制（框架级） | ✅（有限制但有守卫） | video0 出流中对 video4 做 960×600 `S_FMT` → `VIDIOC_S_FMT: failed: Device or resource busy` + `[ERR] video0 is streaming 1920x1200 from the same sensor, refusing the 960x600 S_FMT on video4`；video0 的 `prs_in` 保持 1920×1200 | 同一颗 sensor 的两个节点**只能同尺寸**（`vin_pipeline_try_format()` 把整链按同一尺寸配） | 想在小尺寸取第二路要走 `VIDIOC_S_SELECTION`，**未验证**；本轮该次尝试的**同一次**流后续掉到 38 fps（见 4.3 的偶发性说明） |
| IOMMU | ⚠️ | 所有 VIN 设备（vind/vinc×3/isp/tdm/scaler×3/`vind:scaler@16`）+ VE×2 + DE + deinterlace + DRM + G2D **同属一个 `iommu_groups/0`（16 个设备）** | — | 一个组里的任何设备都能 DMA 到另一设备的内存；也解释了空指针 DMA 的下游后果（0008 只是让错配状态进不来） |

### 1.4 编码 / 解码

| 项 | 状态 | 本次实测证据 | 条件/限制 | 遗留缺陷 |
|---|---|---|---|---|
| H.264 零拷贝 @1920×1200@120 | ⚠️ | `ar0234-rec -w 1920 -h 1200 -f 120 -c h264 -b 20M -n 240` → **240 frames in 2.32 s = 103.29 fps，encode 9.6 ms/frame**，4999224 B | **编码器跟不上 1200p120**（需要 ≤8.33 ms） | 文档「1200p120 最多约 115 fps」本次测到 **103 fps**（内容/码率相关，需按实际场景重标） |
| H.264 @1080p | ⚠️ | `-h 1080 -n 300` → 300 frames in 2.54 s = **118.16 fps**，encode 8.1 ms/frame | 1080p 也要 8.1 ms > 8.33 ms 才刚好够 | 1080p120 也是临界 |
| H.265 零拷贝 | ❌ @1200p120 | `-c h265 -n 120` → **`done: 6 frames in 2.08s (2.89 fps)`**，124202 B；dmesg `[ERR] isp0 configuration error` + `[ERR] isp0 height error` | **只在 1920×1200@120 破** | 这是本轮最明确的"文档高估"：HANDOFF 把 H.264/H.265 一起记 ✅ |
| H.265 其它组合 | ✅ | 1200p60 → 120 帧/2.03 s = **59.00 fps**；1200p30 → 29.84 fps；1080p30 → 29.70 fps；**720p120 → 118.49 fps**（3.9 ms/frame） | — | 1200 高度非 64 倍数（CTU 对齐）+ 120 fps 的组合嫌疑最大，未定位 |
| 拷贝模式（`-C`）对比 | ⚠️ | `-C` 120 帧 → 71.02 fps，encode 13.8 ms/frame（vs 零拷贝 9.6 ms） | 零拷贝快 ~1.5× | — |
| 硬件 JPEG（VE2 零拷贝） | ✅ | `jpeg_test /tmp/snap.jpg 90` → `OK: /tmp/snap.jpg (239725 bytes)`；SOI=`ffd8`、EOI=`ffd9`；自解 SOF0：**precision=8, 1920x1080, components=3** | 走 `VideoEncCreate(VENC_CODEC_JPEG)` + `GetVeIommuAddr` 零拷贝 | 文档写"33KB"是别的质量/内容，本次 q90 是 240 KB —— 写指标时别用固定值 |
| 硬件解码（厂商 demo） | ✅ | `vdecoderdemo -i /tmp/rec_zc.h264 -codFmat 1 -o /tmp/dec_own.out -n 5 -sn 5 -outFmat 1` → `Picture Size: 1920x1200`、`decode frame: 6, display frame: 5` | 必须先 `AddVDPlugin()`；链接 `-lvideoengine` | `-o` 的输出文件本次没落地（`out=NONE`），demo 用途是"能解"的证明 |
| 解码自己的 H.265 | ❌ | 同一 demo `-codFmat 2` → **`Segmentation fault`** | — | 与 H.265 编码在 1200p120 失败同源或在解码侧另有问题，未定位 |
| 解码 t30.h264（厂商样本） | ✅ | `Picture Size: 1920x1088`、decode 6 / display 5 | — | — |
| 为什么必须用厂商 demo | ✅（机制确认） | demo 需要双线程：`strings` 里有 `pthread_create`、`AddVDPlugin`；插件库在 `/usr/lib/aarch64-linux-gnu/libawh264.so`、`libawh265.so`；`dec_test` ldd 只链 `libvdecoder.so` + `libvideoengine.so` | 自研单线程直调会把内核挂死（上一轮记录，**本轮按计划未复现**） | 集成时必须照抄 demo 的「喂流线程 + displayPicture 线程」结构 |

### 1.5 2D / 3D / 计算

| 块 | 状态 | 本次实测证据 | 条件/限制 | 遗留缺陷 |
|---|---|---|---|---|
| G2D 驱动存在 | ✅ | `modinfo g2d_sunxi` → `/lib/modules/6.6.98-sun60iw2/kernel/bsp/drivers/g2d/g2d_sunxi.ko`；`modprobe g2d_sunxi` 后 `g2d_sunxi 94208 0` | — | — |
| G2D **开机不自动加载** | ❌（缺配置） | 开机快照：**`/dev/g2d` 不存在**；`/etc/modules-load.d/ar0234.conf` 只有 `vin_v4l2`；`/etc/udev/rules.d/99-ar0234-camera.rules` 只管 `cedar_dev_ve2` | 必须手工 `modprobe g2d_sunxi` | **T7 的 modules-load + udev 仍未做** |
| G2D 设备权限 | ⚠️ | modprobe 后 `crw------- 1 root root 234, 0 /dev/g2d` | **root only**，`video` 组用不了 | 同上，T7 未做 |
| G2D QUERY_VERSION | ✅ | `g2d version 0x10112114` | — | — |
| G2D BITBLT fd→fd（NV12，`YUV420UVC` 4:2:0） | ⚠️ 可跑但**永不位精确** | 全范围随机数据：Y 差 ~129 500/2 073 600（maxdelta **恰好 16**，即全部是钳位）、UV 差 ~1 025 400/1 036 800；真实 ISP 帧：**UV 0 差异**、Y 514 456 字节被抬到 16 | 第八轮：冲激定位证明色度是**重采样滤波器**（跨 -2..+4 字节、带负瓣），Y 是**下钳 16**；地址/stride（UV 行距 1920 B）经冲激回读确认**正确** |
| G2D NV12 位精确拷贝（两次 `G2D_FORMAT_Y8`） | ✅ | 全范围随机数据与真实 ISP 帧均 `0/3 110 400`（Y 0/2 073 600、UV 0/1 036 800） | 把缓冲看成 1920×1620 单平面，`clip_rect.y=0/h=1080` 取 Y、`y=1080/h=540` 取 UV |
| G2D ARGB8888 → NV12 / NV12 → ARGB8888 | ✅ 但要用 0x28 | `0x28`：红 `Y=76 UV[0]=84 UV[1]=255`、NV12→ARGB 复原 CPU 参考 `R=238 G=14 B=14` 精确；`0x29`：红蓝对调 | `0x28` 才是真实 NV12（U 在偶字节），`0x29` 是 NV21 —— 第六/七轮用的是 0x29 |
| G2D 画框（`G2D_CMD_FILLRECT_H` on NV12） | ✅ | `color=(Y<<16)|(U<<8)|V` 原样写入；矩形 `(100,64,256x64)` 内 16 384/16 384 改动、外 0 | 标注框的安全原语 |
| G2D 2× 缩放 | ✅ | `scale: OK (nonzero bytes 777600/777600)` | — | 未做逐像素比对 |
| GPU 驱动 | ✅ | `pvr/version`：`Rogue_DDK_Linux_WS rogueddk 24.2@6603887`；`pvr/status`：`Driver Status: OK`、`Firmware Status: OK`、`GPU variant BVNC: 36.56.104.183`、`Server Errors: 0`、`WGP/TRP/FWF/CRR/SLR = 0`；`gpu0 = 400 MHz`（devfreq 也在） | — | `APM Event Count: 98`（累积，非本轮错误） |
| GPU OpenCL | ✅ | `cltest2`：`platforms: 1`、`platform: PowerVR`、`version: OpenCL 3.0`、`device: PowerVR B-Series BXM-4-64` | — | — |
| OpenCL 扩展（含 DMA-BUF 导入） | ✅ | `PLATFORM_EXTENSIONS` 里含 `cl_khr_external_memory`、`cl_arm_import_memory`、**`cl_arm_import_memory_dma_buf`**、**`cl_khr_external_memory_dma_buf`**、`cl_img_yuv_image`（NV12 直处理）、`cl_khr_fp16`、`cl_khr_integer_dot_product`、`cl_khr_il_program` | — | — |
| OpenCL 设备级 EXTENSIONS 查询 | ❌（库缺陷） | `clGetDeviceInfo(CL_DEVICE_EXTENSIONS)` 返回 `rc=0` 但 size=**4**、内容 `01 00 00 00`（二进制垃圾） | 必须改查 `CL_PLATFORM_EXTENSIONS`(0x0904) | 确认 HANDOFF T13 的记录 |
| OpenCL 开发头文件 | ❌（缺包） | `/usr/include/CL` 不存在 | 装 `opencl-c-headers` 即可（注意别覆盖厂商 `libOpenCL.so`） | — |
| NPU 内核驱动 | ✅ | `/dev/vipcore`（rw-rw-rw）、`vipcore 245760 0`；`viplite/vip_info` → `pid=0x1000003b, date=0x20230518, ver1=0x9000, ver2=0x9202`；`clk_freq` 支持 492/852/**1008** MHz（当前 1008） | — | debugfs 打印有一条 `seq_file: buggy .next function vipdrv_seq_next` 内核提示（无害） |
| NPU 用户态 API | ✅（**文档说 ❌，实测可用**） | `libVIPlite.so` 确实**全盘不存在**（`find /`），但 **`/usr/lib/libNBGlinker.so` 导出了 `vip_lite.h` 的整套 API**：`vip_init` `vip_destroy` `vip_create_network` `vip_query_network` `vip_prepare_network` `vip_set_input` `vip_set_output` `vip_run_network` `vip_finish_network` **`vip_create_buffer_from_fd`** `vip_create_buffer_from_handle` `vip_map_buffer` `vip_destroy_network` …（共 40 个导出） | 链接 `-lNBGlinker -lVIPhal` 即可；`/usr/include/vip_lite.h` 在位 | 头文件里有 `vip_query_driver_version`，但库里没有该符号（API 版本差异），用前要按 `vip_lite.h` 实际导出核对 |
| NPU 实机推理 | ✅ | `cd /opt/vpm_run && ./vpm_run -s sample.txt -l 1` → `VIPLite driver software version 2.0.3.2-AW-2024-08-30`、`vip lite init OK`、`cid=0x1000003b, device_count=1`、`create network 868 us`、`input dim 224 224 3 1`、**`run time for this network 0: 2848 us`**（`profile inference time=2747us`）、`vpm run ret=0` | 需要 `libNBGlinker.so` + `libVIPhal.so`（都在） | — |
| NPU + 视觉 demo | ✅ | `cd /opt/yolov5 && ./yolov5 model/yolov5.nb input_data/dog_640_640.jpg` → `detection num: 3`：dog 82% [112,233,257,609]、truck 69%、bicycle 47%；重写 `result.png` | 依赖 OpenCV 运行库（板上已装：`libopencv_core/imgproc/imgcodecs 4.5`） | — |
| NPU 模型工具链 | ⬜/⚠️ | 板上**有**可用模型：`/opt/vpm_run/network_binary.nb`（964 KB）、`/opt/yolov5/model/yolov5.nb`（5.0 MB）；`/usr/bin` 里**没有** ONNX/TFLite→`.nb` 的转换工具 | — | 要跑自己的模型仍需厂商 NBG 转换工具（版本对齐 VIPLite 2.0.3.2） |
| NPU ← ISP 零拷贝（真正用途） | ⚠️ 未验证 | `vip_create_buffer_from_fd` 符号确认存在 | — | **没有端到端验证"ISP 640×400 BGR → NPU"**；这是 T19 的下一步 |

### 1.6 时钟与电源

| 项 | 状态 | 本次实测证据 | 条件/限制 | 遗留缺陷 |
|---|---|---|---|---|
| 图像链路时钟 | ✅ | `clk_summary`：**`isp = 324 MHz`**、`csi = 324 MHz`，父都是 `pll-video0-4x`（324 MHz）；`pll-video0 = 1296 MHz`；`pll-video0-3x = 432 MHz` 供 `csi_mclk{0,1,2}_pll` | DT `vind@5800800`：`csi_top = 324000000`、`csi_isp = 324000000`（内核源码树里的 dtsi 写的是 600/540 —— 板级 DTS 覆盖成 324） | **324 是配置选择，不是硅片上限**；单路"刚好够"（162 MHz 必崩） |
| VE/G2D/NPU/GPU 时钟 | ✅ | `ve-enc0 = 624 MHz`（父 `pll-ve1`）、`ve-dec = 624 MHz`、`g2d = 300 MHz`、`npu = 1008 MHz`、`gpu0 = 400 MHz`；cedarc 自报 `set ve freq to 624 Mhz` | VE 频率由 cedarc 用户态设定 | — |
| 用户态改 ISP 时钟 | ❌（只读） | `-r--r--r-- /sys/kernel/debug/clk/isp/clk_rate` | 只能改 DT 或内核代码 | — |
| 硬件复位（ISP） | ❌（缺硬件描述） | `fdtget -t i <dtb> /soc/vind@5800800 resets` → **只有 `18 115`**（= `&ccu RST_BUS_CSI`），第二个 phandle 是空的 `<>`，编译进 DTB 后被丢掉；`reset-names = "csi_ret","isp_ret"` | 驱动：`isp_ret` 拿不到只 `vin_warn("Get isp reset control fail")` 并把 `clk_reset[VIN_ISP_RET] = NULL`；`reset_control_deassert(NULL)` 按设计返回 0 ⇒ **所有 ISP reset 操作都是空操作** | CCU 里存在 `RST_BUS_VIDEO_IN`（`0x1884` BIT(16)）但没人引用 → **"把 ISP 硬复位接上"是可能的**，属可修项 |
| Power domain / runtime PM | ✅/⚠️ | `pm_genpd`：`pd_vi = off-0`（含 `isp`/`csi` 的 iommu master）、`pd_ve_enc = off-0`、`pd_npu = on`；vinc/vind 的 `runtime_status = suspended` | 空闲能正确下电 | **`5900000.isp`/`5908000.tdm`/`5910000.scaler` 的 runtime PM 是 `unsupported`**；`vin_core_runtime_suspend/resume` 是空桩，且**全驱动没有 `pm_runtime_use_autosuspend`** |
| 热 | ✅ | 空闲时 `cpub 48.7 ℃`、`cpul 48.9 ℃`、`ddr 47.9 ℃`、`npu 48.4 ℃`、`gpu 46.8 ℃`、`skin 33.8 ℃`（满负载 1200p120 长期热表现见 T11，本轮未做长稳） | — | — |

### 1.7 内存 / 带宽 / IOMMU

| 项 | 状态 | 本次实测证据 | 条件/限制 | 遗留缺陷 |
|---|---|---|---|---|
| `mpp/vi` CSI Bandwidth | ❌（结构性恒 0） | 单路、双路、空闲三种场景下 `CSI Bandwidth: 0`；`CSI Bandwidth total`/`ISP Bandwidth total` 两行**根本不打印** | — | 三处原因（整数截断、`VIN_MAX_DEV-1` 永不成立、`ISP_600` 下带宽函数被 `#if` 排除）见 `analysis/t17/REPORT-0916-isp-capacity.md` §3.3；**本轮确认仍未修** |
| `mpp/vi` 其它计数 | ✅ | `prs_in hb/hs`、`bkuf cnt/size/rest`、`frame cnt/lost_cnt/error_cnt`、`internal avg/max/min` 全是真数据 | **必须出流时读**（空闲时 CSIC/VIPP 时钟被门控） | — |
| DDR 带宽计数器 | ❌（不可用） | `/sys/class/devfreq/` 只有 `1800000.gpu`(400 MHz) 与 `3600000.npu`(1008 MHz)，**没有 DDR/DMC devfreq 设备** | — | 想拿真实 DDR 带宽只能改内核放开那三处，或用 DMC 寄存器 |
| DMA-BUF | ✅ | 出流中 `dma_buf/bufinfo` 有 6 个 `size 00368640 (3 573 312 B)` 对象，`exp_name=system`；`/dev/dma_heap/{system,reserved}` 在位 | — | `bufinfo` 的 "Attached Devices" 显示的是历史 attach（`1c0e000.ve`），读数时别误判 |
| IOMMU 组 | ⚠️ | `/sys/kernel/iommu_groups/0`：`1c0e000.ve 1c10000.ve2 5000000.de 5400000.deinterlace 5440000.g2d 5800800.vind:scaler@16 5830000.vinc 5831000.vinc 5832000.vinc 5900000.isp 5908000.tdm 5910000/5910400/5910800.scaler soc@3000000:sunxi-drm soc@3000000:ve1@1c0e000`（16 个） | 加载 `g2d_sunxi` 会把 `5440000.g2d` 加进同一组 | 一个组 = 无隔离；也是 IOMMU 空指针 DMA 那类问题的最坏后果面 |
| IOMMU 空指针 DMA | ⚠️（未再复现） | 全 session `is not mapped = 0`、`Bug is in = 0`、`Unable to handle = 0` | 0008 的三道闸门（共享上行尺寸预检/拒绝 + STREAMON 尺寸比对）在起作用 | 根因仍在 ISP 侧地址状态机，闸门只是让错配进不来 |
| 内存 | ✅ | 3847 MB 总量，出流中 used 537 MB / available 3220 MB；无 swap 压力 | — | — |

### 1.8 同步与触发

| 项 | 状态 | 本次实测证据 | 条件/限制 | 遗留缺陷 |
|---|---|---|---|---|
| 驱动侧触发/闪光灯代码 | ✅（存在） | 模块参数在位：`trigger_mode=0`、`flash_enable=N`、`flash_delay=0`；`.ko` 字符串：`[%s]trigger mode %d, flash %d`、`parm=flash_enable:drive the FLASH pin high during exposure`、`parm=flash_delay:flash lead (<0) / lag (>0) in ~3.4us steps, -128..127` | — | 只有符号与参数，**没有实测路径** |
| video 节点闪光灯控件 | ✅（存在） | `vin_flash_ctrl 0x00981967 (menu) min=0 max=2`、`led_mode 0x009c0901 (menu) min=0 max=4` | — | 未驱动任何硬件 |
| 引脚/接线 | ⬜ **缺硬件** | DT：`sensor@5812000` 有 `flash_handle`、`sensor0_reset` **为空**、`sensor0_pwdn = PE6`、`sensor0_mclk_id=1`、`sensor0_twi_addr=32`；可用 PWM：`pwmchip0/10/20` 各 10 路；GPIO：`gpiochip0`(2000000.pinctrl)/`gpiochip352`(7025000.pinctrl) | TRIG 是 **1.8 V** 电平，要电平转换 | **需要**：40pin 上 PWM/TRIG/FLASH 的实际映射（原理图）+ 电平转换板 + 模组原理图（PWDN/RESET 真脚位） |
| 从机同步 / XVS-XHS | ⬜ **缺硬件** | `VI_IOCTL_SYNC_CTRL` 在驱动里存在，但 zero3w 没引出同步引脚（见 `hwapi/README.md` §1） | — | 需要能引线的板子 |
| 外部不定时帧 | ⬜ | 未做 | — | 依赖上面两项 |

### 1.9 ISP 标定现状

| 项 | 状态 | 本次实测证据 | 条件/限制 | 遗留缺陷 |
|---|---|---|---|---|
| BLC | ⚠️ 有值未重标 | `make_isp_bin.py --black 42`（10-bit），参数体里 14 个 ISO 档都写 `-42` | — | 未做暗场扫描验证 |
| CCM | ⚠️ 第三方值 | 生成时打印 `ccm 2800K [422,-78,-88,...]`、`4000K`、`6500K`（来自 `tuning-ref/kurokesu_libcamera_vc4_ar0234.json`） | 这是 **Kurokesu 模组的 libcamera 调参**，不是本模组+本镜头标定 | 需要用色卡重标 |
| AWB | ⚠️ 第三方值 | 同上，`awb_light 3..8: CT 3000/4000/4200/5000/6500/7500` 的 R/G、B/G 表被写入 | 同上 | 需要用不同色温光源重标 |
| LSC / MSC | ❌ 未标定（关着） | 部署参数里 `lsc=0`、`msc=0`（文件 162..193 逐字节读出） | 偏移已知（结构体 3130 起 12×768 u16，触发色温 21562，MSC 21574/21632），`tools/calibrate_lsc.py` + `make_isp_bin.py --lsc-json` 工具链齐 | **缺实拍平场**（本轮未做）；且 `lsc_center` 的 (2048,2048) 与通道顺序 R/G/B 仍是假设 |
| sharp / DRC / PLTM / defog / LCA / GCA / CNR / CEM / gamma | ⚠️ 全开、未标定 | 见 1.2 的开关表（全为 1） | — | 工业用途应该关掉其中大部分（T4 未做）；gamma 表偏移(结构体 56480)已知但没生成线性 gamma |
| 3DNR（tdf） | ⚠️ 可用但有硬限制 | 1200p120：驱动 `3DNR forced off, sensor vblank 136 us < 500 us`；切到 no3dnr 参数集（文件偏移 175 = 0） | 只有消隐 ≥500 µs 的模式能开（1200p60、1080p30、720p120 可以） | `PKG` 模式报 width error、`COMPRESS_EN` 会 IOMMU fault（HANDOFF §3.8，本轮未复测） |

### 1.10 稳定性面的已知坑（逐条复验）

| 坑 | 文档说法 | 本次实测 | 判定 |
|---|---|---|---|
| `rmmod vin_v4l2` 不可能 | 循环 refcount | `rmmod vin_v4l2` → **`rmmod: ERROR: Module vin_v4l2 is in use`**；`lsmod`：`vin_v4l2 … 1`，`vin_io 61440 3 ar0234_mipi,vin_v4l2`（互引） | ✅ 确认 |
| 只重载传感器模块的后果 | 0007 修了 panic，但管线要重启 | `rmmod ar0234_mipi` **成功** → 之后 `S_PARM 1/120 failed: Invalid argument`、`S_FMT: Invalid argument`，dmesg：`sensor ar0234_mipi: no link to sunxi_mipi.0, cannot (dis)able it` + `sensor setup link failed`（×4）、`scaler get_selection error`（×2）、`ar0234_mipi is not used, video0 cannot be close!`。**没有 panic**（0007 生效）。重启后恢复 | ✅ 确认（"要重启"这一半也确认） |
| 出过流后 ISP 出错必须重启 | §3.2「ISP 出过错之后…先重启再测」 | **反例**：1080p@136 打出 400 次 `frame lost` + 400 次 `sunxi_isp_reset`（`ISP frame number is 0` 循环）后，**同一个 boot 里**紧接的 1200p120 → `119.78 fps, 0 超时, 0 丢帧`；1200p30 → `29.95 fps` 干净 | ❌ **过宽**：`frame_lost/reset` 风暴**不需要重启** |
| T6 偶发"一帧不出" | 十几次里 1 次 | **40 次 1200p120@4 s 连续压测：4 次坏启动（10%）**。签名极稳定：`frames=4 wall=4.048s fps=0.99 timeouts=4`（就是 4 帧后停）+ `vi0 frame cnt` 复位为 0 + dmesg **0 条** `frame lost`。**紧接着的下一次一定正常**（3 次重试全 `119.5/119.8 fps`） | ⚠️ **频率被低估（10%），但"必须重启"被推翻** |
| T16b 第二节点 open/close 拖慢第一路 | 0009 已修复并验证 | **s11：12/12 干净**（含 8 次 `open(O_RDWR)+close(/dev/video4)`，`vi0` 全程 119–120 fps、0 超时、0 新 dmesg）。**但另有两起独立发作**：s6 一次在 touch 后 `vi0` 从 907 冻住（该流 30.37 fps / 23 超时）；s10 一次迭代 1 干净、**迭代 2–10 连续 9 个 20 s 流完全没有帧**（`vi0` 冻在 2361、vfr 没有 RESULT），几分钟后自愈 | ⚠️ **不是"已修复"，是"偶发、概率大幅下降"**（11 次干净 / 2 次发作） |
| `Runtime PM usage count underflow` | 0009 已修（0 次） | **本 boot 又出现 1 次**，伴生一条新的内核 WARNING：`WARNING: CPU: 6 … drivers/regulator/core.c:3012 _regulator_disable` + `[ERR] vin_pin_disable: disable vind_mclkpin error, fail to disable regulator!` + `sunxi-vin-core 5831000.vinc: Runtime PM usage count underflow!` | ❌ **未根除**；且暴露一个新的 regulator 引用计数问题（`vind_mclkpin` 被 disable 时未使能） |
| `isp01` 绝对不要开 | 机制未定位（候选：寄存器窗口 4 字节错位别名） | **按约束本轮没有 enable 它**。只读旁证：出流中 `0x5900000 = 0x00000005`（ISP 寄存器），而 `0x58ffffc = 0x00000000`、`0x58ffff8 = 0`、`0x5901300 = 0` ⇒ ISP 块从 `0x5900000` 开始，`0x58ffffc` 处不是 ISP 寄存器 | ⚠️ **候选机制被旁证支持，仍未直接观测**；`of_iomap` 取区间起点 ⇒ isp01 每次读写都低 4 字节 的解释与此一致 |
| 空 resets 占位 | §3 / T16「`resets` 是空占位」 | DTB 实测 `resets = <18 115>`（只有 1 项）；驱动侧 `reset_control_deassert(NULL)` 恒 0 | ✅ 确认（且 CCU 里有未用的 `RST_BUS_VIDEO_IN` ⇒ 可修） |
| panic → 16 s 看门狗自动复位 | RuntimeWatchdogSec=16s | `sunxi-wdt 2050000.watchdog: Watchdog enabled (timeout=16 sec)`；`/etc/systemd/system.conf` = `RuntimeWatchdogSec=16s`；`panic_on_oops=1` | ✅ 配置在位（本轮没触发 panic） |

---

## 2. 深度思考

### 2.1 缺陷分类

**① 驱动/框架 bug（应该能修）**
| 缺陷 | 判断依据 | 证据等级 |
|---|---|---|
| 流启动偶发"4 帧后卡死"（原 T6） | 40 次 4 次、签名一致、**下一次必好** ⇒ 确定性很强的状态机 bug，不是随机硬件问题 | 复现 |
| `Runtime PM usage count underflow` 未根除 + `vind_mclkpin` regulator WARN | 同一 boot 各出现 1 次，且 `vin_pin_disable` 失败是新证据 | 观察 |
| 第二节点 open/close 偶发拖死第一路（原 T16b） | 两次独立发作（一次冻结、一次 9 连死），恢复靠时间 | 复现 |
| `LC21`(LBC) 输出被接受却 0 帧、内核静默 | S_FMT 成功、`vi0 output fmt: LBC_2X`、`frame cnt 0`、0 报错 | 确认 |
| H.265 @1200p120 触发 `isp0 configuration error/height error` | 唯一失败组合，其它分辨率/帧率全好 | 复现 |
| 自研单线程解码会挂内核 | 上一轮记录；本轮确认 demo 是双线程结构、插件机制 | 观察（本轮未复现） |
| 设备级 `CL_DEVICE_EXTENSIONS` 查询返回 4 字节垃圾 | `rc=0` 但 size=4、内容 `01 00 00 00` | 确认 |
| `mpp/vi` 三处计数器恒 0（含 `VIN_MAX_DEV-1` 永不成立） | 源码 + 三场景一致为 0 | 确认 |

**② 配置/设备树问题（改配置能解决）**
| 缺陷 | 依据 | 证据等级 |
|---|---|---|
| `G2D` 不开机加载、`/dev/g2d` 只 root 可访问 | `/etc/modules-load.d/ar0234.conf` 只有 `vin_v4l2`；udev 规则只管 `cedar_dev_ve2` | 确认 |
| ISP 时钟锁在 324 MHz（单路"刚好够"） | DT `csi_isp = csi_top = 324000000`；源码树 dtsi 是 540/600 | 确认 |
| TDM 是 online ⇒ 多路进一块 ISP 的前提（offline）未启用 | DT `work_mode = 0`；online 模式驱动明确 `can not be open again!` | 确认 |
| `resets` 第二个 phandle 空 ⇒ ISP 无硬复位 | DTB 只有 `18 115`；驱动 `clk_reset[VIN_ISP_RET] = NULL` | 确认 |
| 工业固定模式没有配置文件 | `/etc/ar0234.conf` 不存在；T3 未做 | 确认 |
| 工业 ISP 参数集没做（defog/LCA/GCA/sharp/DRC/PLTM 全开） | 参数开关表逐字节读出 | 确认 |

**③ SoC 硬件限制（只能接受或换硬件）**
| 限制 | 依据 | 证据等级 |
|---|---|---|
| 全片只有 **一块 ISP602**（`isp 1`），`isp01/02/03` 是虚拟别名 | `mpp/vi` 自报 + isp01 的 reg 起点比 isp00 低 4 字节 | 确认 |
| ISP 输入侧**没有缓冲**，是逐行实时流 + 行消隐 ≥128 ISP 周期 | `hblank short` 中断 + 3DNR 的 per-line 亏欠模型 + `ISP_ONLINE` | 确认 |
| 1200 高度非 64 倍数 + 120 fps 时 H.265 编码破 | 唯一失败组合 | 复现 |
| 1200p120 编码吞吐 ~103 fps < 120 fps | 多次一致（9.6/9.4/8.9 ms/帧） | 复现 |
| 同一颗 sensor 的两个 video 节点只能同尺寸 | `vin_pipeline_try_format()` 把整链按同一尺寸配 | 确认 |
| 所有 VIN/VE/DRM/G2D 在同一个 IOMMU group | sysfs 16 个设备同组 | 确认 |
| 没引出同步/触发引脚、TRIG 是 1.8 V | `hwapi/README.md` + DT 里 `sensor0_reset` 为空 | 观察 |

**④ 缺厂商 SDK/资料**
| 缺的东西 | 影响 | 证据等级 |
|---|---|---|
| ONNX/TFLite → `.nb` 模型转换工具（版本需配 VIPLite 2.0.3.2） | 只能用板上自带的两个 nb（224×224 分类、yolov5） | 确认 |
| ISP602 数据手册/带宽表 | 单相机下无法把"两路需要多少 MHz"从外推变成结论 | 观察 |
| 模组原理图（PWDN/RESET/TRIG/FLASH/MCLK 真脚位） | 第二颗模组与触发接线的**唯一**阻塞 | 观察 |
| G2D 用户态库/文档 | 只能直接 ioctl；位精确性**分路径**：ARGB8888(alpha=0xFF) 与 两次-Y8 的 NV12 拷贝位精确，单次 4:2:0 `YUV420UVC` 永不位精确 | 确认（第八轮机制级） |
| OpenCL 开发头文件 | 需要 `apt install opencl-c-headers` | 确认 |

**⑤ 缺硬件/接线**
| 缺的东西 | 影响的项 |
|---|---|
| 第二颗 AR0234 模组 + MIPI-B 排线（cci 9、PE10） | 双相机、ISP 时分吞吐、offline TDM 全部无法验证 |
| 触发/闪光灯接线（含 1.8 V→3.3 V 电平转换） | T18 全部 |
| UVC gadget / USB 网卡（图像外传） | T22 |
| 黑白版 AR0234（ID 0x1A56） | Y8/Y10 + ISP bypass |

### 2.2 哪些是"必须接受"的

1. **一块 ISP602，输入侧零缓冲。** 这是硅片和 ISP 工作模式（`ISP_ONLINE`）的既定事实。单相机 1200p120 在 324 MHz 下**不宽裕**：162 MHz 立刻崩（`hblank short`），所以"再去调低 ISP 时钟"这条路不存在；反过来，想给两路留余量必须改 DT 的 `csi_isp`。
2. **厂商 dtsi 的 `<>` 空 resets ⇒ 没有 ISP 硬复位。** 结果就是"ISP 出问题只能软件重开流或重启"。注意这一条**只在特定故障下才咬人**：本轮证明 `frame_lost` 风暴不需要重启（见 2.3），真正需要重启的是"只重载传感器模块"这一类（link 只在 probe 时建）。
3. **1200p120 的编码吞吐 ~103 fps（H.264）。** 传感器的 120 fps 和 VE 的 8.9–9.6 ms/帧是硬的。要真 120 fps 录像，只能降分辨率（1080p 118 fps 临界）或降帧率，或接受丢帧。
4. **H.265 在 1920×1200@120 不可用。** 根因未定位，但可绕（用 H.264，或 1200p60/720p120 用 H.265）。定焦工业相机场景 H.264 通常够。
5. **同一颗 sensor 的两个 video 节点只能同尺寸。** 想"全分辨率 + 小尺寸预览"必须走 `VIDIOC_S_SELECTION`（未验证）或改框架。
6. **IOMMU 无隔离（16 个设备一组）。** 这是 BSP 的组划分方式，改它风险远大于收益。
7. **`mpp/vi` 的带宽字段恒 0、没有 DDR devfreq。** 想量化 DDR 带宽只能动内核代码。
8. **NPU 的模型转换工具链。** 板上能跑，但换自己的模型需要厂商工具——这不是软件能补的。

### 2.3 哪些是"应该能修但还没修"（按 收益 ÷ 风险 排序）

| 序 | 项 | 收益 | 风险 | 下一步最小验证动作 |
|---|---|---|---|---|
| 1 | **G2D 开机加载 + 权限**（modules-load + udev，T7 剩余） | 中高（否则每个部署都要手工 modprobe；非 root 根本用不了） | **极低**（两行配置 + 重启） | 写 `/etc/modules-load.d/g2d.conf` + `KERNEL=="g2d", GROUP="video", MODE="0660"`，重启后以 `orangepi` 用户跑 `tools/g2d_test.cpp` |
| 2 | **流启动"4 帧卡死"（10%）** | 高（工业相机最怕偶发启动失败；一行重试就能兜住） | 低（先做用户态重试，不动内核） | 在采集库里加"开流后 N ms 无帧即 STREAMOFF→重开（最多 2 次）"，用 40×4 s 循环验证 0 残留 |
| 3 | **把"ISP 出错就得重启"改写成精确规则并写进 README/HANDOFF** | 高（避免无谓重启、避免误判"驱动坏了"） | **零**（文档） | 复述本轮两个反例即可 |
| 4 | **T16b 的偶发复发复核** | 中高（否则用户会遇到"偶发不出图"） | 低（只读+压测） | 跑 30 次"20 s 流 + touch 第二节点"，记录冻结次数；若 ≥3 次，用 0009 之前/之后对比定位 |
| 5 | **`resets` 指向 `RST_BUS_VIDEO_IN`** | 中（ISP 出错后有可能软件复位救回，而不必重启） | 中（改 DT + 可能引入 probe 竞态；必须**一次只动一个变量**、串口+看门狗兜底、准备好 DTB 回退） | 先在**不改 DT** 的前提下验证 `RST_BUS_VIDEO_IN` 是否真的连到 ISP（用 `vinreg` 在出流中读/写 `0x2001884` 的 bit16 看行为），确认了再考虑 DT |
| 6 | **LSC/MSC 标定**（T20 剩的一步） | 高（暗角是工业检测里实打实的误差源） | 低（只换参数文件，可回退） | 拍一张平场（均匀光源/积分球），跑 `tools/calibrate_lsc.py` → `make_isp_bin.py --lsc-json`，然后**开关对比**同一场景的角部亮度 |
| 7 | **工业 ISP 参数集**（T4：关 defog/LCA/GCA/sharp/DRC/PLTM + 线性 gamma） | 中高（定焦固定曝光场景里这些块只会破坏线性度/复现性） | 低（参数文件可回退） | `make_isp_bin.py --set defog=0 --set lca=0 --set gca=0 --set sharp=0 --set pltm=0 --set drc=0` 出 `isp_param_industrial.bin`，做曝光扫描看线性度（相关系数） |
| 8 | **H.265@1200p120 的根因** | 中（能省一半码率） | 低（诊断） | 只改一个变量：把 1200 改成 1216/1280 高度试编码；再固定高度只改 fps=119/118；看 `isp0 height error` 是否随高度对齐消失 |
| 9 | **`underflow` + `vind_mclkpin` regulator WARN** | 中（涉及电源引用计数，可能是更广的 PM 漏洞） | 中（动 PM 代码，需重启验证） | 在 `vin_pin_disable` 附近加一次"使能计数不匹配就打 ERR"的只读诊断，先定位是谁多减了一次 |
| 10 | **`CSI Bandwidth` 三处（截断 / `VIN_MAX_DEV-1` / `ISP_600` 排除）** | 中（两路场景的 DDR 压力需要它） | 中（改内核代码；`#if` 放开可能带出未编译代码） | 先只改截断那一处（`1000*1000/frame_internal`），重启看数值是否合理 |
| 11 | **`LC21` 静默 0 帧** | 低（工业场景不用 LBC 输出） | 低 | 在 `sunxi_scaler` 对 LBC 输出格式直接 `-EINVAL` 并打 ERR，或补上 enable |
| 12 | **`isp01 = okay` 的破坏机制** | 低（都已知"不要开"） | **高**（一 enable 就毁第一路，且只能靠重启/断电恢复） | 见 HANDOFF T16c；**建议保持不动**，只在确实需要四路 ISP 时再查 |

### 2.4 相互依赖关系

```
AR0234 ─I2C(cci)─┐
                 ├─ MCLK(pll-video0-3x=432M → 24M) ─┐
   PWDN=PE6 ─────┘                                 │
                                                   ▼
MIPI-A PHY(4 lane,844Mbps,RAW10) → CSI300_500 → TDM_rx0(online) → ISP602(324MHz,online)
                                                                        │
                          ┌─────────────────────────────────────────────┼──────────────────────┐
                          ▼                                             ▼                      ▼
                  sunxi_h3a.0 (3A 统计)                        scaler.0 → /dev/video0   scaler.4 → /dev/video4
                          │  ▲                                        │  (vipp0)              │ (vipp4)
                          │  │ V4L2_EVENT_FRAME_SYNC                 │                       │
                          ▼  │                                        └───────┬───────────────┘
                  ar0234-3ad ─── libisp(AWIspApi) ── 参数文件 /mnt/extsd/isp_param_config.bin   │
                     (每路一个 libisp 会话；无硬复位可救)                                      │
                                                                                             ▼
  依赖关系（谁离开谁不成立）                                                      DMA-BUF(EXPBUF, bkuf 4×3.457MB)
  ① 3A/固定曝光 → 必须先有流（libisp 要 sensor 管线已绑定）                          │
  ② 零拷贝编码/JPEG/G2D/OpenCL/NPU → 依赖 DMA-BUF（V4L2 EXPBUF 或 dma_heap）        │
  ③ 零拷贝编码 → 依赖 ISP 输出的 NV12（ISP 挂了就没有输入）                          │
  ④ 第二路 video4 → 依赖同一条 mipi/csi/tdm/isp 上行 ⇒ 尺寸必须相同                 │
  ⑤ 改尺寸/S_FMT → 必须先停另一路（否则 -EBUSY，或者更糟：静默改坏上行）             │
  ⑥ G2D 用 capture 缓冲做 dst → 必须先停流（否则 vin 还在往同一缓冲 DMA，出现“漂移”）│
  ⑦ ISP 硬复位 → 依赖 DT 里把空的 resets 补上（当前不存在）⇒ 出错后只能停流重开      │
  ⑧ offline TDM / 两路不同源 → 依赖第二颗模组 + DT work_mode=1 + csi_isp 抬高        │
  ⑨ NPU ← ISP 零拷贝 → 依赖 vip_create_buffer_from_fd + (已经具备的) libNBGlinker  │
```

**三条"必须先做"的顺序约束**（踩过才知道）：
1. **开 ISP 前先限帧率**（`v4l2-ctl -d /dev/v4l-subdev0 -c frame_rate=…` 或 `S_PARM`）—— 不限就是满速跑，ISP 每帧丢帧。
2. **改第二路尺寸前先停第一路** —— 0008 会拒绝，但拒绝之前的上行预检本身也可能扰动（见 4.3）。
3. **先插模组再 enable sensor 槽位节点** —— 反了就是 probe 阶段 panic + boot loop（本板 16 s 看门狗能拉回，但需要断电保底）。

### 2.5 单相机场景下真正会咬人的（Top 5）

1. **开流偶发"只出 4 帧就死"（10%）** —— 这是唯一一条**在单相机最简用法下都会随机命中**的问题（40 次里 4 次）。所幸**重开流即好、不需要重启**，所以正确对策是把"开流后 X ms 无帧 ⇒ STREAMOFF/重开（≤2 次）"写进采集库，而不是去查内核。**优先级最高。**
2. **"ISP 出错要重启"这条错误认知** —— 会让运维/上层程序在本来能自愈的情况下白白重启（甚至断电）。实际规则：`frame_lost/reset` 风暴**停流重开即可**；只有"只重载了传感器模块"这种 link 丢失才必须重启。
3. **G2D 不可用（默认没加载 + 只有 root 能开）** —— 用户要在 BGR 上画框/裁 ROI 时才会发现 `/dev/g2d` 根本不存在。修它只要两行配置。
4. **H.265@1200p120 直接失败** —— 如果用户按"1200p120 + H.265"这个组合去搭（很自然的组合），会得到 6 帧的坏文件和一条 `isp0 configuration error`，容易误以为驱动坏了。
5. **LSC/MSC 关着 + defog/LCA/GCA/sharp 开着** —— 这直接决定"RGB 图的可用性"：暗角未校正、边缘有额外处理，做测量/比对类算法时不可复现。用户的目标是"定焦、固定曝光、要 RGB 图"，**第 5 条才是这个目标的核心缺口**，只是它不像前四条那样会"报错"。

### 2.6 证据等级标注

| 等级 | 含义 | 本轮条目 |
|---|---|---|
| **确认** | 多场景一致 + 源码/sysfs 对得上 | 链路每级在位、格式矩阵（除 LC21/FC21）、双路 119+119 fps、`rmmod vin_v4l2` 不可能、DSB md5/模块 srcversion、ISP 时钟 324 与 DT 来源、ISP 无硬复位（源码）、IOMMU 单组、`CSI Bandwidth` 恒 0、G2D 版本/权限、OpenCL 扩展、NPU 三种（驱动/API 符号/实跑推理）、mpp 计数语义 |
| **复现** | 本轮亲自触发过，含失败原文 | T6 坏启动 4/40（10%）、1080p@136 风暴、风暴后可无重启恢复、`LC21` 0 帧、H.265@1200p120 失败、H.265 解码 segfault、G2D 4:2:0 blit 永不位精确（重采样滤波 + Y 钳 16，非缓存竞态）、两次-Y8 的 NV12 拷贝 0 差异、只重载传感器模块后的 `no link` |
| **观察**（单次/不可控） | 只见到一次或只能在特定时机看到 | `underflow` + `vind_mclkpin` regulator WARN（各 1 次）、T16b 两次发作、`max: 33 ms` 的调度毛刺、`temperature_approx_degc` 恒 0 |
| **候选**（有依据未直接测） | 机制推理 + 旁证 | `isp01` 的 4 字节窗口错位别名（旁证：`0x58ffffc` 不是 ISP 寄存器）、H.265@1200p120 的根因、`FC21` 载荷实为 NV12、1080p 悬崖在 3A 曝光上限而非传感器 |
| **外推** | 由实测区间推出来的数字 | 两路 1200p120 需要 ≥600 MHz ISP 时钟（`analysis/t17/…capacity.md` §2.3） |
| **文档记载 / 本轮未验证** | 不能当已确认用 | 单线程解码挂内核（刻意未复现）、触发/闪光灯实测、`VIDIOC_S_SELECTION` 小尺寸第二路、两路 DDR 带宽、4 h 长稳、NV12→G2D→NPU 端到端零拷贝 |

---

## 3. 与文档不符 / 文档已过期之处

| # | 文档 | 文档说法 | 本轮实测 | 性质 |
|---|---|---|---|---|
| 1 | `HANDOFF.md` §1 | vin 模块 = **0008**（`9899a15c…`/`659707E6…`） | 板上实际是 **0009**（`2715c25c…`/`843BC1A03606D35EC062D57`） | 文档落后（§1 没跟着 T16b 一起更新） |
| 2 | `HANDOFF.md` §1「硬件编码」 | ✅ H.264/**H.265** 零拷贝 | H.264 ✅；**H.265 在 1920×1200@120 只出 6 帧 + `isp0 configuration error/height error`** | 高估（缺条件） |
| 3 | `HANDOFF.md` §1「G2D」 | 完全打通、fd→fd BITBLT **字节级精确（0 差异）** | 213834/3110400 字节不同（**全部 ≤2 LSB，无 >8**）；且 `/dev/g2d` 开机不存在、只有 root 能用 | 高估 + 条件缺失 |
| 4 | `HANDOFF.md` T16b / §3.27 第五轮 | 0009「已修复并验证…119.04 fps / 0 超时 / 0 underflow / 0 新 WARN」 | 11 次干净，但**两次独立发作**（一次冻结第一路，一次让 video0 连死 9 个流）；且 `underflow` 本 boot 又出现 1 次 | 过度乐观 |
| 5 | `HANDOFF.md` §3.20 | 「1920×1080 可用上限是 132 fps，不是 133；133（vts 1100）会从第 4 帧起 frame lost + reset 无限循环」 | 请求 **130/132/133/134/135 全部干净**；**136 才崩**，且 134/135 与 136 编制的是同一个 `fll=1096` | **错**（边界不对，且 133 是干净的） |
| 6 | `HANDOFF.md` §3.2 / T6 | 「十几次里有 1 次」开流后一帧不出 | **4/40 = 10%**（签名：4 帧后停 + `vi0` 计数复位 + 0 条 dmesg） | 严重低估 |
| 7 | `HANDOFF.md` §2（坑 2）/§3.1 | 「ISP 出过错之后，下一次测试结果不可信，先重启再测」 | **反例**：400 次 `frame_lost` + 400 次 reset 之后，同 boot 的下一次流 119.78 fps 干净 | 过宽 |
| 8 | `hwapi/README.md` §5 / `HANDOFF` T19 | NPU：**❌ 缺 `libVIPlite.so`**，用户态不全 | **`/usr/lib/libNBGlinker.so` 导出全套 `vip_*` API**（含 `vip_create_buffer_from_fd`），且 `vpm_run` 实跑推理成功（2747 µs）、`yolov5` demo 认出 dog 82% | **错**（可从 ❌ 改 ✅） |
| 9 | `HANDOFF.md` §1「硬件 JPEG」 | 实测 1080p 单帧 **33KB** | q90 实测 **239 725 B**（有效 SOF0 1920×1080×3） | 数值过时/条件缺失（质量与内容相关） |
| 10 | `hwapi/README.md` §1 | 「BGR 1920x1200@120 有过**一次**开流后一帧不出（十几次里出现 1 次）」 | 同 #6：**10%**，且下一帧流即恢复 | 低估 |
| 11 | `HANDOFF.md` §1「硬件编码」 | 「1200p120 实测 **115.7fps（编码器上限）**」 | H.264 实测 **103.29 fps**（9.6 ms/帧） | 数值过时（与内容/码率有关，需按实际重标） |
| 12 | `HANDOFF.md` §3.8 | 3DNR 在 1920×1200@120 每帧丢帧；PKG 模式报 width error；COMPRESS_EN IOMMU fault | 前半条**已由 0006 的互锁变成自动关**（`3DNR forced off, sensor vblank 136 us < 500 us`）；后两条本轮未复测（不主动踩） | 部分过时（0006/0009 之后行为变了） |
| 13 | `HANDOFF.md` §1「板子安装状态」 | 「板子停在需要断电重启的状态」 | 板子健康：1200p120 120.03 fps / 0 超时；DTB 原样 | 过时 |
| 14 | `baseline`/`ARCHITECTURE.md`（2026-09-15） | 早于 0005–0009 / 双路 / JPEG / G2D / NPU | 未更新 | 过时（见 `BASELINE.md`） |
| 15 | `analysis/libisp-offsets/offs.txt` + `tools/make_isp_bin.py` | 偏移以结构体表述，**未说明文件头部 74 字节** | `isp_param_config.bin` = 74 B 头（4B 长度 + 20B 日期 + 50B note）+ 116284 B 结构体；**文件偏移 = 结构体偏移 + 74**（已用 tdf: 结构体 101 ↔ 文件 175 双向验证） | 文档缺陷（不是工具错） |
| 16 | `hwapi/README.md` §4 | 「G2D…`/dev/g2d` 只有 root 能访问 → 要在 modules-load 加上模块 + udev 规则」 | 至今**仍未做**（T7 剩余），`modprobe` 后仍是 `crw------- root root` | 未完成（不是错） |

---

## 4. 改动清单 / 回退 / 板子状态 / 遗留风险

### 4.1 改动清单

**主机 TL101（`/home/helios/Desktop/orangepi-build/ar0234-port`）—— 只新增，未改任何既有文件，未 git commit**

| 路径 | 说明 |
|---|---|
| `analysis/hardware-audit/REPORT-0916-hardware-audit.md` | 本报告（即 `analysis/REPORT-0916-hardware-audit.md` 的副本/正本） |
| `analysis/hardware-audit/s1..s17.out` | 本轮 18 组实验的**原始输出**（含失败原文） |
| `analysis/hardware-audit/s1..s17.sh` | 本轮所有测试脚本（可重跑） |
| `analysis/hardware-audit/reproA.out`, `diag.sh` | 用仓库自带 `tools/t17-repro-close.sh` 的复现记录 + 一次现场诊断 |
| `HANDOFF.md` | 追加「第六轮」小节 + §3.28b 订正块 + 修正 §1 的模块版本/编码/JPEG/G2D 四行；备份 `HANDOFF.md.bak-pre-audit`（**未 git commit**） |
| ⚠️ 注意 | 仓库 `.gitignore` 第 8 行 `/analysis/*` 是**白名单**模式，`analysis/hardware-audit/*.out` 这类日志**不会进 git**（与 `analysis/t17/*.out` 一致）；它们留在磁盘上，要长期保存需手工 `git add -f` |

**板子（Orange Pi Zero 3W）—— 零持久改动**

| 路径 | 动作 |
|---|---|
| `/boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb` | **未动**，md5 `d4ee5b6869e7b7cd39ce3762d0e078f9` |
| `/lib/modules/6.6.98-sun60iw2/updates/vin_v4l2.ko` | **未动**，仍是 0009（`2715c25c…`） |
| `g2d_sunxi` | 只在本轮为测试 `modprobe` 过，**已 `rmmod` 还原**（开机态本来就没加载） |
| `/tmp/*`（s1..s17 脚本、`openclose`、`vinreg`、`nputry`、`tune.bin`、测试码流） | tmpfs，重启即清空 |
| `/mnt/extsd/isp_param_config.bin` | **内容未被我改写**（mtime 变化是 `ar0234-3ad` 按流切换 3dnr/no3dnr 参数集的正常行为） |
| `/etc/`, systemd, udev, clock, `d3d_min_vblank_us` | **未动**（`d3d_min_vblank_us` 保持 500） |

### 4.2 回退方法

```sh
# 1) 万一需要回退模块（本板 rmmod vin_v4l2 不可能，必须靠重启）
tools/ssh_board.sh "printf ' \n' | sudo -S -p '' sh -c 'cp \
  /lib/modules/6.6.98-sun60iw2/updates/vin_v4l2.ko.bak-0008 \
  /lib/modules/6.6.98-sun60iw2/updates/vin_v4l2.ko; depmod -a'" </dev/null
tools/ssh_board.sh "printf ' \n' | sudo -S -p '' systemd-run --on-active=2 /bin/systemctl reboot" </dev/null

# 2) 万一 DTB 被改（本轮没改）
tools/ssh_board.sh "printf ' \n' | sudo -S -p '' sh -c 'cp \
  /boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb.pre-0008 \
  /boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb; sync; md5sum \
  /boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb'" </dev/null   # 期望 d4ee5b68…

# 3) 本轮的测试脚本/二进制都在板上 /tmp（tmpfs），重启即消失，无需清理
```

### 4.3 板子当前状态（收工时实测）

```
uptime            597 s（干净重启后）
DTB               d4ee5b6869e7b7cd39ce3762d0e078f9  （原样）
vin_v4l2          srcversion 843BC1A03606D35EC062D57 = 0009
ISP / CSI 时钟     324000000 / 324000000 Hz
d3d_min_vblank_us 500
ar0234_mipi       trigger_mode=0  flash_enable=N  frame_rate=0
ar0234-3ad        active
/dev/g2d          不存在（= 开机态，g2d_sunxi 已 rmmod）
残留进程           仅内核 kworker（无 vfr / v4l2-ctl / ar0234-rec / vpm_run）
健康回归           1920x1200 NV12 @120 → frames=720 wall=6.007s fps=119.86 timeouts=0
```

### 4.4 遗留风险（诚实列出）

1. **T6 的 10% 坏启动没有被修**，只是被量化了。任何"启动一次就假设能出图"的上层程序都有 10% 的失败率。**对策必须做**（重试），否则现场会碰到。
2. **T16b 仍有低概率复发**（本轮 11 干净 / 2 发作）。发作时表现为"video0 完全没有帧"且内核几乎无提示，恢复可能要等几分钟或重启。
3. **`underflow` + `vind_mclkpin` regulator WARN 仍在**（各 1 次/boot）。涉及电源引用计数，可能还有没暴露的分支。
4. **本轮没做长时间稳定性**（T11 的 4 h × 2 组）。热数据只在空闲态。
5. **没有验证"NPU ← ISP 零拷贝"**，所以"NPU 可用"目前只到"能用自带的 `.nb` 和文件输入"这一步。
6. **`isp01 = okay` 依然是禁区**，本轮严格没碰；机制仍是候选。
7. **`FC21` 载荷疑为 NV12**（格式被接受但数据未验证）——若有人真的用它，需要先做一次逐字节判定。
8. **H.265@1200p120 与 H.265 解码 segfault 的根因都没定位**，只知道可绕。
9. **触发/闪光灯、第二颗模组、图像外传**全部停在"缺硬件"，本轮没有制造任何假数据。
10. 本轮**没有改内核树、没有跑 `apply.sh`、没有 git commit**；`analysis/hardware-audit/` 里是新增文件，不影响既有流程。

---

## 附录 A：本轮实验索引（原始输出在 `analysis/hardware-audit/`）

| 脚本 | 内容 | 关键结果 |
|---|---|---|
| `s1.sh/.out` | 只读基线：设备节点、media 拓扑、video0/4 控件、`mpp/*`、时钟、IOMMU、dma-buf、热、服务、参数、dmesg | 发现 `/dev/g2d` 缺失、`mpp/isp`+`mpp/ve` 不存在、21 实体拓扑 |
| `s2.sh/.out` | follow-up：clk 明细、厂商 .so 位置、udev/modules、DT 只读、genpd/runtime PM、dmesg 过滤 | `libisp/libvencoder/...` 在 `/usr/lib/aarch64-linux-gnu`；`resets = 18 115`；ISP runtime PM = unsupported |
| `s3.sh`(batch A/B) | 格式/模式/帧率矩阵（25 个组合） | NV12/BGR3/RGB3/BA10/GREY/NV21/YU12/RGBP ✅；LC21 ❌ 0 帧；YUYV/UYVY/RGB4 EINVAL |
| `s4.sh/.out` | 受控故障：最小尺寸、1080p 阶梯、**风暴后能否不重启恢复**、`rmmod` | 136 崩 → **1200p120 立即恢复**；`rmmod vin_v4l2` = in use |
| `s5.sh/.out` | 只重载传感器模块的后果 + fll 阶梯 | `no link to sunxi_mipi.0`、`S_FMT EINVAL`、无 panic、需重启 |
| `s6.sh/.out` | 重启后证据：开机 dmesg、健康 30 s、1080p 阶梯、**双路同跑**、流中 MIPI PHY、open/close、尺寸锁 | 双路 119.02/119.04 fps；PHY 4 lane 844 Mbps RAW10；`prs_in` 等 |
| `s7.sh/.out` | T16b 控制序列（3 种 arm） | 20 次里含 3 次 touch 全 120 fps 干净 |
| `s8.sh/.out` | 连续流 vs 带间隔流 × 3A 开关（34 次） | 全部干净 ⇒ 无"重启竞态" |
| `s9.sh/.out` | **40×4 s 压测 + 坏启动即时重试** | **4/40 = 10% 坏启动，下一次必好** |
| `s10.sh/.out` | 10×[20 s + touch] | 迭代 1 干净、**2–10 完全没帧**（一次 T16b 发作） |
| `s11.sh/.out` | 3-arm 判别（gap/touch） | **12/12 干净（含 8 次 touch）** |
| `s12b.sh/.out` | VE H.264/H.265/拷贝模式、JPEG、解码往返 | H.264 103.29 fps；**H.265@1200p120 6 帧**；JPEG 239725 B；H.265 解码 segfault |
| `s13.sh/.out` | H.264/H.265 条件矩阵、G2D、OpenCL、NPU 依赖 | H.265 只在 1200p120 破；G2D 版本/差异统计；OpenCL 扩展；`libVIPlite.so` 缺 |
| `s14.sh/.out` | NPU 运行时搜寻、时钟明细、genpd/PM/热、**流中 dma-buf/IOMMU/带宽**、ISP 参数、触发面 | `/opt/vpm_run`+`/opt/yolov5`；IOMMU 组 16 设备；带宽字段恒 0 |
| `s15.sh/.out` | NPU 文档与 demo 用法、参数文件 3dnr/no3dnr 逐字节 diff、寄存器别名只读旁证 | 只差 **文件偏移 175**；`0x5900000=5` vs `0x58ffffc=0` |
| `s16.sh/.out` | 内核 trace 溯源、参数偏移定位、**NPU 实跑**、vinreg | `underflow`+regulator WARN 原文；**`vpm_run` 推理 2747 µs**；`yolov5` 3 个检出 |
| `s17.sh/.out` | `libNBGlinker` 符号表、流中 ISP/CSI/VIPP 寄存器、3A 固定模式、**清理 + 收工状态** | 40 个 `vip_*` 导出；ISP `0x5900000=5`、CSI `0x5800000=0x80000003`；收工健康 119.86 fps |
