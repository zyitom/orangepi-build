# RT 内核上板自检指南（抓"自旋锁毛病"的实操方法）

适用内核：`/home/helios/rt-kernel-test/linux-6.6.98-sun60iw2-rt`（已开自检选项的重编版本）
开启的开关及作用：

| 选项 | 抓什么 |
|---|---|
| `CONFIG_DEBUG_ATOMIC_SLEEP` | **自旋类问题的第一报警器**：在硬中断/关抢占/raw 锁区内调用了会睡眠的东西（RT 上 spinlock_t 也算），立刻打 `BUG: sleeping function called from invalid context` + 完整回溯 |
| `CONFIG_PROVE_LOCKING` + `CONFIG_PROVE_RAW_LOCK_NESTING` | lockdep：锁顺序死锁、raw 锁里嵌睡眠锁（`PROVE_RAW_LOCK_NESTING` 就是专管"自旋那种"的验证器） |
| `CONFIG_DEBUG_PREEMPT` | preempt_disable/enable 不配对（抢占计数被驱动改坏） |
| `CONFIG_DEBUG_OBJECTS_TIMERS` | timer/hrtimer 的初始化/销毁/重复启动类 bug（对应 mali、loopback 那类 hrtimer 隐患） |
| `CONFIG_WQ_WATCHDOG` / `CONFIG_DETECT_HUNG_TASK` | workqueue 卡死、D 状态任务卡住 |
| `IRQSOFF/PREEMPT/SCHED/OSNOISE/TIMERLAT/HWLAT_TRACER` | 延迟取证：最大延迟来自谁、关中断了多久 |

## 1. 开机后先做两件事

```bash
uname -v                      # 必须看到 PREEMPT_RT
dmesg | grep -iE "lockdep|BUG|WARNING" | head
```

lockdep 开机自检通过会打一句 `lockdep: ... OK`。之后 **dmesg 里出现下面任何一条 = 有毛病**，直接定位到具体驱动函数：

```
BUG: sleeping function called from invalid context   ← 拿了会睡的锁/调用在原子区
BUG: scheduling while atomic                          ← 原子区里主动睡眠
Possible unsafe locking scenario / inconsistent lock state  ← 锁顺序死锁风险
DEBUG_LOCKS_WARN_ON ... raw_spin_lock ...             ← raw/睡眠锁嵌套违规
ODEBUG: ... active timer                              ← 定时器没删就释放
```

**dmesg 干净不等于没事，必须压着负载跑**（见第 3 步），因为很多路径只在特定事件触发。

## 2. 确认各驱动中断在 RT 下的真实形态

```bash
cat /proc/interrupts          # RT 内核会把线程化的中断显示为线程名
ps -eLo pid,cls,rtprio,comm | grep -iE "irq|ktimersd|rcu"
```

看两点：摄像头（VIN）、WiFi（aic8800）、GPU（pvrsrvkm）、串口（uart）的中断是否都有对应内核线程（RT 默认线程化）；`ksoftirqd/ktimersd` 是否存在。有 `IRQF_NO_THREAD` 的少数硬中断（rtc alarm 等）没有线程是正常的。

## 3. 负载矩阵（每一项跑的时候都开着 `dmesg -w` 盯报错）

| 负载 | 覆盖的驱动 |
|---|---|
| `stress-ng --cpu 4 --io 2 --vm 1` + `cyclictest -m -Sp95 -i1000 -h400 -D30m` | 调度/定时器/整体延迟直方图 |
| glmark2-es2-fb / 连续 compositor 渲染 | img-bxm GPU（pvrsrvkm），GPU 满载下的延迟 |
| iperf3 双向 + 长时间 scp | aic8800 WiFi（tasklet/IRQ 路径最忙的场景） |
| `v4l2-ctl --stream-mmap` 连续抓流 | VIN 摄像头链路（含 ar0234） |
| HDMI 点亮 + 视频播放 | DRM disp/HDMI |
| dd 到 TF/emmc + USB 拷贝 | mmc/USB |

压测时同时看延迟直方图：`cyclictest` 结束后看 Max 一列；也可以用 osnoise：

```bash
cd /sys/kernel/tracing
echo osnoise > current_tracer && echo 100 > osnoise/stop_tracing_us
sleep 60 && cat trace | head -50     # 直接显示每次 >100us 噪声的元凶调用栈
```

## 4. 逐驱动"定点抓捕"（怀疑谁就抓谁）

ftrace 可以盯着某个驱动的回调，看它有没有睡：

```bash
cd /sys/kernel/tracing
echo function_graph > current_tracer
echo sunxi_uart_irq > set_graph_function     # 换成怀疑的回调名
echo 1 > tracing_on; sleep 10; echo 0 > tracing_on
cat trace | grep -E "msleep|mutex|schedule" | head
```

## 5. 判定标准

- 全负载矩阵跑完 + `dmesg` 无 BUG/WARNING + cyclictest Max 稳定（几 ms 内、无离谱尖峰）→ 这套 BSP 在 RT 下可以判定干净。
- 出现第 1 节的任一报错 → 回溯里最后一行就是犯事的驱动函数，拿着它来改（改成 raw 锁、挪出原子区或丢到 workqueue/kthread）。
- 报错只来自 `__schedule`/调度器自身而各驱动函数都不出现 → 更可能是核心代码语义冲突，把 dmesg 完整回溯发出来分析。
