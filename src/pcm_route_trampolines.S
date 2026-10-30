/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * Type-1 task-local call targets for a virtual FLAC PCM byte stream.
 *
 * Each hook reads the task-state format object (+92). While any FLAC
 * descriptor is known, active or evicted, it calls the C bridge with that
 * object as an extra argument; the bridge selects or reopens the slot for it
 * and forwards everything else to the stock routine. With no known FLAC
 * descriptor the stock routine is entered directly.
 *
 * The format object travels with the call: a shared field written here,
 * outside the decoder lock, could name another call's track by the time the
 * bridge reads it. The bridge still chooses the track by the call's file,
 * since the format object can belong to another track than the file context.
 * The object is also stored at context offset 0
 * for a memory dump.
 */

    .section .text.xdj_pcm_trampolines,"ax"
    .align 2

    /* r1 = format object; falls through only while FLAC descriptors are known. */
    .macro route_request task_register, stock_label
    mov     #92,r0
    mov.l   @(r0,\task_register),r1
    mov.l   .Lflac_context,r2
    mov.l   r1,@r2
    mov.l   @(4,r2),r0
    tst     r0,r0
    bt      \stock_label
    .endm

    /*
     * seek(file_context, offset, origin, fs_state) in r4-r7; the bridge takes
     * the format object as a fifth argument, on the stack.
     */
    .macro call_seek
    sts.l   pr,@-r15
    mov.l   r1,@-r15
    mov.l   .Lvirtual_seek,r0
    jsr     @r0
     nop
    add     #4,r15
    lds.l   @r15+,pr
    rts
     nop
    .endm

    .global _xdj_virtual_seek_r12
    .type _xdj_virtual_seek_r12,@function
_xdj_virtual_seek_r12:
    route_request r12,.Lstock_seek_r12
    call_seek
.Lstock_seek_r12:
    mov.l   .Lstock_seek,r0
    jmp     @r0
     nop
    .size _xdj_virtual_seek_r12,.-_xdj_virtual_seek_r12

    .align 2
    .global _xdj_virtual_seek_r10
    .type _xdj_virtual_seek_r10,@function
_xdj_virtual_seek_r10:
    route_request r10,.Lstock_seek_r10
    call_seek
.Lstock_seek_r10:
    mov.l   .Lstock_seek,r0
    jmp     @r0
     nop
    .size _xdj_virtual_seek_r10,.-_xdj_virtual_seek_r10

    /*
     * read(file_context, output, count, status) in r4-r7 and fs_state on the
     * stack; the bridge takes fs_state and the format object as its fifth
     * and sixth arguments.
     */
    .align 2
    .global _xdj_virtual_read_r12
    .type _xdj_virtual_read_r12,@function
_xdj_virtual_read_r12:
    route_request r12,.Lstock_read_r12
    mov.l   @r15,r3
    sts.l   pr,@-r15
    mov.l   r1,@-r15
    mov.l   r3,@-r15
    mov.l   .Lvirtual_read,r0
    jsr     @r0
     nop
    add     #8,r15
    lds.l   @r15+,pr
    rts
     nop
.Lstock_read_r12:
    mov.l   .Lstock_read,r0
    jmp     @r0
     nop
    .size _xdj_virtual_read_r12,.-_xdj_virtual_read_r12

    /*
     * end_of_stream(file_context, fs_state) in r4-r5; the bridge takes the
     * format object as a third argument, in r6.
     */
    .align 2
    .global _xdj_virtual_eof_r12
    .type _xdj_virtual_eof_r12,@function
_xdj_virtual_eof_r12:
    route_request r12,.Lstock_eof_r12
    mov     r1,r6
    mov.l   .Lvirtual_eof,r0
    jmp     @r0
     nop
.Lstock_eof_r12:
    mov.l   .Lstock_eof,r0
    jmp     @r0
     nop
    .size _xdj_virtual_eof_r12,.-_xdj_virtual_eof_r12

    .align 2
.Lflac_context:
    .long   _xdj_flac_context
.Lstock_seek:
    .long   0x08BE8E58
.Lstock_read:
    .long   0x08BE8F40
.Lvirtual_seek:
    .long   _xdj_flac_virtual_seek_format
.Lvirtual_read:
    .long   _xdj_flac_virtual_read_format
.Lstock_eof:
    .long   0x08BE8DC2
.Lvirtual_eof:
    .long   _xdj_flac_virtual_eof_format
