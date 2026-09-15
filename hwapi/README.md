# A733（Orange Pi Zero 3W）图像相关硬件底层 API 汇总

整理日期 2026-09-15，内核 6.6.98-sun60iw2（香橙派 BSP），板上实查。

状态标记：✅ 板上实测可用　⚠️ 驱动/库存在，功能未测　❌ 缺失

## 0. 总览

| 硬件 | 设备节点 | 内核驱动源码（`kernel/orange-pi-6.6-sun60iw2/`） | 用户态库 / 头文件 | 状态 |
|---|---|---|---|---|
| 相机采集 MIPI/CSI/ISP/缩放 | `/dev/video0`、`/dev/v4l-subdev*`、`/dev/media0` | `bsp/drivers/vin/` | 标准 V4L2 + `sunxi_camera_v2.h` | ✅ |
| AR0234 传感器 | `/dev/v4l-subdev0` | `ar0234-port/ar0234_mipi.c` | V4L2 subdev 控件 | ✅ |
| ISP 3A 算法 | （经 ISP 子设备） | — | `libAWIspApi.so`/`libisp.so`，`AWIspApi.h` | ✅ |
| VE 编码 H.264/H.265 | `/dev/cedar_dev_ve2` | `bsp/drivers/ve/cedar-ve/` | `libvencoder.so`，`vencoder.h` | ✅ |
| VE JPEG 编码、YUV 裁剪 | 同上 | 同上 | `AWJpecEnc`、`AWCropYuv` | ⚠️ |
| VE 解码 | `/dev/cedar_dev` | 同上 | `libvdecoder.so`，`vdecoder.h` | ⚠️ |
| G2D 2D 加速 | `/dev/g2d` | `bsp/drivers/g2d/` | 无库，直接 ioctl，`sunxi-g2d.h` | ⚠️ 驱动能响应 |
| NPU | `/dev/vipcore` | `bsp/drivers/npu/aw_nna_vip/vip2/` | `vip_lite.h` 有；**`libVIPlite.so` 缺** | ❌ 用户态不全 |
| GPU PowerVR BXM-4-64 | `/dev/dri/renderD128` | `bsp/modules/gpu/img-bxm/` | `libOpenCL.so`（OpenCL 3.0）、Vulkan、GLES | ✅ OpenCL 能枚举设备 |
| 显示 | `/dev/dri/card0`、`card1` | `bsp/drivers/drm/` | libdrm（头文件没装），`sunxi_drm.h` | ⚠️ |
| DMA-BUF 内存 | `/dev/dma_heap/system`、`reserved` | 主线 | `linux/dma-heap.h` | ✅（vin 导出 → VE 导入已实测） |
| PWM / GPIO | `pwmchip0/10/20`、`gpiochip0/1` | `bsp/drivers/pwm/`、`pinctrl/` | sysfs / libgpiod | ⚠️ |

**闭源的只有用户态 .so**；所有内核驱动都有源码（能改、能重编）。libisp 与 cedarc 导出同名但不兼容的
iniparser 符号，**不能链接进同一个进程**。

## 目录内容

| 路径 | 内容 |
|---|---|
| `board-include/` | 从板子 `/usr/include` 拷出的厂商头文件（ISP 3A、cedarc 编解码、VIPLite NPU） |
| `kernel-uapi/` | 内核用户态接口头文件：`sunxi_camera_v2.h`、`sunxi-g2d.h`、`cedar_ve_uapi.h`、`sunxi_drm.h`、`sunxi_display2.h`、`npu/vip_drv_*.h` |
| `symbols/` | 各厂商 .so 导出的函数名（`nm -D`），没有文档时查接口用 |
| `media-topology.txt` | `media-ctl -p` 完整拓扑 |
| `board-inventory.txt` | 设备节点、模块、PWM、video0 和传感器的全部控件（名称、ID、范围） |
| `vendor-samples/csi_test_mplane.c` | 全志自带的 V4L2 多平面采集示例 |
| `vendor-samples/probe_accel.c` | 探测 G2D、NPU 驱动是否响应 |

可工作的完整代码在 `ar0234-port/userspace/`（C++20）。

---

## 1. 相机采集：V4L2 + sunxi-vin

### 链路（`media-topology.txt`）

