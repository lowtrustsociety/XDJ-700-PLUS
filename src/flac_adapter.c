/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * XDJ-700 FLAC adapter bridge.
 *
 * The adapter exposes decoded FLAC as a virtual stereo PCM byte stream (with
 * XDJ_MONO a mono file too, each sample on both sides), signed 16-bit or
 * packed 24-bit, matching the stock WAV and AIFF descriptor
 * contract. The stock PCM path remains responsible for buffering and DSP
 * delivery. WAV and AIFF files the stock readers refuse (88.2 and 96 kHz,
 * extensible WAV headers) are served through the same bridge.
 *
 * Each track gets its own decoder slot, found by the file a call reads.
 * Parsing another track therefore never disturbs the decoder of a track that
 * is still playing. The stock track cache keeps up to five parsed descriptors
 * and reuses them without a new dispatch, so an evicted decoder is remembered
 * and reopened on demand. File I/O moves from the parser's shared file slot
 * to the input-file task's own handle as soon as that task touches the track,
 * so the decoder has the same handle lifetime as stock WAV playback.
 */

#include <stddef.h>

#ifdef XDJ_HOST_DEBUG_FAILURES
#include <stdio.h>
#endif
#include "dr_flac.h"
#include "xdj_flac_seek.h"
#ifdef XDJ_CHIP_FLAC
/*
 * Built for the release (--release): a FLAC at 44.1 or 48 kHz is fed to the
 * audio processor. The stream stays a virtual 16-bit PCM file, but its frames
 * are tags and the file's bytes go into the audio processor's memory
 * (src/chip_flac.h). Our block is put there
 * at the first FLAC open after power-on (xdj_chip_load in
 * src/chip_flac_loader.c); if the audio processor is started over,
 * fed tracks are decoded here again, still as 16-bit streams, and the next
 * open puts the block back.
 */
#include "chip_flac_feed.h"
int xdj_chip_load(void);
drflac_bool32 xdj_flac_locate(
    drflac* decoder,
    drflac_uint64 frame_index,
    xdj_flac_seek_state* state,
    drflac_uint32* offset,
    drflac_uint32* first
);
#endif


/* Parser file wrappers: (context, ...) with the file handle at context + 4. */
#define XDJ_FW_SEEK_ADDRESS 0x08C54DBAUL
#define XDJ_FW_READ_ADDRESS 0x08C54DFCUL
#define XDJ_FW_TELL_ADDRESS 0x08C54E70UL
#define XDJ_FW_SIZE_ADDRESS 0x08C54EB0UL
/* RTOS task delay, called by the stock read at end of data (0xBE8FAA). */
#define XDJ_TASK_DELAY_ADDRESS 0x08E0335CUL
/*
 * Paced fetching (build option XDJ_FETCH_PACE_MS).
 * The input manager keeps 25 to 30 s of audio ahead of the playing position
 * and tops it up in one run of reads. The screen task has a lower priority
 * than the input task, so while we decode in those reads it is not drawn.
 * With the option, a read of that top-up sleeps until it has taken
 * XDJ_FETCH_PACE_MS for each 1/75 s of audio it delivered. Playing in
 * reverse, the same holds for the audio behind the playing position.
 *  - the stock routine that reads an audio processor register;
 *  - the registers with the audio held ahead of the playing position and
 *    behind it, in 1/75 s units (what the manager's state routine 0x08BFB13C
 *    compares with 25 and 30 s);
 *  - the command the input task is running (its workspace 0x09D289F0 + 0x80;
 *    1 is the fill ahead, 2 the fill behind) and the word the manager sets
 *    to break it off (+ 0x74).
 */
#define XDJ_DSP_REGISTER_READ_ADDRESS 0x08BF2BCCUL
#define XDJ_DSP_AHEAD_REGISTER 0x11837CD0UL
#define XDJ_DSP_BEHIND_REGISTER 0x11837CCCUL
#define XDJ_INPUT_COMMAND_ADDRESS 0x09D28A70UL
#define XDJ_INPUT_ABORT_ADDRESS 0x09D28A64UL
/* The top-up pace needs this much in hand (24 s). */
#ifndef XDJ_FETCH_PACE_AHEAD_UNITS
#define XDJ_FETCH_PACE_AHEAD_UNITS 1800U
#endif
/*
 * The refill after a load or a jump, from nothing up to the top-up mark, can
 * have a pace of its own (XDJ_FETCH_REFILL_MS, 0 for none) once this much is
 * in hand (5 s). Below it every read runs at full speed, so a load, a jump
 * or a fast spin that drains the read-ahead is never slowed.
 */
#ifndef XDJ_FETCH_REFILL_MS
#define XDJ_FETCH_REFILL_MS 0U
#endif
#ifndef XDJ_FETCH_REFILL_AHEAD_UNITS
#define XDJ_FETCH_REFILL_AHEAD_UNITS 375U
#endif
/* More than the read-ahead can hold: not a level, so no pacing. */
#define XDJ_FETCH_PACE_AHEAD_LIMIT 3600U
/* The sleep is taken in 2-tick slices so a break-off is seen at once; this bounds it. */
#define XDJ_FETCH_PACE_MAX_SLICES 32U
/*
 * Diagnostics clock. Kernel start-up (0x08D1B4AC) turns TMU0 into the 1 ms
 * NORTi tick: the low word of the kernel time (0x0B0E5D10) counts
 * milliseconds, and TCNT0 counts down from 10,415 within each one. Time is
 * kept in sixteenths of a millisecond.
 */
#define XDJ_KERNEL_MS_ADDRESS 0x0B0E5D10UL
#define XDJ_TMU0_TCNT_ADDRESS 0xFFD8000CUL
#define XDJ_TMU0_TCR_ADDRESS 0xFFD80010UL
#define XDJ_TMU0_TCR_UNF 0x0100U
#define XDJ_TMU0_RELOAD 10415U
#define XDJ_UNITS_PER_MS 16U
/*
 * A load or cue served by the bridge is one episode: a seek that moves the
 * play position and the calls that follow it until the bridge has been idle
 * for XDJ_EPISODE_IDLE_MS. A FLAC open is an episode of its own.
 */
#define XDJ_EPISODE_IDLE_MS 20U
#define XDJ_EPISODE_JUMP 1U
#define XDJ_EPISODE_OPEN 2U
/* Calls that failed, or met a stale window, while no episode was running. */
#define XDJ_EPISODE_FAILURE 3U
#define XDJ_EPISODE_HISTORY 32U
/*
 * Each finished episode is reported in the decoded audio, which is muted
 * meanwhile: two 4 kHz marker tones, then one tone per field at 400 Hz plus
 * 40 Hz per unit (0 to 63). Tones last 60 ms with 20 ms gaps.
 * tools/decode_diag_tones.py reads them back from a recording.
 */
#define XDJ_REPORT_FIELDS 27U
/* Builds for normal playing set this to 0: episodes are still timed and kept, but not played. */
#ifndef XDJ_AUDIBLE_REPORTS
#define XDJ_AUDIBLE_REPORTS 1
#endif
/* Flags added to the kind field. */
#define XDJ_REPORT_FLAG_FALLBACK 4U
#define XDJ_REPORT_FLAG_CACHE_HIT 8U
#define XDJ_REPORT_FLAG_NO_SIZE 16U   /* the filesystem gave no file size */
#define XDJ_REPORT_FLAG_FAILED 32U    /* a call in the episode failed */
/*
 * A file snapshot is a report whose kind field is 63. It records the handle a
 * call read and the file the bridge expected when they disagree (a stale
 * window), and once for a normal handle, as a baseline. One is taken per reason, and they have
 * their own queue, played before episode reports.
 */
#define XDJ_SNAPSHOT_MARK 63U
#define XDJ_SNAPSHOT_QUEUE 4U
#define XDJ_SNAPSHOT_LIMIT 4U
#define XDJ_SNAPSHOT_BASELINE 1U     /* a track read through a new handle */
#define XDJ_SNAPSHOT_STALE 2U        /* a readable file that is no FLAC track */
#define XDJ_SNAPSHOT_FOREIGN 3U      /* a handle whose size cannot be read */
#define XDJ_SNAPSHOT_NO_HANDLE 4U    /* a zeroed handle */
/* The filesystem's device-to-type table, indexed by 12 x (device byte 17 | 32) (FS read, 0x08DDFD74). */
#define XDJ_FS_TYPE_TABLE_ADDRESS 0x0C8D8028UL
#define XDJ_REPORT_MARKS 2U
#define XDJ_REPORT_SYMBOLS (XDJ_REPORT_MARKS + XDJ_REPORT_FIELDS)
#define XDJ_REPORT_QUEUE 4U
#define XDJ_REPORT_MARK_HZ 4000U
#define XDJ_REPORT_BASE_HZ 400U
#define XDJ_REPORT_STEP_HZ 40U
#define XDJ_REPORT_MAX_VALUE 63U
/* Failures play long low beeps whose count names the failure. */
#define XDJ_SIGNAL_NONE 0U
#define XDJ_SIGNAL_FAILURE 5U     /* about 400 Hz, long: failure code */
#define XDJ_FAILURE_READ 1U
#define XDJ_FAILURE_SEEK 2U
#define XDJ_FAILURE_REOPEN 3U
#define XDJ_FAILURE_OPENING 4U
#define XDJ_SIGNAL_MAX_BEEPS 9U
#define XDJ_SIGNAL_AMPLITUDE 8192
/* A request for a track still being opened waits up to this many ticks. */
#define XDJ_OPEN_WAIT_TICKS 500U
/* Filesystem reads between voluntary yields during long decoder work. */
#define XDJ_READS_PER_YIELD 4U
/* Input-file task routines replaced by the PCM literal hooks. */
#define XDJ_STOCK_SEEK_ADDRESS 0x08BE8E58UL
#define XDJ_STOCK_READ_ADDRESS 0x08BE8F40UL
/*
 * The PCM path's end-of-stream test (context, fs_state): true when the raw
 * handle is at end of file or past data start + length.
 */
#define XDJ_STOCK_EOF_ADDRESS 0x08BE8DC2UL

/* Five stock track-cache entries plus one track being opened. */
#define XDJ_FLAC_SLOT_COUNT 6U
#define XDJ_FLAC_TOMBSTONE_COUNT 24U
#ifdef XDJ_BIG_BLOCKS
/*
 * Room for FLAC blocks of up to 16,384 stereo frames, the largest the
 * format's usual subset allows at 88.2 and 96 kHz:
 * 128 KB of decoded samples, dr_flac's read buffer and state, and a seek
 * table of a few hundred points.
 */
#define XDJ_FLAC_ARENA_BYTES 147456U
#else
#define XDJ_FLAC_ARENA_BYTES 98304U
#endif
#define XDJ_OUTPUT_PATH_OFFSET 12U
#define XDJ_TOTAL_CD_FRAMES_OFFSET 16U
/*
 * Stock path-type-1 contract (WAV adapter 0xC01F74, AIFF adapter 0xC020B0):
 * +20 data start, +24 PCM data length, +28 bytes after the data. The load
 * manager copies them into file-context offsets 4, 8, and 12, which bound
 * every stock read and seek.
 */
#define XDJ_DATA_START_OFFSET 20U
#define XDJ_DATA_BYTES_OFFSET 24U
#define XDJ_TRAILING_BYTES_OFFSET 28U
#define XDJ_PCM_FORMAT_OFFSET 340U
#define XDJ_CD_FRAMES_PER_SECOND 75U
#define XDJ_SEEK_RANGE_ERROR (-2)
#define XDJ_SEEK_IO_ERROR (-1)

/*
 * Private copy of a parser file context. The wrappers read the handle at +4,
 * pass +8 to the filesystem as its state block, and record the last operation
 * and error at +36 and +40.
 */
typedef struct {
    drflac_uint32 reserved;
    void* handle;
    unsigned char fs_state[28];
    drflac_uint32 last_operation;
    drflac_int32 last_error;
} xdj_fw_file_context;

typedef int (*xdj_fw_seek_proc)(xdj_fw_file_context* context, int offset, int origin);
typedef size_t (*xdj_fw_read_proc)(
    xdj_fw_file_context* context,
    void* output,
    size_t byte_count,
    int* status
);
typedef int (*xdj_fw_tell_proc)(xdj_fw_file_context* context);
/* Returns 0 and stores the file size; the per-filesystem routines only read the handle. */
typedef int (*xdj_fw_size_proc)(xdj_fw_file_context* context, drflac_uint32* size);
typedef int (*xdj_stock_seek_proc)(void* file_context, int offset, int origin, void* fs_state);
typedef int (*xdj_task_delay_proc)(int ticks);
typedef drflac_uint32 (*xdj_dsp_register_read_proc)(drflac_uint32 dsp_address);
typedef int (*xdj_stock_eof_proc)(void* file_context, void* fs_state);
typedef size_t (*xdj_stock_read_proc)(
    void* file_context,
    void* output,
    size_t byte_count,
    int* status,
    void* fs_state
);

#ifdef XDJ_HOST_TEST
/* Host mocks receive the handle; runners pass their mock file as the handle. */
extern int xdj_host_mock_seek(void* handle, int offset, int origin);
extern size_t xdj_host_mock_read(void* handle, void* output, size_t byte_count, int* status);
extern int xdj_host_mock_tell(void* handle);
/* Size queries made, for runners that check a call costs no network request. */
drflac_uint32 xdj_host_size_queries = 0;
static int xdj_host_file_size(void* handle, drflac_uint32* size)
{
    int saved = handle != NULL ? xdj_host_mock_tell(handle) : -1;
    int end;

    xdj_host_size_queries++;

    if (saved < 0 || xdj_host_mock_seek(handle, 0, 2) != 0) {
        return -1;
    }
    end = xdj_host_mock_tell(handle);
    xdj_host_mock_seek(handle, saved, 0);
    if (end < 0) {
        return -1;
    }
    *size = (drflac_uint32)end;
    return 0;
}
#define XDJ_FW_SEEK_CALL(io, off, orig) xdj_host_mock_seek((io)->handle, off, orig)
#define XDJ_FW_READ_CALL(io, out, cnt, stat) xdj_host_mock_read((io)->handle, out, cnt, stat)
#define XDJ_FW_TELL_CALL(io) xdj_host_mock_tell((io)->handle)
#define XDJ_FW_SIZE_CALL(io, size) xdj_host_file_size((io)->handle, size)
#define XDJ_TASK_DELAY_CALL(ticks) ((void)(ticks))
/* Host runners advance this clock, in sixteenths of a millisecond. */
drflac_uint32 xdj_host_clock_units = 0;
#define XDJ_TIMER_NOW() (xdj_host_clock_units)
#ifdef XDJ_FETCH_PACE_MS
/* Host runners set the player's state; a pacing sleep advances the clock and is counted. */
drflac_uint32 xdj_host_input_command = 0;
drflac_uint32 xdj_host_input_abort = 0;
drflac_uint32 xdj_host_ahead_units = 0;
drflac_uint32 xdj_host_behind_units = 0;
drflac_uint32 xdj_host_pace_sleeps = 0;
#define XDJ_INPUT_COMMAND() (xdj_host_input_command)
#define XDJ_INPUT_ABORT() (xdj_host_input_abort)
#define XDJ_DSP_REGISTER(dsp_address) \
    ((dsp_address) == XDJ_DSP_AHEAD_REGISTER ? xdj_host_ahead_units : xdj_host_behind_units)
#define XDJ_PACE_SLEEP(ticks) ((void)(xdj_host_clock_units += (ticks) * XDJ_UNITS_PER_MS, xdj_host_pace_sleeps++))
#endif
#define XDJ_PARSER_HANDLE(parser_context) (parser_context)
/*
 * Host runners pass a struct whose first member is the FILE; the context is
 * the handle, and a zeroed first member stands for a zeroed handle, as on the
 * player (where the handle is the context's first word).
 */
#define XDJ_TASK_HANDLE(file_context) \
    (*(void* const*)(file_context) != NULL ? (void*)(file_context) : NULL)
#ifdef XDJ_HOST_CONTEXT_DATA_BYTES
/* Runners that model the file context put the data length and the data start right after the FILE. */
typedef struct {
    void* handle;
    drflac_uint32 data_bytes;
    drflac_uint32 data_start;
} xdj_host_context_layout;
#define XDJ_TASK_DATA_BYTES(file_context) (((const xdj_host_context_layout*)(file_context))->data_bytes)
#define XDJ_TASK_DATA_START(file_context) (((const xdj_host_context_layout*)(file_context))->data_start)
#else
#define XDJ_TASK_DATA_BYTES(file_context) ((void)(file_context), 0U)
#define XDJ_TASK_DATA_START(file_context) ((void)(file_context), 0U)
#endif
/* Calls handed on to the stock seek and read, for runners that check where a call ended up. */
drflac_uint32 xdj_host_stock_calls = 0;
#define XDJ_STOCK_SEEK_CALL(ctx, off, orig, state) \
    ((void)(ctx), (void)(off), (void)(orig), (void)(state), xdj_host_stock_calls++, XDJ_SEEK_IO_ERROR)
#define XDJ_STOCK_READ_CALL(ctx, out, cnt, stat, state) \
    ((void)(ctx), (void)(out), (void)(cnt), (void)(state), xdj_host_stock_calls++, \
     ((stat) != NULL ? (void)(*(stat) = -1) : (void)0), (size_t)0)
#define XDJ_STOCK_EOF_CALL(ctx, state) ((void)(ctx), (void)(state), 0)
#define XDJ_CLEAR_CONTEXT_STATUS(ctx) ((void)(ctx))
#else
#define XDJ_FW_SEEK_CALL(io, off, orig) (((xdj_fw_seek_proc)XDJ_FW_SEEK_ADDRESS)(io, off, orig))
#define XDJ_FW_READ_CALL(io, out, cnt, stat) (((xdj_fw_read_proc)XDJ_FW_READ_ADDRESS)(io, out, cnt, stat))
#define XDJ_FW_TELL_CALL(io) (((xdj_fw_tell_proc)XDJ_FW_TELL_ADDRESS)(io))
#define XDJ_FW_SIZE_CALL(io, size) (((xdj_fw_size_proc)XDJ_FW_SIZE_ADDRESS)(io, size))
#define XDJ_TASK_DELAY_CALL(ticks) ((void)((xdj_task_delay_proc)XDJ_TASK_DELAY_ADDRESS)(ticks))
#define XDJ_TIMER_NOW() xdj_timer_now()
#define XDJ_INPUT_COMMAND() (*(volatile const drflac_uint32*)XDJ_INPUT_COMMAND_ADDRESS)
#define XDJ_INPUT_ABORT() (*(volatile const drflac_uint32*)XDJ_INPUT_ABORT_ADDRESS)
#define XDJ_DSP_REGISTER(dsp_address) \
    (((xdj_dsp_register_read_proc)XDJ_DSP_REGISTER_READ_ADDRESS)(dsp_address))
