/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#ifndef XDJ_PANEL_GUARD_H
#define XDJ_PANEL_GUARD_H

#include <stdint.h>

/* Called in place of the stock 1 ms timer handler, which it calls first. */
void xdj_panel_guard_tick(void);

/* Restarts by the 200 ms rule and by the 2 s rule, and the longest stand that ended by itself (ms). */
void xdj_panel_guard_counts(uint16_t *fast, uint16_t *slow, uint16_t *longest);

#endif
