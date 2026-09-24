# Round 10 — 纯软件批次（C 组收口 + 1080p 高帧率风暴机制定位）

日期：2026-09-17。主机 TL101，板子 orangepizero3w（6.6.98-sun60iw2）。
本批**没有跑 `apply.sh`、没有改板上 DT 属性之外的内容**；板上换过 3 次内核模块 + 2 次 boot.scr（都有 .bak-c12 / .orig 备份，收工状态 = 重建模块 0013-diag + 重建 DTB + 出厂 16 MB CMA + auto 模式 conf）。

| 任务 | 结果 |
|---|---|
| C13 g2d_test.cpp sync 空操作 | ✅ 修复 + 板上复测，结论不变 |
| C10 NPU←ISP 零拷贝 | ✅ **300/300 帧 A/B 输出逐字节一致**（真实 ISP 帧） |
| C14 /etc/ar0234.conf 固定模式 | ✅ 双路径验证：**(a) 源码级死路**（MELIS 未编入），**(b) 实测 EXP_LINES/AGAIN/WB Gain 恒定** |
| C12 userpatches 正式化 | ✅ 0001–0013 进 `userpatches/`，**重建模块+DTB 上板 120.06 fps / 0 超时** |
| C11 CMA 16 MB | ⚠️ **前提被实测推翻**：相机栈不走 CMA（IOMMU）；`cma=` 机制已工具化并验证 16→64→16 |
| C15b 1080p 136 fps 边界 | ✅ **机制定位**：不是 136 硬边界，是 ispSetFpsRanage ≥133 的概率性风暴；已修复（3ad） |
| C15c vind_mclkpin 复发防护 | ✅ 0013 只读诊断 + 40×4 s / 30× 压测全绿 |
| C15a isp01 破坏机制 | ✅ 只读机制解释完成（DT 几何 + of_iomap），**维持禁区** |

---

## T-C13 `DMA_BUF_IOCTL_SYNC` 旧编码修复（tools/g2d_test.cpp + g2d-probes）

**改动**
- `tools/g2d_test.cpp`：删除硬编码 mainline 编码（READ 1<<2 / END 1<<0），改为**运行时协商**（先 legacy 后 mainline，与 `userspace/src/g2d.cpp` 的 `ar0234::DmaBuffer` 同一套协商逻辑）；`sync()` 失败改为**抛异常**，main() 顶层 catch → `CONTROL-FAIL`（退出码 2），不再可能静默空操作。
- `tools/g2d-probes/*.c`（全部 11 个）：每份复制粘贴的宏块换成共享头 `tools/g2d-probes/dmabuf_sync.h`（C 版协商 + 失败 `exit(2)`）；`syncbuf`/`sync_` 调用点零改动。README 构建说明同步更新。

**板上复测（g2d_test 无 --capture，1920×1080，orangepi 用户）**
```
ctrl1/ctrl2 全 [EXACT]；phase1 full-range Y [DIFFERS]（maxdelta 16 = <16 钳）、UV [DIFFERS]；
in-range Y [EXACT]；phase1b Y8 对 [EXACT] 0/3110400；ARGB8888 四通道 [EXACT]；result: OK
```
四个机制探针（g2d_work/diag/mech/mark）全部 rc=0，质化结论与 `analysis/g2d/REPORT.md` 一致：两次 Y8 位精确、单次 4:2:0 不精确、FILLRECT 直写、色度是归一化多抽头滤波。
（再次确认 §0.7：差异字节数逐次漂移，不是常数，别再当指标。）

**证据等级**：确认（sync 生效 = 协商探测 + 后续调用返回 0，任何失败会 exit 2，实际退出码 0）。

---

## T-C10 NPU ← ISP 零拷贝（最高性价比项，验收通过）

**交付**：`userspace/apps/ar0234-npu-zerocopy.cpp`（Makefile 里 built-not-installed，链接 `-lNBGlinker`）+ `G2d::convert_nv12_to_bgr888()`（userspace/src/g2d.cpp，NV12→BGR888 一次 blit 带缩放）+ `DmaBuffer` 支持选堆（system/reserved）。

**管线**（CPU 全程不碰像素）：
```
/dev/video0 NV12 640×400 (V4L2 EXPBUF dma-buf)
   → G2D 一遍 blit：缩放 640×400 → 224×224 + NV12→BGR888（设备侧）
   → npu_in dma-heap dma-buf
       A 路: vip_create_buffer_from_fd(fd)  → vip_set_input → vip_run_network   ← 零拷贝
       B 路: CPU memcpy 出去 → vip_create_buffer → vip_flush_buffer(FLUSH) → run  ← 对照
```