#define XDJ_PACE_SLEEP(ticks) XDJ_TASK_DELAY_CALL(ticks)
#define XDJ_PARSER_HANDLE(parser_context) \
    (*(void* const*)((const unsigned char*)(parser_context) + 4))
#define XDJ_TASK_HANDLE(file_context) (*(void* const*)(file_context))
/* The data length the load manager copied from the descriptor (+24) into the file context. */
#define XDJ_TASK_DATA_BYTES(file_context) (*(const drflac_uint32*)((const unsigned char*)(file_context) + 8))
/* Where the audio starts in the file (the descriptor's +20): the stock seek adds it to every offset. */
#define XDJ_TASK_DATA_START(file_context) (*(const drflac_uint32*)((const unsigned char*)(file_context) + 4))
#define XDJ_STOCK_SEEK_CALL(ctx, off, orig, state) \
    (((xdj_stock_seek_proc)XDJ_STOCK_SEEK_ADDRESS)(ctx, off, orig, state))
#define XDJ_STOCK_READ_CALL(ctx, out, cnt, stat, state) \
    (((xdj_stock_read_proc)XDJ_STOCK_READ_ADDRESS)(ctx, out, cnt, stat, state))
#define XDJ_STOCK_EOF_CALL(ctx, state) (((xdj_stock_eof_proc)XDJ_STOCK_EOF_ADDRESS)(ctx, state))
/* The stock test clears the context's last operation and error (+16, +20) first. */
#define XDJ_CLEAR_CONTEXT_STATUS(ctx) \
    ((void)(((drflac_uint32*)(ctx))[4] = 0U), (void)(((drflac_uint32*)(ctx))[5] = 0U))
#endif

#define XDJ_WATCH_ENTER(site)

#if defined(XDJ_OWN_STACK) && !defined(XDJ_HOST_TEST)
/*
 * v0.9 on (build switch XDJ_OWN_STACK): the player's
 * file task (tsk_DJcontInBufFile) has 2 KB of stack, and with our read on it
 * the player's own file seek below ours ran past the bottom after a far hot
 * cue jump. So every read, seek and end test that comes from that task runs
 * on the 8 KB of src/fed_stack.S, and the player's file calls below
 * ours with it; the task keeps only what the player itself had used before
 * calling us. Only the file task makes these calls (the opens come from the
 * open task, on its own 3 KB), so nothing else is switched, and only one
 * task is ever on that stack, even while a read waits (our pacing). The
 * entries the player calls are the wrappers further down; the work is done
 * by the *_here functions. Without the switch none of this exists.
 */
#define XDJ_FILE_TASK_STACK_BASE 0x09D2705CU
#define XDJ_FILE_TASK_STACK_BYTES 2048U
#define XDJ_OWN_STACK_BYTES 8192U
extern unsigned char xdj_fed_stack[];
unsigned xdj_call_on_fed_stack(unsigned (*fn)(void*, unsigned, unsigned char*), void* a, unsigned b, unsigned char* c);

static drflac_uint32 xdj_stack_pointer(void)
{
    drflac_uint32 sp;

    __asm__ volatile("mov r15,%0" : "=r"(sp));
    return sp;
}

static drflac_bool32 xdj_in_file_task(void)
{
    return xdj_stack_pointer() - XDJ_FILE_TASK_STACK_BASE < XDJ_FILE_TASK_STACK_BYTES;
}

#ifdef XDJ_CHIP_FLAC
/* Only the fed read asks (a build with the stack fix but without FLAC on the audio processor has none). */
static drflac_bool32 xdj_on_own_stack(void)
{
    return xdj_stack_pointer() - (drflac_uint32)xdj_fed_stack < XDJ_OWN_STACK_BYTES;
}
#endif
/* With the recorder, the wrappers note the entry, on the file task's stack, before they switch. */
#define XDJ_WATCH_BODY_ENTER(site)
#else
#define XDJ_WATCH_BODY_ENTER(site) XDJ_WATCH_ENTER(site)
#endif

typedef struct {
    drflac_uint32 sample_rate;
    drflac_uint64 frame_count;
    drflac_uint8 channels;
    drflac_uint8 bits_per_sample;
} xdj_flac_stream_info;

typedef union {
    drflac_uint64 alignment;
    unsigned char bytes[XDJ_FLAC_ARENA_BYTES];
} xdj_flac_arena_storage;

/*
 * Where one bridge call spent its time, in sixteenths of a millisecond. The
 * reopen, dr_flac seek, and decode times include their filesystem time.
 */
typedef struct {
    drflac_uint32 fs_seek;
    drflac_uint32 fs_read;
    drflac_uint32 wait;
    drflac_uint32 reopen;
    drflac_uint32 flac_seek;
    drflac_uint32 decode;
    drflac_uint32 fs_reads;
    drflac_uint32 physical_seeks;
    /* Frame-header probes by the bounded seek, cache hits, and fallbacks to dr_flac's seek. */
    drflac_uint32 probes;
    drflac_uint32 cache_hits;
    drflac_uint32 fallbacks;
    /* Filesystem calls that reported an error, and the last one's code and operation. */
    drflac_uint32 fs_errors;
    drflac_int32 fs_error;
    drflac_uint32 fs_operation;
    /* A FLAC slot served the call; the call was a read; a seek moved the play position. */
    drflac_uint8 served;
    drflac_uint8 is_read;
    drflac_uint8 moved;
    /* The call's XDJ_FAILURE code and the bounded seek's XDJ_SEEK_FAIL reason. */
    drflac_uint8 failure;
    drflac_uint8 seek_failure;
    drflac_uint8 no_size;
    /*
     * Handle positions found wrong: after our seek (misplaced), and after our
     * read (moved); decoded frames whose number disagreed with the position.
     */
    drflac_uint32 misplaced;
    drflac_uint32 handle_moved;
    drflac_uint32 sequence_breaks;
    /*
     * Stale calls (a zeroed handle, or a handle lost during the call) and
     * calls whose handle size could not be read (a closed handle), all
     * answered with silence or a no-op; end-of-stream answers that said
     * "ended".
     */
    drflac_uint32 stale_calls;
    drflac_uint32 foreign_stale;
    drflac_uint32 eof_ends;
} xdj_call_breakdown;

/* One load, cue, or open served by the bridge. Times in sixteenths of a ms. */
typedef struct {
    drflac_uint32 sequence;
    drflac_uint32 kind;
    drflac_uint32 start;
    drflac_uint32 end;
    /* From the start to the end of the first read, once has_first_read is set. */
    drflac_uint32 first_read;
    drflac_uint32 has_first_read;
    drflac_uint32 busy;
    drflac_uint32 fs_seek;
    drflac_uint32 fs_read;
    drflac_uint32 wait;
    drflac_uint32 reopen;
    drflac_uint32 flac_seek;
    drflac_uint32 decode;
    drflac_uint32 calls;
    drflac_uint32 fs_reads;
    drflac_uint32 physical_seeks;
    drflac_uint32 probes;
    drflac_uint32 cache_hits;
    drflac_uint32 fallbacks;
    drflac_uint32 fs_errors;
    drflac_int32 fs_error;
    drflac_uint32 fs_operation;
    drflac_uint32 failure;
    drflac_uint32 seek_failure;
    drflac_uint32 no_size;
    drflac_uint32 misplaced;
    drflac_uint32 handle_moved;
    drflac_uint32 sequence_breaks;
    drflac_uint32 stale_calls;
    drflac_uint32 foreign_stale;
    drflac_uint32 eof_ends;
} xdj_episode;

/* Playback position inside one audible report. */
typedef struct {
    drflac_uint8 fields[XDJ_REPORT_FIELDS];
    drflac_uint32 symbol;
    drflac_uint32 position;
    drflac_uint32 phase;
} xdj_report_player;

/*
 * What a slot's track is. FLAC goes through dr_flac. WAV and AIFF files the
 * stock readers refuse (88.2 and 96 kHz, extensible WAV headers) are read
 * as sample bytes through xdj_pcm_source; for those, decoder only marks the
 * slot as in use and is never handed to dr_flac.
 */
#define XDJ_KIND_FLAC 0U
#define XDJ_KIND_WAV 1U
#define XDJ_KIND_AIFF 2U
/* The dispatcher's file types (descriptor +8): 11 WAV, 12 AIFF, 5 FLAC. */
#define XDJ_FILE_TYPE_OFFSET 8U
#define XDJ_FILE_TYPE_WAV 11U
#define XDJ_FILE_TYPE_AIFF 12U
/* Adapter result for a WAV or AIFF file that is left to the stock reader. */
#define XDJ_ADAPTER_DECLINED 1

typedef struct {
    drflac* decoder;
    void* format_context;
    drflac_uint64 frame_count;
    drflac_uint64 cursor_frame;
    drflac_uint32 declared_bytes;
    drflac_uint32 sample_rate;
    drflac_uint32 file_bytes;
    drflac_uint32 stream_position;
    drflac_uint8 kind;
    /*
     * The virtual PCM stream is byte-exact, like a WAV file: 4 bytes per
     * frame for 16-bit sources, 6 for 24-bit. The player may read it in
     * chunks that split a frame (it can size reads by another track's
     * format). frame_offset counts the bytes of frame cursor_frame already
     * delivered. While carry_valid is set, carry holds that frame and the
     * decoder stands one frame past cursor_frame; otherwise it stands at
     * cursor_frame.
     */
    drflac_uint8 bytes_per_frame;
    drflac_uint8 frame_offset;
    drflac_uint8 carry_valid;
    unsigned char carry[6];
    /*
     * 88.2 and 96 kHz sources are converted down while decoding. Frame counts
     * and positions in this file are converted frames. reposition is set when
     * a conversion read stopped early, leaving the decoder off the play
     * position: the next read seeks first. The converter's state is kept
     * apart (xdj_flac_decimators) so it takes no room in the flashed image.
     */
    drflac_uint8 reposition;
    /* Breakdown of the bridge call currently using this slot, or NULL. */
    xdj_call_breakdown* breakdown;
    drflac_uint32 last_use;
    size_t arena_used;
    xdj_fw_file_context io;
    /* Bounded-seek cache and statistics for this decoder. */
    xdj_flac_seek_state seek;
    /* Reserved by a dispatch whose open runs without the lock. */
    drflac_uint8 opening;
    /*
     * The handle stopped being this track's file during the current call
     * (closed or reused by the player); and the decoder must search afresh
     * before its next read, since its read-ahead may hold such bytes.
     */
    drflac_uint8 handle_lost;
    drflac_uint8 resync;
    /*
     * Size checks by filesystem (xdj_strict_filesystem): on FAT the size is a
     * field of the open handle, so it is checked on every call and after
     * every read. On a network filesystem (LINK, NFS) a size query is a
     * network request, so the track is identified by the file context's data
     * length instead, and only the (local) position check follows each read.
     */
    drflac_uint8 strict_checks;
    /*
     * The handle our last filesystem call used and the position we left it
     * at. On a network filesystem, finding the same handle at the same place
     * means nobody touched it; otherwise its size is checked once (a network
     * request) before it is trusted.
     */
    void* known_handle;
    int known_position;
#ifdef XDJ_CHIP_FLAC
    /*
     * The track is fed to the audio processor: reads give tags, and
     * fed_position is the frame the next one starts at (what the decoder's
     * own position is otherwise). fed_form says the player was given the
     * track as a fed one, a 16-bit stream whatever the file's depth: it stays
     * so when the bridge decodes it again (fed 0). fed_ready says the open
     * found the file fit for it, under load number fed_load.
     */
    drflac_uint8 fed;
    drflac_uint8 fed_form;
    drflac_uint8 fed_ready;
    drflac_uint32 fed_load;
    drflac_uint32 fed_position;
#endif
} xdj_flac_slot;

/*
 * Shared state read by the assembly trampolines and lock. Offsets 0, 4, and
 * 44 are fixed by pcm_route_trampolines.S and decoder_lock.S. The counters are
 * diagnostics for inspection through the serial shell's memory dump.
 */
typedef struct {
    /* The last format object a PCM trampoline saw; calls carry their own. Host runners select with it. */
    void* request_format;
    drflac_uint32 routed_descriptors;
    drflac_uint32 use_clock;
    drflac_uint32 open_count;
    drflac_uint32 open_failures;
    drflac_uint32 seek_failures;
    drflac_uint32 read_failures;
    drflac_uint32 read_retries;
    drflac_uint32 handle_switches;
    drflac_uint32 repositions;
    drflac_uint32 evictions;
    volatile unsigned char decoder_lock;
    drflac_uint32 reopens;
    drflac_uint32 reopen_failures;
    drflac_uint32 tombstone_next;
    drflac_uint32 io_reads;
    drflac_uint32 yields;
    drflac_uint32 error_delays;
    /* Failure beeps. */
    drflac_uint32 signals_played;
    drflac_uint32 pending_kind;
    drflac_uint32 pending_beeps;
    drflac_uint32 sequence_kind;
    drflac_uint32 sequence_beeps_left;
    drflac_uint32 sequence_position;
    drflac_uint32 sequence_phase;
    drflac_int32 sequence_level;
    drflac_uint32 physical_seeks;
    drflac_uint32 lazy_seeks;
    drflac_uint32 opening_waits;
    drflac_uint32 last_failure;
    drflac_uint32 fs_errors;
    drflac_uint32 misplaced;
    drflac_uint32 handle_moved;
    drflac_uint32 stale_calls;
    drflac_uint32 foreign_stale;
    drflac_uint32 eof_ends;
    drflac_uint32 handles_lost;
    drflac_uint32 resyncs;
    drflac_uint32 network_size_checks;
    drflac_uint32 duplicate_parses;
    drflac_uint32 snapshots_taken;
    /* Bit per snapshot reason already recorded: one snapshot per reason. */
    drflac_uint32 snapshot_reasons;
    drflac_uint32 snapshot_head;
    drflac_uint32 snapshot_count;
    drflac_uint8 snapshot_queue[XDJ_SNAPSHOT_QUEUE][XDJ_REPORT_FIELDS];
    /* Episode timing (sixteenths of a ms) and the audible report queue. */
    xdj_episode episode;
    drflac_uint32 episode_active;
    drflac_uint32 episode_sequence;
    drflac_uint32 max_jump_units;
    drflac_uint32 max_open_units;
    drflac_uint32 history_count;
    drflac_uint32 reports_queued;
    drflac_uint32 reports_dropped;
    drflac_uint32 reports_played;
    drflac_uint32 report_head;
    drflac_uint32 report_count;
    drflac_uint32 report_active;
    drflac_uint8 report_queue[XDJ_REPORT_QUEUE][XDJ_REPORT_FIELDS];
    xdj_report_player report_player;
} xdj_flac_context_storage;

/*
 * A FLAC track whose decoder was evicted while the player may still hold it
 * (track cache, hot cue bank). Identified by its file, like every call.
 * format_context only marks the entry as in use.
 */
typedef struct {
    void* format_context;
    drflac_uint64 frame_count;
    drflac_uint64 cursor_frame;
    drflac_uint32 declared_bytes;
    drflac_uint32 file_bytes;
    drflac_uint8 frame_offset;
    drflac_uint8 kind;
#ifdef XDJ_CHIP_FLAC
    drflac_uint8 fed;
#endif
} xdj_flac_tombstone;

#if defined(__SH4A__) || defined(__sh__) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4) || (defined(__INTPTR_WIDTH__) && __INTPTR_WIDTH__ == 32)
_Static_assert(
    offsetof(xdj_flac_context_storage, request_format) == 0U,
    "assembly request-format offset must match the C layout"
);
_Static_assert(
    offsetof(xdj_flac_context_storage, routed_descriptors) == 4U,
    "assembly routed-descriptor offset must match the C layout"
);
_Static_assert(
    offsetof(xdj_flac_context_storage, decoder_lock) == 44U,
    "assembly decoder-lock offset must match the C layout"
);
_Static_assert(
    offsetof(xdj_fw_file_context, handle) == 4U &&
    offsetof(xdj_fw_file_context, last_operation) == 36U &&
    offsetof(xdj_fw_file_context, last_error) == 40U,
    "private file context must match the parser wrapper layout"
);
#endif

#if defined(__APPLE__) && defined(__MACH__)
#define XDJ_DATA_SECTION __attribute__((section("__DATA,__data")))
#elif defined(__GNUC__)
#define XDJ_DATA_SECTION __attribute__((section(".data")))
#else
#define XDJ_DATA_SECTION
#endif

/*
 * Startup does not clear memory beyond the stock image, so every field that
 * must start at zero lives in .data. The arenas may start with any contents
 * because the allocator zeroes each allocation.
 */
static xdj_flac_arena_storage xdj_flac_arenas[XDJ_FLAC_SLOT_COUNT];
/*
 * One slot past the working ones serves only to look at the header of a WAV
 * or AIFF file before deciding whose it is (xdj_pcm_is_ours): it is never
 * found, claimed or evicted, and only the parser task uses it.
 */
#define XDJ_PROBE_SLOT XDJ_FLAC_SLOT_COUNT
/* Rate-conversion state and WAV/AIFF source of each slot, set by every open. */
static xdj_flac_decimator xdj_flac_decimators[XDJ_FLAC_SLOT_COUNT + 1U];
static xdj_pcm_source xdj_pcm_sources[XDJ_FLAC_SLOT_COUNT + 1U];
static xdj_flac_slot xdj_flac_slots[XDJ_FLAC_SLOT_COUNT + 1U] XDJ_DATA_SECTION = {{0}};
static xdj_flac_tombstone xdj_flac_tombstones[XDJ_FLAC_TOMBSTONE_COUNT] XDJ_DATA_SECTION = {{0}};
/* The last finished episodes, for a memory dump; history_count says how many are valid. */
static xdj_episode xdj_flac_history[XDJ_EPISODE_HISTORY];

xdj_flac_context_storage xdj_flac_context XDJ_DATA_SECTION = {0};
#ifdef XDJ_CHIP_FLAC
/*
 * Where the audio processor's side stands. Not ready at power-on; the first
 * FLAC open that could be fed loads our block (loading meanwhile, with no
 * track fed), then it is ready or, if the load failed, failed for good. A
 * read that finds the converter table without our entry sets it back to not
 * ready. Changed under the adapter lock; in .data, so it starts as zero.
 */
