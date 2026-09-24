# AR0234 × Orange Pi Zero 3W（A733）交接文档：待办任务

> **2026-09-24 仓库重组说明**（本文正文是当时的记录，路径以此为准）：
> `patches/`、`apply.sh`、`prebuilt/*.ko` 已删除 —— 内核补丁唯一出处是 orangepi-build 的
> `userpatches/kernel/sun60iw2-current/`（构建时自动打上，0013 已按新内核基线重做），模块由内核包提供；
> 一次性实验脚本移到 `analysis/scripts/`，测试程序移到 `tools/hwtest/`，救援脚本移到 `tools/rescue/`；
> 板子 sudo 统一用 `tools/ssh_board.sh -s "<cmd>"`。

更新：2026-09-17（**第十轮：纯软件批次 C10–C15 全部完成**）。改了用户态（C10 NPU 零拷贝程序、C14 `/etc/ar0234.conf`、C13 sync 修复、C15b 3ad 两处修复）、新增内核补丁 `0013`（只读诊断）、正式化了 `../userpatches/`（C12）、交付 `tools/cma_resize.sh`（C11）。**板上现在跑的就是 userpatches 的重建产物**（DTB md5 8b1d83…，vin_v4l2 srcversion 456F967D…），且 1200p120 回归 120.02 fps / 0 丢。**没有跑 `apply.sh`、没有 `git commit`**。
本轮报告：**`analysis/round10/REPORT.md`**（每个任务一节，命令+退出码+原始输出+证据等级）。
**剩余工作请看同目录的 `docs/NEXT-TASKS.md`** §0.0（第十轮完成表）与正文（A 需硬件 / B 改 DT 有风险 / D 低优先 / E 已排除的死路；C 组已清空，长出了 N1–N4 小尾巴）。
最近几轮的报告：
- **第十轮** `analysis/round10/REPORT.md`（NPU←ISP 零拷贝 300/300 逐字节一致、/etc/ar0234.conf 固定模式实测、userpatches 黄金验证、**1080p 高帧率风暴机制定位与修复**、CMA 前提订正（IOMMU）、0013 诊断、isp01 只读解释）；
- **第九轮** `analysis/round9/REPORT.md`（ISP 硬复位写实验 = 确认、`patches/0012` 修 PM 引用计数、H.265@1200p120 **未复现**、G2D 位精确封装、`DMA_BUF_IOCTL_SYNC` 旧编码）；
- **第八轮** `analysis/g2d/REPORT.md`（G2D 机制级结论：4:2:0 重采样滤波器 / Y 下钳 16 / 两次 Y8 位精确 / 0x28 vs 0x29）；
- **第七轮** `analysis/fix-round/REPORT.md`（G2D 开机加载与权限、开流看门狗、工业 ISP 参数集、LC21 显式失败、CSI Bandwidth、PM 引用计数定位）。
上一轮：2026-09-16（**第六轮：底层硬件栈可用性审计**）。完整矩阵/深度分析/与文档不符清单见 **`analysis/REPORT-0916-hardware-audit.md`**。
**本文件与报告冲突时，以报告为准**（报告里每格都有命令、退出码与关键输出）。
每个任务都写了背景、做法、验收标准。**完成一项就在这里更新状态。**

## 3.32 第十轮订正与新增 —— 2026-09-17（读 §3.30 之后接着读这条）

