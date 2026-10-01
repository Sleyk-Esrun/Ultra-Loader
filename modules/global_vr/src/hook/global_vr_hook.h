/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  global_vr_hook.h -- shared declarations for the Global VR hook TUs
 * =============================================================================
 *
 *  ultra_global_vr_hook{32,64}.dll is a fully independent, self-contained hook
 *  DLL for the Global VR PC-arcade board family (Ferrari F355 Challenge arcade
 *  port, Big Buck Hunter Safari, NASCAR Team Racing, PGA Tour Golf, Madden NFL
 *  Football, America's Army, Blazing Angels). It shares NO compiled object code
 *  with native_x86/src/hook/ or with any other board's module -- every TU below
 *  lives only in modules/global_vr/src/hook/ and is linked only into this DLL.
 *
 *  Files in this tree:
 *    iat_patch_gvr.{c,h}      -- generic IAT-walk patcher + ULTRA_JVS_LOG logger
 *    jvs_core_gvr.{c,h}       -- clean-room JVS protocol engine (pure logic)
 *    ultra_io_shared_gvr.h    -- loader<->hook shared-memory IPC struct
 *    jvs_serial_hook_gvr.c    -- COM-port transport (JVS-over-serial + the I/O
 *                                board's watchdog heartbeat) + Hasp dongle
 *                                presence bypass
 *    dinput8_emu_gvr.c        -- clean-room synthetic DirectInput8 device
 *    d3d9_throttle_gvr.c      -- D3D9 present hook: frame limiter + force-
 *                                windowed + optional upscale-to-window
 *    audio_hook_gvr.c         -- DirectSound + XAudio2 diagnostic hook
 *    omni_bridge.{c,h}        -- Ultra Omni-Bridge (5th Omni service): raw
 *                                SendInput cursor/click/key synthesis from a
 *                                gamepad, for a future title that (like Type
 *                                X2's Haunted Museum) turns out to read raw
 *                                OS keyboard/mouse instead of JVS/DirectInput8.
 *                                Verbatim per-board copy of native_x86/src/
 *                                hook/omni_bridge.{c,h} (reference only, no
 *                                link -- see that file's own header). OFF by
 *                                default (ULTRA_BRIDGE_ENABLE) -- no Global VR
 *                                dump has confirmed this need yet; this board
 *                                passes NULL for get_pose (no dedicated live-
 *                                lightgun-pose reader exists here -- see
 *                                hook_dllmain_gvr.c's install call).
 *    hook_dllmain_gvr.c       -- DllMain: owns the JVS node + input bridge,
 *                                fires every installer, standalone-gamepad
 *                                fallback thread
 *
 *  Injected into the game process by global_vr_main.c via a suspended-launch +
 *  QueueUserAPC(LoadLibraryA) (primary) + CreateRemoteThread(LoadLibraryA)
 *  (fallback). No exported entry point is required for that contract -- all
 *  wiring happens in DllMain on DLL_PROCESS_ATTACH, matching how
 *  global_vr_main.c already expects a board-native hook to behave (mirrors the
 *  Type X hook contract it falls back to when this DLL is absent).
 *
 *  Env flags this hook responds to (all optional; safe defaults when unset):
 *    ULTRA_JVS_LOG            -- diagnostic trace file path
 *    ULTRA_JVS_COM            -- COM port(s) to claim ("any" or a list; default 2,3)
 *    ULTRA_JVS_ID             -- override the JVS IOIDENT string
 *    ULTRA_JVS_SCREENPOS      -- "1" advertises a touch/light-gun screen-position
 *                                 channel (Big Buck Hunter Safari et al.)
 *    ULTRA_JVS_ANALOG         -- advertised analog channel count (default 8)
 *    ULTRA_GVR_WATCHDOG       -- "0" disables the COM heartbeat emulation (on by default)
 *    ULTRA_DONGLE_BYPASS      -- "0" disables the Hasp/Aladdin/SafeNet device-open
 *                                 presence bypass (on by default for this board)
 *    ULTRA_DDRIVE             -- redirect target for a virtual D:\ data drive
 *    ULTRA_NO_DINPUT8         -- "1" skips the DirectInput8 emulation entirely
 *    ULTRA_GVR_WINDOWED / ULTRA_WINDOWED / ULTRA_BORDERLESS / ULTRA_SEGA_WINDOWED
 *                             -- force the D3D9 device windowed (any set to "1" arms it;
 *                                 global_vr_main.c already defaults ULTRA_SEGA_WINDOWED=1)
 *    ULTRA_RES / ULTRA_RES_W / ULTRA_RES_H -- upscale target for the present hook
 *    ULTRA_FPS_CAP            -- frame limiter target (default 60)
 *    ULTRA_BRIDGE_ENABLE / ULTRA_BRIDGE_* -- Omni-Bridge (see omni_bridge_gvr.h
 *                                and work/include/omni_client.h). Default OFF;
 *                                opt-in per game once a real dump confirms need.
 * =============================================================================
 */
#ifndef ULTRA_GLOBAL_VR_HOOK_H
#define ULTRA_GLOBAL_VR_HOOK_H

#if defined(_WIN32)
#include <windows.h>
#include <stdint.h>

#include "iat_patch_gvr.h"
#include "jvs_core_gvr.h"
#include "ultra_io_shared_gvr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- hook_dllmain_gvr.c: the live JVS node + the loader<->game input bridge */
ultra_jvs_node*   ultra_gvr_hook_node(void);
ultra_io_shared*  ultra_gvr_hook_shared(void);
void              ultra_gvr_hook_current_input(ultra_jvs_input* out);
void              ultra_gvr_hook_pull_input(ultra_jvs_node* n);
/* Forwarders so a future per-game tweak can drive input without reaching into
 * hook_dllmain_gvr.c's statics directly (keeps write-access centralized). */
void              ultra_gvr_hook_set_local_input(const ultra_jvs_input* in);
void              ultra_gvr_hook_local_coin_insert(int slot);

/* ---- per-subsystem installers (each idempotent + safe to call repeatedly) -- */
int  ultra_gvr_jvs_serial_install(void);   /* jvs_serial_hook_gvr.c */
void ultra_gvr_dinput8_install(void);      /* dinput8_emu_gvr.c */
void ultra_gvr_d3d9_install(void);         /* d3d9_throttle_gvr.c (arms the Direct3DCreate9 IAT hook) */
void ultra_gvr_d3d9_teardown(void);        /* pairs the frame-limiter's timeBeginPeriod */
HWND ultra_gvr_d3d9_window(void);          /* d3d9_throttle_gvr.c: current device window, NULL if none yet */
void ultra_gvr_gl_install(void);           /* gl_throttle_gvr.c: SwapBuffers / wglSwapBuffers frame limiter */
void ultra_gvr_gl_teardown(void);          /* gl_throttle_gvr.c: pairs timeBeginPeriod */
HWND ultra_gvr_gl_window(void);            /* gl_throttle_gvr.c: OpenGL render window from WindowFromDC */
void ultra_gvr_audio_install(void);        /* audio_hook_gvr.c */

#ifdef __cplusplus
}
#endif
#endif /* _WIN32 */
#endif /* ULTRA_GLOBAL_VR_HOOK_H */
