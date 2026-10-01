/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_ringedge.c  --  ultra_sega_ringedge_hook{32,64}.dll entry point.
 * =============================================================================
 *
 *  Injected into the Sega RingEdge game process by sega_ringedge_main.c via a
 *  suspended-launch + QueueUserAPC(LoadLibraryA) (primary) + a fallback
 *  CreateRemoteThread(LoadLibraryA) -- see find_hook()/inject_dll_apc() /
 *  inject_dll_remote_thread() in that file. sega_ringedge_main.c never calls
 *  GetProcAddress for any particular export afterward; DllMain does the work
 *  the instant the DLL is loaded, exactly like every other Ultra Loader hook.
 *
 *  On DLL_PROCESS_ATTACH this file:
 *    - opens the loader's shared-memory input block if the loader created one
 *      (ULTRA_IO_SHM), else falls back to reading a local XInput gamepad so a
 *      standalone/test launch is still controllable end-to-end,
 *    - creates one clean-room JVS node (jvs_core_ringedge.c),
 *    - installs the JVS-serial transport hook (jvs_serial_hook_ringedge.c),
 *      which also arms the D3D9 present hook (d3d9_throttle_ringedge.c),
 *    - installs the presence-only mxparallel keychip shim (env-gated, OFF by
 *      default -- sega_keychip_ringedge.c),
 *    - installs the DirectSound/XAudio2 diagnostic audio hook
 *      (audio_hook_ringedge.c),
 *    - installs the Ultra Omni-Bridge KBM-emulation service (env-gated, OFF by
 *      default -- omni_bridge.c), for the rare raw-keyboard/mouse RingEdge
 *      title once one is confirmed (mirrors taito_typex2's Haunted Museum).
 *
 *  This DLL is THIS BOARD'S OWN independent artifact: it is compiled from
 *  files ONLY in modules/sega_ringedge/src/hook/, and does not #include, link
 *  against, or load at runtime any file or DLL from the native_x86 module or
 *  from modtest/. No game code, no third-party loader code -- Ultra Loader's
 *  own work over the public Win32 API + the open JVS spec.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "jvs_core_ringedge.h"
#include "ultra_io_shared_ringedge.h"
#include "omni_bridge.h"

#include "ultra_transport.h"   /* shared universal I/O transport layer */
/* from the other hook translation units in this board's OWN tree */
int  ultra_ringedge_jvs_serial_install(void);
void ultra_ringedge_sega_keychip_install(void);   /* env-gated (OFF by default) */
void ultra_ringedge_audio_install(void);
void ultra_ringedge_d3d9_teardown(void);          /* pair timeBeginPeriod/timeEndPeriod */
HWND ultra_ringedge_d3d9_window(void);            /* device window getter (d3d9_throttle_ringedge.c) */

/* ---- shared diagnostic log (ULTRA_JVS_LOG), used by every TU in this tree - */
void ultra_ringedge_log(const char* fmt, ...) {
    const char* path = getenv("ULTRA_JVS_LOG");
    if (!path || !*path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fclose(f);
}

/* -------------------------------------------------------------------------- */
static ultra_re_jvs_node* g_node   = NULL;
static HANDLE             g_map    = NULL;
static ultra_io_shared*   g_shared = NULL;
static uint32_t           g_coin_seen[ULTRA_IO_COINS] = {0};
static volatile LONG      g_stop   = 0;

ultra_re_jvs_node* ultra_ringedge_jvs_node(void) {
    return g_node;   /* created by DllMain before any hook can fire */
}

/* ---- standalone gamepad (XInput) fallback ---------------------------------
 * When no loader is feeding the shared block (e.g. a standalone test launch),
 * read the player's gamepad directly so the game is actually controllable --
 * matching the project's per-game-complete rule (render + SOUND + CONTROLS,
 * all three). XInput is loaded dynamically (no hard dependency on a given
 * version of the DLL). */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } re_xpad;
typedef struct { DWORD packet; re_xpad pad; } re_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, re_xstate*);
static p_xinput_get     s_xget = NULL;
static ultra_re_jvs_input g_xinput_in;
static volatile LONG    g_local_coin = 0;
static long             g_local_coin_seen = 0;

/* This board has NO controls-profile system - there is no controls_*.c or
 * input_profiles_*.c here at all. The coin-gate below was pasted in from a
 * board that has one, so this call never even linked and the board has not
 * built since. No profile can be active, so the honest answer is 0 and the
 * legacy coin path keeps ownership, which is the behavior that predates
 * the paste. */
static int ultra_input_map_active(void) { return 0; }

