#!/bin/sh
# vk-check.sh — Vulkan 上板确认。板上以 root 运行，vktest / vkcomp 与本脚本放同一目录
# （交叉编译方法见 vkcomp/vkcomp.c 开头）。
D=$(dirname "$0")
echo "== 1. WSI 库依赖（libVK_IMG 运行时 dlopen 它，缺依赖就是 -9）"
n=$(/lib/ld-linux-aarch64.so.1 --list /usr/local/lib/libpvr_mesa_wsi.so 2>&1 | grep -c 'not found')
[ "$n" = 0 ] && echo "   OK: 依赖全部找到" || /lib/ld-linux-aarch64.so.1 --list /usr/local/lib/libpvr_mesa_wsi.so | grep 'not found'
echo "== 2. vktest（instance 创建，期望 VkResult=0）"
"$D/vktest"
echo "== 3. vulkaninfo --summary（期望：PowerVR B-Series BXM-4-64 MC1）"
vulkaninfo --summary 2>&1 | grep -iE 'deviceName|apiVersion|driverName|ERROR|incompatible' | head -8
echo "== 4. vkcomp（compute 冒烟，期望 COMPUTE OK: 256/256 correct, bad=0）"
"$D/vkcomp"
