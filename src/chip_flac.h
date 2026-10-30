/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * FLAC decoded on the audio processor, fed by the main processor through the
 * PCM path: what the two sides share.
 *
 * The bridge on the main processor presents a FLAC as a 16-bit stereo PCM
 * file, as it always has, but in place of decoded audio it hands out a tag
 * for every stereo frame, and before that it puts the FLAC file's own bytes
 * into a ring in the audio processor's free memory. On the audio processor,
 * the stock table of PCM converter routines has its 16-bit stereo entry
 * pointed at a routine of ours, which decodes the tagged frames out of the
 * ring and leaves everything else to the stock converter.
 *
 * Memory, all in the stretch of the audio processor's external memory that
 * its stock program never touches, 0xC1F3F420 to 0xC1FFFFFF;
 * src/chip_flac_loader.c puts the block there. What the
 * audio processor writes and what the main processor writes never share a
 * cache line (64 bytes): our routine runs with the cache on, and a line it
 * has changed is written back whole.
 */
#ifndef XDJ_CHIP_FLAC_H
#define XDJ_CHIP_FLAC_H

/* The block: the status words, then our code and data (src/chip_flac_layout.ld). */
#define XDJ_CHIP_BASE            0xC1F40000u
#define XDJ_CHIP_STATUS_BYTES    0x00000200u  /* the status words' room; the code follows */
#ifdef XDJ_BIG_BLOCKS
/*
 * Build switch XDJ_BIG_BLOCKS: a FLAC block of up to
 * 16,384 stereo frames needs about 136 KB of decoder memory. Our block uses
 * less than 48 KB of its 96, so its room ends earlier and the decoder's
 * memory grows to 160 KB (src/chip_flac_big_layout.ld); the ring and
 * what follows stay where they are.
 */
#define XDJ_CHIP_BLOCK_LIMIT     0xC1F50000u
#define XDJ_CHIP_STACK_TOP       0xC1F54000u  /* 16 KB of stack below this */
#define XDJ_CHIP_ARENA           0xC1F54000u  /* the decoder's memory */
#define XDJ_CHIP_ARENA_BYTES     0x00028000u
#else
#define XDJ_CHIP_BLOCK_LIMIT     0xC1F58000u
#define XDJ_CHIP_STACK_TOP       0xC1F5C000u  /* 16 KB of stack below this */
#define XDJ_CHIP_ARENA           0xC1F5C000u  /* the decoder's memory */
#define XDJ_CHIP_ARENA_BYTES     0x00020000u
#endif
#define XDJ_CHIP_MEMORY_END      0xC2000000u
/* Bit 0 makes the 16 MB of external memory from 0xC1000000 cacheable. */
#define XDJ_CHIP_CACHE_REGION    0x01848304u
#ifdef XDJ_CHIP_TICK_STACK
/*
 * Build switch XDJ_CHIP_TICK_STACK: the output
 * interrupt, which runs the whole audio engine every 16 frames, is kept off
 * our stack. Its vector's two address words (an mvk and an mvkh of the
 * routine's address into b0), in the table the processor uses (the stock
 * program runs a copy of its table in internal memory), are pointed at
 * xdj_chip_tick (src/chip_flac_glue.S) once, at our first unit, after
 * checking they hold the stock routine; they are written back from the data
 * cache and the program cache is emptied. The constant sits in bits 7 to 22.
 */
#define XDJ_CHIP_TICK_VECTOR_AT  0x104u       /* the words in the table the processor uses (ISTP) */
#define XDJ_CHIP_DATA_CACHE_BACK 0x01845040u  /* L1DWB */
#define XDJ_CHIP_TICK_STOCK_LOW  0x0011B02Au  /* mvk .S2 0x2360, b0 */
#define XDJ_CHIP_TICK_STOCK_HIGH 0x0008C06Au  /* mvkh .S2 0x11800000, b0 */
#define XDJ_CHIP_TICK_FIELD      (0xFFFFu << 7)
#define XDJ_CHIP_PROGRAM_CACHE_EMPTY 0x01845028u  /* L1PINV */
#endif
/*
 * What the main processor's loader leaves at XDJ_CHIP_BASE + XDJ_CHIP_LOADER_NOTE,
 * after the status words: its mark, the words it wrote, how many read back
 * differently, the address of the first of those, and the milliseconds the
 * load took.
 */
