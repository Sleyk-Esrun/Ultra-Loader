/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_examu.c -- ultra_examu_exboard_hook32.dll entry point.
 * =============================================================================
 *
 *  This is the Examu eX-BOARD board's OWN, fully self-contained hook DLL
 *  glue -- the equivalent of modules/native_x86/src/hook/hook_dllmain.c (read
 *  as reference only) but built from ONLY this board's own files, with no
 *  #include, link, or runtime dependency on native_x86/src/hook/'s compiled
 *  output or on modtest/. Per the project owner's directive, every board gets
 *  its own fully independent, self-contained hook DLL -- no shared runtime
 *  binary with any other board.
 *
 *    - Filename: ultra_examu_exboard_hook32.dll (HOOK_DLL_NAME in
 *      examu_exboard_main.c). No exports beyond a normal DllMain are required
 *      by the injector -- examu_exboard_main.c injects via
 *      VirtualAllocEx+WriteProcessMemory+CreateRemoteThread calling
 *      LoadLibraryA on this path; everything this hook does, it does from
 *      DllMain(DLL_PROCESS_ATTACH), matching every other Ultra Loader board
 *      hook's contract.
 *    - Env vars this DLL reads (all set by examu_exboard_main.c before
 *      CreateProcess, all default OFF/byte-safe so an unrelated injection is
 *      inert):
 *        ULTRA_D3D9=1            -- arms d3d9_throttle_examu.c's frame
 *          limiter + present hook (Arcana Heart family is D3D9).
 *        ULTRA_SEGA_WINDOWED=1    -- force D3D9 windowed (mirrors the Type X2
 *          Haunted Museum fix: fullscreen-exclusive device-LOST stall).
 *        ULTRA_DONGLE_BYPASS=1    -- generic USB/parallel dongle CreateFile
 *          bypass (jvs_serial_hook_examu.c's is_dongle_device_examu()).
 *        ULTRA_DISMISS_DIALOGS=1  -- MessageBox auto-dismiss
 *          (dongle_bypass_examu.c).
 *        ULTRA_FPS_CAP=60         -- eX-BOARD target rate.
 *        ULTRA_JVS_ANALOG=2       -- advertised JVS analog channel count
 *          (fighter: 2 sticks, no wheel/pedals).
 *        ULTRA_CONTROLS=1         -- set by examu_exboard_main.c for parity
 *          with the reference contract; THIS board's own hook does not have
 *          (and does not need) native_x86's separate controls.c/
 *          input_profiles.c apparatus, so this var is not read here. Instead
 *          the keyboard+XInput input source below is ALWAYS active,
 *          independent of ULTRA_CONTROLS, exactly mirroring how
 *          modules/sega_europar/src/hook/hook_dllmain_europar.c's own
 *          independent hook provides input for its board.
 *        ULTRA_JVS_LOG            -- diagnostic sink every file in this
 *          board's hook writes through (ultra_examu_log(), owned by
 *          jvs_serial_hook_examu.c).
 *
 *  Installs, in order:
 *    1. kernel32 IAT machinery + JVS-over-serial (jvs_serial_hook_examu.c) --
 *       this is eX-BOARD's REAL coin/service/TEST/player-switch transport
 *       (COM2/COM3), not an inert scaffold.
 *    2. DirectInput8 emulation (dinput8_emu_examu.c) -- many eX-BOARD titles
 *       read player controls via DirectInput8 rather than (or in addition
 *       to) JVS; both paths read the SAME input source below.
 *    3. Audio diagnostic hook (audio_hook_examu.c) -- DirectSound/XAudio2.
 *    4. Dialog auto-dismiss + byte-patch seam (dongle_bypass_examu.c).
 *
 *  Input source: keyboard (1=START, 5=COIN, F1=SERVICE, F2=TEST, arrow keys
 *  = digital directions, Z/X/C/A/S/D = 6 attack buttons -- the standard
 *  fighting-game convention) with an XInput pad as an equally-live second
 *  source (dpad/left-stick = directions, A/B/X/Y/LB/RB = the 6 buttons,
 *  Start+Select = START/SERVICE, right trigger = COIN). Coin is edge-detected
 *  (one insert per press, not held-down flooding).
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 API.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "jvs_core_examu.h"
#include "ultra_transport.h"   /* shared universal I/O transport layer */
#include "omni_bridge.h"        /* ultra_omni_bridge_install/stop -- Omni-Bridge */
#include "ultra_tracer.h"

