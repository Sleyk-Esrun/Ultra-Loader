/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_unico.c -- ultra_unico_pc_hook{32,64}.dll entry point.
 * =============================================================================
 *
 *  This is UNICO PC's OWN, fully self-contained hook DLL glue -- the
 *  equivalent of modules/native_x86/src/hook/hook_dllmain.c (read as
 *  reference only) but built from ONLY this board's own files under
 *  modules/unico_pc/src/hook/, with no #include, link, or runtime dependency
 *  on native_x86/src/hook/'s compiled output, on modtest/, or on any other
 *  board's module. Per the project directive: every board gets its own fully
 *  independent, self-contained hook DLL.
 *
 *    - Filename: ultra_unico_pc_hook{32,64}.dll (HOOK_DLL_PREF_{32,64} in
 *      unico_pc_main.c). No exports beyond a normal DllMain are required by
 *      the injector -- unico_pc_main.c injects via QueueUserAPC /
 *      CreateRemoteThread calling LoadLibraryA on this path (matching every
 *      other Ultra Loader board hook's contract); ultra_unico_pc_hook_id() is
 *      an extra export purely so a build/install can be verified.
 *    - Env vars this DLL reads (all set by unico_pc_main.c or the loader's
 *      universal settings_apply(), all default to a safe/OFF value so an
 *      unrelated injection is inert):
 *        ULTRA_SEGA_WINDOWED   -- force D3D9 windowed (see
 *          d3d9_throttle_unico.c; unico_pc_main.c defaults this to "1").
 *        ULTRA_DISMISS_DIALOGS -- auto-answer blocking MessageBox popups
 *          (unico_pc_main.c defaults this to "1").
 *        ULTRA_FPS_CAP, ULTRA_RES*, ULTRA_ROTATE, ULTRA_ASPECT, ULTRA_SHARPEN,
 *          ULTRA_AA, ULTRA_BORDERLESS, ULTRA_WINDOWED -- universal present-hook
 *          knobs (loader settings_apply()), read by d3d9_throttle_unico.c.
 *        ULTRA_UNICO_JVS_COM, ULTRA_DONGLE_BYPASS -- JVS transport + USB
 *          dongle knobs (jvs_serial_hook_unico.c).
 *        ULTRA_NO_DINPUT8 -- disable the synthetic DirectInput8 device.
 *        ULTRA_JVS_LOG -- diagnostic sink every file in this board's hook
 *          writes through (ultra_unico_log(), owned by
 *          jvs_serial_hook_unico.c).
 *        ULTRA_BRIDGE_ENABLE, ULTRA_BRIDGE_GAMEPAD, ULTRA_BRIDGE_STICK_RIGHT,
 *          ULTRA_BRIDGE_DEADZONE, ULTRA_BRIDGE_SENS, ULTRA_BRIDGE_P2_RIGHT,
 *          ULTRA_BRIDGE_VK_START/COIN/SERVICE/TEST -- Ultra Omni-Bridge
 *          (omni_bridge.c) config, read via omni_client.h's
 *          omni_client_read_bridge(); ULTRA_BRIDGE_ENABLE defaults OFF (opt-in
 *          only, matches the other 4 Omni services -- not enabled here).
 *
 *  Installs, in order:
 *    1. JVS engine + JVS-over-serial COM hook (jvs_serial_hook_unico.c) --
 *       also installs this board's kernel32 IAT patch machinery every other
 *       piece below reuses, and statically + dynamically arms the D3D9
 *       create-fn hook (BP_HOOK_D3D9_AND_D3D11 default path: D3D9 first,
 *       see d3d9_throttle_unico.c's header for why a D3D11 sibling is
 *       deferred until a real dump exists to verify against).
 *    2. Synthetic DirectInput8 device (dinput8_emu_unico.c) -- some UNICO
 *       titles may poll DI8 instead of / alongside JVS.
 *    3. Audio diagnostic hook (audio_hook_unico.c) -- DirectSound/XAudio2.
 *    4. MessageBoxA/W auto-dismiss (this file) -- ULTRA_DISMISS_DIALOGS=1.
 *    5. Ultra Omni-Bridge (omni_bridge.c) -- gamepad/lightgun -> synthesized
 *       cursor+keys for raw-OS-input titles, per the taito_typex2 Haunted
 *       Museum precedent. Installed after step 1 so the D3D9 present hook is
 *       armed (window getter needs it). UNICO has no lightgun pose source
 *       (driving/redemption catalog), so pose getter is NULL -- gamepad-only,
 *       and opt-in via ULTRA_BRIDGE_ENABLE (default off, not set by this
 *       board's own main.c).
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 API.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "jvs_core_unico.h"
#include "omni_bridge.h"

#include "ultra_transport.h"   /* shared universal I/O transport layer */
/* from the other TUs in this board's own hook */
int  ultra_jvs_serial_install_unico(void);
void ultra_dinput8_install_unico(void);
void ultra_audio_install_unico(void);
void ultra_unico_log(const char* fmt, ...);
void ultra_iat_patch_all_unico(const char* impdll, const char* impfn, void* newfn);
HWND ultra_d3d9_get_window_unico(void);
static void ultra_unico_log_attach_marker(void);
static void ultra_dialog_install_unico(void);

/* -------------------------------------------------------------------------- */
static ultra_jvs_node_unico* g_node = NULL;

ultra_jvs_node_unico* ultra_hook_node_unico(void) {
    return g_node;
}

/* ---- standalone input source for the JVS engine + DirectInput8 emulation --
 * UNICO's board profile advertises JVS-over-serial + an analog wheel
 * (BP_IO_JVS_SERIAL | BP_IO_ANALOG_WHEEL). Convention for the analog channels
 * this board's own files agree on: channel 0 = wheel (steer), channel 1 =
 * accelerator, channel 2 = brake. Digital: TEST via F2, SERVICE via F1, START
 * via '1', plus a real XInput pad (if connected) for player 1's digital +
 * analog inputs -- the same "no shared-memory bridge yet" convention this
 * project's other DUMP-GATED board hooks use until a real title is available
 * to test a richer input-mapping layer against. */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } un_xpad;
typedef struct { DWORD packet; un_xpad pad; } un_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, un_xstate*);
static p_xinput_get s_xget = NULL;
static int          s_xget_tried = 0;

