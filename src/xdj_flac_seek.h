/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * Bounded FLAC seek shared by the adapter bridge and the decoder unit.
 *
 * Include after dr_flac.h (declarations only are enough).
 */

#ifndef XDJ_FLAC_SEEK_H
#define XDJ_FLAC_SEEK_H

#define XDJ_SEEK_CACHE_ENTRIES 8U

/* A frame whose CRC-16 checked out: header byte offset, first PCM frame, length. */
typedef struct {
    drflac_uint32 offset;
    drflac_uint32 first;
    drflac_uint32 frames;
} xdj_seek_cache_entry;

/*
 * Per-decoder seek state. The cache remembers frames found by earlier seeks,
 * so a repeated hot cue seeks straight to its frame. probes, used_cache and
 * used_fallback describe the most recent seek, for diagnostics.
 */
typedef struct {
    xdj_seek_cache_entry entries[XDJ_SEEK_CACHE_ENTRIES];
    drflac_uint32 next_entry;
    drflac_uint32 stream_bytes;
    drflac_uint32 probes;
    /* Blocking strategy seen in a verified frame: 0 unknown, 1 fixed, 2 variable. */
    drflac_uint8 blocking;
    drflac_uint8 used_cache;
    drflac_uint8 used_fallback;
    /* Why the last seek's search stopped (XDJ_SEEK_FAIL_*), plus 8 if the fallback failed too. */
    drflac_uint8 failure;
} xdj_flac_seek_state;

#define XDJ_SEEK_FAIL_NONE 0U
#define XDJ_SEEK_FAIL_BOUNDS 1U      /* no usable range: file size or first frame */
#define XDJ_SEEK_FAIL_FIRST_FRAME 2U /* the stream's first frame header did not parse */
#define XDJ_SEEK_FAIL_DECODE 3U      /* the target's frame did not decode (CRC-16 or read) */
#define XDJ_SEEK_FAIL_SPAN 4U        /* the byte range closed without finding the frame */
#define XDJ_SEEK_FAIL_PROBES 5U      /* probe budget used up */
#define XDJ_SEEK_FAIL_IO 6U          /* a stream seek was refused */
#define XDJ_SEEK_FAIL_VERIFY 7U      /* the frame reached is not the one the position says */
#define XDJ_SEEK_FAIL_FALLBACK 8U    /* added when the last-resort decode failed as well */

/* Forget cached frames; stream_bytes is the file size, 0 if unknown. */
void xdj_flac_seek_reset(xdj_flac_seek_state* state, drflac_uint32 stream_bytes);

/*
 * Seek to a PCM frame, decoding at most the FLAC frame that contains it. Falls
 * back to dr_flac's own seek only when the bounded search cannot finish.
 */
/* Make the next seek search afresh (bytes read through a lost handle are not trusted). */
void xdj_flac_invalidate(drflac* decoder);

/* Decoded frames whose frame number disagreed with the decoder's position, since start-up. */
extern drflac_uint32 xdj_flac_sequence_breaks;

drflac_bool32 xdj_flac_seek(drflac* decoder, drflac_uint64 frame_index, xdj_flac_seek_state* state);

/*
 * Rate conversion for 88.2 and 96 kHz sources: the rate is halved through a
 * half-band low-pass of XDJ_HALFBAND_TAPS taps (47 unless the build says
 * otherwise; 31, 23 and 15 trade treble and fold-back suppression for
 * speed). The filter keeps the frames it still needs from before the next
 * one (its history), so consecutive reads continue seamlessly.
 */
#ifndef XDJ_HALFBAND_TAPS
#define XDJ_HALFBAND_TAPS 47
#endif
#define XDJ_DECIMATOR_HISTORY ((unsigned)XDJ_HALFBAND_TAPS - 1U)
typedef struct {
    /* 1 when the rate is halved, 0 for no conversion. */
    drflac_uint8 shift;
    drflac_int32 history[XDJ_DECIMATOR_HISTORY * 2U];
} xdj_flac_decimator;

/* No conversion, and silence before the first frame. */
void xdj_flac_decimator_reset(xdj_flac_decimator* decimator);

/*
 * Read converted frames (bytes_per_frame 4 or 6); a NULL output discards them.
 * Built with XDJ_CHIP_HIRES, bytes_per_frame 0 gives each sample as a word
 * holding its 24-bit value (the audio processor's converter).
 */
drflac_uint64 xdj_flac_read_decimated(
    drflac* decoder,
    xdj_flac_decimator* decimator,
    drflac_uint64 frame_count,
    unsigned char* output,
    drflac_uint8 bytes_per_frame
);

/* Seek to a converted frame and refill the filter history from the frames before it. */
drflac_bool32 xdj_flac_seek_decimated(
    drflac* decoder,
    drflac_uint32 frame_index,
    xdj_flac_seek_state* state,
    xdj_flac_decimator* decimator
);

/*
 * WAV and AIFF files the stock readers refuse but the bridge can serve: PCM
 * at 88.2 or 96 kHz (converted down like FLAC at those rates) and WAV files
 * whose header is the extensible form (as Bandcamp writes them) at any
 * served rate. The source reads sample bytes straight from the file.
 */
typedef struct {
    drflac_read_proc on_read;
    drflac_seek_proc on_seek;
    void* user_data;
    /* File offset of the first sample byte, and the stream length in source frames. */
    drflac_uint32 data_offset;
    drflac_uint32 frames;
    /* Bytes of one frame in the file: 4 or 6 for stereo (and with XDJ_MONO 2 or 3 for mono). */
    drflac_uint8 frame_bytes;
    drflac_uint8 big_endian;
#ifdef XDJ_MONO
    /* 1 or 2; a mono file is handed out as stereo, the one channel on both sides. */
    drflac_uint8 channels;
#endif
} xdj_pcm_source;

/*
 * Mono files (build switch XDJ_MONO, v0.9 on) are
 * handed out as stereo, each sample on both sides. Without the switch only
 * stereo is taken, and the code is the code of the builds before it.
 */
#ifdef XDJ_MONO
#define XDJ_CHANNELS_TAKEN(channels) ((channels) == 1U || (channels) == 2U)
#else
#define XDJ_CHANNELS_TAKEN(channels) ((channels) == 2U)
#endif

/*
 * Read the header of a WAV (aiff 0) or AIFF/AIFC (aiff 1) file. Returns
 * true and fills source, rate, frames and bits when the file is one of
 * ours; false for every other file, which is then left to the stock reader
 * (plain 44.1 and 48 kHz files, and whatever it refuses anyway).
 * stream_bytes is the file size, 0 if unknown.
 */
drflac_bool32 xdj_pcm_open(
    xdj_pcm_source* source,
    drflac_bool32 aiff,
    drflac_uint32 stream_bytes,
    drflac_uint32* sample_rate,
    drflac_uint32* frame_count,
    drflac_uint8* bits_per_sample
);

/* Read frames in the output form (little-endian, bytes_per_frame 4 or 6), halved when the decimator says so. */
drflac_uint64 xdj_pcm_read(
    xdj_pcm_source* source,
    xdj_flac_decimator* decimator,
    drflac_uint64 frame_count,
    unsigned char* output,
    drflac_uint8 bytes_per_frame
);

/* Seek to an output frame; with rate conversion, refill the filter history from the frames before it. */
drflac_bool32 xdj_pcm_seek(xdj_pcm_source* source, drflac_uint32 frame_index, xdj_flac_decimator* decimator);

#endif
