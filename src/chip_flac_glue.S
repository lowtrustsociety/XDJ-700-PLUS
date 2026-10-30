/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * Entry code of FLAC decoded on the audio processor (src/chip_flac.c;
 * the release, --release). With XDJ_EARLY_CACHE, given to the assembler, the
 * cache is switched on here, before our stack is used (see below).
 *
 * xdj_chip_entry is the first code of our block and stands in the stock
 * table of PCM converters for the 16-bit stereo one (src/chip_flac.h).
 * It is called like a converter: a4 the unit's bytes, b4 where the left
 * channel's samples go, a6 the number of samples, b3 the way back.
 *
 * A unit whose first frame does not carry the mark bits is none of ours: it
 * goes straight to the stock converter, registers untouched. Otherwise the
 * arguments and the stack pointer are kept, the stack is changed for our own
 * (in one instruction) and xdj_chip_unit is called, which switches the cache
 * on for our part of the memory. Back here, on the stock stack again and
 * touching nothing of the cached part, the cache is written back and
 * switched off, the way the player has run it since the first tests there.
 * 
 * With XDJ_SHORT_HOLD, given to the assembler, the
 * cache is written back a second time before interrupts are held, so that
 * what is left to do while they are held is only what changed in between:
 * held too long, the output interrupt misses a block of 16 frames.
 * Then, if the unit was ours, back to the caller; if not, on to the stock
 * converter with the arguments as they came.
 *
 * The stock converter and our block are too far apart for a direct branch,
 * so all branches between them go through a register. A branch takes effect
 * after five more instructions and a load delivers after four: hence the
 * nops. The cycle counter runs from its first write on.
 */

        .section .entry, "ax"
        .global xdj_chip_entry
        .global xdj_chip_save

xdj_chip_entry:
        ldnw    .D1T1   *a4, a0
        mvkl    .S2     xdj_chip_stock, b0
        mvkh    .S2     xdj_chip_stock, b0
        mvkl    .S1     xdj_chip_save, a3
        mvkh    .S1     xdj_chip_save, a3
        shru    .S1     a0, 30, a0
        cmpeq   .L1     2, a0, a1
 [!a1]  b       .S2     b0
        nop     5

        stw     .D1T1   a4, *a3
        stw     .D1T2   b4, *+a3(4)
        stw     .D1T1   a6, *+a3(8)
        stw     .D1T2   b3, *+a3(12)
        stw     .D1T2   b15, *+a3(16)
        mvc     .S2     b0, tscl
        mvkl    .S2     xdj_chip_stack, b1
        mvkh    .S2     xdj_chip_stack, b1
        mvkl    .S2     xdj_chip_unit, b0
        mvkh    .S2     xdj_chip_unit, b0
        mvkl    .S2     xdj_chip_back, b3
        mvkh    .S2     xdj_chip_back, b3
.ifdef XDJ_EARLY_CACHE
/*
 * With XDJ_EARLY_CACHE: our part of the memory is
 * made cacheable here, before our stack is used, and read back so that the
 * write has taken effect. Without it xdj_chip_unit does it as its first act,
 * after its prologue has already saved registers on our stack while that was
 * uncached: an output interrupt in that window ran all of its calls on an
 * uncached stack and could outlast its block.
 */
        mvkl    .S1     0x01848304, a5
        mvkh    .S1     0x01848304, a5
        ldw     .D1T1   *a5, a7
        nop     4
        set     .S1     a7, 0, 0, a7
        stw     .D1T1   a7, *a5
        ldw     .D1T1   *a5, a7
        nop     4
.endif
        mv      .L2     b1, b15
        b       .S2     b0
        nop     5

/* Back from xdj_chip_unit: a4 says whether the unit was ours. */
xdj_chip_back:
        mvkl    .S1     xdj_chip_save, a3
        mvkh    .S1     xdj_chip_save, a3
        ldw     .D1T2   *+a3(16), b15
        stw     .D1T1   a4, *+a3(20)
        nop     3
        mvkl    .S1     0x01845040, a5
        mvkh    .S1     0x01845040, a5
        mvk     .S1     1, a7
        mvkl    .S2     2000000, b0
        mvkh    .S2     2000000, b0
        stw     .D1T1   a7, *a5
xdj_chip_wait_1:
        ldw     .D1T1   *a5, a0
        sub     .L2     b0, 1, b0
        nop     3
        and     .L1     1, a0, a0
 [!b0]  mvk     .S1     0, a0
 [a0]   b       .S1     xdj_chip_wait_1
        nop     5
        stw     .D1T2   b0, *+a3(24)
.ifdef XDJ_SHORT_HOLD
        mvkl    .S2     2000000, b0
        mvkh    .S2     2000000, b0
        stw     .D1T1   a7, *a5
xdj_chip_again:
        ldw     .D1T1   *a5, a0
        sub     .L2     b0, 1, b0
        nop     3
        and     .L1     1, a0, a0
 [!b0]  mvk     .S1     0, a0
 [a0]   b       .S1     xdj_chip_again
        nop     5
.endif
        mvkl    .S2     200000, b0
        mvkh    .S2     200000, b0
        dint
        stw     .D1T1   a7, *+a5(4)
