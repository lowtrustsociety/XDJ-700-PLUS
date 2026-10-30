/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * The decoder unit of the XDJ-700 mod: dr_flac with the parts the adapter
 * needs (open, decode, close), the bounded seek that replaces dr_flac's own,
 * and the halving of 88.2 and 96 kHz sources to 44.1 and 48 kHz.
 *
 * It holds no device addresses. Memory comes from the caller's arena, and
 * the file is read through the callbacks the adapter passes in. (The file
 * began as a code-size probe, which is where the xdj_probe names come from.)
 */

#include <stddef.h>

static void* xdj_probe_default_malloc(size_t size);
static void* xdj_probe_default_realloc(void* pointer, size_t size);
static void xdj_probe_default_free(void* pointer);
static void* xdj_probe_copy(void* destination, const void* source, size_t size);
static void* xdj_probe_zero(void* destination, size_t size);

#define DRFLAC_API static
#define DRFLAC_ASSERT(expression) ((void)(expression))
#define DRFLAC_MALLOC(size) xdj_probe_default_malloc(size)
#define DRFLAC_REALLOC(pointer, size) xdj_probe_default_realloc(pointer, size)
#define DRFLAC_FREE(pointer) xdj_probe_default_free(pointer)
#define DRFLAC_COPY_MEMORY(destination, source, size) \
    xdj_probe_copy((destination), (source), (size))
#define DRFLAC_ZERO_MEMORY(destination, size) \
    xdj_probe_zero((destination), (size))
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_PICTURE_METADATA_MALLOC
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#include "xdj_flac_seek.h"

typedef struct {
    drflac_uint32 sample_rate;
    drflac_uint64 frame_count;
    drflac_uint8 channels;
    drflac_uint8 bits_per_sample;
} xdj_flac_stream_info;

static void* xdj_probe_default_malloc(size_t size)
{
    (void)size;
    return NULL;
}

static void* xdj_probe_default_realloc(void* pointer, size_t size)
{
    (void)pointer;
    (void)size;
    return NULL;
}

static void xdj_probe_default_free(void* pointer)
{
    (void)pointer;
}

static void* xdj_probe_copy(void* destination, const void* source, size_t size)
{
    unsigned char* dst = (unsigned char*)destination;
    const unsigned char* src = (const unsigned char*)source;

    if ((((size_t)dst | (size_t)src) & 3U) == 0U) {
        drflac_uint32* dst32 = (drflac_uint32*)dst;
        const drflac_uint32* src32 = (const drflac_uint32*)src;
        while (size >= 4U) {
            *dst32++ = *src32++;
            size -= 4U;
        }
        dst = (unsigned char*)dst32;
        src = (const unsigned char*)src32;
    }
    while (size > 0U) {
        *dst++ = *src++;
        size--;
    }
    return destination;
}

static void* xdj_probe_zero(void* destination, size_t size)
{
    unsigned char* dst = (unsigned char*)destination;

    if (((size_t)dst & 3U) == 0U) {
        drflac_uint32* dst32 = (drflac_uint32*)dst;
        while (size >= 4U) {
            *dst32++ = 0;
            size -= 4U;
        }
        dst = (unsigned char*)dst32;
    }
    while (size > 0U) {
        *dst++ = 0;
        size--;
    }
    return destination;
}

#if defined(__sh__) || defined(__SH4A__)
void* memcpy(void* destination, const void* source, size_t size)
{
    return xdj_probe_copy(destination, source, size);
}

void* memset(void* destination, int value, size_t size)
{
    unsigned char* dst = (unsigned char*)destination;
    unsigned char byte_val = (unsigned char)value;

    if (((size_t)dst & 3U) == 0U) {
        drflac_uint32 word_val = (drflac_uint32)byte_val |
                                 ((drflac_uint32)byte_val << 8) |
                                 ((drflac_uint32)byte_val << 16) |
                                 ((drflac_uint32)byte_val << 24);
        drflac_uint32* dst32 = (drflac_uint32*)dst;
        while (size >= 4U) {
            *dst32++ = word_val;
            size -= 4U;
        }
        dst = (unsigned char*)dst32;
    }
    while (size > 0U) {
        *dst++ = byte_val;
        size--;
    }
    return destination;
}
#endif

drflac* xdj_flac_open(
    drflac_read_proc read_callback,
    drflac_seek_proc seek_callback,
    drflac_tell_proc tell_callback,
    void* stream_context,
    const drflac_allocation_callbacks* allocation_callbacks,
    xdj_flac_stream_info* info
)
{
    drflac* decoder = drflac_open(
        read_callback,
        seek_callback,
        tell_callback,
        stream_context,
        allocation_callbacks
    );

    if (decoder != NULL && info != NULL) {
        info->sample_rate = decoder->sampleRate;
        info->frame_count = decoder->totalPCMFrameCount;
        info->channels = decoder->channels;
        info->bits_per_sample = decoder->bitsPerSample;
    }
    return decoder;
}

/*
 * With XDJ_MONO, mono streams are handed out as stereo, the one channel on
 * both sides: the player and the audio processor's converter only take two
 * channels. The decoder writes frames samples at the start of output; they
 * are spread out from the last one back, so nothing is overwritten before it
 * is read. Without the switch a mono stream never gets this far (the adapter
 * and the audio processor refuse it) and these do nothing.
 */
static void xdj_spread_mono_s32(const drflac* decoder, drflac_int32* output, drflac_uint64 frames)
{
#ifdef XDJ_MONO
    drflac_uint32 index = (drflac_uint32)frames;

    if (decoder->channels == 1U) {
        while (index-- != 0U) {
            output[index * 2U + 1U] = output[index];
            output[index * 2U] = output[index];
        }
    }
#else
    (void)decoder;
    (void)output;
    (void)frames;
#endif
}

static void xdj_spread_mono_s16(const drflac* decoder, drflac_int16* output, drflac_uint64 frames)
{
#ifdef XDJ_MONO
    drflac_uint32 index = (drflac_uint32)frames;

    if (decoder->channels == 1U) {
        while (index-- != 0U) {
            output[index * 2U + 1U] = output[index];
            output[index * 2U] = output[index];
        }
    }
#else
    (void)decoder;
    (void)output;
    (void)frames;
#endif
}

#ifdef XDJ_CHIP_AHEAD
/*
 * Build switch XDJ_CHIP_AHEAD, for the audio
 * processor's converter (src/chip_flac.c): the FLAC frame after the one
 * being read is decoded ahead, a channel at a time, into a second buffer, and
 * takes the place of the one being read when that runs out. The converter
 * calls xdj_flac_work_ahead after a unit's output is written, so the decoder
 * no longer stops in the middle of a unit's output to decode a whole frame.
 *
 * Only the decoder handed to xdj_flac_ahead_begin decodes ahead; every read of
 * it goes through xdj_ahead_read_s32 (XDJ_READ_S32), which never lets dr_flac
 * read the stream itself once a frame ahead is begun, and it is never sought.
 * If the second buffer cannot be had (blocks too large for the arena), each
 * frame is decoded when it is needed, as before.
 */
#define XDJ_AHEAD_DONE 0x100U    /* below it: 0 nothing begun, else the header read and this many less one channels decoded */
#define XDJ_AHEAD_FAILED 0x200U  /* the frame ahead could not be had: the reads end there, as dr_flac's would */
static struct {
    drflac* decoder;
    drflac_int32* samples;
    drflac_frame frame;
    unsigned state;
} xdj_ahead;
#ifdef XDJ_HOST_TEST
/* For the host check: frames ahead found whole when they were needed, and those finished only then. */
unsigned xdj_flac_ahead_ready = 0;
unsigned xdj_flac_ahead_late = 0;
#endif

void xdj_flac_ahead_begin(drflac* decoder)
{
    xdj_ahead.decoder = decoder;
    xdj_ahead.samples = NULL;
    xdj_ahead.state = 0;
    if (decoder != NULL) {
        /* As dr_flac's own buffer: each channel in whole lines of 64 bytes, on a line's edge. */
        size_t bytes = (((size_t)decoder->maxBlockSizeInPCMFrames + 15U) & ~(size_t)15U) * decoder->channels * 4U;
        unsigned char* raw = (unsigned char*)drflac__malloc_from_callbacks(bytes + 64U, &decoder->allocationCallbacks);

        if (raw != NULL) {
            xdj_ahead.samples = (drflac_int32*)(void*)(raw + (-(size_t)raw & 63U));
        }
    }
}

/* One step of the frame ahead: its header and first channel, or its next channel. 1 while there is more to do. */
static int xdj_ahead_step(drflac* pFlac)
{
    drflac_frame* frame = &xdj_ahead.frame;
    unsigned channel;

    if (xdj_ahead.state >= XDJ_AHEAD_DONE) {
        return 0;
    }
    if (xdj_ahead.state == 0U) {
        /* As drflac__read_and_decode_next_flac_frame and drflac__decode_flac_frame do, into the frame ahead. */
        DRFLAC_ZERO_MEMORY(frame, sizeof(*frame));
        if (!drflac__read_next_flac_frame_header(&pFlac->bs, pFlac->bitsPerSample, &frame->header) ||
            frame->header.blockSizeInPCMFrames > pFlac->maxBlockSizeInPCMFrames ||
            drflac__get_channel_count_from_channel_assignment(frame->header.channelAssignment) != (int)pFlac->channels) {
            xdj_ahead.state = XDJ_AHEAD_FAILED;
            return 0;
        }
        xdj_ahead.state = 1U;
    }
    channel = xdj_ahead.state - 1U;
    if (!drflac__decode_subframe(&pFlac->bs, frame, (int)channel,
                                 xdj_ahead.samples + frame->header.blockSizeInPCMFrames * channel)) {
        xdj_ahead.state = XDJ_AHEAD_FAILED;
        return 0;
    }
    xdj_ahead.state++;
    if (channel + 1U < pFlac->channels) {
        return 1;
    }
    {
        drflac_uint8 paddingSizeInBits = (drflac_uint8)(DRFLAC_CACHE_L1_BITS_REMAINING(&pFlac->bs) & 7);
        drflac_uint16 desiredCRC16;
#ifndef XDJ_SKIP_CRC16
        drflac_uint16 actualCRC16;
#endif

        if (paddingSizeInBits > 0) {
            drflac_uint8 padding = 0;

            if (!drflac__read_uint8(&pFlac->bs, paddingSizeInBits, &padding)) {
                xdj_ahead.state = XDJ_AHEAD_FAILED;
                return 0;
            }
        }
#ifndef XDJ_SKIP_CRC16
        actualCRC16 = drflac__flush_crc16(&pFlac->bs);
#endif
        if (!drflac__read_uint16(&pFlac->bs, 16, &desiredCRC16)) {
            xdj_ahead.state = XDJ_AHEAD_FAILED;
            return 0;
        }
#ifndef XDJ_SKIP_CRC16
        if (actualCRC16 != desiredCRC16) {
            /* dr_flac goes on to the next frame. */
            xdj_ahead.state = 0;
            return 1;
        }
#endif
    }
    frame->pcmFramesRemaining = frame->header.blockSizeInPCMFrames;
    xdj_ahead.state = XDJ_AHEAD_DONE;
    return 0;
}

