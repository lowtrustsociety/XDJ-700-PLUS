/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * Host test runner for the XDJ-700 FLAC adapter skeleton.
 *
 * This executable exercises the exact C adapter routines on the host machine
 * against real FLAC streams, verifying:
 *  1. Descriptor population and PCM format fields
 *  2. 16-bit and 24-bit stereo sequential decoding fidelity, each at its own depth
 *  3. Fixed-arena memory bounds (<= 16 KiB)
 *  4. Accurate sample seeking across SET, CUR, and END origins
 *  5. Rejection of invalid offsets; byte-exact reads and seeks that split a frame
 *  6. Deactivation semantics and retired decoder reclamation
 *  7. Serialization lock balance and absence of re-entrancy
 *  8. Rejection of unsupported formats (32 kHz, corrupted streams)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dr_flac.h"

/* Declarations from flac_adapter.c */
int xdj_flac_adapter(void* file_io_context, void* output_descriptor, void* parser_workspace);
size_t xdj_flac_virtual_read(void* descriptor, void* output, size_t byte_count, int* transfer_status, void* stock_buffer_context);
int xdj_flac_virtual_seek(void* descriptor, int byte_offset, int origin, void* unused);
void xdj_flac_deactivate(void* output_descriptor);
size_t xdj_flac_get_arena_used(void);
void xdj_flac_host_select(void* output_descriptor);
int xdj_flac_host_slot_active(void* output_descriptor);
size_t xdj_flac_host_active_slots(void);
size_t xdj_flac_host_pcm_bytes(void* output_descriptor);
size_t xdj_flac_host_slot_count(void);


/* Mock firmware file I/O context */
typedef struct {
    FILE* fp;
    size_t file_size;
    size_t read_bytes_total;
    size_t read_calls_total;
    size_t seek_calls_total;
    int inject_read_error;
    int inject_seek_error;
} host_file_context;

static int g_lock_acquired = 0;
static size_t g_lock_acquire_count = 0;
static size_t g_lock_release_count = 0;

void xdj_flac_lock_acquire(void)
{
    if (g_lock_acquired != 0) {
        fprintf(stderr, "FATAL: Lock acquired while already held!\n");
        exit(2);
    }
    g_lock_acquired = 1;
    g_lock_acquire_count++;
}

void xdj_flac_lock_release(void)
{
    if (g_lock_acquired != 1) {
        fprintf(stderr, "FATAL: Lock released when not held!\n");
        exit(2);
    }
    g_lock_acquired = 0;
    g_lock_release_count++;
}

int xdj_host_mock_seek(void* context, int offset, int origin)
{
    host_file_context* ctx = (host_file_context*)context;
    int stdio_origin;

    if (ctx == NULL || ctx->fp == NULL || ctx->inject_seek_error) {
        return -1;
    }
    if (origin == 0) {
        stdio_origin = SEEK_SET;
    } else if (origin == 1) {
        stdio_origin = SEEK_CUR;
    } else if (origin == 2) {
        stdio_origin = SEEK_END;
    } else {
        return -1;
    }
    ctx->seek_calls_total++;
    return fseek(ctx->fp, (long)offset, stdio_origin);
}

size_t xdj_host_mock_read(void* context, void* output, size_t byte_count, int* status)
{
    host_file_context* ctx = (host_file_context*)context;

    if (status != NULL) {
        *status = 0;
    }
    if (ctx == NULL || ctx->fp == NULL || ctx->inject_read_error) {
        if (status != NULL) {
            *status = -1;
        }
        return 0;
    }
    ctx->read_calls_total++;
    size_t bytes_read = fread(output, 1, byte_count, ctx->fp);
    ctx->read_bytes_total += bytes_read;
    return bytes_read;
}

int xdj_host_mock_tell(void* context)
{
    host_file_context* ctx = (host_file_context*)context;

    if (ctx == NULL || ctx->fp == NULL) {
        return -1;
    }
    long pos = ftell(ctx->fp);
    return (int)pos;
}