**板上实测（300 帧，真实 ISP 帧，/opt/vpm_run 的 224×224 分类 .nb）**：
```
model: input dims [224,224,3,1] fmt UINT8 quant TF(scale 1/255, zero 0) = 150528 B
       output dims [2,1] fmt UINT8 quant TF(scale 0.00162549, zero 128)
G2D convert:      1.44 ms avg / 2.24 ms max
infer zero-copy:  2.95 ms avg / 3.12 ms max
infer CPU-copy:   2.90 ms avg / 3.14 ms max
e2e (dequeue→output ready): 4.39 ms avg / 5.07 ms max   ← 120 fps 帧周期 8.33 ms 装得下
process CPU per frame（两次推理+转换）: 0.71 ms
A/B compare: 0/300 frames differ, max byte delta 0    ← 逐字节一致
stream: 1 start call, 0 failed probes, 0 hard reopens, 0 stalls
```
`--fanout` 变体（同一捕获帧再 move_nv12 一份给"OpenCV 型"消费者并周期性校验和）：240/240 一致，checksum CPU 开销 0.01 ms/次。

**机制性结论**
1. `vip_create_buffer_from_fd` 对 `/dev/dma_heap/system` 的 dma-buf **直接接受**（无需 reserved/CMA 堆——第一次就成功，回退路径没触发）。
2. fd 路径 cache 维护：按头文件契约**宿主不维护、也不需要**——G2D（写）与 NPU（读）都是设备，汇合于 DDR；CPU 从不写该缓冲。CPU 拷贝路径用 `vip_flush_buffer(FLUSH/INVALIDATE)`，结果与零拷贝逐字节一致 ⇒ 两条 cache 契约在板上都成立。
3. `G2D_FORMAT_BGR888` 内存序实测 = B,G,R（224×224 dump 与 NV12 参考色调对齐，as-RGB 渲染偏品红）。喂"RGB 序"模型前要么换 `G2D_FORMAT_RGB888`，要么确认模型通道序。
4. vpm_run 那个 .nb 输出是 **2 类**分类头；本批只对 A/B 等价性背书，语义正确性要等 NBG 转换工具（A5）。

**证据等级**：确认（逐字节一致 + /tmp/npudump 图像核验：两张 PNG 都是真实场景）。

---

## T-C14 `/etc/ar0234.conf` 工业固定模式（验收通过）

**路径 (a) v4l2 子设备控件——源码级死路（新机制结论）**
板上 sunxi_isp 子设备确实暴露 `exposure`(0x00980911)/`gain_automatic`/`white_balance_automatic` 等控件，但内核处理函数 `__sunxi_isp_ctrl()`（bsp/drivers/vin/vin-isp/sunxi_isp.c）对它们的**唯一消费者**是：
```c
#if IS_ENABLED(CONFIG_ISP_SERVER_MELIS)
        if (isp->h3a_stat.state == ISPSTAT_ENABLED) { ... isp_rpmsg_send(...) }
#endif
```
而本内核 `.config:315` → **`# CONFIG_ISP_SERVER_MELIS is not set`**。⇒ 控件写进控制缓存后没有任何去向，libisp 永远看不到。**"libisp 会不会响应"的答案是：不会，机制上不可能。** 控件路径仅剩的用途是没有 3A 时直接手动驱动 sensor（vin 的 video 节点控件），与本配置文件无关。

**路径 (b) 参数文件——实现并实测通过**
- 新模块 `userspace/{include/ar0234,src}/ar0234conf.{hpp,cpp}`：解析 `mode/params/exposure_lines/exposure_us/gain/awb/wb_temperature/ae_log`（未知键、坏值、重复曝光键都会在启动日志里告警；单位全部锚定驱动 `sensor_s_exp_gain`：曝光 = 1/16 行，增益 = 1/16×，行时 = 6.8 µs 恒定（hts 612 / pclk 90 MHz））。
- `IspParamSets::select()` 支持命名参数集（`params=industrial` 等）；新增 `apply_fixed_mode()`：往激活参数文件写 `isp_gain(68)/isp_exp_line(72)/isp_color_temp(76)/isp_log_param(64)/manual_en(88)=1/ae_en(90)=0/awb_en(92)=0`（**结构体偏移，文件 +74**，头部长度做校验），原子替换，值不变不写盘。
- `ar0234-3ad` 启动读 `/etc/ar0234.conf`（`-c` 可覆盖），每路流的子进程先选参数集、再打固定模式补丁、然后起 libisp ⇒ **对流谁都生效**。
- 交付件：`board/ar0234.conf`（注释齐全的样例，出厂 = auto 模式，行为与之前完全一致）；`board/install.sh`（不覆盖已有 conf）；`packaging/build-deb.sh`（`/etc/ar0234.conf` 注册为 **conffile**，dpkg 永不覆盖操作员改动）。

