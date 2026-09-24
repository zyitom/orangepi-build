#!/bin/bash
cd /home/helios/Desktop/orangepi-build/e902/e902-fw || exit 1
TC=/home/helios/Desktop/orangepi-build/e902/toolchains/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0/bin/riscv64-unknown-elf-

${TC}objdump -d build/fw-scp.elf > /tmp/fwscp.dis

echo "===== call order inside main() (must start with the handshake) ====="
awk '/<main>:/,/<handle_arm_message>:/' /tmp/fwscp.dis | grep -E 'jal|call' | head -12

echo
echo "===== symbols of interest ====="
${TC}nm build/fw-scp.elf | grep -E ' (__start|main|msgbox_send_startup_feedback|pinmux_s_uart0|uart_init|spi_init)$'

echo
echo "===== sections (entry at 0x40004000) ====="
${TC}objdump -h build/fw-scp.elf | grep -E '^ *[0-9]+ (\.text|\.vectors|\.rodata|\.data|\.bss|\.stack)'

echo
echo "===== artefact ====="
ls -la build/fw.bin build/fw-scp.bin build/fw-scp-padded.bin
sha256sum build/fw.bin build/fw-scp-padded.bin
