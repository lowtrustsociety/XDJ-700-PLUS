/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * Host runner for the panel guard (src/panel_guard.c) and the version
 * label (src/version_label.c; in test builds it carries the counts).
 * Run by tools/check_panel_guard.py, which reads what it prints.
 *
 * A pretend memory: the exchange's byte, the exchange in use, the port line,
 * the player's object and call count, the port's status and line status
 * registers, and the two channels' control registers. Each step is one
 * millisecond: the stock timer handler is counted, then the guard looks.
 *
 * The exchange normally runs one frame every 3 ms: the byte 1 for the first
 * of them, the call count up by one at the third. A scene name chooses what
 * goes wrong at 60 s:
 *   normal     nothing;
 *   stuck      the byte stays 1 and the count stands, the port's overrun set
 *              (the stall all six records showed), until the guard clears it;
 *   nooverrun  the same without the overrun flag;
 *   off        the same, but the exchange in use is 0;
 *   moving     the byte stays 1 but the count moves on (the player woken);
 *   late       the byte stays 1 and the count stands for 150 ms, then the
 *              exchange ends by itself;
 *   hard       as stuck, but the byte is set back to 1 at once every time;
 *   early      as stuck, from 5 s after power-on (before the guard arms);
 *   noobject   as stuck, with the player's object not in main memory.
 * It prints "restart <ms> <fast> <slow>" at every restart it sees, then
 * "end ticks=<n> fast=<f> slow=<s> longest=<l> busy=<b> port=<p> lsr=<x>
 * fsr=<y> rx=<chcr> tx=<chcr>" and "label <text>".
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "panel_guard.h"

const char* xdj_version_label(void);

#define BUSY 0x0B0E46F8u
#define ON 0x0B0E4B2Du
#define PORT 0xA4050132u
#define PLAYER 0x09D24360u
#define OBJECT 0x09D28B04u
#define FSR 0xFFE20010u
#define LSR 0xFFE20024u
#define RX_CHCR 0xFE00803Cu
#define TX_CHCR 0xFE00804Cu

static struct {
    uint8_t busy, on, port;
    uint32_t player, calls;
    uint16_t fsr, lsr;
    uint32_t rx, tx;
} hw;

static uint32_t stock_ticks;

static uint32_t *word(uint32_t address)
{
    if (address == PLAYER) {
        return &hw.player;
    }
    if (address == OBJECT + 0xD0u) {
        return &hw.calls;
    }
    if (address == RX_CHCR) {
        return &hw.rx;
    }
    if (address == TX_CHCR) {
        return &hw.tx;
    }
    fprintf(stderr, "unexpected 32-bit access at %08x\n", address);
    return NULL;
}

uint32_t xdj_host_peek32(uint32_t address)
{
    uint32_t *at = word(address);

    return at != NULL ? *at : 0xDEADBEEFu;
}

void xdj_host_poke32(uint32_t address, uint32_t value)
{
    uint32_t *at = word(address);

    if (at != NULL) {
        *at = value;
    }
}

uint16_t xdj_host_peek16(uint32_t address)
{
    if (address == FSR) {
        return hw.fsr;
    }
    if (address == LSR) {
        return hw.lsr;
    }
    fprintf(stderr, "unexpected 16-bit read at %08x\n", address);
    return 0xFFFFu;
}

void xdj_host_poke16(uint32_t address, uint16_t value)
{
    if (address == FSR) {
        hw.fsr = value;
    } else if (address == LSR) {
        hw.lsr = value;
    } else {
        fprintf(stderr, "unexpected 16-bit write at %08x\n", address);
    }
}

uint8_t xdj_host_peek8(uint32_t address)
{
    if (address == BUSY) {
        return hw.busy;
    }
    if (address == ON) {
        return hw.on;
    }
    if (address == PORT) {
        return hw.port;
    }
    fprintf(stderr, "unexpected 8-bit read at %08x\n", address);
    return 0xFFu;
}

void xdj_host_poke8(uint32_t address, uint8_t value)
{
    if (address == BUSY) {
        hw.busy = value;
    } else if (address == PORT) {
        hw.port = value;
    } else {
        fprintf(stderr, "unexpected 8-bit write at %08x\n", address);
    }
}

void xdj_host_stock_timer(void)
{
    stock_ticks++;
}

int main(int argc, char **argv)
{
    const char *scene = argc > 1 ? argv[1] : "normal";
    uint32_t start = strcmp(scene, "early") == 0 ? 5000u : 60000u;
    uint32_t ms;
    uint16_t fast, slow, longest, last_fast = 0u, last_slow = 0u;
    int stuck = 0;
    int stalled_once = 0;

    hw.on = 1u;
    hw.port = 0xFEu;
    hw.player = strcmp(scene, "noobject") == 0 ? 0x00001234u : OBJECT;
    hw.rx = 0x40004805u;
    hw.tx = 0x40001800u;
    hw.fsr = 0x0060u;

    /* The image's value: a counted label must read 0/0/0 before the first tick. */
    printf("label %s\n", xdj_version_label());
    for (ms = 0; ms < 120000u; ms++) {
        int special = strcmp(scene, "normal") != 0 && ms >= start && !stalled_once;

        if (special && !stuck) {
            stuck = 1;
            hw.busy = 1u;
            hw.port &= 0xFEu;
            hw.rx = 0x40004805u;
            hw.tx = 0x40001800u;
            if (strcmp(scene, "nooverrun") != 0 && strcmp(scene, "late") != 0 && strcmp(scene, "moving") != 0) {
                hw.lsr = 1u;
            }
            if (strcmp(scene, "off") == 0) {
                hw.on = 0u;
            }
        }
        if (stuck) {
            if (strcmp(scene, "moving") == 0) {
                hw.calls++;
            }
            if (strcmp(scene, "late") == 0 && ms >= start + 150u) {
                /* "done" came after all: the byte 0, the player woken. */
                hw.busy = 0u;
                hw.calls++;
                stuck = 0;
                stalled_once = 1;
            }
            if (strcmp(scene, "hard") == 0 && hw.busy == 0u) {
                hw.busy = 1u;
                hw.lsr = 1u;
            }
            if (hw.busy == 0u && strcmp(scene, "hard") != 0) {
                stuck = 0;
                stalled_once = 1;
            }
        }
        if (!stuck) {
            /* Normal running: one frame every 3 ms. */
            hw.busy = ms % 3u == 0u ? 1u : 0u;
            hw.port = (uint8_t)(hw.busy ? hw.port & 0xFEu : hw.port | 1u);
            if (ms % 3u == 2u) {
                hw.calls++;
            }
        }
        xdj_panel_guard_tick();
        xdj_panel_guard_counts(&fast, &slow, &longest);
        if (fast != last_fast || slow != last_slow) {
            printf("restart %u %u %u\n", ms, fast, slow);
            last_fast = fast;
            last_slow = slow;
        }
        if (strcmp(scene, "hard") == 0 && ms >= start + 3000u) {
            break;
        }
    }
    xdj_panel_guard_counts(&fast, &slow, &longest);
    printf("end ticks=%u fast=%u slow=%u longest=%u busy=%u port=%u lsr=%u fsr=%u rx=%08x tx=%08x\n", stock_ticks,
           fast, slow, longest, hw.busy, hw.port & 1u, hw.lsr, hw.fsr, hw.rx, hw.tx);
    printf("label %s\n", xdj_version_label());
    return 0;
}
