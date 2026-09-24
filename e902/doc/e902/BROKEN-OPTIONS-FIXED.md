# 三个编不过的内核选项 — 修复记录（2026-09-22 深夜）

## 结论

| 选项 | 状态 | 修复内容 | 产物 |
|---|---|---|---|
| `AW_AMP_TIMESTAMP` | ✅ 编译通过 | **新写驱动**（厂商树里根本没有这个驱动） | `bsp/drivers/misc/amp_timestamp.o` (208240 B) |
| `AW_LRADC` | ✅ 编译通过 | `indio_dev->mlock` → `iio_device_claim_direct_mode()` | `bsp/drivers/lradc/sunxi-lradc.o` (324848 B) |
| `AW_TRNG` | ✅ 编译通过 | **补齐缺失的 `include/sunxi-trng.h`** | `bsp/drivers/ce/sunxi_trng/sunxi_trng.o` (265056 B) |
| `AW_GPADC` | ✅ 编译通过（附带确认，无需修改） | — | `bsp/drivers/gpadc/sunxi_gpadc.o` (664192 B) |

配置（`userpatches/linux-sun60iw2-current-a733.config`，备份链 `.bak-20260922-225134` 为改动前）：

```
CONFIG_AW_AMP_TIMESTAMP=y     # 跨核共享时间戳，越早越好，内置
CONFIG_AW_TRNG=m              # 按需加载
CONFIG_AW_LRADC=m
CONFIG_AW_GPADC=m
```

## 各项细节

### 1. AW_AMP_TIMESTAMP —— 全新驱动

**厂商树里 `Kconfig` 有这个选项（甚至被 `AW_RPMSG_PERF_TRACE` select），但驱动源码整个缺失**，所以开了必挂。

新写了两份文件：

- `include/linux/amp_timestamp.h`
  - `CONFIG_AW_AMP_TIMESTAMP` 未定义时给 `static inline` stub，定义时给原型；
  - 导出 `amp_ts_get_dev / amp_ts_get_timestamp / amp_ts_get_freqid`。
- `bsp/drivers/misc/amp_timestamp.c`
  - platform 驱动，`compatible = "allwinner,amp-timestamp"`；
  - reg-names `sta`(0x08010000 计数器) / `ctrl`(0x08020000 使能+频率档)；
  - 探测时把 `freqid` 写回 `CNT_FREQID`（本板 24 MHz → 档 0），并使能计数器；
  - 防撕裂 64 位读（低→高→低，高变了重读）；
  - sysfs：`counter` / `freqid` / `usec`；
  - `dev_info` 打印一律整数运算 —— **内核 ARM64 编译带 `-mgeneral-regs-only`，浮点直接编译错**（第一版踩了这个坑）。

Kconfig 条目加在 `bsp/drivers/misc/Kconfig` 的 `endmenu` 前；Makefile 加
`obj-$(CONFIG_AW_AMP_TIMESTAMP) += amp_timestamp.o`。

DTS 节点插在 `arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi` 的
`r_pio: pinctrl@7025000` 之前：

```dts
amp_timestamp: amp-timestamp@8010000 {
        compatible = "allwinner,amp-timestamp";
        /* TIMESTAMP_STA (live counter) + TIMESTAMP_CTRL (enable/freqid) */
        reg = <0x0 0x08010000 0x0 0x1000>,
              <0x0 0x08020000 0x0 0x1000>;
        reg-names = "sta", "ctrl";
        status = "okay";
};
```

0x08010000 / 0x08020000 两段地址此前已确认无冲突。**dtb 已编译出含该节点**
（`strings sun60i-a733-orangepi-zero3w.dtb | grep amp` 可见 `amp-timestamp@8010000`）。

### 2. AW_LRADC —— mlock API 移植

Linux 6.6 删除了 `struct iio_dev->mlock`。`bsp/drivers/lradc/sunxi-lradc.c`
的 `read_raw` 里两处：

```c
-	mutex_lock(&indio_dev->mlock);
+	/* Linux 6.6 removed iio_dev->mlock; use the official claim/release API. */
+	if (iio_device_claim_direct_mode(indio_dev))
+		return -EBUSY;
 	...
-	mutex_unlock(&indio_dev->mlock);
+	iio_device_release_direct_mode(indio_dev);
```

`sunxi_gpadc.c` 没用 mlock，所以本来就能编（无需改动，已验证）。

