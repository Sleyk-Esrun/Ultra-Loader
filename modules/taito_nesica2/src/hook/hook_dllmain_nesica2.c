/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_nesica2.c -- entry point for Taito NESiCAxLive2's own,
 *                           independent hook DLL (ultra_taito_nesica2_
 *                           hook32.dll / ...hook64.dll, built ONLY from this
 *                           board's own src/hook tree).
 * =============================================================================
 *
 *  Injected into the NLX2 game by this module's own
 *  src/taito_nesica2_main.c (CreateProcess CREATE_SUSPENDED + LoadLibraryA /
 *  ultra-inject64.exe helper inject + ResumeThread). On attach it,
 *  synchronously (the module injects us into the SUSPENDED game and resumes
 *  right after, so hooks must be in place before the game's first thread
 *  runs):
 *    - creates the one JVS node this process owns (jvs_core_nesica2.c) --
 *      the canonical cabinet input/coin model every other subsystem reads,
 *    - installs the JVS-over-serial (COM2/COM3) transport shim -- this
 *      board's actual cabinet I/O transport (jvs_serial_hook_nesica2.c),
 *    - installs BOTH the D3D11 present hook (d3d11_throttle_nesica2.c,
 *      NLX2's PRIMARY render path) and the D3D9 present hook
 *      (d3d9_throttle_nesica2.c, fall-through for the rare 32-bit repack)
 *      -- "first wins": whichever API a given title actually calls fires,
 *      the other is a clean no-op IAT patch that never triggers,
 *    - installs the DirectSound + XAudio2 diagnostic hook
 *      (audio_hook_nesica2.c),
 *    - installs the NESiCA card-reader PRESENCE stub (card_reader_nesica2.c)
 *      -- OFF unless ULTRA_CARD_READER=1 (taito_nesica2_main.c now sets this
 *      default; ethical DEFER preserved -- presence/idle only, never
 *      online-auth/decrypt),
 *    - starts a standalone XInput + keyboard input reader so the JVS node
 *      reports real cabinet state even with no loader-side shared-memory
 *      feed (this board's own taito_nesica2_main.c does not set up an IPC
 *      block today -- same deliberate choice the taito_typex3 /
 *      taito_typezero independent builds already made),
 *  then starts a short re-patch worker to catch any module the game loads
 *  fractionally after our first pass (delay-loaded imports).
 *
 *  This TU also owns the two generic Win32 mechanisms every sibling TU in
 *  this DLL needs: tn2_log() (an optional-file diagnostic logger, gated by
 *  ULTRA_JVS_LOG) and tn2_iat_patch_all() (an IAT-name-and-value patcher,
 *  bounds-checked against each module's mapped image size since mingw has no
 *  SEH __try/__except). Written fresh for this board; no shared source file,
 *  no shared compiled binary with any other board's hook DLL (including
 *  taito_nesica, the gen-1 sibling -- a SEPARATE board being handled
 *  elsewhere).
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "taito_nesica2_hook.h"
#include "omni_bridge.h"

#include "ultra_transport.h"   /* shared universal I/O transport layer */
#include "ultra_tracer.h"
/* -------------------------------------------------------------------------- */
static ultra_jvs_node* g_node = NULL;
static volatile LONG    g_stop = 0;

ultra_jvs_node* tn2_hook_node(void) { return g_node; }

/* ---- standalone gamepad (XInput) + keyboard -> JVS ------------------------
 * This independent build has no loader shared-memory IPC wired (see file
 * header), so the hook reads a physical controller / keyboard itself --
 * same rationale as the shared multi-board hook's own standalone fallback
 * path, and the same choice the taito_typex3 / taito_typezero independent
 * builds already made for their own boards.
 *
 * Keyboard mapping (test/attract-friendly defaults, all standard VKs):
 *   1 = P1 START, 5 = P1 COIN, F2 = TEST, F1 = SERVICE, Z/X/C/V = PUSH1..4
 * XInput mapping (pad 0): D-pad/stick = P1 UP/DOWN/LEFT/RIGHT,
 *   A/B/X/Y = PUSH1..4, Start = START, Back = COIN (edge-triggered),
 *   LB = SERVICE. */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } tn2_xpad;
typedef struct { DWORD packet; tn2_xpad pad; } tn2_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, tn2_xstate*);
static p_xinput_get    s_xget = NULL;
static ultra_jvs_input g_xinput_in;
static volatile LONG   g_local_coin = 0;
static long            g_local_coin_seen = 0;

