# NEXT-TASKS.md —— AR0234 × Orange Pi Zero 3W（A733）剩余工作交接清单

更新：2026-09-17（第十轮，**纯软件批次收官：C10–C15 全部完成**，证据见 `analysis/round10/REPORT.md`）。
本文件是给**下一个 agent** 用的**自包含**清单：每一条都写清 **现状 → 要做什么 → 验收判据 → 涉及文件/位置 → 阻塞点**。
所有事实都来自仓库里的报告；**与报告冲突时以报告为准**（报告里每一格都有命令、退出码、原始输出）：

## 0.0 第十轮完成情况（2026-09-17，先读这个）

| 项 | 结果 | 一句话 |
|---|---|---|
| C10 | ✅ | `userspace/apps/ar0234-npu-zerocopy.cpp`：真实 ISP 帧 → G2D(缩放+NV12→BGR888) → `vip_create_buffer_from_fd` → NPU；**300/300 帧 A/B 输出逐字节一致**，e2e 4.39 ms，CPU 0.71 ms/帧；`--fanout` 同帧喂 NPU+第二消费者也过。BGR888 内存序实测 = B,G,R |
| C11 | ✅⚠️ | **前提被实测推翻**：vinc/isp/tdm 全在 IOMMU 后面，**相机栈不占用全局 CMA**（1200p120 满载 CmaFree 与空载一字不差）；调大 CMA 对相机无收益。`tools/cma_resize.sh` 已交付并端到端验证（16→64→16 MB 往返）；DMA-BUF 扇出有 bufinfo 内核级证据（g2d+npu 同挂一个 system-heap 缓冲） |
| C12 | ✅ | `userpatches/kernel/sun60iw2-current/`（0001–0013，vin 相对路径已改写为内核根相对）+ `userpatches/linux-sun60iw2-current-a733.config`；**黄金验证**：pristine 树 13 补丁全干净应用 → 重建 DTB 与板上逐属性一致 → 重建模块上板 **120.06 fps / 0 超时**。手动构建必须 `LOCALVERSION=-sun60iw2`（坑） |
| C13 | ✅ | g2d_test.cpp + 11 个 g2d-probes 的 sync 改为运行时协商 + 失败必须出声（共享头 `tools/g2d-probes/dmabuf_sync.h`）；板上复测结论不变 |
| C14 | ✅ | `/etc/ar0234.conf` 落地。**路径 (a) v4l2 子设备控件 = 源码级死路**（`CONFIG_ISP_SERVER_MELIS` 未编入，控件写入无人消费）；**路径 (b) 参数文件补丁实测通过**：EXP_LINES/AGAIN/WB Gain/CT 逐帧恒定且等于配置值（单位假设 1/16 行、1/16× 被精确证实）；对流谁都生效；conffile 注册 |
| C15a | ✅ | isp01 破坏机制**只读解释闭环**（DT 别名几何：起点 -4 B / 终点同为 0x5901300 + `of_iomap(np,0)` 取起点 ⇒ 每次寄存器访问偏 -4 字节静默改写 ISP0 配置）。**维持禁区** |
| C15b | ✅ | **"136 硬边界"口径作废**。真实机制：3ad 把测得 fps 喂 `ispSetFpsRanage`（≥133）→ 该调用会把 isp_fps 写回驱动、**在流中改写 FLL** → vts 掉到 1100/1096、vblank<500 µs → 内核 0006 interlock 强制关 3DNR ↔ 3ad 旧规则装 3DNR-ON 参数集 → 冲突 → `sunxi_isp_reset` 风暴；概率性来自首两帧测量值 132 vs 133/134 的漂移。修复：`needs_3dnr_off` 对齐 interlock（按 vblank 估算）+ 传给 3A 的 fps 钳 120。修复后 133×5 / 136×5 全干净（120.0 fps）；**行为变化：3A 在位时请求 >120 稳定交付 120**；不带 3A 可 133.4 fps |
| C15c | ✅ | `patches/0013`（vin_pin_disable 只读失衡诊断）+ 40×4 s / 30× 压测：三签名 0、诊断误报 0、mclkpin enable count 回 0。prebuilt 已刷新为 0013 构建 |

第十一轮（2026-09-17 晚）收口：**N1 完成**（`userspace/apps/ar0234-cv-consumer.cpp`，板上 OpenCV 4.5.1，
capture→cv::Mat 3.64 ms）；**N3 关闭**（"流后 gain 读回 1.0x"不是复位 bug——libisp 手动模式根本不写
传感器增益，见 round11 报告 T2）；**N4 完成**（npu-zerocopy 进 deb）。N2 维持候选级挂账。
**固定模式重设计为直通架构**（mode=fixed 时不跑 libisp、子设备直写曝光/增益，实测钉住且画面正常）；
gamma 走整文件重生成路线（make_isp_bin.py），字节补丁全部退役。黑帧事件定性见 round11 报告 T1。
新挂账：**N7** 无 ctx 时 ISP 走上电默认配置的覆盖范围未系统验证（实测能出图）；**N8** 固定模式的
AWB 钉扎（需要重生成参数文件）未实现。

| 报告 | 内容 |
|---|---|
| `analysis/round9/REPORT.md` | 第九轮：ISP 硬复位只读+写实验（T1/T1-C）、PM 引用计数修复（T2，= `patches/0012`）、第二节点压测（T3）、H.265@1200p120 未复现（T4）、G2D 位精确封装（T5）、`DMA_BUF_IOCTL_SYNC` 旧编码（新发现） |
| `analysis/g2d/REPORT.md` | 第八轮：G2D 机制级结论（4:2:0 重采样滤波器 / Y 下钳 16 / 两次 Y8 位精确 / 0x28 vs 0x29） |
| `analysis/fix-round/REPORT.md` | 第七轮：G2D 开机加载与权限、开流看门狗、工业 ISP 参数集、LC21 显式失败、CSI Bandwidth、PM 引用计数定位、文档订正 |
| `analysis/hardware-audit/REPORT-0916-hardware-audit.md` | 第六轮：全硬件栈逐块实测矩阵（链路每级 / ISP 功能面 / 编解码 / G2D / NPU / 时钟电源 / 内存带宽 / 触发）+ §3「与文档不符」16 条 |
| `analysis/t17/REPORT-0916-isp-capacity.md` | 第五轮：单块 ISP602 容量实测与外推（162 MHz 必崩、两路 ≈600 MHz 外推、offline TDM 前提） |
| `analysis/t14/`（`CHECKLIST-second-camera.md`、`T14-findings.md`） | 第四轮：双路打通、第二颗模组的接线/前置条件清单 |
| `HANDOFF.md` | 主交接文档（状态表、坑清单、任务清单） |

---

## 0. 环境与入口

### 0.1 主机 TL101（所有仓库操作都在这里）

```sh
ssh -o BatchMode=yes helios@TL101 '<cmd>'          # 免密
# 仓库根（远端）：
/home/helios/Desktop/orangepi-build/ar0234-port
# 内核源码树（属主 root，里面有 GPU 编译残留 —— 不要直接改）：
/home/helios/Desktop/orangepi-build/kernel/orange-pi-6.6-sun60iw2
# orangepi-build 的 userpatches（T8 的落点，现在是空的，只有 atf/ 与 customize-image.sh）：
/home/helios/Desktop/orangepi-build/userpatches
```

- 复杂脚本**不要硬拼 ssh 引号**：本地写文件 → `base64 -w0` → 远端 `base64 -d` 落地再跑（本批的 DT 查询就是这么做的）。
- **`sudo bash apply.sh` 要先得到用户同意**（它会改内核树）。

### 0.2 板子（Orange Pi Zero 3W / A733 / sun60iw2 / 内核 6.6.98-sun60iw2）

```sh
cd ~/Desktop/orangepi-build/ar0234-port
tools/ssh_board.sh "<cmd>"          # 默认 172.16.0.193，orangepi 用户；BOARD=<ip> 可覆盖
tools/sc.py "<cmd>"                 # 走串口 /dev/ttyUSB0 115200（与 serial_log.py 互斥）
```

- **板上 sudo 密码是一个空格**：`printf ' \n' | sudo -S -p '' <cmd>`（`ssh_board.sh` 已封装）。
- **板上 `/tmp` 是 tmpfs**，重启即清空；长任务用 `sudo systemd-run --unit=xxx ...`，不要在 ssh 里用 `nohup`。
- 重启：`sudo systemd-run --on-active=2 /bin/systemctl reboot`；**换 vin 模块必须重启**（§3.16）。
- 用户态程序**在板上本地编译**（g++ 10.2，支持 C++20，没有 CMake）：
  `tar -czf - userspace | tools/ssh_board.sh "tar -xzf - -C ~/ar0234test"` 然后 `make -C ~/ar0234test/userspace -j8`。
  （交叉工具链 gcc 11.2 **太新**，板上 glibc 2.31 跑不了，除非静态链接。）
- **每轮开工/收工第一件事**：`dmesg` 环形缓冲只有约 1900 行，会被刷屏吃掉（§3.18）。

### 0.3 收工快照（本批核对过的板上实测）