static DWORD WINAPI xinput_thread(LPVOID arg) {
    (void)arg;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
    for (int i = 0; dlls[i] && !s_xget; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) s_xget = (p_xinput_get)(void*)GetProcAddress(h, "XInputGetState");
    }
    if (!s_xget) return 0;
    for (int a = 0; a < JVS_MAX_ANALOG; ++a) g_xinput_in.analog[a] = 0x8000;
    g_xinput_in.screen_x = g_xinput_in.screen_y = 0x8000;
    int coin_was = 0;
    while (!g_stop) {
        re_xstate st; memset(&st, 0, sizeof st);
        if (s_xget(0, &st) == 0) {                 /* controller 0 present */
            WORD b = st.pad.wButtons;
            int up=!!(b&0x0001), dn=!!(b&0x0002), lf=!!(b&0x0004), rt=!!(b&0x0008);
            int start=!!(b&0x0010), test=!!(b&0x0020) /*BACK*/, svc=!!(b&0x0100) /*LB*/;
            int p1=!!(b&0x1000)/*A*/, p2=!!(b&0x2000)/*B*/, p3=!!(b&0x4000)/*X*/,
                p4=!!(b&0x8000)/*Y*/, p5=!!(b&0x0200)/*RB*/, p6=!!(b&0x0080)/*R3*/;
            if (st.pad.lx < -10000) lf = 1; else if (st.pad.lx > 10000) rt = 1;
            if (st.pad.ly >  10000) up = 1; else if (st.pad.ly < -10000) dn = 1;
            uint8_t byte1 = (uint8_t)((start<<7)|(svc<<6)|(up<<5)|(dn<<4)|(lf<<3)|(rt<<2)|(p1<<1)|p2);
            uint8_t byte2 = (uint8_t)((p3<<7)|(p4<<6)|(p5<<5)|(p6<<4));
            ultra_re_jvs_input in; memset(&in, 0, sizeof in);
            in.player[0] = (uint16_t)((byte1<<8)|byte2);
            in.system    = test ? 0x80 : 0;         /* TEST switch */
            in.analog[0] = (uint16_t)((int)st.pad.lx + 0x8000);
            in.analog[1] = (uint16_t)(0x7FFF - (int)st.pad.ly);
            for (int a = 2; a < JVS_MAX_ANALOG; ++a) in.analog[a] = 0x8000;
            in.screen_x = in.screen_y = 0x8000;
            g_xinput_in = in;
            /* LEGACY STANDALONE COIN - SELECT only, and only when nothing
             * else owns the coin line.
             *
             * This used to be `bRT > 128 || L3`, i.e. the RIGHT TRIGGER inserted
             * a credit. That is a hardcoded path in this thread which bypasses
             * the controls profile AND the loader's shared block entirely, and
             * ultra_hook_pull_input drains it unconditionally - so on a racer,
             * where the right trigger is the ACCELERATOR, every press of the gas
             * fed the coin slot. Reported live on Wacky Race: "R2 is still
             * credit" even after the profile had been reassigned.
             *
             * Now: SELECT/BACK only (matching the profile and the publisher), and
             * suppressed whenever a controls profile or a loader block is present,
             * since those own the coin line and this would double-credit. */
            int legacy_owns_coin = !ultra_input_map_active() &&
                                   !(g_shared && g_shared->magic == ULTRA_IO_MAGIC);
            int coin_now = legacy_owns_coin && !!(b & 0x0020) /* BACK/SELECT */;
            if (coin_now && !coin_was) InterlockedIncrement(&g_local_coin);
            coin_was = coin_now;
        }
        Sleep(8);
    }
    return 0;
}
static void ultra_ringedge_xinput_start(void) { CreateThread(NULL, 0, xinput_thread, NULL, 0, NULL); }

/* The input the game should see: the loader's shared block when present,
 * otherwise the local gamepad. */
static void ultra_ringedge_current_input(ultra_re_jvs_input* out) {
    memset(out, 0, sizeof *out);
    for (int a = 0; a < JVS_MAX_ANALOG; ++a) out->analog[a] = 0x8000;
    out->screen_x = out->screen_y = 0x8000;
    ultra_io_shared* s = g_shared;
    if (s && s->magic == ULTRA_IO_MAGIC) {
        ultra_io_shared snap; int got = 0;
        for (int attempt = 0; attempt < 8; ++attempt) {
            uint32_t s1 = s->seq; MemoryBarrier(); snap = *s; MemoryBarrier();
            uint32_t s2 = s->seq; if (s1 == s2 && !(s1 & 1u)) { got = 1; break; }
        }
        if (!got) snap = *s;
        out->system = snap.system;
        for (int p = 0; p < ULTRA_IO_PLAYERS && p < JVS_MAX_PLAYERS; ++p) out->player[p] = snap.player[p];
        for (int a = 0; a < ULTRA_IO_ANALOG && a < JVS_MAX_ANALOG; ++a) out->analog[a] = snap.analog[a];
        out->screen_x = snap.screen_x; out->screen_y = snap.screen_y;
    } else {
        *out = g_xinput_in;                        /* standalone gamepad fallback */
    }
}

