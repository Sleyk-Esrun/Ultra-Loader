/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_generic.c  --  ultra_generic_pc_hook{32,64}.dll entry point.
 * =============================================================================
 *
 *  Injected into an UNRECOGNISED PC-based arcade game process by
 *  generic_pc_main.c via a suspended-launch + QueueUserAPC(LoadLibraryA)
 *  (primary) + a fallback CreateRemoteThread(LoadLibraryA) -- see find_hook()/
 *  inject_dll_apc()/inject_dll_remote_thread() in that file. generic_pc_main.c
 *  never calls GetProcAddress for any particular export afterward; DllMain does
 *  the work the instant the DLL is loaded, exactly like every other Ultra
 *  Loader hook.
 *
 *  generic_pc is the DELIBERATE catch-all fallback board profile: it exists for
 *  a PC arcade dump that doesn't match any recognized vendor board. Because we
 *  do not know the game's graphics API ahead of time, this DLL PROBES/ARMS ALL
 *  FOUR candidate present-hook chains (D3D8, D3D9, D3D11/DXGI, OpenGL)
 *  simultaneously -- whichever one the game's real imports resolve to is the
 *  one that actually fires (see jvs_serial_hook_generic.c's install path and
 *  its GetProcAddress interception for the packed/dynamic-resolve case).
 *
 *  On DLL_PROCESS_ATTACH this file:
 *    - opens the loader's shared-memory input block if the loader created one
 *      (ULTRA_IO_SHM), else falls back to reading a local XInput gamepad so a
 *      standalone/test launch is still controllable end-to-end,
 *    - creates one clean-room JVS node (jvs_core_generic.c) -- JVS-over-serial
 *      is the safest universal default I/O assumption for an unrecognised PC
 *      board (the overwhelming majority of the post-2000 crop uses it),
 *    - installs the JVS-serial transport hook (jvs_serial_hook_generic.c),
 *      which ALSO arms all four present-hook IAT patches unconditionally,
 *    - installs the synthetic DirectInput8 device (dinput8_emu_generic.c) --
 *      the board profile also declares BP_IO_DINPUT8 as a possible input path,
 *    - installs the DirectSound/XAudio2 diagnostic audio hook
 *      (audio_hook_generic.c).
 *
 *  This DLL is THIS BOARD'S OWN independent artifact: it is compiled from
 *  files ONLY in modules/generic_pc/src/hook/, and does not #include, link
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

#include "jvs_core_generic.h"
#include "ultra_io_shared_generic.h"
#include "omni_bridge.h"
#include "omni_client.h"       /* omni_client_lightgun_enabled() -- Omni-Track gate */
#include "ultra_transport.h"   /* shared universal I/O transport layer */

/* from the other hook translation units in this board's OWN tree */
int  ultra_genpc_jvs_serial_install(void);
void ultra_genpc_dinput8_install(void);
void ultra_genpc_audio_install(void);
void ultra_genpc_d3d8_teardown(void);    /* pair timeBeginPeriod/timeEndPeriod */
void ultra_genpc_d3d9_teardown(void);
void ultra_genpc_d3d11_teardown(void);
void ultra_genpc_gl_teardown(void);

/* Per-API device-window getters (one of these four fires -- see the file
 * comment above on the 4-way present-hook auto-detect). Each is NULL until
 * that API's CreateDevice/swap-chain-create has actually happened. */
HWND ultra_genpc_d3d8_window(void);
HWND ultra_genpc_d3d9_window(void);
HWND ultra_genpc_d3d11_window(void);
HWND ultra_genpc_gl_window(void);

/* ---- shared diagnostic log (ULTRA_JVS_LOG), used by every TU in this tree - */
void ultra_genpc_log(const char* fmt, ...) {
    const char* path = getenv("ULTRA_JVS_LOG");
    if (!path || !*path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fclose(f);
}

/* -------------------------------------------------------------------------- */
static ultra_gp_jvs_node*  g_node   = NULL;
static HANDLE              g_map    = NULL;
static ultra_io_shared*    g_shared = NULL;
static uint32_t            g_coin_seen[ULTRA_IO_COINS] = {0};
static volatile LONG       g_stop   = 0;

ultra_gp_jvs_node* ultra_genpc_jvs_node(void) {
    return g_node;   /* created by DllMain before any hook can fire */
}

/* ---- standalone gamepad (XInput) fallback ---------------------------------
 * When no loader is feeding the shared block (e.g. a standalone test launch),
 * read the player's gamepad directly so the game is actually controllable --
 * matching the project's per-game-complete rule (render + SOUND + CONTROLS,
 * all three). XInput is loaded dynamically (no hard dependency on a given
 * version of the DLL). */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } gp_xpad;
