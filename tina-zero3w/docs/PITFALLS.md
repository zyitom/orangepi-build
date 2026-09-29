# 踩坑记录（滚动更新）

## 本轮移植预判的坑（实施时逐条核对/更新）

- **add_sum**：boot_package 头部 add_sum 不重算，下次上电 boot0 直接掉 FEL。
  一律走 `e902/tools/bootpkg.py`（离线方式：空白头镜像 + set-scp --write，见
  build-image.sh 第 3 步），换完必须看到 `checksum PASS`。
- **boot_package 布局**：包头在 SD 的 0x1004000（16400 KiB）；换入 120856 字节的
  vendor-scp 后 valid_len 从 0x154000 → 0x158000，回读时取 0x158000 字节。
- **uImage/地址**：U-Boot 2018.07 默认内核区只有 32 MiB，带 ftrace 的 uImage 尾部
  会被 dtb 覆盖（Bad Data CRC）。boot.cmd 重设 kernel 0x41000000 / fdt 0x44000000
  （48 MiB 间距），post-build.sh 校验 uImage ≤ 48 MiB。
- **rogue_km 符号链接**：`bsp/modules/gpu` 里 145 个 symlink 指向
  `/root/orangepi/kernel/...`，编模块前重指到 buildroot 内核目录
  （`-xtype l -lname '/root/orangepi/kernel/*'`）。
- **GPU Makefile 平台变量**：`bsp/modules/gpu/Makefile` 的 `CONFIG_OS_TYPE` 缺省是
  android，必须传 `LICHEE_PLATFORM=linux`，否则走进 android 目录拿不到
  `binary_sunxi_linux_nulldrmws_release`。
- **ICD 路径**：厂家 ICD 写 `/lib/lib*.so`（merged-usr 假设）；buildroot rootfs
  /lib 是真实目录，ICD 文本改 `/usr/lib/lib*.so`（br2-external/overlay）。
- **Vulkan 必须有 libpvr_mesa_wsi.so**，且 ld.so 搜索顺序 `/usr/local/lib` 优先
  （00-pvr-priority.conf，overlay）。
- **Tina buildroot git 快照不完整**：11 个仓库大文件只在 MEGA；且它离了 SDK 布局
  不能用——同版本上游 2022.05 有 2621 个包目录、Tina 2637（"~2900" 是拿更新上游
  误比），121 个厂商配方全 `SITE_METHOD = local` 指向 SDK 内部 blob，基树
  `include ../config/buildroot/*.mk` 硬挂相对路径。故构建基座用上游 2022.05。
  注意 ocl-icd 是上游 2022.11 才有的包，2022.05 没有 —— OpenCL loader
  （libOpenCL）由 zero3w-ocl-icd 包（Khronos ICD-Loader）提供。
- **mkusers 密码语法**：users 表密码字段 `=xxx` 是"把 xxx 当明文加密"（不是复制
  别的用户）；裸明文会被原样写进 shadow（无法登录）。orangepi 用户用 `=orangepi`
  （与 root 同明文，PASSWD_METHOD 一致加密）。
- **fixincludes**：板上 gcc 包用 `--with-build-sysroot=` 指向 staging 的**副本**，
  避免 fixincludes 改写共享 staging（否则后续包的构建结果被污染）。
- **板上 gcc 链接环境**：gmp/mpfr/mpc 必须是 aarch64 版（--with-gmp=staging），
  板上由 /usr/lib 命中；安装阶段再把 staging 头文件/.so 脚本/.a/.o 铺进 target。

## 实施中新踩的坑

