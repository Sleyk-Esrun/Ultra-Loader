/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  iat_patch_gvr.h  --  shared IAT patcher + diagnostic logger (Global VR)
 * =============================================================================
 *  INDEPENDENT COPY: the pure "walk every loaded module's import table and
 *  repoint one function slot" mechanism used by every hook in this project
 *  (originally in native_x86/src/hook/jvs_serial_hook.c, read there for
 *  REFERENCE ONLY), reimplemented here as its own small self-contained unit so
 *  this board's hook DLL (ultra_global_vr_hook{32,64}.dll) has NO link
 *  dependency on native_x86's compiled output, or on any other board's. No
 *  inline trampolines, no third-party hooking library -- just IAT patching
 *  over the public PE format.
 * =============================================================================
 */
#ifndef ULTRA_IAT_PATCH_GVR_H
#define ULTRA_IAT_PATCH_GVR_H

#ifdef __cplusplus
extern "C" {
#endif

/* Point every loaded module's import-address-table slot for impdll!impfn at
 * newfn. Idempotent (safe to call repeatedly / from multiple installers) and a
 * clean no-op for any module that doesn't actually import that name. */
void ultra_gvr_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* Walk mod's export directory directly (bypassing any GetProcAddress hook we
 * may have installed) and return the named export's address, or NULL for a
 * forwarded / absent export.  Used by throttle hooks to resolve the real
 * SwapBuffers / Direct3DCreate9 / wglSwapBuffers without recursing into
 * themselves. */
FARPROC ultra_gvr_raw_export(HMODULE mod, const char* want);

/* Diagnostic trace, gated on ULTRA_JVS_LOG=<path> (same knob name the rest of
 * Ultra Loader's boards use, so existing capture tooling/scripts keep working
 * unchanged for this board too). No-op when the env var is unset. Appends a
 * trailing newline; format strings should not include one. */
void ultra_gvr_log(const char* fmt, ...);

#ifdef __cplusplus
}
#endif
#endif /* ULTRA_IAT_PATCH_GVR_H */