**板上验收（`mode=fixed, params=industrial, exposure_lines=200, gain=4.0, awb=fixed, wb_temperature=5000, ae_log=1`）**
```
ar0234-3ad: parameters /mnt/extsd/ar0234/isp_param_industrial.bin
ar0234-3ad: fixed mode applied to /mnt/extsd/isp_param_config.bin:
            exposure 200 lines, gain 4.000000x, awb pinned, ae_log on
AE 日志（libisp，逐帧）：EXP_LINES: 3200 (=200×16 ✓)  AGAIN: 64 (=4.0×16 ✓)
                        ColorTemp Result: 5000 (=配置值 ✓)  WB Gain: 279 256 397 恒定
→ 600 帧全程恒定；切到 v4l2-ctl --stream-mmap 出流同样生效（"任何程序"验收 ✓）
```
两个单位假设（isp_exp_line = 1/16 行、isp_gain = 1/16×）被日志读数**精确证实**。
收尾已把板上恢复为 auto 样例配置。遗留小疑点（不影响验收）：流结束后 video0 的 gain 控件读回 1600（=1.0×）而 AE 日志全程 AGAIN 64 —— 疑似 close 路径把 info.gain 复位了，Exposure 读回 3200 正常；挂账不影响"逐帧恒定"这一验收主体。
"开关灯亮度/色偏不变"：三个量全被钉死，机制上保证；实灯开关留给用户手测。

**证据等级**：确认（路径 a：源码+config 双证；路径 b：板上日志逐帧）。

---

## T-C12 内核侧改动正式化进 userpatches/（黄金验证通过）

**落点（对照 scripts/compilation.sh 核实过）**
- 内核补丁目录：`userpatches/kernel/sun60iw2-current/`（build 系统 `patch --batch -p1 -N` 从内核根按文件名序应用）。
- 内核配置覆盖：`userpatches/linux-sun60iw2-current-a733.config`（`LINUXCONFIG` 由 `external/config/sources/families/sun60iw2.conf` 对 orangepizero3w+current 指到 `-a733`）。

**内容**
- `userpatches/kernel/sun60iw2-current/`：0001、0003、0008 原样（本来就是内核根相对），**0005/0006/0007/0009/0010/0011/0012/0013 做了纯机械路径改写**（`a/vin-…` → `a/bsp/drivers/vin/vin-…`，apply.sh 是在 bsp/drivers/vin 里打的，build 系统在内核根打）；目录内 README.md 写明来源与改写规则。
- 0002/0004（build 系统自己的 config 补丁）**不进**这里——被整份配置覆盖取代；仍留在 ar0234-port/patches 供 apply.sh 流程。
- `linux-sun60iw2-current-a733.config` = output 生成配置 + `CONFIG_SENSOR_AR0234=m` + D3D/LBC 块（与 0002/0004 增量逐字一致）。
- 新增 `patches/0013`（见 T-C15c），apply.sh 已纳入。

**DTS 对齐核对（板上 DTB d4ee5b68 vs 源码树）**：差异只有 0001 已覆盖的两条（sensor0_mname、sensor2 disabled）；vinc20 okay、tdm work_mode=0、csi_isp/csi_top=324 MHz（板级 dts 覆盖 dtsi 的 600/540）、resets 等本来就一致。**刻意不含** B6（RST_BUS_VIDEO_IN）与 B7（抬时钟）。