#define XDJ_CHIP_STATE_NOT_READY 0U
#define XDJ_CHIP_STATE_LOADING 1U
#define XDJ_CHIP_STATE_READY 2U
#define XDJ_CHIP_STATE_FAILED 3U
static struct {
    drflac_uint32 state;
    drflac_uint32 loads;     /* loads that succeeded: a slot is fed only under the one it was opened with */
    drflac_uint32 failures;  /* loads that failed */
    drflac_uint32 losses;    /* times the table was found without our entry */
    drflac_uint32 fallbacks; /* fed tracks decoded by the bridge from then on */
} xdj_chip XDJ_DATA_SECTION = {0};
/* The feed of each slot's track, set by every open that finds the file fit. */
static xdj_feed_track xdj_chip_tracks[XDJ_FLAC_SLOT_COUNT + 1U];
#endif

drflac* xdj_flac_open(
    drflac_read_proc read_callback,
    drflac_seek_proc seek_callback,
    drflac_tell_proc tell_callback,
    void* stream_context,
    const drflac_allocation_callbacks* allocation_callbacks,
    xdj_flac_stream_info* info
);
void xdj_flac_close(drflac* decoder);
drflac_uint64 xdj_flac_read_s16(
    drflac* decoder,
    drflac_uint64 frame_count,
    drflac_int16* output
);
drflac_uint64 xdj_flac_read_s24(
    drflac* decoder,
    drflac_uint64 frame_count,
    unsigned char* output
);
void xdj_flac_lock_acquire(void);
void xdj_flac_lock_release(void);

#ifdef XDJ_HOST_TEST
size_t xdj_flac_get_arena_used(void)
{
    size_t index;
    size_t peak = 0;

    for (index = 0; index < XDJ_FLAC_SLOT_COUNT; ++index) {
        if (xdj_flac_slots[index].arena_used > peak) {
            peak = xdj_flac_slots[index].arena_used;
        }
    }
    return peak;
}

void xdj_flac_host_select(void* output_descriptor)
{
    xdj_flac_context.request_format =
        (unsigned char*)output_descriptor + XDJ_PCM_FORMAT_OFFSET;
}

/*
 * Host helpers find a descriptor's track by its content (the data length the
 * adapter wrote at +24), not its address: the player reuses one working
 * descriptor for every track.
 */
static xdj_flac_slot* xdj_host_slot_for_descriptor(const void* output_descriptor)
{
    drflac_uint32 declared = *(const drflac_uint32*)((const unsigned char*)output_descriptor + XDJ_DATA_BYTES_OFFSET);
    size_t index;

    for (index = 0; index < XDJ_FLAC_SLOT_COUNT; ++index) {
        if (xdj_flac_slots[index].decoder != NULL && xdj_flac_slots[index].declared_bytes == declared) {
            return &xdj_flac_slots[index];
        }
    }
    return NULL;
}

size_t xdj_flac_host_pcm_bytes(void* output_descriptor)
{
    xdj_flac_slot* slot = xdj_host_slot_for_descriptor(output_descriptor);

    return slot != NULL ? (size_t)slot->frame_count * slot->bytes_per_frame : 0U;
}

size_t xdj_flac_host_slot_count(void)
{
    return XDJ_FLAC_SLOT_COUNT;
}

size_t xdj_flac_host_active_slots(void)
{
    size_t index;
    size_t count = 0;

    for (index = 0; index < XDJ_FLAC_SLOT_COUNT; ++index) {
        count += xdj_flac_slots[index].decoder != NULL ? 1U : 0U;
    }
    return count;
}

int xdj_flac_host_slot_active(void* output_descriptor)
{
    return xdj_host_slot_for_descriptor(output_descriptor) != NULL;
}
#endif

static void xdj_write_u32(void* base, size_t offset, drflac_uint32 value)
{
    *(drflac_uint32*)((unsigned char*)base + offset) = value;
}

static void xdj_write_u8(void* base, size_t offset, drflac_uint8 value)
{
    *((unsigned char*)base + offset) = value;
}

static size_t xdj_slot_index(const xdj_flac_slot* slot)
{
    return (size_t)(slot - xdj_flac_slots);
}

static xdj_flac_decimator* xdj_slot_decimator(const xdj_flac_slot* slot)
{
    return &xdj_flac_decimators[xdj_slot_index(slot)];
}

static xdj_pcm_source* xdj_slot_pcm(const xdj_flac_slot* slot)
{
    return &xdj_pcm_sources[xdj_slot_index(slot)];
}

/* dr_flac calls only for FLAC slots: the other kinds have no decoder object. */
static void xdj_close_source(drflac_uint8 kind, drflac* decoder)
{
    if (kind == XDJ_KIND_FLAC) {
        xdj_flac_close(decoder);
    }
}

static void xdj_slot_invalidate(xdj_flac_slot* slot)
{
    if (slot->kind == XDJ_KIND_FLAC) {
        xdj_flac_invalidate(slot->decoder);
    }
}

static void* xdj_arena_malloc(size_t size, void* user_data)
{
    xdj_flac_slot* slot = (xdj_flac_slot*)user_data;
    unsigned char* arena = xdj_flac_arenas[xdj_slot_index(slot)].bytes;
    size_t aligned_size;
    size_t index;
    void* result;

    aligned_size = (size + 7U) & ~(size_t)7U;
    if (aligned_size > XDJ_FLAC_ARENA_BYTES - slot->arena_used) {
        return NULL;
    }
    result = &arena[slot->arena_used];
    for (index = 0; index < aligned_size; ++index) {
        ((unsigned char*)result)[index] = 0;
    }
    slot->arena_used += aligned_size;
    return result;
}

static void* xdj_arena_realloc(void* pointer, size_t size, void* user_data)
{
    xdj_flac_slot* slot = (xdj_flac_slot*)user_data;
    unsigned char* arena = xdj_flac_arenas[xdj_slot_index(slot)].bytes;

    if (pointer == NULL) {
        return xdj_arena_malloc(size, user_data);
    }
    if (pointer == arena && size <= XDJ_FLAC_ARENA_BYTES) {
        size_t new_used = (size + 7U) & ~(size_t)7U;
        if (new_used > slot->arena_used) {
            size_t index;
            for (index = slot->arena_used; index < new_used; ++index) {
                arena[index] = 0;
            }
        }
        slot->arena_used = new_used;
        return pointer;
    }
    return NULL;
}

static void xdj_arena_free(void* pointer, void* user_data)
{
    (void)pointer;
    (void)user_data;
}

/*
 * The input-file task shares its priority with the DSP transmit tasks and
 * runs above the UI, and the RTOS does not time-slice equal priorities. A
 * WAV seek is a file-pointer move, but a FLAC seek decodes, so long decoder
 * work sleeps one tick every few filesystem reads to let those tasks run.
 */
#ifndef XDJ_HOST_TEST
/*
 * Milliseconds from the kernel time plus the part of the current tick that
 * TCNT0 has counted. Reading again until the millisecond word and TCR0 are
 * stable avoids pairing a millisecond with a count from the next one; an
 * underflow whose tick interrupt has not run yet ends that millisecond.
 */
static drflac_uint32 xdj_timer_now(void)
{
    volatile const drflac_uint32* milliseconds = (volatile const drflac_uint32*)XDJ_KERNEL_MS_ADDRESS;
    volatile const drflac_uint32* counter = (volatile const drflac_uint32*)XDJ_TMU0_TCNT_ADDRESS;
    volatile const drflac_uint16* control = (volatile const drflac_uint16*)XDJ_TMU0_TCR_ADDRESS;
    drflac_uint32 ms;
    drflac_uint32 counts;
    drflac_uint16 flags;

    do {
        ms = *milliseconds;
        flags = *control;
        counts = *counter;
    } while (*control != flags || *milliseconds != ms);
    if ((flags & XDJ_TMU0_TCR_UNF) != 0U) {
        ms++;
    }
    if (counts > XDJ_TMU0_RELOAD) {
        counts = XDJ_TMU0_RELOAD;
    }
    /*
     * Elapsed counts times 16 / 10,416, as a multiply by 2^26 / 10,416 = 6,443
     * and a shift: the no-FPU libgcc lacks the SH-4 helper GCC picks for a
     * variable divisor.
     */
    return ms * XDJ_UNITS_PER_MS + (((XDJ_TMU0_RELOAD - counts) * 6443U) >> 22);
}
#endif

/* Time since start; an interval that would be negative counts as zero. */
static drflac_uint32 xdj_since(drflac_uint32 start)
{
    drflac_uint32 elapsed = XDJ_TIMER_NOW() - start;

    return elapsed > 0x80000000UL ? 0U : elapsed;
}

/*
 * Report values use two scales so short times keep their detail: times are
 * 10 ms steps up to 310 ms, then 80 ms steps from 320 ms up to 2,800 ms;
 * counts are exact up to 31, then steps of 8 from 32 up to 280. 63 means
 * "that much or more".
 */
static drflac_uint8 xdj_field_time(drflac_uint32 units)
{
    drflac_uint32 ms = units / XDJ_UNITS_PER_MS;
    drflac_uint32 value = ms < 315U ? (ms + 5U) / 10U : 32U + (ms - 280U) / 80U;

    return (drflac_uint8)(value > XDJ_REPORT_MAX_VALUE ? XDJ_REPORT_MAX_VALUE : value);
}

static drflac_uint8 xdj_field_count(drflac_uint32 count)
{
    drflac_uint32 value = count < 32U ? count : 32U + (count - 28U) / 8U;

    return (drflac_uint8)(value > XDJ_REPORT_MAX_VALUE ? XDJ_REPORT_MAX_VALUE : value);
}

static void xdj_raise_signal(drflac_uint32 kind, drflac_uint32 beeps)
{
    if (beeps == 0U) {
        beeps = 1U;
    }
    if (beeps > XDJ_SIGNAL_MAX_BEEPS) {
        beeps = XDJ_SIGNAL_MAX_BEEPS;
    }
    if (kind > xdj_flac_context.pending_kind ||
        (kind == xdj_flac_context.pending_kind && beeps > xdj_flac_context.pending_beeps)) {
        xdj_flac_context.pending_kind = kind;
        xdj_flac_context.pending_beeps = beeps;
    }
}

/* Stop any slot from adding to a breakdown that is about to go out of scope. */
static void xdj_detach_breakdown(const xdj_call_breakdown* breakdown)
{
    size_t index;

    for (index = 0; index < XDJ_FLAC_SLOT_COUNT; ++index) {
        if (xdj_flac_slots[index].breakdown == breakdown) {
            xdj_flac_slots[index].breakdown = NULL;
        }
    }
}

/* Lock acquisition, with the wait added to the call's breakdown. */
static void xdj_lock_timed(xdj_call_breakdown* breakdown)
{
    drflac_uint32 start = XDJ_TIMER_NOW();

    xdj_flac_lock_acquire();
    breakdown->wait += xdj_since(start);
}

/*
 * Report fields, in order: sequence, kind plus flags, then times: wall, to
 * the end of the first read, busy, reopen, seek, decode, filesystem read,
 * filesystem seek, wait; then counts: bridge calls, filesystem reads,
 * physical seeks, seek probes; then the largest wall time so far for this
 * kind; then the failure code, the seek's failure reason, filesystem errors,
 * the last filesystem error (its magnitude) and operation, handle positions
 * found wrong (after seeks, plus 8 times after reads, each capped at 7),
 * frame-number breaks, stale calls, calls with an unreadable handle size,
 * end-of-stream answers that said "ended"; and a checksum of the fields
 * before it.
 */
static void xdj_encode_report(const xdj_episode* episode, drflac_uint8* fields)
{
    drflac_uint32 index;
    drflac_uint32 checksum = 0;

    fields[0] = (drflac_uint8)(episode->sequence & XDJ_REPORT_MAX_VALUE);
    fields[1] = (drflac_uint8)(episode->kind |
                               (episode->fallbacks != 0U ? XDJ_REPORT_FLAG_FALLBACK : 0U) |
                               (episode->cache_hits != 0U ? XDJ_REPORT_FLAG_CACHE_HIT : 0U) |
                               (episode->no_size != 0U ? XDJ_REPORT_FLAG_NO_SIZE : 0U) |
                               (episode->failure != 0U ? XDJ_REPORT_FLAG_FAILED : 0U));
    fields[2] = xdj_field_time(episode->end - episode->start);
    fields[3] = xdj_field_time(episode->has_first_read ? episode->first_read : 0U);
    fields[4] = xdj_field_time(episode->busy);
    fields[5] = xdj_field_time(episode->reopen);
    fields[6] = xdj_field_time(episode->flac_seek);
    fields[7] = xdj_field_time(episode->decode);
    fields[8] = xdj_field_time(episode->fs_read);
    fields[9] = xdj_field_time(episode->fs_seek);
    fields[10] = xdj_field_time(episode->wait);
    fields[11] = xdj_field_count(episode->calls);
    fields[12] = xdj_field_count(episode->fs_reads);
    fields[13] = xdj_field_count(episode->physical_seeks);
    fields[14] = xdj_field_count(episode->probes);
    fields[15] = xdj_field_time(
        episode->kind == XDJ_EPISODE_OPEN ? xdj_flac_context.max_open_units : xdj_flac_context.max_jump_units
    );
    fields[16] = (drflac_uint8)(episode->failure & XDJ_REPORT_MAX_VALUE);
    fields[17] = (drflac_uint8)(episode->seek_failure & XDJ_REPORT_MAX_VALUE);
    fields[18] = xdj_field_count(episode->fs_errors);
    fields[19] = (drflac_uint8)(episode->fs_error < 0 ?
        (episode->fs_error < -63 ? 63 : -episode->fs_error) :
        (episode->fs_error > 63 ? 63 : episode->fs_error));
    fields[20] = (drflac_uint8)(episode->fs_operation & XDJ_REPORT_MAX_VALUE);
    fields[21] = (drflac_uint8)((episode->misplaced > 7U ? 7U : episode->misplaced) |
                                ((episode->handle_moved > 7U ? 7U : episode->handle_moved) << 3));
    fields[22] = xdj_field_count(episode->sequence_breaks);
    fields[23] = xdj_field_count(episode->stale_calls);
    fields[24] = xdj_field_count(episode->foreign_stale);
    fields[25] = xdj_field_count(episode->eof_ends);
    for (index = 0; index + 1U < XDJ_REPORT_FIELDS; ++index) {
        checksum += fields[index];
    }
    fields[XDJ_REPORT_FIELDS - 1U] = (drflac_uint8)(checksum & XDJ_REPORT_MAX_VALUE);
}

/* Keep a finished episode in the history and queue its report. */
static void xdj_finish_episode(const xdj_episode* episode)
{
    drflac_uint32 wall = episode->end - episode->start;
    drflac_uint32* maximum = episode->kind == XDJ_EPISODE_OPEN ?
        &xdj_flac_context.max_open_units : &xdj_flac_context.max_jump_units;
    drflac_uint32 tail;

    if (wall > *maximum) {
        *maximum = wall;
    }
    xdj_flac_history[xdj_flac_context.history_count % XDJ_EPISODE_HISTORY] = *episode;
    xdj_flac_context.history_count++;
    if (!XDJ_AUDIBLE_REPORTS) {
        return;
    }
    if (xdj_flac_context.report_count >= XDJ_REPORT_QUEUE) {
        /* Keep the newest reports; each one carries the running maximum. */
        xdj_flac_context.report_head = (xdj_flac_context.report_head + 1U) % XDJ_REPORT_QUEUE;
        xdj_flac_context.report_count = XDJ_REPORT_QUEUE - 1U;
        xdj_flac_context.reports_dropped++;
    }
    tail = (xdj_flac_context.report_head + xdj_flac_context.report_count) % XDJ_REPORT_QUEUE;
    xdj_encode_report(episode, xdj_flac_context.report_queue[tail]);
    xdj_flac_context.report_count++;
    xdj_flac_context.reports_queued++;
}

static void xdj_close_episode(void)
{
    if (xdj_flac_context.episode_active) {
        xdj_flac_context.episode_active = 0;
        xdj_finish_episode(&xdj_flac_context.episode);
    }
}

static void xdj_begin_episode(xdj_episode* episode, drflac_uint32 kind, drflac_uint32 start)
{
    xdj_episode empty = {0};

    *episode = empty;
    episode->sequence = ++xdj_flac_context.episode_sequence;
    episode->kind = kind;
    episode->start = start;
    episode->end = start;
}

static void xdj_add_call(
    xdj_episode* episode,
    drflac_uint32 start,
    drflac_uint32 end,
    const xdj_call_breakdown* breakdown
)
{
    episode->busy += end - start;
    episode->fs_seek += breakdown->fs_seek;
    episode->fs_read += breakdown->fs_read;
    episode->wait += breakdown->wait;
    episode->reopen += breakdown->reopen;
    episode->flac_seek += breakdown->flac_seek;
    episode->decode += breakdown->decode;
    episode->fs_reads += breakdown->fs_reads;
    episode->physical_seeks += breakdown->physical_seeks;
    episode->probes += breakdown->probes;
    episode->cache_hits += breakdown->cache_hits;
    episode->fallbacks += breakdown->fallbacks;
    episode->fs_errors += breakdown->fs_errors;
    if (breakdown->fs_errors != 0U) {
        episode->fs_error = breakdown->fs_error;
        episode->fs_operation = breakdown->fs_operation;
    }
    if (breakdown->failure != 0U) {
        episode->failure = breakdown->failure;
    }
    if (breakdown->seek_failure != 0U) {
        episode->seek_failure = breakdown->seek_failure;
    }
    episode->no_size |= breakdown->no_size;
    episode->misplaced += breakdown->misplaced;
    episode->handle_moved += breakdown->handle_moved;
    episode->sequence_breaks += breakdown->sequence_breaks;
    episode->stale_calls += breakdown->stale_calls;
    episode->foreign_stale += breakdown->foreign_stale;
    episode->eof_ends += breakdown->eof_ends;
    episode->calls++;
    if (breakdown->is_read && !episode->has_first_read) {
        episode->has_first_read = 1;
        episode->first_read = end - episode->start;
    }
    episode->end = end;
}

/*
 * Account one read or seek, with the lock held. An idle bridge ends the
 * current episode; a seek that moves a FLAC track's play position starts the
 * next. Calls that no FLAC slot served (other formats) are not counted.
 */