```
uptime            27 min（干净重启后）
uname             6.6.98-sun60iw2 #1.0.0 SMP PREEMPT ... aarch64
DTB               d4ee5b6869e7b7cd39ce3762d0e078f9   /boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb
vin_v4l2          srcversion BA48201D23BA4260923B53E（= 补丁 0009+0010+0011+0012）
设备节点          /dev/video0、/dev/video4、/dev/g2d 全部 crw-rw---- root video
ISP / CSI 时钟     csi_isp = csi_top = 324000000（DTB 覆盖；源码树 dtsi 写 540/600）
CMA               CmaTotal = 16384 kB（16 MB）、CmaFree = 14080 kB（空载）
isp@58ffffc        disabled（isp01，**禁区**）；isp@5900000 = okay
sensor@5812000     okay（mname = ar0234_mipi，cci 11，addr 0x20，pwdn = PE6，mclk_id 1）
sensor@5812010     disabled（mname = ov13850_mipi，pwdn = PE6 ← 与第一颗冲突，别用）
sensor@5812020     disabled（mname = imx219_2，cci 9，addr 0x20，pwdn = PE10 ← 待接第二颗）
vinc@5832000       okay（device_id 8，csi_sel 1，mipi_sel 1，isp_sel 0 = mipi1/csi1 + 共用 isp0）
tdm@5908000        work_mode = 0（online）
vind@5800800       resets = <18 115>（只有 csi_ret；isp_ret 是空占位）、reset-names = csi_ret isp_ret
```

### 0.4 `tools/` 里现成的脚本（按用途）

| 用途 | 脚本 |
|---|---|
| 访问板子 / 送文件 | `ssh_board.sh`、`put_board.sh`、`sc.py`、`serial_log.py`、`serial_cmd.py`、`get_file.py`、`send_file.py` |
| 编 vin 模块（外部编译，约 9 s） | `build_vin.sh <tag>`（产物 `build/vin-d3d-lbc/out/vin_v4l2-<tag>.ko`） |
| 编/扫 ISP 时钟 | `build_ispclk.sh`、`t17-ispclk-scan.sh`、`t17-isp-div-scan.sh`、`t17-isp-iso-scan.sh`、`t17-ispclk/ispclk_scan.c` |
| DT / 第二颗模组 | `dt_second_cam.sh <sensor2-on\|sensor2-off\|isp01-on\|isp01-off>`（带 DTB 备份）、`dual_dtb.sh`、`uboot_fix_dtb.txt`、`uboot_repair.py`、`uboot_cmd.py`、`uboot_rescue.py`、`uboot_shell_rescue.py` |
| 出流 / 帧率 / 稳定性 | `vfr.c` + `vfrrun.sh`、`cap.c`、`stream_watchdog_test.cpp`（40×4 s 压测 + 看门狗覆盖率）、`t17-openclose.c`（只 open+close，不发 ioctl）、`t17-repro-close.sh`、`boardtest.sh`、`dualtest.sh` |
| ISP 参数 / 标定 | `make_isp_bin.py`（`--set 模块=0`、`--gamma-table linear`、`--lsc-json`；**注意文件偏移 = 结构体偏移 + 74**）、`calibrate_lsc.py`、`exp_linearity.cpp`（曝光线性度 Pearson r） |
| G2D | `g2d_test.cpp`（⚠️ 见 C13：它的 `DMA_BUF_SYNC_*` 是 mainline 编码且忽略返回值，**sync 是空操作**）、`g2d-probes/*.c`（冲激/机制探针，同样有这个问题） |
| 寄存器只读窥探 | `vinreg.c`（`/dev/mem` + mmap，root；**必须出流时读**，空闲时时钟被门控读全 0）、`dmcount.sh`、`dmfilter.sh` |
| VPU / NPU | `jpeg_test.cpp`、`dec_test.cpp`、`cltest2.c` |
| 串口救板 | `sysrq.py`、`sysrq2.py`、`sysrq3.py`（**这块板上 SysRq 实测不可用**，见 §3.25） |
| 其它 | `36_config.sh`、`ar0234_tuning.h`、`t14*.sh`（第四轮双路现场）、`t16b.sh`、`t17-*`（第五轮） |

**新封装（已交付、可直接复用）**：`userspace/include/ar0234/g2d.hpp` + `userspace/src/g2d.cpp`
（`G2d::move_nv12()` 两次 Y8 位精确搬运、`fill_rect_nv12()` 标注、`DmaBuffer` 会**运行时协商** dma-buf sync 编码）、
`userspace/include/ar0234/v4l2.hpp` + `src/v4l2.cpp`（`Capture` 自带到「开流看门狗 + 重试」）、
`userspace/apps/ar0234-g2d-selftest.cpp`（退出码 0/1/2/3 的自检）。

### 0.5 `HANDOFF.md` §3「必读的坑」——开工前先读这几条

- **§3.1 / §3.16**：出过流之后不要 `rmmod`/重加载 vin 模块；`rmmod vin_v4l2` 在这块板上**根本不可能**（与 `vin_io` 循环 refcount），**只有重启能换模块**。
- **§3.2**（已被两轮订正）：ISP 出错**不一定要重启**，按成因分 —— 行时序类（`frame_lost`/`sunxi_isp_reset` 风暴、开流坏启动）停流重开即自愈；**资源性原因（时钟被压低/供电）救不回来**，必须重启。
- **§3.18**：dmesg 环形缓冲约 1900 行，刷屏会吃掉开机证据；**每次重启后第一件事就是落盘 dmesg**。
- **§3.25**：不要用 `init=/bin/sh` 救砖（console 落到 HDMI、且没有 systemd 就没有 16 s 看门狗）；串口 SysRq 也不可用。
- **§3.26**：在 `sensor@5812010`/`5812020` 上设 `status = okay` 而**没有对应模组** ⇒ probe 阶段 panic + boot loop。**顺序永远是「先插模组，再开节点」。**
- **§3.19 / T16c**：`isp01`（`isp@58ffffc`）**绝对不要打开** —— 一 enable 就把第一路变成静默 0 帧（内核一条错都不报）。
- **§3.28c / §3.29**：参数文件偏移 +74；G2D 位精确性取决于路径；`LC21` 已改成显式 `EINVAL`；「只 open 不出流就 close」的三条 PM 报错**已由 `patches/0012` 修掉**；开流必须带看门狗。
- **§3.30**（本批新增）：`DMA_BUF_SYNC_*` 旧编码、CMA 只有 16 MB、`isp_ret` 地址订正、`RST_BUS_VIDEO_IN` 已确认可控 ISP。

### 0.6 工作纪律（本项目已经踩出来的约定）

1. **一次只动一个变量**；改 DT / 内核前必须有可回退路径（DTB 备份 + 串口 + 16 s 看门狗，必要时断电保底）。
2. **不造数据**：缺硬件/缺器材的项标 ⬜，不要用演示数据冒充实测。
3. **不碰禁区**：`isp01`、`rmmod vin_v4l2`、无模组开 sensor 槽位、`init=/bin/sh`。
4. **板子上的测试程序尽量放 `/tmp`（tmpfs）**，收工不留残留进程；内核 trace/kprobe 用完要清。
5. 报告要写**命令 + 退出码 + 原始输出**，并标**证据等级**（确认 / 复现 / 观察 / 候选 / 外推）。

### 0.7 已知的文档不一致（下一个 agent 别被绕进去）

| 位置 | 写法 | 实际 |
|---|---|---|
| `HANDOFF.md` §6 的 T29 | `0x2001884` | **错**（按 CCU base=0x2000000 推的，偏了 0x2000）。CCU 节点是 `soc@3000000/ccu@2002000`（reg `<0 0x2002000 0 0x2000>`），所以 `RST_BUS_VIDEO_IN` 的绝对地址是 **`0x2003884`**（`RST_BUS_CSI` 是 `0x2003844`）。用「CSI 位随出流/停流翻转」这个正对照锁死过。 |
| `HANDOFF.md` §1 硬件编码行 / §3.28c-2 | 「H.265 在 1920×1200@120 不可用」 | 第九轮 **6/6 次全速通过**（117.67–118.66 fps，7.0 ms/帧），**未复现**；详见 A/B 组之外的说明与 C 组边界。 |
| 三份报告里的 G2D「差异字节数」 | 213 834 / 136 451 / 514 456 … | **互相矛盾、不是常数**（同一组合两次跑就变）。只有质化结论稳定：单次 4:2:0 不位精确；两次 Y8 位精确；ARGB 不透明位精确；Y 只在 `<16` 被钳。**别再引用具体字节数当指标。** |

---

## 优先级总览

| 组 | 含义 | 条目 |
|---|---|---|
| **A** | 需要硬件，先决条件在**用户手上**（agent 做不了） | 1 2 3 4 5 |
| **B** | **能修，但要改 DT / 有风险**（建议下一批一起做，一次一个变量） | 6 7 8 9 |
| **C** | **能修，不需要硬件**（纯软件，性价比最高） | 10 11 12 13 14 15 |
| **D** | 队列里但优先级低 | 16 |
| **E** | 已排除的**死路**（别让下一个 agent 重走） | — |

**建议的开工顺序（如果只做一批）**：C10（NPU←ISP 零拷贝，价值最高且零风险）→ C13（G2D sync 空操作，半小时）→ C14（工业固定模式）→ C12（userpatches 正式化，防丢改动）→ C11（CMA）→ 然后 B 组。A 组的硬件到手后按 1→9→8→7→6 的顺序做（先双路，再 offline TDM，再抬时钟，最后接硬复位）。

---

## A. 需要硬件，先决条件在用户手上（agent 做不了）

### A1. 第二颗 AR0234 模组（MIPI-B）

