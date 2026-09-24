/*
 * Link script for the "scp replacement" flavour of the firmware.
 *
 * bl31 loads the `scp` item of the boot package to 0x40004000 and then sets
 * E902_RST_START_ADDR to that same address before releasing reset (measured:
 * the running vendor system reports RST_START = 0x40004000). So an image that
 * is meant to be started BY bl31 must be linked at 0x40004000, not at the
 * 0x40020000 used by the "load it from Linux by hand" flavour.
 *
 * Region: 0x40004000 .. 0x4002F000. SRAM_A2 extends to 0x40033FFF, but the
 * vendor firmware's stack top is 0x4002F000; staying below it keeps the same
 * margin the vendor uses. The image MUST also fit inside the boot-package TOC
 * size for the scp item, which is exactly 0x19DB8 = 105912 bytes.
 */

MEMORY
{
	SRAM (rwx) : ORIGIN = 0x40004000, LENGTH = 0x0002B000	/* ends at 0x4002F000 */
}

ENTRY(__start)
STACK_SIZE = 0x1000;

SECTIONS
{
	.text : ALIGN(4)
	{
		KEEP(*(.text.init))
		*(.text .text.*)
	} > SRAM

	.vectors : ALIGN(64)
	{
		KEEP(*(.vectors))
	} > SRAM

	.rodata : ALIGN(4)
	{
		*(.rodata .rodata.*)
		*(.srodata .srodata.*)
		. = ALIGN(4);
	} > SRAM

	.data : ALIGN(4)
	{
		__data_load_start = LOADADDR(.data);
		__data_start = .;
		*(.data .data.*)
		*(.sdata .sdata.*)
		. = ALIGN(4);
		__data_end = .;
	} > SRAM

	.bss (NOLOAD) : ALIGN(4)
	{
		__bss_start = .;
		*(.bss .bss.*)
		*(.sbss .sbss.*)
		*(COMMON)
		. = ALIGN(4);
		__bss_end = .;
	} > SRAM

	.stack (NOLOAD) : ALIGN(16)
	{
		. = . + STACK_SIZE;
		__stack_top = .;
	} > SRAM

	__image_end = .;

	/DISCARD/ : { *(.comment) *(.eh_frame) *(.riscv.attributes) }
}

ASSERT(__image_end <= 0x4002F000,
	"firmware overruns its region (ends past 0x4002F000)")
ASSERT((__image_end - 0x40004000) <= 0x19DB8,
	"firmware is larger than the boot-package scp slot (0x19DB8 = 105912 bytes)")