static void xdj_note_call(drflac_uint32 start, drflac_uint32 end, const xdj_call_breakdown* breakdown)
{
    drflac_uint32 idle = start - xdj_flac_context.episode.end;

    if (xdj_flac_context.episode_active &&
        idle < 0x80000000UL && idle > XDJ_EPISODE_IDLE_MS * XDJ_UNITS_PER_MS) {
        xdj_close_episode();
    }
    /* Calls for other formats are not counted; failures are, served or not. */
    if (!breakdown->served && breakdown->failure == 0U) {
        return;
    }
    if (breakdown->moved) {
        xdj_close_episode();
        xdj_begin_episode(&xdj_flac_context.episode, XDJ_EPISODE_JUMP, start);
        xdj_flac_context.episode_active = 1;
    } else if ((breakdown->failure != 0U || breakdown->stale_calls != 0U || breakdown->foreign_stale != 0U) &&
               !xdj_flac_context.episode_active) {
        xdj_begin_episode(&xdj_flac_context.episode, XDJ_EPISODE_FAILURE, start);
        xdj_flac_context.episode_active = 1;
    }
    if (xdj_flac_context.episode_active) {
        xdj_add_call(&xdj_flac_context.episode, start, end, breakdown);
    }
}

/* A FLAC open is reported on its own, with the lock held. */
static void xdj_note_open(drflac_uint32 start, drflac_uint32 end, const xdj_call_breakdown* breakdown)
{
    xdj_episode open;

    xdj_begin_episode(&open, XDJ_EPISODE_OPEN, start);
    xdj_add_call(&open, start, end, breakdown);
    xdj_finish_episode(&open);
}

/* A quarter-amplitude-margin sine: 12,000 peak, 64 steps per cycle. */
static const drflac_int16 xdj_sine_table[64] = {
    0, 1176, 2341, 3483, 4592, 5657, 6667, 7613, 8485, 9276, 9978, 10583,
    11087, 11483, 11769, 11942, 12000, 11942, 11769, 11483, 11087, 10583,
    9978, 9276, 8485, 7613, 6667, 5657, 4592, 3483, 2341, 1176, 0, -1176,
    -2341, -3483, -4592, -5657, -6667, -7613, -8485, -9276, -9978, -10583,
    -11087, -11483, -11769, -11942, -12000, -11942, -11769, -11483, -11087,
    -10583, -9978, -9276, -8485, -7613, -6667, -5657, -4592, -3483, -2341,
    -1176
};

/*
 * Next sample of a report, or 0 once it has ended. The phase step is the
 * frequency times 2^32 / sample rate; only 44.1 and 48 kHz reach a slot.
 */
static int xdj_report_sample(xdj_report_player* player, drflac_uint32 sample_rate, drflac_int16* sample)
{
    drflac_uint32 tone_frames = sample_rate == 44100U ? 2646U : 2880U;
    drflac_uint32 gap_frames = sample_rate == 44100U ? 882U : 960U;
    drflac_uint32 step = sample_rate == 44100U ? 97392U : 89478U;
    drflac_uint32 frequency;

    if (player->symbol >= XDJ_REPORT_SYMBOLS) {
        return 0;
    }
    *sample = 0;
    if (player->position < tone_frames) {
        frequency = player->symbol < XDJ_REPORT_MARKS ?
            XDJ_REPORT_MARK_HZ :
            XDJ_REPORT_BASE_HZ + XDJ_REPORT_STEP_HZ * player->fields[player->symbol - XDJ_REPORT_MARKS];
        *sample = xdj_sine_table[player->phase >> 26];
        player->phase += frequency * step;
    }
    if (++player->position >= tone_frames + gap_frames) {
        player->position = 0;
        player->phase = 0;
        player->symbol++;
    }
    return 1;
}

static void xdj_start_report(xdj_report_player* player, const drflac_uint8* fields)
{
    drflac_uint32 index;

    for (index = 0; index < XDJ_REPORT_FIELDS; ++index) {
        player->fields[index] = fields[index];
    }
    player->symbol = 0;
    player->position = 0;
    player->phase = 0;
}

/*
 * Put pending reports and failure beeps into freshly decoded audio. A report
 * replaces the music while it plays. Failure beeps are square waves at
 * -12 dBFS, 300 ms on and 100 ms off, continued across reads.
 */
/* Store one signal sample in both channels of a 16-bit or packed 24-bit frame. */
static void xdj_put_signal_frame(unsigned char* frame, drflac_uint8 bytes_per_frame, drflac_int16 sample)
{
    unsigned char low = (unsigned char)((drflac_uint16)sample & 0xFFU);
    unsigned char high = (unsigned char)((drflac_uint16)sample >> 8);

    if (bytes_per_frame == 6U) {
        frame[0] = 0;
        frame[1] = low;
        frame[2] = high;
        frame[3] = 0;
        frame[4] = low;
        frame[5] = high;
    } else {
        frame[0] = low;
        frame[1] = high;
        frame[2] = low;
        frame[3] = high;
    }
}

static void xdj_play_signals(
    unsigned char* output,
    drflac_uint64 frames,
    drflac_uint32 sample_rate,
    drflac_uint8 bytes_per_frame
)
{
    drflac_uint32 on_frames;
    drflac_uint32 half_period;
    drflac_uint64 index;
    drflac_int16 sample;

#ifdef XDJ_HOST_TEST
    /* Host tests compare decoded audio bit for bit and take reports directly. */
    (void)output;
    (void)frames;
    (void)sample_rate;
    (void)bytes_per_frame;
    (void)on_frames;
    (void)half_period;
    (void)index;
    (void)sample;
    xdj_flac_context.pending_kind = XDJ_SIGNAL_NONE;
    xdj_flac_context.pending_beeps = 0;
    xdj_flac_context.sequence_beeps_left = 0;
    return;
#endif
    for (index = 0; index < frames; ++index) {
        int reported = 0;

        while (!reported && (xdj_flac_context.report_active || xdj_flac_context.report_count != 0U ||
                             xdj_flac_context.snapshot_count != 0U)) {
            if (!xdj_flac_context.report_active && xdj_flac_context.snapshot_count != 0U) {
                xdj_start_report(
                    &xdj_flac_context.report_player,
                    xdj_flac_context.snapshot_queue[xdj_flac_context.snapshot_head]
                );
                xdj_flac_context.snapshot_head = (xdj_flac_context.snapshot_head + 1U) % XDJ_SNAPSHOT_QUEUE;
                xdj_flac_context.snapshot_count--;
                xdj_flac_context.report_active = 1;
                xdj_flac_context.reports_played++;
            } else if (!xdj_flac_context.report_active) {
                xdj_start_report(
                    &xdj_flac_context.report_player,
                    xdj_flac_context.report_queue[xdj_flac_context.report_head]
                );
                xdj_flac_context.report_head = (xdj_flac_context.report_head + 1U) % XDJ_REPORT_QUEUE;
                xdj_flac_context.report_count--;
                xdj_flac_context.report_active = 1;
                xdj_flac_context.reports_played++;
            }
            if (xdj_report_sample(&xdj_flac_context.report_player, sample_rate, &sample)) {
                xdj_put_signal_frame(output + (size_t)index * bytes_per_frame, bytes_per_frame, sample);
                reported = 1;
            } else {
                xdj_flac_context.report_active = 0;
            }
        }
        if (reported) {
            continue;
        }
        if (xdj_flac_context.sequence_beeps_left == 0U) {
            if (xdj_flac_context.pending_kind == XDJ_SIGNAL_NONE) {
                return;
            }
            xdj_flac_context.sequence_kind = xdj_flac_context.pending_kind;
            xdj_flac_context.sequence_beeps_left = xdj_flac_context.pending_beeps;
            xdj_flac_context.pending_kind = XDJ_SIGNAL_NONE;
            xdj_flac_context.pending_beeps = 0;
            xdj_flac_context.sequence_position = 0;
            xdj_flac_context.sequence_phase = 0;
            xdj_flac_context.sequence_level = XDJ_SIGNAL_AMPLITUDE;
            xdj_flac_context.signals_played++;
        }
        on_frames = sample_rate == 44100U ? 13230U : 14400U;
        half_period = sample_rate == 44100U ? 55U : 60U;
        sample = 0;
        if (xdj_flac_context.sequence_position < on_frames) {
            sample = (drflac_int16)xdj_flac_context.sequence_level;
            if (++xdj_flac_context.sequence_phase >= half_period) {
                xdj_flac_context.sequence_phase = 0;
                xdj_flac_context.sequence_level = -xdj_flac_context.sequence_level;
            }
        }
        xdj_put_signal_frame(output + (size_t)index * bytes_per_frame, bytes_per_frame, sample);
        if (++xdj_flac_context.sequence_position >=
            on_frames + (sample_rate == 44100U ? 4410U : 4800U)) {
            xdj_flac_context.sequence_position = 0;
            xdj_flac_context.sequence_beeps_left--;
        }
    }
}

static void xdj_yield_after_read(xdj_flac_slot* slot)
{
    drflac_uint32 start;

    if ((++xdj_flac_context.io_reads % XDJ_READS_PER_YIELD) == 0U) {
        xdj_flac_context.yields++;
        start = XDJ_TIMER_NOW();
        XDJ_TASK_DELAY_CALL(1);
        if (slot->breakdown != NULL) {
            slot->breakdown->wait += xdj_since(start);
        }
    }
}

/* Like the stock read at end of data, pause before reporting a failure so a
 * caller that retries at once cannot spin. Called without the lock held. */
static void xdj_delay_before_error(drflac_uint32 failure)
{
    xdj_flac_context.error_delays++;
    xdj_flac_context.last_failure = failure;
    xdj_raise_signal(XDJ_SIGNAL_FAILURE, failure);
    XDJ_TASK_DELAY_CALL(2);
}

/* A filesystem call reported an error: count it and keep its code for the report. */
static void xdj_note_fs_error(xdj_flac_slot* slot)
{
    xdj_flac_context.fs_errors++;
    if (slot->breakdown != NULL) {
        slot->breakdown->fs_errors++;
        slot->breakdown->fs_error = slot->io.last_error;
        slot->breakdown->fs_operation = slot->io.last_operation;
    }
}

/*
 * Seeks from dr_flac only record the logical position. The filesystem seek
 * happens here, right before a read, and only when the handle is elsewhere:
 * another user may have moved it, a handle switch always does, and runs of
 * dr_flac seeks collapse into one. Seeks are absolute: a relative one would
 * land wrong if the handle moved between the tell and the seek. The handle
 * is checked again after the seek.
 */
static drflac_bool32 xdj_stream_reposition(xdj_flac_slot* slot)
{
    drflac_uint32 start = XDJ_TIMER_NOW();
    int position = XDJ_FW_TELL_CALL(&slot->io);
    int result;

#ifndef XDJ_HOST_TRUST_EVERY_HANDLE
    if (!slot->strict_checks && slot->file_bytes != 0U &&
        (slot->io.handle != slot->known_handle || position != slot->known_position)) {
        drflac_uint32 size = 0;

        xdj_flac_context.network_size_checks++;
        if (XDJ_FW_SIZE_CALL(&slot->io, &size) != 0 || size != slot->file_bytes) {
            slot->handle_lost = 1;
            if (slot->breakdown != NULL) {
                slot->breakdown->fs_seek += xdj_since(start);
            }
            return DRFLAC_FALSE;
        }
    }
#endif
    slot->known_handle = slot->io.handle;

    if (position >= 0 && (drflac_uint32)position == slot->stream_position) {
        result = 0;
    } else {
        xdj_flac_context.repositions++;
        xdj_flac_context.physical_seeks++;
        if (slot->breakdown != NULL) {
            slot->breakdown->physical_seeks++;
        }
        result = XDJ_FW_SEEK_CALL(&slot->io, (int)slot->stream_position, 0);
        if (result != 0) {
            xdj_note_fs_error(slot);
            slot->known_handle = NULL;
        } else if (XDJ_FW_TELL_CALL(&slot->io) != (int)slot->stream_position) {
            xdj_flac_context.misplaced++;
            if (slot->breakdown != NULL) {
                slot->breakdown->misplaced++;
            }
        }
    }
    if (slot->breakdown != NULL) {
        slot->breakdown->fs_seek += xdj_since(start);
    }
    return result == 0;
}

static size_t xdj_stream_read(
    void* user_data,
    void* output,
    size_t bytes_to_read
)
{
    xdj_flac_slot* slot = (xdj_flac_slot*)user_data;
    size_t total_read = 0;
    unsigned char* dest = (unsigned char*)output;
    drflac_uint32 start;

    if (slot->handle_lost || !xdj_stream_reposition(slot)) {
        return 0;
    }
    while (total_read < bytes_to_read) {
        int status = 0;
        size_t requested = bytes_to_read - total_read;
        size_t actual;

        start = XDJ_TIMER_NOW();
        actual = XDJ_FW_READ_CALL(&slot->io, dest + total_read, requested, &status);
        if (slot->breakdown != NULL) {
            slot->breakdown->fs_read += xdj_since(start);
            slot->breakdown->fs_reads++;
        }
        xdj_yield_after_read(slot);
        if (actual > requested) {
            /* Not a byte count. */
            actual = 0;
            status = -1;
        }
        if (status != 0) {
            xdj_note_fs_error(slot);
        }
        /* On an error the player's read still returns the bytes it did get. */
        total_read += actual;
        if (actual == 0 || status != 0) {
            break;
        }
    }
    /*
     * The player may close this handle, and the filesystem reuse it for the
     * next track, while this call waits on USB: check the file is still ours.
     */
#ifndef XDJ_HOST_TRUST_EVERY_HANDLE
    if (slot->strict_checks && slot->file_bytes != 0U) {
        drflac_uint32 size = 0;

        if (XDJ_FW_SIZE_CALL(&slot->io, &size) != 0 || size != slot->file_bytes) {
            slot->handle_lost = 1;
            return 0;
        }
    }
#endif
    /*
     * The handle must end where these bytes end. If it does not, it was
     * closed and reopened (the same track at another place) during the call,
     * and the bytes cannot be trusted (diag7 found no such case in normal
     * playing).
     */
    if (XDJ_FW_TELL_CALL(&slot->io) != (int)(slot->stream_position + (drflac_uint32)total_read)) {
        xdj_flac_context.handle_moved++;
        if (slot->breakdown != NULL) {
            slot->breakdown->handle_moved++;
        }
#ifndef XDJ_HOST_TRUST_EVERY_HANDLE
        slot->handle_lost = 1;
        return 0;
#endif
    }
    slot->stream_position += (drflac_uint32)total_read;
    slot->known_position = (int)slot->stream_position;
    return total_read;
}

static drflac_bool32 xdj_stream_seek(
    void* user_data,
    int offset,
    drflac_seek_origin origin
)
{
    xdj_flac_slot* slot = (xdj_flac_slot*)user_data;
    drflac_int64 target;

    xdj_flac_context.lazy_seeks++;
    if (origin == DRFLAC_SEEK_SET) {
        target = offset;
    } else if (origin == DRFLAC_SEEK_CUR) {
        target = (drflac_int64)slot->stream_position + offset;
    } else if (origin == DRFLAC_SEEK_END && slot->file_bytes != 0U) {
        target = (drflac_int64)slot->file_bytes + offset;
    } else {
        return DRFLAC_FALSE;
    }
    /* Like a failed filesystem seek, refuse positions past the end. */
    if (target < 0 || target > 0x7FFFFFFF ||
        (slot->file_bytes != 0U && target > (drflac_int64)slot->file_bytes)) {
        return DRFLAC_FALSE;
    }
    slot->stream_position = (drflac_uint32)target;
    return DRFLAC_TRUE;
}

static drflac_bool32 xdj_stream_tell(
    void* user_data,
    drflac_int64* cursor
)
{
    xdj_flac_slot* slot = (xdj_flac_slot*)user_data;

    if (cursor == NULL) {
        return DRFLAC_FALSE;
    }
    *cursor = slot->stream_position;
    return DRFLAC_TRUE;
}

static void xdj_update_routing(void);

static void xdj_release_slot(xdj_flac_slot* slot)
{
    if (slot->decoder != NULL) {
        xdj_close_source(slot->kind, slot->decoder);
        slot->decoder = NULL;
    }
    slot->format_context = NULL;
    slot->arena_used = 0;
    xdj_update_routing();
}

static drflac_bool32 xdj_same_track(
    drflac_uint32 file_bytes_a, drflac_uint64 frames_a,
    drflac_uint32 file_bytes_b, drflac_uint64 frames_b
)
{
    return file_bytes_a == file_bytes_b && frames_a == frames_b;
}

static void xdj_record_tombstone(const xdj_flac_tombstone* record)
{
    xdj_flac_tombstone* tombstone = NULL;
    size_t index;

    for (index = 0; tombstone == NULL && index < XDJ_FLAC_TOMBSTONE_COUNT; ++index) {
        if (xdj_flac_tombstones[index].format_context != NULL &&
            xdj_same_track(xdj_flac_tombstones[index].file_bytes, xdj_flac_tombstones[index].frame_count,
                           record->file_bytes, record->frame_count)) {
            tombstone = &xdj_flac_tombstones[index];
        }
    }

    for (index = 0; tombstone == NULL && index < XDJ_FLAC_TOMBSTONE_COUNT; ++index) {
        if (xdj_flac_tombstones[index].format_context == NULL) {
            tombstone = &xdj_flac_tombstones[index];
        }
    }
    if (tombstone == NULL) {
        /* All entries in use: overwrite the oldest. */
        tombstone = &xdj_flac_tombstones[xdj_flac_context.tombstone_next];
        xdj_flac_context.tombstone_next =
            (xdj_flac_context.tombstone_next + 1U) % XDJ_FLAC_TOMBSTONE_COUNT;
    }
    *tombstone = *record;
    xdj_update_routing();
}