static DWORD WINAPI tn2_input_thread(LPVOID arg) {
    (void)arg;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
    for (int i = 0; dlls[i] && !s_xget; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) s_xget = (p_xinput_get)(void*)GetProcAddress(h, "XInputGetState");
    }
    tn2_log("input: XInput %s", s_xget ? "available" : "unavailable (keyboard-only fallback)");
    for (int a = 0; a < JVS_MAX_ANALOG; ++a) g_xinput_in.analog[a] = 0x8000;
    g_xinput_in.screen_x = g_xinput_in.screen_y = 0x8000;

    int coin_was = 0;
    while (!g_stop) {
        uint8_t byte1 = 0, byte2 = 0, sysb = 0;
        uint16_t ax0 = 0x8000, ax1 = 0x8000;
        int coin_now = 0;

        /* keyboard fallback (works even with no gamepad plugged in) */
        if (GetAsyncKeyState('1')   & 0x8000) byte1 |= 0x80;   /* START   */
        if (GetAsyncKeyState(VK_F1) & 0x8000) byte1 |= 0x40;   /* SERVICE */
        if (GetAsyncKeyState(VK_F2) & 0x8000) sysb  |= 0x80;   /* TEST    */
        if (GetAsyncKeyState('5')   & 0x8000) coin_now = 1;    /* COIN    */
        if (GetAsyncKeyState(VK_UP)    & 0x8000) byte1 |= 0x20;
        if (GetAsyncKeyState(VK_DOWN)  & 0x8000) byte1 |= 0x10;
        if (GetAsyncKeyState(VK_LEFT)  & 0x8000) byte1 |= 0x08;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) byte1 |= 0x04;
        if (GetAsyncKeyState('Z') & 0x8000) byte1 |= 0x02;     /* PUSH1 */
        if (GetAsyncKeyState('X') & 0x8000) byte1 |= 0x01;     /* PUSH2 */
        if (GetAsyncKeyState('C') & 0x8000) byte2 |= 0x80;     /* PUSH3 */
        if (GetAsyncKeyState('V') & 0x8000) byte2 |= 0x40;     /* PUSH4 */

        if (s_xget) {
            tn2_xstate st; memset(&st, 0, sizeof st);
            if (s_xget(0, &st) == 0) {
                WORD b = st.pad.wButtons;
                if (b & 0x0001) byte1 |= 0x20;                 /* DPAD_UP    */
                if (b & 0x0002) byte1 |= 0x10;                 /* DPAD_DOWN  */
                if (b & 0x0004) byte1 |= 0x08;                 /* DPAD_LEFT  */
                if (b & 0x0008) byte1 |= 0x04;                 /* DPAD_RIGHT */
                if (b & 0x0010) byte1 |= 0x80;                 /* START      */
                if (b & 0x0100) byte1 |= 0x40;                 /* LB->SERVICE*/
                if (b & 0x0020) sysb  |= 0x80;                 /* BACK->TEST */
                if (b & 0x1000) byte1 |= 0x02;                 /* A->PUSH1   */
                if (b & 0x2000) byte1 |= 0x01;                 /* B->PUSH2   */
                if (b & 0x4000) byte2 |= 0x80;                 /* X->PUSH3   */
                if (b & 0x8000) byte2 |= 0x40;                 /* Y->PUSH4   */
                if (st.pad.lx < -10000) byte1 |= 0x08; else if (st.pad.lx > 10000) byte1 |= 0x04;
                if (st.pad.ly >  10000) byte1 |= 0x20; else if (st.pad.ly < -10000) byte1 |= 0x10;
                ax0 = (uint16_t)((int)st.pad.lx + 0x8000);
                ax1 = (uint16_t)(0x7FFF - (int)st.pad.ly);
                if (st.pad.bRT > 128) coin_now = 1;            /* right trigger -> coin */
            }
        }

        ultra_jvs_input in; memset(&in, 0, sizeof in);
        in.system    = sysb;
        in.player[0] = (uint16_t)((byte1 << 8) | byte2);
        in.analog[0] = ax0; in.analog[1] = ax1;
        for (int a = 2; a < JVS_MAX_ANALOG; ++a) in.analog[a] = 0x8000;
        in.screen_x = in.screen_y = 0x8000;
        g_xinput_in = in;

        if (coin_now && !coin_was) InterlockedIncrement(&g_local_coin);
        coin_was = coin_now;

        Sleep(8);   /* ~125 Hz poll */
    }
    return 0;
}
static void tn2_input_start(void) { CreateThread(NULL, 0, tn2_input_thread, NULL, 0, NULL); }

/* Snapshot the current input for the JVS engine. */
void tn2_current_input(ultra_jvs_input* out) {
    memset(out, 0, sizeof *out);
    for (int a = 0; a < JVS_MAX_ANALOG; ++a) out->analog[a] = 0x8000;
    out->screen_x = out->screen_y = 0x8000;
    *out = g_xinput_in;
}

/* Pull input into the JVS node + inject any coin edges. Called from
 * jvs_serial_hook_nesica2.c's WriteFile shim on every JVS poll. */
void tn2_hook_pull_input(ultra_jvs_node* n) {
    if (!n) return;
    ultra_jvs_input in;
    tn2_current_input(&in);
    jvs_set_input(n, &in);
    long lc = g_local_coin;
    if (lc != g_local_coin_seen) {
        /* Slot 0: COIN_DECREASE maps 1-based wire slot 1 -> our array index 0
         * (jvs_core_nesica2.c), so a credit inserted here is the one the
         * game spends. */
        int delta = (int)(lc - g_local_coin_seen);
        jvs_insert_coin(n, 0, delta);
        g_local_coin_seen = lc;
    }
}

