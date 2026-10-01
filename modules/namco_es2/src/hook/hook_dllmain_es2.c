/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_es2.c -- ultra_namco_es2_hook{32,64}.dll entry point
 * =============================================================================
 *
 *  Namco System ES2's own, fully independent I/O hook. Injected into a Namco System ES2
 *  game (suspended, resumed right after) by modules/namco_es2/src/
 *  namco_es2_main.c. No shared source file, no shared compiled binary with
 *  modules/native_x86/src/hook/'s multi-board hook or any other board's
 *  module -- see hook_es2.h for the full rationale.
 *
 *  On attach it installs, synchronously (the module injects us into the
 *  SUSPENDED game and resumes right after, so hooks must be in place before
 *  the game's first thread runs):
 *    - the JVS-over-serial path (jvs_serial_hook_es2.c -> jvs_core_es2.c)
 *    - the clean-room DirectInput8 emulation (dinput8_emu_es2.c)
 *    - the DirectSound/XAudio2 diagnostic hook (audio_hook_es2.c)
 *  (the D3D9 present/throttle hook arms itself the first time the game's own
 *  Direct3DCreate9 call is intercepted -- see d3d9_throttle_es2.c)
 *  then starts a short re-patch worker to catch any module the game loads
 *  fractionally after our first pass (delay-loaded imports).
 *
 *  Namco System ES2 has no real game dump on hand yet (framework-only per
 *  docs/STOCK_TAKE.md), so the local input source below is a plain, direct
 *  keyboard + XInput reader (no per-game profile system is needed or ported --
 *  that whole framework lives in modules/native_x86/src/hook/input_profiles.c
 *  and friends and was correctly left out of this board's independent reference
 *  set). Default mapping (standard MAME-like convention):
 *    Arrows = P1 dpad, LCtrl/LAlt/Space/LShift/Z/X = P1 buttons 1-6,
 *    1 = P1 start, F2 = test, Tab = service, 5/6 = coin slot 1/2.
 *  XInput controller 0 (if connected) drives the same fields: dpad + A/B/X/Y/
 *  LB/RB = buttons 1-6, Start = start, Back = service, RightShoulder = coin.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "hook_es2.h"
#include "omni_bridge.h"

#include "ultra_transport.h"   /* shared universal I/O transport layer */
#include "ultra_tracer.h"
/* -------------------------------------------------------------------------- */
/*  Shared diagnostic sink is defined in jvs_serial_hook_es2.c (ultra_es2_log) */
/* -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- */
/*  The one JVS node for this process                                          */
/* -------------------------------------------------------------------------- */
static ultra_jvs_node_es2* g_node = NULL;

ultra_jvs_node_es2* ultra_hook_node_es2(void) {
    if (!g_node) {
        g_node = jvs_es2_create();
        ultra_es2_log("JVS: node created (Namco System ES2 clean-room engine)");
    }
    return g_node;
}

/* -------------------------------------------------------------------------- */
/*  Local input source: keyboard + XInput controller 0                         */
/* -------------------------------------------------------------------------- */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } es2_xpad;
typedef struct { DWORD packet; es2_xpad pad; } es2_xstate;
typedef DWORD (WINAPI *pXInputGetState)(DWORD, es2_xstate*);

static pXInputGetState s_xget  = NULL;
static int             s_xtried = 0;

static void xinput_try_load(void) {
    if (s_xtried) return;
    s_xtried = 1;
    static const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
    for (int i = 0; dlls[i] && !s_xget; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) s_xget = (pXInputGetState)(void*)GetProcAddress(h, "XInputGetState");
    }
}

/* Reads the live local input (keyboard OR'd with XInput pad 0) into a plain
 * ultra_jvs_input_es2. Used by BOTH the JVS engine (via ultra_hook_pull_input_es2)
 * and the DirectInput8 emulation (via ultra_hook_current_input_es2) so both
 * transports agree on what the player is doing. */
