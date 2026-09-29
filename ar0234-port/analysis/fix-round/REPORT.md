# 修复轮报告（fix-round）—— 2026-09-16

输入清单：`analysis/hardware-audit/REPORT-0916-hardware-audit.md` §2.3
板子：Orange Pi Zero 3W / A733（sun60iw2），内核 6.6.98-sun60iw2
起始状态：DTB `d4ee5b68…`（本批全程不改）、模块 = 补丁 0009、`/dev/video0`+`/dev/video4`、ISP 时钟 324 MHz

本文件按项增量追加，每项包含：命令 / 退出码 / 关键输出 / 结论。

---

## P0-1　G2D 开机加载 + 权限 —— 已修复（热插拔 + 冷启动均已验收）

**问题（审计 §1.5）**：`/dev/g2d` 开机不存在（`g2d_sunxi` 不在 modules-load），且手工 modprobe 后是 `crw------- root root`，
非 root 用不了。

**改动（仓库）**

| 文件 | 改动 |
|---|---|
| `board/g2d.conf` | 新增，内容 `g2d_sunxi`（+ 注释说明为什么必须显式列） |
| `board/99-ar0234-camera.rules` | 新增 `KERNEL=="g2d", GROUP="video", MODE="0660"` |
| `board/install.sh` | 装 `/etc/modules-load.d/g2d.conf`；`install` udev 规则（原先 install.sh **根本没装**这个 rules 文件，只有 deb 装）；`udevadm control --reload-rules`；`backup()` 改成能容忍文件不存在 |
| `packaging/build-deb.sh` | payload 里加 `etc/modules-load.d/g2d.conf` |
| `packaging/postinst` | 加 `udevadm control --reload-rules` |

**改动（板上，已执行）**

```
备份: /etc/udev/rules.d/99-ar0234-camera.rules.orig-pre-g2d
新写: /etc/modules-load.d/g2d.conf                       (g2d_sunxi)
覆写: /etc/udev/rules.d/99-ar0234-camera.rules           (追加 g2d 规则)
```

回退（板上）：
```sh
sudo rm -f /etc/modules-load.d/g2d.conf
sudo cp -a /etc/udev/rules.d/99-ar0234-camera.rules.orig-pre-g2d /etc/udev/rules.d/99-ar0234-camera.rules
sudo udevadm control --reload-rules
```

**验证 1：热插拔路径（不等重启）**

```
$ modprobe g2d_sunxi; udevadm control --reload-rules
modprobe rc=0
$ ls -l /dev/g2d
crw-rw---- 1 root video 510, 0 Sep 16 14:13 /dev/g2d        <-- 权限已是 video 组、0660（原来 crw------- root root）
$ udevadm info /dev/g2d
P: /devices/virtual/g2d/g2d   N: g2d   E: SUBSYSTEM=g2d   E: MAJOR=510  E: MINOR=0
```

注意：**主设备号从审计里的 `234` 变成 `510`** —— 说明它不是 misc 设备而是自注册 class `g2d`；
规则用 `KERNEL=="g2d"`（设备名，非主次号），所以两种情况下都命中。审计里写的 `234, 0` 与本次不一致，
但结论（权限 0600 root:root）一致。

**验证 2：以 `orangepi`（非 sudo、`video` 组成员）打开**

```
$ id          -> uid=1000(orangepi) ... 44(video) ...
$ python3: os.open("/dev/g2d", os.O_RDWR)
open /dev/g2d OK as uid 1000
```
`/dev/dma_heap/system`、`/dev/dma_heap/reserved` 也是 `crw-rw---- root video`（`99-t527-permissions.rules` 管的），
所以 `g2d_test`（需要 dma_heap）在非 root 下也具备全部前提。**完整 `g2d_test` 以 orangepi 跑通见 P0-2 末尾**（两处共用一次部署）。

**验证 3：cold boot 自动加载（收工重启后实测，见文末「收工状态」）**

重启后**没有手工 modprobe**，`/dev/g2d` 自动出现：

```
crw-rw---- 1 root video 237, 0 Sep 16 14:43 /dev/g2d
g2d_sunxi              94208  0        # lsmod 可见，由 /etc/modules-load.d/g2d.conf 拉起
```

注意主设备号是**动态**的：本批两次冷启动分别见到 **510** 和 **237**。所以规则必须匹配设备名
`KERNEL=="g2d"`，不能匹配主次号 —— 这一点写进了 HANDOFF §3.28c-4。
同时以 `orangepi` 用户（非 sudo）跑通 `tools/g2d_test.cpp`：`rc=0`，`g2d version 0x10112114`，
BITBLT 与 2× 缩放均成功。

冷启动后验收清单（同一 boot）：
```
/dev/g2d     crw-rw---- root video        <- 自动出现，权限正确
/dev/video0  crw-rw---- root video
/dev/video4  crw-rw---- root video
ar0234-3ad   active
回归         1920x1200@120 -> 119.93 fps, timeouts=0
```


## P0-2　开流「只出 4 帧就死」的兜底 —— 已修复并验收（40×4 s 坏启动 0 残留）

