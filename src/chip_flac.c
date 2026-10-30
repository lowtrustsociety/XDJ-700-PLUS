/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * FLAC decoded on the audio processor, fed through the PCM path (the
 * release, --release; src/chip_flac.h says how the two sides meet).
 *
 * xdj_chip_unit stands in for the stock converter of 16-bit stereo PCM. It
 * is entered from src/chip_flac_glue.S for every unit (1/75 s of
 * frames) whose first frame carries the mark bits, on a stack of its own.
 * If the unit starts with a run of tags it is ours: the run's frames are
 * decoded out of the ring and written as floating-point samples, and any
 * other frame of the unit is converted like PCM. If not, it answers 0 and
 * the entry code hands the unit to the stock converter untouched.
 *
 * It switches the cache on for its part of the memory; the entry code
 * switches it off again after every call. Built
 * without the small-data model: the data pointer register belongs to the
 * stock program.
 *
 * Build switches for later versions (history kept: without them the code is
 * that of v0.9): XDJ_CHIP_FAST copies the ring's bytes by the word (and the
 * build turns on dr_flac.h's XDJ_FAST_CLZ, the processor's instruction for
 * leading zeros; both measured); XDJ_CHIP_HIRES
 * takes stretches of 88.2 and 96 kHz FLAC and halves them here
 * (src/chip_flac.h); XDJ_CHIP_RESTART gives up on a stream the
 * decoder cannot go on in and asks the main processor for a new one
 * (src/chip_flac.h, xdj_chip_status); XDJ_CHIP_DIRECT has the decoder
 * write straight into the two channels; XDJ_CHIP_WITHDRAW (with
 * XDJ_CHIP_RESTART) withdraws the decoder's reading place when it gives a
 * stream up; XDJ_CHIP_AHEAD decodes the next FLAC frame ahead, a channel per
 * unit, after the unit's output is written (src/flac_decoder.c);
 * XDJ_CHIP_EARLY_CACHE leaves the cache switch to the entry code, which makes
 * it before our stack is used (src/chip_flac_glue.S).
 */

#include <stddef.h>

#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#include "dr_flac.h"
#include "chip_flac.h"
#ifdef XDJ_CHIP_HIRES
#include "xdj_flac_seek.h"
#endif

typedef struct {
    drflac_uint32 sample_rate;
    drflac_uint64 frame_count;
    drflac_uint8 channels;
    drflac_uint8 bits_per_sample;
} xdj_flac_stream_info;

drflac* xdj_flac_open(drflac_read_proc, drflac_seek_proc, drflac_tell_proc, void*,
                      const drflac_allocation_callbacks*, xdj_flac_stream_info*);
drflac_uint64 xdj_flac_read_s32(drflac*, drflac_uint64, drflac_int32*);
#ifdef XDJ_CHIP_DIRECT
/* src/flac_decoder.c: straight into the two channels, each sample times scale. */
drflac_uint64 xdj_flac_read_float(drflac*, drflac_uint64, float*, float*, float);
#ifdef XDJ_CHIP_HIRES
drflac_uint64 xdj_flac_read_decimated_float(drflac*, xdj_flac_decimator*, drflac_uint64, float*, float*, float);
#endif
#endif
#ifdef XDJ_CHIP_AHEAD
void xdj_flac_ahead_begin(drflac*);
void xdj_flac_work_ahead(drflac*);
#endif

#ifdef XDJ_HOST_TEST
/* On a computer the runner supplies the memory, the cycle counter and the cache switch. */
extern unsigned char* xdj_host_chip_memory(unsigned address);
extern unsigned xdj_host_cycles(void);
extern void xdj_host_cache_on(void);
#define XDJ_AT(type, address) ((type*)xdj_host_chip_memory(address))
#define XDJ_CYCLES() xdj_host_cycles()
#define XDJ_REGISTER(address) (0xC0DE0000u | ((address) & 0xFFFFu))
#define XDJ_CACHE_ON() xdj_host_cache_on()
#else
extern unsigned xdj_chip_cycles(void);
#define XDJ_AT(type, address) ((type*)(address))
#define XDJ_CYCLES() xdj_chip_cycles()
#define XDJ_REGISTER(address) (*(const volatile unsigned*)(address))
#define XDJ_CACHE_ON() (*(volatile unsigned*)XDJ_CHIP_CACHE_REGION |= 1u)
#endif