- **systemd 250.4 + Ubuntu 24.04 gcc（_FORTIFY_SOURCE=3）**：`read_virtual_file_fd()`
  里 `read(fd, buf, size+1)` 的长度取自 `MALLOC_SIZEOF_SAFE(buf)`（malloc_usable_size），
  而 gcc 在 FORTIFY=3 下按 `malloc()` 调用推导 buf 的动态对象大小 → glibc
  `__read_chk` 运行时判 `nbytes > buflen` 直接 abort（"buffer overflow detected"），
  杀死 `systemd --bus-introspect list`（meson 的 export-dbus-interfaces 目标）。
  修复：br2-external/global-patches/systemd/250.4/0001-*.patch 改用 malloc 派生的 size。
  定位手段：`gdb -batch -ex run -ex bt --args ./build/systemd --bus-introspect list`
  （新 glibc 的 abort 输出已不带 backtrace，必须上 gdb）。

- **补丁系列 ≠ 可重放**：厂家树工作区 = commit 2ac08e8c7 + 0000-0015 就地应用 +
  未提交微调。把补丁重放到纯净 commit 上，0006/0008/0009 在 sunxi_isp.c:68 /
  vin_video.c:1310/4016 上下文漂移直接失败。**内核 tarball 改为对工作区做确定性
  快照**（临时 GIT_INDEX_FILE/GIT_OBJECT_DIRECTORY + `git add -u` + `write-tree`
  → 固定日期 commit-tree → `git archive`；厂家 .git root 属主，全程只读）。
- **untracked ≠ 垃圾**：工作区里有 8 个 untracked 但属于验证树的文件——补丁新增
  （amp_timestamp.c/.h、ar0234_mipi.c）、RT 组件（kernel/printk/nbcon.c、
  localversion-rt、include/sunxi-trng.h）、GPU 模块源（2 个 .c）。当年仓库 root
  属主没法 `git add`，所以补丁重放也救不回它们；`git add -u` 只会拿 tracked 文件。
  快照脚本用 `ls-files --others --exclude-standard` + 垃圾过滤补收。
- **绝不能同时跑两个 buildroot make（同一 O=）**：后台任务还活着时又起一次
  build-image.sh，两个 make 在同一包上赛跑——一个正 `Building` 另一个的
  dirclean 把 `build/<pkg>-<ver>` 删了 → `touch .stamp_built: No such file or
  directory`，且复活的一方 rsync 回来的目录没有 EXTRACT_CMDS 产物（data/），
  install 必炸。表象极具迷惑性（错误指向 stamp 而不是真实命令）。起任务前先
  `ps aux | grep make` 确认没有存活的构建；`<pkg>-dirclean` 和 make 必须同一
  次串行完成。
- **`grep -q` + pipefail = SIGPIPE 假失败**（踩了三次）：`长输出 | grep -q`
  匹配即关管道，写端吃 SIGPIPE(141)，pipefail 下整个脚本被带崩且**没有任何错误
  信息**（如 `prepare-kernel.sh` 在 307MB tar 流上自检时静默死）。长流一律
  `grep pattern >/dev/null`。
- **GPU 模块 symlink：`-xtype l` 抓不全**：rogue_km 里 145 个绝对 symlink 早年已
  被 compilation.sh repoint 成 `/home/.../kernel/<树名>/...`。在本机构建时这些链
  **能解析**（不是断链），`-xtype l` 一个都抓不到，模块会静默编进外面那棵工作区。
  修复规则改为"目标为绝对路径且含 `kernel/` 组件 → 用 sed 剥到 `kernel/<树名>/`
  为止、重指 `$(LINUX_DIR)`"，与是否断链无关。
- **buildroot DEPENDENCIES 不等于使能**：`.mk` 里 `DEPENDENCIES = gmp mpfr mpc`
  只排构建顺序；包必须由 Config.in `select`（或 defconfig）使能，否则构建序不
  生效。zero3w-native-gcc 的 Config.in 里显式 `select BR2_PACKAGE_GMP/MPFR/MPC`。
- **EXTRA_DOWNLOADS 的 basename 撞车**：OpenCL-ICD-Loader 主源码和 OpenCL-Headers
  的 github 归档都叫 `v<版本>.tar.gz`，同版本时在 DL_DIR 互相覆盖（谁后下谁赢，
  解包必坏）。headers 错开用 v2023.02.06。