**问题（审计 §1.10 / §2.5-1）**：约 5–10% 的 `STREAMON` 会「出几帧后永久停住」，内核 `dmesg` **一条错都不报**，
`vi0 frame cnt` 复位；下一次启动一定正常。

### 改动（`userspace/`，采集库）

`userspace/include/ar0234/v4l2.hpp` + `userspace/src/v4l2.cpp`：

- `CaptureConfig` 新增看门狗参数：
  - `probe_timeout_ms = 1500`（首帧预算；**=0 关闭看门狗**，外触发/≤1fps 场景必须关）
  - `probe_frames = 8`（连续收到 8 帧才算「流活着」）
  - `retries = 2`（重开次数）
  - `stall_timeout_ms = 0` → 自动 = `max(4×帧周期, 250 ms)`
- `Capture::start()` 现在 = `STREAMON` + **探针**：首帧用 1500 ms 预算，之后每帧必须落在 stall 预算内；
  任何一次超时都判为坏启动 → 重开 → 再探。
- 新增 `StreamStats { start_calls, failed_probes, streamon_failures, hard_reopens, retries_used }`
  与 `Capture::stream_stats()`，每次重开都往 stderr 打一行可识别日志。
- **重开策略 = 关闭 fd 重新 open + 重新 configure**（`restart_hard()`），不是 `STREAMOFF`+`STREAMON`。
  这是实测逼出来的：软重启在坏流上直接 `VIDIOC_STREAMON: Invalid argument`（见下「过程中发现的东西」）。

### 验收：40 次 × 4 s @1920×1200@120（同一 boot，`ar0234-3ad` 在跑）

工具 `tools/stream_watchdog_test.cpp`（新增）：每次迭代 = 新建 Capture → start → 数 4 s 的帧 → 关闭；
**「BAD」定义 = 该次收到的帧少于健康值的一半**（正是坏启动的形状）。

**基线（`--off`，关掉看门狗 = 旧行为）** —— 现象照样复现：

```
iter 24/40: frames=6 wall=4.068s fps=1.47 ... BAD
iter 28/40: frames=6 wall=4.066s fps=1.48 ... BAD
iter 40/40: frames=6 wall=4.067s fps=1.48 ... BAD
==== summary ====
BAD starts remaining   : 3      <-- 3/40 = 7.5%
frames total : 17618 in 160.426 s (109.820 fps overall)
```
（审计上一轮是 4/40 = 10%、`frames=4`；本次 3/40 = 7.5%、`frames=6`。
**同一条故障，频率 5–10% 之间波动，帧数 4~6 不等**——「4 帧」不是硬数字，是「一个 buffer 队列的长度量级」。）
并且再次确认「**紧接着的下一次一定正常**」（24→25、28→29 都是 118–120 fps）。

**看门狗开启**：

```
iter  6/40: ... retries=1 failed_probes=1      <- 坏启动被抓住并救回
ar0234: stream watchdog: restart 1/2, reopening /dev/video0
iter 34/40: ... retries=1 failed_probes=1
==== summary ====
starts                 : 40
frames total           : 19140 in 160.251 s (119.437 fps overall)
failed probes          : 2
starts that retried    : 2  (5.0%)
retries that recovered : 2/2          <-- 重试覆盖率 100%
BAD starts remaining   : 0            <-- 验收指标
threw                  : 0
```

**结论（确认级）**：坏启动仍以 ~5% 的概率发生（内核侧未修，本批按约定不动内核），
但用户态看门狗把**全部 2 次**坏启动都救回来了，40 次里**坏启动残留 0**，
重试代价 = 每次多一次 open+configure（本次实测未影响其余 38 次的 118–120 fps）。
整体 fps 从 109.82（坏启动污染）升到 **119.44**。

### 过程中发现的东西（新，值得记）

1. **坏流上 `STREAMOFF` + `STREAMON` 会返回 `EINVAL`**（第一次实现用的软重启，实测在 iter 2 直接
   `error: VIDIOC_STREAMON: Invalid argument` 抛出）。说明坏的状态不只在「流」里，而在**文件描述符/管线绑定**里。
   ⇒ 只 close+open（等价于「重跑一次程序」）有效，软重启无效。这条以前没有记录过。
2. `g2d_test.cpp` 顺带复测：**G2D fd→fd 拷贝依然不是位精确**，而且这次偏差更大
   （`differing bytes 136451/3110400`，`exact 2973949, <=2 21780, <=8 72138, >8 42533`），
   与审计那次的「全部 ≤2 LSB、>8 为 0」不一致 ⇒ **偏差量级本身在两次运行之间就变了**，
   「6.9% 的字节差 ≤2 LSB」不能当成稳定指标；稳定结论只有「不是位精确」。列入下一批。

### 备注 / 待办
- `ar0234-rec` 用的是同一个 `Capture`，因此自动获得看门狗（`CaptureConfig` 默认值即开启）。
- 默认 `probe_frames = 8`；**外触发/自由运行 ≤1 fps 的场景必须把 `probe_timeout_ms` 设 0 或 `probe_frames` 设 1**，
  否则探针会把正常慢流判成坏流。已在头文件注释里写清。