**黄金验证（重建 = 上板的完整链路）**
1. `git archive HEAD`（HEAD=8a9be72c9 pristine；146 个脏文件全在 GPU 残留）导出 → **0001–0013 十三个补丁按序全干净应用**；应用后的 vin 源码与出片的 build/vin-d3d-lbc 镜像**逐字节一致**，ar0234_mipi.c md5 相同。
2. 配置：olddefconfig "**No change**"，SENSOR_AR0234=m / D3D=y(LBC) / vin 模块全部存活。
3. 构建：DTB 编出；**逐属性对比运行中板子的 DTB——全部一致**（sensor/vinc8/tdm/csi 时钟/resets…）。
4. vin 模块编出（gcc-11.2 交叉，与镜像同一工具链）；符号表与镜像产物**完全一致**；D3D 代码两侧都在（srcversion 不同仅因镜像强包 board_compat.h，记账差异）。
5. **上板**：重建 vin_v4l2.ko + ar0234_mipi.ko + DTB（备份 .bak-c12）→ 重启 → probe 干净（0xa56, find the onsemi AR0234）→ `vfrrun` **1200p120 = 120.06 fps / timeouts=0 / lost_cnt=0**。

**踩过的坑（进 §3）**：手动验证构建必须带 `LOCALVERSION=-sun60iw2`（build.sh 是从 compilation.sh:498 以 make 环境变量传入的，.config 里 CONFIG_LOCALVERSION=""），否则 version magic 不匹配 → "Exec format error"；改了它还要删 include/config/kernel.release 重生成（缓存在那里）。

**证据等级**：确认（逐属性 DTB 对比 + 板上 120 fps 回归）。

---

## T-C11 CMA 16 MB —— 前提被实测推翻（重要订正）+ 机制工具化

**实测（推翻"CMA 只能放 4 帧"的前提）**
- 1200p120 出流中（vin bkuf 4×3 457 024 B + EXPBUF 全开 + G2D + NPU 同时跑）：`CmaFree` **全程 13952 kB，与空载一字不差**；bufinfo 里 4 个 3 457 024 B 的 `videobuf2_dma_contig` 缓冲明晃晃在册。
- 原因在 DT 里：**vinc*/isp/tdm 全都带 `iommus = <&sunxi-iommu …>`** —— "contiguous" 是 IOVA 连续，物理页散的，走页分配器 + IOMMU，根本不进全局 CMA。全相机栈对 CMA 的占用 ≈ 128 kB。
- ⇒ **"CMA 16 MB 不够放 4 个 1200p 帧" 是当时的算术推论，不是事实**。调大 CMA 对相机管线没有收益；只有未来出现"无 IOMMU 且要大块物理连续"的消费者才有意义。

**DMA-BUF 扇出（bufinfo 内核级证据）**
```
00151552 system  attached: 5440000.g2d + 3600000.npu   ← 同一个缓冲，两个设备消费者（G2D 写、NPU 读）
03457024 videobuf2_dma_contig  attached: 5440000.g2d   ← 捕获帧被 G2D 直接消费（缩放+色转）
```
这正是 C10/C11 要的"一帧同时喂 NPU + OpenCV"形态，已经跑通，不需要任何额外采集。

**`cma=` 机制（工具化 + 端到端验证后回滚）**
- `tools/cma_resize.sh <show|set <MB>|rollback>`：改 /boot/boot.cmd 的 bootargs 行 + mkimage 重生成 boot.scr，自带备份/回滚。kernel/dma/contiguous.c 的 `cma=` 覆盖逻辑成立。
- 实测：`set 64` → 重启 → `CmaTotal 65536 kB`、`cma=64M` 在 cmdline → vfr 回归 119.09 fps / 0 超时（无副作用）→ `rollback` → 重启 → `CmaTotal 16384 kB`（出厂态）。
- 坑：**板上重启会清 /tmp**，第一次 rollback 因此静默没跑成（脚本没了）；重推脚本再回滚成功。别把跨重启的脚本放 /tmp。

**证据等级**：确认（CmaFree 全程采样 + bufinfo + DT iommus + 16↔64 往返）。

---

## T-C15b 1080p "136 fps 边界" —— 机制定位 + 修复（本轮最大发现）

**旧口径（NEXT-TASKS 原文）**："130–135 全干净（131.6–131.8 fps），136 才崩（3 s 内 201 次 frame_lost + isp_reset）⇒ 机制未定位"。

