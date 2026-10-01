/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  sega_alls_hook.h -- shared declarations for the Sega ALLS hook TUs.
 * =============================================================================
 *
 *  This is a FULLY INDEPENDENT, board-owned hook: every subsystem it needs
 *  (JVS input struct, IO4-USB I/O, D3D11 present hook, keychip presence stub,
 *  audio diagnostic hook, DllMain) is compiled from source files that live
 *  ONLY in this folder (modules/sega_alls/src/hook/). Nothing here links
 *  against, or requires, native_x86's compiled hook DLL/import lib -- the
 *  native_x86 source tree was read ONLY as reference (public Win32/DXGI/D3D11
 *  API + the open JVS spec + documented Sega keychip protocol shapes) and the
 *  logic re-implemented here, standalone, for this board.
 *
 *  Layout mirrors modules/sega_nu/src/hook/sega_nu_hook.h (the prior per-board
 *  hook split), extended with the subsystems the project directive requires
 *  for sega_alls specifically: D3D11 present hook, ALKey-framing keychip I/O,
 *  and IO4-USB.
 *
 *  Every installer below is:
 *    - env-gated (armed only when the matching ULTRA_* flag is "1", set by
 *      this board's OWN sega_alls_main.c spawner before CreateProcess), and
 *    - idempotent (safe to call more than once; the periodic re-patch worker
 *      in hook_dllmain_alls.c relies on this).
 *
 *  Env flags this hook tree reads (all set by sega_alls_main.c):
 *    ULTRA_SEGA_WINDOWED     -- force-windowed DXGI swap chain
 *    ULTRA_D3D11 / ULTRA_SEGA_D3D11 -- arm the D3D11 present/throttle hook
 *    ULTRA_IO4               -- arm the IO4-USB (837-15257-class) I/O emulation
 *    ULTRA_SEGA_KEYCHIP      -- arm the ALKey-framing keychip presence stub
 *    ULTRA_SEGA_REGION_MASK / ULTRA_SEGA_GAME_ID / ULTRA_SEGA_PLATFORM_ID /
 *    ULTRA_SEGA_KEYCHIP_VER  -- keychip presence metadata (never secret)
 *    ULTRA_FPS_CAP           -- D3D11 frame-limiter target (default 60)
 *    ULTRA_JVS_ANALOG        -- advertised analog-channel count for FEATCHK
 *
 *  ETHICAL LINE (non-negotiable, project directive): the keychip stub answers
 *  ONLY presence/identity/session metadata. ALKey command class 30-33 (Auth
 *  2.0 key material) and GkeyEnc/GkeyDec (cmd 11/12, the per-game AES content
 *  key) are DEFERRED -- this hook returns 0/not-implemented for them and NEVER
 *  derives or fabricates the secret. See sega_keychip_alls.c.
 * =============================================================================
 */
#ifndef ULTRA_SEGA_ALLS_HOOK_H
#define ULTRA_SEGA_ALLS_HOOK_H

#if defined(_WIN32)
#include <windows.h>
#include <stdint.h>

/* ---- shared infra (hook_dllmain_alls.c) ----------------------------------- */
/* Log helper -- writes to stderr AND, if ULTRA_JVS_LOG is set, appends to that
 * file too (same diagnostic convention as every other board's hook). */
void ul_alls_log(const char* fmt, ...);

/* Walk every loaded module and rewrite the named import to newfn. RVA-bounded
 * (never walks past the mapped image), name- and value-matched (catches bound
 * imports whose names were stripped) -- same shape as every other board hook
 * in this project, re-derived here independently for this file set. */
void ul_alls_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* Env helpers. */
const char* ul_alls_env(const char* name);      /* raw env value or NULL */
int         ul_alls_env_on(const char* name);   /* env var == "1" */

/* ---- per-subsystem installers -- all idempotent + env-gated -------------- */
void ul_alls_install_keychip(void);   /* sega_keychip_alls.c */
void ul_alls_install_io4(void);       /* sega_io4_alls.c     */
void ul_alls_d3d11_install(void);     /* d3d11_throttle_alls.c */
void ul_alls_d3d11_teardown(void);    /* pairs the d3d11 frame-limiter timer  */
void ul_alls_audio_install(void);     /* audio_hook_alls.c   */

/* Current render/device window (DXGI_SWAP_CHAIN_DESC::OutputWindow captured
 * when the D3D11 swap chain is hooked), or NULL if not known yet. Used as the
 * Ultra Omni-Bridge window getter (hook_dllmain_alls.c). */
HWND ul_alls_device_window(void);     /* d3d11_throttle_alls.c */

/* Whether the IO4 emulation is currently armed (sega_io4_alls.c); the coin
 * bridge in hook_dllmain_alls.c mirrors a coin pulse into the IO4 counters
 * only when this is true. */
int  ul_alls_io4_enabled(void);
void ul_alls_io4_insert_coin(int slot, int count);

/* ---- shared live-input query ---------------------------------------------
 * Everything that needs the cabinet's current controller state (IO4 report
 * builder) calls this. Backed by the standalone XInput gamepad reader in
 * hook_dllmain_alls.c (this hook has no dependency on any other board's
 * shared-memory ABI); see jvs_core_alls.h for the ultra_jvs_input layout. */
struct ultra_jvs_input;
void ultra_alls_current_input(struct ultra_jvs_input* out);

#endif /* _WIN32 */
#endif /* ULTRA_SEGA_ALLS_HOOK_H */