```
ar0234_mipi (subdev0) → sunxi_mipi.0 (subdev9) → sunxi_csi.0 (subdev2) → sunxi_tdm_rx.0 (subdev5)
  → sunxi_isp.0 (subdev12) ─┬→ sunxi_scaler.0 (subdev14) → vin_cap.0 (subdev1) → /dev/video0
                            ├→ sunxi_scaler.8 / .16（设备树里未接出 video 节点）
                            └→ sunxi_h3a.0 (subdev13，3A 统计)
```

### 标准流程（`../userspace/src/v4l2.cpp`）

1. `open("/dev/video0", O_RDWR | O_NONBLOCK)` —— **只能被一个进程打开**，第二次打开返回 EBUSY
2. `VIDIOC_S_INPUT {index=0}` —— 绑定传感器管线（补丁 0003 让不发这一步的通用程序也能用）
3. `VIDIOC_S_PARM`（`V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE`，`timeperframe = 1/fps`，`capturemode = 0x0002` 视频模式）
4. `VIDIOC_S_FMT`：`V4L2_PIX_FMT_NV12`（经 ISP）或 `V4L2_PIX_FMT_SGRBG10`（`BA10`，RAW，不经 ISP 处理）
5. `VIDIOC_REQBUFS`（MMAP）→ `VIDIOC_QUERYBUF` → `mmap`；`VIDIOC_EXPBUF` 导出 DMA-BUF fd 给其他硬件
6. `VIDIOC_QBUF` → `VIDIOC_STREAMON` → `poll` + `VIDIOC_DQBUF` / `VIDIOC_QBUF` 循环

输出格式：NV12 单平面（Y 后面紧跟 UV），缓冲按 16 对齐分配；RAW10 为 16bit 小端，黑电平 42。

### 硬件直接输出 RGB 和缩小图（✅ 2026-09-15 实测，不需要 G2D 或 CPU）

- 缩放器（VIPP）带硬件 YUV→RGB（`CONFIG_VIPP_YUV2RGB=y`、`VIPP_200`）。`S_FMT` 请求 `RGB3` 或 `BGR3` 都可以，
  **但两者输出的字节顺序完全一样，实际都是 B,G,R**（驱动里两种格式都映射到 `YUV2RGB888`）。按 BGR 使用，正好是 OpenCV 默认顺序。
- 每行 3×宽 字节，`sizeimage` 按 16 对齐的高度计算（1920x1080 → 1920×1088×3）。
- 同一个 `S_FMT` 就能直接要缩小尺寸，由 ISP 硬件缩放：640x400、640x360、960x600、320x200 的 BGR 都实测到 120fps；
  1920x1080、1600x1000 为 120fps，1920x1200 为 110–120fps。宽高比和传感器窗口不同时（例如 640x640）会被拉伸。
- CPU 对比：普通 C 代码把 1920x1200 NV12 转 RGB，A76 大核 16ms/帧，A55 小核 34ms/帧。
- ⚠️ BGR 1920x1200@120 有过一次开流后一帧不出、持续 `isp0 frame lost`（十几次里出现 1 次，之后复现不了，原因未查清）。
  应用里建议加看门狗：超过一定时间没收到帧就重新开流。
- 只要灰度的话，NV12 前 宽×高 字节就是 Y 灰度图，零开销。要线性数据用 RAW。

### 传感器控件（`/dev/v4l-subdev0`，直接写 AR0234 寄存器）

| 控件 | 单位 / 取值 | 说明 |
|---|---|---|
| `exposure` | 1/16 行（`值 >> 4` = 行数） | 请求了帧率时被限制在一帧以内 |
| `gain` | 1600 = 1x，最大 256x | 模拟 16x 以内，其余走数字增益 |
| `frame_rate` | fps，0 = 本模式原生帧率 | 通用程序不发 S_PARM 时用它 |
| `test_pattern` | 0 关，1–4 纯色、彩条等 | 下次开流生效 |
| `horizontal_flip` / `vertical_flip` | bool | |
| `temperature_approx_degc` | 只读 | 传感器片上温度 |
| 模块参数 `trigger_mode` / `flash_enable` / `flash_delay` | 0 自由运行，1 外部触发，2 从机同步 | ⚠️ 未实测，TRIG 为 1.8V 电平 |

### ISP 模式下 `/dev/video0` 的控件（交给 libisp，见 `board-inventory.txt`）

