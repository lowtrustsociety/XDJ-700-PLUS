/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * Puts the audio processor's part of FLAC decoding in place (the release,
 * --release; src/chip_flac.h).
 *
 * The build appends our block for the audio processor (code and data,
 * src/chip_flac.c) after the tail's own memory. The bridge calls
 * xdj_chip_load at the first FLAC open after power-on, and again after the
 * audio processor was started over: it writes the block into the stretch of
 * the audio processor's memory that the stock program never touches, clears
 * the block's status words and own memory and the main processor's control
 * words, reads everything back, and only if every word matches does it point
 * the stock table of PCM converters at our routine: its 16-bit stereo entry,
 * only if that held the stock converter, and read back. No instruction of
 * the audio processor's stock program is changed.
 *
 * Through the stock routines the application uses for the audio processor's
 * port: 0x08BF2C92(address, words, count) writes a run of words (as the
 * feed sends the FLAC bytes, src/chip_flac_feed.c), 0x08BF2C30(address,
 * value) one word and 0x08BF2BCC(address) reads one.
 */

#include <stddef.h>
#include <stdint.h>

#include "chip_flac.h"
/* A turn for the other tasks after this many words. */
#define XDJ_CHIP_TURN_WORDS 256u

#ifdef XDJ_HOST_TEST
/* The runner supplies the audio processor, the stock calls and what the build appends. */
extern uint32_t xdj_host_dsp_read(uint32_t address);
extern void xdj_host_dsp_write(uint32_t address, uint32_t value);
extern void xdj_host_dsp_write_words(uint32_t address, const uint32_t *words, uint32_t count);
extern int xdj_host_delay(int ticks);
extern const uint32_t *xdj_host_chip_blob(void);
#define XDJ_DSP_READ xdj_host_dsp_read
#define XDJ_DSP_WRITE xdj_host_dsp_write
#define XDJ_DSP_WRITE_WORDS xdj_host_dsp_write_words
#define XDJ_TASK_DELAY xdj_host_delay
#define XDJ_CHIP_BLOB xdj_host_chip_blob()
#else
typedef uint32_t (*xdj_dsp_read_fn)(uint32_t address);
typedef void (*xdj_dsp_write_fn)(uint32_t address, uint32_t value);
typedef void (*xdj_dsp_write_words_fn)(uint32_t address, const uint32_t *words, uint32_t count);
typedef int (*xdj_task_delay_fn)(int ticks);
#define XDJ_DSP_READ ((xdj_dsp_read_fn)0x08BF2BCCu)
#define XDJ_DSP_WRITE ((xdj_dsp_write_fn)0x08BF2C30u)
#define XDJ_DSP_WRITE_WORDS ((xdj_dsp_write_words_fn)0x08BF2C92u)
#define XDJ_TASK_DELAY ((xdj_task_delay_fn)0x08E0335Cu)
/* The build appends it right after the tail's own memory (__xdj_tail_bss_end of the linker script). */
extern const uint32_t _xdj_tail_bss_end[];
#define XDJ_CHIP_BLOB _xdj_tail_bss_end
#endif

static uint32_t xdj_chip_written;
static uint32_t xdj_chip_mismatches;
static uint32_t xdj_chip_first_bad;
static uint32_t xdj_chip_turn;

static void xdj_chip_pause(void) {
    xdj_chip_turn++;
    if (xdj_chip_turn >= XDJ_CHIP_TURN_WORDS) {
        xdj_chip_turn = 0;
        XDJ_TASK_DELAY(1);
    }
}

/* Reads them back and counts what differs. */
static void xdj_chip_verify(uint32_t address, const uint32_t *words, uint32_t count) {
    uint32_t index;

    for (index = 0; index < count; index++) {
        if (XDJ_DSP_READ(address + index * 4u) != (words != NULL ? words[index] : 0u)) {
            if (xdj_chip_mismatches == 0u) {
                xdj_chip_first_bad = address + index * 4u;
            }
            xdj_chip_mismatches++;
        }
        xdj_chip_pause();
    }
}

/* What the loader leaves after the status words: its mark, the words written, those read back wrong, the first. */
static void xdj_chip_note(void) {
    XDJ_DSP_WRITE(XDJ_CHIP_BASE + XDJ_CHIP_LOADER_NOTE, XDJ_CHIP_LOADER_MAGIC);
    XDJ_DSP_WRITE(XDJ_CHIP_BASE + XDJ_CHIP_LOADER_NOTE + 4u, xdj_chip_written);
    XDJ_DSP_WRITE(XDJ_CHIP_BASE + XDJ_CHIP_LOADER_NOTE + 8u, xdj_chip_mismatches);
    XDJ_DSP_WRITE(XDJ_CHIP_BASE + XDJ_CHIP_LOADER_NOTE + 12u, xdj_chip_first_bad);
}

