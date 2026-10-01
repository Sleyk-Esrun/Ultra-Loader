/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_igs.c  --  ultra_igs_pc_hook{32,64}.dll entry point
 * =============================================================================
 *
 *  PROVENANCE: forked (and trimmed) from modules/native_x86/src/hook/
 *  hook_dllmain.c into IGS PC's own independent hook tree. See igs_hook.h for
 *  the full rationale -- this file shares NO source file and NO compiled
 *  binary with the multi-board hook or any other board's per-board hook.
 *
 *  Injected into an IGS PC game by modules/igs_pc/src/igs_pc_main.c. On attach
 *  it:
 *    - opens the shared-memory input block the loader created (ULTRA_IO_SHM),
 *    - creates one clean-room JVS node,
 *    - installs the serial-transport hook, the synthetic DirectInput8 device,
 *      the D3D9 present hook, the DirectSound/XAudio2 diagnostic hook, and the
 *      MessageBox auto-dismiss,
 *    - starts a standalone XInput-fed fallback thread so a direct/standalone
 *      test run (no loader driving the shared block) is still playable,
 *    - re-applies the IAT patches for a short window so DLLs the game loads
 *      late (delay-loaded imports, plugins) are caught too.
 *
 *  Not ported from the shared multi-board hook (no evidence any IGS PC title
 *  needs it yet -- framework-only, no dump per docs/STOCK_TAKE.md; add later
 *  once a real dump proves otherwise, following the same minimal/auditable
 *  discipline Cave PC's fork established):
 *    - board_io_ops.h per-game seam / any game_*.c file
 *    - D3D8/D3D11/OpenGL present hooks (D3D9 only, matching the Type X posture
 *      this board's I/O is modeled on)
 *    - Namco ES3 USB dongle bypass, Sega/Konami/Taito board I/O shims
 *    - the shared loader pause-menu overlay wiring
 *    - per-game autopilot scripts (no known IGS PC title to script against)
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 API + the open
 *  JVS spec. No game code, no third-party loader code.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "igs_hook.h"
#include "ultra_io_shared_igs.h"
#include "omni_bridge.h"

#include "ultra_transport.h"   /* shared universal I/O transport layer */
#include "ultra_tracer.h"
/* -------------------------------------------------------------------------- */
static ultra_jvs_node*    g_node   = NULL;
static HANDLE             g_map    = NULL;
static ultra_io_shared*   g_shared = NULL;
static uint32_t           g_coin_seen[ULTRA_IO_COINS] = {0};
static volatile LONG      g_stop   = 0;

ultra_jvs_node* igs_hook_node(void) { return g_node; }   /* created before any hook can fire */

/* ---- standalone gamepad (XInput) ------------------------------------------
 * When no loader is feeding the shared block (e.g. a direct/standalone test
 * launch), the hook reads the player's gamepad itself so the game is actually
 * playable. XInput is loaded dynamically (no hard dependency on a given
 * version). */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } ul_xpad;
typedef struct { DWORD packet; ul_xpad pad; } ul_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, ul_xstate*);
static p_xinput_get   s_xget = NULL;
static ultra_jvs_input g_xinput_in;          /* updated by the XInput thread */
static volatile LONG   g_local_coin = 0;     /* coin pulses from the gamepad */
static long            g_local_coin_seen = 0;

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
    if (!s_xget) return 0;
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
static void igs_xinput_start(void) { CreateThread(NULL, 0, xinput_thread, NULL, 0, NULL); }

/* The input the game should see: the loader's shared block when a loader
 * drives it, otherwise the local gamepad (XInput). Used by the JVS engine AND
 * the DirectInput8 emulation so both report the same controller state. */
void igs_hook_current_input(ultra_jvs_input* out) {
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
void igs_hook_pull_input(ultra_jvs_node* n) {
    if (!n) return;
    ultra_jvs_input in;
    igs_hook_current_input(&in);
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
        /* Credit the first chute (slot 0). The game READ_COIN reads slot 0 and
         * its COIN_DECREASE slot=1 maps to coin[0] via the 1-based->0-based fix
         * in jvs_core_igs.c, so a credit inserted here is the one the game spends. */
        int delta = (int)(lc - g_local_coin_seen);
        jvs_insert_coin(n, 0, delta);
        g_local_coin_seen = lc;
    }
}

