# AR0234 × Orange Pi Zero 3W（A733）交接文档：待办任务

更新：2026-09-15。给接手的 agent 用：先读「0 背景」「1 当前状态」「2 环境」「3 必读的坑」，再从「4 任务」里领任务。
每个任务都写了背景、做法、验收标准。**完成一项就在这里更新状态。**

---

## 0. 背景与目标

- 用户把这套板子和相机当**工业相机**用：**定焦、固定曝光和增益**，最终要的是 **RGB 图像**。
  算法类型（AI 或传统视觉）、结果是在板上用还是把图传出去，**用户还没定**（见第 5 节）。
- 硬件：Orange Pi Zero 3W（Allwinner A733：2×A76 + 6×A55，ISP602，VE，G2D，NPU，PowerVR BXM-4-64，E902），
  AR0234 彩色全局快门，4-lane MIPI-A，RAW10 GRBG。**板子没有以太网**，USB 设备口只有 USB 2.0 high-speed。
- 相关文档：`README.md`（驱动和用户态说明）、`hwapi/README.md`（**全部硬件底层 API，先读**）、
  `../rt-check/`（RT 补丁试打结果）。

## 1. 当前状态（均已在板上实测）

| 模块 | 状态 |
|---|---|
| 传感器驱动 `ar0234_mipi.c` | ✅ 1920x1200/1080、1280x720、960x600（binning）；曝光、增益、帧率、翻转、测试图、温度。触发和闪光灯写了但**没测** |
| vin 模块 | ✅ `prebuilt/vin_v4l2.ko` = BSP 源码 + 补丁 0003 + `CONFIG_D3D` LBC 模式（3DNR）；`vin_io.ko` 必须用原装的 |
| ISP | ✅ NV12 / **BGR（硬件转换，请求 RGB3 或 BGR3 都得到 B,G,R 顺序）** / RAW BA10；ISP 硬件缩放直接输出 640x400 等小尺寸 BGR @120fps |
| 3A | ✅ `ar0234-3ad`（C++，systemd 开机自启）：按 ISP 事件为每路流启动 libisp，按帧率限制 AE 曝光，1200p120 自动关 3DNR |
| 硬件编码 | ✅ `ar0234-rec`（C++）H.264/H.265，零拷贝，`-e/-g` 手动曝光增益 |
| ISP 参数 | ⚠️ 初版：gc05a2 模板 + Kurokesu CCM/AWB；LSC/MSC 没做；`isp/isp_param_3dnr.bin`、`isp_param_no3dnr.bin` |
| 板子安装状态 | 模块、dtb（fdtput）、参数、服务都已装；内核和 dtb 包 apt-mark hold；备份 `vin_v4l2.ko.bak-pre-d3d`、`ar0234_mipi.ko.bak-pre-cpp`、`*.dtb.orig` |
| 补丁 | `patches/0001`（驱动 + Kconfig + Makefile + DTS）、`0002`（配置加 SENSOR_AR0234）、`0003`（vin 自动 S_INPUT 等）、`0004`（配置开 D3D LBC）；**都还没打进内核树** |
| git | 2026-09-15 已 `git init` 独立仓库（branch main），远端待用户决定；**推送必须私有**（含全志文件与板上口令），见 `BASELINE.md`、`ARCHITECTURE.md` |

BSP 基线：orangepi-build `bdba421`；内核 github `orangepi-xunlong/linux-orangepi` 分支 `orange-pi-6.6-sun60iw2` 提交 `8a9be72`（6.6.98）。

## 2. 环境与访问

- **串口**：`/dev/ttyUSB0` 115200；用户 `orangepi`，登录、ssh、sudo 的密码都是**一个空格**；`tools/sc.py "cmd"` 通过串口执行命令。
- **SSH**：`tools/ssh_board.sh "cmd"`（主机上没有 sshpass；IP 默认 172.16.0.193，可用环境变量 `BOARD` 覆盖）。
  板上 sudo 写法：`printf ' \n' | sudo -S -p '' <cmd>`。板上 `/tmp` 是 tmpfs，重启就清空。
