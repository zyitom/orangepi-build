/*
 * min3.S - staged bring-up probe.
 *
 * WHY THIS EXISTS
 * ---------------
 * min2.c and the full firmware share start.S. If start.S traps in its first
 * few instructions (for example on `csrw 0x307` / mtvt, or mstatus), then
 * BOTH images die before writing the heartbeat, and "the core never started"
 * cannot be distinguished from "the core started and trapped immediately".
 *
 * This probe writes a distinct marker to SRAM after each successive
 * initialisation step, starting with the very first instruction. Reading the
 * scratch block then shows exactly how far the core got:
 *
 *   +0x00 0xE902C0DE  <- the core executed OUR first instruction
 *   +0x04 0x11111111  <- register file cleared
 *   +0x08 0x22222222  <- stack pointer loaded
 *   +0x0C 0x33333333  <- mtvec written
 *   +0x10 0x44444444  <- mtvt (CSR 0x307) written
 *   +0x14 0x55555555  <- CLIC CFG/MINTTHRESH written
 *   +0x18 0x66666666  <- reached the idle loop
 *
 * No peripheral is touched and no interrupt is enabled, so nothing here can
 * fault for any reason other than an unsupported instruction/CSR.
 *
 * RV32E: only x0..x15 exist. Usable GPRs are ra(x1) sp(x2) gp(x3) tp(x4)
 * t0(x5) t1(x6) t2(x7) s0(x8) s1(x9) a0..a5(x10..x15). Anything above x15
 * is rejected by the assembler -- that is the point of the E profile.
 */
	.section .text.init, "ax"
	.global __start
__start:
	li	t0, 0x4001E000		/* t0 = x5 */
	li	t1, 0xE902C0DE		/* t1 = x6 */
	sw	t1, 0(t0)		/* STAGE 1: first instruction */

	li	ra, 0
	li	gp, 0
	li	tp, 0
	li	t1, 0
	li	t2, 0
	li	s0, 0
	li	s1, 0
	li	a0, 0
	li	a1, 0
	li	a2, 0
	li	a3, 0
	li	a4, 0
	li	a5, 0
	li	t1, 0x11111111
	sw	t1, 4(t0)		/* STAGE 2: register file cleared */

	la	sp, __stack_top
	li	t1, 0x22222222
	sw	t1, 8(t0)		/* STAGE 3: stack set */

	la	a0, park
	csrw	mtvec, a0
	li	t1, 0x33333333
	sw	t1, 12(t0)		/* STAGE 4: mtvec ok */

	li	a0, 0xE0800000
	csrw	0x307, a0		/* mtvt - a core without CLIC rejects this */
	li	t1, 0x44444444
	sw	t1, 16(t0)		/* STAGE 5: mtvt ok */

	sb	zero, 0(a0)		/* CLICCFG    = 0 */
	sb	zero, 8(a0)		/* MINTTHRESH = 0 */
	li	t1, 0x55555555
	sw	t1, 20(t0)		/* STAGE 6: CLIC configured */

	li	t1, 0x66666666
	sw	t1, 24(t0)		/* STAGE 7: all init done */

park:
	wfi
	j	park

	.section .bss
	.align 2
	.global last_mcause
last_mcause:
	.long 0
	.global last_mepc
last_mepc:
	.long 0