static xdj_flac_slot* xdj_claim_slot(void)
{
    xdj_flac_slot* oldest;
    size_t index;

    oldest = NULL;
    for (index = 0; index < XDJ_FLAC_SLOT_COUNT; ++index) {
        if (xdj_flac_slots[index].opening) {
            continue;
        }
        if (xdj_flac_slots[index].decoder == NULL) {
            return &xdj_flac_slots[index];
        }
        if (oldest == NULL || xdj_flac_slots[index].last_use < oldest->last_use) {
            oldest = &xdj_flac_slots[index];
        }
    }
    /* At most two opens run at once, so this only guards against misuse. */
    if (oldest == NULL) {
        return NULL;
    }
    /* The player may still hold this track (track cache, hot cue bank). */
    xdj_flac_context.evictions++;
    {
        xdj_flac_tombstone record;
        record.format_context = oldest->format_context;
        record.frame_count = oldest->frame_count;
        record.cursor_frame = oldest->cursor_frame;
        record.frame_offset = oldest->frame_offset;
        record.kind = oldest->kind;
        record.declared_bytes = oldest->declared_bytes;
        record.file_bytes = oldest->file_bytes;
#ifdef XDJ_CHIP_FLAC
        record.fed = oldest->fed_form;
#endif
        xdj_record_tombstone(&record);
    }
    xdj_release_slot(oldest);
    return oldest;
}

static void xdj_touch_slot(xdj_flac_slot* slot)
{
    slot->last_use = ++xdj_flac_context.use_clock;
}

/* Seek the slot's decoder, adding the seek's statistics to the current call's breakdown. */
static drflac_bool32 xdj_slot_seek(xdj_flac_slot* slot, drflac_uint64 frame);

/*
 * Start a call on a slot whose handle checked out. After a lost handle the
 * decoder may hold bytes from another file: it forgets its place, and the
 * caller seeks it back to the play position. Returns whether it did.
 */
static drflac_bool32 xdj_begin_slot_call(xdj_flac_slot* slot)
{
    slot->handle_lost = 0;
    if (!slot->resync) {
        return DRFLAC_FALSE;
    }
    slot->resync = 0;
    xdj_slot_invalidate(slot);
    xdj_flac_context.resyncs++;
    return DRFLAC_TRUE;
}

/* The handle was lost during the call: treat it as a stale window. */
static drflac_bool32 xdj_end_if_lost(xdj_flac_slot* slot, xdj_call_breakdown* breakdown)
{
    if (!slot->handle_lost) {
        return DRFLAC_FALSE;
    }
    slot->handle_lost = 0;
    slot->resync = 1;
    breakdown->stale_calls++;
    xdj_flac_context.stale_calls++;
    xdj_flac_context.handles_lost++;
    return DRFLAC_TRUE;
}

static drflac_bool32 xdj_slot_seek(xdj_flac_slot* slot, drflac_uint64 frame)
{
    drflac_bool32 result;

    /* The decoder moves: a carried frame no longer matches its position. */
    slot->carry_valid = 0;
    slot->reposition = 0;
#ifdef XDJ_CHIP_FLAC
    if (slot->fed) {
        /* Nothing is decoded here: the next read starts a stream at this frame. */
        slot->fed_position = (drflac_uint32)frame;
        return DRFLAC_TRUE;
    }
#endif
    if (slot->kind != XDJ_KIND_FLAC) {
        slot->seek.probes = 0;
        slot->seek.used_cache = 0;
        slot->seek.used_fallback = 0;
        slot->seek.failure = XDJ_SEEK_FAIL_NONE;
        result = xdj_pcm_seek(xdj_slot_pcm(slot), (drflac_uint32)frame, xdj_slot_decimator(slot));
    } else if (xdj_slot_decimator(slot)->shift != 0U) {
        result = xdj_flac_seek_decimated(slot->decoder, (drflac_uint32)frame, &slot->seek, xdj_slot_decimator(slot));
    } else {
        result = xdj_flac_seek(slot->decoder, frame, &slot->seek);
    }

    if (slot->breakdown != NULL) {
        slot->breakdown->probes += slot->seek.probes;
        slot->breakdown->cache_hits += slot->seek.used_cache;
        slot->breakdown->fallbacks += slot->seek.used_fallback;
        if (!result) {
            slot->breakdown->seek_failure = slot->seek.failure != 0U ? slot->seek.failure : 16U;
        }
    }
    return result;
}

/* Record whether the filesystem gave the slot's file size. */
static void xdj_note_size(const xdj_flac_slot* slot, xdj_call_breakdown* breakdown)
{
    breakdown->no_size = slot->file_bytes == 0U ? 1U : 0U;
}

/*
 * The trampolines skip the bridge entirely while this count is zero, so it
 * covers evicted descriptors as well as active slots.
 */
static void xdj_update_routing(void)
{
    drflac_uint32 count = 0;
    size_t index;

    for (index = 0; index < XDJ_FLAC_SLOT_COUNT; ++index) {
        if (xdj_flac_slots[index].decoder != NULL || xdj_flac_slots[index].opening) {
            count++;
        }
    }
    for (index = 0; index < XDJ_FLAC_TOMBSTONE_COUNT; ++index) {
        if (xdj_flac_tombstones[index].format_context != NULL) {
            count++;
        }
    }
    xdj_flac_context.routed_descriptors = count;
}

/* The file a PCM call reads: its size, and the data length in its file context (0 when unknown). */
typedef struct {
    drflac_uint32 size;
    drflac_uint32 declared;
    void* handle;
    drflac_uint8 size_known;
    drflac_uint8 strict;
} xdj_call_file;

/* A handle's device byte (device +17) and filesystem type, as FS read finds them; 0 on the host. */
static void xdj_handle_type(void* handle, drflac_uint32* device_byte, drflac_uint32* fs_type)
{
    *device_byte = 0;
    *fs_type = 0;
#ifndef XDJ_HOST_TEST
    if (handle != NULL && ((drflac_uint32)handle & 3U) == 0U) {
        const unsigned char* device = *(const unsigned char* const*)handle;

        if (device != NULL) {
            drflac_uint32 index = ((drflac_uint32)device[17] | 32U) & 0xFFU;

            *device_byte = device[17];
            *fs_type = *(const volatile drflac_uint16*)(XDJ_FS_TYPE_TABLE_ADDRESS + index * 12U);
        }
    }
#else
    (void)handle;
#endif
}

/* Six 6-bit fields, most significant first. */
static void xdj_put_field_u32(drflac_uint8* fields, drflac_uint32 value)
{
    drflac_uint32 index;

    for (index = 0; index < 6U; ++index) {
        fields[index] = (drflac_uint8)((value >> (6U * (5U - index))) & XDJ_REPORT_MAX_VALUE);
    }
}

/*
 * Snapshot fields: 0 number, 1 the mark 63, 2 reason, 3 filesystem type,
 * 4-5 device byte, 6-11 the call's file size, 12-17 the expected track's
 * file size, 18-23 the file context's data length, 24 flags (1 size known,
 * 2 a track was expected, 4 it was evicted), 25 active tracks with the call's
 * file size, 26 checksum.
 */
static void xdj_take_snapshot(
    drflac_uint32 reason,
    const xdj_call_file* file,
    drflac_uint32 expected_bytes,
    drflac_uint32 flags
)
{
    drflac_uint8* fields;
    drflac_uint32 device_byte;
    drflac_uint32 fs_type;
    drflac_uint32 matches = 0;
    drflac_uint32 checksum = 0;
    drflac_uint32 index;

    if (xdj_flac_context.snapshots_taken >= XDJ_SNAPSHOT_LIMIT ||
        xdj_flac_context.snapshot_count >= XDJ_SNAPSHOT_QUEUE ||
        (xdj_flac_context.snapshot_reasons & (1UL << reason)) != 0U) {
        return;
    }
    xdj_flac_context.snapshot_reasons |= 1UL << reason;
    fields = xdj_flac_context.snapshot_queue[
        (xdj_flac_context.snapshot_head + xdj_flac_context.snapshot_count) % XDJ_SNAPSHOT_QUEUE];
    xdj_handle_type(file->handle, &device_byte, &fs_type);
    for (index = 0; index < XDJ_FLAC_SLOT_COUNT; ++index) {
        if (xdj_flac_slots[index].decoder != NULL && file->size != 0U &&
            xdj_flac_slots[index].file_bytes == file->size) {
            matches++;
        }
    }
    fields[0] = (drflac_uint8)(++xdj_flac_context.snapshots_taken & XDJ_REPORT_MAX_VALUE);
    fields[1] = XDJ_SNAPSHOT_MARK;
    fields[2] = (drflac_uint8)reason;
    fields[3] = (drflac_uint8)(fs_type & XDJ_REPORT_MAX_VALUE);
    fields[4] = (drflac_uint8)((device_byte >> 6) & XDJ_REPORT_MAX_VALUE);
    fields[5] = (drflac_uint8)(device_byte & XDJ_REPORT_MAX_VALUE);
    xdj_put_field_u32(fields + 6, file->size);
    xdj_put_field_u32(fields + 12, expected_bytes);
    xdj_put_field_u32(fields + 18, file->declared);
    fields[24] = (drflac_uint8)((file->size_known ? 1U : 0U) | flags);
    fields[25] = (drflac_uint8)(matches & XDJ_REPORT_MAX_VALUE);
    for (index = 0; index + 1U < XDJ_REPORT_FIELDS; ++index) {
        checksum += fields[index];
    }
    fields[XDJ_REPORT_FIELDS - 1U] = (drflac_uint8)(checksum & XDJ_REPORT_MAX_VALUE);
    if (XDJ_AUDIBLE_REPORTS) {
        xdj_flac_context.snapshot_count++;
    }
}

/*
 * Read through the file context's current handle. The player opens a handle
 * per command and closes it afterwards, so a handle is
 * never kept from one call to the next: xdj_request_slot has already checked
 * that this one is the slot's file.
 */
static void xdj_attach_task_handle(xdj_flac_slot* slot, void* file_context)
{
    void* handle;

    if (file_context == NULL) {
        return;
    }
    handle = XDJ_TASK_HANDLE(file_context);
    if (handle != NULL && handle != slot->io.handle) {
        slot->io.handle = handle;
        xdj_flac_context.handle_switches++;
    }
}

/*
 * Split a byte position into whole frames and the bytes into the next one.
 * The divisor is spelled out: the target has no helper for dividing by a
 * variable.
 */
static drflac_uint32 xdj_whole_frames(drflac_uint32 byte_count, drflac_uint8 bytes_per_frame)
{
    return bytes_per_frame == 6U ? byte_count / 6U : byte_count >> 2;
}

static drflac_uint8 xdj_bytes_per_frame(drflac_uint8 bits_per_sample)
{
    return bits_per_sample == 24U ? 6U : 4U;
}

/*
 * Decode frames at the decoder's position in the slot's output format. 16-bit
 * frames are decoded in place when the destination is aligned for 16-bit
 * stores (the SH-4 traps on unaligned ones) and one by one through an aligned
 * frame otherwise.
 */
#ifdef XDJ_CHIP_FLAC
/* What the feed needs from the file: the FLAC frame that holds a frame, through the slot's decoder. */
static int xdj_fed_locate(void* user, unsigned frame, unsigned* offset, unsigned* first)
{
    xdj_flac_slot* slot = (xdj_flac_slot*)user;
    drflac_uint32 found_offset = 0;
    drflac_uint32 found_first = 0;

    if (!xdj_flac_locate(slot->decoder, frame, &slot->seek, &found_offset, &found_first)) {
        return 0;
    }
    *offset = found_offset;
    *first = found_first;
    return 1;
}


/* And the file's bytes, through the slot's handle with all its checks. More than asked means failure. */
static unsigned xdj_fed_file(void* user, unsigned offset, unsigned char* output, unsigned count)
{
    xdj_flac_slot* slot = (xdj_flac_slot*)user;
    size_t got;

    if (!xdj_stream_seek(slot, (int)offset, DRFLAC_SEEK_SET)) {
        return count + 1U;
    }
    got = xdj_stream_read(slot, output, count);
    /* Fewer bytes are the file's end only if that is where they end. */
    if (slot->handle_lost || (got < count && slot->file_bytes != 0U && offset + (unsigned)got < slot->file_bytes)) {
        return count + 1U;
    }
    return (unsigned)got;
}

/* Tags for frames at the slot's position, with the file's bytes sent ahead of them. */
static drflac_uint64 xdj_fed_read(xdj_flac_slot* slot, drflac_uint64 frames, unsigned char* output);
#ifndef XDJ_HOST_TEST
/* src/fed_stack.S: fn(a, b, c) on a stack of our own. */
unsigned xdj_call_on_fed_stack(unsigned (*fn)(void*, unsigned, unsigned char*), void* a, unsigned b, unsigned char* c);

static unsigned xdj_fed_read_entry(void* slot, unsigned frames, unsigned char* output)
{
    return (unsigned)xdj_fed_read((xdj_flac_slot*)slot, frames, output);
}
#endif

static drflac_uint64 xdj_fed_read(xdj_flac_slot* slot, drflac_uint64 frames, unsigned char* output)
{
    xdj_feed_io io;
    unsigned done;

    io.user = slot;
    io.locate = xdj_fed_locate;
    io.read = xdj_fed_file;
    done = xdj_feed_read(&xdj_chip_tracks[xdj_slot_index(slot)], slot->fed_position, (unsigned)frames, output, &io);
    slot->fed_position += done;
    return done;
}

/*
 * Put the audio processor's side in place if it is not yet: at the first
 * open of a FLAC that could be fed after power-on, and after the audio
 * processor was started over. The load itself runs without the lock (the
 * playing tracks go on meanwhile, decoded by the bridge as long as nothing
 * is fed) and takes a fraction of a second.
 */
static void xdj_chip_make_ready(void)
{
    drflac_bool32 load = DRFLAC_FALSE;
    int loaded;

    xdj_flac_lock_acquire();
    if (xdj_chip.state == XDJ_CHIP_STATE_NOT_READY) {
        xdj_chip.state = XDJ_CHIP_STATE_LOADING;
        load = DRFLAC_TRUE;
    }
    xdj_flac_lock_release();
    if (!load) {
        return;
    }
    loaded = xdj_chip_load();
    xdj_flac_lock_acquire();
    if (loaded) {
        xdj_feed_reset();
        xdj_chip.loads++;
        xdj_chip.state = XDJ_CHIP_STATE_READY;
    } else {
        xdj_chip.failures++;
        xdj_chip.state = XDJ_CHIP_STATE_FAILED;
    }
    xdj_flac_lock_release();
}

/*
 * At an open: see whether the file can be fed (its stream information where
 * it should be), without moving the decoder's place in the file. Only the
 * parser's open (the slot is opening) may load our block: a reopen runs
 * under the lock.
 */
static void xdj_fed_prepare(xdj_flac_slot* slot, const xdj_flac_stream_info* info)
{
    unsigned char head[XDJ_CHIP_STREAM_HEADER];
    drflac_uint32 place = slot->stream_position;

    slot->fed = 0;
    slot->fed_form = 0;
    slot->fed_ready = 0;
    slot->fed_position = 0;
#ifdef XDJ_CHIP_HIRES
    /* 88.2 and 96 kHz too: the audio processor halves them (src/chip_flac.h). */
    if (slot->kind != XDJ_KIND_FLAC || info->frame_count > 0x3FFFFFFFULL) {
        return;
    }
#else
    if (slot->kind != XDJ_KIND_FLAC || xdj_slot_decimator(slot)->shift != 0U ||
        info->frame_count > 0x3FFFFFFFULL) {
        return;
    }
#endif
    if (slot->opening) {
        xdj_chip_make_ready();
    }
    if (xdj_chip.state != XDJ_CHIP_STATE_READY) {
        return;
    }
    slot->fed_load = xdj_chip.loads;
    if (xdj_stream_seek(slot, 0, DRFLAC_SEEK_SET) &&
        xdj_stream_read(slot, head, sizeof(head)) == sizeof(head) &&
        xdj_feed_open(&xdj_chip_tracks[xdj_slot_index(slot)], head, (unsigned)info->frame_count,
                      info->bits_per_sample)) {
        slot->fed_ready = 1;
#ifdef XDJ_CHIP_HIRES
        if (xdj_slot_decimator(slot)->shift != 0U) {
            xdj_feed_halve(&xdj_chip_tracks[xdj_slot_index(slot)]);
        }
#endif
    }
    (void)xdj_stream_seek(slot, (int)place, DRFLAC_SEEK_SET);
}

/*
 * Under the lock, before a fed track's read: whether tags may still be
 * handed out. Only while our block is in place, under the load the track was
 * opened with, and the converter table holds our entry (one word read). If
 * not, the track is decoded by the bridge from here on, at its play position
 * and still as a 16-bit stream; a frame half handed out is decoded again.
 */
static void xdj_fed_check(xdj_flac_slot* slot)
{
    if (xdj_chip.state == XDJ_CHIP_STATE_READY && slot->fed_load == xdj_chip.loads) {
        if (xdj_feed_chip_there()) {
            return;
        }
        /* The audio processor was started over: the next open puts our block back. */
        xdj_chip.losses++;
        xdj_chip.state = XDJ_CHIP_STATE_NOT_READY;
    }
    xdj_chip.fallbacks++;
    slot->fed = 0;
    slot->carry_valid = 0;
    slot->reposition = 1;
}
#endif

static drflac_uint64 xdj_read_frames(xdj_flac_slot* slot, drflac_uint64 frames, unsigned char* output)
{
    drflac_uint64 total = 0;

#ifdef XDJ_CHIP_FLAC
    if (slot->fed) {
#ifdef XDJ_HOST_TEST
        return xdj_fed_read(slot, frames, output);
#else
#ifdef XDJ_OWN_STACK
        /* The whole read is on our stack already (the wrappers below): never switch twice. */
        if (xdj_on_own_stack()) {
            return xdj_fed_read(slot, frames, output);
        }
#endif
        /* On a stack of our own (src/fed_stack.S): the input task's 2 KB were not enough. */
        return xdj_call_on_fed_stack(xdj_fed_read_entry, slot, (unsigned)frames, output);
#endif
    }
#endif

    if (slot->kind != XDJ_KIND_FLAC || xdj_slot_decimator(slot)->shift != 0U) {
        if (slot->kind != XDJ_KIND_FLAC) {
            total = xdj_pcm_read(xdj_slot_pcm(slot), xdj_slot_decimator(slot), frames, output, slot->bytes_per_frame);
        } else {
            total = xdj_flac_read_decimated(
                slot->decoder, xdj_slot_decimator(slot), frames, output, slot->bytes_per_frame
            );
        }
        if (total != frames) {
            slot->reposition = 1;
        }
        return total;
    }
    if (slot->bytes_per_frame == 6U) {
        return xdj_flac_read_s24(slot->decoder, frames, output);
    }
    if (((size_t)output & 1U) == 0U) {
        return xdj_flac_read_s16(slot->decoder, frames, (drflac_int16*)output);
    }
    while (total < frames) {
        drflac_int16 frame[2];
        const unsigned char* source = (const unsigned char*)frame;

        if (xdj_flac_read_s16(slot->decoder, 1U, frame) != 1U) {
            break;
        }
        output[0] = source[0];
        output[1] = source[1];
        output[2] = source[2];
        output[3] = source[3];
        output += 4;
        total++;
    }
    return total;
}

