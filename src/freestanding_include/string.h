/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#ifndef XDJ_PROBE_STRING_H
#define XDJ_PROBE_STRING_H

#include <stddef.h>

/* Minimal declarations for a freestanding link-size measurement. */
void* memcpy(void* destination, const void* source, size_t size);
void* memset(void* destination, int value, size_t size);

#endif
