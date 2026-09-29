# 单块 ISP602 能不能支撑两路 1920×1200@120 —— 机制、实测与结论

日期：2026-09-16（第五轮）
作者：ZCode（agent）
板子：Orange Pi Zero 3W / Allwinner A733（sun60iw2），内核 `6.6.98-sun60iw2`
模块：本轮开始是 **0008**（`srcversion 659707E6FD376A25E1E38CB`，md5 `9899a15c…`），
本轮结束是 **0009**（`srcversion 843BC1A03606D35EC062D57`，md5 `2715c25c40085869f0573a43b52528ae`）
DTB：`/boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb`，**本轮没有改动过任何 DT**

结论分级沿用 HANDOFF：**观察 / 候选 / 复现 / 确认**。

---

## 0. 一句话结论

- **目标 2**：在当前板子的实际配置（ISP 核心时钟 = **324 MHz**，由 DTB `csi_isp = <324000000>` 决定）
  下，**没有任何证据表明一块 ISP602 能吃下 2 × 1920×1200@120（553 Mpix/s）**；相反，
  单路的实测下界（162 MHz 就崩）外推出来的需求是 **≈600 MHz 量级，也就是当前时钟的约 2 倍**。
  但 **324 MHz 不是硅片上限**：时钟框架可以把 ISP 时钟设到 **≥648 MHz 并跑干净**，
  所以「两路」的障碍是**时钟配置 + TDM 通路模式**，不是 ISP602 的晶体管。
  置信度：**下界区间是实测（确认）；603 MHz 这个具体数字是外推（中等置信度）**。
- **目标 1**：ISP 的输入是**逐行实时流**（当前配置下连一个缓冲都没有）；`tdm_rx` 在
  `TDM_ONLINE` 模式下**只是直通**，在 `TDM_OFFLINE` 模式下才是「多路相机经 DDR 时分复用进同一块 ISP」
  的接收级。DDR 只出现在 D3D 参考帧、load image 和输出 DMA 上，**输入侧不缓冲**。
- **目标 3**：3.1 已修复并验证（新补丁 0009）；3.2 给出候选机制（暂不确认）；3.3 采集完成，
  并且发现那两个 Bandwidth 字段在本 SoC 上**结构性永远为 0**；3.4 复核见 §4。

---

## 1. 目标 1：ISP 输入端是缓冲队列还是实时行流

### 1.1 结论

**逐行实时流，行时间级别约束，输入侧完全没有帧缓冲。**

三条互相独立的证据：

**(a) 硬件把「行时间不够」做成了一个专门的中断。** `vin-isp/sunxi_isp.c`：

```c
	if (bsp_isp_get_irq_status(isp->id, HB_SHORT_PD)) {
		vin_err("isp%d hblank short, hblank need morn than 128 cycles!\n", isp->id);
		bsp_isp_clr_irq_status(isp->id, HB_SHORT_PD);
		sunxi_isp_reset(isp);
	}
```

ISP602 要求每个有效行的**消隐段不少于 128 个 ISP 时钟周期**。消隐长度由传感器决定，
所以 ISP 的时钟一旦低到「128 个周期都凑不齐」，硬件就直接报错并复位自己。
**这是行级、时钟周期级的实时约束**——如果输入是攒好的帧缓冲，这个约束根本不会存在。
本轮实测到了它：把 ISP 核心时钟单独降到 162 MHz（CSI 时钟仍是 324 MHz），
dmesg 立刻刷 `isp0 hblank short, hblank need morn than 128 cycles!` + `sunxi_isp_reset`。

**(b) 3DNR 的失败模式只能由行时间来解释。** `sunxi_isp.c` 头部注释（补丁 0006 写下并实测过）：

```c
/*
 * 3DNR (the ISP D3D block) keeps its reference frame in DDR and feeds it back
 * while the next frame is processed, so it cannot quite finish an active line
 * inside the sensor's line time. The per-line deficit has to be absorbed by the
 * vertical blanking, and when the blanking is too short the ISP raises
 * FRAME_LOST on every frame and resets itself until the stream never delivers.
 *
 * Measured on the AR0234 (6.8us line time, 1920 pixels per line, ISP clock
 * 324MHz) at 1920x1200:
 *   fll 1220   20 blank lines  120 fps ->  136us : FRAME_LOST every frame
 *   fll 1331  131 blank lines  110 fps ->  894us : clean
 *   fll 2445 1365 blank lines  1080p @ 60 -> 9.3ms : clean
 *   fll 1220  500 blank lines  720p @ 120 -> 3.4ms : clean
 * With 3DNR disabled the very same 1920x1200@120 runs at 0 lost frames over
 * minutes, so the rest of the ISP pipeline has plenty of margin and it is D3D
 * alone that misses the line time.
 */
```

「每行欠一点点、必须靠帧消隐吸收」= 输入是按行推进的实时数据；攒成帧缓冲的话
欠多少都无所谓。

**(c) ISP 在流启动时做的事是「开捕获」不是「挂缓冲」。** `sunxi_isp_logic_s_stream()`：

```c
	if (on) {
		bsp_isp_sram_boot_mode_ctrl(logic_isp->id, SRAM_NORMAL_MODE);
		bsp_isp_enable(logic_isp->id, on);
		bsp_isp_mode(logic_isp->id, logic_isp->work_mode);
		bsp_isp_top_capture_start(logic_isp->id);
	}
```

`bsp_isp_mode()` 写的是 `ISP_TOP_CFG0_REG.isp_mode` 这一位：

```c
enum isp_work_mode {
	ISP_ONLINE = 0,
	ISP_OFFLINE = 1,
};
```

`ISP_ONLINE = 0`，而 DT 里 `isp@5900000` 是 `work_mode = <0x0>`（见 §3.3 的读法）。
online 模式下 ISP 的输入直接来自 CSI/parser 的实时输出，没有 DDR 中转。

