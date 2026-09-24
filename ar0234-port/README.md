# AR0234 → Orange Pi Zero 3W (A733/sun60iw2) BSP 摄像头驱动移植

> **内核改动（驱动 + vin 补丁 + RT）只在 `../userpatches/kernel/sun60iw2-current/` 一处**，由 orangepi-build 编内核时自动打上；
> 本目录只放用户态、打包、参数和调试记录。
> 接手工作先读 [`docs/HANDOFF.md`](docs/HANDOFF.md)（待办任务、环境、必读的坑），硬件接口见 [`hwapi/README.md`](hwapi/README.md)。

4-lane MIPI（MIPI-A），10-bit RAW (GRBG)，24MHz MCLK，走 `vind0 → mipi0 → csi0 → tdm_rx0 → isp0 → vinc00`
（沿用板上原 imx219 的链路）。

## 当前状态（2026-09-15 上板实测）

| 项 | 结果 |
|---|---|
| 探测 | chip id 0x0A56（彩色版） |
| MIPI 4-lane 900Mbps RAW10 | ✅ 测试图案 (0x3070=2) 逐像素正确，无丢帧/错位 |
| 1920x1200 / 1920x1080 / 1280x720 / 960x600(bin) | ✅ 120.44 / 133.57 / 198.47 / 235.30 fps（各 240 帧） |
| 曝光 / 增益 / test_pattern 控件 | ✅ 寄存器回读一致；gain/exposure 控件实时反映 libisp AE 写入的值 |
| ISP NV12 + libisp 3A + 3DNR（`ar0234-3ad` 服务，任意程序出流都生效） | ✅ 1080p30/60、720p120、1200p30/120 |
| VE 硬件编码（`ar0234-rec`，零拷贝） | ⚠️ **H.264 ✅**（1200p120 实测 **103 fps**，编码 9.6 ms/帧 → 编码器是瓶颈，不是相机；1080p 118 fps）。**H.265 ⚠️ 只在 1920×1200@120 破**（只出 6 帧 + `isp0 configuration error`/`height error`）；1200p60/1200p30/1080p30/**720p120** 都正常。**别按「1200p120 + H.265」搭系统**，那一步用 H.264。旧记录「115.7fps」是别的取景内容，测出来的 103 fps 才是同一场景下的数字。 |
| 手动曝光/增益（经 libisp） | ✅ `ar0234-rec -e 8000 -g 4` |

**ISP 参数是初版**（gc05a2 A733 模板 + Kurokesu CCM/AWB），LSC/MSC、AWB、CCM 都没有标定，见 `analysis/`。

## 用户态（`userspace/`，C++20，板上 g++ 10 本地编译）

```bash
make -C userspace -j8 && sudo make -C userspace install    # /usr/local/bin + systemd unit
sudo systemctl enable --now ar0234-3ad
ar0234-rec -w 1920 -h 1200 -f 120 -n 600 -c h264 -o x.h264  # -n 0 录到 Ctrl-C
# H.265 在 1920x1200@120 实际可用（第九轮 6/6 次全速通过，117.7–118.7 fps）
# 固定曝光/增益/AWB：编辑 /etc/ar0234.conf（mode=fixed + exposure_lines/gain/awb/wb_temperature），
#   ar0234-3ad 在每路流启动时应用，对流谁都生效；字段说明和工业模式样例就在文件里
# NPU 零拷贝（C10 验收工具，build 不 install）：
#   userspace/build/ar0234-npu-zerocopy [--frames N] [--fanout] [--dump DIR]
#   真实 ISP 帧 → G2D(缩放+NV12→BGR888) → vip_create_buffer_from_fd → NPU，300 帧 A/B 逐字节一致
```

| 部件 | 说明 |
|---|---|
| `include/ar0234/unique_fd.hpp` | fd RAII、`xioctl`（EINTR 重试） |
| `v4l2.hpp` / `src/v4l2.cpp` | 按 sysfs 名找节点；`Subdev`（事件订阅、pad 格式）；`Capture`（S_INPUT/S_PARM/S_FMT、mmap + DMA-BUF 导出，`Frame` 析构自动回队）。**开流看门狗**：`start()` 会等首帧 + 连续 `probe_frames` 帧，超时（`probe_timeout_ms`/`stall_timeout_ms`）就 close+open 重开（最多 `retries` 次，默认 2），统计在 `stream_stats()`。外触发/≤1 fps 场景请把 `probe_timeout_ms` 设 0 关闭。 |
| `isp3a.hpp` / `src/isp3a.cpp` | `Isp3A`：一次 libisp 会话（pimpl 隐藏厂商头），`ispSetFpsRanage` 把 AE 曝光限制在一帧内（3ad 现在把传入 fps 钳到 120，见下）；`IspParamSets`：按模式原子替换参数文件，支持 `params=` 指定命名集；`apply_fixed_mode`：`/etc/ar0234.conf` 固定模式往参数文件写手动曝光/增益/AWB（结构体偏移，文件 +74） |
| `ar0234conf.hpp` / `src/ar0234conf.cpp` | `/etc/ar0234.conf` 解析：`mode/params/exposure_lines/exposure_us/gain/awb/wb_temperature/ae_log`，坏值/未知键在 3ad 启动日志告警 |
| `encoder.hpp` / `src/encoder.cpp` | cedar VE H.264/H.265，DMA-BUF 导入 VE IOMMU 零拷贝，失败回退拷贝 |
| `apps/ar0234-3ad.cpp` | 3A 服务，见下 |
| `apps/ar0234-rec.cpp` | 录像；`-e/-g` 手动曝光增益（退出时恢复自动） |

libisp 和 cedarc 导出同名但不兼容的 iniparser 符号，Makefile 保证两者不进同一个可执行文件。

**`ar0234-3ad` 的工作方式**：vin 不允许第二个进程打开忙碌的 `/dev/video0`，libisp 又必须在 sensor 管线
绑定（有程序 S_INPUT/出流）后才能初始化，所以服务订阅 `sunxi_isp.0` 子设备事件：每次开流前两帧有
`V4L2_EVENT_FRAME_SYNC`，停流有 `V4L2_EVENT_VIN_ISP_OFF`。两帧事件的时间差得到帧率，ISP sink pad
格式得到 sensor 窗口，选参数（**vblank < 500 µs 就关 3DNR**——与内核 0006 interlock 对齐的估算，
高度 ≥1200 且 >110fps 依旧强制关），装 `/etc/ar0234.conf` 要求的参数集/固定模式，然后 fork 子进程跑
一次 libisp 会话（传给 `ispSetFpsRanage` 的 fps 钳到 120：更高的上限永远绑定不到传感器最长快照，
而 ≥133 的实测值会触发 0 帧风暴，见 HANDOFF §3.32），停流 SIGTERM 结束。每路流一个新进程，
libisp 全局状态不会残留。

- libisp 的 AE 表来自 gc05a2（最长 1/10s）；不调 `ispSetFpsRanage` 时 AE 以为 100ms、sensor 实际 33ms，
  LV 偏低 1.6EV。现在按实测帧率限制，日志里 30/60/120fps 的曝光上限分别是 30/13.9/8.3ms。
- ISP 模式下 `/dev/video0` 的 exposure/gain/auto_exposure 控件交给 libisp（`sunxi_isp.c`），不直接写
  sensor，并且 libisp 只响应它运行期间的改动，所以 `ar0234-rec` 在开流 1 秒后才下发。单位：
  `exposure_time_absolute` 微秒，`gain` 256 = 1x。
- 驱动修正：libisp 的 `VIDIOC_VIN_SENSOR_SET_FPS` 以前写进 `sensor->fps` 并一直保留，压过下一路流的
  S_PARM（720p120 录成 30fps），现在存到 `isp_fps`，停流清零；gain 控件改为 volatile，读回 1/1600 单位。
- **出过流之后不要 rmmod/重新加载 vin 模块**：ISP 没有复位（加载时 `Get isp reset control fail`），之后
  开流一直 `isp0 width error`，只能重启。
- **「ISP 出过错就得重启」是过宽的说法**（第六/七轮实测）：`frame lost`/`sunxi_isp_reset` 风暴、以及开流
  「只出几帧就死」都**不需要重启** —— 前者停流重开即可，后者 `userspace/` 的采集库会自动重开救回。
  真正必须重启的只有「只重载了传感器模块」这一类（`no link to sunxi_mipi.0`）。
- **ISP 参数文件偏移**：文档里所有偏移都是**结构体偏移**，**文件偏移 = 结构体偏移 + 74**
  （74 = 4 B 长度 + 20 B 日期 + 50 B note）。例：`tdf` 结构体 101 ↔ 文件 175。
- **G2D**：`/dev/g2d` 开机即有、`video` 组可用（第七轮）；但 fd→fd 拷贝**不是位精确**，
  两次实测偏差量级都不同，需要逐像素一致时请用 CPU 实现。
- **NPU 可用**：`/usr/lib/libNBGlinker.so` 提供整套 `vip_*` API，`/opt/vpm_run` 真实推理 2747 µs、`/opt/yolov5` demo 认出 dog 82%）；
  只剩「自己的模型要有厂商 NBG 工具转换」这一步。

## 可调参数地图（2026-09-17 第十一轮实测口径）

| 参数 | 怎么调 | 单位/范围 | 生效方式 |
|---|---|---|---|
| 曝光（固定） | `/etc/ar0234.conf` `mode=fixed` + `exposure_lines=N`，或流内 `Capture::set_control(0x00980911,N)`、`ar0234-rec -e` | 传感器行，1 行=6.8 µs；120fps 下上限 fll-4≈1200 行 | fixed 直通模式（3ad 写传感器子设备，无 AE 竞争）钉住 |
| 增益（固定） | 同上 `gain=X`，或流内/子设备写 `0x00980913`（单位 1/1600×） | 1.0–255.9×；模拟 AGC 表顶到 ~6.25×，其余数字增益补足 | 同上；**libisp 手动模式不写传感器增益**（round 11 实测），必须走直通/子设备 |
| 曝光/增益（自动） | 默认（3ad auto）：libisp AE/AWB 全自动 | AE 目标亮度由参数文件决定 | 每帧自适应；`ae_log = 1` 可在 journal 逐帧观察 |
| 帧率 | `V4L2_CID_FRAME_RATE` / S_PARM / 模块参数 `frame_rate` | 1–120 fps（>120 会触发 3DNR interlock 风暴，3ad 已钳） | 驱动写 FLL |
| gamma | **整文件重生成**：`tools/make_isp_bin.py … --gamma-table linear` 后按 P0-3 三处安装 + 清 ctx | 5 行×3 通道×1024 点，0..4095 | 无运行时控件；字节补丁已退役（round 11） |
| 白平衡 | auto 默认；`ae_log=1` 可观察 ColorTemp/WB Gain | — | 固定 WB 需重生成参数文件（未实现） |
| 翻转 | `V4L2_CID_HFLIP/VFLIP`（video0 或传感器子设备） | bool | 驱动写 0x301C，Bayer 序保持 GRBG |
| 测试图案 | 传感器子设备 `test_pattern`（0x009f0903，0..4） | 菜单 | 直写 0x3070，场景无关（排障利器） |
| 触发/闪光 | 模块参数 `trigger_mode/flash_enable/flash_delay` + `vin_flash_ctrl/led_mode` 控件 | 见 A2 | 硬件接线未验证 |
| 3DNR | 自动（3ad 按帧率/快门时间选参数集） | — | vblank<500µs 强制关（内核 0006 interlock） |

**注意**：3A 运行期间，video0 的 exposure/gain 控件交给 libisp（每帧覆盖手写值）；要手动钉扎，
用 `mode=fixed`（无 libisp 竞争）或先停 3ad。传感器子设备节点（`/dev/v4l-subdev*` 中 name=ar0234_mipi
的那个）**永远不会忙**，是流运行中唯一可外部读写的入口。

## ISP 硬件模块（2026-09-15 更新，按出流时 libisp 下发的 MODULE_BYPASS1 寄存器核实）

`isp/isp_param_3dnr.bin` 下发 `0x3d07f9fe`：

| 状态 | 模块 |
|---|---|
| 开 | DPC、CTC、GCA（中心改为 960,600）、D2D、**D3D 3DNR**、BLC、WB、DG、PLTM、LCA、SHARP、CCM、CNR、DRC、GAMMA、CEM、去雾；AE/AWB/AFS/HIST 统计 |
| 关，缺标定 | LSC、MSC（没有 AR0234 镜头的平场表） |
| 用不上 | WDR（AR0234 无 HDR）、AF（定焦） |

- 3DNR 需要 `CONFIG_D3D=y` 且 **LBC 模式**（已写进 `../userpatches/linux-sun60iw2-current-a733.config`）。
  厂商 A733 配置里 D3D 是关的；PKG 模式一出流就 `isp0 width error`，COMPRESS_EN 会 IOMMU fault。
  `vin_io.ko` 必须用系统原装的，树里重编的 vin_io 同样出不了图。
- 1920x1200@120 帧长 1216 行、消隐只有 16 行，3DNR 来不及更新参考帧，每帧 `isp0 frame lost`
  （1200p110、1080p120 正常）。`ar0234-3ad` 按模式自动换参数：
  `/mnt/extsd/ar0234/isp_param_no3dnr.bin`（1200 高且 >110fps）/ `isp_param_3dnr.bin`（其他），
  拷到 libisp 固定读取的 `/mnt/extsd/isp_param_config.bin`。
- 3DNR 实测：1080p30 帧间噪声（相邻帧亮度差中位数）1.03 → 0.03，编码方块明显减少。
- `isp/isp_param_config.bin` 是之前 3DNR 关闭的版本，留作对照。

## 目录

| 路径 | 说明 |
|---|---|
| `../userpatches/kernel/sun60iw2-current/` | **内核补丁（唯一出处）**：0000 RT、0001 AR0234 驱动 + DTS、0003–0013 vin 修复；README 里有逐个说明 |
| `userspace/` | C++20 用户态：`ar0234-3ad` 3A 服务、`ar0234-rec` 录像、G2D/NPU 工具（见上）；`vendor-include/` 是 sunxi-g2d UAPI 头 |
| `packaging/` | 一键打 `ar0234-camera` deb 包（0.2.0 起不带内核模块） |
| `board/` | 板上安装脚本、udev 规则、`/etc/ar0234.conf` 样例 |
| `isp/` | libisp 参数文件（3DNR / 无 3DNR / 工业固定模式）与 gc05a2 模板 |
| `tools/` | 常用工具：`ssh_board.sh`（`-s` 以 sudo 执行）、`put_board.sh`、串口 `sc.py`/`serial_log.py`、`make_isp_bin.py`、`build_vin.sh`、`vinreg.c`、DT/CMA 辅助 |
| `tools/hwtest/` | 硬件测试程序：抓图 `cap.c`、`vfr`、G2D/JPEG/解码/OpenCL、软件 ISP |
| `tools/rescue/` | 板子起不来时的 u-boot / sysrq 串口救援脚本 |
| `docs/` | `HANDOFF.md`（坑与任务清单）、`NEXT-TASKS.md`、`ARCHITECTURE.md`、`BASELINE.md` |
| `analysis/` | 各轮调查报告（`REPORT-*.md`、`round9/`、`round10/`、`t14/`、`g2d/`…）；`analysis/scripts/` 是当时一次性用的实验脚本 |
| `hwapi/` | A733 图像相关硬件底层 API 汇总（ISP/V4L2、3A、VE、G2D、NPU、GPU、显示、DMA-BUF、PWM/GPIO） |
| `driver-completion/` | 非相机外设补全（RTC、触摸、音频、40-pin） |
| `legacy/` | 旧的 C 版录像/3A 辅助进程，已被 `userspace/` 取代 |
| `samples/`、`tuning-ref/` | 样张、Kurokesu 调参参考 |
| `vendor-ref/`、`build/` | 本地：libAWIspApi 源码克隆、vin 外部编译目录（不入 git） |

## 相对 RPi 驱动/上一版的修正

- **曝光控件从来没生效**：exposure 控件带 `V4L2_CTRL_FLAG_VOLATILE`，v4l2 核心对 volatile 控件
  不调 `s_ctrl`（`cluster_changed()` 直接跳过）。加 `EXECUTE_ON_WRITE` 修复。
- **模拟增益公式**：0x3060 = coarse[6:4] / fine[3:0]，s=0,2: 2^s·32/(32-t)；s=1,3: 2^s·16/(16-t/2)；
  s=4: 16·8/(8-t/4)。下限 0x0D (1.68x，同 RPi)，上限 0x40 (16x)，其余走 0x305E 数字增益。
  原来按 2^(reg/16) 近似。
- 开流时驱动会软复位 sensor，现在复位后重新下发曝光/增益/test_pattern，并重置 0x30BA 缓存；
  未开流时设置控件只记下来，不再去写没上电的 sensor。
- **960x600 binning**：RPi 默认 FLL=616 在 sunxi-vin 上一帧都收不到，≥620 正常，改为 620（235fps）。
- 新增 `test_pattern` 控件（`/dev/v4l-subdev0`）和 `frame_rate` 模块参数（拉长 FLL 限帧率，
  如 `insmod ar0234_mipi.ko frame_rate=30`，也可运行时写 `/sys/module/ar0234_mipi/parameters/frame_rate`）。
- 上一版装在板上的其实是 **RAW8 / 360MHz(720Mbps)** 的实验版本（描述字符串仍写 10-bit）；
  实测 10-bit/900Mbps 完全正常，现已换回 10-bit。

## 编译

正式出包：在 orangepi-build 根目录 `./build.sh BOARD=orangepizero3w BRANCH=current BUILD_OPT=kernel`。
构建系统会把内核树 `git checkout -f` + `git clean` 回原样，再按顺序打上
`userpatches/kernel/sun60iw2-current/*.patch`，所以**不要直接改 `kernel/orange-pi-6.6-sun60iw2`**，改动都写成补丁。

调试时想秒级重编 vin / 传感器模块，用 `tools/build_vin.sh`（在 `build/` 下外部编译）。

## 板上使用 / 测试

板子现状（2026-09-24）：跑 RT 内核 `6.6.98-rt58-sun60iw2`，`ar0234_mipi.ko` / `vin_v4l2.ko` 由内核包提供；
但 `/dev/video*` 不存在、`ar0234-3ad` 没在跑 —— 换 RT 内核后相机栈还没重新装好，先跑 `board/install.sh`
或装 0.2.0 deb 包，再验证。

RAW 抓图（ISP 模式下也能直接要 BA10，不经 ISP 处理）：

```bash
cd ~/ar0234test && gcc -O2 -o cap cap.c
# 宽 高 fourcc 帧数 曝光(行) 增益(x100) 输出前缀
sudo ./cap 1920 1200 BA10 120 800 400 /tmp/cam/f1
sudo v4l2-ctl -d /dev/v4l-subdev0 -c test_pattern=1   # 彩条，下次开流生效
# 调试读写寄存器（开流期间）：cap 支持 WREG="3070=0002" RREG="3012,3060"
```

注意：
- 内核补丁 0003 让 vin 在开流时自动 S_INPUT，`v4l2-ctl --stream-mmap` 这类通用程序可以直接用。
  帧率用 `v4l2-ctl -d /dev/v4l-subdev0 -c frame_rate=N`（通用程序不发 S_PARM 时）。
- ISP 模式下 cap 的曝光/增益（经 video0）不会写到 sensor，手动值请用 `/dev/v4l-subdev0` 的控件。
- RAW10 缓冲是 16-bit 小端，每像素 2 字节，值域 0~1023，黑电平 42（0x301E）。

## 仍需确认

1. AR0234 ISP tuning（见上，出彩色图的前提）。
2. libisp 按 `名字_宽_高_fps_…` 查配置，fps 取自 `fps_fixed`（沿用 imx219 模板的 1）。做 tuning 时名字要对上，
   或者把 `fps_fixed` 改成真实帧率（会影响 `sensor_find_frame_size` 的选窗逻辑）。
3. 高帧率下 ISP 吞吐：1080p@134fps ≈ 278MP/s，比 A733 ISP 标称 4K30 高；上 ISP 时建议 `frame_rate=60`。
4. 模块供电/复位脚：目前沿用 imx219 的 PE6 pwdn、无 reset，实测能正常探测和出流。