/* ---- Ultra Omni-Bridge window accessor ------------------------------------
 * NLX2 is a mixed D3D9/D3D11 board (D3D11 primary for 64-bit titles, D3D9
 * fall-through for the rare 32-bit repack -- "first wins": whichever API a
 * given title actually calls captures its window, the other stays NULL).
 * This merges both into the single ultra_bridge_window_fn Ultra Omni-Bridge
 * (omni_bridge.c) needs; it stays NULL (bridge fully inert) until whichever
 * D3D hook actually fires captures a window. */
static HWND tn2_bridge_get_window(void) {
    HWND w = tn2_d3d11_window();
    if (w) return w;
    return tn2_d3d9_window();
}

/* -------------------------------------------------------------------------- */
static void configure_node(void) {
    g_node = jvs_create();
    if (!g_node) return;
    const char* id = getenv("ULTRA_JVS_ID");
    if (id && *id) jvs_set_id(g_node, id);
    int screenpos = 0;
    const char* sp = getenv("ULTRA_JVS_SCREENPOS");
    if (sp) screenpos = (sp[0] == '1');
    uint8_t analog_ch = 0;   /* 0 = keep node default (8) */
    const char* ach = getenv("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= JVS_MAX_ANALOG) analog_ch = (uint8_t)v; }
    jvs_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* ============================================================================
 *  Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 *  imports). The first install already happened synchronously in DllMain;
 *  every install() below is idempotent (name+value based patch), so calling
 *  again is safe.
 * ========================================================================= */
static DWORD WINAPI tn2_worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        tn2_jvs_serial_install();
        tn2_d3d11_install();
        tn2_d3d9_install();
        tn2_audio_install();
        tn2_card_reader_install();
    }
    return 0;
}

/* ---- crash diagnostics (VEH) ------------------------------------------------ */
static LONG WINAPI tn2_crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "(unknown)";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
#ifdef _WIN64
        tn2_log("CRASH: ACCESS_VIOLATION at RIP=%p in %s", addr, modname);
        tn2_log("CRASH: RAX=%p RBX=%p RCX=%p RDX=%p",
            (void*)ep->ContextRecord->Rax, (void*)ep->ContextRecord->Rbx,
            (void*)ep->ContextRecord->Rcx, (void*)ep->ContextRecord->Rdx);
#else
        tn2_log("CRASH: ACCESS_VIOLATION at EIP=%p in %s", addr, modname);
        tn2_log("CRASH: EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX",
            ep->ContextRecord->Eax, ep->ContextRecord->Ebx,
            ep->ContextRecord->Ecx, ep->ContextRecord->Edx);
        tn2_log("CRASH: ESP=%08lX EBP=%08lX ESI=%08lX EDI=%08lX",
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
            utxp.board        = "taito_nesica2";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        AddVectoredExceptionHandler(1, tn2_crash_handler);
        tn2_log("=== ultra_taito_nesica2_hook%s.dll attached (NESiCAxLive2-only, independent) pid=%lu ===",
#ifdef _WIN64
                "64",
#else
                "32",
#endif
                GetCurrentProcessId());
        /* Install SYNCHRONOUSLY: the module injects us into the SUSPENDED
         * game and resumes right after, so hooks must be in place before the
         * game's first thread runs. */
        configure_node();
        tn2_jvs_serial_install();
        tn2_d3d11_install();   /* PRIMARY: NLX2 is a D3D11 board */
        tn2_d3d9_install();    /* fall-through: rare 32-bit NLX2 repack */
        tn2_audio_install();
        tn2_card_reader_install();   /* OFF unless ULTRA_CARD_READER=1 */
        tn2_input_start();
        CreateThread(NULL, 0, tn2_worker, NULL, 0, NULL);
        /* Ultra Omni-Bridge (5th Omni service, opt-in via ULTRA_BRIDGE_ENABLE,
         * default OFF): synthesizes raw cursor/click/key input for an NLX2
         * title that reads OS input directly instead of JVS. Called after
         * both tn2_d3d11_install()/tn2_d3d9_install() above so the combined
         * window accessor has a live present hook to observe -- it just
         * returns NULL until a device/swap chain actually shows up, so this
         * is safe either way. NLX2 titles are all fighters (digital stick +
         * buttons, no lightgun peripheral and no <board>_gun_bridge.c-style
         * pose source), so get_pose is NULL -- gamepad-to-cursor only, same
         * choice made for every other stick/button-only board this framework
         * has been rolled out to (taito_nesica, taito_typezero, ...). */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(tn2_bridge_get_window, NULL);
ultra_pausemenu_install(tn2_bridge_get_window);
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();
ultra_pausemenu_shutdown();
        tn2_d3d11_teardown();
        tn2_d3d9_teardown();
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader/user can confirm the right (independent)
 * DLL was injected, distinct from any other board's hook (including the
 * shared modtest/ultra_typex_hook{32,64}.dll this module used to copy). */
__declspec(dllexport) const char* ultra_taito_nesica2_hook_id(void) {
    return "Ultra Loader Taito NESiCAxLive2 I/O hook (independent, clean-room) v1";
}

#endif /* _WIN32 */
