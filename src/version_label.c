/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * What VERSION No. in UTILITY shows (build flag --version-label TEXT).
 *
 * The firmware keeps its version as one text field ("1.15"), read by fourteen places: the update check turns it into a number,
 * the player copies it into what it tells other devices and into every log
 * line. Those must keep seeing "1.15". Only the two routines that put it on
 * the screen (the start-up screen, 0x08DD2E74, and UTILITY's VERSION No.,
 * 0x08DD18E0, which shows "Ver " and the text) are pointed at this one
 * instead, through the address each keeps of the version routine; the build
 * checks those addresses are the stock ones first.
 */

#ifndef XDJ_VERSION_LABEL
#error "XDJ_VERSION_LABEL must be given by the build"
#endif

static const char xdj_version_label_text[] = XDJ_VERSION_LABEL;

const char* xdj_version_label(void)
{
    return xdj_version_label_text;
}