/* from the other TUs in this board's own hook */
int  ultra_jvs_serial_install_examu(void);
void ultra_dinput8_install_examu(void);
void ultra_audio_install_examu(void);
void ultra_dongle_install_examu(void);
void ultra_examu_log(const char* fmt, ...);
HWND ultra_examu_d3d9_get_window(void);   /* d3d9_throttle_examu.c -- current D3D9 device window */
static void ultra_examu_log_attach_marker(void);

/* -------------------------------------------------------------------------- */
static ultra_jvs_node_examu* g_node = NULL;

ultra_jvs_node_examu* ultra_hook_node_examu(void) {
    return g_node;
}

/* ---- keyboard + XInput input source ---------------------------------------
 * Fighter-style digital mapping (Arcana Heart family: 2 sticks, no wheel/
 * pedals -- see examu_exboard_main.c's ULTRA_JVS_ANALOG=2 default). Feeds
 * BOTH the JVS engine (via jvs_serial_hook_examu.c's pull) and the synthetic
 * DirectInput8 device (dinput8_emu_examu.c) from this single function, so
 * whichever path a given eX-BOARD title actually reads, the same physical
 * input shows up. */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } ex_xpad;
typedef struct { DWORD packet; ex_xpad pad; } ex_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, ex_xstate*);
static p_xinput_get s_xget = NULL;
static int          s_xget_tried = 0;

static void ensure_xinput_examu(void) {
    if (s_xget || s_xget_tried) return;
    s_xget_tried = 1;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (size_t i = 0; i < sizeof(dlls)/sizeof(dlls[0]) && !s_xget; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) s_xget = (p_xinput_get)(void*)GetProcAddress(h, "XInputGetState");
    }
}

void ultra_hook_current_input_examu(ultra_jvs_input_examu* out) {
    memset(out, 0, sizeof *out);
    for (int a = 0; a < JVS_EXAMU_MAX_ANALOG; ++a) out->analog[a] = 0x8000;
    out->screen_x = out->screen_y = 0x8000;
    ensure_xinput_examu();

    int up=0, down=0, left=0, right=0, start=0, service=0, test=0;
    int push1=0, push2=0, push3=0, push4=0, push5=0, push6=0;

    /* keyboard (always sampled -- cheap, and lets a keyboard-only test box
     * drive the game even with no pad plugged in) */
    if (GetAsyncKeyState(VK_UP)    & 0x8000) up = 1;
    if (GetAsyncKeyState(VK_DOWN)  & 0x8000) down = 1;
    if (GetAsyncKeyState(VK_LEFT)  & 0x8000) left = 1;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) right = 1;
    if (GetAsyncKeyState('1')      & 0x8000) start = 1;
    if (GetAsyncKeyState(VK_F1)    & 0x8000) service = 1;
    if (GetAsyncKeyState(VK_F2)    & 0x8000) test = 1;
    if (GetAsyncKeyState('Z')      & 0x8000) push1 = 1;
    if (GetAsyncKeyState('X')      & 0x8000) push2 = 1;
    if (GetAsyncKeyState('C')      & 0x8000) push3 = 1;
    if (GetAsyncKeyState('A')      & 0x8000) push4 = 1;
    if (GetAsyncKeyState('S')      & 0x8000) push5 = 1;
    if (GetAsyncKeyState('D')      & 0x8000) push6 = 1;

    if (s_xget) {
        ex_xstate xs; memset(&xs, 0, sizeof xs);
        if (s_xget(0, &xs) == ERROR_SUCCESS) {
            WORD b = xs.pad.wButtons;
            if (b & 0x0001) up = 1;      /* DPAD_UP    */
            if (b & 0x0002) down = 1;    /* DPAD_DOWN  */
            if (b & 0x0004) left = 1;    /* DPAD_LEFT  */
            if (b & 0x0008) right = 1;   /* DPAD_RIGHT */
            if (b & 0x0010) start = 1;   /* START      */
            if (b & 0x0020) service = 1; /* BACK       */
            if (b & 0x1000) push1 = 1;   /* A */
            if (b & 0x2000) push2 = 1;   /* B */
            if (b & 0x4000) push3 = 1;   /* X */
            if (b & 0x8000) push4 = 1;   /* Y */
            if (b & 0x0100) push5 = 1;   /* LB */
            if (b & 0x0200) push6 = 1;   /* RB */
            if (xs.pad.lx < -10000) left = 1;  else if (xs.pad.lx > 10000) right = 1;
            if (xs.pad.ly >  10000) up = 1;    else if (xs.pad.ly < -10000) down = 1;
        }
    }

    uint8_t byte1 = (uint8_t)((start<<7)|(service<<6)|(up<<5)|(down<<4)|(left<<3)|(right<<2)|(push1<<1)|push2);
    uint8_t byte2 = (uint8_t)((push3<<7)|(push4<<6)|(push5<<5)|(push6<<4));
    out->system    = test ? 0x80 : 0;
    out->player[0] = (uint16_t)((byte1 << 8) | byte2);
}