/*
 * Decode frames starting at the slot's play position. After a resync the
 * decoder is first sought back there. When nothing comes out, resynchronize
 * once before the caller declares a failure: the decoder may already be at
 * the play position with nothing left in its read-ahead, so it forgets its
 * place first and the seek rereads from the file.
 */
static drflac_uint64 xdj_decode_at_cursor(
    xdj_flac_slot* slot,
    drflac_uint64 frames,
    unsigned char* output,
    drflac_bool32* resynced
)
{
    drflac_uint64 decoded = 0;

    if ((!*resynced && !slot->reposition) || xdj_slot_seek(slot, slot->cursor_frame)) {
        decoded = xdj_read_frames(slot, frames, output);
    }
    *resynced = DRFLAC_FALSE;
    if (decoded == 0U && !slot->handle_lost) {
        xdj_flac_context.read_retries++;
        xdj_slot_invalidate(slot);
        if (xdj_slot_seek(slot, slot->cursor_frame)) {
            decoded = xdj_read_frames(slot, frames, output);
        }
    }
    return decoded;
}

/*
 * Open a decoder in a claimed slot through the given file handle. Only stereo
 * (with XDJ_MONO also mono, handed out as stereo, each sample on both sides)
 * 16-bit or 24-bit input at 44.1 or 48 kHz is accepted (88.2 and 96 kHz are
 * converted down). The file size is reported when the filesystem provides it, otherwise
 * zero.
 */
static drflac* xdj_open_decoder(
    xdj_flac_slot* slot,
    void* handle,
    xdj_flac_stream_info* info,
    drflac_uint32* file_bytes
)
{
    drflac_allocation_callbacks allocation_callbacks;
    drflac* decoder;

    slot->io.reserved = 0;
    slot->io.handle = handle;
    slot->io.last_operation = 0;
    slot->io.last_error = 0;
    slot->stream_position = 0;
    slot->arena_used = 0;
    slot->known_handle = NULL;
    slot->known_position = 0;
    slot->strict_checks = 1;

    allocation_callbacks.pUserData = slot;
    allocation_callbacks.onMalloc = xdj_arena_malloc;
    allocation_callbacks.onRealloc = xdj_arena_realloc;
    allocation_callbacks.onFree = xdj_arena_free;

    /*
     * The size routine reads the handle; a seek to the end would walk the
     * file. The header probe goes without the size: over LINK each query is
     * a network round trip, and with no size no read is followed by one.
     */
    *file_bytes = 0;
    if (slot == &xdj_flac_slots[XDJ_PROBE_SLOT] || XDJ_FW_SIZE_CALL(&slot->io, file_bytes) != 0) {
        *file_bytes = 0;
    }
    slot->file_bytes = *file_bytes;
    xdj_flac_seek_reset(&slot->seek, *file_bytes);

    if (slot->kind == XDJ_KIND_FLAC) {
        decoder = xdj_flac_open(
            xdj_stream_read,
            xdj_stream_seek,
            xdj_stream_tell,
            slot,
            &allocation_callbacks,
            info
        );
    } else {
        xdj_pcm_source* source = xdj_slot_pcm(slot);
        drflac_uint32 frames = 0;

        source->on_read = xdj_stream_read;
        source->on_seek = xdj_stream_seek;
        source->user_data = slot;
        info->channels = 2;
        decoder = NULL;
        if (xdj_pcm_open(source, slot->kind == XDJ_KIND_AIFF, *file_bytes, &info->sample_rate, &frames,
                         &info->bits_per_sample)) {
            info->frame_count = frames;
            /* Never dereferenced: marks the slot as in use, like a decoder. */
            decoder = (drflac*)source;
        }
    }
    slot->reposition = 0;
    xdj_flac_decimator_reset(xdj_slot_decimator(slot));
    if (decoder != NULL) {
        /* From here on the stream is described as the player gets it: 44.1 or 48 kHz. */
        if (info->sample_rate == 88200U || info->sample_rate == 96000U) {
            {
                xdj_slot_decimator(slot)->shift = 1;
                info->sample_rate >>= 1;
                info->frame_count >>= 1;
            }
        }
    }
    if (decoder != NULL &&
        (!XDJ_CHANNELS_TAKEN(info->channels) ||
         (info->bits_per_sample != 16U && info->bits_per_sample != 24U) ||
         (info->sample_rate != 44100U && info->sample_rate != 48000U
         ) ||
         info->frame_count == 0 ||
         info->frame_count * xdj_bytes_per_frame(info->bits_per_sample) > 0x7FFFFFFFULL)) {
        xdj_close_source(slot->kind, decoder);
        decoder = NULL;
    }
    /* The header reader leaves the file somewhere in the header: go to the first frame. */
    if (decoder != NULL && slot->kind != XDJ_KIND_FLAC &&
        !xdj_pcm_seek(xdj_slot_pcm(slot), 0, xdj_slot_decimator(slot))) {
        decoder = NULL;
    }
    if (decoder == NULL) {
        slot->arena_used = 0;
    }
#ifdef XDJ_CHIP_FLAC
    if (decoder != NULL) {
        xdj_fed_prepare(slot, info);
    } else {
        slot->fed = 0;
        slot->fed_form = 0;
        slot->fed_ready = 0;
    }
#endif
    return decoder;
}

/*
 * A call reads the file of a FLAC track whose decoder was evicted. Reopen it
 * through the call's handle and return to the recorded position. On failure
 * the caller reports an error: a FLAC file must never reach the raw stock read.
 */
static xdj_flac_slot* xdj_reopen_slot(
    xdj_flac_tombstone* tombstone,
    void* file_context,
    xdj_call_breakdown* breakdown
)
{
    xdj_flac_tombstone saved;
    xdj_flac_stream_info info;
    xdj_flac_slot* slot;
    drflac* decoder;
    drflac_uint32 file_bytes;
    void* handle;

    if (file_context == NULL) {
        xdj_flac_context.reopen_failures++;
        return NULL;
    }
    handle = XDJ_TASK_HANDLE(file_context);
    if (handle == NULL) {
        xdj_flac_context.reopen_failures++;
        return NULL;
    }
    /* Claiming a slot may record another tombstone, so work from a copy. */
    saved = *tombstone;
    tombstone->format_context = NULL;
    xdj_update_routing();
    slot = xdj_claim_slot();
    if (slot == NULL) {
        xdj_record_tombstone(&saved);
        xdj_flac_context.reopen_failures++;
        return NULL;
    }
    slot->breakdown = breakdown;
    slot->kind = saved.kind;
    decoder = xdj_open_decoder(slot, handle, &info, &file_bytes);
    if (decoder != NULL && info.frame_count != saved.frame_count) {
        /* Not the track this descriptor describes. */
        xdj_close_source(slot->kind, decoder);
        decoder = NULL;
        slot->arena_used = 0;
    }
    if (decoder == NULL) {
        /* Keep the descriptor known so it never reaches the raw stock read. */
        xdj_record_tombstone(&saved);
        xdj_flac_context.reopen_failures++;
        return NULL;
    }
    slot->decoder = decoder;
    slot->format_context = saved.format_context;
    slot->frame_count = saved.frame_count;
    slot->declared_bytes = saved.declared_bytes;
    slot->sample_rate = info.sample_rate;
    slot->bytes_per_frame = xdj_bytes_per_frame(info.bits_per_sample);
#ifdef XDJ_CHIP_FLAC
    /*
     * The player holds the descriptor it was given: the stream stays what it
     * was then. A fed track is fed again if it can be, and decoded here as a
     * 16-bit stream if not.
     */
    slot->fed_form = saved.fed;
    slot->fed = saved.fed && slot->fed_ready;
    if (slot->fed_form) {
        slot->bytes_per_frame = 4U;
    }
#endif
    slot->frame_offset = 0;
    slot->carry_valid = 0;
    slot->cursor_frame = 0;
    if (saved.cursor_frame >= slot->frame_count) {
        slot->cursor_frame = slot->frame_count;
    } else if (saved.cursor_frame == 0U) {
        slot->frame_offset = saved.frame_offset;
    } else if (xdj_slot_seek(slot, saved.cursor_frame)) {
        slot->cursor_frame = saved.cursor_frame;
        slot->frame_offset = saved.frame_offset;
    }
    xdj_touch_slot(slot);
    xdj_update_routing();
    xdj_flac_context.reopens++;
    return slot;
}


/*
 * FAT handles (filesystem type 3) keep their size and position in the handle,
 * so checking them on every call is free. Other types, above all the NFS
 * client LINK uses (type 5, whose size routine 0x08D74C28 waits on the
 * network), are checked once per handle. On the host every handle is strict.
 */
static drflac_bool32 xdj_strict_filesystem(void* handle)
{
#ifdef XDJ_HOST_TEST
    (void)handle;
#ifdef XDJ_HOST_NETWORK_FS
    return DRFLAC_FALSE;
#else
    return DRFLAC_TRUE;
#endif
#else
    drflac_uint32 device_byte;
    drflac_uint32 fs_type;

    xdj_handle_type(handle, &device_byte, &fs_type);
    return fs_type == 3U || fs_type == 0U;
#endif
}

static void xdj_query_call_size(xdj_call_file* file)
{
    xdj_fw_file_context probe = {0};

    probe.handle = file->handle;
    if (XDJ_FW_SIZE_CALL(&probe, &file->size) != 0) {
        file->size = 0;
        file->size_known = 0;
    } else {
        file->size_known = 1;
    }
}

/*
 * The call's handle and its filesystem's check policy, and its size. On a
 * network filesystem the file context's data length (+8, copied by the load
 * manager from the descriptor the adapter filled) identifies the track without
 * a network request when exactly one track has it; the size is queried only
 * when that is ambiguous.
 *
 * Our tracks are declared with their audio starting at byte 0, which no stock
 * WAV or AIFF does (a header comes first), so on a network filesystem a
 * context with another data start is not ours and is answered at once, with
 * no request: a size query for every read of a stock file starves the player
 * over LINK, and the same recording kept as a stock file has our track's
 * data length to the byte. The manager writes the
 * handle and these words together, so they belong to the same track.
 */
static xdj_flac_slot* xdj_identify_call_file(void* file_context, xdj_call_file* file)
{
    void* handle;
    size_t index;

    file->size = 0;
    file->declared = 0;
    file->handle = NULL;
    file->size_known = 0;
    file->strict = 1;
    if (file_context == NULL) {
        return NULL;
    }
    handle = XDJ_TASK_HANDLE(file_context);
    file->handle = handle;
    file->declared = XDJ_TASK_DATA_BYTES(file_context);
    if (handle == NULL) {
        return NULL;
    }
    file->strict = xdj_strict_filesystem(handle) ? 1U : 0U;
    if (!file->strict && XDJ_TASK_DATA_START(file_context) != 0U) {
        /* A size of 0 matches no track: the caller hands the call to the stock routine. */
        file->size_known = 1;
        return NULL;
    }
    if (!file->strict && file->declared != 0U) {
        xdj_flac_slot* match = NULL;
        xdj_flac_tombstone* evicted = NULL;
        size_t matches = 0;

        for (index = 0; index < XDJ_FLAC_SLOT_COUNT; ++index) {
            if (xdj_flac_slots[index].decoder != NULL &&
                xdj_flac_slots[index].declared_bytes == file->declared) {
                match = &xdj_flac_slots[index];
                matches++;
            }
        }
        for (index = 0; index < XDJ_FLAC_TOMBSTONE_COUNT; ++index) {
            if (xdj_flac_tombstones[index].format_context != NULL &&
                xdj_flac_tombstones[index].declared_bytes == file->declared) {
                evicted = &xdj_flac_tombstones[index];
                matches++;
            }
        }
        if (matches == 1U && match != NULL) {
            return match;
        }
        if (matches == 1U && evicted != NULL) {
            /* Let the caller find and reopen it by its recorded size. */
            file->size = evicted->file_bytes;
            file->size_known = 1;
            return NULL;
        }
    }
    xdj_query_call_size(file);
    return NULL;
}

/*
 * The active slot whose file is the call's: same size, preferring one whose
 * declared length matches too. The data length is only a tie-breaker, since
 * a file context may be read before the load manager fills it in.
 */
static xdj_flac_slot* xdj_find_slot_for_file(const xdj_call_file* file)
{
    xdj_flac_slot* found = NULL;
    size_t index;

    for (index = 0; index < XDJ_FLAC_SLOT_COUNT; ++index) {
        xdj_flac_slot* slot = &xdj_flac_slots[index];

        if (slot->decoder == NULL || slot->file_bytes == 0U || slot->file_bytes != file->size) {
            continue;
        }
        if (file->declared == slot->declared_bytes) {
            return slot;
        }
        if (found == NULL) {
            found = slot;
        }
    }
    return found;
}

static xdj_flac_tombstone* xdj_find_tombstone_for_file(const xdj_call_file* file)
{
    xdj_flac_tombstone* found = NULL;
    size_t index;

    for (index = 0; index < XDJ_FLAC_TOMBSTONE_COUNT; ++index) {
        xdj_flac_tombstone* tombstone = &xdj_flac_tombstones[index];

        if (tombstone->format_context == NULL || tombstone->file_bytes == 0U ||
            tombstone->file_bytes != file->size) {
            continue;
        }
        if (file->declared == tombstone->declared_bytes) {
            return tombstone;
        }
        if (found == NULL) {
            found = tombstone;
        }
    }
    return found;
}

/* An opening slot (parse running without the lock) whose file is the call's. */
static drflac_bool32 xdj_file_is_opening(const xdj_call_file* file)
{
    size_t index;

    for (index = 0; index < XDJ_FLAC_SLOT_COUNT; ++index) {
        if (xdj_flac_slots[index].opening && xdj_flac_slots[index].file_bytes != 0U &&
            xdj_flac_slots[index].file_bytes == file->size) {
            return DRFLAC_TRUE;
        }
    }
    return DRFLAC_FALSE;
}

/*
 * Slot for a PCM request, chosen by the file the call reads.
 * The player parses every track into one working descriptor and
 * copies it in and out of its track cache, so a descriptor or format object
 * address says nothing about which track a call is for. The file context
 * does: its handle's file and its data length come from the track's own
 * descriptor. In order:
 *  - an active FLAC track whose file this is;
 *  - a FLAC track still being parsed for this file (waited for);
 *  - an evicted FLAC track whose file this is (reopened through this handle);
 *  - otherwise NULL: the file is no FLAC track, and the stock routine reads
 *    it, as it would for a WAV track.
 * A zeroed or closed handle sets *stale: the caller answers with silence or
 * a no-op.
 * Sets *failure to an XDJ_FAILURE code when the call is FLAC but no decoder
 * can serve it.
 */
static xdj_flac_slot* xdj_request_slot(
    void* format_context,
    void* file_context,
    drflac_uint32* failure,
    drflac_bool32* stale,
    xdj_call_breakdown* breakdown
)
{
    xdj_flac_slot* slot;
    xdj_flac_tombstone* tombstone;
    xdj_call_file file;
    drflac_uint32 waited;
    drflac_uint32 wait_start;
    drflac_uint32 reopen_start;

    (void)format_context;
    *failure = 0;
    *stale = DRFLAC_FALSE;
    slot = xdj_identify_call_file(file_context, &file);
    if (slot != NULL) {
        slot->strict_checks = 0;
        return slot;
    }
#ifndef XDJ_HOST_TRUST_EVERY_HANDLE
    if (file.handle == NULL) {
        /*
         * A zeroed handle: the load manager has just started a new routine
         * over this context. No track can be read through
         * it; answer as a stale window.
         */
        breakdown->stale_calls++;
        xdj_flac_context.stale_calls++;
        xdj_take_snapshot(XDJ_SNAPSHOT_NO_HANDLE, &file, 0U, 0U);
        *stale = DRFLAC_TRUE;
        return NULL;
    }
    if (!file.size_known) {
        /*
         * A handle whose size cannot be read. The size routine exists for every
         * filesystem type (FAT, and types 4 to 7), so this is a handle closed
         * under us: a stale window too.
         */
        breakdown->foreign_stale++;
        xdj_flac_context.foreign_stale++;
        xdj_take_snapshot(XDJ_SNAPSHOT_FOREIGN, &file, 0U, 0U);
        *stale = DRFLAC_TRUE;
        return NULL;
    }
#endif
    slot = xdj_find_slot_for_file(&file);
    if (slot == NULL && xdj_file_is_opening(&file)) {
        wait_start = XDJ_TIMER_NOW();
        for (waited = 0; waited < XDJ_OPEN_WAIT_TICKS && xdj_file_is_opening(&file); ++waited) {
            xdj_flac_lock_release();
            XDJ_TASK_DELAY_CALL(1);
            xdj_flac_lock_acquire();
        }
        breakdown->wait += xdj_since(wait_start);
        xdj_flac_context.opening_waits++;
        if (xdj_file_is_opening(&file)) {
            *failure = XDJ_FAILURE_OPENING;
            return NULL;
        }
        slot = xdj_find_slot_for_file(&file);
    }
    if (slot != NULL) {
        if (slot->io.handle != file.handle) {
            xdj_take_snapshot(XDJ_SNAPSHOT_BASELINE, &file, slot->file_bytes, 2U);
        }
        slot->strict_checks = file.strict;
        return slot;
    }
    tombstone = xdj_find_tombstone_for_file(&file);
    if (tombstone != NULL) {
        reopen_start = XDJ_TIMER_NOW();
        slot = xdj_reopen_slot(tombstone, file_context, breakdown);
        breakdown->reopen += xdj_since(reopen_start);
        if (slot == NULL) {
            *failure = XDJ_FAILURE_REOPEN;
        } else {
            slot->strict_checks = file.strict;
        }
        return slot;
    }
    xdj_take_snapshot(XDJ_SNAPSHOT_STALE, &file, 0U, 0U);
    return NULL;
}