### 3. AW_TRNG —— 补缺失头文件

`bsp/drivers/ce/sunxi_trng/sunxi_trng.c` `#include <sunxi-trng.h>`，但这个头文件
**没随树发布**（所有 `TRNG_*` 寄存器宏都在 .c 里自己定义了，真正缺的只有驱动的
私有结构体）。新写 `include/sunxi-trng.h`：

```c
struct sunxi_trng {
	struct device		*dev;
	struct platform_device	*pdev;
	struct resource		*res;
	struct clk		*clk;
	struct reset_control	*reset;
	struct hwrng		*hwrng;
};
int sunxi_trng_exstract_random(u8 *trng_buf, u32 trng_len);
```

**注意**：`wait_field_equ()` 不要自己写 —— 它已经存在于
`bsp/include/sunxi-bitops.h:211`，头文件里 `#include <sunxi-bitops.h>` 即可
（第一版重复定义会撞车）。

## 附带发现：rpmsg 系列在本树**根本编不过**（已回退为 n）

把 `AW_RPMSG_PERF_TRACE=y` 打开后（它 select `AW_AMP_TIMESTAMP`，所以连带
`AW_RPMSG_VIRTIO` 也得开），暴露出**两个与本任务无关的厂商遗留移植缺陷**：

| 文件 | 错误 | 原因 |
|---|---|---|
| `aw_virtio_rpmsg_bus.c:953` | `implicit declaration of 'rpmsg_chrdev_register_device'` | 该函数在 6.6 已被 `rpmsg_ctrldev_register_device` 取代 |
| `rpmsg_master.c:1006` | `class_create` 参数不匹配 | 6.6 把 `class_create(owner, name)` 改成了单参数 |

处理：**把 `AW_RPMSG*`、`AW_RPBUF*` 九项在两份配置里都回退为 `n`**，保持构建
绿色。理由：

1. 修这两个属于 6.6 API 移植工作，超出本轮范围；
2. **BMI088 方案（SPI3 + 共享 SRAM 环形缓冲 + MSGBOX 通知）完全不需要 rpmsg**；
3. `rpmsg_perf.o` 本身已验证能编（配置同步后），证明 `AW_AMP_TIMESTAMP` 的
   导出符号链路是通的。

## 验证证据

```text
✅ bsp/drivers/misc/amp_timestamp.o   208240 bytes
✅ bsp/drivers/lradc/sunxi-lradc.o    324848 bytes
✅ bsp/drivers/ce/sunxi_trng/sunxi_trng.o  265056 bytes
✅ bsp/drivers/gpadc/sunxi_gpadc.o    664192 bytes
✅ dtb 含 amp-timestamp@8010000 节点
（全量内核编译：另行后台验证，见 verify-logs/kbuild-*.log）
```

编译方式（供复现）：

```bash
cd /home/helios/Desktop/orangepi-build/kernel/orange-pi-6.6-sun60iw2
export ARCH=arm64
export PATH=/home/helios/Desktop/orangepi-build/toolchains/gcc-arm-9.2-2019.12-x86_64-aarch64-none-linux-gnu/bin:$PATH
make CROSS_COMPILE=aarch64-none-linux-gnu- bsp/drivers/misc/amp_timestamp.o
```

**踩过的坑（复现时注意）**：

1. `kernel/.config` 与 `userpatches/*.config` 是**两份**文件 —— 只改 userpatches
   不够，定向编译用的是 `kernel/.config`；改完要 `make olddefconfig`。
2. 工具链是 `gcc-arm-9.2-…-aarch64-none-linux-gnu`，**不是** `aarch64-linux-gnu-`。
3. 新增 Kconfig 选项后 `olddefconfig` 会吃掉交互提示（`</dev/null`）。
4. 内核代码禁浮点：`-mgeneral-regs-only`，格式化毫秒要用整数除法。

## 下一步

1. 全量编译结果确认（后台进行中）。
2. 板子恢复上电后：刷入带 pacing 修复的固件（`fw-out/scp-ours-padded-105912.bin`
   sha256 `e9a361d2…`，**记得 `fix-bootpkg-sum.py --write`**）。
3. 驱动上板验证：`/sys/.../amp-timestamp@8010000/counter` 与 ARM 侧
   `CLOCK_MONOTONIC` 对照（期望恒定偏移、零漂移）。
