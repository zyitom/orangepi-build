# round9 报告（T1~T5）—— 2026-09-16

开局: uptime 2026-09-16T23:53:34+08:00, DTB d4ee5b68..., vin_v4l2 srcversion 7922F65188E60D338D42B73 (=0009+0010+0011)


## T1　ISP 硬复位是否真的可接 —— 只读验证（DT 未改、未改任何寄存器）

### 静态证据（内核源码，只读）

```
$ grep -n RST_BUS_VIDEO_IN bsp/include/dt-bindings/reset/sun60iw2-ccu.h
126:#define RST_BUS_VIDEO_IN	116
$ sed -n '121,127p' .../sun60iw2-ccu.h
#define RST_BUS_VIDEO_OUT0	111
...
#define RST_BUS_CSI		115
#define RST_BUS_VIDEO_IN	116        <-- 紧邻 CSI，同一个 "video in" 分组
$ grep -n 'RST_BUS_CSI\]\|RST_BUS_VIDEO_IN\]' bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c
	[RST_BUS_CSI]		= { 0x1844, BIT(16) },
	[RST_BUS_VIDEO_IN]	= { 0x1884, BIT(16) },
```
CCU 节点 = `soc@3000000/ccu@2002000`（reg `<0 0x2002000 0 0x2000>`，板上 /proc/device-tree 确认），
所以两个复位寄存器地址是 **0x2003844（CSI）/ 0x2003884（VIDEO_IN）**
（注意：审计报告里写的 `0x2001884` 是按 base=0x2000000 推的，**地址偏了 0x2000**，本批已用
「CSI 位随出流/停流翻转」这一现象把正确地址锁死，见下）。

**极性（源码级确认）**：`bsp/drivers/clk/sunxi-ng/ccu_reset.c` ——
`assert()` 做 `writel(reg & ~bit)`（**清位**）、`deassert()` 做 `writel(reg | bit)`（**置位**）。
即 **bit = 1 表示「已解除复位」，bit = 0 表示「处于复位中」**（与 reset 框架语义相反，文件里有注释）。

**DT 原文（厂商 dtsi，未改）**：`bsp/configs/linux-6.6/sun60iw2p1.dtsi`
```
		vind0: vind@5800800 {
			...
			resets = <&ccu RST_BUS_CSI>, <>;
			reset-names = "csi_ret", "isp_ret";
```
`isp_ret` 的 phandle 是**空的 `<>`**，编译进 DTB 后被丢弃 ⇒ 驱动 `vin.c:256` `devm_reset_control_get(dev,"isp_ret")`
失败 ⇒ `reset_control_deassert(NULL)` 恒返回 0（**空操作**）。与审计一致。

**全树没有第二个候选**：`grep -rn 0x1844|0x1864|0x1884 bsp/drivers --include=*.c` 除无线网卡无关命中外，
只有 `vin.c` 里 **FPGA 分支**（`#ifndef FPGA_VER` 的 `#else`，本配置**不编译**）硬编码
`writel(0x00010001, clk_base+0x1864) /* ISP RET GATING */`。
即：真实 SoC 上唯一未被引用的、名字/分组都指向 video-in 的复位就是 `RST_BUS_VIDEO_IN`。
（`0x1864` 在非 FPGA 的复位表里**根本不是复位寄存器**，读出来恒 0。）

### 板上实测（只读寄存器）

工具：`tools/vinreg.c` → 板上 `~/vinreg`（`/dev/mem` + mmap，root）。

```
== 空闲（无流出） ==
0x02003844 = 0x00000000      <- RST_BUS_CSI        : bit16=0 = 处于复位
0x02003864 = 0x00000000      <- （FPGA 提示位，非复位寄存器）
0x02003884 = 0x00010000      <- RST_BUS_VIDEO_IN   : bit16=1 = 已解除复位
0x05900000 = 0x00000000      <- ISP 寄存器块（空闲时被门控，读 0，符合 HANDOFF §3.24）

== 出流中（1200p120，vfr, 118.98 fps / 0 超时） ==
0x02003844 = 0x00010001      <- CSI 位**翻转了**：bit16 置 1（解除复位）+ bit0=1（gating）
0x02003864 = 0x00000000
0x02003884 = 0x00010000      <- VIDEO_IN 位**不动**
0x05900000 = 0x00000005      <- ISP 活着（与审计一致）

== 停流后 ==
0x02003844 = 0x00000000      <- CSI 又被 assert（驱动 vin_md_clk_disable 的行为）
0x02003884 = 0x00010000      <- VIDEO_IN 仍然不动
```

**这一段给出了一个可靠的正对照**：`0x2003844` 的 bit16 随「驱动 deassert/assert `csi_ret`」精确翻转
（源码里那句 `reset_control_deassert(vind->clk_reset[VIN_CSI_RET])` / `reset_control_assert(...)`），
⇒ ① 地址 0x2003844/0x2003884 是对的；② 极性（1=解除）是对的；③ 寄存器是活的、可观测的。

**读出的事实**：`RST_BUS_VIDEO_IN` 的 bit16 = **1（已解除复位）且全程不变**，
而同一时刻 `RST_BUS_CSI` 的 bit16 = 0（空闲时被驱动 assert）。
⇒ 与「ISP 由一条**从不被软件碰过**的复位线控制、POR 默认处于解除状态」完全吻合 —— **这是 ISP 能跑起来的前提**；
如果这条线真的连着 ISP 而且处于「复位中」，ISP 根本不会工作。
**结论：`RST_BUS_VIDEO_IN` 位是 1（=解除），与「它是 ISP 复位且当前被默认解除」自洽；**
但只靠读无法排除「这个位没接到任何东西、POR 就读 1」这一种可能 —— 见下「待判」。

