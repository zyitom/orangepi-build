# OPI5 原生 IgH EtherCAT（ec_dwmac_rockchip）

2026-10-18 完成并上板验证。把 IgH EtherCAT Master 1.6.13（stable-1.6 分支）的
**原生** stmmac/dwmac-rk 驱动移植到 OPI5 的 6.1.99-rt36 BSP 内核，替换掉
`ec_generic`（packet socket）路径——EtherCAT 帧直接走 stmmac DMA。

## 上板验证结论（2026-10-18）

```
Master0
  Phase: Idle
  Main: 00:00:a4:0a:65:eb (attached)
      Link: UP                                  ← 原生 PHY 路径
      Tx frame rate [1/s]: 499                  ← IDLE 线程 ~500 datagrams/s 全走原生 DMA
```

dmesg 关键链：`Accepting ... as main device for master 0` →
`Starting EtherCAT-IDLE thread` → `deferred EtherCAT device open completed` →
`ecm0 (uninitialized): Link is Up - 1Gbps/Full`。Rx=0 是预期（口上没接从站）。

## 架构

单模块 `ec_dwmac_rockchip.ko` = 官方 ethercat 化 stmmac core（26 个编译单元）
+ 平台 DT 解析 + dwmac-rk RK 胶水 + dwmac-rk-tool，全部**内部链接、零
EXPORT_SYMBOL**——内置驱动导出的 15 个同名符号（stmmac_dvr_probe 等）不会
遮蔽本模块的调用；当前 STMMAC=y 内核上靠 sysfs unbind/bind 接管，未来 =m
内核上靠 blacklist 干净接管（userpatches 配置已改）。

三个成分的来源与加工（全部由 `gen-port.sh` 可重复生成）：

1. **core**：官方 stable-1.6 的 `devices/stmmac/*-6.1-ethercat.*`（mainline
   6.1.99 + ecdev 变换，与 BSP 核心逐字节等价——20/26 个文件无任何厂商改动），
   backport BSP 相对 v6.1.99 的 ~140 行厂商增量（get_eth_addr /
   integrated_phy_power / flow-ctrl / tx-rx-dma-size / mdio_idle /
   STMMAC_FULL 开关 / napi weight）。
2. **胶水**：BSP 的 `dwmac-rk.c`（1808 行厂商差异，整体拷入）+
   `dwmac-rk-tool.c` + `stmmac_platform.c`，只做机械变换：include 指向
   ethercat 头、`stmmac_dvr_probe/remove → stmmac_ec_*`、
   `stmmac_bus_clks_config → ec_stmmac_bus_clks_config`、
   `STMMAC_RESOURCE_NAME → "stmmaceth"`、驱动名 `rk_gmac-dwmac-ec`、
   `module_platform_driver` → 自定义 init/exit（先 `stmmac_init()`）。
3. **延迟 open**（VJW rk3588-igh-native-stmmac 项目的结论）：platform probe
   上下文不能直接 `ecdev_open`，延迟 100ms 到 workqueue 里开。

## 日常使用（板上）

```bash
sudo ecat-native start    # eth0 消失、SSH 断开（单口板代价）；串口/WiFi 继续
sudo ecat-native status
sudo ecat-native stop     # 恢复 eth0，NM 自动重连
```

模块加载顺序是硬约束：**必须先 `modprobe ec_master main_devices=<MAC>` 再
`modprobe ec_dwmac_rockchip`**——顺序反了依赖解析会把主站无参拉起，
`master_count=0`，offer 无人接收。`ecat-native` 已处理。

- MAC 用 `00:00:a4:0a:65:eb`（RK serial 派生基址，跨重启稳定）。ec 模式不注册
  netdev，派生 id 不递增，**ec 设备 MAC 恒为 :eb**；但一次开机内 stock 驱动每次
  rebind 会递增（:ec/:ed/:f0…），所以不要读 eth0 当前 MAC 来配置主站。
- `/etc/ethercat.conf`：`MASTER0_DEVICE="00:00:a4:0a:65:eb"`,
  `DEVICE_MODULES="dwmac_rockchip"`（标准 ethercatctl 路径也配好了）。