**现状**
- 软件侧**已经全部就绪**（本批在板上逐项核对过 DTB）：
  - `vinc@5832000` **已经是 `status = okay`**，`device_id = 8`，`vinc8_csi_sel = 1`、`vinc8_mipi_sel = 1`、`vinc8_isp_tx_ch = 0`、`vinc8_isp_sel = 0` ⇒ 走 **mipi1/csi1 + 共用 isp0**（厂商预留的这一组就是给第二颗模组的）。
  - `csi@5821000`（csi1）与 `mipi@5810200`（mipi1）都是 `okay`。
  - `sensor@5812020`：`status = disabled`、`sensor2_pwdn = <&pio 4 10 0>` = **PE10**、`sensor2_twi_cci_id = 9`、`sensor2_twi_addr = 32`（0x20）、`sensor2_mclk_id = 2`、`sensor2_mname = "imx219_2"`（**待改成 `ar0234_mipi`**）。
  - 第一颗在 MIPI-A：`mipi0` / **cci 11** / addr 0x20 / pwdn **PE6** / mclk_id 1。两颗模组的 i2c 地址都是 0x20，所以**必须在不同总线上**（cci 9 vs cci 11）。
- `ar0234_mipi.c` 的多实例审计结论 = **无缺陷**（per-stream 状态都在 `struct ar0234` 里，probe/remove 按 `client->name` 查 `cci_drv[]`，无全局假设）。
- 接线与前置条件清单：`analysis/t14/CHECKLIST-second-camera.md`。

**要做什么**
1. 硬件：第二颗 AR0234 插 **MIPI-B**，4 条 data lane + 时钟、MCLK 24 MHz、PWDN 接 **PE10**、i2c 接 **cci 9**。
2. **先插模组，再改 DT**（顺序反了就是 probe 回滚 panic + boot loop，§3.26）：
   `tools/dt_second_cam.sh sensor2-on`（脚本自带 DTB 备份），并把 `sensor2_mname` 从 `imx219_2` 改成 `ar0234_mipi`。
3. `depmod` 不需要（模块没换）；直接重启，然后确认 `/dev/video8` 与第二路出流。

**验收判据**
- `/dev/video8` 存在，`media-ctl -p` 拓扑里 `vinc8 → isp0 → scaler.8 → video8` 链路正确。
- 单路 1920×1200 NV12 @120：≥119 fps、`timeouts=0`、`mpp/vi` 里 `lost_cnt 0`、dmesg 无 sensor2/isp 相关 `[ERR]`。
- 两路同跑：各 ≈118–120 fps、两边 `lost_cnt` 全 0、dmesg 干净（参考 `tools/dualtest.sh` 与第四轮 `analysis/REPORT-0916-0008-second-channel.md` 的方法）。
- **注意框架级限制**：同一颗 sensor 的两个 video 节点只能同尺寸（`vin_pipeline_try_format()` 把整链按同一尺寸配，§3.28）。不同 sensor 的两路能否不同尺寸**未验证**；要小尺寸第二路得走 `VIDIOC_S_SELECTION`（未验证）。改第二路尺寸前必须先停第一路。

**涉及文件/位置**
- DT：`vind@5800800/sensor@5812020` 的 `status` / `sensor2_mname`；`tools/dt_second_cam.sh`、`tools/dual_dtb.sh`、`tools/uboot_repair.py`。
- 驱动：`ar0234_mipi.c`（多实例已是干净的，不需要改）。
- 清单/报告：`analysis/t14/CHECKLIST-second-camera.md`、`analysis/t14/T14-findings.md`、`analysis/REPORT-0916-0008-second-channel.md`、`analysis/REPORT-0916-0006-0007-dual.md`。

**阻塞点**：第二颗模组 + MIPI-B 排线在用户手上（`HANDOFF.md` §5-7 已向用户提出）。
**坑**：① 不要用 `sensor@5812010`（sensor1 槽）——它的 pwdn 也是 **PE6**，与第一颗冲突；② **不要用 `isp01` 当第二块 ISP**（见 E）；③ 出问题时的恢复路径要靠串口 + 16 s 看门狗，必要时断电。

---

### A2. 外部触发 / 闪光灯

**现状**
- 驱动侧代码与参数**在位**：模块参数 `trigger_mode=0`、`flash_enable=N`、`flash_delay=0`；`.ko` 里有 `[%s]trigger mode %d, flash %d`、`parm=flash_enable:drive the FLASH pin high during exposure`、`parm=flash_delay:flash lead (<0) / lag (>0) in ~3.4us steps, -128..127`；video 节点暴露了 `vin_flash_ctrl`（menu, 0..2）与 `led_mode`（menu, 0..4）控件。
- **没有实测路径**：这两个控件目前不驱动任何硬件；`VI_IOCTL_SYNC_CTRL` 在驱动里存在，但 zero3w **没有引出同步引脚**。
- 电气条件：**TRIG 是 1.8 V 电平**，要电平转换；DT 里 `sensor0_pwdn = PE6`，`sensor0_reset` 是**空的**；可用 PWM `pwmchip0/10/20` 各 10 路；GPIO 控制器 `gpiochip0`(2000000.pinctrl) / `gpiochip352`(7025000.pinctrl)。

**要做什么**
1. 用户提供：40pin 上 PWM/TRIG/FLASH 的实际映射（原理图）+ **1.8 V ↔ 3.3 V 电平转换板** + **模组原理图**（TRIG/FLASH/PWDN/RESET 真脚位）。
2. 接线后：`modprobe ar0234_mipi trigger_mode=1 flash_enable=1 flash_delay=<0..127>`，再用 `v4l2-ctl` 操作 `vin_flash_ctrl` / `led_mode`。
3. 用示波器/逻辑分析仪量：TRIG 与帧起始的对齐、FLASH 脉宽 = 曝光时间 ± `flash_delay`（每步 ≈3.4 µs）。
4. 外部不定时帧：触发源频率变化时用 `tools/vfrrun.sh` 记录帧间隔是否跟随。

**验收判据**
- 外部触发下帧率/帧间隔与触发源一致（不是自由运行的 120 fps）；无触发时**不出帧**（不是自由运行）。
- 闪光脉宽与曝光时间的对应关系可测且可复现；`flash_delay` 正负方向正确。
- 长曝光（µs 级到 ms 级）与手动增益配合下，`vfr` 不报 `frame_lost`。
- **采集库必须关掉开流看门狗**（外触发/≤1 fps 场景）：`probe_timeout_ms = 0` 或 `probe_frames = 1`，否则探针会把正常慢流判成坏流（`userspace/include/ar0234/v4l2.hpp` 里已写清）。

**涉及文件/位置**：`ar0234_mipi.c`（触发/闪光灯代码）、`tools/vfr.c`+`vfrrun.sh`、`userspace/include/ar0234/v4l2.hpp`、audit §1.8、`hwapi/README.md` §1。

**阻塞点**：接线 + 电平转换板 + 模组原理图（三样都在用户手上）。

---

### A3. LSC/MSC 标定（暗角校正）

**现状**
- **工具链已就绪并各自验证过**：`tools/calibrate_lsc.py`（平场 RAW → 增益表，合成数据回路验证通过）+ `tools/make_isp_bin.py --lsc-json`（注入参数文件，回路验证通过）。
- **表偏移已逆向完成**（不需要厂商 Tuning Tool）：参数体 = 内核 `isp_tuning_priv.h` 布局 + bayer_gain 前插 2848 字节；**LSC 表 @结构体偏移 3130（= 文件偏移 3204）**，`u16[12][768]`（3 通道 × 256 点径向，Q10，1.0 = 1024）；触发色温 @结构体 21562；MSC @21574/21632。见 `analysis/libisp-offsets/offs.txt`。
- **板上参数文件里 `lsc = 0`、`msc = 0`（关着）** —— 暗角未校正。这是「定焦 + 固定曝光 + 要 RGB」这个目标里**最实打实的一个误差源**。

**要做什么**
1. 拍一张**均匀平场**（积分球最好；退一步用均匀光源 + 毛玻璃 / 积分板，注意别让镜头看到光源本体）。
2. `tools/calibrate_lsc.py` 出增益表 → `tools/make_isp_bin.py --lsc-json` 注入 → 回读文件字节确认写进去了。
3. 装机时**三处都要换**（否则 `ar0234-3ad` 开流时会把参数集换回去）：
   `/mnt/extsd/isp_param_config.bin`、`/mnt/extsd/ar0234/isp_param_3dnr.bin`、`/mnt/extsd/ar0234/isp_param_no3dnr.bin`；再 `rm -f /mnt/isp0_*_ar0234_mipi_ctx_saved.bin`（装/卸命令见 `analysis/fix-round/REPORT.md` P0-3）。
4. **开/关对比**同一场景的角部亮度。

**验收判据**
- 同一场景、同一曝光/增益下，开 LSC 后中心-角部亮度差显著变小（给出开关两组的具体数字）。
- 必须**实拍确认**两个假设：`lsc_center = (2048, 2048)`，以及通道顺序 **R/G/B**（这两个目前仍是假设）。
- MSC 同理给开关对比。

**涉及文件/位置**：`tools/calibrate_lsc.py`、`tools/make_isp_bin.py`、`analysis/libisp-offsets/offs.txt`、`isp/`、`analysis/fix-round/REPORT.md` P0-3（装机/回退命令）。**注意：所有偏移都是结构体偏移，文件偏移 = 结构体偏移 + 74。**

