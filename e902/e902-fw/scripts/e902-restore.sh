#!/bin/sh
# Put the vendor SCP firmware back on the E902, from a running Linux.
#
# This is the counterpart to e902-load.sh. Together they let you test custom
# firmware without touching u-boot at all: the stock scp.fex keeps loading
# normally at boot, and you only borrow the core while you are testing.
#
# Why this works: nothing in the Linux device tree references CLK_RISCV or
# RST_BUS_RISCV, so no driver holds a reference to the E902's clocks or reset.
# We can stop and restart the core behind Linux's back.
#
# What is lost while YOUR firmware runs (restored by this script):
#   - DRAM DFS      ccu-ddr.c:77 ARM_SVC_SUNXI_DDRFREQ has no responder
#   - suspend       irq-sunxi-wakeupgen.c:49 SET_WAKEUP_SRC has no responder
#   - PMIC watching E902 polls AXP515/AXP8191 over s_twi0
#
# Do NOT suspend while custom firmware is running -- see README.
#
# ADDRESSES: LOAD_ADDR is in the E902 view. SRAM_A2 is at 0x00040000 from
# the ARM and 0x40000000 from the E902, while ARM 0x40000000 is DRAM --
# awdevmem.py does the translation. See the comment block in e902-load.sh.
#
# The vendor image loads at 0x40004000, NOT 0x40014000. Measured on a
# running board: E902_RST_START_ADDR (0x07032204) reads 0x40004000, and the
# 105912 bytes of SRAM at that address match u-boot's scp.fex byte for byte
# except for 10 bytes of runtime scratch at 0x40010738..0x40010767.
set -eu

SCP="${1:-}"
LOAD_ADDR=0x40004000		# E902 view; where bl31 puts scp.fex
RISCV_BGR=0x0701021C
E902_START=0x07032204
# See the comment in e902-load.sh: RISCV_BGR bit16/bit1 belong to the
# RISCV_CFG register block (where 0x07032204 lives), bit0 is the core clock.
# Zeroing the whole register gates away the register we need to write next.
BGR_STOP=0x00010002	# core clock OFF, cfg block still accessible
BGR_RUN=0x00010003	# core clock ON

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MEM="$DIR/awdevmem.py"

die() { echo "error: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "must run as root"

# Locate the vendor firmware if not given explicitly.
if [ -z "$SCP" ]; then
	for c in ./scp.fex /lib/firmware/scp.fex /boot/scp.fex; do
		[ -f "$c" ] && { SCP="$c"; break; }
	done
fi
[ -n "$SCP" ] && [ -f "$SCP" ] || die "vendor scp.fex not found.
Copy it from the build tree first:
  u-boot/v2018.05-sun60iw2/scp.fex
then pass it as an argument, or drop it next to this script."

[ -r /dev/mem ] || die "/dev/mem not readable"
[ -f "$MEM" ] || die "helper not found: $MEM"
command -v python3 >/dev/null 2>&1 || die "python3 required (it is on the stock image)"

rd() { python3 "$MEM" read "$1" | sed 's/.*: //'; }
wr() { python3 "$MEM" write "$1" "$2" >/dev/null; }

SIZE=$(stat -c%s "$SCP")
echo "restoring: $SCP ($SIZE bytes) to $LOAD_ADDR (E902 view)"
echo "before   : RISCV_BGR=$(rd $RISCV_BGR)  RST_START=$(rd $E902_START)"

echo "1/4 asserting reset ..."
wr "$RISCV_BGR" "$BGR_STOP"

echo "2/4 writing vendor image (verified read-back) ..."
python3 "$MEM" load "$SCP" --e902 "$LOAD_ADDR" \
	|| die "load into SRAM_A2 failed -- core is left in reset, nothing started"

echo "3/4 setting reset vector ..."
wr "$E902_START" "$LOAD_ADDR"
[ "$(rd $E902_START)" = "0x$(printf '%08X' $LOAD_ADDR)" ] \
	|| die "reset vector did not stick (reads $(rd $E902_START)); core left in reset"

echo "4/4 releasing reset ..."
wr "$RISCV_BGR" "$BGR_RUN"

echo "after    : RISCV_BGR=$(rd $RISCV_BGR)  RST_START=$(rd $E902_START)"
echo
echo "Vendor SCP restarted. Note it boots cold here, without the parameter"
echo "block bl31 normally hands it (DRAM timings, DVFS voltage table, PMIC"
echo "config -- see u-boot drivers/arisc/arisc_i.h 'struct dts_cfg_64')."
echo "Basic services should come back; if DFS or suspend still misbehave,"
echo "reboot for a clean vendor-managed start."