/* The frame being read has run out: the frame ahead, finished now if need be, takes its place. 0 if there is none. */
static drflac_bool32 xdj_ahead_take(drflac* pFlac)
{
    drflac_int32* samples = pFlac->pDecodedSamples;

#ifdef XDJ_HOST_TEST
    if (xdj_ahead.state == XDJ_AHEAD_DONE) {
        xdj_flac_ahead_ready++;
    } else {
        xdj_flac_ahead_late++;
    }
#endif
    while (xdj_ahead_step(pFlac)) {
    }
    if (xdj_ahead.state != XDJ_AHEAD_DONE) {
        return DRFLAC_FALSE;
    }
    pFlac->pDecodedSamples = xdj_ahead.samples;
    xdj_ahead.samples = samples;
    DRFLAC_COPY_MEMORY(&pFlac->currentFLACFrame, &xdj_ahead.frame, sizeof(xdj_ahead.frame));
    xdj_ahead.state = 0;
    return DRFLAC_TRUE;
}

static drflac_uint64 xdj_ahead_read_s32(drflac* pFlac, drflac_uint64 framesToRead, drflac_int32* pBufferOut)
{
    drflac_uint64 total = 0;

    if (pFlac == NULL || pFlac != xdj_ahead.decoder || xdj_ahead.samples == NULL) {
        return drflac_read_pcm_frames_s32(pFlac, framesToRead, pBufferOut);
    }
    while (total < framesToRead) {
        drflac_uint64 part = framesToRead - total;
        drflac_uint64 frames;

        if (pFlac->currentFLACFrame.pcmFramesRemaining == 0U && !xdj_ahead_take(pFlac)) {
            break;
        }
        if (part > pFlac->currentFLACFrame.pcmFramesRemaining) {
            part = pFlac->currentFLACFrame.pcmFramesRemaining;
        }
        /* Inside the frame in hand: dr_flac does not touch the stream. */
        frames = drflac_read_pcm_frames_s32(pFlac, part, pBufferOut + total * pFlac->channels);
        total += frames;
        if (frames < part) {
            break;
        }
    }
    return total;
}

void xdj_flac_work_ahead(drflac* decoder)
{
    if (decoder != NULL && decoder == xdj_ahead.decoder && xdj_ahead.samples != NULL) {
        (void)xdj_ahead_step(decoder);
    }
}
#define XDJ_READ_S32 xdj_ahead_read_s32
#else
#define XDJ_READ_S32 drflac_read_pcm_frames_s32
#endif

drflac_uint64 xdj_flac_read_s32(
    drflac* decoder,
    drflac_uint64 frame_count,
    drflac_int32* output
)
{
    drflac_uint64 frames = XDJ_READ_S32(decoder, frame_count, output);

    xdj_spread_mono_s32(decoder, output, frames);
    return frames;
}

drflac_uint32 xdj_flac_sequence_breaks = 0;

/*
 * The frame being decoded must hold the decoder's position: its first frame
 * number plus its length equals the position plus the frames still to come.
 * A frame read from the wrong place in the file breaks this.
 */
static drflac_bool32 xdj_position_consistent(drflac* pFlac)
{
    drflac_uint64 first;
    drflac_uint64 last;

    if (pFlac->currentFLACFrame.header.blockSizeInPCMFrames == 0) {
        return DRFLAC_TRUE;
    }
    drflac__get_pcm_frame_range_of_current_flac_frame(pFlac, &first, &last);
    return pFlac->currentPCMFrame + pFlac->currentFLACFrame.pcmFramesRemaining == last + 1U;
}

drflac_uint64 xdj_flac_read_s16(
    drflac* decoder,
    drflac_uint64 frame_count,
    drflac_int16* output
)
{
    drflac_uint64 frames = drflac_read_pcm_frames_s16(decoder, frame_count, output);

    xdj_spread_mono_s16(decoder, output, frames);
    if (frames != 0U && !xdj_position_consistent(decoder)) {
        xdj_flac_sequence_breaks++;
    }
    return frames;
}

/*
 * Packed 24-bit little-endian output, as a 24-bit WAV holds it. dr_flac
 * decodes to 32-bit left-justified samples; they pass through a scratch
 * buffer and their top three bytes are stored. The scratch is static, not on
 * the 2 KB input-task stack, and is only used under the decoder lock.
 */
#define XDJ_S24_SCRATCH_FRAMES 256U
/* Room in front for the rate conversion's history (see xdj_halve). */
static drflac_int32 xdj_pcm_work[(XDJ_DECIMATOR_HISTORY + XDJ_S24_SCRATCH_FRAMES) * 2U];
#define xdj_s24_scratch (xdj_pcm_work + XDJ_DECIMATOR_HISTORY * 2U)

drflac_uint64 xdj_flac_read_s24(
    drflac* decoder,
    drflac_uint64 frame_count,
    unsigned char* output
)
{
    drflac_uint64 total = 0;

    while (total < frame_count) {
        drflac_uint64 chunk = frame_count - total;
        drflac_uint64 frames;
        drflac_uint32 index;

        if (chunk > XDJ_S24_SCRATCH_FRAMES) {
            chunk = XDJ_S24_SCRATCH_FRAMES;
        }
        frames = XDJ_READ_S32(decoder, chunk, xdj_s24_scratch);
        if (frames == 0U) {
            break;
        }
        xdj_spread_mono_s32(decoder, xdj_s24_scratch, frames);
        if (!xdj_position_consistent(decoder)) {
            xdj_flac_sequence_breaks++;
        }
        for (index = 0; index < (drflac_uint32)frames * 2U; ++index) {
            drflac_uint32 sample = (drflac_uint32)xdj_s24_scratch[index];

            output[0] = (unsigned char)(sample >> 8);
            output[1] = (unsigned char)(sample >> 16);
            output[2] = (unsigned char)(sample >> 24);
            output += 3;
        }
        total += frames;
        if (frames < chunk) {
            break;
        }
    }
    return total;
}

/*
 * Rate conversion (xdj_flac_seek.h). Samples are filtered as 24-bit values.
 *
 * The half-band low-pass: the centre tap is one half, every second tap is
 * zero, and the others come in equal pairs at odd distances from the centre.
 * XDJ_HALFBAND_TERMS lists (distance, coefficient) for one side, the
 * coefficients in Q31, summing to exactly one quarter so the gain at DC is
 * one. All are Kaiser-windowed sincs. For a 96 kHz source (the 88.2 kHz
 * figures scale):
 *
 *   taps  20 kHz    18 kHz    fold-back from 30 kHz   from 34 kHz
 *   47    -0.01 dB  0.00 dB   -67 dB                  -76 dB
 *   31    -0.26 dB  +0.02 dB  -54 dB                  -64 dB
 *   23    -0.74 dB  -0.08 dB  -41 dB                  -58 dB
 *   15    -1.54 dB  -0.54 dB  -24 dB                  -46 dB
 *
 * Sound from 30 kHz up folds to 18 kHz and below. The terms are written out
 * one by one: as a loop over a table the compiler spends more on the loop
 * than on the multiplications.
 */
#if XDJ_HALFBAND_TAPS == 47
#define XDJ_HALFBAND_TERMS(T) \
    T(1, 680080786) T(3, -217448948) T(5, 119956607) T(7, -75367437) T(9, 49179841) T(11, -32050941) \
    T(13, 20367062) T(15, -12364974) T(17, 7005226) T(19, -3574160) T(21, 1529889) T(23, -442039)
#elif XDJ_HALFBAND_TAPS == 31
#define XDJ_HALFBAND_TERMS(T) \
    T(1, 676920943) T(3, -208216827) T(5, 105996654) T(7, -58523329) T(9, 31512636) T(11, -15469736) \
    T(13, 6323524) T(15, -1672953)
#elif XDJ_HALFBAND_TAPS == 23
#define XDJ_HALFBAND_TERMS(T) \
    T(1, 672518999) T(3, -196168795) T(5, 89020184) T(7, -40231851) T(9, 15287188) T(11, -3554813)
#elif XDJ_HALFBAND_TAPS == 15
#define XDJ_HALFBAND_TERMS(T) T(1, 663513846) T(3, -172063906) T(5, 58656197) T(7, -13235225)
#else
#error "XDJ_HALFBAND_TAPS must be 47, 31, 23 or 15"
#endif
/* Frames a seek decodes ahead of its target so the history is that of straight playing (23 are needed). */
#define XDJ_DECIMATOR_WARMUP 32U

/*
 * Halve the rate of the frames at work + history. The history goes in
 * front of them, each output frame is the filter over the input frames
 * ending one past its own pair, and the last frames become the new history.
 * The output replaces the input: frame n is written before the frames from
 * 2n + 1 on, which are all that later outputs read.
 */