### 待判（需要一次可回退的写：留到本批末尾 / 下一批）
决定性实验（**只写 CCU 那一个位，写完立刻读回并恢复原值**）：
```sh
# 出流中，原值 = 0x00010000
~/vinreg w 0x2003884 0x00000000    # bit16 清 0 => 按极性=「置于复位」
# 期望若真连着 ISP：vi0 frame cnt 立刻停住 / dmesg 出现 isp 相关 Err / vfr 0 帧
~/vinreg w 0x2003884 0x00010000    # 恢复（这才是回退动作，写回 1）
```
- 若停帧 ⇒ **确认**该位就是 ISP 复位（分辨率：确认）。
- 若毫无反应 ⇒ 该位不驱动 ISP ⇒ 接上去等于「接一条查不到效果的线」，**不要改 DT**。

### 如果要接：精确 DT 改法与回退（本批未执行）

只有一行要改（**内核树只读，改法写成补丁形式，本批没有打**）：
```diff
--- a/bsp/configs/linux-6.6/sun60iw2p1.dtsi   (源)
+++ b/bsp/configs/linux-6.6/sun60iw2p1.dtsi
@@ -vind0: vind@5800800
-			resets = <&ccu RST_BUS_CSI>, <>;
+			resets = <&ccu RST_BUS_CSI>, <&ccu RST_BUS_VIDEO_IN>;
 			reset-names = "csi_ret", "isp_ret";
```
注意 **DTB 是编译产物**（`arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi` 才是编进 DTB 的那份，
`bsp/configs/linux-6.6/` 是厂商参考副本；两处都要确认哪份在链上，**这一步没做，所以下一批开工前必须先确认**），
并且**必须重新编 dtb + 换 boot 分区 dtb**，因此**一定会换掉 `d4ee5b68…` 这个 DTB**，需要有 DTB 备份+串口+16 s 看门狗兜底。

**风险（评估）**
| 风险 | 说明 |
|---|---|
| probe 竞态 | 驱动在 `vin_md_clk_enable()` 里先 `deassert(isp_ret)` 再使能 isp clk；若这条线真的控制 ISP，则**顺序是对的**（先出复位再给时钟），风险低 |
| 关流时被 assert | `vin_md_clk_disable()` 会在关流时 `reset_control_assert(isp_ret)` ⇒ 之后**每次关流都会把 ISP 按进复位**，与 0009 的「兄弟节点关流」路径耦合；最坏情况是「ISP 出过错后更救不回来」 |
| 收益 | 中等：`sunxi_isp_reset` 类故障目前只能「停流重开」，接上后理论上能软复位 ISP 而不必停流 |
| 回退 | 恢复 DTB（`d4ee5b68…` 备份）+ 重启；不改内核源码即可回退 |

**本批结论**：静态链路（CCU 表 / DT 占位 / 极性 / 无第二候选）+ 只读实测（该位=1 且恒定，正对照锁死了地址与极性）
⇒ **「RST_BUS_VIDEO_IN 是候选且唯一候选」= 候选级**；**「它真连到 ISP602」= 未确认**，
差一次「写 0 看 ISP 是否停」的决定性实验。
（写实验安排在本 boot 末尾、T4 之后执行，因为它可能把 ISP 按进复位；结果见 §T1-C。）

---

## T4　H.265 @1920×1200@120 只出 6 帧 —— **未复现，且原「根因假设」被证伪**

板子上先用仓库当前 `userspace/` 重新编译了 `ar0234-rec`（原 `~/ar0234test/ar0234_rec` 是 Sep 15 的旧二进制，**不认识 `-c`**，
说明审计那一轮用的是另一个（更新的）构建；本次统一用 `~/ar0234test/userspace/build/ar0234-rec`）：

```
make -C ~/ar0234test/userspace -j8   → rc=0（仅 v4l2.cpp 那个已知的 -Wmaybe-uninitialized 误报）
```

### 一、逐变量矩阵（每次 `-b 20M -n 240`，零拷贝）

| # | 组合 | 结果 | dmesg 里 ISP 错误 |
|---|---|---|---|
| 1 | **1920×1200@120 h265**（失败组合，第 1 次） | **240 帧 / 2.03 s = 118.18 fps，7.2 ms/帧**，639217 B | **0** |
| 2 | 1920×1216@120 h265 | 240 帧 / 2.01 s = 119.15 fps，637988 B | 0 |
| 3 | 1920×1280@120 h265 | 240 帧 / 2.02 s = 118.67 fps，697915 B | 0 |
| 4 | 1920×1200@119 h265 | 240 帧 / 2.02 s = 118.67 fps | 0 |
| 5 | 1920×1200@118 h265 | 240 帧 / 2.06 s = 116.70 fps | 0 |
| 6 | 1920×1200@60 h265 | 240 帧 / 4.01 s = 59.89 fps | 0 |
| 7 | 1920×1200@120 h264（对照） | 240 帧 / 2.11 s = 113.59 fps，8.7 ms/帧 | 0 |
| 8 | **1920×1200@120 h265**（精确重复第 2 次） | 240 帧 / 2.03 s = **118.19 fps** | 0 |

再补 4 次精确重复（专门数 ISP 错误条数）：

```
  run 1: done: 240 frames in 2.02s (118.66 fps), 7.0 ms/frame   isp-errors: 0
  run 2: done: 240 frames in 2.04s (117.67 fps), 7.0 ms/frame   isp-errors: 2   <-- 关键
  run 3: done: 240 frames in 2.02s (118.66 fps), 7.0 ms/frame   isp-errors: 0
  run 4: done: 240 frames in 2.03s (118.20 fps), 7.0 ms/frame   isp-errors: 0
```

**H.265@1920×1200@120 共 6 次运行（含 6 次精确重复）：6/6 全速通过，117.67–118.66 fps，最快 7.0 ms/帧。**
编码吞吐上没有 `isp0 configuration error` 的痕迹。

### 二、run 2 是本次最关键的一条：**ISP 错误与 6 帧失败没有因果关系**

run 2 里 ISP 打了 2 条 `configuration error`/`height error`，**同一次运行照样编出 240/240 帧、117.67 fps**。
⇒ `CFG_ERROR_PD`/`HEIGHT_ERROR` 是 ISP 自己的输入时序自检中断（源码 `sunxi_isp.c:2864/2903`，
命中后只做 `sunxi_isp_reset()` 自恢复），**它不会让 1200p120 的 H.265 停住**。

