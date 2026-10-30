/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * Host runner: two tasks play two different FLAC tracks at once, as the
 * player's input-file task and a buffer manager do with a hot cue bank. Each
 * thread seeks and reads its own track through the bridge and checks every
 * sample against a sequential decode of that track.
 *
 * Mode "format" passes each call's own format object, as the PCM
 * trampolines do, and must play exactly. The other modes model pairings the
 * player does not make: "shared" passes whatever a shared
 * field holds, "crossed" the other track's format object, "oneslot" has both
 * threads read one track. With the bridge treating a mismatched pair as a
 * stale window, those modes produce silence, not the other track.
 *
 * Usage: flac_adapter_two_track_runner <a.flac> <b.flac> <seconds> <format|shared|crossed|oneslot>
 */

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dr_flac.h"

typedef struct {
    drflac_uint32 sample_rate;
    drflac_uint64 frame_count;
    drflac_uint8 channels;
    drflac_uint8 bits_per_sample;
} xdj_flac_stream_info;

int xdj_flac_adapter(void* file_io_context, void* output_descriptor, void* parser_workspace);
size_t xdj_flac_virtual_read_format(
    void* file_context, void* output, size_t byte_count, int* transfer_status, void* fs_state, void* format
);
int xdj_flac_virtual_seek_format(void* file_context, int byte_offset, int origin, void* fs_state, void* format);
void xdj_flac_host_select(void* output_descriptor);
drflac* xdj_flac_open(
    drflac_read_proc read_callback,
    drflac_seek_proc seek_callback,
    drflac_tell_proc tell_callback,
    void* stream_context,
    const drflac_allocation_callbacks* allocation_callbacks,
    xdj_flac_stream_info* info
);
drflac_uint64 xdj_flac_read_s16(drflac* decoder, drflac_uint64 frame_count, drflac_int16* output);
void xdj_flac_close(drflac* decoder);

#define PCM_FORMAT_OFFSET 340U

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_stop = 0;
static int g_shared_mode = 0;
static int g_crossed_mode = 0;
static void* g_other_format[2];
/* The shared request field, as the old trampolines wrote it. */
static void* volatile g_shared_request = NULL;

void xdj_flac_lock_acquire(void)
{
    pthread_mutex_lock(&g_lock);
}

void xdj_flac_lock_release(void)
{
    pthread_mutex_unlock(&g_lock);
}

typedef struct {
    FILE* fp;
} host_file;

int xdj_host_mock_seek(void* context, int offset, int origin)
{
    host_file* file = (host_file*)context;
    int whence = origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END;

    return fseek(file->fp, (long)offset, whence) == 0 ? 0 : -1;
}

size_t xdj_host_mock_read(void* context, void* output, size_t byte_count, int* status)
{
    host_file* file = (host_file*)context;

    if (status != NULL) {
        *status = 0;
    }
    /* Give the other thread a chance to run in the middle of a call. */
    sched_yield();
    return fread(output, 1, byte_count, file->fp);
}

int xdj_host_mock_tell(void* context)
{
    return (int)ftell(((host_file*)context)->fp);
}

static size_t ref_read(void* user, void* output, size_t bytes)
{
    return fread(output, 1, bytes, (FILE*)user);
}

static drflac_bool32 ref_seek(void* user, int offset, drflac_seek_origin origin)
{
    int whence = origin == DRFLAC_SEEK_SET ? SEEK_SET : origin == DRFLAC_SEEK_CUR ? SEEK_CUR : SEEK_END;
    return fseek((FILE*)user, offset, whence) == 0;
}

