/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_europar.c -- ultra_sega_europar_hook32.dll entry point.
 * =============================================================================
 *
 *  This is Sega Europa-R's OWN, fully self-contained hook DLL glue -- the
 *  equivalent of modules/native_x86/src/hook/hook_dllmain.c (read as
 *  reference only) but built from ONLY this board's own files, with no
 *  #include, link, or runtime dependency on native_x86/src/hook/'s compiled
 *  output or on modtest/. See sega_europar_main.c's header for the full
 *  rationale (2026-07-23 controls rework + board-pinning incident) and for
 *  exactly what this DLL's injection contract must satisfy:
 *
 *    - Filename: ultra_sega_europar_hook32.dll (HOOK_DLL_NAME in
 *      sega_europar_main.c). No exports beyond a normal DllMain are required
 *      by the injector -- sega_europar_main.c injects via QueueUserAPC /
 *      CreateRemoteThread calling LoadLibraryA on this path; everything this
 *      hook does, it does from DllMain(DLL_PROCESS_ATTACH), matching every
 *      other Ultra Loader board hook's contract.
 *    - Env vars this DLL reads (all set by sega_europar_main.c before
 *      CreateProcess, all default OFF so an unrelated injection is inert):
 *        ULTRA_SEGA_WINDOWED / ULTRA_TYPEX2_WINDOWED -- force D3D9 windowed
 *          (d3d9_throttle_europar.c honors either, see that file's header).
 *        ULTRA_SEGA_KEYCHIP    -- mxparallel keychip presence shim.
 *        ULTRA_INPOUT32_BYPASS -- neutralize inpout32.dll so its DllMain
 *          never opens the (absent, on a user machine) kernel driver handle.
 *        ULTRA_DISMISS_DIALOGS -- auto-answer blocking error popups.
 *        ULTRA_FPS_CAP, ULTRA_RES*, ULTRA_ROTATE, ULTRA_ASPECT, ULTRA_SHARPEN,
 *          ULTRA_AA, ULTRA_BORDERLESS, ULTRA_WINDOWED -- universal present-hook
 *          knobs (loader settings_apply()), read by d3d9_throttle_europar.c.
 *        ULTRA_JVS_LOG -- diagnostic sink every file in this board's hook
 *          writes through (ultra_europar_log(), owned by
 *          jvs_serial_hook_europar.c).
 *    - dinput8.CRK.dll (dinput8_europar.c) is a SEPARATE artifact, file-
 *      swapped into the game's own folder as "dinput8.dll" by
 *      sega_europar_main.c -- it is NOT part of this DLL and this file does
 *      not reference it. Ford Racing's actual controls come from that file,
 *      not from anything below (see dinput8_europar.c's header for the full
 *      trace of why).
 *
 *  Installs, in order:
 *    1. JVS engine + JVS-over-serial COM hook (jvs_serial_hook_europar.c) --
 *       also installs this board's kernel32 IAT patch machinery every other
 *       piece below reuses, and statically + dynamically arms the D3D9
 *       create-fn hook.
 *    2. Audio diagnostic hook (audio_hook_europar.c) -- DirectSound/XAudio2.
 *    3. Keychip / inpout32 / dialog-dismiss shim (sega_keychip_europar.c).
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 API.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "jvs_core_europar.h"
#include "omni_bridge_europar.h"

#include "ultra_transport.h"   /* shared universal I/O transport layer */
#include "ultra_tracer.h"
/* from the other TUs in this board's own hook */
int  ultra_jvs_serial_install_europar(void);
void ultra_audio_install_europar(void);
void ultra_sega_keychip_install_europar(void);
void happio_europar_install(void);
void gearpad_europar_install(void);
void gearpad_europar_stop(void);
void ultra_europar_log(const char* fmt, ...);
static void ultra_europar_log_attach_marker(void);

/* device-window getter, owned by d3d9_throttle_europar.c (NULL until its
 * CreateDevice hook has fired once -- Ultra Omni-Bridge stays inert until
 * then, matching every other board's wiring of this same reference file). */
HWND ultra_europar_get_device_window(void);

/* Shared pause/settings overlay (pause_menu_wire_europar.c -> work/src/
 * pause_menu_service.c). Self-contained poll thread, see that file's header. */
void ultra_pausemenu_install(HWND (*get_hwnd)(void));
void ultra_pausemenu_shutdown(void);

/* This board has no lightgun/shared-pose source (Europa-R is a driving-
 * cabinet board -- Ford Racing reads its wheel/pedals through
 * dinput8_europar.c, not a gun API), so Omni-Bridge is installed with
 * get_pose = NULL: gamepad-to-cursor only, no lightgun poll thread. */

/* -------------------------------------------------------------------------- */
static ultra_jvs_node_europar* g_node = NULL;

ultra_jvs_node_europar* ultra_hook_node_europar(void) {
    return g_node;
}

/* ---- standalone input source for the (inert-for-Ford-Racing) JVS engine ---
 * Ford Racing's real controls come from dinput8_europar.c, not this path (see
 * sega_europar_main.c's header). This keeps the JVS engine functionally
 * complete -- not dead scaffolding -- for a future Europa-R title that
 * genuinely talks JVS-over-serial to a real IO4 board: TEST via F2, SERVICE
 * via F1, START via 1, and a real XInput pad (if connected) for player 1's
 * digital + analog inputs, exactly mirroring the convention the rest of this
 * project's boards use for a "no shared-memory bridge yet" JVS node. */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } eu_xpad;
