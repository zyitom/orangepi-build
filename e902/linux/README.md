# amp_timestamp：让 Linux 读两核共用的 24 MHz 时间戳

A733 有一个 64 位自由运行计数器（`TIMESTAMP_STA` @ `0x08010000`），大核（Linux）和小核（E902）读的是
**同一个计数器**，所以两边的时间戳可以直接比较，不需要任何对时协议。E902 固件用它给事件打时间戳；
这个驱动让 Linux 侧也能正常使用它（sysfs + 内核 API），不再需要 `/dev/mem`。

## 现状（2026-09-23，已在板上验证）

| 项 | 结果 |
|---|---|
| 运行内核 | `6.6.98-rt58-sun60iw2`（PREEMPT_RT），**内核镜像与主 DTB 未改动** |
| 驱动 | 树外模块 `/lib/modules/6.6.98-rt58-sun60iw2/updates/amp_timestamp.ko`，开机按 DT 别名自动加载 |
| DT 节点 | 用户 overlay `/boot/overlay-user/amp-timestamp.dtbo`，`/boot/orangepiEnv.txt` 里 `user_overlays=amp-timestamp` |
| sysfs | `/sys/bus/platform/devices/8010000.amp-timestamp/{counter,freqid,usec}` |
| 精度 | `usec` 相对 CLOCK_MONOTONIC 走速 1.000000；freqid = 24000000 |

## 用法

用户态：

```bash
cat /sys/bus/platform/devices/8010000.amp-timestamp/counter   # 原始 24 MHz 计数
cat /sys/bus/platform/devices/8010000.amp-timestamp/usec      # 换算成微秒
```

内核驱动（`#include <linux/amp_timestamp.h>`）：

```c
void *ts; u64 now;
if (!amp_ts_get_dev(0, &ts))
        amp_ts_get_timestamp(ts, &now);   /* 与 E902 读到的是同一个计数器 */
```

## 文件

- `amp_timestamp/` —— 模块源码（与 RT 内核树 `bsp/drivers/misc/amp_timestamp.c` 同步）+ `include/linux/amp_timestamp.h`
  + 树外编译用的 `Kbuild`/`Makefile`
- `amp-timestamp.dts` / `.dtbo` —— DT overlay 源码与产物

## 重新编译 / 安装（在板上做，用已装的 linux-headers 包）

```bash
# TL101 上
tar -C e902/linux -czf - amp_timestamp amp-timestamp.dtbo | ar0234-port/tools/ssh_board.sh 'tar -xzf - -C ~'
# 板上
cd ~/amp_timestamp && make && sudo make install          # -> updates/ + depmod
sudo install -m644 ~/amp-timestamp.dtbo /boot/overlay-user/amp-timestamp.dtbo
grep -q '^user_overlays=' /boot/orangepiEnv.txt || echo 'user_overlays=amp-timestamp' | sudo tee -a /boot/orangepiEnv.txt
sudo reboot
```

为什么在板上编：RT 树里的 `.config` 是一份调试配置（PROVE_LOCKING 等），和板上运行内核的配置不同，
在那棵树里编出的模块 `struct module` 大小不符，加载报 `Exec format error`。板上的
`/usr/src/linux-headers-6.6.98-rt58-sun60iw2` 就是运行内核的确切配置。
运行内核的配置里没有 `CONFIG_AW_AMP_TIMESTAMP`，所以 `Kbuild` 里显式定义了 `CONFIG_AW_AMP_TIMESTAMP_MODULE=1`，
否则头文件会退化成桩函数。

改 overlay 前先在 TL101 上验证（板上的 boot.cmd **在 overlay 失败时不会回退原 DTB**）：

```bash
dtc -@ -I dts -O dtb -o e902/linux/amp-timestamp.dtbo e902/linux/amp-timestamp.dts
ar0234-port/tools/ssh_board.sh 'cat /boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb' > /tmp/board.dtb
fdtoverlay -i /tmp/board.dtb -o /tmp/merged.dtb e902/linux/amp-timestamp.dtbo && echo APPLY-OK
```

## 撤销

```bash
sudo sed -i '/^user_overlays=amp-timestamp$/d' /boot/orangepiEnv.txt   # 备份在 orangepiEnv.txt.bak-before-amp-ts
sudo rm /lib/modules/$(uname -r)/updates/amp_timestamp.ko && sudo depmod -a
```

板子若因 overlay 起不来：SD 卡插 TL101 读卡器，在 rootfs 分区（`opi_root`）的 `/boot/orangepiEnv.txt` 里删掉那一行。

## 以后整棵重编 RT 内核时

RT 树（`~/rt-kernel-test/linux-6.6.98-sun60iw2-rt`）已经加好：驱动、头文件、Kconfig（`AW_AMP_TIMESTAMP`）、
Makefile 条目，以及 `sun60iw2p1.dtsi` 里的 `amp-timestamp@8010000` 节点（带 `clock-frequency = <24000000>`）。
出包时在配置里打开 `CONFIG_AW_AMP_TIMESTAMP=m`（或 `=y`）即可；换上新 DTB 后可以删掉 `user_overlays=amp-timestamp`
（留着也无害，两处节点内容相同），并删除 `updates/` 里的树外模块，以免同名模块冲突。

## 已知

- `CNT_FREQID_REG`（0x08020020）从两个核读都是 0；驱动在 FREQID 为 0 时改用 DT 的 `clock-frequency`。
- 开机日志里有一条 `invalid sysfs_emit` 的 WARNING（`host_chose` ← `set_otg_role`），来自厂商 USB OTG 驱动，
  由 `orangepi-hardware` 服务触发，与本驱动无关。
