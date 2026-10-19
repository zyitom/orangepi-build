#!/bin/bash
# native-test.sh v2 —— OPI5 原生 IgH EtherCAT (ec_dwmac_rockchip) 接管验证 + 自动回滚。
# v2 修正：sysfs unbind 不会自动触发其他驱动 reprobe，必须显式 bind 到
# rk_gmac-dwmac-ec；回滚时同理显式 bind 回内置 rk_gmac-dwmac。
LOG=/tmp/ecat-native-test.log
ETH=fe1c0000.ethernet
STOCK_DRIVER=rk_gmac-dwmac
EC_DRIVER=rk_gmac-dwmac-ec

log(){ echo "[$(date +%H:%M:%S)] $*" >> "$LOG"; }
s(){ echo orangepi | sudo -S sh -c "$*" >> "$LOG" 2>&1; }

: > "$LOG"
log "=== native EtherCAT 接管测试 v2 (kernel $(uname -r)) ==="

# 必须先带 main_devices 加载主站：若 ec_dwmac_rockchip 先加载，其依赖
# 会把 ec_master 无参自动拉起，master_count=0，offer 无人接收。
s "modprobe ec_master main_devices=00:00:a4:0a:65:eb"
if [ ! -d "/sys/bus/platform/drivers/$EC_DRIVER" ]; then
    s "modprobe ec_dwmac_rockchip"
fi
if [ ! -d "/sys/bus/platform/drivers/$EC_DRIVER" ]; then
    log "FATAL: ec 驱动未能注册，放弃（网络未动）"; exit 1
fi

s "modprobe ec_dwmac_rockchip"
sleep 1

# /dev/EtherCAT0 兜底（udev 规则没生效时手动建节点）
s "ls /etc/udev/rules.d/ | grep -i ether"
s "ls -la /dev/EtherCAT*"
s "grep EtherCAT /proc/devices && { [ -e /dev/EtherCAT0 ] || mknod /dev/EtherCAT0 c \$(grep EtherCAT /proc/devices | awk '{print \$1}') 0; }"
s "ls -la /dev/EtherCAT0"

log "--- unbind 内置 $STOCK_DRIVER，显式 bind 到 $EC_DRIVER（SSH 将断开）---"
s "echo $ETH > /sys/bus/platform/drivers/$STOCK_DRIVER/unbind"
sleep 1
s "echo $ETH > /sys/bus/platform/drivers/$EC_DRIVER/bind"

sleep 10   # probe + 100ms 延迟 open + idle 总线扫描

log "--- 接管后 dmesg ---"
s "dmesg | grep -iE 'ethercat|dwmac|gmac' | tail -60"
log "--- ethercat 工具视角 ---"
s "/usr/local/bin/ethercat master"
s "/usr/local/bin/ethercat slaves"
log "--- 驱动绑定状态 ---"
s "ls /sys/bus/platform/drivers/$EC_DRIVER/"
log "--- 主站空跑 15s ---"
sleep 15
s "/usr/local/bin/ethercat master"

log "--- 回滚：解绑原生驱动，恢复内置驱动 ---"
s "echo $ETH > /sys/bus/platform/drivers/$EC_DRIVER/unbind"
sleep 1
s "rmmod ec_dwmac_rockchip"
s "rmmod ec_master"
sleep 1
s "echo $ETH > /sys/bus/platform/drivers/$STOCK_DRIVER/bind"
sleep 4
log "--- 恢复后 ---"
s "ip -br link show eth0"
log "=== 测试结束 ==="