/* 2 to the power -31 and -15: a 32-bit and a 16-bit sample as a fraction. */
#define XDJ_SCALE_32 4.656612873077392578125e-10f
#define XDJ_SCALE_16 3.0517578125e-05f
/* A position this far ahead in the same stream is reached by decoding on; further, the stream is opened afresh. */
#define XDJ_SKIP_ON 16384u
/* No stretch starts further into its stream than a stretch is long plus the largest FLAC block. */
#define XDJ_SKIP_MOST (XDJ_CHIP_TAG_FRAMES + 65536u)
#ifdef XDJ_CHIP_HIRES
/* 2 to the power -23: a 24-bit value as a fraction (what the halving filter gives). */
#define XDJ_SCALE_24 1.1920928955078125e-07f
/* Likewise at half the rate, in frames of the file: a stretch is twice as long there, and the warm-up comes first. */
#define XDJ_SKIP_MOST_HALVED (2u * XDJ_CHIP_TAG_FRAMES + 65536u + 2u * XDJ_CHIP_HALVED_WARMUP)
#endif

/* The stream being read: a place in the ring, as far as the record in hand says it is there. */
typedef struct {
    unsigned start;     /* place in the ring of its first byte */
    unsigned length;    /* bytes there, as read from the record at the unit's start */
    unsigned position;
    unsigned pushed;    /* the main processor's ring place, as read at the unit's start */
    unsigned lost;      /* a read fell on bytes already overwritten */
} xdj_chip_stream;

static xdj_chip_stream xdj_chip_file;
static drflac* xdj_chip_decoder;
static unsigned xdj_chip_arena_used;
static unsigned xdj_chip_position;   /* frames from the stream's first sample the decoder stands at */
static unsigned xdj_chip_expected;   /* the tag that follows the last one served, or 0 */
#ifdef XDJ_CHIP_HIRES
/* The decoder in hand halves (its stream is hi-res); its filter, whose history goes on from read to read. */
static unsigned xdj_chip_halved;
static xdj_flac_decimator xdj_chip_decimator;
#endif
#ifdef XDJ_CHIP_RESTART
/* The stream the decoder could not go on in: its tags are silence until their record points elsewhere. */
static unsigned xdj_chip_stuck;
static unsigned xdj_chip_stuck_start;
#endif
#ifdef XDJ_CHIP_FAST
/* A word from anywhere: copied by the word (the compiler makes it a software-pipelined loop). */
typedef struct __attribute__((packed)) {
    unsigned word;
} xdj_chip_unaligned;
#endif

#ifdef XDJ_HOST_TEST
/*
 * On the audio processor this memory is cleared by every load of the block;
 * on a computer the runner calls this where the audio processor would be
 * started over.
 */
void xdj_host_chip_restart(void)
{
    xdj_chip_decoder = NULL;
    xdj_chip_arena_used = 0;
    xdj_chip_position = 0;
    xdj_chip_expected = 0;
#ifdef XDJ_CHIP_HIRES
    xdj_chip_halved = 0;
#endif
#ifdef XDJ_CHIP_RESTART
    xdj_chip_stuck = 0;
    xdj_chip_stuck_start = 0;
#endif
    xdj_chip_file.start = 0;
    xdj_chip_file.length = 0;
    xdj_chip_file.position = 0;
    xdj_chip_file.pushed = 0;
    xdj_chip_file.lost = 0;
}
#endif

