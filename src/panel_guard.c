/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * The panel guard (build flag --panel-guard, part of the release).
 *
 * The player and its front panel swap 32 bytes at a time over a serial port
 * (SCIF2, 0xFFE20000) and two channels of the transfer controller. The
 * sender sets the byte 0x0B0E46F8 to 1 when it starts an exchange; the
 * "done" interrupt (0x08CEAFA4) sets it back to 0, and the sender starts the
 * next exchange only then. Now and then the port's receive buffer overflows
 * at the start of an exchange (most likely while a USB transfer holds the
 * transfer controller), the port stops receiving, "done" never comes, and
 * the byte stays 1: no more frames from the panel, the controls and the
 * screen stand still while the music plays on. The stock code never clears
 * it once the panel runs.
 *
 * The guard runs in the player's 1 ms timer interrupt: its handler's address
 * in the stock definition (0x080B8708) points here, and this calls the stock
 * handler (0x08D632C0) first. Then it looks: the byte 1, the exchange in use
 * (0x0B0E4B2D not 0) and the player's call count (the status routine's,
 * object [0x09D24360] + 0xD0) standing still, every millisecond in a row.
 * After 200 ms of that with the port's overrun flag set (the stall all six
 * records showed), or after 2 s without it (a stall never seen; the slow
 * rule of the restart build), it does what "done" would have done to the
 * hardware: the port's error flags cleared, both channels off, the port line
 * up again, and the byte 0. The sender then starts the next exchange as
 * always. One frame is lost; the sound is not touched.
 *
 * Nothing runs in the first 20 s after power-on (the stock start-up test of
 * the panel has its own recovery). Interrupt context: no kernel calls, a few
 * reads and writes. The tail's zero area is not cleared at boot, so the
 * state starts from a value in the image and is cleared at the first tick.
 */

#include <stdint.h>

#include "panel_guard.h"

#ifdef XDJ_HOST_TEST
uint32_t xdj_host_peek32(uint32_t address);
uint16_t xdj_host_peek16(uint32_t address);
uint8_t xdj_host_peek8(uint32_t address);
void xdj_host_poke8(uint32_t address, uint8_t value);
void xdj_host_poke16(uint32_t address, uint16_t value);
void xdj_host_poke32(uint32_t address, uint32_t value);
void xdj_host_stock_timer(void);
#define XDJ_PEEK32(address) xdj_host_peek32(address)
#define XDJ_PEEK16(address) xdj_host_peek16(address)
#define XDJ_PEEK8(address) xdj_host_peek8(address)
#define XDJ_POKE8(address, value) xdj_host_poke8(address, value)
#define XDJ_POKE16(address, value) xdj_host_poke16(address, value)
#define XDJ_POKE32(address, value) xdj_host_poke32(address, value)
#define XDJ_PG_STOCK_TIMER() xdj_host_stock_timer()
#else
#define XDJ_PEEK32(address) (*(volatile const uint32_t *)(uintptr_t)(address))
#define XDJ_PEEK16(address) (*(volatile const uint16_t *)(uintptr_t)(address))
#define XDJ_PEEK8(address) (*(volatile const uint8_t *)(uintptr_t)(address))
#define XDJ_POKE8(address, value) (*(volatile uint8_t *)(uintptr_t)(address) = (value))
#define XDJ_POKE16(address, value) (*(volatile uint16_t *)(uintptr_t)(address) = (value))
#define XDJ_POKE32(address, value) (*(volatile uint32_t *)(uintptr_t)(address) = (value))
/* The stock 1 ms timer handler: clears its flag, counts, runs the player's timer work. */
#define XDJ_PG_STOCK_TIMER() ((void (*)(void))0x08D632C0u)()
#endif

#define XDJ_PG_BUSY 0x0B0E46F8u      /* 1 from an exchange's start to its "done" */
#define XDJ_PG_ON 0x0B0E4B2Du        /* the exchange in use; "done" acts only when not 0 */
#define XDJ_PG_PORT 0xA4050132u      /* the line the start clears and "done" sets (bit 0) */
#define XDJ_PG_PLAYER 0x09D24360u    /* the player's object; its call count at +0xD0 */
#define XDJ_PG_SCIF 0xFFE20000u      /* SCIF2 */
#define XDJ_PG_SCIF_STATUS 0x10u     /* SCFSR */
#define XDJ_PG_SCIF_LINE 0x24u       /* SCLSR: bit 0 the overrun */
#define XDJ_PG_RECEIVE 0xFE008030u   /* channel 1: SAR, DAR, TCR, CHCR */
#define XDJ_PG_SEND 0xFE008040u      /* channel 2 */

#define XDJ_PG_FRESH 0x5Au
#define XDJ_PG_ARM_MS 20000u         /* after power-on */
#define XDJ_PG_FAST_MS 200u          /* standing, with the overrun */
#define XDJ_PG_SLOW_MS 2000u         /* standing, without it */
#define XDJ_PG_MOST 0xFFFFu