#define XDJ_CHIP_LOADER_NOTE     0x1E0u
#define XDJ_CHIP_LOADER_MAGIC    0x4C4A4458u  /* "XDJL" */
/* The mark at the start of what the build appends for the loader: then the block's words, 0, and the block's words again. */
#define XDJ_CHIP_BLOB_MAGIC      0x444A4458u  /* "XDJD" */

/* The stock table of converter routines (internal memory) and its 16-bit stereo little-endian entry. */
#define XDJ_CHIP_TABLE_ENTRY     0x1182E908u
#define XDJ_CHIP_STOCK_CONVERTER 0xC003C558u
/* Our routine: the first code of the block. */
#define XDJ_CHIP_ENTRY           (XDJ_CHIP_BASE + XDJ_CHIP_STATUS_BYTES)
/* A converter's right channel lies this many bytes after its left. */
#define XDJ_CHIP_RIGHT_OFFSET    3528u
#define XDJ_CHIP_UNIT_MOST       640u         /* frames in a unit: 588 at 44.1 kHz, 640 at 48 kHz */

/* Written by the audio processor: its status, at XDJ_CHIP_BASE. */
#define XDJ_CHIP_STATUS          XDJ_CHIP_BASE
#define XDJ_CHIP_STATUS_MAGIC    0x464A4458u  /* "XDJF" */
/* Written by the main processor: its control words, then the stretch records. */
#define XDJ_CHIP_CONTROL         0xC1F7E000u
#define XDJ_CHIP_CONTROL_BYTES   0x00001000u
#define XDJ_CHIP_CONTROL_MAGIC   0x434A4458u  /* "XDJC" */
#define XDJ_CHIP_RECORDS         (XDJ_CHIP_CONTROL + 0x40u)
#define XDJ_CHIP_RECORD_COUNT    64u
/* The ring of FLAC bytes. Places in it are counted on without end; the address is the count modulo its size. */
#define XDJ_CHIP_RING            0xC1F80000u
#define XDJ_CHIP_RING_BYTES      0x00080000u
/* Decoded frames on their way to the converter's output: 1,024 stereo frames of 32 bits. */
#define XDJ_CHIP_SCRATCH         0xC1F7C000u
#define XDJ_CHIP_SCRATCH_FRAMES  1024u

/*
 * A tag: the four bytes of one 16-bit stereo frame, read as a little-endian
 * word. Two mark bits, the stretch's number, the frame's number within the
 * stretch.
 */
#define XDJ_CHIP_TAG_MARK        0x80000000u
#define XDJ_CHIP_TAG_MARK_MASK   0xC0000000u
#define XDJ_CHIP_TAG_NUMBER_SHIFT 20u
#define XDJ_CHIP_TAG_NUMBERS     1024u
#define XDJ_CHIP_TAG_FRAMES      0x00100000u  /* frames in a stretch at most */
/*
 * On its way a tag has its low 16 bits inverted while bit 16 is set (doing
 * it twice gives the tag back). Should tags ever reach the outputs as audio,
 * the left channel is then a slow triangle without steps, not a sawtooth
 * that clicks every second and a half; the right channel stands still.
 */
#define XDJ_CHIP_TAG_FOLD(word)  (((word) & 0x00010000u) != 0u ? (word) ^ 0x0000FFFFu : (word))
/* A run of tags at a unit's start is taken for ours from this length on (a whole unit always is). */
#define XDJ_CHIP_RUN_LEAST       8u
/* So the bridge ends every track with this many frames of plain silence: no shorter run can arise. */
#define XDJ_CHIP_TAIL_FRAMES     8u

/*
 * A stretch record (four words). A stream is a FLAC stream of its own in
 * the ring: a 42-byte header (the file's own, marked as the last metadata
 * block and with the length left open), then the file's bytes from the
 * start of one FLAC frame on. A jump starts a new stream; a stretch that
 * outgrows its frame numbers goes on in a new stretch of the same stream.
 */