- **自检算式**：tar 条目数 = tracked − gitlinks + untracked，漏加 untracked 会把
  正确的 tarball 判成失败。
- **_FORTIFY_SOURCE=3 下的 systemd 250.4 host 构建**：Ubuntu 24.04 的 gcc 默认
  FORTIFY=3（动态 object size）。`read_virtual_file_fd()` 先 `malloc(size+1)` 再用
  `MALLOC_SIZEOF_SAFE(buf)`（malloc_usable_size）放大 `size` 后 `read(fd, buf,
  size+1)`；gcc 从 malloc 表达式推出 buf 的动态大小 = size+1，glibc 返回更大块时
  实读字节数超过它 → `__read_chk` 运行时 "*** buffer overflow detected ***"
  → SIGABRT（表现为 meson `export-dbus-interfaces` 步骤跑 host `systemd
  --bus-introspect list` 时崩，glibc 2.34+ 的该消息不带 backtrace）。修法：
  br2-external/global-patches/systemd/250.4/ 回退该 opportunism（v251 上游已改写）。
  gdb 定位套路：`gdb -batch -ex run -ex bt --args <二进制> <崩溃参数>`，FORTIFY
  的栈顶是 `__chk_fail`/`__read_chk`，下一层就是肇事函数。
- **同一 FORTIFY=3 病灶有两处**：0001 修了 `read_virtual_file_fd()` 后，
  target-finalize 的 `udevadm hwdb --update`（parse_env_file →
  read_full_file_full）又在 `read_full_stream_full()` 的
  `MALLOC_SIZEOF_SAFE(buf)` 处崩（`__fread_chk`）。0002 补丁同法处理：读长度
  一律用循环自增的 n_next，不再从 malloc_usable_size 放大。systemd 250.4 里
  fileio.c 只剩这两处此模式，但后续升级或新宿主编译器时要警惕第三处。
- **local 站点包没有 extract 步骤**：`SITE_METHOD = local` 的包 buildroot 只做
  rsync（files/ → $(@D)），自定义 `EXTRACT_CMDS` **整个被跳过**，且不报错——
  直到我写的 deb 解包逻辑全没执行、安装步骤 `cp usr/lib/lib*.so*` 才暴露。
  三个包（gpu/isp/native-gcc）踩中。修法：解包放 `CONFIGURE_CMDS`（该步骤对
  local 包照常运行），改 .mk 后记得 `<pkg>-dirclean`（buildroot 不按 .mk mtime
  失效 stamp）。
- **DL_DIR 按包名分子目录**：2022.05 下载存 `$(DL_DIR)/<包名>/`，不是平铺。
  自定义 EXTRACT_CMDS 里引用 `$(DL_DIR)/$(SOURCE)` 会 No such file——用
  `$(<PKG>_DL_DIR)`。
- **mv 源码目录进已存在的 $(@D)**：generic 包的 $(@D) 在 rsync 阶段就建好了
  （.keep），github 归档解包后 `mv <topdir> $(@D)` 会把源码挪进 $(@D)/<topdir>
  而不是替代 $(@D)，configure 找不到 CMakeLists.txt。用 `cp -a <topdir>/. $(@D)/`。
- **宏定义了但没接线**：native-gcc 的 BUILD_BINUTILS/BUILD_GCC/INSTALL_CMDS
  只是 define，没有赋给 BUILD_CMDS/INSTALL_TARGET_CMDS → 包"秒过"（stamps 全
  生成、什么都没编）。新增自定义宏后必须检查有没有接到 buildroot 的钩子上。
  变体：`<PKG>_INSTALL_STAGING_CMDS` 定义了但没设 `<PKG>_INSTALL_STAGING = YES`
  → install-staging 步骤整个跳过、无任何报错（zero3w-gpu 的 libvulkan 就这样
  没进 staging，vulkaninfo 链接时 `cannot find -lvulkan`；另一表象是
  `$(O)/staging` 符号链接消失也不影响构建——所有 recipe 用的是
  `$(STAGING_DIR)` = host/<tuple>/sysroot 实体目录，符号链接只是摆设）。
