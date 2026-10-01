/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_bemani.c  --  ultra_konami_bemani_hook{32,64}.dll entry point.
 * =============================================================================
 *
 *  Injected into the Konami Bemani PC game process by konami_bemani_main.c via
 *  a suspended-launch + QueueUserAPC(LoadLibraryA) (primary) + a fallback
 *  CreateRemoteThread(LoadLibraryA) -- see find_hook()/inject_dll_apc()/
 *  inject_dll_remote_thread() in that file. konami_bemani_main.c never calls
 *  GetProcAddress for any particular export afterward; DllMain does the work
 *  the instant the DLL is loaded, exactly like every other Ultra Loader hook.
 *
 *  On DLL_PROCESS_ATTACH this file:
 *    - opens the loader's shared-memory input block if the loader created one
 *      (ULTRA_IO_SHM), else falls back to reading a local XInput gamepad so a
 *      standalone/test launch is still controllable end-to-end,
 *    - creates one clean-room JVS node (jvs_core_bemani.c) -- available as a
 *      generic coin-door/service-panel transport target on any Bemani cabinet
 *      revision that bridges one (its coin counters are fed every poll either
 *      way, so the shared IPC block's coin_pulse/coin_ack telemetry always has
 *      a real consumer even before a plain-JVS transport is wired up),
 *    - installs the Konami ACIO bit-protocol I/O scaffold + the shared IAT
 *      patcher + the D3D9/GetProcAddress packed-path hook
 *      (konami_io_bemani.c, env-gated OFF unless ULTRA_KONAMI_IO=1),
 *    - installs the clean-room DirectInput8 emulation as a safety-net fallback
 *      (dinput8_emu_bemani.c),
 *    - installs the DirectSound/XAudio2 diagnostic audio hook
 *      (audio_hook_bemani.c).
 *
 *  This DLL is THIS BOARD'S OWN independent artifact: it is compiled from
 *  files ONLY in modules/konami_bemani/src/hook/, and does not #include, link
 *  against, or load at runtime any file or DLL from the native_x86 module, any
 *  other board module (including konami_pc), or from modtest/. No game code,
 *  no third-party loader code -- Ultra Loader's own work over the public
 *  Win32 API + the open JVS spec + public Konami ACIO protocol facts. Every
 *  RSA/AES-protected per-game data path and every real dongle-key derivation
 *  stays OUT of scope -- presence-only, never implemented (see
 *  konami_io_bemani.c's banner for the exact ethical boundary preserved here).
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "jvs_core_bemani.h"
#include "ultra_io_shared_bemani.h"
#include "omni_bridge_bemani.h"

#include "ultra_transport.h"   /* shared universal I/O transport layer */

/* ---------------------------------------------------------------------------
 *  ultra_input_map_active -- this board has no controls-profile layer.
 *
 *  The coin guard below asks whether a profile owns the coin line. This board
 *  never gained the controls/ subsystem, so the honest answer is "no": there is
 *  no profile, ownership falls through to the shared block, which is precisely
 *  what the expression is written to do. Boards that DO carry the layer get the
 *  real implementation from their own controls.c instead of this.
 * ------------------------------------------------------------------------- */
static int ultra_input_map_active(void) { return 0; }

/* Shared pause/settings overlay (pause_menu_wire_bemani.c -> work/src/
 * pause_menu_service.c). Self-contained poll thread, see that file's header. */
void ultra_pausemenu_install(HWND (*get_hwnd)(void));
void ultra_pausemenu_shutdown(void);

/* from the other hook translation units in this board's OWN tree */
void ultra_kb_konami_io_install(void);   /* ACIO scaffold + shared IAT patcher + D3D9 arm */
void ultra_kb_dinput8_install(void);     /* clean-room DirectInput8 emulation (fallback)  */
void ultra_kb_audio_install(void);       /* DirectSound + XAudio2 diagnostic hook          */
void ultra_kb_d3d9_teardown(void);       /* pair timeBeginPeriod/timeEndPeriod             */
HWND ultra_kb_d3d9_get_window(void);     /* current device/render window (d3d9_throttle_bemani.c) */

/* ---- shared diagnostic log (ULTRA_JVS_LOG), used by every TU in this tree - */
void ultra_kb_log(const char* fmt, ...) {
    const char* path = getenv("ULTRA_JVS_LOG");
    if (!path || !*path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fclose(f);
}

/* -------------------------------------------------------------------------- */
static ultra_kb_jvs_node*  g_node   = NULL;
static HANDLE              g_map    = NULL;
static ultra_io_shared*    g_shared = NULL;
static uint32_t            g_coin_seen[ULTRA_IO_COINS] = {0};
static volatile LONG       g_stop   = 0;

ultra_kb_jvs_node* ultra_kb_jvs_node_get(void) {
    return g_node;   /* created by DllMain before any hook can fire */
}

/* ---- standalone gamepad (XInput) fallback ---------------------------------
 * When no loader is feeding the shared block (e.g. a standalone test launch),
 * read the player's gamepad directly so the game is actually controllable --
 * matching the project's per-game-complete rule (render + SOUND + CONTROLS,
 * all three). XInput is loaded dynamically (no hard dependency on a given
 * version of the DLL). */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } kb_xpad;
typedef struct { DWORD packet; kb_xpad pad; } kb_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, kb_xstate*);
static p_xinput_get      s_xget = NULL;
static ultra_kb_jvs_input g_xinput_in;
static volatile LONG     g_local_coin = 0;
static long              g_local_coin_seen = 0;

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
        kb_xstate st; memset(&st, 0, sizeof st);
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
            ultra_kb_jvs_input in; memset(&in, 0, sizeof in);
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
static void ultra_kb_xinput_start(void) { CreateThread(NULL, 0, xinput_thread, NULL, 0, NULL); }

