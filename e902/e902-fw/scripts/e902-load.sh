#!/bin/sh
# Load and start firmware on the A733's E902, from a running Linux.
# Run as root ON THE BOARD.
#
# No reboot needed: stop the core, copy the image into SRAM_A2, point the
# reset vector at it, release reset. Repeat as often as you like.
#
# Registers (A733 manual):
#   0x0701021C  RISCV_BGR_REG            4.2.5.24  bit16 rst, bit1/0 gating
#   0x07032204  E902_RST_START_ADDR_REG  5.2.4.8   must be set BEFORE reset release
#
# ADDRESSES -- read this before changing any constant here.
#
# SRAM_A2 lives at two different addresses, and 0x40000000 means two
# different things depending on which core is asking:
#
#   E902 view : SRAM_A2  0x40000000..0x40033FFF   (manual ch.2, and fw.ld)
#   ARM  view : SRAM_A2  0x00040000               (u-boot cpu_autogen.h:5,
#                                                  SUNXI_SRAM_A2_BASE)
#   ARM  view : 0x40000000 is the base of DRAM    (/proc/iomem: System RAM)
#
# So LOAD_ADDR below is an E902-view address and must NOT be used as an
# ARM-side /dev/mem offset. An earlier version of this script did exactly
# that -- it wrote the image to ARM 0x40014000, which on this board is
# ordinary unreserved kernel memory, and since CONFIG_STRICT_DEVMEM is off
# the kernel allowed it: dd returned 0, the E902 received nothing, and 128K
# of live page cache / heap / slab was overwritten. awdevmem.py does the
# translation (ARM = 0x00040000 + (E902 - 0x40000000)) and reads the region
# back to verify, so a silent no-op like that cannot recur.
#
# E902_RST_START_ADDR reads 0x40004000 on a running vendor system, because
# that is where bl31 loads scp.fex -- not 0x40014000. We set the register
# explicitly rather than relying on any default.
set -eu

FW="${1:-fw.bin}"

# E902-view load address; matches ORIGIN in src/fw.ld.
#
# 0x40020000 (not 0x40014000): the vendor scp.fex lives at 0x40004000 and is
# 0x19DB8 bytes long, so it ends at 0x4001DDB8. The old base sat *inside* that
# image and forced e902-restore.sh to rewrite all 105912 bytes. From
# 0x40020000 the image is untouched, so handing the core back is a plain
# reset. SRAM_A2 runs to 0x40033FFF and the vendor stack starts at 0x4002F000,
# so 0x40020000..0x4002F000 is the safe window.
LOAD_ADDR=0x40020000

RISCV_BGR=0x0701021C
E902_START=0x07032204

# RISCV_BGR bit assignments (manual 4.2.5.24) -- note what these actually are:
#   bit16  RISCV_CFG_RST     reset for the RISCV_CFG *register block*, not the core
#   bit1   RISCV_CFG_GATING  bus clock for RISCV_CFG, i.e. for 0x07032xxx itself
#   bit0   RISCV_GATING      bus clock for the RISCV core
#
# There is NO core-reset bit here. Stopping the core means clearing bit0.
#
# Writing 0x00000000 to stop the core is a trap: it also gates and resets the
# RISCV_CFG block, which is where E902_RST_START_ADDR (0x07032204) lives. The
# register then reads back its old value and silently ignores writes, so the
# reset vector cannot be set. Measured on hardware: with BGR=0x00000000 the
# write to 0x07032204 is dropped; with BGR=0x00010002 it takes effect
# immediately.
BGR_STOP=0x00010002	# core clock OFF, cfg block up so we can still write it
BGR_RUN=0x00010003	# same, plus bit0: core clock ON

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MEM="$DIR/awdevmem.py"

die() { echo "error: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "must run as root"
[ -f "$FW" ] || die "firmware not found: $FW"
[ -r /dev/mem ] || die "/dev/mem not readable"
[ -f "$MEM" ] || die "helper not found: $MEM"
command -v python3 >/dev/null 2>&1 || die "python3 required (it is on the stock image)"

rd() { python3 "$MEM" read "$1" | sed 's/.*: //'; }
wr() { python3 "$MEM" write "$1" "$2" >/dev/null; }

SIZE=$(stat -c%s "$FW")
echo "firmware : $FW ($SIZE bytes)"
echo "load addr: $LOAD_ADDR (E902 view)"

# SRAM_A2 ends at 0x40034000 in the E902 view, and the vendor SCP's stack
# tops out at 0x4002F000, so the usable window above LOAD_ADDR is 0xF000.
if [ "$SIZE" -gt 61440 ]; then
	die "firmware is $SIZE bytes, exceeds the 60K window above $LOAD_ADDR (0x4002F000)"
fi

echo
echo "before: RISCV_BGR=$(rd $RISCV_BGR)  RST_START=$(rd $E902_START)"

echo "1/4 asserting reset ..."
wr "$RISCV_BGR" "$BGR_STOP"

echo "2/4 copying image into SRAM_A2 (verified read-back) ..."
python3 "$MEM" load "$FW" --e902 "$LOAD_ADDR" \
	|| die "load into SRAM_A2 failed -- core is left in reset, nothing started"

echo "3/4 setting reset vector ..."
wr "$E902_START" "$LOAD_ADDR"
[ "$(rd $E902_START)" = "0x$(printf '%08X' $LOAD_ADDR)" ] \
	|| die "reset vector did not stick (reads $(rd $E902_START)); core left in reset"

echo "4/4 releasing reset ..."
wr "$RISCV_BGR" "$BGR_RUN"

echo
echo "after : RISCV_BGR=$(rd $RISCV_BGR)  RST_START=$(rd $E902_START)"
echo
echo "E902 should be running. Watch its output on the S_UART0 pins"
echo "(PL2 = TX, PL3 = RX), 115200 8N1."
echo
echo "This overwrote part of the vendor scp.fex, which bl31 loaded at"
echo "0x40004000 spanning 0x4001DDB8 -- $LOAD_ADDR is inside it. Use"
echo "e902-restore.sh with the full scp.fex to put the vendor core back."