/* The kind of the file being parsed, from the dispatcher's file type. */
static drflac_uint8 xdj_file_kind(const void* output_descriptor)
{
    drflac_uint32 type = *(const drflac_uint32*)((const unsigned char*)output_descriptor + XDJ_FILE_TYPE_OFFSET);

    return type == XDJ_FILE_TYPE_WAV ? XDJ_KIND_WAV : type == XDJ_FILE_TYPE_AIFF ? XDJ_KIND_AIFF : XDJ_KIND_FLAC;
}

/*
 * Whether a WAV or AIFF file is one the bridge serves. Most are not: they
 * are the stock reader's, and it must find the handle where it was. The
 * header is read on the probe slot, which claims nothing and evicts nothing.
 */
static drflac_bool32 xdj_pcm_is_ours(void* handle, drflac_uint8 kind)
{
    xdj_flac_slot* probe = &xdj_flac_slots[XDJ_PROBE_SLOT];
    xdj_flac_stream_info info;
    drflac_uint32 file_bytes;
    drflac* source;
    int position;

    probe->kind = kind;
    probe->handle_lost = 0;
    probe->resync = 0;
    probe->breakdown = NULL;
    probe->io.reserved = 0;
    probe->io.handle = handle;
    position = XDJ_FW_TELL_CALL(&probe->io);
    source = xdj_open_decoder(probe, handle, &info, &file_bytes);
    (void)XDJ_FW_SEEK_CALL(&probe->io, position < 0 ? 0 : position, 0);
    return source != NULL;
}

static int xdj_flac_adapter_impl(
    void* file_io_context,
    void* output_descriptor,
    void* parser_workspace,
    xdj_call_breakdown* breakdown
)
{
    xdj_flac_stream_info info;
    xdj_flac_slot* slot;
    drflac* decoder;
    void* format_context;
    drflac_uint32 bytes_per_frame;
    drflac_uint32 bytes_per_cd_frame;
    drflac_uint32 total_cd_frames;
    drflac_uint32 pcm_bytes;
    drflac_uint32 declared_bytes;
    drflac_uint32 file_bytes;
    drflac_uint8 kind = xdj_file_kind(output_descriptor);

    (void)parser_workspace;
    if (kind != XDJ_KIND_FLAC && !xdj_pcm_is_ours(XDJ_PARSER_HANDLE(file_io_context), kind)) {
        return XDJ_ADAPTER_DECLINED;
    }
    format_context = (unsigned char*)output_descriptor + XDJ_PCM_FORMAT_OFFSET;
    xdj_lock_timed(breakdown);
    xdj_flac_context.open_count++;

    /*
     * Every track is parsed into the same working descriptor,
     * so a dispatch says nothing about the tracks parsed before: they
     * keep their decoders, and a slot is claimed (evicting the least recently
     * used track, which keeps a record for reopening).
     */
    slot = xdj_claim_slot();
    if (slot == NULL) {
        xdj_flac_context.open_failures++;
        xdj_flac_lock_release();
        return -1;
    }
    /*
     * The open reads the file header over USB. Reserve the slot and release
     * the lock meanwhile, so the playing track keeps decoding. The reserved
     * slot is private to this task: it is never found, claimed, or evicted.
     */
    slot->format_context = format_context;
    slot->opening = 1;
    slot->breakdown = breakdown;
    slot->kind = kind;
    xdj_update_routing();
    xdj_flac_lock_release();

    decoder = xdj_open_decoder(slot, XDJ_PARSER_HANDLE(file_io_context), &info, &file_bytes);

    xdj_lock_timed(breakdown);
    xdj_note_size(slot, breakdown);
    slot->opening = 0;
    if (decoder == NULL) {
        slot->format_context = NULL;
        xdj_update_routing();
        xdj_flac_context.open_failures++;
        xdj_flac_lock_release();
        return -1;
    }

    /*
     * The stream keeps the source's depth: signed 16-bit or packed 24-bit
     * little-endian stereo, declared like the stock WAV adapter declares it.
     */
    bytes_per_frame = xdj_bytes_per_frame(info.bits_per_sample);
#ifdef XDJ_CHIP_FLAC
    /*
     * A fed track is 16-bit whatever the file's depth: its frames are tags.
     * A track already served by another slot keeps the form that slot has,
     * and is fed only if it can be.
     */
    {
        size_t index;
        drflac_uint8 form = slot->fed_ready;

        for (index = 0; index < XDJ_FLAC_SLOT_COUNT; ++index) {
            const xdj_flac_slot* other = &xdj_flac_slots[index];

            if (other != slot && other->decoder != NULL &&
                xdj_same_track(other->file_bytes, other->frame_count, file_bytes, info.frame_count)) {
                form = other->fed_form;
            }
        }
        slot->fed_form = form;
        slot->fed = form && slot->fed_ready;
        if (form) {
            bytes_per_frame = 4U;
        }
    }
#endif
    pcm_bytes = (drflac_uint32)info.frame_count * bytes_per_frame;
    /*
     * The stock player stops file reads at the declared data length, so a
     * file larger than its PCM could not be read to its end: a FLAC with
     * large artwork, and every converted WAV or AIFF, whose file holds twice
     * the frames. Such files declare their file size, rounded up to whole
     * frames. The track time still comes from the real frame count, and reads
     * past the real audio report end of data.
     */
    declared_bytes = pcm_bytes;
    if (file_bytes > pcm_bytes && file_bytes <= 0x7FFFFFF0UL) {
        declared_bytes = bytes_per_frame == 6U ? ((file_bytes + 5U) / 6U) * 6U : (file_bytes + 3U) & ~3U;
    }
    bytes_per_cd_frame = (info.sample_rate * bytes_per_frame) / XDJ_CD_FRAMES_PER_SECOND;
    total_cd_frames =
        (drflac_uint32)((info.frame_count * XDJ_CD_FRAMES_PER_SECOND +
                         info.sample_rate - 1U) /
                        info.sample_rate);

    /*
     * The same file parsed again (a bank holding the playing track, a reload):
     * keep the decoder that already serves it, with its position, and drop
     * this one. The parse has filled the descriptor, which is all the player
     * needs from it. An evicted record for the file is replaced by this one.
     */
    {
        size_t index;
        xdj_flac_slot* existing = NULL;

        for (index = 0; index < XDJ_FLAC_SLOT_COUNT; ++index) {
            xdj_flac_slot* other = &xdj_flac_slots[index];

            if (other != slot && other->decoder != NULL &&
                xdj_same_track(other->file_bytes, other->frame_count, file_bytes, info.frame_count)) {
                existing = other;
            }
        }
        for (index = 0; index < XDJ_FLAC_TOMBSTONE_COUNT; ++index) {
            xdj_flac_tombstone* tombstone = &xdj_flac_tombstones[index];

            if (tombstone->format_context != NULL &&
                xdj_same_track(tombstone->file_bytes, tombstone->frame_count, file_bytes, info.frame_count)) {
                tombstone->format_context = NULL;
            }
        }
        if (existing != NULL) {
            xdj_close_source(kind, decoder);
            slot->decoder = NULL;
            slot->format_context = NULL;
            slot->arena_used = 0;
            xdj_touch_slot(existing);
            xdj_flac_context.duplicate_parses++;
        } else {
            slot->decoder = decoder;
            slot->format_context = format_context;
            slot->frame_count = info.frame_count;
            slot->cursor_frame = 0;
            slot->bytes_per_frame = (drflac_uint8)bytes_per_frame;
            slot->frame_offset = 0;
            slot->carry_valid = 0;
            slot->declared_bytes = declared_bytes;
            slot->sample_rate = info.sample_rate;
            xdj_touch_slot(slot);
        }
    }
    xdj_update_routing();

    xdj_write_u32(output_descriptor, XDJ_OUTPUT_PATH_OFFSET, 1U);
    xdj_write_u32(output_descriptor, XDJ_TOTAL_CD_FRAMES_OFFSET, total_cd_frames);
    xdj_write_u32(output_descriptor, XDJ_DATA_START_OFFSET, 0U);
    xdj_write_u32(output_descriptor, XDJ_DATA_BYTES_OFFSET, declared_bytes);
    xdj_write_u32(output_descriptor, XDJ_TRAILING_BYTES_OFFSET, 0U);
    xdj_write_u32(output_descriptor, XDJ_PCM_FORMAT_OFFSET, bytes_per_cd_frame);
    xdj_write_u8(output_descriptor, XDJ_PCM_FORMAT_OFFSET + 4U, 0U);
    xdj_write_u8(output_descriptor, XDJ_PCM_FORMAT_OFFSET + 5U, 2U);
    xdj_write_u8(
        output_descriptor,
        XDJ_PCM_FORMAT_OFFSET + 6U,
        (drflac_uint8)(info.sample_rate / 1000U)
    );
    xdj_write_u8(output_descriptor, XDJ_PCM_FORMAT_OFFSET + 7U, (drflac_uint8)(bytes_per_frame >> 1));
    xdj_flac_lock_release();
    return 0;
}

#ifdef XDJ_FETCH_PACE_MS
/*
 * Spread the filling of the read-ahead (see XDJ_DSP_AHEAD_REGISTER). Called
 * at the end of a served read, without the lock: sleep until the read has
 * taken its pace for each 1/75 s (unit_bytes) of the done bytes, counted
 * from start. Only the fills ahead and behind are paced, by the audio in
 * hand on their side; a read that was slow by itself (a network file) is
 * not slowed more.
 */
static void xdj_pace_fetch(drflac_uint32 start, drflac_uint32 done, drflac_uint32 unit_bytes, xdj_call_breakdown* breakdown)
{
    drflac_uint32 command = XDJ_INPUT_COMMAND();
    drflac_uint32 sleep_start;
    drflac_uint32 in_hand;
    drflac_uint32 due;
    drflac_uint32 slices;

    if (command != 1U && command != 2U) {
        return;
    }
    in_hand = XDJ_DSP_REGISTER(command == 1U ? XDJ_DSP_AHEAD_REGISTER : XDJ_DSP_BEHIND_REGISTER);
    if (in_hand < XDJ_FETCH_REFILL_AHEAD_UNITS || in_hand > XDJ_FETCH_PACE_AHEAD_LIMIT) {
        return;
    }
    due = done * XDJ_UNITS_PER_MS *
          (in_hand >= XDJ_FETCH_PACE_AHEAD_UNITS ? (drflac_uint32)XDJ_FETCH_PACE_MS : (drflac_uint32)XDJ_FETCH_REFILL_MS);
    sleep_start = XDJ_TIMER_NOW();
    for (slices = 0; slices < XDJ_FETCH_PACE_MAX_SLICES; ++slices) {
        if (XDJ_INPUT_ABORT() != 0U || xdj_since(start) * unit_bytes >= due) {
            break;
        }
        XDJ_PACE_SLEEP(2);
    }
    breakdown->wait += xdj_since(sleep_start);
}
#endif

static size_t xdj_flac_virtual_read_impl(
    void* format_context,
    void* file_context,
    void* output,
    size_t byte_count,
    int* transfer_status,
    void* fs_state,
    xdj_call_breakdown* breakdown
)
{
    xdj_flac_slot* slot;
    unsigned char* bytes;
    drflac_uint32 remaining_bytes;
    drflac_uint32 wanted;
    drflac_uint32 done;
    drflac_uint32 start_position;
    drflac_uint32 failed;
    drflac_uint32 decode_start;
    drflac_uint32 breaks_before;
#ifdef XDJ_FETCH_PACE_MS
    drflac_uint32 unit_bytes;
#endif
    drflac_uint8 bytes_per_frame;
    drflac_bool32 decode_failed;
    drflac_bool32 stale;
    drflac_bool32 resynced;

    breakdown->is_read = 1;
    xdj_lock_timed(breakdown);
    slot = xdj_request_slot(format_context, file_context, &failed, &stale, breakdown);
    if (stale) {
        /*
         * A stale window: the command this read belongs to is being replaced,
         * and its data will be discarded. Answer with silence so the stream
         * loop and the DSP fill carry on, and leave the decoder alone.
         */
        size_t silent = byte_count & ~(size_t)3U;

        breakdown->served = 1;
        xdj_flac_lock_release();
        if (output != NULL) {
            unsigned char* bytes = (unsigned char*)output;
            size_t index;

            for (index = 0; index < silent; ++index) {
                bytes[index] = 0;
            }
        }
        if (transfer_status != NULL) {
            *transfer_status = 0;
        }
        return output != NULL ? silent : 0U;
    }
    if (slot != NULL) {
        slot->breakdown = breakdown;
        breakdown->served = 1;
    }
    if (slot == NULL) {
        xdj_flac_lock_release();
        if (failed) {
            breakdown->failure = (drflac_uint8)failed;
            xdj_delay_before_error(failed);
            if (transfer_status != NULL) {
                *transfer_status = -1;
            }
            return 0;
        }
        return XDJ_STOCK_READ_CALL(file_context, output, byte_count, transfer_status, fs_state);
    }
    if (transfer_status != NULL) {
        *transfer_status = 0;
    }
    xdj_touch_slot(slot);
    xdj_attach_task_handle(slot, file_context);
    resynced = xdj_begin_slot_call(slot);
    xdj_note_size(slot, breakdown);
#ifdef XDJ_CHIP_FLAC
    if (slot->fed) {
        xdj_fed_check(slot);
    }
#endif
    if (output == NULL) {
        xdj_flac_lock_release();
        if (transfer_status != NULL) {
            *transfer_status = -1;
        }
        return 0;
    }
    /* Like the stock read, report end of data as zero bytes with status 0. */
    if (slot->cursor_frame >= slot->frame_count) {
        xdj_flac_lock_release();
        return 0;
    }
    bytes_per_frame = slot->bytes_per_frame;
    remaining_bytes =
        (drflac_uint32)(slot->frame_count - slot->cursor_frame) * bytes_per_frame - slot->frame_offset;
    wanted = (drflac_uint32)byte_count;
    if (wanted > remaining_bytes) {
        wanted = remaining_bytes;
    }
    if (wanted == 0U) {
        xdj_flac_lock_release();
        return 0;
    }
    if (resynced) {
        slot->carry_valid = 0;
    }
    bytes = (unsigned char*)output;
    start_position = (drflac_uint32)slot->cursor_frame * bytes_per_frame + slot->frame_offset;
    done = 0;
    decode_failed = DRFLAC_FALSE;
    decode_start = XDJ_TIMER_NOW();
    breaks_before = xdj_flac_sequence_breaks;

    /* The rest of a frame an earlier read (or a seek) left half delivered. */
    if (slot->frame_offset != 0U) {
        if (!slot->carry_valid && xdj_decode_at_cursor(slot, 1U, slot->carry, &resynced) == 1U) {
            slot->carry_valid = 1;
        }
        if (!slot->carry_valid) {
            decode_failed = DRFLAC_TRUE;
        } else {
            drflac_uint32 part = (drflac_uint32)bytes_per_frame - slot->frame_offset;
            drflac_uint32 index;

            if (part > wanted) {
                part = wanted;
            }
            for (index = 0; index < part; ++index) {
                bytes[index] = slot->carry[slot->frame_offset + index];
            }
            done = part;
            slot->frame_offset = (drflac_uint8)(slot->frame_offset + part);
            if (slot->frame_offset == bytes_per_frame) {
                /* The decoder already stands after this frame. */
                slot->frame_offset = 0;
                slot->carry_valid = 0;
                slot->cursor_frame++;
            }
        }
    }
    /* Whole frames, decoded straight into the player's buffer. */
    if (!decode_failed && slot->frame_offset == 0U) {
        drflac_uint64 whole = xdj_whole_frames(wanted - done, bytes_per_frame);

        if (whole != 0U) {
            drflac_uint64 decoded = xdj_decode_at_cursor(slot, whole, bytes + done, &resynced);

            if (decoded != 0U && !slot->handle_lost
#ifdef XDJ_CHIP_FLAC
                && !slot->fed
#endif
            ) {
                xdj_play_signals(bytes + done, decoded, slot->sample_rate, bytes_per_frame);
            }
            slot->cursor_frame += decoded;
            done += (drflac_uint32)decoded * bytes_per_frame;
            if (decoded != whole) {
                decode_failed = DRFLAC_TRUE;
            }
        }
        /* The start of a frame the read ends inside. */
        if (!decode_failed && done < wanted) {
            if (xdj_decode_at_cursor(slot, 1U, slot->carry, &resynced) != 1U) {
                decode_failed = DRFLAC_TRUE;
            } else {
                drflac_uint32 part = wanted - done;
                drflac_uint32 index;

                for (index = 0; index < part; ++index) {
                    bytes[done + index] = slot->carry[index];
                }
                slot->carry_valid = 1;
                slot->frame_offset = (drflac_uint8)part;
                done += part;
            }
        }
    }
    breakdown->decode += xdj_since(decode_start);
    breakdown->sequence_breaks += xdj_flac_sequence_breaks - breaks_before;
    if (xdj_end_if_lost(slot, breakdown)) {
        /*
         * The player closed or reused the handle during this read: a stale
         * window after all. Answer with silence, keep the play position where
         * the player expects it, and resync the decoder next time.
         */
        drflac_uint32 index;
        drflac_uint32 position;
        drflac_uint32 frames;

        for (index = 0; index < wanted; ++index) {
            bytes[index] = 0;
        }
        position = start_position + wanted;
        frames = xdj_whole_frames(position, bytes_per_frame);
        slot->cursor_frame = frames;
        slot->frame_offset = (drflac_uint8)(position - frames * bytes_per_frame);
        slot->carry_valid = 0;
        xdj_flac_lock_release();
        return (size_t)wanted;
    }
    if (done == 0U) {
#ifdef XDJ_HOST_DEBUG_FAILURES
        fprintf(stderr, "read failure: slot %zu cursor %llu of %llu resynced %d seek failure %u stream %u file %u "
                "decoder at %llu remaining %u wanted %u\n",
                xdj_slot_index(slot), (unsigned long long)slot->cursor_frame, (unsigned long long)slot->frame_count,
                (int)resynced, (unsigned)slot->seek.failure, slot->stream_position, slot->file_bytes,
                slot->kind == XDJ_KIND_FLAC ? (unsigned long long)slot->decoder->currentPCMFrame : 0ULL,
                slot->kind == XDJ_KIND_FLAC ? (unsigned)slot->decoder->currentFLACFrame.pcmFramesRemaining : 0U,
                (unsigned)wanted);
#endif
        /* Data is missing before the declared end: an error, not end of track. */
        xdj_flac_context.read_failures++;
        breakdown->failure = XDJ_FAILURE_READ;
        xdj_flac_lock_release();
        xdj_delay_before_error(XDJ_FAILURE_READ);
        if (transfer_status != NULL) {
            *transfer_status = -1;
        }
        return 0;
    }
#ifdef XDJ_FETCH_PACE_MS
    /* Bytes of one 1/75 s at the rate the player gets, read while the slot is ours. */
    unit_bytes = (drflac_uint32)bytes_per_frame * (slot->sample_rate == 44100U ? 588U : 640U);
#ifdef XDJ_CHIP_FLAC
    /* Nothing was decoded here: there is no load to spread. */
    if (slot->fed) {
        xdj_flac_lock_release();
        return (size_t)done;
    }
#endif
    xdj_flac_lock_release();
    xdj_pace_fetch(decode_start, done, unit_bytes, breakdown);
#else
    xdj_flac_lock_release();
#endif
    return (size_t)done;
}

