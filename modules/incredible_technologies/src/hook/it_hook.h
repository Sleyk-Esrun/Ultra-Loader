/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  it_hook.h -- shared declarations for the Incredible Technologies board's
 *              OWN, fully independent hook DLL (ultra_incredible_technologies_hook32.dll)
 * =============================================================================
 *
 *  Per the project owner's directive, every modern arcade board gets its OWN
 *  self-contained hook DLL: no shared runtime binary with any other board,
 *  and no dependency on modules/native_x86/src/hook's compiled output. This
 *  header + the TUs that implement it are the incredible_technologies
 *  module's complete, independent copy -- reference logic (JVS engine, COM-
 *  port IAT interception, D3D9 present hook, DirectSound/XAudio2 diagnostic)
 *  was READ from native_x86/src/hook (*.c) for research only; every byte here
 *  was (re)written into this board's own tree.
 *
 *  Incredible Technologies "nowboard" (per this module's own UNTESTED.txt
 *  research note): generic JVS I/O, Golden Tee-family trackball cabinets
 *  (Golden Tee Golf 2005/Live/Unplugged/PGA, Silver Strike Bowling, Target
 *  Toss Pro, Big Buck Safari IT variant, Powerputt, Shuffleboard). The
 *  trackball enumerates as a PS/2 or USB HID mouse -- this hook reads the
 *  real Windows mouse and turns its relative motion into JVS ROTARY counters
 *  (see jvs_core_it.h). Render is Direct3D 9; audio is DirectSound7.
 *
 *  ITNet online-auth (RSA-signed license blob) and the Rainbow/SafeNet iKey
 *  2032 USB dongle on later revisions are OUT OF SCOPE for this hook -- no
 *  dump exists to verify a stub against (see UNTESTED.txt), and per the
 *  per-game-complete rule a protection bypass must be verified against a
 *  real boot, not guessed. This hook covers I/O + render throttle + audio
 *  diagnostics only, exactly the subsystems the project owner's directive
 *  for this board asked for.
 *
 *  Every installer below is idempotent (a repeat call is a no-op) and fires
 *  unconditionally from DllMain -- this DLL is ONLY ever injected into a
 *  game already resolved to the incredible_technologies board. Tunables
 *  remain env-overridable (see each TU's own header comment).
 * =============================================================================
 */
#ifndef ULTRA_IT_HOOK_H
#define ULTRA_IT_HOOK_H

#if defined(_WIN32)
#include <windows.h>
#include <stdint.h>

#include "jvs_core_it.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- shared diagnostic log -------------------------------------------------
 * Writes to stderr (captured by the loader into ultra.log via inherited
 * handles) AND, when ULTRA_JVS_LOG names a file, appends there too -- the
 * same diagnostic convention every other board's hook in this project uses. */
void ul_it_log(const char* fmt, ...);

/* ---- generic IAT patcher ---------------------------------------------------
 * Walks every loaded module's import table and rewrites the (impdll, impfn)
 * slot to newfn. RVA-bounded against each module's mapped image size so a
 * malformed/packed import directory is skipped, never dereferenced OOB
 * (mingw has no SEH __try/__except here). Patches by name AND by current
 * value (idempotent re-patch, and catches bound imports with stripped names). */
void ul_it_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* ---- env helpers ------------------------------------------------------------ */
const char* ul_it_env(const char* name);      /* raw value or NULL             */
int         ul_it_env_on(const char* name);   /* "1" (exactly)                 */
const char* ul_it_env_or(const char* name, const char* dflt); /* value or dflt */

/* ---- the single cabinet-input source ---------------------------------------
 * hook_dllmain_it.c owns ONE ultra_jvs_node instance, fed by a local mouse
 * (trackball) + keyboard poll thread -- this module has no loader shared-
 * memory dependency, it is fully self-contained (matches the precedent set
 * by other independent per-board hooks in this project). */
ultra_jvs_node* ul_it_node(void);
void            ul_it_pull_input(ultra_jvs_node* n);   /* refresh n->in from the poll thread */

/* ---- per-subsystem installers (idempotent) --------------------------------- */
int  ul_it_jvs_serial_install(void); /* COM-port IAT interception -> jvs_core_it  */
void ul_it_install_d3d9(void);       /* Direct3DCreate9 -> Present/Reset throttle */
void ul_it_install_audio(void);      /* DirectSound/XAudio2 diagnostic hook       */

/* Current D3D9 device window (the hDeviceWindow/hwnd captured in
 * Hook_CreateDevice9), or NULL before any CreateDevice has fired yet. Used by
 * omni_bridge.c's window-getter callback (ultra_omni_bridge_install). */
HWND ul_it_get_render_window(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* _WIN32 */
#endif /* ULTRA_IT_HOOK_H */