**本轮实测把这个口径改写了**——它不是 136 硬边界，是**概率性风暴区**：
- 第九轮口径复测：1080p@120 干净（119.84 fps / 0 丢）；**请求 133–137 连打 5 发全崩**（含第九轮"干净"的 133），每发 ~533 次 frame_lost + isp_reset、0.5–1 fps。
- 变量隔离：换回 BA48 旧模块 → 照崩；去掉 ae_log → 照崩；**停掉 ar0234-3ad → 立刻全干净**（133 → 132.82 fps / 0 丢；**136 → 133.42 fps / 0 丢**）。
- ⇒ 风暴完全由 **3A 路径**引入；传感器本身在 vts=1096 下跑 133.4 fps 毫无问题。

**机制链（确认级，每一环都有实测）**
1. 3ad 用头两帧量周期 → 请求 ≥133 时测得 133/134 → `ispSetFpsRanage(id, ≥133)`。
2. ispSetFpsRanage 不只是"曝光上限"——它经 VIDIOC_VIN_SENSOR_SET_FPS 把 isp_fps 写回驱动，驱动**在流中重写 FLL**：vts → 1100（133）或 1096（≥134），vblank → 136/109 µs。
3. vblank < 500 µs 触发内核 0006 interlock 强制关 3DNR；而 3ad 旧规则（height≥1200 才关 3DNR）在 1080p 装的是 **3DNR-ON** 参数集 ⇒ 两边打架 → `sunxi_isp_reset` 风暴、0 帧、dmesg 不报因。装 no3dnr 能救 133（5/5 干净）但 136 仍 2/3 崩 —— 第二层：libisp 的 isp_fps ≥133 本身就致 0 帧（ctx 缓存/内部状态，候选级）。
4. 概率性的来源：头两帧的测量值落在 132 还是 133/134（上一模式的时序残留），以及 ctx 缓存。第九轮"134/135 干净、136 必崩"就是这个小概率游戏的一次抽样。

**修复（都在 ar0234-3ad，板上部署后回归）**
- `needs_3dnr_off()` 与内核 interlock 对齐：按驱动常数（pclk 90 MHz / hts 612 / FLL 开销 5 / 各模式 vts_min 1216·1096·736·620）估算 vblank，< 500 µs 就装 no3dnr（原 height≥1200 && fps>110 规则保留为子集）。
- 传给 ispSetFpsRanage 的 fps **钳到 120**：语义无损（传感器最长曝光 = fll-4 ≤ 1216 行 = 8.27 ms < 1/120 s，更高的上限永远不会绑定），却彻底避开 ≥133 的致 0 帧区。

**修复后回归（3ad 在位）**：1080p@133 ×5 = 131.8–133.0 fps **全 0 超时**；1080p@136 ×5 = **120.0 fps 全干净**；1200p120 = 120.02 / 0 丢；1080p120 = 119.96 / 0。
**行为变化（要写进文档的取舍）**：3A 在位时，请求 >120 fps 现在稳定交付 120 fps（而不是概率性 133）；要 133 fps 就别带 3A（libisp），实测 133.4 fps 稳定。用户的生产模式是 1200p120，不受影响。

**证据等级**：机制链 = 确认（停/开 3A 对照 + AE 日志 + vts/vblank dmesg + 修复前后概率对比）；第二层（isp_fps≥133 直接致 0 帧的 libisp 内部原因）= 候选。

---

## T-C15c vind_mclkpin 复发防护（0013 只读诊断，验收通过）

- 新补丁 `patches/0013-vin-pin-disable-imbalance-diagnostic.patch`（vin.c，内核根相对；userpatches 同步）：`vin_pin_disable()` 在 disable 前用 `regulator_is_enabled()`（纯查询）检查，发现已关的 regulator 就 `vin_err("... already disabled (enable-count imbalance: a disable without a partner enable)")`——不改任何状态机行为，把 0012 之前那三类签名如果在别的分支复发时的报错变回"一行说人话"。
- 板上部署（srcversion 456F967D77F423AE3373A95，与 userpatches 0001–0013 构建一致）后压测：
  - `stream_watchdog_test -n 40 -t 4`：40/40 starts 干净，4840 帧，failed probes 0，threw 0。
  - T3 式（1200p120 出流中 open/close(/dev/video4) ×30，get-fmt 路径）：三条签名（underflow / fail to disable regulator / already-disabled 诊断）dmesg 计数 **0**；诊断**误报 0**；`regulator_summary` 的 `5800800.vind-vind_mclkpin` enable count 回到 **0**。
