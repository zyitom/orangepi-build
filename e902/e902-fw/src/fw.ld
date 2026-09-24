/*
 * Link script for the A733 E902 firmware.
 *
 * The image is loaded to, and runs entirely from, SRAM_A2. There is no
 * separate load/run split for .text; only .data is copied by start.S, and its
 * load copy is appended to .text so the whole thing stays one flat binary.
 *
 * WHY 0x40020000 AND NOT 0x40014000
 * SRAM_A2 in the E902 view is 0x40000000..0x40033FFF (208K, manual ch.2).
 * bl31 loads the vendor scp.fex at 0x40004000 with a length of 0x19DB8, so
 * the vendor image occupies 0x40004000..0x4001DDB8 and its stack sits at
 * 0x4002F000 growing down. The old 0x40014000 base is *inside* that vendor
 * image (offset 0x10000) -- loading there destroys the middle of the running
 * SCP, so `e902-restore.sh` had to rewrite all 105912 bytes to get it back.
 *
 * Loading at 0x40020000 instead lands in the gap between the vendor image
 * end and the vendor stack: the vendor image stays byte-identical, so the
 * core can be handed back by a plain reset with no SRAM rewrite at all, and
 * a failed experiment cannot corrupt the firmware that manages DRAM/PMIC.
 *
 * Layout used (all E902 view):
 *   0x40004000..0x4001DDB8   vendor scp.fex        (untouched)
 *   0x4001E000..0x4001E100   heartbeat block       (heartbeat.c)
 *   0x40020000..0x4002F000   this firmware + stack
 *   0x4002F000               vendor stack top      (untouched)
 */

MEMORY
{
	SRAM (rwx) : ORIGIN = 0x40020000, LENGTH = 0x0000F000	/* 60K */
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

ASSERT(__image_end <= 0x4002F000,
	"firmware overruns SRAM_A2 above 0x4002F000 (would clobber vendor SCP stack)")
