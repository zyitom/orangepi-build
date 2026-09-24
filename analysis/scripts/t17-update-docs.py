#!/usr/bin/env python3
"""Tidy the report appendix and fold round-5 results into HANDOFF.md."""
import os
import re
import sys

ROOT = "/home/helios/Desktop/orangepi-build/ar0234-port"
REPORT = os.path.join(ROOT, "analysis/t17/REPORT-0916-isp-capacity.md")
HANDOFF = os.path.join(ROOT, "docs/HANDOFF.md")
NL = chr(10)


def fix_report():
    with open(REPORT, newline="") as f:
        s = f.read()
    old = """| ISP 时钟可达范围（只读 round_rate 扫描） | `dmesg`（模块 `ispclk_scan` 加载时），见 §2.1(d) 摘录 |
| 时钟阶梯（框架法，含混淆） | `analysis/t17/iso-top.out`、`iso2.out`、`isp-div.out` |
| 纯分频法 324/162（本报告 §2.2 的出处） | `analysis/t17/pure-div.out` |
| 流运行中改时钟的检验（A/B/C/D） | `analysis/t17/pure-div.out`、`isp-div.out` |
| 3.1 复现（0008） | `analysis/t17/repro-close.out` |
| 3.1 验证（0009） | 同上文件（最后一次运行），`srcversion 843BC1A03606D35EC062D57` |"""
    new = """| ISP 时钟可达范围（只读 round_rate 扫描） | 本报告 §2.1(d) 里有逐条摘录（原始 dmesg 在下面的串口日志里） |
| 纯分频法 324/162（§2.2 的出处） | 本报告 §2.2 有逐条摘录 |
| 3.1 复现（0008）与验证（0009） | `analysis/t17/repro-close.out`（最后一次运行是 0009 的验证，`srcversion 843BC1A03606D35EC062D57`） |

⚠️ 本轮各次时钟阶梯的原始输出**打在板子 `/tmp` 里，而 `/tmp` 是 tmpfs、期间重启过多次**，
所以那些文件已经不在了（`analysis/t17/{pure-div,isp-div,iso2}.out` 只剩空壳）。
**结论所依赖的每一段原始文本都已逐字复制进本报告正文**（§2.1(d)、§2.2、§3.1、§3.3），
串口侧的全量日志留在 `analysis/t17/serial.log`。以后跑这类实验，输出要直接落到主机。"""
    if old not in s:
        print("report appendix pattern not found")
        return 1
    s = s.replace(old, new)
    with open(REPORT, "w", newline="") as f:
        f.write(s)
    print("report appendix tidied")
    return 0


