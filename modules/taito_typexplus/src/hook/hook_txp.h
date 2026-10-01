/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_txp.h -- shared declarations across ultra_taito_typexplus_hook{32,64}.dll
 * =============================================================================
 *
 *  Taito Type X+'s own, fully independent hook DLL. No shared source file and no
 *  shared compiled binary with modules/native_x86/src/hook/'s multi-board hook,
 *  or with any other board's module folder -- see the per-project directive
 *  requiring every board to own its hook outright.
 *
 *  Files in this tree (all board-pinned, all this DLL only):
 *    jvs_core_txp.{c,h}        clean-room JVS engine (own copy)
 *    jvs_serial_hook_txp.c     COM-port interception + generic IAT patcher
 *    d3d9_throttle_txp.c       D3D9 present hook: frame limiter + upscale/rotate
 *    dinput8_emu_txp.c         clean-room DirectInput8 emulation
 *    audio_hook_txp.c          DirectSound/XAudio2 diagnostic hook
 *    omni_bridge.{c,h}         Ultra Omni-Bridge (KBM emulation from gamepad/
 *                              lightgun); this board's own copy of the
 *                              reference at native_x86/src/hook/omni_bridge.c,
 *                              opt-in via ULTRA_BRIDGE_ENABLE (default OFF)
 *    hook_dllmain_txp.c        DllMain, JVS node + local input source, glue
 *
 *  Taito Type X+ is a generic-JVS Chinese PC arcade board with no dump on hand
 *  (framework-only per docs/STOCK_TAKE.md); this hook is built to the plain
 *  public JVS/DirectX/DirectInput contracts with no per-game hacks. Per the
 *  project owner's standing instruction, any online/live-service auth concept
 *  this board family may turn out to need is DEFERRED here: presence is only
 *  logged (see hook_dllmain_txp.c's online-auth presence probe), nothing is
 *  bypassed or implemented.
 * =============================================================================
 */
#ifndef ULTRA_HOOK_TXP_H
#define ULTRA_HOOK_TXP_H

#include <windows.h>
#include "jvs_core_txp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- hook_dllmain_txp.c ----------------------------------------------------
 * One JVS node for the process, and the local input source (keyboard + XInput
 * controller 0) that feeds both the JVS engine and the DirectInput8 emulation
 * -- one input path, two consumers, so both transports agree. */
ultra_jvs_node_txp* ultra_hook_node_txp(void);
void                 ultra_hook_pull_input_txp(ultra_jvs_node_txp* n);
void                 ultra_hook_current_input_txp(ultra_jvs_input_txp* out);

/* Shared diagnostic sink (set ULTRA_JVS_LOG=<path> to enable). Every subsystem
 * in this DLL logs through this one function. */
void ultra_txp_log(const char* fmt, ...);

/* ---- jvs_serial_hook_txp.c --------------------------------------------------
 * Generic PE import-address-table patcher (by name, and by value for stripped/
 * bound imports) -- used by every subsystem in this DLL, not just the serial
 * hook. This is the one hooking mechanism the whole DLL relies on. */
void ultra_iat_patch_all_txp(const char* impdll, const char* impfn, void* newfn);

/* Two-gun JVS cabinet mode (ULTRA_TXP_JVS_GUNS=1). No-op unless set. */
void ultra_txp_install_iogate(void);
int  ultra_jvs_serial_install_txp(void);

/* ---- d3d9_throttle_txp.c ---------------------------------------------------- */
void  ultra_d3d9_install_throttle_txp(void* d3d9);
void* __stdcall ultra_Hook_Direct3DCreate9_txp(UINT ver);
HWND  ultra_txp_d3d9_window(void);   /* device window getter, for Omni-Bridge */

/* ---- dinput8_emu_txp.c ------------------------------------------------------ */
void ultra_dinput8_install_txp(void);

/* ---- audio_hook_txp.c -------------------------------------------------------- */
void ultra_audio_install_txp(void);

#ifdef __cplusplus
}
#endif
#endif /* ULTRA_HOOK_TXP_H */
