# 交接简报：A733 / Orange Pi Zero 3W —— RT Debian 整机镜像

生成时间：2026-09-22 01:48 · 主机 `ssh helios@TL101`· 板子经 `~/Desktop/orangepi-build/ar0234-port/tools/ssh_board.sh`
项目根：`/home/helios/Desktop/orangepi-build`

---

## ★ 现在卡在哪一步（唯一阻塞点）

**RT 配置没有真正生效。**

- RT 补丁**已应用**（`localversion-rt = -rt58`），内核 release 串也显示 `6.6.98-rt58-sun60iw2`；
- **但内核 `.config` 里是 `# CONFIG_PREEMPT_RT is not set`** → 编出来的内核**不是 PREEMPT_RT**（只有补丁是死的，版本串是"装饰"）。
- **根因**：`CONFIG_PREEMPT_RT=y` 与 `CONFIG_PREEMPT=y` 同属一个 kconfig `choice`，两个都写 → kconfig 取了 `PREEMPT`、丢掉 `RT`。
- **已修**（本简报生成时）：从 `userpatches/linux-sun60iw2-current-a733.config` 删掉了 `CONFIG_PREEMPT=y`，现在只剩 `8058:CONFIG_PREEMPT_RT=y`。

### 下一位接手者的第一件事

```bash
cd ~/Desktop/orangepi-build
# 1) 确认配置正确（只该有 PREEMPT_RT=y，没有 CONFIG_PREEMPT=y）
grep -nE 'PREEMPT_RT|^CONFIG_PREEMPT=' userpatches/linux-sun60iw2-current-a733.config
# 2) 重跑整机镜像（rootfs 已被缓存，主要是重编内核）
sudo ./build.sh BOARD=orangepizero3w BRANCH=current RELEASE=bookworm BUILD_OPT=image BUILD_MINIMAL=yes BUILD_DESKTOP=no
# 3) 编完必须验证（关键！）
grep -n 'CONFIG_PREEMPT_RT' kernel/orange-pi-6.6-sun60iw2/.config     # 必须是 =y
ls -la output/images/                                                # 拿到 *.img
```

**注意**：本简报生成时有一个**正在跑的旧构建**（非 RT 配置），它会产出 `output/images/*.img` —— **那个 .img 不是 RT，别拿去刷**。它的价值是把 rootfs 缓存建好，让下一次跑更快。

---

## 已完成 / 已验证（交接上下文）

| 项 | 状态 | 位置 |
|---|---|---|
| RT 补丁 | ✅ 已应用 | `userpatches/kernel/sun60iw2-current/0000-rt58.patch`（源：`rt-check/patch-6.6.99-rt58.patch`，干跑 0 FAILED） |
| 内核 release | ✅ `6.6.98-rt58-sun60iw2` | — |
| ar0234 相机补丁 ×13 | ✅ 全部 `o.k.` | `userpatches/kernel/sun60iw2-current/0001…0013` |
| 内核 deb | ✅ 已产出 | `output/debs/linux-{image,headers,dtb}-current-sun60iw2_*.deb`、`-dbg` |
| u-boot deb | ✅ | `output/debs/u-boot/linux-u-boot-current-orangepizero3w_1.0.0_arm64.deb` |
| 已开启的外设配置 | ✅ | 见下表 |

**`userpatches/linux-sun60iw2-current-a733.config` 里本次加/改的项**：
- 打开：`CONFIG_PREEMPT_RT=y`
- 外设：`AW_CE_SOCKET / AW_CE_IOCTL / AW_HWRNG_DRIVER / AW_SPI / SPI_SPIDEV / AW_RTC / AW_RTC_REBOOT_FLAG`
- E902 相关：`AW_MSGBOX / AW_HWSPINLOCK`
- 传感器框架：`IIO / IIO_BUFFER / IIO_KFIFO_BUF / IIO_TRIGGERED_BUFFER`
- **被迫关掉**（编译不过，都是厂商 BSP 的老问题）：
  - `AW_TRNG` —— `bsp/drivers/ce/sunxi_trng/` **只有 .c 没有 .h**（头文件缺失）
  - `AW_LRADC` / `AW_GPADC` —— 驱动里用 **6.6 已删除的 `iio_dev->mlock`**（`bsp/drivers/lradc/sunxi-lradc.c:753,763`）。要用它们必须先移植到 `iio_device_claim_direct_mode()`。

备份：`userpatches/linux-sun60iw2-current-a733.config.bak-*`（改前的版本都在）。

---

## 上板后必须做的自检（否则 RT 等于没验）

照 `rt-check/rt-runtime-guide.md`（作者已写好）：
```bash
uname -v                                    # 必须看到 PREEMPT_RT
dmesg | grep -iE "lockdep|BUG|WARNING"      # 不能有 sleeping function called from invalid context 之类
```
再看 `rt-check/rt-driver-audit.md`：**RT 下驱动隐患清单**（Mali/loopback hrtimer、WiFi tasklet 等），压负载矩阵前先过一遍。

---

## 其他线程的当前状态（供接手者参考，不要重复劳动）

1. **E902 协处理器** —— 卡在 **SCP 参数块协议**（u-boot `arisc_i.h` 的 `dts_cfg_64_t`：DRAM 时序/DVFS/space/msgbox…/start_os）。已实测：换 `scp.fex` 会导致 **bl31 挂死、板子起不来**（无握手）。备份与恢复脚本齐全：
   - `e902/backup/sd-boot-head.img`（24MB，SD 引导头，救砖用）
   - `e902/backup/board-boot-backup.tgz`（板上 /boot + /lib/modules）
   - `e902/restore-sd-auto.sh`（插卡自动写回）
   - 中断通路已查清：ARM→E902 写 `CPUS_MSGBOX 0x07094000`（E902 侧 STBY irq 39）；E902→ARM 写 `CPUX_MSGBOX 0x03004000`（ARM 侧 irq 48）；门控 `r-CCU+0x0744` 板上已是 `0x00010001`。
   - 已发现固件可能 bug：`e902-fw` 用 CLIC irq **48**，但 48 是 CPUX 的写中断，CPUS(E902) 自己的读中断应是 **39**。
2. **底层驱动补全**（CE/SPI/RTC/40-pin/GPI/IIO）—— 可执行产物已在 `ar0234-port/driver-completion/`（`config.fragment`、`merge-config.sh`、`dt-enable.sh`、`verify.sh`、`40pin/`）。
3. **实时性实测**（已做）：`cyclictest` 1 kHz —— 绑核+FIFO 最坏 **5–22 µs**；不绑核最坏 194–806 µs、甚至 5 ms 离群。自研工具 `e902/ts_scan.c`、`lat_test.c`。

---

## 风险与注意

- **改 `userpatches/` 会影响之后所有构建**（已多处备份 `.bak-*`）。
- 内核树 `kernel/orange-pi-6.6-sun60iw2` 属主是 **root**，构建在树内进行（项目既有做法）；不要在树里手工乱改。
- **别刷那个非 RT 的 .img**；刷写前先备份一张能启动的 SD。
- 板子当前**健康**（相机栈齐全、厂商 SCP 在跑）。
