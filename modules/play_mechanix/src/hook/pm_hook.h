/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  pm_hook.h -- shared declarations for Ultra Loader's Play-Mechanix-ONLY hook DLL
 * =============================================================================
 *
 *  Everything under modules/play_mechanix/src/hook/ builds into
 *  ultra_play_mechanix_hook{32,64}.dll, a hook binary dedicated to the Play
 *  Mechanix / Raw Thrills (Chicago PC) lightgun family (Big Buck Hunter and
 *  kin) ONLY. Per the project owner's standing directive, it shares NO source
 *  file and NO compiled binary with the multi-board hook at
 *  modules/native_x86/src/hook/ (the "Type X hook", ultra_typex_hook{32,64}.dll)
 *  or with any other board module's own hook tree. Files referenced there were
 *  read ONLY as reference while writing this tree; the logic below is
 *  re-expressed board-locally so this DLL builds and links standalone.
 *
 *  STATUS: this board is framework-only (per docs/STOCK_TAKE.md) -- no real
 *  game dump and no confirmed I/O map are on hand. This tree implements the
 *  generic, published JVS protocol (the same open spec every JVS cabinet uses)
 *  over RS-485-style serial, plus a lightgun screen-position channel, because
 *  that is the best-evidenced I/O shape for a JAMMA-descended US lightgun
 *  cabinet of this era. It has never been playtested against a real dump --
 *  see the module's UNTESTED.txt.
 *
 *  Scope (own, independent copies -- see each .c file's own header for the
 *  exact provenance / adaptation notes):
 *    - jvs_core_pm.{c,h}     : clean-room JVS I/O engine (protocol logic only,
 *                               no OS dependency). Renamed API (jvs_pm_*) so it
 *                               can never be confused with, or linked against,
 *                               any other board's copy.
 *    - ultra_io_shared_pm.h  : this board's OWN copy of the loader<->hook IPC
 *                               contract (named shared memory), so a real
 *                               loader-mapped controller/gun pose feeds this
 *                               board exactly like every other board, with
 *                               zero shared source/binary.
 *    - jvs_serial_hook_pm.c  : COM-port IAT interception feeding the JVS engine
 *                               (the RS-485 JVS transport).
 *    - pm_gun_bridge.c       : lightgun aim + trigger source. Prefers a live
 *                               Ultra Omni-Track pose from the shared block;
 *                               falls back to the OS mouse cursor (mapped to
 *                               the D3D9 device window's client rect) when no
 *                               loader is attached, so a standalone test run
 *                               is still aimable or without a physical gun.
 *    - d3d9_throttle_pm.c    : Direct3D9 present hook -- frame limiter +
 *                               force-windowed (same proven shape as every
 *                               other board's D3D9 hook; Play Mechanix's
 *                               Chicago-PC catalog is D3D9-era hardware).
 *    - audio_hook_pm.c       : DirectSound + XAudio2 diagnostic hook (logs +
 *                               the SetCooperativeLevel retry fix).
 *    - pm_plumbing.c         : generic Win32 mechanism (IAT patcher + optional
 *                               file logger) with ZERO board-specific logic.
 *    - hook_dllmain_pm.c     : entry point; owns the JVS node + shared block,
 *                               installs every piece above.
 *
 *  Deliberately NOT ported (no evidence this board needs it; keeps the tree
 *  minimal and auditable -- add later if a real dump proves otherwise):
 *    - Synthetic DirectInput8 device emulation (dinput8_emu.c) -- no evidence
 *      this catalog reads DirectInput; the JVS engine + gun bridge cover the
 *      only I/O shape this board is known/assumed to need.
 *    - Dongle bypass, D3D8/D3D11/GL present hooks, Sega/Konami/Namco/Taito
 *      board-specific I/O shims, card readers, keychips -- all belong to other
 *      manufacturers' hardware, not this board.
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 / DirectX 9 API
 *  and the open JVS standard. No third-party loader or emulator source.
 * =============================================================================
 */
#ifndef ULTRA_PM_HOOK_H
#define ULTRA_PM_HOOK_H

#if defined(_WIN32)

#include <windows.h>
#include <stdint.h>

#include "jvs_core_pm.h"

/* ---- pm_plumbing.c ---------------------------------------------------------
 * Generic mechanism shared by every Play Mechanix hook TU. No board-specific
 * meaning: an IAT-name+value patcher and an optional-file diagnostic logger.
 * Uses the SAME env var (ULTRA_JVS_LOG) every other Ultra Loader hook uses so
 * one log file covers a whole run even though this DLL is fully independent. */
void pm_log(const char* fmt, ...);
void pm_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* ---- jvs_serial_hook_pm.c ---------------------------------------------------
 * Installs the COM-port JVS transport. Call once from DllMain
 * (DLL_PROCESS_ATTACH), before the game's first thread resumes. Idempotent --
 * safe to call again from the re-patch worker. */
int pm_jvs_serial_install(void);

/* ---- pm_gun_bridge.c ---------------------------------------------------------
 * Lightgun aim + trigger + standalone coin/start/test input. Installs a
 * polling thread that feeds the JVS node every ~8ms. */
void pm_gun_bridge_install(void);
void pm_gun_bridge_stop(void);
/* The D3D9 hook hands us the game's device window so the mouse-fallback aim
 * maps to the right client rect. NULL until CreateDevice fires. */
void pm_gun_bridge_set_window(HWND hwnd);
/* Current merged aim/trigger/digital snapshot (single-writer poll thread,
 * POD-copy read). Cumulative coin-key taps are read separately so the hook
 * can edge-diff them into jvs_pm_insert_coin without losing/doubling a pulse. */
void pm_gun_bridge_current(ultra_pm_jvs_input* out);
long pm_gun_bridge_coin_total(void);

/* ---- d3d9_throttle_pm.c ------------------------------------------------------
 * Installs the D3D9 present-hook (frame limiter + force-windowed + optional
 * upscale/rotate, same env contract as every other board's D3D9 hook).
 * Idempotent -- safe to call again from the re-patch worker. */
void pm_d3d9_install(void);
void pm_d3d9_teardown(void);   /* pair timeBeginPeriod on DLL_PROCESS_DETACH */
HWND pm_d3d9_get_window(void); /* current device/render window, NULL until CreateDevice fires */

/* ---- audio_hook_pm.c ----------------------------------------------------------
 * Installs the DirectSound + XAudio2 diagnostic hook (always-on, passthrough
 * except the documented SetCooperativeLevel retry-on-fail fix). */
void pm_audio_install(void);

/* ---- hook_dllmain_pm.c: shared node/shared-block accessors for the other TUs
 * (ultra_pm_jvs_node is jvs_core_pm.h's opaque node type, already in scope via
 * the include above). */
ultra_pm_jvs_node* pm_hook_node(void);
void               pm_hook_pull_input(ultra_pm_jvs_node* n);
void*              pm_hook_shared(void);   /* ultra_io_shared_pm* or NULL */

#endif /* _WIN32 */
#endif /* ULTRA_PM_HOOK_H */