- `/dev/EtherCAT0` 由 udev 规则 `99-ethercat.rules`（MODE 0666）放开权限。
- 从站应用按 IgH 常规方式跑（libethercat 在 /usr/local，`ethercat` 工具已装）。

## 真实从站验证（2026-10-18，意优 PHU20 关节电机）

接一台意优 EYOU_ServoModule（PHU20，Vendor 0x1097 / Product 0x00010002，
与 `docs/motors/eyou-PHU` ESI 完全一致）在原生主站上全链路实测：

- 总线扫描/SII：`0 0:0 PREOP + EYOU_ServoModule`，DL 信息、64bit DC 能力、
  CoE+FoE 邮箱全部正确；`ethercat pdos` 读出标准 CiA402 CSP 映射
  （SM2 输出 33B：6040/6060/607a/6081/60ff/240d/6071/6083/6084/6087；
  SM3 输入 29B：6041/6061/603f/6064/606c/6077/6074/60f4/6079/60fd）。
- 邮箱：SDO Info 完整枚举对象字典；SAFEOP 下 `ethercat upload 0x6041` 读回
  0x03d0（Switch-on disabled）。
- 数据面：IDLE+SAFEOP 累计 >94 万 datagram，**Lost frames = 0**。
- **应用级 OP**（`ecat_test.c`，SII PDO 配置 + 500µs 周期 + 显式 OP 请求，
  controlword 恒 0x0000 永不上电）：约 1s 内进 OP，之后 60000/60000 周期
  wkc=3 满分零丢帧，位置/速度反馈实时更新（编码器静止噪声 ±几个 count），
  statusword 全程 0x03d0。
- 教训（对 xbot 联调有用）：IDLE 主站直接请求 OP 会被驱动器拒（SAFEOP+ERROR，
  超时 10s）——**必须有应用做 PDO 配置后再走 OP**，与
  `docs/ethercat-startup-sdo.md` 的启动流程一致；DC（ESI 0x0300 SYNC0）下
  驱动器要求先建立 DC 同步才肯走 SAFEOP→OP，应用必须显式请求 OP 并配 DC。
- 遗留：周期抖动用专门插桩/cyclictest 另测（本轮两次抖动数字被 DC 失败半程
  污染，不作数）；`ecat_test.c` 已在板上 /tmp，源码在本目录。

## 底层优化落地与 CAN-FD bring-up（2026-10-18 第二轮）

**已完成的优化（全部实测）**：

| 项 | 手段 | 实测效果 |
| --- | --- | --- |
| 周期唤醒抖动 | 测试应用尾部自旋（提前 3µs 睡醒自旋到点） | mean 2000ns → **131ns**，max 9.4µs → 3.0µs |
| sync monitor 竞态 | queue 挪到 process 后/send 前 | 漂移连续测量 mean 219ns / max 2.7µs，UNMATCHED=0 |
| GMAC 中断亲和 | ecat-native start 内置（IRQ 挪离 cpu4，mask=f） | 随每次 start 生效 |
| xbot dc_sync_state 时序 | 核查结论：**本来就正确**（receive 后采样、send 内重挂），无需修改 | — |

**CAN-FD bring-up（can0=fea60000、can1=fea70000，均 FD+loopback 验证通过）**：
DTS 使能两步走——正式补丁已入
`userpatches/kernel/rockchip-rk3588-current/0003-dts-orangepi5-can1-can2-canfd.patch`
（以后内核构建自动带上）；本轮用 dtc 反编译→改→重编的方式先行验证。测试：
`ip link set can1 type can loopback on bitrate 500000 dbitrate 2000000 fd on` +
cansend 经典帧和 FD 帧（##5 BRS）内环回全部收到。

**新踩的两个 BSP 坑**：

