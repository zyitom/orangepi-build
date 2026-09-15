# 架构说明（为什么长这样）

## 三层结构

```
补丁层    patches/                 对 BSP 内核的全部改动（驱动、vin 框架、配置、DTS）
   ↓ apply / 外部编译
构建层    ar0234_mipi.c + build/   外部模块编译，内核树 root 属主不动，秒级迭代
   ↓ strip
产物层    prebuilt/ → board/       .ko、udev、systemd、ISP 参数 → 部署到板
          userspace/               3A 服务 ar0234-3ad、录像 ar0234-rec（C++20）
```

关键约束决定了形态：

1. **内核树 root 属主且不宜 in-tree 开发** → 一切改动沉淀为 `patches/`，
   编译走外部模块 / 源码副本沙盒（`build/`，不入库可重建）。
2. **3A 在闭源 libisp**（不像树莓派 libcamera 全开源）→ 必须有用户态守护进程，
   所以 `userspace/` 是一等公民，不只是示例程序。
3. **板子是另一台设备** → `tools/` 是主机↔板通信工具，`board/` 是装进板子
   系统的文件（udev 规则、安装脚本）。

## 入库策略

| 目录 | 性质 | 入库 |
|---|---|---|
| patches/、userspace/、isp/、board/、tools/、legacy/、prebuilt/ | 源码 / 数据 / 产物 | ✅ |
| hwapi/、isp/template_gc05a2_a733.blob | **全志厂商文件** | ✅ 但**仓库必须私有**；若将来开源需抽离到 vendor/ 或删除 |
| analysis/ | 实验记录 | 仅文本与源码（白名单），数据不入 |
| build/、userspace/build/ | 编译沙盒 | ❌ 可重建 |
| samples/ 视频、所有 *.h264/*.raw 等 | 测试大数据 | ❌ |

`.gitignore` 用 analysis/ 白名单 + 全局媒体扩展名两条规则实现。
注意：`HANDOFF.md` 含板上 IP 与口令，仓库不要公开。

## deb 打包路线（两步走）

**第一步 T5 —— 附加包 `ar0234-camera`**（适配当前手工部署的内核；**2026-09-16 已实现**：
`packaging/build-deb.sh` 一条命令打出 `ar0234-camera_0.1.0_arm64.deb`，安装/还原脚本与
部署清单见 `packaging/README.md`；板上装包验证待做）：

- 内容：2×.ko → `/lib/modules/<kver>/updates/`、2 程序 → `/usr/local/bin/`、
  systemd unit、udev 规则、modules-load、ISP 参数文件。
- postinst：备份后 `fdtput` 改 dtb、depmod、enable 服务；prerm/postrm 全部还原。
- `Depends: linux-image-current-sun60iw2 (= <精确版本>)`（外部模块对内核版本敏感）。
- 前置验证：`updates/` 目录的模块是否优先于原装模块加载（kmod 默认搜索顺序）。

**第二步 T8 —— 正式并入内核包**：

- `0001`/`0003` → `userpatches/kernel/sun60iw2-current/`，配置改动 →
  `userpatches/linux-sun60iw2-current-a733.config`，`./build.sh BOARD=orangepizero3w
  BRANCH=current BUILD_OPT=kernel` 出自带驱动的内核 deb。
- 此后附加包瘦身为纯用户态 + 数据（.ko 和 dtb 改动随内核包走），
  板上不再需要 prebuilt 模块和 fdtput。
- T8 动内核前必须先准备回退方案（备份 /boot、确认串口可救）。

顺序：先 T5 后 T8。T5 风险低、立刻可用；T8 动构建系统，需要回退预案。
