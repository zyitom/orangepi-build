# E902 小核（A733 CPUS 域 SCP）

Orange Pi Zero 3W（全志 A733）CPUS 域里有一颗玄铁 E902 RISC-V 小核，负责电源、休眠和 DRAM 调频。
它上面跑的固件是 SD 卡启动包里的 `scp` 条目，由 boot0 载入、bl31 放复位，还必须和 bl31 完成 mailbox 握手，
否则板子起不来。本目录包含：怎么构建和替换这份固件、为此做过的全部逆向和实测记录，以及 Linux 侧的配套驱动。

## 现状（2026-09-24）

- **板上已换成厂商源码构建的固件**（`vendor-scp/`，sha256 `302deda8…`）。2026-09-24 刷入后已连续重启 3 次，每次 bl31 都报
  `arisc version: [0170020e…-dirty]` 并完成握手；R-CCU 定时器时钟寄存器读到 `0x41`（mux 4），说明 26 MHz 修正已生效（台账 T37）。
  固件本身基于全志公开的 Tina 5.0 `arisc` + `dramlib`，打了 3 个补丁，
  其中最关键的一个把 S_TIMER 的时钟源从 26 MHz 的 DCXO 改到 24 MHz 的 SYS_CLK24M。
  这样拿到了厂商的完整电源、休眠和 DRAM 调频功能，同时修掉出厂镜像定时快 8.3% 的问题。
- 自研固件 `e902-fw/` 保留作为实验平台，已在板上验证过：中断延迟 0.04–0.38 µs、mailbox 往返中位 4.1 µs、
  两核共用 24 MHz 时间戳（台账 T31–T35）。
- Linux 侧 `linux/amp_timestamp`：在 Linux 上读两核共用的时间戳计数器，跑在 RT 内核上。

## 目录

```
e902/
├── README.md            本文件
├── FLASHING.md          ★ 烧录 / 回滚操作手册
├── vendor-scp/          ★ 厂商 SCP 构建：build.sh + patches/（源码固定到上游提交）
├── tools/               bootpkg.py（启动包条目读写 + add_sum）、flash-scp.sh（在线）、
│                        flash-scp-reader.sh（读卡器）、capture-e902-console.sh、preflight-image.sh
├── e902-fw/             自研 SCP 固件（make scpfw / make hosttest），scripts/ 里是板上调试工具
├── linux/               amp_timestamp 驱动 + DT overlay
├── tests/               板上测试（tests/board/）与回滚演练脚本
├── doc/e902/            逆向与实测记录：FINDINGS-LEDGER.md（台账 T1–T37）、RESOURCE-MAP.md、RISKS.md …
├── doc/delivery/        交付文档（复现与回滚手册、资源白皮书）
├── verify-logs/         上板串口日志
├── fw-out/SHA256SUMS    已刷过的镜像哈希记录（镜像本身不入库）
└── legacy/              早期方案：最小 SCP 雏形 e902-fw-scp/、固定槽位的刷写/恢复脚本（留作参考）
```

## 本地目录（不入 git，见 `.gitignore`）

| 目录 | 内容 | 怎么得到 |
|---|---|---|
| `toolchains/` | Xuantie GCC 3.2.0 | `bash e902/e902-fw/scripts/get-toolchain.sh` |
| `vendor-ref/` | 全志 `arisc`、`dramlib` 源码（无 LICENSE，不能再分发） | `bash e902/vendor-scp/build.sh` 自动拉取 |
| `opene902/` | 玄铁开源 E902 RTL 与文档 | `git clone https://github.com/XUANTIE-RV/opene902` |
| `backup/` | SD 卡头 24 MiB 备份、板子 `/boot` 备份 | 刷写脚本每次写卡前自动生成 |
| `doc/*.pdf`、`doc/vendor-v1/` | A733 手册、datasheet、原理图、E902 手册 | 全志 GitLab `product/docs`、Orange Pi 官网、玄铁官网 |
| `fw-out/*.bin` | 构建产物 | `vendor-scp/build.sh` / `make -C e902-fw scpfw` |

## 必须知道的硬件事实（都实测过）

1. **本板晶振（DCXO）是 26 MHz**，不是厂商代码注释里写的 24M。需要 24 MHz 的地方一律用
   SYS_CLK24M / pll-ref（R-CCU mux 4）。
2. **改 `scp` 条目后必须重算启动包的 `add_sum`**，否则 boot0 拒绝启动包，板子进 FEL。`tools/bootpkg.py` 会处理。
3. **bl31 的 mailbox RPC 没有超时**。自研固件必须轮询 ch3、同步请求原样回显，不能发未经请求的包（台账 T29/T30）。
4. 软件侧**没有** E902 内核复位位，唯一的启动路径是 boot0 载入 + bl31 放复位（台账 T9–T12）。
5. S_TIMER 寄存器布局是 `0x07091000 + 0x20*(n+1)`（不是手册写的 0x10 步长），每个定时器还要单独打开
   `R-CCU 0x100+4n` 的时钟。

## 还没做的

- 厂商固件其余回归：`poweroff` 后上电、休眠/唤醒（`/sys/power/mem_sleep` = deep）、DRAM 调频。这几项都需要有人在板子旁边。
- 厂商固件不往小核串口打印任何东西（boot0 报 `dtb not found for scp`，出厂固件也一样），所以小核运行时的状态目前看不到。
- GINTC（GIC → E902 中断转发）的基址，手册和源码里都没有，所以大核外设的中断还转不到 E902。
- 让 orangepi-build 打包镜像时直接用 `vendor-scp` 的产物（目前还是
  `external/packages/pack-uboot/sun60iw2/bin/scp.fex` 出厂件）。
- 用 orangepi-build 重新编一版内核并装到板上：amp_timestamp、RT、AR0234 现在都在 `userpatches/kernel/sun60iw2-current/`
  （0014 是 amp_timestamp；2026-09-24 已在厂商最新基线上确认能打上、能编译），装好之后就不再需要树外模块和 overlay 了。

## 第一次看这些文档，建议顺序

1. 本文件 → `FLASHING.md`
2. `doc/e902/CORRECTION-2026-09-22.md`（先清掉早期文档里的错误认识）
3. `doc/e902/RESOURCE-MAP.md` → `doc/e902/FINDINGS-LEDGER.md`
4. `doc/e902/HANDOVER-2026-09-23.md`（自研固件阶段的交接快照）
