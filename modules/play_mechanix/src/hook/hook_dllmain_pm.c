/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_pm.c -- ultra_play_mechanix_hook{32,64}.dll entry point
 * =============================================================================
 *
 *  Injected into a Play Mechanix / Raw Thrills (Chicago PC) game by
 *  modules/play_mechanix/src/play_mechanix_main.c (CREATE_SUSPENDED spawn +
 *  QueueUserAPC/CreateRemoteThread LoadLibrary inject, unchanged -- this hook
 *  is a drop-in for the board-native hook path play_mechanix_main.c already
 *  searches for by name).
 *
 *  On attach it, synchronously (the module injects us into the SUSPENDED game
 *  and resumes right after, so hooks must be in place before the game's first
 *  thread runs):
 *    - opens the shared-memory input block the loader may have created
 *      (ULTRA_IO_SHM) -- absent for a standalone run, which is fine,
 *    - creates one clean-room JVS node with the lightgun screen-position
 *      feature ALWAYS on (this board is a lightgun cabinet by definition),
 *    - installs the JVS-over-serial transport, the D3D9 present hook, the
 *      audio diagnostic hook, and the lightgun/standalone-input bridge,
 *    - re-applies the IAT patches for a short window so DLLs the game loads
 *      late (delay-loaded imports) are caught too.
 *
 *  Fully independent of every other board's hook DLL: no shared source file,
 *  no shared compiled binary, per the project owner's standing directive.
 *  Clean-room: Ultra Loader's own work over the public Win32 API + the open
 *  JVS spec.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "jvs_core_pm.h"
#include "ultra_io_shared_pm.h"
#include "pm_hook.h"
#include "omni_bridge.h"

#include "ultra_transport.h"   /* shared universal I/O transport layer */
#include "ultra_tracer.h"
/* -------------------------------------------------------------------------- */
static ultra_pm_jvs_node* g_node   = NULL;
static HANDLE             g_map    = NULL;
static ultra_io_shared_pm* g_shared = NULL;
static uint32_t           g_coin_seen[ULTRA_IO_COINS] = {0};
static long               g_local_coin_seen = 0;
static volatile LONG      g_stop   = 0;

ultra_pm_jvs_node* pm_hook_node(void)   { return g_node; }
void*              pm_hook_shared(void) { return g_shared; }

/* Pull input into the JVS node + drain coin edges (loader shared block AND/OR
 * the gun bridge's standalone keyboard coin taps -- whichever this run uses;
 * the inactive path's counter simply never moves, so merging both is safe). */
void pm_hook_pull_input(ultra_pm_jvs_node* n) {
    if (!n) return;
    ultra_pm_jvs_input in;
    pm_gun_bridge_current(&in);
    jvs_pm_set_input(n, &in);

    ultra_io_shared_pm* s = g_shared;
    if (s && s->magic == ULTRA_IO_MAGIC) {
        for (int c = 0; c < ULTRA_IO_COINS; ++c) {
            uint32_t pulse = s->coin_pulse[c];
            if (pulse != g_coin_seen[c]) {
                int delta = (int)(pulse - g_coin_seen[c]);
                jvs_pm_insert_coin(n, c, delta);
                g_coin_seen[c] = pulse; s->coin_ack[c] = pulse;
            }
        }
        s->jvs_address = jvs_pm_address(n); s->poll_count++;
    }
    long lc = pm_gun_bridge_coin_total();
    if (lc != g_local_coin_seen) {
        /* Credit the first chute (slot 0); COIN_DECREASE slot=1 maps to
         * coin[0] via the 1-based->0-based fix in jvs_core_pm.c, so a credit
         * inserted here is the one the game spends. */
        int delta = (int)(lc - g_local_coin_seen);
        jvs_pm_insert_coin(n, 0, delta);
        g_local_coin_seen = lc;
    }
}

/* -------------------------------------------------------------------------- */
static void open_shared(void) {
    const char* name = getenv(ULTRA_IO_ENVVAR);
    if (!name || !*name) return;
    g_map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!g_map) return;
    g_shared = (ultra_io_shared_pm*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0,
                                                  sizeof(ultra_io_shared_pm));
    if (g_shared && g_shared->magic == ULTRA_IO_MAGIC)
        g_shared->flags |= ULTRA_IO_FLAG_HOOK_ATTACHED;
}