/* Words of one block write; the zeros it sends where memory is cleared. */
#define XDJ_CHIP_LOAD_PIECE_WORDS 256u
/* The kernel's millisecond count (as the bridge's diagnostics clock reads it): the note says how long a load took. */
#ifdef XDJ_HOST_TEST
extern uint32_t xdj_host_ms(void);
#define XDJ_NOW_MS() xdj_host_ms()
#else
#define XDJ_NOW_MS() (*(volatile const uint32_t *)0x0B0E5D10u)
#endif
static uint32_t xdj_chip_zeros[XDJ_CHIP_LOAD_PIECE_WORDS];

/* Writes count words (zeros when words is NULL) from address on, a block write at a time, each followed by a turn. */
static void xdj_chip_send(uint32_t address, const uint32_t *words, uint32_t count) {
    uint32_t done;

    for (done = 0; done < count; done += XDJ_CHIP_LOAD_PIECE_WORDS) {
        uint32_t part = count - done < XDJ_CHIP_LOAD_PIECE_WORDS ? count - done : XDJ_CHIP_LOAD_PIECE_WORDS;

        XDJ_DSP_WRITE_WORDS(address + done * 4u, words != NULL ? words + done : xdj_chip_zeros, part);
        xdj_chip_written += part;
        XDJ_TASK_DELAY(1);
    }
}

/*
 * Puts our block into the audio processor's memory and points the stock
 * converter table at it. Returns 1 if the table holds our entry when it is
 * done, 0 if not; then nothing of ours is reached on the audio processor.
 * Called by the bridge without its lock, from the parser task, at most one
 * at a time, while no track is fed (nothing else writes there meanwhile).
 */
int xdj_chip_load(void) {
    const uint32_t *blob = XDJ_CHIP_BLOB;
    uint32_t block_words;
    uint32_t block_end;
    uint32_t pass;
    uint32_t index;
    uint32_t started;

    /* What the build appended: the mark, the block's length in words, 0, the same length again. */
    if (blob[0] != XDJ_CHIP_BLOB_MAGIC || blob[2] != 0u || blob[3] != blob[1]) {
        return 0;
    }
    block_words = blob[1];
    block_end = XDJ_CHIP_BASE + XDJ_CHIP_STATUS_BYTES + block_words * 4u;
    if (block_words == 0u || block_words > (XDJ_CHIP_BLOCK_LIMIT - XDJ_CHIP_BASE - XDJ_CHIP_STATUS_BYTES) / 4u) {
        return 0;
    }
    /* Only over the program the build checked: the entry still the stock converter. Nothing is written otherwise. */
    if (XDJ_DSP_READ(XDJ_CHIP_TABLE_ENTRY) != XDJ_CHIP_STOCK_CONVERTER) {
        return 0;
    }

    started = XDJ_NOW_MS();
    /* In the tail's BSS, which is not cleared at boot. */
    for (index = 0; index < XDJ_CHIP_LOAD_PIECE_WORDS; index++) {
        xdj_chip_zeros[index] = 0u;
    }
    xdj_chip_written = 0;
    xdj_chip_mismatches = 0;
    xdj_chip_first_bad = 0;
    xdj_chip_turn = 0;
    /*
     * Pass 0 writes, pass 1 reads back: the status words cleared, the
     * block, its own memory cleared, the main processor's control words and
     * stretch records cleared.
     */
    for (pass = 0; pass < 2u; pass++) {
        void (*step)(uint32_t, const uint32_t *, uint32_t) = pass == 0u ? xdj_chip_send : xdj_chip_verify;

        step(XDJ_CHIP_BASE, NULL, XDJ_CHIP_STATUS_BYTES / 4u);
        step(XDJ_CHIP_BASE + XDJ_CHIP_STATUS_BYTES, blob + 4, block_words);
        step(block_end, NULL, (XDJ_CHIP_BLOCK_LIMIT - block_end) / 4u);
        step(XDJ_CHIP_CONTROL, NULL, XDJ_CHIP_CONTROL_BYTES / 4u);
    }
    xdj_chip_note();
    /* After the note, the milliseconds this load has taken so far (the wait of a read for this open is 500). */
    XDJ_DSP_WRITE(XDJ_CHIP_BASE + XDJ_CHIP_LOADER_NOTE + 16u, XDJ_NOW_MS() - started);
    if (xdj_chip_mismatches != 0u || XDJ_DSP_READ(XDJ_CHIP_TABLE_ENTRY) != XDJ_CHIP_STOCK_CONVERTER) {
        return 0;
    }
    /* Last of all: from here on the audio processor passes its 16-bit stereo PCM through our routine. */
    XDJ_DSP_WRITE(XDJ_CHIP_TABLE_ENTRY, XDJ_CHIP_ENTRY);
    /* The bridge hands out tags only if the table really holds our routine now. */
    return XDJ_DSP_READ(XDJ_CHIP_TABLE_ENTRY) == XDJ_CHIP_ENTRY;
}