/* Pull input into the JVS node + inject coin edges (shared block + gamepad). */
void ultra_ringedge_pull_input(ultra_re_jvs_node* n) {
    if (!n) return;
    ultra_re_jvs_input in;
    ultra_ringedge_current_input(&in);
    jvs_re_set_input(n, &in);
    ultra_io_shared* s = g_shared;
    if (s && s->magic == ULTRA_IO_MAGIC) {
        for (int c = 0; c < ULTRA_IO_COINS; ++c) {
            uint32_t pulse = s->coin_pulse[c];
            if (pulse != g_coin_seen[c]) {
                int delta = (int)(pulse - g_coin_seen[c]);
                jvs_re_insert_coin(n, c, delta);
                g_coin_seen[c] = pulse; s->coin_ack[c] = pulse;
            }
        }
        s->jvs_address = jvs_re_address(n); s->poll_count++;
    }
    long lc = g_local_coin;                        /* gamepad coin (standalone) */
    if (lc != g_local_coin_seen) {
        int delta = (int)(lc - g_local_coin_seen);
        jvs_re_insert_coin(n, 0, delta);
        g_local_coin_seen = lc;
    }
}

/* ---- Ultra Omni-Track lightgun pose source --------------------------------
 * Coherent snapshot of the loader's seqlocked aiming pose: screen_x/screen_y
 * (already 0..65535 screen-normalized by the loader's own calibration) plus the
 * player[0] trigger bit. The loader writes those three fields inside an
 * odd/even seq window, so reading them raw can latch a TORN pose (X from frame
 * N, trigger from frame N+1) -- retry until we observe a stable, even seq.
 *
 * WHY THE LOADER_ALIVE GATE (do not drop it): a valid magic only proves the
 * mapping still EXISTS, not that anybody is still writing into it. The shared
 * block outlives a loader that exited -- or a gun that went inactive -- with
 * its last pose frozen in place, so trusting magic alone would leave the bridge
 * warping the cursor to, and re-firing at, a stale aim point forever. The
 * loader clears ULTRA_IO_FLAG_LOADER_ALIVE on its way out, so a cleared flag
 * must mean "no pose right now" (the bridge then falls back to the gamepad and
 * leaves a real mouse alone), never "last known pose".
 *
 * Deliberately NOT reusing ultra_ringedge_current_input()'s retry loop above:
 * that one falls through to an unguarded copy when the seqlock never settles
 * (harmless for a JVS poll, which simply repeats next frame) and does not check
 * liveness at all. An aiming pose has to fail closed instead. */
static int gun_pose_snapshot(ultra_io_shared* s, int* out_x, int* out_y, int* out_trig) {
    if (!s || s->magic != ULTRA_IO_MAGIC) return 0;
    if (!(s->flags & ULTRA_IO_FLAG_LOADER_ALIVE)) return 0;
    uint16_t sx = 0x8000, sy = 0x8000, p0 = 0; int got = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t s1 = s->seq; MemoryBarrier();
        sx = s->screen_x; sy = s->screen_y; p0 = s->player[0];
        MemoryBarrier();
        uint32_t s2 = s->seq;
        if (s1 == s2 && !(s1 & 1u)) { got = 1; break; }
    }
    if (!got) return 0;
    *out_x = sx; *out_y = sy;               /* pass through: already 0..65535 */
    *out_trig = (p0 & 0x0200) != 0;         /* bit9 = PUSH1 = gun trigger     */
    return 1;
}

/* ultra_bridge_pose_fn adapter -- signature (int*,int*,int*)->0/1 matches
 * omni_bridge.h exactly, so it is handed straight to
 * ultra_omni_bridge_install() below. Reads the file-static mapping directly (no
 * accessor needed: the shared block is owned by this translation unit). */
int ultra_ringedge_gun_pose(int* out_x, int* out_y, int* out_trig) {
    return gun_pose_snapshot(g_shared, out_x, out_y, out_trig);
}