- 编译告警一条（`g++ 13` 对 `Frame` 移动构造的 `-Wmaybe-uninitialized` 误报，非真实缺陷）。


## P0-3　工业 ISP 参数集 `isp_param_industrial.bin` —— 已生成、已验收（线性度 r = 0.99989）

**产物**：`isp/isp_param_industrial.bin`（116358 B，md5 `f3fe7e50c89061f28c88e96ea504dbc7`）

生成命令（`tools/make_isp_bin.py` 新增 `--gamma-table linear`）：
```sh
python3 tools/make_isp_bin.py isp/template_gc05a2_a733.blob \
        tuning-ref/kurokesu_libcamera_vc4_ar0234.json isp/isp_param_industrial.bin \
        --gamma-table linear \
        --set defog=0 --set lca=0 --set gca=0 --set sharp=0 --set pltm=0 \
        --set drc=0 --set cem=0 --set cnr=0 --set gtm=0 \
        --note "industrial: linear gamma, no beautify blocks"
```

### 关掉/保留了什么

| | 模块 |
|---|---|
| **关掉（0）** | `defog` `lca` `gca` `sharp` `pltm` `drc` `cem` `cnr` **`gtm`** |
| **保留（1）** | `blc` `otf_dpc` `ctc` `wb` `cfa` `ccm` `afs` `ae` `awb` `hist` `nrp` `denoise` `tdf` `dig_gain` `gamma` |
| 未动 | `lsc`/`msc` 本来就是 0（缺标定器材，见「不作为本批」） |

> `gtm`（global tone mapping）**不在任务给的清单里，是我加的**：它和 `drc`/`pltm` 是同一类「色调映射」块，
> 留着会直接破坏曝光-亮度线性。已单列出来，如果只想严格照清单执行，去掉 `--set gtm=0` 重新生成即可。

### 关于 gamma：先测出来布局才敢写

`analysis/libisp-offsets/offs.txt` 只写了 `GAMMA_TBL 56480 u16[5][3072]`。写之前先把模板里的实际数据读出来看：

```
trig: (1300, 1100, 900, 600, 300)      <- 5 个 LV 行
row0: min 0 max 4086  first (0,10,21,31,42,52,62,73)  last (4076,...,4086)
  seg0/seg1/seg2: 三段各自 min 0 max 4086、首 0 尾 4086
```
⇒ **每行 = 3 个通道 × 1024 点**，12 bit 输出码（满量程 4086 ≈ 4095），单调递增，模板里 R=G=B。
所以线性表 = `round(4095*i/1023), i=0..1023`，复制 3 份填满 3072，5 行相同。

### 偏移回读验证（**关键：文件偏移 = 结构体偏移 + 74**）

直接读 **文件** 字节，不是读结构体内存：

```
file[162] struct[88]  manual=0  afs=1 ae=1 af=0 awb=1 hist=1 wdr_split=0 wdr_stitch=0
file[170] struct[96]  otf_dpc=1 ctc=1
file[172] struct[98]  gca=0      <-- was 1
file[175] struct[101] tdf=1
file[176] struct[102] blc=1 wb=1 dig_gain=1 lsc=0 msc=0
file[181] struct[107] pltm=0     <-- was 1
file[182] struct[108] cfa=1
file[183] struct[109] lca=0      <-- was 1
file[184] struct[110] sharp=0    <-- was 1
file[185] struct[111] ccm=1
file[186] struct[112] defog=0    <-- was 1
file[187] struct[113] cnr=0      <-- was 1
file[188] struct[114] drc=0      <-- was 1
file[189] struct[115] gtm=0      <-- was 1
file[190] struct[116] gamma=1 cem=0   <-- cem was 1
(文件偏移 162..194 = 结构体 88..120，逐个对上；这就是 +74 规则本身的一次现场验证)

gamma 表 @ file 56554 (= 结构体 56480 + 74)：
  row0..row4 均为 (0,4,8,12,16,20, ... ,4083,4087,4091,4095)，三段完全相同  -> 线性表已写入
```

### 曝光扫描（验收）

工具 `tools/exp_linearity.cpp`（新增）：先让 AE 收敛，读回它的曝光/增益，然后**固定增益**、
曝光按 ×1/64…×1/4 取 5 档，量中心半个画面的平均亮度（跳过切换后的前 6 帧，否则第一档会读到旧曝光）。

同一 boot、同一次 AE 收敛点（gain code 20200 = 78.91×）：

| | 曝光 469 µs | 937 | 1875 | 3749 | 7499 | **Pearson r** |
|---|---|---|---|---|---|---|
| **出厂参数集**（gamma+DRC+PLTM+GTM+defog/sharp 全开） | 39.96 | 66.50 | 111.91 | 157.88 | 206.28 | **0.95676** |
| **工业参数集**（线性 gamma，美化块全关） | 7.70 | 14.26 | 29.59 | 56.99 | 116.26 | **0.99989** |

工业集逐档比值：14.26/7.70=1.85、29.59/14.26=2.08、56.99/29.59=1.93、116.26/56.99=2.04
（曝光每翻倍、亮度就翻倍 ⇒ 真线性）；出厂集则是低端被压、高端被抬（典型 gamma/DRC 曲线）。

