/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * Host runner: the player's track lifecycle.
 *
 * Every track is parsed into one working descriptor, as the player's
 * InBufMngSub does (the dispatcher passes the fixed buffer 0x09D2834C), and
 * copied from there into its cache entry; a load copies the track's cached
 * descriptor back into the working descriptor, so the workspace's format
 * object has the same address for every track.
 *
 * A manager thread plays the load manager. For each switch it does what the
 * load routine does: the file context on its "stack" is zeroed, the old
 * handle is closed, a new handle for the next track is opened into the same
 * context, it waits (the DSP fill wait), writes the workspace's format object
 * for the new track, and sends the new command.
 *
 * A stream thread plays the input task: on each command it seeks to the cue
 * and then keeps reading and asking the end-of-stream test through the
 * workspace (format object and file context read on every call), until the
 * next command arrives, as commands 1 and 2 do.
 *
 * Tracks keep their depth (16-bit or packed 24-bit), and the stream thread
 * sizes its reads as the player does for either depth and rate, whatever the
 * track: the player sizes reads by the workspace's format object, which in a
 * hot cue bank can be another track's. Reads and cues may therefore split a
 * frame, and the stream must stay byte-exact. 88.2 and 96 kHz tracks (FLAC,
 * and WAV or AIFF served by the bridge) are converted down; their reference is one straight conversion from the start,
 * which every seek, eviction and reopen must reproduce exactly.
 *
 * Outside a switch every read must be byte-exact for the command's track;
 * inside one (from the zeroing until the stream thread has taken the new
 * command) anything may come back. No call may ever fail, in or out of a
 * switch.
 *
 * Built with XDJ_HOST_NETWORK_FS and XDJ_HOST_CONTEXT_DATA_BYTES, it models a
 * network filesystem (LINK): the bridge checks each handle once, and every
 * command gets a context from a small pool, so handle objects are reused
 * across tracks, with the track's data length in the context as on the player.
 *
 * --twin checks one case by itself: with a track of ours open, a stock file
 * must be left to the stock reader, also when its audio has exactly our
 * track's length (the same recording in two formats), and on a network
 * filesystem without a size query, which is a network request there.
 *
 * Usage: flac_adapter_lifecycle_runner <seconds> <seed> <a.flac> <b.flac> [...]
 *        flac_adapter_lifecycle_runner --twin <a.flac> <any other file>
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
#include "xdj_flac_seek.h"

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
int xdj_flac_virtual_eof_format(void* file_context, void* fs_state, void* format);
drflac* xdj_flac_open(
    drflac_read_proc read_callback,
    drflac_seek_proc seek_callback,
    drflac_tell_proc tell_callback,
    void* stream_context,
    const drflac_allocation_callbacks* allocation_callbacks,
    xdj_flac_stream_info* info
);
drflac_uint64 xdj_flac_read_s16(drflac* decoder, drflac_uint64 frame_count, drflac_int16* output);
drflac_uint64 xdj_flac_read_s24(drflac* decoder, drflac_uint64 frame_count, unsigned char* output);
void xdj_flac_close(drflac* decoder);
extern drflac_uint32 xdj_host_stock_calls;
extern drflac_uint32 xdj_host_size_queries;

#define PCM_FORMAT_OFFSET 340U
#define MAX_TRACKS 8
/* 4 units of 1/75 s, as the stock stream read asks: 16-bit and 24-bit, 44.1 and 48 kHz. */
static const size_t g_read_sizes[4] = {9408U, 10240U, 14112U, 15360U};
#define READ_BYTES 15360U

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_file_lock = PTHREAD_MUTEX_INITIALIZER;

void xdj_flac_lock_acquire(void)
{
    pthread_mutex_lock(&g_lock);
}

void xdj_flac_lock_release(void)
{
    pthread_mutex_unlock(&g_lock);
}

/* On the host the file context is the handle; its FILE is what the manager swaps. */
typedef struct {
    FILE* fp;
    drflac_uint32 data_bytes;
    /* Where the audio starts in the file: 0 for a track of ours, the header's length for a stock file. */
    drflac_uint32 data_start;
} host_file;

/*
 * The manager swaps the FILE under the stream thread, so each mock call reads
 * it once, under a lock, like one filesystem operation on the player.
 */
int xdj_host_mock_seek(void* context, int offset, int origin)
{
    host_file* file = (host_file*)context;
    int whence = origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END;
    int result = -1;

    pthread_mutex_lock(&g_file_lock);
    if (file->fp != NULL) {
        result = fseek(file->fp, (long)offset, whence) == 0 ? 0 : -1;
    }
    pthread_mutex_unlock(&g_file_lock);
    return result;
}

