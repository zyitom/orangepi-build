#!/bin/bash

# arguments: $RELEASE $LINUXFAMILY $BOARD $BUILD_DESKTOP
#
# This is the image customization script

# NOTE: It is copied to /tmp directory inside the image
# and executed there inside chroot environment
# so don't reference any files that are not already installed

# NOTE: If you want to transfer files between chroot and host
# userpatches/overlay directory on host is bind-mounted to /tmp/overlay in chroot
# The sd card's root path is accessible via $SDCARD variable.

RELEASE=$1
LINUXFAMILY=$2
BOARD=$3
BUILD_DESKTOP=$4

Main() {
	case $RELEASE in
		xenial)
			# your code here
			;;
		stretch)
			# your code here
			# InstallOpenMediaVault # uncomment to get an OMV 4 image
			;;
		buster)
			# your code here
			;;
		bullseye)
			# your code here
			;;
		bionic)
			# your code here
			;;
		focal)
			# your code here
			;;
		bookworm|jammy|noble)
			# 最小镜像默认不带 WiFi supplicant：NetworkManager 会报
			# "Failed to D-Bus activate wpa_supplicant service"，把 wlan0
			# 标成 unavailable、扫描被拒。补上，否则镜像没有 WiFi。
			apt-get -y -qq install wpasupplicant >/dev/null 2>&1 || \
				echo "customize-image: wpasupplicant install failed"
			# RT / 相机测试工具：cyclictest、trace-cmd、压力负载，以及板上编
			# ar0234 userspace 用的 g++/make。rtla 不在循环里：Ubuntu/Debian
			# 的发行版都没有 rtla 包，装了必然失败；二进制走下面的 overlay 拷贝。
			# 逐个装，某个包缺了不影响其它的。
			for p in rt-tests trace-cmd stress-ng v4l-utils build-essential gpiod; do
				apt-get -y -qq install $p >/dev/null 2>&1 || \
					echo "customize-image: $p install failed"
			done
		# rtla（timerlat/osnoise）：userpatches/overlay/rtla/rtla 是从
		# kernel 6.6 的 tools/tracing/rtla 交叉编的，不依赖内核版本，
		# 只要 tracefs 和 libtraceevent/libtracefs（trace-cmd 会带上）。
		if [ -f /tmp/overlay/rtla/rtla ]; then
			install -m 755 /tmp/overlay/rtla/rtla /usr/local/bin/rtla && \
				echo "customize-image: rtla installed from overlay"
		fi
		# mpp 库 SIGSEGV 哨兵：Xunlong jammy vendor mpp 1.5.0-2（2023-02 构建）的
		# MppBufferService::get_group 在分配器创建失败时只打一条默认级别被压制的
		# 断言就返回半初始化的组，首次 mpp_buffer_create 解引用空指针，gst 管线
		# 以"挂起"假象永久卡死（详见 rt-check/patch-mpp-jammy-segv.py）。
		# 缓存里的 librockchip-mpp1 deb 已用该脚本重打包；这里校验实际装进镜像
		# 的库确实不含缺陷（noble 的 PPA 版 2024-07 已含上游修复，同样通过）。
		if grep -q "p->allocator" /usr/lib/aarch64-linux-gnu/librockchip_mpp.so.0 2>/dev/null; then
			echo "customize-image: WARNING librockchip_mpp.so.0 still has the get_group NULL-allocator SIGSEGV bug (see rt-check/patch-mpp-jammy-segv.py)"
		else
			echo "customize-image: librockchip_mpp.so.0 OK (get_group NULL-allocator bug absent)"
		fi
		# dma-heap 节点别名：BSP 6.1 内核只建 system/system-uncached/reserved 三个
		# 堆，而 vendor mpp 1.5.0-2 的默认堆名是 system-uncached-dma32（新内核才
		# 有）。root 进程（is_valid 探测通过）会走 dma-heap 并因 ENOENT 静默失败，
		# 首次 mpp_buffer_create 即 SEGV；udev 规则补齐别名后 root/用户路径都正常。
		# 本板 4GB RAM 全在 4GB 以下且编解码器前有 IOMMU，-dma32 语义可安全等同。
		if [ -f /tmp/overlay/dma-heap/60-dma-heap-aliases.rules ]; then
			install -m 644 /tmp/overlay/dma-heap/60-dma-heap-aliases.rules \
				/etc/udev/rules.d/60-dma-heap-aliases.rules && \
				echo "customize-image: dma-heap alias rules installed"
		else
			echo "customize-image: dma-heap alias rules missing in overlay"
		fi
		# GLX 硬件渲染（jammy 桌面）：vendor 的 panfork mesa（kbase 后端，与 blob 共用
		# 内核驱动）让 GLX 应用走 G610；发行版自带 mesa 没有 G610 驱动，GLX 只能
		# llvmpipe 软渲染。装的是 Xunlong jammy GNOME 镜像同款 debs（overlay 拷入）。
		# EGL 保持 mali blob 为默认（glvnd 按 ICM 文件名排序 50-mali 在前），
		# panfork 只接管 GLX——与板上实测一致（glxinfo: Mali-G610, Accelerated: yes）。
		if [ "$RELEASE" = jammy ] && ls /tmp/overlay/mesa-jammy/*.deb >/dev/null 2>&1; then
			dpkg -i /tmp/overlay/mesa-jammy/*.deb >/dev/null 2>&1 && \
				echo "customize-image: panfork mesa installed for GLX hw rendering" || \
				echo "customize-image: panfork mesa install failed"
		fi
			# Ubuntu noble 移除了 pam_lastlog.so，但 /etc/pam.d/login 还在引用，
			# 每次控制台登录报 "PAM unable to dlopen"。
			sed -i '/pam_lastlog/d' /etc/pam.d/login 2>/dev/null || true
			if [ "$BOARD" = orangepizero3w ]; then Zero3wTweaks; fi
			if [ "$LINUXFAMILY" = "rockchip-rk3588" ]; then RockchipRk3588Noble; fi
			;;
	esac
} # Main

# Zero 3W 板级修正（文件与 Buildroot 镜像共用：userpatches/overlay/zero3w/README.md）
Zero3wTweaks() {
	local Z=/tmp/overlay/zero3w
	# WiFi：AIC8800 开省电时漏回 AP 的保活探测，每 5-10 分钟被以 reason=4 断开
	install -D -m 644 $Z/70-wifi-powersave-off.rules /etc/udev/rules.d/70-wifi-powersave-off.rules
	mkdir -p /etc/NetworkManager/conf.d
	printf '[connection]\nwifi.powersave = 2\n' > /etc/NetworkManager/conf.d/zero3w-wifi-powersave-off.conf
	# 实时核（cpu5）所在的 A55 簇固定频率：schedutil 下每秒调压 ~100 次（I2C 写 PMIC）
	# 休眠默认 s2idle：deep 进固件后醒不来（RTC 闹钟/WoWLAN 都试过）
	install -D -m 644 $Z/zero3w-sleep.conf /etc/tmpfiles.d/zero3w-sleep.conf
	install -m 644 $Z/zero3w-rt-cpufreq.service /etc/systemd/system/
	systemctl enable zero3w-rt-cpufreq.service >/dev/null 2>&1
	# 蓝牙：厂家 hciattach 的 aic 初始化总写 10:11:12:13:14:15，改成每颗芯片自己的地址
	if gcc -O2 -Wall -o /usr/local/bin/aic-btaddr $Z/aic-btaddr.c; then
		install -m 644 $Z/zero3w-btaddr.service /etc/systemd/system/
		systemctl enable zero3w-btaddr.service >/dev/null 2>&1
	else
		echo "customize-image: aic-btaddr build failed"
	fi
}

# Orange Pi 5 (rk3588) noble：GPU / NPU / 硬编解码用户态。官方 RK3588 没有
# noble 支持（rk35xx_packages 分支无 noble 目录），这里复刻 ubuntu-rockchip 在
# noble + Rockchip 6.1 内核上验证过的栈，全部来自 Launchpad PPA，构建时需要
# 网络。PPA 会留在镜像的 apt 源里，板上 apt upgrade 继续跟上游：
#   GPU    ppa:jjriek/panfork-mesa（mesa 23.0.5 panfork，dist-upgrade 接管发行版
#          mesa）+ libmali-g610-x11 blob + mesa-opencl-icd（G610 上的 OpenCL）
#   编解码 ppa:jjriek/rockchip-multimedia（mpp / rga / gst-rkmpp / 带 rkmpp 的
#          ffmpeg / rkaiq 摄像头 ISP）
#   NPU    rknpu2 v1.5.2 运行时（userpatches/overlay/rknpu2/，与官方
#          rk35xx_packages 分支同版本；内核 CONFIG_ROCKCHIP_RKNPU=y 已内置），
#          rknn_server 起 systemd 服务
RockchipRk3588Noble() {
	# add-apt-repository 要连 keyserver.ubuntu.com:11371（HKP）拉密钥，墙内必挂
	# （第一版构建时两个 PPA 都因此失败）。改成宿主机预先取好密钥放进
	# overlay/apt-keys/jjriek-ppa.gpg —— 两个 PPA 同一把签名密钥，指纹
	# 3CC0D9D1F3F0354B50D24F51F02122ECF25FB4D7，已用它离线验证过两个仓库的
	# InRelease 签名。仓库本身走 http 的 ppa.launchpadcontent.net，可达。
	if [ -f /tmp/overlay/apt-keys/jjriek-ppa.gpg ]; then
		install -D -m 644 /tmp/overlay/apt-keys/jjriek-ppa.gpg /etc/apt/keyrings/jjriek-ppa.gpg
		for ppa in panfork-mesa rockchip-multimedia; do
			echo "deb [signed-by=/etc/apt/keyrings/jjriek-ppa.gpg] http://ppa.launchpadcontent.net/jjriek/$ppa/ubuntu noble main" \
				> /etc/apt/sources.list.d/jjriek-$ppa.list
		done
	else
		echo "customize-image: jjriek-ppa.gpg missing in overlay"
	fi
	apt-get -qq update >/dev/null 2>&1

	# 无值守装包：BSP 预置的 /etc/pulse/client.conf 与 libpulse0 包撞车时 dpkg
	# 会弹 conffile 提问，EOF 直接判失败并连锁拖死 ffmpeg 整串——必须带
	# force-confdef/force-confold（构建系统 OMV 那段同款做法）。
	local APT="env DEBIAN_FRONTEND=noninteractive apt-get -y -qq -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold"

	# GPU：先装固件；dist-upgrade 让 panfork mesa 覆盖发行版 mesa；再装 blob 和 OpenCL
	$APT install mali-g610-firmware >/dev/null 2>&1 || \
		echo "customize-image: mali-g610-firmware install failed"
	$APT dist-upgrade >/dev/null 2>&1 || \
		echo "customize-image: dist-upgrade (panfork mesa) failed"
	$APT install libmali-g610-x11 mesa-opencl-icd >/dev/null 2>&1 || \
		echo "customize-image: libmali/opencl install failed"
	echo "PAN_MESA_DEBUG=gofaster" >> /etc/environment

	# 硬编解码 + 摄像头 ISP（-dev 包方便板上直接编自己的用户态）
	$APT install librga2 librga-dev librockchip-mpp1 librockchip-mpp-dev \
		rockchip-mpp-demos libv4l-rkmpp libv4l-dev gstreamer1.0-rockchip1 ffmpeg \
		camera-engine-rkaiq-rk3588 >/dev/null 2>&1 || \
		echo "customize-image: multimedia install failed"

	# NPU 运行时：官方 tar 解到根（librknnrt.so / rknn_server / demo 模型）
	if tar -xf /tmp/overlay/rknpu2/rknpu2-v1.5.2.tar -C / 2>/dev/null; then
		cat > /etc/systemd/system/rknn-server.service <<-__EOF__
		[Unit]
		Description=RKNN NPU runtime server
		After=multi-user.target

		[Service]
		Type=simple
		ExecStart=/usr/bin/rknn_server
		Restart=always

		[Install]
		WantedBy=multi-user.target
		__EOF__
		systemctl enable rknn-server.service >/dev/null 2>&1
	else
		echo "customize-image: rknpu2 overlay tar missing"
	fi
}

InstallOpenMediaVault() {
	# use this routine to create a Debian based fully functional OpenMediaVault
	# image (OMV 3 on Jessie, OMV 4 with Stretch). Use of mainline kernel highly
	# recommended!
	#
	# Please note that this variant changes Orange Pi default security 
	# policies since you end up with root password 'openmediavault' which
	# you have to change yourself later. SSH login as root has to be enabled
	# through OMV web UI first
	#
	# This routine is based on idea/code courtesy Benny Stark. For fixes,
	# discussion and feature requests please refer to
	# https://forum.armbian.com/index.php?/topic/2644-openmediavault-3x-customize-imagesh/

	echo root:openmediavault | chpasswd
	rm /root/.not_logged_in_yet
	. /etc/default/cpufrequtils
	export LANG=C LC_ALL="en_US.UTF-8"
	export DEBIAN_FRONTEND=noninteractive
	export APT_LISTCHANGES_FRONTEND=none

	case ${RELEASE} in
		jessie)
			OMV_Name="erasmus"
			OMV_EXTRAS_URL="https://github.com/OpenMediaVault-Plugin-Developers/packages/raw/master/openmediavault-omvextrasorg_latest_all3.deb"
			;;
		stretch)
			OMV_Name="arrakis"
			OMV_EXTRAS_URL="https://github.com/OpenMediaVault-Plugin-Developers/packages/raw/master/openmediavault-omvextrasorg_latest_all4.deb"
			;;
	esac

	# Add OMV source.list and Update System
	cat > /etc/apt/sources.list.d/openmediavault.list <<- EOF
	deb https://openmediavault.github.io/packages/ ${OMV_Name} main
	## Uncomment the following line to add software from the proposed repository.
	deb https://openmediavault.github.io/packages/ ${OMV_Name}-proposed main
	
	## This software is not part of OpenMediaVault, but is offered by third-party
	## developers as a service to OpenMediaVault users.
	# deb https://openmediavault.github.io/packages/ ${OMV_Name} partner
	EOF

	# Add OMV and OMV Plugin developer keys, add Cloudshell 2 repo for XU4
	if [ "${BOARD}" = "odroidxu4" ]; then
		add-apt-repository -y ppa:kyle1117/ppa
		sed -i 's/jessie/xenial/' /etc/apt/sources.list.d/kyle1117-ppa-jessie.list
	fi
	mount --bind /dev/null /proc/mdstat
	apt-get update
	apt-get --yes --force-yes --allow-unauthenticated install openmediavault-keyring
	apt-key adv --keyserver hkp://keyserver.ubuntu.com:80 --recv-keys 7AA630A1EDEE7D73
	apt-get update

	# install debconf-utils, postfix and OMV
	HOSTNAME="${BOARD}"
	debconf-set-selections <<< "postfix postfix/mailname string ${HOSTNAME}"
	debconf-set-selections <<< "postfix postfix/main_mailer_type string 'No configuration'"
	apt-get --yes --force-yes --allow-unauthenticated  --fix-missing --no-install-recommends \
		-o Dpkg::Options::="--force-confdef" -o Dpkg::Options::="--force-confold" install \
		debconf-utils postfix
	# move newaliases temporarely out of the way (see Ubuntu bug 1531299)
	cp -p /usr/bin/newaliases /usr/bin/newaliases.bak && ln -sf /bin/true /usr/bin/newaliases
	sed -i -e "s/^::1         localhost.*/::1         ${HOSTNAME} localhost ip6-localhost ip6-loopback/" \
		-e "s/^127.0.0.1   localhost.*/127.0.0.1   ${HOSTNAME} localhost/" /etc/hosts
	sed -i -e "s/^mydestination =.*/mydestination = ${HOSTNAME}, localhost.localdomain, localhost/" \
		-e "s/^myhostname =.*/myhostname = ${HOSTNAME}/" /etc/postfix/main.cf
	apt-get --yes --force-yes --allow-unauthenticated  --fix-missing --no-install-recommends \
		-o Dpkg::Options::="--force-confdef" -o Dpkg::Options::="--force-confold" install \
		openmediavault

	# install OMV extras, enable folder2ram and tweak some settings
	FILE=$(mktemp)
	wget "$OMV_EXTRAS_URL" -qO $FILE && dpkg -i $FILE
	
	/usr/sbin/omv-update
	# Install flashmemory plugin and netatalk by default, use nice logo for the latter,
	# tweak some OMV settings
	. /usr/share/openmediavault/scripts/helper-functions
	apt-get -y -q install openmediavault-netatalk openmediavault-flashmemory
	AFP_Options="mimic model = Macmini"
	SMB_Options="min receivefile size = 16384\nwrite cache size = 524288\ngetwd cache = yes\nsocket options = TCP_NODELAY IPTOS_LOWDELAY"
	xmlstarlet ed -L -u "/config/services/afp/extraoptions" -v "$(echo -e "${AFP_Options}")" /etc/openmediavault/config.xml
	xmlstarlet ed -L -u "/config/services/smb/extraoptions" -v "$(echo -e "${SMB_Options}")" /etc/openmediavault/config.xml
	xmlstarlet ed -L -u "/config/services/flashmemory/enable" -v "1" /etc/openmediavault/config.xml
	xmlstarlet ed -L -u "/config/services/ssh/enable" -v "1" /etc/openmediavault/config.xml
	xmlstarlet ed -L -u "/config/services/ssh/permitrootlogin" -v "0" /etc/openmediavault/config.xml
	xmlstarlet ed -L -u "/config/system/time/ntp/enable" -v "1" /etc/openmediavault/config.xml
	xmlstarlet ed -L -u "/config/system/time/timezone" -v "UTC" /etc/openmediavault/config.xml
	xmlstarlet ed -L -u "/config/system/network/dns/hostname" -v "${HOSTNAME}" /etc/openmediavault/config.xml
	xmlstarlet ed -L -u "/config/system/monitoring/perfstats/enable" -v "0" /etc/openmediavault/config.xml
	echo -e "OMV_CPUFREQUTILS_GOVERNOR=${GOVERNOR}" >>/etc/default/openmediavault
	echo -e "OMV_CPUFREQUTILS_MINSPEED=${MIN_SPEED}" >>/etc/default/openmediavault
	echo -e "OMV_CPUFREQUTILS_MAXSPEED=${MAX_SPEED}" >>/etc/default/openmediavault
	for i in netatalk samba flashmemory ssh ntp timezone interfaces cpufrequtils monit collectd rrdcached ; do
		/usr/sbin/omv-mkconf $i
	done
	/sbin/folder2ram -enablesystemd || true
	sed -i 's|-j /var/lib/rrdcached/journal/ ||' /etc/init.d/rrdcached

	# Fix multiple sources entry on ARM with OMV4
	sed -i '/stretch-backports/d' /etc/apt/sources.list

	# rootfs resize to 7.3G max and adding omv-initsystem to firstrun -- q&d but shouldn't matter
	echo 15500000s >/root/.rootfs_resize
	sed -i '/systemctl\ disable\ orangepi-firstrun/i \
	mv /usr/bin/newaliases.bak /usr/bin/newaliases \
	export DEBIAN_FRONTEND=noninteractive \
	sleep 3 \
	apt-get install -f -qq python-pip python-setuptools || exit 0 \
	pip install -U tzupdate \
	tzupdate \
	read TZ </etc/timezone \
	/usr/sbin/omv-initsystem \
	xmlstarlet ed -L -u "/config/system/time/timezone" -v "${TZ}" /etc/openmediavault/config.xml \
	/usr/sbin/omv-mkconf timezone \
	lsusb | egrep -q "0b95:1790|0b95:178a|0df6:0072" || sed -i "/ax88179_178a/d" /etc/modules' /usr/lib/orangepi/orangepi-firstrun
	sed -i '/systemctl\ disable\ orangepi-firstrun/a \
	sleep 30 && sync && reboot' /usr/lib/orangepi/orangepi-firstrun

	# add USB3 Gigabit Ethernet support
	echo -e "r8152\nax88179_178a" >>/etc/modules

	case ${BOARD} in
		odroidxu4)
			HMP_Fix='; taskset -c -p 4-7 $i '
			# Cloudshell stuff (fan, lcd, missing serials on 1st CS2 batch)
			echo "H4sIAKdXHVkCA7WQXWuDMBiFr+eveOe6FcbSrEIH3WihWx0rtVbUFQqCqAkYGhJn
			tF1x/vep+7oebDfh5DmHwJOzUxwzgeNIpRp9zWRegDPznya4VDlWTXXbpS58XJtD
			i7ICmFBFxDmgI6AXSLgsiUop54gnBC40rkoVA9rDG0SHHaBHPQx16GN3Zs/XqxBD
			leVMFNAz6n6zSWlEAIlhEw8p4xTyFtwBkdoJTVIJ+sz3Xa9iZEMFkXk9mQT6cGSQ
			QL+Cr8rJJSmTouuuRzfDtluarm1aLVHksgWmvanm5sbfOmY3JEztWu5tV9bCXn4S
			HB8RIzjoUbGvFvPw/tmr0UMr6bWSBupVrulY2xp9T1bruWnVga7DdAqYFgkuCd3j
			vORUDQgej9HPJxmDDv+3WxblBSuYFH8oiNpHz8XvPIkU9B3JVCJ/awIAAA==" \
			| tr -d '[:blank:]' | base64 --decode | gunzip -c >/usr/local/sbin/cloudshell2-support.sh
			chmod 755 /usr/local/sbin/cloudshell2-support.sh
			apt install -y i2c-tools odroid-cloudshell cloudshell2-fan
			sed -i '/systemctl\ disable\ orangepi-firstrun/i \
			lsusb | grep -q -i "05e3:0735" && sed -i "/exit\ 0/i echo 20 > /sys/class/block/sda/queue/max_sectors_kb" /etc/rc.local \
			/usr/sbin/i2cdetect -y 1 | grep -q "60: 60" && /usr/local/sbin/cloudshell2-support.sh' /usr/lib/orangepi/orangepi-firstrun
			;;
		bananapim3|nanopifire3|nanopct3plus|nanopim3)
			HMP_Fix='; taskset -c -p 4-7 $i '
			;;
		edge*|ficus|firefly-rk3399|nanopct4|nanopim4|nanopineo4|renegade-elite|roc-rk3399-pc|rockpro64)
			HMP_Fix='; taskset -c -p 4-5 $i '
			;;
	esac
	echo "* * * * * root for i in \`pgrep \"ftpd|nfsiod|smbd|afpd|cnid\"\` ; do ionice -c1 -p \$i ${HMP_Fix}; done >/dev/null 2>&1" \
		>/etc/cron.d/make_nas_processes_faster
	chmod 600 /etc/cron.d/make_nas_processes_faster

	# add SATA port multiplier hint if appropriate
	[ "${LINUXFAMILY}" = "sunxi" ] && \
		echo -e "#\n# If you want to use a SATA PM add \"ahci_sunxi.enable_pmp=1\" to bootargs above" \
		>>/boot/boot.cmd

	# Filter out some log messages
	echo ':msg, contains, "do ionice -c1" ~' >/etc/rsyslog.d/omv-orangepi.conf
	echo ':msg, contains, "action " ~' >>/etc/rsyslog.d/omv-orangepi.conf
	echo ':msg, contains, "netsnmp_assert" ~' >>/etc/rsyslog.d/omv-orangepi.conf
	echo ':msg, contains, "Failed to initiate sched scan" ~' >>/etc/rsyslog.d/omv-orangepi.conf

	# Fix little python bug upstream Debian 9 obviously ignores
	if [ -f /usr/lib/python3.5/weakref.py ]; then
		wget -O /usr/lib/python3.5/weakref.py \
		https://raw.githubusercontent.com/python/cpython/9cd7e17640a49635d1c1f8c2989578a8fc2c1de6/Lib/weakref.py
	fi

	# clean up and force password change on first boot
	umount /proc/mdstat
	chage -d 0 root
} # InstallOpenMediaVault

