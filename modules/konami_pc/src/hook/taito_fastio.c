/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  taito_fastio.c -- Taito "Fast I/O" (iDmacDrv) shim scaffold (env-gated, OFF).
 * =============================================================================
 *
 *  Taito Type X3 / X4 carry, in addition to JVS-over-COM, a Taito-proprietary
 *  "Fast I/O" path: the game links iDmacDrv32.dll (32-bit titles) / iDmacDrv64.dll
 *  (64-bit titles) and DMACs blocks of input/output through a custom I/O board.
 *  Public reverse-engineering of the leaked Type X3/X4 titles shows the export
 *  surface is a tiny C API -- open/close the DMA channel, then read/write fixed
 *  buffers that carry the same digital/analog cabinet inputs JVS does.
 *
 *  WHAT THIS FILE HONESTLY IS:
 *    A clean-room SCAFFOLD that, when env-gated on (ULTRA_FAST_IO=1, set by the
 *    board profile for X3/X4), intercepts a game's calls to iDmacDrv*.dll and
 *    services them FROM THE EXISTING JVS CORE -- i.e. the same loader<->hook input
 *    block + clean-room JVS node that already drives Type X / X2. So a Type X3/X4
 *    title that polls Fast-I/O instead of (or as well as) JVS-over-COM still finds
 *    its cabinet inputs.
 *
 *    The PRECISE iDmacDrv export ordinals/signatures + the exact DMA buffer layout
 *    can only be pinned against a real Type X3/X4 dump (each title's I/O board
 *    contract differs slightly). So this scaffold provides the CLEAN-ROOM SEAM:
 *    a named-export shim table + a buffer-fill backed by jvs_core, with the buffer
 *    field offsets exposed as env knobs the user's per-dump profile sets. It ships
 *    NO game data and is OFF unless armed. With no dump it is inert scaffolding.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the public Win32 API + the open JVS
 *  spec + the EXISTING ultra_io_shared input block. No third-party loader source,
 *  no game profile, no copied iDmacDrv binary was read.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "jvs_core.h"

extern void ultra_d3d8_log(const char* fmt, ...);
#define flog ultra_d3d8_log

/* from jvs_serial_hook.c -- the IAT patcher + the input the game should see. */
extern void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);
extern void ultra_hook_current_input(ultra_jvs_input* out);

static int g_enabled = 0;

/* The Fast-I/O DMA buffer the game reads is, on the public titles, a small fixed
 * struct of digital switch words + analog channel words. We expose a synthetic
 * read buffer and fill it from the live JVS input. The exact layout differs per
 * title; the user's per-dump profile sets the field offsets via env so we never
 * bake a single title's layout in. Defaults match the common JVS-shaped layout:
 *   [0..1] = player-1 switch word (byte1<<8 | byte2)
 *   [2..3] = player-2 switch word
 *   [4..5] = system/coin word
 *   [6..]  = analog channels, 2 bytes each, 16-bit left-justified (0x8000 center)
 */
#define FASTIO_BUF_BYTES 64
static int g_off_p1     = 0;
static int g_off_p2     = 2;
static int g_off_system = 4;
static int g_off_analog = 8;
static int g_analog_n   = 8;

static void load_offsets(void) {
    const char* e;
    if ((e = getenv("ULTRA_FASTIO_OFF_P1"))     && *e) g_off_p1     = atoi(e);
    if ((e = getenv("ULTRA_FASTIO_OFF_P2"))     && *e) g_off_p2     = atoi(e);
    if ((e = getenv("ULTRA_FASTIO_OFF_SYSTEM")) && *e) g_off_system = atoi(e);
    if ((e = getenv("ULTRA_FASTIO_OFF_ANALOG")) && *e) g_off_analog = atoi(e);
    if ((e = getenv("ULTRA_FASTIO_ANALOG_N"))   && *e) { int v = atoi(e); if (v >= 0 && v <= 16) g_analog_n = v; }
}

/* Fill a caller buffer with the current cabinet input in the Fast-I/O layout. */
static void fastio_fill(void* buf, int cap) {
    if (!buf || cap <= 0) return;
    unsigned char* b = (unsigned char*)buf;
    if (cap > FASTIO_BUF_BYTES) cap = FASTIO_BUF_BYTES;
    memset(b, 0, cap);

    ultra_jvs_input in;
    ultra_hook_current_input(&in);

    #define PUT16(off, val) do { int o=(off); if (o>=0 && o+1 < cap) { \
        b[o] = (unsigned char)((val) >> 8); b[o+1] = (unsigned char)((val) & 0xFF); } } while (0)
    PUT16(g_off_p1,     in.player[0]);
    if (JVS_MAX_PLAYERS > 1) PUT16(g_off_p2, in.player[1]);
    PUT16(g_off_system, (unsigned)in.system << 8);
    for (int a = 0; a < g_analog_n && a < JVS_MAX_ANALOG; ++a)
        PUT16(g_off_analog + a*2, in.analog[a]);
    #undef PUT16
}