static void xdj_halve(drflac_int32* history, drflac_int32* work, drflac_uint32 frames)
{
    drflac_uint32 index;

    for (index = 0; index < XDJ_DECIMATOR_HISTORY * 2U; ++index) {
        work[index] = history[index];
    }
    for (index = 0; index < frames / 2U; ++index) {
        const drflac_int32* centre = work + (2U * index + XDJ_DECIMATOR_HISTORY / 2U + 1U) * 2U;
        int channel;

        for (channel = 0; channel < 2; ++channel, ++centre) {
            /*
             * The sum is kept in 1/32 of a 24-bit step: half the centre
             * sample, plus one half for rounding and one half step per term,
             * because each term rounds down.
             */
            drflac_int32 sum = centre[0] * 16 + 16 + (XDJ_HALFBAND_TAPS + 1) / 16;

/* A pair sum has 25 bits; times 64 it fills a word, and the high word of its product with a Q31 coefficient is the term. */
#define XDJ_HALFBAND_TERM(distance, coefficient) \
    sum += (drflac_int32)(((drflac_int64)(coefficient) * \
                           ((centre[-(distance) * 2] + centre[(distance) * 2]) * 64)) >> 32);
            XDJ_HALFBAND_TERMS(XDJ_HALFBAND_TERM)
#undef XDJ_HALFBAND_TERM
            sum >>= 5;
            if (sum > 8388607) {
                sum = 8388607;
            } else if (sum < -8388608) {
                sum = -8388608;
            }
            work[index * 2U + (drflac_uint32)channel] = sum;
        }
    }
    for (index = 0; index < XDJ_DECIMATOR_HISTORY * 2U; ++index) {
        history[index] = work[frames * 2U + index];
    }
}

/* Store converted frames (24-bit values at the start of the work buffer) as 16-bit or packed 24-bit. */
static unsigned char* xdj_pack_halved(drflac_uint32 frames, unsigned char* output, drflac_uint8 bytes_per_frame)
{
    drflac_uint32 index;

    for (index = 0; index < frames * 2U; ++index) {
        drflac_uint32 sample = (drflac_uint32)xdj_pcm_work[index];

        if (bytes_per_frame == 6U) {
            *output++ = (unsigned char)sample;
        }
        *output++ = (unsigned char)(sample >> 8);
        *output++ = (unsigned char)(sample >> 16);
    }
    return output;
}

void xdj_flac_decimator_reset(xdj_flac_decimator* decimator)
{
    DRFLAC_ZERO_MEMORY(decimator, sizeof(*decimator));
}

drflac_uint64 xdj_flac_read_decimated(
    drflac* decoder,
    xdj_flac_decimator* decimator,
    drflac_uint64 frame_count,
    unsigned char* output,
    drflac_uint8 bytes_per_frame
)
{
    drflac_uint64 total = 0;

    while (total < frame_count) {
        drflac_uint32 chunk = XDJ_S24_SCRATCH_FRAMES >> decimator->shift;
        drflac_uint32 source;
        drflac_uint64 frames;
        drflac_uint32 index;

        if (frame_count - total < chunk) {
            chunk = (drflac_uint32)(frame_count - total);
        }
        source = chunk << decimator->shift;
        frames = XDJ_READ_S32(decoder, source, xdj_s24_scratch);
        xdj_spread_mono_s32(decoder, xdj_s24_scratch, frames);
        if (frames != 0U && !xdj_position_consistent(decoder)) {
            xdj_flac_sequence_breaks++;
        }
        if (frames != source) {
            /* The history no longer matches the decoder's position: the caller seeks again. */
            break;
        }
        for (index = 0; index < source * 2U; ++index) {
            xdj_s24_scratch[index] >>= 8;
        }
        xdj_halve(decimator->history, xdj_pcm_work, source);
        if (output != NULL) {
#ifdef XDJ_CHIP_HIRES
            if (bytes_per_frame == 0U) {
                /* For the audio processor's converter (src/chip_flac.c): the 24-bit values as words. */
                for (index = 0; index < chunk * 2U; ++index) {
                    ((drflac_int32*)(void*)output)[index] = xdj_pcm_work[index];
                }
                output += chunk * 8U;
            } else
#endif
            output = xdj_pack_halved(chunk, output, bytes_per_frame);
        }
        total += chunk;
    }
    return total;
}

#ifdef XDJ_CHIP_DIRECT
/*
 * For the audio processor's converter, which writes floating point
 * (src/chip_flac.c): read straight into its two channels, each sample
 * times scale. The decoder's frames pass through a small buffer that stays in
 * the cache, not through a large one in the slow memory.
 * With XDJ_TOUCH_WRITES each line of the output is read before it is
 * written, so that the writes find it in the cache.
 */
#define XDJ_DIRECT_FRAMES 128U
static drflac_int32 xdj_direct_work[XDJ_DIRECT_FRAMES * 2U];

#ifdef XDJ_TOUCH_WRITES
#define XDJ_TOUCH(pointer) \
    do { \
        if (((size_t)(pointer) & 63U) == 0U) { \
            (void)*(volatile const float*)(pointer); \
        } \
    } while (0)
#else
#define XDJ_TOUCH(pointer) ((void)0)
#endif

drflac_uint64 xdj_flac_read_float(drflac* decoder, drflac_uint64 frame_count, float* left, float* right, float scale)
{
    drflac_uint64 total = 0;

    while (total < frame_count) {
        drflac_uint32 chunk = frame_count - total < XDJ_DIRECT_FRAMES ? (drflac_uint32)(frame_count - total)
                                                                        : XDJ_DIRECT_FRAMES;
        drflac_uint32 frames = (drflac_uint32)xdj_flac_read_s32(decoder, chunk, xdj_direct_work);
        drflac_uint32 index;

        for (index = 0; index < frames; ++index) {
            XDJ_TOUCH(left + total + index);
            XDJ_TOUCH(right + total + index);
            left[total + index] = (float)xdj_direct_work[index * 2U] * scale;
            right[total + index] = (float)xdj_direct_work[index * 2U + 1U] * scale;
        }
        total += frames;
        if (frames < chunk) {
            break;
        }
    }
    return total;
}

/* As xdj_flac_read_decimated, the halved frames written straight into the two channels. */
drflac_uint64 xdj_flac_read_decimated_float(
    drflac* decoder,
    xdj_flac_decimator* decimator,
    drflac_uint64 frame_count,
    float* left,
    float* right,
    float scale
)
{
    drflac_uint64 total = 0;

    while (total < frame_count) {
        drflac_uint32 chunk = XDJ_S24_SCRATCH_FRAMES >> decimator->shift;
        drflac_uint32 source;
        drflac_uint64 frames;
        drflac_uint32 index;

        if (frame_count - total < chunk) {
            chunk = (drflac_uint32)(frame_count - total);
        }
        source = chunk << decimator->shift;
        frames = XDJ_READ_S32(decoder, source, xdj_s24_scratch);
        xdj_spread_mono_s32(decoder, xdj_s24_scratch, frames);
        if (frames != 0U && !xdj_position_consistent(decoder)) {
            xdj_flac_sequence_breaks++;
        }
        if (frames != source) {
            break;
        }
        for (index = 0; index < source * 2U; ++index) {
            xdj_s24_scratch[index] >>= 8;
        }
        xdj_halve(decimator->history, xdj_pcm_work, source);
        for (index = 0; index < chunk; ++index) {
            XDJ_TOUCH(left + total + index);
            XDJ_TOUCH(right + total + index);
            left[total + index] = (float)xdj_pcm_work[index * 2U] * scale;
            right[total + index] = (float)xdj_pcm_work[index * 2U + 1U] * scale;
        }
        total += chunk;
    }
    return total;
}
#endif

drflac_bool32 xdj_flac_seek_decimated(
    drflac* decoder,
    drflac_uint32 frame_index,
    xdj_flac_seek_state* state,
    xdj_flac_decimator* decimator
)
{
    drflac_uint32 warmup = frame_index < XDJ_DECIMATOR_WARMUP ? frame_index : XDJ_DECIMATOR_WARMUP;

    if (!xdj_flac_seek(decoder, (drflac_uint64)(frame_index - warmup) << decimator->shift, state)) {
        return DRFLAC_FALSE;
    }
    /* From silence, like the start of the track; the warm-up outlasts the filter. */
    DRFLAC_ZERO_MEMORY(decimator->history, sizeof(decimator->history));
    return xdj_flac_read_decimated(decoder, decimator, warmup, NULL, 0) == warmup;
}

/*
 * WAV and AIFF sources (xdj_flac_seek.h).
 */
static drflac_uint32 xdj_get_u32(const unsigned char* p, drflac_bool32 big_endian)
{
    return big_endian ? ((drflac_uint32)p[0] << 24) | ((drflac_uint32)p[1] << 16) | ((drflac_uint32)p[2] << 8) | p[3]
                      : ((drflac_uint32)p[3] << 24) | ((drflac_uint32)p[2] << 16) | ((drflac_uint32)p[1] << 8) | p[0];
}

static drflac_uint32 xdj_get_u16(const unsigned char* p, drflac_bool32 big_endian)
{
    return big_endian ? ((drflac_uint32)p[0] << 8) | p[1] : ((drflac_uint32)p[1] << 8) | p[0];
}

static drflac_bool32 xdj_tag_is(const unsigned char* p, const char* tag)
{
    return p[0] == (unsigned char)tag[0] && p[1] == (unsigned char)tag[1] &&
           p[2] == (unsigned char)tag[2] && p[3] == (unsigned char)tag[3];
}

/* Chunks looked at before giving up: real files have a handful before the audio. */
#define XDJ_PCM_MAX_CHUNKS 32U
/* The largest chunk body the header reader needs: an extensible WAV format chunk. */
#define XDJ_PCM_HEADER_BYTES 40U

#ifdef XDJ_MONO
/*
 * Whole frames in a number of sample bytes. A frame is 2, 3, 4 or 6 bytes
 * (16 or 24 bit, mono or stereo); the player's code has no routine for a
 * division by a variable, so each size is divided on its own.
 */