再回看审计那一轮的**原始日志**（`analysis/hardware-audit/s13.out`，只读引用）：
```
  h265 1920x1200@120  size=124202  dmesg-new=[396.281069] isp0 configuration error|[396.281105] isp0 height error
  h264 1920x1200@120  size=2499329 dmesg-new=[396.281069] isp0 configuration error|[396.281105] isp0 height error
```
**同样两个时间戳被同时记在 h264 与 h265 两行**，而那两次 h264 都是 120/120 帧、9.4/8.9 ms/帧 正常完成的。
也就是说审计那张表里「h265 失败 ↔ 这两条 ISP 错误」的归属本身不可靠（`dmesg-new` 的行归属有偏移），
**而 h264 在同分辨率同帧率下两次都过** —— 这一条当时没有被注意到。

### 三、高度对齐变量本身也无法成立（新证据）

```
$ v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=<H>,pixelformat=NV12 ; --get-fmt-video
  request height=1200  ->  1920/1200     rc=0
  request height=1216  ->  1920/1200     rc=0     <-- 静默被改回 1200（V4L2 允许驱动返回最接近的尺寸）
  request height=1280  ->  1920/1200     rc=0     <-- 同上
  request height=1080  ->  1920/1080     rc=0
```
并且第 2/3 行运行时 `vi0 prs_in` 始终是 `x:1920 y:1200`。
⇒ **1216/1280 这两个「对齐 64」的高度在 capture 节点上根本无法表达**（跑 `-h 1216` 时实际采的还是 1200）。
再叠加 `1080`（同样不是 64 的倍数）的 H.265 一直正常 ⇒ **「高度对齐 64」不是原因**。

**顺带发现（候选，属 userspace 缺陷）**：`userspace/src/v4l2.cpp:136-138` 把**驱动返回**的尺寸存进 `size_`，
但 `ar0234-rec` 用**请求的** `cfg.size` 去配编码器 ⇒ `-h 1216` 会「按 1920×1216 编 1920×1200 的缓冲」
（本次没崩，但语义是错的）。最小改法：`ar0234-rec` 用 `capture.size()` 配编码器，或在尺寸被改动时直接报错退出。
**本批未改**（不在 T4 范围内，且属于新发现）。

### 四、结论与可绕方案

| 假设 | 判定 | 证据 |
|---|---|---|
| 高度 1200 非 64 倍数 | **不是原因** | 1216/1280 无法表达；1080（也非 64 倍数）正常；1200 现在 6/6 通过 |
| 120 fps 帧率 | **不是原因** | 120/119/118/60 全部正常出帧 |
| 两者叠加 | **不成立** | 同上 |
| 「ISP configuration error 打死 1200p120 H.265」 | **证伪** | run 2：有 2 条 ISP 错误而 240/240 帧 117.67 fps |
| 最可能解释（**候选**） | 那一次是 **P0-2/T6 的「开流只出 4~6 帧就死」（5~10%）** | 签名完全一致（`6 frames in 2.08s` = 出几帧后停 + 2 s wall）；本轮 8+4 次没再抽到；该故障与 codec 无关，用户态看门狗已兜住 |

**结论等级：未复现（含反证）。**
**可绕方案：不需要绕。** 当前栈上 `1920×1200@120 + H.265 零拷贝 = 118.2 fps / 7.0 ms 每帧`，
比同参数 H.264（113.6 fps / 8.7 ms）**还快**，且码率只有约 1/3（639 KB vs 962 KB / 240 帧）。
⇒ `README.md` 里那条「1200p120 请用 H.264」的警告应当撤回/改写（本批按纪律只写进本报告，
README/HANDOFF 的订正留给下一批一次性做，避免和 T2 的改动混在一起）。

### T1-C　决定性写实验（只写 CCU 一个位，同一轮内立刻恢复原值）—— **结果：确认，该位确实控制 ISP / video-in 块**

脚本只做三件事：出流中把 `0x2003884` 写成 `0x00000000`（按极性 = 置于复位）→ 观察 → 写回 `0x00010000`。
原始值已先读出（`0x00010000`），所以写回就是精确回退，且**没有触碰任何别的位**。

```
== t0（出流前）==
0x02003884 = 0x00010000            <- RST_BUS_VIDEO_IN，解除状态
0x02003844 = 0x00000000            <- RST_BUS_CSI，空闲时被驱动 assert
== 开流 20 s，t+4 s 健康 ==
prs_in => x: 1920, y: 1200, hb: 478, hs: 1724
frame => cnt: 424, lost_cnt: 0, error_cnt: 0

== 写 0x2003884 <- 0x00000000（assert）==
0x02003884 <- 0x00000000 (readback 0x00000000)      <- 寄存器可写，位真的落下去
-- 2 s 后 --
frame => cnt: 427, lost_cnt: 0, error_cnt: 0        <- **只走了 3 帧**（2 s 应该 ~240 帧）=> 流停了
0x02003884 = 0x00000000
0x02003844 = 0x00010001                             <- **CSI 位没被动过**（仍是 0x0001_0001）=> 隔离干净
0x05900000 = 0x00000000                             <- ISP 寄存器块从 0x5 变成 **0**

== 写 0x2003884 <- 0x00010000（deassert，恢复）==
-- 4 s 后 --
frame => cnt: 912, lost_cnt: 0, error_cnt: 0        <- **485 帧/约 4 s => 流自己恢复了**（没有停流重开）
0x02003884 = 0x00010000
0x05900000 = 0x00000005                             <- ISP 回来了，与出流中的已知值一致

== 整段 vfr ==
RESULT dev=/dev/video0 1920x1200 NV12 frames=2141 wall=20.002s fps=107.04 timeouts=2
GAPS n=2140 median=0.008330s (120.05 fps) min=0.003010 max=2.171157 >1.5x=1 >3x=1
        （max gap 2.17 s 就是被按住复位的那 2 秒；除此之外中位帧间隔 8.330 ms = 120.05 fps）
dmesg：这一段里 **没有任何** isp0 报错/复位日志（只有开流时的 sensor/ISP INFO 行）
```