1. **compatible 决定 CAN 模式**：BSP 设备树的 `"rockchip,can-2.0"` 在
   rockchip_canfd 驱动里映射为**经典 CAN 模式**（无数据相位、无 FD/loopback，
   `ROCKCHIP_CAN_MODE`）；FD 能力要用 **`"rockchip,canfd-1.0"`**
   （`ROCKCHIP_CANFD_MODE`）。IP 是 FD 的，compatible 绑错就全废——症状：
   loopback Invalid argument、cansend 报 not CAN FD capable。
2. **重启后 udev 冷插拔陷阱**：ec 模块会被无参自动拉起（master_count=0，
   /dev/EtherCAT0 不出现），且 ec 驱动可能抢先绑定设备——`ecat-native start`
   已改为清场式（双向 unbind → rmmod → 带 MAC 重载 → bind），兼容任意前置状态。

**rescan 坑（未修，规避）**：从站枚举期间打断 rescan 会把主站的从站 SII/PDO
信息留成空（后续应用报 "PDO entry not mapped"），恢复 = 重载主站模块。规则：
运行期不发 rescan；rescan 后必须确认 `ethercat pdos` 非空再继续。

**遗留**：STMMAC =m 内核重编——**deb 已编出并验证**（2026-10-18 见下节），
板上安装因串口事故待板子重启后自动完成（`finish_install.py` + 定时任务）；
isolcpus/nohz_full（tune_rt 决策，未应用）；PTP 硬件时钟纪律 DC（单板无收益，
多板时间基准阶段再做）；上游 MR（方案冻结后整理）。

## STMMAC =m 内核重编（2026-10-18，deb 已产出待上板）

`userpatches/linux-rockchip-rk3588-current.config` 四项 `=m`（STMMAC_ETH/
STMMAC_PLATFORM/DWMAC_ROCKCHIP/DWMAC_ROCKCHIP_TOOL——最后一项是 **bool**，
olddefconfig 会归回 `=y`，`dwmac-rk-tool.o` 随 `dwmac-rk.ko` 模块一起编入，
无害）；DWMAC_GENERIC 显式改 `=m`（依赖 STMMAC_PLATFORM，不能保 `=y`）。

```bash
# 构建入口（约 1 分钟，ccache 命中；配置变更只重编受影响文件）
docker run --rm --cap-add=SYS_ADMIN --cap-add=MKNOD --cap-add=SYS_PTRACE \
  --security-opt=apparmor:unconfined --device /dev/loop-control \
  --device-cgroup-rule='b 7:* rmw' --device-cgroup-rule='b 259:* rmw' \
  -v /dev:/tmp/dev:ro -v /home/zyi/Desktop/orangepi5-build:/root/orangepi \
  -v orangepi-cache:/root/orangepi/cache -v orangepi-ccache:/root/.ccache \
  -e COLUMNS=200 -e LINES=50 -e TERM=dumb \
  orangepi:latest opi5-rt-full BUILD_OPT=kernel IGNORE_UPDATES=yes
```

产物验证：deb 内 `stmmac.ko/stmmac-platform.ko/dwmac-generic.ko/dwmac-rockchip.ko`
四个模块 + modules.dep 依赖链完整；内嵌 config 确认 `=m`。旧 deb 备份在
`output/debs-backup-20261018/`。

## 串口传输与 FIQ debugger 陷阱（2026-10-18 事故记录，必读）

RK `fiq_debugger` 的输入状态机（fiq_debugger.c:940）：**任意 CR/LF 会把
debugger 置为 armed**；armed 后收到 **UART BREAK 线路条件**（不是字节值，
`FIQ_DEBUGGER_BREAK=0x00ff0100`）即劫持控制台进 `debug>` shell。1.5Mbaud 下
持续二进制流会诱发接收端成帧错误 → 被当成 BREAK → 劫持。

- `debug>` 里敲 `console` 回车可退出劫持；但一次 XMODEM 二进制传输（~539KB 处）
  之后 **FIQ 控制台整体卡死**（输入彻底无响应、唤醒洪水只能冲出缓冲的旧内核
  消息）——本日实际发生，只能断电重启。教训：**永远不要往控制台灌原始二进制**。
