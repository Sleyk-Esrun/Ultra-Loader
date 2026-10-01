/*
 * lpt_shim.c
 * Parallel-port (LPT) I/O shim - Raw Thrills UHB emulation for Cruis'n Blast.
 *
 * See lpt_shim.h for design rationale. In short: Cruis'n Blast issues
 * `iopl(3)` (stubbed → 0 in stubs_cxx.c) then executes raw `IN AL,DX` /
 * `OUT DX,AL` on the LPT base register. Those ring-3 port instructions
 * raise EXCEPTION_PRIV_INSTRUCTION under Windows, which lindbergh_boards.c
 * already traps in `portio_veh`. This file provides the LPT-side responder;
 * portio_veh consults `lpt_shim_is_lpt_port` and, on match, routes the
 * read/write here instead of to the security board.
 *
 * The LPT register file is 3 bytes wide relative to the base:
 *
 *     base + 0   Data     (write == set output pins D0..D7)
 *     base + 1   Status   (read  == coin/start/service/test switches; bit 7
 *                          on the wire is inverted - the shim inverts on
 *                          READ so callers get an active-high 1==pressed
 *                          input, matching how ButtonInput() reads it)
 *     base + 2   Control  (write == coin lockout, ticket dispense, lamps)
 *
 * The Raw Thrills UHB uses this layout (see `docs/X86_BOARDS.md`), so an
 * accurate byte-level responder here is enough for the RIO2 wheel/pedal
 * path (which is separate, over FTDI USB) plus the operator inputs the
 * game polls via LPT.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include "loader.h"
#include "lpt_shim.h"

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static uint16_t g_lpt_base    = LPT_BASE_LPT1;   /* 0x378 by default */
static uint8_t  g_data_reg    = 0x00;             /* last write to base+0 */
static uint8_t  g_control_reg = 0x00;             /* last write to base+2 */
static uint8_t  g_buttons     = 0x00;             /* active-high switches */

static unsigned g_stats_reads  = 0;
static unsigned g_stats_writes = 0;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

uint16_t lpt_shim_configure(uint16_t base)
{
    if (base == 0) base = LPT_BASE_LPT1;
    g_lpt_base = base;
    loader_log(LOG_INFO, "[LPT] configured base = 0x%03X", g_lpt_base);
    return g_lpt_base;
}

bool lpt_shim_parse_argv(const char *arg, uint16_t *out_base)
{
    if (!arg || !out_base) return false;
    /* Accept "-lpt378", "-lpt=0x278", "-lpt 0x378", "--lpt=378". Numeric
     * portion is always parsed as hex (Raw Thrills convention). */
    const char *p = arg;
    if (*p != '-') return false;
    if (*p == '-') p++;
    if (*p == '-') p++;
    if (strncmp(p, "lpt", 3) != 0) return false;
    p += 3;
    if (*p == '=' || *p == ' ') p++;
    if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    if (!*p) return false;

    unsigned base = 0;
    while (*p) {
        char c = *p++;
        int d;
        if (c >= '0' && c <= '9')       d = c - '0';
        else if (c >= 'a' && c <= 'f')  d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')  d = c - 'A' + 10;
        else break;
        base = (base << 4) | (unsigned)d;
        if (base > 0xFFFFu) return false;
    }
    if (base == 0) return false;
    *out_base = (uint16_t)base;
    return true;
}

bool lpt_shim_is_lpt_port(uint16_t port)
{
    /* The 3-byte LPT window sits at base .. base+2. Keep the check tight to
     * avoid stealing IN/OUT for adjacent security-board ports on titles that
     * share a common address decoder. */
    return (port >= g_lpt_base) && (port <= (uint16_t)(g_lpt_base + 2));
}

uint8_t lpt_shim_read(uint16_t port)
{
    g_stats_reads++;
    unsigned off = (unsigned)(port - g_lpt_base);

    switch (off) {
    case 0: /* Data register - mirrors last write */
        return g_data_reg;

    case 1: {
        /* Status register. Wire convention (Centronics):
         *   bit 7 = /BUSY (inverted on wire)
         *   bit 6 = /ACK
         *   bit 5 = PE  (paper end)
         *   bit 4 = SELECT
         *   bit 3 = /ERROR
         *   bits 0..2 unused (spec) - Raw Thrills co-opts these + the four
         *   lower "data" pins wired back through a latch for its coin/start
         *   inputs. Since the game reads Status and masks a set of bits it
         *   knows correspond to the UHB switch matrix, we hand back the shim
         *   button byte directly - the game's mask filters what it wants.
         *
         * We deliberately keep the four HIGH status pins in their idle state
         * so any BIOS-style probe (which reads status looking for a printer)
         * doesn't misidentify the port as busy. */
        uint8_t v = 0;
        /* Idle-printer pattern: SELECT=1, PE=0, /ACK=1, /BUSY=1 (inverted → 0) */
        v |= 0x40; /* /ACK   idle */
        v |= 0x10; /* SELECT idle */
        /* Overlay the operator switches. Active-high in our API - matches
         * how Raw Thrills' Input.cpp reads the raw byte and tests bits. */
        v |= (uint8_t)(g_buttons & 0x3F);
        return v;
    }

    case 2:
        /* Control register readback - spec says only bits 0..3 are readable
         * and reflect the last write. */
        return (uint8_t)(g_control_reg & 0x0F);

    default:
        return 0xFF;
    }
}

void lpt_shim_write(uint16_t port, uint8_t value)
{
    g_stats_writes++;
    unsigned off = (unsigned)(port - g_lpt_base);

    switch (off) {
    case 0:
        g_data_reg = value;
        break;
    case 2: {
        uint8_t prev = g_control_reg;
        g_control_reg = value;
        /* Bit 0 = COIN LOCKOUT engage (active high in UHB firmware).
         * Bit 1 = COIN METER pulse (edge-triggered).
         * Bit 2 = TICKET DISPENSE (unused in Cruis'n Blast - no ticket).
         * Bit 3 = MARQUEE LAMP.
         *
         * We only log transitions so the boot console can confirm the game
         * is driving the outputs. Nothing physical needs to happen; the
         * inputs the game reads back are already synthesised. */
        uint8_t rising = (uint8_t)(value & ~prev);
        if (rising & 0x02)
            loader_log(LOG_DEBUG, "[LPT] coin meter pulse");
        if ((value ^ prev) & 0x08)
            loader_log(LOG_DEBUG, "[LPT] marquee lamp %s",
                       (value & 0x08) ? "on" : "off");
        break;
    }
    default:
        /* Writes to Status are ignored on real hardware. */
        break;
    }
}

void lpt_shim_set_button(unsigned mask, bool pressed)
{
    if (pressed) g_buttons |= (uint8_t)(mask & 0xFF);
    else         g_buttons &= (uint8_t)~(mask & 0xFF);
}

uint8_t lpt_shim_get_buttons(void)
{
    return g_buttons;
}

unsigned lpt_shim_stats_reads(void)  { return g_stats_reads;  }
unsigned lpt_shim_stats_writes(void) { return g_stats_writes; }