- **§3.30 C11 前提作废**：CMA 16 MB **不是**相机栈的约束。DT 里 vinc*/isp/tdm 全带 `iommus`，捕获缓冲（videobuf2_dma_contig 3.46 MB×4）是 IOVA 连续、物理散页，1200p120 满载时 `CmaFree` 与空载一字不差（13952 kB）。别再为相机调大 CMA；`tools/cma_resize.sh` 是给未来"无 IOMMU 的物理连续消费者"预备的，已端到端验证（16→64→16）。
- **§3.30 C15b 口径作废**：不存在"136 fps 硬边界"。1080p 请求 ≥133 时，3ad 把测得 fps 喂给 `ispSetFpsRanage`，**该调用会把 isp_fps 写回驱动并在流中改写 FLL**（vts→1100/1096，vblank→136/109 µs），触发内核 0006 interlock（强关 3DNR）与 3ad 旧规则（1080p 装 3DNR-ON 参数集）打架 ⇒ `sunxi_isp_reset` 风暴 0 帧；"134/135 干净、136 必崩"是概率抽样（首两帧测量值 132 vs 133/134 漂移）。已修（3ad 对齐 interlock + fps 钳 120）。**新口径：3A 在位时请求 >120 稳定交付 120 fps；不带 3A（libisp）可到 133.4 fps 稳定**。
- **v4l2 子设备 3A 控件是死的**：`__sunxi_isp_ctrl()` 的唯一消费路径在 `CONFIG_ISP_SERVER_MELIS` 下面，本内核未编入 ⇒ 写 exposure/gain/WB 控件到 sunxi_isp 子设备不会有任何效果（只有 video 节点直驱 sensor 的控件有效，且仅在没有 libisp 抢时）。
- **`ispSetFpsRanage` 会改传感器帧率**，不只是曝光上限——它是把流推进 interlock 冲突区的扳机；3ad 现在把它钳到 120（语义无损：传感器最长快照 1216 行=8.27 ms < 1/120 s）。
- **手动编内核/模块必须 `LOCALVERSION=-sun60iw2`**（build.sh 经 compilation.sh:498 以环境变量传入，.config 里是空串）；改完删 `include/config/kernel.release` 再编，否则 vermagic 用缓存，insmod 报 "Exec format error"。
- **板上重启清 /tmp**：跨重启的脚本放 `$HOME`；**本地掐 ssh 不会掐远端进程**：长任务一律 `systemd-run`（第一轮 watchdog 压测因此和重跑的第二轮撞了 /dev/video0，"Device or resource busy" 的真相）。
- **`vip_create_buffer_from_fd` 实测可用**：直接吃 `/dev/dma_heap/system` 的 dma-buf；fd 路径 cache 维护按头文件契约宿主不管（设备↔设备本来就不经 CPU），A/B 输出逐字节一致（300 帧）。`G2D_FORMAT_BGR888` 内存序 = B,G,R（与名字一致）。vpm_run 那个 .nb 是 2 类分类头，224×224×3 UINT8。
- **libisp ctx 缓存**（`/mnt/isp0_*_ctx_saved.bin`）会压过参数文件刚改的值——换参数集/打固定模式补丁后要 `rm -f` 它（P0-3 老规矩）。
- **挂账疑点（不影响验收）**：流结束后 video0 的 gain 控件读回 1.0×（exposure 读回正常）——疑似 close 路径复位 `info.gain`；NEXT-TASKS N3。

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
| vin 模块 | ✅ **2026-09-17 第九轮板上实测核对：`srcversion BA48201D23BA4260923B53E`、md5 `375f6f4f5c15f72b2d2323f32e7af087` = 补丁 0009 + 0010 + 0011 + 0012**（`build/vin-d3d-lbc/out/vin_v4l2-0012.ko`；回退备份 `…/updates/vin_v4l2.ko.bak-pre-0012`）。`vin_io.ko` 必须用原装的。**注意 `srcversion` 的写法：是 `…4260923B53E`，不是 `…4260993B53E`**（`analysis/round9/REPORT.md` 与板上一致，只有这一处正确）。版本史：0009 = `843BC1A03606D35EC062D57`/md5 `2715c25c…`；0009+0010+0011 = `7922F65188E60D338D42B73`/md5 `241c8631…`。第四轮写的「板上是 0008」**已过时**。重编用 `tools/build_vin.sh <tag>` |
| ISP | ✅ NV12 / **BGR（硬件转换，请求 RGB3 或 BGR3 都得到 B,G,R 顺序）** / RAW BA10；ISP 硬件缩放直接输出 640x400 等小尺寸 BGR @120fps |
| 3A | ✅ `ar0234-3ad`（C++，systemd 开机自启）：按 ISP 事件为每路流启动 libisp，按帧率限制 AE 曝光，1200p120 自动关 3DNR |
| 硬件编码 | ⚠️ **2026-09-17 第九轮订正**：`ar0234-rec`（C++）H.264 零拷贝 ✅（1200p120 实测 **103.3 fps**，encode 9.6 ms/帧；第九轮同参数另一次 240 帧/2.11 s = **113.6 fps** ⇒ **这个数字与取景内容/码率相关，别当常数**）；**H.265 在 1920×1200@120 的「直接失败」在第九轮 6/6 次精确重复里全部通过**（117.67–118.66 fps、7.0 ms/帧，比同参数 H.264 还快、码率约 1/3）⇒ 那次 6 帧失败的成因是**开流坏启动（5~10%，原 T6/P0-2）**，不是 codec 或「高度对齐」（后者已证伪，见 §3.30）；1200p60、1200p30、1080p30、720p120 本来就 ✅；1080p118.2 fps；`-e/-g` 手动曝光增益可用 |
| 硬件 JPEG | ✅ 第六轮复核：`VideoEncCreate(VENC_CODEC_JPEG)` 走 VE2 零拷贝，`tools/hwtest/jpeg_test.cpp` 出有效 1080p JPEG（**本次 q90 = 239 725 B**，SOF0 1920×1080×3；早先记的 33KB 是别的质量/内容，**不要当固定值**） |
| G2D | ✅ 可用（第八/九轮机制级结论，证据 `analysis/g2d/REPORT.md` + `analysis/round9/REPORT.md` T5）。**G2D 开机加载与权限已修**：`/etc/modules-load.d/g2d.conf` + udev `KERNEL=="g2d", GROUP="video", MODE="0660"`，冷启动实测 `/dev/g2d` = `crw-rw---- root video`，`orangepi` 用户免 sudo 可用。版本 `0x10112114`，BITBLT/2× 缩放可用。**「位精确」完全取决于走哪条路径（质化结论，不要再引用任何「差异字节数」——三份报告的数字互相矛盾、不是常数，见 §3.30）**：① 单次 4:2:0 `YUV420UVC` blit **永不位精确**（Y 被硬钳到 ≥16 ⇒ 暗部被削；色度进 mixer 后被 4:2:0 重采样滤波器处理），**不是**缓存/同步/地址/对齐问题；② **两次 `G2D_FORMAT_Y8` blit 才位精确**（同一缓冲看成 `w×1.5h` 单平面图，`clip_rect.y=0/h=Y`、`y=h/h/2=UV`），已封装成 `ar0234::G2d::move_nv12()`（`userspace/include/ar0234/g2d.hpp` + `src/g2d.cpp`，含 ISP dma-buf 零拷贝源）；③ `ARGB8888` 在 `alpha=0xFF` 时逐通道位精确，随机 alpha 会触发预乘（alpha=0 的像素 RGB 被清零）；④ 画框用 `G2D_CMD_FILLRECT_H`，`dst_image_h.color = (Y<<16)|(U<<8)|V` 原样写入、裁剪正确。**格式常量：NV12 = `G2D_FORMAT_YUV420UVC_V1U1V0U0`（`0x28`）；名字像「U 在前」的 `…U1V1U0V0`（`0x29`）其实是 NV21** —— 第六/七轮的探针用错了 0x29（YUV→YUV 拷贝自洽看不出来，ARGB 方向会红蓝对调）。别忘：`bbuff` 必须 1；测试时先停流 |
| 硬件解码 | ⚠️ 2026-09-16：**解码本身可用**——厂商 `/usr/bin/vdecoderdemo -i t30.h264 -codFmat 1 -o /tmp/dec.out -n 5 -sn 5 -outFmat 1` 实测解出 1080p。关键：必须先 `AddVDPlugin()`（加载 libaw*.so，链接 -lvideoengine），否则报 unsupported format。**自研精简封装会把内核挂死（串口/网全断，需断电）**——demo 是喂流/取图双线程模式，集成时照抄其结构 |
| ISP 参数 | ⚠️ `isp/isp_param_3dnr.bin`、`isp_param_no3dnr.bin`（gc05a2 模板 + Kurokesu CCM/AWB；**LSC/MSC 关着 = 暗角未校正**，缺均匀光源/积分球）；**第七轮新增已交付** `isp/isp_param_industrial.bin`（关 defog/lca/gca/sharp/pltm/drc/cem/cnr/gtm + 线性 gamma，曝光线性度 **r = 0.99989** vs 出厂集 0.95676），板上**没有**设成默认。**偏移口径：文件偏移 = 结构体偏移 + 74** |
| 板子安装状态 | 模块、dtb（fdtput）、参数、服务都已装；内核和 dtb 包 apt-mark hold；备份 `vin_v4l2.ko.bak-pre-0012`/`.bak-pre-0010`/`.bak-0008`/`.bak-0006`/`.orig-bsp`/`.fix-t14`/`.pre-0006`、`ar0234_mipi.ko.bak-pre-cpp`、`*.dtb.orig*`。**2026-09-17 第九轮实测：板子健康、DTB 原样**（`d4ee5b68…`）、`/dev/video0` + `/dev/video4` + `/dev/g2d` 都在、`ar0234-3ad` active、1200p120 回归 ≥119 fps。**第四轮写的「板子停在需要断电重启的状态」已完全过时（第六轮就已核实板子健康），不要再照它做动作。** 当前唯一硬阻塞是「第二颗模组没插」（`sensor@5812010`/`5812020` 都保持 `disabled`，这是**正确状态**） |
| 补丁 | `patches/0001`（驱动+Kconfig+Makefile+DTS）、`0002`（配置加 SENSOR_AR0234）、`0003`（vin 自动 S_INPUT 等）、`0004`（配置开 D3D LBC）、`0005`（第二 vinc 绑定顺序）、`0006`（**3DNR/消隐互锁，1200p120 丢帧的正式修复**）、`0007`（**`__vin_sensor_setup_link` 空指针 panic 修复**）、`0008`（**第二个 VI 通道 `/dev/video4` 打通 + 5 处防静默保护**）、`0009`（**关流完整回滚，修掉 §3.27**）、`0010`（**LC21/LBC 输出改成显式 `EINVAL`**）、`0011`（**`CSI Bandwidth` 整数截断修复**）、`0012`（**`__vin_pipeline_close()` 电源引用计数修复**，`analysis/round9/REPORT.md` T2）。`apply.sh` 现在按序施 0001–0012。**都还没打进内核树**（`apply.sh` 未跑）；`0003+0005+0006+0007+0008` 施加到原始 BSP 后与 `build/vin-d3d-lbc` 逐字节一致（验证命令见 `analysis/REPORT-0916-0006-0007-dual.md` §4.3 与 `REPORT-0916-0008-second-channel.md` §8.3） |
| git | 2026-09-15 已 `git init` 独立仓库（branch main），远端待用户决定；**推送必须私有**（含全志文件与板上口令），见 `docs/BASELINE.md`、`docs/ARCHITECTURE.md` |

BSP 基线：orangepi-build `bdba421`；内核 github `orangepi-xunlong/linux-orangepi` 分支 `orange-pi-6.6-sun60iw2` 提交 `8a9be72`（6.6.98）。

## 2. 环境与访问

- **串口**：`/dev/ttyUSB0` 115200；用户 `orangepi`，登录、ssh、sudo 口令见 `$BOARD_PASS`（现在的镜像是默认的 `orangepi`）；`tools/sc.py "cmd"` 通过串口执行命令。
- **SSH**：`tools/ssh_board.sh "cmd"`（主机上没有 sshpass；IP 默认 172.16.0.193，可用环境变量 `BOARD` 覆盖）。
  板上 sudo：`tools/ssh_board.sh -s "<cmd>"`。板上 `/tmp` 是 tmpfs，重启就清空。
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
2. **「ISP 出过错就得重启」是错的（第六轮实测推翻，第七轮写进 §3.28c，第九轮补上「资源性原因」这一类）。精确规则要按成因分**：
   - `isp0 frame lost!` / `sunxi_isp_reset` 风暴（例如 1080p 请求 136 fps）→ **停流重开即可，不要重启**
     （实测同一个 boot 里 400 次 reset 之后，紧接的 1200p120 立刻 119.78 fps / 0 丢帧）；
   - 开流「只出 4~6 帧就死」（**实测 40 次里 4 次 = 10%（第六轮）、3/40 = 7.5%（第七轮关掉看门狗的基线）、1/30 = 3.3%（第九轮），合计 70 次 4 次 = 5.7%**）→ **不要重启**，重开一次必好，
     `userspace/` 的采集库已经用「开流看门狗」自动兜住（第七轮新增，40 次里坏启动残留 0）；
   - **只重载了传感器模块**（`no link to sunxi_mipi.0`；link 只在 probe 时建）→ **必须重启**；
   - ⚠️ **资源性原因（时钟/供电）救不回来**：把 ISP 核心时钟压到 162 MHz 后，`vi0` 帧计数直接停在 0 / 或刷 `isp0 hblank short` + reset 风暴，**就算把时钟调回 324 也救不回来，只能重启**（`analysis/t17/REPORT-0916-isp-capacity.md` §2.2 确认）。这一类和上面的行时序类要分开判断；
   - 只有出现 `isp0 width error`、或连续多次重开仍然 0 帧，才考虑重启。
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
11. **开 ISP 前必须先限帧率**：`v4l2-ctl -d /dev/v4l-subdev0 -c frame_rate=30`。sensor 默认跑满
    （1080p 133fps，FLL=1096，消隐只剩 16 行）时 ISP 每帧丢帧，dmesg 刷 `isp0 frame lost!` +
    `sunxi_isp_reset:isp0 reset!!!,ISP frame number is 0`，看起来像驱动坏了/双路坏了，其实只是帧率。
    实测：fr=30 → 120/120 帧、0 报错；fr=60 → 58fps 正常；默认 133 → 8 秒只有 7 帧。
