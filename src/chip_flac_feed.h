/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * The main processor's side of FLAC decoded on the audio processor
 * (src/chip_flac.h): tags handed out in place of decoded audio, and
 * the FLAC file's bytes put into the audio processor's ring before them.
 * Used by the bridge (src/flac_adapter.c) and, on a
 * computer, by the host tests.
 */
#ifndef XDJ_CHIP_FLAC_FEED_H
#define XDJ_CHIP_FLAC_FEED_H

#include "chip_flac.h"

/* What the feed needs from the file, supplied by its user. */
typedef struct {
    void* user;
    /*
     * The FLAC frame that holds track frame `frame`: the file offset of its
     * header and the track frame it starts with. Returns 0 if it cannot be
     * found.
     */
    int (*locate)(void* user, unsigned frame, unsigned* offset, unsigned* first);
    /* Up to count bytes of the file from offset. Fewer only at the file's end, or on failure. */
    unsigned (*read)(void* user, unsigned offset, unsigned char* output, unsigned count);
} xdj_feed_io;

#ifdef XDJ_CHIP_PLACES
/*
 * Build switch XDJ_CHIP_PLACES: the player reads a
 * track at several places by turns (ahead of the playing position, behind
 * it, its cue points) and other tracks in between. Each time a read cannot
 * carry on the ring's last stream, a new stream starts at the frame asked
 * for, mostly inside a FLAC block, and the audio processor decodes that
 * block from its start and drops the frames before the place: its costliest
 * unit. With the switch the feed remembers the places it fed; a read that
 * comes back to one is given tags of that place's old stream (its bytes are
 * still in the ring) up to the next FLAC block, and the new stream starts on
 * that block: nothing to drop. Not for hi-res, whose halving filter needs
 * the frames before.
 */
#define XDJ_FEED_PLACES 4u
typedef struct {
    unsigned used;
    unsigned next;          /* the track frame its reads carry on at */
    unsigned number;        /* its stretch */
    unsigned stretch_first; /* the track frame of that stretch's frame 0 */
    unsigned stream_start;  /* its stream's place in the ring */
    unsigned stream_bytes;  /* bytes of that stream in the ring */
    unsigned file_start;    /* file offset of that stream's first FLAC frame */
} xdj_feed_place;
#endif

/* A track being fed. */
typedef struct {
    unsigned char header[XDJ_CHIP_STREAM_HEADER]; /* what every stream of this track starts with */
    unsigned frame_count;   /* frames of the track */
    unsigned raw_bytes;     /* bytes of one stereo frame as plain PCM at the file's depth: 4 or 6 */
    unsigned block_most;    /* frames in the file's largest FLAC block */
    /* The stream in the ring that carries this track on, while it is the ring's last. */
    unsigned live;
    unsigned serial;        /* which stream of all it is */
    unsigned stream_start;  /* its place in the ring */
    unsigned stream_bytes;  /* bytes of it in the ring */
    unsigned file_start;    /* file offset of its first FLAC frame */
    unsigned first_frame;   /* track frame of its first sample */
    unsigned ended;         /* the file's end is in the ring */
    unsigned next;          /* the track frame the next read carries on at */
    unsigned number;        /* the stretch in hand */
    unsigned stretch_first; /* the track frame of its frame 0 */
#ifdef XDJ_CHIP_HIRES
    /*
     * 1 for hi-res (xdj_feed_halve): track frames are at half the file's
     * rate, while first_frame, block_most and the byte reckoning count frames
     * of the file.
     */
    unsigned shift;
#endif
#ifdef XDJ_CHIP_RESTART
    /*
     * The stretch before the one in hand, if this stream carried it on from
     * there (the player may still hold its tags): its number, its first
     * track frame, and the stream its record points at.
     */
    unsigned has_prev;
    unsigned prev_number;
    unsigned prev_first;
    unsigned prev_stream;
#endif
#ifdef XDJ_CHIP_PLACES
    xdj_feed_place places[XDJ_FEED_PLACES];
    unsigned place_turn;     /* where the next place remembered goes */
    unsigned handover;       /* 1 while tags of an old stream are handed out, up to handover_end */
    unsigned handover_end;   /* the track frame where the next FLAC block starts */
    unsigned handover_number;
    unsigned handover_first;
#endif
} xdj_feed_track;

/* Start afresh: the audio processor's side has just been put in place. */
void xdj_feed_reset(void);

/*
 * Whether the audio processor's table of converters still holds our entry
 * (one word read through the port). It does not once the audio processor
 * has been started over with its program: then no tag may be handed out.
 */
int xdj_feed_chip_there(void);

/*
 * Take a track on. head is the file's first XDJ_CHIP_STREAM_HEADER bytes;
 * bits is the file's depth. Returns 0 if the file cannot be fed (its stream
 * information is not where it should be).
 */
int xdj_feed_open(xdj_feed_track* track, const unsigned char* head, unsigned frame_count, unsigned bits);

#ifdef XDJ_CHIP_HIRES
/*
 * After xdj_feed_open, for an 88.2 or 96 kHz file: the track is played at
 * half the rate (frame_count was given at that rate), and the audio
 * processor halves it (src/chip_flac.h, XDJ_CHIP_RECORD_HALVED).
 */
void xdj_feed_halve(xdj_feed_track* track);
#endif

/*
 * Tags for frames track frames from position, four bytes each, into output;
 * the bytes the audio processor needs for them are in the ring when this
 * returns. Returns the number of frames done: fewer than asked only when
 * the file could not be read.
 */
unsigned xdj_feed_read(xdj_feed_track* track, unsigned position, unsigned frames, unsigned char* output,
                       const xdj_feed_io* io);

#endif