- 安全做法 = 全可打印行协议 + 逐行 ACK：主机 `xfer2.py`（base64 行 + CRC16 +
  'K'/'N' 确认 + 卡死自动发 `\nconsole\n` 逃逸），板端 `/tmp/rx2.py`
  （raw termios 接收）；小文件用 `push_file.py`（55 字符 base64 行 echo 推入）。
  `finish_install.py` 把"探测板子恢复→推接收器→传 3 个 deb→dpkg→重启→验证
  eth0/ecat-native→回归"串成幂等流程。
- **实测 console 的硬上限：一个 tty 会话只能承受 ~181 块（~141KB）持续 TX**，
  之后 FIQ 控制台整体卡死（输入死、僵尸接收进程杀不掉——它的输入路径先死，
  'Z' 退出符必须前置一个换行冲掉残行才可能送达）。解决方案 = v4 窗口化协议：
  接收器支持 append 续传（seq 从文件大小恢复，`RX2READY <seq>` 汇报），
  发送端每 ~150 块重启接收器；文件是唯一真相源，跨会话跨运行均可续传。
  ACK 带接收端累计状态（`\nK%04x\n`，base64 载荷不可能含换行所以无法伪造），
  发送端全窗口扫描、按状态跳转/回退，永不失步。
- **不要用降波特率 0x00 合成 BREAK**：它能进 debug shell（nosleep 可达），
  但会毒化控制台输入路径（ring 不再排水，接收器 ~1KB 后冻结）。pyserial 的
  break_condition 则根本到不了板子（CH340）。
- **串口是最后的手段，不是首选**：正确通道是 SSH——板子 eth0 已配 10.0.0.2/30，
  主机 `nmcli con up static-172`（enp44s0，10.0.0.1/30）+ 公钥已授权，
  `finish_ssh.py`（scp 3 个 deb 秒级 + dpkg + 重启 + 验证 + 回归全自动）。
  两个 finisher 共享锁与 `/tmp/opi5_finish.done` 标记，cron 每 10 分钟双通道
  自愈。
- 串口粘贴 >80 字符行会被控制台回折/插 CR 弄脏：交互命令一律 ≤70 字符。
- 网络现状：HANDOFF 记录的 10.0.0.2/30 直连网线当前不在主机 enp44s0 上
  （载波 DOWN，static-172 配置仍在）；主机 USB 网卡是 **Jetson gadget**
  （0955:7020），与 OPI5 无关。板子 eth0 现插在某个 100M 交换机上，与主机无
  L2 路径——串口是唯一通道。

## 特性覆盖矩阵（最终版，2026-10-18）

**已测全通过**（原生栈 + 真实 PHU20）：

- IgH 库层：总线扫描/SII、SDO Info 枚举、SDO 上传（含字符串/固件版本）、
  **SDO 下载**（配置期 sdo16/32 实测生效 + 运行期邮箱写 0x6071=0 十周期完成、
  运行期读 0x1008 回读 "PHU20 "）、**CompleteAccess 下载机制**（`--ca` 开启；
  机制通但写只读对象驱动器容忍不稳定——实际只用于 RW 对象）、PDO(SII) 配置 +
  500µs 周期交换、DC 全链（config_dc + application_time + sync_reference/
  slave_clocks + monitor，OP with DC）、全状态机 + 错误恢复、运行期状态查询、
  主站 debug 数据报环。
- 内核/驱动层：ec_dwmac_rockchip 多轮接管/回滚、百万 datagram 零丢帧、
  故障注入恢复、满载压力、reg_read（AL 0x134）、rescan（SII 异步重读，
  重读期间身份显示 0 为瞬态）、crc 诊断（总线 CRC=0）。
- 工具层：slaves/-v、pdos、sdos、master、upload、reg_read、rescan、crc、
  states、debug、foe_read、foe_write 全部实际使用通过。
- 回归：`regression.sh` 12/12 PASS（xbot 同款调用序列：`sync_reference_clock_to`
  变体已并入测试应用）。