12. **板子 panic 会整机冻死，但可以自动恢复**：`sunxi-wdt` 已绑定 `2050000.watchdog`，实测 open
    `/dev/watchdog` 后不喂狗 **16 s 整机复位**。开启 `RuntimeWatchdogSec=16s`
    （`/etc/systemd/system.conf` + `systemctl daemon-reexec`，**本轮已开**）后 panic/硬挂都能自动重启，
    不必手动断电。回退：改回 `0`。
13. **`tools/serial_log.py`**（本轮新增）：独占 `/dev/ttyUSB0` 全量落盘串口日志 + 向
    `/tmp/t14.cmd` 追加一行即等于在板子控制台敲一条命令。**它开着的时候 `tools/sc.py` 不能用**；
    收工记得 `sudo systemctl stop t14serial`。
14. **HANDOFF/README 可能落后于板子**：接手先跑
    `md5sum /lib/modules/*/updates/*.ko`、`cat /sys/module/vin_v4l2/srcversion`、`/proc/uptime`、
    `ps ax | grep v4l2-ctl`。本轮就发现文档说「第二路已禁用、修复待验证」，实际板上修复版模块 + 第二路
    已启用 + 上一轮测试进程挂着 7.8 小时。

15. 不要 `apt-mark unhold` 后升级内核，不要装 Debian 的 `linux-image-arm64`。`apt upgrade` 升级系统软件是安全的。

16. **换 vin 模块必须重启**：`vin_v4l2` 与 `vin_io` 互相引用，`rmmod vin_v4l2` 会报 `Module vin_v4l2 is in use`（`lsmod` 里 use count 1 但 used-by 为空），`rmmod -f` 之外的组合都卸不掉。装模块 → `depmod -a` → 重启。
17. **只重载传感器模块会 panic（0007 已修，但仍有后遗症）**：`rmmod ar0234_mipi; modprobe ar0234_mipi` 之后任何 `VIDIOC_S_INPUT`（v4l2-ctl/GStreamer/vfr/OpenCV 都会调）曾触发 `mutex_lock` 空指针 panic（看门狗 16 s 拉回）。装上 0007 后变成`sensor …: no link to sunxi_mipi.0, cannot (dis)able it` + `S_FMT: Invalid argument`，**不 panic 了，但管线要重启才能恢复**（`vin_create_media_links()` 只在 probe 时建 link）。回归脚本 `analysis/scripts/t16b.sh`。
18. **dmesg 环形缓冲只有约 1900 行，会被刷屏吃掉**：一次 1080p133 的 `frame lost` 刷屏（894+ 行）就能把开机 probe 阶段的证据全部挤出去。**每次重启后第一件事**：`tools/ssh_board.sh -s "dmesg" > analysis/dmesg-boot-<标签>.txt`。
19. **`isp01` 绝对不要打开**：`isp@58ffffc` 是同一块 ISP602 的虚拟实例（见 T14）。把它 `status = okay` 之后**第一路会静默 0 帧**（`VIDIOC_S_INPUT` 返回空设备名、`vi0 prs_in y:0`、内核一条错都不报）。厂商把 isp01/02/03 全部 `disabled` 就是这个原因。开关用 `tools/dt_second_cam.sh isp01-on|isp01-off`（带 dtb 备份）。
    **2026-09-16 第四轮更正机制**：加了驱动保护（0008）之后 `sunxi_isp.1` **不再被注册**（media graph 21 个实体与正常启动完全一致），**但第一路照样 0 帧** ⇒ 坏事的是**这个 DT 节点被 enable 本身**，不是"多注册了一个空媒体实体"；驱动里那条 ERR 只能让它**不再静默**，不能阻止损坏。机制不在 `sunxi_isp.c` 里（唯一残差是启动多一行 `vin_isp 58ffffc.isp: Adding to iommu group 0`），**未定位**。
20. ~~**1920x1080 的可用上限是 132 fps，不是驱动给的 133**~~ **（第六轮订正，见 §3.28b-1：130/132/133/134/135 全干净，136 才崩）**：133 fps（vts 1100，消隐 136 µs）会从第 4 帧起 `isp0 frame lost!` + reset 无限循环（`s_fmt … fll = 1100 (133 fps)`），而 132 fps（vts 1109）0 丢帧。注意 136 µs 消隐在 1200p120 / 720p198 上都能跑，所以这不是「消隐不够」，是 1080p 模式自己的问题。**用户的 1200p120 目标不受影响。**
21. **1200p120 实测 fps 会在 119.07–120.04 之间浮动**：传感器驱动按 `frame_rate` 控件当前值算 FLL，AE 的长曝光会拉长帧长（同 §3.10）。两种情况下 `lost_cnt` 都是 0，但写报告/指标时不要写死「正好 120.000」。
23. **`vinc*_isp_tx_ch` 必须是 0**（第四轮，T14 的根因）：这个属性会经 `csic_vipp_input_select()` 写进 `CSIC_VIPPx_IN`，语义是"喂这个 VIPP 的 **ISP 输出通道号**"。ISP602 只有一个输出通道，写 1 的后果是——scaler 配好了、VIPP 使能了、DMA 起了、**内核一条错不报、video 节点就是 0 帧**。在位实验：`/dev/video4` 0 帧时用 `tools/vinreg w 0x58008a4 0` 把 `CSIC_VIPP1_IN` 从 1 改回 0，**立刻变 120 fps**。0008 已强制回 0 并打 ERR。
24. **读 VIN 寄存器要先出流**：空闲时 CSIC/VIPP 的时钟被门控，`/dev/mem` 读出来**全是 0**（看着像地址错了）。用 `tools/vinreg.c` 编出的 `/tmp/vinreg`，在 `vfr` 跑着的时候读。
25. **不要在这块板上用 `init=/bin/sh` 救砖**：`boot.scr` 生成的 consoleargs 是 `"console=ttyS0,115200 console=tty1"`，**`/dev/console` 是 tty1（HDMI）**，shell 落在 HDMI 上、串口既看不到也进不去；而且没有 systemd ⇒ **16 s 看门狗不会被打开**，卡住只能断电。要用就显式写 `setenv extraargs "console=ttyS0,115200 init=/bin/sh"`。**串口 SysRq 也不可用**（控制台驱动是厂商 `uart-ng`；BREAK / `tcsendbreak` / 降波特率造长 BREAK 实测三种都没反应）。
26. **在 `sensor@5812020`/`sensor@5812010` 上设 `status = okay` 而没有对应模组 ⇒ probe 阶段 panic + boot loop**：`v4l2_i2c_new_subdev()` 建了 client 后认为驱动没绑上，走失败回滚 `i2c_unregister_device()` → `ar0234_mipi.remove` → `cci_dev_remove_helper()` → 对没建成的 device 做 `device_unregister()` → NULL 解引用（`pc : device_del+0x48`，完整栈见 `analysis/t14/serial-recover.log`）。厂商把 sensor1/2/3 全部 `disabled` 就是这个原因。**顺序：先插模组，再开节点。**
27. **"打开第二个 video 节点又关掉（不出流）"会把第一路从 120 fps 拖到 ~18 fps**（第四轮 E1–E5 对照：与 3A 服务无关、与 S_FMT 成功与否无关。`video0 单跑 119.96 fps / 0 超时`，中途 open+close 一次 `video4` 后 `17.9–18.1 fps / 26 次超时`）。伴生 `Runtime PM usage count underflow`（`5831000.vinc`）与 `ar0234_mipi is not used, video0 cannot be close!` ⇒ 关节点路径在"传感器未真正绑定"时提前 return、跳过下电/PM 归还。**未修**；正常双路（都出流）不受影响。
    第五轮更新：**✅ 2026-09-16 第五轮：已修复 = `patches/0009`**（复现 → 定位 → 修复 → 验证全做完，见 `analysis/t17/REPORT-0916-isp-capacity.md` §3.1）。复现用 `analysis/scripts/t17-openclose.c`（**只有 open()+close()，不发任何 ioctl**）+ `analysis/scripts/t17-repro-close.sh`：0008 下 E1 单路 120.00 fps / 0 超时，E2 中途 open+close 一次 video4 → **29.59 fps / 23 次超时**、`vi0` 帧计数冻在 908、`sunxi-vin-core 5831000.vinc: Runtime PM usage count underflow!` ×1、`sensor_read error! sensor is not used!` ×3。定位到 `vin_video.c` `vin_close()` 的提前 return 跳过了 `vin_pipeline_call(vinc, close, ...)` →`__vin_pipeline_close()` → `vin_video_core_s_power(CAPTURE, 0)` = `pm_runtime_put_sync(&vinc->pdev->dev)`。0009 改成 `goto shared_teardown`，并且对"从没 S_INPUT 过、管线没 prepare"的节点跳过 pipeline close（否则必踩 `vin.c:1287` 的 `WARN_ON`，开机时 udev 的 `v4l_id` 每次都踩）。验证：**E1 120.03 fps、E2 119.04 fps，都是 0 超时、0 underflow、0 新 WARN**（全机 WARNING 只剩既有的 sysfs_emit/orangepi-hardware 那一条）。`apply.sh` 已接 0009。