**(d) 失败后的处置也说明是实时流。** `FRAME_LOST` 的处理是「清 D3D_EN + 复位整条 ISP」，
没有「丢弃一帧缓冲再继续」这种选项：

```c
	if (bsp_isp_get_irq_status(isp->id, FRAME_LOST_PD)) {
		vin_err("isp%d frame lost!\n", isp->id);
		bsp_isp_clr_irq_status(isp->id, FRAME_LOST_PD);
		...
		sunxi_isp_reset(isp);
		__sunxi_isp_reset_v3(isp);
	}
```

### 1.2 `tdm_rx` 这一级是什么

**它是「CSI 输出 → ISP 输入」之间的接收/重放级，并且它有两种完全不同的工作模式**
（`vin-tdm/tdm200/tdm200_reg.h`：`TDM_ONLINE = 0`, `TDM_OFFLINE = 1`）。

**(a) ONLINE —— 直通，零缓冲，且只允许一路。**
`vin_tdm.c` 里缓冲区数量是这么决定的：

```c
		if (tdm->work_mode == TDM_ONLINE) {
			if (tdm_rx->ws.pkg_en || tdm_rx->ws.lbc_en)
				tdm_buf_num = 1;
			else
				tdm_buf_num = 0;      /* <-- 不分配任何 DDR 缓冲 */
		} else {
			if (tdm_rx->sensor_fps <= 30)
				tdm_buf_num = 2;
			else if (tdm_rx->sensor_fps <= 60)
				tdm_buf_num = 3;
			else if (tdm_rx->sensor_fps <= 120)
				tdm_buf_num = 4;
			else
				tdm_buf_num = TDM_BUFS_NUM;      /* = 6 */
		}
		if (tdm_buf_num) {
			...
			ret = tdm_rx_bufs_alloc(tdm_rx, size, tdm_buf_num);
			...
			csic_tdm_rx_set_buf_num(tdm->id, tdm_rx->id, tdm_buf_num - 1);
			for (i = 0; i < tdm_buf_num; i++)
				csic_tdm_rx_set_address(tdm->id, tdm_rx->id, (unsigned long)tdm_rx->buf[i].dma_addr);
		} else
			csic_tdm_rx_set_buf_num(tdm->id, tdm_rx->id, 0);
```

`tdm_rx_dev` 里的 `buf_size` / `buf_cnt` / `struct vin_mm ion_man[TDM_BUFS_NUM]` /
`struct tdm_buffer buf[TDM_BUFS_NUM]` 就是这批 **DDR 环形缓冲**。
online 且不压缩时 `tdm_buf_num = 0` ⇒ 一个缓冲都不分配 ⇒ 数据是实时穿过去的。

同一条路在 online 模式下**明确拒绝第二路**：

```c
	if (tdm->work_mode == TDM_ONLINE) {
		if (!list_empty(head)) {
			rcf = list_entry(head->next, struct rx_chn_fmt, list);
			vin_err("tdm%d working online mode, rx%d working, tdm can not be open again!",
				tdm->id, rcf->rx_dev->id);
			return -1;
		}
```

以及 `vin_tdm.c:634`：`if (tdm->work_mode == TDM_ONLINE && rx->id != 0)` 直接报错。
**本板实测：DTB 里 `tdm@5908000 work_mode = 0`（online），
`/sys/kernel/debug/mpp/vi` 两路都写 `tdm_rx0`。**

**(b) OFFLINE —— 每路一组 DDR 缓冲 + 重放给 ISP，这才是「多路进一块 ISP」。**

```c
		if (tdm_rx->ws.tx_func_en)
			csic_tdm_rx_tx_enable(tdm->id, tdm_rx->id);
		else
			csic_tdm_rx_tx_disable(tdm->id, tdm_rx->id);
```

`tx_func_en` 让 TDM 从自己的 DDR 缓冲里把行**重放**给 ISP，并且此时 ISP 的「行时序」
是 TDM 合成的，可以在驱动里配：

```c
	csic_tdm_set_hblank(tdm->id, TDM_TX_HBLANK_OFFLINE);   /* = 128 */
	csic_tdm_set_bblank_fe(tdm->id, TDM_TX_VBLANK/2);
	csic_tdm_set_bblank_be(tdm->id, TDM_TX_VBLANK/2);
```

`rx_chn_fmt` / `working_chn_fmt` / `tdm->ws.chn_total` / `bitmap_chn_use` 这一套就是
多路 rx 的**轮转调度表**：可以多个 `tdm_rxN` 同时挂上，各自往自己的 DDR 缓冲写，
TDM TX 再按时分轮流喂给同一块 ISP。

**所以：多路输入不是「排队」（排队意味着输入侧有队列），而是时分复用；
而时分复用的实现位置正是 `tdm_rx` 的 DDR 缓冲 + TDM TX 重放。
当前板子是 online 模式，连这个能力都没启用。**

### 1.3 ISP 在哪些地方用 DDR

| 位置 | 代码 | 说明 |
|---|---|---|
| D3D（3DNR）参考帧 pingpong | `isp->d3d_pingpong[0..3]`，`os_mem_alloc(&isp->pdev->dev, &isp->d3d_pingpong[i])` | 时域降噪要存上一帧参考帧，**在 DDR 里来回读写**；`isp600` 路径下 `d3d_pingpong[0] = hgt + height*(line_stride<<2)`（bayer 帧）、`[1]`/`[2]`/`[3]` 是 k0/k1/status，`cmp_ratio` 决定 LBC 压缩比 |
| ISP load image（寄存器表） | `isp->isp_load`，`memcpy(isp->isp_load.vir_addr, &isp->load_shadow[0], ISP_LOAD_DRAM_SIZE)` | 整套 ISP 寄存器 + 3A 统计表放在 DDR 里，硬件每帧/每次 `load_flag` 时从 DDR 取 |
| 输出 DMA（VIPP/scaler） | `bkuf => cnt: 4 size: 3457024`（实测） | ISP 输出经 VIPP 写进 4 个 NV12 缓冲，每块 3 457 024 B |
| TDM rx 缓冲（仅 OFFLINE） | `tdm_rx->buf[]` + `csic_tdm_rx_set_address()` | 多路相机的行数据中转，**当前 online 模式下 = 0 块** |

