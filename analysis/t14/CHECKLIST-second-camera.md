# 第二颗 AR0234 接线后：一页验证清单

前置：第一颗模组照旧在 MIPI-A（`mipi0`/cci 11/PE6）；第二颗插 **MIPI-B（`mipi1`）**，
i2c 接 **cci 9**（0x20），PWDN 走 **PE10**。板上 sudo 密码是空格。
主机在 TL101 的 `ar0234-port/`，板子 IP 用 `tools/ssh_board.sh`（默认 172.16.0.193）。

> ⚠️ **先插模组、再开 DT 节点**。反过来（节点 okay 而模组不在）会在 probe 阶段 panic
> 并进入 boot loop，而且那时候没有 ssh（见 REPORT §6.5）。

## 0. 接线前确认（不确认则后面全是空转）

```sh
# 两颗模组不能在一条 i2c 总线上（地址都是 0x20）
tools/ssh_board.sh "ls /sys/bus/i2c/devices/ | tr '\n' ' '"
# 期望：第一颗在 i2c-11（cci 11），第二颗那条总线是 i2c-9
# 用示波器/万用表确认 MIPI-B 的 4 条 data lane + 时钟、MCLK 24 MHz、PWDN=PE10
```

## 1. 打开第二路（DT 只改 fdtput，自动备份）

```sh
tools/put_board.sh tools/dt_second_cam.sh /tmp/dt_second_cam.sh
tools/ssh_board.sh "printf ' \n' | sudo -S -p '' sh /tmp/dt_second_cam.sh sensor2-on"
tools/ssh_board.sh "printf ' \n' | sudo -S -p '' systemd-run --on-active=2 /bin/systemctl reboot"
```

## 2. 重启后 60 s 内必须看到（否则停手）

```sh
tools/ssh_board.sh "printf ' \n' | sudo -S -p '' dmesg | grep -E 'ar0234|vinc8|sensor2|no sensor is bound'" < /dev/null
```
* ✅ 期望出现**两条** `[ar0234_mipi]V4L2_IDENT_SENSOR = 0xa56`（第一颗 + 第二颗）。
* ❌ 只有一条 + `vinc8 (device_id 8) needs sensor2 …` ⇒ 第二颗没被 i2c 探到：
  查线、查 cci 9、查 PE10 pwdn、查 MCLK。
* ❌ 出现 `Oops` / `Kernel panic` ⇒ 立刻 `tools/dual_dtb` 回退或断电，见 §6。

```sh
tools/ssh_board.sh "ls /dev/video*; media-ctl -d /dev/media0 -p | grep -c entity" < /dev/null
# 期望：/dev/video0 /dev/video4 /dev/video8 ；实体数 21 -> 更多（新增 vin_cap.8 / vin_video8）
tools/ssh_board.sh "media-ctl -d /dev/media0 -p | grep -A4 'sunxi_tdm_rx.1'" < /dev/null
# 期望：sunxi_tdm_rx.1 的 pad1 -> sunxi_isp.0:0 变成 [ENABLED]
```

## 3. 第一路零回归（必做）

```sh
tools/vfrrun.sh chk-cam1 60 -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120
```
判据：`RESULT … fps≈120 timeouts=0`、`GAPS … >1.5x=0 >3x=0`、
`vi0 frame cnt … lost_cnt 0 error_cnt 0`、dmesg `frame lost 0 / sunxi_isp_reset 0`。

## 4. 第二路单独跑（第一路先别开）

```sh
tools/vfrrun.sh chk-cam2 20 -d /dev/video8 -w 1920 -h 1200 -f NV12 -p 1/120
```
判据：`frames≈2400`、`vi8 lost_cnt 0`、dmesg 无 `frame lost`、无 `Oops/BUG/not mapped/CSI module`。

## 5. 两路同跑（先主路、后副路，顺序不能反）

```sh
tools/ssh_board.sh "printf ' \n' | sudo -S -p '' sh -c '~/ar0234test/vfr -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120 -t 40 >/tmp/c0.log 2>&1 &
sleep 4
~/ar0234test/vfr -d /dev/video8 -w 1920 -h 1200 -f NV12 -p 1/120 -t 30 >/tmp/c8.log 2>&1
wait; echo \"--- cam0 ---\"; grep -E \"RESULT|GAPS\" /tmp/c0.log; echo \"--- cam8 ---\"; grep -E \"RESULT|GAPS\" /tmp/c8.log'" < /dev/null
tools/ssh_board.sh "printf ' \n' | sudo -S -p '' dmesg | tail -40" < /dev/null
```
判据：两条流各自 fps 稳定、`lost_cnt 0`；dmesg 里
`Oops|BUG:|WARNING:|not mapped|CSI module|sunxi_iommu` **全 0**。

## 6. 任何一步失败时的回退

```sh
tools/ssh_board.sh "printf ' \n' | sudo -S -p '' sh -c 'ls -1t /boot/dtb/allwinner/*.bak-* | head -1'" < /dev/null
tools/ssh_board.sh "printf ' \n' | sudo -S -p '' sh /tmp/dt_second_cam.sh sensor2-off"
tools/ssh_board.sh "printf ' \n' | sudo -S -p '' systemd-run --on-active=2 /bin/systemctl reboot"
```
起不来时（没有 ssh）：串口 `tools/serial_log.py` 看现场；U-Boot 提示符里
`setenv extraargs "console=ttyS0,115200 init=/bin/sh"` + `run bootcmd` 可以拿 root shell
**把 `/boot/dtb/allwinner/…dtb.pre-0008` 拷回去**（不要漏掉 `console=ttyS0`，否则 shell 会落到
HDMI 上、串口完全失联）。

## 7. 需要记录/回报

* 两条流的实际 fps、`lost_cnt/error_cnt`（`/sys/kernel/debug/mpp/vi`）
* dmesg 的 8 个计数（清单第 5 步）
* 第二颗 sensor 的 `V4L2_IDENT_SENSOR`
* 是否出现"第二路能出但第一路掉速" ⇒ 那就是 ISP602 时分不够，按 REPORT §10.5 降帧/走 RAW
