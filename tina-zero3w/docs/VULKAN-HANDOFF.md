# A733 (BXM-4-64) Vulkan：根因与修复

GPU：PowerVR BXM-4-64，BVNC 36.56.104.183 ｜ DDK 24.2@6603887（内核模块 pvrsrvkm +
用户态 libVK_IMG/libsrv_um/libpvr_mesa_wsi，来自厂家 deb `xserver-xorg-img-bxm_1.21.1-2_arm64.deb`）

> 2026-09-29 重写。本文件 09-28 版的结论（"厂家 DDK 从未支持设备枚举、平台级限制、
> 要 Vulkan 只能换非 RT 内核 + Mesa pvr"）是**错的**，原因见第三节。

## 一、结论

**Buildroot 镜像上 `vkCreateInstance` 返回 -9 的根因：rootfs 缺 `libxshmfence.so.1`。**

libVK_IMG 在创建 instance 时枚举设备（Connect … GetMultiCoreInfo → Disconnect），
随后 `dlopen("libpvr_mesa_wsi.so", RTLD_NOW)` 加载 WSI 模块。libpvr_mesa_wsi 的
NEEDED 里有 libxshmfence.so.1，Buildroot rootfs 没有它 → dlopen 失败 → ICD 返回
VK_ERROR_INITIALIZATION_FAILED(-3) → Vulkan loader 在没有任何 ICD 能建 instance 时
统一报 **VK_ERROR_INCOMPATIBLE_DRIVER(-9)**。dlopen 这一步不走内核，所以之前看到的
"所有 bridge 调用都成功、最后一个是 Disconnect、然后 -9"与它完全吻合。

修复：`br2-external/package/zero3w-gpu/Config.in` 增加
`select BR2_PACKAGE_XLIB_LIBXSHMFENCE`（连同 XORG7/LIBXCB/XLIB_LIBX11，使 GPU 包自带
全部依赖）；`build-image.sh` 的闭源库依赖扫描补上 `/usr/local/lib` 和 libpvr_mesa_wsi，
这类缺库以后在构建时就会报出来。

## 二、证据

1. **反汇编 libVK_IMG**（0x3eb5c → 0x3e8d0 → 0x3e920）：GetMultiCoreInfo 之后
   PVRSRVDisconnect，紧接着 `dlopen("libpvr_mesa_wsi.so", 2 /*RTLD_NOW*/)`；失败分支
   （0x3eca0）以及 dlsym(`pvr_mesa_wsi_sym_addr`)/`pvr_mesa_wsi_init` 失败分支都返回 -3。
2. **依赖核对**：libpvr_mesa_wsi 的 16 个 NEEDED 里，Buildroot target 只缺 libxshmfence.so.1；
   libVK_IMG、libPVROCL、libvulkan 的依赖链完整（qemu 下 `ld.so --list` 递归解析）。
3. **离线复现**（qemu-aarch64 + Buildroot target 作 sysroot，同样 `RTLD_NOW`）：
   原样 → `dlopen FAILED: libxshmfence.so.1: cannot open shared object file`；
   只补这一个库 → `dlopen OK`。
4. **对照**：我们编的 Ubuntu noble 1.0.2 镜像装有 libxshmfence1（及 vulkan-tools），
   2026-09-28 在板上 vulkaninfo 枚举出 BXM-4-64 MC1（conformance 1.3.8.1），最小
   compute 着色器端到端通过（e902/doc/e902/FINDINGS-LEDGER.md T38 追记 9、10；
   测试程序 `tests/vkcomp/`）。

## 三、以前哪些结论是错的

| 旧结论 | 实际 |
|---|---|
| "vendor 官方 noble 镜像 Devices 为空，原栈同样失败" | 所测镜像名、rt58 内核与我们 `output/images` 的 noble 构建一致，不是厂家原版；同一张卡次日实测枚举 + compute 通过。那一次为空的原因未能复现，需上板再确认（见第五节） |
| "ICD 在收到全部成功应答后内部判定，判定点不可从外部观测" | 判定点是 dlopen，`LD_DEBUG=libs` 或 strace 的 `openat` 就能看到 |
| "libpvr_mesa_wsi 曾被截断 667KB，修复后不变" | 那是 Buildroot 正常 strip（`--strip-unneeded`）；对 deb 原件做同样 strip，md5 与 target 完全相同 |
| "库依赖全解析" | 只查了 libVK_IMG 的 NEEDED；运行时 dlopen 的 libpvr_mesa_wsi 没查；构建脚本的依赖扫描也不看 `/usr/local/lib` |
| 多核拓扑 / PRIME-import / 内核模块构建 / CapabilityFlags | 与此无关。相关诊断补丁已移到 `userpatches/kernel/sun60iw2-current/experimental/`，不进构建 |
| "平台级限制，需非 RT 内核 + Mesa pvr" | 不需要；厂家 DDK 在 RT 内核上 compute 可用 |

## 四、真实存在的限制（不是 bug）

- **上屏（present）只能经 X11**：ICD 的 WSI 扩展只有 `VK_KHR_xcb_surface`、
  `VK_KHR_xlib_surface`、`VK_EXT_headless_surface`，没有 `VK_KHR_display`，不能直连 KMS。
  X server 还必须支持 DRI3/Present（厂家 deb 里的 Xorg 支持；Xvfb 不支持，所以
  Xvfb 下"选不出 present 队列"是预期行为）。
- **无头场景**（计算、离屏渲染）不需要 X：用 compute 队列或 `VK_EXT_headless_surface`。

## 五、上板确认（一次即可）

```sh
bash tina-zero3w/build-image.sh && bash tina-zero3w/flash-image.sh /dev/sdX
# 板上：
ldd /usr/local/lib/libpvr_mesa_wsi.so | grep 'not found'        # 期望：无输出
vulkaninfo --summary                                            # 期望：GPU0 PowerVR BXM-4-64 MC1
# 编译并运行 tests/vkcomp（compute 冒烟：256 线程算 i*2+42 并逐个校验，bad=0）
```

noble 镜像上若仍遇到 Devices 为空，先跑 `LD_DEBUG=libs vulkaninfo --summary 2>&1 | grep -i 'wsi\|shmfence\|cannot'`
看 dlopen 哪个文件失败，再看 `/dev/dri/renderD128` 权限。

## 六、工具（仓库内）

- `tests/vktest.c` —— instance 创建探针（多 API 版本）
- `tests/vkcomp/` —— compute 冒烟测试（SPIR-V 内嵌）
- `tests/ioctl_shim.c` —— PVR bridge 协议解码 LD_PRELOAD（查内核交互用；这次的根因不在内核）
- `vulkan-debug.sh` —— 板上诊断