/* ---- Ultra Omni-Track lightgun pose ---------------------------------------
 * A coherent snapshot of the loader's seqlocked aiming pose
 * (screen_x/screen_y/player[0]) in exactly the shape ultra_bridge_pose_fn
 * wants: x/y 0..65535 screen-normalized, trigger 0/1. This is what lets the
 * Omni-Bridge aim the OS cursor from a real lightgun instead of only from a
 * gamepad stick. g_shared is already in scope here, so no accessor is needed.
 *
 * Deliberately STRICTER than igs_hook_current_input() above, in two ways:
 *
 *  1. LOADER_ALIVE gate. igs_hook_current_input() checks only the magic,
 *     because the JVS node must keep answering polls with *something* even if
 *     the loader died -- a stale button mask is harmless. A stale POSE is not.
 *     The loader clears ULTRA_IO_FLAG_LOADER_ALIVE when it exits (or when the
 *     gun goes inactive); if we kept reporting the last pose, the bridge would
 *     warp the OS cursor to a dead gun's final aim point ~120x a second --
 *     fighting the player's real mouse -- and, if the trigger bit happened to
 *     be set in that last sample, hold the shot down forever. So a dead loader
 *     must mean "no pose" (return 0 -> the bridge falls back to the gamepad
 *     path / leaves the mouse alone), NEVER "last known pose".
 *
 *  2. No torn fallback. igs_hook_current_input() accepts a torn read rather
 *     than stall the JVS wire; here, a seqlock that never settles just means
 *     "no pose this tick". One skipped cursor update is invisible at 120 Hz,
 *     whereas a torn pose mixes X from frame N with the trigger from N+1. */
int igs_pc_gun_pose(int* out_x, int* out_y, int* out_trig) {
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
    if (!got) return 0;
    *out_x = sx; *out_y = sy;
    *out_trig = (p0 & 0x0200) != 0;   /* bit9 = PUSH1 = gun trigger (jvs_core_igs.h) */
    return 1;
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
    /* Advertised analog-channel count. Default 8; a driving/motion cabinet
     * polls more, so let the loader bump it via ULTRA_JVS_ANALOG so FEATCHK
     * matches the game's READ_ANALOG count. */
    uint8_t analog_ch = 0;   /* 0 = keep node default (8) */
    const char* ach = getenv("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= JVS_MAX_ANALOG) analog_ch = (uint8_t)v; }
    /* defaults (2p/13sw/2coin/8analog@16/2gpo) + optional light-gun feature */
    jvs_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 * imports, render/audio plugins). The first install already happened
 * synchronously in DllMain; this only re-points IAT slots periodically. */
static DWORD WINAPI igs_worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        igs_jvs_serial_install();   /* idempotent (name + value based) */
        igs_dinput8_install();      /* idempotent */
        igs_render9_install();      /* idempotent */
    }
    return 0;
}

/* ---- crash diagnostics (VEH) ------------------------------------------------ */
static LONG WINAPI igs_crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "(unknown)";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
#ifdef _WIN64
        igs_log("CRASH: ACCESS_VIOLATION at RIP=%p in %s", addr, modname);
        igs_log("CRASH: RAX=%p RBX=%p RCX=%p RDX=%p",
            (void*)ep->ContextRecord->Rax, (void*)ep->ContextRecord->Rbx,
            (void*)ep->ContextRecord->Rcx, (void*)ep->ContextRecord->Rdx);
#else
        igs_log("CRASH: ACCESS_VIOLATION at EIP=%p in %s", addr, modname);
        igs_log("CRASH: EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX",
            ep->ContextRecord->Eax, ep->ContextRecord->Ebx,
            ep->ContextRecord->Ecx, ep->ContextRecord->Edx);
        igs_log("CRASH: ESP=%08lX EBP=%08lX ESI=%08lX EDI=%08lX",
            ep->ContextRecord->Esp, ep->ContextRecord->Ebp,
            ep->ContextRecord->Esi, ep->ContextRecord->Edi);
