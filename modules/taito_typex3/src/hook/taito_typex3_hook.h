/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  taito_typex3_hook.h -- shared declarations for Ultra Loader's Taito Type X3
 *                         -ONLY hook DLL (ultra_typex_hook32.dll / ...hook64.dll
 *                         built from THIS directory's own sources).
 * =============================================================================
 *
 *  Everything under modules/taito_typex3/src/hook/ builds into this board's own
 *  hook DLL. It shares NO source file and NO compiled binary with the
 *  multi-board hook at modules/native_x86/src/hook/ (the "Type X hook") or with
 *  any other board module's hook tree. Per the project owner's per-board
 *  directive: every modern arcade board gets its own fully independent,
 *  self-contained hook DLL, built from its own sources only, with no
 *  #include/link/runtime dependency on native_x86/src/hook or on modtest/.
 *
 *  Taito Type X3 posture (2011+ Taito board, 32/64-bit Windows PE games,
 *  D3D9/D3D11 rendering depending on title, Taito "Fast I/O" iDmacDrv32/64
 *  DMAC transport for cabinet I/O -- NOT JVS-over-serial like Type X / X2):
 *    - jvs_core_typex3.{c,h}   : the canonical in-process cabinet input/coin
 *                                 model (clean-room JVS engine; see its own
 *                                 header for why it's still useful on a board
 *                                 whose primary transport isn't JVS-over-serial).
 *    - taito_fastio_typex3.c   : the iDmacDrv32/64 Fast-I/O shim -- THIS
 *                                 board's actual cabinet I/O transport.
 *    - d3d9_throttle_typex3.c  : Direct3D 9 present hook (frame limiter +
 *                                 force-windowed + diagnostic backbuffer dump).
 *    - audio_hook_typex3.c     : DirectSound(8) + XAudio2 diagnostic hook
 *                                 (SetCooperativeLevel retry-with-fallback-
 *                                 window, same generic fix class as every
 *                                 other board's audio hook).
 *    - hook_dllmain_typex3.c   : DllMain, the IAT-patch mechanism, the file
 *                                 logger, the standalone XInput/keyboard input
 *                                 reader (no shared-memory IPC contract exists
 *                                 for this board today -- see the module's own
 *                                 taito_typex3_main.c), and the JVS node owner.
 *
 *    - omni_bridge.{c,h}       : Ultra Omni-Bridge -- optional (ULTRA_BRIDGE_
 *                                 ENABLE, default OFF) keyboard/mouse-emulation
 *                                 bridge for raw-input PC-arcade dumps, ported
 *                                 unchanged (per-board copy, per this project's
 *                                 independence rule) from the reference at
 *                                 modules/native_x86/src/hook/omni_bridge.c.
 *                                 This board has no lightgun/shared-pose source
 *                                 of its own, so it is wired with get_pose=NULL
 *                                 (gamepad-to-cursor bridging only).
 *
 *  Deliberately NOT ported into this independent tree (dump-gated board; add
 *  later once a real Type X3 dump confirms it's needed):
 *    - JVS-over-serial (COM) transport -- Type X3 uses Fast-I/O instead.
 *    - Synthetic DirectInput8 device emulation.
 *    - D3D11 present hook (some Type X3 titles are D3D11; the D3D9 hook here
 *      is a clean no-op IAT patch for a title that doesn't import d3d9.dll,
 *      so it never mis-fires -- a D3D11 sibling can be added the same way
 *      once a real dump confirms the swap-chain path it uses).
 *    - Dongle/keychip bypass, card-reader stubs (no evidence any exist here).
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 / Direct3D 9 /
 *  DirectSound / XAudio2 API and the published JVS spec. No third-party
 *  loader or emulator source referenced.
 * =============================================================================
 */
#ifndef ULTRA_TAITO_TYPEX3_HOOK_H
#define ULTRA_TAITO_TYPEX3_HOOK_H

#if defined(_WIN32)

#include <windows.h>
#include <stdint.h>

#include "jvs_core_typex3.h"

/* ---- hook_dllmain_typex3.c --------------------------------------------------
 * Generic mechanism every other TU in this DLL uses. No board-specific
 * meaning: an optional-file diagnostic logger (ULTRA_JVS_LOG, the same env
 * var name convention Ultra Loader's other boards use, so one log file can
 * cover a whole run) and an IAT-name-and-value patcher. */
void t3_log(const char* fmt, ...);
void t3_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* The one JVS node this process owns (created in DllMain before any hook can
 * fire) and the live snapshot of what the cabinet looks like right now. Both
 * taito_fastio_typex3.c and d3d9_throttle_typex3.c may read these. */
ultra_jvs_node* t3_hook_node(void);
void            t3_current_input(ultra_jvs_input* out);

/* ---- taito_fastio_typex3.c ---------------------------------------------------
 * Installs the iDmacDrv32/64 Fast-I/O shim -- THIS board's cabinet I/O
 * transport. Idempotent; safe to call again from the re-patch worker. */
void t3_fastio_install(void);

/* ---- d3d9_throttle_typex3.c ---------------------------------------------------
 * Installs the D3D9 present-hook (frame limiter + force-windowed + optional
 * diagnostic backbuffer dump). A clean no-op if the game doesn't import
 * d3d9.dll (e.g. a D3D11 Type X3 title). Idempotent. */
void t3_d3d9_install(void);
void t3_d3d9_teardown(void);   /* pair timeBeginPeriod on DLL_PROCESS_DETACH */
HWND t3_d3d9_get_window(void); /* current device window, or NULL if none yet */

void ultra_cursor_install(void); /* keep the mouse pointer drawn (cursor_typex3.c) */
void ultra_cursor_stop(void);

/* ---- audio_hook_typex3.c -----------------------------------------------------
 * Installs the DirectSound(8) + XAudio2 diagnostic hook. Always-on passthrough
 * with one concrete fix (SetCooperativeLevel retry). Idempotent. */
void t3_audio_install(void);

#endif /* _WIN32 */
#endif /* ULTRA_TAITO_TYPEX3_HOOK_H */
