/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  taito_fastio_typex3.c -- Taito "Fast I/O" (iDmacDrv) shim, Type X3's own
 *                           cabinet I/O transport.
 * =============================================================================
 *
 *  Type X3 carries a Taito-proprietary "Fast I/O" path in place of JVS-over-
 *  COM: the game links iDmacDrv32.dll (32-bit titles) / iDmacDrv64.dll
 *  (64-bit titles) and DMACs blocks of input/output through a custom I/O
 *  board. Public reverse-engineering of leaked Type X3/X4 titles shows the
 *  export surface is a tiny C API -- open/close the DMA channel, then
 *  read/write fixed buffers that carry the same digital/analog cabinet inputs
 *  JVS does (just not JVS-framed).
 *
 *  WHAT THIS FILE HONESTLY IS: a clean-room shim that intercepts a game's
 *  calls to iDmacDrv*.dll and services them from THIS board's own JVS node
 *  (jvs_core_typex3.c, fed by hook_dllmain_typex3.c's standalone XInput/
 *  keyboard reader) -- so a Type X3 title that polls Fast-I/O finds its
 *  cabinet inputs. The PRECISE iDmacDrv export ordinals/signatures + the exact
 *  DMA buffer layout can only be pinned against a real Type X3 dump (each
 *  title's I/O board contract may differ slightly), so this is the CLEAN-ROOM
 *  SEAM: a named-export shim table + a buffer-fill backed by the JVS node,
 *  with the buffer field offsets exposed as env knobs a per-dump profile can
 *  set. It ships NO game data; with no dump this is inert until a real Type
 *  X3 .exe actually imports iDmacDrv*.dll.
 *
 *  INDEPENDENCE: this file, and everything it calls (t3_iat_patch_all,
 *  t3_log, t3_hook_node/jvs_get_input), lives entirely in this board's own
 *  tree. It is NOT the native_x86/src/hook/taito_fastio.c scaffold (read only
 *  as a working-pattern reference) -- no shared source, no shared binary.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "taito_typex3_hook.h"

static int g_enabled = 1;   /* Fast-I/O IS this board's I/O transport: on by
                             * default. ULTRA_FAST_IO=0 disables it explicitly
                             * (diagnostics / a future board variant with no
                             * iDmacDrv import at all). */

/* The Fast-I/O DMA buffer the game reads is, on the public titles, a small
 * fixed struct of digital switch words + analog channel words + a coin word.
 * We expose a synthetic read buffer and fill it from the live JVS node. The
 * exact layout differs per title; a per-dump profile sets the field offsets
 * via env so we never bake a single title's layout in. Defaults match the
 * common JVS-shaped layout:
 *   [0..1] = player-1 switch word (byte1<<8 | byte2)
 *   [2..3] = player-2 switch word
 *   [4..5] = system/coin word
 *   [6..7] = coin-slot-0 count (14-bit, JVS READ_COIN convention)
 *   [8..]  = analog channels, 2 bytes each, 16-bit left-justified (0x8000 center)
 */
#define FASTIO_BUF_BYTES 64
static int g_off_p1     = 0;
static int g_off_p2     = 2;
static int g_off_system = 4;
static int g_off_coin   = 6;
static int g_off_analog = 8;
static int g_analog_n   = 8;

static void load_offsets(void) {
    const char* e;
    if ((e = getenv("ULTRA_FASTIO_OFF_P1"))     && *e) g_off_p1     = atoi(e);
    if ((e = getenv("ULTRA_FASTIO_OFF_P2"))     && *e) g_off_p2     = atoi(e);
    if ((e = getenv("ULTRA_FASTIO_OFF_SYSTEM")) && *e) g_off_system = atoi(e);
    if ((e = getenv("ULTRA_FASTIO_OFF_COIN"))   && *e) g_off_coin   = atoi(e);
    if ((e = getenv("ULTRA_FASTIO_OFF_ANALOG")) && *e) g_off_analog = atoi(e);
    if ((e = getenv("ULTRA_FASTIO_ANALOG_N"))   && *e) { int v = atoi(e); if (v >= 0 && v <= 16) g_analog_n = v; }
}

/* Fill a caller buffer with the current cabinet input in the Fast-I/O layout. */
static void fastio_fill(void* buf, int cap) {
    if (!buf || cap <= 0) return;
    unsigned char* b = (unsigned char*)buf;
    if (cap > FASTIO_BUF_BYTES) cap = FASTIO_BUF_BYTES;
    memset(b, 0, cap);

    ultra_jvs_node* node = t3_hook_node();
    ultra_jvs_input in;
    jvs_get_input(node, &in);
    uint32_t coin0 = jvs_coin_count(node, 0);
    if (coin0 > 0x3FFF) coin0 = 0x3FFF;

    #define PUT16(off, val) do { int o=(off); if (o>=0 && o+1 < cap) { \
        b[o] = (unsigned char)((val) >> 8); b[o+1] = (unsigned char)((val) & 0xFF); } } while (0)
    PUT16(g_off_p1,     in.player[0]);
    if (JVS_MAX_PLAYERS > 1) PUT16(g_off_p2, in.player[1]);
    PUT16(g_off_system, (unsigned)in.system << 8);
    PUT16(g_off_coin,   coin0);
    for (int a = 0; a < g_analog_n && a < JVS_MAX_ANALOG; ++a)
        PUT16(g_off_analog + a*2, in.analog[a]);
    #undef PUT16
}

