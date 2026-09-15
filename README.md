# AR0234 → Orange Pi Zero 3W (A733/sun60iw2) BSP 摄像头驱动移植

> 接手工作先读 [`HANDOFF.md`](HANDOFF.md)（待办任务、环境、必读的坑），硬件接口见 [`hwapi/README.md`](hwapi/README.md)。

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
| VE 硬件编码（`ar0234-rec`，零拷贝） | ✅ H.264 / H.265；1200p120 实测 115.7fps（编码器上限） |
| 手动曝光/增益（经 libisp） | ✅ `ar0234-rec -e 8000 -g 4` |

**ISP 参数是初版**（gc05a2 A733 模板 + Kurokesu CCM/AWB），LSC/MSC、AWB、CCM 都没有标定，见 `analysis/`。

## 用户态（`userspace/`，C++20，板上 g++ 10 本地编译）

```bash
make -C userspace -j8 && sudo make -C userspace install    # /usr/local/bin + systemd unit
sudo systemctl enable --now ar0234-3ad
ar0234-rec -w 1920 -h 1200 -f 120 -n 600 -c h265 -o x.h265  # -n 0 录到 Ctrl-C
```

| 部件 | 说明 |
|---|---|
| `include/ar0234/unique_fd.hpp` | fd RAII、`xioctl`（EINTR 重试） |
| `v4l2.hpp` / `src/v4l2.cpp` | 按 sysfs 名找节点；`Subdev`（事件订阅、pad 格式）；`Capture`（S_INPUT/S_PARM/S_FMT、mmap + DMA-BUF 导出，`Frame` 析构自动回队） |
| `isp3a.hpp` / `src/isp3a.cpp` | `Isp3A`：一次 libisp 会话（pimpl 隐藏厂商头），`ispSetFpsRanage` 把 AE 曝光限制在一帧内；`IspParamSets`：按模式原子替换参数文件 |
| `encoder.hpp` / `src/encoder.cpp` | cedar VE H.264/H.265，DMA-BUF 导入 VE IOMMU 零拷贝，失败回退拷贝 |
| `apps/ar0234-3ad.cpp` | 3A 服务，见下 |
| `apps/ar0234-rec.cpp` | 录像；`-e/-g` 手动曝光增益（退出时恢复自动） |

libisp 和 cedarc 导出同名但不兼容的 iniparser 符号，Makefile 保证两者不进同一个可执行文件。

**`ar0234-3ad` 的工作方式**：vin 不允许第二个进程打开忙碌的 `/dev/video0`，libisp 又必须在 sensor 管线
绑定（有程序 S_INPUT/出流）后才能初始化，所以服务订阅 `sunxi_isp.0` 子设备事件：每次开流前两帧有
`V4L2_EVENT_FRAME_SYNC`，停流有 `V4L2_EVENT_VIN_ISP_OFF`。两帧事件的时间差得到帧率，ISP sink pad
格式得到 sensor 窗口，选参数（1200 高且 >110fps 关 3DNR），然后 fork 子进程跑一次 libisp 会话，停流
SIGTERM 结束。每路流一个新进程，libisp 全局状态不会残留。

- libisp 的 AE 表来自 gc05a2（最长 1/10s）；不调 `ispSetFpsRanage` 时 AE 以为 100ms、sensor 实际 33ms，
  LV 偏低 1.6EV。现在按实测帧率限制，日志里 30/60/120fps 的曝光上限分别是 30/13.9/8.3ms。
- ISP 模式下 `/dev/video0` 的 exposure/gain/auto_exposure 控件交给 libisp（`sunxi_isp.c`），不直接写
  sensor，并且 libisp 只响应它运行期间的改动，所以 `ar0234-rec` 在开流 1 秒后才下发。单位：
  `exposure_time_absolute` 微秒，`gain` 256 = 1x。
- 驱动修正：libisp 的 `VIDIOC_VIN_SENSOR_SET_FPS` 以前写进 `sensor->fps` 并一直保留，压过下一路流的
  S_PARM（720p120 录成 30fps），现在存到 `isp_fps`，停流清零；gain 控件改为 volatile，读回 1/1600 单位。