def fix_handoff():
    with open(HANDOFF, newline="") as f:
        s = f.read()
    orig = s
    n = 0

    # 1) §3.27 - mark fixed
    old = "27. **\"打开第二个 video 节点又关掉（不出流）\"会把第一路从 120 fps 拖到 ~18 fps**"
    if old not in s:
        print("3.27 anchor not found")
    else:
        anchor = "**未修**；正常双路（都出流）不受影响。"
        new = ("**✅ 2026-09-16 第五轮：已修复 = `patches/0009`**（复现 → 定位 → 修复 → 验证全做完，"
               "见 `analysis/t17/REPORT-0916-isp-capacity.md` §3.1）。"
               "复现用 `analysis/scripts/t17-openclose.c`（**只有 open()+close()，不发任何 ioctl**）+ `analysis/scripts/t17-repro-close.sh`："
               "0008 下 E1 单路 120.00 fps / 0 超时，E2 中途 open+close 一次 video4 → **29.59 fps / 23 次超时**、"
               "`vi0` 帧计数冻在 908、`sunxi-vin-core 5831000.vinc: Runtime PM usage count underflow!` ×1、"
               "`sensor_read error! sensor is not used!` ×3。"
               "定位到 `vin_video.c` `vin_close()` 的提前 return 跳过了 `vin_pipeline_call(vinc, close, ...)` →"
               "`__vin_pipeline_close()` → `vin_video_core_s_power(CAPTURE, 0)` = `pm_runtime_put_sync(&vinc->pdev->dev)`。"
               "0009 改成 `goto shared_teardown`，并且对\"从没 S_INPUT 过、管线没 prepare\"的节点跳过 pipeline close"
               "（否则必踩 `vin.c:1287` 的 `WARN_ON`，开机时 udev 的 `v4l_id` 每次都踩）。"
               "验证：**E1 120.03 fps、E2 119.04 fps，都是 0 超时、0 underflow、0 新 WARN**（全机 WARNING 只剩既有的 "
               "sysfs_emit/orangepi-hardware 那一条）。`apply.sh` 已接 0009。")
        s = s.replace(anchor, "**未修**；正常双路（都出流）不受影响。" + NL +
                      "    第五轮更新：" + new)
        n += 1

    # 2) T16b row
    old_t16b = "| T16b | 第二个 video 节点 open/close 拖慢第一路（§3.27） | 已复现、已定性、**未修**"
    if old_t16b in s:
        idx = s.index(old_t16b)
        end = s.index(NL, idx)
        s = (s[:idx] +
             "| T16b | 第二个 video 节点 open/close 拖慢第一路（§3.27） | ✅ **2026-09-16 第五轮：已修复并验证 = `patches/0009`**"
             "（`goto shared_teardown` + 未 prepare 的管线跳过 pipeline close）。0008 下 29.59 fps / 23 超时 / 1 次 underflow →"
             " 0009 下 **119.04 fps / 0 超时 / 0 underflow / 0 新 WARN**。复现工具 `analysis/scripts/t17-openclose.c`、`analysis/scripts/t17-repro-close.sh`；"
             "报告 `analysis/t17/REPORT-0916-isp-capacity.md` §3.1 |" +
             s[end:])
        n += 1
    else:
        print("T16b anchor not found")

    # 3) T16c row -> candidate
    old_t16c = "| T16c | `isp01 = okay` 破坏第一路的机制（§3.19 更正） | 已复现（加驱动保护后仍然坏）、**机制未定位**"
    if old_t16c in s:
        idx = s.index(old_t16c)
        end = s.index(NL, idx)
        s = (s[:idx] +
             "| T16c | `isp01 = okay` 破坏第一路的机制（§3.19 更正） | 已复现（加驱动保护后仍然坏）。"
             "**2026-09-16 第五轮：候选机制已给出**——四个 isp 节点是同一个 0x1300 字节寄存器窗口的四个**别名**"
             "（结束地址都是 0x5901300，起始地址每个差 4 字节：`0x5900000/0x58ffffc/0x58ffff8/0x58ffff4`），"
             "而 `of_iomap()` 拿的是区间**起始**，所以 isp01 实例的每次寄存器访问都比 ISP0 的同一个字段**低 4 字节**，"
             "写成功但写进了别的寄存器 ⇒ 第一路静默 0 帧且内核无报错。定级 **候选**（未直接观测）。"
             "只读验证法：出流中同时读 `0x5900000` 与 `0x58ffffc` 比对，或统计 isp01 实例的 `bsp_isp_*` 调用。"
             "结论：**仍然绝对不要打开这个节点**。见报告 §3.2 |" +
             s[end:])
        n += 1
    else:
        print("T16c anchor not found")

    # 4) prepend round-5 header note near the top
    marker = "更新：2026-09-16（第四轮）。"
    if marker in s:
        s = s.replace(marker,
                      "更新：2026-09-16（**第五轮**）。**本轮新增：单块 ISP602 的容量实测/外推结论 + 补丁 `0009`（修掉 §3.27）。**"
                      + NL + "报告：`analysis/t17/REPORT-0916-isp-capacity.md`（含\"接上第二颗模组后还要补测什么\"的一页清单）。"
                      + NL + "上一轮：2026-09-16（第四轮）。", 1)
        n += 1
    else:
        print("header anchor not found")

    if s != orig:
        with open(HANDOFF, "w", newline="") as f:
            f.write(s)
        print("HANDOFF updated (%d edits)" % n)
    return 0


if __name__ == "__main__":
    r = fix_report()
    h = fix_handoff()
    sys.exit(r or h)