**判读**
1. `0x1884` 的 bit16 **可写且写进去有效**（readback 直接跟随），排除「该位是只读常量」。
2. 清掉它 → **ISP 寄存器块读 0 + VIN 帧计数停走 + 帧间隔出现 2.17 s 空档**；
   置回它 → **ISP 寄存器块复现 0x5 + 帧计数恢复 120 fps，且不需要停流重开**。
3. 同一个窗口里 `0x2003844`（CSI 复位）**保持在 streaming 状态不变** ⇒ 效果来自 `0x1884` 这一位本身，
   不是「顺手把 CSI 也复位了」。
4. ⇒ **`RST_BUS_VIDEO_IN`（CCU reset id 116，reg 0x1884 bit16）确实驱动 ISP / video-in 块，
   而且是可逆的**。这一条把 T1 从「候选」升到 **确认（该位可控、作用于 ISP 块）**。

**一处必须写清的保留**：读 `0x5900000` 得 0 这一点本身**不能单独区分**「ISP 被复位」与「ISP 时钟被门控」
（空闲时时钟被门控，读出来同样是 0，见 HANDOFF §3.24）。**连通性的证据是行为，不是这个 0**：
位被清掉的那 2 s 里 VIN 帧计数只走 3 帧、帧间隔出现 2.17 s 空档，置回后立刻恢复 —— 只有「这一位真的
作用于该子系统」才能解释。至于它内部是「复位」还是「复位+门控」，本实验分辨不了，也不影响结论。
**推测（候选）**：它可能同时重置 CSI/VIPP 这一段视频输入链（帧计数都停了），不止 ISP 一个块；
这也解释了为什么厂商把它命名成笼统的 `VIDEO_IN`。

### T1 结论与建议（仍然不改 DT）

| 项 | 等级 | 说明 |
|---|---|---|
| `RST_BUS_VIDEO_IN` 存在、未被任何驱动/DT 引用 | **确认** | CCU 复位表 `{0x1884, BIT(16)}`；全树零引用 |
| 该位**确实驱动 ISP / video-in 块**，且可逆 | **确认** | 上面的 assert/deassert 实验（帧计数停走/恢复 + `0x5900000` 0↔5） |
| DT 里 `isp_ret` 是空占位，所有 ISP reset 是空操作 | **确认** | `resets = <&ccu RST_BUS_CSI>, <>` + `reset_control_deassert(NULL)` |
| 「接上它就能在 ISP 出错后软复位救回」 | **候选** | 现在只证明「能复位 ISP」；**能不能救回 `frame lost` 风暴还没有测**（本批没做） |

**如果要接（下一批做，本批一律不改 DT）**：DT 改一行（见上「精确 DT 改法」），风险表里新增一条**硬约束**：

> ⚠️ **接 `isp_ret` 必须和 T2 的电源引用计数修复一起上，而且要单独验证。**
> 因为 `vin_md_clk_disable()`（`vin.c:543-566`）在关流路径里会 `reset_control_assert(clk_reset[VIN_ISP_RET])`——
> 现在这行是空操作，接上之后**就变成真的把 ISP 按进复位**。而 T2/T3 已经证明「关了不该关的东西」
> 这条路径上存在引用计数缺口（一个多出来的 close 就会走到这里）。两者叠加的最坏结果：
> 第二个节点被关掉时，正在出流的第一路被**硬件复位**（本实验已经演示了这个现象）。
> 所以顺序必须是：**先 T2 的引用计数修复 → 回归 T3 的「兄弟节点开关」场景 → 再单独一轮接 `isp_ret`**。

---

## T2　电源引用计数缺口（underflow + `vind_mclkpin` regulator WARN）—— **已修复并验收**

### 改动：`patches/0012-vin-pipeline-close-refcount.patch`（新增，已接进 `apply.sh`）

只改 `vin.c` 的 `__vin_pipeline_close()`，**10 行逻辑、不碰 DT、不改 ABI**：把 open 侧做过的那三个操作
一起放到同一个引用计数判断下面（`vind->use_count`），close 侧就只回退「真的 open 过」的东西：

```diff
 	vind = entity_to_vin_mdev(&sd->entity);
 	if (vind) {
+		/* ...注释见补丁... */
+		if (vind->use_count) {
 #ifdef CSIC_SDRAM_DFS
 			csic_chfreq_disable(vind->id);
 			vin_chfreq_clk_set(0);
 #endif
 			vin_md_set_power(vind, 0);
 			vin_pin_disable(vind);
 			if (p->sd[VIN_IND_CAPTURE] && p->sd[VIN_IND_CAPTURE]->entity.graph_obj.mdev)
 				/* power off the ppu */
 				vin_video_core_s_power(p->sd[VIN_IND_CAPTURE], 0);
+		}
 	}
```

**为什么是这三行**：`__vin_pipeline_open()` 做三件事 —— `vin_video_core_s_power(capture,1)`
（`pm_runtime_get_sync`）、`vin_md_set_power(vind,1)`（`use_count++`）、`vin_pin_enable(vind)`
（`regulator_enable` ×3）；`__vin_pipeline_close()` 对称地做三件相反的事，但**只有中间那件带引用计数**
（`use_count == 0` 时 `vin_md_set_power` 直接 return），另外两件无条件执行。而
`vin_open()` **不碰管线**（管线由第一次 `S_FMT`/`S_INPUT` 打开），所以「只 open 不出流就 close」
到这条路径时 `use_count == 0`：`vin_md_set_power(0)` 安全地空转，**正好把另外两件暴露出来** ——
一次多出来的 `regulator_disable`（regulator core 打 WARN 并返回 -EIO）+ 一次多出来的
`pm_runtime_put_sync`（underflow）。三条报错，一个根因，与上一轮的定位完全一致。