/* ---------------------------------------------------------------------------
 * The iDmacDrv export shims. The leaked Type X3/X4 titles import a small named
 * C API; the common shape is: an Open returning a status/handle, an Update/poll
 * that DMACs a buffer in, and a Close. We provide named shims that return success
 * and, on the read/update path, fill the caller's buffer from the JVS input.
 *
 * Because the EXACT ordinals/signatures are title-specific (and only knowable
 * from a dump), these are the clean-room SEAM: named exports the IAT patcher can
 * point a game's iDmacDrv import slot at. Calling-convention is __stdcall (the
 * Win32 driver-DLL norm); a 64-bit build ignores the decoration. Each returns the
 * common "0 = success" Taito status. Signatures use generic pointer/length args so
 * a title whose prototype differs still gets a safe success + a filled buffer.
 * ------------------------------------------------------------------------- */
int WINAPI Shim_iDmacDrvOpen(int channel) {
    (void)channel;
    flog("[fastio] iDmacDrvOpen -> synthetic channel (JVS-backed)\n");
    return 0;   /* success */
}
int WINAPI Shim_iDmacDrvClose(int channel) { (void)channel; return 0; }

/* Update/poll: the game hands a buffer (and usually a length) it expects the I/O
 * board to fill. We fill it from the live cabinet input. Tolerant of the common
 * 2-arg (buf,len) and 3-arg (chan,buf,len) shapes via a single widest prototype;
 * unused args are simply ignored by the ABI on the caller side. */
int WINAPI Shim_iDmacDrvUpdate(int channel, void* buf, unsigned int len) {
    (void)channel;
    fastio_fill(buf, (int)len);
    return 0;
}
/* Output/rumble/lamp write path: accept + ignore (no cabinet lamps to drive). */
int WINAPI Shim_iDmacDrvWrite(int channel, const void* buf, unsigned int len) {
    (void)channel; (void)buf; (void)len; return 0;
}

/* ---------------------------------------------------------------------------
 * Install. Env-gated (ULTRA_FAST_IO=1, set by the X3/X4 board profile). Patches
 * any iDmacDrv32/64 import the game has to our shims. Idempotent + a no-op when
 * the game does not import iDmacDrv (so a plain Type X/X2 run is byte-identical).
 * ------------------------------------------------------------------------- */
void ultra_taito_fastio_install(void) {
    const char* e = getenv("ULTRA_FAST_IO");
    g_enabled = e && e[0] == '1';
    if (!g_enabled) return;
    load_offsets();

    const char* dlls[] = { "iDmacDrv32.dll", "iDmacDrv64.dll", "Idmacdrv32.dll", "iDmacDrv.dll", NULL };
    /* The leaked titles use these export names (and/or ordinals). We point every
     * name we know at the matching shim; the IAT patch is a no-op for names the
     * game does not import, so over-listing is harmless. */
    struct { const char* fn; void* shim; } X[] = {
        { "iDmacDrvOpen",   (void*)Shim_iDmacDrvOpen   },
        { "iDmacDrvClose",  (void*)Shim_iDmacDrvClose  },
        { "iDmacDrvUpdate", (void*)Shim_iDmacDrvUpdate },
        { "iDmacDrvWrite",  (void*)Shim_iDmacDrvWrite  },
        /* common alternate spellings seen across X3/X4 revisions */
        { "iDmacDrvRead",   (void*)Shim_iDmacDrvUpdate },
        { "iDmacDrvExec",   (void*)Shim_iDmacDrvUpdate },
    };
    for (int d = 0; dlls[d]; ++d)
        for (size_t i = 0; i < sizeof(X)/sizeof(X[0]); ++i)
            ultra_iat_patch_all(dlls[d], X[i].fn, X[i].shim);

    flog("[fastio] Taito Fast-I/O (iDmacDrv) shim armed -- JVS-backed buffers "
         "(p1@%d p2@%d sys@%d analog@%d x%d). NOTE: exact buffer layout is "
         "title-specific; tune via ULTRA_FASTIO_OFF_*.\n",
         g_off_p1, g_off_p2, g_off_system, g_off_analog, g_analog_n);
}

/* Test hook: lets the structural test exercise the buffer-fill without a game. */
void ultra_taito_fastio_test_fill(void* buf, int cap) { fastio_fill(buf, cap); }
int  ultra_taito_fastio_enabled(void) { return g_enabled; }

#endif /* _WIN32 */
