# Round 11 — 可调参数落地（gain/gamma/exposure）+ 黑帧事件根因 + N1/N3/N4 收口

日期：2026-09-17（接第十轮）。本轮从"用户问 gain/gamma/exposure 能不能调"开始，经历了一次
完整的黑帧排障（最终证实是两个平凡原因叠加），交付了**真正可用的固定曝光/增益模式**。

## T1 固定模式重设计：fixed 直通（本批核心交付）

**旧实现（第十轮 C14）的缺陷链（全部实测）**
1. libisp 手动模式（参数文件补丁 manual_en=1/ae_en=0）下，libisp 仍**每帧**调用传感器 AE 入口
   （`sensor_s_exp_gain`）：曝光字段带钉扎值（✓ C14 验证的 EXP_LINES 恒定就是它），但 **gain 字段
   恒为钳制下限 16（=1.0x）**——传感器模拟增益从未被写过，一直停在 1.0×。
   ⇒ `gain = 4.0` 配置实际产出 1.0× 增益的画面；室内场景亮度数学：84 × (200/1225行) × (1.0/34.7) ≈ 0.4
   —— **第十轮 C14 的"固定模式"画面其实全是黑的**，当时只验收了日志恒定、没人看过图像。
2. 竞速实测：流运行中向传感器子设备（/dev/v4l2-subdev0，**不忙**）写 gain=16x：ioctl 立即生效
   （读回 25600），**1 秒内被 libisp 的下一次 AE 写覆盖回 1600**。120 Hz 重写，用户态竞速必输。
3. 无 libisp 时写同样的值：**钉住不丢**，画面 Y 从 7 跳到 82。

**新实现**：`/etc/ar0234.conf` `mode = fixed` 时，ar0234-3ad **不启动 libisp 会话**（无 AE 即无竞争），
直接把 `exposure_lines`/`gain` 通过传感器子设备写入（一次，流停即随流结束）：
```
ar0234-3ad: fixed exposure 1200 lines
ar0234-3ad: fixed gain 16.00x
ar0234-3ad: fixed passthrough via /dev/v4l-subdev0 (no libisp, no AE)
板上验收（exppoll 流中读回 + CPU 直读帧统计）：
  exp=19200 (1200 lines) gain=25600 (16.00x) | Y mean=82.8   ← 钉住 + 画面正常
```
- ISP 侧配置来自内核 ctx 恢复（`/mnt/isp0_*.ctx_saved.bin`，最后一次 auto 会话所存）或 ISP 上电默认
  （实测两者都能出图；无 ctx 时画面也正常）。
- **运维要求**：装完先跑一次普通 auto 流（让 ctx 建立）；AWB 停留在最后 auto 会话的增益。
- **参数文件字节补丁全部退场**：fixed 不再碰参数文件；`apply_fixed_mode` 仅保留 ae_log 诊断用途；
  gamma 的字节补丁代码已删除。

**黑帧事件（round 11 中段 18:46–19:15）的定性与自愈**
- 现象：从 fx-1200-100 实验起所有 dump 变黑（Y≈0.3-7），持续到本轮 19:2x 自愈，期间重启不恢复。
- 判决实验：管线 CPU 直读 Y=84（健康）、测试图案控件在位、场景明亮（AE 34.69x 饱和）。
- 复盘结论：**两个平凡原因叠加，不存在"管线毒化"**：
  ① fixed 模式 gain 不生效（缺陷 T1-1）→ 固定曝光画面天然黑（Y≈0.4-15，随设置变化）；
  ② 实验室灯光在 ~18:56 后变化 → AE 最大化（1225 行×34.7x）也只够 Y≈0.3-7 的画面。
  所有"黑"读数都能被这两个因素定量解释；被怀疑的"参数文件被改→libisp 拒绝→黑"没有成立过
  （矩阵实验的文件状态链后来发现被 select() 的自动回装污染，相关轮次的结论作废）。
- 交接警示：**ae_log/亮度判读必须同时看 AE 决策值与实际帧内容**；实验室灯光变化会伪装成管线故障。

## T2 N3 关闭：流后 gain 读回的真相
- "流结束后 gain 控件读回 1.0x"（第十轮 N3 挂账）：**不是复位 bug**。fixed 会话里传感器增益本来就
  没被写过（T1-1），控件显示的 1.0x 是驱动 `info->gain` 的真实值——libisp 日志里的 AGAIN:64 是它
  自己的状态，从未落地。auto 会话无此现象（流中/流后读回一致，如 25.94x/22.38x）。
- 结论：驱动行为正确，无需改动。N3 关闭。

## T3 N1 关闭：真实 OpenCV 消费者（板上 OpenCV 4.5.1 开发包已在）
- `userspace/apps/ar0234-cv-consumer.cpp`（`make cv-consumer`，需 pkg-config opencv4）：
  捕获帧 → G2D 两次 Y8 位精确搬运到消费者 dma-buf → **cv::Mat 直接覆盖在 mmap 的 dma-buf 上**
  （零拷贝视图）→ `cv::cvtColorTwoPlane` NV12→BGR → cv::mean/可选 imwrite。
- 板上实测 240 帧：G2D 搬运 2.64 ms（设备侧），OpenCV 转换+统计 1.00 ms avg / 10.5 ms max（CPU），
  **capture → cv::Mat BGR 就绪 3.64 ms avg**；BGR 均值 (82,91,87) 为真实场景。
- 与 C10 的 NPU 路径（同帧 --fanout 喂 NPU）合起来 = "一帧同时给 NPU + OpenCV"完整落地。

## T4 N4 关闭：deb payload
- `ar0234-npu-zerocopy` 进入 deb（/usr/local/bin）；payload 的 ar0234-3ad 刷新为本轮直通版；
  deb 重建验证通过。

## T5 gamma 的正确路线（回答用户"gamma 能不能调"）
- 能，但**不是运行时控件、也不是参数文件字节补丁**（后者已全部退役）：gamma 表是 ISP 参数文件里的
  5×(3 通道×1024 点) 数据（结构体偏移 56480，文件 +74）。**唯一验证过的方式是整文件重生成**：
  `tools/make_isp_bin.py <template> <tuning> out.bin --gamma-table linear`（线性 0..4095）或默认出厂
  曲线，然后按 P0-3 的三处安装流程整体替换 + 清 ctx。`/etc/ar0234.conf` 的 `gamma =` 键保留作说明。
- 亮度类运行时可调项见 README「可调参数地图」（本轮新增）。

## 板上收工状态
```
/etc/ar0234.conf = 新版样例（auto 默认，fixed 语义已更新）
ar0234-3ad = 直通版（payload 同步）；auto AE 正常收敛（Y=91 @ 24.9x）
ctx 文件已归位；packaging deb 已重建（含 npu-zerocopy）
exppoll / cv-consumer 构建在 ~/ar0234test/userspace/build/
```

## 证据等级
- T1 全链路 = 确认（读回 + 亮度数学 + 双向对照：有/无 libisp 写 gain 的存活）
- 黑帧定性 = 确认（两个因素的定量解释 + 自愈后健康复核）；"ctx 毒化/自愈"细节 = 候选（未复现）
- T3/T4 = 确认（板上实测输出）
