/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_typex4.h  --  shared declarations for the Taito Type X4 hook.
 * =============================================================================
 *
 *  This board's hook DLL (ultra_taito_typex4_hook{32,64}.dll) is built ENTIRELY
 *  from files in this folder (src/hook/ under modules/taito_typex4/). It is a
 *  fully independent tree: no #include, no link, and no runtime dependency on
 *  modules/native_x86/src/hook/ or on modtest/. It is injected only into a
 *  Taito Type X4 game by this board's own spawner (../taito_typex4_main.c).
 *
 *  hook_dllmain_typex4.c owns:
 *    - the one clean-room JVS node (jvs_core_typex4.c) that Fast-I/O answers
 *      questions from,
 *    - the standalone input capture (XInput + keyboard -- this board has no
 *      loader-side shared-memory feed of its own, so the hook reads the local
 *      controller/keyboard directly, same fallback pattern proven elsewhere
 *      in this project),
 *    - the generic IAT-patch-by-name-or-value utility every other TU here
 *      uses to redirect a game's imports at its own hook functions,
 *    - a tiny diagnostic logger (ULTRA_JVS_LOG-gated, same env-var convention
 *      used project-wide so a user's existing log tooling keeps working),
 *    - DllMain itself, which installs Fast-I/O, the D3D11 present hook, and
 *      the audio diagnostic hook on DLL_PROCESS_ATTACH.
 * =============================================================================
 */
#ifndef ULTRA_HOOK_DLLMAIN_TYPEX4_H
#define ULTRA_HOOK_DLLMAIN_TYPEX4_H

#include <windows.h>
#include "jvs_core_typex4.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- logging (ULTRA_JVS_LOG=<path> to arm; no-op otherwise) ---------------- */
void ultra_tx4_log(const char* fmt, ...);

/* ---- generic IAT patcher: walk every loaded module's import table and
 * redirect impdll!impfn -> newfn (name-based AND value-based, so a re-patch
 * after the game loads a delay-loaded copy is idempotent). ------------------- */
void ultra_tx4_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* ---- the one JVS node + the live input snapshot every transport reads from - */
ultra_jvs_node_tx4* ultra_tx4_node(void);
void ultra_tx4_current_input(ultra_jvs_input_tx4* out);
void ultra_tx4_pull_input(void);   /* refresh the node from the live snapshot + coin edges */

/* ---- per-subsystem installers (each is idempotent + safe with no device) -- */
void ultra_tx4_fastio_install(void);      /* taito_fastio_typex4.c   */
int  ultra_tx4_fastio_enabled(void);
void ultra_tx4_d3d11_install(void);       /* d3d11_throttle_typex4.c */
void ultra_tx4_d3d11_teardown(void);
HWND ultra_tx4_device_window(void);       /* d3d11_throttle_typex4.c -- NULL until a swap chain is hooked */
void ultra_tx4_audio_install(void);       /* audio_hook_typex4.c     */
void ultra_tx4_felica_stub_install(void); /* felica_spoof_typex4.c (env-gated, OFF by default) */

#ifdef __cplusplus
}
#endif
#endif /* ULTRA_HOOK_DLLMAIN_TYPEX4_H */