**阻塞点**：**缺均匀光源/积分球**（器材在用户手上）。
**副作用提醒**：`isp/isp_param_industrial.bin`（已交付，`r = 0.99989`）是把美化块关掉的工业集；LSC 标定应与它一起用，但**别把两者混在一批改动里**。

---

### A4. 黑白版模组（Y8/Y10 + ISP bypass）

**现状**
- 驱动只验证过**彩色** AR0234：开机 dmesg `[ar0234_mipi]V4L2_IDENT_SENSOR = 0xa56` + `find the onsemi AR0234`。黑白版芯片 ID 是 **0x1A56**，**未验证**。
- 已经验证过的两条可用路径：**BA10（RAW，绕 ISP）** `1920×1200@120 → 960 帧`（sizeimage 4608000 / bpl 3840，16 bit 小端）；**GREY**（等价于 NV12 的 Y 平面）`1920×1200@120 → 960 帧`、`640×400@120 → 118.86 fps`。硬件缩放直出小尺寸也可用（640×400 BGR @118.94 fps）。

**要做什么**
1. 拿到黑白模组后，在 `ar0234_mipi.c` 里加 ID 分支（`0x1A56`）与对应的格式集合（Y8/Y10）。
2. 用 BA10 或 GREY 走 bypass，确认 ISP 是否需要整体旁路（`sensor0_isp_used` / `sensor0_fmt` 这两个 DT 属性是开关点）。
3. 与彩色模组做同一场景对比。

**验收判据**
- 黑白模组能出流；Y8/Y10 数据无彩色伪影；RAW 直出（BA10）与 ISP bypass 的结果口径一致。
- 帧率/丢帧与彩色模组同级（1200p120 ≥119 fps / 0 超时）。

**涉及文件/位置**：`ar0234_mipi.c`、DT `sensor@5812000` 的 `sensor0_mname`/`sensor0_isp_used`/`sensor0_fmt`、audit §1.1/§1.2。

**阻塞点**：黑白版模组在用户手上。

---

### A5. 厂商 NBG 模型转换工具（用自己的 NPU 模型必需）

**现状**
- 板上**能跑** NPU：`/dev/vipcore`（rw-rw-rw）、时钟 1008 MHz、`viplite/vip_info` → `ver1=0x9000, ver2=0x9202`。
- 运行时**已经可用**：`/usr/lib/libNBGlinker.so` 导出整套 `vip_lite.h` API（40 个符号，含 `vip_create_buffer_from_fd`）；`/usr/include/vip_lite.h` 在位。`/opt/vpm_run` 实跑推理成功（`VIPLite driver software version 2.0.3.2-AW-2024-08-30`，2747 µs）；`/opt/yolov5` demo 认出 dog 82%。
- **缺的是模型转换工具**：`/usr/bin` 里没有 ONNX/TFLite → `.nb` 的转换器。板上现成可用的模型只有 `/opt/vpm_run/network_binary.nb`（964 KB，224×224 分类）和 `/opt/yolov5/model/yolov5.nb`（5.0 MB）。
- 口径提醒（旧文档的错）：**`libVIPlite.so` 确实全盘不存在，但这不等于用不了 NPU** —— `libNBGlinker.so` 就够了。
- 头文件里有 `vip_query_driver_version` 但库里**没有**这个符号（API 版本差异），用前按实际导出核对。

**要做什么**
- 向厂商/香橙派索取 **NBG 模型转换工具**（ACUITY / NBG），**版本必须配 VIPLite 2.0.3.2**（版本不匹配的 `.nb` 会被拒绝或跑出错误结果）。
- 拿到后：把自己训练的 ONNX 转 `.nb`，用 `vpm_run` 先冒烟，再用 `libNBGlinker` 自写程序跑。

**验收判据**
- 自己训练的模型经工具转出的 `.nb` 能在板上跑通；同一输入下板上输出与 PC 参考实现一致（分类 top-1 一致 / 检测框 IoU 达标）。
- 记录转换工具与 VIPLite 的版本对应关系，写进交接文档。

**涉及文件/位置**：`hwapi/README.md` §5、audit §1.5、`/opt/vpm_run/`、`/opt/yolov5/`。

**阻塞点**：厂商工具（不是软件能补的）。

---

## B. 能修，但要改 DT / 有风险（建议下一批一起做）

> **B 组的共同纪律**：一次只动一个变量；每个变量单独一轮验证；动手前备份 DTB（`d4ee5b68…`）+ 串口可用 + 16 s 看门狗在跑；准备好回退命令。
> **B 组的顺序有依赖**：B9（第二路）依赖 B8（offline TDM）与 B7（抬时钟）；B6（硬复位）**必须**在 `patches/0012` 之后（已完成）。

### B6. 接上 ISP 硬复位（`RST_BUS_VIDEO_IN`）

**现状**
- DT `vind@5800800`：`resets = <&ccu RST_BUS_CSI>, <>;`、`reset-names = "csi_ret","isp_ret"` —— 第二个 phandle 是**空的 `<>`**，编译进 DTB 后被丢弃（板上实测 `fdtget -t i … resets` 只有 `18 115`）。驱动 `devm_reset_control_get(dev,"isp_ret")` 失败 → `clk_reset[VIN_ISP_RET] = NULL` → `reset_control_deassert(NULL)` 按设计返回 0 ⇒ **所有 ISP reset 操作都是空操作**，开机固定一条 `Get isp reset control fail` warning。
- CCU 里存在一个**全树零引用**的 `RST_BUS_VIDEO_IN`：reset id **116**、寄存器 `{0x1884, BIT(16)}`、CCU 节点 `soc@3000000/ccu@2002000`（reg `<0 0x2002000 0 0x2000>`）⇒ **绝对地址 `0x2003884`**（`RST_BUS_CSI` = id 115、`{0x1844, BIT(16)}` = `0x2003844`）。
- **第九轮已做决定性实验（确认级，只写 CCU 一个位、写完立刻恢复原值）**：
  - 出流中把 `0x2003884` 写成 `0x00000000` → 读回跟着变、`vi0` 帧计数 **2 s 只走 3 帧**、`0x5900000`（ISP 寄存器块）从 `0x5` 变 `0`；
  - 写回 `0x00010000` → 4 s 内 **485 帧 ≈120 fps 自动恢复，不需要停流重开**；
  - 同一窗口里 `0x2003844`（CSI）保持在 streaming 状态不变 ⇒ 效果来自 `0x1884` 这一位本身；
  - ⇒ **该位确实驱动 ISP / video-in 块，而且可逆**。
- **极性（源码级）**：`ccu_reset.c` 的 `assert()` 是**清位**、`deassert()` 是**置位** ⇒ **bit = 1 表示「已解除复位」，bit = 0 表示「处于复位中」**（与 reset 框架语义相反，文件里有注释）。POR 默认 = 1（解除）。
- **一处必须保留的含糊**：读 `0x5900000` 得 0 不能单独区分「ISP 被复位」与「ISP 时钟被门控」；连通性的证据是**行为**（帧计数停走/恢复），不是那个 0。它在内部是「复位」还是「复位+门控」本实验分辨不了 —— 也不影响结论。
- **前置条件已完成**：`patches/0012`（引用计数修复，板上 `srcversion BA48201D23BA4260923B53E`）。**这一条是硬约束**：`vin_md_clk_disable()`（`vin.c:543-566`）在关流路径里会 `reset_control_assert(clk_reset[VIN_ISP_RET])` —— 现在这行是空操作，**接上之后就变成真把 ISP 按进复位**，而这条路径上存在过引用计数缺口（一个多出来的 close 就会走到这里）。两者叠加的最坏结果是：关第二个节点时把正在出流的第一路**硬件复位**（实验已经演示过这个现象）。

**要做什么**
1. DT 只改一行：
   ```diff
   -			resets = <&ccu RST_BUS_CSI>, <>;
   +			resets = <&ccu RST_BUS_CSI>, <&ccu RST_BUS_VIDEO_IN>;
   ```
   **先确认链上到底是哪份 dtsi**：内核源码树里 `arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi` 才是编进 DTB 的那份，`bsp/configs/linux-6.6/sun60iw2p1.dtsi` 是厂商参考副本（**这一步还没做**）。或者用风险最低的办法：**直接在板上用 `fdtput` 改 `resets` 的属性值**（与现有工具链口径一致），不动源码树。
2. 重编/换 boot 分区 DTB 一定会换掉 `d4ee5b68…` ⇒ 备份 DTB + 串口在位 + 16 s 看门狗 + 必要时断电。
3. 装上后先验证「复位线真的被驱动引用了」：开机 dmesg 不再有 `Get isp reset control fail`；`fdtget` 能读出两个 phandle。

**验收判据**
- 开机 warning 消失；`/dev/video0` 回归 1200p120 ≥119 fps / 0 超时。
- **救回能力**：故意打一次 `frame_lost` 风暴（例如 1080p 请求 136 fps）后，**在不停流**的情况下 assert → deassert 该复位线能否救回（这是接线的真正收益；本批只证明了「能复位 ISP」，**能不能救回风暴还没有测**）。
- **不能改过头**：合法关流路径仍要正常下电 —— 反复「出流 + 关流」之后 `regulator_summary` 的 `5800800.vind-vind_mclkpin` enable count 要回到 0（`patches/0012` 的验收方法，见 `analysis/round9/REPORT.md` T2）。
- 兄弟节点场景回归：video0 出流中 `open+close(/dev/video4)` ×N，video0 不掉帧（T3 的方法，30 轮）。