typedef struct { DWORD packet; eu_xpad pad; } eu_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, eu_xstate*);
static p_xinput_get s_xget = NULL;
static int          s_xget_tried = 0;

static void ensure_xinput(void) {
    if (s_xget || s_xget_tried) return;
    s_xget_tried = 1;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (size_t i = 0; i < sizeof(dlls)/sizeof(dlls[0]) && !s_xget; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) s_xget = (p_xinput_get)(void*)GetProcAddress(h, "XInputGetState");
    }
}

/* coin: edge-detected keyboard '5' or XInput right trigger / R3 click --
 * matches the convention every other board in this project uses (see e.g.
 * examu_exboard/src/hook/hook_dllmain_examu.c). This board never had ANY
 * coin mapping at all -- neither keyboard nor XInput -- until this fix.
 * Root-caused 2026-08-03: X86_BOARDS.md §9's live-testing session tried
 * keyboard '1' for START (correctly received, per this function's own
 * GetAsyncKeyState('1') check) but the game never advanced past "PRESS
 * START BUTTON" -- because no arcade game will honor START at 0 credits,
 * and this board could never reach 1 credit since no key or pad button
 * was ever wired to jvs_europar_insert_coin(). That session's own
 * suspicion that ultra_input_map_install() silently failing was the cause
 * was a dead end: this board's hook never links controls.c or calls that
 * function at all (see build_europar_hook.sh's HOOK_SRC list) -- input has
 * always gone through this file's own direct GetAsyncKeyState/XInput
 * polling instead, same as jvs_serial_hook_europar.c's JVS layer. */
static int s_coin_prev = 0;

void ultra_hook_pull_input_europar(ultra_jvs_node_europar* n) {
    if (!n) return;
    ensure_xinput();

    ultra_jvs_input_europar in;
    memset(&in, 0, sizeof in);
    for (int a = 0; a < JVS_EUROPAR_MAX_ANALOG; ++a) in.analog[a] = 0x8000;
    in.screen_x = in.screen_y = 0x8000;

    if (GetAsyncKeyState(VK_F2) & 0x8000) in.system  |= 0x80;   /* TEST    */
    if (GetAsyncKeyState(VK_F1) & 0x8000) in.player[0] |= 0x4000; /* SERVICE */
    if (GetAsyncKeyState('1')  & 0x8000)  in.player[0] |= 0x8000; /* START   */

    int coin_now = (GetAsyncKeyState('5') & 0x8000) ? 1 : 0;

    if (s_xget) {
        eu_xstate xs; memset(&xs, 0, sizeof xs);
        if (s_xget(0, &xs) == ERROR_SUCCESS) {
            /* 0x0010/0x0020 are the REAL XINPUT_GAMEPAD_START/BACK bits (this
             * file defines its own eu_xpad/eu_xstate structs instead of
             * including xinput.h, so there's no compiler-checked constant to
             * catch a wrong literal here). The previous 0x1000/0x2000 were
             * actually XINPUT_GAMEPAD_A/B (the face buttons) mislabeled as
             * START/BACK -- same copy-pasted bug found in unico_pc's hook;
             * sega_nu/sega_nu2/sega_nuc/lindbergh all use the correct bits
             * via the real xinput.h constants. Fixed here to match. */
            if (xs.pad.wButtons & 0x0010) in.player[0] |= 0x8000; /* XINPUT_GAMEPAD_START */
            if (xs.pad.wButtons & 0x0020) in.player[0] |= 0x4000; /* XINPUT_GAMEPAD_BACK -> SERVICE */
            if (!coin_now && (xs.pad.bRT > 128 || (xs.pad.wButtons & 0x0080))) coin_now = 1; /* RT or R3 -> COIN */
            /* left stick -> analog[0] (steer), triggers -> analog[1]/[2] */
            in.analog[0] = (uint16_t)((int)xs.pad.lx + 32768);
            in.analog[1] = (uint16_t)(xs.pad.bRT << 8 | xs.pad.bRT);
            in.analog[2] = (uint16_t)(xs.pad.bLT << 8 | xs.pad.bLT);
        }
    }

    if (coin_now && !s_coin_prev) jvs_europar_insert_coin(n, 0, 1);
    s_coin_prev = coin_now;

    jvs_europar_set_input(n, &in);
}