- **vulkaninfo 输出名不能叫 vulkaninfo**：`$(@D)/vulkaninfo` 是源码目录，
  `-o $(@D)/vulkaninfo` 报 "cannot open output file: Is a directory"；输出用
  `vulkaninfo-bin`，安装时改名。另外 vulkaninfo.hpp 用 CTAD
  （`std::array{...}`）需要 `-std=c++17`（gcc 默认 gnu++14）；volk 头文件
  模式需要 `-DVK_NO_PROTOTYPES`（否则 volk.h 的函数指针声明与 vulkan.h 的
  原型全部 redeclared 冲突）。
- **target-finalize 的三道非特权陷阱（都靠 fakeroot 阶段解决）**：
  1. `udevadm hwdb --update --root $(TARGET_DIR)` 是宿主编译的 systemd 工具，
     `_FORTIFY_SOURCE=3` 下第二处 `MALLOC_SIZEOF_SAFE` 病灶在
     `read_full_stream_full()`（0002 补丁）；
  2. sanity check 禁止 target 里有 `/etc/ld.so.conf{,.d}`——rootfs overlay
     是在 target-finalize **内**拷贝的（先于该检查），所以 PVR 的
     00-pvr-priority.conf 不能放 overlay，要在 post-fakeroot 脚本里写入
     （fakeroot 阶段在检查之后、镜像生成之前）；
  3. post-build 脚本里 `chown 0:0` 非特权直接 EPERM 打断构建——属主一律交给
     fs/common.mk 的 fakeroot `chown -R 0:0`（或 post-fakeroot 脚本），
     post-build 只做 chmod。

## 09-26 上板验收发现的坑（镜像能启动之后）

- **uImage 的 `-A` 必须是 `arm`（arch=0x02）不是 `arm64`（0x16）**：vendor U-Boot 的
  legacy bootm 不认 0x16，报 `Unsupported Architecture 0x16`。与 vendor 1.0.2 镜像
  的 uImage 头逐字节对比确认（os=05 arch=02 type=02 comp=00）。
- **boot.scr 缺 `fdt resize 65536`**：U-Boot 往 dtb 追加 /chosen 时报
  `FDT_ERR_NOSPACE`，`/chosen node create failed` 直接挂死。vendor boot.cmd 里有。
- **kconfig 静默丢弃第三例**：`BR2_PACKAGE_UTIL_LINUX_TASKSET` 不存在——taskset 在
  `BR2_PACKAGE_UTIL_LINUX_SCHEDUTILS` 里。改 defconfig 后还要 `dirclean util-linux`
  （config 变化不触发已构建包重装）。
- **板上 gcc 三件套被 target-finalize 拆毁**：① 无条件删光 target 的 `*.a`
  （libgcc.a/libc_nonshared.a 阵亡）；② `install-gcc` 的 EXTRA_PARTS
  （crtbegin*.o/crtend*.o）在本配置下没装；③ glibc 的 libc.so 链接脚本引用绝对
  路径 `/usr/lib64/...` 而 target 没有 lib64 符号链接。post-fakeroot 里全部补齐
  （staging .a 重铺 + gcc-build 的 crt*.o/libgcc.a + `ln -s lib usr/lib64`）。
- **/etc/ld.so.conf.d 单独存在无效**：glibc 根本不读 conf.d（需要 /etc/ld.so.conf
  include 它，而那个被 buildroot sanity check 禁止）。/usr/local/lib 里的
  libvulkan 用符号链接补进 /usr/lib 才能被动态链接器找到。
- **这台 GPU（rogue 24.2 nulldrmws）的节点是 DRM**：`/dev/dri/card1` +
  `renderD128`，不是 `/dev/pvrsrvkm`（pvr_platform_drv.c 走 drm_dev_register）。