/* -------------------------------------------------------------------------- */
static void open_shared(void) {
    const char* name = getenv(ULTRA_IO_ENVVAR);
    if (!name || !*name) return;
    g_map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!g_map) return;
    g_shared = (ultra_io_shared*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0,
                                               sizeof(ultra_io_shared));
    if (g_shared && g_shared->magic == ULTRA_IO_MAGIC)
        g_shared->flags |= ULTRA_IO_FLAG_HOOK_ATTACHED;
}

static void configure_node(void) {
    g_node = jvs_re_create();
    if (!g_node) return;
    const char* id = getenv("ULTRA_JVS_ID");
    if (id && *id) jvs_re_set_id(g_node, id);
    int screenpos = 0;
    const char* sp = getenv("ULTRA_JVS_SCREENPOS");
    if (sp) screenpos = (sp[0] == '1');
    /* Advertised analog-channel count. Default 8; RingEdge driving/gun titles
     * may poll more, so let the loader bump it via ULTRA_JVS_ANALOG so FEATCHK
     * matches the game's READ_ANALOG count. */
    uint8_t analog_ch = 0;   /* 0 = keep node default (8) */
    const char* ach = getenv("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= JVS_MAX_ANALOG) analog_ch = (uint8_t)v; }
    jvs_re_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 * imports, render plugins). The first install already happened synchronously
 * in DllMain; this only re-points IAT slots periodically. */
static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        ultra_ringedge_jvs_serial_install();   /* idempotent (name + value based) */
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:

    {   /* Universal transport layer: declare this board's shape so the shared
         * pacing/range/codec core is live and self-describing in the log.
         * Serial boards are REQUEST/RESPONSE -- their reply queue already ends
         * the game's read loop, so pacing is deliberately inert here; injecting
         * empty reads would truncate a reply frame. Behavior is unchanged. */
        static int s_utx_once;
        if (!s_utx_once) { static utx_profile utxp;
            s_utx_once = 1;
            memset(&utxp, 0, sizeof utxp);
            utxp.board        = "sega_ringedge";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        ultra_ringedge_log("attach: ultra_sega_ringedge_hook loaded pid=%lu (%d-bit)\n",
                           GetCurrentProcessId(), (int)(sizeof(void*) * 8));
        /* Install SYNCHRONOUSLY here: the module injects us into the SUSPENDED
         * game and resumes right after, so the hooks must be in place before
         * the game's first thread runs (else its first CreateFile(COM) is
         * missed). */
        open_shared();
        configure_node();
        ultra_ringedge_jvs_serial_install();     /* JVS transport + D3D9 present hook */
        ultra_ringedge_sega_keychip_install();   /* env-gated (OFF by default) */
        ultra_ringedge_audio_install();          /* DirectSound + XAudio2 diagnostic hook */
        ultra_ringedge_xinput_start();           /* standalone gamepad fallback */
        /* Ultra Omni-Bridge (KBM emulation for raw-input-only game dumps).
         * Opt-in (ULTRA_BRIDGE_ENABLE, default OFF -- see omni_client_read_bridge()),
         * so this is a no-op unless a user/future session turns it on for a
         * confirmed raw-keyboard/mouse RingEdge title. Installed after the JVS
         * serial hook above so the D3D9 present hook (which the window getter
         * depends on) is already armed. get_pose is ultra_ringedge_gun_pose():
         * the very same shared-block screen_x/y this board already answers JVS
         * READ_SCRPOS from is a fully calibrated Ultra Omni-Track pose, so a
         * RingEdge gun title that reads the raw OS mouse INSTEAD of JVS gets
         * the real lightgun (aim + trigger), not gamepad-only. The bridge
         * spawns its gun thread only when this argument is non-NULL. */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(ultra_ringedge_d3d9_window, ultra_ringedge_gun_pose);
ultra_pausemenu_install(ultra_ringedge_d3d9_window);
        CreateThread(NULL, 0, worker, NULL, 0, NULL);   /* periodic re-patch only */
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();
ultra_pausemenu_shutdown();
        ultra_ringedge_d3d9_teardown();   /* timeEndPeriod(1) if the limiter raised it */
        if (g_shared) { UnmapViewOfFile(g_shared); g_shared = NULL; }
        if (g_map)    { CloseHandle(g_map); g_map = NULL; }
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader/diagnostics can confirm the right DLL was
 * injected (mirrors the shared Type X hook's ultra_typex_hook_id()). */
__declspec(dllexport) const char* ultra_sega_ringedge_hook_id(void) {
    return "Ultra Loader Sega RingEdge I/O hook (clean-room JVS, board-own build) v1";
}

#endif /* _WIN32 */
