# packaging/ — 一键打 deb 包

主机上：`bash packaging/build-deb.sh` → 产出 `packaging/ar0234-camera_0.2.0_arm64.deb`。
板上安装：`sudo apt install ./ar0234-camera_0.2.0_arm64.deb`，重启生效。
卸载：`sudo apt remove ar0234-camera`（自动还原 dtb、停止服务），重启。

## 0.2.0 起不再打包内核模块

`ar0234_mipi.ko` 和打过补丁的 `vin_v4l2.ko` 现在由内核包自带：orangepi-build 编内核时会打上
`userpatches/kernel/sun60iw2-current/` 下的补丁。板上的 RT 内核 `6.6.98-rt58-sun60iw2` 已经如此，
模块在 `/lib/modules/<kver>/kernel/bsp/drivers/vin/`。
0.1.0 包里带的 `prebuilt/*.ko` 是为非 RT 的 `6.6.98-sun60iw2` 编的，已删除（git 历史里还在）。

## 装了什么、装到哪

| deb 内路径 | 来源（仓库里） | 作用 |
|---|---|---|
| `/usr/local/bin/ar0234-3ad` | `packaging/payload/`（不入库） | 3A 守护进程 |
| `/usr/local/bin/ar0234-rec` | 同上 | VE 硬件编码录像 |
| `/usr/local/bin/ar0234-npu-zerocopy` | 同上 | NPU 零拷贝验收工具 |
| `/etc/systemd/system/ar0234-3ad.service` | `userspace/systemd/` | 3A 服务开机自启 |
| `/etc/udev/rules.d/99-ar0234-camera.rules` | `board/` | cedar_dev_ve2 / g2d 权限 |
| `/etc/ar0234.conf` | `board/` | 固定曝光/增益/AWB 配置（conffile） |
| `/etc/modules-load.d/{ar0234,g2d}.conf` | 打包脚本内联 / `board/` | 开机加载 vin_v4l2、g2d |
| `/mnt/extsd/ar0234/isp_param_{3dnr,no3dnr}.bin` | `isp/` | libisp 参数（3ad 按模式选用） |

安装时（postinst）还会：备份 dtb 并用 `fdtput` 修改（sensor0=ar0234、ISP 直连、关 MIPI-B；
内核包的 dtb 已经是这样，这一步只是保险），`systemctl enable`，`apt-mark hold` 两个内核包。
卸载时（prerm/postrm）全部还原。

## 二进制怎么来（不入 git）

用户态在板上用 g++ 10 本地编译，然后取回主机：

```bash
tar -czf - userspace | tools/ssh_board.sh "rm -rf ~/ar0234test/userspace/build && tar -xzf - -C ~/ar0234test && make -C ~/ar0234test/userspace -j8"
mkdir -p /tmp/f && tools/ssh_board.sh "cd ~/ar0234test/userspace/build && tar -czf - ar0234-3ad ar0234-rec ar0234-npu-zerocopy" | tar -xzf - -C /tmp/f
toolchains/.../aarch64-none-linux-gnu-strip /tmp/f/ar0234-*
install -D -m 755 /tmp/f/ar0234-* -t packaging/payload/usr/local/bin/
```

## 验证记录

- 2026-09-16（0.1.0，非 RT 内核）：装包 → 重启 → 模块自动加载、`ar0234-3ad` active，cap 实测
  1920x1200 RAW10 120fps ✅；卸载 → 重启，dtb、模块、服务、参数都还原到 imx219 出厂状态 ✅。
- 打包必须用 `-Zxz`：板上 dpkg 较老，不认 zstd 压缩的 control 归档（build-deb.sh 已内置）。
- 0.2.0 还没在 RT 内核上装过。2026-09-24 板上 `/dev/video*` 不存在、`ar0234-3ad` 没在跑，需要先查清楚。