`auto_exposure`（0 自动，1 手动）、`exposure_time_absolute`（**微秒**）、`gain_automatic`、`gain`（**256 = 1x**）、
`white_balance_automatic`、`white_balance_temperature`、`power_line_frequency`、`auto_exposure_bias`、
`autoexposure_win_*`（测光区域）、`iso_sensitivity`、`3a_lock` 等。

- 只有 libisp 正在运行时（ar0234-3ad 会在开流后自动启动它）改动才生效，开流前设置会被忽略 —— ✅ 手动曝光和增益已实测
- 白平衡锁定、测光区域、`3a_lock` ⚠️ 未测

### sunxi-vin 私有接口（`kernel-uapi/sunxi_camera_v2.h`）

| 分类 | ioctl / 事件 | 用途 |
|---|---|---|
| 3A 统计 | `VIDIOC_VIN_ISP_H3A_CFG`、`VIDIOC_VIN_ISP_STAT_REQ`、`VIDIOC_VIN_ISP_STAT_EN`，事件 `V4L2_EVENT_VIN_H3A` | 读取 ISP 硬件的 AE/AWB/AF/直方图统计，可用来自己写 3A ⚠️ |
| ISP 寄存器 | `VIDIOC_VIN_ISP_LOAD_REG`、`VIDIOC_VIN_ISP_TABLE1_MAP`、`VIDIOC_VIN_ISP_TABLE2_MAP` | libisp 下发寄存器表（`../analysis/ispreg_spy.c` 可截获） |
| 传感器 | `VIDIOC_VIN_SENSOR_EXP_GAIN`、`VIDIOC_VIN_SENSOR_SET_FPS`/`GET_FPS`、`VIDIOC_VIN_SENSOR_CFG_REQ`、`VIDIOC_VIN_SENSOR_GET_TEMP` | 在 ISP 子设备或传感器子设备上调用 |
| 缩放 | `VIDIOC_VIN_SET_SCALER_CFG`、`VIDIOC_VIN_SET_SCALER_RESOLUTION`、`VIDIOC_SET_VIPP_SHRINK` | 硬件缩放、画面缩小 ⚠️ |
| 多相机同步 | `VIDIOC_SYNC_CTRL`（XVS/XHS 同步信号发生器） | zero3w 没引出引脚 |
| 其他 | `VIDIOC_VIN_PTN_CFG`（CSI 测试图）、`VIDIOC_SET_D3DLBCRATIO`（3DNR 压缩比）、`VIDIOC_VIN_SET_LDCI_MODE`、`VIDIOC_SET_VE_ONLINE`（ISP 直连 VE） | ⚠️ |
| 开流/停流事件 | ISP 子设备：`V4L2_EVENT_FRAME_SYNC`（每次开流前两帧）、`V4L2_EVENT_VIN_ISP_OFF` | ✅ ar0234-3ad 就是靠它们工作 |

### 注意

- **出过流之后不要 rmmod 或重新加载 vin 模块**：ISP 没被复位，之后一直 `isp0 width error`，只能重启。
- 设备树里 `vinc10`（同一个 ISP 的第二路缩放输出）是 disabled。打开后也许能同时输出两种分辨率（比如全分辨率加 NPU 输入尺寸） ⚠️ 未验证。
- 3DNR 在 1920x1200@120 下会每帧丢帧（消隐太短），ar0234-3ad 会自动切换参数。

---

## 2. ISP 3A：libAWIspApi（`board-include/AWIspApi.h`）

结构体里是函数指针，由 `CreateAWIspApi()` / `DestroyAWIspApi()` 创建和销毁：

| 函数 | 说明 |
|---|---|
| `ispApiInit()` / `ispApiUnInit()` | 每个进程一次 |
| `ispGetIspId(video_id)` | video0 → isp0 |
| `ispStart(isp_id)` / `ispStop` / `ispWaitToExit` | **必须在传感器管线已绑定后调用**（有程序 S_INPUT/出流），否则失败 |
| `ispSetFpsRanage(isp_id, fps)` | 让 AE 曝光不超过一帧 ✅（不调用时会按 1/10s 计划曝光） |
| `ispSetAeRegions` / `ispSetAfRegions` / `ispSetSceneMode` | ⚠️ |
| `ispGetIspGain` / `ispGetIspExp` / `ispGet3AParameters` / `ispGetDebugMessage` | 读当前 3A 状态 ⚠️ |

