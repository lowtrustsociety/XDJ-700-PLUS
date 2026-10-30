/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * Host test runner for the bounded FLAC seek (xdj_flac_seek).
 *
 * For one FLAC file it checks that every seek lands sample-exactly: the audio
 * read after the seek must equal the same span of a full sequential decode.
 * Targets are the stream edges, frame boundaries, random positions, and
 * repeats of earlier targets (which the seek cache should serve). It counts
 * the read and seek callbacks each seek needs, and does the same with
 * dr_flac's original seek on the same targets for comparison.
 *
 * Usage: flac_seek_runner <file.flac> <random_targets> <seed>
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dr_flac.h"
#include "xdj_flac_seek.h"

typedef struct {
    drflac_uint32 sample_rate;
    drflac_uint64 frame_count;
    drflac_uint8 channels;
    drflac_uint8 bits_per_sample;
} xdj_flac_stream_info;

drflac* xdj_flac_open(
    drflac_read_proc read_callback,
    drflac_seek_proc seek_callback,
    drflac_tell_proc tell_callback,
    void* stream_context,
    const drflac_allocation_callbacks* allocation_callbacks,
    xdj_flac_stream_info* info
);
drflac_uint64 xdj_flac_read_s16(drflac* decoder, drflac_uint64 frame_count, drflac_int16* output);
drflac_bool32 xdj_flac_seek_original(drflac* decoder, drflac_uint64 frame_index);
void xdj_flac_close(drflac* decoder);
extern int xdj_seek_force_fallback;

#define COMPARE_FRAMES 4096U
#define MAX_TARGETS 4096U

typedef struct {
    FILE* fp;
    long position;
    unsigned long reads;
    unsigned long seeks;
    unsigned long long bytes;
} counted_file;

static size_t on_read(void* user, void* output, size_t bytes)
{
    counted_file* file = (counted_file*)user;
    size_t got = fread(output, 1, bytes, file->fp);

    file->reads++;
    file->bytes += got;
    file->position += (long)got;
    return got;
}

static drflac_bool32 on_seek(void* user, int offset, drflac_seek_origin origin)
{
    counted_file* file = (counted_file*)user;
    int whence = origin == DRFLAC_SEEK_SET ? SEEK_SET : origin == DRFLAC_SEEK_CUR ? SEEK_CUR : SEEK_END;

    file->seeks++;
    if (fseek(file->fp, offset, whence) != 0) {
        return DRFLAC_FALSE;
    }
    file->position = ftell(file->fp);
    return DRFLAC_TRUE;
}