static size_t xdj_chip_read(void* context, void* output, size_t bytes)
{
    xdj_chip_stream* stream = (xdj_chip_stream*)context;
    unsigned char* destination = (unsigned char*)output;
    const unsigned char* ring = XDJ_AT(unsigned char, XDJ_CHIP_RING);
    size_t count = stream->position < stream->length ? stream->length - stream->position : 0u;
    unsigned place = stream->start + stream->position;
    size_t index;

    if (count > bytes) {
        count = bytes;
    }
    /* Bytes the ring has gone over since are no longer the stream's. */
    if (count != 0u && stream->pushed - place > XDJ_CHIP_RING_BYTES) {
        stream->lost = 1u;
        return 0;
    }
#ifdef XDJ_CHIP_FAST
    {
        /* By the word up to the ring's end, once the destination is on a word's edge; the rest by the byte. */
        unsigned at = place & (XDJ_CHIP_RING_BYTES - 1u);
        size_t straight = XDJ_CHIP_RING_BYTES - at < count ? XDJ_CHIP_RING_BYTES - at : count;

        for (index = 0; index < straight && ((size_t)(destination + index) & 3u) != 0u; index++) {
            destination[index] = ring[at + index];
        }
        for (; index + 4u <= straight; index += 4u) {
            *(unsigned*)(void*)(destination + index) = ((const xdj_chip_unaligned*)(const void*)(ring + at + index))->word;
        }
        for (; index < count; index++) {
            destination[index] = ring[(place + index) & (XDJ_CHIP_RING_BYTES - 1u)];
        }
    }
#else
    for (index = 0; index < count; index++) {
        destination[index] = ring[(place + index) & (XDJ_CHIP_RING_BYTES - 1u)];
    }
#endif
    stream->position += (unsigned)count;
    return count;
}

static drflac_bool32 xdj_chip_seek(void* context, int offset, drflac_seek_origin origin)
{
    xdj_chip_stream* stream = (xdj_chip_stream*)context;
    long target = offset;

    if (origin == DRFLAC_SEEK_CUR) {
        target += (long)stream->position;
    } else if (origin == DRFLAC_SEEK_END) {
        target += (long)stream->length;
    }
    if (target < 0 || (unsigned long)target > stream->length) {
        return DRFLAC_FALSE;
    }
    stream->position = (unsigned)target;
    return DRFLAC_TRUE;
}

static drflac_bool32 xdj_chip_tell(void* context, drflac_int64* cursor)
{
    *cursor = ((xdj_chip_stream*)context)->position;
    return DRFLAC_TRUE;
}

/* The decoder asks for its memory once per open; nothing is given back before the next. */
static void* xdj_chip_malloc(size_t size, void* user)
{
    unsigned start = (xdj_chip_arena_used + 7u) & ~7u;

    (void)user;
    if (size > XDJ_CHIP_ARENA_BYTES || start > XDJ_CHIP_ARENA_BYTES - size) {
        return NULL;
    }
    xdj_chip_arena_used = start + (unsigned)size;
    return XDJ_AT(unsigned char, XDJ_CHIP_ARENA + start);
}

static void* xdj_chip_realloc(void* pointer, size_t size, void* user)
{
    (void)pointer;
    return xdj_chip_malloc(size, user);
}

static void xdj_chip_free(void* pointer, void* user)
{
    (void)pointer;
    (void)user;
}

static const drflac_allocation_callbacks xdj_chip_allocator = {
    NULL, xdj_chip_malloc, xdj_chip_realloc, xdj_chip_free
};

/*
 * One frame's four bytes as the tag they may be: a little-endian word (the
 * bytes need not lie on a word's edge) with the fold of its way undone.
 */
static unsigned xdj_chip_word(const unsigned char* bytes)
{
    unsigned word = (unsigned)bytes[0] | ((unsigned)bytes[1] << 8) | ((unsigned)bytes[2] << 16) | ((unsigned)bytes[3] << 24);

    return XDJ_CHIP_TAG_FOLD(word);
}

/* Decode frames and drop them. Returns 0 if the decoder gives out. */
static int xdj_chip_drop(xdj_chip_status* status, unsigned frames)
{
    drflac_int32* scratch = XDJ_AT(drflac_int32, XDJ_CHIP_SCRATCH);

    status->skips += frames;
    while (frames != 0u) {
        unsigned part = frames > XDJ_CHIP_SCRATCH_FRAMES ? XDJ_CHIP_SCRATCH_FRAMES : frames;

        if ((unsigned)xdj_flac_read_s32(xdj_chip_decoder, part, scratch) != part) {
            return 0;
        }
        frames -= part;
        xdj_chip_position += part;
    }
    return 1;
}