/* -------------------------------------------------------------------------- */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)hinst; (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        /* FIRST. The hook is injected into a SUSPENDED process, so this
         * runs before the game's entry point: the tracer therefore sees the
         * very first fault and the very first failed load. Observe-only -
         * it swallows no exception and changes no return value.
         * ULTRA_TRACE=0 disables. See work/include/ultra_tracer.h. */
        /* FIRST, before anything else installs. This board's DllMain runs
         * synchronously into a still-SUSPENDED process, so a byte patch applied here
         * lands before the game's first instruction. Used by Ford Racing's GameShell
         * to rewrite two hardcoded d:\sega\... bitmap paths into relative ones.
         * No-op unless ULTRA_PATCH_FILE names a profile. */
        { extern void ultra_europar_patch_install(void); ultra_europar_patch_install(); }
        /* The escape hatch, armed before ANYTHING that can block. It runs on its own
         * thread polling GetAsyncKeyState, so Ctrl+Alt+Q still kills the process when
         * the game's message loop is wedged -- which is exactly when the overlay
         * cannot help. Off unless a container asks. */
        { extern void ultra_panic_install(void); ultra_panic_install(); }
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
            utxp.board        = "sega_europar";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        if (!g_node) g_node = jvs_europar_create();

        /* 1. kernel32 IAT machinery + JVS-over-serial + static/dynamic D3D9 hook. */
        ultra_jvs_serial_install_europar();

        /* 2. DirectSound + XAudio2 diagnostic hook (new capability). */
        ultra_audio_install_europar();

        /* 3. keychip presence / inpout32 neutralize / dialog dismiss. */
        ultra_sega_keychip_install_europar();

        /* 3a. HAPP UGCI raw-HID emulation -- Ford Racing's REAL wheel/pedal/
         * button input path (2026-08-05 finding: DirectInput was never it;
         * see happio_europar.c's header for the full reverse-engineered
         * protocol). Installed right after the kernel32 IAT machinery is up
         * (step 1 above), same as this board's other IAT-hook consumers. */
        happio_europar_install();
        /* Gamepad -> keyboard gear shift (RB=up, LB=down) via SendInput; keys set
         * by ULTRA_EUROPAR_GEARUP_VK/GEARDOWN_VK, or ULTRA_EUROPAR_GEARSCAN=1 to find them. */
        gearpad_europar_install();

        /* 4. Ultra Omni-Bridge (gamepad -> cursor/keys; opt-in, OFF unless
         * ULTRA_BRIDGE_ENABLE=1 -- see omni_client.h). Installed last, after
         * the D3D9 present hook above has armed, since the window getter
         * needs that hook's g_devwnd to be populated. */
        ultra_omni_bridge_install(ultra_europar_get_device_window, NULL);

        /* 5. Shared pause/settings overlay -- Start+Select / LB+RB (pad) or
         * F1/Esc/P (keyboard) to open. Own self-contained poll thread. */
        ultra_pausemenu_install(ultra_europar_get_device_window);

        ultra_europar_log_attach_marker();
        break;

    case DLL_PROCESS_DETACH:
        /* Process is exiting either way; no teardown needed for the rest of
         * this board's injected, one-shot arcade-game hook (matches the rest
         * of this project's per-board hooks) -- except Omni-Bridge and the
         * pause menu, which own poll threads / a layered window and must
         * force-release any held mouse button / destroy the overlay. */
        ultra_omni_bridge_stop();
        gearpad_europar_stop();
        ultra_pausemenu_shutdown();
        break;
    }
    return TRUE;
}

/* Tiny, unconditional "we're alive" log line -- independent of any env var so
 * a verifier can grep ultra.log for this board's own hook name even before
 * any feature knob is set. Named distinctly from ultra_europar_log() (the
 * variadic sink in jvs_serial_hook_europar.c) to avoid signature confusion. */
static void ultra_europar_log_attach_marker(void) {
    ultra_europar_log("ultra_sega_europar_hook32.dll attached (own independent board hook)");
}

#endif /* _WIN32 */