**结论（确认级）**：`r = 0.99989 > 0.99`，验收通过；同时给出对照——出厂参数集只有 0.95676，
证明差异来自参数集本身而不是测量噪声。

### 板子上的安装方式 / 回退

扫描脚本 `b-p03.sh` 的流程（已跑完并**已回退**）：

```sh
# 安装（备份 + 三处都换，否则 ar0234-3ad 会在开流时把参数集换回去）
cp -a /mnt/extsd/isp_param_config.bin            /mnt/extsd/isp_param_config.bin.orig-preindustrial
cp -a /mnt/extsd/ar0234/isp_param_3dnr.bin       .../isp_param_3dnr.bin.orig-preindustrial
cp -a /mnt/extsd/ar0234/isp_param_no3dnr.bin     .../isp_param_no3dnr.bin.orig-preindustrial
cp isp/isp_param_industrial.bin /mnt/extsd/isp_param_config.bin
cp isp/isp_param_industrial.bin /mnt/extsd/ar0234/isp_param_3dnr.bin
cp isp/isp_param_industrial.bin /mnt/extsd/ar0234/isp_param_no3dnr.bin
rm -f /mnt/isp0_*_ar0234_mipi_ctx_saved.bin
# 回退
cp -a /mnt/extsd/*.orig-preindustrial ...（三个文件各自 copy 回去）
```

**收工状态**：三个文件都已回退出厂内容（md5 `a1c392e5…` / `a1c392e5…` / `08215861…` 与安装前一致）。
工业集**没有**设为默认——它是测量/复现用途的选项，装上去会让画面明显变暗变平（线性 gamma 的正常表现），
是否切换由使用者决定。

### 备注
- 现场光照很暗：AE 收敛在 `29994 µs` × `gain code 20200 (78.91×)`，所以 5 档只能取到 1/4 以下才不饱和。
- `--gamma-table linear` 是 `make_isp_bin.py` 的**新增可选参数**，默认仍是 `template`，所以原有生成流程不受影响。


## P0-4　`LC21`（LBC 输出）静默 0 帧 → 改成显式失败 —— 已修复并验收

**问题（审计 §1.2）**：`S_FMT` 设 `LC21`（`V4L2_PIX_FMT_LBC_2_0X`）被接受、VIPP 也配好了，然后 **0 帧**，
`dmesg` 一条都不报。

**根因（源码级，确认）**

1. `vin-vipp/sunxi_scaler.c:816-838` 的 `sunxi_scaler_subdev_s_stream()` 把 4 个 LBC fourcc 和
   `NV12/YUV420` 放在同一个 case 里，统一按 `YUV420` 配 `vipp_output_fmt_cfg()` —— 也就是说**硬件从没被告知要打包**。
2. `grep -rn lbc vin-vipp/vipp100 vin-vipp/vipp200` → **零命中**：这一代 VIPP 根本没有 LBC 输出的寄存器操作。
   全片唯一的 LBC 实现是 **ISP 侧 D3D 3DNR** 那条路（就是 `CONFIG_D3D` / `CONFIG_D3D_LBC_MODE` 在开的那个）。
   所以「LC21 作为 capture 节点输出格式」在这颗 SoC 上是**没有实现**的，不是配置问题。
3. 于是 DMA 写入和 buffer 尺寸口径不一致 → frame-done 永远不来 → 0 帧，且没有任何一方认为出错。

**改动**：`patches/0010-lbc-output-refused-in-scaler.patch`（新增，接进 `apply.sh`）
- 在 `sunxi_scaler.c` 增加 `__scaler_reject_lbc_output()`：若 capture 节点的 `res_pix_fmt` 是那 4 个 LBC fourcc，
  打一条说明性 `vin_err` 并返回 `-EINVAL`；
- 在两个版本的 `sunxi_scaler_subdev_set_fmt()` 里、**source pad** 上调它（sink pad 不查，ISP 侧 LBC/3DNR 路径不受影响）。

**验收（板上实测，模块 srcversion `7922F65188E60D338D42B73`）**

```
$ v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1200,pixelformat=LC21
VIDIOC_S_FMT: failed: Invalid argument          <-- 明确 EINVAL（原来 rc=0）

$ v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1200,pixelformat=NV12
rc=0                                            <-- 对照：正常格式不受影响

dmesg:
[ERR]: vipp0: output pixelformat 0x3132434c (LBC) is not implemented on this SoC;
       the VIPP scaler has no LBC packing output (LBC exists only on the ISP 3DNR path),
       so the stream would run and silently produce 0 frames. Use NV12/YUV420 instead.
[ERR]: vin_pipeline_try_format failed
[ERR]: set fmt error
```
（`0x3132434c` = 'L','C','2','1' 小端 —— 确认拿到的就是请求的 fourcc。）

**结论（确认级）**：从「静默 0 帧」变成「S_FMT 就 `EINVAL` + 一条能读懂原因的 `vin_err`」。
`patches/0010` 对内核树 `--dry-run` 干净通过。

---