xdj_chip_wait_2:
        ldw     .D1T1   *+a5(4), a0
        sub     .L2     b0, 1, b0
        nop     3
        and     .L1     1, a0, a0
 [!b0]  mvk     .S1     0, a0
 [a0]   b       .S1     xdj_chip_wait_2
        nop     5
        stw     .D1T2   b0, *+a3(28)
        mvkl    .S1     0x01848304, a5
        mvkh    .S1     0x01848304, a5
        ldw     .D1T1   *a5, a7
        nop     4
        clr     .S1     a7, 0, 0, a7
        stw     .D1T1   a7, *a5
        rint

        ldw     .D1T1   *+a3(20), a1
        ldw     .D1T2   *+a3(12), b3
        mvkl    .S2     xdj_chip_stock, b0
        mvkh    .S2     xdj_chip_stock, b0
        nop     2
 [a1]   b       .S2     b3
        nop     5

/* Not ours after all: the stock converter, with the arguments as they came. */
        ldw     .D1T1   *a3, a4
        ldw     .D1T2   *+a3(4), b4
        ldw     .D1T1   *+a3(8), a6
        b       .S2     b0
        nop     5

/*
 * The kept arguments, return address and stack pointer, the answer of
 * xdj_chip_unit, and what was left of the counts of the cache's two waits.
 */
        .section .entry_data, "aw"
        .align  4
xdj_chip_save:
        .word   0, 0, 0, 0, 0, 0, 0, 0

.ifdef XDJ_TICK_STACK
/*
 * The output interrupt kept off our stack (the
 * switch XDJ_CHIP_TICK_STACK in chip_flac.h). Its vector is pointed here at our
 * first unit; it comes as the stock routine would, interrupts held, every
 * register as the interrupted code left it. On any stack but ours
 * (0xC1Fxxxxx) it goes on to the stock routine at once. On ours it runs the
 * stock routine on the stock stack, from where that stood when our unit was
 * entered (xdj_chip_save+16): where it would have run had the stock
 * converter been called in our place. Its way back is pointed at
 * xdj_chip_tick_back, which returns to our stack and to where the interrupt
 * came. A load delivers after four instructions, so b15 is changed by a
 * load while the loads after it still use the old one.
 */
        .text
        .global xdj_chip_tick
        .global xdj_chip_tick_moved
xdj_chip_tick:
        stw     .D2T2   b0, *b15--(8)
        stw     .D2T2   b1, *+b15(4)
        shru    .S2     b15, 20, b0
        mvk     .S2     0xC1F, b1
        cmpeq   .L2     b0, b1, b1
        mvkl    .S2     xdj_chip_tick_ours, b0
        mvkh    .S2     xdj_chip_tick_ours, b0
 [b1]   b       .S2     b0
        nop     5

/* Not on our stack: the stock routine, everything as it came. */
xdj_chip_tick_plain:
        ldw     .D2T2   *+b15(4), b1
        mvkl    .S2     0x11802360, b0
        mvkh    .S2     0x11802360, b0
        b       .S2     b0
        ldw     .D2T2   *++b15(8), b0
        nop     4

xdj_chip_tick_ours:
        mvkl    .S2     xdj_chip_tick_slots, b0
        mvkh    .S2     xdj_chip_tick_slots, b0
        add     .L2     8, b15, b1
        stw     .D2T2   b1, *b0
        mvc     .S2     irp, b1
        stw     .D2T2   b1, *+b0(4)
        ldw     .D2T2   *+b0(8), b1
        nop     4
        add     .L2     1, b1, b1
        stw     .D2T2   b1, *+b0(8)
        mvkl    .S2     xdj_chip_tick_back, b1
        mvkh    .S2     xdj_chip_tick_back, b1
        mvc     .S2     b1, irp
        mvkl    .S2     xdj_chip_save, b0
        mvkh    .S2     xdj_chip_save, b0
        ldw     .D2T2   *+b0(16), b15
        ldw     .D2T2   *+b15(4), b1
        mvkl    .S2     0x11802360, b0
        mvkh    .S2     0x11802360, b0
        b       .S2     b0
||      ldw     .D2T2   *+b15(8), b0
        nop     5

/*
 * The stock routine returns here, on the stock stack, interrupts allowed
 * again. One that comes before the dint finds the stock stack and comes back
 * here; after it the way back is set in the interrupt return pointer.
 */
xdj_chip_tick_back:
        dint
        stw     .D2T2   b0, *b15--(8)
xdj_chip_tick_back_rest:
        mvkl    .S2     xdj_chip_tick_slots, b0
        mvkh    .S2     xdj_chip_tick_slots, b0
        ldw     .D2T2   *+b0(4), b0
        nop     4
        mvc     .S2     b0, irp
        mvkl    .S2     xdj_chip_tick_slots, b0
        mvkh    .S2     xdj_chip_tick_slots, b0
        ldw     .D2T2   *b0, b15
        ldw     .D2T2   *+b15(8), b0
        b       .S2     irp
        nop     5


/* Our stack pointer and where the interrupt came while it runs on the stock stack; how many it moved. */
        .section .entry_data, "aw"
        .align  4
xdj_chip_tick_slots:
        .word   0, 0
xdj_chip_tick_moved:
        .word   0
.endif

/* The processor's cycle counter, for the C code. */
        .text
        .global xdj_chip_cycles
xdj_chip_cycles:
        mvc     .S2     tscl, b4
        b       .S2     b3
        mv      .L1X    b4, a4
        nop     4