/* coin: edge-detected keyboard '5' or XInput right trigger / RS click */
static int g_coin_prev = 0;

void ultra_hook_pull_input_examu(ultra_jvs_node_examu* n) {
    if (!n) return;
    ultra_jvs_input_examu in;
    ultra_hook_current_input_examu(&in);
    jvs_examu_set_input(n, &in);

    int coin_now = (GetAsyncKeyState('5') & 0x8000) ? 1 : 0;
    if (!coin_now && s_xget) {
        ex_xstate xs; memset(&xs, 0, sizeof xs);
        if (s_xget(0, &xs) == ERROR_SUCCESS)
            if (xs.pad.bRT > 128 || (xs.pad.wButtons & 0x0080)) coin_now = 1;  /* RT or R3 */
    }
    if (coin_now && !g_coin_prev) jvs_examu_insert_coin(n, 0, 1);
    g_coin_prev = coin_now;
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
            utxp.board        = "examu_exboard";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        if (!g_node) {
            g_node = jvs_examu_create();
            /* JVS analog-channel count: examu_exboard_main.c sets
             * ULTRA_JVS_ANALOG=2 (fighter: 2 sticks, no wheel/pedals) so
             * FEATCHK matches what the loader/auto-pilot expects. */
            const char* ach = getenv("ULTRA_JVS_ANALOG");
            if (ach && *ach) {
                int v = atoi(ach);
                if (v > 0 && v <= JVS_EXAMU_MAX_ANALOG)
                    jvs_examu_set_features(g_node, 0, 0, 0, (uint8_t)v, 0, 0, 0);
            }
        }

        /* 1. kernel32 IAT machinery + JVS-over-serial + static/dynamic D3D9 hook. */
        ultra_jvs_serial_install_examu();

        /* 2. synthetic DirectInput8 device (same input source as JVS). */
        ultra_dinput8_install_examu();

        /* 3. DirectSound + XAudio2 diagnostic hook. */
        ultra_audio_install_examu();

        /* 4. dialog auto-dismiss + profile-driven byte-patch seam. */
        ultra_dongle_install_examu();

        /* 5. Ultra Omni-Bridge: SendInput gamepad -> raw keyboard/mouse
         * bridge, for any eX-BOARD title that turns out to read raw OS input
         * directly instead of JVS/DirectInput8 (mirrors the Haunted Museum
         * finding on taito_typex2). get_window comes from the D3D9 present
         * hook armed via ultra_jvs_serial_install_examu()'s Direct3DCreate9
         * IAT patch above -- it stays NULL (bridge fully inert) until the
         * game actually calls CreateDevice. This board (Arcana Heart family,
         * fighter-only, no lightgun) has no board-specific gun-pose reader,
         * so get_pose is NULL -- the bridge drives the cursor from an
         * XInput gamepad only, same as the keyboard+XInput input source
         * above. Off by default (ULTRA_BRIDGE_ENABLE=0 in omni_client.h);
         * opt-in per game once verified. */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(ultra_examu_d3d9_get_window, NULL);
ultra_pausemenu_install(ultra_examu_d3d9_get_window);

        ultra_examu_log_attach_marker();
        break;

    case DLL_PROCESS_DETACH:
        /* Process is exiting either way; no teardown needed for an injected,
         * one-shot arcade-game hook (matches the rest of this project's
         * per-board hooks), except releasing any OS mouse buttons Omni-
         * Bridge may currently be holding down. */
        ultra_omni_bridge_stop();
ultra_pausemenu_shutdown();
        break;
    }
    return TRUE;
}

/* Tiny, unconditional "we're alive" log line -- independent of any env var so
 * a verifier can grep ultra.log for this board's own hook name even before
 * any feature knob is set. */
static void ultra_examu_log_attach_marker(void) {
    ultra_examu_log("ultra_examu_exboard_hook32.dll attached (own independent board hook)");
}

/* An explicit export so the loader/tools can confirm the right DLL loaded. */
__declspec(dllexport) const char* ultra_examu_exboard_hook_id(void) {
    return "Ultra Loader Examu eX-BOARD I/O hook (clean-room JVS + DInput8) v1";
}

#endif /* _WIN32 */