static void ensure_xinput_unico(void) {
    if (s_xget || s_xget_tried) return;
    s_xget_tried = 1;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (size_t i = 0; i < sizeof(dlls)/sizeof(dlls[0]) && !s_xget; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) s_xget = (p_xinput_get)(void*)GetProcAddress(h, "XInputGetState");
    }
}

/* Shared by the JVS engine (via ultra_hook_pull_input_unico) AND the
 * DirectInput8 emulation (dinput8_emu_unico.c) so both paths report the same
 * controller state regardless of which one a given title happens to poll. */
void ultra_hook_current_input_unico(ultra_jvs_input_unico* out) {
    memset(out, 0, sizeof *out);
    for (int a = 0; a < JVS_UNICO_MAX_ANALOG; ++a) out->analog[a] = 0x8000;
    out->screen_x = out->screen_y = 0x8000;

    ensure_xinput_unico();

    if (GetAsyncKeyState(VK_F2) & 0x8000) out->system    |= 0x80;   /* TEST    */
    if (GetAsyncKeyState(VK_F1) & 0x8000) out->player[0] |= 0x4000; /* SERVICE */
    if (GetAsyncKeyState('1')  & 0x8000)  out->player[0] |= 0x8000; /* START   */
    /* keyboard wheel/pedal fallback (arrow keys / Z-X) when no pad is present */
    if (GetAsyncKeyState(VK_LEFT)  & 0x8000) out->analog[0] = 0x0000;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) out->analog[0] = 0xFFFF;
    if (GetAsyncKeyState(VK_UP)    & 0x8000) out->analog[1] = 0xFFFF;   /* accel */
    if (GetAsyncKeyState(VK_DOWN)  & 0x8000) out->analog[2] = 0xFFFF;   /* brake */

    if (s_xget) {
        un_xstate xs; memset(&xs, 0, sizeof xs);
        if (s_xget(0, &xs) == ERROR_SUCCESS) {
            if (xs.pad.wButtons & 0x1000) out->player[0] |= 0x8000; /* XINPUT_GAMEPAD_START */
            if (xs.pad.wButtons & 0x2000) out->player[0] |= 0x4000; /* XINPUT_GAMEPAD_BACK -> SERVICE */
            /* left stick -> analog[0] (steer), triggers -> analog[1]/[2] (accel/brake) */
            out->analog[0] = (uint16_t)((int)xs.pad.lx + 32768);
            out->analog[1] = (uint16_t)(xs.pad.bRT << 8 | xs.pad.bRT);
            out->analog[2] = (uint16_t)(xs.pad.bLT << 8 | xs.pad.bLT);
        }
    }
}