## P1-5　`mpp/vi` 的 `CSI Bandwidth` 恒 0（只修整数截断那处）—— 已修复并验收

**改动**：`patches/0011-vin-csi-bandwidth-fix.patch`（新增，接进 `apply.sh`）
`vin-video/vin_core.c` 的 `vin_status_dump()`：

```c
/* 旧 */  vinc->bandwidth = vinc->vin_status.buf_size * (1000/frame_internal/1000)
/* 新 */  vinc->bandwidth = frame_internal ?
              (unsigned int)(((unsigned long long)buf_size * 1000000ULL) / frame_internal) : 0;
```

`frame_internal` 的单位是**微秒**（同一处按 `/1000` 当毫秒打印），所以旧式子里 `1000/frame_internal` 对任何
低于 1000 fps 的帧率都是 0 —— **整条乘积恒为 0**。新式子直接算字节/秒（下面 `/(1024*1024)` 打印成 `M`
正好就是 MB/s），并顺手挡掉 `frame_internal == 0`（第一帧之前）的除零。

**验收（出流中读 debugfs）**

```
$ vfr -t 10 -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 &   # 4 s 后读
  frame => cnt: 550, lost_cnt: 0, error_cnt: 0
  internal => avg: 8(ms), max: 8(ms), min: 7(ms)
  CSI Bandwidth: 411207803                <-- 411 MB/s（原来恒 0）
  --- 同一次 dump 的三个 vinc ---
  CSI Bandwidth: 411012245                <-- vi0（在流）
  CSI Bandwidth: 0                        <-- vi4（不在流，正确）
  CSI Bandwidth: 0                        <-- 第三个 vinc（无实体）
```
手算核对：`1920*1200*1.5*120 = 414.7 MB/s`；实测 411.2 MB/s（`internal avg 8 ms` 略高于 8.33 ms 理论值）。
**量级正确 ⇒ 不再是 0，且不是编出来的数。**

**②③ 未动（按约定只做分析）**
- ② `if (vinc->id == VIN_MAX_DEV - 1)`：`VIN_MAX_DEV = 18`，而板上只有 id 0/4/…实体的 vinc 存在，
  所以那句「CSI Bandwidth total / ISP Bandwidth total」**永不打印**。最小改法是改成检查「这个 dump 的
  vinc 已经是最后一个非空 vinc」或者干脆把它挪到 `vin_debugfs_open()` 的循环之后（那里本来就在遍历）。
- ③ ISP 带宽累加点在 `sunxi_isp.c:748 sunxi_isp_cal_bandwidth_memory()`，被 `#if !defined ISP_600` 排除，
  而本片是 `ISP602`。放开这个 `#if` 会带出没编译过的代码，风险如上表所列，**本批不动**。

---

## P1-6　`underflow` + `vind_mclkpin` regulator 引用计数 —— 已定位（含可复现的最小触发序列），未改代码

**结论一句话**：**一个引用计数缺口，产生三条报错**。触发条件是「capture 节点 open 之后不做 S_FMT/S_INPUT
就 close」；此时 `vin_close()` 会走到 `__vin_pipeline_close()`，而这条路径**没有**配对的 open。

### 静态证据：三个操作、两套语义（`vin.c`）

| `__vin_pipeline_open()`（vin.c:1249-1270） | `__vin_pipeline_close()`（vin.c:1281-1308） | 是否引用计数 |
|---|---|---|
| `vin_md_set_power(vind, 1)` | `vin_md_set_power(vind, 0)` | **有**：`vind->use_count`（`on && use_count++ > 0` 直接 return；`!on && use_count == 0` 直接 return） |
| `vin_pin_enable(vind)` → `regulator_enable()` ×3 | `vin_pin_disable(vind)` → `regulator_disable()` ×3 | **没有**（vin.c:1168/1184，无条件调用） |
| `vin_video_core_s_power(capture, 1)` → `pm_runtime_get_sync` | `vin_video_core_s_power(capture, 0)` → `pm_runtime_put_sync` | **没有**（vin.c:1220） |

三者相邻、成对，但只有第一个带引用计数。所以一次「多出来的 close」的结果精确对应实测的三条输出：

```
sunxi-vin-core 5830000.vinc: Runtime PM usage count underflow!        <- pm_runtime_put_sync 多减一次
WARNING: CPU: 5 PID: 5059 at drivers/regulator/core.c:3012 _regulator_disable+0xf0/0x1c8
sunxi:vin:[ERR]: vin_pin_disable: disable vind_mclkpin error, fail to disable regulator!
sunxi-vin-core 5830000.vinc: Runtime PM usage count underflow!
```
—— `vin_md_set_power(vind,0)` 因为 `use_count == 0` 安全地 no-op 了，**正好暴露出另外两个没有计数**。

### 最小触发序列（只读复现，本批实测）