- 参数文件：libisp 只读 `/mnt/extsd/isp_param_config.bin`；结构偏移见 `../tools/make_isp_bin.py`；
  偏移 64 处 `isp_log_param = 0x3` 可打开 AE/AWB 日志。
- 完整用法：`../userspace/src/isp3a.cpp`、`../userspace/apps/ar0234-3ad.cpp`。

---

## 3. VE 视频编码：libvencoder（`board-include/vencoder.h`）

### 编码流程（✅ `../userspace/src/encoder.cpp`）

```
MemAdapterGetOpsS() + CdcMemOpen()
VideoEncCreate(VENC_CODEC_H264 | VENC_CODEC_H265 | VENC_CODEC_JPEG)
VideoEncSetParameter(VENC_IndexParamH264Param / VENC_IndexParamH265Param, ...)
VideoEncInit(enc, &VencBaseConfig{输入宽高, eInputFormat = VENC_PIXEL_YUV420SP})
VideoEncGetParameter(VENC_IndexParamH264SPSPPS / VENC_IndexParamH265Header)   → 码流头
零拷贝：VideoEncoderGetVeIommuAddr(enc, {fd = V4L2 EXPBUF 的 fd})             → VE 地址
每帧：AddOneInputBuffer → VideoEncodeOneFrame → AlreadyUsedInputBuffer
      → GetOneBitstreamFrame / FreeOneBitStreamFrame
拷贝模式：AllocInputBuffer / GetOneAllocInputBuffer / FlushCacheAllocInputBuffer / ReturnOneAllocInputBuffer
```

### 已踩过的坑

- VE 按 16x16 宏块读取整块对齐后的平面：输入缓冲不按 16 对齐会触发 IOMMU 越界。
- 零拷贝时 `nShareBufFd` 必须设为 -1，否则底部色度错位变绿。
- H.265 的 `nGopSize` 大于 63 时，库会静默改成 2。
- 实测吞吐：1080p H.264 约 8.7ms/帧，所以 1200p120 最多约 115fps。

### 其他接口

- `AWJpecEnc(JpegEncInfo*, EXIFInfo*, ...)`：⚠️ 未测（但 JPEG 已走标准编码器路径打通，见下）
- `AWCropYuv` / `AWCropYuvAndRotate`：YUV 裁剪和旋转 ⚠️
- `VideoEncIspCreate` / `VideoEncIspFunction`、`GetIspPhyAddrByFd`：编码器与 ISP 联动 ⚠️

### JPEG 编码（✅ 2026-09-16 实测，`../tools/jpeg_test.cpp`）

`VideoEncCreate(VENC_CODEC_JPEG)` + `VideoEncSetParameter(VENC_IndexParamJpegQuality, &int)` +
`VideoEncInit(YUV420SP)`，零拷贝流程与 H.264 相同（GetVeIommuAddr/nShareBufFd=-1），
`GetOneBitstreamFrame` 直接出完整 JPEG（无独立码流头）。实测 1080p 单帧 33KB，走 **VE2**
（`/dev/cedar_dev_ve2`）。无 SPS/PPS 调用。

### 解码 libvdecoder（⚠️→✅ 2026-09-16，`../tools/dec_test.cpp` + 厂商 demo）

**解码可用，但有三个坑（2026-09-16 实测）：**
1. 必须先 `AddVDPlugin()`（加载 libawh264/libawh265 等插件），链接 **`-lvideoengine`**；
   否则 `VideoEngineCreate` 报 `unsupported format H264`（= 没有插件注册该格式）。
2. 厂商验证：`/usr/bin/vdecoderdemo -i t30.h264 -codFmat 1 -o /tmp/dec.out -n 5 -sn 5 -outFmat 1`
   解码 1920x1088 H.264 成功（decode 7 帧 / display 5 帧）。
3. **警告**：按 vdecoder.h 单线程直调的自研封装曾把内核整体挂死（2026-09-16，断电恢复）。
   demo 是「喂流线程 + displayPicture 线程」双线程结构——集成时先照抄 demo 结构，不要单线程直调。

---

## 4. G2D 2D 加速（`kernel-uapi/sunxi-g2d.h`）

