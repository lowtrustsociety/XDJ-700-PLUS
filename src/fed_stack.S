/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * A stack of our own for the fed reads of FLAC decoded on the audio
 * processor (the release, --release; src/flac_adapter.c).
 *
 * The bridge's reads run on the player's input-file task
 * (tsk_DJcontInBufFile), whose stack is 2 KB. A fed read can go deep (the
 * bounded seek that finds the FLAC frame of a jump, the filesystem below the
 * file reads, the port routines, and whatever an interrupt or a task switch
 * leaves on the stack meanwhile), and on the player that task's stack check
 * caught it overflowing. So the fed read runs here,
 * on 8 KB of our own.
 *
 * The same holds for every read, seek and end test of ours in that task: the
 * player's file seek below our read overflowed it after a far hot cue jump on
 * a hi-res WAV. With XDJ_OWN_STACK (v0.9 on) those
 * entries run here too, when the call comes from the file task's stack; the
 * fed read then finds itself here already and does not switch again. Measured
 * on the player with that switch: 1,876 of the 8,192 bytes at the deepest.
 *
 * xdj_call_on_fed_stack(fn, a, b, c) calls fn(a, b, c) with the stack pointer
 * moved to the top of that area, and moves it back. Only one call at a time
 * is on it: reads, seeks and end tests come from the file task alone (the
 * recording shows no other task calling them). The stack
 * pointer is changed by single instructions, so an interrupt always finds a
 * whole stack under it.
 */

    .section .text.xdj_fed_stack,"ax"
    .align 2

    .global _xdj_call_on_fed_stack
    .type _xdj_call_on_fed_stack,@function
_xdj_call_on_fed_stack:
    mov.l   r14,@-r15
    sts.l   pr,@-r15
    mov     r15,r14
    mov.l   .Lfed_stack_top,r15
    mov     r4,r0
    mov     r5,r4
    mov     r6,r5
    jsr     @r0
     mov    r7,r6
    mov     r14,r15
    lds.l   @r15+,pr
    rts
     mov.l  @r15+,r14

    .align 2
.Lfed_stack_top:
    .long   _xdj_fed_stack + 8192

    .size _xdj_call_on_fed_stack,.-_xdj_call_on_fed_stack

    .section .bss.xdj_fed_stack,"aw",@nobits
    .align 3
    .global _xdj_fed_stack
_xdj_fed_stack:
    .space  8192