- **WiFi 冷插拔竞态**：SDIO 枚举晚于 udev 冷插拔时 aic8800_fdrv 不自动加载，
  wpa_supplicant.service 启动时 wlan0 不存在就退出了。aic8800_bsp/fdrv +
  vin_v4l2/ar0234_mipi 全写进 modules-load.d/zero3w.conf 早载。
- **验收脚本采集要用 sudo dmesg**：非 root klogctl 被拒（dmesg 空输出 ≠ 没探测）。
  amp-timestamp 实际探测成功：`freqid=24000000 (24.000 MHz)`。

## 09-26 Vulkan 排查终局 + 冷启动提速

- **Vulkan ICD 在 vendor 生态从未可用**：user 说 Ubuntu 上没问题，但 hwapi 盘点
  （board-inventory）显示 noble 上只验证过 OpenCL（"✅ OpenCL 能枚举设备"），
  vendor noble minimal 镜像里 icd.d/img_icd.json 存在而 **libVK_IMG 库本体缺失**
  ——json 指向不存在的文件。e902-noble-check.sh 的 VULKAN-WSI 检查项跑
  vulkaninfo 也只在库齐的桌面上跑过，无通过记录。
- 排除清单（vktest 探针 + vendor 预编译模块 vermagic 二进制补丁换装对比）：
  内核模块（vendor ko 换装后 OCL 照常、VK 依旧 -9）、库依赖（全解析）、
  ld.so 搜索路径（LD_LIBRARY_PATH=/usr/local/lib 无效，deb 的 /etc/environment
  就是设这个）、固件（dmesg 见 BVNC 注册 + rgx.fw/rgx.sh 加载）、API 版本
  （1.0/1.3.277/1.3.280 全部 -9）。剩余假设：ICD 经 libpvr_mesa_wsi 急切连接
  X（Ubuntu 有桌面 X，buildroot 无头）——待板上起 Xorg 验证。
- **冷启动分解（power→login ≈ 20s）**：U-Boot ~6s（33MB 无压缩 Image 读卡 ~5s）
  + 内核 8.2s + systemd 12s。115200 串口打几千行 dmesg 是真实耗时（每字符
  ~0.9ms），loglevel=7 是最大拖累。
- **提速三招（#42 镜像）**：① bootargs 改 `quiet loglevel=3`（日志仍全在缓冲区，
  板上 dmesg 可查）；② uImage 改 `-C gzip`（gzip -9 压 Image，读卡省 ~3s，
  vendor U-Boot legacy bootm 自动解压）；③ mask systemd-pstore/remote-fs。
- **buildroot 2022.05 自带的 strace 5.17 编不过 6.6 内核**：内核 UAPI 把
  BTRFS_EXTENT_REF_V0_KEY 注释掉了，strace xlat 表 -Werror 直接炸。走
  BR2_GLOBAL_PATCH_DIR 打兜底 #define（值 180 与 v6.5/strace bundled 头一致）。
  **已被取代**：后续还有 fcntl*64、io_uring 连环坑，最终改为把 strace 升到 6.6，
  见下文"strace 5.17 对 6.6 内核 UAPI 的兼容问题是连环坑"。
- **vulkaninfo 排查新证据（09-26）**：LD_DEBUG=bindings 抓到 ICD 调用序列——
  services 连接成功（固件都加载了），走到 PVRSRVGetMultiCoreInfo 后立即
  Disconnect 放弃 → 0 设备 → INCOMPATIBLE_DRIVER。同时排除了 DRM 节点权限
  （root 下全通、名字 pvr、版本 24.2.6603887 匹配）和 X 急切连接（宿主 Xvfb
  经 ssh unix socket 转发到板上，X 连通后依然 -9；板卡内核寄存器
  RGX_CR_MULTICORE_SYSTEM 实读 1 核为硬件真实值）。下一步 strace 抓
  GetMultiCoreInfo 对应的 bridge ioctl 返回值。