**结论：`输入侧不是 buffer、内部（D3D + load image）与输出侧才用 DDR` 这个说法可以被源码支持。**
源码里唯一的反例是 `TDM_OFFLINE`，而它在本板上没有被启用——这也正是「两路」必须先改的东西。

另外注意一个**必须写进结论的编译期事实**：ISP 的 DDR 带宽估算函数在本 SoC 上**根本没编进去**——

```c
#if !defined ISP_600
static void sunxi_isp_cal_bandwidth_memory(struct v4l2_subdev *sd, unsigned int on)
{
	...
#if IS_ENABLED(CONFIG_D3D)
	bandw += isp->mf.height * isp->mf.width * 5 / 8 * 2 * res->fps * 10 / D3D_RAW_LBC_MODE;
#endif
	...
}
#endif            /* <-- ISP600 下整段被排除 */
```

`platform/platform_cfg.h:64` 有 `#define ISP_600`，所以 `isp_bd_tatol` 恒为 0（见 §3.3）。

---

## 2. 目标 2：单块 ISP602 能否支撑 2 × 1920×1200@120

### 2.1 先把「时钟为什么是 324」查清（结论：这是 DT 的选择，不是硅片上限）

**(a) DTB 里写的就是 324。** 只读读取运行中的 DTB：

```
$ fdtget /boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb /soc/vind@5800800 csi_top
324000000
$ fdtget ... /soc/vind@5800800 csi_isp
324000000
```

注意**内核源码树里的 `sun60iw2p1.dtsi` 写的是 `csi_top = <600000000>; csi_isp = <540000000>;`**，
和板子上跑的 DTB 不一样——所以「为什么只有 324」的答案是
**厂家的板级 DTB 把这两个值配成了 324，驱动忠实地执行了它**，
既不是驱动 bug 也不是硬件限制。

**(b) 驱动在每次开流时把这三个时钟全部重写回 324。** 用临时模块的裸寄存器旋钮做单变量实验，
在**没有开流**时把分层设成「父 648 / csi M=2 → 324 / isp M=2 → 324」，
一开流立刻被驱动改回去（寄存器实测）：

```
### SETUP (no stream open): parent -> 648, csi_div -> 2, isp_div -> 2
### setup result: pll=0x01123500 csi=0x05000001 isp=0x04000001      <-- 分层生效
=== POINT isp_div=2 ===
  after stream-on: pll=0xfd323500 csi=0x85000000 isp=0x84000000     <-- 全被改回 324
```

`0x2002120` = PLL_VIDEO0_CTRL（`N` bits 8..15、`/3x` bits 16..18、`/4x` bits 20..22），
`0x2003840` = CSI_CLK_REG，`0x2003860` = ISP_CLK_REG（M bits 0..4、mux bits 24..26、gate bit 31）。
`0x01123500 → d4x=2`（父 648 MHz），`0xfd323500 → d4x=4`（父 324 MHz）；
`csi=0x05000001 → M=2`，`0x85000000 → M=1`（= 324）。

**(c) 时钟链路与可调范围（`bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c`）：**

```c
static struct ccu_nm pll_video0_clk = {
	.n		= _SUNXI_CCU_MULT_MIN_MAX(8, 8, 53, 105),
	.min_rate	= 1272000000,
	.max_rate	= 2520000000,          /* pll-video0 最高 2520 MHz */
	...
};
static SUNXI_CCU_M(pll_video0_4x_clk, "pll-video0-4x", "pll-video0", 0x0120, 20, 3,
		CLK_SET_RATE_PARENT);
static const char * const isp_parents[] = { "pll-video2-4x", "pll-peri0-480m",
		"pll-peri0-400m", "pll-peri0-600m", "pll-video0-4x", "pll-video1-4x" };
static SUNXI_CCU_M_WITH_MUX_GATE(isp_clk, "isp", isp_parents, 0x1860,
		0, 5, 24, 3, BIT(31),
		CLK_SET_RATE_PARENT | CLK_SET_RATE_NO_REPARENT | CLK_IGNORE_UNUSED);
```

`isp` 的 M 分频是 5 bit（/1…/32），但 `NO_REPARENT` 把它钉在 `pll-video0-4x` 上。
所以 **ISP 时钟上限 = pll-video0 上限 / 4 分频后的可达值**，天花板远高于 324。

**(d) 实测可达范围（`clk_round_rate` 只读扫描 + 真实写入）：**

```
ispclk:   req  10000000 -> round  10000000
ispclk:   req  324000000 -> round 324000000
ispclk:   req  540000000 -> round 546000000
ispclk:   req  630000000 -> round 630000000
ispclk:   req  790000000 -> round 784000000
```

真实写入（`clk_set_rate`）每一次都**精确命中**，并且是靠**重调 pll-video0** 做到的：

```
ispclk: set chain[0] req=600000000 isp 324000000 -> 600000000   [chain moved]
ispclk:     chain[1] 324000000 -> 600000000  <== MOVED     (pll-video0-4x)
ispclk:     chain[2] 1296000000 -> 1800000000  <== MOVED   (pll-video0)
ispclk: set chain[0] req=540000000 isp 600000000 -> 540000000
```

**并且 648 MHz 能跑真实流**：在 `/dev/video0` 1920×1200@120 运行中请求 648 MHz，
ISP 与 CSI 都到 648 MHz，结果干净（这一步没做到隔离，见 §2.4）：

```
  vfr: RESULT dev=/dev/video0 1920x1200 NV12 frames=1679 wall=14.000s fps=119.92 timeouts=0
  counts: frame_lost=0 hblank_short=0 resets=0
```