生成方式（**内核树全程只读**）：把 `$K/bsp/drivers/vin/vin.c` 复制到 `/tmp` 改，再
`diff -u --label a/vin.c --label b/vin.c` 出补丁；同一处替换用脚本同步到外部编译树
`build/vin-d3d-lbc/vin.c`。补丁对内核树 `patch -p1 --dry-run` 干净通过（`checking file vin.c`，无失败）。
备份：`build/vin-d3d-lbc/vin.c` → `/tmp/vin.c.bak-pre-0012`，`apply.sh` → `/tmp/apply.sh.bak-pre-0012`。
`bash -n apply.sh` rc=0（本机与 TL101 各验一次）。

### 模块

```
tools/build_vin.sh 0012
  -> build/vin-d3d-lbc/out/vin_v4l2-0012.ko
     md5 375f6f4f5c15f72b2d2323f32e7af087
     srcversion BA48201D23BA4260923B53E          (= 0009+0010+0011+0012)
板上：/lib/modules/6.6.98-sun60iw2/updates/vin_v4l2.ko（旧模块备份 .bak-pre-0012，md5 241c8631…）
      depmod -a + 重启（换 vin 模块必须重启，HANDOFF §3.16）
重启后：DTB d4ee5b68…（未变）、/dev/video0、/dev/video4、/dev/g2d 均在
开机 dmesg 落盘 analysis/dmesg-boot-0012.txt（1622 行）：三条签名 **0 条**
```

### 验收（同一脚本、同一台板、同一 boot 内前后对比）

触发序列用的是 `tools/t17-openclose.c`（**只有 open()+close()，一个 ioctl 都不发**）。

| 序列 | **打补丁前**（srcversion 7922F651…） | **打补丁后**（BA48201D…） |
|---|---|---|
| A：`open+close(/dev/video0)` ×5（出过流之后） | **30 行**（5×underflow + 5×`vin_pin_disable ... fail!` + 5 组 `_regulator_disable` WARNING/栈） | **0** |
| B：`v4l2-ctl --get-fmt-video /dev/video0` ×5 | **30 行** | **0** |
| 出流中的兄弟节点：video0 出流 12 s，中途 `open+close(/dev/video4)` | 0 行（0009 已兜住这一条） | 0 行，video0 **119.98 fps / 0 超时** |
| 回归：1920×1200@120 | 119.97 fps / 0 超时 | **119.88–119.91 fps / 0 超时 ×5 次**（另一次 117.90，0 超时） |

打补丁前的原始输出（节选）：
```
== sequence A: open()+close() /dev/video0, no ioctl at all, x5 ==
   signature lines: 30
      5 sunxi-vin-core 5830000.vinc: Runtime PM usage count underflow!
      5 sunxi:vin:[ERR]: vin_pin_disable: disable vind_mclkpin error, fail to disable regulator!
```
打补丁后：`signature lines: 0`（A/B/C 三个序列全部 0），并且
```
== mclk regulator state BEFORE any streaming ==
       5800800.vind-vind_mclkpin   0   ...      <- enable count = 0
== 5 x (stream 6 s + close) ==  ... 全部 0 超时
== mclk regulator state AFTER 5 stream+close cycles ==
       5800800.vind-vind_mclkpin   0   ...      <- 仍然是 0
== whole-boot counters ==
  underflow+pin_disable lines: 0
  _regulator_disable lines: 0
  WARNING（除已知 sysfs_emit）: 无
```
最后这两条是**防「改过头」**的关键：`regulator_summary` 的 enable count 在 5 轮「出流+关流」之后
**回到 0**，说明合法关流路径**没有**因为新判断被跳过（否则会一次都不 disable、计数递增）。
换句话说：**该跳的跳了（use_count==0 的多余 close），该做的还在做（真出流之后的 close）。**

### 一条必须如实写的观察

补丁后第一次跑该脚本时，**回归那一段抽到了「坏启动」**：
```
== regression: 1920x1200@120, 15 s ==
  [14.1s] no frame for >1s (frames=3)
RESULT ... frames=3 wall=15.052s fps=0.20 timeouts=15
```
签名与 P0-2/T6 记录的坏启动一致（**出几帧后停住、内核一条错不报、DQBUF 全部超时**），
并且**紧接着的 8 次出流全部 119.88–120.10 fps / 0 超时**（含再做 5 次「出流+close」循环、
12 s 兄弟节点场景），完全符合「下一次一定正常」。这一条**不归因于 0012**：它是第七轮就量化过的
5–10% 固有故障（40 次压测 3/40），且本轮打补丁前的对照跑里没有抽到。**等级：观察**（不是"已确认无关"，
但它落在既有故障的签名内，且补丁只删多余操作、不引入新操作）。T3 的 30 次压测会再给一个独立的样本。

### 结论

**已修复（确认级）**：验收指标全部达成 —— 触发序列 A/B 的三条签名 **30 → 0**；
合法路径回归 1200p120 **0 丢帧**；`regulator_summary` 计数证明关流路径未被误跳过。

---

## T5　G2D 位精确路径固化成可复用件 —— **已交付并验收（0 差异）**

### 新增（`userspace/`）

| 文件 | 内容 |
|---|---|
| `userspace/include/ar0234/g2d.hpp` | `ar0234::G2d` / `ar0234::DmaBuffer` / `ar0234::Rect` / `ar0234::DiffStat` + 每个常量背后的实测依据 |
| `userspace/src/g2d.cpp` | 实现：两次 Y8 blit 的位精确搬运、`G2D_CMD_FILLRECT_H` 标注、dma-heap 分配与 dma-buf sync |
| `userspace/apps/ar0234-g2d-selftest.cpp` | 示例 + 自检（两个对照 + 4 个相位），退出码 0/1/2/3 |
| `userspace/Makefile` | 新增 `build/ar0234-g2d-selftest` 目标；`-I../hwtest-include`（`sunxi-g2d.h` 是 UAPI，内核不导出，仓库里的副本才是构建基准） |

