/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * The main processor's side of FLAC decoded on the audio processor: see
 * src/chip_flac_feed.h and src/chip_flac.h.
 */

#include <stddef.h>

#include "chip_flac_feed.h"

/* Frames of the file for track frames: twice as many for a halved track (XDJ_CHIP_HIRES). */
#ifdef XDJ_CHIP_HIRES
#define XDJ_FEED_FILE_FRAMES(track, frames) ((frames) << (track)->shift)
#else
#define XDJ_FEED_FILE_FRAMES(track, frames) (frames)
#endif

#ifdef XDJ_HOST_TEST
/* On a computer the runner stands in for the port to the audio processor. */
extern unsigned xdj_host_port_read(unsigned address);
extern void xdj_host_port_write(unsigned address, unsigned value);
extern void xdj_host_port_write_words(unsigned address, const unsigned* words, unsigned count);
#define XDJ_PORT_READ xdj_host_port_read
#define XDJ_PORT_WRITE xdj_host_port_write
#define XDJ_PORT_WRITE_WORDS xdj_host_port_write_words
#else
/* The stock routines: one word read, one word written, a run of words written. */
typedef unsigned (*xdj_port_read_fn)(unsigned address);
typedef void (*xdj_port_write_fn)(unsigned address, unsigned value);
typedef void (*xdj_port_write_words_fn)(unsigned address, const unsigned* words, unsigned count);
#define XDJ_PORT_READ ((xdj_port_read_fn)0x08BF2BCCu)
#define XDJ_PORT_WRITE ((xdj_port_write_fn)0x08BF2C30u)
#define XDJ_PORT_WRITE_WORDS ((xdj_port_write_words_fn)0x08BF2C92u)
#endif

/* File bytes go to the ring through this, a piece at a time. */
#define XDJ_FEED_PIECE_WORDS 512u
/* The decoder reads ahead of what it decodes by up to this much, with room to spare. */
#define XDJ_FEED_AHEAD_BYTES 4200u
/* The ring is never filled closer than this to where the decoder reads. */
#define XDJ_FEED_RING_SPARE 0x10000u

/* In the tail's BSS on the player (not cleared at boot): xdj_feed_reset sets all of it. */
static struct {
    unsigned pushed;    /* place in the ring after the last byte put there */
    unsigned streams;   /* streams started */
    unsigned numbers;   /* stretches started */
#ifdef XDJ_CHIP_RESTART
    unsigned answered;  /* the audio processor's last ask dealt with (its ask_sequence) */
    unsigned restarts;  /* new streams started on its asking */
    /*
     * The stream given up on at the last restart, plus 1 (0: none). Where
     * the decoder last read in it says nothing any more about how far the
     * ring may be filled: it will not read there again.
     */
    unsigned abandoned;
#endif
} xdj_feed;
static unsigned xdj_feed_piece[XDJ_FEED_PIECE_WORDS];
#define XDJ_PLACES_NOTE(why, detail) ((void)0)

void xdj_feed_reset(void)
{
    xdj_feed.pushed = 0;
    xdj_feed.streams = 0;
    xdj_feed.numbers = 0;
#ifdef XDJ_CHIP_RESTART
    xdj_feed.answered = 0;
    xdj_feed.restarts = 0;
    xdj_feed.abandoned = 0;
#endif
    XDJ_PORT_WRITE(XDJ_CHIP_CONTROL + 4u, 0u);
    XDJ_PORT_WRITE(XDJ_CHIP_CONTROL, XDJ_CHIP_CONTROL_MAGIC);
}

int xdj_feed_chip_there(void)
{
    return XDJ_PORT_READ(XDJ_CHIP_TABLE_ENTRY) == XDJ_CHIP_ENTRY;
}