**⇒ 324 MHz 是配置值；ISP602 至少在 648 MHz 上工作正常。**

### 2.2 单路 1200p120 对 ISP 时钟的需求（实测下界）

方法：**只动 ISP 自己的 M 分频**（父 `pll-video0-4x` 与 CSI 时钟寄存器保持不动），
这样唯一变化的量就是 ISP 核心时钟。

| ISP 核心时钟 | cycle/pixel @276.5 Mpix/s | 结果 |
|---|---|---|
| 324 MHz | 1.172 | **干净**：119.02 fps，`vi0 lost_cnt` 增量 0，dmesg 0 条错误（多次复现） |
| 162 MHz | 0.586 | **崩溃**：`vi0 frame_cnt` 直接停在 0；另一种写法（框架 `clk_set_rate`）下刷 `isp0 hblank short, hblank need morn than 128 cycles!` + `sunxi_isp_reset` 风暴 |

实测片段（纯分频法，父与 CSI 未动，寄存器自证）：

```
=== POINT isp_div=1 -> isp core ~= 324 MHz ===
  after  write: pll=0xfd323500 csi=0x85000000 isp=0x84000000
  vi0 frames 1202 -> 555 -> 1510 (delta 955 in 8s = 119 fps)
  vi0 lost   0 -> 0 -> 0 (delta 0)
  counts: frame_lost=0 hblank_short=0 resets=0
  vfr: RESULT dev=/dev/video0 ... frames=1667 wall=14.006s fps=119.02 timeouts=0

=== POINT isp_div=2 -> isp core ~= 162 MHz ===
  after  write: pll=0xfd323500 csi=0x85000000 isp=0x84000001
  vi0 frames 1668 -> 0 -> 0 (delta 0 in 8s = 0 fps)
  vfr: RESULT dev=/dev/video0 ... frames=432 wall=14.648s fps=29.49 timeouts=11
```

**得到 ISP 核心需求的区间：每个像素需要 (0.586, 1.172] 个 ISP 时钟周期。**
（下界 <1.172 是「324 能跑」，上界 >0.586 是「162 不能跑」。）

### 2.3 外推到两路（**这是外推，不是实测**）

两路 = 553 Mpix/s。在固定时钟 f 下的吞吐上限是 `f / cyc_per_px`，所以要满足

```
f >= 553e6 * cyc_per_px ,  cyc_per_px ∈ (0.586, 1.172]
⇒ f ∈ (324 MHz, 648 MHz]
```

**当前时钟 324 MHz 正好落在这个区间的下端点**，也就是说：
即使 ISP602 的效率好到区间下界（0.586 cyc/px，这个值已经被 162 MHz 的失败排除了），
324 MHz 也**只是刚好**够——而实测已经证明 0.586 是不够的。
⇒ **在 as-shipped 的 324 MHz 下，两路不可行。**

配合「每行至少 128 个 ISP 周期消隐」这条硬件规则做一个更物理的模型：
设 ISP 处理一个有效行需要 `a·1920 + b` 个周期（a = 1 px/cycle 时的 a=1），
行周期 6.83 µs（1200+20 行 @120 fps），要求 `a·1920 + b + 128 <= 6.83µs · f`：

- f = 324：`a·1920 + b <= 2085` ⇒ 若 a=1，则 b ≤ 165（成立，所以 324 能跑）
- f = 162：`a·1920 + b <= 978` ⇒ 若 a=1，需要 1920+b ≤ 978，**不可能**（所以 162 必崩）
- 两路（每个行周期要处理 2 个有效行）：`2·(1920+128) + b <= 6.83µs · f`，取 b≈165
  ⇒ `f >= 4261 / 6.83µs ≈ 624 MHz`

**⇒ 外推需求 ≈ 600 MHz 量级（约当前值的 1.9 倍）。**
这个数字对 a（每周期几个像素）和 b（固定开销）敏感，而这两个参数**本轮没有测出来**，
所以标为 **外推 / 中等置信度**，不是结论。

### 2.4 方法学、误差来源与置信度（必须一起读）

**实测（确认）的部分：**
1. ISP 输入是逐行实时流，崩溃模式是 `hblank short`（≥128 ISP 周期的行级约束）——实测 + 源码。
2. 当前 ISP 核心时钟 = CSI 时钟 = 324 MHz，且**每次开流都由驱动重写为 324**——寄存器实测。
3. DTB 里 `csi_isp = csi_top = <324000000>`——DTB 只读实测。
4. 324 MHz 下单路 1200p120 干净（119–120 fps，`lost_cnt` 0，0 错误）——多次实测。
5. 162 MHz 下单路 1200p120 崩溃——两种独立方法各测到一次。
6. ISP 时钟可达 ≥648 MHz，并且 648 MHz 下真实流干净——实测。
7. `tdm@5908000 work_mode = 0`（online），输入侧零缓冲，online 明确拒绝第二路 rx——DTB 只读 + 源码。

**外推（不是结论）的部分：**
8. 「两路需要 ≈600 MHz」——由 (0.586, 1.172] 的区间 + 每行 128 周期模型推出，**未实测**。

**误差来源 / 已知混淆项（诚实列出）：**
- **ISP 时钟与 CSI 时钟绑在同一个父上。** `isp`(0x1860) 与 `csi`(0x1840) 都是
  `pll-video0-4x` 上的 M 分频，驱动开流时把两者都写成 M=1（= 324）。
  于是：
  - 想让 ISP 落在 162…324 之间（216、243 等）**必须动父分频**，
    而那会同时改 CSI 时钟；
  - **流运行中改 CSI 时钟会直接把流打死**（实测：父 324→648 + csi M=2 的组合下
    `vi0 lost_cnt` 从 0 涨到 1182），所以这些中间点**本轮没能干净地测到**，
    状态是 **未知（UNKNOWN）**，不是「失败」。
  - 「先设好父/分频再开流」也不行：驱动开流会全部改回 324（§2.1(b) 的实测）。
