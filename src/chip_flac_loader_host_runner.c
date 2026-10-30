/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * Host test runner for the loader of FLAC decoded on the audio processor
 * (src/chip_flac_loader.c).
 *
 * Against a pretend audio processor whose memory starts as rubbish, it
 * checks that xdj_chip_load, which the bridge calls, puts the block in place
 * with block writes, clears the status words, the block's own memory and the
 * main processor's control words, touches nothing else, leaves its note, and
 * points the converter table at our routine as its very last write; that it
 * writes nothing at all over a table that does not hold the stock converter,
 * or from what the build appended if that is not right; and that it answers
 * 0 whenever the table does not hold our entry at the end.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chip_flac.h"

/* The "switch": the 16-bit stereo entry of the stock converter table, the stock routine before, ours after. */
#define SWITCH XDJ_CHIP_TABLE_ENTRY
#define SWITCH_OFF XDJ_CHIP_STOCK_CONVERTER
#define SWITCH_ON XDJ_CHIP_ENTRY

int xdj_chip_load(void);

/* The pretend memory: the converter table, a blank stretch of the stock program, and the free stretch. */
#define GAP_START 0x1182E900u
#define GAP_BYTES 0x100u
#define GAP2_START 0xC0052000u
#define GAP2_BYTES 0x200u
#define FREE_START 0xC1F3F420u
#define FREE_BYTES (XDJ_CHIP_MEMORY_END - FREE_START)
#define COPY_BYTES 0x1000u
#define CONTROL_COPY 0x800u
#define FILE_BYTES (GAP_BYTES + GAP2_BYTES + COPY_BYTES + CONTROL_COPY)

#define BLOCK_WORDS 3001u

static uint8_t g_gap[GAP_BYTES];
static uint8_t g_gap2[GAP2_BYTES];
static uint8_t *g_free;
static uint8_t *g_before;
static uint32_t g_blob[4u + BLOCK_WORDS];

static uint32_t g_writes = 0;
static uint32_t g_block_writes = 0;   /* calls of the block write */
static uint32_t g_words_in_blocks = 0;
static uint32_t g_writes_outside = 0;
static uint32_t g_last_write_address = 0;
static uint32_t g_reads_outside = 0;
static uint32_t g_stuck_address = 0;   /* a word that does not take what is written */
static unsigned g_delays = 0;

static int g_failures = 0;
static int g_checks = 0;

static void check(int condition, const char *name) {
    g_checks++;
    if (!condition) {
        g_failures++;
        fprintf(stderr, "FAILED: %s\n", name);
    }
}

static uint8_t *place(uint32_t address) {
    if ((address & 3u) != 0u) {
        return NULL;
    }
    if (address >= GAP_START && address - GAP_START < GAP_BYTES) {
        return g_gap + (address - GAP_START);
    }
    if (address >= GAP2_START && address - GAP2_START < GAP2_BYTES) {
        return g_gap2 + (address - GAP2_START);
    }
    if (address >= FREE_START && address - FREE_START < FREE_BYTES) {
        return g_free + (address - FREE_START);
    }
    return NULL;
}

uint32_t xdj_host_dsp_read(uint32_t address) {
    uint8_t *at = place(address);
    uint32_t value = 0;

    if (at == NULL) {
        g_reads_outside++;
        return 0;
    }
    memcpy(&value, at, 4u);
    return value;
}

void xdj_host_dsp_write(uint32_t address, uint32_t value) {
    uint8_t *at = place(address);

    g_writes++;
    g_last_write_address = address;
    if (at == NULL) {
        g_writes_outside++;
        return;
    }
    if (address == g_stuck_address) {
        return;
    }
    memcpy(at, &value, 4u);
}

/* The stock block write: count words, one after the other. */
void xdj_host_dsp_write_words(uint32_t address, const uint32_t *words, uint32_t count) {
    uint32_t index;

    g_block_writes++;
    g_words_in_blocks += count;
    for (index = 0; index < count; index++) {
        xdj_host_dsp_write(address + index * 4u, words[index]);
    }
}

const uint32_t *xdj_host_chip_blob(void) {
    return g_blob;
}