- 没有用户态库，直接对 `/dev/g2d` 发 ioctl。
- ✅ 驱动能响应：`G2D_CMD_QUERY_VERSION` 返回 `g2d_version = 0x10112114`（`vendor-samples/probe_accel.c`）
- ⚠️ 模块 `g2d_sunxi` **默认不自动加载**，`/dev/g2d` 只有 root 能访问 → 要在 `/etc/modules-load.d/` 加上模块，再加一条 udev 规则
- ⚠️→部分打通（2026-09-16，`../tools/g2d_test.cpp`）：`modprobe g2d_sunxi` 后 fd→fd BITBLT 可执行、
  2x 缩放输出尺寸正确。**两个关键发现**：
  1. `src_image_h.bbuff` 必须设 **1**（fd 缓冲标志），否则 G2D 把源读成全黑（输出 Y=16 的有限色域黑帧）；
  2. NV12→NV12 全屏拷贝仍有 ~5% 像素逐次漂移（幅度 >8，缓存同步协议问题：DMA_BUF_IOCTL_SYNC 无效，
     疑似 vb2 导出缓冲的 begin/end_cpu_access 没实现），T7 集成时要么换 uncached 分配、要么自己 flush。
  NV12 格式值 = `G2D_FORMAT_YUV420UVC_U1V1U0V0`（0x29）。

| 命令 | 结构体 | 用途 |
|---|---|---|
| `G2D_CMD_BITBLT_H` | `g2d_blt_h { flag_h, src_image_h, dst_image_h }` | **单图处理：缩放、裁剪、颜色格式转换（NV12↔RGB）、旋转** |
| `G2D_CMD_BLD_H` | Porter-Duff 混合 | 两图合成、画叠加层 |
| `G2D_CMD_FILLRECT_H` | `g2d_fillrect_h` | 填充矩形（画框） |
| `G2D_CMD_MASK_H` | `g2d_maskblt` | 按掩码做光栅运算 |
| `G2D_CMD_MIXER_TASK` / `CREATE_TASK` / `TASK_APPLY` / `TASK_DESTROY` | `mixer_para` | 批量提交多个任务，完成后只产生一次中断 |
| `G2D_CMD_LBC_ROT` | `g2d_lbc_rot` | LBC 压缩格式旋转 |

`g2d_image_enh` 的关键字段：`format`（`g2d_fmt_enh`，比如 NV12、RGB888）、`width`/`height`、`clip_rect`（ROI）、
`align[3]`、**`fd`（直接传 DMA-BUF，比如 V4L2 EXPBUF 的 fd，实现零拷贝）**、`use_phy_addr = 0`、`gamut`、`color_range`。

典型用途：画检测框、图层叠加、旋转、从一帧里裁多个 ROI、LBC 格式处理。
**单纯 NV12→RGB 和缩放不需要 G2D**：ISP 缩放器能直接输出指定尺寸的 BGR（见第 1 节）。

---

## 5. NPU（VeriSilicon VIPLite）

- ✅ 内核驱动 `vipcore` 1.13.0 已加载，`/dev/vipcore` 普通用户可读写
- `debugfs viplite/vip_info`：`pid=0x1000003b, ver1=0x9000, ver2=0x9202`
- `debugfs viplite/core_loading`：NPU 负载；`clk_freq`：492M、852M、1008M（当前）
- 用户态：`/usr/include/vip_lite.h` 有，`/usr/lib/libVIPhal.so`（只导出底层 `viphal_*`）和 `libNBGlinker.so` 有
- ❌ **缺 `libVIPlite.so`**（`vip_init` 等函数的实现），也缺把 ONNX/TFLite 模型转换、量化成 `.nb` 的工具

`vip_lite.h` 里的推理流程（拿到库之后照这个用）：

```
vip_init()
vip_create_network(.nb 文件或内存, &network) → vip_query_network（输入/输出张量信息）
vip_create_buffer / vip_create_buffer_from_fd（DMA-BUF 零拷贝输入）/ vip_create_buffer_from_handle
vip_prepare_network → vip_set_input / vip_set_output → vip_run_network → vip_finish_network
vip_map_buffer / vip_unmap_buffer 读结果
vip_destroy_buffer / vip_destroy_network / vip_destroy
```

内核命令接口在 `kernel-uapi/npu/vip_drv_interface.h`（ioctl 码 `VIPDRV_IOCTL = 30000`），只给库用；网络二进制格式由厂商工具生成，不能绕过库直接用。