static struct {
    uint8_t fresh;
    uint8_t armed;
    uint16_t standing;   /* milliseconds in a row the exchange has stood */
    uint16_t up;         /* milliseconds since power-on, until armed */
    uint16_t fast;       /* restarts by the 200 ms rule */
    uint16_t slow;       /* restarts by the 2 s rule */
    uint16_t longest;    /* the longest stand that ended by itself, ms */
    uint32_t calls;
} xdj_pg = { .fresh = XDJ_PG_FRESH };

static uint32_t xdj_pg_calls(void)
{
    uint32_t object = XDJ_PEEK32(XDJ_PG_PLAYER);

    /* Main memory only: 0x08000000 to 0x0FFFFFFF. */
    if ((object & 0xF8000000u) != 0x08000000u || (object & 3u) != 0u) {
        return 0u;
    }
    return XDJ_PEEK32(object + 0xD0u);
}

/* A channel off as the "done" interrupt does it: its interrupt, then the channel, then its end flag. */
static void xdj_pg_channel_off(uint32_t channel)
{
    XDJ_POKE32(channel + 12u, XDJ_PEEK32(channel + 12u) & ~4u);
    XDJ_POKE32(channel + 12u, XDJ_PEEK32(channel + 12u) & ~1u);
    XDJ_POKE32(channel + 12u, XDJ_PEEK32(channel + 12u) & ~2u);
}

static void xdj_pg_restart(uint8_t on)
{
    uint16_t status;

    /* The port's flags, each cleared only when read set (overrun; error, break, receive). */
    status = XDJ_PEEK16(XDJ_PG_SCIF + XDJ_PG_SCIF_LINE);
    if ((status & 1u) != 0u) {
        XDJ_POKE16(XDJ_PG_SCIF + XDJ_PG_SCIF_LINE, (uint16_t)(status & ~1u));
    }
    status = XDJ_PEEK16(XDJ_PG_SCIF + XDJ_PG_SCIF_STATUS);
    if ((status & 0x93u) != 0u) {
        XDJ_POKE16(XDJ_PG_SCIF + XDJ_PG_SCIF_STATUS, (uint16_t)(status & ~0x93u));
    }
    xdj_pg_channel_off(XDJ_PG_RECEIVE);
    xdj_pg_channel_off(XDJ_PG_SEND);
    if (on == 1u) {
        XDJ_POKE8(XDJ_PG_PORT, (uint8_t)(XDJ_PEEK8(XDJ_PG_PORT) | 1u));
    }
    XDJ_POKE8(XDJ_PG_BUSY, 0u);
}

void xdj_panel_guard_tick(void)
{
    uint32_t calls;
    uint8_t busy;
    uint8_t on;

    XDJ_PG_STOCK_TIMER();
    if (xdj_pg.fresh == XDJ_PG_FRESH) {
        xdj_pg.fresh = 0u;
        xdj_pg.armed = 0u;
        xdj_pg.standing = 0u;
        xdj_pg.up = 0u;
        xdj_pg.fast = 0u;
        xdj_pg.slow = 0u;
        xdj_pg.longest = 0u;
        xdj_pg.calls = 0u;
        return;
    }
    if (!xdj_pg.armed) {
        if (++xdj_pg.up >= XDJ_PG_ARM_MS) {
            xdj_pg.armed = 1u;
        }
        return;
    }
    busy = XDJ_PEEK8(XDJ_PG_BUSY);
    on = XDJ_PEEK8(XDJ_PG_ON);
    calls = xdj_pg_calls();
    if (busy != 1u || on == 0u || calls != xdj_pg.calls) {
        if (xdj_pg.standing > xdj_pg.longest) {
            xdj_pg.longest = xdj_pg.standing;
        }
        xdj_pg.standing = 0u;
        xdj_pg.calls = calls;
        return;
    }
    if (xdj_pg.standing < XDJ_PG_MOST) {
        xdj_pg.standing++;
    }
    if ((XDJ_PEEK16(XDJ_PG_SCIF + XDJ_PG_SCIF_LINE) & 1u) != 0u && xdj_pg.standing >= XDJ_PG_FAST_MS) {
        xdj_pg_restart(on);
        if (xdj_pg.fast < XDJ_PG_MOST) {
            xdj_pg.fast++;
        }
        xdj_pg.standing = 0u;
    } else if (xdj_pg.standing >= XDJ_PG_SLOW_MS) {
        xdj_pg_restart(on);
        if (xdj_pg.slow < XDJ_PG_MOST) {
            xdj_pg.slow++;
        }
        xdj_pg.standing = 0u;
    }
}

void xdj_panel_guard_counts(uint16_t *fast, uint16_t *slow, uint16_t *longest)
{
    int fresh = xdj_pg.fresh == XDJ_PG_FRESH;

    *fast = fresh ? 0u : xdj_pg.fast;
    *slow = fresh ? 0u : xdj_pg.slow;
    *longest = fresh ? 0u : xdj_pg.longest;
}
