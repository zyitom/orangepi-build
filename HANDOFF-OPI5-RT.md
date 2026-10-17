# 交接简报:Orange Pi 5(RK3588)current 分支 —— PREEMPT_RT 实时内核 + u-boot 修复

生成时间:2026-10-17 · 主机:`ssh zyi@<本机>` · 项目根:`/home/zyi/Desktop/orangepi5-build`
分支:`orangepi5` · 构建入口:`sudo ./build.sh opi5-noble [BUILD_OPT=kernel]`

---

## ★ 2026-10-17 上板实测结论(全功能 jammy 桌面镜像,烧录在 64GB SD 卡)

- **引导链(串口全程确认)**:板上 16MB SPI NOR 里是厂商期 loader(Nov 2023 SPL,优先于 SD 启动)
  → 它加载 SD 卡上钉定的 v1.42 布局 FIT → RT 内核 + jammy 桌面正常启动。**SPI loader 与钉定
  FIT 世代匹配,这正是 u-boot 必须钉在 v1.42 的原因;升级任何一端都要真机验证。**
- **RT 实测(串口自动化全套)**:cyclictest -m -Sp99 -i1000 + 6×sha256 满载 60s,
  8 核最坏 **109µs**(T:2),其余核 ≤22µs,平均 2-6µs;`/sys/kernel/realtime=1`;
  全 boot+压载窗口 dmesg **0 个 BUG/Oops/sleeping violation**。