| 序列 | 操作 | 结果 |
|---|---|---|
| **A** | `open("/dev/video0")` + `close()` ×5，**不打任何 ioctl** | **每次一轮三条报错**（复现） |
| **B** | `v4l2-ctl -d /dev/video0 --get-fmt-video` ×5（= open + G_FMT + close） | **每次一轮三条报错**（复现） |
| C | `v4l2-ctl --set-fmt-video=...NV12` ×3（S_FMT 不流） | **0 条**（S_FMT 会触发自动 S_INPUT，于是 open 真的发生了一次，close 就对上了） |
| D | video0 出流 14 s，期间 `open+close /dev/video4` ×5 再 `open+close /dev/video0` ×2（T16b） | **0 条**，且 video0 **119.96 fps / 0 超时**（0009 对「流中的兄弟节点」确实有效） |

补充两点实测事实：
- **冷启动后、在任何人做过 S_INPUT 之前**，本 boot 的 `grep -c underflow` = **0**。这与 0009 的注释吻合：
  首次 open/close 时 `cap->pipe.sd[VIN_IND_SENSOR]` 还是 NULL，`skip_pipeline_close` 会生效。
  所以审计里那句「每 boot 1 次」不是无条件成立 —— 需要该节点的管线**此前已经被 prepare 过**。
- 序列 A/B 之所以能复现，是因为本次会话之前已经跑过出流（`pipe.sd[]` 已填好）。
  也就是说：**任何「只查设备不出流」的工具（`v4l2-ctl --all`、`v4l_id` udev 助手、桌面环境的探测）
  在任何出过流之后运行，都会把这三条噪声打进 dmesg**。这解释了为什么它看起来"随机"。

### 影响评估（诚实）

- **功能上无影响**：序列 D 里在 5 次触发之后，video0 仍然 119.96 fps、0 超时、0 丢帧。
  `_regulator_disable` 在 `use_count == 0` 时只是 WARN 并返回 `-EIO`（不真的关东西），`pm_runtime_put_sync`
  的 underflow 只会少一次 suspend。**不会 panic、不会掉流**。
- **但它污染 dmesg**：一次 `v4l2-ctl --all` 就能刷 6+ 行 `[ERR]`/`WARNING`/Call trace，
  并且会挤掉环形缓冲里的早期证据（HANDOFF §3.18 的坑），排查时非常容易误判"驱动坏了"。

### 最小修复方案（**本批未实施**，下一批）

给三个操作统一到同一个引用计数上，改动 ~10 行、不碰 DT、不改 ABI：

```c
/* vin.c: __vin_pipeline_close() */
	vind = entity_to_vin_mdev(&sd->entity);
	if (vind) {
#ifdef CSIC_SDRAM_DFS
		csic_chfreq_disable(vind->id);
		vin_chfreq_clk_set(0);
#endif
		/*
		 * 只回退 open 真正做过的事：vin_md_set_power() 有 use_count 保护，
		 * vin_pin_disable()/vin_video_core_s_power() 没有，所以一个没有配对
		 * open 的 close（open+close 但从不 S_INPUT）会把 regulator 和
		 * runtime PM 各多减一次。
		 */
		if (vind->use_count) {              /* <-- 新增判断（在递减之前） */
			vin_md_set_power(vind, 0);
			vin_pin_disable(vind);
			if (p->sd[VIN_IND_CAPTURE] && p->sd[VIN_IND_CAPTURE]->entity.graph_obj.mdev)
				vin_video_core_s_power(p->sd[VIN_IND_CAPTURE], 0);
		}
	}
```
（等价方案：让 `vin_md_set_power()` 返回「是否真的发生了状态切换」，把另外两个调用搬进它的切换分支里 ——
更彻底，但动的地方稍多。）
**为什么不本批做**：这是电源路径，必须重启验证，且 0009 已经证明这条路径与「兄弟节点掉流」耦合；
按任务约束「本批不强行改 PM 代码」。建议下一批用「序列 A/B 复现 → 打补丁 → 同样序列 0 条 + 回归 40×4 s」做验收。


## P1-7　文档订正 —— 已完成

改动前先留了备份：`HANDOFF.md.bak-pre-p17`、`README.md.bak-pre-p17`、`hwapi/README.md.bak-pre-p17`。

| 文件 | 订正内容 |
|---|---|
| `HANDOFF.md` §3.2 | 「ISP 出过错之后，下一次测试结果不可信，先重启再测」→ **删掉，换成精确规则**（见下）|
| `HANDOFF.md` §3.20 | 1080p 上限「132 fps」→ **加删除线 + 指向 §3.28b-1**（实际 136 才崩）|
| `HANDOFF.md` **新增 §3.28c** | 第七轮订正与新增 10 条：偏移 +74 / H.265@1200p120 / G2D 非位精确 / G2D 开机可用 / NPU 可用 / 模块版本 / LC21 显式失败 / 三条 PM 噪声 / 开流必须带看门狗 |
| `HANDOFF.md` **新增 §3.29** | 「只 open 不出流就 close」三条报错的触发条件、定量、根因与最小修复 |
| `HANDOFF.md` §1 G2D 行 | 补「✅ 第七轮已修开机加载与权限」+ 主设备号是动态的（510/237 都见过）这条坑 |
| `HANDOFF.md` §4 任务清单 | 表头插入「第七轮完成情况」汇总表；T4 / T7 标题改为 ✅ 已完成并附产物 |
| `README.md` §能力表 | VE 行拆开：H.264 ✅（1200p120 实测 103 fps）vs **H.265 只在 1920×1200@120 破**；「115.7 fps」注明是别的取景内容 |
| `README.md` 示例 | `ar0234-rec ... -c h265`（1200p120）改成 `-c h264` + 一行警告 |
| `README.md` §坑 | 新增 4 条：**ISP 出错不用重启的精确规则**、参数偏移 +74、G2D 开机可用但非位精确、NPU 可用（`libNBGlinker.so`） |
| `README.md` §部件表 | `v4l2.hpp/src/v4l2.cpp` 行补上开流看门狗的参数与关闭方法 |
| `hwapi/README.md` | 参数偏移处补 **文件偏移 = 结构体偏移 + 74** + 三个具体例子（88..120↔162..194、101↔175、56480↔56554）|
| `analysis/libisp-offsets/offs.txt` | 在 `payload = ...` 那行下面插入 6 行 `!!` 警告块，明确「本文件所有偏移都是结构体偏移」 |
| `tools/make_isp_bin.py` docstring | 同样插入 `!!` 警告块（顺手，属 P1-7 要求）|

