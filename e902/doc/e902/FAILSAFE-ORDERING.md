
---

# 补充记录 T19（2026-09-22）：**失败安全设计** —— 握手包前移到 `main()` 最前

## 为什么改

bl31 的流程是：载入镜像 → 设 `RST_START_ADDR` → 放复位 → **`arm_svc_arisc_wait_ready()` 等小核报就绪** → 继续启动 Linux。
如果这个"就绪"信号没到，**挂起发生在 bl31 里，板子根本起不来**。

原先把握手包放在 `main()` 中段（引脚复用、UART、SPI 初始化之后）。这意味着：
只要中途任何一步出错（引脚写失败、UART 初始化卡住、SPI 时钟没开导致访问 fault），
握手就发不出去 → bl31 挂住 → 需要读卡器才能恢复。

## 改法

把 `msgbox_init()` + `msgbox_send_startup_feedback()` **提到 `main()` 的开头**，
在任何外设初始化之前（只保留 `hb_init()` 写 SRAM 标志）。

## 指令级确认（`objdump -d build/fw-scp.elf`，`main()` 的调用序列）

```
40004be4: jal 400049be <hb_init>
40004be8: jal 40004a06 <hb_stage>
40004bea: jal 40004514 <msgbox_init>
40004bec: jal 40004532 <msgbox_send_startup_feedback>   ← 握手最先
40004bf0: jal 400041bc <pinmux_s_uart0>
40004bf8: jal 40004a06 <hb_stage>
40004c00: jal 40004216 <uart_init>
40004c04: jal 4000420e <pinmux_read_pl_cfg0>
...
```

## 效果（这是本轮最重要的风险削减）

| | 改前 | 改后 |
|---|---|---|
| 握手失败/初始化中途挂掉 | bl31 等不到就绪 → **板子起不来，需读卡器** | 握手已先发出 → **板子照常启动**，最坏只是小核后续死掉（丢 DRAM 变频/suspend） |

也就是说：**唯一的"砖"风险被压到只剩"握手包格式猜错"这一种**；而握手包的格式不是猜的——
它来自 `scp.fex` 反汇编（`0x4000b1d2..0x4000b202`：memset 52B → count=13 → 通道 3 发包 → 打印 `"startup feedback ok"`）。

## 产物更新

| 文件 | 大小 | sha256 |
|---|---|---|
| `fw-out/scp-ours-padded-105912.bin`（待刷） | 105912 | `063f4130c8ec96e524199cd909f39134e17377879c3b30d8129d9332665ddc15` |
| `fw-out/scp-ours-6208.bin` | 6208 | `711f604a5f0245e76235ae142d25bd67139c677bd9301832aa45df139db88e34` |

（旧版 6232/`94f9d088…` 已被本次覆盖，请以本页 sha256 为准。）

## 仍未验证

`msgbox_send_startup_feedback()` 是否**真的**能喂饱 bl31 的 `wait_ready`，只有实际刷写并用板子
`/dev/ttyUSB0` 观察才能确认。但**即使猜错，最坏后果也已从"砖"降级为"小核不可用"**——
因为我们的固件不写任何持久介质，回滚 `restore-scp.sh` 在板子正常启动后随时可执行。



---

## 更新（2026-09-22 T21）

- 本页产物哈希已被最终版取代：padded = **8aaf9486151a…**（105912B），fw-scp.bin = bc3848f7…（6216B）。
- 握手包头实测修正：hdr=0 被 bl31 拒绝（板子 no-boot 一次，已恢复）；正确包头 = 0x00900200
  （builder 0x4000b1b0..0x4000b202 / sender 0x4000721e..0x4000733c 逐指令复核，两轮独立）。
- 失败安全结论不变且已被实弹验证的一半证明：握手放在 main() 最前，初始化后续失败不再砖。




## 更新 2（2026-09-22 T22）

本页"握手失败 → bl31 挂起 → 需读卡器"的风险评估需要修正：两次实际启动失败都发生在
**boot0 的启动包 add_sum 校验**（早于 bl31），已由串口实录（bad checksum/bad magic）+ 源码
（board_common.c:846 / private_toc.h）定案。修复 = `fix-bootpkg-sum.py --write`（4 字节）。
失败安全设计（握手前置）结论不变；bl31 对 0x00900200 的接受性仍为 Pending。