**要向香橙派或全志要：A733 NPU SDK（VIPLite 运行库 + 模型转换工具链），版本要和内核驱动 1.13.0 匹配。**

---

## 6. GPU：PowerVR B-Series BXM-4-64

- ✅ OpenCL 3.0：`libOpenCL.so` 能枚举到 `PowerVR B-Series BXM-4-64`，普通用户可用（`../analysis/cltest.c`）
- 驱动 DDK 24.2@6603887（内核 `pvrsrvkm` 必须和用户库版本一致）
- 还有 Vulkan（`libvulkan.so.1`）、GLES2（`libGLESv2_PVR_MESA.so`）、EGL
- ⚠️ **开发头文件都没装**（`/usr/include/CL`、`EGL`、`GLES2`、`vulkan`、`libdrm` 都没有）：需要装 `opencl-c-headers` 等头文件包，
  **注意别装会替换厂商 `libOpenCL.so` 的 ICD 加载器包**
- ⚠️ 未测：OpenCL 能否直接导入 DMA-BUF 实现零拷贝（要查 PowerVR 的扩展支持）；OpenCV T-API

适合：RAW 去马赛克、平场校正、滤波、去畸变（remap）、二值化，这类传统视觉算法的并行加速。

---

## 7. 显示

- DRM/KMS：`/dev/dri/card0`、`card1`，标准 libdrm 接口（头文件没装）；`sunxi_drm.h` 只多了一个 `DRM_IOCTL_SUNXI_PQ_PROC`（画质调节）
- `sunxi_display2.h` 是旧的 `/dev/disp` 接口，这块板子上**没有 `/dev/disp`**，用 DRM
- 硬件图层叠加可以零拷贝显示 NV12 预览 ⚠️ 未测

---

## 8. 零拷贝对接一览（DMA-BUF）

| 从 → 到 | 接口 | 状态 |
|---|---|---|
| ISP（V4L2 EXPBUF）→ VE 编码 | `VideoEncoderGetVeIommuAddr` | ✅ |
| ISP → G2D | `g2d_image_enh.fd` | ⚠️ 接口支持，未测 |
| ISP / G2D → NPU | `vip_create_buffer_from_fd` | ❌ 缺库 |
| ISP → GPU OpenCL | 取决于 PowerVR 扩展 | ⚠️ 未知 |
| ISP → 显示 | DRM PRIME 导入 | ⚠️ 未测 |
| 自己分配 | `/dev/dma_heap/system`（`DMA_HEAP_IOCTL_ALLOC`） | ✅ 节点存在 |

---

## 9. 工业相机相关的 IO

| 功能 | 接口 | 状态 |
|---|---|---|
| 硬件 PWM（产生触发） | `pwmchip0`（2527000.pwm）、`pwmchip10`（2528000.pwm）、`pwmchip20`（7023000.pwm），每个 10 路 | ⚠️ 哪几路引到了 40pin、引脚复用有没有配，都还没查 |
| GPIO | `/dev/gpiochip0`、`gpiochip1`（libgpiod，命令行工具没装） | ⚠️ |
| 传感器触发和闪光灯 | `ar0234_mipi` 模块参数 | ⚠️ |
| E902 RISC-V 小核 | `~/e902/` 脚本加载固件（没有 remoteproc 驱动） | ⚠️ 可以考虑用来做精确触发 |
| USB 摄像头模式 | 内核已开 `CONFIG_USB_CONFIGFS_F_UVC`，UDC `4100000.udc-controller` | ⚠️ 需要用户态 UVC 程序 |
| 以太网 | 板子没有网口 | — |

---

## 10. 缺口清单

1. **NPU SDK**：`libVIPlite.so` + 模型转换工具（找厂商要）
2. **ISP tuning**：全志 ISP Tuning Tool 及板端配合程序（找厂商要），或者自己拿白纸、灰卡、色卡标定
3. **开发头文件**：OpenCL / EGL / GLES / Vulkan / libdrm（apt 安装，注意别覆盖厂商库）
4. **G2D**：开机自动加载 + 设备权限
5. **PWM 触发引脚**：确认 40pin 映射，必要时改设备树
6. **双路 ISP 输出**：设备树 `vinc10`，未验证