static void configure_node(void) {
    g_node = jvs_pm_create();
    if (!g_node) return;
    const char* id = getenv("ULTRA_JVS_ID");
    if (id && *id) jvs_pm_set_id(g_node, id);
    /* This board IS a lightgun cabinet -- screen position is ON by default
     * (unlike a generic multi-board hook where it's opt-in). ULTRA_JVS_SCREENPOS=0
     * can still force it off for a diagnostic run. */
    int screenpos = 1;
    const char* sp = getenv("ULTRA_JVS_SCREENPOS");
    if (sp && *sp) screenpos = (sp[0] == '1');
    uint8_t analog_ch = 0;   /* 0 = keep node default (8) */
    const char* ach = getenv("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= JVS_MAX_ANALOG) analog_ch = (uint8_t)v; }
    jvs_pm_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
    pm_log("[play_mechanix] JVS node configured (screen_pos=%d)", screenpos);
}

/* ---- Ultra Omni-Bridge (5th Omni service) wiring --------------------------
 * Window getter: the D3D9 hook's own device-window global (armed the moment
 * CreateDevice fires; NULL until then, which the bridge treats as "stay
 * inert").
 *
 * get_pose is play_mechanix_gun_pose (pm_gun_bridge.c) -- this is a LIGHTGUN
 * cabinet, so the Omni-Track pose lane must be live; NULL there would disable
 * the gun path entirely and leave only the gamepad->cursor path, which is how
 * Omni-Track ended up unable to reach this board at all.
 *
 * An earlier comment here claimed a pose fn would put "a second thread
 * fighting the same cursor" against pm_gun_bridge.c. That is NOT true of this
 * lane, on the code: pm_gun_bridge.c never WRITES the OS cursor -- it only
 * READS it (GetCursorPos, in mouse_gun_snapshot()), and only on the branch
 * where shared_gun_snapshot() already returned 0. Omni-Bridge's gun_thread is
 * the sole SetCursorPos writer and only acts when the same pose returns 1. The
 * two are therefore mutually exclusive by construction; exactly one of them
 * touches the cursor at any moment.
 *
 * The two lanes are complementary, not duplicated:
 *   - pm_gun_bridge.c feeds the pose INTO JVS (screen_x/screen_y answered on
 *     JVS_CMD_READ_SCRPOS + the PUSH1 trigger bit) -- the path a real cabinet
 *     game reads.
 *   - Omni-Bridge feeds the same pose OUT to the OS cursor + left button, so a
 *     title that aims by mouse instead of JVS is also driven.
 * Consequence to be aware of on a live playtest: one physical trigger pull now
 * produces BOTH the JVS PUSH1 bit and an OS left-click. Harmless for a
 * JVS-reading title (this board's design assumption), but a title that reads
 * the OS mouse *as well* would see a double input. Cannot be settled
 * statically -- flag it if a catalog title double-fires.
 *
 * Off by default either way (omni_client_read_bridge() defaults
 * ULTRA_BRIDGE_ENABLE=0); a user opts in per game once verified, matching
 * every other Omni service here. */
static HWND pm_bridge_get_window(void) { return pm_d3d9_get_window(); }
/* pm_gun_bridge.c -- signature is exactly ultra_bridge_pose_fn. Declared here
 * rather than in pm_hook.h to keep the diff to the wiring itself. */
extern int play_mechanix_gun_pose(int* out_x, int* out_y, int* out_trig);

/* Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 * imports). The first install already happened synchronously in DllMain. */
static DWORD WINAPI pm_worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        pm_jvs_serial_install();   /* idempotent (name + value based) */
    }
    return 0;
}

/* ---- crash diagnostics (VEH) ----------------------------------------------- */
static LONG WINAPI pm_crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "(unknown)";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
        pm_log("[play_mechanix] CRASH: ACCESS_VIOLATION at EIP=%p in %s", addr, modname);
#ifndef _WIN64
        pm_log("[play_mechanix] CRASH: EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX",
            ep->ContextRecord->Eax, ep->ContextRecord->Ebx,
            ep->ContextRecord->Ecx, ep->ContextRecord->Edx);
        pm_log("[play_mechanix] CRASH: ESP=%08lX EBP=%08lX ESI=%08lX EDI=%08lX",
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
            utxp.board        = "play_mechanix";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        AddVectoredExceptionHandler(1, pm_crash_handler);
        pm_log("[play_mechanix] === ultra_play_mechanix_hook.dll attached (independent) ===");
        /* Install SYNCHRONOUSLY: the module injects us into the SUSPENDED
         * game and resumes right after, so hooks must be in place before the
         * game's first thread runs (else its first CreateFile(COM) is missed). */
        open_shared();
        configure_node();
        pm_jvs_serial_install();
        pm_d3d9_install();
        pm_audio_install();
        pm_gun_bridge_install();
        /* After the D3D9 present hook (pm_d3d9_install) so pm_bridge_get_window()
         * has a real hook armed to eventually report a window from, and after
         * pm_gun_bridge_install() so the pose source is running -- see the
         * comment on pm_bridge_get_window() above. */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(pm_bridge_get_window, play_mechanix_gun_pose);
ultra_pausemenu_install(pm_bridge_get_window);
        CreateThread(NULL, 0, pm_worker, NULL, 0, NULL);
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();
ultra_pausemenu_shutdown();
        pm_gun_bridge_stop();
        pm_d3d9_teardown();
        if (g_shared) { UnmapViewOfFile(g_shared); g_shared = NULL; }
        if (g_map)    { CloseHandle(g_map); g_map = NULL; }
        if (g_node)   { jvs_pm_destroy(g_node); g_node = NULL; }
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader/user can confirm the right DLL was
 * injected (and that it is the independent Play Mechanix-only build). */
__declspec(dllexport) const char* ultra_play_mechanix_hook_id(void) {
    return "Ultra Loader Play Mechanix / Raw Thrills lightgun I/O hook (independent, clean-room) v1";
}

#endif /* _WIN32 */