**API（刻意做窄）**
```cpp
ar0234::G2d g2d;                                  // 打开 /dev/g2d，需要 video 组
ar0234::DmaBuffer src{bytes}, dst{bytes};          // /dev/dma_heap/system
g2d.move_nv12(src, dst, w, h);                     // 位精确 NV12 搬运（两次 Y8 blit）
g2d.move_nv12(v4l2_dmabuf_fd, dst.fd(), w, h);     // 零拷贝源：直接吃 ISP 的 dma-buf
g2d.fill_rect_nv12(dst, w, h, {x,y,w,h}, Y, U, V); // 在 NV12 上画矩形（颜色直写，无 RGB 矩阵）
```
**故意没有暴露单次 4:2:0 blit** —— 它在 Mixer 里过重采样滤波器、Y 还会被钳到 ≥16，
不可能位精确（`analysis/g2d/REPORT.md` §2.9）；API 层面给不出这个口子，调用方就不会踩。

### 板上验收（`orangepi` 用户、**不用 sudo**，1920×1200 NV12）

```
$ make -C userspace -j8        # 板上本地编译，0 warning（-Wall -Wextra）
$ ./userspace/build/ar0234-g2d-selftest
  control B: CPU memcpy through the buffers [EXACT] ok
  control A: constant plane through G2D     [EXACT] ok
== phase 1: 随机满量程数据（最坏情况）==
  luma (Y)                       [EXACT]   0/2304000 differing
  chroma (UV)                    [EXACT]   0/1152000 differing
== phase 2: 原位搬运（src == dst）==
  unchanged after in-place move  [EXACT]   0/3456000 differing
== phase 4: 矩形标注 ==
  rect (100,64,256x64)  luma inside 16384/16384 = colour, outside 0 changed
                        chroma inside 4096/4096 = colour, outside 0 changed
  rect (0,0,64x64)      同上（角点）         rect (1856,1136,64x64) 同上（贴边裁剪）
result: OK   rc=0

$ ./userspace/build/ar0234-g2d-selftest --capture      # 真帧
  real frame luma (Y)            [EXACT]   0/2304000 differing
  real frame chroma (UV)         [EXACT]   0/1152000 differing
  ISP dma-buf -> heap, luma      [EXACT]   0/2304000 differing     <-- 零拷贝源，直接吃 ISP buffer
  ISP dma-buf -> heap, chroma    [EXACT]   0/1152000 differing
  rect (32,32,128x128)           luma 16384/16384 + outside 0；chroma 4096/4096 + outside 0
result: OK   rc=0
```
**验收指标「真实 ISP 帧搬运 0 差异」达成**（0/3456000，且零拷贝路径同样为 0）。

补充说明两点，免得把话说过头：
- **phase 2「原位」的语义**：`move_nv12(src, src)` 按定义是「把一帧拷到自己身上 = 不变」，
  所以实现里直接 return，不自找硬件读写同一片行。自检验的是「这个契约成立」（3456000 字节一字未动）。
  真正的「原位寻址」体现在 phase 1/3：**同一块 NV12 缓冲被当成 width×(h+h/2) 的 Y8 图**，
  Y 用 `clip_rect.y=0/h`、UV 用 `y=h/h/2` **在同一缓冲内**取两份矩形 —— 这才是位精确的机制本身。
- phase 4 的「outside 0 changed」是**逐字节**扫全平面（不是抽样），矩形用整数折半给色度面，
  贴边 `(1856,1136,64x64)` 也精确落在 `(928,568,32,32)` 的色度矩形上。

### 顺带定位一个**仓库里一直存在的静默 bug**（新，未修，留给下一批）

`DMA_BUF_IOCTL_SYNC` 的 flag 编码**不是 mainline 那套**：本内核 `include/uapi/linux/dma-buf.h` 是旧布局
（`READ=1<<0, WRITE=2<<0, RW=3, START=0<<2, END=1<<2`，`VALID_FLAGS_MASK = 7`），
而不是 mainline 6.12+ 的（`READ=1<<2, WRITE=2<<2, RW=12, END=1<<0`，mask = 13）。
`tools/g2d_test.cpp` 里写的是 mainline 那套（`READ (1<<2)`、`WRITE (2<<2)`），
`sync_()` 又**忽略返回值** ⇒ 于是 `START|RW = 12` 每次都被内核以 `EINVAL` 拒绝，
**所谓「修正后的 sync 次序」从头到尾都是空操作**。

实测（`/tmp/syncprobe`，root 与 orangepi 两次结果一致；`DMA_BUF_IOCTL_SYNC = 0x40086200`）：
```
flags=0  EINVAL          flags=1  OK (START|READ)     flags=2  OK (START|WRITE)
flags=3  OK (START|RW)   flags=4  EINVAL (END 无方向) flags=5  OK (END|READ)
flags=6  OK (END|WRITE)  flags=7  OK (END|RW)         flags=12 EINVAL (mainline)
flags=13 EINVAL (mainline)
```
⇒ 旧编码；方向位是**必需**的（0 和「只有 END」都会被拒）。内核源码侧完全对得上：
`drivers/dma-buf/dma-buf.c` 的 mask 检查 + `switch (sync.flags & DMA_BUF_SYNC_RW)` 的 default 分支。

新建的 `ar0234::DmaBuffer` **不再硬编码任何一种编码**，而是在第一次 sync 时两种都试一次、记住内核接受的那个
（两种编码互相排斥：旧 RW=3 落在新 mask 之外，新 RW=12 落在旧 mask 之外，所以不会误判）。
这一条也解释了为什么「有没有 sync」在结果上看不出差别：**这次是仓库里第一次真的发出了有效的 sync**，
而 0 差异的结果与之前「sync 无效」时一致 ⇒ `/dev/dma_heap/system` 的映射在这块板子上本来就不需要 CPU 侧
cache 维护（CPU 写 G2D 能读到、G2D 写 CPU 能读到，两者都由 0 差异实验双向证明）。

**建议下一批**（不在本批范围）：把 `tools/g2d_test.cpp` 与 `tools/g2d-probes/*.c` 里的
`DMA_BUF_SYNC_*` 常量换成旧编码（或直接改用 `ar0234::DmaBuffer`），并**不再忽略** ioctl 返回值；
换完要重跑一次它们的对照，确认结论不变（这项改动在本批**没有执行**）。

