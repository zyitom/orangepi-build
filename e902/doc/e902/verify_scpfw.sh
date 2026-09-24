#!/bin/bash
cd /home/helios/Desktop/orangepi-build/e902/e902-fw || exit 1
TC=/home/helios/Desktop/orangepi-build/e902/toolchains/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0/bin/riscv64-unknown-elf-

echo "===== 1. sizes and hashes ====="
ls -la build/fw-scp.bin build/fw-scp-padded.bin
sha256sum build/fw-scp.bin build/fw-scp-padded.bin

echo
echo "===== 2. entry / sections (must be inside 0x40004000..0x4002F000) ====="
${TC}objdump -h build/fw-scp.elf | grep -E '^ *[0-9]+ (\.text|\.vectors|\.rodata|\.data|\.bss|\.stack)'
${TC}nm build/fw-scp.elf | grep -E ' (__start|trap_entry|vector_table|main|msgbox_send_startup_feedback)$'

echo
echo "===== 3. no relocations ====="
${TC}readelf -r build/fw-scp.elf | head -3

echo
echo "===== 4. first 16 bytes of the padded image (must match the vendor prologue) ====="
od -A d -t x1 -N 16 build/fw-scp-padded.bin
echo "vendor scp.fex prologue:"
od -A d -t x1 -N 16 ../../u-boot/v2018.05-sun60iw2/scp.fex

echo
echo "===== 5. the handshake must be in there: channel-3 regs and the feedback symbol ====="
${TC}objdump -d build/fw-scp.elf > /tmp/fwscp.dis
echo "lui 0x3004 (CPUX_MSGBOX):  $(grep -cE 'lui[[:space:]]+[a-z0-9]+,0x3004' /tmp/fwscp.dis)"
echo "store +124 (=0x7C ch3):    $(grep -cE 'sw[[:space:]]+[a-z0-9]+,124\(' /tmp/fwscp.dis)"
echo "0x709406c / 0x709407c:     $(grep -cE '709406c|709407c' /tmp/fwscp.dis)"
echo "irq bit 64 (1<<6):         $(grep -cE 'li[[:space:]]+a4,64' /tmp/fwscp.dis)"

echo
echo "===== 6. tail of the padded image must be zeros (padding) ====="
tail -c 64 build/fw-scp-padded.bin | od -A d -t x1 | head -4