28. **同一个 sensor 的两个 video 节点只能请求同一尺寸**：`vin_pipeline_try_format()` 把整条链（含 scaler 的 sink/source）按同一尺寸配，表达不了"1920x1200 输入 → 640x400 输出"。0008 现在会在另一个节点出流时用 `-EBUSY` 明确拒绝改尺寸（不再静默改坏）；要小尺寸第二路得走 `VIDIOC_S_SELECTION`（未验证）。
22. **D3D/消隐互锁（0006）出事后救不回来**：`FRAME_LOST` 中断里的兜底只清 D3D_EN，不丢弃已带 D3D/LBC 状态的 load image，所以互锁关闭（`d3d_min_vblank_us=0`）时整条流仍然是 0 帧。**开流前必须已经决定**。调试时可以 `echo 0 > /sys/module/vin_v4l2/parameters/d3d_min_vblank_us` 复现旧行为、`echo 500` 恢复，不需要重启。

---

## 3.28b 第六轮实测订正（三条被高估 / 被低估的坑）

> 详细证据见 `analysis/REPORT-0916-hardware-audit.md` §1.10 与 §3。

1. **§3.20 的 1080p 上限写错了**：请求 130/132/133/134/135 全部干净（实测 131.6–131.8 fps），
   **136 才崩**（3 s 内 201 次 `frame lost` + 201 次 `sunxi_isp_reset`）。所以「132 好 / 133 坏」不成立。
   注：驱动把 ≥134 的请求都钳到 `fll = 1096 (133 fps)`，而 134/135 干净、136 崩 —— 机制未定位。
2. **「ISP 出错之后必须重启」过宽**：1080p@136 打出 400 次 `frame lost` + 400 次 `sunxi_isp_reset`
   之后，**同一个 boot 里**下一次 1200p120 立刻 `119.78 fps / 0 超时 / 0 丢帧`。
   真正需要重启的只有「只重载了传感器模块」那一类（`no link to sunxi_mipi.0`，link 只在 probe 时建）。
3. **T6「十几次里 1 次」被严重低估 = 实测 10%**：40 次 1200p120@4 s 压测里 **4 次坏启动**，
   签名一致（`frames=4 … fps=0.99 timeouts=4` + `vi0 frame cnt` 复位 + dmesg **0 条** `frame lost`），
   而且**紧接着的下一次一定正常、不需要重启**。⇒ 上层必须做「开流后无帧就重开」的重试。
4. **T16b（§3.27）不是「已修复」而是「偶发、概率下降」**：s11 的 12/12（含 8 次 `open+close` 第二节点）
   全干净，但另有两次独立发作（一次第一路冻结；一次 video0 连续 9 个 20 s 流完全没帧，几分钟后自愈）。
5. **`Runtime PM usage count underflow` 未根除**：本 boot 又出现 1 次，并伴生一条新的内核 WARNING
   `_regulator_disable+0xf0` + `vin_pin_disable: disable vind_mclkpin error, fail to disable regulator!`。

## 3.28c 第七轮（修复轮）订正与新增 —— 2026-09-16

> 详细证据（命令、退出码、原始输出）见 `analysis/fix-round/REPORT.md`。

1. **ISP 参数文件的偏移有两套，必须分清**：`tools/make_isp_bin.py`、`analysis/libisp-offsets/offs.txt`
   里写的**全都是结构体偏移**。**文件偏移 = 结构体偏移 + 74**
   （74 = 4 B 长度 + 20 B 日期 + 50 B note）。
   双向验证过：`tdf` 结构体 101 ↔ 文件 175；结构体 88..120（模块开关）↔ 文件 162..194。
   `make_isp_bin.py` 的 docstring 和 `hwapi/README.md` 已补上这条。
2. **H.265 在 1920×1200@120 不可用**：只出 6 帧 + `isp0 configuration error` / `height error`。
   1200p60、1200p30、1080p30、**720p120** 都正常。**不要按「1200p120 + H.265」这个组合搭系统**；
   1200p120 请用 H.264（实测 103 fps，编码器本身是瓶颈，不是相机）。
   （注：任务书里写的「1200×1200@120」有笔误，本轮实际复验的是 **1920×1200@120**，即传感器原生窗口。）
3. **G2D 的位精确性取决于走哪条路径**（第八轮机制确认，完整证据 `analysis/g2d/REPORT.md`）：
   - 单次 `G2D_CMD_BITBLT_H` + 4:2:0 UVC 格式（NV12→NV12）**永远不可能位精确**，而且**不是缓存/同步问题**
     （第六、七轮的"逐次漂移/≤2 LSB/疑似 begin_cpu_access 没实现"这个解释是错的）。真实机制：Y 平面被硬钳到 ≥16，
     色度平面进 mixer 后被 4:2:0 重采样滤波器（冲激响应跨 -2..+4 字节、带负瓣）处理过。
     真实 ISP 帧上：色度恰好 0 差异（真实色度足够平滑），**Y 有一大片像素被抬到 16** ⇒ 暗部被削。
     **⚠️ 不要引用任何「差异字节数」当指标**：三份报告里的数字（第六轮 213 834、第七轮 136 451、第八轮 514 456 …）**互相矛盾、不是常数**
     （同一组合两次跑就变），只有质化结论稳定。见 §3.30-1。
   - **要位精确就用两次 `G2D_FORMAT_Y8` blit**：把 NV12 缓冲看成 `w×1.5h` 的 8 位单平面图，
     `clip_rect=(0,0,1920,1080)` 拷 Y、`clip_rect=(0,1080,1920,540)` 拷 UV。
     全范围随机数据、真实 ISP 帧都**位精确（0 差异）**，且 Y8 路径**不钳位、不滤波、不越界**。← 推荐搬运方式
     （已封装成 `ar0234::G2d::move_nv12()`，`userspace/include/ar0234/g2d.hpp` + `src/g2d.cpp`）
   - ARGB8888：`alpha=0xFF` 时逐通道位精确；随机 alpha 会触发预乘（alpha=0 的像素 RGB 被清零）。
     `G2D_BLT_COPYPEN` 返回 -1；`bbuff` 必须 1。
   - **格式常量**：NV12 = `G2D_FORMAT_YUV420UVC_V1U1V0U0`（**0x28**）；名字读起来像"U 在前"的
     `G2D_FORMAT_YUV420UVC_U1V1U0V0`（0x29）**其实是 NV21**，用在 NV12 上会让所有 ARGB 转换红蓝对调
     （本条是**我们自己的用法错误**，与色度滤波无关）。
   - 画框用 `G2D_CMD_FILLRECT_H`，`dst_image_h.color = (Y<<16)|(U<<8)|V` 原样写入（这条路径没有 RGB→YUV 矩阵），
     裁剪正确（矩形内 16384/16384 改动、矩形外 0 改动）。
4. **G2D 现在开机就有**：第七轮加了 `/etc/modules-load.d/g2d.conf` + udev
   `KERNEL=="g2d", GROUP="video", MODE="0660"`，冷启动实测 `/dev/g2d` = `crw-rw---- root video`，
   `orangepi` 用户（video 组）**不需要 sudo** 就能跑 `tools/hwtest/g2d_test.cpp`。
   （注意主设备号是动态的：两次启动见过 510 和 237，所以规则必须匹配**设备名** `g2d`，不能匹配主次号。）
5. **NPU 可用**：`/usr/lib/libNBGlinker.so` 导出了整套 `vip_*` API，`/opt/vpm_run` 真实推理 2747 µs，
   `/opt/yolov5` demo 认出 dog 82%。文档里"缺 `libVIPlite.so` ⇒ 用不了"是**过期结论**。
   剩下的限制只有「自己的模型仍需厂商 NBG 转换工具」和「ISP→NPU 零拷贝未验证」（缺第二项时不造数据）。
6. **1080p 的边界是 136，不是 132/133**（§3.20 已订正，§3.28b-1 有原始数字）。
7. **模块版本基线**：**第九轮板上实测 = 补丁 `0009` + `0010` + `0011` + `0012`**，
   `srcversion BA48201D23BA4260923B53E`（**注意是 `…0923B53E`，不是 `…0993B53E`**）、
   md5 `375f6f4f5c15f72b2d2323f32e7af087`（= `build/vin-d3d-lbc/out/vin_v4l2-0012.ko`、`prebuilt/vin_v4l2.ko`）。
   版本史：0009+0010+0011 = `7922F65188E60D338D42B73` / md5 `241c8631…`；
   0009 = `843BC1A03606D35EC062D57` / md5 `2715c25c…`。**每次都要现场核对**：
   `cat /sys/module/vin_v4l2/srcversion` + `md5sum /lib/modules/*/updates/vin_v4l2.ko`（§3.14）。
8. **LC21（LBC 输出）现在是显式失败**：`S_FMT` 直接 `EINVAL` + 一条说明性 `vin_err`
   （原来静默 0 帧）。`mpp/vi` 的 `CSI Bandwidth` 也修好了，出流时不再恒 0（1200p120 实测 411 MB/s）。
