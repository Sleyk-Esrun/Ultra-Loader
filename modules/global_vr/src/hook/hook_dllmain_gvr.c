/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_gvr.c  --  ultra_global_vr_hook{32,64}.dll entry point
 * =============================================================================
 *
 *  Injected into the Global VR PC-arcade game process by global_vr_main.c
 *  (QueueUserAPC(LoadLibraryA) primary path, CreateRemoteThread(LoadLibraryA)
 *  fallback -- see that file). On DLL_PROCESS_ATTACH it:
 *    - opens the shared-memory input block Ultra Loader created (ULTRA_IO_SHM),
 *      if any -- standalone runs fall back to a local XInput gamepad,
 *    - creates one clean-room JVS node (jvs_core_gvr.c),
 *    - installs the serial-transport hook (jvs_serial_hook_gvr.c: JVS-over-COM
 *      + the I/O board's watchdog heartbeat + Hasp dongle presence bypass),
 *    - installs the synthetic DirectInput8 device (dinput8_emu_gvr.c),
 *    - arms the D3D9 present hook (d3d9_throttle_gvr.c),
 *    - arms the DirectSound/XAudio2 diagnostic hook (audio_hook_gvr.c),
 *    - starts the standalone-gamepad fallback thread.
 *
 *  The serial hook and the DirectInput8 emulation both call
 *  ultra_gvr_hook_node() / ultra_gvr_hook_current_input() here to get the node
 *  and the latest controller state, so JVS and DirectInput8 always agree on
 *  what the player is doing.
 *
 *  No game code, no third-party loader code -- Ultra Loader's own work over
 *  the public Win32 API + the open JVS spec. This TU (and every other file in
 *  modules/global_vr/src/hook/) shares no object code with native_x86 or any
 *  other board's module.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "global_vr_hook.h"
#include "ultra_transport.h"   /* shared universal I/O transport layer */
#include "omni_bridge.h"   /* Ultra Omni-Bridge (5th Omni service), OFF by default */

/* -------------------------------------------------------------------------- */
static ultra_jvs_node*   g_node   = NULL;
static HANDLE            g_map    = NULL;
static ultra_io_shared*  g_shared = NULL;
static uint32_t          g_coin_seen[ULTRA_IO_COINS] = {0};
static volatile LONG     g_stop   = 0;

ultra_jvs_node* ultra_gvr_hook_node(void) { return g_node; }
ultra_io_shared* ultra_gvr_hook_shared(void) { return g_shared; }

/* ---- Ultra Omni-Track lightgun pose source (for the Omni-Bridge) -----------
 * Coherent snapshot of the loader's seqlocked aiming pose
 * (screen_x/screen_y/player[0]). The loader writes those three fields inside an
 * odd/even seq window, so reading them raw can latch a TORN pose (X from frame
 * N with the trigger from frame N+1) -- mirror ultra_gvr_hook_current_input()'s
 * bounded retry loop instead.
 *
 * The signature is exactly ultra_bridge_pose_fn (int*,int*,int*)->0/1, so this
 * drops straight into ultra_omni_bridge_install() as the get_pose argument.
 * x/y come out 0..65535 screen-normalized; the trigger is JVS PUSH1 (bit9,
 * mask 0x0200 -- see jvs_core_gvr.h), which is the bit the loader sets for a
 * gun trigger.
 *
 * WHY THE LOADER_ALIVE GATE: magic alone only proves the mapping still exists,
 * not that anyone is still writing it. A loader that exited (or whose gun went
 * inactive) clears ULTRA_IO_FLAG_LOADER_ALIVE but leaves the last-written pose
 * sitting in the block forever. Trusting that stale pose would make the bridge
 * warp the cursor to wherever the gun last pointed and re-fire a latched
 * trigger, 120x/sec, with nobody holding the gun. "No live loader" must
 * therefore mean "NO pose" (return 0 -> the bridge falls back to the gamepad
 * path and leaves a real mouse alone), never "last known pose". Nothing else in
 * this hook reads this flag, so the gate has to live here. */
int ultra_gvr_gun_pose(int* out_x, int* out_y, int* out_trig) {
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
    if (!got) return 0;            /* never hand back a torn pose */
    *out_x = sx; *out_y = sy; *out_trig = (p0 & 0x0200) != 0;   /* PUSH1 */
    return 1;
}

/* ---- standalone gamepad (XInput) ------------------------------------------
 * When no loader is feeding the shared block (e.g. a standalone test launch),
 * the hook reads the player's gamepad itself so the board is actually
 * playable. XInput is loaded dynamically (no hard dependency on a version). */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } ul_xpad;
