#!/bin/sh
# vulkan-parity-check.sh — 对照"已知良好"清单逐项核查板上 Vulkan 文件环境
# 用法: BOARD=<IP> tools/ssh_board.sh < 本脚本  (或在板上直接 sh 本脚本)
echo "== 1. ICD 注册 =="
for f in /usr/share/vulkan/icd.d/img_icd.json /etc/vulkan/icd.d/img_icd.json; do
  [ -f "$f" ] && { echo "[ok] $f"; cat "$f"; }
done
echo "== 2. ICD 库与符号链接 =="
ls -la /lib/libVK_IMG.so /usr/lib/libVK_IMG.so 2>/dev/null
md5sum /usr/lib/libVK_IMG.so.24.2.6603887 2>/dev/null   # 应为 fbb14790...(stripped) 
echo "== 3. 完整用户态集（已知良好清单）==="
for f in libsrv_um libusc libufwriter libglslcompiler libpvr_mesa_wsi libPVROCL libVK_IMG; do
  for d in /usr/lib /usr/local/lib; do
    ls $d/$f.so* >/dev/null 2>&1 && echo "[ok] $d/$f.so*" || true
  done
  find /usr/lib /usr/local/lib /lib -name "$f.so*" 2>/dev/null | head -2
done
echo "== 4. ldconfig 缓存 =="
ls -la /etc/ld.so.cache 2>/dev/null && ldconfig -p 2>/dev/null | grep -E 'libVK_IMG|libsrv_um|libpvr_mesa_wsi' | head -4 || echo "无 ld.so.cache（dlopen 走默认路径）"
echo "== 5. 设备节点 =="
ls -la /dev/dri/ 2>/dev/null
echo "== 6. 枚举测试 =="
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json /tmp/vktest 2>&1 | tail -2