- **出过流之后不要 rmmod/重新加载 vin 模块**：ISP 没有复位（加载时 `Get isp reset control fail`），之后
  开流一直 `isp0 width error`，只能重启。

## ISP 硬件模块（2026-09-15 更新，按出流时 libisp 下发的 MODULE_BYPASS1 寄存器核实）

`isp/isp_param_3dnr.bin` 下发 `0x3d07f9fe`：

| 状态 | 模块 |
|---|---|
| 开 | DPC、CTC、GCA（中心改为 960,600）、D2D、**D3D 3DNR**、BLC、WB、DG、PLTM、LCA、SHARP、CCM、CNR、DRC、GAMMA、CEM、去雾；AE/AWB/AFS/HIST 统计 |
| 关，缺标定 | LSC、MSC（没有 AR0234 镜头的平场表） |
| 用不上 | WDR（AR0234 无 HDR）、AF（定焦） |

- 3DNR 需要 `CONFIG_D3D=y` 且 **LBC 模式**（`patches/0004`，`prebuilt/vin_v4l2.ko` 已带）。
  厂商 A733 配置里 D3D 是关的；PKG 模式一出流就 `isp0 width error`，COMPRESS_EN 会 IOMMU fault。
  `vin_io.ko` 必须用系统原装的，树里重编的 vin_io 同样出不了图。
- 1920x1200@120 帧长 1216 行、消隐只有 16 行，3DNR 来不及更新参考帧，每帧 `isp0 frame lost`
  （1200p110、1080p120 正常）。`ar0234-3ad` 按模式自动换参数：
  `/mnt/extsd/ar0234/isp_param_no3dnr.bin`（1200 高且 >110fps）/ `isp_param_3dnr.bin`（其他），
  拷到 libisp 固定读取的 `/mnt/extsd/isp_param_config.bin`。
- 3DNR 实测：1080p30 帧间噪声（相邻帧亮度差中位数）1.03 → 0.03，编码方块明显减少。
- `isp/isp_param_config.bin` 是之前 3DNR 关闭的版本，留作对照。

## 内容

| 文件 | 说明 |
|---|---|
| `ar0234_mipi.c` | BSP sunxi-vin 框架 sensor 驱动（以 imx219.c 为模板），寄存器表来自 RPi/Kurokesu ar0234 驱动 |
| `patches/0001-vin-sun60iw2-add-ar0234-sensor.patch` | 内核补丁：新驱动 + sensor/Kconfig + sensor/Makefile + zero3w DTS 的 `sensor0_mname` |
| `patches/0002-configs-enable-sensor-ar0234.patch` | 三个内核配置加 `CONFIG_SENSOR_AR0234=m` |
| `patches/0004-configs-enable-isp-3dnr-d3d-lbc.patch` | 三个内核配置开 `CONFIG_D3D=y` + `CONFIG_D3D_LBC_MODE=y`（ISP 硬件 3DNR） |
| `build/vin-d3d-lbc/` | 外部编译 vin 模块的目录（vin 源码拷贝 + 0003 + `board_compat.h` 定义 D3D），`make -C <kernel> M=$PWD` 约 9 秒 |
| `apply.sh` | 一键把上面两个补丁打进 root 属主的内核树 |
| `userspace/` | C++20 用户态：`ar0234-3ad` 3A 服务、`ar0234-rec` 录像（见上） |
| `tools/cap.c` | 板上抓图测试程序（S_INPUT→S_FMT→STREAMON→设曝光增益→统计帧率→存 raw + 缩略 pgm） |
| `legacy/` | 旧的 C 版录像/3A 辅助进程（`ar0234_rec.c`、`ar0234_3a.c`），已被 `userspace/` 取代，留作参考 |
| `analysis/` | ISP 诊断：寄存器截获 `ispreg_spy.c`、RAW/NV12 统计、3A 日志、3DNR 对比 |
| `hwapi/` | **A733 图像相关硬件底层 API 汇总**：ISP/V4L2、3A、VE、G2D、NPU、GPU、显示、DMA-BUF、PWM/GPIO 的头文件、导出符号、调用流程和验证状态 |
| `../rt-check/` | PREEMPT_RT 补丁（6.6.97-rt57 / 6.6.99-rt58）对 BSP 内核的试打结果：0 冲突；暂不做 RT |
| `tools/sc.py` / `tools/get_file.py` / `send_file.py` | 串口执行命令 / 从板上取文件 / 往板上传文件 |

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

