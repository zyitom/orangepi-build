/*
 * Link script for the A733 E902 firmware.
 *
 * The image is loaded to, and runs entirely from, SRAM_A2 at the address
 * the SoC's E902_RST_START_ADDR register points at. There is no separate
 * load/run split for .text; only .data is copied by start.S, and its load
 * copy is appended to .text so the whole thing stays one flat binary.
 *
 * SRAM_A2 in the E902 view is 0x40000000..0x40033FFF (208K, manual ch.2).
 * We deliberately start at 0x40014000 -- the reset default of
 * E902_RST_START_ADDR (manual 5.2.4.8) -- so no register write is needed
 * to set the entry point when starting from a cold reset.
 *
 * Note: the vendor scp.fex loads at this same address, so this firmware
 * and the stock SCP firmware are mutually exclusive.
 */

MEMORY
{
	SRAM (rwx) : ORIGIN = 0x40004000, LENGTH = 0x00020000	/* 128K */
}

ENTRY(__start)

/* Stack lives at the top of our region, growing down. 4K is plenty for
 * a polling loop plus one level of interrupt. */
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

	/* .data run address follows .rodata; its initial image is stored
	 * right here too, so load == run and start.S's copy loop is a nop.
	 * Keeping the loop costs nothing and makes the layout easy to change. */
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

ASSERT(__image_end <= 0x40034000,
	"firmware overruns SRAM_A2 (ends past 0x40034000)")