typedef struct { DWORD packet; gp_xpad pad; } gp_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, gp_xstate*);
static p_xinput_get     s_xget = NULL;
static ultra_gp_jvs_input g_xinput_in;
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
        gp_xstate st; memset(&st, 0, sizeof st);
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
            ultra_gp_jvs_input in; memset(&in, 0, sizeof in);
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
static void ultra_genpc_xinput_start(void) { CreateThread(NULL, 0, xinput_thread, NULL, 0, NULL); }

/* The input the game should see: the loader's shared block when present,
 * otherwise the local gamepad. Used by the JVS engine AND the DirectInput8
 * emulation so both report the same controller state. */
void ultra_genpc_current_input(ultra_gp_jvs_input* out) {
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
void ultra_genpc_pull_input(ultra_gp_jvs_node* n) {
    if (!n) return;
    ultra_gp_jvs_input in;
    ultra_genpc_current_input(&in);
    jvs_gp_set_input(n, &in);
    ultra_io_shared* s = g_shared;
    if (s && s->magic == ULTRA_IO_MAGIC) {
        for (int c = 0; c < ULTRA_IO_COINS; ++c) {
            uint32_t pulse = s->coin_pulse[c];
            if (pulse != g_coin_seen[c]) {
                int delta = (int)(pulse - g_coin_seen[c]);
                jvs_gp_insert_coin(n, c, delta);
                g_coin_seen[c] = pulse; s->coin_ack[c] = pulse;
            }
        }
        s->jvs_address = jvs_gp_address(n); s->poll_count++;
    }
    long lc = g_local_coin;                        /* gamepad coin (standalone) */
    if (lc != g_local_coin_seen) {
        int delta = (int)(lc - g_local_coin_seen);
        jvs_gp_insert_coin(n, 0, delta);
        g_local_coin_seen = lc;
    }
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
    g_node = jvs_gp_create();
    if (!g_node) return;
    const char* id = getenv("ULTRA_JVS_ID");
    if (id && *id) jvs_gp_set_id(g_node, id);
    int screenpos = 0;
    const char* sp = getenv("ULTRA_JVS_SCREENPOS");
    if (sp) screenpos = (sp[0] == '1');
    /* Advertised analog-channel count. Default 8; an unrecognised driving/gun
     * title may poll more, so let the loader bump it via ULTRA_JVS_ANALOG so
     * FEATCHK matches the game's READ_ANALOG count. */
    uint8_t analog_ch = 0;   /* 0 = keep node default (8) */
    const char* ach = getenv("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= JVS_MAX_ANALOG) analog_ch = (uint8_t)v; }
    jvs_gp_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* ---- Ultra Omni-Bridge (keyboard/mouse emulation from gamepad/lightgun) ---
 * Some PC-arcade dumps read the raw OS keyboard+mouse directly instead of
 * JVS/DirectInput/a gun API (proven live 2026-08-02 on taito_typex2 / Haunted
 * Museum). generic_pc is the catch-all for an UNRECOGNISED board, so it is
 * exactly the profile most likely to hit that case. The bridge's window
 * getter tries all four candidate present-hook chains (whichever one this
 * game's real imports resolved to is the one with a non-NULL window).
 * Defaults OFF (ULTRA_BRIDGE_ENABLE=0) via omni_client_read_bridge() --
 * opt-in per game once a specific dump is confirmed to need it. */
static HWND ultra_genpc_bridge_window(void) {
    HWND w;
    if ((w = ultra_genpc_d3d9_window())  != NULL) return w;
    if ((w = ultra_genpc_d3d8_window())  != NULL) return w;
    if ((w = ultra_genpc_d3d11_window()) != NULL) return w;
    if ((w = ultra_genpc_gl_window())    != NULL) return w;
    return NULL;
}

/* ---- Ultra Omni-Track lightgun pose (ultra_bridge_pose_fn) ----------------
 * The bridge's 2nd install argument is the ONLY route an Omni-Track pose has
 * into a game on this board, so it can no longer be NULL. It is instead
 * DOUBLE-gated, because on generic_pc "the shared block is mapped" does NOT
 * mean "a gun is aiming":
 *
 *  1. ULTRA_LIGHTGUN_ENABLE (omni_client_lightgun_enabled(), default 0).
 *     The loader maps the block and stamps LOADER_ALIVE for EVERY launch, and
 *     seeds screen_x/screen_y = 0x8000 (dead center) whether or not a gun is
 *     attached. So magic+liveness ALONE would report a bogus "live pose" on
 *     every non-gun generic_pc title, and the bridge's gun_thread would pin
 *     the cursor to screen center 120x/sec -- exactly the regression this
 *     board's old get_pose=NULL note warned about. Gating on the user's own
 *     Omni-Track switch keeps the whole path inert unless a gun is wanted.
 *
 *  2. ULTRA_IO_FLAG_LOADER_ALIVE. Magic is not liveness: the mapped view (and
 *     the magic word inside it) outlives the loader process, so a read after
 *     the loader exits returns a STALE frame. A dead loader must therefore
 *     mean "no pose" -- the bridge then leaves the cursor alone and a real
 *     mouse still works -- and never "last known pose", which would warp the
 *     aim to wherever the gun was pointing at exit and re-fire a trigger
 *     frozen down in that stale snapshot.
 *
 * screen_x/screen_y/player[0] are read under the loader's seqlock (bounded
 * retries, same pattern as ultra_genpc_current_input above) so the aim point
 * and the trigger can never be latched from two different frames. */
static int g_pose_last_x = 0x8000;   /* last ON-SCREEN aim, held across a reload */
static int g_pose_last_y = 0x8000;

int generic_pc_gun_pose(int* out_x, int* out_y, int* out_trig) {
    if (!out_x || !out_y || !out_trig) return 0;
    if (!omni_client_lightgun_enabled()) return 0;   /* gate 1: Omni-Track off */
    ultra_io_shared* s = g_shared;
    if (!s || s->magic != ULTRA_IO_MAGIC) return 0;
    if (!(s->flags & ULTRA_IO_FLAG_LOADER_ALIVE)) return 0;   /* gate 2: dead loader -> no pose, not a stale one */

    uint16_t sx = 0x8000, sy = 0x8000, p0 = 0; int got = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t s1 = s->seq; MemoryBarrier();
        sx = s->screen_x; sy = s->screen_y; p0 = s->player[0];
        MemoryBarrier();
        uint32_t s2 = s->seq;
        if (s1 == s2 && !(s1 & 1u)) { got = 1; break; }
    }
    if (!got) return 0;                              /* never hand back a torn pose */

    /* Off-screen (PUSH2, bit8) is a gun-game RELOAD, and the loader parks the
     * pose at 0,0 to say so. The bridge feeds an OS cursor clamped inside the
     * client rect, so passing 0,0 through would aim at the game's top-left
     * CORNER -- an in-window shot, not a reload. Hold the last on-screen aim
     * and drop the trigger instead: the cursor doesn't jump, and the mouse
     * button is guaranteed released (returning 0 here would instead leave a
     * button that was already down latched down). */
    if (p0 & 0x0100) {                               /* PUSH2 = off-screen/reload */
        *out_x = g_pose_last_x; *out_y = g_pose_last_y; *out_trig = 0;
        return 1;
    }
    g_pose_last_x = sx; g_pose_last_y = sy;
    *out_x = sx; *out_y = sy;
    *out_trig = (p0 & 0x0200) != 0;                  /* PUSH1 = gun trigger */
    return 1;
}

/* Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 * imports, render plugins). The first install already happened synchronously
 * in DllMain; this only re-points IAT slots periodically. */
static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        ultra_genpc_jvs_serial_install();   /* idempotent (name + value based) */
        ultra_genpc_dinput8_install();      /* idempotent */
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
            utxp.board        = "generic_pc";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        ultra_genpc_log("attach: ultra_generic_pc_hook loaded pid=%lu (%d-bit)\n",
                        GetCurrentProcessId(), (int)(sizeof(void*) * 8));
        /* Install SYNCHRONOUSLY here: the module injects us into the SUSPENDED
         * game and resumes right after, so the hooks must be in place before
         * the game's first thread runs (else its first CreateFile(COM) is
         * missed). */
        open_shared();
        configure_node();
        ultra_genpc_jvs_serial_install();   /* JVS transport + ALL FOUR present hooks armed */
        ultra_genpc_dinput8_install();      /* synthetic DirectInput8 device */
        ultra_genpc_audio_install();        /* DirectSound + XAudio2 diagnostic hook */
        ultra_genpc_xinput_start();         /* standalone gamepad fallback */
        /* Omni-Bridge install AFTER the present hooks arm above, so its
         * window getter has something to poll for. Default OFF. The pose fn
         * carries its own ULTRA_LIGHTGUN_ENABLE + LOADER_ALIVE gates (see
         * generic_pc_gun_pose above), so wiring it here cannot disturb a
         * non-gun title. */
        ultra_omni_bridge_install(ultra_genpc_bridge_window, generic_pc_gun_pose);
        CreateThread(NULL, 0, worker, NULL, 0, NULL);   /* periodic re-patch only */
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();
        ultra_genpc_d3d8_teardown();    /* timeEndPeriod(1) if that limiter raised it */
        ultra_genpc_d3d9_teardown();
        ultra_genpc_d3d11_teardown();
        ultra_genpc_gl_teardown();
        if (g_shared) { UnmapViewOfFile(g_shared); g_shared = NULL; }
        if (g_map)    { CloseHandle(g_map); g_map = NULL; }
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader/diagnostics can confirm the right DLL was
 * injected (mirrors the shared Type X hook's ultra_typex_hook_id()). */
__declspec(dllexport) const char* ultra_generic_pc_hook_id(void) {
    return "Ultra Loader Generic PC I/O hook (clean-room JVS + D3D8/9/11/GL auto-detect, board-own build) v1";
}

#endif /* _WIN32 */
