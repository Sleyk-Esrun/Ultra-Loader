/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  allsux_hook.h -- shared declarations for the Sega ALLS UX board's OWN,
 *                   fully independent hook DLL (ultra_sega_alls_ux_hook{32,64}.dll).
 * =============================================================================
 *
 *  Per the project owner's directive, every modern arcade board gets its OWN
 *  self-contained hook DLL: no shared runtime binary with any other board
 *  (including sega_alls, a separate board handled elsewhere), and no
 *  dependency on modules/native_x86/src/hook's compiled output. This header
 *  + the TUs that implement it are the sega_alls_ux module's complete,
 *  independent copy -- reference logic (JVS engine, Sega IO4-USB, keychip
 *  presence/DEFER framing, D3D11 present hook, DirectInput8 emulation, audio
 *  diagnostic) was READ from native_x86's hook sources for research only;
 *  every byte here was (re)written into this board's own tree.
 *
 *  Sega ALLS UX (per sega_board.c / board_profile.c "sega_alls" entry, the
 *  family this board shares): Win10 IoT, 64-bit, D3D11/DXGI present, Sega
 *  IO4-USB (837-15257) cabinet I/O, USB N2 (L3.1) smart-card keychip +
 *  BitLocker(TPM) container, ALL.Net Auth-2.0 (ALKey, driver cmds 30-33).
 *  UX specifically wires a lightgun/touch bridge on gun cabinets.
 *
 *  PRESENCE-ONLY, non-negotiable: the keychip's key-derivation / content-
 *  transform command classes (HmacChange/CKeyChange cmd 3/4, GkeyWrite/Enc/Dec
 *  cmd 10/11/12, ALKey* cmd 30-33) and any ALL.Net live-service impersonation
 *  are NEVER implemented here -- see sega_keychip_allsux.c's DEFER table. This
 *  header exists so that boundary is documented at the single choke point
 *  every other TU in this tree includes.
 *
 *  Every installer below is idempotent (a repeat call is a no-op) and, unless
 *  noted, fires unconditionally from DllMain -- this DLL is ONLY ever injected
 *  into a game already resolved to the sega_alls_ux board, so (unlike the
 *  multi-board shared hook) there is no other board's process to stay inert
 *  for. Tunables remain env-overridable (see each TU's own header comment).
 * =============================================================================
 */
#ifndef ULTRA_ALLSUX_HOOK_H
#define ULTRA_ALLSUX_HOOK_H

#if defined(_WIN32)
#include <windows.h>
#include <stdint.h>

#include "jvs_core_allsux.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- shared diagnostic log ------------------------------------------------- *
 * Writes to stderr (captured by the loader into ultra.log via inherited
 * handles) AND, when ULTRA_JVS_LOG names a file, appends there too -- matching
 * the diagnostic convention every other board's hook in this project uses. */
void ul_ax_log(const char* fmt, ...);

/* ---- generic IAT patcher --------------------------------------------------- *
 * Walks every loaded module's import table and rewrites the (impdll, impfn)
 * slot to newfn. RVA-bounded against each module's mapped image size so a
 * malformed/packed import directory is skipped, never dereferenced OOB
 * (mingw has no SEH __try/__except here). Patches by name AND by current
 * value (idempotent re-patch, and catches bound imports with stripped names). */
void ul_ax_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* ---- env helpers ------------------------------------------------------------ */
const char* ul_ax_env(const char* name);      /* raw value or NULL            */
int         ul_ax_env_on(const char* name);   /* "1" (exactly)                 */
/* value if set, else dflt (never NULL when dflt isn't NULL) */
const char* ul_ax_env_or(const char* name, const char* dflt);

/* ---- the single cabinet-input source -------------------------------------- *
 * Every subsystem that needs "what does the cabinet read right now" (Sega
 * IO4-USB report, synthetic DirectInput8 device, the optional wire-JVS node)
 * calls this ONE function so they always agree. Backed by a local XInput
 * gamepad poll (this module has no loader shared-memory dependency -- it is
 * fully self-contained) with every axis/button centered/neutral until a pad
 * is present. */
void     ul_ax_current_input(ultra_jvs_input* out);
/* Coin edges the gamepad thread detected, not yet drained into a counter.
 * ul_ax_drain_coin() returns the pending delta and resets it to 0 (call once
 * per consumer per tick -- sega_io4_allsux.c is the only consumer today). */
unsigned ul_ax_drain_coin(void);

/* ---- per-subsystem installers (idempotent) --------------------------------- */
void ul_ax_install_d3d11(void);      /* DXGI IDXGISwapChain::Present throttle   */
void ul_ax_teardown_d3d11(void);     /* pairs the frame-limiter timeBeginPeriod */
/* Current render/device window (captured off the hooked swap chain's
 * DXGI_SWAP_CHAIN_DESC::OutputWindow), or NULL before the first swap chain is
 * created. Used as the window-getter for Ultra Omni-Bridge. */
HWND ul_ax_get_device_window(void);
void ul_ax_install_io4(void);        /* Sega IO4-USB (837-15257) I/O emulation  */
int  ul_ax_io4_enabled(void);
void ul_ax_install_keychip(void);    /* USB N2 keychip presence (DEFER on ALKey)*/
void ul_ax_install_dinput8(void);    /* synthetic DirectInput8 device            */
void ul_ax_install_audio(void);      /* DirectSound/XAudio2 diagnostic hook      */

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* _WIN32 */
#endif /* ULTRA_ALLSUX_HOOK_H */