int xdj_host_delay(int ticks) {
    (void)ticks;
    g_delays++;
    return 0;
}

/* A clock that moves one millisecond per turn given. */
uint32_t xdj_host_ms(void) {
    return 1000u + g_delays;
}


static void reset(void) {
    g_writes = 0;
    g_block_writes = 0;
    g_words_in_blocks = 0;
    g_writes_outside = 0;
    g_reads_outside = 0;
    g_delays = 0;
}

static uint32_t word(uint32_t address) {
    return xdj_host_dsp_read(address);
}

/* Every word of [from, to) of the free stretch is zero, the loader's note left out. */
static int all_zero(uint32_t from, uint32_t to) {
    uint32_t address;

    for (address = from; address < to; address += 4u) {
        if ((address < XDJ_CHIP_BASE + XDJ_CHIP_LOADER_NOTE || address >= XDJ_CHIP_BASE + XDJ_CHIP_LOADER_NOTE + 20u) &&
            word(address) != 0u) {
            return 0;
        }
    }
    return 1;
}

static int run(const char *test) {
    uint32_t block_end = XDJ_CHIP_BASE + XDJ_CHIP_STATUS_BYTES + BLOCK_WORDS * 4u;
    uint32_t expected_words = XDJ_CHIP_STATUS_BYTES / 4u + BLOCK_WORDS + (XDJ_CHIP_BLOCK_LIMIT - block_end) / 4u +
                              XDJ_CHIP_CONTROL_BYTES / 4u;

    if (strcmp(test, "load") == 0) {

        /* The load, as the bridge calls it. */
        reset();
        check(xdj_chip_load() == 1, "the load answers 1");
        check(memcmp(g_free + (XDJ_CHIP_ENTRY - FREE_START), g_blob + 4, BLOCK_WORDS * 4u) == 0, "the block is in its place");
        check(all_zero(XDJ_CHIP_BASE, XDJ_CHIP_BASE + XDJ_CHIP_STATUS_BYTES) && all_zero(block_end, XDJ_CHIP_BLOCK_LIMIT),
              "the status words and the block's own memory are cleared");
        check(all_zero(XDJ_CHIP_CONTROL, XDJ_CHIP_CONTROL + XDJ_CHIP_CONTROL_BYTES), "the control words are cleared");
        check(memcmp(g_free, g_before, XDJ_CHIP_BASE - FREE_START) == 0 &&
              memcmp(g_free + (XDJ_CHIP_BLOCK_LIMIT - FREE_START), g_before + (XDJ_CHIP_BLOCK_LIMIT - FREE_START),
                     XDJ_CHIP_CONTROL - XDJ_CHIP_BLOCK_LIMIT) == 0 &&
              memcmp(g_free + (XDJ_CHIP_CONTROL + XDJ_CHIP_CONTROL_BYTES - FREE_START),
                     g_before + (XDJ_CHIP_CONTROL + XDJ_CHIP_CONTROL_BYTES - FREE_START),
                     XDJ_CHIP_MEMORY_END - XDJ_CHIP_CONTROL - XDJ_CHIP_CONTROL_BYTES) == 0,
              "nothing else in the free stretch is touched");
        check(g_writes_outside == 0u && g_reads_outside == 0u, "no word outside the pretend memory");
        check(word(XDJ_CHIP_BASE + XDJ_CHIP_LOADER_NOTE) == XDJ_CHIP_LOADER_MAGIC &&
              word(XDJ_CHIP_BASE + XDJ_CHIP_LOADER_NOTE + 4u) == expected_words &&
              word(XDJ_CHIP_BASE + XDJ_CHIP_LOADER_NOTE + 8u) == 0u &&
              word(XDJ_CHIP_BASE + XDJ_CHIP_LOADER_NOTE + 12u) == 0u, "the loader's note");
        check(word(XDJ_CHIP_BASE + XDJ_CHIP_LOADER_NOTE + 16u) == g_delays, "the note says how long the load took");
        check(g_words_in_blocks == expected_words && g_block_writes >= expected_words / 256u &&
              g_writes == expected_words + 5u + 1u,
              "everything but the note and the table entry goes by block writes");
        check(word(SWITCH) == SWITCH_ON && g_last_write_address == SWITCH, "the table entry is ours, the very last write");
        check(g_delays >= g_block_writes + expected_words / 256u, "the other tasks get their turns: after every block write, and every 256 words read back");

    } else if (strcmp(test, "stuck") == 0) {
        /* A word that reads back wrong. */
        g_stuck_address = XDJ_CHIP_CONTROL + 400u;
        reset();
        check(xdj_chip_load() == 0, "a word read back wrong: the load answers 0");
        check(word(SWITCH) == SWITCH_OFF, "the table is left as it was");
        check(word(XDJ_CHIP_BASE + XDJ_CHIP_LOADER_NOTE) == XDJ_CHIP_LOADER_MAGIC &&
              word(XDJ_CHIP_BASE + XDJ_CHIP_LOADER_NOTE + 8u) == 1u &&
              word(XDJ_CHIP_BASE + XDJ_CHIP_LOADER_NOTE + 12u) == g_stuck_address, "the note counts it and names its address");
    } else if (strcmp(test, "tablestuck") == 0) {
        /* The table does not take our entry. */
        g_stuck_address = SWITCH;
        reset();
        check(xdj_chip_load() == 0 && word(SWITCH) == SWITCH_OFF, "a table that does not take the entry: the load answers 0");
    } else {
        /* Nothing is written over a table that does not hold the stock converter, or from a wrong blob. */
        if (strcmp(test, "nostock") == 0) {
            uint32_t other = 0xC003C55Cu;

            memcpy(g_gap + (SWITCH - GAP_START), &other, 4u);
        } else if (strcmp(test, "nomark") == 0) {
            g_blob[0] = 0;
        } else if (strcmp(test, "badsum") == 0) {
            g_blob[3]++;
        } else if (strcmp(test, "bigblock") == 0) {
            g_blob[1] = (XDJ_CHIP_BLOCK_LIMIT - XDJ_CHIP_BASE - XDJ_CHIP_STATUS_BYTES) / 4u + 1u;
            g_blob[3] = g_blob[1];
        } else if (strcmp(test, "bigclips") == 0) {
            /* The block takes nothing after it (the timing test's clips came there). */
            g_blob[2] = 1u;
            g_blob[3] = g_blob[1] + g_blob[2];
        } else {
            return 2;
        }
        reset();
        check(xdj_chip_load() == 0 && g_writes == 0u && memcmp(g_free, g_before, FREE_BYTES) == 0, "nothing is written to the audio processor");
        check(strcmp(test, "nostock") == 0 ? word(SWITCH) == 0xC003C55Cu : word(SWITCH) == SWITCH_OFF,
              "the table is left as it was");
    }

    printf("{\"checks\": %d, \"failures\": %d, \"passed\": %s}\n", g_checks, g_failures, g_failures == 0 ? "true" : "false");
    return g_failures == 0 ? 0 : 3;
}