- 镜像产物线同步：0013 已打进 build/vin-d3d-lbc 并刷新 `prebuilt/vin_v4l2.ko`（srcversion 4143BEC2FE8CD378A0B652D，md5 204ffce5…），packaging deb 随之重建。

**证据等级**：确认（压测日志 + regulator_summary 读数）。

---

## T-C15a isp01 破坏机制 —— 只读机制解释完成（结论：维持禁区）

不需要 enable 就能解释"enable 一个不注册设备的节点为什么毁掉第一路"：

1. **DT 几何（板上 DTB 反编译）**：
   `isp@5900000` reg = `<0x0 0x5900000 0x0 0x1300>`；`isp@58ffffc` reg = `<0x0 0x58ffffc 0x0 0x1304>` —— **起点低 4 字节、长度补 4 字节，两个窗口的结束地址同为 0x5901300**（isp02/isp03 依次再各低 4 字节）。四个节点是同一块 ISP602 寄存器窗口的错位别名。
2. **驱动映射（源码）**：`sunxi_isp.c:3427` `isp->base = of_iomap(np, 0)` —— 拿的是 DT reg 的**起点**。
3. **推论（机制）**：如果 isp01 被使能，它的驱动实例会以 base=0x58ffffc 映射并按同一套寄存器偏移读写——**每一次访问都落在 isp00 视角的 -4 字节处**：写自己的"寄存器 N"实际写的是 ISP0 的"寄存器 N-4"，而 ISP 块起始的几个字是配置/全局控制字 ⇒ 静默改写正在出流的 ISP0 的配置 ⇒ 0 帧且内核一条错都不报。这与第九轮的只读旁证（出流中 `0x5900000=0x5`、`0x58ffffc=0`）自洽。
4. **本轮新旁证（当前 0013-diag 模块下复测）**：出流中 `/dev/mem` 读 `0x5900000 = 0x00000005`（活）、`0x58ffffc = 0x00000000`（死）、`0x5901300 = 0x00000000`（窗口尾）——与机制一致。

**结论**：机制解释闭环，**维持"绝对不要开"**。当年驱动侧保护（0008 拒绝虚拟 isp 实例）已挡住注册路径，剩下的危险只有"enable DT 节点"本身，不碰即无风险。

---

## 本轮踩坑清单（补进 HANDOFF §3 的素材）

- **LOCALVERSION 陷阱**：手动 make 内核/模块必须 `LOCALVERSION=-sun60iw2`（build.sh 经 compilation.sh:498 以环境变量传入，.config 里是空串）；改完删 `include/config/kernel.release` 再编，否则 vermagic 用缓存 → insmod "Exec format error"。
- **板上重启清 /tmp**：跨重启的操作脚本放 `$HOME`，别放 /tmp（第一次 CMA 回滚因此静默失败）。
- **长任务必须 systemd-run**：本地掐 ssh 不会掐远端进程——第一轮 watchdog 测试因此与重跑的第二轮撞 /dev/video0（"Device or resource busy"×40 的真相）。
- **两路流互斥**：/dev/video0 同时只能一个程序出流；v4l2-ctl 连 --get-ctrl 都进不去（open 即 EBUSY），运行中读控件要么用进程内 Capture::get_control，要么读 AE 日志/流后再读。
- **`ispSetFpsRanage` 会改传感器帧率**（不只是曝光上限）：经 isp_fps 重写 FLL，是把流推进 interlock 冲突区的扳机。
- **libisp ctx 缓存**（/mnt/isp0_*_ctx_saved.bin）会压过参数文件里刚改的值——换参数集后 rm 掉它（P0-3 老规矩）。

## 收工板上快照（核对过）

```
uptime <5 min; uname 6.6.98-sun60iw2
DTB md5 8b1d837220d7f474ed262eed32ecb6f9（= userpatches 重建产物; .bak-c12 = d4ee5b68… 原板快照）
vin_v4l2 srcversion 456F967D77F423AE3373A95（= userpatches 0001–0013 构建；镜像线 prebuilt = 4143BEC2FE8CD378A0B652D）
CMA 16384 kB（出厂值，cma_resize.sh rollback 已验证）
/etc/ar0234.conf = 出厂样例（auto）；ar0234-3ad active；/etc/ar0234.conf.c15b 测试残留已清理
1200p120 ≥119 fps / 0 超时 / 0 丢帧（vfrrun c15b-final-1200p120: 120.02）
```