int xdj_feed_open(xdj_feed_track* track, const unsigned char* head, unsigned frame_count, unsigned bits)
{
    unsigned index;

    /* "fLaC", then the stream information block: type 0, 34 bytes. */
    if (head[0] != 'f' || head[1] != 'L' || head[2] != 'a' || head[3] != 'C' ||
        (head[4] & 0x7Fu) != 0u || head[5] != 0u || head[6] != 0u || head[7] != 34u ||
        (bits != 16u && bits != 24u) || frame_count <= XDJ_CHIP_TAIL_FRAMES) {
        return 0;
    }
    for (index = 0; index < XDJ_CHIP_STREAM_HEADER; index++) {
        track->header[index] = head[index];
    }
    /* A stream of ours has no other metadata, and its length is left open (it starts anywhere in the file). */
    track->header[4] = 0x80u;
    track->header[21] &= 0xF0u;
    for (index = 22; index < XDJ_CHIP_STREAM_HEADER; index++) {
        track->header[index] = 0;
    }
    track->frame_count = frame_count;
    track->raw_bytes = bits == 24u ? 6u : 4u;
    track->block_most = ((unsigned)head[10] << 8) | head[11];
    track->live = 0;
    track->next = 0;
#ifdef XDJ_CHIP_HIRES
    track->shift = 0;
#endif
#ifdef XDJ_CHIP_PLACES
    for (index = 0; index < XDJ_FEED_PLACES; index++) {
        track->places[index].used = 0;
    }
    track->place_turn = 0;
    track->handover = 0;
#endif
    return track->block_most != 0u;
}

#ifdef XDJ_CHIP_HIRES
void xdj_feed_halve(xdj_feed_track* track)
{
    track->shift = 1;
    track->live = 0;
#ifdef XDJ_CHIP_PLACES
    track->handover = 0;
#endif
}
#endif

/* The words of bytes[0..count), count a multiple of 4 or padded by the caller, to the ring at place. */
static void xdj_feed_put(unsigned place, unsigned words)
{
    unsigned at = place & (XDJ_CHIP_RING_BYTES - 1u);
    unsigned room = (XDJ_CHIP_RING_BYTES - at) / 4u;

    if (words > room) {
        XDJ_PORT_WRITE_WORDS(XDJ_CHIP_RING + at, xdj_feed_piece, room);
        XDJ_PORT_WRITE_WORDS(XDJ_CHIP_RING, xdj_feed_piece + room, words - room);
    } else {
        XDJ_PORT_WRITE_WORDS(XDJ_CHIP_RING + at, xdj_feed_piece, words);
    }
}

/* Where the record of the stretch in hand lies. */
static unsigned xdj_feed_record(const xdj_feed_track* track)
{
    return XDJ_CHIP_RECORDS + 16u * (track->number % XDJ_CHIP_RECORD_COUNT);
}

/* A new stretch of the stream in hand, with its frame 0 at track frame position. */
static void xdj_feed_stretch(xdj_feed_track* track, unsigned position)
{
    unsigned address;

#ifdef XDJ_CHIP_RESTART
    /* The stretch in hand goes on in this one's stream (xdj_feed_start clears this for a new stream). */
    track->has_prev = 1;
    track->prev_number = track->number;
    track->prev_first = track->stretch_first;
    track->prev_stream = track->stream_start;
#endif
    track->number = xdj_feed.numbers++ % XDJ_CHIP_TAG_NUMBERS;
    track->stretch_first = position;
    address = xdj_feed_record(track);
    /* The id goes last: until then the record is not this stretch's. */
    XDJ_PORT_WRITE(address, 0u);
    XDJ_PORT_WRITE(address + 4u, track->stream_start);
    XDJ_PORT_WRITE(address + 8u, XDJ_FEED_FILE_FRAMES(track, track->stretch_first) - track->first_frame);
    XDJ_PORT_WRITE(address + 12u, track->stream_bytes);
#ifdef XDJ_CHIP_HIRES
    XDJ_PORT_WRITE(address, (track->shift != 0u ? XDJ_CHIP_RECORD_HALVED : XDJ_CHIP_RECORD_LIVE) | track->number);
#else
    XDJ_PORT_WRITE(address, XDJ_CHIP_RECORD_LIVE | track->number);
#endif
}

/*
 * Bring the stream to at least wanted bytes (or to the file's end), in
 * pieces that leave its length a multiple of four. Returns 0 if the file
 * could not be read.
 */