typedef struct { DWORD packet; ul_xpad pad; } ul_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, ul_xstate*);
static p_xinput_get    s_xget = NULL;
static ultra_jvs_input g_xinput_in;          /* updated by the gamepad thread */
static volatile LONG   g_local_coin = 0;     /* coin pulses from the gamepad  */
static long            g_local_coin_seen = 0;

void ultra_gvr_hook_set_local_input(const ultra_jvs_input* in) { if (in) g_xinput_in = *in; }
void ultra_gvr_hook_local_coin_insert(int slot) {
    if (slot == 0) InterlockedIncrement(&g_local_coin);
}

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
    for (int a = 0; a < JVS_MAX_ANALOG; ++a) g_xinput_in.analog[a] = 0x8000;
    g_xinput_in.screen_x = g_xinput_in.screen_y = 0x8000;
    if (!s_xget) return 0;   /* no XInput available -- shared block (if any) still works */

    int coin_was = 0;
    while (!g_stop) {
        ul_xstate st; memset(&st, 0, sizeof st);
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
            ultra_jvs_input in; memset(&in, 0, sizeof in);
            in.player[0] = (uint16_t)((byte1<<8)|byte2);
            in.system    = test ? 0x80 : 0;        /* TEST switch */
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
static void ultra_xinput_start(void) { CreateThread(NULL, 0, xinput_thread, NULL, 0, NULL); }

/* The input the game should see: the loader's shared block when a loader is
 * driving it, otherwise the local gamepad. Used by the JVS engine AND the
 * DirectInput8 emulation so both report the same controller state. */
void ultra_gvr_hook_current_input(ultra_jvs_input* out) {
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
        *out = g_xinput_in;                        /* standalone gamepad */
    }
}