#endif
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* -------------------------------------------------------------------------- */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        /* FIRST. The hook is injected into a SUSPENDED process, so this
         * runs before the game's entry point: the tracer therefore sees the
         * very first fault and the very first failed load. Observe-only -
         * it swallows no exception and changes no return value.
         * ULTRA_TRACE=0 disables. See work/include/ultra_tracer.h. */
        ultra_tracer_install();


    {   /* Universal transport layer: declare this board's shape so the shared
         * pacing/range/codec core is live and self-describing in the log.
         * Serial boards are REQUEST/RESPONSE -- their reply queue already ends
         * the game's read loop, so pacing is deliberately inert here; injecting
         * empty reads would truncate a reply frame. Behavior is unchanged. */
        static int s_utx_once;
        if (!s_utx_once) { static utx_profile utxp;
            s_utx_once = 1;
            memset(&utxp, 0, sizeof utxp);
            utxp.board        = "igs_pc";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        AddVectoredExceptionHandler(1, igs_crash_handler);
        igs_log("=== ultra_igs_pc_hook.dll attached (IGS-PC-only, independent) ===");
        /* Install SYNCHRONOUSLY here: the module injects us into the SUSPENDED
         * game and resumes right after, so the hooks must be in place before
         * the game's first thread runs (else its first CreateFile(COM) is
         * missed). */
        open_shared();
        configure_node();
        igs_jvs_serial_install();
        igs_dinput8_install();      /* synthetic DInput8 device */
        igs_render9_install();      /* D3D9 present hook (static-import path) */
        igs_audio_install();        /* DirectSound + XAudio2 diagnostic hook */
        igs_dialog_install();       /* MessageBox auto-dismiss (env-gated) */
        igs_xinput_start();         /* standalone gamepad fallback */
        /* Ultra Omni-Bridge: raw OS cursor/click/key synthesis for a dump that
         * reads keyboard+mouse directly instead of JVS/DirectInput (see
         * omni_bridge.h). Installed after igs_render9_install() above, which
         * is what arms the D3D9 present hook that igs_device_window() reads --
         * must be armed first so the window accessor has something to return.
         * get_pose is igs_pc_gun_pose(): the Ultra Omni-Track lightgun pose out
         * of this board's own shared block, so a real gun aims the cursor and
         * a gamepad only fills in when no live pose is present. It reports "no
         * pose" (rather than the last one) whenever the loader is gone -- see
         * the LOADER_ALIVE note on igs_pc_gun_pose() above. This board is still
         * framework-only (no known IGS PC dump per docs/STOCK_TAKE.md) and is
         * modeled as a generic JVS cabinet, so the gun path here is unexercised
         * against a real title -- but it is now WIRED rather than disabled, and
         * costs nothing when no gun is mapped. The bridge as a whole still only
         * runs when a user opts in via ULTRA_BRIDGE_ENABLE=1 (default off,
         * matching every other Omni service; see UNTESTED.txt). */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(igs_device_window, igs_pc_gun_pose);
ultra_pausemenu_install(igs_device_window);
        CreateThread(NULL, 0, igs_worker, NULL, 0, NULL);   /* periodic re-patch */
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();  /* release any held mouse buttons */
ultra_pausemenu_shutdown();
        igs_render9_teardown();   /* timeEndPeriod(1) if the limiter raised it */
        if (g_shared) { UnmapViewOfFile(g_shared); g_shared = NULL; }
        if (g_map)    { CloseHandle(g_map); g_map = NULL; }
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader/user can confirm the right DLL was
 * injected (and that it is the independent IGS PC build, not the shared
 * Type X hook). */
__declspec(dllexport) const char* ultra_igs_pc_hook_id(void) {
    return "Ultra Loader IGS PC I/O hook (independent, clean-room) v1";
}

#endif /* _WIN32 */
