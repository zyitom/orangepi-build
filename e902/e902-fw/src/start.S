/*
 * E902 startup code for A733 (sun60iw2).
 *
 * Derived from opene902/smart_run/tests/lib/crt0.s (Apache-2.0,
 * Copyright 2018-2021 T-Head Semiconductor Co., Ltd.), adapted for
 * this SoC: single-image layout in SRAM_A2, stack from the linker
 * script, CLIC vectored mode.
 *
 * RV32E: only x0-x15 exist. Touching x16+ traps.
 */
	.section .text.init, "ax"
	.global __start
__start:
	/* zero the register file so state is deterministic on soft reset */
	li	x1, 0
	li	x2, 0
	li	x3, 0
	li	x4, 0
	li	x5, 0
	li	x6, 0
	li	x7, 0
	li	x8, 0
	li	x9, 0
	li	x10, 0
	li	x11, 0
	li	x12, 0
	li	x13, 0
	li	x14, 0
	li	x15, 0

	/* stack: top of the reserved region (16-byte aligned by the script) */
	la	sp, __stack_top

	/* copy .data from its load address (in .text) to its run address */
	la	a0, __data_load_start
	la	a1, __data_start
	la	a2, __data_end
	beq	a1, a2, 2f
1:	lw	a3, 0(a0)
	sw	a3, 0(a1)
	addi	a0, a0, 4
	addi	a1, a1, 4
	blt	a1, a2, 1b
2:

	/* zero .bss */
	la	a0, __bss_start
	la	a1, __bss_end
	beq	a0, a1, 4f
3:	sw	zero, 0(a0)
	addi	a0, a0, 4
	blt	a0, a1, 3b
4:

	/* trap/exception base: non-vectored entry point */
	la	a0, trap_entry
	csrw	mtvec, a0

	/* CLIC vector table base (mtvt = CSR 0x307) */
	la	a0, vector_table
	csrw	0x307, a0

	/* CLIC in fully-vectored mode: nlbits = 0, all levels */
	li	a0, 0
	li	a1, 0xE0800000
	sb	a0, 0(a1)		/* CLICCFG */

	/* interrupt threshold 0: accept everything */
	sb	a0, 8(a1)		/* MINTTHRESH */

	jal	ra, soc_early_init

	/* enable machine interrupts (mstatus.MIE) */
	csrsi	mstatus, 0x8

	jal	ra, main

	/* main returned: park with interrupts still live */
halt:
	wfi
	j	halt

/*
 * Non-vectored trap entry (MTVEC, E902 mode is hard-wired to 3 = CLIC).
 * Synchronous exceptions land here, and so would any interrupt whose
 * CLICINTATTR.shv is 0 (we never configure one, but a stray one must not
 * kill the core).
 *
 * v63: this used to record mcause and park in wfi forever. That turned any
 * single bad access (e.g. a read of an unclocked peripheral) into a dead
 * SCP -- and a dead SCP wedges bl31 for good, because bl31's mailbox RPCs
 * and its reboot/poweroff sender have no timeout. Now the caller-saved
 * registers are preserved, trap_handler() (main.c) logs the trap into the
 * heartbeat block and returns the address to resume at (normally the
 * instruction after the faulting one), and we mret.
 *
 * RV32E: x0-x15 only. The C ABI (ilp32e) treats ra, t0-t2, a0-a5 as
 * caller-saved; s0/s1 are preserved by trap_handler itself.
 */
	.align 6
	.global trap_entry
trap_entry:
	addi	sp, sp, -40
	sw	ra,  0(sp)
	sw	t0,  4(sp)
	sw	t1,  8(sp)
	sw	t2, 12(sp)
	sw	a0, 16(sp)
	sw	a1, 20(sp)
	sw	a2, 24(sp)
	sw	a3, 28(sp)
	sw	a4, 32(sp)
	sw	a5, 36(sp)
	csrr	a0, mcause
	csrr	a1, mepc
	jal	ra, trap_handler
	csrw	mepc, a0
	lw	ra,  0(sp)
	lw	t0,  4(sp)
	lw	t1,  8(sp)
	lw	t2, 12(sp)
	lw	a0, 16(sp)
	lw	a1, 20(sp)
	lw	a2, 24(sp)
	lw	a3, 28(sp)
	lw	a4, 32(sp)
	lw	a5, 36(sp)
	addi	sp, sp, 40
	mret

/*
 * CLIC vector table. Entry i is the address of the handler for
 * interrupt i. Table must be 64-byte aligned (mtvt requirement).
 * A733 routes at most 70 interrupts to E902 (manual Table 12-2);
 * 96 entries covers that with room to spare.
 */
	.section .vectors, "a"
	.align 6
	.global vector_table
vector_table:
	.rept 96
	.long default_isr
	.endr

	.section .bss
	.align 2
	.global last_mcause
last_mcause:
	.long 0
	.global last_mepc
last_mepc:
	.long 0
