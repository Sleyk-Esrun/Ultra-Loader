/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_wpc.h -- shared declarations across ultra_wahlap_wpc_hook{32,64}.dll
 * =============================================================================
 *
 *  Wahlap WPC's own, fully independent hook DLL. No shared source file and no
 *  shared compiled binary with modules/native_x86/src/hook/'s multi-board hook,
 *  or with any other board's module folder -- see the per-project directive
 *  requiring every board to own its hook outright.
 *
 *  Files in this tree (all board-pinned, all this DLL only):
 *    jvs_core_wpc.{c,h}        clean-room JVS engine (own copy)
 *    jvs_serial_hook_wpc.c     COM-port interception + generic IAT patcher
 *    d3d9_throttle_wpc.c       D3D9 present hook: frame limiter + upscale/rotate
 *    dinput8_emu_wpc.c         clean-room DirectInput8 emulation
 *    audio_hook_wpc.c          DirectSound/XAudio2 diagnostic hook
 *    omni_bridge.{c,h}         Ultra Omni-Bridge (KBM emulation from gamepad/
 *                              lightgun); this board's own copy of the
 *                              reference at native_x86/src/hook/omni_bridge.c,
 *                              opt-in via ULTRA_BRIDGE_ENABLE (default OFF)
 *    hook_dllmain_wpc.c        DllMain, JVS node + local input source, glue
 *
 *  Wahlap WPC is a generic-JVS Chinese PC arcade board with no dump on hand
 *  (framework-only per docs/STOCK_TAKE.md); this hook is built to the plain
 *  public JVS/DirectX/DirectInput contracts with no per-game hacks. Per the
 *  project owner's standing instruction, any online/live-service auth concept
 *  this board family may turn out to need is DEFERRED here: presence is only
 *  logged (see hook_dllmain_wpc.c's online-auth presence probe), nothing is
 *  bypassed or implemented.
 * =============================================================================
 */
#ifndef ULTRA_HOOK_WPC_H
#define ULTRA_HOOK_WPC_H

#include <windows.h>
#include "jvs_core_wpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- hook_dllmain_wpc.c ----------------------------------------------------
 * One JVS node for the process, and the local input source (keyboard + XInput
 * controller 0) that feeds both the JVS engine and the DirectInput8 emulation
 * -- one input path, two consumers, so both transports agree. */
ultra_jvs_node_wpc* ultra_hook_node_wpc(void);
void                 ultra_hook_pull_input_wpc(ultra_jvs_node_wpc* n);
void                 ultra_hook_current_input_wpc(ultra_jvs_input_wpc* out);

/* Shared diagnostic sink (set ULTRA_JVS_LOG=<path> to enable). Every subsystem
 * in this DLL logs through this one function. */
void ultra_wpc_log(const char* fmt, ...);

/* ---- jvs_serial_hook_wpc.c --------------------------------------------------
 * Generic PE import-address-table patcher (by name, and by value for stripped/
 * bound imports) -- used by every subsystem in this DLL, not just the serial
 * hook. This is the one hooking mechanism the whole DLL relies on. */
void ultra_iat_patch_all_wpc(const char* impdll, const char* impfn, void* newfn);
int  ultra_jvs_serial_install_wpc(void);

/* ---- d3d9_throttle_wpc.c ---------------------------------------------------- */
void  ultra_d3d9_install_throttle_wpc(void* d3d9);
void* __stdcall ultra_Hook_Direct3DCreate9_wpc(UINT ver);
HWND  ultra_wpc_d3d9_window(void);   /* device window getter, for Omni-Bridge */

/* ---- dinput8_emu_wpc.c ------------------------------------------------------ */
void ultra_dinput8_install_wpc(void);

/* ---- audio_hook_wpc.c -------------------------------------------------------- */
void ultra_audio_install_wpc(void);

#ifdef __cplusplus
}
#endif
#endif /* ULTRA_HOOK_WPC_H */