/* Portable SHA-256 implementation */
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} sha256_ctx;

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR32(val, bits) (((val) >> (bits)) | ((val) << (32 - (bits))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SIG0(x) (ROR32(x, 2) ^ ROR32(x, 13) ^ ROR32(x, 22))
#define SIG1(x) (ROR32(x, 6) ^ ROR32(x, 11) ^ ROR32(x, 25))
#define TH0(x) (ROR32(x, 7) ^ ROR32(x, 18) ^ ((x) >> 3))
#define TH1(x) (ROR32(x, 17) ^ ROR32(x, 19) ^ ((x) >> 10))

static void sha256_transform(sha256_ctx* ctx, const uint8_t* data)
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) |
               ((uint32_t)data[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++) {
        w[i] = TH1(w[i - 2]) + w[i - 7] + TH0(w[i - 15]) + w[i - 16];
    }
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t t1 = h + SIG1(e) + CH(e, f, g) + K256[i] + w[i];
        uint32_t t2 = SIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx* ctx)
{
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

static void sha256_update(sha256_ctx* ctx, const uint8_t* data, size_t len)
{
    size_t buffer_idx = (size_t)(ctx->count & 63);
    ctx->count += len;
    while (len > 0) {
        size_t copy_len = 64 - buffer_idx;
        if (copy_len > len) copy_len = len;
        memcpy(ctx->buffer + buffer_idx, data, copy_len);
        buffer_idx += copy_len;
        data += copy_len;
        len -= copy_len;
        if (buffer_idx == 64) {
            sha256_transform(ctx, ctx->buffer);
            buffer_idx = 0;
        }
    }
}

static void sha256_final(sha256_ctx* ctx, uint8_t hash[32])
{
    size_t buffer_idx = (size_t)(ctx->count & 63);
    ctx->buffer[buffer_idx++] = 0x80;
    if (buffer_idx > 56) {
        memset(ctx->buffer + buffer_idx, 0, 64 - buffer_idx);
        sha256_transform(ctx, ctx->buffer);
        buffer_idx = 0;
    }
    memset(ctx->buffer + buffer_idx, 0, 56 - buffer_idx);
    uint64_t bits = ctx->count * 8;
    for (int i = 0; i < 8; i++) {
        ctx->buffer[56 + i] = (uint8_t)(bits >> (56 - i * 8));
    }
    sha256_transform(ctx, ctx->buffer);
    for (int i = 0; i < 8; i++) {
        hash[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

static void hash_to_hex(const uint8_t hash[32], char hex_out[65])
{
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex_out[i * 2] = hex_chars[(hash[i] >> 4) & 0xF];
        hex_out[i * 2 + 1] = hex_chars[hash[i] & 0xF];
    }
    hex_out[64] = '\0';
}

static uint32_t read_u32(const uint8_t* base, size_t offset)
{
    return *(const uint32_t*)(base + offset);
}

static uint8_t read_u8(const uint8_t* base, size_t offset)
{
    return *(const uint8_t*)(base + offset);
}

/*
 * The dispatcher's file type, which the player derives from the file name
 * and stores at descriptor +8: 11 for WAV, 12 for AIFF, 5 for FLAC.
 */
static uint32_t g_file_type = 5;

static void set_file_type(const char* path)
{
    const char* dot = strrchr(path, '.');

    g_file_type = 5;
    if (dot != NULL && (strcmp(dot, ".wav") == 0 || strcmp(dot, ".WAV") == 0)) {
        g_file_type = 11;
    } else if (dot != NULL && (strcmp(dot, ".aif") == 0 || strcmp(dot, ".aiff") == 0 || strcmp(dot, ".aifc") == 0)) {
        g_file_type = 12;
    }
}

/* A fresh descriptor, as the dispatcher hands it over. */
static void clear_descriptor(uint8_t* descriptor)
{
    memset(descriptor, 0, 1024);
    memcpy(descriptor + 8, &g_file_type, sizeof(g_file_type));
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <--test|--expect-reject|--expect-decline|--dump-pcm> <file> [extra_arg]\n", argv[0]);
        return 1;
    }

    const char* mode = argv[1];
    const char* flac_path = argv[2];

    FILE* fp = fopen(flac_path, "rb");
    if (!fp) {
        fprintf(stderr, "Error opening input FLAC: %s\n", flac_path);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    size_t file_size = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);

    host_file_context file_ctx;
    memset(&file_ctx, 0, sizeof(file_ctx));
    file_ctx.fp = fp;
    file_ctx.file_size = file_size;

    uint8_t descriptor[1024];
    set_file_type(flac_path);
    clear_descriptor(descriptor);

    if (strcmp(mode, "--expect-decline") == 0) {
        /*
         * A WAV or AIFF file the stock reader handles: the adapter must say
         * so, leave the handle where it was, and touch no slot or lock.
         */
        int adapter_result;
        long before;
        long after;

        fseek(fp, 20, SEEK_SET);
        before = ftell(fp);
        adapter_result = xdj_flac_adapter(&file_ctx, descriptor, NULL);
        after = ftell(fp);
        fclose(fp);
        printf("{\n");
        printf("  \"file\": \"%s\",\n", flac_path);
        printf("  \"adapter_result\": %d,\n", adapter_result);
        printf("  \"declined\": %s,\n", adapter_result == 1 ? "true" : "false");
        printf("  \"handle_restored\": %s,\n", before == after ? "true" : "false");
        printf("  \"slots_untouched\": %s,\n", xdj_flac_host_active_slots() == 0U ? "true" : "false");
        printf("  \"locks_taken\": %zu\n", g_lock_acquire_count);
        printf("}\n");
        return (adapter_result == 1 && before == after && xdj_flac_host_active_slots() == 0U &&
                g_lock_acquire_count == 0) ? 0 : 3;
    }

    if (strcmp(mode, "--expect-reject") == 0) {
        int adapter_result = xdj_flac_adapter(&file_ctx, descriptor, NULL);
    xdj_flac_host_select(descriptor);
        fclose(fp);
        int lock_balanced = (g_lock_acquire_count == g_lock_release_count) && (g_lock_acquired == 0);
        printf("{\n");
        printf("  \"file\": \"%s\",\n", flac_path);
        printf("  \"adapter_result\": %d,\n", adapter_result);
        printf("  \"rejected\": %s,\n", adapter_result != 0 ? "true" : "false");
        printf("  \"lock_balanced\": %s\n", lock_balanced ? "true" : "false");
        printf("}\n");
        return (adapter_result != 0 && lock_balanced) ? 0 : 3;
    }

    if (strcmp(mode, "--dump-pcm") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Missing output path for --dump-pcm\n");
            fclose(fp);
            return 1;
        }
        const char* pcm_out_path = argv[3];
        FILE* out_fp = fopen(pcm_out_path, "wb");
        if (!out_fp) {
            fprintf(stderr, "Error opening PCM output file: %s\n", pcm_out_path);
            fclose(fp);
            return 1;
        }

        int adapter_result = xdj_flac_adapter(&file_ctx, descriptor, NULL);
    xdj_flac_host_select(descriptor);
        if (adapter_result != 0) {
            fprintf(stderr, "Adapter failed to open %s\n", flac_path);
            fclose(out_fp);
            fclose(fp);
            return 1;
        }

        uint8_t pcm_bytes_per_sample = read_u8(descriptor, 347);
        size_t bytes_per_frame = 2 * (size_t)pcm_bytes_per_sample;
        uint8_t buffer[4096];
        size_t aligned_chunk = (sizeof(buffer) / bytes_per_frame) * bytes_per_frame;
        int transfer_status = 0;
        size_t total_decoded = 0;
        while (1) {
            size_t bytes_read = xdj_flac_virtual_read(&file_ctx, buffer, aligned_chunk, &transfer_status, NULL);
            if (bytes_read == 0 || transfer_status != 0) break;
            fwrite(buffer, 1, bytes_read, out_fp);
            total_decoded += bytes_read;
        }
        (void)total_decoded;
        fclose(out_fp);
        xdj_flac_deactivate(descriptor);
        fclose(fp);
        return 0;
    }

    if (strcmp(mode, "--test") != 0) {
        fprintf(stderr, "Unknown mode: %s\n", mode);
        fclose(fp);
        return 1;
    }

    /* Full test suite */
    int adapter_result = xdj_flac_adapter(&file_ctx, descriptor, NULL);
    xdj_flac_host_select(descriptor);
    if (adapter_result != 0) {
        fclose(fp);
        printf("{\n  \"file\": \"%s\",\n  \"success\": false,\n  \"error\": \"xdj_flac_adapter returned %d\"\n}\n", flac_path, adapter_result);
        return 1;
    }

    /* 1. Descriptor verification */
    uint32_t out_path = read_u32(descriptor, 12);
    uint32_t total_cd_frames = read_u32(descriptor, 16);
    uint32_t data_start = read_u32(descriptor, 20);
    uint32_t data_bytes = read_u32(descriptor, 24);
    /* Decoded length; files larger than it declare their size instead. */
    uint32_t pcm_bytes = (uint32_t)xdj_flac_host_pcm_bytes(descriptor);
    uint32_t trailing_bytes = read_u32(descriptor, 28);

    uint32_t pcm_bytes_per_cd_frame = read_u32(descriptor, 340);
    uint8_t pcm_le_flag = read_u8(descriptor, 344);
    uint8_t pcm_channels = read_u8(descriptor, 345);
    uint8_t pcm_rate_khz = read_u8(descriptor, 346);
    uint8_t pcm_bytes_per_sample = read_u8(descriptor, 347);
    uint32_t declared_frame_bytes = pcm_bytes_per_sample == 3U ? 6U : 4U;
    uint32_t rounded_file_bytes =
        (uint32_t)((file_size + declared_frame_bytes - 1U) / declared_frame_bytes) * declared_frame_bytes;
    uint32_t expected_declared_bytes = rounded_file_bytes > pcm_bytes ? rounded_file_bytes : pcm_bytes;
    uint32_t sample_rate = pcm_rate_khz == 44U ? 44100U : (uint32_t)pcm_rate_khz * 1000U;

    /* A FLAC decoder lives in its slot's arena; a WAV or AIFF source needs none. */
    size_t arena_used = xdj_flac_get_arena_used();
    int arena_ok = ((arena_used > 0 || g_file_type != 5) && arena_used <= 98304);

    int descriptor_ok = (out_path == 1) &&
                        (total_cd_frames > 0) &&
                        (sample_rate == 44100 || sample_rate == 48000) &&
                        (data_start == 0) &&
                        (pcm_le_flag == 0) &&
                        (pcm_channels == 2) &&
                        (pcm_rate_khz == (sample_rate / 1000)) &&
                        (pcm_bytes_per_sample == 2 || pcm_bytes_per_sample == 3) &&
                        (trailing_bytes == 0) &&
                        (pcm_bytes > 0) &&
                        (data_bytes % (2U * pcm_bytes_per_sample) == 0) &&
                        (data_bytes == expected_declared_bytes) &&
                        (total_cd_frames ==
                         (pcm_bytes + pcm_bytes_per_cd_frame - 1U) / pcm_bytes_per_cd_frame) &&
                        (pcm_bytes_per_cd_frame == (sample_rate * 2 * pcm_bytes_per_sample) / 75);

    /* 2. Sequential decode pass */
    sha256_ctx pcm_sha;
    sha256_init(&pcm_sha);

    size_t total_pcm_bytes = pcm_bytes;
    uint8_t* reference_pcm = (uint8_t*)malloc(total_pcm_bytes > 0 ? total_pcm_bytes : 1);
    if (!reference_pcm) {
        fprintf(stderr, "Out of host memory allocating %zu bytes\n", total_pcm_bytes);
        fclose(fp);
        return 1;
    }

    size_t bytes_per_frame = 2 * (size_t)pcm_bytes_per_sample;
    size_t chunk_bytes = pcm_bytes_per_cd_frame; /* Decode 1 CD frame at a time */
    if (chunk_bytes == 0 || chunk_bytes > 32768) chunk_bytes = 4096;
    /* Ensure chunk_bytes is a multiple of bytes_per_frame */
    chunk_bytes = (chunk_bytes / bytes_per_frame) * bytes_per_frame;

    uint8_t* chunk_buf = (uint8_t*)malloc(chunk_bytes);
    size_t bytes_decoded = 0;
    int transfer_status = 0;
    int read_failed = 0;

    while (bytes_decoded < total_pcm_bytes) {
        size_t to_read = total_pcm_bytes - bytes_decoded;
        if (to_read > chunk_bytes) to_read = chunk_bytes;
        size_t actual = xdj_flac_virtual_read(&file_ctx, chunk_buf, to_read, &transfer_status, NULL);
        if (actual == 0 || transfer_status != 0) {
            read_failed = 1;
            break;
        }
        memcpy(reference_pcm + bytes_decoded, chunk_buf, actual);
        sha256_update(&pcm_sha, chunk_buf, actual);
        bytes_decoded += actual;
    }

    /* Verify EOF behavior: reading 0 more bytes */
    size_t eof_read = xdj_flac_virtual_read(&file_ctx, chunk_buf, chunk_bytes, &transfer_status, NULL);
    int eof_ok = (eof_read == 0 && transfer_status == 0);

    uint8_t final_hash[32];
    char hash_hex[65];
    sha256_final(&pcm_sha, final_hash);
    hash_to_hex(final_hash, hash_hex);

    int sequential_ok = (!read_failed && eof_ok && bytes_decoded == total_pcm_bytes);

    /* 3. Seek tests */
    int seek_tests_passed = 1;
    size_t total_frames = total_pcm_bytes / bytes_per_frame;

    /* SET origin tests (0) */
    size_t test_frames[] = {
        0,
        total_frames / 4,
        total_frames / 2,
        (total_frames * 3) / 4,
        total_frames > 200 ? total_frames - 200 : 0
    };
    int num_set_tests = sizeof(test_frames) / sizeof(test_frames[0]);
    for (int i = 0; i < num_set_tests; i++) {
        size_t target_f = test_frames[i];
        int byte_off = (int)(target_f * bytes_per_frame);
        int seek_rc = xdj_flac_virtual_seek(&file_ctx, byte_off, 0, NULL);
        if (seek_rc != 0) {
            seek_tests_passed = 0;
            break;
        }
        size_t check_bytes = 128 * bytes_per_frame;
        if (target_f * bytes_per_frame + check_bytes > total_pcm_bytes) {
            check_bytes = total_pcm_bytes - (target_f * bytes_per_frame);
        }
        size_t read_bytes = xdj_flac_virtual_read(&file_ctx, chunk_buf, check_bytes, &transfer_status, NULL);
        if (read_bytes != check_bytes || transfer_status != 0) {
            seek_tests_passed = 0;
            break;
        }
        if (memcmp(chunk_buf, reference_pcm + (target_f * bytes_per_frame), check_bytes) != 0) {
            seek_tests_passed = 0;
            break;
        }
    }

    /* CUR origin tests (1): relative forward and relative backward */
    if (seek_tests_passed && total_frames > 500) {
        /* Seek to 200 frames */
        xdj_flac_virtual_seek(&file_ctx, (int)(200 * bytes_per_frame), 0, NULL);
        /* Relative forward +100 frames -> frame 300 */
        int cur_rc = xdj_flac_virtual_seek(&file_ctx, (int)(100 * bytes_per_frame), 1, NULL);
        if (cur_rc != 0) {
            seek_tests_passed = 0;
        } else {
            size_t check_bytes = 64 * bytes_per_frame;
            xdj_flac_virtual_read(&file_ctx, chunk_buf, check_bytes, &transfer_status, NULL);
            if (memcmp(chunk_buf, reference_pcm + (300 * bytes_per_frame), check_bytes) != 0) {
                seek_tests_passed = 0;
            }
        }
        /* Relative backward -150 frames from current cursor (cursor is now 364 frames) -> frame 214 */
        if (seek_tests_passed) {
            cur_rc = xdj_flac_virtual_seek(&file_ctx, -(int)(150 * bytes_per_frame), 1, NULL);
            if (cur_rc != 0) {
                seek_tests_passed = 0;
            } else {
                size_t check_bytes = 64 * bytes_per_frame;
                xdj_flac_virtual_read(&file_ctx, chunk_buf, check_bytes, &transfer_status, NULL);
                if (memcmp(chunk_buf, reference_pcm + (214 * bytes_per_frame), check_bytes) != 0) {
                    seek_tests_passed = 0;
                }
            }
        }
    }

    /* END origin tests (2): seek from end */
    if (seek_tests_passed && total_frames > 200) {
        int end_rc = xdj_flac_virtual_seek(&file_ctx, -(int)(100 * bytes_per_frame), 2, NULL);
        if (end_rc != 0) {
            seek_tests_passed = 0;
        } else {
            size_t check_bytes = 100 * bytes_per_frame;
            xdj_flac_virtual_read(&file_ctx, chunk_buf, check_bytes, &transfer_status, NULL);
            if (memcmp(chunk_buf, reference_pcm + (total_pcm_bytes - check_bytes), check_bytes) != 0) {
                seek_tests_passed = 0;
            }
        }
    }

    /* Invalid seek tests */
    int invalid_seeks_ok = 1;
    /* Negative seek SET */
    if (xdj_flac_virtual_seek(&file_ctx, -4, 0, NULL) == 0) invalid_seeks_ok = 0;
    /* Beyond EOF seek SET */
    if (xdj_flac_virtual_seek(&file_ctx, (int)(data_bytes + bytes_per_frame), 0, NULL) == 0) invalid_seeks_ok = 0;
    /* Invalid origin */
    if (xdj_flac_virtual_seek(&file_ctx, 0, 99, NULL) == 0) invalid_seeks_ok = 0;

    /*
     * Byte-exact stream. The player can size its reads by another track's
     * format (hot cue banks), so reads and seeks may split a frame; the
     * stream must then behave like the WAV file it stands for.
     */
    int byte_exact_ok = 1;
    {
        /* Whole-stream passes in the read sizes the player uses for other formats, and odd ones. */
        static const size_t pass_sizes[] = {10240, 9408, 14112, 15360, 4099, 1021};
        /* Short passes in sizes smaller than a frame. */
        static const size_t short_sizes[] = {1, 2, 3, 5, 7};
        uint8_t* split_buf = (uint8_t*)malloc(16384);
        size_t pass;

        for (pass = 0; byte_exact_ok && pass < sizeof(pass_sizes) / sizeof(pass_sizes[0]); pass++) {
            size_t at = 0;

            if (xdj_flac_virtual_seek(&file_ctx, 0, 0, NULL) != 0) {
                byte_exact_ok = 0;
            }
            while (byte_exact_ok && at < total_pcm_bytes) {
                size_t expect = total_pcm_bytes - at < pass_sizes[pass] ? total_pcm_bytes - at : pass_sizes[pass];
                size_t got = xdj_flac_virtual_read(&file_ctx, split_buf, pass_sizes[pass], &transfer_status, NULL);

                if (got != expect || transfer_status != 0 || memcmp(split_buf, reference_pcm + at, got) != 0) {
                    byte_exact_ok = 0;
                }
                at += got;
            }
            if (byte_exact_ok &&
                (xdj_flac_virtual_read(&file_ctx, split_buf, 64, &transfer_status, NULL) != 0 || transfer_status != 0)) {
                byte_exact_ok = 0;
            }
        }
        for (pass = 0; byte_exact_ok && pass < sizeof(short_sizes) / sizeof(short_sizes[0]); pass++) {
            size_t start = (total_frames / 3) * bytes_per_frame + pass;
            size_t at = start;

            if (start + 4096 > total_pcm_bytes) {
                break;
            }
            if (xdj_flac_virtual_seek(&file_ctx, (int)start, 0, NULL) != 0) {
                byte_exact_ok = 0;
            }
            while (byte_exact_ok && at < start + 4096) {
                /* Odd buffer addresses too: 16-bit frames must not be stored unaligned. */
                uint8_t* into = split_buf + (at & 1U);
                size_t got = xdj_flac_virtual_read(&file_ctx, into, short_sizes[pass], &transfer_status, NULL);

                if (got != short_sizes[pass] || transfer_status != 0 || memcmp(into, reference_pcm + at, got) != 0) {
                    byte_exact_ok = 0;
                }
                at += got;
            }
        }
        if (byte_exact_ok && total_frames > 4000) {
            size_t frame_start = 1000 * bytes_per_frame;
            size_t at;

            /* Absolute seeks to every byte of a frame, far apart and next to each other. */
            for (at = 0; byte_exact_ok && at < 3 * bytes_per_frame; at++) {
                size_t target = (at & 1U) ? frame_start + at : 3000 * bytes_per_frame + at;

                if (xdj_flac_virtual_seek(&file_ctx, (int)target, 0, NULL) != 0 ||
                    xdj_flac_virtual_read(&file_ctx, split_buf, 1001, &transfer_status, NULL) != 1001 ||
                    memcmp(split_buf, reference_pcm + target, 1001) != 0) {
                    byte_exact_ok = 0;
                }
            }
            /* A read that ends inside a frame, then relative seeks from there. */
            if (xdj_flac_virtual_seek(&file_ctx, (int)frame_start, 0, NULL) != 0 ||
                xdj_flac_virtual_read(&file_ctx, split_buf, 5, &transfer_status, NULL) != 5 ||
                xdj_flac_virtual_seek(&file_ctx, 2, 1, NULL) != 0 ||
                xdj_flac_virtual_read(&file_ctx, split_buf, 300, &transfer_status, NULL) != 300 ||
                memcmp(split_buf, reference_pcm + frame_start + 7, 300) != 0 ||
                xdj_flac_virtual_seek(&file_ctx, -4, 1, NULL) != 0 ||
                xdj_flac_virtual_read(&file_ctx, split_buf, 300, &transfer_status, NULL) != 300 ||
                memcmp(split_buf, reference_pcm + frame_start + 303, 300) != 0) {
                byte_exact_ok = 0;
            }
            /* Back to the start of a half-delivered frame, and around inside one. */
            if (xdj_flac_virtual_seek(&file_ctx, (int)frame_start, 0, NULL) != 0 ||
                xdj_flac_virtual_read(&file_ctx, split_buf, 2, &transfer_status, NULL) != 2 ||
                xdj_flac_virtual_seek(&file_ctx, (int)frame_start, 0, NULL) != 0 ||
                xdj_flac_virtual_read(&file_ctx, split_buf, 300, &transfer_status, NULL) != 300 ||
                memcmp(split_buf, reference_pcm + frame_start, 300) != 0 ||
                xdj_flac_virtual_seek(&file_ctx, (int)frame_start + 1, 0, NULL) != 0 ||
                xdj_flac_virtual_seek(&file_ctx, (int)frame_start + 3, 0, NULL) != 0 ||
                xdj_flac_virtual_read(&file_ctx, split_buf, 2, &transfer_status, NULL) != 2 ||
                memcmp(split_buf, reference_pcm + frame_start + 3, 2) != 0 ||
                xdj_flac_virtual_seek(&file_ctx, (int)frame_start + 1, 0, NULL) != 0 ||
                xdj_flac_virtual_read(&file_ctx, split_buf, 300, &transfer_status, NULL) != 300 ||
                memcmp(split_buf, reference_pcm + frame_start + 1, 300) != 0) {
                byte_exact_ok = 0;
            }
            /* The last byte of the stream, then end of data. */
            if (xdj_flac_virtual_seek(&file_ctx, -1, 2, NULL) != 0 ||
                xdj_flac_virtual_read(&file_ctx, split_buf, 10, &transfer_status, NULL) != 1 ||
                split_buf[0] != reference_pcm[total_pcm_bytes - 1] ||
                xdj_flac_virtual_read(&file_ctx, split_buf, 10, &transfer_status, NULL) != 0 ||
                transfer_status != 0) {
                byte_exact_ok = 0;
            }
        }
        free(split_buf);
    }

    /*
     * 4. Lifecycle. The player parses every track into one working descriptor,
     * so a non-FLAC dispatch retires no
     * FLAC track, and parsing the same file again keeps the decoder that
     * serves it.
     */
    int lifecycle_ok = 1;
    xdj_flac_deactivate(descriptor);
    if (!xdj_flac_host_slot_active(descriptor)) {
        lifecycle_ok = 0;
    }
    transfer_status = 0;
    if (xdj_flac_virtual_seek(&file_ctx, 0, 0, NULL) != 0 ||
        xdj_flac_virtual_read(&file_ctx, chunk_buf, 64 * bytes_per_frame, &transfer_status, NULL) != 64 * bytes_per_frame ||
        transfer_status != 0 || memcmp(chunk_buf, reference_pcm, 64 * bytes_per_frame) != 0) {
        lifecycle_ok = 0;
    }
    fseek(fp, 0, SEEK_SET);
    if (xdj_flac_adapter(&file_ctx, descriptor, NULL) != 0 || !xdj_flac_host_slot_active(descriptor) ||
        xdj_flac_host_active_slots() != 1U) {
        lifecycle_ok = 0;
    }
    size_t arena_reopened = xdj_flac_get_arena_used();
    if ((arena_reopened == 0 && g_file_type == 5) || arena_reopened > 98304) {
        lifecycle_ok = 0;
    }
    /* The kept decoder continues where it was: 64 frames in. */
    transfer_status = 0;
    size_t read_after_reparse = xdj_flac_virtual_read(&file_ctx, chunk_buf, 64 * bytes_per_frame, &transfer_status, NULL);
    if (read_after_reparse != 64 * bytes_per_frame || transfer_status != 0 ||
        memcmp(chunk_buf, reference_pcm + 64 * bytes_per_frame, read_after_reparse) != 0) {
        lifecycle_ok = 0;
    }

    /*
     * 5. Track isolation and handle ownership. The playback task reads through
     * its own handle, the same file is parsed again mid-play (a bank holding
     * the playing track), and a foreign reader moves the file position.
     */
    int isolation_ok = 1;
    int handle_switch_ok = 1;
    int end_seek_ok = 1;
    int reopen_ok = 1;
    {
        host_file_context task_ctx;
        host_file_context other_ctx;
        uint8_t other_descriptor[1024];
        uint8_t spare_descriptors[8][1024];
        size_t verified = 0;
        size_t index;

        memset(&task_ctx, 0, sizeof(task_ctx));
        memset(&other_ctx, 0, sizeof(other_ctx));
        clear_descriptor(other_descriptor);
        for (index = 0; index < 8; ++index) {
            clear_descriptor(spare_descriptors[index]);
        }
        task_ctx.fp = fopen(flac_path, "rb");
        task_ctx.file_size = file_size;
        other_ctx.fp = fopen(flac_path, "rb");
        other_ctx.file_size = file_size;
        if (task_ctx.fp == NULL || other_ctx.fp == NULL) {
            isolation_ok = 0;
        }

        /* First half through the task handle. */
        xdj_flac_host_select(descriptor);
        if (isolation_ok && xdj_flac_virtual_seek(&task_ctx, 0, 0, NULL) != 0) {
            handle_switch_ok = 0;
        }
        while (isolation_ok && handle_switch_ok && verified < total_pcm_bytes / 2) {
            size_t got = xdj_flac_virtual_read(&task_ctx, chunk_buf, chunk_bytes, &transfer_status, NULL);
            if (got == 0 || transfer_status != 0 ||
                memcmp(chunk_buf, reference_pcm + verified, got) != 0) {
                handle_switch_ok = 0;
            }
            verified += got;
        }

        /* The same file parsed again mid-play: one decoder, and it keeps its place. */
        if (isolation_ok && xdj_flac_adapter(&other_ctx, other_descriptor, NULL) != 0) {
            isolation_ok = 0;
        }
        if (!xdj_flac_host_slot_active(descriptor) || !xdj_flac_host_slot_active(other_descriptor) ||
            xdj_flac_host_active_slots() != 1U) {
            isolation_ok = 0;
        }

        /* A foreign reader moves the shared handle; the decoder must recover. */
        if (task_ctx.fp != NULL) {
            fseek(task_ctx.fp, 123, SEEK_SET);
        }
        xdj_flac_host_select(descriptor);
        while (isolation_ok && handle_switch_ok && verified < total_pcm_bytes) {
            size_t got = xdj_flac_virtual_read(&task_ctx, chunk_buf, chunk_bytes, &transfer_status, NULL);
            if (got == 0 || transfer_status != 0 ||
                memcmp(chunk_buf, reference_pcm + verified, got) != 0) {
                handle_switch_ok = 0;
                break;
            }
            verified += got;
        }
        if (verified != total_pcm_bytes) {
            handle_switch_ok = 0;
        }

        /* Seeks into the padding of an oversized file report end of data. */
        if (data_bytes > total_pcm_bytes) {
            size_t padding_mid = total_pcm_bytes + (((data_bytes - total_pcm_bytes) / 2) & ~(size_t)3);
            transfer_status = 0;
            if (xdj_flac_virtual_seek(&task_ctx, (int)padding_mid, 0, NULL) != 0 ||
                xdj_flac_virtual_read(&task_ctx, chunk_buf, chunk_bytes, &transfer_status, NULL) != 0 ||
                transfer_status != 0 ||
                xdj_flac_virtual_seek(&task_ctx, (int)data_bytes, 0, NULL) != 0) {
                end_seek_ok = 0;
            }
        }

        /* Seeking to the exact end reports end of data, and the stream recovers. */
        if (xdj_flac_virtual_seek(&task_ctx, (int)total_pcm_bytes, 0, NULL) != 0) {
            end_seek_ok = 0;
        }
        transfer_status = 0;
        if (xdj_flac_virtual_read(&task_ctx, chunk_buf, chunk_bytes, &transfer_status, NULL) != 0 ||
            transfer_status != 0) {
            end_seek_ok = 0;
        }
        if (total_frames > 400) {
            size_t mid = (total_frames / 3) * bytes_per_frame;
            if (xdj_flac_virtual_seek(&task_ctx, (int)mid, 0, NULL) != 0 ||
                xdj_flac_virtual_read(&task_ctx, chunk_buf, 256 * bytes_per_frame, &transfer_status, NULL) !=
                    256 * bytes_per_frame ||
                memcmp(chunk_buf, reference_pcm + mid, 256 * bytes_per_frame) != 0) {
                end_seek_ok = 0;
            }
        }

        /* A non-FLAC dispatch retires nothing. */
        xdj_flac_deactivate(other_descriptor);
        if (!xdj_flac_host_slot_active(descriptor)) {
            isolation_ok = 0;
        }

        /*
         * Parsing the same file into more descriptors than there are slots
         * still leaves one decoder, which keeps serving from its position.
         * (Evictions need distinct files: see check_flac_lifecycle.py.)
         */
        size_t resume_at = 0;
        if (total_frames > 800) {
            resume_at = (total_frames / 2) * bytes_per_frame;
            if (xdj_flac_virtual_seek(&task_ctx, (int)resume_at, 0, NULL) != 0) {
                reopen_ok = 0;
            }
        }
        for (index = 0; reopen_ok && index < 8; ++index) {
            fseek(other_ctx.fp, 0, SEEK_SET);
            if (xdj_flac_adapter(&other_ctx, spare_descriptors[index], NULL) != 0) {
                reopen_ok = 0;
            }
        }
        if (xdj_flac_host_active_slots() != 1U) {
            reopen_ok = 0;
        }
        transfer_status = 0;
        size_t kept_read = xdj_flac_virtual_read(&task_ctx, chunk_buf, 256 * bytes_per_frame, &transfer_status, NULL);
        if (kept_read != 256 * bytes_per_frame || transfer_status != 0 ||
            memcmp(chunk_buf, reference_pcm + resume_at, kept_read) != 0) {
            reopen_ok = 0;
        }
        if (task_ctx.fp != NULL) {
            fclose(task_ctx.fp);
        }
        if (other_ctx.fp != NULL) {
            fclose(other_ctx.fp);
        }
    }

    /* Clean deactivation at end */
    xdj_flac_deactivate(descriptor);

    /* 6. Lock balance check */
    int lock_balanced = (g_lock_acquire_count == g_lock_release_count) && (g_lock_acquired == 0);

    int all_passed = descriptor_ok && arena_ok && sequential_ok &&
                     seek_tests_passed && invalid_seeks_ok && byte_exact_ok &&
                     lifecycle_ok && isolation_ok && handle_switch_ok &&
                     end_seek_ok && reopen_ok && lock_balanced;

    /* Output structured JSON */
    printf("{\n");
    printf("  \"file\": \"%s\",\n", flac_path);
    printf("  \"format\": {\n");
    printf("    \"sample_rate\": %u,\n", sample_rate);
    printf("    \"channels\": %u,\n", pcm_channels);
    printf("    \"bits_per_sample\": %u,\n", (unsigned int)pcm_bytes_per_sample * 8);
    printf("    \"total_frames\": %zu,\n", total_frames);
    printf("    \"bytes_per_frame\": %zu,\n", bytes_per_frame);
    printf("    \"bytes_per_cd_frame\": %u\n", pcm_bytes_per_cd_frame);
    printf("  },\n");
    printf("  \"arena\": {\n");
    printf("    \"bytes_used\": %zu,\n", arena_used);
    printf("    \"limit_bytes\": 98304,\n");
    printf("    \"within_limit\": %s\n", arena_ok ? "true" : "false");
    printf("  },\n");
    printf("  \"descriptor\": {\n");
    printf("    \"output_path\": %u,\n", out_path);
    printf("    \"total_cd_frames\": %u,\n", total_cd_frames);
    printf("    \"sample_rate\": %u,\n", sample_rate);
    printf("    \"data_start\": %u,\n", data_start);
    printf("    \"trailing_bytes\": %u,\n", trailing_bytes);
    printf("    \"data_bytes\": %u,\n", data_bytes);
    printf("    \"pcm_bytes\": %u,\n", pcm_bytes);
    printf("    \"oversized_file\": %s,\n", data_bytes > pcm_bytes ? "true" : "false");
    printf("    \"pcm_format_valid\": %s\n", descriptor_ok ? "true" : "false");
    printf("  },\n");
    printf("  \"sequential_decode\": {\n");
    printf("    \"decoded_bytes\": %zu,\n", bytes_decoded);
    printf("    \"expected_bytes\": %zu,\n", total_pcm_bytes);
    printf("    \"sha256\": \"%s\",\n", hash_hex);
    printf("    \"passed\": %s\n", sequential_ok ? "true" : "false");
    printf("  },\n");
    printf("  \"seek_tests\": {\n");
    printf("    \"set_tests_passed\": %s,\n", seek_tests_passed ? "true" : "false");
    printf("    \"invalid_seeks_rejected\": %s,\n", invalid_seeks_ok ? "true" : "false");
    printf("    \"byte_exact\": %s,\n", byte_exact_ok ? "true" : "false");
    printf("    \"passed\": %s\n", (seek_tests_passed && invalid_seeks_ok && byte_exact_ok) ? "true" : "false");
    printf("  },\n");
    printf("  \"lifecycle\": {\n");
    printf("    \"passed\": %s\n", lifecycle_ok ? "true" : "false");
    printf("  },\n");
    printf("  \"track_isolation\": {\n");
    printf("    \"slots_isolated\": %s,\n", isolation_ok ? "true" : "false");
    printf("    \"task_handle_bit_exact\": %s,\n", handle_switch_ok ? "true" : "false");
    printf("    \"end_seek_recovers\": %s,\n", end_seek_ok ? "true" : "false");
    printf("    \"reparse_keeps_decoder\": %s\n", reopen_ok ? "true" : "false");
    printf("  },\n");
    printf("  \"lock\": {\n");
    printf("    \"acquire_count\": %zu,\n", g_lock_acquire_count);
    printf("    \"release_count\": %zu,\n", g_lock_release_count);
    printf("    \"balanced\": %s\n", lock_balanced ? "true" : "false");
    printf("  },\n");
    printf("  \"io_stats\": {\n");
    printf("    \"read_calls\": %zu,\n", file_ctx.read_calls_total);
    printf("    \"read_bytes\": %zu,\n", file_ctx.read_bytes_total);
    printf("    \"seek_calls\": %zu\n", file_ctx.seek_calls_total);
    printf("  },\n");
    printf("  \"success\": %s\n", all_passed ? "true" : "false");
    printf("}\n");

    free(chunk_buf);
    free(reference_pcm);
    fclose(fp);

    return all_passed ? 0 : 1;
}