/* Pull input into the JVS node + inject coin edges (shared block + gamepad). */
void ultra_gvr_hook_pull_input(ultra_jvs_node* n) {
    if (!n) return;
    ultra_jvs_input in;
    ultra_gvr_hook_current_input(&in);
    jvs_set_input(n, &in);
    ultra_io_shared* s = g_shared;
    if (s && s->magic == ULTRA_IO_MAGIC) {
        for (int c = 0; c < ULTRA_IO_COINS; ++c) {
            uint32_t pulse = s->coin_pulse[c];
            if (pulse != g_coin_seen[c]) {
                int delta = (int)(pulse - g_coin_seen[c]);
                jvs_insert_coin(n, c, delta);
                g_coin_seen[c] = pulse; s->coin_ack[c] = pulse;
            }
        }
        s->jvs_address = jvs_address(n); s->poll_count++;
    }
    long lc = g_local_coin;                        /* gamepad coin (standalone) */
    if (lc != g_local_coin_seen) {
        /* Credit the first chute (slot 0). READ_COIN reads slot 0, and its
         * COIN_DECREASE slot=1 maps to coin[0] via the 1-based->0-based fix in
         * jvs_core_gvr.c, so a credit inserted here is the one the game spends. */
        int delta = (int)(lc - g_local_coin_seen);
        jvs_insert_coin(n, 0, delta);
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
    g_node = jvs_create();
    if (!g_node) return;
    const char* id = getenv("ULTRA_JVS_ID");
    if (id && *id) jvs_set_id(g_node, id);
    int screenpos = 0;
    const char* sp = getenv("ULTRA_JVS_SCREENPOS");
    if (sp) screenpos = (sp[0] == '1');
    /* Advertised analog-channel count. Default 8; a driving title (Ferrari F355
     * Challenge, NASCAR Team Racing) polls more, so let the loader bump it via
     * ULTRA_JVS_ANALOG so FEATCHK matches the game's READ_ANALOG count. */
    uint8_t analog_ch = 0;   /* 0 = keep node default (8) */
    const char* ach = getenv("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= JVS_MAX_ANALOG) analog_ch = (uint8_t)v; }
    jvs_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* Combined game-window getter: D3D9 titles set g_devwnd in CreateDevice;
 * OpenGL titles set it in SwapBuffers via WindowFromDC.  Bridge and pause
 * menu both need a HWND -- try whichever path is live. */
static HWND get_game_window(void) {
    HWND w = ultra_gvr_d3d9_window();
    if (!w) w = ultra_gvr_gl_window();
    return w;
}

/* Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 * imports, render/audio plugins). The first install already happened
 * synchronously in DllMain; this only re-points IAT slots periodically. */
static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        ultra_gvr_jvs_serial_install();   /* idempotent (name + value based) */
        ultra_gvr_dinput8_install();      /* idempotent */
        ultra_gvr_d3d9_install();         /* idempotent */
        ultra_gvr_gl_install();           /* idempotent: OpenGL SwapBuffers throttle */
        ultra_gvr_audio_install();        /* idempotent */
    }
    return 0;
}

/* ---- crash diagnostics (VEH) -----------------------------------------------
 * Cheap and safe to leave on unconditionally: only fires on an actual access
 * violation and just logs (gated on ULTRA_JVS_LOG), never alters control flow. */
static LONG WINAPI ultra_gvr_crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "(unknown)";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
#ifdef _WIN64
        ultra_gvr_log("[gvr-hook] CRASH: ACCESS_VIOLATION at RIP=%p in %s", addr, modname);
#else
        ultra_gvr_log("[gvr-hook] CRASH: ACCESS_VIOLATION at EIP=%p in %s", addr, modname);
#endif
    }
    return EXCEPTION_CONTINUE_SEARCH;
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
            utxp.board        = "global_vr";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        AddVectoredExceptionHandler(1, ultra_gvr_crash_handler);
        ultra_gvr_log("[gvr-hook] attach: ultra_global_vr_hook loaded pid=%lu (%d-bit)",
                      GetCurrentProcessId(), (int)(sizeof(void*) * 8));
        /* Install SYNCHRONOUSLY here: the module injects us into the SUSPENDED
         * game and resumes right after, so the hooks must be in place before
         * the game's first thread runs (else its first CreateFile(COM) or
         * Direct3DCreate9 call would be missed). */
        open_shared();
        configure_node();
        ultra_gvr_jvs_serial_install();
        ultra_gvr_dinput8_install();
        ultra_gvr_d3d9_install();
        ultra_gvr_gl_install();           /* OpenGL SwapBuffers throttle + window tracker */
        ultra_gvr_audio_install();
        ultra_xinput_start();                       /* standalone gamepad fallback */
        /* Ultra Omni-Bridge: OFF by default (ULTRA_BRIDGE_ENABLE).
         * get_game_window() covers both D3D9 and OpenGL titles so the bridge
         * can synthesize cursor/click events on the correct window regardless
         * of renderer.  get_pose = ultra_gvr_gun_pose() routes the loader's
         * lightgun track data to the bridge's aim synthesizer. */
        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));
        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(get_game_window, ultra_gvr_gun_pose);
        ultra_pausemenu_install(get_game_window);
        CreateThread(NULL, 0, worker, NULL, 0, NULL);   /* periodic re-patch only */
        ultra_gvr_log("[gvr-hook] all installers fired");
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();    /* releases any held mouse buttons */
        ultra_pausemenu_shutdown();
        ultra_gvr_d3d9_teardown();   /* timeEndPeriod(1) if the D3D9 limiter raised it */
        ultra_gvr_gl_teardown();     /* timeEndPeriod(1) if the GL limiter raised it */
        if (g_shared) { UnmapViewOfFile(g_shared); g_shared = NULL; }
        if (g_map)    { CloseHandle(g_map); g_map = NULL; }
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader/diagnostics can confirm the right DLL was
 * injected (not required by global_vr_main.c's inject contract -- it never
 * calls GetProcAddress on the hook -- but cheap and useful). */
__declspec(dllexport) const char* ultra_global_vr_hook_id(void) {
    return "Ultra Loader Global VR PC-arcade I/O hook (clean-room JVS) v1";
}

#endif /* _WIN32 */