/*
 * The loader keeps its state like the player does until power-off, so the
 * runner is run once per case: argv[1] names it.
 */
int main(int argc, char **argv) {
    const char *test = argc > 1 ? argv[1] : "load";
    uint32_t index;
    uint32_t stock = SWITCH_OFF;

    g_free = malloc(FREE_BYTES);
    g_before = malloc(FREE_BYTES);
    if (g_free == NULL || g_before == NULL) {
        return 2;
    }
    for (index = 0; index < FREE_BYTES; index++) {
        g_free[index] = (uint8_t)(index * 31u + (index >> 9) + 5u);
    }
    memcpy(g_before, g_free, FREE_BYTES);
    memset(g_gap, 0xFF, sizeof(g_gap));
    memset(g_gap2, 0xFF, sizeof(g_gap2));
    memcpy(g_gap + (SWITCH - GAP_START), &stock, 4u);
    g_blob[0] = XDJ_CHIP_BLOB_MAGIC;
    g_blob[1] = BLOCK_WORDS;
    g_blob[2] = 0;
    g_blob[3] = BLOCK_WORDS;
    for (index = 4; index < sizeof(g_blob) / sizeof(g_blob[0]); index++) {
        g_blob[index] = index * 2654435761u ^ 0x5A5A0000u;
    }
    return run(test);
}
