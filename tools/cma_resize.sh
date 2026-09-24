#!/bin/sh
# cma_resize.sh <show | set <MB> | rollback>
#
# Resize the kernel's global CMA region on the board via the bootargs override
# (kernel/dma/contiguous.c: a `cma=` boot argument overrides
# CONFIG_CMA_SIZE_MBYTES, no kernel rebuild needed). Edits /boot/boot.cmd,
# regenerates /boot/boot.scr with mkimage, takes backups.
#
# MEASURED CONTEXT (2026-09-17, round 10 -- why this is a tool, not a fix):
#   The camera stack does NOT allocate from the global CMA on this board. Every
#   vin device node carries an `iommus` property (vinc*/isp/tdm ->
#   sunxi-iommu), so the 3.46 MB videobuf2_dma_contig capture buffers are
#   IOVA-contiguous and physically scattered: CmaFree stays at 13952 kB under a
#   full 1200p120 + G2D + NPU fan-out load, identical to idle. Enlarging CMA
#   changes nothing for the camera pipeline; it only matters for a FUTURE
#   consumer that needs physically contiguous memory without an IOMMU.
#
# Examples:
#   sh tools/cma_resize.sh show          # current bootargs + CmaTotal
#   sh tools/cma_resize.sh set 64        # cma=64M after next reboot
#   sh tools/cma_resize.sh rollback      # restore the shipped boot.cmd
# Run on the board (needs root for /boot and reboot for the effect):
#   printf ' \n' | sudo -S -p '' sh tools/cma_resize.sh set 64
set -e
BOOT_CMD=/boot/boot.cmd
BOOT_SCR=/boot/boot.scr
BACKUP=$BOOT_CMD.orig

show()
{
	echo "cmdline: $(cat /proc/cmdline)"
	grep Cma /proc/meminfo || true
	echo "bootargs line:"
	grep -n 'setenv bootargs' "$BOOT_CMD"
}

rollback()
{
	[ -f "$BACKUP" ] || { echo "no $BACKUP, nothing to roll back"; exit 1; }
	cp "$BACKUP" "$BOOT_CMD"
	mkimage -A arm -O linux -T script -C none -a 0 -e 0 \
		-n "boot" -d "$BOOT_CMD" "$BOOT_SCR" > /dev/null
	sync
	echo "rolled back to the shipped boot.cmd; boot.scr regenerated (reboot to apply)"
}

[ -f "$BOOT_CMD" ] || { echo "ERROR: $BOOT_CMD missing" >&2; exit 2; }
[ -x /usr/bin/mkimage ] || { echo "ERROR: mkimage missing (u-boot-tools)" >&2; exit 2; }

case "${1:-}" in
show) show ;;
rollback) rollback ;;
set)
	[ -n "${2:-}" ] || { echo "usage: cma_resize.sh set <MB>" >&2; exit 2; }
	case "$2" in (*[!0-9]*|'') echo "ERROR: <MB> must be a number" >&2; exit 2;; esac
	[ -f "$BACKUP" ] || cp "$BOOT_CMD" "$BACKUP"
	# remove any previous cma= from this tool, then insert the new one right
	# after clk_ignore_unused so it is easy to find again
	sed -i 's/ cma=[0-9]*M//g' "$BOOT_CMD"
	sed -i "s/clk_ignore_unused /clk_ignore_unused cma=$2M /" "$BOOT_CMD"
	grep -q "cma=$2M" "$BOOT_CMD" || { echo "ERROR: edit did not stick" >&2; exit 1; }
	mkimage -A arm -O linux -T script -C none -a 0 -e 0 \
		-n "boot" -d "$BOOT_CMD" "$BOOT_SCR" > /dev/null
	sync
	echo "boot.cmd now sets cma=$2M (backup: $BACKUP); reboot to apply, then check:"
	echo "  grep Cma /proc/meminfo   # expect CmaTotal: $2 * 1024 kB"
	show
;;
*)
	echo "usage: cma_resize.sh <show | set <MB> | rollback>" >&2
	exit 2
;;
esac
