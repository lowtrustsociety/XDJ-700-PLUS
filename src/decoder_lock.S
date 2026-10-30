/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/* SH-4A decoder lock shared by the parser and input-file tasks. */

    .section .text.xdj_decoder_lock,"ax"
    .align 2

    .global _xdj_flac_lock_acquire
    .type _xdj_flac_lock_acquire,@function
_xdj_flac_lock_acquire:
    mov.l   r8,@-r15
    sts.l   pr,@-r15
    mov.l   .Lflac_context,r8
    add     #44,r8
.Llock_retry:
    tas.b   @r8
    bt      .Llock_acquired
    mov.l   .Ltask_delay,r0
    jsr     @r0
     mov    #1,r4
    bra     .Llock_retry
     nop
.Llock_acquired:
    lds.l   @r15+,pr
    rts
     mov.l  @r15+,r8

    .align 2
.Lflac_context:
    .long   _xdj_flac_context
.Ltask_delay:
    .long   0x08E0335C

    .size _xdj_flac_lock_acquire,.-_xdj_flac_lock_acquire

    .global _xdj_flac_lock_release
    .type _xdj_flac_lock_release,@function
_xdj_flac_lock_release:
    mov.l   .Lrelease_context,r1
    mov     #0,r0
    add     #44,r1
    mov.b   r0,@r1
    synco
    rts
     nop

    .align 2
.Lrelease_context:
    .long   _xdj_flac_context

    .size _xdj_flac_lock_release,.-_xdj_flac_lock_release
