/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_pcplus.c  --  ultra_andamiro_pcplus_hook{32,64}.dll entry point
 * =============================================================================
 *
 *  Injected into the Andamiro PC+ (Pump It Up) game process by
 *  andamiro_pcplus_main.c via a suspended-launch + QueueUserAPC(LoadLibraryA)
 *  (primary) + a fallback CreateRemoteThread(LoadLibraryA) -- see find_hook()/
 *  inject_dll_apc()/inject_dll_remote_thread() in that file.
 *  andamiro_pcplus_main.c never calls GetProcAddress for any particular export
 *  afterward; DllMain does the work the instant the DLL is loaded, exactly
 *  like every other Ultra Loader hook.
 *
 *  On DLL_PROCESS_ATTACH this file:
 *    - opens the loader's shared-memory input block if the loader created one
 *      (ULTRA_IO_SHM), else falls back to reading a local XInput gamepad so a
 *      standalone/test launch is still controllable end-to-end,
 *    - creates one clean-room JVS node (jvs_core_pcplus.c),
 *    - installs the JVS-serial transport hook (jvs_serial_hook_pcplus.c),
 *      which also arms the D3D9 present hook (d3d9_throttle_pcplus.c) and the
 *      DirectInput8 emulation (dinput8_emu_pcplus.c) for packed imports,
 *    - installs the DirectInput8 emulation for the static-import case too
 *      (dance-pad panels as a generic joystick -- no real dump/protocol
 *      capture exists yet, see UNTESTED.txt, so BOTH transports are covered),
 *    - installs the DirectSound/XAudio2 diagnostic audio hook
 *      (audio_hook_pcplus.c).
 *
 *  This DLL is THIS BOARD'S OWN independent artifact: it is compiled from
 *  files ONLY in modules/andamiro_pcplus/src/hook/, and does not #include,
 *  link against, or load at runtime any file or DLL from the native_x86
 *  module or from modtest/. No game code, no third-party loader code --
 *  Ultra Loader's own work over the public Win32 API + the open JVS spec.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "jvs_core_pcplus.h"
#include "ultra_io_shared_pcplus.h"
#include "omni_bridge.h"

#include "ultra_transport.h"   /* shared universal I/O transport layer */
/* from the other hook translation units in this board's OWN tree */
int  ultra_pcplus_jvs_serial_install(void);
void ultra_pcplus_dinput8_install(void);
void ultra_pcplus_audio_install(void);
void ultra_pcplus_d3d9_teardown(void);          /* pair timeBeginPeriod/timeEndPeriod */
HWND ultra_pcplus_device_window(void);          /* d3d9_throttle_pcplus.c; Omni-Bridge window accessor */

