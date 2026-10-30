/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * Host runner for FLAC decoded on the audio processor, both sides together:
 * the main processor's feed (src/chip_flac_feed.c) and the audio
 * processor's converter (src/chip_flac.c), with one stretch of memory
 * standing for the audio processor's and plain functions for the port.
 *
 * It plays the part of the player: it reads the virtual PCM of one or two
 * FLAC files through the feed in pieces, with jumps, cuts what it gets into
 * units of 1/75 s as the audio processor does, and gives each unit to the
 * converter, a few reads late (the tags are on their way for a while). What
 * the converter wrote goes to a file, with the place in the track it was
 * read for, for tools/check_chip_flac.py to compare with the reference
 * decoder. Units the converter leaves to the stock converter are counted.
 *
 * Built with XDJ_CHIP_HIRES, 88.2 and 96 kHz files are taken at half the
 * rate and halved on the audio processor's side, and mode "halvedref" writes
 * the first file as the bridge's own halving gives it (every frame from the
 * start, as 24-bit values in words), for the check to compare with.
 *
 * Built with XDJ_CHIP_RESTART, mode "skip" plays a track as a very fast jog
 * does: now and then the units of many reads never reach the converter, so
 * the next ones lie far ahead in the stream. The converter must then ask
 * for a new stream, write silence meanwhile, and be right again soon after.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#include "dr_flac.h"
#include "xdj_flac_seek.h"
#include "chip_flac_feed.h"

typedef struct {
    drflac_uint32 sample_rate;
    drflac_uint64 frame_count;
    drflac_uint8 channels;
    drflac_uint8 bits_per_sample;
} xdj_flac_stream_info;

drflac* xdj_flac_open(drflac_read_proc, drflac_seek_proc, drflac_tell_proc, void*,
                      const drflac_allocation_callbacks*, xdj_flac_stream_info*);
drflac_bool32 xdj_flac_locate(drflac*, drflac_uint64, xdj_flac_seek_state*, drflac_uint32*, drflac_uint32*);
unsigned xdj_chip_unit(const unsigned char* source, float* left, unsigned count);

/* The audio processor's free memory. */
static unsigned char g_memory[XDJ_CHIP_MEMORY_END - XDJ_CHIP_BASE];
static unsigned g_cycles = 0;
static unsigned g_cache_on = 0;
static unsigned g_faults = 0;

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

    memcpy(&value, xdj_host_chip_memory(address), 4);
    return value;
}

void xdj_host_port_write(unsigned address, unsigned value)
{
    memcpy(xdj_host_chip_memory(address), &value, 4);
}

void xdj_host_port_write_words(unsigned address, const unsigned* words, unsigned count)
{
    if ((address & 3u) != 0u || count == 0u) {
        g_faults++;
    }
    memcpy(xdj_host_chip_memory(address), words, (size_t)count * 4u);
    (void)xdj_host_chip_memory(address + count * 4u - 1u);
}

/* A FLAC file in memory, the decoder the main processor keeps for it, and its feed. */
typedef struct {
    unsigned char* data;
    unsigned bytes;
    unsigned position;
    drflac* decoder;
    xdj_flac_seek_state seek;
    xdj_feed_track feed;
    xdj_feed_io io;
    unsigned frames;
    unsigned locates;
    unsigned file_reads;
    unsigned file_bytes_read;
    unsigned halved;
} track;

static size_t file_read(void* user, void* output, size_t count)
{
    track* t = (track*)user;
    size_t left = t->bytes - t->position;

    if (count > left) {
        count = left;
    }
    memcpy(output, t->data + t->position, count);
    t->position += (unsigned)count;
    return count;
}

static drflac_bool32 file_seek(void* user, int offset, drflac_seek_origin origin)
{
    track* t = (track*)user;
    long target = origin == DRFLAC_SEEK_SET ? offset : origin == DRFLAC_SEEK_CUR ? (long)t->position + offset : (long)t->bytes + offset;

    if (target < 0 || (unsigned long)target > t->bytes) {
        return DRFLAC_FALSE;
    }
    t->position = (unsigned)target;
    return DRFLAC_TRUE;
}