static int xdj_feed_extend(xdj_feed_track* track, unsigned wanted, const xdj_feed_io* io)
{
    unsigned char* bytes = (unsigned char*)xdj_feed_piece;

    wanted = (wanted + 3u) & ~3u;
    while (!track->ended && track->stream_bytes < wanted) {
        unsigned count = wanted - track->stream_bytes;
        unsigned got;
        unsigned index;

        if (count > XDJ_FEED_PIECE_WORDS * 4u) {
            count = XDJ_FEED_PIECE_WORDS * 4u;
        }
        got = io->read(io->user, track->file_start + (track->stream_bytes - XDJ_CHIP_STREAM_HEADER), bytes, count);
        if (got > count) {
            return 0;
        }
        if (got < count) {
            /* The file's end: the last word is filled up, the length says where the bytes end. */
            track->ended = 1;
            for (index = got; index < ((got + 3u) & ~3u); index++) {
                bytes[index] = 0;
            }
        }
        if (got != 0u) {
            xdj_feed_put(track->stream_start + track->stream_bytes, (got + 3u) / 4u);
            track->stream_bytes += got;
            xdj_feed.pushed = track->stream_start + ((track->stream_bytes + 3u) & ~3u);
        }
    }
    XDJ_PORT_WRITE(XDJ_CHIP_CONTROL + 4u, xdj_feed.pushed);
    return 1;
}

#ifdef XDJ_CHIP_PLACES
/* Keep the place the stream in hand feeds (XDJ_CHIP_PLACES in chip_flac_feed.h), the oldest giving way. */
static void xdj_feed_remember(xdj_feed_track* track)
{
    xdj_feed_place* place = &track->places[track->place_turn++ % XDJ_FEED_PLACES];

    place->used = 1;
    place->next = track->next;
    place->number = track->number;
    place->stretch_first = track->stretch_first;
    place->stream_start = track->stream_start;
    place->stream_bytes = track->stream_bytes;
    place->file_start = track->file_start;
}

/*
 * A read at track frame at that cannot carry on the ring's last stream: if
 * it comes back to a place fed before, and that place's old stream holds
 * every byte up to the next FLAC block (still in the ring, its record still
 * its own), hand out that stream's tags up to the block and start the new
 * stream there. Returns 1 if so (track->handover is set), 0 for a new stream
 * at at as before.
 */
static int xdj_feed_hand_over(xdj_feed_track* track, unsigned at, const xdj_feed_io* io)
{
    xdj_feed_place found;
    unsigned index;
    unsigned offset = 0;
    unsigned first = 0;
    unsigned boundary_offset = 0;
    unsigned boundary = 0;
    unsigned address;
    int live = 0;

#ifdef XDJ_CHIP_HIRES
    if (track->shift != 0u) {
        XDJ_PLACES_NOTE(XDJ_PLACES_HIRES, 0u);
        return 0;
    }
#endif
    if (track->live && track->next == at) {
        found.next = track->next;
        found.number = track->number;
        found.stretch_first = track->stretch_first;
        found.stream_start = track->stream_start;
        found.stream_bytes = track->stream_bytes;
        found.file_start = track->file_start;
        live = 1;
    } else {
        for (index = 0; index < XDJ_FEED_PLACES; index++) {
            if (track->places[index].used && track->places[index].next == at) {
                break;
            }
        }
        if (index == XDJ_FEED_PLACES) {
            XDJ_PLACES_NOTE(XDJ_PLACES_NONE, 0u);
            return 0;
        }
        found = track->places[index];
        track->places[index].used = 0;
    }
    /* At a block's start a new stream drops nothing anyway. */
    if (!io->locate(io->user, at, &offset, &first) || first >= at ||
        first + track->block_most >= track->frame_count - XDJ_CHIP_TAIL_FRAMES ||
        !io->locate(io->user, first + track->block_most, &boundary_offset, &boundary) ||
        boundary <= at || boundary_offset <= offset) {
        return 0;
    }
    /*
     * Every byte from the block that holds at to the boundary in the old
     * stream, and still in the ring (it was pushed with the place's last reads).
     */
    if (offset < found.file_start || boundary_offset - found.file_start > found.stream_bytes - XDJ_CHIP_STREAM_HEADER ||
        xdj_feed.pushed - (found.stream_start + XDJ_CHIP_STREAM_HEADER + (offset - found.file_start)) >
            XDJ_CHIP_RING_BYTES - XDJ_FEED_RING_SPARE ||
        at < found.stretch_first || boundary - found.stretch_first >= XDJ_CHIP_TAG_FRAMES) {
        return 0;
    }
    /* Its stretch's record still its own (the records are used in turn). */
    address = XDJ_CHIP_RECORDS + 16u * (found.number % XDJ_CHIP_RECORD_COUNT);
    if (XDJ_PORT_READ(address) != (XDJ_CHIP_RECORD_LIVE | found.number) || XDJ_PORT_READ(address + 4u) != found.stream_start) {
        XDJ_PLACES_NOTE(XDJ_PLACES_RECORD, 0u);
        return 0;
    }
    XDJ_PLACES_NOTE(XDJ_PLACES_HANDED, boundary - at);
    if (track->live && !live) {
        xdj_feed_remember(track);
    }
    track->live = 0;
    track->handover = 1;
    track->handover_end = boundary;
    track->handover_number = found.number;
    track->handover_first = found.stretch_first;
    track->next = at;
    return 1;
}
#endif