## 编译（外部模块，秒级）

内核树是 root 属主，不想打补丁也可以在别处建目录，把 `bsp/drivers/vin/*` 与
`bsp/drivers/vin/modules/*`（sensor 除外）软链过去、`modules/sensor/*.h` 也软链，
再放 `ar0234_mipi.c` + 一行 `obj-m := ar0234_mipi.o` 的 Makefile：

```bash
K=/home/helios/Desktop/orangepi-build/kernel/orange-pi-6.6-sun60iw2
TC=/home/helios/Desktop/orangepi-build/toolchains/gcc-arm-11.2-2022.02-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-
make -C $K M=$PWD ARCH=arm64 CROSS_COMPILE=$TC modules
${TC}strip --strip-debug -o ar0234_mipi.stripped.ko ar0234_mipi.ko   # 510K -> 25K，串口好传
```

或者 `sudo bash ar0234-port/apply.sh` 后走内核树 `make modules`，完整出包仍走 build.sh。

## 板上使用 / 测试

板子当前状态：DTB `sensor0_isp_used=1`；`vin_v4l2.ko`（D3D LBC）与 `ar0234_mipi.ko` 为 `prebuilt/` 版本，
旧版备份 `vin_v4l2.ko.bak-pre-d3d`、`ar0234_mipi.ko.bak-pre-cpp`；`/etc/modules-load.d/ar0234.conf` 开机加载 vin；
`ar0234-3ad.service` 开机启动；内核/dtb 包 apt-mark hold。

RAW 抓图（ISP 模式下也能直接要 BA10，不经 ISP 处理）：

```bash
cd ~/ar0234test && gcc -O2 -o cap cap.c
# 宽 高 fourcc 帧数 曝光(行) 增益(x100) 输出前缀
sudo ./cap 1920 1200 BA10 120 800 400 /tmp/cam/f1
sudo v4l2-ctl -d /dev/v4l-subdev0 -c test_pattern=1   # 彩条，下次开流生效
# 调试读写寄存器（开流期间）：cap 支持 WREG="3070=0002" RREG="3012,3060"
```

注意：
- `patches/0003` 让 vin 在开流时自动 S_INPUT，`v4l2-ctl --stream-mmap` 这类通用程序可以直接用。
  帧率用 `v4l2-ctl -d /dev/v4l-subdev0 -c frame_rate=N`（通用程序不发 S_PARM 时）。
- ISP 模式下 cap 的曝光/增益（经 video0）不会写到 sensor，手动值请用 `/dev/v4l-subdev0` 的控件。
- RAW10 缓冲是 16-bit 小端，每像素 2 字节，值域 0~1023，黑电平 42（0x301E）。

## 仍需确认

1. AR0234 ISP tuning（见上，出彩色图的前提）。
2. libisp 按 `名字_宽_高_fps_…` 查配置，fps 取自 `fps_fixed`（沿用 imx219 模板的 1）。做 tuning 时名字要对上，
   或者把 `fps_fixed` 改成真实帧率（会影响 `sensor_find_frame_size` 的选窗逻辑）。
3. 高帧率下 ISP 吞吐：1080p@134fps ≈ 278MP/s，比 A733 ISP 标称 4K30 高；上 ISP 时建议 `frame_rate=60`。
4. 模块供电/复位脚：目前沿用 imx219 的 PE6 pwdn、无 reset，实测能正常探测和出流。