static drflac_bool32 file_tell(void* user, drflac_int64* cursor)
{
    *cursor = ((track*)user)->position;
    return DRFLAC_TRUE;
}

static int feed_locate(void* user, unsigned frame, unsigned* offset, unsigned* first)
{
    track* t = (track*)user;

    t->locates++;
    return xdj_flac_locate(t->decoder, frame, &t->seek, offset, first) ? 1 : 0;
}

static unsigned feed_read(void* user, unsigned offset, unsigned char* output, unsigned count)
{
    track* t = (track*)user;
    unsigned left = offset < t->bytes ? t->bytes - offset : 0u;

    if (count > left) {
        count = left;
    }
    memcpy(output, t->data + offset, count);
    t->file_reads++;
    t->file_bytes_read += count;
    return count;
}

static void* host_malloc(size_t size, void* user)
{
    (void)user;
    return malloc(size);
}

static void* host_realloc(void* pointer, size_t size, void* user)
{
    (void)user;
    return realloc(pointer, size);
}

static void host_free(void* pointer, void* user)
{
    (void)user;
    free(pointer);
}

static int open_track(track* t, const char* path)
{
    static const drflac_allocation_callbacks allocator = { NULL, host_malloc, host_realloc, host_free };
    xdj_flac_stream_info info;
    FILE* file = fopen(path, "rb");

    memset(t, 0, sizeof(*t));
    if (file == NULL) {
        return 0;
    }
    fseek(file, 0, SEEK_END);
    t->bytes = (unsigned)ftell(file);
    fseek(file, 0, SEEK_SET);
    t->data = malloc(t->bytes);
    if (t->data == NULL || fread(t->data, 1, t->bytes, file) != t->bytes) {
        return 0;
    }
    fclose(file);
    t->decoder = xdj_flac_open(file_read, file_seek, file_tell, t, &allocator, &info);
    if (t->decoder == NULL) {
        return 0;
    }
    xdj_flac_seek_reset(&t->seek, t->bytes);
    t->frames = (unsigned)info.frame_count;
    t->io.user = t;
    t->io.locate = feed_locate;
    t->io.read = feed_read;
#ifdef XDJ_CHIP_HIRES
    if (info.sample_rate == 88200u || info.sample_rate == 96000u) {
        /* As the bridge gives it to the player: at half the rate. */
        t->frames >>= 1;
        if (!xdj_feed_open(&t->feed, t->data, t->frames, info.bits_per_sample)) {
            return 0;
        }
        xdj_feed_halve(&t->feed);
        t->halved = 1;
        return 1;
    }
#endif
    return xdj_feed_open(&t->feed, t->data, t->frames, info.bits_per_sample);
}

/* Tags on their way: what a read produced, given to the converter a few reads later. */
#define QUEUE 16u
#define READ_MOST 8192u
typedef struct {
    unsigned track;
    unsigned position;      /* track frame of the first frame */
    unsigned frames;
    unsigned char bytes[READ_MOST * 4u];
} flight;
static flight g_queue[QUEUE];
static unsigned g_head = 0, g_count = 0;

static FILE* g_out;
static unsigned g_units = 0, g_units_ours = 0, g_units_stock = 0, g_frames_out = 0;
/* Units still to be passed over without reaching the converter (modes "skip" and "skip2"). */
static unsigned g_skip = 0, g_skipped = 0;
#ifdef XDJ_CHIP_RESTART
static unsigned g_double = 0;
#endif
/* "skip2": after a gap, one unit converted (it finds the stream far ahead and gives it up), then a second gap. */
static unsigned g_again = 0, g_between = 0;

