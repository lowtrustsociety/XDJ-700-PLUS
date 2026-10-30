/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * Host test runner for the adapter's episode timing and audible reports.
 *
 * The mock filesystem advances the host clock by a fixed amount per call, so
 * every measured interval is known exactly:
 *  1. A FLAC open is reported as its own episode
 *  2. Sequential reads never start an episode
 *  3. A seek that moves the play position starts one; the following calls
 *     join it until the bridge is idle, and time adds up per category
 *  4. A seek to the current position and calls for non-FLAC descriptors are
 *     not counted
 *  5. An evicted track's reopen is timed inside the episode
 *  6. The report queue keeps the newest reports, and values are capped
 *  7. A repeated cue is found in the seek cache
 *  8. Long times are capped
 *  9. Filesystem errors are counted, and failed seeks and reads are
 *     reported with their failure codes
 * 10. Stale windows (a call whose file is not its named track's) are served
 *     as silence without touching the decoder, and the end-of-stream test
 *     answers from the virtual position
 *
 * --render writes reports to a WAV file as the device would play them, for
 * the tone decoder's round-trip test.
 *
 * --pace (built with XDJ_FETCH_PACE_MS and XDJ_FETCH_REFILL_MS) checks the
 * paced fetch: a read of the fill ahead or behind sleeps until it has taken
 * the time its level in hand asks for, and no other read sleeps at all.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dr_flac.h"

int xdj_flac_adapter(void* file_io_context, void* output_descriptor, void* parser_workspace);
size_t xdj_flac_virtual_read(void* file_context, void* output, size_t byte_count, int* transfer_status, void* fs_state);
int xdj_flac_virtual_seek(void* file_context, int byte_offset, int origin, void* fs_state);
int xdj_flac_virtual_eof(void* file_context, void* fs_state);
void xdj_flac_host_select(void* output_descriptor);
size_t xdj_flac_host_pcm_bytes(void* output_descriptor);
int xdj_flac_host_take_report(unsigned char* fields);
int xdj_flac_host_take_snapshot(unsigned char* fields);
drflac_uint32 xdj_flac_host_reports_dropped(void);
int xdj_flac_host_last_episode(drflac_uint32* values);
size_t xdj_flac_host_render_report(
    const unsigned char* fields,
    drflac_uint32 sample_rate,
    drflac_int16* output,
    size_t max_frames
);

extern drflac_uint32 xdj_host_clock_units;
#ifdef XDJ_FETCH_PACE_MS
extern drflac_uint32 xdj_host_input_command;
extern drflac_uint32 xdj_host_input_abort;
extern drflac_uint32 xdj_host_ahead_units;
extern drflac_uint32 xdj_host_behind_units;
extern drflac_uint32 xdj_host_pace_sleeps;
#endif

#define UNITS_PER_MS 16U
#define READ_UNITS 32U  /* each mock filesystem read takes 2 ms */
#define SEEK_UNITS 8U   /* each mock filesystem seek takes 0.5 ms */
#define PACE_AHEAD_UNITS 1800U   /* the top-up pace needs 24 s in hand, in 1/75 s units */
#define REFILL_AHEAD_UNITS 375U  /* the refill pace needs 5 s */
#define FIELDS 27U
#define DESCRIPTORS 7U

/* Order of xdj_flac_host_last_episode values. */
enum {
    E_SEQUENCE, E_KIND, E_START, E_END, E_FIRST_READ, E_HAS_FIRST_READ, E_BUSY,
    E_FS_SEEK, E_FS_READ, E_WAIT, E_REOPEN, E_FLAC_SEEK, E_DECODE, E_CALLS,
    E_FS_READS, E_PHYSICAL_SEEKS, E_PROBES, E_CACHE_HITS, E_FALLBACKS, E_FS_ERRORS,
    E_FS_ERROR, E_FS_OPERATION, E_FAILURE, E_SEEK_FAILURE, E_NO_SIZE, E_MISPLACED,
    E_MOVED, E_SEQUENCE_BREAKS, E_STALE, E_FOREIGN, E_EOF_ENDS, E_COUNT
};

typedef struct {
    FILE* fp;
} host_file;

static int g_lock_held = 0;
static int g_lock_errors = 0;
static drflac_uint32 g_extra_read_units = 0;
/* Reads left to fail, and how: 1 returns half the bytes, 2 returns none. Both report status -1. */
static unsigned long g_failing_reads = 0;
static int g_failure_mode = 0;

void xdj_flac_lock_acquire(void)
{
    if (g_lock_held) {
        g_lock_errors++;
    }
    g_lock_held = 1;
}

void xdj_flac_lock_release(void)
{
    if (!g_lock_held) {
        g_lock_errors++;
    }
    g_lock_held = 0;
}

int xdj_host_mock_seek(void* context, int offset, int origin)
{
    host_file* file = (host_file*)context;
    int whence = origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END;

    if (file->fp == NULL) {
        return -1;
    }
    xdj_host_clock_units += SEEK_UNITS;
    return fseek(file->fp, (long)offset, whence) == 0 ? 0 : -1;
}

size_t xdj_host_mock_read(void* context, void* output, size_t byte_count, int* status)
{
    host_file* file = (host_file*)context;

    xdj_host_clock_units += READ_UNITS + g_extra_read_units;
    g_extra_read_units = 0;
    if (status != NULL) {
        *status = 0;
    }
    if (g_failing_reads != 0U) {
        g_failing_reads--;
        if (status != NULL) {
            *status = -1;
        }
        return g_failure_mode == 1 ? fread(output, 1, byte_count / 2U, file->fp) : 0U;
    }
    return fread(output, 1, byte_count, file->fp);
}

int xdj_host_mock_tell(void* context)
{
    host_file* file = (host_file*)context;

    return file->fp != NULL ? (int)ftell(file->fp) : -1;
}

static int g_failures = 0;
static int g_checks = 0;

static void check(int condition, const char* name)
{
    g_checks++;
    if (!condition) {
        g_failures++;
        fprintf(stderr, "FAILED: %s\n", name);
    }
}

/* Reference encoders, written independently of the adapter's. */
static unsigned int field_time(drflac_uint32 units)
{
    unsigned int ms = units / 16U;
    unsigned int value = ms < 315U ? (ms + 5U) / 10U : 32U + (ms - 280U) / 80U;
    return value > 63U ? 63U : value;
}

static unsigned int field_count(drflac_uint32 count)
{
    unsigned int value = count < 32U ? count : 32U + (count - 28U) / 8U;
    return value > 63U ? 63U : value;
}

static int checksum_ok(const unsigned char* fields)
{
    unsigned int sum = 0;
    unsigned int index;

    for (index = 0; index + 1U < FIELDS; ++index) {
        sum += fields[index];
    }
    return (sum & 63U) == fields[FIELDS - 1U];
}

static void drain_reports(void)
{
    unsigned char fields[FIELDS];

    while (xdj_flac_host_take_report(fields)) {
    }
}

static void idle(drflac_uint32 ms)
{
    xdj_host_clock_units += ms * UNITS_PER_MS;
}

static size_t read_chunk(host_file* file)
{
    static unsigned char buffer[8192];
    int status = 0;

    return xdj_flac_virtual_read(file, buffer, sizeof(buffer), &status, NULL);
}

static int run_episodes(const char* path, const char* other_path, char** extra_paths, int extra_count)
{
    static unsigned char descriptors[DESCRIPTORS][1024];
    unsigned char other_descriptor[1024];
    unsigned char fields[FIELDS];
    drflac_uint32 values[E_COUNT];
    host_file file;
    size_t pcm_bytes;
    drflac_uint32 middle;
    drflac_uint32 quarter;
    drflac_uint32 sequence_before;
    unsigned int index;
    int taken;

    file.fp = fopen(path, "rb");
    if (file.fp == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    memset(descriptors, 0, sizeof(descriptors));
    memset(other_descriptor, 0, sizeof(other_descriptor));

    /* 1. An open is one episode of its own. */
    check(xdj_flac_adapter(&file, descriptors[0], NULL) == 0, "open succeeds");
    xdj_flac_host_select(descriptors[0]);
    check(xdj_flac_host_last_episode(values), "open recorded");
    taken = xdj_flac_host_take_report(fields);
    check(taken, "open report queued");
    check(fields[1] == 2U, "open report kind");
    check(fields[11] == 1U, "open report counts one call");
    check(values[E_CALLS] == 1U && values[E_FS_READS] > 0U, "open counts its filesystem reads");
    check(values[E_FS_READ] == values[E_FS_READS] * READ_UNITS, "open read time adds up exactly");
    check(values[E_BUSY] == values[E_END] - values[E_START], "open busy time equals its wall time");
    check(fields[2] == field_time(values[E_END] - values[E_START]), "open wall field");
    check(checksum_ok(fields), "open report checksum");
    check(!xdj_flac_host_take_report(fields), "open queues exactly one report");

    pcm_bytes = xdj_flac_host_pcm_bytes(descriptors[0]);
    middle = (drflac_uint32)(pcm_bytes / 2U) & ~3U;
    quarter = (drflac_uint32)(pcm_bytes / 4U) & ~3U;

    /* 2. Sequential reads never start an episode, however long the gaps. */
    for (index = 0; index < 5U; ++index) {
        idle(100);
        read_chunk(&file);
    }
    check(!xdj_flac_host_take_report(fields), "sequential reads queue no report");

    /* 3. A jump: a moving seek, three reads back to back, then idle. */
    idle(100);
    sequence_before = values[E_SEQUENCE];
    check(xdj_flac_virtual_seek(&file, (int)middle, 0, NULL) == 0, "jump seek succeeds");
    read_chunk(&file);
    read_chunk(&file);
    read_chunk(&file);
    check(!xdj_flac_host_take_report(fields), "episode stays open while busy");
    idle(100);
    read_chunk(&file);
    taken = xdj_flac_host_take_report(fields);
    check(taken, "idle bridge ends the episode");
    check(xdj_flac_host_last_episode(values), "jump recorded");
    check(values[E_SEQUENCE] == sequence_before + 1U, "jump sequence follows the open");
    check(fields[0] == (values[E_SEQUENCE] & 63U), "sequence field");
    check((fields[1] & 3U) == 1U, "jump report kind");
    check(values[E_PROBES] > 0U && values[E_PROBES] <= 40U && fields[14] == field_count(values[E_PROBES]), "jump seek probes counted");
    check(values[E_FALLBACKS] == 0U && (fields[1] & 4U) == 0U, "jump seek needs no fallback");
    check(values[E_CALLS] == 4U && fields[11] == 4U, "seek and three reads join; the idle read does not");
    check(values[E_BUSY] == values[E_END] - values[E_START], "back-to-back calls leave no gap");
    check(values[E_FS_READ] == values[E_FS_READS] * READ_UNITS, "jump read time adds up exactly");
    check(values[E_FS_READS] > 0U && fields[12] == field_count(values[E_FS_READS]), "filesystem read count field");
    check(values[E_FLAC_SEEK] > 0U, "dr_flac seek time measured");
    check(values[E_DECODE] > 0U, "decode time measured");
    check(values[E_FS_READ] + values[E_FS_SEEK] <= values[E_FLAC_SEEK] + values[E_DECODE],
          "filesystem time falls inside dr_flac seeks and decoding");
    check(values[E_HAS_FIRST_READ] && values[E_FIRST_READ] >= values[E_FLAC_SEEK], "first read ends after the seek");
    check(values[E_REOPEN] == 0U, "no reopen without an eviction");
    check(fields[2] == field_time(values[E_END] - values[E_START]), "jump wall field");
    check(fields[4] == field_time(values[E_BUSY]), "jump busy field");
    check(fields[6] == field_time(values[E_FLAC_SEEK]), "jump seek field");
    check(fields[7] == field_time(values[E_DECODE]), "jump decode field");
    check(fields[8] == field_time(values[E_FS_READ]), "jump read field");
    check(fields[15] == field_time(values[E_END] - values[E_START]), "running maximum field");
    check(checksum_ok(fields), "jump report checksum");

    /* 4. A seek to the current position is not a jump, nor is another format. */
    {
        drflac_uint32 before = values[E_SEQUENCE];
        int status = 0;
        unsigned char buffer[16];

        idle(100);
        /* The cursor is where the last read left it. */
        xdj_flac_virtual_seek(&file, 0, 1, NULL);
        read_chunk(&file);
        idle(100);
        read_chunk(&file);
        check(!xdj_flac_host_take_report(fields), "a seek that does not move starts no episode");
        /* Another format's track reads its own file, which no FLAC slot holds. */
        host_file foreign;

        foreign.fp = fopen(other_path, "rb");
        xdj_flac_host_select(other_descriptor);
        idle(100);
        xdj_flac_virtual_seek(&foreign, 64, 0, NULL);
        xdj_flac_virtual_read(&foreign, buffer, sizeof(buffer), &status, NULL);
        idle(100);
        xdj_flac_virtual_seek(&foreign, 128, 0, NULL);
        fclose(foreign.fp);
        check(!xdj_flac_host_take_report(fields), "calls for other formats start no episode");
        xdj_flac_host_last_episode(values);
        check(values[E_SEQUENCE] == before, "no episode was recorded for them");
    }

    /* 5. Six more tracks evict the first one; its next jump reopens it. */
    check(extra_count >= (int)DESCRIPTORS - 1, "six extra tracks given");
    for (index = 1; index < DESCRIPTORS && (int)index <= extra_count; ++index) {
        host_file extra_parser;

        extra_parser.fp = fopen(extra_paths[index - 1], "rb");
        check(extra_parser.fp != NULL && xdj_flac_adapter(&extra_parser, descriptors[index], NULL) == 0,
              "extra open succeeds");
        if (extra_parser.fp != NULL) {
            fclose(extra_parser.fp);
        }
        idle(10);
    }
    drain_reports();
    xdj_flac_host_select(descriptors[0]);
    idle(100);
    check(xdj_flac_virtual_seek(&file, (int)quarter, 0, NULL) == 0, "seek on an evicted track succeeds");
    read_chunk(&file);
    idle(100);
    read_chunk(&file);
    taken = xdj_flac_host_take_report(fields);
    check(taken, "reopen episode reported");
    xdj_flac_host_last_episode(values);
    check(values[E_REOPEN] > 0U, "reopen time measured");
    check(fields[5] == field_time(values[E_REOPEN]), "reopen field");
    check(checksum_ok(fields), "reopen report checksum");

    /* 6. Six jumps without playback keep the newest four reports. */
    {
        drflac_uint32 dropped = xdj_flac_host_reports_dropped();
        drflac_uint32 last_sequence;
        unsigned int expected;

        drain_reports();
        for (index = 0; index < 6U; ++index) {
            idle(100);
            xdj_flac_virtual_seek(&file, (int)(index % 2U == 0U ? middle : quarter), 0, NULL);
            read_chunk(&file);
        }
        idle(100);
        read_chunk(&file);
        xdj_flac_host_last_episode(values);
        last_sequence = values[E_SEQUENCE];
        check(xdj_flac_host_reports_dropped() == dropped + 2U, "two oldest reports dropped");
        for (index = 0; index < 4U; ++index) {
            expected = (unsigned int)((last_sequence - 3U + index) & 63U);
            check(xdj_flac_host_take_report(fields) && fields[0] == expected, "newest four reports kept in order");
        }
        check(!xdj_flac_host_take_report(fields), "queue holds four reports");
    }

    /* 7. Jumping back to an earlier cue finds its frame in the seek cache. */
    {
        drflac_uint32 cue = (drflac_uint32)(pcm_bytes / 3U) & ~3U;

        drain_reports();
        idle(100);
        check(xdj_flac_virtual_seek(&file, (int)cue, 0, NULL) == 0, "first jump to the cue");
        read_chunk(&file);
        idle(100);
        check(xdj_flac_virtual_seek(&file, (int)middle, 0, NULL) == 0, "jump elsewhere");
        read_chunk(&file);
        idle(100);
        check(xdj_flac_virtual_seek(&file, (int)cue, 0, NULL) == 0, "jump back to the cue");
        read_chunk(&file);
        idle(100);
        read_chunk(&file);
        xdj_flac_host_last_episode(values);
        check(values[E_CACHE_HITS] == 1U && values[E_PROBES] == 1U, "the repeated cue seeks straight to its frame");
        drain_reports();
    }

    /* 8. Long times are capped at 63. */
    idle(100);
    g_extra_read_units = 3000U * UNITS_PER_MS;
    xdj_flac_virtual_seek(&file, (int)middle, 0, NULL);
    read_chunk(&file);
    check(g_extra_read_units == 0U, "the long read happened inside the episode");
    idle(100);
    read_chunk(&file);
    check(xdj_flac_host_take_report(fields), "long episode reported");
    check(fields[2] == 63U && fields[4] == 63U && fields[15] == 63U, "long times capped at 63");
    check(checksum_ok(fields), "capped report checksum");

    /* 9. Filesystem errors. */
    {
        drflac_uint32 target = (drflac_uint32)(pcm_bytes / 8U * 5U) & ~3U;
        int sought;

        /* One read in a jump fails after half its bytes: counted, and the jump still lands. */
        drain_reports();
        idle(100);
        g_failure_mode = 1;
        g_failing_reads = 1;
        sought = xdj_flac_virtual_seek(&file, (int)target, 0, NULL);
        read_chunk(&file);
        idle(100);
        read_chunk(&file);
        check(g_failing_reads == 0U, "the failing read happened inside the jump");
        check(xdj_flac_host_take_report(fields), "jump with a filesystem error reported");
        xdj_flac_host_last_episode(values);
        check(values[E_FS_ERRORS] == 1U && fields[18] == 1U, "the filesystem error is counted");
        check(sought == 0 && values[E_FAILURE] == 0U && (fields[1] & 32U) == 0U && fields[16] == 0U,
              "a partial read does not fail the jump");
        check(values[E_MISPLACED] == 0U && values[E_MOVED] == 0U && fields[21] == 0U, "the handle stays where it should");
        check(values[E_SEQUENCE_BREAKS] == 0U && fields[22] == 0U, "no frame-number breaks");
        check(values[E_STALE] == 0U && fields[23] == 0U, "no stale calls");
        check(checksum_ok(fields), "filesystem error report checksum");

        /* Every read fails: the jump fails with its reason. */
        drain_reports();
        idle(100);
        g_failure_mode = 2;
        g_failing_reads = 1000000U;
        sought = xdj_flac_virtual_seek(&file, (int)(quarter + 4096U), 0, NULL);
        idle(100);
        g_failing_reads = 0;
        read_chunk(&file);
        check(sought != 0, "the jump fails when nothing can be read");
        check(xdj_flac_host_take_report(fields), "failed jump reported");
        xdj_flac_host_last_episode(values);
        check((fields[1] & 3U) == 1U && (fields[1] & 32U) != 0U && fields[16] == 2U, "failed jump: kind, flag, code");
        check(values[E_SEEK_FAILURE] != 0U && fields[17] == values[E_SEEK_FAILURE] &&
              (values[E_SEEK_FAILURE] & 8U) != 0U, "failed jump: search and fallback reasons");
        check(values[E_FS_ERRORS] > 0U && fields[18] == field_count(values[E_FS_ERRORS]), "failed jump: errors counted");
        check(checksum_ok(fields), "failed jump report checksum");

        /* Reads fail during playback, with no episode running: reported on their own. */
        drain_reports();
        idle(100);
        g_failing_reads = 1000000U;
        for (index = 0; index < 40U && read_chunk(&file) != 0U; ++index) {
        }
        idle(100);
        g_failing_reads = 0;
        read_chunk(&file);
        check(xdj_flac_host_take_report(fields), "read failure reported");
        xdj_flac_host_last_episode(values);
        check((fields[1] & 3U) == 3U && (fields[1] & 32U) != 0U && fields[16] == 1U, "read failure: kind, flag, code");
        check(checksum_ok(fields), "read failure report checksum");
    }

    /* 10. Tracks are found by their files; stale and unknown handles; the end-of-stream test. */
    {
        host_file other;
        host_file other_parser;
        host_file no_file;
        host_file unknown;
        unsigned char other_descriptors[1024];
        static unsigned char before[8192];
        static unsigned char expect[8192];
        static unsigned char buffer[8192];
        static unsigned char filler[1000];
        unsigned char snapshot[FIELDS];
        int reasons = 0;
        int status = 0;
        size_t got;
        size_t zeros;

        drain_reports();
        other.fp = fopen(other_path, "rb");
        other_parser.fp = fopen(other_path, "rb");
        no_file.fp = NULL;
        memset(other_descriptors, 0, sizeof(other_descriptors));
        /* Opened through the parser's handle and then read through the task's, as on the player. */
        check(other.fp != NULL && other_parser.fp != NULL &&
              xdj_flac_adapter(&other_parser, other_descriptors, NULL) == 0, "second track opens");
        xdj_flac_host_select(other_descriptors);
        idle(100);
        check(xdj_flac_virtual_seek(&other, 0, 0, NULL) == 0 &&
              xdj_flac_virtual_read(&other, expect, sizeof(expect), &status, NULL) == sizeof(expect),
              "second track plays through the task's handle");

        /* The first track's audio at a known place, for comparison later. */
        xdj_flac_host_select(descriptors[0]);
        idle(100);
        check(xdj_flac_virtual_seek(&file, (int)quarter, 0, NULL) == 0, "first track seeks");
        got = xdj_flac_virtual_read(&file, before, sizeof(before), &status, NULL);
        check(got == sizeof(before) && status == 0, "first track reads");

        /* The first track's format object with the second track's file: the file decides. */
        idle(100);
        check(xdj_flac_virtual_seek(&other, 0, 0, NULL) == 0, "seek through the second track's file");
        got = xdj_flac_virtual_read(&other, buffer, sizeof(buffer), &status, NULL);
        check(got == sizeof(buffer) && status == 0 && memcmp(buffer, expect, got) == 0,
              "a call plays the file it reads, whatever the format object");

        /* A zeroed handle, as at the start of a load routine: a stale window. */
        idle(100);
        memset(buffer, 0x55, sizeof(buffer));
        got = xdj_flac_virtual_read(&no_file, buffer, sizeof(buffer), &status, NULL);
        for (zeros = 0; zeros < got && buffer[zeros] == 0U; ++zeros) {
        }
        check(got == sizeof(buffer) && status == 0 && zeros == got, "a zeroed handle reads full-length silence");
        check(xdj_flac_virtual_seek(&no_file, (int)middle, 0, NULL) == 0, "a zeroed handle seeks as a no-op");
        check(xdj_flac_virtual_eof(&no_file, NULL) == 0, "a zeroed handle is not at the end");
        idle(100);
        read_chunk(&file);
        xdj_flac_host_last_episode(values);
        check(values[E_STALE] >= 2U && values[E_FAILURE] == 0U, "stale calls counted, no failure");
        while (xdj_flac_host_take_report(fields)) {
        }
        check(fields[23] == field_count(values[E_STALE]) && (fields[1] & 32U) == 0U && checksum_ok(fields),
              "stale field, no failure flag");

        /* The first track was not disturbed: same audio at the same place. */
        idle(100);
        check(xdj_flac_virtual_seek(&file, (int)quarter, 0, NULL) == 0, "first track seeks again");
        got = xdj_flac_virtual_read(&file, buffer, sizeof(buffer), &status, NULL);
        check(got == sizeof(before) && memcmp(buffer, before, got) == 0, "first track unchanged");

        /* A file that is no FLAC track goes to the stock routine (a host stub that fails). */
        unknown.fp = tmpfile();
        fwrite(filler, 1, sizeof(filler), unknown.fp);
        got = xdj_flac_virtual_read(&unknown, buffer, sizeof(buffer), &status, NULL);
        check(got == 0U && status == -1, "another format's file reaches the stock routine");
        fclose(unknown.fp);

        /* End of stream answers from the virtual position. */
        check(xdj_flac_virtual_eof(&file, NULL) == 0, "not ended mid-track");
        check(xdj_flac_virtual_seek(&file, (int)(pcm_bytes & ~(size_t)3U), 0, NULL) == 0, "seek to the end");
        check(xdj_flac_virtual_eof(&file, NULL) == 1, "ended at the end");

        while (xdj_flac_host_take_snapshot(snapshot)) {
            check(snapshot[1] == 63U && checksum_ok(snapshot), "snapshot mark and checksum");
            reasons |= 1 << snapshot[2];
        }
        check(reasons == ((1 << 1) | (1 << 2) | (1 << 4)), "snapshots: baseline, not a FLAC track, zeroed handle");
        fclose(other.fp);
        fclose(other_parser.fp);
    }

    check(g_lock_errors == 0 && !g_lock_held, "lock balanced");
    fclose(file.fp);

    printf("{\n  \"checks\": %d,\n  \"failures\": %d,\n  \"passed\": %s\n}\n",
           g_checks, g_failures, g_failures == 0 ? "true" : "false");
    return g_failures == 0 ? 0 : 3;
}

#ifdef XDJ_FETCH_PACE_MS
/* One read with the player in the given state: the pacing sleeps it took, and its length on the clock. */
static drflac_uint32 paced_read(
    host_file* file, drflac_uint32 command, drflac_uint32 ahead, drflac_uint32 behind, drflac_uint32* units
)
{
    drflac_uint32 sleeps = xdj_host_pace_sleeps;
    drflac_uint32 start = xdj_host_clock_units;

    xdj_host_input_command = command;
    xdj_host_ahead_units = ahead;
    xdj_host_behind_units = behind;
    check(read_chunk(file) == 8192U, "paced read delivers its bytes");
    *units = xdj_host_clock_units - start;
    return xdj_host_pace_sleeps - sleeps;
}

/* The read took its time, and stopped at the first slice past it. */
static int took(drflac_uint32 units, drflac_uint32 due)
{
    return units >= due && units < due + 2U * UNITS_PER_MS + 1U;
}

static int run_pace(const char* path)
{
    static unsigned char descriptor[1024];
    /* 8,192 bytes of 16-bit stereo at 44.1 kHz, 2,352 bytes per 1/75 s, in sixteenths of a millisecond. */
    const drflac_uint32 top_up = 8192U * XDJ_FETCH_PACE_MS * UNITS_PER_MS / 2352U;
    const drflac_uint32 refill = 8192U * XDJ_FETCH_REFILL_MS * UNITS_PER_MS / 2352U;
    host_file file;
    drflac_uint32 units;
    drflac_uint32 sleeps;
    unsigned int tries;

    file.fp = fopen(path, "rb");
    if (file.fp == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    memset(descriptor, 0, sizeof(descriptor));
    check(xdj_flac_adapter(&file, descriptor, NULL) == 0, "open succeeds");
    xdj_flac_host_select(descriptor);
    check(refill > 4U * UNITS_PER_MS && refill < top_up, "the runner is built with a refill pace quicker than the top-up");

    /* The fill ahead, by the audio ahead: full speed, the refill pace, the top-up pace. */
    check(paced_read(&file, 1U, 0U, 9000U, &units) == 0U, "ahead, nothing in hand: not paced");
    check(paced_read(&file, 1U, REFILL_AHEAD_UNITS - 1U, 9000U, &units) == 0U, "ahead, under the refill mark: not paced");
    check(paced_read(&file, 1U, REFILL_AHEAD_UNITS, 0U, &units) > 0U && took(units, refill), "ahead, at the refill mark: refill pace");
    check(paced_read(&file, 1U, PACE_AHEAD_UNITS - 1U, 0U, &units) > 0U && took(units, refill), "ahead, under the top-up mark: refill pace");
    check(paced_read(&file, 1U, PACE_AHEAD_UNITS, 0U, &units) > 0U && took(units, top_up), "ahead, at the top-up mark: top-up pace");
    check(paced_read(&file, 1U, 2200U, 0U, &units) > 0U && took(units, top_up), "ahead, full: top-up pace");
    check(paced_read(&file, 1U, 100000U, 2000U, &units) == 0U, "ahead, an impossible level: not paced");

    /* The fill behind goes by the audio behind, whatever is ahead. */
    check(paced_read(&file, 2U, 2200U, 0U, &units) == 0U, "behind, nothing in hand: not paced");
    check(paced_read(&file, 2U, 2200U, REFILL_AHEAD_UNITS - 1U, &units) == 0U, "behind, under the refill mark: not paced");
    check(paced_read(&file, 2U, 0U, REFILL_AHEAD_UNITS, &units) > 0U && took(units, refill), "behind, at the refill mark: refill pace");
    check(paced_read(&file, 2U, 0U, 2200U, &units) > 0U && took(units, top_up), "behind, full: top-up pace");
    check(paced_read(&file, 2U, 2000U, 100000U, &units) == 0U, "behind, an impossible level: not paced");

    /* No other command is paced. */
    check(paced_read(&file, 0U, 2200U, 2200U, &units) == 0U, "no command: not paced");
    check(paced_read(&file, 3U, 2200U, 2200U, &units) == 0U, "cue: not paced");
    check(paced_read(&file, 4U, 2200U, 2200U, &units) == 0U, "load: not paced");
    check(paced_read(&file, 5U, 2200U, 2200U, &units) == 0U, "track cache: not paced");

    /* A pending break-off ends the sleep before it starts. */
    xdj_host_input_abort = 1U;
    check(paced_read(&file, 1U, 2200U, 0U, &units) == 0U, "break-off pending: not paced");
    xdj_host_input_abort = 0U;

    /*
     * A read that was slow by itself is not slowed more. The extra time goes
     * to the next filesystem read, which not every decoder read makes.
     */
    for (tries = 0; tries < 8U; ++tries) {
        g_extra_read_units = top_up;
        sleeps = paced_read(&file, 1U, 2200U, 0U, &units);
        if (g_extra_read_units == 0U) {
            break;
        }
    }
    check(g_extra_read_units == 0U && sleeps == 0U && units >= top_up, "slow read: not paced");
    g_extra_read_units = 0;

    /* A seek is never paced. */
    xdj_host_input_command = 1U;
    xdj_host_ahead_units = 2200U;
    sleeps = xdj_host_pace_sleeps;
    check(xdj_flac_virtual_seek(&file, 0, 0, NULL) == 0, "seek to the start");
    check(xdj_host_pace_sleeps == sleeps, "seek: not paced");

    check(g_lock_errors == 0 && !g_lock_held, "lock balanced");
    fclose(file.fp);
    printf("{\n  \"checks\": %d,\n  \"failures\": %d,\n  \"passed\": %s\n}\n",
           g_checks, g_failures, g_failures == 0 ? "true" : "false");
    return g_failures == 0 ? 0 : 3;
}
#endif

static void put_u32(FILE* out, drflac_uint32 value)
{
    unsigned char bytes[4] = {
        (unsigned char)value, (unsigned char)(value >> 8),
        (unsigned char)(value >> 16), (unsigned char)(value >> 24)
    };
    fwrite(bytes, 1, 4, out);
}

static void put_u16(FILE* out, unsigned int value)
{
    unsigned char bytes[2] = {(unsigned char)value, (unsigned char)(value >> 8)};
    fwrite(bytes, 1, 2, out);
}

/*
 * --render <out.wav> <rate> <header_rate> <report>[;<report>...], each report
 * 26 comma-separated fields plus a checksum. Reports are separated by 0.5 s of noise, like
 * music between reports. A header rate other than the render rate plays the
 * file faster or slower, as a pitch fader would.
 */
static int run_render(const char* path, drflac_uint32 rate, drflac_uint32 header_rate, const char* reports)
{
    static drflac_int16 samples[2U * 48000U * 60U];
    size_t frames = 0;
    size_t capacity = sizeof(samples) / sizeof(samples[0]) / 2U;
    drflac_uint32 noise = 12345U;
    const char* cursor = reports;
    FILE* out;
    size_t index;

    while (*cursor != '\0') {
        unsigned char fields[FIELDS];
        unsigned int count = 0;
        char* end;

        for (index = 0; index < rate / 2U && frames < capacity; ++index) {
            noise = noise * 1103515245U + 12345U;
            samples[frames * 2U] = (drflac_int16)((drflac_int32)(noise >> 16) - 32768) / 4;
            samples[frames * 2U + 1U] = samples[frames * 2U];
            frames++;
        }
        while (count < FIELDS) {
            fields[count++] = (unsigned char)strtoul(cursor, &end, 10);
            cursor = end;
            if (*cursor == ',') {
                cursor++;
            }
        }
        if (*cursor == ';') {
            cursor++;
        }
        frames += xdj_flac_host_render_report(fields, rate, samples + frames * 2U, capacity - frames);
    }
    out = fopen(path, "wb");
    if (out == NULL) {
        return 1;
    }
    fwrite("RIFF", 1, 4, out);
    put_u32(out, (drflac_uint32)(36U + frames * 4U));
    fwrite("WAVEfmt ", 1, 8, out);
    put_u32(out, 16U);
    put_u16(out, 1U);
    put_u16(out, 2U);
    put_u32(out, header_rate);
    put_u32(out, header_rate * 4U);
    put_u16(out, 4U);
    put_u16(out, 16U);
    fwrite("data", 1, 4, out);
    put_u32(out, (drflac_uint32)(frames * 4U));
    for (index = 0; index < frames * 2U; ++index) {
        put_u16(out, (unsigned int)(drflac_uint16)samples[index]);
    }
    fclose(out);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 4 && strcmp(argv[1], "--episodes") == 0) {
        return run_episodes(argv[2], argv[3], argv + 4, argc - 4);
    }
#ifdef XDJ_FETCH_PACE_MS
    if (argc == 3 && strcmp(argv[1], "--pace") == 0) {
        return run_pace(argv[2]);
    }
#endif
    if (argc == 6 && strcmp(argv[1], "--render") == 0) {
        return run_render(
            argv[2],
            (drflac_uint32)strtoul(argv[3], NULL, 10),
            (drflac_uint32)strtoul(argv[4], NULL, 10),
            argv[5]
        );
    }
    fprintf(stderr, "usage: %s --episodes <flac> <other flac> <six extra flacs> | --render <wav> <rate> <header_rate> <reports>\n", argv[0]);
    return 1;
}
