# 编译选项对照表：文档要求 vs 当前实际

日期：2026-09-22 晚 · 核对对象：`userpatches/linux-sun60iw2-current-a733.config`
（另有 `kernel/orange-pi-6.6-sun60iw2/.config` 二次核对，两者一致）

---

## 结论：文档里列的那份清单，**绝大多数早就开好了**

来源：`HANDOFF-RT-IMAGE.md`（第 45–52 行）、`DESIGN-NOTES.md`（第 234–239 / 531–533 行）。

| 文档要求的项 | 当前实际值 | 状态 |
|---|---|---|
| `CONFIG_PREEMPT_RT` | **`=y`** | ✅ 已开（且 `CONFIG_PREEMPT` 已从配置里移除，避免 kconfig choice 冲突把 RT 顶掉 —— 这正是 HANDOFF §1 修的那个坑） |
| `CONFIG_AW_MSGBOX` | `=y` | ✅ 已开（**必需**，否则 ARM 侧收不到消息） |
| `CONFIG_AW_HWSPINLOCK` | `=y` | ✅ 已开 |
| `CONFIG_AW_SPI` / `CONFIG_SPI_SPIDEV` | 都是 `=y` | ✅ 已开（SPI 字符设备） |
| `CONFIG_AW_CE_SOCKET` / `AW_CE_IOCTL` / `AW_HWRNG_DRIVER` | 都是 `=y` | ✅ 已开 |
| `CONFIG_AW_RTC` / `AW_RTC_REBOOT_FLAG` | 都是 `=y` | ✅ 已开 |
| **`CONFIG_IIO` / `IIO_BUFFER` / `IIO_KFIFO_BUF` / `IIO_TRIGGERED_BUFFER`** | 全部 `=y` | ✅ 已开 ← **这对 BMI088 很重要**（Linux 侧可走 IIO 框架） |
| `CONFIG_STRICT_DEVMEM` | `not set` | ✅ 正是我们要的（`/dev/mem` 可用，否则 `awdevmem.py` 读不了） |
| `CONFIG_AW_TRNG` / `AW_LRADC` / `AW_GPADC` | 不在配置里 | ✅ 已按文档**被迫关掉**（缺头文件 / 用了 6.6 删掉的 `iio_dev->mlock`） |

### 需要标注的几项（不影响启动，但要知道）

| 项 | 实际 | 说明 |
|---|---|---|
| `CONFIG_AW_DMC_DEVFREQ` | **`=m`**（文档建议 `not set`） | 是模块且**没加载**（板上 `/sys/class/devfreq` 只有 NPU）→ 效果等价于没跑，但严格说不是文档建议的形态 |
| `CONFIG_AW_REMOTEPROC` | `not set` | 文档也没要求开（DTS 里没有 remoteproc 节点，开了 probe 不到） |
| `CONFIG_SUN6I_MSGBOX` | `=y` | mainline 驱动，**compatible 不匹配 A733** → 无害但无用 |
| `CONFIG_AW_CRASHDUMP` / `CONFIG_SUNXI_SMC` | 都未设 | 这两条 SMC 路径没编；不影响启动 |
| `CONFIG_AW_WATCHDOG` | `=y` | CPUX 域硬件看门狗（不是 SCP 的） |

---

## 本轮**新加**的 10 项（上一轮会话没做过的）

| 选项 | 值 | 作用 |
|---|---|---|
| `CONFIG_RPMSG` / `RPMSG_NS` / `RPMSG_VIRTIO` | `=y` | 主线 rpmsg/virtio 栈 |
| `CONFIG_VIRTIO` | `=y`（原本就是） | — |
| `CONFIG_AW_RPMSG_VIRTIO` | `=y` | **Allwinner 的 AMP rpmsg 总线**（`aw_virtio_rpmsg_bus.c`） |
| `CONFIG_AW_RPMSG_CTRL` | `=y` | 导出 `/dev/rpmsg-*`（用户态可建 endpoint） |
| `CONFIG_AW_RPMSG_CLASS` | `=y` | rpmsg class |
| `CONFIG_AW_RPBUF` / `RPBUF_DEV` / `RPBUF_SERVICE_RPMSG` | `=y` | rpmsg 之上大块数据交换 |

**故意没开**：`CONFIG_AW_RPMSG_PERF_TRACE` —— 它 `select AW_AMP_TIMESTAMP`，而
**`AW_AMP_TIMESTAMP` 在这棵树里没有 Kconfig 定义**、**`include/linux/amp_timestamp.h` 文件不存在**
（更新版 SDK 才有）。开了会**编译失败**。

> 另注：这 10 项即使编进内核，**目前也不会有功能效果** —— 全部 DTS 里没有任何
> `rpmsg` / `rpbuf` / `remoteproc` 节点，驱动 probe 不到对端。它们只是把基础设施备好。

---

## 最重要的一句：**这些选项要在板上生效，必须重新编译内核并刷机**

配置改的是**源码树**，不是运行时。所以流程是：

```bash
cd /home/helios/Desktop/orangepi-build
sudo ./build.sh userpatches/config-a733.conf        # 重编内核 + 组装镜像
# 验证内核 .config 真的吃到了：
grep -nE 'PREEMPT_RT|AW_RPMSG_VIRTIO|CONFIG_IIO=' kernel/orange-pi-6.6-sun60iw2/.config
```
再刷 `output/images/…*.img`。

---

## 备份链（改之前的状态都在，可随时回退）

```
userpatches/linux-sun60iw2-current-a733.config.bak-014821
userpatches/linux-sun60iw2-current-a733.config.bak-20260922-005119
userpatches/linux-sun60iw2-current-a733.config.bak-20260922-225134   ← 本轮改动前的
```
