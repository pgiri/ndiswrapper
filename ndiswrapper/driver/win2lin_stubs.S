/*
 *  Copyright (C) 2005 Karl Vogel, Giridhar Pemmasani
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

#ifdef CONFIG_X86_64

/*
# Windows <---> Linux register usage conversion when calling functions
# V = Volatile
# NV = Non Volatile (needs to be saved)
#
#         Win                     Lin
# ---------------------------------------
# Rax    Return           V       Return          V
# Rbx                     NV                      NV
# Rcx     Arg1            V       Arg4            V
# Rdx     Arg2            V       Arg3            V
# Rsi                     NV      Arg2            V
# Rdi                     NV      Arg1            V
# Rsp                     NV                      NV
# Rbp                     NV                      NV
# R8      Arg3            V       Arg5            V
# R9      Arg4            V       Arg6            V
# R10                     V                       V
# R11                     V                       V
# R12                     NV                      NV
# R13                     NV                      NV
# R14                     NV                      NV
# R15                     NV                      NV
#
# In addition, Linux uses %rax to indicate number of SSE registers used
# when variadic functions are called. Since there is no way to obtain this
# from Windows, for now, we just assume this is 0 (hence %rax is cleared).
#
# Windows pushes arguments 5 and higher onto stack in case of integer
# variables and 4 and higher in case of floating point variables (passed
# in SSE registers).

In a windows function, the stackframe/registers look like this:

# 0x0048 ....
# 0x0040 arg8
# 0x0038 arg7
# 0x0030 arg6
# 0x0028 arg5
# 0x0020 shadow/spill space for arg4
# 0x0018 shadow/spill space for arg3
# 0x0010 shadow/spill space for arg2
# 0x0008 shadow/spill space for arg1
# 0x0000 ret

# register spill space is same irrespective of number of arguments - even
# if Windows function takes less than 4 arguments, 32 bytes above return
# address is reserved for the function

In Linux it should look like:

# 0x0018 ....
# 0x0010 arg8
# 0x0008 arg7
# 0x0000 ret

*/

	.text

#define LINUX_REG_ARGS 6
#define LOOP_THRESHOLD 9
#define WORD_BYTES 8

/*
 * %rsi and %rdi must be saved because they are not saved by Linux calls, but
 * Windows callers expect them to be saved.  %rbp is saved to create a stack
 * frame, which can help with debugging.  We need to reserve space for an odd
 * number of registers anyway to keep 16-bit alignment of the stack (one more
 * position is used by the return address).
 */
#define SAVED_REGS 3

/*
 * When calling the Linux function, several registers are saved on the stack.
 * When passing more than 6 arguments, arguments starting with argument 7 are
 * pushed to the stack as well.
 *
 * We also need to allocate an additional word on the stack to keep it aligned
 * to the 16-bit boundary if the number of saved arguments plus one (for the
 * return address) is odd.
 */

/*
 * Number of arguments we pass on stack to the Linux function.
 * The value of true is -1 in assembler, so we multiply it by another true
 * value.
 */
#define stack_args(argc)					\
	((0 < 1) * (argc > LINUX_REG_ARGS) * (argc - LINUX_REG_ARGS))

/* Full required change of stack pointer, in words */
#define stack_words_raw(argc) (stack_args(argc) + SAVED_REGS + 1)

/* Full actual change of stack pointer, in words (must be even) */
#define stack_words_aligned(argc) ((stack_words_raw(argc) + 1) & ~1)

/* Space allocated for Linux arguments on stack */
#define stack_space(argc) \
	((stack_words_aligned(argc) - SAVED_REGS - 1) * WORD_BYTES)

/*
 * win2lin_win_arg(N, ARGC) gives the address of the Windows argument N out of
 * total ARGC after the stack has been prepared for the Linux function call.
 *
 * When called from Windows, the Nth argument is at (N * 8)(%rsp).  We add the
 * stack space allocated by the Linux function to compensate for %rsp change.
 *
 * Don't call with N less than 5!
 */
#define win2lin_win_arg(n, argc) \
	((n + SAVED_REGS) * WORD_BYTES + stack_space(argc))(%rsp)