- 长时间任务用 `sudo systemd-run --unit=xxx ...` 放到后台，不要在 ssh 里用 nohup。重启：`sudo systemd-run --on-active=2 /bin/systemctl reboot`。
- **用户态**：在板上本地编译（g++ 10.2 支持 C++20，没有 CMake）：
  `tar -czf - userspace | tools/ssh_board.sh "tar -xzf - -C ~/ar0234test"`，然后 `make -C ~/ar0234test/userspace -j8 && sudo make install`。
- **内核模块**：在主机上外部编译，约 9 秒。目录 `build/vin-d3d-lbc/`（vin 源码拷贝 + 0003 + `board_compat.h` 里定义 `CONFIG_D3D`/`CONFIG_D3D_LBC_MODE`、undef `CONFIG_AW_DMC_DEVFREQ`）：
  ```
  K=/home/helios/Desktop/orangepi-build/kernel/orange-pi-6.6-sun60iw2
  TC=/home/helios/Desktop/orangepi-build/toolchains/gcc-arm-11.2-2022.02-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-
  make -C $K M=$PWD ARCH=arm64 CROSS_COMPILE=$TC CONFIG_CSI_VIN=m CONFIG_AW_VIDEO_SUNXI_VIN=m -j6 modules
  ${TC}strip --strip-debug -o out/x.ko x.ko
  ```
  内核树属主是 root，里面还有 GPU 编译留下的改动，**不要直接修改**。打补丁用 `sudo bash apply.sh`，要先得到用户同意。
- ISP 调试：`analysis/ispreg_spy.c`（LD_PRELOAD 截获 libisp 下发的模块使能寄存器）；参数文件偏移 64 写 `isp_log_param=0x3` 可打开 AE/AWB 日志；
  `analysis/rawstat.c`、`nv12stat.c` 做逐帧统计。

## 3. 必读的坑（每一条都实际踩过）

1. **出过流之后不要 rmmod 或重新加载 vin 模块**：ISP 没被复位，之后一直 `isp0 width error`，只能重启。换模块要么装好后重启，要么开机后第一次出流前换。
2. ISP 出过错之后，**下一次测试结果不可信**，先重启再测。
3. `/dev/video0` 只允许一个进程打开（第二次打开返回 EBUSY）。3A 服务是通过 ISP 子设备事件工作的。
4. libisp 必须在传感器管线已绑定后才能 `ispStart`，否则报 `unable to initialize sensor subdev`。
5. libisp 和 cedarc 导出同名但不兼容的 iniparser 符号，**不能链接进同一个进程**。
6. 曝光、增益单位：
   - `/dev/v4l-subdev0`：exposure 按 1/16 行，gain 1600 = 1x，frame_rate 单位 fps；
   - `/dev/video0` 和 ISP 子设备：`exposure_time_absolute` 单位微秒，gain 256 = 1x。只有 libisp 运行期间改的才生效。
7. RGB3 和 BGR3 输出都是 B,G,R 顺序。
8. 3DNR 在 1920x1200@120（消隐只有 16 行）下每帧丢帧；PKG 模式报 width error；COMPRESS_EN 会 IOMMU fault。
9. VE：输入缓冲按 16 对齐；零拷贝时 `nShareBufFd=-1`；H.265 `nGopSize` 大于 63 会被静默改成 2。
10. 不带帧率请求时，AE 的长曝光会拉长帧长（通用程序要用 `frame_rate` 控件）。
11. 不要 `apt-mark unhold` 后升级内核，不要装 Debian 的 `linux-image-arm64`。`apt upgrade` 升级系统软件是安全的。

---

## 4. 任务清单

优先级：**P0 = 无阻塞、价值高，先做**；P1 = 无阻塞、次要；P2 = 被外部条件卡住（硬件、厂商、用户决定）。

### P0

#### T1 仓库整理（推送被 URL 卡住）
- 在 `ar0234-port` 里 `git init`（独立仓库），写 `.gitignore`：排除 `build/`、`analysis/` 下的 `*.h264 *.h265 *.raw *.yuv *.bgr *.png *.jpg`、`samples/` 里的视频、`userspace/build/`。
- 新建 `BASELINE.md`，记录第 1 节的 BSP 基线版本和复现步骤。
- 决定 `tools/ar0234_rec.c`、`tools/ar0234_3a.c`（旧 C 版）是删掉还是移到 `legacy/`，**先问用户**。
- `hwapi/board-include`、`kernel-uapi`、`symbols`、`isp/template_gc05a2_a733.blob` 是全志的文件，只能放私有仓库。
- 验收：`git status` 干净，仓库不超过约 3MB；远端 URL 由用户提供后再推送（提交署名按系统提示）。