/* ---- shared diagnostic log (ULTRA_JVS_LOG), used by every TU in this tree - */
void ultra_pcplus_log(const char* fmt, ...) {
    const char* path = getenv("ULTRA_JVS_LOG");
    if (!path || !*path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fclose(f);
}

/* -------------------------------------------------------------------------- */
static ultra_pcplus_jvs_node* g_node   = NULL;
static HANDLE                 g_map    = NULL;
static ultra_io_shared*       g_shared = NULL;
static uint32_t               g_coin_seen[ULTRA_IO_COINS] = {0};
static volatile LONG          g_stop   = 0;

ultra_pcplus_jvs_node* ultra_pcplus_jvs_node_get(void) {
    return g_node;   /* created by DllMain before any hook can fire */
}

/* The loader's mapped shared-input block, or NULL when no loader is attached.
 * Companion to ultra_pcplus_jvs_node_get() above: g_shared is file-static, so
 * anything else in this DLL reads it through here instead of re-opening the
 * mapping. Plain read -- g_shared is NULLed immediately after UnmapViewOfFile
 * in DLL_PROCESS_DETACH, matching the existing readers' behavior. */
ultra_io_shared* ultra_pcplus_shared(void) {
    return g_shared;
}

/* ---- Ultra Omni-Track lightgun pose --------------------------------------
 * Coherent snapshot of the loader's seqlocked aiming pose (screen_x/screen_y,
 * 0..65535 screen-normalized) plus player[0]'s trigger bit. The signature is
 * exactly ultra_bridge_pose_fn ((int*,int*,int*) -> 0/1, omni_bridge.h:30), so
 * this drops straight into the 2nd argument of ultra_omni_bridge_install()
 * below and gives Ultra Omni-Track a way to reach this board.
 *
 * The loader writes those three fields inside an odd/even seq window, so read
 * them under the same bounded retry loop ultra_pcplus_current_input() uses --
 * a raw read can latch a torn pose (X from frame N, trigger from frame N+1).
 *
 * WHY LOADER_ALIVE AND NOT JUST MAGIC: the magic word is written once and
 * survives in the mapping after the loader exits (or after its gun goes
 * inactive) -- the bytes stay valid-looking forever. A magic-only gate would
 * therefore keep reporting the LAST pose as if it were live: the bridge would
 * warp the cursor to a stale aim point every 8ms (fighting a real mouse) and
 * re-assert whatever trigger state happened to be latched when the loader
 * died. Returning 0 instead makes the bridge fall back to its gamepad path and
 * leave the cursor alone. A dead loader must mean "no pose", never "last known
 * pose". */
int andamiro_pcplus_gun_pose(int* out_x, int* out_y, int* out_trig) {
    ultra_io_shared* s = ultra_pcplus_shared();
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
    if (!got) return 0;   /* writer never settled -> report "no pose", not a torn one */
    *out_x = sx; *out_y = sy;
    *out_trig = (p0 & 0x0200) != 0;   /* bit9 = PUSH1 = gun trigger (jvs_core_pcplus.h) */
    return 1;
}

/* ---- standalone gamepad (XInput) fallback ---------------------------------
 * When no loader is feeding the shared block (e.g. a standalone test launch),
 * read the player's gamepad directly so the game is actually controllable --
 * matching the project's per-game-complete rule (render + SOUND + CONTROLS,
 * all three). XInput is loaded dynamically (no hard dependency on a given
 * version of the DLL). The dance pad's directional panels map to the D-pad /
 * left stick; the diagonal corner panels (this board's actual 5-panel "X"
 * layout, see jvs_core_pcplus.h) map to the face buttons -- a config/controls
 * layer is where the FINAL physical-panel assignment belongs once a real
 * cabinet capture is available; this fallback exists purely so a standalone
 * test launch is controllable today. */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } pc_xpad;
typedef struct { DWORD packet; pc_xpad pad; } pc_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, pc_xstate*);
static p_xinput_get           s_xget = NULL;
static ultra_pcplus_jvs_input g_xinput_in;
static volatile LONG          g_local_coin = 0;
static long                   g_local_coin_seen = 0;

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
        pc_xstate st; memset(&st, 0, sizeof st);
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
            ultra_pcplus_jvs_input in; memset(&in, 0, sizeof in);
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
static void ultra_pcplus_xinput_start(void) { CreateThread(NULL, 0, xinput_thread, NULL, 0, NULL); }

/* The input the game should see: the loader's shared block when present,
 * otherwise the local gamepad. Used by the JVS engine AND the DirectInput8
 * emulation so both transports report the same controller state. */
