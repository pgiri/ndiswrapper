/*
 *  Copyright (C) 2011 Pavel Roskin
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 */

#include <linux/linkage.h>
#include <linux/version.h>

	.text

#define WORD_BYTES 8
#define LINUX_REG_ARGS 6
#define WINDOWS_REG_ARGS 4

/* %rbp is saved to create a stack frame, which can help with debugging */
#define SAVED_REGS 1

/*
 * When calling a Windows function, stack space is allocated for at least 4
 * arguments even if the number of arguments is less than 4.  The value of
 * true is -1 in assembler, so we multiply it by another true value.
 */
#define stack_args(argc)						\
	(WINDOWS_REG_ARGS +						\
	 (0 < 1) * (argc > WINDOWS_REG_ARGS) * (argc - WINDOWS_REG_ARGS))

/* Full required change of stack pointer, in words */
#define stack_words_raw(argc) (stack_args(argc) + SAVED_REGS + 1)

/* Full actual change of stack pointer, in words (must be even) */
#define stack_words_aligned(argc) ((stack_words_raw(argc) + 1) & ~1)

/* Space allocated for Linux arguments on stack */
#define stack_space(argc) \
	((stack_words_aligned(argc) - SAVED_REGS - 1) * WORD_BYTES)

/*
 * lin2win_win_arg(N) gives the address of the Nth Windows argument on our
 * stack frame.  %rsp points to the first argument.  The Nth argument is
 * therefore at ((N - 1) * 8)(%rsp).
 *
 * Don't call with N less than 5!
 */
#define lin2win_win_arg(n) ((n - 1) * WORD_BYTES)(%rsp)

/*
 * lin2win_lin_arg(N, ARGC) gives the address of the Nth Linux argument after
 * the stack has been prepared for a Windows function call with ARGC arguments.
 *
 * When called from Linux, the Nth argument is at ((N - 6) * 8)(%rsp).  We add
 * the allocated stack space and saved registers to compensate for %rsp change.
 *
 * Don't call with N less than 7!
 */
#define lin2win_lin_arg(n, argc)					\
	(stack_space(argc) +						\
	 (SAVED_REGS + n - LINUX_REG_ARGS) * WORD_BYTES)(%rsp)

/*
 * lin2win(func, winarg1, winarg2, ...)
 * Call Windows FUNC function with ARGC arguments WINARG1, WINARG2, ...
 * We get (ARGC + 1) arguments.
 */
.macro lin2win name, argc
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,5,0)
	SYM_FUNC_START(\name)
#else
	.type \name, @function
	ENTRY(\name)
#endif

	/* Create a call frame - it's optional, but good for debugging */
	.cfi_startproc
	push %rbp
	.cfi_def_cfa %rsp, 2 * WORD_BYTES
	.cfi_offset %rbp, -2 * WORD_BYTES
	mov %rsp, %rbp
	.cfi_def_cfa %rbp, 2 * WORD_BYTES

	/* Allocate space for Windows arguments */
	sub $stack_space(\argc), %rsp

	/* arg7 to winarg6 */
	.if (\argc >= 6)
		mov lin2win_lin_arg(7, \argc), %r11
		mov %r11, lin2win_win_arg(6)
	.endif

	/* arg6 to winarg5 */
	.if (\argc >= 5)
		mov %r9, lin2win_win_arg(5)
	.endif

	/* arg5 to winarg4 */
	.if (\argc >= 4)
		mov %r8, %r9
	.endif

	/* arg4 to winarg3 */
	.if (\argc >= 3)
		mov %rcx, %r8
	.endif

	/* arg3 to winarg2 - nothing needed, both are in %rdx */

	/* arg2 to winarg1 */
	.if (\argc >= 1)
		mov %rsi, %rcx
	.endif

	/* Call function (arg1) */
	call *%rdi

	/* Reclaim space for Windows arguments */
	add $stack_space(\argc), %rsp

	/* Return to the caller */
	leave
	.cfi_def_cfa %rsp, WORD_BYTES
	.cfi_restore %rbp
	ret
	.cfi_endproc
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,5,0)
	SYM_FUNC_END(\name)
#else
	.size	\name, (. - \name)
#endif
.endm

/* Define lin2winN functions */
lin2win lin2win0, 0
lin2win lin2win1, 1
lin2win lin2win2, 2
lin2win lin2win3, 3
lin2win lin2win4, 4
lin2win lin2win5, 5
lin2win lin2win6, 6