- **buildroot 补丁机制的两个坑**：① `apply-patches.sh` 的 duplicate 检查查的是
  build 目录里持久化的 `.applied_patches_list`，同名补丁即使换了目录重用也会被
  判重——目录名/位置不要随意迁移；② `BR2_GLOBAL_PATCH_DIR` 指向
  `br2-external/patches` 这类会被自动收集的路径时同一补丁应用两次报
  duplicate——统一放到 `br2-external/global-patches` 并显式指定，挪过位置后必须
  对受影响包 dirclean（并确认 build 目录真被删掉）再重建。
- **strace 5.17 对 6.6 内核 UAPI 的兼容问题是连环坑**（btrfs → fcntl*64 →
  io_uring 结构体改名），逐个打补丁不值。最终方案：SDK buildroot 树里直接把
  package/strace 升到 6.6（strace.mk 版本号 + strace.hash 重算，源码在
  ~/tina5/dl/strace/）。SDK 是 git clone，重新拉取会丢这个改动，所以它已导出为
  `tina-zero3w/buildroot-patches/0001-package-strace-bump-to-6.6.patch`，
  fetch-sdk.sh 克隆/更新上游 Buildroot 后自动打上（已打过则跳过）。
- **vendor U-Boot legacy bootm 不支持 gzip 压缩内核（#51 事故）**：uImage comp
  字节改成 01（gzip）后 150 秒重启 13 次（~11 秒/循环）——卡死在 bootm 解压、
  看门狗复位，串口全程无 "Uncompressing" 输出。uImage 必须保持 `-C none`。
  串口静默不代表没在跑：quiet 模式下误发一个回车还会打断 autoboot 倒计时掉进
  U-Boot 提示符（测串口时输入要延时到登录提示出现之后）。
- **冷启动时间的正确测法**：串口静默后只能靠 U-Boot 内部时间戳（[04.7s] 等）
  和 "Hit any key" 倒计时边界算；登录提示出现前不要向 tty 发任何字节。
- **内核补丁的两层含义**：userpatches/kernel/sun60iw2-current/ 的补丁文件只是
  「来源记录」，真正生效的是厂家树**工作区内容**（tarball = 工作区快照）。
  新增补丁必须 `git apply` 到工作区，只建文件不 apply 的话 prepare-kernel 的
  反向校验只给 [warn] 不阻断，构建出来的内核静默缺少改动（#55 教训，0021
  补了文件没 apply，热替换推上去的还是旧模块）。
- **热替换内核模块调试法**：不必为每个诊断补丁重烧卡——模块构建产物
  gzip+base64 推到板上 `rmmod; insmod` 即可（同一内核、vermagic 相同）。
  gunzip 会保留源文件 mtime，判断模块新旧别看文件时间。
- **strace 的盲区**：PVR bridge ioctl 在内核层全返回 0，失败的
  PVRSRV_ERROR 写在 out 结构体里——strace 看不到，必须内核侧日志（0021）。
- **换 tarball 内容不会触发 buildroot 重解包**：CUSTOM_TARBALL 文件原地更新后
  .stamp_extracted 不失效（#56 教训：tarball 哈希变了、build/linux-custom 还是
  旧树）。改了内核内容必须 `make linux-dirclean` 全量重编。

## 09-27 Vulkan 调查终局（内核侧全部排除，定性为 vendor 闭源 ICD 缺陷）

仪器化三板斧全部上板实测：
1. 0021（GetMultiCoreInfo KM 层日志）——**零输出**：ICD 根本没调这个 bridge
   （LD_DEBUG 的符号绑定是加载期解析，顺序 ≠ 调用顺序，之前据此推断的
   "死在 GetMultiCoreInfo" 不成立）；
2. 0022（BridgedDispatchKM 无条件失败日志）——**零输出**：ICD 发出的每一个
   bridge 调用内核都返回 PVRSRV_OK；
3. strace 全量：所有 PVR bridge ioctl 返回 0，无 ENOTTY。