---

## T3　第二个 video 节点 open/close 偶发拖死第一路 —— 30 次压测：**该故障 0 次，另抽到 1 次坏启动**

### 方法

板上 `systemd-run --unit=t3stress`（脱离 ssh），30 轮：
每轮 = `vfr -t 20 /dev/video0 1920x1200@120`，**第 6 秒时**用 `tools/t17-openclose.c`（只有 `open()`+`close()`，
一个 ioctl 都不发）对 `/dev/video4` 开关 **3 次**；每轮记录
`vfr` 的 fps/超时/停顿次数 + 该轮的**新增内核消息**（`underflow|fail to disable regulator|frame lost|height error|width error|configuration error|sensor_read error`）。
脚本 `/home/orangepi/t3.sh`，原始日志 `~/t3.log`（已取回本机 `r9/t3.out`）。模块 = 0012，同一 boot。

### 结果（30/30 轮，`DONE` 于 16:20:43）

| 观测 | 次数 |
|---|---|
| 第一路被拖慢/拖死（原 §3.27 签名：掉到 ~18 fps、`sensor_read error`、`vi0` 帧计数冻住） | **0 / 30** |
| 停顿（`no frame for >1s`） | 1 / 30（第 16 轮） |
| **新增内核消息（六类全算）** | **0 / 30** |
| fps | 29 轮 117.97–120.02；第 16 轮 0.20 |

第 16 轮原文：
```
iter 16: RESULT dev=/dev/video0 1920x1200 NV12 frames=4 wall=20.067s fps=0.20 timeouts=20 | stalls=20 | new-msgs=0
iter 17: RESULT dev=/dev/video0 1920x1200 NV12 frames=2400 wall=20.002s fps=119.99 timeouts=0 | stalls=0 | new-msgs=0
```
**判读：这是「坏启动」，不是「被兄弟节点拖死」。** 依据：
- 形状不同 —— 坏启动是「一开始就只出 4 帧然后彻底不动」（`frames=4`、20 次超时、20 次 >1 s 停顿，
  说明第 1 秒起就没帧了）；兄弟节点故障是「先全速跑，video4 一开关才崩」（那样我们会看到 ~720 帧之后才出现停顿）。
- 与 P0-2/T6 的坏启动签名逐字一致（`frames=4`、内核 **0 条**新消息、`vi0` 计数复位、**下一次一定正常** ——
  第 17 轮立刻 119.99 fps）。
- 与 T2 同源性：**不同源**。T2 的引用计数缺口是**确定性**的（每次「无配对 open 的 close」必打 3 条报错，
  但功能不受影响）；T3 的第 16 轮是**概率性**的坏启动（功能受影响，但下一次必好）。本轮 30 轮里
  `underflow`/`regulator` 报错 **0 条**，而 T2 的序列 A/B 在同一个 boot 里必然打出 30 行 ——
  两个现象在数据上就不共存。

### 发作率与既有数据对齐

| 批次 | 启动次数 | 坏启动 | 故障率 |
|---|---|---|---|
| 第七轮 P0-2 基线（40×4 s） | 40 | 3 | 7.5% |
| **本轮 T3（30×20 s）** | 30 | 1 | **3.3%** |
| 合计 | 70 | 4 | **5.7%** |

⇒ 与「5–10%」一致，**每轮只有「启动」这一刻有风险**，所以 20 s 的长跑与 4 s 的短跑给出的故障率同量级。

### 结论

| 问题 | 等级 | 结论 |
|---|---|---|
| 「第二节点 open/close 拖死第一路」（HANDOFF §3.27 / 审计 §2.3 第 4 项） | **未复现** | 30 次里 0 次；`new-msgs` 全 0；第一路 29/30 轮 118–120 fps |
| 与 T2 引用计数缺口是否同源 | **否** | 确定性 vs 概率性；同一 boot 内 T2 必现 30 行、T3 全 0 |
| 唯一一次发作 | **已定性** | 是 P0-2/T6 的「开流坏启动」（5~10%），非兄弟节点所致；用户态看门狗已能兜住 |

**没有新增证据能把「偶发拖死」再往前推一步** —— 因为它在本批的压测里没有发生。
建议下一批若还要查，把压测加长到 100 轮以上，或者改在**两个节点都真正 prepare 过**的前提下测
（本轮 video4 走的是「管线从未 prepare」路径，即 0009 的 `skip_pipeline_close` 分支；
真正危险的是「video4 的管线曾被 prepare 过、但当前 `entity.use_count == 0`」那种状态，
本轮的 30 轮没有构造到它 —— 这一条是**推论，未实测**）。

---

## 补丁 / 文件清单（本批）

### 新增补丁（已归档、已接进 `apply.sh`、`--dry-run` 干净、`bash -n` 通过）

| 补丁 | 内容 | 对应 |
|---|---|---|
| `patches/0012-vin-pipeline-close-refcount.patch` | `vin.c: __vin_pipeline_close()` 把三个 open 侧操作统一到 `vind->use_count` 之下 | T2 |

`apply.sh`：在 0011 之后插入 0012（含说明注释），本机与 TL101 各跑一次 `bash -n`，rc=0。
**没有跑 `sudo bash apply.sh`**，内核树全程只读（`cp` 到 `/tmp` 改，`diff -u` 出补丁；
对内核树 `patch -p1 --dry-run` 通过）。

### 新增 / 改动文件

| 文件 | 改动 |
|---|---|
| `userspace/include/ar0234/g2d.hpp`、`userspace/src/g2d.cpp`、`userspace/apps/ar0234-g2d-selftest.cpp` | **新增**：G2D 位精确封装 + 示例/自检（T5） |
| `userspace/Makefile` | 新增 selftest 目标 + `-I../hwtest-include`（T5） |
| `build/vin-d3d-lbc/vin.c` | 打上 0012（外部编译树，**内核树未动**） |
| `prebuilt/vin_v4l2.ko` | 更新为 0012 构建（md5 `375f6f4f…`），与 `install.sh`/`build-deb.sh` 口径一致 |
| `analysis/round9/REPORT.md` | 本文件 |
| `/lib/modules/6.6.98-sun60iw2/updates/vin_v4l2.ko`（板上） | 换成 0012，`depmod -a` + 重启 |