static drflac_uint32 xdj_pcm_frames_in(const xdj_pcm_source* source, drflac_uint32 bytes)
{
    switch (source->frame_bytes) {
    case 2U:
        return bytes >> 1;
    case 3U:
        return bytes / 3U;
    case 4U:
        return bytes >> 2;
    default:
        return bytes / 6U;
    }
}
#endif

drflac_bool32 xdj_pcm_open(
    xdj_pcm_source* source,
    drflac_bool32 aiff,
    drflac_uint32 stream_bytes,
    drflac_uint32* sample_rate,
    drflac_uint32* frame_count,
    drflac_uint8* bits_per_sample
)
{
    unsigned char bytes[XDJ_PCM_HEADER_BYTES];
    drflac_uint32 offset = 12;
    drflac_uint32 rate = 0;
    drflac_uint32 bits = 0;
    drflac_uint32 data_bytes = 0;
    drflac_uint32 declared_frames = 0xFFFFFFFFU;
#ifdef XDJ_MONO
    drflac_uint32 channels = 0;
#endif
    drflac_uint32 chunk;
    drflac_bool32 have_format = DRFLAC_FALSE;
    drflac_bool32 have_data = DRFLAC_FALSE;
    drflac_bool32 extensible = DRFLAC_FALSE;

    source->big_endian = aiff ? 1U : 0U;
    if (!source->on_seek(source->user_data, 0, DRFLAC_SEEK_SET) ||
        source->on_read(source->user_data, bytes, 12) != 12 ||
        !xdj_tag_is(bytes, aiff ? "FORM" : "RIFF") ||
        !(xdj_tag_is(bytes + 8, aiff ? "AIFF" : "WAVE") || (aiff && xdj_tag_is(bytes + 8, "AIFC")))) {
        return DRFLAC_FALSE;
    }
    for (chunk = 0; chunk < XDJ_PCM_MAX_CHUNKS && !(have_format && have_data); ++chunk) {
        drflac_uint32 size;
        drflac_uint32 wanted;

        if (offset > 0x7FFFFFF0U || !source->on_seek(source->user_data, (int)offset, DRFLAC_SEEK_SET) ||
            source->on_read(source->user_data, bytes, 8) != 8) {
            return DRFLAC_FALSE;
        }
        size = xdj_get_u32(bytes + 4, aiff);
        if (!aiff && xdj_tag_is(bytes, "fmt ")) {
            drflac_uint32 format;

            wanted = size < XDJ_PCM_HEADER_BYTES ? size : XDJ_PCM_HEADER_BYTES;
            if (size < 16U || source->on_read(source->user_data, bytes, wanted) != wanted) {
                return DRFLAC_FALSE;
            }
            format = xdj_get_u16(bytes, DRFLAC_FALSE);
            if (format == 0xFFFEU && wanted == XDJ_PCM_HEADER_BYTES) {
                /* Extensible: the real format is the first field of the sub-format. */
                format = xdj_get_u16(bytes + 24, DRFLAC_FALSE);
                extensible = DRFLAC_TRUE;
            }
            rate = xdj_get_u32(bytes + 4, DRFLAC_FALSE);
            bits = xdj_get_u16(bytes + 14, DRFLAC_FALSE);
#ifdef XDJ_MONO
            /* Linear PCM, mono or stereo, with frames of exactly one sample per channel. */
            channels = xdj_get_u16(bytes + 2, DRFLAC_FALSE);
            if (format != 1U || (channels != 1U && channels != 2U) ||
                xdj_get_u16(bytes + 12, DRFLAC_FALSE) * 8U != bits * channels) {
                return DRFLAC_FALSE;
            }
#else
            /* Linear PCM, stereo, with frames of exactly two samples. */
            if (format != 1U || xdj_get_u16(bytes + 2, DRFLAC_FALSE) != 2U ||
                xdj_get_u16(bytes + 12, DRFLAC_FALSE) * 4U != bits) {
                return DRFLAC_FALSE;
            }
#endif
            have_format = DRFLAC_TRUE;
        } else if (aiff && xdj_tag_is(bytes, "COMM")) {
            drflac_uint32 exponent;

            wanted = size < 22U ? size : 22U;
            if (size < 18U || source->on_read(source->user_data, bytes, wanted) != wanted) {
                return DRFLAC_FALSE;
            }
            declared_frames = xdj_get_u32(bytes + 2, DRFLAC_TRUE);
            bits = xdj_get_u16(bytes + 6, DRFLAC_TRUE);
            /* The rate is an 80-bit float; the rates wanted here have exponent 16 (65536 to 131071). */
            exponent = xdj_get_u16(bytes + 8, DRFLAC_TRUE);
            rate = exponent == 16383U + 16U ? xdj_get_u32(bytes + 10, DRFLAC_TRUE) >> 15 : 0U;
#ifdef XDJ_MONO
            channels = xdj_get_u16(bytes, DRFLAC_TRUE);
            if (channels != 1U && channels != 2U) {
                return DRFLAC_FALSE;
            }
#else
            if (xdj_get_u16(bytes, DRFLAC_TRUE) != 2U) {
                return DRFLAC_FALSE;
            }
#endif
            if (wanted == 22U) {
                /* AIFC: big-endian ("NONE", "twos", "in24") or little-endian ("sowt") PCM only. */
                if (xdj_tag_is(bytes + 18, "sowt")) {
                    source->big_endian = 0;
                } else if (!xdj_tag_is(bytes + 18, "NONE") && !xdj_tag_is(bytes + 18, "twos") &&
                           !xdj_tag_is(bytes + 18, "in24")) {
                    return DRFLAC_FALSE;
                }
            }
            have_format = DRFLAC_TRUE;
        } else if (!aiff && xdj_tag_is(bytes, "data")) {
            source->data_offset = offset + 8U;
            data_bytes = size;
            have_data = DRFLAC_TRUE;
        } else if (aiff && xdj_tag_is(bytes, "SSND")) {
            drflac_uint32 skip;

            if (size < 8U || source->on_read(source->user_data, bytes, 8) != 8) {
                return DRFLAC_FALSE;
            }
            skip = xdj_get_u32(bytes, DRFLAC_TRUE);
            if (skip > size - 8U) {
                return DRFLAC_FALSE;
            }
            source->data_offset = offset + 16U + skip;
            data_bytes = size - 8U - skip;
            have_data = DRFLAC_TRUE;
        }
        /* Chunks are padded to an even length. */
        if (size > 0x7FFFFFF0U) {
            break;
        }
        offset += 8U + size + (size & 1U);
    }
    if (!have_format || !have_data || (bits != 16U && bits != 24U)) {
        return DRFLAC_FALSE;
    }
    /* Ours: the rates the bridge converts, and extensible headers at the rates it passes through. */
    if (rate != 88200U && rate != 96000U && !(extensible && (rate == 44100U || rate == 48000U))) {
        return DRFLAC_FALSE;
    }
    /* A data length that runs past the end of the file (or is unset) means "to the end". */
    if (stream_bytes > source->data_offset && data_bytes > stream_bytes - source->data_offset) {
        data_bytes = stream_bytes - source->data_offset;
    }
#ifdef XDJ_MONO
    source->channels = (drflac_uint8)channels;
    source->frame_bytes = (drflac_uint8)((bits >> 3) * channels);
    source->frames = xdj_pcm_frames_in(source, data_bytes);
#else
    source->frame_bytes = (drflac_uint8)(bits >> 2);
    source->frames = bits == 24U ? data_bytes / 6U : data_bytes >> 2;
#endif
    if (source->frames > declared_frames) {
        source->frames = declared_frames;
    }
    *sample_rate = rate;
    *frame_count = source->frames;
    *bits_per_sample = (drflac_uint8)bits;
    return DRFLAC_TRUE;
}

/* Read whole frames of the file's sample bytes; returns the frames read. */
static drflac_uint32 xdj_pcm_read_raw(xdj_pcm_source* source, drflac_uint32 frames, unsigned char* output)
{
    drflac_uint32 bytes = frames * source->frame_bytes;
    drflac_uint32 got = (drflac_uint32)source->on_read(source->user_data, output, bytes);

#ifdef XDJ_MONO
    return got == bytes ? frames : xdj_pcm_frames_in(source, got);
#else
    return got == bytes ? frames : (source->frame_bytes == 6U ? got / 6U : got >> 2);
#endif
}

/*
 * Sample bytes are read from the file in blocks of up to this many source
 * frames, so a player read costs a few filesystem reads, not one per
 * conversion step. The buffer is only used inside one xdj_pcm_read call,
 * under the decoder lock (or by the parser task's header probe, which reads
 * no samples).
 */
#define XDJ_PCM_RAW_FRAMES 2048U
static unsigned char xdj_pcm_raw[XDJ_PCM_RAW_FRAMES * 6U];

