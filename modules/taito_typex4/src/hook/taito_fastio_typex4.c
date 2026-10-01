/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  taito_fastio_typex4.c -- Taito "Fast I/O" (iDmacDrv) transport, Type X4's
 *                           OWN independent copy.
 * =============================================================================
 *
 *  Type X4 carries its cabinet inputs primarily over Taito's proprietary
 *  "Fast I/O" path: the game links iDmacDrv64.dll (64-bit titles -- Type X4 is
 *  a 64-bit-only board) and DMACs blocks of input/output through a custom I/O
 *  board. Public reverse-engineering of the leaked Type X3/X4 titles shows the
 *  export surface is a tiny C API -- open/close the DMA channel, then
 *  read/write fixed buffers that carry the same digital/analog cabinet inputs
 *  JVS does.
 *
 *  WHAT THIS FILE HONESTLY IS: a clean-room shim that intercepts a game's
 *  calls to iDmacDrv*.dll and services them from THIS board's own JVS core
 *  (jvs_core_typex4.c) -- so a Type X4 title that polls Fast-I/O finds its
 *  cabinet inputs. The PRECISE iDmacDrv export ordinals/signatures + the exact
 *  DMA buffer layout can only be pinned against a real Type X4 dump (each
 *  title's I/O board contract can differ). This gives the CLEAN-ROOM SEAM: a
 *  named-export shim table + a buffer-fill backed by jvs_core_typex4, with the
 *  buffer field offsets exposed as env knobs a per-dump profile can set. It
 *  ships NO game data and is inert scaffolding until a game actually imports
 *  iDmacDrv*.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the public Win32 API + the open
 *  JVS spec + this board's own hook_dllmain_typex4.c input snapshot. Nothing
 *  here #includes, links, or copies-at-runtime any file from
 *  modules/native_x86/src/hook/ or modtest/.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "jvs_core_typex4.h"
#include "hook_dllmain_typex4.h"

static int g_enabled = 0;

/* The Fast-I/O DMA buffer the game reads is, on the public titles, a small
 * fixed struct of digital switch words + analog channel words. We expose a
 * synthetic read buffer and fill it from the live JVS input. The exact layout
 * differs per title; a per-dump profile sets the field offsets via env so we
 * never bake a single title's layout in. Defaults match the common JVS-shaped
 * layout:
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
    if ((e = getenv("ULTRA_TX4_FASTIO_OFF_P1"))     && *e) g_off_p1     = atoi(e);
    if ((e = getenv("ULTRA_TX4_FASTIO_OFF_P2"))     && *e) g_off_p2     = atoi(e);
    if ((e = getenv("ULTRA_TX4_FASTIO_OFF_SYSTEM")) && *e) g_off_system = atoi(e);
    if ((e = getenv("ULTRA_TX4_FASTIO_OFF_ANALOG")) && *e) g_off_analog = atoi(e);
    if ((e = getenv("ULTRA_TX4_FASTIO_ANALOG_N"))   && *e) { int v = atoi(e); if (v >= 0 && v <= 16) g_analog_n = v; }
}

/* Fill a caller buffer with the current cabinet input in the Fast-I/O layout. */
static void fastio_fill(void* buf, int cap) {
    if (!buf || cap <= 0) return;
    unsigned char* b = (unsigned char*)buf;
    if (cap > FASTIO_BUF_BYTES) cap = FASTIO_BUF_BYTES;
    memset(b, 0, cap);

    ultra_jvs_input_tx4 in;
    ultra_tx4_current_input(&in);

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
 * C API; the common shape is: an Open returning a status/handle, an
 * Update/poll that DMACs a buffer in, and a Close. Calling-convention is
 * __stdcall (the Win32 driver-DLL norm). Each returns the common "0 = success"
 * Taito status.
 * ------------------------------------------------------------------------- */
int WINAPI Shim_TX4_iDmacDrvOpen(int channel) {
    (void)channel;
    ultra_tx4_log("fastio: iDmacDrvOpen -> synthetic channel (JVS-backed)");
    return 0;   /* success */
}
int WINAPI Shim_TX4_iDmacDrvClose(int channel) { (void)channel; return 0; }

int WINAPI Shim_TX4_iDmacDrvUpdate(int channel, void* buf, unsigned int len) {
    (void)channel;
    fastio_fill(buf, (int)len);
    return 0;
}
int WINAPI Shim_TX4_iDmacDrvWrite(int channel, const void* buf, unsigned int len) {
    (void)channel; (void)buf; (void)len; return 0;
}

/* ---------------------------------------------------------------------------
 * Install. Patches any iDmacDrv import the game has to our shims. Idempotent
 * and a no-op when the game does not import iDmacDrv.
 * ------------------------------------------------------------------------- */
void ultra_tx4_fastio_install(void) {
    /* Default-armed for this board (Type X4's own board profile marks
     * BP_IO_FAST_IO as its native transport); ULTRA_TX4_NO_FASTIO=1 disables
     * it for diagnosis. */
    const char* off = getenv("ULTRA_TX4_NO_FASTIO");
    g_enabled = !(off && off[0] == '1');
    if (!g_enabled) { ultra_tx4_log("fastio: disabled via ULTRA_TX4_NO_FASTIO"); return; }
    load_offsets();

    const char* dlls[] = { "iDmacDrv64.dll", "iDmacDrv32.dll", "Idmacdrv64.dll", "iDmacDrv.dll", NULL };
    struct { const char* fn; void* shim; } X[] = {
        { "iDmacDrvOpen",   (void*)Shim_TX4_iDmacDrvOpen   },
        { "iDmacDrvClose",  (void*)Shim_TX4_iDmacDrvClose  },
        { "iDmacDrvUpdate", (void*)Shim_TX4_iDmacDrvUpdate },
        { "iDmacDrvWrite",  (void*)Shim_TX4_iDmacDrvWrite  },
        { "iDmacDrvRead",   (void*)Shim_TX4_iDmacDrvUpdate },
        { "iDmacDrvExec",   (void*)Shim_TX4_iDmacDrvUpdate },
    };
    for (int d = 0; dlls[d]; ++d)
        for (size_t i = 0; i < sizeof(X)/sizeof(X[0]); ++i)
            ultra_tx4_iat_patch_all(dlls[d], X[i].fn, X[i].shim);

    ultra_tx4_log("fastio: Taito Fast-I/O (iDmacDrv) shim armed -- JVS-backed buffers "
                  "(p1@%d p2@%d sys@%d analog@%d x%d). NOTE: exact buffer layout is "
                  "title-specific; tune via ULTRA_TX4_FASTIO_OFF_*.",
                  g_off_p1, g_off_p2, g_off_system, g_off_analog, g_analog_n);
}

int ultra_tx4_fastio_enabled(void) { return g_enabled; }

#endif /* _WIN32 */