- **流运行中重调 PLL 本身会打出假故障。** 实测：486 MHz（需要重调 pll-video0）出现
  `hblank_short=1 resets=1`，而更高的 648 MHz（只需改 /4x 分频，pll-video0 不变）完全干净。
  从「时钟越低越容易 hblank short」的角度看，486 失败在物理上说不通，
  所以那一次是**重调的瞬态**，不能当成频率阈值。
- **「29.5 fps」这个特征值是另一回事，不是时钟造成的。** 本轮发现它其实是
  §3.1 那个 `vin_close()` 提前返回的 bug（`Runtime PM usage count underflow`）的签名。
  早先时钟阶梯里几个 29.5 fps 的点，**不能作为时钟证据使用**，已在结论里剔除。
- **单相机无法直接测 ISP 的吞吐上限。** 一块 AR0234 在 1200p 上最高只有 ~120 fps
  （276.5 Mpix/s），板上没有别的办法把 ISP 的输入像素率抬上去，
  所以「ISP 在 324 MHz 下最多能吞多少 Mpix/s」**本轮测不到**；
  只能用「降时钟 ⇒ 等效降低吞吐能力」来间接逼近，也就是上面这条区间。

### 2.5 结论与建议

| 问题 | 回答 |
|---|---|
| 现在这块板子（324 MHz）能跑两路 1920×1200@120 吗？ | **不能。** 单路的实测需求已经落在 (0.586, 1.172] cyc/px，两路需要 (324, 648] MHz，而当前就是 324。**置信度：高**（因为 162 MHz 已经实测失败，即下界 0.586 被排除，两路必然 >324） |
| 一块 ISP602 本身扛得住吗？ | **很可能扛得住，但没有直接证据。** 时钟框架能把它设到 ≥648 MHz 并跑干净；缺口是**配置和通路**，不是 ISP 本身。**置信度：中** |
| 两路到底需要多少 MHz？ | **外推 ≈600 MHz**（区间 (324, 648]）。**置信度：中，且明确是外推** |
| 需要改什么才能真跑两路？ | ① DTB `csi_isp` 抬到 ≥540–600 MHz（**改的是属性值，不是打开节点**，风险等级最低）；② `tdm@5908000 work_mode` 由 0(online) 改成 1(offline)，让多个 `tdm_rxN` 能同时挂上并各拿 2–6 块 DDR 缓冲；③ 第二颗模组的 sensor 槽位（顺序：**先插模组再开节点**，HANDOFF §3.26）。 |

### 2.6 要把 §2.3 的外推变成结论，需要补的实验（一页清单见 §5）

1. **只动 ISP 一个时钟、覆盖 162…648 MHz 的细阶梯。** 本轮做不到是因为 ISP/CSI 共用父时钟。
   最省事的合法做法：把 DTB 的 `csi_isp` 改成目标值（属性值改动，不动 status），
   这样驱动开流时会**主动**把 ISP 设成目标值，CSI 仍留在 `csi_top` 的值上，
   中间点（216/243/270/324/405/486/540）就都能测。
   **必须**：改前备份 DTB → `fdtget` 复核属性 → 确认没有任何「节点 okay 但模组不在」
   → 保留可 U-Boot 回退的备份（HANDOFF §3.26）。
2. **两路真机**：第二颗模组 + `vinc@5832000`/`sensor@5812020` + offline TDM。
   这是唯一能直接回答「553 Mpix/s 行不行」的实验。
3. **D3D 开/关做轻重负载两点标定**：固定时钟、固定分辨率，`d3d_min_vblank_us=0` 强制开 D3D
   （已知 1200p120 会每帧丢帧），用「需要多少帧消隐才干净」反推 D3D 每行多花了多少周期，
   从而估出 non-D3D 部分还剩多少余量。**这是单相机下最接近 throughput 上限的信息**，
   本轮没做（预算用在了时钟阶梯上）。
4. 若是关键决策：向全志要 ISP602 的数据手册/带宽表，直接看它标称的 cycle/pixel。

---

## 3. 目标 3：遗留问题清点

### 3.1 「打开第二个 video 节点又关掉会把第一路从 120 拖到 18 fps」

**状态：已修复（新补丁 `patches/0009`），行为验证通过。**

**复现（0008 模块，确定性）** —— 用 `tools/t17-openclose.c`（**只有 `open()`+`close()`，一个 ioctl 都不发**）
配 `tools/t17-repro-close.sh`：

```
=== E1: video0 alone, 30 s ===
  RESULT dev=/dev/video0 1920x1200 NV12 frames=3601 wall=30.008s fps=120.00 timeouts=0
=== E2: video0 30 s, open+close video4 once at t=8 s ===
  --- counters before touching video4 ---  vi0 frame => cnt: 906, lost_cnt: 0
  open/close /dev/video4 #1 ok
  --- counters right after ---             vi0 frame => cnt: 908, lost_cnt: 0
  --- counters 8 s later ---               vi0 frame => cnt: 908, lost_cnt: 0   <-- 冻住
  RESULT dev=/dev/video0 ... frames=907 wall=30.656s fps=29.59 timeouts=23
=== dmesg ===
  sensor_read error! sensor is not used!                              (x3)
  sunxi-vin-core 5831000.vinc: Runtime PM usage count underflow!      (1)
  ar0234_mipi is not used, video0 cannot be close!
```

**定位（源码 + 逐行）**：`vin-video/vin_video.c` 的 `vin_close()` 在
`!cap->pipe.sd[VIN_IND_SENSOR] || !...->entity.use_count` 时**提前 return -1**，
跳过了函数尾部那一整套拆卸：

```
vin_pipeline_call(vinc, close, &cap->pipe)
  -> __vin_pipeline_close()          (vin.c:1277)
     -> vin_pipeline_s_power(p, 0)
     -> vin_md_set_power(vind, 0)
     -> vin_video_core_s_power(CAPTURE, 0)
          = pm_runtime_put_sync(&vinc->pdev->dev)      <-- vin_video.c:6076
```