void ultra_hook_current_input_es2(ultra_jvs_input_es2* out) {
    memset(out, 0, sizeof(*out));
    for (int a = 0; a < JVS_ES2_MAX_ANALOG; ++a) out->analog[a] = 0x8000;   /* centered */
    out->screen_x = out->screen_y = 0x8000;

    int test = 0, service = 0, start = 0;
    int up = 0, down = 0, left = 0, right = 0;
    int b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    int coin0 = 0, coin1 = 0;

    /* keyboard */
    test    |= (GetAsyncKeyState(VK_F2)     & 0x8000) != 0;
    service |= (GetAsyncKeyState(VK_TAB)    & 0x8000) != 0;
    start   |= (GetAsyncKeyState('1')       & 0x8000) != 0;
    up      |= (GetAsyncKeyState(VK_UP)     & 0x8000) != 0;
    down    |= (GetAsyncKeyState(VK_DOWN)   & 0x8000) != 0;
    left    |= (GetAsyncKeyState(VK_LEFT)   & 0x8000) != 0;
    right   |= (GetAsyncKeyState(VK_RIGHT)  & 0x8000) != 0;
    b1      |= (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0;
    b2      |= (GetAsyncKeyState(VK_LMENU)    & 0x8000) != 0;
    b3      |= (GetAsyncKeyState(VK_SPACE)    & 0x8000) != 0;
    b4      |= (GetAsyncKeyState(VK_LSHIFT)   & 0x8000) != 0;
    b5      |= (GetAsyncKeyState('Z')         & 0x8000) != 0;
    b6      |= (GetAsyncKeyState('X')         & 0x8000) != 0;
    coin0   |= (GetAsyncKeyState('5')         & 0x8000) != 0;
    coin1   |= (GetAsyncKeyState('6')         & 0x8000) != 0;

    /* XInput controller 0 (optional; OR'd in, never required) */
    xinput_try_load();
    if (s_xget) {
        es2_xstate st; memset(&st, 0, sizeof st);
        if (s_xget(0, &st) == 0) {
            const es2_xpad* p = &st.pad;
            WORD b = p->wButtons;
            up    |= (b & 0x0001) != 0;   /* DPAD_UP */
            down  |= (b & 0x0002) != 0;   /* DPAD_DOWN */
            left  |= (b & 0x0004) != 0;   /* DPAD_LEFT */
            right |= (b & 0x0008) != 0;   /* DPAD_RIGHT */
            start   |= (b & 0x0010) != 0; /* START */
            service |= (b & 0x0020) != 0; /* BACK */
            coin0   |= (b & 0x0200) != 0; /* RIGHT_SHOULDER */
            b1 |= (b & 0x1000) != 0;      /* A */
            b2 |= (b & 0x2000) != 0;      /* B */
            b3 |= (b & 0x4000) != 0;      /* X */
            b4 |= (b & 0x8000) != 0;      /* Y */
            b5 |= (b & 0x0100) != 0;      /* LEFT_SHOULDER */
            b6 |= (p->bRT > 30);          /* right trigger as button 6 */
            /* sticks feed the analog channels directly (left-justified,
             * 0x8000 = center) so a future wheel/analog-reading title still
             * gets real data even though digital dirs drive dpad games. */
            out->analog[0] = (uint16_t)(p->lx + 32768);
            out->analog[1] = (uint16_t)(-p->ly + 32767);
            out->analog[2] = (uint16_t)(p->rx + 32768);
            out->analog[3] = (uint16_t)(-p->ry + 32767);
        }
    }

    if (left)  out->analog[0] = 0x0000; else if (right) out->analog[0] = 0xFFFF;
    if (up)    out->analog[1] = 0x0000; else if (down)  out->analog[1] = 0xFFFF;

    out->system = (uint8_t)((test ? 0x80 : 0) | (0 /* tilt unused */));
    uint16_t sw = 0;
    if (start)   sw |= 0x8000;
    if (service) sw |= 0x4000;
    if (up)      sw |= 0x2000;
    if (down)    sw |= 0x1000;
    if (left)    sw |= 0x0800;
    if (right)   sw |= 0x0400;
    if (b1)      sw |= 0x0200;
    if (b2)      sw |= 0x0100;
    if (b3)      sw |= 0x0080;
    if (b4)      sw |= 0x0040;
    if (b5)      sw |= 0x0020;
    if (b6)      sw |= 0x0010;
    out->player[0] = sw;

    /* coin edge-detect + insert straight into the shared node (best-effort:
     * harmless if called before the node exists -- pull happens right after). */
    static int coin0_was = 0, coin1_was = 0;
    if (coin0 && !coin0_was && g_node) jvs_es2_insert_coin(g_node, 0, 1);
    if (coin1 && !coin1_was && g_node) jvs_es2_insert_coin(g_node, 1, 1);
    coin0_was = coin0; coin1_was = coin1;
}

void ultra_hook_pull_input_es2(ultra_jvs_node_es2* n) {
    if (!n) return;
    ultra_jvs_input_es2 in;
    ultra_hook_current_input_es2(&in);   /* also edge-detects+inserts coins above */
    jvs_es2_set_input(n, &in);
}

/* -------------------------------------------------------------------------- */
/*  Presence-only online/live-service auth probe (DEFERRED)                    */
/* -------------------------------------------------------------------------- */
static void es2_online_auth_presence_probe(void) {
    /* Namco System ES2 is a Chinese PC arcade board and prior project notes flag a
     * possible live-service/online-auth component for some titles on this
     * family. Per the project owner's standing instruction, that concept is
     * DEFERRED here: this only logs whether a module with a network/auth-
     * looking name is already loaded in the process -- it does NOT probe the
     * network, does NOT answer any handshake, and does NOT implement a bypass.
     * If a real dump surfaces and needs this, the work lands here, isolated to
     * this board. */
    static const char* needles[] = {
        "namco", "wlnet", "netauth", "onlineauth", "license", "activation", NULL
    };
    HMODULE mods[256]; DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
    DWORD count = need / sizeof(HMODULE);
    if (count > 256) count = 256;
    for (DWORD i = 0; i < count; ++i) {
        char name[MAX_PATH];
        if (!GetModuleFileNameA(mods[i], name, MAX_PATH)) continue;
        char low[MAX_PATH]; size_t k = 0;
        for (; name[k] && k < sizeof(low) - 1; ++k) low[k] = (char)tolower((unsigned char)name[k]);
        low[k] = 0;
        for (int j = 0; needles[j]; ++j) {
            if (strstr(low, needles[j])) {
                ultra_es2_log("online-auth: module '%s' matches '%s' -- presence logged, DEFERRED (no implementation)",
                              name, needles[j]);
                break;
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  Re-patch worker + crash diagnostics                                        */
/* -------------------------------------------------------------------------- */
static volatile LONG g_stop = 0;

static DWORD WINAPI es2_worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        ultra_jvs_serial_install_es2();
        ultra_dinput8_install_es2();
        ultra_audio_install_es2();
    }
    return 0;
}

static LONG WINAPI es2_crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "(unknown)";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
        ultra_es2_log("CRASH: ACCESS_VIOLATION at EIP=%p in %s", addr, modname);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

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
            utxp.board        = "namco_es2";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        AddVectoredExceptionHandler(1, es2_crash_handler);
        ultra_es2_log("=== ultra_namco_es2_hook.dll attached (Namco System ES2-only, independent) ===");
        ultra_hook_node_es2();
        ultra_jvs_serial_install_es2();   /* JVS transport + D3D9 present hook */
        ultra_dinput8_install_es2();
        ultra_audio_install_es2();
        es2_online_auth_presence_probe();
        /* Ultra Omni-Bridge (KBM emulation for raw-input-only game dumps).
         * Opt-in (ULTRA_BRIDGE_ENABLE, default OFF -- see omni_client_read_bridge()),
         * so this is a no-op unless a user/future session turns it on for a
         * confirmed raw-keyboard/mouse Namco System ES2 title. Installed after the
         * JVS serial hook above so the D3D9 present hook (which the window
         * getter depends on) is already armed. This board has no lightgun/
         * shared-pose source of its own (see d3d9_throttle_es2.c's header
         * comment), so get_pose is NULL: gamepad-only. */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(ultra_es2_d3d9_window, NULL);
ultra_pausemenu_install(ultra_es2_d3d9_window);
        CreateThread(NULL, 0, es2_worker, NULL, 0, NULL);
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();
ultra_pausemenu_shutdown();
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader/user can confirm the right DLL was
 * injected (and that it is the independent Namco System ES2-only build). */
__declspec(dllexport) const char* ultra_namco_es2_hook_id(void) {
    return "Ultra Loader Namco System ES2 I/O hook (independent, clean-room) v1";
}

#endif /* _WIN32 */