void ultra_hook_pull_input_unico(ultra_jvs_node_unico* n) {
    if (!n) return;
    ultra_jvs_input_unico in;
    ultra_hook_current_input_unico(&in);
    jvs_unico_set_input(n, &in);
}

/* ---- MessageBoxA/W auto-dismiss (ULTRA_DISMISS_DIALOGS=1) -----------------
 * unico_pc_main.c defaults ULTRA_DISMISS_DIALOGS to "1" unconditionally
 * before CreateProcess, so this hook must honor it or that knob is dead
 * configuration. Off by default if the env var is ever cleared. */
typedef int (WINAPI *pMessageBoxA)(HWND,LPCSTR,LPCSTR,UINT);
typedef int (WINAPI *pMessageBoxW)(HWND,LPCWSTR,LPCWSTR,UINT);
static pMessageBoxA Real_MessageBoxA_Unico;
static pMessageBoxW Real_MessageBoxW_Unico;

static int dialogs_dismissed_unico(void) {
    const char* e = getenv("ULTRA_DISMISS_DIALOGS");
    return e && e[0] == '1';
}
static int default_button_unico(UINT type) {
    switch (type & 0xF) {
        case MB_OK:                return IDOK;
        case MB_OKCANCEL:          return IDCANCEL;
        case MB_YESNO:             return IDNO;
        case MB_YESNOCANCEL:       return IDNO;
        case MB_RETRYCANCEL:       return IDCANCEL;
        case MB_ABORTRETRYIGNORE:  return IDIGNORE;
        case MB_CANCELTRYCONTINUE: return IDCONTINUE;
        default:                   return IDOK;
    }
}
static int WINAPI Hook_MessageBoxA_Unico(HWND w, LPCSTR t, LPCSTR c, UINT type) {
    ultra_unico_log("MESSAGEBOX title=\"%s\" text=\"%s\"", c ? c : "", t ? t : "");
    if (dialogs_dismissed_unico()) return default_button_unico(type);
    return Real_MessageBoxA_Unico(w, t, c, type);
}
static int WINAPI Hook_MessageBoxW_Unico(HWND w, LPCWSTR t, LPCWSTR c, UINT type) {
    char tt[256] = {0}, cc[256] = {0};
    if (t) WideCharToMultiByte(CP_ACP, 0, t, -1, tt, sizeof(tt)-1, NULL, NULL);
    if (c) WideCharToMultiByte(CP_ACP, 0, c, -1, cc, sizeof(cc)-1, NULL, NULL);
    ultra_unico_log("MESSAGEBOX title=\"%s\" text=\"%s\"", cc, tt);
    if (dialogs_dismissed_unico()) return default_button_unico(type);
    return Real_MessageBoxW_Unico(w, t, c, type);
}
static void ultra_dialog_install_unico(void) {
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (!u32) return;
    Real_MessageBoxA_Unico = (pMessageBoxA)(void*)GetProcAddress(u32, "MessageBoxA");
    Real_MessageBoxW_Unico = (pMessageBoxW)(void*)GetProcAddress(u32, "MessageBoxW");
    if (Real_MessageBoxA_Unico) ultra_iat_patch_all_unico("user32.dll", "MessageBoxA", (void*)Hook_MessageBoxA_Unico);
    if (Real_MessageBoxW_Unico) ultra_iat_patch_all_unico("user32.dll", "MessageBoxW", (void*)Hook_MessageBoxW_Unico);
}