**「ISP 出错之后」的精确规则**（现在写在 HANDOFF §3.2 和 README 里）：

| 现象 | 要不要重启 |
|---|---|
| `isp0 frame lost!` + `sunxi_isp_reset` 风暴（如 1080p@136） | **不用**。停流重开即可（实测同一 boot 里 400 次 reset 后立刻 119.78 fps / 0 丢帧） |
| 开流「只出几帧就死」（5~10%） | **不用**。重开一次必好，采集库的看门狗自动兜住 |
| 只 open 不出流就 close 的三条 PM 报错 | **不用**，功能无影响（只是 dmesg 噪声） |
| 「只重载了传感器模块」（`no link to sunxi_mipi.0`） | **必须重启**（link 只在 probe 时建） |
| `isp0 width error`，或连续多次重开仍 0 帧 | 才需要考虑重启 |

> 订正说明：任务书里写的是「H.265 在 **1200×1200**@120 不可用」。本轮实测复验的组合是
> **1920×1200@120**（传感器原生窗口，也是文档/命令里一直在用的那个），失败现象一致
> （6 帧 + `isp0 configuration error`/`height error`）。**`1200×1200` 这个尺寸本轮没有测过，
> 文档里我按实测写成 1920×1200**，没有把没测过的数字写进去。

---

## 补丁与新增文件清单

### 新增补丁（已归档，已接进 `apply.sh`，对内核树 `--dry-run` 干净）

| 补丁 | 内容 | 对应项 |
|---|---|---|
| `patches/0010-lbc-output-refused-in-scaler.patch` | `sunxi_scaler.c`：拒绝 LBC 输出 fourcc，`-EINVAL` + 说明性 `vin_err` | P0-4 |
| `patches/0011-vin-csi-bandwidth-fix.patch` | `vin_core.c`：`CSI Bandwidth` 按字节/秒计算，去掉整数截断 + 防除零 | P1-5 |

两者都用 `-p1 -d "$K/bsp/drivers/vin"`（与 0005~0009 同惯例）。
**没有跑 `sudo bash apply.sh`** —— 内核树没动，模块走外部编译 `tools/build_vin.sh 0010`。

### 模块状态

```
build/vin-d3d-lbc/out/vin_v4l2-0010.ko   md5 241c86316662a79921d7c4bd1de155d5
                                        srcversion 7922F65188E60D338D42B73   (= 0009+0010+0011)
prebuilt/vin_v4l2.ko                    同上（install.sh / build-deb.sh 用的就是它）
板上 /lib/modules/.../updates/vin_v4l2.ko  同上；旧模块备份在 ...ko.bak-pre-0010
```

### 仓库改动

| 文件 | 改动 |
|---|---|
| `userspace/include/ar0234/v4l2.hpp`、`userspace/src/v4l2.cpp` | 开流看门狗 + `get_control()` + `StreamStats`（P0-2） |
| `tools/stream_watchdog_test.cpp` | **新增**：40 次压测 / 量化坏启动与重试覆盖率（P0-2） |
| `tools/exp_linearity.cpp` | **新增**：曝光扫描 + Pearson r（P0-3） |
| `tools/make_isp_bin.py` | `--gamma-table linear` + 偏移 +74 警告（P0-3 / P1-7） |
| `tools/g2d_test.cpp` | 打印 `stream_stats()` |
| `isp/isp_param_industrial.bin` | **新增**（P0-3） |
| `board/g2d.conf`、`board/99-ar0234-camera.rules`、`board/install.sh` | G2D 开机加载 + 权限（P0-1） |
| `packaging/build-deb.sh`、`packaging/postinst` | 同上，进 deb（P0-1） |
| `apply.sh` | 接入 0010 / 0011 |
| `HANDOFF.md`、`README.md`、`hwapi/README.md`、`analysis/libisp-offsets/offs.txt` | 文档订正（P1-7） |
| `analysis/fix-round/REPORT.md` | 本文件 |