/* A new stream for the track, from the FLAC frame that holds track frame position. Returns 0 on failure. */
static int xdj_feed_start(xdj_feed_track* track, unsigned position, const xdj_feed_io* io)
{
    unsigned char* bytes = (unsigned char*)xdj_feed_piece;
    unsigned offset = 0;
    unsigned first = 0;
    unsigned got;
    unsigned index;
#ifdef XDJ_CHIP_PLACES
    /* The place the stream in hand fed: reads may come back to it. */
    if (track->live) {
        xdj_feed_remember(track);
    }
#endif
#ifdef XDJ_CHIP_HIRES
    /* A halved stream starts with the warm-up before the frame asked for (src/chip_flac.h). */
    unsigned from = track->shift == 0u ? position
                    : (position - (position < XDJ_CHIP_HALVED_WARMUP ? position : XDJ_CHIP_HALVED_WARMUP)) << 1;

    track->live = 0;
    if (!io->locate(io->user, from, &offset, &first) || first > from) {
        return 0;
    }
#else

    track->live = 0;
    if (!io->locate(io->user, position, &offset, &first) || first > position) {
        return 0;
    }
#endif
    /* The header and the file's first two bytes make eleven words. */
    for (index = 0; index < XDJ_CHIP_STREAM_HEADER; index++) {
        bytes[index] = track->header[index];
    }
    got = io->read(io->user, offset, bytes + XDJ_CHIP_STREAM_HEADER, 2u);
    if (got != 2u) {
        return 0;
    }
    track->stream_start = xdj_feed.pushed;
    track->file_start = offset;
    track->first_frame = first;
    track->ended = 0;
    xdj_feed_put(track->stream_start, (XDJ_CHIP_STREAM_HEADER + 2u) / 4u);
    track->stream_bytes = XDJ_CHIP_STREAM_HEADER + 2u;
    xdj_feed.pushed = track->stream_start + track->stream_bytes;
    track->serial = ++xdj_feed.streams;
    track->live = 1;
    xdj_feed_stretch(track, position);
#ifdef XDJ_CHIP_RESTART
    track->has_prev = 0;
#endif
    return 1;
}

#ifdef XDJ_CHIP_RESTART
/* Whether the record of the stretch before is still that stretch's: the 64 records are used in turn. */
static int xdj_feed_prev_record(const xdj_feed_track* track)
{
    unsigned id = XDJ_PORT_READ(XDJ_CHIP_RECORDS + 16u * (track->prev_number % XDJ_CHIP_RECORD_COUNT));

#ifdef XDJ_CHIP_HIRES
    if (id == (XDJ_CHIP_RECORD_HALVED | track->prev_number)) {
        return 1;
    }
#endif
    return id == (XDJ_CHIP_RECORD_LIVE | track->prev_number);
}