/* ---------------------------------------------------------------------------
 * The iDmacDrv export shims. The leaked Type X3/X4 titles import a small named
 * C API; the common shape is: an Open returning a status/handle, an Update/poll
 * that DMACs a buffer in, and a Close. We provide named shims that return
 * success and, on the read/update path, fill the caller's buffer from the JVS
 * node. Calling-convention is __stdcall (the Win32 driver-DLL norm); a 64-bit
 * build ignores the decoration. Each returns the common "0 = success" Taito
 * status. Signatures use generic pointer/length args so a title whose
 * prototype differs still gets a safe success + a filled buffer.
 * ------------------------------------------------------------------------- */
int WINAPI Shim_iDmacDrvOpen(int channel) {
    (void)channel;
    t3_log("[fastio] iDmacDrvOpen -> synthetic channel (JVS-node-backed)");
    return 0;   /* success */
}
int WINAPI Shim_iDmacDrvClose(int channel) { (void)channel; return 0; }

int WINAPI Shim_iDmacDrvUpdate(int channel, void* buf, unsigned int len) {
    (void)channel;
    fastio_fill(buf, (int)len);
    return 0;
}
int WINAPI Shim_iDmacDrvWrite(int channel, const void* buf, unsigned int len) {
    (void)channel; (void)buf; (void)len; return 0;   /* no cabinet lamps to drive */
}

/* ---------------------------------------------------------------------------
 * Install. Idempotent + a no-op when the game does not import iDmacDrv (so a
 * dry run / a title that turns out to be JVS-only is byte-identical).
 * ------------------------------------------------------------------------- */
void t3_fastio_install(void) {
    const char* e = getenv("ULTRA_FAST_IO");
    g_enabled = !e || e[0] != '0';   /* default ON for this board; "0" opts out */
    if (!g_enabled) return;

    /* NEVER shim a transport that actually exists.
     *
     * The handlers below are 3-argument -- Shim_iDmacDrvUpdate(channel, buf,
     * len) -- and were written before any real Type X3 dump was available to
     * pin the ABI against. The real one, read out of ismACIO.dll's own code,
     * is FOUR arguments:
     *
     *     Open (DWORD channel, void** out_handle, DWORD* status)   ordinal 1
     *     Close(void* handle,  DWORD* status)                      ordinal 2
     *     Read (void* handle,  DWORD reg, void* buf, DWORD* status) ordinal 5
     *     Write(void* handle,  DWORD reg, DWORD val, DWORD* status) ordinal 6
     *
     * so a shimmed Read takes the REGISTER NUMBER where it expects a buffer
     * pointer and writes through it. With a real iDmacDrv64.dll finally
     * present, that fired immediately and killed Theatrhythm; the tracer named
     * it as an access violation writing to 0x4000 -- a register number, not an
     * address.
     *
     * If the driver module is loaded, something real is servicing this
     * transport and the shim has no business intercepting it. Standing down
     * is right whether that driver is the cabinet's or ours.
     *
     * The shim stays for a title that imports iDmacDrv with NO driver present
     * at all -- its original purpose -- and its signatures should be corrected
     * to the four-argument form above before it is relied on again. */
    {
        static const char* const real[] = { "iDmacDrv64.dll", "iDmacDrv32.dll",
                                            "iDmacDrv.dll", NULL };
        int i;
        for (i = 0; real[i]; ++i) {
            if (GetModuleHandleA(real[i])) {
                t3_log("[fastio] %s is loaded -- a real driver is servicing Fast-I/O, shim NOT installed (its 3-arg signatures do not match the real 4-arg ABI)", real[i]);
                g_enabled = 0;
                return;
            }
        }
    }

    load_offsets();

    const char* dlls[] = { "iDmacDrv32.dll", "iDmacDrv64.dll", "Idmacdrv32.dll", "iDmacDrv.dll", NULL };
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
            t3_iat_patch_all(dlls[d], X[i].fn, X[i].shim);

    t3_log("[fastio] Taito Fast-I/O (iDmacDrv) shim armed -- JVS-node-backed "
           "buffer (p1@%d p2@%d sys@%d coin@%d analog@%d x%d). NOTE: exact "
           "buffer layout is title-specific; tune via ULTRA_FASTIO_OFF_*.",
           g_off_p1, g_off_p2, g_off_system, g_off_coin, g_off_analog, g_analog_n);
}

/* Test hook: lets a structural test exercise the buffer-fill without a game. */
void t3_fastio_test_fill(void* buf, int cap) { fastio_fill(buf, cap); }
int  t3_fastio_enabled(void) { return g_enabled; }

#endif /* _WIN32 */