static int xdj_flac_virtual_seek_impl(
    void* format_context,
    void* file_context,
    int byte_offset,
    int origin,
    void* fs_state,
    xdj_call_breakdown* breakdown
)
{
    xdj_flac_slot* slot;
    drflac_int64 target_byte;
    drflac_uint64 total_byte_count;
    drflac_uint32 target_frame;
    drflac_uint32 failed;
    drflac_uint32 seek_start;
    drflac_uint8 bytes_per_frame;
    drflac_uint8 target_offset;
    drflac_bool32 sought;
    drflac_bool32 stale;
    drflac_bool32 resynced;

    xdj_lock_timed(breakdown);
    slot = xdj_request_slot(format_context, file_context, &failed, &stale, breakdown);
    if (stale) {
        /* A stale window: succeed without moving; the next command seeks again. */
        breakdown->served = 1;
        xdj_flac_lock_release();
        return 0;
    }
    if (slot != NULL) {
        slot->breakdown = breakdown;
        breakdown->served = 1;
    }
    if (slot == NULL) {
        xdj_flac_lock_release();
        if (failed) {
            breakdown->failure = (drflac_uint8)failed;
            xdj_delay_before_error(failed);
            return XDJ_SEEK_IO_ERROR;
        }
        return XDJ_STOCK_SEEK_CALL(file_context, byte_offset, origin, fs_state);
    }
    xdj_touch_slot(slot);
    xdj_attach_task_handle(slot, file_context);
    resynced = xdj_begin_slot_call(slot);
    xdj_note_size(slot, breakdown);
    bytes_per_frame = slot->bytes_per_frame;
    total_byte_count = slot->frame_count * bytes_per_frame;
    if (origin == 0) {
        target_byte = byte_offset;
    } else if (origin == 1) {
        target_byte = (drflac_int64)(slot->cursor_frame * bytes_per_frame + slot->frame_offset) + byte_offset;
    } else if (origin == 2) {
        target_byte = (drflac_int64)total_byte_count + byte_offset;
    } else {
        xdj_flac_lock_release();
        return XDJ_SEEK_RANGE_ERROR;
    }
    if (target_byte < 0 || (drflac_uint64)target_byte > slot->declared_bytes) {
        xdj_flac_lock_release();
        return XDJ_SEEK_RANGE_ERROR;
    }
    /* Any byte is a valid target, as in a WAV file: a frame and an offset into it. */
    target_frame = xdj_whole_frames((drflac_uint32)target_byte, bytes_per_frame);
    target_offset = (drflac_uint8)((drflac_uint32)target_byte - target_frame * bytes_per_frame);
    /*
     * After a resync the decoder has forgotten its place, so even a seek to
     * the play position must put it back: nothing below may skip the seek.
     */
    if (target_frame != slot->cursor_frame || target_offset != slot->frame_offset) {
        breakdown->moved = 1;
    } else if (!resynced) {
        xdj_flac_lock_release();
        return 0;
    }
    /*
     * Seeking to the end, or into the padding between the real end and the
     * declared length, needs no decoding; reads there report end of data.
     * dr_flac itself is left where it is and is repositioned by the next seek
     * that lands inside the stream.
     */
    if (target_frame >= slot->frame_count) {
        slot->cursor_frame = slot->frame_count;
        slot->frame_offset = 0;
        slot->carry_valid = 0;
        xdj_flac_lock_release();
        return 0;
    }
    /*
     * Another byte of the frame the play position is in. The decoder already
     * stands at that frame, or one past it with the frame carried; only a
     * move to the start of a carried frame needs the decoder moved back.
     */
    if (!resynced && target_frame == slot->cursor_frame && (!slot->carry_valid || target_offset != 0U)) {
        slot->frame_offset = target_offset;
        xdj_flac_lock_release();
        return 0;
    }
    seek_start = XDJ_TIMER_NOW();
    sought = xdj_slot_seek(slot, target_frame);
    breakdown->flac_seek += xdj_since(seek_start);
    if (xdj_end_if_lost(slot, breakdown)) {
        /* The handle was lost during the seek: the next read seeks again. */
        slot->cursor_frame = target_frame;
        slot->frame_offset = target_offset;
        xdj_flac_lock_release();
        return 0;
    }
    if (!sought) {
        xdj_flac_context.seek_failures++;
        breakdown->failure = XDJ_FAILURE_SEEK;
        xdj_flac_lock_release();
        xdj_delay_before_error(XDJ_FAILURE_SEEK);
        return XDJ_SEEK_IO_ERROR;
    }
    slot->cursor_frame = target_frame;
    slot->frame_offset = target_offset;
    xdj_flac_lock_release();
    return 0;
}

/*
 * Called for every non-FLAC dispatch. The working descriptor is shared by all
 * tracks, so another format's parse releases nothing: FLAC
 * tracks are found by their files.
 */
void xdj_flac_deactivate(void* output_descriptor)
{
    (void)output_descriptor;
}

/*
 * Timed entry points. Timing never changes a result; the episode bookkeeping
 * runs under the lock because the parser and the input-file task both call in.
 */
int xdj_flac_adapter(
    void* file_io_context,
    void* output_descriptor,
    void* parser_workspace
)
{
    XDJ_WATCH_ENTER(XDJ_WATCH_ENTRY_OPEN)
    xdj_call_breakdown breakdown = {0};
    drflac_uint32 start = XDJ_TIMER_NOW();
    int result = xdj_flac_adapter_impl(file_io_context, output_descriptor, parser_workspace, &breakdown);
    drflac_uint32 end = XDJ_TIMER_NOW();

    if (result == XDJ_ADAPTER_DECLINED) {
        /* The stock reader's file: no slot was touched and nothing is recorded. */
        return result;
    }
    xdj_detach_breakdown(&breakdown);
    xdj_flac_lock_acquire();
    xdj_note_open(start, end, &breakdown);
    xdj_flac_lock_release();
    return result;
}

/*
 * The PCM trampolines pass the task state's format object with each call, so
 * no shared field written outside the lock can name another call's track.
 * The slot is still chosen by the call's file (xdj_request_slot).
 */
#if defined(XDJ_OWN_STACK) && !defined(XDJ_HOST_TEST)
#define xdj_flac_virtual_read_format xdj_read_format_here
#define xdj_flac_virtual_seek_format xdj_seek_format_here
#define xdj_flac_virtual_eof_format xdj_eof_format_here
static size_t xdj_read_format_here(void* file_context, void* output, size_t byte_count, int* transfer_status,
                                   void* fs_state, void* format_context);
static int xdj_seek_format_here(void* file_context, int byte_offset, int origin, void* fs_state, void* format_context);
static int xdj_eof_format_here(void* file_context, void* fs_state, void* format_context);
#define XDJ_ENTRY static
#else
#define XDJ_ENTRY
#endif
XDJ_ENTRY size_t xdj_flac_virtual_read_format(
    void* file_context,
    void* output,
    size_t byte_count,
    int* transfer_status,
    void* fs_state,
    void* format_context
)
{
    XDJ_WATCH_BODY_ENTER(XDJ_WATCH_ENTRY_READ)
    xdj_call_breakdown breakdown = {0};
    drflac_uint32 start = XDJ_TIMER_NOW();
    size_t result = xdj_flac_virtual_read_impl(
        format_context, file_context, output, byte_count, transfer_status, fs_state, &breakdown
    );
    drflac_uint32 end = XDJ_TIMER_NOW();

    xdj_detach_breakdown(&breakdown);
    xdj_flac_lock_acquire();
    xdj_note_call(start, end, &breakdown);
    xdj_flac_lock_release();
    return result;
}

XDJ_ENTRY int xdj_flac_virtual_seek_format(
    void* file_context,
    int byte_offset,
    int origin,
    void* fs_state,
    void* format_context
)
{
    XDJ_WATCH_BODY_ENTER(XDJ_WATCH_ENTRY_SEEK)
    xdj_call_breakdown breakdown = {0};
    drflac_uint32 start = XDJ_TIMER_NOW();
    int result = xdj_flac_virtual_seek_impl(
        format_context, file_context, byte_offset, origin, fs_state, &breakdown
    );
    drflac_uint32 end = XDJ_TIMER_NOW();

    xdj_detach_breakdown(&breakdown);
    xdj_flac_lock_acquire();
    xdj_note_call(start, end, &breakdown);
    xdj_flac_lock_release();
    return result;
}

/*
 * The PCM path's end-of-stream test, for the four hooked call sites.
 * For a FLAC track the stock test would look at the raw
 * FLAC file's handle; the answer comes from the virtual position instead. A
 * stale window answers "not ended" and other formats go to the stock test.
 */
XDJ_ENTRY int xdj_flac_virtual_eof_format(void* file_context, void* fs_state, void* format_context)
{
    XDJ_WATCH_BODY_ENTER(XDJ_WATCH_ENTRY_EOF)
    xdj_call_breakdown breakdown = {0};
    xdj_flac_slot* slot;
    drflac_uint32 failed;
    drflac_bool32 stale;
    int ended;

    xdj_flac_lock_acquire();
    slot = xdj_request_slot(format_context, file_context, &failed, &stale, &breakdown);
    xdj_detach_breakdown(&breakdown);
    if (stale || (slot == NULL && failed)) {
        xdj_flac_lock_release();
        return 0;
    }
    if (slot == NULL) {
        xdj_flac_lock_release();
        return XDJ_STOCK_EOF_CALL(file_context, fs_state);
    }
    xdj_touch_slot(slot);
    ended = slot->cursor_frame >= slot->frame_count ? 1 : 0;
    if (ended) {
        xdj_flac_context.eof_ends++;
    }
    XDJ_CLEAR_CONTEXT_STATUS(file_context);
    xdj_flac_lock_release();
    return ended;
}

#if defined(XDJ_OWN_STACK) && !defined(XDJ_HOST_TEST)
#undef xdj_flac_virtual_read_format
#undef xdj_flac_virtual_seek_format
#undef xdj_flac_virtual_eof_format

/* One call's arguments and result, handed to our stack. */
typedef struct {
    void* file_context;
    void* buffer;
    size_t byte_count;
    int* transfer_status;
    void* fs_state;
    void* format_context;
    int byte_offset;
    int origin;
    size_t result;
} xdj_entry_call;


static unsigned xdj_read_on_own_stack(void* argument, unsigned unused_b, unsigned char* unused_c)
{
    xdj_entry_call* call = (xdj_entry_call*)argument;

    (void)unused_b;
    (void)unused_c;
    call->result = xdj_read_format_here(call->file_context, call->buffer, call->byte_count, call->transfer_status,
                                        call->fs_state, call->format_context);
    return 0;
}

static unsigned xdj_seek_on_own_stack(void* argument, unsigned unused_b, unsigned char* unused_c)
{
    xdj_entry_call* call = (xdj_entry_call*)argument;

    (void)unused_b;
    (void)unused_c;
    call->result = (size_t)xdj_seek_format_here(call->file_context, call->byte_offset, call->origin, call->fs_state,
                                                call->format_context);
    return 0;
}

static unsigned xdj_eof_on_own_stack(void* argument, unsigned unused_b, unsigned char* unused_c)
{
    xdj_entry_call* call = (xdj_entry_call*)argument;

    (void)unused_b;
    (void)unused_c;
    call->result = (size_t)xdj_eof_format_here(call->file_context, call->fs_state, call->format_context);
    return 0;
}

size_t xdj_flac_virtual_read_format(
    void* file_context,
    void* output,
    size_t byte_count,
    int* transfer_status,
    void* fs_state,
    void* format_context
)
{
    xdj_entry_call call;

    XDJ_WATCH_ENTER(XDJ_WATCH_ENTRY_READ)
    if (!xdj_in_file_task()) {
        return xdj_read_format_here(file_context, output, byte_count, transfer_status, fs_state, format_context);
    }
    call.file_context = file_context;
    call.buffer = output;
    call.byte_count = byte_count;
    call.transfer_status = transfer_status;
    call.fs_state = fs_state;
    call.format_context = format_context;
    (void)xdj_call_on_fed_stack(xdj_read_on_own_stack, &call, 0U, NULL);
    return call.result;
}

int xdj_flac_virtual_seek_format(
    void* file_context,
    int byte_offset,
    int origin,
    void* fs_state,
    void* format_context
)
{
    xdj_entry_call call;

    XDJ_WATCH_ENTER(XDJ_WATCH_ENTRY_SEEK)
    if (!xdj_in_file_task()) {
        return xdj_seek_format_here(file_context, byte_offset, origin, fs_state, format_context);
    }
    call.file_context = file_context;
    call.byte_offset = byte_offset;
    call.origin = origin;
    call.fs_state = fs_state;
    call.format_context = format_context;
    (void)xdj_call_on_fed_stack(xdj_seek_on_own_stack, &call, 0U, NULL);
    return (int)call.result;
}

int xdj_flac_virtual_eof_format(void* file_context, void* fs_state, void* format_context)
{
    xdj_entry_call call;

    XDJ_WATCH_ENTER(XDJ_WATCH_ENTRY_EOF)
    if (!xdj_in_file_task()) {
        return xdj_eof_format_here(file_context, fs_state, format_context);
    }
    call.file_context = file_context;
    call.fs_state = fs_state;
    call.format_context = format_context;
    (void)xdj_call_on_fed_stack(xdj_eof_on_own_stack, &call, 0U, NULL);
    return (int)call.result;
}
#endif

#ifdef XDJ_HOST_TEST
int xdj_flac_virtual_eof(void* file_context, void* fs_state)
{
    return xdj_flac_virtual_eof_format(file_context, fs_state, xdj_flac_context.request_format);
}

/* Host runners select the track with xdj_flac_host_select instead of a task state. */
size_t xdj_flac_virtual_read(
    void* file_context,
    void* output,
    size_t byte_count,
    int* transfer_status,
    void* fs_state
)
{
    return xdj_flac_virtual_read_format(
        file_context, output, byte_count, transfer_status, fs_state, xdj_flac_context.request_format
    );
}

int xdj_flac_virtual_seek(
    void* file_context,
    int byte_offset,
    int origin,
    void* fs_state
)
{
    return xdj_flac_virtual_seek_format(
        file_context, byte_offset, origin, fs_state, xdj_flac_context.request_format
    );
}

/* Take the oldest queued report; returns 1 when there was one. */
int xdj_flac_host_take_report(unsigned char* fields)
{
    drflac_uint32 index;
    const drflac_uint8* queued;

    if (xdj_flac_context.report_count == 0U) {
        return 0;
    }
    queued = xdj_flac_context.report_queue[xdj_flac_context.report_head];
    for (index = 0; index < XDJ_REPORT_FIELDS; ++index) {
        fields[index] = queued[index];
    }
    xdj_flac_context.report_head = (xdj_flac_context.report_head + 1U) % XDJ_REPORT_QUEUE;
    xdj_flac_context.report_count--;
    return 1;
}

/* Take the oldest queued file snapshot; returns 1 when there was one. */
int xdj_flac_host_take_snapshot(unsigned char* fields)
{
    drflac_uint32 index;

    if (xdj_flac_context.snapshot_count == 0U) {
        return 0;
    }
    for (index = 0; index < XDJ_REPORT_FIELDS; ++index) {
        fields[index] = xdj_flac_context.snapshot_queue[xdj_flac_context.snapshot_head][index];
    }
    xdj_flac_context.snapshot_head = (xdj_flac_context.snapshot_head + 1U) % XDJ_SNAPSHOT_QUEUE;
    xdj_flac_context.snapshot_count--;
    return 1;
}

drflac_uint32 xdj_flac_host_reports_dropped(void)
{
    return xdj_flac_context.reports_dropped;
}

/* The last finished episode, field by field in declaration order; 0 if none. */
int xdj_flac_host_last_episode(drflac_uint32* values)
{
    const xdj_episode* episode;

    if (xdj_flac_context.history_count == 0U) {
        return 0;
    }
    episode = &xdj_flac_history[(xdj_flac_context.history_count - 1U) % XDJ_EPISODE_HISTORY];
    values[0] = episode->sequence;
    values[1] = episode->kind;
    values[2] = episode->start;
    values[3] = episode->end;
    values[4] = episode->first_read;
    values[5] = episode->has_first_read;
    values[6] = episode->busy;
    values[7] = episode->fs_seek;
    values[8] = episode->fs_read;
    values[9] = episode->wait;
    values[10] = episode->reopen;
    values[11] = episode->flac_seek;
    values[12] = episode->decode;
    values[13] = episode->calls;
    values[14] = episode->fs_reads;
    values[15] = episode->physical_seeks;
    values[16] = episode->probes;
    values[17] = episode->cache_hits;
    values[18] = episode->fallbacks;
    values[19] = episode->fs_errors;
    values[20] = (drflac_uint32)episode->fs_error;
    values[21] = episode->fs_operation;
    values[22] = episode->failure;
    values[23] = episode->seek_failure;
    values[24] = episode->no_size;
    values[25] = episode->misplaced;
    values[26] = episode->handle_moved;
    values[27] = episode->sequence_breaks;
    values[28] = episode->stale_calls;
    values[29] = episode->foreign_stale;
    values[30] = episode->eof_ends;
    return 1;
}

/* Render one report as the device plays it; returns the number of stereo frames. */
size_t xdj_flac_host_render_report(
    const unsigned char* fields,
    drflac_uint32 sample_rate,
    drflac_int16* output,
    size_t max_frames
)
{
    xdj_report_player player;
    drflac_int16 sample;
    size_t frames = 0;

    xdj_start_report(&player, fields);
    while (frames < max_frames && xdj_report_sample(&player, sample_rate, &sample)) {
        output[frames * 2U] = sample;
        output[frames * 2U + 1U] = sample;
        frames++;
    }
    return frames;
}
#endif