/* As the audio processor does: units of unit frames cut from the bytes of one read, each to the converter. */
static void convert(const flight* f, unsigned unit)
{
    static float out[2u * 1024u];
    unsigned done = 0;

    while (done < f->frames) {
        unsigned frames = f->frames - done < unit ? f->frames - done : unit;
        static unsigned char block[XDJ_CHIP_UNIT_MOST * 4u];
        float* left = out;
        float* right = (float*)((unsigned char*)out + XDJ_CHIP_RIGHT_OFFSET);
        unsigned index;
        unsigned handled;
        unsigned header[3];

        if (g_skip != 0u) {
            g_skip--;
            g_skipped++;
            done += frames;
            if (g_skip == 0u && g_again != 0u) {
                g_between = 1u;
            }
            continue;
        }
        if (g_between != 0u && --g_between == 0u) {
            g_skip = g_again;
            g_again = 0;
        }
        /* A last, short piece of a read is filled up with plain silence, as a fill's end is. */
        memset(block, 0, sizeof(block));
        memcpy(block, f->bytes + done * 4u, frames * 4u);
        for (index = 0; index < 2u * 1024u; index++) {
            out[index] = 99.0f;
        }
        g_cache_on = 0;
        handled = xdj_chip_unit(block, left, unit * 2u);
        if (!g_cache_on) {
            g_faults++;
        }
        g_units++;
        if (handled) {
            g_units_ours++;
        } else {
            /* The stock converter. */
            g_units_stock++;
            for (index = 0; index < unit; index++) {
                left[index] = (float)(short)(block[index * 4u] | (block[index * 4u + 1u] << 8)) / 32768.0f;
                right[index] = (float)(short)(block[index * 4u + 2u] | (block[index * 4u + 3u] << 8)) / 32768.0f;
            }
        }
        header[0] = f->track;
        header[1] = f->position + done;
        header[2] = frames;
        fwrite(header, 4, 3, g_out);
        for (index = 0; index < frames; index++) {
            fwrite(&left[index], 4, 1, g_out);
            fwrite(&right[index], 4, 1, g_out);
        }
        /* What follows the piece in a short unit must be silence. */
        for (index = frames; index < unit; index++) {
            if (left[index] != 0.0f || right[index] != 0.0f) {
                g_faults++;
            }
        }
        g_frames_out += frames;
        done += frames;
    }
}

static void drain(unsigned keep, unsigned unit)
{
    while (g_count > keep) {
        convert(&g_queue[g_head], unit);
        g_head = (g_head + 1u) % QUEUE;
        g_count--;
    }
}

static unsigned g_seed = 12345u;
static unsigned rnd(unsigned below)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return ((g_seed >> 8) & 0xFFFFFFu) % below;
}

/* One read of frames track frames at position through the feed; returns the frames got. */
static unsigned g_most_ahead = 0;

static unsigned read_track(track* tracks, unsigned which, unsigned position, unsigned frames, unsigned delay, unsigned unit)
{
    flight* f;
    unsigned got;
    xdj_chip_status* status = (xdj_chip_status*)xdj_host_chip_memory(XDJ_CHIP_STATUS);

    drain(delay, unit);
    f = &g_queue[(g_head + g_count) % QUEUE];
    f->track = which;
    f->position = position;
    got = xdj_feed_read(&tracks[which].feed, position, frames, f->bytes, &tracks[which].io);
    f->frames = got;
    if (got != 0u) {
        g_count++;
    }
    /* How far the ring runs ahead of where the audio processor's decoder reads. */
    if (status->sequence != 0u) {
        unsigned ahead = xdj_host_port_read(XDJ_CHIP_CONTROL + 4u) - (status->at_stream + status->at_bytes);

        if (ahead > g_most_ahead && ahead < 0x80000000u) {
            g_most_ahead = ahead;
        }
    }
    return got;
}