9. **「只 open 不出流就 close」会刷 3 条 Error/Warning**（`Runtime PM usage count underflow` +
   `_regulator_disable` WARNING + `vin_pin_disable ... error`）：**已由 `patches/0012` 修掉**
   （第九轮验收：触发序列 A/B 从 **30 行 → 0 行**，合法回归 1200p120 `119.88–119.91 fps / 0 超时` ×5，
   且 `regulator_summary` 的 `5800800.vind-vind_mclkpin` enable count 在 5 轮「出流+关流」后**回到 0**，
   证明关流路径没被误跳过）。根因是 `vin_pin_disable()` / `vin_video_core_s_power()` 没有引用计数
   （`vin_md_set_power()` 有）。见 `analysis/round9/REPORT.md` T2。
10. **开流必须带看门狗**：**实测 5~10%** 的 `STREAMON` 会「出几帧后永久停住」且内核零报错
    （第六轮 4/40 = 10%、第七轮关掉看门狗的基线 3/40 = 7.5%、第九轮 30 轮里 1 次 = 3.3%；合计 70 次 4 次 = 5.7%；
    「4 帧」不是硬数字，实测见过 4 帧与 6 帧，量级 = 一个 buffer 队列的长度）。
    第七轮把「STREAMON 后等不到帧就 close+open 重开（最多 2 次）」做进了 `userspace/` 的 `Capture`，
    40 次 × 4 s 压测 **坏启动残留 0**。注意：坏流上 `STREAMOFF`+`STREAMON` 会返回 `EINVAL`，
    **只有重新 open 才救得回来**（坏状态在 fd/管线绑定里，不在「流」里）。
11. **`DMA_BUF_IOCTL_SYNC` 是本内核自己的旧编码，不是 mainline 那套**：见 §3.30-2。用错编码 + 忽略返回值 = sync 是空操作。
12. **CMA 只有 16 MB**（`CONFIG_CMA_SIZE_MBYTES=16`，不是 bootargs/DT）：1920×1200 NV12 单帧 3.46 MB，
    约 4 个缓冲就到顶；多消费者必须走 DMA-BUF 扇出。见 §3.30-3。

## 3.29 「只 open 不出流就 close」的三条报错（第七轮定位）

- 触发：任何「打开 capture 节点但不做 S_FMT/S_INPUT 就关闭」的操作 —— `v4l2-ctl --get-fmt-video`、
  `v4l2-ctl --all`、udev 的 `v4l_id`、桌面环境的设备探测，全都算。**在任何人出过流之后**运行就会命中
  （首次 open/close 时 `cap->pipe.sd[VIN_IND_SENSOR]` 还是 NULL，0009 的 `skip_pipeline_close` 生效，所以冷启动
  那一下反而不报）。
- 现象：一轮三条 —— `Runtime PM usage count underflow!` / `WARNING ... _regulator_disable` /
  `[ERR] vin_pin_disable: disable vind_mclkpin error`。
- 定量：序列 A（`close(open("/dev/video0"))` ×5）和序列 B（`v4l2-ctl --get-fmt-video` ×5）**每次都复现**；
  序列 C（只 S_FMT 不流）和序列 D（出流中开关第二节点）**都是 0 条**，且序列 D 里 video0 仍 119.96 fps / 0 超时。
- 根因与最小修复方案：见 `analysis/fix-round/REPORT.md` P1-6。**第九轮已实施 = `patches/0012`**
  （`vin.c: __vin_pipeline_close()` 把三个 open 侧操作统一到 `vind->use_count` 之下），板上验收通过（§3.28c-9）。

## 3.30 第九轮订正与新增 —— 2026-09-17

> 详细证据（命令、退出码、原始输出）见 `analysis/round9/REPORT.md`（T1/T1-C、T2、T3、T4、T5）。

1. **G2D 的「差异字节数」不是常数，别再当指标引用**。三份报告里同一组合的数字互相矛盾
   （第六轮 213 834/3 110 400、第七轮 136 451/3 110 400、第八轮 514 456/2 073 600 …），**两次跑就会变**。
   稳定可引用的只有**质化结论**：单次 4:2:0 `YUV420UVC` blit **不位精确**（Y 硬钳 ≥16 + 色度重采样滤波）；
   两次 `G2D_FORMAT_Y8` blit **位精确（0 差异）**；`ARGB8888` alpha=0xFF **逐通道位精确**；
   **Y 只在 `<16` 被钳**；格式常量 **NV12 = `0x28`、NV21 = `0x29`**（第六/七轮用错了 0x29）。
   机制与安全/危险操作清单见 `analysis/g2d/REPORT.md` §2.9 / §3。
2. **`DMA_BUF_IOCTL_SYNC` 用的是本内核自己的旧编码，不是 mainline**（第九轮新发现，**仓库里的旧工具还没改**）：
   本内核 `include/uapi/linux/dma-buf.h`：
   `DMA_BUF_SYNC_READ = 1<<0`、`WRITE = 2<<0`、`RW = 3`、`START = 0<<2`、`END = 1<<2`、`VALID_FLAGS_MASK = RW|END = 7`；
   而 mainline 6.12+ 是 `READ 1<<2 / WRITE 2<<2 / RW 12 / END 1<<0`、mask 13。
   板上实测（`DMA_BUF_IOCTL_SYNC = 0x40086200`）：`flags=0 EINVAL`、`1/2/3 OK`、`4 EINVAL`、`5/6/7 OK`、
   `12/13 EINVAL` ⇒ **旧编码，而且方向位是必需的**（0 与「只有 END」都被拒）。
   ⇒ `tools/hwtest/g2d_test.cpp:109-113` 写的是 mainline 常量（`START|RW = 12`），**每次都被 `EINVAL` 拒绝**，
   而 `sync_()` **忽略返回值** ⇒ 所谓「修正后的 sync 次序」**从头到尾是空操作**。`tools/hwtest/g2d-probes/*.c` 同理。
   **新封装 `ar0234::DmaBuffer`（`userspace/src/g2d.cpp`）已经是对的** —— 它不硬编码任何一种编码，
   第一次 sync 时两种都试一次、记住内核接受的那个（两种编码互相排斥，不会误判）。旧工具的修法见 `docs/NEXT-TASKS.md` C13。
3. **CMA 只有 16 MB**，来源是**内核编译配置** `CONFIG_CMA_SIZE_MBYTES=16`（`kernel/orange-pi-6.6-sun60iw2/.config:7735`）；
   `/proc/cmdline` 里**没有** `cma=`，DTB 里也没有 CMA reserved-memory 节点（只有 `bl31`）。
   板上实测 `CmaTotal: 16384 kB` / 空载 `CmaFree: 14080 kB`。
   1920×1200 NV12 单帧 = 1 920 × 1 200 × 1.5 = **3 456 000 B ≈ 3.46 MB** ⇒ CMA 里大约只能放 **4 个** 1200p 帧；
   而 VIN 每路自己就要 `bkuf cnt: 4`（4 × 3.457 MB ≈ 13.8 MB）。
   **想调大**：`kernel/dma/contiguous.c:228` 是 `if (size_cmdline != -1) selected_size = size_cmdline; else 用 CONFIG_CMA_SIZE_MBYTES`
   ⇒ **bootargs 加 `cma=<size>` 就能覆盖，不用重编内核**；或重编改配置。
   **多消费者（NPU + OpenCV + 录像）必须走 DMA-BUF 扇出**（同一个 V4L2 EXPBUF fd 直接给各消费者），不要各拷一份。
   详见 `docs/NEXT-TASKS.md` C11。
4. **`RST_BUS_VIDEO_IN` 已确认可控 ISP（确认级）**：出流中只把 CCU 的 `0x2003884`（`{0x1884, BIT(16)}`，reset id **116**）
   bit16 清 0 → `vi0` 帧计数 **2 s 只走 3 帧**、ISP 寄存器块 `0x5900000` 从 `0x5` 变 `0`；置回 → 4 s 内 **485 帧 ≈120 fps 自动恢复，
   不需要停流重开**；同窗口 `0x2003844`（CSI）位没被动过。极性：**bit=1 = 解除复位、bit=0 = 处于复位**（与 reset 框架语义相反）。
   ⇒ 「接上 ISP 硬复位」这一项从「候选」升级为「可做」，但**必须与 `patches/0012` 一起上**（否则关流路径会把 ISP 真按进复位），
   而且**「能不能救回 `frame_lost` 风暴」还没测**。精确 DT diff / 风险表 / 回退见 `analysis/round9/REPORT.md` T1。
   **⚠️ 地址订正**：之前 T29 写的 `0x2001884` **是错的**（按 CCU base=0x2000000 推的，偏了 0x2000）。
   CCU 节点是 `soc@3000000/ccu@2002000`（reg `<0 0x2002000 0 0x2000>`），所以正确地址是 **`0x2003884`**（CSI 是 `0x2003844`）。