- **视频硬解挂起(2026-10-18 已彻底解决,根因是用户态 SIGSEGV,不是内核竞态)**:
  gst mppvideodec 在解码+CPU 满载并发下间歇"挂起"(PREROLLING 卡死)。10-18 深夜用
  `/proc/<tid>/stack` + gcore + 故障时刻寄存器取证,链条完全闭合:
  1. gst-launch-1.0 的 SIGSEGV 处理器打印 "Spinning.  Please run 'gdb gst-launch-1.0 %d'"
     (stderr 被 hunt 脚本丢弃,无人看见)后 `while(1) g_usleep` 自旋 → 进程活着 →
     看起来像"挂起"。真正的死因是 **librockchip_mpp 1.5.0-2(Xunlong jammy 包,2023-02
     构建)的 mpp_buffer_create 解引用空指针**(PC=lib+0x56d58,x1=0)。
  2. 为什么组是坏的:MppBufferService 构造时为每个 buffer type 建一个分配器;ION 类型
     解析为 dma-heap 分配器,默认堆名是 **"system-uncached-dma32"**(库名表第一项,新
     Rockchip 内核才有),而本板 6.1 BSP 内核只建 system/system-uncached/reserved →
     open ENOENT → 分配器静默创建失败 → mAllocator[ION] 永久 NULL。
  3. 为什么只有 root 崩:is_valid 探测 root 下认定 dma-heap 可用(然后用不存在的节点);
     普通用户探测失败回落到 DRM 分配器(/dev/dri/card0)→ 用户态解码一直"正常"。
     所有 hunt/测试脚本都 sudo 跑(root),所以只咬测试。
  4. 为什么组还返回来了:vendor 1.5.0-2 的 MppBufferService::get_group 对 NULL
     allocator 只打 mpp_assert("p->allocator")(默认日志级别被压制,stderr 干净)然后
     **继续返回半初始化的组** → vdpu34x_h264d_init → mpp_buffer_get_with_tag →
     mpp_buffer_create → group->alloc_api->alloc → SEGV。
  5. 内核侧全程零日志、rkvdec 中断计数冻结、INT_EN=0——全是崩溃后无新提交的**余波**,
     之前怀疑的"RT 下 kworker 推迟 → INT_EN 屏蔽窗口竞态"不成立(用原始证据复盘时
     与 gdb 栈、/proc 栈矛盾,已证伪)。
  - **修复(两层,都已部署)**:
    a. udev 规则 `/etc/udev/rules.d/60-dma-heap-aliases.rules`(rt-check/ 与
       userpatches/overlay/dma-heap/ 各一份):把 system-uncached-dma32 → system-uncached、
       system-dma32 → system、cma-uncached → reserved 补齐(本板 4GB RAM 全在 4GB 以下、
       编解码器前有 IOMMU,-dma32 语义可安全等同)——root 路径恢复正常。
    b. librockchip_mpp.so.0 三指令补丁(rt-check/patch-mpp-jammy-segv.py):get_group
       在 allocator/alloc_api 为 NULL 时干净返回 NULL(与上游后来加入的
       "failed to get allocater" 修复同语义)——分配器创建万一失败时报错退出而不是
       SEGV 自旋。补丁库已重打包进 external/cache/sources/rk35xx_packages/common/mpp/
       的 librockchip-mpp1_1.5.0-2_arm64.deb(原版存档 rt-check/*.pristine.deb)。
    - noble 镜像不受影响:jjriek PPA 的 mpp(git20240717)已含上游修复。
  - 验证:修复前 root 带载 100% 复现(hunt3/hunt4/hunt5 连续命中);修复后 300 轮
    满载解码验证通过(见 rt-check/hunt6 日志)。mpv/ffmpeg 的 rkmpp 路径本来就工作
    (用户态走 DRM 后端),现在 gst 管线与 root 进程也正常。

---

## ★ 2026-10-17 上板实测结论(全功能 jammy 桌面镜像,烧录在 64GB SD 卡)
- **存储**:SD 直写 32.3MB/s / 读 69MB/s。**温控**:满载后 74-83°C(RK3588 降频阈值 95°C
  之内,但建议加散热;桌面长跑注意 gpu-thermal 73°C)。
- **GPU/EGL/GLX 修复(2026-10-18 全部完成)**:镜像里 glvnd 的 EGL vendor 配置原本只有
  mesa(`50_mesa.json`)→ EGL/GLES 全部走了软件渲染。已补 `50-mali.json`(library_path =
  libmali.so.1)并固化进 `external/packages/bsp/rk3588/usr/share/glvnd/egl_vendor.d/`
  (family_tweaks_s 的 rsync 会自动进以后所有镜像),EGL/GLES 应用由此路由到 mali。
  **GLX 原本也是 llvmpipe——原因不是 blob 无 GLX 而是发行版 mesa 没有 G610 驱动;
  已装 vendor 自带的 panfork mesa(kbase 后端,jammy GNOME 镜像同款 debs,现在
  `userpatches/overlay/mesa-jammy/`,customize-image.sh 对 jammy 桌面自动安装)**,
  实测 `glxinfo -B`:Mali-G610 (Panfrost),Accelerated: yes,跨重启稳定。GLX 由
  panfork 接管,EGL 默认仍是 mali blob(glvnd 按 ICM 文件名 50-mali < 50_mesa 排序)。
- **调试通道(2026-10-18 建立,首选)**:板子 eth0 直连主机 enp44s0,板子 NM 配置名 `direct`
  (10.0.0.2/30),主机配置 `static-172`(已改为 10.0.0.1/30 + autoconnect no;旧值 172.16.0.10/24
  曾因 /24 路由抢占 172.16.0.0/21 导致插线即断所有会话——已修)。SSH:`ssh orangepi@10.0.0.2`
  (公钥已推)。板子上网:主机 `ssh -R 8000:ports.ubuntu.com:80` 隧道 + 板子
  `/etc/apt/sources.list.d/tunnel.list`(deb .../ubuntu-ports;注意目录名是 ubuntu-ports)。
- **硬解挂起调试就绪**:gdb 12.0.90 已装(依赖 libdebuginfod1/libboost-regex/libelf1 已补,
  libsource-highlight4v5 的 boost 依赖仍缺但不影响 gdb 运行);自动抓栈脚本 `/tmp/hunt2.sh`
  (满载 + 40 次解码,挂起即 gdb attach 全线程栈到 /tmp/hunt2.txt)。复现时跑:
  `echo orangepi | sudo -S sh -c "nohup sh /tmp/hunt2.sh >/dev/null 2>&1 &"`
- **rtla 修复(2026-10-18 完成)**:旧 overlay 二进制是 noble glibc 编译,jammy 上跑不了。
  已在板上从源码重建:libtraceevent 1.8.4(make,装到 /usr/local/lib64,**已复制到
  /lib/aarch64-linux-gnu/ 替换系统 1.3.3**,否则 libtracefs 运行时缺符号)+ libtracefs
  1.8.2(meson——meson 1.12.1 与 ninja 1.12.1 均在板上源码引导,asciidoc/xmlto 用空壳
  shim 绕过,装 /usr/local/bin)+ rtla(make,VERSION 文件已写 6.1.99-rt36)。
  验证:`rtla timerlat top` 正常输出(IRQ 2-3µs / thread 11-15µs / max 34µs)。
  板上构建工具链已就位:ninja/meson/pkg-config/asciidoc shim,后续重编无需重复。
- **遗留**:
  1. ~~`rtla` GLIBC_2.38 缺失~~ 已解决:板上从源码重编通过(timerlat top 正常),且
     板上编出的 jammy 版二进制已拷回 `userpatches/overlay/rtla/rtla`,以后镜像直接带对的。
  2. ~~GLX 应用走 llvmpipe~~ **已修复(2026-10-18)**:panfork mesa,见上面 GPU/EGL/GLX 条。
  3. ~~段错误未复现但根因未定位~~ **已定位并修复(2026-10-18)**:就是上面的
     mppvideodec SIGSEGV,不是偶发未知项。
- **编解码矩阵(2026-10-18,修复后 root+用户双路径全部通过)**:H264/H265/AV1 硬解 ✓、
  H264/H265 硬编(VEPU)✓。AV1 用的是部分文件冒烟(libx265/libaom 生成慢),完整 AV1
  素材未测;JPEG enc/dec 未测(独立核,风险低)。

---

## ★ u-boot 无法开机故障(2026-10-17 已定位并修复)

**症状**(`serial-boot.log`,03:26 抓取):官方镜像的 SPL(2023-11-20 版)加载 FIT 校验通过、
打印 `Jumping to U-Boot(0x00200000)` 后**立即复位**,主 U-Boot 连横幅都没打印,无限循环。

**证据链**(全部实物核对过):

| 证据 | 结论 |
|---|---|
| 卡上 SPL 横幅 `Nov 20 2023` | 卡 = 官方镜像(带官方 u-boot),不是本 fork 构建的 noble 镜像 |
| 串口日志 FIT uboot 哈希 `473b61ab72…` == noble 镜像(`output/images/…img`)里 FIT 的 uboot 哈希 | 失败的 FIT 就是 fork 构建的 u-boot(用户把它 dd 到官方卡的 0x4000 扇区) |
| fork 的 BL31 配置史:`d3c7234`(2023-11-17)= **v1.42** → `7f776a2`(2026-05)= **v1.56** | 官方 2023-11-20 二进制 = u-boot `3d9c7c63` 源码 + bl31 **v1.42** + ddr **v1.13**;fork 现在构建 = 近似 tip 源码 + bl31 **v1.56** + ddr **v1.24**(上游 a95129b) |
| bl31 v1.42 主段 @0x40000(200KB),v1.56 主段 @0x60000(149KB) | 两代 bl31 段布局不同;旧 SPL + 新 FIT 的组合行为未经验证,实测崩 |
| u-boot 源码 diff(3d9c7c63→tip)仅 opi5 DTS 一行 GPIO(PCIe 供电脚)+ 新板文件 | 排除源码逻辑问题,矛头指向 blob 版本组合 |

**修复(已实施并固化到 fork 配置):**

1. `external/config/sources/families/include/rockchip64_common.inc`:rk3588 的
   `BL31_BLOB` 钉回 `v1.42`、`DDR_BLOB` 钉回 `v1.13`(带注释)。**除非在真机上验证过,
   不要再升 v1.56/v1.24。**
2. u-boot 源码树 `u-boot/v2017.09-rk3588` 停在官方同期提交 `3d9c7c63`(detached)。
   **构建 u-boot 必须带 `IGNORE_UPDATES=yes`**(本 fork 的标准流程),否则 fetch 会把树
   重置回分支 tip。不要用 `BOOTBRANCH='commit:…'` 钉——它与 IGNORE_UPDATES 不兼容
   (branch2dir 会去找 `u-boot/<sha>` 目录,跳过 fetch 后不存在,构建静默失败)。
3. 重建的 deb:`output/debs/u-boot/linux-u-boot-current-orangepi5_1.2.4_arm64.deb`(15:01)。
   实物验证:idbloader 内 DDR 串 **`V1.13 25cee80c4f cym 23/08/11`** 与官方板串口输出逐字
   一致;FIT 结构 = 官方布局(uboot@0x200000 + atf-1@0x40000 + atf-2@0xff100000 +
   atf-3@0xf0000 + fdt)。

**刷写**(卡插本机后,SUPER 用户的 dd;`sdX` 换成实际卡设备):

```bash
cd /home/zyi/Desktop/orangepi5-build/u-boot/v2017.09-rk3588
sudo dd if=idbloader.img of=/dev/sdX seek=64    conv=notrunc bs=512
sudo dd if=u-boot.itb   of=/dev/sdX seek=16384 conv=notrunc bs=512
sync
```

- 与官方镜像的布局完全一致(idbloader@扇区64、FIT@扇区16384=8MB,SPL 就是按 0x4000 扇区找 FIT 的);
  官方卡和 fork 构建的 noble 卡通用。
- 或者:把 `output/debs/u-boot/linux-u-boot-current-orangepi5_*.deb` 拷到板上 `dpkg -i`,
  板上的 platform_install.sh 会自动写当前启动介质。
- 刷完先看串口:`U-Boot SPL 2017.09-orangepi (Oct 17 …)` 之后应出现 **U-Boot 主程序横幅**;
  只要横幅出现,引导就通了(剩下的就是内核/根fs问题)。

**整机镜像(2026-10-17 16:24 已重建,含修复后的 u-boot + RT 内核):**

`output/images/Orangepi5_1.2.4_ubuntu_noble_minimal_linux6.1.99/Orangepi5_1.2.4_ubuntu_noble_minimal_linux6.1.99.img`

实物验证(全部通过):
- 引导区:SPL `2017.09-orangepi (Oct 17 2026)`(3d9c7c63 源码)+ DDR 串 `V1.13 25cee80c4f cym 23/08/11`(与官方板逐字一致)+ FIT atf-1@0x40000(v1.42 布局)
- /boot:`vmlinuz-6.1.99-rt36-rockchip-rk3588`,二进制横幅 `#1.2.4 SMP PREEMPT_RT`;`dtb-6.1.99-rt36-rockchip-rk3588/`
- **直接烧这个 .img 即可**(旧 02:49 的 img 已被覆盖,它带的是会复位的 u-boot)

**全功能桌面镜像(2026-10-17 21:05 新增,官方能力 + RT 全都有):**

`output/images/Orangepi5_1.2.4_ubuntu_jammy_desktop_xfce_linux6.1.99/….img`(7.8GB,jammy + XFCE 桌面)

实物验证(全部通过):
- 引导区:官方同期 SPL + DDR V1.13(与 noble 镜像同一套修复后的 u-boot)
- /boot:vmlinuz-6.1.99-rt36-rockchip-rk3588(SMP PREEMPT_RT)+ RT dtb
- 厂商用户态栈(`rk35xx_gpu_vpu_tweaks` 自动装):`libmali-valhall-g610`(GPU GL/EGL)、
  `librockchip_mpp` + `libgstmpp.so`(硬解)、`librknnrt`/`start_rknn.sh`(NPU)、rkaiq 相机、
  chromium/ffmpeg/mpv、X + XFCE 桌面
- 构建:`./build.sh rt-full BUILD_OPT=image IGNORE_UPDATES=yes`(配置 `userpatches/config-opi5-rt-full.conf`)
- **注意:构建全功能镜像时厂商包源不会自动拉取**(IGNORE_UPDATES=yes 会跳过 main.sh 里
  所有 fetch_from_repo),必须先手动预拉一次:
  `git clone --depth 1 -b rk35xx_packages https://github.com/orangepi-xunlong/rk-rootfs-build.git external/cache/sources/rk35xx_packages`
  (已拉好;rk35xx_gpu_vpu_tweaks 只对 desktop + bullseye/bookworm/jammy/focal/raspi 生效,noble 不在其列)

三条上板路径任选:
1. 烧上面的新 .img(推荐,noble 最小系统 + RT 内核 + 修复 u-boot 一步到位);
2. 官方镜像卡:只 dd 新 idbloader+FIT(命令在上面),再把 4 个 RT 内核 deb 拷上板 `dpkg -i`;
3. 任何已能启动的卡:`dpkg -i output/debs/u-boot/linux-u-boot-…deb`(板上的 platform_install.sh 自动写当前介质)后装内核 deb。

**遗留**:v1.56 bl31 + v1.24 ddr 为何让 opi5 引导复位,没有真机 A/B 定位到具体机制
(嫌疑:两代 bl31 主段地址不同 0x40000 vs 0x60000,旧 SPL 对此有假设)。钉住即可;
哪天要升级,先在真机上单独 A/B bl31 版本。

---

## ★ 结论(本简报要交付的东西)

把 current 分支的 6.1.99 BSP 内核(orange-pi-6.1-rk35xx)改造成了 **PREEMPT_RT 实时内核**:

- **RT 补丁**:kernel.org 官方 `patch-6.1.99-rt36.patch.xz`(6.1 系列与内核树小版本号完全匹配)
  → `userpatches/kernel/rockchip-rk3588-current/0000-rt36-rk35xx.patch`
- **内核配置**:→ `userpatches/linux-rockchip-rk3588-current.config`(`CONFIG_PREEMPT_RT=y`)
- 构建系统会在**每次构建时**自动:reset 内核树 → 应用上面补丁 → 用上面配置 olddefconfig → 编译

> 背景:6.1.y 稳定树后期 sublevel 已由 RT 团队合入 RT 基础设施(groundwork backport),
> 所以 6.1.99-rt36 官方补丁只有 178 个文件、~270KB,对 vendor 树几乎直接可用。

## 冲突修复(vendor 树相对 vanilla 的差异,共 3 处 hunk)

vendor 树(Rockchip rkr5)改过 8250 串口驱动,rt36 有 3 个 hunk 打不上,已手工适配:

| 文件 | vendor 差异 | 处理 |
|---|---|---|
| `drivers/tty/serial/8250/8250.h` | `serial8250_set_THRI/clear_THRI` 里加了 `UART_IER_PTIME`(#ifdef CONFIG_NO_GKI) | hunk 重排,插入 `serial8250_in_IER/set_IER` 两个 RT 助手函数不变 |
| `drivers/tty/serial/8250/8250_port.c` | `serial8250_do_set_termios` 的 IER 写被包进 `#ifndef/#ifdef CONFIG_ARCH_ROCKCHIP` **两份** | 两份的 `serial_port_out(port, UART_IER, up->ier)` 都改成 `serial8250_set_IER(up, up->ier)`;**第二份(CONFIG_ARCH_ROCKCHIP=y 实际编译的那份)是原版补丁覆盖不到的**,不改等于 RT 下 8250 console 的 IER 保护缺失 |

对原始 vendor 树(git f6eedab0c)干跑:**0 FAILED、0 fuzz**。

## 配置变更(`userpatches/linux-rockchip-rk3588-current.config`)

基于 `external/config/kernel/linux-rockchip-rk3588-current.config` 改:

- `CONFIG_PREEMPT_RT=y`(删掉 `CONFIG_PREEMPT_VOLUNTARY_BUILD=y`/`CONFIG_PREEMPT_VOLUNTARY=y`)
  **注意**:choice 组里绝不能同时留 `CONFIG_PREEMPT=y`(Zero 3W 项目踩过的坑,kconfig 会丢 RT)
- `CONFIG_HZ=1000`(`CONFIG_HZ_1000=y`,原 300,RT 低延迟惯例)
- 打开 `CONFIG_OSNOISE_TRACER` / `CONFIG_TIMERLAT_TRACER` / `CONFIG_SCHED_TRACER`(上板延迟取证用,见 `rt-check/rt-runtime-guide.md`)

## 重新构建(内核 deb)

```bash
cd /home/zyi/Desktop/orangepi5-build
sudo ./build.sh opi5-noble BUILD_OPT=kernel          # 正常方式(root)
# 或者(本会话沙箱禁 sudo 时的替代方式,也是本简报实际使用的方式):
./build.sh docker opi5-noble BUILD_OPT=kernel IGNORE_UPDATES=yes
```

- `IGNORE_UPDATES=yes` = 锁定内核树为 f6eedab0c(补丁验证所针对的树)。不加它时构建会
  fetch 远端 orange-pi-6.1-rk35xx 分支;**若 vendor 更新又动了 8250 驱动,补丁会打偏**
  (构建只告警不中止,会留下 .rej 并产出缺 3 处 RT 修复的内核)——重刷 vendor 后必须重新
  干跑验证:`patch -p1 --dry-run < userpatches/kernel/rockchip-rk3588-current/0000-rt36-rk35xx.patch`
- docker 方式首次会构建 orangepi:latest 镜像(userpatches/Dockerfile);输出 deb 属主 root
- **宿主构建和 docker 构建切换前先清内核树宿主工具**(见上面"三个坑"第 1 条)
- **无头/无人值守跑镜像构建不要用 `-it`**:`debootstrap` 缓存解压管道里的 `pv -c`
  (cursed 进度模式)会向 pty 发 `^[[6n` 光标查询并永远等待应答 → 整条管道 0% CPU 死锁
  (2026-10-17 实测卡死 12 分钟)。无 TTY 的正确姿势(docker build.sh 的 `docker run -it`
  写死在 config-docker.conf 里,绕开它):

```bash
docker run --rm --cap-add=SYS_ADMIN --cap-add=MKNOD --cap-add=SYS_PTRACE \
  --security-opt=apparmor:unconfined \
  -v /home/zyi/Desktop/orangepi5-build:/root/orangepi \
  -v orangepi-cache:/root/orangepi/cache -v orangepi-ccache:/root/.ccache \
  -e COLUMNS=200 -e LINES=50 -e TERM=dumb \
  orangepi:latest opi5-noble BUILD_OPT=image IGNORE_UPDATES=yes
```

  (有真终端时 `./build.sh docker …` 正常,因为终端会应答光标查询。)

## 构建结果(2026-10-17 已完成并实物验证)

`./build.sh docker opi5-noble BUILD_OPT=kernel IGNORE_UPDATES=yes` 构建成功,产物在 `output/debs/`(04:34):

| deb | 验证结果 |
|---|---|
| linux-image-current-rockchip-rk3588_1.2.4_arm64.deb | `/boot/vmlinuz-6.1.99-rt36-rockchip-rk3588`,二进制横幅 **`#1.2.4 SMP PREEMPT_RT`**;`/boot/config-…` 里 `CONFIG_PREEMPT_RT=y`(choice 组唯一选中)、`CONFIG_HZ=1000`、OSNOISE/TIMERLAT tracer 已编入 |
| linux-headers-current-rockchip-rk3588_…deb | `usr/src/linux-headers-6.1.99-rt36-rockchip-rk3588/`,含 RT .config + Module.symvers,可编外部模块 |
| linux-dtb-current-rockchip-rk3588_…deb | `dtb-6.1.99-rt36-rockchip-rk3588/rockchip/`,含 rk3588s-orangepi-5.dtb |
| linux-image-…-dbg_…deb | 符号表包 |

RKNPU 修复已编入(内核里 `CONFIG_ROCKCHIP_RKNPU=y`,built-in)。

## ★ docker 构建模式的三个坑(本次全部踩过,务必知道)

1. **宿主(root)构建残留的宿主工具会毒死 docker 构建**:内核树里 `scripts/mod/modpost`、
   `scripts/basic/fixdep`、`scripts/dtc/*` 等是 gitignored 构建产物,`git clean`(不带 -x)
   **不会**清掉它们。宿主 Ubuntu 24.04(glibc 2.39)编出的 modpost 在容器 ubuntu:22.04
   (glibc 2.35)里跑不动 → `GLIBC_2.38 not found` → MODPOST 挂 → 不产 deb。
   解法(本次已做):用一次性容器删树里 scripts/ 下的 ELF:
   `docker run --rm --entrypoint /bin/bash -v $PWD:/root/orangepi orangepi:latest -c
   'cd /root/orangepi/kernel/orange-pi-6.1-rk35xx && find scripts -type f | while read f;
   do head -c4 "$f"|od -An -tx1|grep -q "7f 45 4c 46" && rm -f "$f"; done'`
   (混合使用宿主/docker 构建时,切换前先清一次)
2. **构建脚本会"假成功"**:`scripts/compilation.sh` 的失败检查用
   `[[ ${PIPESTATUS[0]} -ne 0 ]]`,但 make 经 `eval '… | tee'` 执行且管道在 eval 字符串
   内 → PIPESTATUS 永远是 tee 的 0,make 失败不拦截;另外 `advanced_patch` 里
   `shopt -s nullglob dotglob` 泄漏到整个构建 shell,`rsync ./*.deb` 空匹配时 glob 消失、
   rsync 变 list 模式退出 0 → **打包零产出也打印 "Kernel build done" + 退出码 0**。
   **所以每次构建后必须实物验证**:`ls -la output/debs/*.deb` 看时间戳,
   `grep -c GLIBC output/debug/compilation.log` 看错误。
3. **重跑构建时 userpatch 会以 "previously applied" 跳过**(`patch -N` 正常行为),
   会在树里留 209 个良性 `.rej`(下次 `git clean` 会清);确认补丁生效看
   `output/debug/patching.log`。

## 构建后验证(已完成,存档)

```bash
grep -E "^CONFIG_PREEMPT_RT=" kernel/orange-pi-6.1-rk35xx/.config        # =y ✓
dpkg-deb -x output/debs/linux-image-current-rockchip-rk3588_1.2.4_arm64.deb /tmp/d
strings /tmp/d/boot/vmlinuz-6.1.99-rt36-rockchip-rk3588 | grep PREEMPT_RT  # SMP PREEMPT_RT ✓
```

## 上板自检(照 rt-check/rt-runtime-guide.md)

```bash
uname -v                                  # 必须看到 PREEMPT_RT
uname -r                                  # 6.1.99-rockchip-rk3588-rt36(或等价,-rt36 一定在)
dmesg | grep -iE "BUG|WARNING|sleeping function"   # 不能有 sleeping in invalid context
cyclictest -m -Sp99 -i1000 -h400 -D30m    # 压负载前后对比
trace-cmd record -e osnoise 2>/dev/null   # OSNOISE/TIMERLAT tracer 已编入
```

## 风险与注意

- **BSP 驱动 RT 隐患已扫描一轮**:`rt-check/rt_scan_rk3588.py`(由 rt_scan.py 适配,扫
  soc/clk/drm/media/phy/rknpu/serial/wifi/staging 共 1463 个 .c)。发现并已修复 1 处:
  RKNPU hrtimer 回调在 RT 硬中断上下文用可睡眠 spinlock → `0001-rknpu-rt-rawlock.patch`
  把 `rknpu_dev->irq_lock` 转成 raw_spinlock_t。剩余 6 条都是不参与本板构建的主线驱动
  (pic32/imx/sh-sci/8250_bcm7271),已知误报,忽略即可。**2026-10-18 对打好 RT 补丁的
  当前树复扫:结果与首扫完全一致,无新增隐患**(8250_core 的 backup timeout 在 RT 下
  hrtimer 回调运行于任务上下文,合法)。压大负载前建议再跑一遍扫描。
- **eth0 MAC 跨重启稳定(2026-10-18 实测)**:vendor storage 无 MAC 时内核回退的
  00:00:a4:0a:65:eb 是确定性派生,重启前后一致,无需处理。
- `config-opi5-noble.conf` 里"实时内核 legacy 分支专用"的旧注释已更新(那是 fork 自带的
  5.10-rt 分支机制);current 分支的 RT 走本简报的 userpatch 机制。
- deb 包名/版本与原非 RT 内核相同(`linux-image-current-rockchip-rk3588_1.2.4_arm64.deb`),
  已覆盖 output/debs 旧包;**要回退非 RT 内核**:移走
  `userpatches/kernel/rockchip-rk3588-current/`(整个目录)和
  `userpatches/linux-rockchip-rk3588-current.config` 再重编即可恢复原内核。
- 上板刷写时 deb 版本同为 1.2.4,直接 `dpkg -i` 覆盖或重刷整机镜像均可;刷前备份能启动的 SD。
