/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * Host-test prelude: make dr_flac take the same code paths as on the SH-4A
 * target, which has a 32-bit bit-reader cache and no architecture-specific
 * assembly. dr_flac selects its 64-bit cache whenever _LP64 or __LP64__ is
 * defined and its ARM or x86 assembly from the CPU macros, so system headers
 * are included first and those macros are removed before dr_flac.h is seen.
 */
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#undef _LP64
#undef __LP64__
#undef __aarch64__
#undef __arm64__
#undef __arm64
#undef __ARM_ARCH
#undef __arm__
#undef __thumb__
#undef __x86_64__
#undef __amd64__
#undef __i386__
#undef __i386