5. **H.265 @1920×1200@120 的「直接失败」在第九轮 6/6 次精确重复里全部通过**（117.67–118.66 fps，7.0 ms/帧，
   码率约 639 KB/240 帧；同参数 H.264 是 113.6 fps / 962 KB）⇒ **未复现**。那次 6 帧失败的成因是
   **开流坏启动（5~10%）**，而且**「ISP `configuration error`/`height error` 打死 H.265」这个说法被证伪**
   （有一次运行里 ISP 打了 2 条错、照样 240/240 帧 117.67 fps）。**「高度对齐 64」也证伪**：
   1216/1280 在 capture 节点上会被静默改回 1200，而 1080（同样非 64 倍数）一直正常。
   ⇒ **`README.md` 里那条「1200p120 请用 H.264」的警告应当撤回/改写**（本批只订正 HANDOFF，README 留给下一批一并做）。
6. **`isp01` 的候选机制有新的只读旁证**：出流中 `0x5900000 = 0x5`（ISP 寄存器），而 `0x58ffffc = 0`、`0x58ffff8 = 0`、`0x5901300 = 0`
   ⇒ 与「四个 isp 节点是同一窗口的别名、`of_iomap()` 取区间起点 ⇒ isp01 每次访问低 4 字节」的假设一致。
   **仍然绝对不要打开它**（第九轮按约束没有 enable）。
7. **第二节点 open/close 拖死第一路（§3.27/T16b）：第九轮 30 轮压测 0 次**（30×[20 s 流 + 第 6 秒起开关 `/dev/video4` ×3]：
   该故障 0/30、六类内核消息 0/30、29 轮 117.97–120.02 fps；第 16 轮那次是**坏启动**，`frames=4` + 下一轮立刻 119.99 fps）。
   ⇒ 与 `patches/0012` **不同源**（一个是确定性引用计数、一个是概率性坏启动）。
   仍未构造到「video4 的管线曾被 prepare 过、但当前 `entity.use_count == 0`」那种状态（**推论，未实测**）。
8. **`patches/0009` 的定性维持「偶发、概率下降」**：第六轮 11 干净 / 2 发作，第九轮 30/30；不要写成「已彻底修复」。

## 3.31 剩余工作（下一批从这里领）

**见同目录 `docs/NEXT-TASKS.md`** —— 自包含清单，分五组：
**A**（1–5）需要硬件/先决条件在用户手上（第二颗模组、触发闪光灯、LSC/MSC 标定、黑白模组、厂商 NBG 工具）；
**B**（6–9）能修但要改 DT / 有风险（接 ISP 硬复位、抬 ISP 时钟、TDM 改 offline、第二路 vinc20）；
**C**（10–15）能修、不需要硬件、性价比最高（NPU←ISP 零拷贝、CMA 16 MB、userpatches 正式化、G2D sync 编码、`/etc/ar0234.conf`、三个低优先诊断）；
**D**（16）低优先（4 h 长稳、NPU 端到端、图像外传、触发实测）；
**E** 已排除的**死路**（7 条，别重走）。

## 4. 任务清单

> ### 第七轮（修复轮）完成情况 —— 2026-09-16
>
> | 任务 | 状态 | 证据 |
> |---|---|---|
> | **T27 / T7** G2D 开机加载 + 权限 | ✅ **已修** | 冷启动 `/dev/g2d` = `crw-rw---- root video`，orangepi 免 sudo 跑通 g2d_test；配置已进 `board/install.sh` + `packaging/build-deb.sh` |
> | **T6** 开流偶发「只出几帧就死」 | ✅ **已兜住**（内核侧未动，按约定） | `userspace/` 采集库开流看门狗；40 次×4 s 压测 **坏启动残留 0**，2/2 重试成功（关掉看门狗时 3/40 复现） |
> | **T4** 工业 ISP 参数集 | ✅ **已完成** | `isp/isp_param_industrial.bin`；曝光线性度 **r = 0.99989**（出厂集 0.95676） |
> | §2.3-11 **LC21 静默 0 帧** | ✅ **已修** | `patches/0010`：`S_FMT LC21` 现在 `EINVAL` + 说明性 `vin_err` |
> | §2.3-10 **CSI Bandwidth 恒 0（截断那处）** | ✅ **已修** | `patches/0011`：出流时 411 MB/s（原来 0）。②③ 未动，分析写在报告里 |
> | §2.3-9 **underflow + regulator WARN** | ✅ **已定位**（本批不改代码） | 触发序列可复现；根因 = `vin_pin_disable()`/`vin_video_core_s_power()` 缺引用计数；最小补丁已写进报告 |
> | **T31** 参数偏移 +74 文档 | ✅ **已订正** | HANDOFF §3.28c-1、`make_isp_bin.py` docstring、`hwapi/README.md`、`offs.txt` |
>
> **本批没做（有理由）**：ISP 硬复位（要先只读验证 CCU `RST_BUS_VIDEO_IN` 是否连着 ISP）、
> H.265@1200p120 根因、第二节点 open/close 偶发、`isp01`（禁区）、LSC/MSC 实拍标定（缺均匀光源）。
> 完整报告：`analysis/fix-round/REPORT.md`。


优先级：**P0 = 无阻塞、价值高，先做**；P1 = 无阻塞、次要；P2 = 被外部条件卡住（硬件、厂商、用户决定）。

### P0

#### T1 仓库整理（推送被 URL 卡住）
- 在 `ar0234-port` 里 `git init`（独立仓库），写 `.gitignore`：排除 `build/`、`analysis/` 下的 `*.h264 *.h265 *.raw *.yuv *.bgr *.png *.jpg`、`samples/` 里的视频、`userspace/build/`。
- 新建 `docs/BASELINE.md`，记录第 1 节的 BSP 基线版本和复现步骤。
- 决定 `tools/ar0234_rec.c`、`tools/ar0234_3a.c`（旧 C 版）是删掉还是移到 `legacy/`，**先问用户**。
- `hwapi/board-include`、`kernel-uapi`、`symbols`、`isp/template_gc05a2_a733.blob` 是全志的文件，只能放私有仓库。
- 验收：`git status` 干净，仓库不超过约 3MB；远端 URL 由用户提供后再推送（提交署名按系统提示）。

> **2026-09-15 完成**：`git init`（main）；`.gitignore` 对 `analysis/` 用白名单（只留文本/源码）+ 全局媒体扩展名；新增 `docs/BASELINE.md`、`docs/ARCHITECTURE.md`；旧 C 工具移入 `legacy/`（未删，要删随时可删）；`prebuilt/` 保留（468K，板上部署依赖它）；hwapi/ 与 isp/template blob 标注为厂商文件、推送需私有仓库。

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

#### T4 工业 ISP 参数集 `isp_param_industrial.bin` —— ✅ 第七轮已完成（2026-09-16）
> 见 `analysis/fix-round/REPORT.md` P0-3。产物 `isp/isp_param_industrial.bin`
> （md5 `f3fe7e50c89061f28c88e96ea504dbc7`）：`defog/lca/gca/sharp/pltm/drc/cem/cnr/gtm` 关，
> `blc/otf_dpc/ctc/wb/cfa/ccm` 留，gamma 换线性表（`make_isp_bin.py --gamma-table linear`）。
> 曝光扫描（固定增益，×1/64…×1/4 五档）：**Pearson r = 0.99989**（出厂参数集 0.95676）。
> 板上**没有**设成默认（线性 gamma 会让画面明显变暗变平），装/卸命令都在报告里。

原任务描述：
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

#### T7 G2D 开机加载、权限、C++ 封装 —— 开机加载 + 权限 ✅ 第七轮已完成
> `/etc/modules-load.d/g2d.conf` + udev 规则已进 `board/install.sh` 与 `packaging/build-deb.sh`；
> 冷启动 `/dev/g2d` = `crw-rw---- root video`，`orangepi` 免 sudo 跑通 `tools/hwtest/g2d_test.cpp`（rc=0）。
> 剩余（本批未做）：库里的 C++ G2D 封装（画框/裁 ROI）。

原任务描述：
- `/etc/modules-load.d` 里加 `g2d_sunxi`；udev 规则 `KERNEL=="g2d", GROUP="video", MODE="0660"`。
- 封装 `BITBLT_H`（裁剪、缩放、旋转）和 `FILLRECT_H`/`BLD_H`（画框、叠加），输入输出都用 DMA-BUF fd（`g2d_image_enh.fd`）。
- 验收：ISP 的 BGR 或 NV12 缓冲（EXPBUF）交给 G2D 画框、裁 ROI，零拷贝，1200p120 下 CPU 占用可忽略；输出和 CPU 实现逐像素对比一致。

### P1