### 备份（改动前）

```
TL101/仓库: apply.sh.bak-pre-0012
            userspace/Makefile.bak-pre-g2dwrap
            build/vin-d3d-lbc/vin.c.bak-pre-0012
板上      : /lib/modules/6.6.98-sun60iw2/updates/vin_v4l2.ko.bak-pre-0012（= 0010+0011 构建，md5 241c8631…）
            （回退 = 把该文件拷回 vin_v4l2.ko + 重启；等效文件在 build/vin-d3d-lbc/out/vin_v4l2-0010.ko，
              md5 已核对一致）
```

## 收工状态（板上实测，`r9/teardown.out`）

```
残留进程       无（vfr / selftest / openclose / v4l2-ctl / t3stress 全不在；t3stress = inactive）
DTB            d4ee5b6869e7b7cd39ce3762d0e078f9   <-- 与开工完全一致，全程未改 DT
模块           srcversion BA48201D23BA4260923B53E  md5 375f6f4f5c15f72b2d2323f32e7af087 (=0009+0010+0011+0012)
设备节点       /dev/video0 crw-rw---- root video、/dev/video4 同、/dev/g2d crw-rw---- root video
服务           ar0234-3ad active；/etc/modules-load.d/g2d.conf 仍在 ⇒ g2d_sunxi 开机自加载
ISP 时钟       pll-video0-4x = 324000000 -> csi_isp_src（324 MHz）；d3d_min_vblank_us = 500
健康回归       1920x1200 NV12 @120 → frames=1200 wall=10.006s fps=119.93 timeouts=0，max gap 9.23 ms
PM/regulator   全 boot 签名计数 = 0
```

## 结论分级汇总

| 项 | 等级 | 一句话 |
|---|---|---|
| **T1** `RST_BUS_VIDEO_IN` 是否连到 ISP | **确认** | 出流中把 `0x2003884` bit16 清 0 → VIN 帧计数只走 3 帧、ISP 寄存器块 0x5900000 从 0x5 变 0；置回 → 立刻恢复 120 fps 且不用停流。CSI 位同窗口未动 ⇒ 该位确实驱动 video-in/ISP 块且可逆。**DT 未改**，改法/回退/风险已写清（并新增一条硬约束：必须与 T2 一起上，否则关流会真复位 ISP） |
| **T2** PM 引用计数缺口 | **已修复（确认）** | `patches/0012`；触发序列 A/B 的三条签名 **30 → 0**；合法回归 1200p120 `119.88–119.91 fps / 0 超时 ×5`；`regulator_summary` 计数回到 0 证明关流路径没被误跳过 |
| **T3** 第二节点拖死第一路 | **未复现** | 30×（20 s + 中途开关 video4 ×3）：该故障 **0/30**；六类内核消息 **0/30**；抽到 1 次**坏启动**（与 T6/P0-2 同源，非兄弟节点所致）⇒ 与 T2 **不同源** |
| **T4** H.265@1200p120 只出 6 帧 | **未复现（含反证）** | 6/6 次精确重复全部 118.2–118.7 fps；且**有** ISP 错误的那一次照样 240/240 帧 ⇒ 「ISP 报错打死 H.265」证伪；高度对齐/帧率两个假设均不成立（1216/1280 在 capture 节点上无法表达）。**不需要绕** |
| **T5** G2D 位精确路径固化 | **已交付（确认）** | `ar0234::g2d_*` + 自检；随机满量程 0/3456000、真实 ISP 帧 0/3456000、**ISP dma-buf 零拷贝源 0/3456000**、矩形标注内外逐字节精确；非 root 跑通，0 warning |
| 新发现：`DMA_BUF_IOCTL_SYNC` 编码 | **确认** | 本内核用旧 flag 编码；仓库工具用的是 mainline 常量且忽略返回值 ⇒ 同步一直是空操作。新封装改为**运行时协商**；旧工具的修法已写入报告，**本批未改**（下一批） |
| 新发现：`ar0234-rec` 用请求尺寸配编码器 | **候选** | `v4l2.cpp` 存的是驱动返回的尺寸，`ar0234-rec` 用请求尺寸配 VE ⇒ `-h 1216` 会「按 1216 编 1200 的缓冲」。最小改法已写，**本批未改** |
| 文档（HANDOFF/README） | **未动** | 本批任务未含文档项；已知会飘的：模块 srcversion/md5 那两处、README「1200p120 请用 H.264」、`analysis/g2d/REPORT.md` §3 里「sync 次序」的说法。**建议下一批一次性订正**（不要和代码改动混在一起） |
| `isp01` / LSC-MSC 标定 / 第二颗模组 | **禁区 / 缺器材** | 全程未 enable、未造数据 |

### 收工时的最终验证（复核记录）

```
$ K=/home/helios/Desktop/orangepi-build/kernel/orange-pi-6.6-sun60iw2
$ patch -p1 --dry-run -d "$K/bsp/drivers/vin" < patches/0012-vin-pipeline-close-refcount.patch
File vin.c is read-only; trying to patch anyway
checking file vin.c
dry-run rc=0                       <-- 补丁对内核树干净通过

$ cmp /tmp/t2/vin.orig.c $K/bsp/drivers/vin/vin.c && echo KERNEL TREE UNCHANGED
KERNEL TREE UNCHANGED (identical to the pre-patch copy)   <-- 内核树一个字节都没动
$ grep -n "if (vind->use_count)" build/vin-d3d-lbc/vin.c
1314:		if (vind->use_count) {    <-- 外部编译树已打上

$ bash -n apply.sh -> APPLY_SH_SYNTAX_OK（本机与 TL101 各一次）
$ md5sum prebuilt/vin_v4l2.ko -> 375f6f4f5c15f72b2d2323f32e7af087（= 0012 构建）
```