由于 video0/video4 共用同一条 `mipi0/csi0/tdm_rx0/isp0`，**出问题的却是还在出流的那一路**：
`5831000.vinc` 的 runtime PM 计数被打散，兄弟节点的帧停在 908、vfr 掉到 29.6 fps。

**修复（`patches/0009-vin-close-complete-rollback.patch`）**：
提前返回改成 `goto shared_teardown`（保留诊断信息与状态清理），
同时给 `vin_pipeline_call(vinc, close, ...)` 加条件：**从没做过 S_INPUT 的节点管线从未 prepare**
（`p->sd[VIN_IND_SENSOR] == NULL`），这时调用它只会触发 `vin.c:1287` 的 `WARN_ON`
（开机时 udev 的 `v4l_id` 每次都会踩到），所以跳过。

**验证（模块 0009，`srcversion 843BC1A03606D35EC062D57`）**：

```
=== E1: video0 alone, 30 s ===
  RESULT ... frames=3602 wall=30.008s fps=120.03 timeouts=0
=== E2: video0 30 s, open+close video4 once at t=8 s ===
  --- counters before ---   vi0 frame => cnt: 907
  --- right after ---       vi0 frame => cnt: 1028
  --- 8 s later ---         vi0 frame => cnt: 1982      <-- 一直正常推进
  RESULT ... frames=3572 wall=30.008s fps=119.04 timeouts=0
=== dmesg since marker ===
  (只有两条正常的 "3DNR forced off" INFO)
=== underflow 计数 ===  0
=== 全机 WARNING 计数 === 1   <-- 且那一条是既有的 sysfs_emit/orangepi-hardware，与 vin 无关
```

`apply.sh` 已接上 0009（备份 `apply.sh.bak-pre-0009`）。

### 3.2 `isp01` 机制之谜

**状态：已定性到候选机制（**候选**，未确认）。仍然不要打开这个节点。**

只读证据（运行中的 DTB + 内核 dtsi）：

| 节点 | 板子 DTB 的 status | reg（起始 + 长度） | 窗口 |
|---|---|---|---|
| `isp@5900000` (isp00) | **okay** | `0x5900000` + `0x1300` | **0x5900000‥0x5901300** |
| `isp@58ffffc` (isp01) | disabled | `0x58ffffc` + `0x1304` | **0x58ffffc‥0x5901300** |
| `isp@58ffff8` (isp02) | disabled | `0x58ffff8` + `0x1308` | **0x58ffff8‥0x5901300** |
| `isp@58ffff4` (isp03) | disabled | `0x58ffff4` + `0x130c` | **0x58ffff4‥0x5901300** |
| `isp@4` / `@5` / `@6` | disabled | **没有 reg** | — |

（`tdm@5908000` = `0x5908000` + 0x400，`scaler@5910000` = `0x5910000` + 0x400。）

**候选机制**：四个 ISP 节点是**同一个 0x1300 字节寄存器窗口的四个别名**——
它们的**结束地址都是 0x5901300**，而**起始地址每个差 4 字节**。
`of_iomap()` / `devm_clk_get` 系列拿的是**区间起始**，所以
`bsp_isp_map_xxx_addr(id, base)` 里的 `base` 对 isp01 而言比 isp00 **低 4 字节**，
于是 isp01 实例的每一次寄存器访问（`isp_regs[1].isp_top_cfg->bits.isp_enable = 1` 之类）
**都写进了 ISP0 的相邻寄存器**。写是成功的，所以内核一条错都不报，
而被写的寄存器属于正在工作的 ISP0 —— 这与「第一路静默 0 帧、内核全程无报错」完全吻合。

这一条比上一轮的残差（只是启动多一行 `vin_isp 58ffffc.isp: Adding to iommu group 0`）更具体，
但**还没有被直接观测到**，所以定级 **候选**。只读可验证的下一步：
出流中用 `tools/vinreg.c` 同时读 `0x5900000` 与 `0x58ffffc`，
确认后者不是 ISP 的同一个寄存器；或在 0009 的模块里加一个只读计数器统计 isp01 实例的
`bsp_isp_*` 调用次数。（**本轮没有做，以避免在没模组的情况下动 DT。**）

顺带确认：这些节点的 `iommus` 全是 `<&mmu_aw 2 0>`（同一个 IOMMU master），
`work_mode = <0xff>`（虚拟实例），与 0008 里那条拒绝注册的 ERR 一致。

### 3.3 `/sys/kernel/debug/mpp/vi` 的 CSI/ISP Bandwidth 读数

**状态：已采集；并确认这两个字段在本 SoC 上结构性永远为 0。**

两个场景都抓了（`analysis/t17/bw.out`，本报告 §附录 A 摘录）：

| 场景 | vi0 | vi4 | `CSI Bandwidth` | `CSI Bandwidth total` |
|---|---|---|---|---|
| 空载 | `frame cnt 2858, lost 0` | `cnt 2621, lost 0` | 0 | **不打印** |
| 单路 `/dev/video0` | **120.01 fps**，`lost_cnt 0`，`error_cnt 0` | 未出流 | 0 | **不打印** |
| **双路 video0+video4** | **119.04 fps**，`lost_cnt 0` | **119.08 fps**，`lost_cnt 0` | 0 | **不打印** |

`prs_in` 两场景都是 `x: 1920, y: 1200, hb: 477/478, hs: 1724`；
`bkuf` 都是 `cnt: 4 size: 3457024 rest: 3, work_mode: online`；
`internal` 都是 `avg 8 ms / max 8 ms / min 7 ms`。

**为什么永远是 0（三个互相独立的原因，都能在源码里指出）：**

1. `CSI Bandwidth` 那一行本身是整数除零截断：
   `vinc->bandwidth = buf_size * (1000/frame_internal/1000)`，
   而 `frame_internal` ≈ 8394（µs），`1000/8394 = 0` ⇒ 恒为 0（`vin_core.c:961`）。