| ID | 任务 | 要点和验收 |
|---|---|---|
| T8 | orangepi-build 正式集成 | 0001、0003 放 `userpatches/kernel/sun60iw2-current/`；配置放 `userpatches/linux-sun60iw2-current-a733.config`（包含 0002、0004 的改动）；`./build.sh BOARD=orangepizero3w BRANCH=current BUILD_OPT=kernel REVISION=1.0.1`。**装板前先准备回退方案**（备份 /boot，确认串口能救）。验收：新 deb 装好后不用 fdtput、不用 prebuilt 模块，相机就正常 |
| T9 | 驱动 ROI 裁剪（AOI） | 传感器窗口寄存器（0x3002–0x3008）+ sunxi-vin 动态窗口，或者用 V4L2 selection 接口。窗口越小帧率越高。验收：比如 1920x400 实测帧率和计算值一致，画面位置正确 |
| T9b | 双相机 | ✅ **2026-09-16 第四轮：路线与前置条件全部备好，只差模组**。**走 `vinc@5832000`（label `vinc20`, device_id 8）+ `sensor@5812020`（sensor 槽 2）**，一条命令 `tools/dt_second_cam.sh sensor2-on`；`ar0234_mipi.c` 多实例审计结论 = **无缺陷**（per-stream 状态都在 `struct ar0234`，probe/remove 按 `client->name` 查 `cci_drv[]`，无全局假设）；`PE6` pwdn 冲突**只影响厂商的 sensor1 槽，sensor2 用 PE10**，天然避开；⚠️ **必须先插模组再开节点**（否则 probe 回滚 panic，§3.26）；接线后按 `analysis/t14/CHECKLIST-second-camera.md` 走。未验证：`/dev/video8`、第二路出流、两路同跑（需模组）。第三轮结论：⚠️ **2026-09-16 第三轮：拓扑结论更正 + 实测到「打开 isp01 会让第一路静默 0 帧」**。① `sunxi_mipi.1`/`sunxi_csi.1`/`sunxi_tdm_rx.1` 实体与 DT `sensor@5812010/5812020` 本来就存在，只缺 `sunxi_isp.1` 和 `video1`；② 用 fdtput 打开 isp01+vinc01+`sensor1_mname=ar0234_mipi` 后`sunxi_isp.1`/`sunxi_h3a.1` 正常出现，**但第一路 1920x1200@120 变成 0 帧且内核无任何报错**；两次隔离重启定位到触发点就是 **`isp01 = okay`**（只开它、连 vinc01 都关掉也照样坏）；③ 已全部回退，第一路恢复（120 s/14410 帧/`lost_cnt 0`）。另：`sensor0_pwdn` 与 `sensor1_pwdn` 都是 `PE6` |
| T10 | MP4 封装和 JPEG 抓图 | 板上有 libavformat 58 运行库，要装 `libavformat-dev`；另做 `ar0234-snap`，用 `AWJpecEnc` 硬件 JPEG。验收：MP4 时间戳正确、能正常播放；JPEG 能正常打开 |
| T11 | 长时间稳定性 | 分两组各连续 4 小时：① 1200p110 BGR 采集（T2 的程序）；② 1200p110 NV12 + VE 录像（`ar0234-rec -n 0`）。video0 同一时间只能输出一种格式，所以不能同时跑。记录：记录传感器温度（`temperature_approx_degc`）、SoC 温度、丢帧、内存、dmesg。输出一份报告 |
| T12 | 实时性基线 | 装 `rt-tests`，带相机满负载跑 `cyclictest`；再试 `isolcpus` + `SCHED_FIFO` + 中断绑核。报告最坏延迟。RT 内核暂缓（见 `../rt-check/`） |
| T13 | GPU OpenCL | ✅ 2026-09-16 实测：PowerVR BXM-4-64 / OpenCL 3.0 可用；设备级 EXTENSIONS 查询是坏的（返回 4 字节二进制），改查 `CL_PLATFORM_EXTENSIONS`(0x0904)。**DMA-BUF 导入确认**：`cl_khr_external_memory_dma_buf`、`cl_arm_import_memory_dma_buf`、`cl_khr_external_memory`，另有 `cl_img_yuv_image`（NV12 直处理）、`cl_khr_fp16`、`cl_khr_integer_dot_product`。测试 `tools/hwtest/cltest2.c`。剩：装 opencl-c-headers 写去马赛克 kernel 测性能 |
| T14 | ISP 双路输出 | ✅ **2026-09-16 第四轮：打通（`patches/0008`）**。根因 = `vinc4_isp_tx_ch = <1>` 让 VIPP1 去取不存在的 "ISP 输出通道 1"（寄存器级证据 + 在位改写 `0x58008a4` 0→120fps 的单变量实验）。修复后实测：`video0` 单路 60 s / 7203 帧 / 120.04 fps / `lost_cnt 0`；`video4` 单路 20 s / 2360 帧 / 117.97 fps / `lost_cnt 0`；**两路同跑各约 118 fps、两边 `lost_cnt` 全 0**。详见 `analysis/REPORT-0916-0008-second-channel.md` §1。遗留：两路只能同尺寸（§3.28），小尺寸第二路要走 `VIDIOC_S_SELECTION`（未验证）。第三轮背景：⚠️ 上一轮结论（0005 生效但 `video4` 永远 0 帧）；`isp01/02/03` **不是**独立 ISP，而是同一块 ISP602 的虚拟实例（寄存器窗口互相覆盖，全部止于 0x5901300）|\n| T15 | dmesg 噪声 | ✅/⚠️ **2026-09-16 第四轮**：`scaler get_selection error` **已修**（`sunxi_scaler` 的 default 分支返回 sink rect，`vin_video.c` 降级为 warn；模块 0008 的开机 dmesg 里 0 次）；`videoN has already stream off` / `%s is not used, videoN cannot be close` **降为 `vin_warn`**；`Runtime PM usage count underflow` **已复现并定性**（见 §3.27：开+关第二个节点但不 stream 就触发，同时把第一路拖到 18 fps；`vin_video.c` close 提前 return 未回滚），**未修**，新增任务 T16b。第三轮定性记录（保留）：⚠️ ① `scaler get_selection error` = `sunxi_scaler.c` 对 CROP/CROP_BOUNDS 以外的 target 一律 `-EINVAL`；② `ar0234_mipi is not used, video0 cannot be close!` = 传感器未绑定时 close；③ `configuration error`/`width error` 刷屏 = 真实故障的伴生；④ `Runtime PM underflow` 上一轮未复现 |
| T16 | vin 重载后 ISP 坏 | ✅/⚠️ **2026-09-16 第三轮：拆成三件事，其中 panic 已修**。① `rmmod vin_v4l2` **在这块板上根本不可能**（`vin_v4l2` ↔ `vin_io` 循环 refcount，`rmmod` 报 `Module vin_v4l2 is in use`，即使无流无进程）；② 只重载传感器模块再调 `VIDIOC_S_INPUT` 会 **NULL deref panic**（`mutex_lock` ← `media_entity_setup_link` ← `__vin_sensor_setup_link+0xb8` ← `__vin_s_input+0x74`；`list_for_each_entry` 找不到时 link 指向链表头，`if (link == NULL)` 是死代码）→ **已修 = `patches/0007`**，复现/回归脚本 `analysis/scripts/t16b.sh`，串口现场 `analysis/t14/serial-0006.log`；修后同一序列变成干净的 `S_FMT: Invalid argument`，看门狗不再触发；③ `Get isp reset control fail` 是**开机就有的 warning**（不是重载才有），根因是厂商 dtsi `resets = <&ccu RST_BUS_CSI>, <>;` 第二个 phandle 是空占位 ⇒ 驱动 `clk_reset[VIN_ISP_RET] = NULL`；也正因为没有硬复位，重载后 ISP 救不回来，仍需重启（这一半未修） |
| T16b | 第二个 video 节点 open/close 拖慢第一路（§3.27） | ✅ **2026-09-16 第五轮：已修复并验证 = `patches/0009`**（`goto shared_teardown` + 未 prepare 的管线跳过 pipeline close）。0008 下 29.59 fps / 23 超时 / 1 次 underflow → 0009 下 **119.04 fps / 0 超时 / 0 underflow / 0 新 WARN**。复现工具 `analysis/scripts/t17-openclose.c`、`analysis/scripts/t17-repro-close.sh`；报告 `analysis/t17/REPORT-0916-isp-capacity.md` §3.1 |
| T16c | `isp01 = okay` 破坏第一路的机制（§3.19 更正） | 已复现（加驱动保护后仍然坏）。**2026-09-16 第五轮：候选机制已给出**——四个 isp 节点是同一个 0x1300 字节寄存器窗口的四个**别名**（结束地址都是 0x5901300，起始地址每个差 4 字节：`0x5900000/0x58ffffc/0x58ffff8/0x58ffff4`），而 `of_iomap()` 拿的是区间**起始**，所以 isp01 实例的每次寄存器访问都比 ISP0 的同一个字段**低 4 字节**，写成功但写进了别的寄存器 ⇒ 第一路静默 0 帧且内核无报错。定级 **候选**（未直接观测）。只读验证法：出流中同时读 `0x5900000` 与 `0x58ffffc` 比对，或统计 isp01 实例的 `bsp_isp_*` 调用。结论：**仍然绝对不要打开这个节点**。见报告 §3.2 |
| T16d | 第二路 DT 的"预验证"（无模组）不可能 | 已定性：打开 `sensor@5812020` 而没有模组 ⇒ probe 回滚 NULL deref panic（§3.26）。所以"预先把第二路 DT 摆好但不插模组"这条路**不存在**；准备态必须保持 `sensor@5812020 status = disabled`（`vinc20` 可以 `okay`，已验证无害且有明确 ERR）。可选下一步：修 `ar0234_mipi` 的 probe/remove 让它对"没绑上"的 client 干净失败（需要模组在场才能回归） |
| T17 | 单元测试和集成测试 | 给 `IspParamSets::needs_3dnr_off`、帧率取整、参数原子替换写单元测试；把各模式、各格式的回归测试写成脚本 |