**没有 git commit。** 板上的临时构建目录 `~/ar0234test/wd/` 是可重建的测试树，不是安装物。

---

## 收工状态（板上实测）

```
uptime            5 min（本轮重启过 1 次：装 0010 模块）
DTB               d4ee5b6869e7b7cd39ce3762d0e078f9   <-- 与开工时完全一致，全程未改 DT
vin_v4l2          srcversion 7922F65188E60D338D42B73 (= 0009+0010+0011)，md5 241c8631…
/dev/video0       crw-rw---- root video
/dev/video4       crw-rw---- root video
/dev/g2d          crw-rw---- root video   <-- 冷启动自动出现（P0-1 目标达成）
g2d_sunxi         已加载（由 /etc/modules-load.d/g2d.conf）
ISP / CSI 时钟     pll-video0-4x = 324000000 -> csi_isp_src  （324 MHz）
d3d_min_vblank_us 500
ar0234-3ad        active
Runtime watchdog  16 s
ISP 参数文件       三个都 = 原厂内容（isp_param_config.bin 已恢复成打包默认的 3dnr 变体）
残留进程           无测试进程（vfr / stream_watchdog_test / g2d_test / v4l2-ctl 都不在）
健康回归           1920x1200 NV12 @120 → frames=1200 wall=10.006s fps=119.93 timeouts=0，最大间隔 8.573 ms
```

回退命令（板上，P0-1 的配置）：
```sh
sudo rm -f /etc/modules-load.d/g2d.conf
sudo cp -a /etc/udev/rules.d/99-ar0234-camera.rules.orig-pre-g2d /etc/udev/rules.d/99-ar0234-camera.rules
sudo udevadm control --reload-rules
```
回退模块到 0009：`sudo cp -a /lib/modules/6.6.98-sun60iw2/updates/vin_v4l2.ko.bak-pre-0010 <目标>` + 重启。

> 说明：本 boot 的 dmesg 里有 P1-6 复现实验故意打出来的 `underflow` / `_regulator_disable` WARNING
> 若干条（序列 A/B）。它们**不影响功能**（序列 D 在触发之后 video0 仍然 119.96 fps / 0 超时），
> 下一批修掉 PM 引用计数就会消失。没有为此再重启一次。

---

## 不作为本批（附理由）

| 项 | 为什么没做 |
|---|---|
| **ISP 硬复位**（DT `resets` 补 `RST_BUS_VIDEO_IN`） | 需要先**只读**验证 CCU 的 `RST_BUS_VIDEO_IN` 位是否真连到 ISP602；本批没有做这一步，直接改 DT 属于盲改。DT 全程零改动是本批的硬约束。 |
| **H.265@1920×1200@120 根因** | 诊断类，任务明确划到下一批。现有可用规避：1200p120 用 H.264。 |
| **第二节点 open/close 偶发拖慢第一路** | 同上；本轮序列 D（出流中开关 video4 ×5）是 **0 条报错 + 119.96 fps**，属"偶发未复现"，不足以定位。 |
| **`isp01`** | **禁区，全程没有 enable 过。** |
| **LSC/MSC 实拍标定** | 需要均匀光源/积分球，本批没有器材 ⇒ 标 ⬜，**没有造任何数据**。 |
| **`CSI Bandwidth` 的第 ②（`VIN_MAX_DEV-1` 永不成立）和第 ③（`ISP_600` 把 ISP 累加排除）** | ②③ 风险高于 ①，任务只要求修 ①；②③ 的分析与最小改法已写在 P1-5 报告里。 |
| **PM 引用计数修复本身** | 任务明确"本批不强行改 PM 代码"；已给最小补丁 + 验收方案，留给下一批。 |
| **第二颗模组 / 外部触发接线 / 厂商 NBG 工具 / 图像外传** | 缺硬件/缺工具，**不造数据**。 |

---

## 结论分级汇总

| 项 | 等级 | 一句话 |
|---|---|---|
| P0-1 G2D 开机加载 + 权限 | **已修复（确认）** | 冷启动 `/dev/g2d` = `crw-rw---- root video`，orangepi 免 sudo 跑通 g2d_test；配置已进 install.sh 与 deb |
| P0-2 开流坏启动兜底 | **已修复（确认）** | 看门狗 ON：40 次×4 s 坏启动残留 **0**，2/2 重试成功；OFF 基线 3/40 复现 |
| P0-3 工业 ISP 参数集 | **已修复（确认）** | `r = 0.99989`（出厂集 0.95676），偏移按文件字节回读验证 |
| P0-4 LC21 静默 0 帧 | **已修复（确认）** | `S_FMT LC21` → `EINVAL` + 说明性 `vin_err`（原来 rc=0 且 0 帧） |
| P1-5 CSI Bandwidth 截断 | **已修复（确认）** | 出流时 411 MB/s（原来恒 0），②③ 只给方案 |
| P1-6 underflow + regulator | **已定位（确认根因 + 可复现触发）**，未改代码 | 一个引用计数缺口产生三条报错；最小补丁与验收方案已给出 |
| P1-7 文档订正 | **已完成** | 4 个文件，含新增 §3.28c / §3.29 |

