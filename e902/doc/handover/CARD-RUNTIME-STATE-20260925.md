# noble 测试卡运行时状态快照（2026-09-25，交板前）

卡上 1.0.2 noble 镜像 + 手工应用的运行时修复。**卡交给他人刷其他系统后这些手工状态会丢**；
新的 1.0.2 noble 镜像（output/images/...0017 版）已把其中大部分烘进镜像，重刷即得。

## 卡上手工状态（镜像之外的）

| 项 | 位置 | 新镜像是否已含 |
|---|---|---|
| RT 启动参数 isolcpus=5 nohz_full=5 rcu_nocbs=5 | /boot/orangepiEnv.txt extraargs（备份 .bak-noble-rt） | ❌ 按卡手配（设计如此） |
| GPADC 静音 | /etc/modprobe.d/blacklist-sunxi-gpadc.conf | ❌ 手配（要用 ADC 口 modprobe sunxi_gpadc） |
| DTB 0016（禁幻影 pmu@34 / hym8563） | /boot/dtb/allwinner/…dtb（备份 .bak-phantom） | ✅ 镜像自带 |
| pinmux 空引脚清理 0017 | （DTB 内） | ✅ 镜像自带 |
| dnsmasq 禁用 | systemd | ✅ 镜像自带 |
| pam_lastlog 清理 | /etc/pam.d/login | ✅ 镜像自带 |
| rtla 真二进制 | /usr/local/bin/rtla（overlay 留档 userpatches/overlay/rtla/） | ✅ 镜像自带 |
| vendor E902 固件 | 启动包 scp 条目（md5 b9904524…） | ✅ 镜像自带 |
| 蓝牙解锁 / G2D | rfkill unblock bluetooth；modprobe g2d_sunxi | ❌ 按需手开 |

## 本文件同目录的归档（2026-09-26 已抓取）

- card-20260925/orangepiEnv.txt —— 含 RT 参数的启动环境
- card-20260925/blacklist-sunxi-gpadc.conf
- card-20260925/login.pam
- card-20260925/dmesg-final.txt —— 修复后的完整开机日志（axp515/hym8563 0 条）
- card-20260925/final-check.txt —— 最终体检输出

## 已知良性开机日志（无需处理）

- `pin-2000000.pinctrl: unknown pin` ×4 —— 0017 已修
- `ccu_ddr-2002000: failed to find dram_clk` —— **有意为之**：/dram 节点无 dram_para，Linux
  侧 DRAM 时钟驱动惰性退出；DRAM 调频归 SCP（dramlib），不要修（修了反而引入双主管）
- `twi-251c000 9th SCL timeout`（早期一次）—— gt9271 触摸屏幻影，0018 已修（见追记）

## 追记（2026-09-26 闭环）

- twi12 上的 9th SCL 超时 = gt9271 触摸屏幻影（补丁 0018 已禁）。
- rtla osnoise top 挂死 = rtla 循环建管道（strace 铁证），用 timerlat。
- 最终交接镜像含 0016+0017+0018；重启断电后 DTB 修复保持。