### P2（被卡住）

| ID | 任务 | 卡在 |
|---|---|---|
| T18 | 外部触发、从机同步、闪光灯实测；vin 对不定时帧的处理；配合手动曝光 | 用户接线（TRIG 是 **1.8V** 电平，要电平转换）；40pin 的 PWM 引脚映射要查设备树和原理图（pwmchip0/10/20 各 10 路） |
| T19 | NPU：ISP 640x400 BGR → `vip_create_buffer_from_fd` → 推理（比如 YOLO） | **订正（第六/九轮）**：NPU **运行时已经可用**——`/usr/lib/libNBGlinker.so` 导出整套 `vip_*` API（含 `vip_create_buffer_from_fd`），`/opt/vpm_run` 实跑推理 2747 µs（`VIPLite 2.0.3.2-AW-2024-08-30`），`/opt/yolov5` 认出 dog 82%；**`libVIPlite.so` 不存在也不影响**（别按「缺它就用不了」判断）。剩下的真阻塞有两项：① **ISP→NPU 零拷贝从未验证**（下一步，见 `docs/NEXT-TASKS.md` C10）；② 换自己的模型仍需**厂商 NBG 转换工具，版本要配 VIPLite 2.0.3.2**（**不是**旧文档写的 1.13.0） |
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
7. 找香橙派或全志要：NPU SDK、ISP Tuning Tool、模组原理图（**第二颗模组的 PWDN/RESET/MCLK 引脚必须靠这张图确认**，见 HANDOFF §3.26 / 报告 §6.6 与 T23）
8. 两颗相机要不要**不同分辨率/格式**（决定是否需要做 scaler 的 `VIDIOC_S_SELECTION` ROI 缩放路径，见 §3.28）
9. ~~**请断电重启板子一次**（第四轮末尾它停在需要断电的状态，见 §1「板子安装状态」）~~
   —— **已作废**：第六轮起板子一直健康（第九轮实测 DTB 原样 `d4ee5b68…`、`/dev/video0`+`/dev/video4`+`/dev/g2d` 都在、
   1200p120 回归 ≥119 fps）。当前唯一硬阻塞是**第二颗模组还没插**（`sensor@5812010`/`5812020` 保持 `disabled` 是正确状态）。
10. **下一批的剩余工作清单见 `docs/NEXT-TASKS.md`**（分 A 需硬件 / B 改 DT 有风险 / C 纯软件 / D 低优先 / E 死路）。

---

## 6. 第六轮：底层硬件栈审计结论（2026-09-16）

- 完整报告：**`analysis/REPORT-0916-hardware-audit.md`**；原始输出：`analysis/hardware-audit/s1..s17.out`。
- 收工状态（第六轮）：DTB 原样 `d4ee5b68…`、模块 0009、ISP/CSI 324 MHz、`d3d_min_vblank_us=500`、
  `/dev/g2d` 不存在（= 当时的开机态；**第七轮起已改成开机自动加载**）、无残留进程、
  1200p120 回归 `fps=119.86 timeouts=0`。**未改内核树、未跑 apply.sh、未 git commit。**
- 收工状态（**第九轮 / 2026-09-17，本批文档同步前的板上实测**）：DTB 原样 `d4ee5b68…`、
  模块 **0012**（`srcversion BA48201D23BA4260923B53E`）、ISP/CSI 324 MHz、`d3d_min_vblank_us=500`、
  `/dev/video0` + `/dev/video4` + `/dev/g2d` 全部 `crw-rw---- root video`、`ar0234-3ad` active、
  `CmaTotal 16384 kB`、`sensor@5812010/5812020` 仍 `disabled`、`tdm work_mode = 0`、`isp@58ffffc` disabled。

**可用性总览**：单相机下 SoC 图像链路每一级都实测跑通 —— MIPI PHY（4 lane / 844 Mbps / RAW10）、
CSI300_500、TDM online、ISP602（唯一一块）、双 VIPP + 双 video 节点（同跑 119.02 / 119.04 fps）、
VE H.264、硬件 JPEG、G2D、PowerVR OpenCL 3.0、**NPU**。格式面：NV12/BGR3/RGB3/BA10/GREY/NV21/YU12/RGBP
全可用；YUYV/UYVY/RGB4 是 `S_FMT` 必失败。
**订正（第七/九轮）**：LC21(LBC) 已从「静默 0 帧」改成 **`S_FMT` 直接 `EINVAL` + 说明性 `vin_err`**（`patches/0010`）；
H.265@1200p120 的失败**第九轮未复现**（见 §3.30-5）。

**三条最重要的状态变更**

| 任务 | 原状态 | 第六轮 |
|---|---|---|
| T19（NPU） | ❌ 缺 `libVIPlite.so` | **✅ 可用**：`/usr/lib/libNBGlinker.so` 导出整套 `vip_*` API（含 `vip_create_buffer_from_fd`）；`/opt/vpm_run` 实跑推理（`VIPLite 2.0.3.2-AW-2024-08-30`，**2747 µs**）；`/opt/yolov5` demo 认出 dog 82%。剩：自己的模型仍需厂商 NBG 转换工具；**ISP→NPU 零拷贝未验证** |
| T6（偶发一帧不出） | 「十几次 1 次」未修 | **量化为 5~10%**（第六轮 4/40、第七轮 3/40、第九轮 1/30，合计 70 次 4 次 = 5.7%），且「下一次必好、无需重启」⇒ 对策变成用户态重试（第七轮已做进 `Capture`，40 次压测坏启动残留 0） |
| T16b（0009） | 第五轮「已修复」 | **降级为 ⚠️ 偶发**（11 干净 / 2 发作） |

**新增任务**

| ID | 任务 | 优先级 |
|---|---|---|
| T26 | 采集库加「开流后 N ms 无帧 ⇒ close+重新 open（≤2 次）」 | ✅ **已完成**（第七轮，`userspace/` 的 `Capture`；注意**不是** `STREAMOFF`+`STREAMON`，坏流上那样会 `EINVAL`） |
| T27 | G2D 开机加载 + 权限（T7 剩余部分） | ✅ **已完成**（第七轮） |
| T28 | `underflow` + `vind_mclkpin` regulator WARN 的引用计数定位与修复 | ✅ **已修复**（第九轮 `patches/0012`；遗留：`vin_pin_disable` 别的分支还没查，见 `docs/NEXT-TASKS.md` C15c） |
| T29 | `resets` 补上 `RST_BUS_VIDEO_IN`（让 ISP 能硬复位）—— **先只读验证 CCU 的 `RST_BUS_VIDEO_IN`（reset id 116，绝对地址 `0x2003884`）与 ISP 的关系再动 DT** | P1（中风险；第九轮已用「出流中只写这一位」的决定性实验**确认该位可逆驱动 ISP**，接线步骤与风险表见 `docs/NEXT-TASKS.md` B6）。**⚠️ 本行原来写的 `0x2001884` 是错的（偏 0x2000）** |
| T30 | H.265@1920×1200@120 的根因（试改高度到 1216/1280；再固定高度改 fps） | ✅ **已证伪/无需做**（第九轮 6/6 通过；「高度对齐」不成立，那次失败是开流坏启动；见 §3.30-5）。**新 P1 = 把 `README.md` 的 H.264 警告撤掉** |
| T31 | `tools/make_isp_bin.py` / `analysis/libisp-offsets/offs.txt` 里补注「偏移是**结构体**偏移，文件偏移 = 结构体偏移 + 74（4B 长度 + 20B 日期 + 50B note）」 | ✅ **已完成**（第七轮） |

**参数文件的实际开关（文件偏移 162..193 逐字节读出）**：BLC / DPC / CTC / WB / CFA / CCM / gamma /
DRC / PLTM / CNR / CEM / GCA / LCA / sharp / defog / dig_gain / NRP / denoise = **1**；
**LSC = 0、MSC = 0**（暗角未校正）；3DNR(tdf) 按流切换（1200p120 下驱动自动关）。
⇒ 当前参数集偏「美化」，**T4 的工业集（关 defog/LCA/GCA/sharp/PLTM/DRC + 线性 gamma）还没做**，
而它才是「定焦 + 固定曝光 + 要 RGB」这个目标的真正缺口。