2. `CSI Bandwidth total %dM, ISP Bandwidth total %dM` 只在
   `if (vinc->id == VIN_MAX_DEV - 1)` 时打印（`vin_core.c:965`），
   而 `platform/sun60iw2_vin_cfg.h:41` 是 `#define VIN_MAX_DEV 18`，
   实际上只存在 vi0/vi4/vi8 ⇒ **这一行永远不会出现**。
3. `isp_bd_tatol` 的唯一累加点在 `sunxi_isp_cal_bandwidth_memory()` 里，
   整个函数被 `#if !defined ISP_600` 包住（`sunxi_isp.c:748`），
   而本 SoC `platform_cfg.h:64` 就是 `#define ISP_600` ⇒ 编译期被排除，恒为 0。

**想真正拿到 DDR 带宽数字，只能：** ① 修这三个地方（截断改成 `1000*1000/frame_internal`、
`VIN_MAX_DEV-1` 改成实际最大 id、把 `sunxi_isp_cal_bandwidth_memory` 的 guard 放开）；
② 或者更可信：直接读 DDR 控制器/devfreq 的带宽计数器。**本轮只做只读采集，没有改这些代码。**

另外，`tdmbuf =>` 与 `ispbuf =>` 两行（会打印 `tdm_rx->buf_cnt/buf_size/cmp_ratio` 与
`d3d_pingpong[]` 尺寸）**本轮没出现**，因为它们被 `#if IS_ENABLED(CONFIG_VIN_LOG)` 包着，
本内核没开 `CONFIG_VIN_LOG`。想看这两个量就要重编内核开这个选项。

### 3.4 上一轮「未验证 / 存疑」条目复核

| 条目 | 本轮结论 |
|---|---|
| HANDOFF §3.27 第二节点 open/close 拖慢第一路（未修） | **已修复**（0009），`lost_cnt` 0、119 fps、0 underflow、0 新 WARN |
| HANDOFF §3.19 / T16c `isp01` 机制（未定位） | **候选机制已给出**（寄存器窗口 4 字节错位别名），仍未直接观测 |
| T15 `scaler get_selection error` 已修 | 复核：0009 开机 dmesg 里 0 次；**确认** |
| T15 `Runtime PM underflow` 已复现未修 | **复现确认 + 已修**（0009），修后 0 次 |
| T14 双路 `/dev/video0`+`/dev/video4` 各约 118 fps | **复现确认**：119.04 / 119.08 fps，双向 `lost_cnt` 0 |
| 「SoC 只有一块 ISP」 | **确认**：`mpp/vi` 打印 `isp 1`；`ISP_VERSION: ISP602_100`；四个 isp 节点 reg 结束地址同为 0x5901300 |
| 「ISP 核心时钟 324 MHz」 | **确认**，并补上根因：DTB `csi_isp = <324000000>`（源码树里的 dtsi 写的是 540000000，两者不同） |
| `frame_internal avg 8 ms` | 复核：单路/双路都是 8 ms，与 120 fps 一致 |
| HANDOFF §3.22 D3D/消隐互锁「出事救不回来」 | **确认**：本轮 162 MHz 把流打死后，回到 324 也救不回来，只能重启 |

---

## 4. 改动清单 / 回退方法 / 板子状态

### TL101（主机）文件

| 路径 | 说明 |
|---|---|
| `patches/0009-vin-close-complete-rollback.patch` | **新增补丁**（4 个 hunk，路径相对 `bsp/drivers/vin`） |
| `apply.sh` | 已接上 0009；备份 `apply.sh.bak-pre-0009` |
| `build/vin-d3d-lbc/vin-video/vin_video.c` | 已施加 0009（上一轮状态备份在 `/tmp/vin_video.c.pre0009`，tmpfs，**重启即失**；原始态可由 `patches/0001..0009` 重放得到） |
| `build/vin-d3d-lbc/out/vin_v4l2-0009.ko` | md5 `2715c25c40085869f0573a43b52528ae`，`srcversion 843BC1A03606D35EC062D57` |
| `tools/build_ispclk.sh`、`tools/t17-ispclk/` | 临时实验模块的构建（**不进补丁序列**） |
| `tools/t17-openclose.c`、`tools/t17-repro-close.sh` | 3.1 的复现工具 |
| `tools/t17-*.sh`、`analysis/t17/` | 本轮所有实验脚本与原始输出 |
| `analysis/t17/REPORT-0916-isp-capacity.md` | 本报告 |

### 板子（Orange Pi Zero 3W）

| 路径 | 说明 |
|---|---|
| `/lib/modules/6.6.98-sun60iw2/updates/vin_v4l2.ko` | **已换成 0009**（md5 `2715c25c…`） |
| `…/updates/vin_v4l2.ko.bak-0008` | 上一轮模块备份，md5 `9899a15c…`（= 回退目标） |
| `/tmp/*` | 各种实验工件，tmpfs，重启即清 |
| **DTB** | **本轮没有改动**（`/boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb`，仍是原样） |

**回退模块 0008：**
```
printf ' \n' | sudo -S -p '' cp -f /lib/modules/6.6.98-sun60iw2/updates/vin_v4l2.ko.bak-0008 \
    /lib/modules/6.6.98-sun60iw2/updates/vin_v4l2.ko
printf ' \n' | sudo -S -p '' depmod -a
printf ' \n' | sudo -S -p '' systemd-run --on-active=2 /bin/systemctl reboot
```
（这块板子上 `rmmod vin_v4l2` 不可能，换模块必须重启，HANDOFF §3.16。）

**板子当前状态（本轮收工时）**：健康。`/dev/video0` + `/dev/video4` 在位；
单路 1200p120 实测 120.03 fps / 0 超时；`ar0234-3ad` active；DTB 原样；
`panic_on_oops=1`、`RuntimeWatchdogSec=16s` 仍在（串口/看门狗兜底可用）。

