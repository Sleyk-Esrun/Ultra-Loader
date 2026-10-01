/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  rawthrills_hook.h -- shared declarations for the Raw Thrills PC hook TUs.
 * =============================================================================
 *
 *  Each stub (jammausb / usbwd / porttalk / ewf / rawart / d3d8 / d3d9 / d3d11 /
 *  audio) exposes a single install entry that rawthrills_hook_main.c calls from
 *  DllMain on DLL_PROCESS_ATTACH. The stubs share a lightweight log helper + an
 *  IAT patcher (both implemented in rawthrills_hook_main.c).
 *
 *  All installers are env-gated: they no-op unless the loader-side spawner
 *  (rawthrills_pc_main.c) has set the corresponding ULTRA_* flag.
 * =============================================================================
 */
#ifndef ULTRA_RAWTHRILLS_HOOK_H
#define ULTRA_RAWTHRILLS_HOOK_H

#if defined(_WIN32)
#include <windows.h>
#include <stdint.h>

/* Log helper (writes to stderr, prefixed [rt-hook]). Implemented in main. */
void ul_rt_log(const char* fmt, ...);

/* Walk every loaded module and rewrite the named import to newfn. */
/* Returns the previous IAT target that was replaced (for hook chaining), or the
 * raw import if nothing chained. Capture it as your Real_ so a stub installed
 * after you calls through to your hook instead of shadowing it. */
void* ul_rt_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* Env helpers implemented in main. */
int  ul_rt_env_on(const char* name);            /* env var == "1" */
const char* ul_rt_env(const char* name);        /* raw env value or NULL */

/* Per-subsystem installers. All idempotent + env-gated. */
void ul_rt_jammausb_install(void);
void ul_rt_usbwd_install(void);
void ul_rt_porttalk_install(void);
void ul_rt_ewf_install(void);
void ul_rt_rawart_install(void);

/* Present-hook installers (added 2026-08-01). RawApp/TFAFSB-class titles render
 * D3D8 (rawthrills_pc_main.c sets ULTRA_D3D8=1 for these, per real sdaemon.exe
 * static analysis); newer Raw Thrills cabs (Nitro / H2Overdrive era) render
 * D3D9 or D3D11 (rawthrills_pc_main.c sets ULTRA_D3D11=auto for these -- one
 * flag arms BOTH D3D9 and D3D11 detection since a title links against exactly
 * one of the two and the other installer's IAT patch is then just a no-op).
 * All three are independent, idempotent, and safe to call unconditionally --
 * each only patches modules that actually import its target DLL/function. */
void ul_rt_d3d8_install(void);
void ul_rt_d3d9_install(void);
void ul_rt_d3d11_install(void);

/* Current device/render window for each present-hook, or NULL if that API's
 * CreateDevice/swap-chain-create hasn't fired yet for this process (added
 * 2026-08-02 for Ultra Omni-Bridge -- see rawthrills_hook_main.c's window-
 * getter adapter, which falls back across all three since only whichever API
 * the title actually links against ever has a non-NULL window). */
HWND ul_rt_d3d8_window(void);
HWND ul_rt_d3d9_window(void);
HWND ul_rt_d3d11_window(void);

/* DirectSound + XAudio2 diagnostic hook (added 2026-08-01). Logs every audio
 * device create + retries a failed SetCooperativeLevel with a fallback window.
 * Gated on ULTRA_RT_AUDIO=1 (set by rawthrills_pc_main.c). */
void ul_rt_audio_install(void);

#endif /* _WIN32 */
#endif /* ULTRA_RAWTHRILLS_HOOK_H */
