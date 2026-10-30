/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * Host runner: the bridge feeding the audio processor (the release,
 * --release), end to end. The bridge (src/flac_adapter.c
 * built with XDJ_CHIP_FLAC), its feed, the loader that puts our block in
 * place (src/chip_flac_loader.c) and the audio processor's converter
 * (src/chip_flac.c) run together, with one stretch of memory standing
 * for the audio processor's free memory, one word for the 16-bit stereo
 * entry of its table of converters, and plain functions for the port.
 *
 * The runner plays the player's part through the bridge's own entry points:
 * it parses tracks, seeks to byte offsets and reads byte counts that split
 * frames, joins the bytes of the reads that follow a seek into one fill, and
 * cuts the fill into units of 1/75 s, as the audio processor does. A unit
 * goes to our converter only while the table holds our entry, as on the
 * audio processor; otherwise to the stock one. What comes out goes to a file
 * for tools/check_chip_flac.py to compare with the reference decoder.
 *
 * Mode "lazy" goes through: a 96 kHz file opened first (never fed, and no
 * load for it); the first FLAC that can be fed, whose open puts our block in
 * place; 16-bit and 24-bit files fed, both presented as 16-bit; more tracks
 * than slots, so that the fed ones lose their slots and are opened again on
 * their next read; then the audio processor started over (the table holds
 * the stock entry again): the fed tracks go on, decoded by the bridge as
 * 16-bit streams, with no tag handed out, and opening a 96 kHz file changes
 * nothing; the next open of a FLAC that can be fed (a track parsed again)
 * puts the block back, the track keeping the form the player was given; a
 * track opened after that is fed; and the older ones, opened again, are fed
 * again.
 *
 * Modes "nostock" (the table's entry is not the stock converter) and "stuck"
 * (a word of the block's memory does not take what is written): the load
 * fails, it is not tried again, and every track is decoded by the bridge as
 * a stream of its own depth.
 *
 * Mode "hires", built with XDJ_CHIP_HIRES (88.2 and 96 kHz fed too): the 96 kHz file opened first puts our block in
 * place and is fed, at half the rate, as a 16-bit stream, among other fed
 * tracks; it loses its slot and is opened again, fed; the audio processor is
 * started over and the bridge decodes it at half the rate, still a 16-bit
 * stream; parsed again, it puts the block back, and once opened again it is
 * fed again.
 *
 * Usage: runner mode out.bin fed16.flac fed24.flac hires.flac more.flac...
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chip_flac.h"

int xdj_flac_adapter(void* file_io_context, void* output_descriptor, void* parser_workspace);
size_t xdj_flac_virtual_read_format(
    void* file_context, void* output, size_t byte_count, int* transfer_status, void* fs_state, void* format
);
int xdj_flac_virtual_seek_format(void* file_context, int byte_offset, int origin, void* fs_state, void* format);
unsigned xdj_chip_unit(const unsigned char* source, float* left, unsigned count);
void xdj_host_chip_restart(void);

#define PCM_FORMAT_OFFSET 340U
#define TRACKS_MOST 12u
#define HIRES 2u
#define BLOCK_WORDS 3001u

void xdj_flac_lock_acquire(void)
{
}

void xdj_flac_lock_release(void)
{
}

typedef struct {
    FILE* fp;
} host_file;

int xdj_host_mock_seek(void* context, int offset, int origin)
{
    host_file* file = (host_file*)context;

    return fseek(file->fp, (long)offset, origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END) == 0 ? 0 : -1;
}

size_t xdj_host_mock_read(void* context, void* output, size_t byte_count, int* status)
{
    if (status != NULL) {
        *status = 0;
    }
    return fread(output, 1, byte_count, ((host_file*)context)->fp);
}

int xdj_host_mock_tell(void* context)
{
    return (int)ftell(((host_file*)context)->fp);
}

/* The audio processor's free memory, its converter table entry, and the port to them. */
static unsigned char g_memory[XDJ_CHIP_MEMORY_END - XDJ_CHIP_BASE];
static uint32_t g_table = XDJ_CHIP_STOCK_CONVERTER;
static uint32_t g_stuck_address = 0;
static uint32_t g_blob[4u + BLOCK_WORDS];
static unsigned g_cycles = 0;
static unsigned g_cache_on = 0;
static unsigned g_faults = 0;
static unsigned g_port_writes = 0;
static unsigned g_loads = 0;      /* block writes to the block's first word: loads begun */

unsigned char* xdj_host_chip_memory(unsigned address)
{
    if (address < XDJ_CHIP_BASE || address >= XDJ_CHIP_MEMORY_END) {
        fprintf(stderr, "address outside the audio processor's free memory: %08x\n", address);
        exit(4);
    }
    return g_memory + (address - XDJ_CHIP_BASE);
}

unsigned xdj_host_cycles(void)
{
    g_cycles += 1000u;
    return g_cycles;
}

void xdj_host_cache_on(void)
{
    g_cache_on = 1;
}

unsigned xdj_host_port_read(unsigned address)
{
    unsigned value;

    if (address == XDJ_CHIP_TABLE_ENTRY) {
        return g_table;
    }
    memcpy(&value, xdj_host_chip_memory(address), 4);
    return value;
}

void xdj_host_port_write(unsigned address, unsigned value)
{
    g_port_writes++;
    if (address == XDJ_CHIP_TABLE_ENTRY) {
        g_table = value;
        return;
    }
    if (address == g_stuck_address) {
        return;
    }
    memcpy(xdj_host_chip_memory(address), &value, 4);
}

void xdj_host_port_write_words(unsigned address, const unsigned* words, unsigned count)
{
    unsigned index;

    if ((address & 3u) != 0u || count == 0u) {
        g_faults++;
    }
    if (address == XDJ_CHIP_ENTRY) {
        g_loads++;
    }
    (void)xdj_host_chip_memory(address + count * 4u - 1u);
    for (index = 0; index < count; index++) {
        xdj_host_port_write(address + index * 4u, words[index]);
    }
}

/* The loader's names for the same port, and what the build appends for it. */
uint32_t xdj_host_dsp_read(uint32_t address)
{
    return xdj_host_port_read(address);
}

void xdj_host_dsp_write(uint32_t address, uint32_t value)
{
    xdj_host_port_write(address, value);
}

void xdj_host_dsp_write_words(uint32_t address, const uint32_t* words, uint32_t count)
{
    xdj_host_port_write_words(address, words, count);
}

const uint32_t* xdj_host_chip_blob(void)
{
    return g_blob;
}

int xdj_host_delay(int ticks)
{
    (void)ticks;
    return 0;
}

uint32_t xdj_host_ms(void)
{
    return 0;
}

typedef struct {
    const char* path;
    unsigned char descriptor[1024];
    host_file parser;
    host_file task;
    unsigned frame_bytes;   /* bytes of a frame as the descriptor says: 4 or 6 */
    unsigned rate;          /* thousands, as the descriptor says */
    unsigned bytes;         /* declared data bytes */
    unsigned units_ours;    /* units of its fills the converter took for tags */
} track;

static track g_tracks[TRACKS_MOST];
static FILE* g_out;
static unsigned g_units_ours = 0, g_units_stock = 0, g_pieces = 0, g_tags_to_stock = 0;
static unsigned g_seed = 4242u;

static unsigned rnd(unsigned below)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return ((g_seed >> 8) & 0xFFFFFFu) % below;
}

static void* format_of(track* t)
{
    return t->descriptor + PCM_FORMAT_OFFSET;
}

static int parse(unsigned which)
{
    track* t = &g_tracks[which];

    if (t->parser.fp == NULL) {
        t->parser.fp = fopen(t->path, "rb");
        t->task.fp = fopen(t->path, "rb");
    }
    if (t->parser.fp == NULL || t->task.fp == NULL || xdj_flac_adapter(&t->parser, t->descriptor, NULL) != 0) {
        fprintf(stderr, "cannot parse %s\n", t->path);
        exit(2);
    }
    t->frame_bytes = (unsigned)t->descriptor[PCM_FORMAT_OFFSET + 7u] * 2u;
    t->rate = t->descriptor[PCM_FORMAT_OFFSET + 6u];
    memcpy(&t->bytes, t->descriptor + 24u, 4);
    return 1;
}

/* One piece of output: which track, which form, the frame it starts at, how many frames. */
static void put_header(unsigned which, unsigned form, unsigned frame, unsigned frames)
{
    unsigned header[4];

    header[0] = which;
    header[1] = form;
    header[2] = frame;
    header[3] = frames;
    fwrite(header, 4, 4, g_out);
    g_pieces++;
}

static unsigned word_at(const unsigned char* bytes)
{
    return XDJ_CHIP_TAG_FOLD((unsigned)bytes[0] | ((unsigned)bytes[1] << 8) | ((unsigned)bytes[2] << 16) |
                             ((unsigned)bytes[3] << 24));
}

/* A unit the stock converter is given that starts with a run of tags: what must never happen. */
static int starts_with_tags(const unsigned char* block)
{
    unsigned first = word_at(block);
    unsigned index;

    if ((first & XDJ_CHIP_TAG_MARK_MASK) != XDJ_CHIP_TAG_MARK) {
        return 0;
    }
    for (index = 1; index < XDJ_CHIP_RUN_LEAST; index++) {
        if (word_at(block + index * 4u) != first + index) {
            return 0;
        }
    }
    return 1;
}

/*
 * A fill: seek to frame, then reads of odd sizes until frames frames are in
 * hand; then, for a 16-bit stream, units through a converter (form 0: ours,
 * form 2: the stock one, both floating point), and for a 24-bit one the
 * bytes as they are (form 1).
 */
static void fill(unsigned which, unsigned frame, unsigned frames)
{
    static unsigned char bytes[64u * XDJ_CHIP_UNIT_MOST * 6u + 64u];
    static float out[2u * 1024u];
    track* t = &g_tracks[which];
    unsigned unit = t->rate == 48u ? 640u : 588u;
    unsigned wanted = frames * t->frame_bytes;
    unsigned have = 0;
    unsigned done;

    if (wanted > sizeof(bytes) - 64u) {
        g_faults++;
        return;
    }
    if (xdj_flac_virtual_seek_format(&t->task, (int)(frame * t->frame_bytes), 0, NULL, format_of(t)) != 0) {
        g_faults++;
        return;
    }
    while (have < wanted) {
        unsigned count = 1u + rnd(9000u);
        int status = 0;
        size_t got;

        if (count > wanted - have) {
            count = wanted - have;
        }
        got = xdj_flac_virtual_read_format(&t->task, bytes + have, count, &status, NULL, format_of(t));
        if (status != 0) {
            g_faults++;
            return;
        }
        if (got == 0u) {
            break;  /* the track's end */
        }
        have += (unsigned)got;
    }
    /* Past the track's end the player's own buffer holds silence. */
    memset(bytes + have, 0, wanted - have);
    if (t->frame_bytes == 6u) {
        put_header(which, 1u, frame, frames);
        fwrite(bytes, 1, wanted, g_out);
        return;
    }
    for (done = 0; done < frames; done += unit) {
        static unsigned char block[XDJ_CHIP_UNIT_MOST * 4u];
        float* left = out;
        float* right = (float*)((unsigned char*)out + XDJ_CHIP_RIGHT_OFFSET);
        unsigned part = frames - done < unit ? frames - done : unit;
        unsigned form = 0;
        unsigned index;

        memset(block, 0, sizeof(block));
        memcpy(block, bytes + done * 4u, part * 4u);
        g_cache_on = 0;
        if (g_table == XDJ_CHIP_ENTRY && xdj_chip_unit(block, left, unit * 2u)) {
            g_units_ours++;
            t->units_ours++;
            if (!g_cache_on) {
                g_faults++;
            }
        } else {
            /* The stock converter. */
            form = 2;
            g_units_stock++;
            if (starts_with_tags(block)) {
                g_tags_to_stock++;
            }
            for (index = 0; index < unit; index++) {
                left[index] = (float)(short)(block[index * 4u] | (block[index * 4u + 1u] << 8)) / 32768.0f;
                right[index] = (float)(short)(block[index * 4u + 2u] | (block[index * 4u + 3u] << 8)) / 32768.0f;
            }
        }
        put_header(which, form, frame + done, part);
        for (index = 0; index < part; index++) {
            fwrite(&left[index], 4, 1, g_out);
            fwrite(&right[index], 4, 1, g_out);
        }
    }
}

/* A few fills at random places of a track, the first at its start and the second at its end. */
static void play(unsigned which, unsigned fills)
{
    track* t = &g_tracks[which];
    unsigned unit = t->rate == 48u ? 640u : 588u;
    unsigned total = t->bytes / t->frame_bytes;
    unsigned index;

    for (index = 0; index < fills; index++) {
        unsigned frame = index == 0u ? 0u : index == 1u ? total - total % unit - unit : rnd(total);

        frame -= frame % unit;
        fill(which, frame, unit * (1u + rnd(12u)));
    }
}

static int block_in_place(void)
{
    return memcmp(xdj_host_chip_memory(XDJ_CHIP_ENTRY), g_blob + 4, BLOCK_WORDS * 4u) == 0;
}

static void print_status(const char* name, const xdj_chip_status* status)
{
    printf(", \"%s\": {\"units_ours\": %u, \"frames\": %u, \"opens\": %u, \"skips\": %u, \"no_record\": %u, "
           "\"lost\": %u, \"short_reads\": %u, \"open_failures\": %u}",
           name, status->units_ours, status->frames, status->opens, status->skips, status->no_record, status->lost,
           status->short_reads, status->open_failures);
}

static void print_list(const char* name, unsigned count, int fed)
{
    unsigned index;

    printf(", \"%s\": [", name);
    for (index = 0; index < count; index++) {
        if (fed) {
            printf("%s%s", index ? ", " : "", g_tracks[index].units_ours != 0u ? "true" : "false");
        } else {
            printf("%s%u", index ? ", " : "", g_tracks[index].frame_bytes);
        }
    }
    printf("]");
}

int main(int argc, char** argv)
{
    xdj_chip_status* status = (xdj_chip_status*)xdj_host_chip_memory(XDJ_CHIP_STATUS);
    const char* mode = argc > 1 ? argv[1] : "";
    unsigned count = (unsigned)argc - 3u;
    unsigned index;

    if (argc < 8 || count > TRACKS_MOST) {
        fprintf(stderr, "usage: runner mode out.bin fed16.flac fed24.flac hires.flac more.flac...\n");
        return 2;
    }
    g_out = fopen(argv[2], "wb");
    if (g_out == NULL) {
        return 2;
    }
    /* The memory starts as rubbish: the loader must clear what our side reads. */
    for (index = 0; index < sizeof(g_memory); index++) {
        g_memory[index] = (unsigned char)(index * 7u + 3u);
    }
    g_blob[0] = XDJ_CHIP_BLOB_MAGIC;
    g_blob[1] = BLOCK_WORDS;
    g_blob[2] = 0;
    g_blob[3] = BLOCK_WORDS;
    for (index = 4; index < sizeof(g_blob) / sizeof(g_blob[0]); index++) {
        g_blob[index] = index * 2654435761u ^ 0x5A5A0000u;
    }
    for (index = 0; index < count; index++) {
        g_tracks[index].path = argv[3u + index];
    }

    if (strcmp(mode, "lazy") == 0) {
        unsigned writes_before_load;
        unsigned table_before_load;
        unsigned loaded_at_first_open;
        unsigned ours_first_round;
        unsigned ours_at_restart;
        unsigned ours_after_restart;
        unsigned loads_after_restart;
        unsigned frame_bytes_after_restart;
        unsigned ours_new_track;
        unsigned ours_before_reopen;
        xdj_chip_status before_restart;

        /* A 96 kHz file first: not fed, and nothing sent for it. */
        parse(HIRES);
        play(HIRES, 2);
        writes_before_load = g_port_writes;
        table_before_load = g_table;

        /* The first FLAC that can be fed puts our block in place. */
        parse(0);
        loaded_at_first_open = g_loads == 1u && block_in_place() && g_table == XDJ_CHIP_ENTRY;
        parse(1);
        for (index = 0; index < 5u; index++) {
            play(0, 3);
            play(1, 3);
            play(HIRES, 2);
        }
        ours_first_round = g_units_ours;
        /* More tracks than there are slots: the first ones are put aside, and opened again when read. */
        for (index = 3; index + 1u < count; index++) {
            parse(index);
            play(index, 2);
        }
        play(0, 4);
        play(1, 4);
        play(HIRES, 2);
        if (g_loads != 1u) {
            g_faults++;
        }

        /* The audio processor is started over: its table holds the stock entry again. */
        memcpy(&before_restart, status, sizeof(before_restart));
        g_table = XDJ_CHIP_STOCK_CONVERTER;
        xdj_host_chip_restart();
        ours_at_restart = g_units_ours;
        play(0, 4);
        play(1, 4);
        /* Opening a 96 kHz file loads nothing. */
        parse(HIRES);
        play(HIRES, 1);
        play(1, 3);
        play(0, 2);
        ours_after_restart = g_units_ours;
        loads_after_restart = g_loads;

        /* The next open of a FLAC that can be fed puts the block back; the same file parsed again keeps its form. */
        parse(1);
        frame_bytes_after_restart = g_tracks[1].frame_bytes;
        play(1, 2);
        /* A track opened now is fed. */
        parse(count - 1u);
        play(count - 1u, 4);
        ours_new_track = g_tracks[count - 1u].units_ours;
        /* The others lose their slots once more; opened again, the fed ones are fed again. */
        for (index = 3; index + 1u < count; index++) {
            parse(index);
            play(index, 1);
        }
        ours_before_reopen = g_units_ours;
        play(0, 4);
        play(1, 4);
        play(HIRES, 1);
        fclose(g_out);

        printf("{\"mode\": \"lazy\", \"faults\": %u, \"pieces\": %u, \"units_ours\": %u, \"units_stock\": %u, "
               "\"port_writes_before_load\": %u, \"table_before_load_stock\": %s, \"loaded_at_first_open\": %s, "
               "\"units_ours_first_round\": %u, \"units_ours_at_restart\": %u, \"units_ours_after_restart\": %u, "
               "\"tags_to_stock\": %u, \"loads_before_reload\": %u, \"frame_bytes_after_restart\": %u, "
               "\"loads\": %u, \"table_at_end_ours\": %s, \"units_ours_new_track\": %u, "
               "\"units_ours_reopened\": %u",
               g_faults, g_pieces, g_units_ours, g_units_stock, writes_before_load,
               table_before_load == XDJ_CHIP_STOCK_CONVERTER ? "true" : "false",
               loaded_at_first_open ? "true" : "false", ours_first_round, ours_at_restart, ours_after_restart,
               g_tags_to_stock, loads_after_restart, frame_bytes_after_restart, g_loads,
               g_table == XDJ_CHIP_ENTRY && block_in_place() ? "true" : "false", ours_new_track,
               g_units_ours - ours_before_reopen);
        print_status("status_before_restart", &before_restart);
#ifdef XDJ_CHIP_HIRES
    } else if (strcmp(mode, "hires") == 0) {
        unsigned loaded_at_hires_open;
        unsigned ours_first;
        unsigned ours_at_restart;
        unsigned ours_after_restart;
        unsigned frame_bytes_after_restart;
        unsigned ours_before_reload;

        /* The 96 kHz file first: it can be fed now, so its open puts our block in place. */
        parse(HIRES);
        loaded_at_hires_open = g_loads == 1u && block_in_place() && g_table == XDJ_CHIP_ENTRY;
        parse(0);
        parse(1);
        for (index = 0; index < 4u; index++) {
            play(HIRES, 3);
            play(0, 2);
            play(1, 2);
        }
        ours_first = g_tracks[HIRES].units_ours;
        /* More tracks than slots: it is put aside, and opened again when read. */
        for (index = 3; index < count; index++) {
            parse(index);
            play(index, 2);
        }
        play(HIRES, 4);
        /* The audio processor is started over: the bridge halves it, a 16-bit stream still. */
        g_table = XDJ_CHIP_STOCK_CONVERTER;
        xdj_host_chip_restart();
        ours_at_restart = g_tracks[HIRES].units_ours;
        play(HIRES, 4);
        ours_after_restart = g_tracks[HIRES].units_ours;
        /*
         * Parsed again: the block goes back and the track keeps its form;
         * put aside once more and opened again, it is fed again.
         */
        parse(HIRES);
        frame_bytes_after_restart = g_tracks[HIRES].frame_bytes;
        for (index = 3; index < count; index++) {
            parse(index);
            play(index, 1);
        }
        ours_before_reload = g_tracks[HIRES].units_ours;
        play(HIRES, 4);
        fclose(g_out);
        printf("{\"mode\": \"hires\", \"faults\": %u, \"pieces\": %u, \"units_ours\": %u, \"units_stock\": %u, "
               "\"loaded_at_hires_open\": %s, \"hires_units_ours_first\": %u, \"hires_units_ours_at_restart\": %u, "
               "\"hires_units_ours_after_restart\": %u, \"hires_frame_bytes_after_restart\": %u, "
               "\"hires_units_ours_reloaded\": %u, \"loads\": %u, \"tags_to_stock\": %u",
               g_faults, g_pieces, g_units_ours, g_units_stock, loaded_at_hires_open ? "true" : "false", ours_first,
               ours_at_restart, ours_after_restart, frame_bytes_after_restart,
               g_tracks[HIRES].units_ours - ours_before_reload, g_loads, g_tags_to_stock);
#endif
    } else if (strcmp(mode, "nostock") == 0 || strcmp(mode, "stuck") == 0) {
        if (strcmp(mode, "nostock") == 0) {
            g_table = 0xC003C55Cu;
        } else {
            g_stuck_address = XDJ_CHIP_BLOCK_LIMIT - 64u;
        }
        for (index = 0; index < count; index++) {
            parse(index);
            play(index, 3);
        }
        play(0, 3);
        play(1, 3);
        fclose(g_out);
        printf("{\"mode\": \"%s\", \"faults\": %u, \"pieces\": %u, \"units_ours\": %u, \"units_stock\": %u, "
               "\"port_writes\": %u, \"loads\": %u, \"table_unchanged\": %s, \"tags_to_stock\": %u",
               mode, g_faults, g_pieces, g_units_ours, g_units_stock, g_port_writes, g_loads,
               g_table == (strcmp(mode, "nostock") == 0 ? 0xC003C55Cu : XDJ_CHIP_STOCK_CONVERTER) ? "true" : "false",
               g_tags_to_stock);
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 2;
    }
    print_list("frame_bytes", count, 0);
    print_list("fed", count, 1);
    print_status("status", status);
    printf("}\n");
    return g_faults == 0u ? 0 : 3;
}
