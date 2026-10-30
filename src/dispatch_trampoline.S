/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * SH-4A trampoline for the XDJ-700 playback dispatcher.
 *
 * A five-instruction entry patch jumps here. File types 5 (FLAC), 11 (WAV)
 * and 12 (AIFF) call the adapter, then rejoin the original result handling
 * and epilogue. The adapter arguments are the file-I/O context, output
 * descriptor, and parser workspace respectively. For WAV and AIFF the
 * adapter usually answers 1: the file is the stock reader's (only 88.2 and
 * 96 kHz files and extensible WAV headers are served by the bridge). Then,
 * and for every other type, the dispatcher resumes after its displaced
 * prologue as if it had never been entered here.
 */

    .section .text.xdj_dispatch_trampoline,"ax"
    .align 2
    .global _xdj_dispatch_trampoline
    .type _xdj_dispatch_trampoline,@function

_xdj_dispatch_trampoline:
    mov.l   @(4,r5),r6
    mov.l   @(8,r6),r0
    cmp/eq  #5,r0
    bt      .Lflac
    cmp/eq  #11,r0
    bt      .Lflac
    cmp/eq  #12,r0
    bt      .Lflac

    mov.l   r4,@-r15
    mov.l   r5,@-r15
    mov.l   r6,@-r15
    mov.l   r7,@-r15
    sts.l   pr,@-r15
    mov.l   .Ldeactivate_address,r0
    jsr     @r0
     mov    r6,r4
    lds.l   @r15+,pr
    mov.l   @r15+,r7
    mov.l   @r15+,r6
    mov.l   @r15+,r5
    mov.l   @r15+,r4

    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   r14,@-r15
    sts.l   pr,@-r15
    add     #-4,r15
    mov.l   .Lresume_address,r0
    jmp     @r0
     nop

.Lflac:
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   r14,@-r15
    sts.l   pr,@-r15
    add     #-4,r15

    mov     r4,r14
    mov     r4,r12
    mov     #-16,r2
    extu.b  r2,r2
    add     r2,r4
    add     #84,r12
    mov     r5,r13
    mov     r6,r5
    mov     r4,r6
    mov     r12,r4
    mov.l   .Ladapter_address,r0
    jsr     @r0
     nop
    cmp/eq  #1,r0
    bt      .Ldeclined
    mov.l   .Lpostprocess_address,r1
    jmp     @r1
     nop

    /*
     * The stock reader's file. The prologue is already on the stack, so
     * restore the dispatcher's arguments (r13 kept the second one; the
     * dispatcher loads r13 itself later) and resume it.
     */
.Ldeclined:
    mov     r14,r4
    mov     r13,r5
    mov.l   @(4,r5),r6
    mov.l   .Lresume_address,r0
    jmp     @r0
     nop

    .align 2
.Lresume_address:
    .long   0x08C01E84
.Lpostprocess_address:
    .long   0x08C01ECC
.Ladapter_address:
    .long   _xdj_flac_adapter
.Ldeactivate_address:
    .long   _xdj_flac_deactivate

    .size _xdj_dispatch_trampoline,.-_xdj_dispatch_trampoline
