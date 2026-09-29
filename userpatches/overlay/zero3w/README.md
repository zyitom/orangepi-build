# userpatches/overlay/zero3w — Zero 3W 板级文件（两种镜像共用的唯一来源）

| 文件 | Debian/Ubuntu（customize-image.sh） | Buildroot（tina-zero3w） |
|---|---|---|
| `70-wifi-powersave-off.rules` | 装到 /etc/udev/rules.d（另写 NetworkManager wifi.powersave=2） | post-build.sh 装入 |
| `zero3w-rt-cpufreq.service` | 装入并启用 | post-build.sh 装入并启用 |
| `aic-btaddr.c` | chroot 里用 gcc 编到 /usr/local/bin | zero3w-bluetooth 包交叉编到 /usr/bin |
| `zero3w-sleep.conf` | 装到 /etc/tmpfiles.d（休眠默认 s2idle） | post-build.sh 装入 |
| `zero3w-btaddr.service` | 装入并启用（厂家 hciattach 之后改地址） | 不用：zero3w-bluetooth.service 的 ExecStartPost 做同一件事 |

来龙去脉见仓库根目录 ZERO3W.md「已修复」一节。