结论：内核对 ICD 的每次请求都成功应答（含设备连接、固件加载），同一连接上
OpenCL 完整可用（cltest mismatches=0）。INCOMPATIBLE_DRIVER 是 libVK_IMG 在
**用户态内部**对成功应答数据做出的拒绝，闭源无法进一步定位（PVRDebugLevel
env/apphint 两种通道实测无效、无 .dbg 构建、PVRSRVDebugPrintf 输出目标不明）。
vendor 侧佐证：官方镜像不含 libVK_IMG（icd.d 的 json 指向不存在的文件）、
无 vulkaninfo、hwapi 盘点只验证过 OpenCL。

定性：**vendor DDK 未提供可用的 Vulkan 用户态**，验收 5b 被 vendor 阻塞。
真正的修复路线 = Mesa `pvr` Vulkan 驱动（对 BXM-4-64 / BVNC 36.56.104.183
有 Vulkan 1.2 一致性认证），但它需要主线内核 pvr DRM（BXM 支持在 6.17/6.18），
与 vendor 6.6 RT 内核的 rogue_km UAPI 不兼容，属于栈级重构，超出本任务范围。

## 09-27 Vulkan 补充实验（LD_PRELOAD ioctl shim 全量解码，调查彻底关闭）

写了 ioctl_shim（tina-zero3w/tests/ioctl_shim.c）：LD_PRELOAD 透传 ioctl 并按
PVRSRV_BRIDGE_PACKAGE 结构（bridgeID/funcID/pvParamIn@8/pvParamOut@16/尺寸@24,28，
cmd 0xc0206440）dump 入出参。解码结果（对照 generated/rogue/srvcore_bridge）：

- Connect(func0)：BVNC=36.56.104.183 正确、eError=OK、CapabilityFlags=0x20000、
  KernelArch=0x40 —— 全部正常；
- AcquireGlobalEventObject/AcquireInfoPage：handle 有效、eError=OK；
- GetMultiCoreInfo(func12)：eError=OK，NumCores=1（HW 寄存器 RGX_CR_MULTICORE_SYSTEM
  实读 1——VKDBG 0023 证实是硬件真值，非配置错）；
- 强制 NumCores=4 + 合成 caps（0023）后 ICD 依旧 INCOMPATIBLE_DRIVER——核数假设排除；
- 全程 0022 内核日志零失败记录。

结论不变且证据升级：**ICD 在收到全部成功应答的情况下自行拒绝枚举设备**，
闭源内部逻辑（其设备支持表/构建选项匹配）无外部干预点。修复只能换栈：
Mesa pvr Vulkan 驱动 + 主线内核 pvr DRM（6.17+）。

## 09-27 Vulkan：Radxa 工作栈移植实验（全部排除，最终定论）

网上找到同 SoC 的成功先例：Radxa Cubie A7A/A7S（同 A733、同 BVNC 36.56.104.183、
同 DDK 24.2.6603887）上 vendor libVK_IMG 可用（DXVK/Zink 跑通，
github.com/ayiejosh/a733-powervr-fex）。逐项移植到我们的板子实测：
1. Radxa 的 libVK_IMG + libsrv_um（md5 与我们的不同！同版本号不同构建）→ -9；
2. PRIME-import 内核补丁（fex 项目的关键补丁）→ 枚举无关，-9；
3. 强制 NumCores=4 → -9（HW 寄存器实读 1 核为真值）；
4. **Radxa 的 img-bxm-dkms 0.1.0-3 模块源码**（与厂家树差异 34 个文件，
   aw-drivers-dkms 0.1.0-3）针对我们内核编译成功、热替换 → 依然 -9，
   bridge 序列与应答数据逐字节一致。

结论：在保留厂家 6.6 内核的前提下，无论换哪个构建的用户态/内核模块，ICD 的
拒绝行为完全一致。剩下的差异只可能在 Radxa 的非 RT 内核平台集成（sunxi-drm、
IOMMU、内存布局）——即"能跑 Vulkan 的 A733"需要整套 Radxa 式内核栈（非 RT），
与本项目"PREEMPT_RT + 厂家 vin/ISP"的目标互斥。Vulkan 定性为：
**当前 RT 产品栈不支持；如需 Vulkan，用独立的主线/Radxa 内核启动介质**。