### 遗留风险

1. **临时实验模块 `ispclk_scan.ko` 是裸操作 CCU 寄存器的**（`ioremap 0x2002000`，
   可写 `0x1840/0x1860` 的分频字段与 `pll-video0-4x` 的速率）。
   **它不应该长期留在板子上**：本轮最后一次使用后已 `rmmod`，且 `/tmp` 里的 .ko 重启即失。
   谁要再跑这些实验，务必先读 §2.4 的混淆项。
2. **`csi_isp`/`csi_top` 仍在 324 MHz**，两路能力没有被解锁。
3. **`tdm` 仍是 online**，多路 rx 会被驱动直接拒绝（`tdm can not be open again!`）。
4. **第二颗模组的前置条件没变**：`sensor@5812020` 必须保持 disabled，
   **先插模组再开节点**，否则 probe 阶段 panic + boot loop（HANDOFF §3.26）。
5. `ISP_CLK = CSI_CLK = 324 MHz` 时单路是**刚好够**，不是宽裕——
   如果把 ISP 时钟往下调（哪怕只是 162 MHz）或将来换更高像素率的传感器，会立刻破。

---

## 5. 接上第二颗模组后还需要补测什么（一页清单）

**A. 先把「两路能不能进去」的判据准备好（不需要模组就能做）**
1. [ ] DTB `vind@5800800` 的 `csi_isp` 抬到 `540000000`（先备份 DTB、`fdtget` 复核、
       `status` 一个都不动）。重启后确认 `cat /sys/kernel/debug/clk/isp/clk_rate` = 540000000，
       且单路 1200p120 仍然 `lost_cnt 0`。
2. [ ] 接着用同一个办法扫 `csi_isp` ∈ {405, 486, 540, 600}，每个点在**单路 1200p120 下**
       跑 60 s，记录 `mpp/vi` 的 `lost_cnt`/`error_cnt`/`frame_internal` 与 dmesg 的
       `frame lost` / `hblank short` 计数 → 得到「单路在哪些时钟下干净」的阶梯，
       把 §2.3 的区间收窄到两三个点以内。
3. [ ] 把 `tdm@5908000 work_mode` 由 0 改成 1（offline），**单路**再跑一次 1200p120 回归。
       offline 会分配 2–6 块 DDR 缓冲，这是两路的前提，也是新增的 DDR 带宽开销，
       必须在只有一路的时候就确认它不掉帧。
4. [ ] 用 `d3d_min_vblank_us` 做 D3D 开/关两点标定（§2.6 第 3 条），
       给出 non-D3D 部分剩余的每行余量估计。

**B. 插上模组之后（按 HANDOFF §3.26 的顺序：先插模组，再开节点）**
5. [ ] `tools/dt_second_cam.sh sensor2-on`，重启后确认没有 panic、`/dev/video8` 出现、
       `media-ctl -p` 的实体数与 CHECKLIST 里记录的 21+ 一致。
6. [ ] 第二路单独出流 60 s：确认 fps、`lost_cnt 0`（按 `analysis/t14/CHECKLIST-second-camera.md`）。
7. [ ] **两路同时出流 60 s**，记录：
       `vi0`/`vi4`（或 `vi8`）各自 fps、`lost_cnt`、`error_cnt`、`frame_internal`；
       dmesg 的 `frame lost` / `hblank short` / `reset` 计数；
       **以及 `bkuf` 的 `cnt/size`**（两路各 4×3.46 MB = 两路 27.6 MB 常驻）。
8. [ ] **两个传感器只能同尺寸**（HANDOFF §3.28），确认这个限制在两路真机下依然成立；
       若要不同分辨率，要走 `VIDIOC_S_SELECTION`（未验证）。
9. [ ] DDR 带宽：本次没能从 `mpp/vi` 拿到（字段结构性为 0，见 §3.3），
       所以要么先放开那三处代码，要么换 devfreq/DMC 计数器——
       两路 + offline TDM 的 DDR 压力比现在高得多，**这是最可能的新瓶颈**。
10. [ ] 4 小时长时间稳定性（HANDOFF T11）在两路负载下重跑。

**C. 判定口径（避免再次把估算当结论）**
- 「两路能跑」= 两路同时出流 ≥10 min，双侧 `lost_cnt` 0、`error_cnt` 0、
  dmesg 无 `frame lost` / `hblank short` / `reset`，且 `frame_internal` 的
  `max` 不出现周期性尖峰。
- 任何一条不满足就记「跑不动」，并立刻记录当时 `clk_rate`/`work_mode`/`d3d` 三个开关的值。

---

## 附录 A：本轮关键原始输出位置

| 内容 | 位置 |
|---|---|
| `mpp/vi` 单路/双路/空载 | `analysis/t17/bw.out` |
| ISP 时钟可达范围（只读 round_rate 扫描） | 本报告 §2.1(d) 里有逐条摘录（原始 dmesg 在下面的串口日志里） |
| 纯分频法 324/162（§2.2 的出处） | 本报告 §2.2 有逐条摘录 |
| 3.1 复现（0008）与验证（0009） | `analysis/t17/repro-close.out`（最后一次运行是 0009 的验证，`srcversion 843BC1A03606D35EC062D57`） |

⚠️ 本轮各次时钟阶梯的原始输出**打在板子 `/tmp` 里，而 `/tmp` 是 tmpfs、期间重启过多次**，
所以那些文件已经不在了（`analysis/t17/{pure-div,isp-div,iso2}.out` 只剩空壳）。
**结论所依赖的每一段原始文本都已逐字复制进本报告正文**（§2.1(d)、§2.2、§3.1、§3.3），
串口侧的全量日志留在 `analysis/t17/serial.log`。以后跑这类实验，输出要直接落到主机。
| 串口全量日志 | `analysis/t17/serial.log`（`systemd-run --unit=t17serial`） |
| 开机 dmesg | `analysis/dmesg-boot-t17.txt` |