size_t xdj_host_mock_read(void* context, void* output, size_t byte_count, int* status)
{
    host_file* file = (host_file*)context;
    size_t got = 0;

    if (status != NULL) {
        *status = 0;
    }
    sched_yield();
    pthread_mutex_lock(&g_file_lock);
    if (file->fp != NULL) {
        got = fread(output, 1, byte_count, file->fp);
    } else if (status != NULL) {
        *status = -1;
    }
    pthread_mutex_unlock(&g_file_lock);
    return got;
}

int xdj_host_mock_tell(void* context)
{
    host_file* file = (host_file*)context;
    int position = -1;

    pthread_mutex_lock(&g_file_lock);
    if (file->fp != NULL) {
        position = (int)ftell(file->fp);
    }
    pthread_mutex_unlock(&g_file_lock);
    return position;
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
    /* The track's cache entry copy of the descriptor. */
    unsigned char descriptor[1024];
    unsigned char* pcm;
    /* Length of the stream in frames and in bytes. */
    drflac_uint64 total;
    drflac_uint64 total_bytes;
    unsigned bytes_per_frame;
} track;

/* The one working descriptor every parse and every load goes through. */
static unsigned char g_working_descriptor[1024];

static track g_tracks[MAX_TRACKS];
static int g_track_count = 0;

/* The input task's workspace: +92 format object and +48 file context. */
static struct {
    void* volatile format;
    host_file* volatile context;
} g_workspace;

/* The load manager's stack frame: the file context lives here. */
static host_file g_stack_context;
#ifdef XDJ_HOST_NETWORK_FS
/* Context objects reused across commands, as a filesystem reuses handle objects. */
static host_file g_context_pool[2];
static unsigned g_context_next = 0;
#endif

/* The command channel: a sequence number and the command's track and cue. */
static atomic_uint g_command_sequence = 0;
static int g_command_track = 0;
static drflac_uint64 g_command_cue = 0;
/* Odd while a switch is under way, from the zeroing until the stream takes the command. */
static atomic_uint g_switch = 0;
static atomic_int g_stop = 0;

static unsigned long g_exact_reads = 0;
static unsigned long g_window_calls = 0;
static unsigned long g_failures = 0;
static unsigned long g_mismatches = 0;
static unsigned long g_eof_wrong = 0;
static unsigned long g_commands = 0;
static char g_first_problem[200] = "";

static void problem(const char* text)
{
    if (g_first_problem[0] == '\0') {
        snprintf(g_first_problem, sizeof(g_first_problem), "%s", text);
    }
}

