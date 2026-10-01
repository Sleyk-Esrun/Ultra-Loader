/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  sega_nu_hook.h -- shared declarations for the Sega Nu hook TUs.
 * =============================================================================
 *
 *  Layout mirrors modules/rawthrills_pc/src/hook/rawthrills_hook.h. Each stub
 *  (keychip / amauth / allnet / truecrypt-bypass / amdaemon / d3d11 present /
 *  audio / IO4-USB) exposes one install entry that sega_nu_hook_main.c fires
 *  from DllMain on DLL_PROCESS_ATTACH. All installers are env-gated (set by
 *  modules/sega_nu/src/sega_nu_main.c before CreateProcess) and idempotent so
 *  a repeat-attach is a no-op.
 *
 *  Env flags (set by the Sega Nu spawner):
 *    ULTRA_SEGA_KEYCHIP       -- arm the mxparallel / USB smart-card stub
 *    ULTRA_SEGA_AMAUTH_STUB   -- arm the amauthd pipe / BootID responder
 *    ULTRA_SEGA_ALLNET_STUB   -- arm allnetd loopback + naomitest.hostname DNS
 *    ULTRA_SEGA_TRUECRYPT_BYPASS  -- redirect Y:\ / X:\ + no-op the mount tool
 *    ULTRA_AMDAEMON           -- amdaemon local IPC presence stub
 *    ULTRA_HOOK_D3D11         -- arm the DXGI/D3D11 present hook (frame cap +
 *                                 force-windowed + optional present dump)
 *    ULTRA_IO4                -- arm the IO4-USB cabinet I/O emulation
 *                                 (keyboard + XInput backed)
 *    (audio diagnostic hook is always installed, no gate -- see audio_hook_nu.c)
 *
 *  A 9th subsystem, omni_bridge.c (Ultra Omni-Bridge -- keyboard/mouse
 *  emulation for raw-input-only game dumps, the 5th Omni service), is wired
 *  in sega_nu_hook_main.c's DllMain after the 8 installers above. It reads
 *  its own opt-in config from work/include/omni_client.h's
 *  omni_client_read_bridge() (ULTRA_BRIDGE_ENABLE, default OFF) so it is a
 *  no-op unless a user/future session arms it for a confirmed raw-keyboard/
 *  mouse Sega Nu title.
 * =============================================================================
 */
#ifndef ULTRA_SEGA_NU_HOOK_H
#define ULTRA_SEGA_NU_HOOK_H

#if defined(_WIN32)
#include <windows.h>
#include <stdint.h>

/* Log helper -- writes to stderr, prefixed [sn-hook]. */
void ul_sn_log(const char* fmt, ...);

/* Walk every loaded module and rewrite the named import to newfn. */
void ul_sn_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* Env helpers. */
int  ul_sn_env_on(const char* name);          /* env var == "1" */
const char* ul_sn_env(const char* name);      /* raw env value or NULL */

/* Per-subsystem installers -- all idempotent + env-gated. */
void ul_sn_install_keychip(void);
void ul_sn_install_amauth(void);
void ul_sn_install_allnet(void);
void ul_sn_install_truecrypt_bypass(void);
void ul_sn_install_amdaemon(void);
void ul_sn_install_d3d11(void);      /* d3d11_throttle_nu.c -- DXGI present hook */
void ul_sn_install_audio(void);      /* audio_hook_nu.c -- DirectSound/XAudio2 diagnostic */
void ul_sn_install_io4(void);        /* sega_io4_nu.c -- IO4-USB cabinet I/O emulation */

/* d3d11_throttle_nu.c -- current render/device window (NULL until the DXGI
 * swap chain is first hooked). This is the ultra_bridge_window_fn handed to
 * ultra_omni_bridge_install() -- see omni_bridge.h / sega_nu_hook_main.c. */
HWND ul_sn_get_render_window(void);

#endif /* _WIN32 */
#endif /* ULTRA_SEGA_NU_HOOK_H */
