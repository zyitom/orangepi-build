#!/bin/bash
# ecat-native.sh —— OPI5 原生 IgH EtherCAT 模式切换（唯一网口 fe1c0000.ethernet）。
#
# start 的清场式时序（每一步都兼容任意前置状态）：
#   1) 设备无论绑在 stock 还是 ec 驱动上，先双向 unbind
#   2) 强制重载模块——udev 冷插拔会把 ec 模块无参自动拉起（master_count=0
#      陷阱），必须显式 rmmod 后带 main_devices 重新加载
#   3) bind 到 ec 驱动 → probe → offer → master 接管 → 延迟 open
#   4) GMAC 中断亲和挪离 RT 核（cpu4）
# stop 反序恢复。任何时候卡死都可以 reboot 兜底（内置驱动 =y 必恢复 eth0）。
#
# MAC 用 RK serial 派生基址（跨重启稳定）。ec 模式不注册 netdev，id 不递增，
# ec 设备 MAC 恒为 :eb；勿读 eth0 现值（stock rebind 会在一次开机内递增）。
set -u
ETH_DEV=fe1c0000.ethernet
STOCK_DRIVER=rk_gmac-dwmac
EC_DRIVER=rk_gmac-dwmac-ec
EC_MAC=00:00:a4:0a:65:eb

msg(){ echo "[ecat-native] $*"; }
s(){ echo orangepi | sudo -S sh -c "$*"; }

case "${1:-}" in
start)
    msg "清场：从两个驱动上解绑设备（若有）"
    s "echo $ETH_DEV > /sys/bus/platform/drivers/$EC_DRIVER/unbind 2>/dev/null"
    s "echo $ETH_DEV > /sys/bus/platform/drivers/$STOCK_DRIVER/unbind 2>/dev/null"
    msg "强制重载模块（先带 MAC 加载主站）"
    s "rmmod ec_dwmac_rockchip 2>/dev/null; rmmod ec_master 2>/dev/null"
    s "modprobe ec_master main_devices=$EC_MAC" || exit 1
    s "modprobe ec_dwmac_rockchip" || exit 1
    if [ ! -d "/sys/bus/platform/drivers/$EC_DRIVER" ]; then
        msg "FATAL: ec 驱动未能注册"; exit 1
    fi
    msg "绑定原生 EtherCAT 驱动（SSH 将断开）"
    s "echo $ETH_DEV > /sys/bus/platform/drivers/$EC_DRIVER/bind"
    sleep 3
    msg "中断亲和：GMAC 相关 IRQ 挪离 RT 核（cpu4）"
    s "for irq in \$(grep -E 'fe1c0000' /proc/interrupts | cut -d: -f1); do echo f > /proc/irq/\$irq/smp_affinity 2>/dev/null; done"
    sleep 2
    s "/usr/local/bin/ethercat master" | head -8
    msg "start 完成（'ecat-native.sh stop' 恢复）"
    ;;
stop)
    msg "解绑原生驱动"
    if [ -d "/sys/bus/platform/drivers/$EC_DRIVER" ]; then
        s "echo $ETH_DEV > /sys/bus/platform/drivers/$EC_DRIVER/unbind"
    fi
    s "rmmod ec_dwmac_rockchip"
    s "rmmod ec_master"
    sleep 1
    msg "恢复内置驱动"
    s "echo $ETH_DEV > /sys/bus/platform/drivers/$STOCK_DRIVER/bind"
    sleep 4
    s "ip -br link show eth0"
    msg "stop 完成"
    ;;
status)
    s "/usr/local/bin/ethercat master" 2>&1 | head -8
    s "lsmod | grep -E 'ec_master|ec_dwmac'"
    ;;
*)
    echo "用法: $0 {start|stop|status}" >&2
    exit 2
    ;;
esac