/* Point the record of a stretch of the track at the stream in hand (the id last, as in xdj_feed_stretch). */
static void xdj_feed_point(const xdj_feed_track* track, unsigned number, unsigned stretch_first)
{
    unsigned address = XDJ_CHIP_RECORDS + 16u * (number % XDJ_CHIP_RECORD_COUNT);

    XDJ_PORT_WRITE(address, 0u);
    XDJ_PORT_WRITE(address + 4u, track->stream_start);
    XDJ_PORT_WRITE(address + 8u, XDJ_FEED_FILE_FRAMES(track, stretch_first) - track->first_frame);
    XDJ_PORT_WRITE(address + 12u, track->stream_bytes);
#ifdef XDJ_CHIP_HIRES
    XDJ_PORT_WRITE(address, (track->shift != 0u ? XDJ_CHIP_RECORD_HALVED : XDJ_CHIP_RECORD_LIVE) | number);
#else
    XDJ_PORT_WRITE(address, XDJ_CHIP_RECORD_LIVE | number);
#endif
}

/*
 * A new stream from track frame position, for the stretches the player
 * holds tags of: as xdj_feed_start, but the stretch in hand (and the one
 * before, if this stream carried it) keeps its number and its record is
 * pointed at the new stream. Reads go on where they were. Returns 0 on
 * failure.
 */
static int xdj_feed_restart(xdj_feed_track* track, unsigned position, const xdj_feed_io* io)
{
    unsigned char* bytes = (unsigned char*)xdj_feed_piece;
    unsigned offset = 0;
    unsigned first = 0;
    unsigned index;
    unsigned from = position;

#ifdef XDJ_CHIP_HIRES
    if (track->shift != 0u) {
        from = (position - (position < XDJ_CHIP_HALVED_WARMUP ? position : XDJ_CHIP_HALVED_WARMUP)) << 1;
    }
#endif
    track->live = 0;
    if (!io->locate(io->user, from, &offset, &first) || first > from) {
        return 0;
    }
    for (index = 0; index < XDJ_CHIP_STREAM_HEADER; index++) {
        bytes[index] = track->header[index];
    }
    if (io->read(io->user, offset, bytes + XDJ_CHIP_STREAM_HEADER, 2u) != 2u) {
        return 0;
    }
    track->stream_start = xdj_feed.pushed;
    track->file_start = offset;
    track->first_frame = first;
    track->ended = 0;
    xdj_feed_put(track->stream_start, (XDJ_CHIP_STREAM_HEADER + 2u) / 4u);
    track->stream_bytes = XDJ_CHIP_STREAM_HEADER + 2u;
    xdj_feed.pushed = track->stream_start + track->stream_bytes;
    track->serial = ++xdj_feed.streams;
    track->live = 1;
    if (track->has_prev && xdj_feed_prev_record(track)) {
        xdj_feed_point(track, track->prev_number, track->prev_first);
        track->prev_stream = track->stream_start;
    } else {
        track->has_prev = 0;
    }
    xdj_feed_point(track, track->number, track->stretch_first);
    return 1;
}

/*
 * The audio processor's ask for a new stream (xdj_chip_status), if it is
 * for a stretch of this track that still points at the stream it is stuck
 * in: a new stream from the frame asked for. An ask for another track's
 * stretch is left to that track's reads; one that no longer applies is
 * dropped.
 */
static void xdj_feed_answer(xdj_feed_track* track, const xdj_feed_io* io)
{
    unsigned sequence = XDJ_PORT_READ(XDJ_CHIP_STATUS + offsetof(xdj_chip_status, ask_sequence));
    unsigned number;
    unsigned frame;
    unsigned stream;
    unsigned position;

    if (sequence == 0u || (sequence & 1u) != 0u || sequence == xdj_feed.answered) {
        return;
    }
    number = XDJ_PORT_READ(XDJ_CHIP_STATUS + offsetof(xdj_chip_status, ask_number));
    frame = XDJ_PORT_READ(XDJ_CHIP_STATUS + offsetof(xdj_chip_status, ask_frame));
    stream = XDJ_PORT_READ(XDJ_CHIP_STATUS + offsetof(xdj_chip_status, ask_stream));
    if (XDJ_PORT_READ(XDJ_CHIP_STATUS + offsetof(xdj_chip_status, ask_sequence)) != sequence) {
        return;
    }
    if (!track->live || (number != track->number && !(track->has_prev && number == track->prev_number))) {
        return;
    }
    xdj_feed.answered = sequence;
    if (number == track->number) {
        if (stream != track->stream_start) {
            return;
        }
        position = track->stretch_first + frame;
    } else {
        if (stream != track->prev_stream) {
            return;
        }
        position = track->prev_first + frame;
    }
    if (position >= track->next || position >= track->frame_count - XDJ_CHIP_TAIL_FRAMES) {
        return;
    }
    xdj_feed.restarts++;
    xdj_feed.abandoned = stream + 1u;
    (void)xdj_feed_restart(track, position, io);
}
#endif