> **2026-09-15 完成**：`git init`（main）；`.gitignore` 对 `analysis/` 用白名单（只留文本/源码）+ 全局媒体扩展名；新增 `BASELINE.md`、`ARCHITECTURE.md`；旧 C 工具移入 `legacy/`（未删，要删随时可删）；`prebuilt/` 保留（468K，板上部署依赖它）；hwapi/ 与 isp/template blob 标注为厂商文件、推送需私有仓库。

#### T2 采集库：支持 BGR/RAW/任意尺寸，加帧元数据和看门狗
- `userspace/include/ar0234/v4l2.hpp` 里的 `CaptureConfig` 加 `pixel_format`（NV12、BGR3、BA10、GREY）；按 `bytesperline` 和 `sizeimage` 处理步长，注意 `sizeimage` 按 16 对齐。
- `Frame` 暴露序号、时间戳（CLOCK_MONOTONIC），并统计丢帧（序号跳变）。
- 看门狗：超过 N ms 没收到帧就自动重新开流（应对 T6 的偶发问题）。
- 示例 `apps/ar0234-grab.cpp`：用 `cv::Mat(h, w, CV_8UC3, data, bytesperline)` 零拷贝包装 BGR。板上没有 OpenCV，装 `libopencv-dev` 前先问用户；也可以先输出 PPM 文件验证。
- 验收：1920x1200 BGR 连续 10 分钟 ≥110fps、640x400 BGR 120fps、BA10 16bit 数据正确；丢帧计数和 dmesg 的 `frame lost` 一致；人为停流后能自动恢复。

#### T3 3A 服务「工业固定模式」
- 配置文件 `/etc/ar0234.conf`：`mode=auto|fixed`、`exposure_us`、`gain`、`awb=auto|fixed`、`wb_temperature` 或 R/B 增益、`params=3dnr|no3dnr|industrial`。
- 两条实现路径，**都要先验证**：
  - (a) 服务在 libisp 启动后，对 **ISP 子设备 `/dev/v4l-subdev12`** 设置 `auto_exposure=1`、`exposure_time_absolute`、`gain_automatic=0`、`gain`、`white_balance_automatic=0`、`white_balance_temperature`（这个节点已经暴露了同一组控件，但**还没验证 libisp 会不会响应**）。
  - (b) 在参数文件的 test 段写固定值：`manual_en`（偏移 88）、`isp_gain`（68）、`isp_exp_line`（72），`ae_en`、`awb_en` 置 0（偏移见 `tools/make_isp_bin.py` 和 `analysis/libisp-offsets/setters.json`（从 libisp_ini.so 的 set_* 函数反汇编得到））。
- 验收：打开 AE/AWB 日志，EXP_TIME、AGAIN、WB Gain 全程不变；开关灯时画面亮度和色偏不变；任何程序（包括 `v4l2-ctl`）出流都生效。

#### T4 工业 ISP 参数集 `isp_param_industrial.bin`
- 关掉 3DNR（tdf）、锐化、PLTM、DRC、去雾、CEM、LCA、GCA；gamma 要么关掉，要么换成线性表（先查 gamma 表偏移）；保留 BLC、DPC、CTC、WB、CFA、CCM。
- 扩展 `tools/make_isp_bin.py` 支持 `--set 模块=0`（已有）和生成线性 gamma。
- 验收：用 `analysis/ispreg_spy.c` 确认对应寄存器位；做曝光扫描（固定增益，曝光按 1、2、4、8 倍变化），输出亮度基本线性（不饱和区相关系数 >0.99）。

#### T5 deb 附加包 `ar0234-camera`
- `packaging/build-deb.sh` 或在 Makefile 里加 `make deb`：包含两个 .ko（放 `/lib/modules/6.6.98-sun60iw2/updates/`）、两个程序、systemd 服务、modules-load 配置、udev 规则、参数文件。
- **先在板上验证 `updates/` 目录里的模块会不会优先于原装模块加载**（板上没有 depmod.d 配置，靠 kmod 默认搜索顺序）。
- postinst：备份后用 fdtput 改 dtb、depmod、启用服务；prerm/postrm：恢复 dtb 和模块、停服务。
- `Depends: linux-image-current-sun60iw2 (= 1.0.0)`。
- 验收：干净的板子装包、重启、相机正常；卸载、重启，恢复到原装 imx219 设备树和原装模块。

