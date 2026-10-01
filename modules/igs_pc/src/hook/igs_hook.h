/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  igs_hook.h -- shared declarations for Ultra Loader's IGS-PC-ONLY hook DLL
 * =============================================================================
 *
 *  Everything under modules/igs_pc/src/hook/ builds into
 *  ultra_igs_pc_hook{32,64}.dll, a hook binary dedicated to the IGS PC family
 *  ONLY (generic JVS I/O; distinct from IGS's PGM3/KOV3 HD board, which is its
 *  own separate module/core). It shares NO source file and NO compiled binary
 *  with modules/native_x86/src/hook/ (the multi-board "Type X hook",
 *  ultra_typex_hook{32,64}.dll) or with any other board's per-board hook
 *  (cave_pc, taito_typex, taito_typex2, sega_aurora, ...).
 *
 *  Pattern: this fork follows the same per-board isolation established for
 *  Cave PC's independent hook (modules/cave_pc/src/hook/cave_hook.h) -- one
 *  board's fix can never again silently regress another board's shared code,
 *  because there is no shared code left to regress.
 *
 *  Contents:
 *    - jvs_core_igs.c/.h    : clean-room JVS protocol engine (own copy).
 *    - jvs_serial_hook_igs.c: COM-port interception feeding the JVS engine +
 *                              dongle/D-drive path bypass + packed-game
 *                              GetProcAddress interception for the D3D9/D3D11/
 *                              GL present hooks + kernel32 IAT install.
 *    - dinput8_emu_igs.c    : clean-room synthetic DirectInput8 device (some
 *                              PC-arcade titles read cabinet input over DI8
 *                              instead of, or in addition to, JVS-over-serial).
 *    - d3d9_throttle_igs.c  : Direct3D9 present hook -- frame limiter, force-
 *                              windowed, optional upscale/rotate/sharpen/dump
 *                              (all default-off, env-gated).
 *    - audio_hook_igs.c     : DirectSound + XAudio2 diagnostic hook (passthrough
 *                              + one generic SetCooperativeLevel retry fix).
 *    - igs_dialog.c         : MessageBoxA/W auto-dismiss (ULTRA_DISMISS_DIALOGS=1).
 *    - igs_plumbing.c       : generic Win32 plumbing (IAT patcher + diagnostic
 *                              logger) with ZERO board-specific logic -- pure
 *                              mechanism, reused by every TU above.
 *    - hook_dllmain_igs.c   : DLL entry point; wires everything together +
 *                              the standalone XInput fallback controller read.
 *
 *  No known IGS PC game dump exists yet (framework-only per docs/STOCK_TAKE.md),
 *  so nothing here is per-title-tuned -- it is the same generic JVS-over-serial
 *  + DirectInput8 + D3D9 present posture proven on Type X, ready the moment a
 *  real IGS PC dump needs it.
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 / DirectX API and
 *  the open JVS standard. No third-party loader or emulator source referenced.
 * =============================================================================
 */
#ifndef ULTRA_IGS_HOOK_H
#define ULTRA_IGS_HOOK_H

#if defined(_WIN32)

#include <windows.h>
#include <stdint.h>

#include "jvs_core_igs.h"

/* ---- igs_plumbing.c ---------------------------------------------------------
 * Generic mechanism shared by every IGS PC hook TU. No board-specific meaning:
 * an optional-file diagnostic logger (ULTRA_JVS_LOG, same env var the rest of
 * the Ultra Loader fleet uses) and an IAT-name/value patcher. */
void igs_log(const char* fmt, ...);
void igs_log_hex(const char* tag, const unsigned char* b, size_t n);
void igs_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* ---- hook_dllmain_igs.c -----------------------------------------------------
 * Owns the JVS node + the loader<->game input bridge every transport reads. */
ultra_jvs_node* igs_hook_node(void);
void            igs_hook_pull_input(ultra_jvs_node* n);
void            igs_hook_current_input(ultra_jvs_input* out);

/* ---- jvs_serial_hook_igs.c ---------------------------------------------------
 * Installs the COM-port / JVS-over-serial transport + dongle/D-drive bypass +
 * packed-game GetProcAddress interception. Call once from DllMain, before the
 * game's first thread resumes; idempotent, safe to call again from a re-patch
 * worker. */
int  igs_jvs_serial_install(void);

/* ---- dinput8_emu_igs.c -------------------------------------------------------
 * Installs the synthetic DirectInput8 device (env: ULTRA_NO_DINPUT8=1 to skip). */
void igs_dinput8_install(void);

/* ---- d3d9_throttle_igs.c ------------------------------------------------------
 * Installs the D3D9 present-hook (frame limiter + force-windowed + optional
 * upscale/rotate/sharpen/dump). Idempotent. */
void igs_render9_install(void);
void igs_render9_teardown(void);   /* pair timeBeginPeriod on DLL_PROCESS_DETACH */
HWND igs_device_window(void);      /* current D3D9 device window (NULL until armed); Omni-Bridge get_window */

/* ---- audio_hook_igs.c ---------------------------------------------------------
 * Installs the DirectSound + XAudio2 diagnostic hook (always-on passthrough). */
void igs_audio_install(void);

/* ---- igs_dialog.c ---------------------------------------------------------------
 * Installs the MessageBoxA/W auto-dismiss hook (only acts when
 * ULTRA_DISMISS_DIALOGS=1 is read at call time). */
void igs_dialog_install(void);

#endif /* _WIN32 */
#endif /* ULTRA_IGS_HOOK_H */