/* Bytes that frames frames can take at most as FLAC: plain PCM and an eighth, and a little. */
static unsigned xdj_feed_bound(const xdj_feed_track* track, unsigned frames)
{
    unsigned plain = frames * track->raw_bytes;

    return plain + (plain >> 3) + 64u;
}

/* Where the audio processor's decoder stands, read through the port (see xdj_chip_status). 0 if not to be had. */
static int xdj_feed_chip_place(unsigned* stream, unsigned* frames, unsigned* bytes)
{
    unsigned tries;

    for (tries = 0; tries < 3u; tries++) {
        unsigned sequence = XDJ_PORT_READ(XDJ_CHIP_STATUS + offsetof(xdj_chip_status, sequence));

        *stream = XDJ_PORT_READ(XDJ_CHIP_STATUS + offsetof(xdj_chip_status, at_stream));
        *frames = XDJ_PORT_READ(XDJ_CHIP_STATUS + offsetof(xdj_chip_status, at_frames));
        *bytes = XDJ_PORT_READ(XDJ_CHIP_STATUS + offsetof(xdj_chip_status, at_bytes));
        if ((sequence & 1u) == 0u && sequence == XDJ_PORT_READ(XDJ_CHIP_STATUS + offsetof(xdj_chip_status, sequence))) {
            return sequence != 0u;
        }
    }
    return 0;
}

/*
 * How many bytes of the stream the audio processor can need to decode up to
 * end frames from the stream's first sample, and no more than the ring can
 * hold ahead of where its decoder reads.
 *
 * Reckoned from where its decoder is, if it is in this stream: all bytes
 * for the frames before at_frames (the end of the block in hand) lie before
 * at_bytes, so the frames from there to end need at most their bound beyond
 * it, plus one largest block (the block end may stand anywhere, even past
 * end), plus the decoder's read-ahead. If the decoder is elsewhere, the
 * same from the stream's start.
 */
static unsigned xdj_feed_needed(const xdj_feed_track* track, unsigned end)
{
    unsigned stream = 0;
    unsigned at_frames = 0;
    unsigned at_bytes = XDJ_CHIP_STREAM_HEADER;
    unsigned frames;
    unsigned bytes;
    unsigned needed;
    int known = xdj_feed_chip_place(&stream, &frames, &bytes);

#ifdef XDJ_CHIP_RESTART
    if (known && xdj_feed.abandoned != 0u && stream + 1u == xdj_feed.abandoned) {
        known = 0;
    }
#endif

    if (known && stream == track->stream_start && bytes <= track->stream_bytes && bytes >= XDJ_CHIP_STREAM_HEADER) {
        at_frames = frames;
        at_bytes = bytes;
    }
    needed = at_bytes + xdj_feed_bound(track, end > at_frames ? end - at_frames : 0u) +
             xdj_feed_bound(track, track->block_most) + XDJ_FEED_AHEAD_BYTES;
    /*
     * Never so far ahead that the ring goes over bytes the decoder has not
     * read yet: what cannot be had in time is silence, what is overwritten
     * would be lost for good. The decoder reads where it reported (if it has
     * not reported yet, the stream's start is taken): in this stream, the
     * stream may reach that far plus the ring less a spare; in an older one,
     * the ring less the spare from that place.
     */
    {
        unsigned reading = known ? stream + bytes : track->stream_start;
        unsigned room = XDJ_CHIP_RING_BYTES - XDJ_FEED_RING_SPARE;
        unsigned behind = track->stream_start - reading;
        unsigned limit;

        if (behind != 0u && behind < XDJ_CHIP_RING_BYTES) {
            limit = behind < room ? room - behind : 0u;
        } else {
            limit = reading - track->stream_start + room;
        }
        if (needed > limit) {
            needed = limit;
        }
    }
    return needed;
}