typedef struct {
    unsigned id;            /* XDJ_CHIP_RECORD_LIVE + the stretch's number */
    unsigned stream_start;  /* place in the ring of the stream's first byte */
    unsigned base_frames;   /* frames from the stream's first sample to this stretch's frame 0 */
    unsigned length;        /* bytes of the stream in the ring so far */
} xdj_chip_record;
#define XDJ_CHIP_RECORD_LIVE     0x52000000u
#define XDJ_CHIP_STREAM_HEADER   42u
#ifdef XDJ_CHIP_HIRES
/*
 * Hi-res FLAC (88.2 and 96 kHz, build switch XDJ_CHIP_HIRES): the player is given the track at half the rate, as the bridge has
 * always given it, and a tag stands for one frame at that rate; the audio
 * processor decodes two frames of the file for each and halves them with the
 * bridge's own filter. Such a stretch's record has this id in place of
 * XDJ_CHIP_RECORD_LIVE, and its base_frames counts frames of the file (from
 * the stream's first sample to twice the stretch's frame 0). A stream starts
 * this many frames at half the rate before the frame asked for, as the
 * bridge's seek does: the filter's history is then the file's, and every
 * frame comes out as it does from the bridge.
 */
#define XDJ_CHIP_RECORD_HALVED   0x53000000u
#define XDJ_CHIP_RECORD_KIND     0xFF000000u
#define XDJ_CHIP_HALVED_WARMUP   32u
#endif

/* The main processor's control words at XDJ_CHIP_CONTROL. */
typedef struct {
    unsigned magic;
    unsigned pushed;        /* place in the ring after the last byte put there */
} xdj_chip_control;

/* The audio processor's status words at XDJ_CHIP_STATUS. */
typedef struct {
    unsigned magic;
    unsigned units;         /* units that reached our routine's C part */
    unsigned units_ours;    /* of those, units with tags */
    unsigned frames;        /* tagged frames served, low word */
    unsigned frames_high;
    unsigned opens;         /* streams opened */
    unsigned skips;         /* frames decoded and dropped to reach a position */
    unsigned no_record;     /* runs whose stretch record was not there */
    unsigned lost;          /* runs whose bytes had already been overwritten in the ring */
    unsigned short_reads;   /* runs the decoder could not fill */
    unsigned open_failures;
    unsigned plain_frames;  /* untagged frames in units of ours, converted as PCM */
    /*
     * How far the decoder is, for the main processor's reckoning of how many
     * bytes the next frames can need: sequence is odd while the three words
     * after it change.
     */
    unsigned sequence;
    unsigned at_stream;     /* stream_start of the stream being decoded */
    unsigned at_frames;     /* frames from the stream's first sample decoded so far (to the end of the block in hand) */
    unsigned at_bytes;      /* bytes of the stream read so far */
    unsigned cycles_low;    /* processor cycles in our routine, 64 bits */
    unsigned cycles_high;
    unsigned longest;       /* cycles of one unit */
    unsigned region_now;    /* 0x01848304 as read inside our routine, plus 0x100 */
#ifdef XDJ_CHIP_RESTART
    /*
     * A way out when the decoder cannot go on in a stream (build switch
     * XDJ_CHIP_RESTART): the place it was asked for
     * lies too far ahead, or its bytes are not in the ring. Then the
     * converter stops trying in that stream, writes silence for its tags,
     * and asks the main processor for a new stream from that place: the
     * stretch's number and the frame in it, under ask_sequence (odd while
     * the two words after it change). The main processor starts a stream
     * there and points the stretch's record at it, as a jump does; the tags
     * the player holds stay as they are.
     */
    unsigned ask_sequence;
    unsigned ask_number;
    unsigned ask_frame;
    unsigned ask_stream;    /* stream_start of the stream it is stuck in */
    unsigned asks;          /* times a stream got stuck */
    unsigned stuck_units;   /* units written as silence while waiting for the new stream */
#endif
} xdj_chip_status;


#endif