static drflac_bool32 on_tell(void* user, drflac_int64* cursor)
{
    *cursor = ((counted_file*)user)->position;
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

static drflac* open_counted(counted_file* file, const char* path, xdj_flac_stream_info* info)
{
    drflac_allocation_callbacks allocation = {NULL, host_malloc, host_realloc, host_free};

    memset(file, 0, sizeof(*file));
    file->fp = fopen(path, "rb");
    if (file->fp == NULL) {
        return NULL;
    }
    return xdj_flac_open(on_read, on_seek, on_tell, file, &allocation, info);
}

static drflac_uint64 g_random = 0x9E3779B97F4A7C15ULL;

static drflac_uint64 next_random(void)
{
    g_random ^= g_random << 13;
    g_random ^= g_random >> 7;
    g_random ^= g_random << 17;
    return g_random;
}

int main(int argc, char** argv)
{
    static drflac_uint64 targets[MAX_TARGETS];
    static drflac_int16 window[COMPARE_FRAMES * 2U];
    xdj_flac_stream_info info;
    counted_file reference_file;
    counted_file file;
    counted_file original_file;
    drflac* reference;
    drflac* decoder;
    drflac* original;
    drflac_int16* pcm;
    xdj_flac_seek_state state;
    drflac_uint64 total;
    drflac_uint64 decoded;
    unsigned int count = 0;
    unsigned int random_targets;
    unsigned int index;
    unsigned long mismatches = 0;
    unsigned long failures = 0;
    unsigned long fallbacks = 0;
    unsigned long cache_hits = 0;
    unsigned long max_probes = 0;
    unsigned long max_reads = 0;
    unsigned long sum_reads = 0;
    unsigned long max_seeks = 0;
    unsigned long long max_bytes = 0;
    unsigned long original_max_reads = 0;
    unsigned long original_sum_reads = 0;
    unsigned long long original_max_bytes = 0;
    unsigned long original_failures = 0;
    unsigned long forced_failures = 0;
    unsigned long forced_mismatches = 0;
    long file_size;
    const char* first_mismatch = "";
    char mismatch_note[128] = "";

    if (argc != 4) {
        fprintf(stderr, "usage: %s <file.flac> <random_targets> <seed>\n", argv[0]);
        return 1;
    }
    random_targets = (unsigned int)strtoul(argv[2], NULL, 10);
    g_random ^= strtoull(argv[3], NULL, 10) * 0x2545F4914F6CDD1DULL;

    /* Reference: one sequential decode of the whole stream. */
    reference = open_counted(&reference_file, argv[1], &info);
    if (reference == NULL || info.frame_count == 0) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    total = info.frame_count;
    pcm = (drflac_int16*)malloc((size_t)total * info.channels * sizeof(drflac_int16));
    decoded = xdj_flac_read_s16(reference, total, pcm);
    xdj_flac_close(reference);
    fclose(reference_file.fp);
    if (decoded != total || info.channels != 2U) {
        fprintf(stderr, "reference decode incomplete: %llu of %llu\n", (unsigned long long)decoded, (unsigned long long)total);
        return 1;
    }

    decoder = open_counted(&file, argv[1], &info);
    original = open_counted(&original_file, argv[1], &info);
    fseek(file.fp, 0, SEEK_END);
    file_size = ftell(file.fp);
    fseek(file.fp, file.position, SEEK_SET);
    xdj_flac_seek_reset(&state, (drflac_uint32)file_size);

    /* Edges and frame boundaries, then random targets, with repeats for the cache. */
    {
        drflac_uint64 block = decoder->maxBlockSizeInPCMFrames;
        drflac_uint64 edges[] = {
            1, block - 1U, block, block + 1U, total / 2U, total / 2U + 1U,
            total - block - 1U, total - block, total - 1U, total - 2U, 1
        };
        for (index = 0; index < sizeof(edges) / sizeof(edges[0]); ++index) {
            if (edges[index] > 0U && edges[index] < total) {
                targets[count++] = edges[index];
            }
        }
    }
    for (index = 0; index < random_targets && count < MAX_TARGETS; ++index) {
        if (index % 5U == 4U && count > 12U) {
            targets[count] = targets[12U + next_random() % (count - 12U)];
        } else {
            targets[count] = 1U + next_random() % (total - 1U);
        }
        count++;
    }

    for (index = 0; index < count; ++index) {
        drflac_uint64 target = targets[index];
        drflac_uint64 want = total - target < COMPARE_FRAMES ? total - target : COMPARE_FRAMES;
        unsigned long reads_before = file.reads;
        unsigned long seeks_before = file.seeks;
        unsigned long long bytes_before = file.bytes;
        unsigned long reads;
        drflac_uint64 got;

        if (!xdj_flac_seek(decoder, target, &state)) {
            failures++;
            continue;
        }
        reads = file.reads - reads_before;
        if (reads > max_reads) {
            max_reads = reads;
        }
        sum_reads += reads;
        if (file.seeks - seeks_before > max_seeks) {
            max_seeks = file.seeks - seeks_before;
        }
        if (file.bytes - bytes_before > max_bytes) {
            max_bytes = file.bytes - bytes_before;
        }
        if (state.probes > max_probes) {
            max_probes = state.probes;
        }
        fallbacks += state.used_fallback;
        cache_hits += state.used_cache;
        if (getenv("SEEK_VERBOSE") != NULL) {
            fprintf(stderr, "target %llu reads %lu probes %u bytes %llu\n", (unsigned long long)target, reads,
                    state.probes, file.bytes - bytes_before);
        }
        got = xdj_flac_read_s16(decoder, want, window);
        if (got != want || memcmp(window, pcm + target * 2U, (size_t)want * 4U) != 0) {
            if (mismatches == 0) {
                snprintf(mismatch_note, sizeof(mismatch_note), "target %llu got %llu of %llu",
                         (unsigned long long)target, (unsigned long long)got, (unsigned long long)want);
                first_mismatch = mismatch_note;
            }
            mismatches++;
        }
    }

    /* The last-resort path must be sample-exact too: force it for every target. */
    xdj_seek_force_fallback = 1;
    for (index = 0; index < count; ++index) {
        drflac_uint64 target = targets[index];
        drflac_uint64 want = total - target < COMPARE_FRAMES ? total - target : COMPARE_FRAMES;

        if (!xdj_flac_seek(decoder, target, &state)) {
            forced_failures++;
            continue;
        }
        if (xdj_flac_read_s16(decoder, want, window) != want || memcmp(window, pcm + target * 2U, (size_t)want * 4U) != 0) {
            forced_mismatches++;
        }
    }
    xdj_seek_force_fallback = 0;

    /* dr_flac's own seek on the same targets. */
    for (index = 0; index < count; ++index) {
        unsigned long reads_before = original_file.reads;
        unsigned long long bytes_before = original_file.bytes;
        unsigned long reads;

        if (!xdj_flac_seek_original(original, targets[index])) {
            original_failures++;
            continue;
        }
        reads = original_file.reads - reads_before;
        original_sum_reads += reads;
        if (reads > original_max_reads) {
            original_max_reads = reads;
        }
        if (original_file.bytes - bytes_before > original_max_bytes) {
            original_max_bytes = original_file.bytes - bytes_before;
        }
        xdj_flac_read_s16(original, 256U, window);
    }

    printf("{\n");
    printf("  \"file\": \"%s\",\n", argv[1]);
    printf("  \"total_frames\": %llu,\n", (unsigned long long)total);
    printf("  \"block_size\": %u,\n", (unsigned int)decoder->maxBlockSizeInPCMFrames);
    printf("  \"seek_points\": %u,\n", (unsigned int)decoder->seekpointCount);
    printf("  \"variable_blocking\": %s,\n", state.blocking == 2 ? "true" : "false");
    printf("  \"targets\": %u,\n", count);
    printf("  \"mismatches\": %lu,\n", mismatches);
    printf("  \"first_mismatch\": \"%s\",\n", first_mismatch);
    printf("  \"failures\": %lu,\n", failures);
    printf("  \"fallbacks\": %lu,\n", fallbacks);
    printf("  \"cache_hits\": %lu,\n", cache_hits);
    printf("  \"max_probes\": %lu,\n", max_probes);
    printf("  \"max_reads\": %lu,\n", max_reads);
    printf("  \"mean_reads\": %.2f,\n", count ? (double)sum_reads / count : 0.0);
    printf("  \"max_seeks\": %lu,\n", max_seeks);
    printf("  \"max_bytes\": %llu,\n", max_bytes);
    printf("  \"fallback_path_failures\": %lu,\n", forced_failures);
    printf("  \"fallback_path_mismatches\": %lu,\n", forced_mismatches);
    printf("  \"original_failures\": %lu,\n", original_failures);
    printf("  \"original_max_reads\": %lu,\n", original_max_reads);
    printf("  \"original_mean_reads\": %.2f,\n", count ? (double)original_sum_reads / count : 0.0);
    printf("  \"original_max_bytes\": %llu\n", original_max_bytes);
    printf("}\n");
    xdj_flac_close(decoder);
    xdj_flac_close(original);
    free(pcm);
    return mismatches == 0 && failures == 0 && forced_failures == 0 && forced_mismatches == 0 ? 0 : 3;
}