unsigned xdj_feed_read(xdj_feed_track* track, unsigned position, unsigned frames, unsigned char* output,
                       const xdj_feed_io* io)
{
    unsigned done = 0;

    if (position >= track->frame_count) {
        return 0;
    }
    if (frames > track->frame_count - position) {
        frames = track->frame_count - position;
    }
#ifdef XDJ_CHIP_RESTART
    xdj_feed_answer(track, io);
#endif
    while (done < frames) {
        unsigned at = position + done;
        unsigned part = frames - done;
        unsigned tagged;
        unsigned index;
        unsigned tag;
#ifdef XDJ_CHIP_PLACES
        unsigned number;
        unsigned stretch_first;
#endif

        /* The track's last frames are plain silence (see XDJ_CHIP_TAIL_FRAMES). */
        if (at >= track->frame_count - XDJ_CHIP_TAIL_FRAMES) {
            for (index = 0; index < part * 4u; index++) {
                output[done * 4u + index] = 0;
            }
            done += part;
            break;
        }
        if (part > track->frame_count - XDJ_CHIP_TAIL_FRAMES - at) {
            part = track->frame_count - XDJ_CHIP_TAIL_FRAMES - at;
        }
#ifdef XDJ_CHIP_PLACES
        if (track->handover && at == track->next && at < track->handover_end) {
            /* Tags of the old stream, up to the block where the new one starts: its bytes are in the ring. */
            if (part > track->handover_end - at) {
                part = track->handover_end - at;
            }
            number = track->handover_number;
            stretch_first = track->handover_first;
        } else {
            track->handover = 0;
#endif
        /*
         * A stream of ours can only be carried on while it is the ring's
         * last and the read goes on where the one before ended.
         */
        if (!track->live || track->serial != xdj_feed.streams || at != track->next) {
#ifdef XDJ_CHIP_PLACES
            if (xdj_feed_hand_over(track, at, io)) {
                continue;
            }
#endif
            if (!xdj_feed_start(track, at, io)) {
                break;
            }
        } else if (at - track->stretch_first >= XDJ_CHIP_TAG_FRAMES) {
            xdj_feed_stretch(track, at);
        }
        tagged = XDJ_CHIP_TAG_FRAMES - (at - track->stretch_first);
        if (part > tagged) {
            part = tagged;
        }
        if (!xdj_feed_extend(track, xdj_feed_needed(track, XDJ_FEED_FILE_FRAMES(track, at + part) - track->first_frame), io)) {
            track->live = 0;
            break;
        }
        /* Only now is the stream's new length the audio processor's to see: one word, so never half written. */
        XDJ_PORT_WRITE(xdj_feed_record(track) + 12u, track->stream_bytes);
#ifdef XDJ_CHIP_RESTART
        /* And the stretch before, while it is carried by the same stream (the player may still hold its tags). */
        if (track->has_prev && track->prev_stream == track->stream_start) {
            if (xdj_feed_prev_record(track)) {
                XDJ_PORT_WRITE(XDJ_CHIP_RECORDS + 16u * (track->prev_number % XDJ_CHIP_RECORD_COUNT) + 12u,
                               track->stream_bytes);
            } else {
                track->has_prev = 0;
            }
        }
#endif
#ifdef XDJ_CHIP_PLACES
            number = track->number;
            stretch_first = track->stretch_first;
        }
        tag = XDJ_CHIP_TAG_MARK | (number << XDJ_CHIP_TAG_NUMBER_SHIFT) | (at - stretch_first);
#else
        tag = XDJ_CHIP_TAG_MARK | (track->number << XDJ_CHIP_TAG_NUMBER_SHIFT) | (at - track->stretch_first);
#endif
        for (index = 0; index < part; index++) {
            unsigned char* bytes = output + (done + index) * 4u;
            unsigned value = XDJ_CHIP_TAG_FOLD(tag + index);

            bytes[0] = (unsigned char)value;
            bytes[1] = (unsigned char)(value >> 8);
            bytes[2] = (unsigned char)(value >> 16);
            bytes[3] = (unsigned char)(value >> 24);
        }
        done += part;
        track->next = at + part;
    }
    return done;
}