> **2026-09-16 完成**：`packaging/` 一条命令出包（必须 `-Zxz`——板上 dpkg 太老不认
> zstd 的 control 归档）；板上全流程回归通过：先 apt 本地重装官方 1.0.0 包还原出厂态
> （apt 源里下不到这两个包，官方 deb 在主机 `output/debs/`）→ 装 deb 重启 → `modinfo -n`
> 解析到 `updates/`（kmod 默认优先级成立，无需 depmod.d）、模块自启、`ar0234-3ad` active、
> cap 实测 1920x1200@120fps 出流 → 卸载重启 → dtb/模块/服务/参数全部还原。
> 板上现为 deb 安装态（deb 副本在 `~/ar0234test/`）。剩：内核升级时需重编模块出新包
> （`Depends (= 1.0.0)` 会挡 apt 升级，属预期行为）。

#### T6 查清 BGR 1920x1200@120 偶发一帧不出
- 现象：十几次里有 1 次开流后持续 `isp0 frame lost`（当时 3A 服务已经正确选了 no3dnr），之后复现不了。
- 猜测：开流时内核沿用上一次 libisp 的寄存器表（`sunxi_isp.c` 里 `load_flag` 时 memcpy `load_shadow`），其中还开着 D3D。
- 做法：写一个压力脚本，混合格式、模式、帧率随机开流 200 轮以上；如果能复现，试在 `sunxi_isp.c` 开流时检测消隐不足并清除 `D3D_EN`（`bsp_isp_module_disable(id, D3D_EN)`）。
- 验收：200 轮 0 失败。

#### T7 G2D 开机加载、权限、C++ 封装
- `/etc/modules-load.d` 里加 `g2d_sunxi`；udev 规则 `KERNEL=="g2d", GROUP="video", MODE="0660"`。
- 封装 `BITBLT_H`（裁剪、缩放、旋转）和 `FILLRECT_H`/`BLD_H`（画框、叠加），输入输出都用 DMA-BUF fd（`g2d_image_enh.fd`）。
- 验收：ISP 的 BGR 或 NV12 缓冲（EXPBUF）交给 G2D 画框、裁 ROI，零拷贝，1200p120 下 CPU 占用可忽略；输出和 CPU 实现逐像素对比一致。

### P1

| ID | 任务 | 要点和验收 |
|---|---|---|
| T8 | orangepi-build 正式集成 | 0001、0003 放 `userpatches/kernel/sun60iw2-current/`；配置放 `userpatches/linux-sun60iw2-current-a733.config`（包含 0002、0004 的改动）；`./build.sh BOARD=orangepizero3w BRANCH=current BUILD_OPT=kernel REVISION=1.0.1`。**装板前先准备回退方案**（备份 /boot，确认串口能救）。验收：新 deb 装好后不用 fdtput、不用 prebuilt 模块，相机就正常 |
| T9 | 驱动 ROI 裁剪（AOI） | 传感器窗口寄存器（0x3002–0x3008）+ sunxi-vin 动态窗口，或者用 V4L2 selection 接口。窗口越小帧率越高。验收：比如 1920x400 实测帧率和计算值一致，画面位置正确 |
| T10 | MP4 封装和 JPEG 抓图 | 板上有 libavformat 58 运行库，要装 `libavformat-dev`；另做 `ar0234-snap`，用 `AWJpecEnc` 硬件 JPEG。验收：MP4 时间戳正确、能正常播放；JPEG 能正常打开 |
| T11 | 长时间稳定性 | 分两组各连续 4 小时：① 1200p110 BGR 采集（T2 的程序）；② 1200p110 NV12 + VE 录像（`ar0234-rec -n 0`）。video0 同一时间只能输出一种格式，所以不能同时跑。记录：记录传感器温度（`temperature_approx_degc`）、SoC 温度、丢帧、内存、dmesg。输出一份报告 |
| T12 | 实时性基线 | 装 `rt-tests`，带相机满负载跑 `cyclictest`；再试 `isolcpus` + `SCHED_FIFO` + 中断绑核。报告最坏延迟。RT 内核暂缓（见 `../rt-check/`） |
| T13 | GPU OpenCL | 只装头文件（`opencl-c-headers`），**不要装替换厂商 libOpenCL 的 ICD 加载器包**；列出扩展，查有没有 DMA-BUF 导入；写一个 RAW 去马赛克 kernel 测性能 |
| T14 | ISP 双路输出 | 设备树 `vinc10`（同一个 ISP，第二个缩放器）用 fdtput 打开（先备份），重启；测试 video0 输出 1920x1200、另一个节点同时输出 640x400 |
| T15 | dmesg 噪声 | 开流、停流时偶发 `isp0 configuration error/height error`、`video0 has already stream off`：查清来源，确认无害或者修掉 |
| T16 | vin 重载后 ISP 坏 | 加载时 `Get isp reset control fail`：查 DTS 里 isp 节点有没有 `resets`，或者在 probe 里强制复位 ISP。验收：出流后 rmmod/modprobe 能继续出流 |
| T17 | 单元测试和集成测试 | 给 `IspParamSets::needs_3dnr_off`、帧率取整、参数原子替换写单元测试；把各模式、各格式的回归测试写成脚本 |