UnattendedStorageBenchmark() {
	# Function to create Orange Pi images ready for unattended storage performance testing.
	# Useful to use the same OS image with a bunch of different SD cards or eMMC modules
	# to test for performance differences without wasting too much time.

	rm /root/.not_logged_in_yet

	apt-get -qq install time

	wget -qO /usr/local/bin/sd-card-bench.sh https://raw.githubusercontent.com/ThomasKaiser/sbc-bench/master/sd-card-bench.sh
	chmod 755 /usr/local/bin/sd-card-bench.sh

	sed -i '/^exit\ 0$/i \
	/usr/local/bin/sd-card-bench.sh &' /etc/rc.local
} # UnattendedStorageBenchmark

InstallAdvancedDesktop()
{
	apt-get install -yy transmission libreoffice libreoffice-style-tango meld remmina thunderbird kazam avahi-daemon
	[[ -f /usr/share/doc/avahi-daemon/examples/sftp-ssh.service ]] && cp /usr/share/doc/avahi-daemon/examples/sftp-ssh.service /etc/avahi/services/
	[[ -f /usr/share/doc/avahi-daemon/examples/ssh.service ]] && cp /usr/share/doc/avahi-daemon/examples/ssh.service /etc/avahi/services/
	apt clean
} # InstallAdvancedDesktop

Main "$@"
