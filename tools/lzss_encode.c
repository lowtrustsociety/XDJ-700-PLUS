/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/* Host-only encoder for the LZSS stream used by the inspected XDJ firmware.
 * It does not contain player code and cannot access a player. */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define HASH_COUNT 65536u
#define WINDOW_BYTES 4096u
#define MAX_MATCH 18u
#define INITIAL_CURSOR (WINDOW_BYTES - MAX_MATCH)
#define NO_POSITION SIZE_MAX

static unsigned hash_three(const uint8_t *data) {
    return ((unsigned)data[0] * 251u + (unsigned)data[1] * 31u + data[2])
           & (HASH_COUNT - 1u);
}

static size_t find_match(const uint8_t *input, size_t input_bytes, size_t position,
                         const size_t *heads, const size_t *previous,
                         size_t *match_position) {
    size_t best = 0;
    size_t candidate;
    unsigned scans = 0;

    if (input_bytes - position < 3) {
        return 0;
    }
    candidate = heads[hash_three(input + position)];
    while (candidate != NO_POSITION && position - candidate <= WINDOW_BYTES
           && scans++ < WINDOW_BYTES) {
        size_t count = 0;
        while (count < MAX_MATCH && count < input_bytes - position
               && input[candidate + count] == input[position + count]) {
            count++;
        }
        if (count >= 3 && count > best) {
            best = count;
            *match_position = candidate;
            if (best == MAX_MATCH) {
                break;
            }
        }
        candidate = previous[candidate];
    }
    return best;
}

size_t xdj_lzss_encode(const uint8_t *input, size_t input_bytes,
                       uint8_t *output, size_t output_capacity) {
    size_t *previous = NULL;
    size_t *heads = NULL;
    size_t position = 0;
    size_t output_position = 0;
    size_t index;

    if (input == NULL || output == NULL
        || input_bytes > SIZE_MAX / sizeof(size_t)
        || input_bytes > SIZE_MAX - input_bytes / 8 - 1
        || output_capacity < input_bytes + input_bytes / 8 + 1) {
        return 0;
    }

    previous = malloc((input_bytes ? input_bytes : 1) * sizeof(size_t));
    heads = malloc(HASH_COUNT * sizeof(size_t));
    if (previous == NULL || heads == NULL) {
        free(previous);
        free(heads);
        return 0;
    }
    for (index = 0; index < HASH_COUNT; index++) {
        heads[index] = NO_POSITION;
    }

    while (position < input_bytes) {
        size_t flags_position = output_position++;
        unsigned bit;
        output[flags_position] = 0;

        for (bit = 0; bit < 8 && position < input_bytes; bit++) {
            size_t match_position = 0;
            size_t best;
            size_t token_start = position;

            best = find_match(input, input_bytes, position, heads, previous,
                              &match_position);
            if (best >= 3 && best < MAX_MATCH && input_bytes - position > 1) {
                size_t next_position = 0;
                size_t next = find_match(input, input_bytes, position + 1,
                                         heads, previous, &next_position);
                if (next > best) {
                    best = 0;
                }
            }

            if (best >= 3) {
                size_t ring_position = (INITIAL_CURSOR + match_position)
                                       & (WINDOW_BYTES - 1u);
                output[output_position++] = (uint8_t)ring_position;
                output[output_position++] = (uint8_t)(
                    ((ring_position >> 4) & 0xF0u) | (best - 3u)
                );
                position += best;
            } else {
                output[flags_position] |= (uint8_t)(1u << bit);
                output[output_position++] = input[position++];
            }

            for (index = token_start; index < position; index++) {
                if (input_bytes - index >= 3) {
                    unsigned hash = hash_three(input + index);
                    previous[index] = heads[hash];
                    heads[hash] = index;
                } else {
                    previous[index] = NO_POSITION;
                }
            }
        }
    }

    free(previous);
    free(heads);
    return output_position;
}