static drflac_uint64 next_random(drflac_uint64* state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

/* A load copies the track's cached descriptor into the working one. */
static void* load_descriptor(int index)
{
    memcpy(g_working_descriptor, g_tracks[index].descriptor, sizeof(g_working_descriptor));
    return g_working_descriptor + PCM_FORMAT_OFFSET;
}

static void sleep_ms(double ms)
{
    struct timespec pause = {(time_t)(ms / 1000.0), (long)((ms - (double)(long)(ms / 1000.0) * 1000.0) * 1e6)};
    nanosleep(&pause, NULL);
}

static void* stream_thread(void* argument)
{
    static unsigned char buffer[READ_BYTES];
    unsigned int taken = 0;
    int command_track = -1;
    drflac_uint64 position = 0;
    drflac_uint64 sizes = 0x9E3779B97F4A7C15ULL;
    int ended = 0;

    (void)argument;
    while (!atomic_load(&g_stop)) {
        unsigned int sequence = atomic_load(&g_command_sequence);
        unsigned int before;
        int status = 0;
        size_t want = g_read_sizes[next_random(&sizes) & 3U];
        size_t got;

        if (sequence != taken) {
            /* A new command: take it, then seek to its cue through the workspace. */
            taken = sequence;
            command_track = g_command_track;
            position = g_command_cue;
            ended = 0;
            g_commands++;
            if (xdj_flac_virtual_seek_format(g_workspace.context, (int)position, 0, NULL, g_workspace.format) != 0) {
                g_failures++;
                problem("a seek failed");
            }
            /* The switch is over once the stream runs the new command. */
            if (atomic_load(&g_switch) & 1U) {
                atomic_fetch_add(&g_switch, 1U);
            }
            continue;
        }
        if (command_track < 0 || ended) {
            sched_yield();
            continue;
        }
        before = atomic_load(&g_switch);
        got = xdj_flac_virtual_read_format(g_workspace.context, buffer, want, &status, NULL, g_workspace.format);
        if (status != 0) {
            g_failures++;
            problem("a read failed");
            continue;
        }
        if ((before & 1U) == 0U && atomic_load(&g_switch) == before && atomic_load(&g_command_sequence) == taken) {
            const track* t = &g_tracks[command_track];
            size_t expect = (size_t)(t->total_bytes - position < want ? t->total_bytes - position : want);
            int eof;

            if (got != expect || memcmp(buffer, t->pcm + position, got) != 0) {
                char text[200];

                g_mismatches++;
                snprintf(text, sizeof(text), "wrong audio outside a switch: track %d byte %llu (got %zu of %zu)",
                         command_track, (unsigned long long)position, got, expect);
                problem(text);
                /* Resynchronise the model on the next command. */
                ended = 1;
                continue;
            }
            g_exact_reads++;
            position += got;
            /* Pace the stream a little, as playback does, so switches meet it mid-track. */
            sleep_ms(0.1);
            eof = xdj_flac_virtual_eof_format(g_workspace.context, NULL, g_workspace.format);
            if (atomic_load(&g_switch) == before && eof != (position >= t->total_bytes ? 1 : 0)) {
                g_eof_wrong++;
                problem("the end-of-stream test disagreed");
            }
            if (position >= t->total_bytes) {
                ended = 1;
            }
        } else {
            g_window_calls++;
            position += got;
            (void)xdj_flac_virtual_eof_format(g_workspace.context, NULL, g_workspace.format);
        }
    }
    return NULL;
}

static int run_twin(const char* flac_path, const char* other_path)
{
    static unsigned char buffer[READ_BYTES];
    host_file parser = {0};
    host_file ours = {0};
    host_file twin = {0};
    drflac_uint32 file_type = 5U;
    void* format = g_working_descriptor + PCM_FORMAT_OFFSET;
    drflac_uint32 queries;
    int status;
    int failures = 0;
    size_t got;

    parser.fp = fopen(flac_path, "rb");
    ours.fp = fopen(flac_path, "rb");
    twin.fp = fopen(other_path, "rb");
    if (parser.fp == NULL || ours.fp == NULL || twin.fp == NULL) {
        fprintf(stderr, "cannot open the files\n");
        return 2;
    }
    memcpy(g_working_descriptor + 8, &file_type, sizeof(file_type));
    if (xdj_flac_adapter(&parser, g_working_descriptor, NULL) != 0) {
        fprintf(stderr, "adapter rejected %s\n", flac_path);
        return 2;
    }
    fclose(parser.fp);
    ours.data_bytes = *(drflac_uint32*)(g_working_descriptor + 24);
    ours.data_start = *(drflac_uint32*)(g_working_descriptor + 20);
    /* The stock file: the same audio length, after a header. */
    twin.data_bytes = ours.data_bytes;
    twin.data_start = 54U;

    status = -1;
    got = xdj_flac_virtual_read_format(&ours, buffer, 9408U, &status, NULL, format);
    if (got != 9408U || status != 0 || ours.data_start != 0U || xdj_host_stock_calls != 0U) {
        fprintf(stderr, "FAILED: our track is not served (%zu bytes, status %d)\n", got, status);
        failures++;
    }
    /* The twin's read and seek must each be handed to the stock routine, once. */
    queries = xdj_host_size_queries;
    got = xdj_flac_virtual_read_format(&twin, buffer, 9408U, &status, NULL, format);
    if (xdj_host_stock_calls != 1U) {
        fprintf(stderr, "FAILED: the stock twin's read was taken for our track (%zu bytes, status %d)\n", got, status);
        failures++;
    }
    xdj_flac_virtual_seek_format(&twin, 0, 0, NULL, format);
    if (xdj_host_stock_calls != 2U) {
        fprintf(stderr, "FAILED: the stock twin's seek was taken for our track\n");
        failures++;
    }
    /* So must those of a stock file of any other length. */
    twin.data_bytes = ours.data_bytes - 6000U;
    xdj_flac_virtual_read_format(&twin, buffer, 9408U, &status, NULL, format);
    xdj_flac_virtual_seek_format(&twin, 0, 0, NULL, format);
    if (xdj_host_stock_calls != 4U) {
        fprintf(stderr, "FAILED: a stock file's calls were not handed to the stock routines\n");
        failures++;
    }
#ifdef XDJ_HOST_NETWORK_FS
    if (xdj_host_size_queries != queries) {
        fprintf(stderr, "FAILED: a stock file's calls cost %u size queries\n", (unsigned)(xdj_host_size_queries - queries));
        failures++;
    }
#else
    (void)queries;
#endif
    /* Our track is still served afterwards, from where it was. */
    status = -1;
    got = xdj_flac_virtual_read_format(&ours, buffer, 9408U, &status, NULL, format);
    if (got != 9408U || status != 0 || xdj_host_stock_calls != 4U) {
        fprintf(stderr, "FAILED: our track is not served after the twin (%zu bytes, status %d)\n", got, status);
        failures++;
    }
    fclose(ours.fp);
    fclose(twin.fp);
    printf("{\"failures\": %d, \"passed\": %s}\n", failures, failures == 0 ? "true" : "false");
    return failures == 0 ? 0 : 3;
}

int main(int argc, char** argv)
{
    drflac_allocation_callbacks allocation = {NULL, host_malloc, host_realloc, host_free};
    pthread_t stream;
    double seconds;
    drflac_uint64 seed;
    struct timespec start;
    struct timespec now;
    FILE* current = NULL;
    int current_track = 0;
    int index;
    int passed;

    if (argc == 4 && strcmp(argv[1], "--twin") == 0) {
        return run_twin(argv[2], argv[3]);
    }
    if (argc < 5 || argc - 3 > MAX_TRACKS) {
        fprintf(stderr, "usage: %s <seconds> <seed> <a.flac> <b.flac> [...] | --twin <a.flac> <other file>\n", argv[0]);
        return 1;
    }
    seconds = atof(argv[1]);
    seed = strtoull(argv[2], NULL, 10) * 0x2545F4914F6CDD1DULL + 1U;
    g_track_count = argc - 3;
    for (index = 0; index < g_track_count; ++index) {
        track* t = &g_tracks[index];
        xdj_flac_stream_info info;
        xdj_flac_decimator decimator;
        xdj_pcm_source source;
        drflac_uint64 decoded;
        FILE* fp = fopen(argv[3 + index], "rb");
        const char* dot = strrchr(argv[3 + index], '.');
        /* The dispatcher's file type, from the name as on the player: 11 WAV, 12 AIFF, 5 FLAC. */
        drflac_uint32 file_type = dot != NULL && strcmp(dot, ".wav") == 0 ? 11U
                                : dot != NULL && strcmp(dot, ".aiff") == 0 ? 12U : 5U;
        drflac* decoder = NULL;
        host_file parser;

        t->path = argv[3 + index];
        if (fp != NULL && file_type == 5U) {
            decoder = xdj_flac_open(ref_read, ref_seek, ref_tell, fp, &allocation, &info);
        } else if (fp != NULL) {
            /* A WAV or AIFF the bridge serves: the reference reads it with the same source. */
            drflac_uint32 frames = 0;

            source.on_read = ref_read;
            source.on_seek = ref_seek;
            source.user_data = fp;
            info.channels = 2;
            if (xdj_pcm_open(&source, file_type == 12U, 0, &info.sample_rate, &frames, &info.bits_per_sample)) {
                info.frame_count = frames;
                decoder = (drflac*)&source;
            }
        }
        if (decoder == NULL || info.channels != 2U) {
            fprintf(stderr, "cannot decode %s\n", t->path);
            return 2;
        }
        xdj_flac_decimator_reset(&decimator);
        if (info.sample_rate == 88200U || info.sample_rate == 96000U) {
            decimator.shift = 1;
            info.sample_rate >>= 1;
            info.frame_count >>= 1;
        }
        t->total = info.frame_count;
        t->bytes_per_frame = info.bits_per_sample == 24U ? 6U : 4U;
        t->total_bytes = t->total * t->bytes_per_frame;
        t->pcm = (unsigned char*)malloc((size_t)t->total_bytes);
        if (file_type != 5U) {
            decoded = xdj_pcm_seek(&source, 0, &decimator)
                          ? xdj_pcm_read(&source, &decimator, t->total, t->pcm, (drflac_uint8)t->bytes_per_frame)
                          : 0;
        } else if (decimator.shift != 0U) {
            decoded = xdj_flac_read_decimated(decoder, &decimator, t->total, t->pcm, (drflac_uint8)t->bytes_per_frame);
        } else if (t->bytes_per_frame == 6U) {
            decoded = xdj_flac_read_s24(decoder, t->total, t->pcm);
        } else {
            decoded = xdj_flac_read_s16(decoder, t->total, (drflac_int16*)t->pcm);
        }
        if (decoded != t->total) {
            return 2;
        }
        if (file_type == 5U) {
            xdj_flac_close(decoder);
        }
        fclose(fp);
        /* The parser opens its own handle and closes it after the open, as on the player. */
        parser.fp = fopen(t->path, "rb");
        memcpy(g_working_descriptor + 8, &file_type, sizeof(file_type));
        if (xdj_flac_adapter(&parser, g_working_descriptor, NULL) != 0) {
            fprintf(stderr, "adapter rejected %s\n", t->path);
            return 2;
        }
        fclose(parser.fp);
        memcpy(t->descriptor, g_working_descriptor, sizeof(t->descriptor));
    }

    /* The first load. */
    current = fopen(g_tracks[0].path, "rb");
    g_stack_context.fp = current;
    g_workspace.context = &g_stack_context;
    g_workspace.format = load_descriptor(0);
    g_stack_context.data_bytes = *(drflac_uint32*)(g_working_descriptor + 24);
    g_command_track = 0;
    g_command_cue = 0;
    atomic_store(&g_command_sequence, 1U);
    pthread_create(&stream, NULL, stream_thread, NULL);

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        int next;
        drflac_uint64 cue;
        FILE* opened;

        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((double)(now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9 > seconds) {
            break;
        }
        sleep_ms(20.0 + (double)(next_random(&seed) % 60U));
        next = (int)(next_random(&seed) % (drflac_uint64)g_track_count);
        /* Cues are byte positions: mostly on a frame, sometimes inside one. */
        cue = (next_random(&seed) % g_tracks[next].total) * g_tracks[next].bytes_per_frame;
        if ((next_random(&seed) & 3U) == 0U) {
            cue += next_random(&seed) % g_tracks[next].bytes_per_frame;
        }
        /* A pad press: the load routine's frame zeroes the context's handle... */
        atomic_fetch_add(&g_switch, 1U);
        pthread_mutex_lock(&g_file_lock);
        g_stack_context.fp = NULL;
        pthread_mutex_unlock(&g_file_lock);
        sleep_ms(1.0);
        /* ...closes the previous track's handle and opens the next one into it... */
        pthread_mutex_lock(&g_file_lock);
        fclose(current);
        opened = fopen(g_tracks[next].path, "rb");
        g_stack_context.fp = opened;
        g_stack_context.data_bytes = *(drflac_uint32*)(g_tracks[next].descriptor + 24);
        pthread_mutex_unlock(&g_file_lock);
        current = opened;
        current_track = next;
        /* ...waits for the DSP fill while the stream keeps reading... */
        sleep_ms(2.0 + (double)(next_random(&seed) % 8U));
        /* ...writes the workspace for the new track and sends the command. */
        g_workspace.format = load_descriptor(next);
#ifdef XDJ_HOST_NETWORK_FS
        /* On a network filesystem each command reads through a handle object from a reused pool. */
        {
            host_file* context = &g_context_pool[g_context_next++ % 2U];

            pthread_mutex_lock(&g_file_lock);
            context->fp = opened;
            context->data_bytes = g_stack_context.data_bytes;
            g_workspace.context = context;
            pthread_mutex_unlock(&g_file_lock);
        }
#endif
        g_command_track = next;
        g_command_cue = cue;
        atomic_fetch_add(&g_command_sequence, 1U);
        /* Wait until the stream has taken it, as the load routine waits for completion. */
        while ((atomic_load(&g_switch) & 1U) != 0U && !atomic_load(&g_stop)) {
            sched_yield();
        }
    }
    atomic_store(&g_stop, 1);
    pthread_join(stream, NULL);
    fclose(current);
    (void)current_track;

    passed = g_failures == 0 && g_mismatches == 0 && g_eof_wrong == 0 && g_exact_reads > 0 && g_commands > 10;
    printf("{\"tracks\": %d, \"commands\": %lu, \"exact_reads\": %lu, \"switch_calls\": %lu, "
           "\"failures\": %lu, \"mismatches\": %lu, \"eof_wrong\": %lu, \"first_problem\": \"%s\", \"passed\": %s}\n",
           g_track_count, g_commands, g_exact_reads, g_window_calls, g_failures, g_mismatches, g_eof_wrong,
           g_first_problem, passed ? "true" : "false");
    return passed ? 0 : 3;
}
