# packaging/ — 一键 deb 打包

主机上：`bash packaging/build-deb.sh` → 产出 `packaging/ar0234-camera_0.1.0_arm64.deb`。
板上安装：`printf ' \n' | sudo -S -p '' apt install ./ar0234-camera_0.1.0_arm64.deb`，重启生效。
卸载：`sudo apt remove ar0234-camera`（自动还原 dtb、停服务、恢复原装模块优先级），重启。

## 装了什么、装到哪（部署清单）

| deb 内路径 | 来源（仓库里） | 作用 |
|---|---|---|
| `/lib/modules/6.6.98-sun60iw2/updates/vin_v4l2.ko` | `prebuilt/` | vin 框架（0003 补丁 + D3D LBC）；kmod 先搜 `updates/`，不覆盖包内文件 |
| `/lib/modules/.../updates/ar0234_mipi.ko` | `prebuilt/` | 传感器驱动 |
| `/usr/local/bin/ar0234-3ad` | `packaging/payload/` | 3A 守护进程（板上 g++ 编译后取回） |
| `/usr/local/bin/ar0234-rec` | `packaging/payload/` | VE 硬件编码录像 |
| `/etc/systemd/system/ar0234-3ad.service` | `userspace/systemd/` | 3A 服务开机自启 |
| `/etc/udev/rules.d/99-ar0234-camera.rules` | `board/` | cedar_dev_ve2 权限（编码器） |
| `/etc/modules-load.d/ar0234.conf` | 打包脚本内联 | 开机加载 vin_v4l2 |
| `/mnt/extsd/ar0234/isp_param_{3dnr,no3dnr}.bin` | `isp/` | libisp 参数（3ad 按模式选用） |

安装时（postinst）还会：`depmod`、备份并用 `fdtput` 改 dtb（sensor0=ar0234、
ISP 直连、关 MIPI-B）、`systemctl enable`、`apt-mark hold` 两个内核包。
卸载时（prerm/postrm）全部还原。

## 各类文件谁需要

- **.ko**：内核运行时需要，版本必须和 `linux-image` 一致（Depends 锁死）。
- **.h**（`hwapi/board-include` 等）：只在**板上编译用户态时**用，且实际用的是
  板子镜像自带的 `/usr/include/`（`hwapi/` 只是参考副本）；deb 里不需要。
- **二进制**：源码改动后需重新在板上编译并取回：

  ```bash
  tar -czf - userspace | tools/ssh_board.sh "rm -rf ~/ar0234test/userspace/build && tar -xzf - -C ~/ar0234test && make -C ~/ar0234test/userspace -j8"
  mkdir -p /tmp/f && tools/ssh_board.sh "cd ~/ar0234test/userspace/build && tar -czf - ar0234-3ad ar0234-rec" | tar -xzf - -C /tmp/f
  toolchains/.../aarch64-none-linux-gnu-strip /tmp/f/ar0234-*
  install -m 755 /tmp/f/ar0234-* packaging/payload/usr/local/bin/
  ```

## 验证记录（2026-09-16，板上实测）

1. `updates/` 优先级 ✅：`modinfo -n` 解析到 updates/（kmod 默认搜索顺序，无需 depmod.d）。
2. 装包 → 重启 → 模块自启、`ar0234-3ad` active、cap 实测 1920x1200 RAW10 120fps ✅；
   卸载 → 重启 → dtb/模块/服务/参数完全还原到 imx219 出厂态 ✅。
3. 打包必须 `-Zxz`：板上 dpkg 较老，不认 zstd 压缩的 control 归档（build-deb.sh 已内置）。
4. 版本联动：`linux-image` 升级时 deb 因 `Depends (= 1.0.0)` 会被 apt 拒绝升级，
   需要先重编模块、改 control 版本再出新包（预期行为）。
