/*
 * Implement __x86_indirect_thunk_* symbols for use with compatbile compilers
 * and the -mindirect-branch=thunk-extern -mindirect-branch-register options.
 *
 * Copyright (c) 2017-2018 Citrix Systems Ltd.
 *
 * This source code is licensed under the GNU General Public License,
 * Version 2.  See the file COPYING for more details.
 */
        .file __FILE__

#include <asm/asm_defns.h>

/* Alignment is dealt with explicitly here; override the respective macro. */
#undef SYM_ALIGN
#define SYM_ALIGN(align...)

#ifdef CONFIG_INDIRECT_THUNK

.macro IND_THUNK_RETPOLINE reg:req
        call 1f
        int3
1:
        mov %\reg, (%rsp)
        ret
.endm

.macro IND_THUNK_LFENCE reg:req
        lfence
        jmp *%\reg
.endm

.macro IND_THUNK_JMP reg:req
        jmp *%\reg
.endm

/*
 * Build the __x86_indirect_thunk_* symbols.  Execution lands on an
 * alternative patch point which implements one of the above THUNK_*'s
 */
.macro GEN_INDIRECT_THUNK reg:req
        .section .text.__x86_indirect_thunk_\reg, "ax", @progbits

        /*
         * The Indirect Target Selection speculative vulnerability means that
         * indirect branches (including RETs) are unsafe when in the first
         * half of a cacheline.  Arrange for them to be in the second half.
         *
         * Align to 64, then skip 32.
         */
        .balign 64
        .fill 32, 1, 0xcc

FUNC(__x86_indirect_thunk_\reg)
        ALTERNATIVE_2 __stringify(IND_THUNK_RETPOLINE \reg),              \
        __stringify(IND_THUNK_LFENCE \reg), X86_FEATURE_IND_THUNK_LFENCE, \
        __stringify(IND_THUNK_JMP \reg),    X86_FEATURE_IND_THUNK_JMP

        int3 /* Halt straight-line speculation */
END(__x86_indirect_thunk_\reg)
.endm

/* Instantiate GEN_INDIRECT_THUNK for each register except %rsp. */
.irp reg, ax, cx, dx, bx, bp, si, di, 8, 9, 10, 11, 12, 13, 14, 15
        GEN_INDIRECT_THUNK reg=r\reg
.endr

#endif /* CONFIG_INDIRECT_THUNK */

#ifdef CONFIG_RETURN_THUNK
        .section .text.entry.__x86_return_thunk, "ax", @progbits

        /*
         * The Indirect Target Selection speculative vulnerability means that
         * indirect branches (including RETs) are unsafe when in the first
         * half of a cacheline.  Arrange for them to be in the second half.
         *
         * Align to 64, then skip 32.
         */
        .balign 64
        .fill 32, 1, 0xcc

FUNC(__x86_return_thunk)
        ret
        int3 /* Halt straight-line speculation */
END(__x86_return_thunk)

#endif /* CONFIG_RETURN_THUNK */