void ultra_pcplus_current_input(ultra_pcplus_jvs_input* out) {
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
void ultra_pcplus_pull_input(ultra_pcplus_jvs_node* n) {
    if (!n) return;
    ultra_pcplus_jvs_input in;
    ultra_pcplus_current_input(&in);
    jvs_pcplus_set_input(n, &in);
    ultra_io_shared* s = g_shared;
    if (s && s->magic == ULTRA_IO_MAGIC) {
        for (int c = 0; c < ULTRA_IO_COINS; ++c) {
            uint32_t pulse = s->coin_pulse[c];
            if (pulse != g_coin_seen[c]) {
                int delta = (int)(pulse - g_coin_seen[c]);
                jvs_pcplus_insert_coin(n, c, delta);
                g_coin_seen[c] = pulse; s->coin_ack[c] = pulse;
            }
        }
        s->jvs_address = jvs_pcplus_address(n); s->poll_count++;
    }
    long lc = g_local_coin;                        /* gamepad coin (standalone) */
    if (lc != g_local_coin_seen) {
        int delta = (int)(lc - g_local_coin_seen);
        jvs_pcplus_insert_coin(n, 0, delta);
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
    g_node = jvs_pcplus_create();
    if (!g_node) return;
    const char* id = getenv("ULTRA_JVS_ID");
    if (id && *id) jvs_pcplus_set_id(g_node, id);
    int screenpos = 0;
    const char* sp = getenv("ULTRA_JVS_SCREENPOS");
    if (sp) screenpos = (sp[0] == '1');
    uint8_t analog_ch = 0;   /* 0 = keep node default (8); unused by a dance pad */
    const char* ach = getenv("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= JVS_MAX_ANALOG) analog_ch = (uint8_t)v; }
    jvs_pcplus_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 * imports, render plugins). The first install already happened synchronously
 * in DllMain; this only re-points IAT slots periodically. */
static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        ultra_pcplus_jvs_serial_install();   /* idempotent (name + value based) */
        ultra_pcplus_dinput8_install();      /* idempotent */
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
            utxp.board        = "andamiro_pcplus";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        ultra_pcplus_log("attach: ultra_andamiro_pcplus_hook loaded pid=%lu (%d-bit)\n",
                         GetCurrentProcessId(), (int)(sizeof(void*) * 8));
        /* Install SYNCHRONOUSLY here: the module injects us into the SUSPENDED
         * game and resumes right after, so the hooks must be in place before
         * the game's first thread runs (else its first CreateFile(COM) is
         * missed). */
        open_shared();
        configure_node();
        ultra_pcplus_jvs_serial_install();   /* JVS transport + D3D9 present hook */
        ultra_pcplus_dinput8_install();      /* synthetic DirectInput8 device */
        ultra_pcplus_audio_install();        /* DirectSound + XAudio2 diagnostic hook */
        ultra_pcplus_xinput_start();         /* standalone gamepad fallback */
        /* Ultra Omni-Bridge: raw OS cursor/click/key synthesis for a dump that
         * reads keyboard+mouse directly instead of JVS/DirectInput (see
         * omni_bridge.h). Installed after the JVS-serial install above, which
         * is what arms the D3D9 present hook that ultra_pcplus_device_window()
         * reads -- must be armed first so the window accessor has something to
         * return. get_pose is andamiro_pcplus_gun_pose (above), which serves
         * the loader's shared screen_x/screen_y + trigger bit: this board is a
         * dance pad rather than a gun game, but the loader's Ultra Omni-Track
         * pose is a generic screen-normalized pointer, and passing NULL here
         * disabled the bridge's pose thread outright (omni_bridge.c only
         * creates it when get_pose is non-NULL), so Omni-Track could not reach
         * this board at all. It self-gates: no live loader -> the pose fn
         * returns 0 and the bridge falls back to the gamepad path exactly as
         * before. Either way the bridge only runs when a user opts in via
         * ULTRA_BRIDGE_ENABLE=1 (default off, matching every other Omni
         * service; no live game dump to verify against yet -- see
         * UNTESTED.txt). */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(ultra_pcplus_device_window, andamiro_pcplus_gun_pose);
ultra_pausemenu_install(ultra_pcplus_device_window);
        CreateThread(NULL, 0, worker, NULL, 0, NULL);   /* periodic re-patch only */
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();       /* release any held mouse buttons */
ultra_pausemenu_shutdown();
        ultra_pcplus_d3d9_teardown();   /* timeEndPeriod(1) if the limiter raised it */
        if (g_shared) { UnmapViewOfFile(g_shared); g_shared = NULL; }
        if (g_map)    { CloseHandle(g_map); g_map = NULL; }
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader/diagnostics can confirm the right DLL was
 * injected (mirrors the shared Type X hook's ultra_typex_hook_id()). */
__declspec(dllexport) const char* ultra_andamiro_pcplus_hook_id(void) {
    return "Ultra Loader Andamiro PC+ I/O hook (clean-room JVS + DirectInput8, board-own build) v1";
}

#endif /* _WIN32 */
