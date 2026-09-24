# A733 E902 firmware.
#
# Needs the T-Head Xuantie bare-metal GNU toolchain (elf-newlib flavour).
# Point CROSS at its bin/<prefix>, e.g.
#   make CROSS=~/toolchains/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0/bin/riscv64-unknown-elf-
# or export XUANTIE_DIR and let the default below find it.
#
# The A733's E902 is RV32EMC: the vendor scp.fex contains 87 mul/div
# instructions, so the M extension is present.

XUANTIE_DIR ?= $(HOME)/Desktop/orangepi-build/toolchains/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0
CROSS       ?= $(XUANTIE_DIR)/bin/riscv64-unknown-elf-

CC      = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
OBJDUMP = $(CROSS)objdump
SIZE    = $(CROSS)size

# _zicsr is needed explicitly: since binutils 2.36 the CSR instructions are a
# separate extension rather than part of the base ISA, and start.S uses
# csrw/csrr/csrsi for mtvec, mtvt (0x307), mstatus, mcause and mepc.
ARCH_FLAGS = -march=rv32emc_zicsr -mabi=ilp32e

CFLAGS = $(ARCH_FLAGS) -Os -g \
         -ffreestanding -fno-builtin -fno-common -fno-pic -fno-pie \
         -ffunction-sections -fdata-sections \
         -Wall -Wextra -Wno-unused-parameter \
         -Isrc

# -no-pie/-static matter: a PIE link pulls in .interp, .dynamic, .got and
# R_RISCV_RELATIVE relocations, all of which land inside the flat binary and
# make it unloadable (nothing is there to apply the relocations at runtime).
LDFLAGS = $(ARCH_FLAGS) -nostdlib -nostartfiles -static -no-pie \
          -Wl,--build-id=none \
          -Tsrc/fw.ld -Wl,--gc-sections -Wl,-Map=build/fw.map

SRCS = src/start.S src/soc.c src/pinmux.c src/uart.c src/timer.c src/msgbox.c src/clic.c src/main.c
OBJS = $(addprefix build/,$(addsuffix .o,$(basename $(notdir $(SRCS)))))

TARGET = build/fw

.PHONY: all clean check-toolchain dis size

all: check-toolchain $(TARGET).bin size

check-toolchain:
	@command -v $(CC) >/dev/null 2>&1 || { \
	  echo "ERROR: $(CC) not found."; \
	  echo "Install the Xuantie elf-newlib toolchain and set CROSS or XUANTIE_DIR."; \
	  echo "  see scripts/get-toolchain.sh"; \
	  exit 1; }
	@$(CC) $(ARCH_FLAGS) -E -x c /dev/null -o /dev/null 2>/dev/null || { \
	  echo "ERROR: $(CC) rejects $(ARCH_FLAGS) -- wrong toolchain flavour?"; \
	  echo "You need the bare-metal (elf-newlib) build, not linux-glibc."; \
	  exit 1; }

build:
	@mkdir -p build

build/%.o: src/%.S | build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS) src/fw.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo "--> $@  ($$(stat -c%s $@) bytes, loads at 0x40014000)"

size: $(TARGET).elf
	@$(SIZE) $<

dis: $(TARGET).elf
	$(OBJDUMP) -d -S $< > build/fw.dis
	@echo "--> build/fw.dis"

clean:
	rm -rf build
