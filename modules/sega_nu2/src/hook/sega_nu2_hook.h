/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  sega_nu2_hook.h -- shared declarations for the Sega Nu2 hook TUs.
 * =============================================================================
 *
 *  Sega Nu2 (Nu 1.1/SX, 2015+) is a DISTINCT board module from "sega_nu" (the
 *  original 2011 Nu). Per the project's per-board directive ("every board
 *  gets its OWN fully independent, self-contained hook DLL -- no shared
 *  runtime binary with any other board"), every symbol declared here is
 *  local to THIS DLL (ultra_sega_nu2_hook{32,64}.dll). Nothing in this file
 *  or its TUs includes, links against, or references any file under
 *  modules/sega_nu/. The overall shape (one install() per subsystem, fired
 *  from DllMain) mirrors the sega_nu / rawthrills_pc hooks as a REFERENCE
 *  pattern only -- every helper below (logger, IAT walker, env helpers) is
 *  a fresh, independently compiled copy that lives ONLY in this DLL.
 *
 *  Each stub (keychip / amauth / allnet / truecrypt-bypass / amdaemon /
 *  D3D11 present / audio / IO4-USB) exposes one install entry that
 *  hook_dllmain_nu2.c fires from DllMain on DLL_PROCESS_ATTACH. All
 *  installers are env-gated (set by modules/sega_nu2/src/sega_nu2_main.c
 *  before CreateProcess) and idempotent so a repeat-attach is a no-op.
 *
 *  Env flags (set by the Sega Nu2 spawner, sega_nu2_main.c):
 *    ULTRA_SEGA_KEYCHIP       -- arm the N2/L3.1 USB smart-card presence stub
 *    ULTRA_SEGA_AMAUTH        -- arm the amauthd pipe / BootID responder
 *    ULTRA_SEGA_ALLNET        -- arm allnetd loopback + naomitest.hostname DNS
 *    ULTRA_SEGA_TRUECRYPT     -- redirect Y:\ / X:\ + no-op the mount tool
 *                                (BitLocker+TPM container on Nu2; the setting
 *                                name is kept for cross-board env compat)
 *    ULTRA_SEGA_AMDAEMON      -- amdaemon local IPC presence stub
 *    ULTRA_HOOK_D3D11         -- arm the DXGI/D3D11 present hook (frame cap +
 *                                force-windowed + optional present dump)
 *    ULTRA_IO4                -- arm the IO4-USB cabinet I/O emulation
 *                                (keyboard + XInput backed)
 *    (audio diagnostic hook is always installed, no gate -- see audio_hook_nu2.c)
 *
 *  Ethical line (non-negotiable, preserved exactly from the reference
 *  implementation): this is PRESENCE-ONLY. No real key derivation or
 *  BitLocker/TPM unlock is ever implemented -- see sega_keychip_nu2.c and
 *  truecrypt_bypass_nu2.c for the deliberate deferrals.
 * =============================================================================
 */
#ifndef ULTRA_SEGA_NU2_HOOK_H
#define ULTRA_SEGA_NU2_HOOK_H

#if defined(_WIN32)
#include <windows.h>
#include <stdint.h>

/* Log helper -- writes to stderr, prefixed [sn2-hook]. */
void ul_sn2_log(const char* fmt, ...);

/* Walk every loaded module and rewrite the named import to newfn. */
void ul_sn2_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* Env helpers. */
int  ul_sn2_env_on(const char* name);          /* env var == "1" */
const char* ul_sn2_env(const char* name);      /* raw env value or NULL */

/* Per-subsystem installers -- all idempotent + env-gated. */
void ul_sn2_install_keychip(void);
void ul_sn2_install_amauth(void);
void ul_sn2_install_allnet(void);
void ul_sn2_install_truecrypt_bypass(void);
void ul_sn2_install_amdaemon(void);
void ul_sn2_install_d3d11(void);      /* d3d11_throttle_nu2.c -- DXGI present hook */
void ul_sn2_install_audio(void);      /* audio_hook_nu2.c -- DirectSound/XAudio2 diagnostic */
void ul_sn2_install_io4(void);        /* sega_io4_nu2.c -- IO4-USB cabinet I/O emulation */

/* Current render/device window (d3d11_throttle_nu2.c), captured from the
 * hooked swap chain's OutputWindow. NULL until a swap chain exists. Used as
 * the ultra_bridge_window_fn passed to ultra_omni_bridge_install() -- see
 * hook_dllmain_nu2.c and omni_bridge.h. */
HWND ul_sn2_get_render_window(void);

#endif /* _WIN32 */
#endif /* ULTRA_SEGA_NU2_HOOK_H */