static drflac_bool32 ref_tell(void* user, drflac_int64* cursor)
{
    *cursor = ftell((FILE*)user);
    return DRFLAC_TRUE;
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

typedef struct {
    const char* path;
    unsigned char descriptor[1024];
    host_file parser;
    host_file task;
    drflac_int16* pcm;
    drflac_uint64 total;
    drflac_uint64 seed;
    unsigned long jumps;
    unsigned long seek_errors;
    unsigned long read_errors;
    unsigned long mismatches;
} track;

static drflac_uint64 next_random(drflac_uint64* state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

static int load_reference(track* t)
{
    drflac_allocation_callbacks allocation = {NULL, host_malloc, host_realloc, host_free};
    xdj_flac_stream_info info;
    FILE* fp = fopen(t->path, "rb");
    drflac* decoder = fp ? xdj_flac_open(ref_read, ref_seek, ref_tell, fp, &allocation, &info) : NULL;

    if (decoder == NULL || info.channels != 2U) {
        return 0;
    }
    t->total = info.frame_count;
    t->pcm = (drflac_int16*)malloc((size_t)t->total * 4U);
    if (xdj_flac_read_s16(decoder, t->total, t->pcm) != t->total) {
        return 0;
    }
    xdj_flac_close(decoder);
    fclose(fp);
    return 1;
}

static void* format_of(track* t)
{
    return t->descriptor + PCM_FORMAT_OFFSET;
}

static track g_tracks[2];

static int track_index(track* t)
{
    return t == &g_tracks[0] ? 0 : 1;
}

static int track_index(track* t);

static int seek_track(track* t, int byte_offset)
{
    if (g_crossed_mode) {
        return xdj_flac_virtual_seek_format(&t->task, byte_offset, 0, NULL, g_other_format[track_index(t)]);
    }
    if (g_shared_mode) {
        g_shared_request = format_of(t);
        sched_yield();
        return xdj_flac_virtual_seek_format(&t->task, byte_offset, 0, NULL, g_shared_request);
    }
    return xdj_flac_virtual_seek_format(&t->task, byte_offset, 0, NULL, format_of(t));
}

static size_t read_track(track* t, void* output, size_t bytes, int* status)
{
    if (g_crossed_mode) {
        return xdj_flac_virtual_read_format(&t->task, output, bytes, status, NULL, g_other_format[track_index(t)]);
    }
    if (g_shared_mode) {
        g_shared_request = format_of(t);
        sched_yield();
        return xdj_flac_virtual_read_format(&t->task, output, bytes, status, NULL, g_shared_request);
    }
    return xdj_flac_virtual_read_format(&t->task, output, bytes, status, NULL, format_of(t));
}

static void* play(void* argument)
{
    track* t = (track*)argument;
    static _Thread_local unsigned char buffer[16384];

    while (!atomic_load(&g_stop)) {
        drflac_uint64 frame = next_random(&t->seed) % t->total;
        unsigned int chunk;

        t->jumps++;
        if (seek_track(t, (int)(frame * 4U)) != 0) {
            t->seek_errors++;
            continue;
        }
        for (chunk = 0; chunk < 3U && frame < t->total; ++chunk) {
            int status = 0;
            size_t expect = (size_t)((t->total - frame) * 4U < sizeof(buffer) ? (t->total - frame) * 4U : sizeof(buffer));
            size_t got = read_track(t, buffer, sizeof(buffer), &status);

            if (status != 0) {
                t->read_errors++;
                break;
            }
            if (got != expect || memcmp(buffer, t->pcm + frame * 2U, got) != 0) {
                t->mismatches++;
                break;
            }
            frame += got / 4U;
        }
    }
    return NULL;
}

int main(int argc, char** argv)
{
    track* tracks = g_tracks;
    pthread_t threads[2];
    double seconds;
    int index;
    int passed = 1;
    int one_slot;

    if (argc != 5) {
        fprintf(stderr, "usage: %s <a.flac> <b.flac> <seconds> <format|shared|crossed|oneslot>\n", argv[0]);
        return 1;
    }
    seconds = atof(argv[3]);
    g_shared_mode = strcmp(argv[4], "shared") == 0;
    g_crossed_mode = strcmp(argv[4], "crossed") == 0 || strcmp(argv[4], "oneslot") == 0;
    one_slot = strcmp(argv[4], "oneslot") == 0;
    for (index = 0; index < 2; ++index) {
        track* t = &tracks[index];

        t->path = argv[one_slot ? 1 : 1 + index];
        t->seed = 0x9E3779B97F4A7C15ULL * (drflac_uint64)(index + 1);
        if (!load_reference(t)) {
            fprintf(stderr, "cannot decode %s\n", t->path);
            return 2;
        }
        t->parser.fp = fopen(t->path, "rb");
        t->task.fp = fopen(t->path, "rb");
        if ((!one_slot || index == 0) && xdj_flac_adapter(&t->parser, t->descriptor, NULL) != 0) {
            fprintf(stderr, "adapter rejected %s\n", t->path);
            return 2;
        }
    }
    g_other_format[0] = format_of(&tracks[one_slot ? 0 : 1]);
    g_other_format[1] = format_of(&tracks[0]);
    for (index = 0; index < 2; ++index) {
        pthread_create(&threads[index], NULL, play, &tracks[index]);
    }
    {
        struct timespec pause = {(time_t)seconds, (long)((seconds - (double)(time_t)seconds) * 1e9)};
        nanosleep(&pause, NULL);
    }
    atomic_store(&g_stop, 1);
    for (index = 0; index < 2; ++index) {
        pthread_join(threads[index], NULL);
    }
    printf("{\"mode\": \"%s\", \"tracks\": [", argv[4]);
    for (index = 0; index < 2; ++index) {
        track* t = &tracks[index];

        printf("%s{\"jumps\": %lu, \"seek_errors\": %lu, \"read_errors\": %lu, \"mismatches\": %lu}",
               index ? ", " : "", t->jumps, t->seek_errors, t->read_errors, t->mismatches);
        passed = passed && t->jumps > 0 && t->seek_errors == 0 && t->read_errors == 0 && t->mismatches == 0;
    }
    printf("], \"passed\": %s}\n", passed ? "true" : "false");
    return passed ? 0 : 3;
}
