# A733 E902 firmware.
#
# Needs the T-Head Xuantie bare-metal GNU toolchain (elf-newlib flavour).
# Point CROSS at its bin/<prefix>, e.g.
#   make CROSS=~/toolchains/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0/bin/riscv64-unknown-elf-
# or export XUANTIE_DIR and let the default below find it.
#
# The A733's E902 is RV32EMC: the vendor scp.fex contains 87 mul/div
# instructions, so the M extension is present.

LOAD_ADDR   ?= 0x40020000

XUANTIE_DIR ?= $(HOME)/Desktop/orangepi-build/e902/toolchains/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0
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

SRCS = src/start.S src/soc.c src/pinmux.c src/uart.c src/timer.c src/msgbox.c \
       src/clic.c src/spi.c src/heartbeat.c src/wdt.c src/clocks.c src/ts_test.c \
       src/power.c src/gintc.c src/main.c
OBJS = $(addprefix build/,$(addsuffix .o,$(basename $(notdir $(SRCS)))))

TARGET = build/fw

.PHONY: all clean check-toolchain dis size min min2 min3 scpfw hosttest

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

# every object depends on the shared headers: without this, editing a
# register macro in a733.h or an HB_* index in fw.h silently left stale
# objects in the image (T31: heartbeat.o kept HB_N = 24)
HDRS = src/a733.h src/fw.h src/spi.h
$(OBJS): $(HDRS)

build/%.o: src/%.S | build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS) src/fw.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo "--> $@  ($$(stat -c%s $@) bytes, loads at $(LOAD_ADDR))"

size: $(TARGET).elf
	@$(SIZE) $<

dis: $(TARGET).elf
	$(OBJDUMP) -d -S $< > build/fw.dis
	@echo "--> build/fw.dis"

# Minimum "is the core executing at all" image (writes a counter to SRAM only).
min: build/min.elf
build/min.elf: src/min.c src/start.S src/fw.ld
	$(CC) $(CFLAGS) -c src/min.c -o build/min.o
	$(CC) $(CFLAGS) -c src/start.S -o build/min_start.o
	$(CC) $(LDFLAGS) build/min_start.o build/min.o -o $@
	$(OBJCOPY) -O binary $@ build/fw_min.bin
	@echo "--> build/fw_min.bin"

# min2: scratch probe at 0x4001E000, no peripheral init at all.
min2: build/min2.elf
build/min2.elf: src/min2.c src/start.S src/fw.ld
	$(CC) $(CFLAGS) -c src/min2.c -o build/min2.o
	$(CC) $(CFLAGS) -c src/start.S -o build/min2_start.o
	$(CC) $(LDFLAGS) build/min2_start.o build/min2.o -o $@
	$(OBJCOPY) -O binary $@ build/fw_min2.bin
	@echo "--> build/fw_min2.bin"

# min3: staged assembly probe, writes a marker after each init step.
min3: build/min3.elf
build/min3.elf: src/min3.S src/fw.ld
	$(CC) $(CFLAGS) -c src/min3.S -o build/min3.o
	$(CC) $(LDFLAGS) build/min3.o -o $@
	$(OBJCOPY) -O binary $@ build/fw_min3.bin
	@echo "--> build/fw_min3.bin"

# scpfw: the flavour meant to be started BY bl31, linked at 0x40004000 and
# padded to exactly the boot-package scp slot size (0x19DB8 = 105912 bytes).
SCP_SLOT = 105912
scpfw: build/fw-scp-padded.bin

build/fw-scp.elf: $(OBJS) src/fw-scp.ld
	$(CC) $(ARCH_FLAGS) -nostdlib -nostartfiles -static -no-pie \
	  -Wl,--build-id=none -Tsrc/fw-scp.ld -Wl,--gc-sections \
	  -Wl,-Map=build/fw-scp.map $(OBJS) -o $@

build/fw-scp.bin: build/fw-scp.elf
	$(OBJCOPY) -O binary $< $@
	@echo "--> $@ ($$(stat -c%s $@) bytes, linked for 0x40004000)"

build/fw-scp-padded.bin: build/fw-scp.bin
	@sz=$$(stat -c%s build/fw-scp.bin); \
	 if [ $$sz -gt $(SCP_SLOT) ]; then echo "ERROR: $$sz > $(SCP_SLOT)"; exit 1; fi; \
	 dd if=/dev/zero of=$@ bs=1 count=$(SCP_SLOT) status=none; \
	 dd if=build/fw-scp.bin of=$@ bs=1 conv=notrunc status=none; \
	 echo "--> $@ ($$(stat -c%s $@) bytes)"; \
	 sha256sum $@

# hosttest: the mailbox protocol code (msgbox.c + main.c) built for the host
# against mock FIFOs, driven with the request patterns bl31 really sends.
# The generated a733.h shim routes readl/writel through the mocks.
HOSTCC ?= cc
HOST_CFLAGS = -O1 -g -Wall -Wno-unused-parameter -Wno-pointer-to-int-cast \
              -Wno-int-to-pointer-cast -include build/host/a733.h -Isrc
hosttest: build/host/mbox_test
	./build/host/mbox_test

build/host/a733.h: src/a733.h | build
	@mkdir -p build/host
	sed -e 's|^#define readl(a).*|u32 mock_readl(unsigned long a);\n#define readl(a) mock_readl((unsigned long)(a))|' \
	    -e 's|^#define writel(v, a).*|void mock_writel(u32 v, unsigned long a);\n#define writel(v, a) mock_writel((u32)(v), (unsigned long)(a))|' \
	    -e 's|^#define readb(a).*|u8 mock_readb(unsigned long a);\n#define readb(a) mock_readb((unsigned long)(a))|' \
	    -e 's|^#define writeb(v, a).*|void mock_writeb(u8 v, unsigned long a);\n#define writeb(v, a) mock_writeb((u8)(v), (unsigned long)(a))|' \
	    $< > $@

build/host/mbox_test: tests/host/mbox_test.c src/msgbox.c src/main.c src/fw.h src/spi.h build/host/a733.h
	$(HOSTCC) $(HOST_CFLAGS) -c src/msgbox.c -o build/host/msgbox.o
	$(HOSTCC) $(HOST_CFLAGS) -Dmain=fw_main -c src/main.c -o build/host/main.o
	$(HOSTCC) $(HOST_CFLAGS) -c tests/host/mbox_test.c -o build/host/mbox_test.o
	$(HOSTCC) build/host/*.o -o $@

clean:
	rm -rf build