int main(int argc, char** argv)
{
    static track tracks[2];
    xdj_chip_status* status = (xdj_chip_status*)xdj_host_chip_memory(XDJ_CHIP_STATUS);
    const char* mode;
    unsigned delay;
    unsigned unit;
    unsigned count;
    unsigned index;

    if (argc < 6) {
        fprintf(stderr, "usage: runner mode delay unit out file.flac [file.flac]\n");
        return 2;
    }
    mode = argv[1];
    delay = (unsigned)strtoul(argv[2], NULL, 10);
    unit = (unsigned)strtoul(argv[3], NULL, 10);
    g_out = fopen(argv[4], "wb");
    count = (unsigned)argc - 5u;
    if (g_out == NULL || delay >= QUEUE || unit > XDJ_CHIP_UNIT_MOST || count > 2u) {
        return 2;
    }
    /* The memory starts as rubbish; the loader clears the status and the control words. */
    for (index = 0; index < sizeof(g_memory); index++) {
        g_memory[index] = (unsigned char)(index * 7u + 3u);
    }
    memset(xdj_host_chip_memory(XDJ_CHIP_STATUS), 0, XDJ_CHIP_STATUS_BYTES);
    memset(xdj_host_chip_memory(XDJ_CHIP_CONTROL), 0, XDJ_CHIP_CONTROL_BYTES);
    xdj_feed_reset();
    for (index = 0; index < count; index++) {
        if (!open_track(&tracks[index], argv[5u + index])) {
            fprintf(stderr, "cannot take on %s\n", argv[5u + index]);
            return 2;
        }
    }

    if (strcmp(mode, "seq") == 0) {
        /* From the start to the end, in reads of mixed sizes. */
        unsigned position = 0;

        while (position < tracks[0].frames) {
            unsigned got = read_track(tracks, 0, position, 1u + rnd(READ_MOST), delay, unit);

            if (got == 0u) {
                g_faults++;
                break;
            }
            position += got;
        }
    } else if (strcmp(mode, "units") == 0) {
        /* As the player reads: one unit at a time, from the start to the end. */
        unsigned position = 0;

        while (position < tracks[0].frames) {
            unsigned got = read_track(tracks, 0, position, unit, delay, unit);

            if (got == 0u) {
                g_faults++;
                break;
            }
            position += got;
        }
    } else if (strcmp(mode, "seeks") == 0) {
        /* Jumps all over the track, each followed by a few reads. */
        unsigned jump;

        for (jump = 0; jump < 60u; jump++) {
            unsigned position = jump == 0u ? tracks[0].frames - 3000u : jump == 1u ? 0u : rnd(tracks[0].frames);
            unsigned reads = 1u + rnd(4u);

            /* Whole units from a unit's edge, as the player asks. */
            position -= position % unit;
            while (reads-- != 0u && position < tracks[0].frames) {
                unsigned got = read_track(tracks, 0, position, unit * (1u + rnd(8u)), delay, unit);

                if (got == 0u) {
                    g_faults++;
                    break;
                }
                position += got;
            }
        }
    } else if (strcmp(mode, "fill") == 0 && count == 2u) {
        /*
         * As the player plays one track: its reads ahead go to the converter
         * in order, while between them it reads what it never plays here:
         * the second track at its cue points (the track cache), and the
         * first behind the playing place. Those tags are thrown away.
         */
        static unsigned char thrown[READ_MOST * 4u];
        unsigned position = 0;
        unsigned turn = 0;

        while (position < tracks[0].frames) {
            unsigned got = read_track(tracks, 0, position, unit * 4u, delay, unit);

            if (got == 0u) {
                g_faults++;
                break;
            }
            position += got;
            turn++;
            if (turn % 7u == 0u) {
                unsigned cue = rnd(tracks[1].frames - unit * 8u);

                (void)xdj_feed_read(&tracks[1].feed, cue, unit * 4u, thrown, &tracks[1].io);
            }
            if (turn % 11u == 0u && position > unit * 40u) {
                (void)xdj_feed_read(&tracks[0].feed, position - unit * 40u, unit * 4u, thrown, &tracks[0].io);
            }
        }
    } else if (strcmp(mode, "two") == 0 && count == 2u) {
        /* Two tracks by turns, as when a hot cue bank is prepared while a track fills. */
        unsigned position[2] = { 0u, 0u };
        unsigned turn;

        for (turn = 0; turn < 80u; turn++) {
            unsigned which = turn % 3u == 2u ? 1u : 0u;
            unsigned got;

            if (rnd(5u) == 0u) {
                position[which] = rnd(tracks[which].frames);
                position[which] -= position[which] % unit;
            }
            if (position[which] >= tracks[which].frames) {
                position[which] = 0;
            }
            got = read_track(tracks, which, position[which], unit * (1u + rnd(6u)), delay, unit);
            if (got == 0u) {
                g_faults++;
                break;
            }
            position[which] += got;
        }
#ifdef XDJ_CHIP_HIRES
    } else if (strcmp(mode, "halvedref") == 0 && tracks[0].halved) {
        /* The bridge's halving of the whole track, from its start: what our converter must give at every frame. */
        static drflac_int32 values[2u * 1024u];
        xdj_flac_decimator decimator;
        unsigned position = 0;

        xdj_flac_decimator_reset(&decimator);
        decimator.shift = 1;
        while (position < tracks[0].frames) {
            unsigned part = tracks[0].frames - position < 1024u ? tracks[0].frames - position : 1024u;

            if ((unsigned)xdj_flac_read_decimated(tracks[0].decoder, &decimator, part, (unsigned char*)values, 0) != part) {
                g_faults++;
                break;
            }
            fwrite(values, 8, part, g_out);
            position += part;
        }
        fclose(g_out);
        printf("{\"frames\": %u, \"faults\": %u}\n", position, g_faults);
        return g_faults == 0u ? 0 : 3;
#endif
#ifdef XDJ_CHIP_RESTART
    } else if (strcmp(mode, "farjump") == 0) {
        /*
         * As the player before a jump: reading far ahead in large reads, so
         * that the ring is filled as far as it may be from where the decoder
         * reads; then a jump, the reads ahead thrown away unconverted, and
         * play from the new place. The new stream must get its bytes.
         */
        unsigned position = 0;
        unsigned round;

        for (round = 0; round < 3u; round++) {
            unsigned reads;

            for (reads = 0; reads < 40u && position < tracks[0].frames; reads++) {
                unsigned got = read_track(tracks, 0, position, unit * 12u, delay, unit);

                if (got == 0u) {
                    g_faults++;
                    break;
                }
                position += got;
            }
            /* The jump: what was read ahead is never played. */
            g_head = (g_head + g_count) % QUEUE;
            g_count = 0;
            position = (tracks[0].frames / 4u) * (round + 1u) + 7777u;
            position -= position % unit;
        }
        while (position < tracks[0].frames) {
            unsigned got = read_track(tracks, 0, position, unit * 12u, delay, unit);

            if (got == 0u) {
                g_faults++;
                break;
            }
            position += got;
        }
    } else if (strcmp(mode, "skip") == 0 || strcmp(mode, "skip2") == 0 || strcmp(mode, "skipfar") == 0) {
        /* Unit by unit from the start; of every 100 units, 70 are passed over (in "skip2" twice 70, close together). */
        unsigned position = 0;
        unsigned reads = 0;

        /* "skipfar": as "skip2", but read far ahead in large reads, as the player does (seconds ahead). */
        unsigned far = strcmp(mode, "skipfar") == 0;

        g_double = strcmp(mode, "skip2") == 0 || far;

        while (position < tracks[0].frames) {
            unsigned got = read_track(tracks, 0, position, far ? unit * 12u : unit, delay, unit);

            if (got == 0u) {
                g_faults++;
                break;
            }
            position += got;
            if (++reads % (far ? 12u : 100u) == 0u) {
                g_skip = 70u;
                /* Mode "skip2": a second gap right after the one unit that gives the stream up. */
                g_again = g_double ? 30u : 0u;
            }
        }
#endif
    } else if (strcmp(mode, "plain") == 0) {
        /* No tags at all: real audio, some of it looking like a tag here and there. The converter must leave it all. */
        static flight f;
        unsigned frame;

        f.track = 0;
        f.position = 0;
        f.frames = unit * 12u;
        for (frame = 0; frame < f.frames; frame++) {
            unsigned word = (rnd(65536u) | (rnd(65536u) << 16));

            if (frame % unit < 7u && frame / unit % 2u == 0u) {
                /* Seven frames that count up with the mark bits set: one short of a run that counts. */
                word = XDJ_CHIP_TAG_MARK | (5u << XDJ_CHIP_TAG_NUMBER_SHIFT) | (1000u + frame % unit);
                word = XDJ_CHIP_TAG_FOLD(word);
            }
            memcpy(f.bytes + frame * 4u, &word, 4);
        }
        convert(&f, unit);
        if (g_units_ours != 0u) {
            g_faults++;
        }
    } else {
        return 2;
    }
    drain(0, unit);
    fclose(g_out);

    printf("{\"units\": %u, \"units_ours\": %u, \"units_stock\": %u, \"frames_out\": %u, \"faults\": %u, "
           "\"status\": {\"magic\": %u, \"units\": %u, \"units_ours\": %u, \"frames\": %u, \"opens\": %u, \"skips\": %u, "
           "\"no_record\": %u, \"lost\": %u, \"short_reads\": %u, \"open_failures\": %u, \"plain_frames\": %u, "
           "\"sequence\": %u, \"cycles_low\": %u, \"longest\": %u}, \"tracks\": [",
           g_units, g_units_ours, g_units_stock, g_frames_out, g_faults,
           status->magic, status->units, status->units_ours, status->frames, status->opens, status->skips,
           status->no_record, status->lost, status->short_reads, status->open_failures, status->plain_frames,
           status->sequence, status->cycles_low, status->longest);
    for (index = 0; index < count; index++) {
        printf("%s{\"frames\": %u, \"file_bytes\": %u, \"locates\": %u, \"file_reads\": %u, \"file_bytes_read\": %u}",
               index ? ", " : "", tracks[index].frames, tracks[index].bytes, tracks[index].locates,
               tracks[index].file_reads, tracks[index].file_bytes_read);
    }
    printf("], \"ring_place\": %u, \"most_ahead\": %u, \"skipped\": %u", xdj_host_port_read(XDJ_CHIP_CONTROL + 4u),
           g_most_ahead, g_skipped);
#ifdef XDJ_CHIP_RESTART
    printf(", \"asks\": %u, \"stuck_units\": %u", status->asks, status->stuck_units);
#endif
#ifdef XDJ_CHIP_ODD_LOG
    {
        const xdj_chip_odd_log* log = (const xdj_chip_odd_log*)xdj_host_chip_memory(XDJ_CHIP_ODD_LOG_AT);

        printf(", \"odd_log\": {\"magic_ok\": %u, \"entries\": %u, \"odd_units\": %u, \"new_stretches\": %u, "
               "\"units_588\": %u, \"units_640\": %u, \"counts\": [%u, %u, %u, %u, %u, %u]}",
               log->magic == XDJ_CHIP_ODD_LOG_MAGIC, log->entries, log->odd_units, log->new_stretches,
               log->units_588, log->units_640, log->counts[0], log->counts[1], log->counts[2], log->counts[3],
               log->counts[4], log->counts[5]);
    }
#endif
#ifdef XDJ_CHIP_AHEAD
    {
        extern unsigned xdj_flac_ahead_ready;
        extern unsigned xdj_flac_ahead_late;

        printf(", \"ahead\": {\"ready\": %u, \"late\": %u}", xdj_flac_ahead_ready, xdj_flac_ahead_late);
    }
#endif
#ifdef XDJ_PLACES_LOG
    {
        extern xdj_places_log xdj_main_places_log;
        const xdj_places_log* log = &xdj_main_places_log;
        unsigned index;

        printf(", \"places_log\": {\"tries\": %u, \"whys\": [", log->tries);
        for (index = 0; index < XDJ_PLACES_WHYS; index++) {
            printf("%s%u", index ? ", " : "", log->whys[index]);
        }
        printf("]}");
    }
#endif
    printf("}\n");
    return g_faults == 0u ? 0 : 3;
}