**涉及文件/位置**
- DT：`vind@5800800` 的 `resets` / `reset-names`；两份 dtsi；`tools/dt_second_cam.sh`（带备份的 fdtput 范式）。
- 只读验证工具：`tools/vinreg.c`（`/dev/mem` + mmap，root，**必须出流时读**）。
- 驱动：`vin.c`（`vin_md_clk_enable/disable`、`VIN_ISP_RET`）、`ccu-sun60iw2.c`、`ccu_reset.c`。
- 报告：`analysis/round9/REPORT.md` T1 + T1-C（含精确 DT diff、风险表、回退方式）。

**阻塞点**：无（风险中，需要一轮专门验证）。
**⚠️ 文档订正**：`HANDOFF.md` §6 的 T29 把地址写成 `0x2001884`，**是错的**（偏了 0x2000），正确是 `0x2003884`。

---

### B7. 抬 ISP 时钟（`vind@5800800` 的 `csi_isp` / `csi_top`）

**现状**
- 板上 DTB 实测：`csi_isp = 324000000`、`csi_top = 324000000`；**内核源码树的 dtsi 写的是 `csi_top = <600000000>`、`csi_isp = <540000000>`**（板级 DTS 把它覆盖成 324）。
- 单路 1200×1200@120（实际是 1920×1200）在 324 MHz 下**「刚好够」**：实测下界是 **162 MHz 必崩** —— 两种独立写法各测到一次：① 纯分频法下 `vi0 frame_cnt` 直接停在 0；② 框架 `clk_set_rate` 路径下刷 `isp0 hblank short, hblank need morn than 128 cycles!` + `sunxi_isp_reset` 风暴。而且**回到 324 也救不回来，只能重启**（这一条同时是 §3.2 里「资源性原因救不回来」的实例）。
- **两路需要 ≈600 MHz 是外推、不是实测**：由「单路实测需求落在 (0.586, 1.172] cyc/px」+「每行 128 周期」模型推出两路需要 (324, 648] MHz（置信度中）；下界区间是实测、`603 MHz` 这个具体数字是外推。
- ISP/CSI 共用父时钟 `pll-video0-4x`（324 MHz），**想单改 ISP 到中间值必须动父分频**（见 t17 §4），所以阶梯点不是随便取的。

**要做什么**
1. **只改属性值、一个 `status` 都不动**（这是 t17 给的「风险等级最低」的改法）。
2. 按 t17 §5 的清单做：先把 `csi_isp` 抬到 `540000000`，重启确认 `cat /sys/kernel/debug/clk/isp/clk_rate`；再用同一个办法扫 `csi_isp ∈ {405, 486, 540, 600}`，**每个点都在单路 1200p120 下测** fps / 丢帧 / `mpp/vi` 计数。
3. 如果只想验证「够不够两路」，至少要到 540–600 才有意义。

**验收判据**
- 每个阶梯点：`clk_rate` 与设定值一致；单路 1200p120 ≥119 fps / `timeouts=0` / `lost_cnt 0`；记录 `mpp/vi` 的 `CSI Bandwidth`（`patches/0011` 之后能看到真实字节/秒）。
- 两路（拿到模组后）：两路都 ≈118–120 fps、`lost_cnt` 全 0，dmesg 无 `hblank short`。
- **反向确认**：把值调回 324 应能复现「刚好够」的边界行为。
- 注意 `csi_isp` 只读（`/sys/kernel/debug/clk/isp/clk_rate` 是 `-r--r--r--`），**只能改 DT**。

**涉及文件/位置**：DT `vind@5800800` 的 `csi_isp`/`csi_top`；`tools/build_ispclk.sh`、`tools/t17-ispclk-scan.sh`、`tools/t17-isp-div-scan.sh`、`tools/t17-isp-iso-scan.sh`、`tools/t17-ispclk/ispclk_scan.c`；报告 `analysis/t17/REPORT-0916-isp-capacity.md` §2/§4/§5。

**阻塞点**：无（风险中，需要重编/替换 DTB）。
**副作用**：抬时钟会让「ISP 出错更少但功耗/热更高」；同时**可能改变 162 MHz 那条「救不回来」的边界**，别把两个变量混在一轮里测。

---

### B8. TDM 改 offline（`tdm@5908000 work_mode` 0 → 1）

**现状**
- 板上实测 `tdm@5908000 work_mode = 0`（**online**）；`mpp/vi` 两路都显示 `work_mode: online`。
- 源码级事实：online 模式下 ISP 的输入**直接来自 CSI/parser 的实时输出，没有 DDR 中转**；且 online 且不压缩时 `tdm_buf_num = 0` ⇒ **TDM rx 的 DDR 环形缓冲一块都不分配**，数据是实时穿过去的。
- online **明确拒绝第二路 rx**：`vin_err("tdm%d working online mode, rx%d working, tdm can not be open again!")`，并且 `if (tdm->work_mode == TDM_ONLINE && rx->id != 0)` 直接报错。
- ⇒ **多路进同一块 ISP 靠的就是 TDM offline 这一级做时分复用**，现在这个能力根本没启用。
- **术语澄清**（容易混淆）：`mpp/vi` 里 `vin_status_dump` 打的 `bkuf cnt: 4 size: 3457024 rest: 3` 是 **vinc 的 capture 缓冲队列**（每路 4 × 3.46 MB，两路共 ~27.6 MB 常驻），**与 TDM rx 缓冲不是一回事** —— 前者 online/offline 都有，后者只有 offline 才有。

**要做什么**
1. DT 只改一个属性值：`tdm@5908000` 的 `work_mode` `0` → `1`。
2. 重启后确认 `mpp/vi` 显示 offline、两个 tdm_rx 都能 open。
3. 观察每路拿到的 DDR 缓冲块数与 DDR 带宽/延迟。

**验收判据**
- offline 下第二路 rx 不再报 `tdm can not be open again!`；驱动日志显示两个 rx 各自拿到 2–6 块 DDR 缓冲。
- **单路回归不减**：单路 1200p120 仍 ≥119 fps / 0 超时（这是「改过头」的防呆）。
- 两路时留意 DDR 压力：`CSI Bandwidth`（0011 已修）能读真实值；必要时用 t17 的方法估算。
- 记录 offline 相对 online 的额外延迟（这是时分复用的代价）。

**涉及文件/位置**：DT `tdm@5908000 work_mode`；驱动 `bsp/drivers/vin/vin-tdm/*.c`（`TDM_ONLINE`/`TDM_OFFLINE` 分支）、`vin-video/vin_core.c:920`（`bkuf` 打印）；报告 `analysis/t17/REPORT-0916-isp-capacity.md` §2.4/§2.5。

**阻塞点**：无（风险中；与 B7 一起做时**一次只动一个**）。

---

### B9. 第二路（vinc20 + sensor2）—— 走厂商预留的 `vinc@5832000`

**现状**（与 A1 是同一件事的软件侧，本批在板上逐项核对）
- **板上 DTB 已经就绪**：`vinc@5832000` **已经是 `status = okay`**、`device_id = 8`、`vinc8_csi_sel = 1`、`vinc8_mipi_sel = 1`、`vinc8_isp_sel = 0`、`vinc8_isp_tx_ch = 0`、`vinc8_tdm_rx_sel = 0` ⇒ **mipi1/csi1 + 共用 isp0**。
- `csi@5821000`（csi1）、`mipi@5810200`（mipi1）都 `okay`；`sensor@5812020` `disabled`（pwdn **PE10**、cci 9、addr 0x20、mclk_id 2、mname 待改）。
- **「`vinc20` 是 `okay`」本身已被验证无害**（第四轮：开着它、不插模组也不坏），真正会 panic 的是**打开 sensor 槽位节点而没有模组**（§3.26）。
- **`isp01` 不是这条路**：`isp@58ffffc` 只是同一块 ISP602 的别名，走它会静默毁掉第一路（见 E）。

**要做什么**
1. 与 A1 完全同一批：`sensor2_mname` → `ar0234_mipi`、插模组后 `status = okay`、重启、确认 `/dev/video8`。
2. **依赖 B8**：两路要同时挂上 TDM，需要 `work_mode = 1`（online 会被驱动直接拒绝）。
3. 如果两路要跑满 1200p120，**需要 B7**（两路 ≈600 MHz 外推）。

**验收判据**
- `/dev/video8` 出流；两路同跑各 ≈118–120 fps、`lost_cnt` 全 0、dmesg 干净、`media-ctl -p` 拓扑正确。
- 尺寸约束：同一颗 sensor 的两个节点只能同尺寸（§3.28）；不同 sensor 的两路**未验证**，若需要不同尺寸得先验证 `VIDIOC_S_SELECTION` 路径。

**涉及文件/位置**：DT `vinc@5832000`（`vinc8_*` 属性组）、`sensor@5812020`；`tools/dt_second_cam.sh`、`tools/dual_dtb.sh`；`analysis/t14/CHECKLIST-second-camera.md`。

**阻塞点**：**模组**（A1）；软件侧零阻塞。

---

## C. 能修、不需要硬件（纯软件，性价比最高）

> C 组全是**用户态 / 配置 / 文档**，不碰 DT、不碰内核，风险最低，**建议先做这一组**。

### C10. 打通并验证「NPU ← ISP 零拷贝」★最高性价比