## 09-27 深夜追加：target 里 vendor 库截断损坏（真缺陷，已修）+ Vulkan 移植实验全记录

- **target/usr/local/lib 整层被截断**： Mesa/WSI/EGL/GL 层（libpvr_mesa_wsi 差
  667KB、dri/*.so 差 1.6MB、libglapi 差 132KB、libEGL 差 66KB、libvulkan 差
  92KB……）——09-25 双会话并发构建战争期间 cp 被打断的残骸，包 stamp 完好所以
  一直没重装。诊断法：按**文件大小**比对 deb 原件（md5 会被 strip 干扰，大小差
  >2KB 即异常；strip 正常只差百字节级）。修复 = zero3w-gpu-dirclean 重装（#59）。
  这批截断库会破坏 GL/EGL/WSI 一切渲染路径（Vulkan ICD dlopen 它）。
- **Vulkan 移植实验全记录（全部 -9）**：Radxa ICD/libsrv_um（同版本号不同构建
  md5 22302f1e/1bb80d24，与我们 deb 逐字节相同——之前"不同"是 strip 假象）、
  PRIME-import 补丁（gem_prime_import + prime_fd_to_handle，影响 buffer 共享
  不影响枚举）、强制 NumCores=4（HW 寄存器 RGX_CR_MULTICORE_SYSTEM 真值 1，
  caps[0]=0x78 含 PRIMARY|GEOMETRY|COMPUTE 拓扑合法）、**Radxa img-bxm-dkms
  0.1.0-3 模块源码全量编译**（34 文件差异，编译成功热替换）——bridge 序列与
  应答逐字节一致，ICD 依旧拒绝。剩余唯一差异 = Radxa 非 RT 内核平台整体
  （sunxi-drm/IOMMU/内存布局）。**工具链注意**：Radxa DKMS 树构建需
  KERNELDIR= 变量 + host/bin/bin/ 双重路径符号链接（gcc.br_real 解引用）。
- **如需 Vulkan**：独立启动介质（Radxa Cubie A7A 镜像或主线 6.17+ 内核 +
  Mesa pvr）与 RT 系统并存，不要混栈。

## 09-27 终极实验：vendor 原版 noble 系统上板实测（Vulkan 调查最终定论）

将 vendor 官方 Orangepizero3w_1.0.2_ubuntu_noble 镜像烧卡、本板启动，运行系统
自带的 vulkaninfo（1.3.275 + 系统还带 vkcube 三件套）：

    Devices:
    ========
    （空——零物理设备）

**vendor 原栈在原厂内核 + 原厂 ICD + 原版镜像上同样枚举不到 GPU 设备。**
此前所有对比实验（同一 ICD 二进制 md5、同一内核 config diff=0、同一 DTB、
同一固件、模块源码三种构建）与此完全自洽。

最终定性（三层）：
1. 我们的 buildroot 移植与 vendor 原栈在 Vulkan 上**行为一致**——移植没有
   引入任何 Vulkan 缺陷（其余 7.5 项验收我们的系统反而领先：板上 gcc、头
   文件、无头精简启动）；
2. vendor 交付物里 Vulkan 是"给了测试套件但驱动从未真正支持设备枚举"的
   状态（HWAPI 盘点"Vulkan 库齐"指的是库文件存在）；
3. 需要真 Vulkan 的路线 = Mesa pvr（主线内核 6.17+ pvr DRM，BXM-4-64 MC1
   36.52.104.182 已支持；我们 BVNC 36.56.104.183 在 Mesa 的支持列表中），
   与 RT 厂家栈互斥，独立立项。

另：noble 镜像（用户日常系统）确实有 libVK_IMG（md5 22302f1e 与我们一致）
——用户记忆中的"GPU 能用"是 OpenCL + GLES，Vulkan 枚举从未工作过。
