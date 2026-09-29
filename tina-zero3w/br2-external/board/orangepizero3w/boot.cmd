# Orange Pi Zero 3W (A733) — SD boot script for the Tina/Buildroot image.
# 地址布局沿用已验证的 boot-sun60iw2.cmd（U-Boot 2018.07 默认把内核压在 32 MiB 里，
# 我们的 uImage 带 ftrace 更大 —— 内核 48 MiB、initrd 48 MiB，都低于 bl31 的 0x48000000）。
setenv kernel_addr_r "0x41000000"
setenv fdt_addr_r "0x44000000"
setenv ramdisk_addr_r "0x45000000"

echo "Zero 3W boot script from ${devtype} ${devnum}"

# root 分区用运行时解析的 PARTUUID，不写死设备号
part uuid ${devtype} ${devnum}:1 partuuid

# RT：独占 A55 cpu5（A55 小核 cpu0-5，A76 cpu6-7 留给其他负载）
# quiet + loglevel=3：115200 串口打几千行 dmesg 要花好几秒（真实耗时，不是显示耗时）；
# 内核日志仍完整保留在缓冲区，板上 dmesg 可查
setenv bootargs "console=ttyS0,115200 quiet loglevel=3 root=PARTUUID=${partuuid} rootwait rw clk_ignore_unused swiotlb=65536 isolcpus=5 nohz_full=5 rcu_nocbs=5"

if load ${devtype} ${devnum} ${ramdisk_addr_r} ${prefix}uInitrd; then
	setenv initrd_arg ${ramdisk_addr_r}
else
	setenv initrd_arg -
fi

load ${devtype} ${devnum} ${kernel_addr_r} ${prefix}uImage
load ${devtype} ${devnum} ${fdt_addr_r} ${prefix}sun60i-a733-orangepi-zero3w.dtb
fdt addr ${fdt_addr_r}
# U-Boot 要往 dtb 里追加 /chosen（bootargs、initrd 等），不加 fdt resize 会
# FDT_ERR_NOSPACE 直接挂死（"/chosen node create failed"）
fdt resize 65536

bootm ${kernel_addr_r} ${initrd_arg} ${fdt_addr_r}

# Recompile with:
# mkimage -C none -A arm -T script -d boot.cmd boot.scr