drflac_uint64 xdj_pcm_read(
    xdj_pcm_source* source,
    xdj_flac_decimator* decimator,
    drflac_uint64 frame_count,
    unsigned char* output,
    drflac_uint8 bytes_per_frame
)
{
    drflac_uint64 total = 0;
#ifdef XDJ_MONO
    drflac_uint32 sample_bytes = source->channels == 2U ? (drflac_uint32)source->frame_bytes >> 1 : source->frame_bytes;
#else
    drflac_uint32 sample_bytes = (drflac_uint32)source->frame_bytes >> 1;
#endif

    if (decimator->shift == 0U) {
        /*
         * Not converted: only extensible WAV files get here (xdj_pcm_open),
         * and their little-endian samples are the output as they stand; with
         * XDJ_MONO, mono ones go out with each sample on both sides.
         */
#ifdef XDJ_MONO
        if (source->channels == 2U) {
            return xdj_pcm_read_raw(source, (drflac_uint32)frame_count, output);
        }
        while (total < frame_count) {
            drflac_uint32 block = XDJ_PCM_RAW_FRAMES;
            drflac_uint32 got;
            drflac_uint32 index;

            if (frame_count - total < block) {
                block = (drflac_uint32)(frame_count - total);
            }
            got = xdj_pcm_read_raw(source, block, xdj_pcm_raw);
            for (index = 0; index < got * sample_bytes; index += sample_bytes) {
                drflac_uint32 byte;

                for (byte = 0; byte < sample_bytes; ++byte) {
                    output[byte] = xdj_pcm_raw[index + byte];
                    output[sample_bytes + byte] = xdj_pcm_raw[index + byte];
                }
                output += 2U * sample_bytes;
            }
            total += got;
            if (got != block) {
                break;
            }
        }
        return total;
#else
        return xdj_pcm_read_raw(source, (drflac_uint32)frame_count, output);
#endif
    }
    while (total < frame_count) {
        drflac_uint32 block = XDJ_PCM_RAW_FRAMES >> 1;
        drflac_uint32 done = 0;

        if (frame_count - total < block) {
            block = (drflac_uint32)(frame_count - total);
        }
        if (xdj_pcm_read_raw(source, block << 1, xdj_pcm_raw) != block << 1) {
            /* As with FLAC: the caller seeks again before the next read. */
            break;
        }
        while (done < block) {
            drflac_uint32 chunk = XDJ_S24_SCRATCH_FRAMES >> 1;
            const unsigned char* p = xdj_pcm_raw + (done << 1) * source->frame_bytes;
            drflac_uint32 index;

            if (block - done < chunk) {
                chunk = block - done;
            }
#ifdef XDJ_MONO
            /*
             * The file's samples as 24-bit values, sign-extended; a mono sample
             * goes to both sides (the pointer moves on after the right one).
             */
            for (index = 0; index < chunk * 4U; ++index, p += (source->channels == 2U || (index & 1U) == 0U) ? sample_bytes : 0U) {
#else
            /* The file's samples as 24-bit values, sign-extended. */
            for (index = 0; index < chunk * 4U; ++index, p += sample_bytes) {
#endif
                drflac_uint32 value;

                if (sample_bytes == 3U) {
                    value = source->big_endian ? ((drflac_uint32)p[0] << 16) | ((drflac_uint32)p[1] << 8) | p[2]
                                               : ((drflac_uint32)p[2] << 16) | ((drflac_uint32)p[1] << 8) | p[0];
                } else {
                    value = (source->big_endian ? ((drflac_uint32)p[0] << 8) | p[1]
                                                : ((drflac_uint32)p[1] << 8) | p[0]) << 8;
                }
                xdj_s24_scratch[index] = (drflac_int32)(value << 8) >> 8;
            }
            xdj_halve(decimator->history, xdj_pcm_work, chunk << 1);
            if (output != NULL) {
                output = xdj_pack_halved(chunk, output, bytes_per_frame);
            }
            done += chunk;
        }
        total += block;
    }
    return total;
}

drflac_bool32 xdj_pcm_seek(xdj_pcm_source* source, drflac_uint32 frame_index, xdj_flac_decimator* decimator)
{
    drflac_uint32 warmup = 0;
    drflac_uint32 byte;

    if (decimator->shift != 0U) {
        warmup = frame_index < XDJ_DECIMATOR_WARMUP ? frame_index : XDJ_DECIMATOR_WARMUP;
    }
    byte = ((frame_index - warmup) << decimator->shift) * source->frame_bytes;
    if (byte > 0x7FFFFFFFU - source->data_offset ||
        !source->on_seek(source->user_data, (int)(source->data_offset + byte), DRFLAC_SEEK_SET)) {
        return DRFLAC_FALSE;
    }
    if (decimator->shift == 0U) {
        return DRFLAC_TRUE;
    }
    /* From silence, like the start of the track; the warm-up outlasts the filter. */
    DRFLAC_ZERO_MEMORY(decimator->history, sizeof(decimator->history));
    return xdj_pcm_read(source, decimator, warmup, NULL, 0) == warmup;
}

#ifndef DR_FLAC_NO_CRC
/*
 * (Left out of a build without dr_flac's CRC-16, which it checks frames with:
 * only the decoder timing test on the audio processor is built so, and that
 * decoder never seeks.)
 *
 * Bounded seek.
 *
 * dr_flac's own seek can fall back to decoding forward from a seek point, up
 * to a whole seek-table interval (about 10 s of audio, 2 MB of 24-bit FLAC),
 * which froze the player on hot cue bank pads. This seek finds the frame that
 * holds the target by reading frame headers only, then decodes that one frame.
 * The frame's CRC-16, checked while decoding it, validates the search.
 *
 * The search keeps two bounds: a frame known to start at or before the target
 * (lo) and a byte offset before which the target's frame must start (hi). It
 * narrows them with the seek table and with frames cached by earlier seeks,
 * then probes: it seeks to a byte between the bounds and reads the first
 * frame header after it. Probes alternate between interpolation and bisection
 * so the range at least halves every other probe.
 *
 * Offsets and frame numbers are 32-bit: FAT32 files stay below 4 GB and the
 * adapter rejects streams of 2^29 frames or more. 64-bit values are split
 * into register pairs on the SH-4, which cost stack on the input-file task.
 */
#define XDJ_SEEK_MAX_PROBES 40U
#define XDJ_SEEKPOINT_PLACEHOLDER 0xFFFFFFFFFFFFFFFFULL
#ifdef XDJ_HOST_TEST
/* Host tests set this to exercise the last-resort path after every search. */
int xdj_seek_force_fallback = 0;
#else
#define xdj_seek_force_fallback 0
#endif
#ifdef XDJ_SEEK_DEBUG
#include <stdio.h>
#define XDJ_TRACE(...) fprintf(stderr, __VA_ARGS__)
#else
#define XDJ_TRACE(...) ((void)0)
#endif

typedef struct {
    drflac_uint32 offset;
    drflac_uint32 first;
    drflac_uint32 frames;
    drflac_uint8 variable;
} xdj_frame_position;

/* Scratch for one header read: the parsed header and where its frame lies. */
typedef struct {
    drflac_frame_header header;
    xdj_frame_position position;
} xdj_found_frame;

void xdj_flac_seek_reset(xdj_flac_seek_state* state, drflac_uint32 stream_bytes)
{
    DRFLAC_ZERO_MEMORY(state, sizeof(*state));
    state->stream_bytes = stream_bytes;
}

/* Byte offset of the next unread byte: what the stream delivered, less what the caches still hold. */
static drflac_bool32 xdj_bs_tell_byte(drflac_bs* bs, drflac_uint32* offset)
{
    drflac_int64 cursor;
    drflac_uint32 unread;

    if (bs->onTell == NULL || !bs->onTell(bs->pUserData, &cursor) || cursor < 0 || cursor > 0xFFFFFFFFLL) {
        return DRFLAC_FALSE;
    }
    /* Only called on byte boundaries, so the L1 cache holds whole bytes. */
    unread = (drflac_uint32)DRFLAC_CACHE_L2_LINES_REMAINING(bs) * (drflac_uint32)DRFLAC_CACHE_L1_SIZE_BYTES(bs) +
             (drflac_uint32)bs->unalignedByteCount +
             (drflac_uint32)(DRFLAC_CACHE_L1_BITS_REMAINING(bs) >> 3);
    if (unread > (drflac_uint32)cursor) {
        return DRFLAC_FALSE;
    }
    *offset = (drflac_uint32)cursor - unread;
    return DRFLAC_TRUE;
}

/*
 * Find the next frame sync code: 0xFF, then a byte of the form 111110xy.
 * dr_flac's own search consumes the byte after any 0xFF, so it misses a sync
 * that directly follows another 0xFF (for example when the previous frame's
 * CRC-16 ends in 0xFF); it never meets that case because it reads frames back
 * to back, but a probe starts at an arbitrary byte. Returns the second sync
 * byte, with the frame's CRC-16 started at the first one, as dr_flac does.
 */
static drflac_bool32 xdj_find_sync(drflac_bs* bs, drflac_uint8* second)
{
    drflac_uint8 byte;

    if (!drflac__seek_bits(bs, DRFLAC_CACHE_L1_BITS_REMAINING(bs) & 7) || !drflac__read_uint8(bs, 8, &byte)) {
        return DRFLAC_FALSE;
    }
    for (;;) {
        if (byte != 0xFF) {
            if (!drflac__read_uint8(bs, 8, &byte)) {
                return DRFLAC_FALSE;
            }
            continue;
        }
        drflac__reset_crc16(bs);
        if (!drflac__read_uint8(bs, 8, &byte)) {
            return DRFLAC_FALSE;
        }
        if ((byte & 0xFC) == 0xF8) {
            bs->crc16 = drflac_crc16_byte(0, 0xFF);
            *second = byte;
            return DRFLAC_TRUE;
        }
    }
}

/*
 * Read the rest of a frame header after its two sync bytes, checking every
 * field against STREAMINFO. strategy is the stream's blocking strategy (0
 * fixed, 1 variable) or -1 when not yet known.
 */
static drflac_bool32 xdj_parse_frame_header(drflac* pFlac, drflac_uint8 second, int strategy, xdj_found_frame* found)
{
    static const drflac_uint32 sample_rates[12] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    static const drflac_uint8 sample_sizes[8] = {0, 8, 12, 0, 16, 20, 24, 0};
    drflac_bs* bs = &pFlac->bs;
    drflac_frame_header* header = &found->header;
    drflac_uint8 crc8 = 0xCE;
    drflac_uint8 blocking = (drflac_uint8)(second & 1U);
    drflac_uint8 block_code;
    drflac_uint8 rate_code;
    drflac_uint8 assignment;
    drflac_uint8 size_code;
    drflac_uint8 reserved;
    drflac_uint8 expected_crc8;
    drflac_uint64 number;
    drflac_uint64 first;
    drflac_uint32 rate;
    drflac_uint32 frames;

    /* The reserved bit (second & 2) must be clear. */
    if ((second & 2U) != 0 || (strategy >= 0 && blocking != (drflac_uint8)strategy)) {
        return DRFLAC_FALSE;
    }
    crc8 = drflac_crc8(crc8, 0, 1);
    crc8 = drflac_crc8(crc8, blocking, 1);
    if (!drflac__read_uint8(bs, 4, &block_code) || block_code == 0) {
        return DRFLAC_FALSE;
    }
    crc8 = drflac_crc8(crc8, block_code, 4);
    if (!drflac__read_uint8(bs, 4, &rate_code) || rate_code == 15) {
        return DRFLAC_FALSE;
    }
    crc8 = drflac_crc8(crc8, rate_code, 4);
    if (!drflac__read_uint8(bs, 4, &assignment) || assignment > 10 ||
        drflac__get_channel_count_from_channel_assignment(assignment) != (int)pFlac->channels) {
        return DRFLAC_FALSE;
    }
    crc8 = drflac_crc8(crc8, assignment, 4);
    if (!drflac__read_uint8(bs, 3, &size_code) ||
        (size_code != 0 && sample_sizes[size_code] != pFlac->bitsPerSample)) {
        return DRFLAC_FALSE;
    }
    crc8 = drflac_crc8(crc8, size_code, 3);
    if (!drflac__read_uint8(bs, 1, &reserved) || reserved != 0) {
        return DRFLAC_FALSE;
    }
    crc8 = drflac_crc8(crc8, reserved, 1);
    if (drflac__read_utf8_coded_number(bs, &number, &crc8) != DRFLAC_SUCCESS) {
        return DRFLAC_FALSE;
    }
    if (block_code == 1) {
        frames = 192;
    } else if (block_code <= 5) {
        frames = 576U << (block_code - 2);
    } else if (block_code <= 7) {
        drflac_uint16 value;
        unsigned int bits = block_code == 6 ? 8U : 16U;
        if (!drflac__read_uint16(bs, bits, &value)) {
            return DRFLAC_FALSE;
        }
        crc8 = drflac_crc8(crc8, value, bits);
        frames = (drflac_uint32)value + 1U;
    } else {
        frames = 256U << (block_code - 8);
    }
    if (rate_code <= 11) {
        rate = sample_rates[rate_code];
    } else {
        unsigned int bits = rate_code == 12 ? 8U : 16U;
        if (!drflac__read_uint32(bs, bits, &rate)) {
            return DRFLAC_FALSE;
        }
        crc8 = drflac_crc8(crc8, rate, bits);
        rate *= rate_code == 12 ? 1000U : rate_code == 14 ? 10U : 1U;
    }
    if ((rate != 0 && rate != pFlac->sampleRate) ||
        !drflac__read_uint8(bs, 8, &expected_crc8) || expected_crc8 != crc8 ||
        frames > pFlac->maxBlockSizeInPCMFrames) {
        return DRFLAC_FALSE;
    }
    first = blocking ? number : number * pFlac->maxBlockSizeInPCMFrames;
    if (first >= pFlac->totalPCMFrameCount || first + frames > pFlac->totalPCMFrameCount) {
        return DRFLAC_FALSE;
    }
    /* With fixed blocking, only the last frame may be shorter than the block size. */
    if (!blocking && frames != pFlac->maxBlockSizeInPCMFrames && first + frames != pFlac->totalPCMFrameCount) {
        return DRFLAC_FALSE;
    }
    header->pcmFrameNumber = blocking ? number : 0;
    header->flacFrameNumber = blocking ? 0 : (drflac_uint32)number;
    header->sampleRate = pFlac->sampleRate;
    header->blockSizeInPCMFrames = (drflac_uint16)frames;
    header->channelAssignment = assignment;
    header->bitsPerSample = pFlac->bitsPerSample;
    header->crc8 = crc8;
    found->position.first = (drflac_uint32)first;
    found->position.frames = frames;
    found->position.variable = blocking;
    return DRFLAC_TRUE;
}

/*
 * Find the next valid frame header at or after the current position. A sync
 * code that is not a valid header is skipped by restarting one byte after it,
 * so a real header overlapping a false one is not missed. Stops without a
 * result at the end of the data or once a sync starts at or after limit.
 */
static drflac_bool32 xdj_next_frame_header(drflac* pFlac, drflac_uint32 limit, int strategy, xdj_found_frame* found)
{
    for (;;) {
        drflac_uint32 offset;
        drflac_uint8 second;

        if (!xdj_find_sync(&pFlac->bs, &second) || !xdj_bs_tell_byte(&pFlac->bs, &offset) || offset < 2U) {
            return DRFLAC_FALSE;
        }
        found->position.offset = offset - 2U;
        if (found->position.offset >= limit) {
            return DRFLAC_FALSE;
        }
        if (xdj_parse_frame_header(pFlac, second, strategy, found)) {
            return DRFLAC_TRUE;
        }
        if (!drflac__seek_to_byte(&pFlac->bs, (drflac_uint64)found->position.offset + 1U)) {
            return DRFLAC_FALSE;
        }
    }
}

/*
 * With the stream just past a frame header, decode that frame (checking its
 * CRC-16) and move to the target inside it.
 */
static drflac_bool32 xdj_decode_frame_to(drflac* pFlac, const xdj_found_frame* found, drflac_uint32 target)
{
    drflac_uint32 skip = target - found->position.first;

    pFlac->currentFLACFrame.header = found->header;
    if (drflac__decode_flac_frame(pFlac) != DRFLAC_SUCCESS) {
        DRFLAC_ZERO_MEMORY(&pFlac->currentFLACFrame, sizeof(pFlac->currentFLACFrame));
        return DRFLAC_FALSE;
    }
    pFlac->currentPCMFrame = found->position.first;
    if (drflac__seek_forward_by_pcm_frames(pFlac, skip) != skip) {
        return DRFLAC_FALSE;
    }
    pFlac->currentPCMFrame = target;
    return DRFLAC_TRUE;
}

/*
 * Seek to a frame known to start at position and decode to the target in it.
 * found receives the frame as read from its header (length, blocking).
 */
static drflac_bool32 xdj_decode_known_frame(
    drflac* pFlac,
    int strategy,
    const xdj_frame_position* position,
    drflac_uint32 target,
    xdj_found_frame* found
)
{
    if (!drflac__seek_to_byte(&pFlac->bs, position->offset) ||
        !xdj_next_frame_header(pFlac, position->offset + 1U, strategy, found) ||
        found->position.offset != position->offset || found->position.first != position->first) {
        return DRFLAC_FALSE;
    }
    return xdj_decode_frame_to(pFlac, found, target);
}

static void xdj_remember_frame(xdj_flac_seek_state* state, const xdj_frame_position* position)
{
    xdj_seek_cache_entry* entry;
    drflac_uint32 index;

    for (index = 0; index < XDJ_SEEK_CACHE_ENTRIES; ++index) {
        if (state->entries[index].frames != 0 && state->entries[index].offset == position->offset) {
            return;
        }
    }
    entry = &state->entries[state->next_entry];
    state->next_entry = (state->next_entry + 1U) % XDJ_SEEK_CACHE_ENTRIES;
    entry->offset = position->offset;
    entry->first = position->first;
    entry->frames = position->frames;
}

/* The target is already in the decoded frame, or a few frames ahead of it. */
static drflac_bool32 xdj_seek_nearby(drflac* pFlac, drflac_uint32 target)
{
    drflac_uint32 current = (drflac_uint32)pFlac->currentPCMFrame;

    if (!drflac__is_current_flac_frame_valid(pFlac)) {
        return DRFLAC_FALSE;
    }
    if (target > current) {
        drflac_uint32 ahead = target - current;

        if (pFlac->currentFLACFrame.pcmFramesRemaining > ahead) {
            pFlac->currentFLACFrame.pcmFramesRemaining -= ahead;
            pFlac->currentPCMFrame = target;
            return DRFLAC_TRUE;
        }
        /* Decoding up to two more frames is cheaper than a search. */
        if (ahead <= (drflac_uint32)pFlac->maxBlockSizeInPCMFrames * 2U) {
            if (drflac__seek_forward_by_pcm_frames(pFlac, ahead) == ahead) {
                pFlac->currentPCMFrame = target;
                return DRFLAC_TRUE;
            }
            return DRFLAC_FALSE;
        }
    } else {
        drflac_uint32 back = current - target;
        drflac_uint32 consumed = pFlac->currentFLACFrame.header.blockSizeInPCMFrames - pFlac->currentFLACFrame.pcmFramesRemaining;

        if (consumed > back) {
            pFlac->currentFLACFrame.pcmFramesRemaining += back;
            pFlac->currentPCMFrame = target;
            return DRFLAC_TRUE;
        }
    }
    return DRFLAC_FALSE;
}

/* Blocking strategy for header checks: 0 fixed, 1 variable, -1 not known yet. */
static int xdj_known_strategy(const drflac* pFlac, const xdj_flac_seek_state* state)
{
    if (state->blocking != 0) {
        return state->blocking == 2 ? 1 : 0;
    }
    if (pFlac->currentFLACFrame.header.pcmFrameNumber != 0) {
        return 1;
    }
    if (pFlac->currentFLACFrame.header.flacFrameNumber != 0) {
        return 0;
    }
    return -1;
}

typedef enum {
    XDJ_SCAN_FOUND,
    XDJ_SCAN_NONE,
    XDJ_SCAN_FAILED
} xdj_scan_result;

/*
 * Probes read raw bytes into this buffer and look for frame headers there,
 * so a false sync costs nothing: going back one byte through dr_flac's
 * bitstream would mean a backward filesystem seek and a re-read, and on the
 * player's FAT32 a backward seek can walk the cluster chain from the start of
 * the file. Seeks run under the decoder lock, so one buffer serves all slots.
 */
#define XDJ_SCAN_BYTES 4096U
#define XDJ_HEADER_MAX_BYTES 16U
/* Two aligned blocks: reads always fill a whole, aligned 4 KB block, as dr_flac's do. */
static drflac_uint32 xdj_scan_words[2U * XDJ_SCAN_BYTES / 4U];

/*
 * A frame header from raw bytes starting at its 0xFF sync byte, with every
 * field checked against STREAMINFO, as xdj_parse_frame_header does.
 */
static drflac_bool32 xdj_parse_header_bytes(
    const drflac* pFlac,
    const drflac_uint8* p,
    drflac_uint32 available,
    int strategy,
    xdj_frame_position* position
)
{
    static const drflac_uint8 sample_sizes[8] = {0, 8, 12, 0, 16, 20, 24, 0};
    static const drflac_uint32 sample_rates[12] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    drflac_uint8 blocking = (drflac_uint8)(p[1] & 1U);
    drflac_uint8 block_code = (drflac_uint8)(p[2] >> 4);
    drflac_uint8 rate_code = (drflac_uint8)(p[2] & 15U);
    drflac_uint8 assignment = (drflac_uint8)(p[3] >> 4);
    drflac_uint8 size_code = (drflac_uint8)((p[3] >> 1) & 7U);
    drflac_uint8 crc8 = 0;
    drflac_uint32 length;
    drflac_uint32 frames;
    drflac_uint32 rate;
    drflac_uint32 index;
    drflac_uint64 number;
    drflac_uint64 first;

    if (available < 6U || (p[1] & 2U) != 0 || (p[3] & 1U) != 0 || block_code == 0 || rate_code == 15 ||
        assignment > 10 || (strategy >= 0 && blocking != (drflac_uint8)strategy) ||
        drflac__get_channel_count_from_channel_assignment(assignment) != (int)pFlac->channels ||
        (size_code != 0 && sample_sizes[size_code] != pFlac->bitsPerSample)) {
        return DRFLAC_FALSE;
    }
    /* The UTF-8-style coded frame or sample number. */
    number = p[4];
    length = 5;
    if (number >= 0x80U) {
        drflac_uint32 bytes = 0;
        drflac_uint8 lead = p[4];

        while ((lead & 0x80U) != 0) {
            bytes++;
            lead = (drflac_uint8)(lead << 1);
        }
        if (bytes < 2U || bytes > 7U || available < 4U + bytes + 1U) {
            return DRFLAC_FALSE;
        }
        number = p[4] & ((1U << (7U - bytes)) - 1U);
        for (index = 1; index < bytes; ++index) {
            if ((p[4 + index] & 0xC0U) != 0x80U) {
                return DRFLAC_FALSE;
            }
            number = (number << 6) | (p[4 + index] & 0x3FU);
        }
        length = 4U + bytes;
    }
    if (block_code == 1) {
        frames = 192;
    } else if (block_code <= 5) {
        frames = 576U << (block_code - 2);
    } else if (block_code == 6) {
        if (available < length + 2U) {
            return DRFLAC_FALSE;
        }
        frames = (drflac_uint32)p[length] + 1U;
        length += 1U;
    } else if (block_code == 7) {
        if (available < length + 3U) {
            return DRFLAC_FALSE;
        }
        frames = ((drflac_uint32)p[length] << 8 | p[length + 1U]) + 1U;
        length += 2U;
    } else {
        frames = 256U << (block_code - 8);
    }
    if (rate_code <= 11) {
        rate = sample_rates[rate_code];
    } else if (rate_code == 12) {
        if (available < length + 2U) {
            return DRFLAC_FALSE;
        }
        rate = (drflac_uint32)p[length] * 1000U;
        length += 1U;
    } else {
        if (available < length + 3U) {
            return DRFLAC_FALSE;
        }
        rate = (drflac_uint32)p[length] << 8 | p[length + 1U];
        if (rate_code == 14) {
            rate *= 10U;
        }
        length += 2U;
    }
    if (available < length + 1U) {
        return DRFLAC_FALSE;
    }
    for (index = 0; index < length; ++index) {
        crc8 = drflac_crc8_byte(crc8, p[index]);
    }
    if (crc8 != p[length] || (rate != 0 && rate != pFlac->sampleRate) || frames > pFlac->maxBlockSizeInPCMFrames) {
        return DRFLAC_FALSE;
    }
    first = blocking ? number : number * pFlac->maxBlockSizeInPCMFrames;
    if (first >= pFlac->totalPCMFrameCount || first + frames > pFlac->totalPCMFrameCount ||
        (!blocking && frames != pFlac->maxBlockSizeInPCMFrames && first + frames != pFlac->totalPCMFrameCount)) {
        return DRFLAC_FALSE;
    }
    position->first = (drflac_uint32)first;
    position->frames = frames;
    position->variable = blocking;
    return DRFLAC_TRUE;
}

/*
 * From a byte offset, find the first header whose frame lies strictly between
 * the bounds' frames, reading forward only. Headers that fail their checks or
 * are out of order (false syncs that passed by chance) are skipped in place.
 */
static xdj_scan_result xdj_scan_frames(
    drflac* pFlac,
    xdj_flac_seek_state* state,
    drflac_uint32 from,
    drflac_uint32 limit,
    drflac_uint32 lo_first,
    drflac_uint32 hi_first,
    int strategy,
    xdj_found_frame* found
)
{
    drflac_bs* bs = &pFlac->bs;
    drflac_uint8* bytes = (drflac_uint8*)xdj_scan_words;
    drflac_uint32 base = from;
    drflac_uint32 length = 0;
    drflac_uint32 index = 0;
    drflac_bool32 at_end = DRFLAC_FALSE;

    if (state->probes >= XDJ_SEEK_MAX_PROBES) {
        state->failure = XDJ_SEEK_FAIL_PROBES;
        return XDJ_SCAN_FAILED;
    }
    state->probes++;
    if (from > 0x7FFFFFFFU || !bs->onSeek(bs->pUserData, (int)from, DRFLAC_SEEK_SET)) {
        state->failure = XDJ_SEEK_FAIL_IO;
        return XDJ_SCAN_FAILED;
    }
    for (;;) {
        /*
         * Keep a whole header's worth of bytes ahead. Blocks are read whole
         * into the second half; once the scan is in it, it moves to the first.
         */
        if (length - index < XDJ_HEADER_MAX_BYTES && !at_end) {
            drflac_uint32 got;

            if (length == 2U * XDJ_SCAN_BYTES) {
                drflac_uint32 word;

                for (word = 0; word < XDJ_SCAN_BYTES / 4U; ++word) {
                    xdj_scan_words[word] = xdj_scan_words[word + XDJ_SCAN_BYTES / 4U];
                }
                base += XDJ_SCAN_BYTES;
                index -= XDJ_SCAN_BYTES;
                length = XDJ_SCAN_BYTES;
            }
            got = (drflac_uint32)bs->onRead(bs->pUserData, bytes + length, XDJ_SCAN_BYTES);
            if (got < XDJ_SCAN_BYTES) {
                at_end = DRFLAC_TRUE;
            }
            length += got;
        }
        if (length - index < 2U || base + index >= limit) {
            return XDJ_SCAN_NONE;
        }
        if (bytes[index] == 0xFF && (bytes[index + 1U] & 0xFCU) == 0xF8U &&
            xdj_parse_header_bytes(pFlac, bytes + index, length - index, strategy, &found->position) &&
            found->position.first > lo_first && found->position.first < hi_first) {
            found->position.offset = base + index;
            return XDJ_SCAN_FOUND;
        }
        index++;
    }
}

/*
 * Last resort when the search cannot finish: decode forward from the closest
 * trusted frame (a seek point, a verified cached frame, or the first frame),
 * never from a probed header. With a seek table this costs at most one
 * interval, like dr_flac's own seek at its worst.
 */
static drflac_bool32 xdj_seek_from_anchor(drflac* pFlac, const xdj_frame_position* anchor, drflac_uint32 target)
{
    drflac_uint32 skip = target - anchor->first;

    if (!drflac__seek_to_byte(&pFlac->bs, anchor->offset)) {
        return DRFLAC_FALSE;
    }
    DRFLAC_ZERO_MEMORY(&pFlac->currentFLACFrame, sizeof(pFlac->currentFLACFrame));
    pFlac->currentPCMFrame = anchor->first;
    if (drflac__seek_forward_by_pcm_frames(pFlac, skip) != skip) {
        return DRFLAC_FALSE;
    }
    pFlac->currentPCMFrame = target;
    return DRFLAC_TRUE;
}

/* On failure, *anchor holds the closest trusted frame at or before the target. */
static drflac_bool32 xdj_seek_bounded(
    drflac* pFlac,
    drflac_uint32 target,
    xdj_flac_seek_state* state,
    xdj_frame_position* anchor
)
{
    xdj_frame_position lo;
    xdj_found_frame found;
    /* Without a known file size, reads past the end simply find no frame. */
    drflac_uint32 hi_offset = state->stream_bytes != 0U ? state->stream_bytes : 0xFFFFFFFFU;
    drflac_uint32 hi_first = (drflac_uint32)pFlac->totalPCMFrameCount;
    drflac_uint32 previous_offset = 0;
    drflac_uint32 index;
    int strategy = xdj_known_strategy(pFlac, state);

    lo.offset = (drflac_uint32)pFlac->firstFLACFramePosInBytes;
    lo.first = 0;
    lo.frames = 0;
    lo.variable = 0;
    *anchor = lo;
    if (hi_offset <= lo.offset) {
        state->failure = XDJ_SEEK_FAIL_BOUNDS;
        return DRFLAC_FALSE;
    }

    /* The seek table narrows the range; its points must rise in both frames and bytes. */
    for (index = 0; index < pFlac->seekpointCount && pFlac->pSeekpoints != NULL; ++index) {
        const drflac_seekpoint* point = &pFlac->pSeekpoints[index];
        drflac_uint64 offset = pFlac->firstFLACFramePosInBytes + point->flacFrameOffset;

        if (point->firstPCMFrame == XDJ_SEEKPOINT_PLACEHOLDER) {
            continue;
        }
        if ((previous_offset != 0 && (point->firstPCMFrame <= lo.first || offset <= previous_offset)) ||
            point->pcmFrameCount == 0 || point->pcmFrameCount > pFlac->maxBlockSizeInPCMFrames ||
            point->firstPCMFrame >= hi_first || offset >= hi_offset) {
            break;
        }
        previous_offset = (drflac_uint32)offset;
        if (point->firstPCMFrame <= target) {
            lo.offset = (drflac_uint32)offset;
            lo.first = (drflac_uint32)point->firstPCMFrame;
            lo.frames = point->pcmFrameCount;
            *anchor = lo;
        } else {
            hi_offset = (drflac_uint32)offset;
            hi_first = (drflac_uint32)point->firstPCMFrame;
            break;
        }
    }

    /* Frames verified by earlier seeks: a direct hit, or tighter bounds. */
    for (index = 0; index < XDJ_SEEK_CACHE_ENTRIES; ++index) {
        xdj_seek_cache_entry* entry = &state->entries[index];

        if (entry->frames == 0) {
            continue;
        }
        if (entry->first <= target && target - entry->first < entry->frames) {
            xdj_frame_position cached;

            cached.offset = entry->offset;
            cached.first = entry->first;
            cached.frames = entry->frames;
            cached.variable = 0;
            state->probes++;
            if (xdj_decode_known_frame(pFlac, strategy, &cached, target, &found)) {
                state->used_cache = 1;
                return DRFLAC_TRUE;
            }
            entry->frames = 0;
            continue;
        }
        if (entry->first <= target && entry->first >= lo.first && entry->offset >= lo.offset) {
            lo.offset = entry->offset;
            lo.first = entry->first;
            lo.frames = entry->frames;
            *anchor = lo;
        } else if (entry->first > target && entry->offset < hi_offset && entry->offset > lo.offset) {
            hi_offset = entry->offset;
            hi_first = entry->first;
        }
    }

    for (;;) {
        drflac_uint32 span;
        drflac_uint32 probe;
        xdj_scan_result result;

        /* The lower bound's length is unknown only for the stream's first frame. */
        if (lo.frames == 0) {
            if (state->probes >= XDJ_SEEK_MAX_PROBES) {
                state->failure = XDJ_SEEK_FAIL_PROBES;
                return DRFLAC_FALSE;
            }
            state->probes++;
            if (!drflac__seek_to_byte(&pFlac->bs, lo.offset) ||
                !xdj_next_frame_header(pFlac, lo.offset + 1U, strategy, &found) ||
                found.position.first != lo.first) {
                state->failure = XDJ_SEEK_FAIL_FIRST_FRAME;
                return DRFLAC_FALSE;
            }
            lo.frames = found.position.frames;
            strategy = found.position.variable ? 1 : 0;
        }
        if (target - lo.first < lo.frames) {
            if (state->probes >= XDJ_SEEK_MAX_PROBES) {
                state->failure = XDJ_SEEK_FAIL_PROBES;
                return DRFLAC_FALSE;
            }
            state->probes++;
            if (!xdj_decode_known_frame(pFlac, strategy, &lo, target, &found)) {
                XDJ_TRACE("  decode of lo failed\n");
                state->failure = XDJ_SEEK_FAIL_DECODE;
                return DRFLAC_FALSE;
            }
            break;
        }
        span = hi_offset - lo.offset;
        XDJ_TRACE("  lo %u@%u+%u hi %u@%u probes %u\n", lo.first, lo.offset, lo.frames, hi_first, hi_offset, state->probes);
        if (span <= 1U) {
            XDJ_TRACE("  span exhausted\n");
            state->failure = XDJ_SEEK_FAIL_SPAN;
            return DRFLAC_FALSE;
        }
        if ((state->probes & 1U) == 0U && hi_first > lo.first) {
            /* Interpolate, aiming half a frame early so the target's own frame follows the probe. */
            drflac_uint32 frames_between = hi_first - lo.first;
            drflac_uint32 guess = (drflac_uint32)((drflac_uint64)(target - lo.first) * span / frames_between);
            drflac_uint32 margin = (drflac_uint32)((drflac_uint64)pFlac->maxBlockSizeInPCMFrames * span / frames_between / 2U);

            probe = lo.offset + (guess > margin ? guess - margin : 0U);
        } else {
            probe = lo.offset + span / 2U;
        }
        if (probe <= lo.offset) {
            probe = lo.offset + 1U;
        }
        if (probe >= hi_offset) {
            probe = hi_offset - 1U;
        }

        result = xdj_scan_frames(pFlac, state, probe, hi_offset, lo.first, hi_first, strategy, &found);
        XDJ_TRACE("  probe %u -> %d first %u@%u+%u\n", probe, (int)result, found.position.first, found.position.offset, found.position.frames);
        while (result == XDJ_SCAN_FOUND && found.position.first <= target && target - found.position.first < found.position.frames) {
            xdj_frame_position frame = found.position;

            if (xdj_decode_known_frame(pFlac, strategy, &frame, target, &found)) {
                goto decoded;
            }
            found.position = frame;
            /* The frame's CRC-16 failed, so its header was false. Scan on past it. */
            if (found.position.offset + 1U >= hi_offset) {
                result = XDJ_SCAN_NONE;
                break;
            }
            result = xdj_scan_frames(pFlac, state, found.position.offset + 1U, hi_offset, lo.first, hi_first, strategy, &found);
        }
        if (result == XDJ_SCAN_FAILED) {
            return DRFLAC_FALSE;
        }
        if (result == XDJ_SCAN_NONE) {
            /* No frame starts between the probe and the upper bound. */
            hi_offset = probe;
        } else if (found.position.first > target) {
            /* The target's frame starts before the first frame after the probe. */
            hi_offset = probe;
            hi_first = found.position.first;
        } else {
            lo = found.position;
        }
    }
decoded:
    xdj_remember_frame(state, &found.position);
    state->blocking = found.position.variable ? 2 : 1;
    return DRFLAC_TRUE;
}

drflac_bool32 xdj_flac_seek(drflac* decoder, drflac_uint64 frame_index, xdj_flac_seek_state* state)
{
    xdj_frame_position anchor;
    drflac_uint32 target;

    state->probes = 0;
    state->used_cache = 0;
    state->used_fallback = 0;
    state->failure = XDJ_SEEK_FAIL_NONE;
    if (decoder->currentPCMFrame == frame_index) {
        return DRFLAC_TRUE;
    }
    /* The adapter never seeks past the end; without these a seek is impossible. */
    if (frame_index >= decoder->totalPCMFrameCount || decoder->totalPCMFrameCount > 0xFFFFFFFFULL ||
        decoder->firstFLACFramePosInBytes == 0 || decoder->firstFLACFramePosInBytes > 0xFFFFFFFFULL ||
        decoder->bs.onTell == NULL) {
        state->failure = XDJ_SEEK_FAIL_BOUNDS;
        return DRFLAC_FALSE;
    }
    if (frame_index == 0) {
        if (!drflac__seek_to_first_frame(decoder)) {
            state->failure = XDJ_SEEK_FAIL_FIRST_FRAME;
            return DRFLAC_FALSE;
        }
        return DRFLAC_TRUE;
    }
    target = (drflac_uint32)frame_index;
    if (xdj_seek_nearby(decoder, target) && xdj_position_consistent(decoder)) {
        return DRFLAC_TRUE;
    }
    XDJ_TRACE("seek %u\n", target);
    if (xdj_seek_bounded(decoder, target, state, &anchor) && !xdj_seek_force_fallback) {
        if (xdj_position_consistent(decoder)) {
            return DRFLAC_TRUE;
        }
        state->failure = XDJ_SEEK_FAIL_VERIFY;
    }
    XDJ_TRACE("FALLBACK %u\n", target);
    state->used_fallback = 1;
    if (!xdj_seek_from_anchor(decoder, &anchor, target)) {
        state->failure |= XDJ_SEEK_FAIL_FALLBACK;
        return DRFLAC_FALSE;
    }
    if (!xdj_position_consistent(decoder)) {
        state->failure = XDJ_SEEK_FAIL_VERIFY | XDJ_SEEK_FAIL_FALLBACK;
        return DRFLAC_FALSE;
    }
    return DRFLAC_TRUE;
}

/*
 * Forget where the decoder is, so the next xdj_flac_seek searches afresh and
 * rereads from the file: used when bytes may have come from a handle that was
 * closed or reused during a call.
 */
void xdj_flac_invalidate(drflac* decoder)
{
    DRFLAC_ZERO_MEMORY(&decoder->currentFLACFrame, sizeof(decoder->currentFLACFrame));
    decoder->currentPCMFrame = decoder->totalPCMFrameCount;
}

#ifdef XDJ_CHIP_FLAC
/*
 * For FLAC decoded on the audio processor (src/chip_flac_feed.h): the
 * FLAC frame that holds frame_index, as the file offset of its header and
 * its first frame. Found by the bounded seek, which decodes that frame and
 * checks its CRC-16 on the way.
 */
drflac_bool32 xdj_flac_locate(
    drflac* decoder,
    drflac_uint64 frame_index,
    xdj_flac_seek_state* state,
    drflac_uint32* offset,
    drflac_uint32* first
)
{
    drflac_uint32 index;

    xdj_flac_invalidate(decoder);
    if (!xdj_flac_seek(decoder, frame_index, state)) {
        return DRFLAC_FALSE;
    }
    if (frame_index == 0) {
        *offset = (drflac_uint32)decoder->firstFLACFramePosInBytes;
        *first = 0;
        return DRFLAC_TRUE;
    }
    for (index = 0; index < XDJ_SEEK_CACHE_ENTRIES; ++index) {
        const xdj_seek_cache_entry* entry = &state->entries[index];

        if (entry->frames != 0 && entry->first <= frame_index && frame_index - entry->first < entry->frames) {
            *offset = entry->offset;
            *first = entry->first;
            return DRFLAC_TRUE;
        }
    }
    return DRFLAC_FALSE;
}
#endif
#endif

void xdj_flac_close(drflac* decoder)
{
    drflac_close(decoder);
}

#ifdef XDJ_HOST_TEST
/* dr_flac's own seek, for comparing costs in host tests. */
drflac_bool32 xdj_flac_seek_original(drflac* decoder, drflac_uint64 frame_index)
{
    return drflac_seek_to_pcm_frame(decoder, frame_index);
}
#endif
