#!/bin/sh
# 把我们的固件换上，重打 u-boot。**不自动刷写**。
# 用法: sh apply-scp.sh apply | revert
set -e
PK="$HOME/Desktop/orangepi-build/external/packages/pack-uboot/sun60iw2/bin"
OURS="$HOME/Desktop/orangepi-build/e902/scp-ours.bin"
case "${1:-}" in
  apply)
    [ -f "$PK/scp.fex.vendor-bak-"* ] 2>/dev/null || cp -a "$PK/scp.fex" "$PK/scp.fex.vendor-bak-$(date +%Y%m%d)"
    cp -f "$OURS" "$PK/scp.fex"
    echo "已替换 $PK/scp.fex 为我们的固件"
    echo "下一步（手动，需能断电）:"
    echo "  cd ~/Desktop/orangepi-build"
    echo "  sudo ./build.sh BOARD=orangepizero3w BRANCH=current BUILD_OPT=u-boot"
    echo "  然后按平台刷写 u-boot（先备份可启动 SD 镜像！）"
    ;;
  revert)
    cp -a "$PK"/scp.fex.vendor-bak-* "$PK/scp.fex" 2>/dev/null && echo "已还原厂商 scp.fex" || echo "没找到备份"
    ;;
  *) echo "usage: $0 apply|revert"; exit 1;;
esac