/* Bring the decoder to target frames from the first sample of the record's stream. Returns 0 if it cannot. */
static int xdj_chip_reach(xdj_chip_status* status, const xdj_chip_record* record, unsigned target)
{
    xdj_chip_file.length = record->length;
    xdj_chip_file.lost = 0;
#ifdef XDJ_CHIP_HIRES
    /* A decoder that halves is no use here; one opened here does not halve. */
    if (xdj_chip_halved) {
        xdj_chip_decoder = NULL;
        xdj_chip_halved = 0;
    }
#endif
    if (xdj_chip_decoder != NULL && xdj_chip_file.start == record->stream_start &&
        target >= xdj_chip_position && target - xdj_chip_position <= XDJ_SKIP_ON) {
        return target == xdj_chip_position || xdj_chip_drop(status, target - xdj_chip_position);
    }
#ifdef XDJ_CHIP_RESTART
    /*
     * Far ahead in the stream in hand: the bytes there may not be in the
     * ring yet, and decoding up to it from the stream's start takes long.
     * The main processor starts a new stream there instead (xdj_chip_serve).
     */
    if (xdj_chip_decoder != NULL && xdj_chip_file.start == record->stream_start && target > xdj_chip_position) {
        return 0;
    }
#endif
    /* Another stream, or another place in this one: from the stream's start. */
    xdj_chip_decoder = NULL;
    if (target > XDJ_SKIP_MOST) {
        return 0;
    }
    xdj_chip_file.start = record->stream_start;
    xdj_chip_file.position = 0;
    xdj_chip_arena_used = 0;
    xdj_chip_position = 0;
    {
        xdj_flac_stream_info info;

        xdj_chip_decoder = xdj_flac_open(xdj_chip_read, xdj_chip_seek, xdj_chip_tell,
                                         &xdj_chip_file, &xdj_chip_allocator, &info);
#ifdef XDJ_MONO
        /* A mono stream comes out of xdj_flac_read_s32 as stereo, the one channel on both sides. */
        if (xdj_chip_decoder == NULL || (info.channels != 1u && info.channels != 2u)) {
#else
        if (xdj_chip_decoder == NULL || info.channels != 2u) {
#endif
            xdj_chip_decoder = NULL;
            status->open_failures++;
            return 0;
        }
    }
#ifdef XDJ_CHIP_AHEAD
    xdj_flac_ahead_begin(xdj_chip_decoder);
#endif
    status->opens++;
    return target == 0u || xdj_chip_drop(status, target);
}

#ifdef XDJ_CHIP_HIRES
/* Open the record's stream afresh, at its first sample. Returns 0 if it cannot. */
static int xdj_chip_open_stream(xdj_chip_status* status, const xdj_chip_record* record)
{
    xdj_flac_stream_info info;

    xdj_chip_decoder = NULL;
    xdj_chip_file.start = record->stream_start;
    xdj_chip_file.position = 0;
    xdj_chip_arena_used = 0;
    xdj_chip_position = 0;
    xdj_chip_decoder = xdj_flac_open(xdj_chip_read, xdj_chip_seek, xdj_chip_tell,
                                     &xdj_chip_file, &xdj_chip_allocator, &info);
#ifdef XDJ_MONO
    if (xdj_chip_decoder == NULL || (info.channels != 1u && info.channels != 2u)) {
#else
    if (xdj_chip_decoder == NULL || info.channels != 2u) {
#endif
        xdj_chip_decoder = NULL;
        status->open_failures++;
        return 0;
    }
#ifdef XDJ_CHIP_AHEAD
    xdj_flac_ahead_begin(xdj_chip_decoder);
#endif
    status->opens++;
    return 1;
}

/* Decode and drop frames at half the rate, the filter's history going on. Returns 0 if the decoder gives out. */
static int xdj_chip_drop_halved(xdj_chip_status* status, unsigned frames)
{
    status->skips += 2u * frames;
    if ((unsigned)xdj_flac_read_decimated(xdj_chip_decoder, &xdj_chip_decimator, frames, NULL, 0) != frames) {
        return 0;
    }
    xdj_chip_position += 2u * frames;
    return 1;
}

/*
 * As xdj_chip_reach, for a stream that is halved: target counts frames of
 * the file from the stream's first sample. Going on in the same stream
 * keeps the filter's history; a stream opened afresh is brought to the
 * warm-up's start, the history cleared, and the warm-up decoded through the
 * filter and dropped, as the bridge's seek does (xdj_flac_seek_decimated).
 */
static int xdj_chip_reach_halved(xdj_chip_status* status, const xdj_chip_record* record, unsigned target)
{
    unsigned warmup;

    xdj_chip_file.length = record->length;
    xdj_chip_file.lost = 0;
    if (xdj_chip_decoder != NULL && xdj_chip_halved && xdj_chip_file.start == record->stream_start &&
        target >= xdj_chip_position && ((target - xdj_chip_position) & 1u) == 0u &&
        target - xdj_chip_position <= XDJ_SKIP_ON) {
        return target == xdj_chip_position || xdj_chip_drop_halved(status, (target - xdj_chip_position) / 2u);
    }
#ifdef XDJ_CHIP_RESTART
    /* Far ahead in the stream in hand: as in xdj_chip_reach. */
    if (xdj_chip_decoder != NULL && xdj_chip_halved && xdj_chip_file.start == record->stream_start &&
        target > xdj_chip_position) {
        return 0;
    }
#endif
    xdj_chip_decoder = NULL;
    xdj_chip_halved = 0;
    if (target > XDJ_SKIP_MOST_HALVED || !xdj_chip_open_stream(status, record)) {
        return 0;
    }
    xdj_chip_halved = 1;
    warmup = target / 2u < XDJ_CHIP_HALVED_WARMUP ? target / 2u : XDJ_CHIP_HALVED_WARMUP;
    xdj_flac_decimator_reset(&xdj_chip_decimator);
    xdj_chip_decimator.shift = 1u;
    return (target == 2u * warmup || xdj_chip_drop(status, target - 2u * warmup)) &&
           (warmup == 0u || xdj_chip_drop_halved(status, warmup));
}
#endif

#ifdef XDJ_CHIP_RESTART
/* Whether the record is that of the stretch numbered so, of either kind. */
static int xdj_chip_record_ours(const xdj_chip_record* record, unsigned number)
{
#ifdef XDJ_CHIP_HIRES
    if (record->id == (XDJ_CHIP_RECORD_HALVED | number)) {
        return 1;
    }
#endif
    return record->id == (XDJ_CHIP_RECORD_LIVE | number);
}

/* Ask the main processor for a new stream from this frame of this stretch (see xdj_chip_status). */
static void xdj_chip_ask(xdj_chip_status* status, unsigned number, unsigned frame, unsigned stream)
{
    status->ask_sequence++;
    status->ask_number = number;
    status->ask_frame = frame;
    status->ask_stream = stream;
    status->ask_sequence++;
}
#endif

/* Tell the main processor how far the decoder is (see xdj_chip_status). */
static void xdj_chip_publish(xdj_chip_status* status)
{
    status->sequence++;
    status->at_stream = xdj_chip_file.start;
    status->at_frames = xdj_chip_position + xdj_chip_decoder->currentFLACFrame.pcmFramesRemaining;
    status->at_bytes = xdj_chip_file.position;
    status->sequence++;
}

/* The tags in sequence from the frame at bytes on, at most limit: same stretch, frame numbers counting up. */
static unsigned xdj_chip_run(const unsigned char* bytes, unsigned limit)
{
    unsigned first = xdj_chip_word(bytes);
    unsigned run = 0;

    if (limit != 0u && (first & XDJ_CHIP_TAG_MARK_MASK) == XDJ_CHIP_TAG_MARK) {
        run = 1;
        while (run < limit && xdj_chip_word(bytes + run * 4u) == first + run &&
               ((first + run) & (XDJ_CHIP_TAG_FRAMES - 1u)) != 0u) {
            run++;
        }
    }
    return run;
}

/* Decode the run of tags that starts with first into left and right; what cannot be had is silence. */
static void xdj_chip_serve(xdj_chip_status* status, unsigned first, unsigned run, float* left, float* right)
{
    const xdj_chip_record* record;
    unsigned number = (first >> XDJ_CHIP_TAG_NUMBER_SHIFT) & (XDJ_CHIP_TAG_NUMBERS - 1u);
    unsigned frame = first & (XDJ_CHIP_TAG_FRAMES - 1u);
#ifndef XDJ_CHIP_DIRECT
    drflac_int32* scratch = XDJ_AT(drflac_int32, XDJ_CHIP_SCRATCH);
#endif
    unsigned served = 0;
    unsigned index;
    int known;
#ifdef XDJ_CHIP_RESTART
    int waiting = 0;
#endif

    record = XDJ_AT(xdj_chip_record, XDJ_CHIP_RECORDS) + (number % XDJ_CHIP_RECORD_COUNT);
    known = record->id == (XDJ_CHIP_RECORD_LIVE | number);
    xdj_chip_file.lost = 0;
#ifdef XDJ_CHIP_RESTART
    if (xdj_chip_stuck && record->stream_start == xdj_chip_stuck_start && xdj_chip_record_ours(record, number)) {
        /* Stuck in this stream: silence until the main processor points the stretch at a new one. */
        xdj_chip_ask(status, number, frame, record->stream_start);
        status->stuck_units++;
        waiting = 1;
    } else
#endif
#ifdef XDJ_CHIP_HIRES
    if (record->id == (XDJ_CHIP_RECORD_HALVED | number)) {
        /* Hi-res: two frames of the file decoded for each tag, and halved. */
        if (xdj_chip_reach_halved(status, record, record->base_frames + 2u * frame)) {
#ifdef XDJ_CHIP_DIRECT
            served = (unsigned)xdj_flac_read_decimated_float(xdj_chip_decoder, &xdj_chip_decimator, run, left, right,
                                                             XDJ_SCALE_24);
            xdj_chip_position += 2u * served;
#else
            served = (unsigned)xdj_flac_read_decimated(xdj_chip_decoder, &xdj_chip_decimator, run,
                                                       (unsigned char*)scratch, 0);
            xdj_chip_position += 2u * served;
            for (index = 0; index < served; index++) {
                left[index] = (float)scratch[index * 2u] * XDJ_SCALE_24;
                right[index] = (float)scratch[index * 2u + 1u] * XDJ_SCALE_24;
            }
#endif
            xdj_chip_publish(status);
        }
        known = 1;
    } else
#endif
    if (!known) {
        status->no_record++;
    } else if (xdj_chip_reach(status, record, record->base_frames + frame)) {
#ifdef XDJ_CHIP_DIRECT
        /* Straight into the two channels. */
        served = (unsigned)xdj_flac_read_float(xdj_chip_decoder, run, left, right, XDJ_SCALE_32);
        xdj_chip_position += served;
#else
        served = (unsigned)xdj_flac_read_s32(xdj_chip_decoder, run, scratch);
        xdj_chip_position += served;
        for (index = 0; index < served; index++) {
            left[index] = (float)scratch[index * 2u] * XDJ_SCALE_32;
            right[index] = (float)scratch[index * 2u + 1u] * XDJ_SCALE_32;
        }
#endif
        xdj_chip_publish(status);
    }
    if (served != run) {
        /* The decoder starts afresh next time. */
#ifdef XDJ_CHIP_RESTART
        if (waiting) {
        } else if (xdj_chip_file.lost) {
#else
        if (xdj_chip_file.lost) {
#endif
            status->lost++;
        } else if (known) {
            status->short_reads++;
        }
#ifdef XDJ_CHIP_RESTART
        if (!waiting && known) {
            /* Try no more in this stream: ask for a new one from the first frame not served. */
            xdj_chip_stuck = 1;
            xdj_chip_stuck_start = record->stream_start;
            status->asks++;
            xdj_chip_ask(status, number, frame + served, record->stream_start);
#ifdef XDJ_CHIP_WITHDRAW
            /*
             * And withdraw where the decoder last read (sequence 0: not
             * known). The main processor fills the ring only so far ahead of
             * that place, and the decoder will not read there again: kept,
             * it could hold a new stream to no bytes at all (the ring filled
             * up to its limit, then a jump).
             */
            status->sequence = 0;
#endif
        }
#endif
        xdj_chip_decoder = NULL;
        for (index = served; index < run; index++) {
            left[index] = 0.0f;
            right[index] = 0.0f;
        }
    }
    status->frames += served;
    if (status->frames < served) {
        status->frames_high++;
    }
    /* The tag to come, unless the stretch's frame numbers end here. */
    xdj_chip_expected = ((first + run) & (XDJ_CHIP_TAG_FRAMES - 1u)) != 0u ? first + run : 0u;
}


/*
 * One unit: count samples (both channels) of 16-bit stereo at source, to be
 * written as floating point at left and, XDJ_CHIP_RIGHT_OFFSET bytes on, at
 * right. Returns 1 if the unit was ours and is done, 0 if it is the stock
 * converter's.
 *
 * A unit is ours if it starts with a run of tags that fills it, or is at
 * least XDJ_CHIP_RUN_LEAST long, or carries on exactly where the last tag
 * served left off: real audio never does any of that. Inside a unit of ours
 * a later run counts on the same terms, or if it reaches the unit's end (a
 * stretch gives way to the next inside a unit now and then); every other
 * frame is plain 16-bit PCM, converted as the stock converter would.
 */


#ifdef XDJ_CHIP_TICK_STACK
/*
 * The output interrupt kept off our stack (and
 * src/chip_flac.h): at our first unit the interrupt's vector is
 * pointed at xdj_chip_tick (src/chip_flac_glue.S), with interrupts
 * held so that no interrupt can find one word changed and not the other.
 * Only if both words are the stock ones; once done it stays (the block is
 * loaded once). The program cache keeps the vector too, so it is emptied.
 */
static unsigned xdj_chip_tick_state;  /* 1 installed, 2 refused */
static unsigned xdj_chip_tick_found[2];
static unsigned xdj_chip_tick_table;  /* the interrupt table pointer as read */
#ifndef XDJ_HOST_TEST
extern void xdj_chip_tick(void);
extern unsigned xdj_chip_tick_moved;
#endif

static void xdj_chip_tick_install(void)
{
#ifndef XDJ_HOST_TEST
    /*
     * The table the processor uses (its pointer, ISTP): the stock program
     * runs a copy of it in internal memory. That
     * memory goes through the data cache, so the changed words are written
     * back before the program cache is emptied. Done before our part of the
     * memory is made cacheable, so the write-back has little to do.
     */
    volatile unsigned* vector;
    volatile unsigned* data_cache = (volatile unsigned*)XDJ_CHIP_DATA_CACHE_BACK;
    volatile unsigned* program_cache = (volatile unsigned*)XDJ_CHIP_PROGRAM_CACHE_EMPTY;
    unsigned address = (unsigned)xdj_chip_tick;
    unsigned low = (XDJ_CHIP_TICK_STOCK_LOW & ~XDJ_CHIP_TICK_FIELD) | ((address & 0xFFFFu) << 7);
    unsigned high = (XDJ_CHIP_TICK_STOCK_HIGH & ~XDJ_CHIP_TICK_FIELD) | ((address >> 16) << 7);
    unsigned table;
    unsigned tries;

    if (xdj_chip_tick_state != 0u) {
        return;
    }
    __asm__ volatile ("mvc .S2 istp, %0" : "=b"(table));
    xdj_chip_tick_table = table;
    vector = (volatile unsigned*)((table & ~0x3FFu) + XDJ_CHIP_TICK_VECTOR_AT);
    xdj_chip_tick_found[0] = vector[0];
    xdj_chip_tick_found[1] = vector[1];
    if (xdj_chip_tick_found[0] != XDJ_CHIP_TICK_STOCK_LOW || xdj_chip_tick_found[1] != XDJ_CHIP_TICK_STOCK_HIGH) {
        xdj_chip_tick_state = 2u;
        return;
    }
    __asm__ volatile ("dint" ::: "memory");
    vector[0] = low;
    vector[1] = high;
    *data_cache = 1u;
    for (tries = 0; tries < 1000000u && (*data_cache & 1u) != 0u; tries++) {
    }
    for (tries = 0; tries < 1000000u && (vector[0] != low || vector[1] != high); tries++) {
    }
    *program_cache = 1u;
    for (tries = 0; tries < 1000000u && (*program_cache & 1u) != 0u; tries++) {
    }
    __asm__ volatile ("rint" ::: "memory");
    xdj_chip_tick_state = 1u;
#endif
}

#endif

#ifdef XDJ_CHIP_AHEAD
/*
 * After a unit's output: a step of the decoder's
 * next frame, if the ring already holds all the bytes that frame can take
 * (reckoned as the main processor does, xdj_feed_bound), so that reading
 * ahead never runs into bytes not yet pushed.
 */
static void xdj_chip_ahead(void)
{
    if (xdj_chip_decoder != NULL) {
        unsigned plain = (unsigned)xdj_chip_decoder->maxBlockSizeInPCMFrames * xdj_chip_decoder->channels *
                         (((unsigned)xdj_chip_decoder->bitsPerSample + 7u) / 8u);

        if (xdj_chip_file.position <= xdj_chip_file.length &&
            xdj_chip_file.length - xdj_chip_file.position >= plain + (plain >> 3) + 64u) {
            xdj_flac_work_ahead(xdj_chip_decoder);
        }
    }
}
#endif


unsigned xdj_chip_unit(const unsigned char* source, float* left, unsigned count)
{
    xdj_chip_status* status;
    float* right = (float*)((unsigned char*)left + XDJ_CHIP_RIGHT_OFFSET);
    unsigned frames = count >> 1;
    unsigned started;
    unsigned spent;
    unsigned run;
    unsigned index;

#ifdef XDJ_CHIP_TICK_STACK
    xdj_chip_tick_install();
#endif
#ifndef XDJ_CHIP_EARLY_CACHE
    XDJ_CACHE_ON();
#endif
    started = XDJ_CYCLES();
    status = XDJ_AT(xdj_chip_status, XDJ_CHIP_STATUS);
    status->magic = XDJ_CHIP_STATUS_MAGIC;
    status->units++;
    status->region_now = 0x100u | (XDJ_REGISTER(XDJ_CHIP_CACHE_REGION) & 0xFFu);
#ifdef XDJ_CHIP_TICK_STACK
#endif

    if (frames == 0u || frames > XDJ_CHIP_UNIT_MOST) {
        return 0;
    }
    run = xdj_chip_run(source, frames);
    if (run < frames && run < XDJ_CHIP_RUN_LEAST && (run == 0u || xdj_chip_word(source) != xdj_chip_expected)) {
        return 0;
    }
    status->units_ours++;
    xdj_chip_file.pushed = XDJ_AT(xdj_chip_control, XDJ_CHIP_CONTROL)->pushed;

    index = 0;
    while (index < frames) {
        const unsigned char* bytes = source + index * 4u;

        if (index != 0u) {
            run = xdj_chip_run(bytes, frames - index);
        }
        if (run >= XDJ_CHIP_RUN_LEAST || (run != 0u && (index + run == frames || index == 0u))) {
            xdj_chip_serve(status, xdj_chip_word(bytes), run, left + index, right + index);
            index += run;
        } else {
            left[index] = (float)(short)((unsigned)bytes[0] | ((unsigned)bytes[1] << 8)) * XDJ_SCALE_16;
            right[index] = (float)(short)((unsigned)bytes[2] | ((unsigned)bytes[3] << 8)) * XDJ_SCALE_16;
            status->plain_frames++;
            index++;
        }
    }
#ifdef XDJ_CHIP_AHEAD
    xdj_chip_ahead();
#endif

    spent = XDJ_CYCLES() - started;
    status->cycles_low += spent;
    if (status->cycles_low < spent) {
        status->cycles_high++;
    }
    if (spent > status->longest) {
        status->longest = spent;
    }
    return 1;
}