**现状**
- `libNBGlinker.so` **已导出** `vip_create_buffer_from_fd`（还有 `vip_create_buffer_from_handle`、`vip_map_buffer`、`vip_set_input`、`vip_set_output`、`vip_run_network` …共 40 个符号），`/usr/include/vip_lite.h` 在位；NPU 内核驱动 `/dev/vipcore` 正常；`vpm_run` 用文件输入跑通（2747 µs），`/opt/yolov5` demo 认出 dog 82%。
- **但从来没有用真实 ISP 帧喂过 NPU** —— 现有两个 demo 的输入都是**文件**。所以「NPU 可用」目前只到「能用自带的 `.nb` 和文件输入」这一步。
- 这是「**一帧同时给 NPU + OpenCV**」方案的关键：ISP 出的 NV12（或 BGR）dma-buf 直接喂 NPU，CPU 不参与搬运。
- 现成的可复用范式：`userspace/src/g2d.cpp` 已经能**直接吃 V4L2 EXPBUF 的 dma-buf fd**（`move_nv12(v4l2_dmabuf_fd, dst.fd(), w, h)`，板上实测 0 差异），`ar0234::DmaBuffer` 也把 dma-heap 分配 + sync 编码协商都封装好了。

**要做什么**
1. 写一个最小的端到端程序（放 `userspace/apps/`）：
   - `Capture` 拿 ISP 输出（**640×400 BGR 是已知可用的最小尺寸**，硬件缩放直出 @118.94 fps；若模型要 224×224 再缩一次）；
   - 从 V4L2 EXPBUF 取 **dma-buf fd** → `vip_create_buffer_from_fd` 建 NPU 输入缓冲 → `vip_set_input` → `vip_run_network`；
   - **对照组**：同一帧拷到 CPU（普通堆内存）再 `vip_create_buffer` + 跑一次；
   - 两组输出逐元素比对。
2. 记录：零拷贝 vs 拷贝的端到端延迟、CPU 占用、以及 1200p120 下是否稳定。
3. 顺带验证 dma-buf 的 **cache 一致性**：ISP 写入 → NPU 读，是否需要显式 sync（参考 C13 的编码坑；`ar0234::DmaBuffer` 的协商逻辑可以借）。

**验收判据**
- 零拷贝推理结果与拷贝版**逐元素一致**（或差异仅在 float 舍入范围内），并且是对**真实 ISP 帧**而不是合成图。
- 全程 CPU 不参与像素搬运（可用 `perf stat` / `/proc/<pid>/stat` 的 CPU 时间佐证）。
- 给出 640×400 @120 fps 下「采集→NPU→取输出」的端到端延迟数字。

**涉及文件/位置**：`userspace/src/g2d.cpp`（dma-buf 零拷贝范式）、`userspace/include/ar0234/v4l2.hpp`（EXPBUF）、`userspace/apps/`、`/usr/include/vip_lite.h`、`/opt/vpm_run/`、`/opt/yolov5/`、audit §1.5、`hwapi/README.md` §5。

**阻塞点**：无（纯用户态）。
**注意**：① `vip_query_driver_version` 头文件有、库里没有，按实际导出核对；② **libisp 与 cedarc 不能链进同一个进程**（同名不兼容的 iniparser 符号，§3.5）—— libNBGlinker 不冲突，但和 OpenCV 一起时要留意；③ NPU 模型要 224×224 就得自己缩，板上没有现成的缩放包装（`G2d` 的 `resize` 可以，但**缩放不是位精确路径**）。

---

### C11. CMA 只有 16 MB —— 评估调大 + 多消费者必须走 DMA-BUF 扇出

**现状**（本批在板上实测 + 源码确认）
- `CmaTotal: 16384 kB`（**16 MB**）、空载 `CmaFree: 14080 kB`。
- 来源是**内核编译配置**：`CONFIG_CMA_SIZE_MBYTES=16`（`kernel/orange-pi-6.6-sun60iw2/.config:7735`，`CONFIG_CMA_SIZE_SEL_MBYTES=y`）。`/proc/cmdline` 里**没有** `cma=`；DTB 里也**没有** CMA reserved-memory 节点（`/reserved-memory` 下只有 `bl31` 那个固定区）。
- 算一下：1920×1200 NV12 单帧 = 1920 × 1200 × 1.5 = **3 456 000 B = 3.46 MB** ⇒ CMA 里大约只能放 **4 个** 1200p 帧；而 VIN 每路自己就要 `bkuf cnt: 4`（4 × 3.457 MB ≈ 13.8 MB）⇒ **一路就几乎吃掉整个 CMA**。
- 好消息：`/dev/dma_heap/system` 与 `/dev/dma_heap/reserved` 都在位，已交付的 `ar0234::DmaBuffer` 就走 dma-heap。

**要做什么**
1. 评估两条路：
   - **(a) bootargs 加 `cma=<size>`** —— 内核源码 `kernel/dma/contiguous.c:228` 明确是 `if (size_cmdline != -1) { selected_size = size_cmdline; … } else { 用 CONFIG_CMA_SIZE_MBYTES }` ⇒ **cmdline 的 `cma=` 会覆盖 build 默认值，不用重编内核**。改 bootargs（uboot env / `boot.scr`，或 DTB 的 `/chosen bootargs`）。
   - **(b) 重编内核改 `CONFIG_CMA_SIZE_MBYTES`** —— 更彻底但要重编、且与「源码树 DTS 已脱节」的 C12 交叉。
2. 评估「多消费者」的正确做法：**必须走 DMA-BUF 扇出**（V4L2 EXPBUF 的同一个 fd 直接给 G2D / NPU / 编码器 / OpenCL），而不是每个消费者各拷一份。这是 16 MB 下唯一能同时供 NPU + OpenCV + 录像的做法。`/dev/dma_heap/reserved` 可用来放需要连续的缓冲。
3. 量化：同时跑「两路 1200p120 + NPU + VE 录像」时的峰值 dma-buf 占用。

**验收判据**
- 改完后 `CmaTotal` 变化符合预期（`cat /proc/meminfo | grep Cma`）；系统其余部分不受影响。
- 目标场景（多消费者同时跑）**不出现分配失败**（`dma_alloc_*` 失败 / `-ENOMEM`），dmesg 无相关报错。
- 给出 `/sys/kernel/debug/dma_buf/bufinfo` 的证据，说明消费者的 fd 复用关系（**注意 bufinfo 的 "Attached Devices" 显示的是历史 attach，别误判**）。

**涉及文件/位置**：bootargs（uboot env / `boot.scr` / DTB `/chosen bootargs`）、`kernel/dma/contiguous.c`、`.config`（`CONFIG_CMA_SIZE_MBYTES`）、`/dev/dma_heap/*`、`userspace/src/g2d.cpp`（dma-buf 用法）、audit §1.7。

**阻塞点**：无。风险：改 bootargs 低、重编内核中。

---

### C12. 源码树 DTS 与板上 DTB 已脱节 —— 把改动正式落进 `userpatches/`（原 T8）

**现状**
- **内核源码树里根本没有 ar0234**：`arch/arm64/boot/dts/allwinner/sun60i-a733-orangepi-zero3w.dts` 里 `sensor0_mname = "imx219"`；全树 `find -name "*ar0234*"` **零命中**（`ar0234_mipi.c` 是仓库根的**外部模块**，不在内核树里）。
- 板上现在的可用状态是**人工 `fdtput` 改 DTB + `updates/` 里的外部 `.ko`** 拼出来的。
- ⇒ **从源码重建内核/DTB 会丢掉全部改动**（这正是原任务 T8 要解决的事，至今没做）。
- `orangepi-build/userpatches/` 现在是**空的**（只有 `atf/` 与 `customize-image.sh`，**没有 `kernel/`**）。
- 现有产物：`patches/0001..0012`（`apply.sh` 会按序施 0001–0012）、`prebuilt/vin_v4l2.ko`（= 0012 构建，md5 `375f6f4f5c15f72b2d2323f32e7af087`）、`packaging/`（`build-deb.sh` + `postinst`）。

**要做什么**
1. 建 `userpatches/kernel/sun60iw2-current/`，把内核侧改动正式化：`0001`（驱动 + Kconfig + Makefile + DTS）、`0003`（vin 自动 S_INPUT 等）等。
2. 建 `userpatches/linux-sun60iw2-current-a733.config`，收 `0002`/`0004` 的配置改动（`SENSOR_AR0234`、`D3D`/`D3D_LBC_MODE`）。
3. 给 **DTS 改动**找正式落点（补丁或 userpatches 覆盖）：传感器的 `mname`/`pwdn`/`cci`/`mclk_id`、第二路槽位、`tdm work_mode`、`csi_isp`、`resets`。**注意**：现在这些在板上是 fdtput 出来的，源码树里连 imx219 都还在。
4. 构建命令：`./build.sh BOARD=orangepizero3w BRANCH=current BUILD_OPT=kernel REVISION=1.0.1`。
5. **装板前先准备回退**：备份 `/boot`，确认串口能救（`tools/uboot_repair.py` / `uboot_rescue.py` / `uboot_fix_dtb.txt`）。

**验收判据**
- 新建的 deb 装到板上后，**不用 `fdtput`、不用手工拷 `prebuilt/` 模块**，相机就能正常出流（与现在的手工状态等价：`/dev/video0` 1200p120 ≥119 fps / 0 超时、`ar0234-3ad` active）。
- 反复「装 → 重启 → 卸载 → 重启」后 dtb/模块/服务/参数全部能还原（`packaging/` 已有这套回归方法，见 §4 的 T5 完成记录）。
- `Depends: linux-image-current-sun60iw2 (= 1.0.0)` 会挡住内核升级——这是预期行为，要写进包说明。