/* The input the game should see: the loader's shared block when present,
 * otherwise the local gamepad. Read by the ACIO poll builder
 * (konami_io_bemani.c) AND the DirectInput8 emulation (dinput8_emu_bemani.c)
 * so both report the same controller state. */
void ultra_kb_hook_current_input(ultra_kb_jvs_input* out) {
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

/* ---- Ultra Omni-Track lightgun pose (Omni-Bridge source) -------------------
 * Signature is exactly ultra_bridge_pose_fn (omni_bridge_bemani.h), so this
 * drops straight into ultra_omni_bridge_install()'s 2nd argument. The loader
 * lands an already-calibrated, screen-normalized pose (0..65535) in
 * screen_x/screen_y and flags the gun trigger in player[0] bit9 (PUSH1 in
 * jvs_core_bemani.h's switch map) -- the same bit an Omni-Track trigger maps to.
 *
 * WHY THE LOADER_ALIVE GATE (a dead loader means "no pose", NOT "last pose"):
 * magic alone only proves the block was ever created -- the mapping stays valid
 * in OUR address space long after the loader process exits, with the final pose
 * it wrote frozen in place. Handing that stale pose back would warp the cursor
 * to wherever the gun last pointed, every tick, forever -- and worse, re-assert
 * a trigger that merely happened to be held at exit, firing the game by itself.
 * The loader clears ULTRA_IO_FLAG_LOADER_ALIVE when it goes away (or when its
 * gun goes inactive), so returning 0 here makes the bridge fall back to the
 * gamepad / the player's real mouse instead.
 *
 * Unlike ultra_kb_hook_current_input() above, a TORN read is deliberately not
 * accepted: that funnel feeds a switch/analog poll where one frame-old bit is
 * harmless, but a torn pose pairs X from one frame with the trigger from the
 * next -- a shot registered at the wrong point on screen. Bounded retries, then
 * report "no pose" rather than guess. */
int konami_bemani_gun_pose(int* out_x, int* out_y, int* out_trig) {
    ultra_io_shared* s = g_shared;
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
    if (!got) return 0;                       /* never publish an incoherent pose */
    if (out_x)    *out_x    = (int)sx;        /* 0..65535, screen-normalized      */
    if (out_y)    *out_y    = (int)sy;
    if (out_trig) *out_trig = (p0 & 0x0200) != 0;   /* bit9 = PUSH1 = gun trigger */
    return 1;
}

/* Pull input into the JVS node + inject coin edges (shared block + gamepad).
 * The node itself is not yet driven by any wire transport in this deliverable
 * (this board's real transport is the ACIO scaffold, konami_io_bemani.c,
 * which reads ultra_kb_hook_current_input() directly) -- but its coin
 * counters and telemetry fields are kept live so the shared IPC contract's
 * coin_pulse/coin_ack/jvs_address/poll_count fields always have a real
 * consumer, exactly like every other board module in this project. */
void ultra_kb_pull_input(ultra_kb_jvs_node* n) {
    if (!n) return;
    ultra_kb_jvs_input in;
    ultra_kb_hook_current_input(&in);
    jvs_kb_set_input(n, &in);
    ultra_io_shared* s = g_shared;
    if (s && s->magic == ULTRA_IO_MAGIC) {
        for (int c = 0; c < ULTRA_IO_COINS; ++c) {
            uint32_t pulse = s->coin_pulse[c];
            if (pulse != g_coin_seen[c]) {
                int delta = (int)(pulse - g_coin_seen[c]);
                jvs_kb_insert_coin(n, c, delta);
                g_coin_seen[c] = pulse; s->coin_ack[c] = pulse;
            }
        }
        s->jvs_address = jvs_kb_address(n); s->poll_count++;
    }
    long lc = g_local_coin;                        /* gamepad coin (standalone) */
    if (lc != g_local_coin_seen) {
        int delta = (int)(lc - g_local_coin_seen);
        jvs_kb_insert_coin(n, 0, delta);
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
    g_node = jvs_kb_create();
    if (!g_node) return;
    const char* id = getenv("ULTRA_JVS_ID");
    if (id && *id) jvs_kb_set_id(g_node, id);
    int screenpos = 0;
    const char* sp = getenv("ULTRA_JVS_SCREENPOS");
    if (sp) screenpos = (sp[0] == '1');
    /* Advertised analog-channel count. Default 8; a slider/knob-heavy Bemani
     * title may poll more, so let the loader bump it via ULTRA_JVS_ANALOG so
     * FEATCHK matches the game's READ_ANALOG count. */
    uint8_t analog_ch = 0;   /* 0 = keep node default (8) */
    const char* ach = getenv("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= JVS_MAX_ANALOG) analog_ch = (uint8_t)v; }
    jvs_kb_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 * imports, render plugins). The first install already happened synchronously
 * in DllMain; this only re-points IAT slots periodically. */
static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        ultra_kb_konami_io_install();   /* idempotent (name + value based IAT patch) */
        ultra_kb_dinput8_install();     /* idempotent */
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
            utxp.board        = "konami_bemani";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        ultra_kb_log("attach: ultra_konami_bemani_hook loaded pid=%lu (%d-bit)\n",
                     GetCurrentProcessId(), (int)(sizeof(void*) * 8));
        /* Install SYNCHRONOUSLY here: the module injects us into the SUSPENDED
         * game and resumes right after, so the hooks must be in place before
         * the game's first thread runs (else its first CreateFile(ACIO
         * device) is missed). */
        open_shared();
        configure_node();
        ultra_kb_konami_io_install();     /* ACIO scaffold + shared IAT patcher + D3D9 arm */
        ultra_kb_dinput8_install();       /* synthetic DInput8 device (safety-net fallback) */
        ultra_kb_audio_install();         /* DirectSound + XAudio2 diagnostic hook */
        ultra_kb_xinput_start();          /* standalone gamepad fallback */
        /* Ultra Omni-Bridge: opt-in (ULTRA_BRIDGE_ENABLE, default OFF) raw
         * gamepad->cursor/keys bridge for a Bemani title that reads OS mouse/
         * keyboard directly instead of JVS/ACIO. Installed after the D3D9
         * present hook above so its window getter has something to find once
         * the game creates its device. 2nd arg = the Ultra Omni-Track pose
         * source: konami_bemani_gun_pose() (above) hands the bridge the
         * loader's live, calibrated lightgun pose out of this board's shared
         * block (screen_x/screen_y + the player[0] PUSH1 trigger bit), and the
         * bridge's gun thread lets it win the cursor over the gamepad. Bemani
         * cabinets ship no gun of their own, but an Omni-Track gun is a general
         * pointing device -- and with no live loader pose the gun thread simply
         * reports "no pose" each tick, so the gamepad path behaves exactly as
         * it did when this argument was NULL. */
        ultra_omni_bridge_install(ultra_kb_d3d9_get_window, konami_bemani_gun_pose);
        /* Shared pause/settings overlay -- Start+Select / LB+RB (pad) or
         * F1/Esc/P (keyboard) to open. Own self-contained poll thread. */
        ultra_pausemenu_install(ultra_kb_d3d9_get_window);
        CreateThread(NULL, 0, worker, NULL, 0, NULL);   /* periodic re-patch only */
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();  /* releases any held mouse buttons first */
        ultra_pausemenu_shutdown();
        ultra_kb_d3d9_teardown();   /* timeEndPeriod(1) if the limiter raised it */
        if (g_shared) { UnmapViewOfFile(g_shared); g_shared = NULL; }
        if (g_map)    { CloseHandle(g_map); g_map = NULL; }
        if (g_node)   { jvs_kb_destroy(g_node); g_node = NULL; }
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader/diagnostics can confirm the right DLL was
 * injected (mirrors every other board hook's own *_hook_id()). */
__declspec(dllexport) const char* ultra_konami_bemani_hook_id(void) {
    return "Ultra Loader Konami Bemani PC I/O hook (clean-room JVS + ACIO, board-own build) v1";
}

#endif /* _WIN32 */