/*
 * win2lin_lin_arg(N) gives the address of the Nth Linux argument on the extra
 * Linux stack frame.  When more than 6 arguments are used, %rsp points to the
 * 7th argument.  The Nth argument is therefore at ((N - 7) * 8)(%rsp).
 *
 * Don't call with N less than 7!
 */
#define win2lin_lin_arg(n) ((n - 1 - LINUX_REG_ARGS) * WORD_BYTES)(%rsp)

/* Declare function LONGNAME, call function SHORTNAME with ARGC arguments */
.macro win2linm longname, shortname, argc
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,5,0)
	SYM_FUNC_START(\longname)
#else
	.type \longname, @function
	ENTRY(\longname)
#endif

	/* Create a call frame - it's optional, but good for debugging */
	.cfi_startproc
	push %rbp
	.cfi_def_cfa %rsp, 2 * WORD_BYTES
	.cfi_offset %rbp, -2 * WORD_BYTES
	mov %rsp, %rbp
	.cfi_def_cfa %rbp, 2 * WORD_BYTES

	/*
	 * Registers %rdi and %rsi are volatile on Linux, but not on Windows,
	 * so save them on the stack.
	 */
	push %rsi
	push %rdi

	/* Allocate extra stack space for arguments 7 and up */
	sub $stack_space(\argc), %rsp

	/*
	 * Copy arguments 7 and up.  We do it early, before %rdi and %rsi
	 * are used for arguments 1 and 2, so we don't have to save them.
	 * We still need to save %rcx if using a string copy.
	 */
	.if (\argc < LOOP_THRESHOLD)
		/* If a few arguments, copy them individually through %r11 */
		.if (\argc >= 7)
			mov win2lin_win_arg(7, \argc), %r11
			mov %r11, win2lin_lin_arg(7)
		.endif
		.if (\argc >= 8)
			mov win2lin_win_arg(8, \argc), %r11
			mov %r11, win2lin_lin_arg(8)
		.endif
	.else
		/* If there are many arguments, copy them in a loop */
		/* Save arg1 to %r11 */
		mov %rcx, %r11
		/* Source and destination */
		lea win2lin_win_arg(LINUX_REG_ARGS + 1, \argc), %rsi
		lea win2lin_lin_arg(LINUX_REG_ARGS + 1), %rdi
		/* Number of arguments to copy (%ecx zero-extends to %rcx) */
		mov $(\argc - LINUX_REG_ARGS), %ecx
		rep movsq
		/* Restore arg1 directly to %rdi */
		mov %r11, %rdi
	.endif

	/*
	 * Argument 1 - %rcx on Windows, %rdi on Linux
	 * Micro-optimization - if we used loop, arg1 is already in %rdi
	 */
	.if (\argc >= 1) && (\argc < LOOP_THRESHOLD)
		mov %rcx, %rdi
	.endif

	/* Argument 2 - %rdx on Windows, %rsi on Linux */
	.if (\argc >= 2)
		mov %rdx, %rsi
	.endif

	/* Argument 3 - %r8 on Windows, %rdx on Linux */
	.if (\argc >= 3)
		mov %r8, %rdx
	.endif

	/* Argument 4 - %r9 on Windows, %rcx on Linux */
	.if (\argc >= 4)
		mov %r9, %rcx
	.endif

	/* Argument 5 - first argument on stack on Windows, %r8 Linux */
	.if (\argc >= 5)
		mov win2lin_win_arg(5, \argc), %r8
	.endif

	/* Argument 6 - second argument on stack on Windows, %r9 Linux */
	.if (\argc >= 6)
		mov win2lin_win_arg(6, \argc), %r9
	.endif

	/* %rax on Linux is the number of arguments in SSE registers (zero) */
	xor %rax, %rax

	/* Call the function */
	call \shortname

	/* Free stack space for arguments 7 and up */
	add $stack_space(\argc), %rsp

	/* Restore saved registers */
	pop %rdi
	pop %rsi

	/* Return to Windows code */
	leave
	.cfi_def_cfa %rsp, WORD_BYTES
	.cfi_restore %rbp
	ret
	.cfi_endproc
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,5,0)
	SYM_FUNC_END(\longname)
#else
	.size \longname, (. - \longname)
#endif
.endm

#define win2lin(name, argc) win2linm win2lin_ ## name ## _ ## argc, name, argc

#include "win2lin_stubs.h"

#endif	/* CONFIG_X86_64 */