**设备不声明 → 不适用**：EoE、SoE、VoE（EYOU ESI 仅 CoE+FoE）。
**本环境不可测**：BOOT 态（需驱动器断电恢复）、真固件 FoE 传输、断电重上电、
29 从站整机拓扑（单台样机）。
**xbot 层**：`IghBackend::send()` 的 DC API 序列与应用级验证完全一致（源码核对
+ 同款调用运行时通过）；联调前在 robot YAML 的 CoE startup 声明补
`0x1c32/0x1c33:01 = 2`（DC 模式必需，见 DC 四坑）。xbot 构建需 gcc14/cmake3.28
（板上 gcc11.4 不满足，走其原有 docker/交叉构建流）。

## 全功能回归矩阵（2026-10-18，回归脚本 12/12 PASS）

`regression.sh`（板上 /tmp，推板方式同前）：调优(=xbot tune_rt 思路: performance
调频 + cpu4 大核 + SCHED_FIFO 80)后跑，全部通过：

| 项 | 结果 |
| --- | --- |
| T1 DC+OP 30s | OP 1.5s 达成，之后 **60000 周期 wkc=3 零丢帧**，唤醒抖动 mean 2.0µs/max 9.2µs，单周期工作 mean 8.5µs |
| T2 满载 DC+OP | 6×sha256 满载下 OP 0.45s 达成，零丢帧，抖动 mean 2.5µs |
| T3 故障注入 | OP 中 kill -9：内核主站干净关闭（从站 PREOP、无锁存），重跑 0.45s 回 OP |
| T4 邮箱 | SDO 字符串/固件版本读回；FoE 握手正常（对不存在文件回 0x8002 abort） |
| T5 环境 | governor 测试后自动还原 ondemand |

**DC 集成的四个坑（xbot/xbot_io_ecat 必读）**——全部踩过并验证：

1. **`ecrt_slave_config_dc(sc, 0x0300, 500000, 0, 0, 0)` 只写 ESC 寄存器**；
   意优 0x1c32/0x1c33:01（Synchronization Type，ESI 默认 1=SM 同步）必须由应用
   在 **PreOP 阶段写 2（DC 同步）**，否则驱动器 SAFEOP→OP 拒绝。
2. **0x1c32:02（Cycle Time）是只读**（设备从 ESC SYNC0 取周期），最小 500000ns；
   千万别往 0x1c32/0x1c33 写周期值——会把 ：01 类型字段污染，连 SAFEOP 都进不去。
3. **每周期必须 `ecrt_master_application_time(master, t)` + `sync_reference_clock`
   + `sync_slave_clocks`**：app_time 只由应用喂（activate 时清零！），不喂则参考
   从站收到冻结时间 → DC 锁不上 → OP 拒绝（AL 0x2D "No Sync Error" +
   "N datagrams UNMATCHED" 刷屏）。t 开环每周期 +500000。
4. **用户态周期线程的环境调优是硬要求**：ondemand+深 idle 下唤醒抖动 mean
   118µs（DC 必挂）；performance + 大核 + FIFO 后 mean 2.0µs / max 9µs。
   即 tune_rt.py 的等效动作。

**已修复（2026-10-18）sync monitor 竞态**：`sync_monitor_queue` 原放在 send 之后，
每周期把 SENT 态数据报重新入队（IgH queue_datagram 注释明确警告的场景）→ 每周期
1 条 UNMATCHED + 漂移恒 0xFFFFFFFF。修复 = queue 挪到 process 消费之后、send 之前。
验证：59761/60000 有效漂移样本，**mean 219ns / max 2.7µs**，UNMATCHED=0。xbot 的
`dc_sync_state()`（IghBackend monitor_queued 路径）注意同样的入队时序。DC 握手期
wkc=0 约 1.5-3s（之后零丢帧）。
回归阈值按 5000 周期设置。CLI 无 CompleteAccess 参数（库级功能）。

## 移植过程中踩掉的三个坑（复现时注意）