/* -------------------------------------------------------------------------- */
/* Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 * imports). The first install already happened synchronously in DllMain, so
 * this only re-points IAT slots periodically. Every *_install_unico() here is
 * idempotent (name+value based patch / internal once-only guard). */
static volatile LONG g_stop = 0;
static DWORD WINAPI unico_worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        ultra_jvs_serial_install_unico();
        ultra_dinput8_install_unico();
        ultra_dialog_install_unico();
    }
    return 0;
}

/* ---- crash diagnostics (VEH) ---------------------------------------------- */
static LONG WINAPI ultra_crash_handler_unico(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "(unknown)";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
        ultra_unico_log("CRASH: ACCESS_VIOLATION at EIP/RIP=%p in %s", addr, modname);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* -------------------------------------------------------------------------- */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
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
            utxp.board        = "unico_pc";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(hinst);
        AddVectoredExceptionHandler(1, ultra_crash_handler_unico);
        if (!g_node) g_node = jvs_unico_create();

        /* Install SYNCHRONOUSLY: unico_pc_main.c injects us into the
         * SUSPENDED game and resumes right after, so hooks must be in place
         * before the game's first thread runs. */

        /* 1. kernel32 IAT machinery + JVS-over-serial + static/dynamic D3D9 hook. */
        ultra_jvs_serial_install_unico();

        /* 2. synthetic DirectInput8 device. */
        ultra_dinput8_install_unico();

        /* 3. DirectSound + XAudio2 diagnostic hook. */
        ultra_audio_install_unico();

        /* 4. MessageBox auto-dismiss. */
        ultra_dialog_install_unico();

        /* 5. Ultra Omni-Bridge (gamepad/lightgun -> synthesized cursor+keys
         * for raw-OS-input titles). Installed after step 1 arms the D3D9
         * present hook so ultra_d3d9_get_window_unico() has a real device
         * window to hand it once CreateDevice fires. UNICO's catalog is
         * driving/redemption (see jvs_core_unico.h), not gun games, and this
         * board has no shared-memory lightgun pose source -- pose getter is
         * NULL, gamepad-only. Opt-in only: omni_client_read_bridge() defaults
         * ULTRA_BRIDGE_ENABLE to 0, unchanged here. */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(ultra_d3d9_get_window_unico, NULL);
ultra_pausemenu_install(ultra_d3d9_get_window_unico);

        CreateThread(NULL, 0, unico_worker, NULL, 0, NULL);

        ultra_unico_log_attach_marker();
        break;

    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();
ultra_pausemenu_shutdown();
        /* Process is exiting either way; no further teardown needed for an
         * injected, one-shot arcade-game hook. */
        break;
    }
    return TRUE;
}

/* Tiny, unconditional "we're alive" log line -- independent of any env var so
 * a verifier can grep ultra.log for this board's own hook name even before
 * any feature knob is set. */
static void ultra_unico_log_attach_marker(void) {
    ultra_unico_log("ultra_unico_pc_hook.dll attached (own independent board hook)");
}

/* Explicit export so the loader/user can confirm the right DLL was injected
 * (and that it is the independent UNICO-only build, not the shared Type X
 * hook or any other board's module). */
__declspec(dllexport) const char* ultra_unico_pc_hook_id(void) {
    return "Ultra Loader UNICO PC I/O hook (independent, clean-room) v1";
}

#endif /* _WIN32 */
