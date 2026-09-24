# SCP 在底层到底做什么？删掉会怎样？—— 来自 bl31 自己的日志字符串

日期：2026-09-22 · 证据：`u-boot/v2018.05-sun60iw2/monitor.fex`（**bl31 本体**，78609 B）里的字符串，
与 `scp.fex` 的字符串对照。**这是目前最权威的一手证据**——bl31 是那个真正启动 SCP 的组件。

---

## 1. bl31（`monitor.fex`）里内置了一整个「SCP 启动器」

`strings monitor.fex` 里的 `[SCP]` 日志（**bl31 自己打印的**），按执行顺序排：

```
❶ [SCP] :load arisc image finish            从启动包取出 scp.fex，拷到 SRAM
❷ [SCP] :setup arisc para finish            写参数块（DRAM 时序/DVFS 表/dev_cfg）
❸ [SCP] :set arisc reset to de-assert state
❹ [SCP] :release arisc reset finish         放复位 → E902 开始跑
❺ [SCP] :load arisc finish
❻ [SCP] :hwmsgbox initialize                初始化 MSGBOX
❼ [SCP] :wait arisc ready....               ★ 在这里等握手（我们上次就是死在这）
❽ [SCP] :arisc version: [%s]                读小核上报的版本
❾ [SCP] :arisc startup ready
❿ [SCP] :arisc startup notify message feedback   ★ 收到 startup notify 的回应
⓫ [SCP] :arisc startup notify message free directly
⓬ [SCP WARNING] :arisc startup waiting ignore message   （容错分支）
⓭ [SCP] :sunxi-arisc driver is starting
⓮ [SCP] :ac327 send hard syn message: 0x%x          ★ 发 hard syn
⓯ [SCP] :send hard sync feedback message: 0x%x      ★ 收 hard syn 回应
⓰ [SCP] :ac327 receive hard syn message feedback: 0x%x
⓱ [SCP] :ac327 receive asyn message: 0x%x
⓲ [SCP] :ac327 send async message : 0x%x
```

`scp.fex` 侧的对应字符串：

```
startup feedback ok / send notify succeed / send notify failed
feedback startup result [%d]
send syn message / query syn msg / query asyn msg / send asyn message
send feedback message
feedback hard syn message : %x
MESSAGE FROM AC327 / wait ac327 resume...
```

### ⇒ 关键结论：**握手不止我们做的那一步**

| 阶段 | 谁发起 | 报文 | 我们上次做了吗 |
|---|---|---|---|
| **① startup notify** | SCP → bl31 | 我们解出的「启动反馈」（头字 `0x00900200`、13 字） | ✅ 做了（**但头字写错了 → 失败**） |
| **② hard syn** | bl31 ↔ SCP 双向 | `hard syn message` + `hard sync feedback message` | ❌ **完全没做** |

**⇒ 上次板子卡死的直接原因很可能不止头字错，还缺了 ② 这一阶段。**

### 另外：**关机/重启也走 SCP**

```
Sunxi System Reset: SCP error %u.
Sunxi System Off:    SCP error %u.
aw System Reset: operation not handled.
plat_aw_system_reset / psci_plat_pm_ops->system_reset
```

即 **PSCI 的 system_reset / system_off 会经 bl31 通知 SCP**。这是 SCP 在启动后的**确定职责之一**。

---

## 2. 「直接删掉 scp 会怎样」—— 分三层回答

### 层 1：构建层 —— **删了会构建不出来**

`u-boot/v2018.05-sun60iw2/boot_package.cfg` 明确列了三项：

```
[package]
item=u-boot,   u-boot.fex
item=monitor,  monitor.fex
item=scp,      scp.fex
```

而 `scripts/pack-uboot.sh:61-62`：

```bash
dragonsecboot -pack boot_package.cfg > /dev/null
[[ $? -ne 0 ]] && exit_with_error "dragon pack error"
```

⇒ **删掉 `scp.fex`，打包直接报错、构建中断。**（不是板子问题，是构建问题）

### 层 2：启动层 —— **写成空的/错的 → 板子起不来**

bl31 的第 ❼ 步 `wait arisc ready....` 会一直等；等不到就不继续。
**这一点我们已经用真机实测确认过**（第一次刷写头字写错 → 整块板子死掉、呼吸灯灭）。

### 层 3：**"整个关掉 ARISC"** 是一条理论上可行的路

u-boot 里有两个开关（`drivers/arisc/Kconfig`）：

```
config SUNXI_ARISC_EXIST             bool "support sunxi arisc"                # 当前 =y
config ARISC_DEASSERT_BEFORE_KERNEL  bool "sunxi arisc deassert before kernel" # 当前 =y
```

- `bootm.c:403` 的 SMC 调用被 `#ifdef CONFIG_ARISC_DEASSERT_BEFORE_KERNEL` 包着
- `board_common.c:767` 显示：**如果关掉 `ARISC_DEASSERT_BEFORE_KERNEL`，改由 `sunxi_arisc_probe()` 启动**（还是启动）
- **只有 `CONFIG_SUNXI_ARISC_EXIST=n` 才是"彻底不碰"** → u-boot 两个路径都不走 → bl31 不启动 SCP

**后果**：
- ✅ 板子**理论上能正常启动**（不需要任何握手）
- ❌ **但 E902 会永远停在复位态、也没法被我们用**（软件侧没有复位源 —— 见 T9/T12）
- ⚠️ 关机/重启路径可能报 `SCP error`（但那是软失败，不影响启动）

**⇒ 这条路适合拿来做"SCP 到底对启动有多必需"的对照实验，但不解决你的目标。**

---

## 3. 结论

| 做法 | 结果 |
|---|---|
| **直接删 `scp.fex`** | ❌ 构建失败（`dragon pack error`） |
| **把 scp 内容清空/写错** | ❌ bl31 卡在 `wait arisc ready` → **板子起不来**（已实测） |
| **`CONFIG_SUNXI_ARISC_EXIST=n` 重编 u-boot** | ⚠️ 板子能启动，但 **E902 永久不可用**（无软件复位） |
| **替换成"会正确握手"的固件** ✅ | 板子启动 + E902 归我们 |

**所以不是"删"，而是"替换"——而且现在知道握手至少是两阶段（startup notify + hard syn）。**

---

## 4. 下一步（零风险，可立刻做）

把 `monitor.fex` 的 **`hard syn` 发送函数**反汇编出来，搞清：
1. `hard syn` 报文的头字/内容是什么
2. 它在启动流程里的**时序**（是在 `wait arisc ready` 之后立刻，还是延后）
3. `ac327` 到底指谁（是 SCP 的别名，还是另一颗核）

有了这两阶段，我们的固件才有机会真正被 bl31 接受。
