/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  iat_patch_bna1.h  --  shared IAT patcher + diagnostic logger (Namco BNA1)
 * =============================================================================
 *  INDEPENDENT COPY: the pure "walk every loaded module's import table and
 *  repoint one function slot" mechanism used by every hook in this project
 *  (originally in native_x86/src/hook/jvs_serial_hook.c), extracted here as its
 *  own small self-contained unit so this board's hook DLL has no link
 *  dependency on any other board's compiled output. No inline trampolines, no
 *  third-party hooking library -- just IAT patching over the public PE format.
 * =============================================================================
 */
#ifndef ULTRA_IAT_PATCH_BNA1_H
#define ULTRA_IAT_PATCH_BNA1_H

#ifdef __cplusplus
extern "C" {
#endif

/* Point every loaded module's import-address-table slot for impdll!impfn at
 * newfn. Idempotent (safe to call repeatedly / from multiple installers) and a
 * clean no-op for any module that doesn't actually import that name. */
void ultra_bna1_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* Diagnostic trace, gated on ULTRA_JVS_LOG=<path> (same knob name the rest of
 * Ultra Loader's boards use, so existing capture tooling/scripts keep working
 * unchanged for this board too). No-op when the env var is unset. Appends a
 * trailing newline; format strings should not include one. */
void ultra_bna1_log(const char* fmt, ...);

#ifdef __cplusplus
}
#endif
#endif /* ULTRA_IAT_PATCH_BNA1_H */
