# 厂商 SCP 固件（E902）—— 从公开源码构建

板子上 E902 小核跑的 SCP 固件（`scp.fex`），改为用全志公开的 Tina 5.0 源码自己编译，而不是用出厂二进制，
也不是 `e902-fw/` 里的自研固件。

```bash
bash e902/e902-fw/scripts/get-toolchain.sh   # 第一次：下载 Xuantie GCC 到 e902/toolchains/
bash e902/vendor-scp/build.sh                # 拉源码 → 打补丁 → 编译 → e902/fw-out/vendor-scp.bin
bash e902/tools/flash-scp.sh                 # 写进板子 SD 卡并重启（见 ../FLASHING.md）
```

## 源码来源（固定版本）

| 仓库 | 提交 | 用途 |
|---|---|---|
| `gitlab.com/tina5.0_aiot/lichee/arisc` | `0170020e`（Tina 1.5.0 snapshot） | SCP 固件源码，`ar100s/` |
| `gitlab.com/tina5.0_aiot/lichee/dramlib` | `7142734a`（aiot-linux-v1.5.0） | 预编译 `libar100s.a`（3 个 OEM DRAM 函数，无源码） |

两个仓库都拉到 `e902/vendor-ref/`，不进 git：arisc 仓库**没有 LICENSE 文件**，不能再分发。
所以本仓库只放我们的补丁和构建脚本，产物 `fw-out/vendor-scp.bin` 也不入库。

## 补丁（`patches/`）

| 补丁 | 原因 |
|---|---|
| `0001-build-fix-for-gcc-14.patch` | 厂商按 2020 年的 GCC 写的；GCC 14 下需要 `-Wno-error=deprecated -fcommon`。另外 `rv32emc` 须写成 `rv32emc_zicsr_zifencei`，这一项由 `build.sh` 通过 `MARCH_FLAGS` 传入 |
| `0002-timer-extended-clock-from-SYS_CLK24M.patch` | 厂商把 S_TIMER 的时钟源设成 mux 0（DCXO），并假设它是 24 MHz。**本板 DCXO 是 26 MHz**（原理图 Y1、datasheet 5.8.1、XO_CTRL 实测），不改的话所有毫秒定时都会快 8.3%。改成 mux 4 = SYS_CLK24M（手册 V1.00 4.2.5.4），字段宽度也从 2 位改成 3 位 |
| `0003-add-orangepi-zero3w-defconfig.patch` | 在 `sun60iw2p1_defconfig` 基础上去掉 `AXP517`，和出厂 `scp.fex` 的配置对齐（出厂镜像里只有 AXP8191/AXP515 的字符串，没有 AXP517） |

## 与出厂镜像的差别

| | 出厂 `scp.fex` | 本构建 |
|---|---|---|
| 大小 | 105912 B | 120856 B（新版 dramlib 带 LPDDR5 表，`.data` 约 64 KB） |
| S_TIMER 时钟 | DCXO（26 MHz，快 8.3%） | SYS_CLK24M（24 MHz） |
| SRAM A2 占用 | 到 `0x4002AF3C` | bss 到 `0x4002D084`；栈在 `0x4002EC00`，仍有余量 |
| 3 个 OEM DRAM 函数 | 与 dramlib 逐字节同源（台账 T36） | 同 |

镜像比出厂的大，放不进原来 105912 B 的槽位。`tools/bootpkg.py` 会同时改 `scp` 条目长度和启动包的
`valid_len`（0x154000 → 0x158000），并重算 `add_sum`。`scp` 是启动包里的最后一个条目，而卡上启动包
后面是空白，脚本写之前会检查这两点。写回出厂镜像后，卡头与原始备份逐字节一致（已离线验证）。

## 自检

`build.sh` 最后会打印 bss 结束地址，这个地址必须低于栈底 `0x4002EC00`。确认时钟补丁已经编进去：

```bash
riscv64-unknown-elf-objdump -d e902/fw-out/vendor-scp.elf --disassemble=timer_init | grep -E 'andi.*-113|ori.*64'
```