### P2（被卡住）

| ID | 任务 | 卡在 |
|---|---|---|
| T18 | 外部触发、从机同步、闪光灯实测；vin 对不定时帧的处理；配合手动曝光 | 用户接线（TRIG 是 **1.8V** 电平，要电平转换）；40pin 的 PWM 引脚映射要查设备树和原理图（pwmchip0/10/20 各 10 路） |
| T19 | NPU：ISP 640x400 BGR → `vip_create_buffer_from_fd` → 推理（比如 YOLO） | 厂商 NPU SDK（`libVIPlite.so` + 模型转换工具，版本要和驱动 1.13.0 匹配） |
| T20 | ISP 标定：LSC/MSC 暗角表、AWB 光源表、CCM、降噪 | **2026-09-16 重大进展：LSC/MSC 表偏移已逆向完成**（无需 Tuning Tool）——参数体 = 内核 `isp_tuning_priv.h` 布局 + bayer_gain 前插 2848 字节；LSC 表 @3130（u16[12][768]，3 通道×256 点径向，Q10，1.0=1024）、触发色温 @21562、MSC @21574/21632，全部经 setter 反汇编 + 数据特征双重验证，详见 `analysis/libisp-offsets/offs.txt`。工具链已就绪：`tools/calibrate_lsc.py`（平场 RAW → 增益表，合成数据验证通过）+ `make_isp_bin.py --lsc-json`（注入，回路验证通过）。**剩：实拍平场标定 + 上板开关 LSC 对比验证**（注意 channel 顺序 R/G/B 和中心坐标 2048,2048 的假设要用实拍确认）。CCM/AWB 已可标定（偏移早就知道）。仍缺：对焦、标定器材 |
| T21 | 黑白版 AR0234（芯片 ID 0x1A56）：Y8/Y10 格式，ISP 旁路 | 需要黑白模组 |
| T22 | 图像外传：USB UVC gadget（内核已支持）或 USB 网卡 | 用户决定方案；USB 2.0 带宽下，全分辨率不压缩最多约 15fps |
| T23 | 对照模组原理图核对电源和复位引脚（现在沿用 imx219 的 PE6 pwdn，reset 注释掉了） | 模组原理图 |
| T24 | PREEMPT_RT 内核 | 用户暂缓；补丁试打无冲突，见 `../rt-check/` 和记忆 |
| T25 | E902 小核产生精确触发 | 用户的 `~/e902` 脚本、固件工具链 |

## 5. 需要用户决定或提供

1. git 远端 URL，公开还是私有（T1）
2. 算法类型：AI 还是传统视觉；要彩色 BGR、灰度还是 RAW；要多大分辨率和帧率（决定 T2、T4、T7、T13、T19 的优先级）
3. 结果在板上用，还是把图像传出去，用什么方式（T22）
4. 固定曝光、增益、白平衡的具体数值，以及对颜色准确度、线性度的要求（T3、T4、T20）
5. 要不要外部触发，触发源是什么，能不能接线（T18）
6. 能不能在板上装 OpenCV、libavformat-dev、rt-tests 这些包
7. 找香橙派或全志要：NPU SDK、ISP Tuning Tool、模组原理图