**涉及文件/位置**：`../userpatches/`、`apply.sh`、`patches/0001..0012`、`ar0234_mipi.c`、`prebuilt/`、`packaging/build-deb.sh`、`packaging/postinst`、`board/install.sh`、`BASELINE.md`、`ARCHITECTURE.md`。

**阻塞点**：无。
**⚠️ 风险**：内核树属主是 root 且里有 GPU 编译残留，**不要直接改**；`apply.sh` 要用户同意才跑。

---

### C13. `tools/g2d_test.cpp` 的 dma-buf sync 一直是空操作

**现状**（第九轮新发现，**本批只写报告、未改**）
- 本内核 `include/uapi/linux/dma-buf.h` 用的是**旧编码**：
  `DMA_BUF_SYNC_READ = 1<<0`、`WRITE = 2<<0`、`RW = 3`、`START = 0<<2`、`END = 1<<2`、`VALID_FLAGS_MASK = (RW | END) = 7`。
- 而 `tools/g2d_test.cpp:109-113` 写的是 **mainline 那套**（`READ 1<<2`、`WRITE 2<<2`、`START 0<<0`、`END 1<<0`），于是 `START|RW = 12` **每次都被内核以 `EINVAL` 拒绝**；`sync_()` 又**忽略返回值** ⇒ 所谓「修正后的 sync 次序」**从头到尾都是空操作**。
- 板上实测（`DMA_BUF_IOCTL_SYNC = 0x40086200`，root 与 orangepi 结果一致）：
  `flags=0 EINVAL`、`1 OK`、`2 OK`、`3 OK`、`4 EINVAL`、`5 OK`、`6 OK`、`7 OK`、`12 EINVAL`、`13 EINVAL` ⇒ **旧编码；而且方向位是必需的**（0 与「只有 END」都被拒）。内核侧对得上：`drivers/dma-buf/dma-buf.c` 的 mask 检查 + `switch (sync.flags & DMA_BUF_SYNC_RW)` 的 default 分支。
- **新封装已经是对的**：`userspace/include/ar0234/g2d.hpp` + `userspace/src/g2d.cpp` 的 `ar0234::DmaBuffer` **不硬编码任何一种编码**，第一次 sync 时两种都试一次、记住内核接受的那个（两种编码互相排斥：旧 RW=3 落在新 mask 之外、新 RW=12 落在旧 mask 之外 ⇒ 不会误判）。
- 顺带解释了一个老疑问：为什么「有没有 sync」在结果上看不出差别 —— 因为 `/dev/dma_heap/system` 的映射在这块板子上本来就不需要 CPU 侧 cache 维护（CPU 写 G2D 能读到、G2D 写 CPU 能读到，0 差异实验双向证明）。

**要做什么**
1. 把 `tools/g2d_test.cpp`（和 `tools/g2d-probes/*.c`）里的 `DMA_BUF_SYNC_*` 换成**旧编码**，**或**直接改用 `ar0234::DmaBuffer`（推荐，顺带拿到协商逻辑）。
2. **不再忽略 ioctl 返回值**：同步失败要打出来（或 assert）。
3. 换完**重跑一次它们的对照**，确认原有结论不变。

**验收判据**
- 板上重跑 `g2d_test`（含 `--capture`）与 `g2d-probes/` 里相关的探针，质化结论不变：两次 `G2D_FORMAT_Y8` blit 的 NV12 搬运**位精确**；单次 4:2:0 `YUV420UVC` blit **不位精确**；`ARGB8888` alpha=0xFF 逐通道位精确；`FILLRECT` 矩形内全改、外 0。
- sync ioctl 的返回码被检查/记录，不再出现「静默失败」。
- **不要**再把「差异字节数」当指标（见 §0.7：三份报告的数字互相矛盾，不是常数）。

**涉及文件/位置**：`tools/g2d_test.cpp`（第 109-113 行 + `sync_()`）、`tools/g2d-probes/*.c`、参考实现 `userspace/src/g2d.cpp`、报告 `analysis/round9/REPORT.md` T5 末尾、`analysis/g2d/REPORT.md` §3。

**阻塞点**：无（半小时级改动）。

---

### C14. 工业固定模式配置文件 `/etc/ar0234.conf`（原 T3 剩余）

**现状**
- `/etc/ar0234.conf` **不存在** —— 3A 服务 `ar0234-3ad`（systemd 开机自启，active；按 ISP 事件为每路流启 libisp，1200p120 自动关 3DNR）**只能靠 `ar0234-rec -e/-g` 在开流后临时改曝光/增益**，没有「固定模式」的配置入口。
- 用户的目标就是**定焦 + 固定曝光/增益 + 要 RGB**，所以这是目标本身的核心缺口（不是报错类问题，容易被忽略）。
- 配套的上游已就绪：`isp/isp_param_industrial.bin`（第七轮已交付，md5 `f3fe7e50c89061f28c88e96ea504dbc7`，曝光线性度 **Pearson r = 0.99989**，出厂集只有 0.95676）；`tools/make_isp_bin.py` 支持 `--set 模块=0` 与 `--gamma-table linear`。

**要做什么**
1. 定义并实现 `/etc/ar0234.conf`：`mode=auto|fixed`、`exposure_us`、`gain`、`awb=auto|fixed`、`wb_temperature`（或 R/B 增益）、`params=3dnr|no3dnr|industrial`。
2. **两条实现路径都要先验证**（原 T3 明确写了「都要先验证」）：
   - **(a) v4l2 控件路径**：服务在 libisp 启动后对 **ISP 子设备 `/dev/v4l-subdev12`** 设 `auto_exposure=1`、`exposure_time_absolute`、`gain_automatic=0`、`gain`、`white_balance_automatic=0`、`white_balance_temperature`。**该节点已经暴露了同一组控件，但「libisp 会不会响应」还没验证** —— 先做一次最小验证（改值 → 读回 → 看 ISP 日志/亮度是否变）。
   - **(b) 参数文件路径**：在 test 段写固定值 —— `manual_en`（**结构体**偏移 88）、`isp_gain`（68）、`isp_exp_line`（72），`ae_en`/`awb_en` 置 0（偏移表见 `tools/make_isp_bin.py` 与 `analysis/libisp-offsets/setters.json`，那份是从 `libisp_ini.so` 的 `set_*` 反汇编得来的）。
     **⚠️ 文件偏移 = 结构体偏移 + 74**（74 = 4 B 长度 + 20 B 日期 + 50 B note），写文件时必须加这个偏移。
3. 打开 AE/AWB 日志的办法：参数文件**偏移 64** 写 `isp_log_param=0x3`。

**验收判据**
- 打开 AE/AWB 日志后，`EXP_TIME`、`AGAIN`、`WB Gain` **全程不变**（不是「变得很慢」，是恒定）。
- **开关灯**时画面亮度与色偏不变（这是「固定」的判定）。
- **任何程序**出流都生效（包括 `v4l2-ctl`、OpenCV、自研程序，不只是 `ar0234-rec`）。
- 与 `isp/isp_param_industrial.bin` 配合使用时，线性度指标不退化。

**涉及文件/位置**：`userspace/apps/ar0234-3ad.cpp`、`userspace/src/isp3a.cpp`、`userspace/include/ar0234/isp3a.hpp`、`tools/make_isp_bin.py`、`analysis/libisp-offsets/setters.json`、`isp/isp_param_industrial.bin`、HANDOFF §4 的 T3 原文。

**阻塞点**：无（需要板上验证，且要重启/换参数文件）。

---

### C15. 低优先诊断三项（其中一项是禁区）

**(a) `isp01` 破坏机制 —— 「禁区」，只在确实需要第四路 ISP 时才查**
- **现状**：`isp@58ffffc` 一 `status = okay` 就把第一路变成**静默 0 帧**（`VIDIOC_S_INPUT` 返回空设备名、`vi0 prs_in y:0`、**内核一条错都不报**）；加了驱动保护（`patches/0008`）之后 `sunxi_isp.1` 不再被注册（media graph 21 个实体与正常启动完全一致），**但第一路照样 0 帧** ⇒ 坏事的是**这个 DT 节点被 enable 本身**。开关用 `tools/dt_second_cam.sh isp01-on|isp01-off`（自带 DTB 备份）。
- **候选机制（未直接观测）**：`isp00/01/02/03` 是**同一个 0x1300 字节寄存器窗口的四个别名**，**结束地址都是 `0x5901300`**，起始地址每个差 4 字节（`0x5900000` / `0x58ffffc` / `0x58ffff8` / `0x58ffff4`），而 `of_iomap()` 拿的是区间**起点** ⇒ isp01 实例的每次寄存器访问都比 ISP0 的同一个字段**低 4 字节**，写成功但写进了别的寄存器。
- **只读旁证已有**：出流中 `0x5900000 = 0x00000005`（ISP 活着），而 `0x58ffffc = 0`、`0x58ffff8 = 0`、`0x5901300 = 0`。
- **要做什么**：不建议主动做。只读验证法：出流中同时读 `0x5900000` 与 `0x58ffffc` 比对；或统计 isp01 实例的 `bsp_isp_*` 调用次数。
- **验收判据**：能解释「enable 一个不注册设备的 DT 节点为什么会让第一路 0 帧」，且**不需要**靠 enable 它来验证。
- **阻塞点**：风险高（一 enable 就毁第一路，只能靠重启/断电恢复）。**结论：保持不动。**