1. **`STMMAC_RESOURCE_NAME`**：官方 ethercat 头把它改名 `"ec_stmmaceth"`（PCI
   变体无 DT 时钟无所谓）。平台设备的 DT clock/reset con_id 是 `"stmmaceth"`，
   不还原则 CSR 时钟 get 失败 → MAC 时钟不开 → 寄存器读 0（`Version ID not
   available`）→ hw 特性探测残废 → open 时 phylink 校验 -EINVAL。
   症状对照：`User ID: 0x30, Synopsys ID: 0x51`=好，`Version ID not available`=坏。
2. **master_count**：`modprobe ec_master` 必须带 `main_devices=<MAC>`，否则
   0 个主站实例（"0 masters waiting for devices"），offer 静默丢弃。
3. **sysfs bind 语义**：`unbind` 不会自动触发其他驱动 reprobe，必须显式
   `echo fe1c0000.ethernet > /sys/bus/platform/drivers/rk_gmac-dwmac-ec/bind`。

## 文件

| 文件 | 作用 |
| --- | --- |
| `gen-port.sh` | 可重复生成器：pristine tarball → 全部移植文件 + 自检 |
| `igh-stable-1.6.tar.gz` | IgH stable-1.6 上游存档（gitlab archive，6363afe9） |
| `src/` | 移植后的 IgH 树（可直接删，gen-port.sh 会重建） |
| `patches/backport-*.patch` | BSP 厂商增量（v6.1.99 → BSP）的 diff 记录 |
| `native-test.sh` | 上板自动化验证（nohup 脱离跑 + 自动回滚） |
| `ecat-native.sh` | 正式 start/stop/status 包装（板上 /usr/local/sbin/ecat-native） |

## 重新构建

板上增量（内核没变时）：源码在板上 `~/ecat-build/src`，
`make -j6 modules && sudo make modules_install && sudo depmod -a`。

全量重生成（gen-port.sh 改过或内核换版）：主机跑 `./gen-port.sh`（依赖
/tmp/v6199 的 mainline v6.1.99 基准文件，脚本里有 URL）→ `./bootstrap` →
打包传板 → 板上 `./configure --with-linux-dir=/lib/modules/$(uname -r)/build
--enable-dwmac-rk --with-stmmac-kernel=6.1 --disable-8139too --disable-generic
--prefix=/usr/local && make -j6 modules && sudo make modules_install && sudo depmod -a`。

## 内核配置（下次编内核生效）

`userpatches/linux-rockchip-rk3588-current.config` 已把
`STMMAC_ETH/STMMAC_PLATFORM/DWMAC_ROCKCHIP/DWMAC_ROCKCHIP_TOOL` 改成 `=m`。
模块形态下 OF 别名仍会自动加载 dwmac-rk，eth0 日常行为不变；跑 EtherCAT 前把
`dwmac-rk`/`stmmac_platform` 卸载或 blacklist 即可让 ec 驱动开机接管（那时
ecat-native start 的 unbind 步骤会变成 no-op，无害）。

## 已知限制与兜底

- **单口板代价**：主站运行期间 eth0/netdev 不存在，SSH 调试通道断开。兜底：
  串口（/dev/ttyUSB0，1500000 8N1）、WiFi/USB 网卡、或直接 reboot（内置驱动
  =y，开机必恢复 eth0）。
- **未接从站**：IDLE 下 Rx=0、datagram 100% 超时是预期。接从站后
  `sudo ethercat slaves` 扫描。
- **实时性参考**：社区 RK35xx 实测 generic 1ms 周期 vs 原生 ec_stmmac 125µs
  周期（cnblogs wsg1100）；VJW 项目在 RK3588+RT 上原生 1ms 周期 A/B 数据
  TX mean 3.31µs→1.81µs。本板暂未做周期实测，跑从站应用时用 cyclictest 另测。
- 参考：官方仓库 gitlab.com/etherlab.org/ethercat（stable-1.6/1.7 目前 stmmac
  只有 PCI 入口，platform 入口从未合并）；github.com/VictorJiaxinWang/
  rk3588-igh-native-stmmac（5.10-rt89 完整配方与排错记录，本移植的方法论来源）。