**(b) 1080p 136 fps 边界的机制 —— 未定位**
- **现状**：请求 **130/132/133/134/135 全部干净**（实测 131.6–131.8 fps），**136 才崩**（3 s 内 201 次 `frame_lost` + 201 次 `sunxi_isp_reset`，`ISP frame number is 0` 无限循环）。驱动把 ≥134 的请求都钳到**同一个 `fll = 1096 (133 fps)`**（132→1109、133→1100），但 **134/135 干净、136 崩** ⇒ 机制未定位。
- 已有的候选：问题可能在 **3A 的 `ispSetFpsRanage` 曝光上限**，不在传感器。
- **要做什么**：固定 1080p，在 133…137 之间细扫；同时读 3A 的曝光上限与 `sensor vblank`。**不要去查硅片**（不是硬件的锅）。
- **验收判据**：给出 fll/曝光上限与「崩」的因果（例如「136 时 3A 把曝光上限算到了 0」这类可检验的机制），而不是又一组现象。
- **阻塞点**：无（低优先）。**注意**：这条风暴**不需要重启**（停流重开即好），但**如果是时钟/供电类原因就救不回来**（见 §3.2 的按成因分类）。

**(c) `vind_mclkpin` 是否还有别的引用计数缺口**
- **现状**：三条签名（`Runtime PM usage count underflow!` + `_regulator_disable` WARNING + `[ERR] vin_pin_disable: disable vind_mclkpin error`）已由 `patches/0012` 修掉 —— 触发序列 A（`open+close(/dev/video0)` ×5）与 B（`v4l2-ctl --get-fmt-video` ×5）从 **30 行 → 0 行**，且 `regulator_summary` 的 `5800800.vind-vind_mclkpin` enable count 在 5 轮出流+关流后**回到 0**（证明合法关流路径**没有**被误跳过）。
- **但**：`0012` 只把 `__vin_pipeline_close()` 里那**三件事**统一到了 `vind->use_count` 之下；`vin_pin_disable()` 沿路径还有别的分支（第六轮曾见过开机态下 `disable vind_mclkpin error`，第七轮定为「每 boot 1 次」，第九轮为 0 次）。**要防它从别处回来。**
- **要做什么**：在 `vin_pin_disable` 附近加一次**只读**诊断（「使能计数不匹配就打 ERR」，不改状态机），然后跑 `tools/stream_watchdog_test.cpp`（40×4 s）+ T3 那种 30×（20 s + touch 第二节点）压测。
- **验收判据**：全 boot 三条签名恒 0，且 `regulator_summary` 计数在长压测后仍能回到 0。
- **涉及**：`patches/0012`、`analysis/round9/REPORT.md` T2/T3、`analysis/fix-round/REPORT.md` P1-6、audit §1.10。
- **阻塞点**：无（低优先）。

---

## D. 队列里但优先级低的

### D16. 长稳 / NPU 端到端 / 图像外传 / 触发实测

- **4 h 长稳（原 T11）** —— 分两组各连续 4 小时：① 1200p**110** BGR 采集；② 1200p**110** NV12 + VE 录像（`ar0234-rec -n 0`）。**video0 同一时间只能输出一种格式，所以不能同时跑。**
  记录：传感器温度（注意 `temperature_approx_degc` 目前**恒读 0**，控件在但值不动）、SoC 温度（`cpub/cpul/ddr/npu/gpu/skin`）、丢帧、内存、dmesg。
  验收：出报告，4 h 无掉流、无新增内核报错；给出热平衡值与最坏帧间隔。
  阻塞：无（但占用板子 8 小时）。**注意**：这是唯一能暴露「T16b 偶发复发」的测试，所以跑的时候要带上第二节点 touch。

- **NPU 端到端（原 T19）** —— 上位目标是完整 pipeline：**ISP → NPU → 后处理 → 输出**。第一步就是 C10（零拷贝喂流），之后才是加后处理与输出。前置：自己的模型需要 A5 的转换工具。

- **图像外传（原 T22）** —— USB UVC gadget（内核已支持，要确认 gadget 配置与 `configfs` 用法）**或** USB 网卡。**板子没有以太网，USB 设备口只有 USB 2.0 high-speed** ⇒ 全分辨率不压缩最多约 15 fps。验收：主机侧能稳定收到 1200p 图像/视频流，给出实际带宽与帧率。

- **触发/闪光灯实测（原 T18）** —— 就是 A2，缺硬件；硬件到手后 A2 与 D 的这一条是同一件事。

---

## E. 已排除的死路（**别让下一个 agent 重走**）

1. **打开 `isp01` 当第二块 ISP** —— ❌ 它不是独立 ISP。`isp00/01/02/03` 是同一块 ISP602 的**别名**（寄存器窗口**结束地址都是 `0x5901300`**，起始地址各差 4 字节），而且会**静默**把第一路变成 **0 帧**（内核一条错都不报）。加驱动保护后仍然坏 ⇒ 坏事的是「enable 这个节点」本身。**绝对不要开**。第二路请走 `vinc@5832000`（见 B9/A1）。
2. **用单次 4:2:0 blit 做 G2D 位精确搬运** —— ❌ **不可能**，是路径决定的：`G2D_BLT_NONE_H` 的 YUV→YUV blit 不是字节搬运器，它走 `ovl_v → scal(VSU) → bld → wb`，色度在 mixer 内部被当成 4:4:4 再重采样（冲激响应跨 -2..+4 字节、带负瓣），Y 还会被**硬钳到 ≥16**。要位精确就用 **两次 `G2D_FORMAT_Y8` blit**（同一缓冲看成 `w×1.5h` 的单平面图，`clip_rect.y=0/h=Y`、`y=h/h/2=UV`），已封装成 `ar0234::G2d::move_nv12()`。
3. **用格式常量 `0x29` 当 NV12** —— ❌ `0x29`（`G2D_FORMAT_YUV420UVC_U1V1U0U0`）**其实是 NV21**，会把**红蓝对调**（YUV→YUV 拷贝自洽所以看不出来，但任何 ARGB 方向转换都会错）。**NV12 是 `0x28`**（`G2D_FORMAT_YUV420UVC_V1U1V0U0`，U 在偶字节）。第六/七轮的探针一直用的是 0x29。
4. **`rmmod vin_v4l2` 想换模块** —— ❌ 与 `vin_io` 循环 refcount（`rmmod` 报 `Module vin_v4l2 is in use`，`lsmod` 里 use count 1 但 used-by 为空），**卸不掉**；换模块只能装好 → `depmod -a` → **重启**。
5. **没有模组就 enable sensor 槽位节点** —— ❌ probe 阶段 panic + boot loop（`v4l2_i2c_new_subdev()` 建了 client 后认为驱动没绑上 → 回滚 `i2c_unregister_device()` → `ar0234_mipi.remove` → `cci_dev_remove_helper()` → 对没建成的 device `device_unregister()` → NULL 解引用，`pc : device_del+0x48`，完整栈在 `analysis/t14/serial-recover.log`）。**顺序永远是「先插模组，再开节点」。**
6. **用 `init=/bin/sh` 救板子** —— ❌ `boot.scr` 生成的 consoleargs 是 `console=ttyS0,115200 console=tty1`，**`/dev/console` 是 tty1（HDMI）**，shell 落在 HDMI 上、串口既看不到也进不去；而且没有 systemd ⇒ **16 s 看门狗不会被打开**，卡住只能断电。要用就显式写 `setenv extraargs "console=ttyS0,115200 init=/bin/sh"`。**串口 SysRq 也不可用**（厂商 `uart-ng` 控制台驱动；BREAK / `tcsendbreak` / 降波特率造长 BREAK 三种实测都没反应）。
7. **H.265@1920×1200@120 的「高度对齐」假设** —— ❌ **已证伪**。第九轮 **6/6 次精确重复全部全速通过**（117.67–118.66 fps，最快 7.0 ms/帧，比同参数 H.264 的 113.6 fps **还快**）。而且「对齐 64」这个变量本身无法成立：capture 节点上请求 `height=1216/1280` 会被**静默改回 1200**（`vi0 prs_in` 始终 `y:1200`），而 1080（同样非 64 倍数）的 H.265 一直正常。第六轮那次「6 帧失败」最可能是 **P0-2/T6 的开流坏启动（5–10%）**：签名完全一致（出几帧 + 2 s wall），且 ISP 的 `configuration error`/`height error` **与失败无因果**（有一次运行里 ISP 打了 2 条错、照样 240/240 帧 117.67 fps）。

---

## 附：写报告时请照抄的口径

- **证据等级**：确认 / 复现 / 观察 / 候选 / 外推 —— 别把「外推」写成「结论」（例如「两路需要 ≈600 MHz」是**外推**）。
- **不要写死数字**：G2D 的「差异字节数」、JPEG 的「33KB」、编码的「115.7 fps」都与内容/质量相关，**不是常数**。
- **区分状态**：能跑 ≠ 已修 ≠ 已验收。`0009` 那种「已修复」后来降级成「偶发、概率下降」就是教训。
- **改完就更新**：本文件、`HANDOFF.md`、`README.md`、`hwapi/README.md` 四处要一起改，并标清**哪一轮**、**什么证据**。
