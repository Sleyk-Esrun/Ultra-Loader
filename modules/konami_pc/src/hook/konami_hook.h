/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  konami_hook.h -- shared declarations for Ultra Loader's Konami-PC-ONLY hook DLL
 * =============================================================================
 *
 *  Everything under modules/konami_pc/src/hook/ builds into
 *  ultra_konami_pc_hook32.dll, a hook binary dedicated to the Konami PC (GHL)
 *  board -- Silent Hill: The Arcade (KSHG.exe) -- ONLY. It shares NO source
 *  file and NO compiled binary with the multi-board hook at
 *  modules/native_x86/src/hook/ (the "Type X hook", ultra_typex_hook32.dll),
 *  and konami_pc_main.c's find_hook() no longer falls back to that shared
 *  binary under any circumstance (see konami_pc_main.c's header comment).
 *
 *  Why a dedicated build exists (2026-08-02): konami_pc_main.c's find_hook()
 *  used to fall back to a renamed COPY of the shared multi-board hook
 *  (modtest/ultra_typex_hook{32,64}.dll) for everything this board's own
 *  source tree didn't cover -- the D3D9 present hook, an audio hook, and a
 *  generic controls/JVS layer. That made this board depend at both build AND
 *  run time on a monolithic binary carrying every OTHER board's per-game
 *  hacks. Same rationale/precedent as cave_pc's src/hook/ fork (see
 *  cave_pc/src/hook/cave_hook.h) and sega_europar's src/hook/ fork.
 *
 *  IMPORTANT ARCHITECTURAL NOTE -- this board is NOT like the others: the
 *  game's own board communication protocol (coin/button/gun/security/sysinfo)
 *  is ALREADY fully reimplemented in src/shaiolib_crk.c (a real, hard-won,
 *  proven clean-room reimplementation -- hundreds of hours of reverse
 *  engineering, do not duplicate any of it here). shaiolib_crk.c is loaded
 *  as shaiolib.dll (a DLL the game itself dynamically loads, NOT injected by
 *  us) and, out of sheer necessity to get ANY frame rendered at all without
 *  real GHL hardware, it ALSO already owns the entire D3D9 device lifecycle:
 *  it bootstraps Direct3DCreate9/CreateDevice itself when the game's own gated
 *  state machine never calls it, hooks BeginScene/EndScene/Present on the
 *  device it creates, and drives a private render thread that calls Present
 *  through a RAW captured function pointer (bypassing its own vtable hook, by
 *  design, to avoid re-entering its own logging wrapper). This is fragile,
 *  actively-being-debugged code (see docs/STOCK_TAKE.md, konami_pc row) that
 *  this board's independence work must NOT regress, and shaiolib_crk.c /
 *  libbms_crk.c / libutil_crk.c / shaerror_crk.c / dinput8_crk.c are to be
 *  left completely unchanged by this rebuild -- not even an additive,
 *  read-only export.
 *
 *  This hook DLL therefore does NOT re-hook Direct3DCreate9, does NOT hold a
 *  D3D9 device pointer, and does NOT hook any D3D9 vtable at all -- see
 *  konami_render9.c's own file header for the full reasoning (in short: the
 *  only two ways to get this DLL a device pointer were (a) an accessor export
 *  from shaiolib_crk.c -- a change to a protected file, ruled out, or
 *  (b) an independent Direct3DCreate9 IAT hook from this DLL, which risks
 *  installing BEFORE shaiolib_crk.c's own match-by-native-value IAT hook and
 *  silently breaking it -- disabling this game's only known path to ever
 *  reach Direct3DCreate9 at all. Both ruled out; konami_render9.c instead
 *  works entirely at the Win32/HWND level, never touching a D3D9 interface).
 *
 *  Scope of this independent build:
 *    - konami_render9.c : render-OUTPUT-WINDOW control, not a D3D9 hook. Finds
 *                          the game's window independently (own EnumWindows
 *                          scan, no call into shaiolib_crk.c), keeps it
 *                          un-minimized/foreground as a second, independent,
 *                          best-effort layer alongside shaiolib_crk.c's own
 *                          rgate_keeper thread, and -- DEFAULT-OFF, opt-in via
 *                          ULTRA_RES/ULTRA_RES_W+H/ULTRA_BORDERLESS only --
 *                          resizes that window, which makes D3D9's own
 *                          automatic NULL-rect Present stretch-blit (already
 *                          used by shaiolib_crk.c's render thread) upscale the
 *                          game for free with no device access needed. No
 *                          rotate, no sharpen, no true per-frame frame
 *                          limiter -- all three genuinely need a Present/
 *                          EndScene hook, which needs the device, which this
 *                          file deliberately cannot have (see above). Frame
 *                          pacing for this game remains entirely
 *                          shaiolib_crk.c's own (unaffected either way).
 *    - konami_audio.c   : DirectSound hook -- IAT-hooks dsound.dll's
 *                          DirectSoundCreate/DirectSoundCreate8 (KSHG.exe
 *                          statically imports dsound.dll -- confirmed by
 *                          shaiolib_crk.c's own DllMain comment) and wraps
 *                          IDirectSound::SetCooperativeLevel to retry once
 *                          with a fallback always-valid window ONLY when the
 *                          game's own call already failed (never overrides a
 *                          working level). Ported from the pattern in
 *                          modules/native_x86/src/hook/audio_hook.c, as this
 *                          board's own independent copy. XAudio2Create is
 *                          log-only passthrough (KSHG.exe has no evidence of
 *                          using it; the hook is a no-op if the game never
 *                          imports any xaudio2_*.dll).
 *    - konami_plumbing.c: generic Win32 mechanism (IAT patcher + diagnostic
 *                          logger) with ZERO Konami-protocol-specific logic --
 *                          pure mechanism, same pattern as cave_plumbing.c.
 *
 *  Deliberately NOT ported (checked first, confirmed not needed for this
 *  board -- keeps the build minimal and auditable):
 *    - jvs_core.c / jvs_serial_hook.c / controls.c-style generic JVS+input
 *      mapping -- the GHL board is NOT JVS. shaiolib_crk.c already fully
 *      reimplements this board's own coin/button/gun/security protocol via
 *      keyboard+mouse (shaiolib_BUTTON_status/GUN_status/COIN_count/etc.),
 *      libutil_crk.c already neutralizes the game's OWN independent DirectInput/
 *      XInput input path, and dinput8_crk.c already neutralizes the system
 *      DirectInput8 device the game creates for its calibration-trigger poll.
 *      Confirmed via board_profile.c's own "konami_pc" entry too: that entry
 *      describes a DIFFERENT, unrelated Konami PC family (Bemani/P3IO/P4IO
 *      turntable-and-keys boards) -- it does not describe this GHL/shaiolib
 *      board at all, and konami_pc_main.c already does not consult it.
 *    - dinput8_emu.c (native reference's synthetic DI8 device) -- this board
 *      already has its own, this-game-specific dinput8_crk.c doing the same
 *      job (LMB->button0, cursor->axes for the calibration poll); a second,
 *      generic synthetic DI8 device from a separate injected DLL would only
 *      race the file-swapped one for no benefit.
 *    - dongle_bypass.c / sega_keychip.c / namco_dongle.c / card_reader.c --
 *      no evidence this board's dongle/keychip posture needs any of them;
 *      libbms_crk.c already handles the e-amusement/BMS network stub.
 *    - A generic MessageBoxA/W dismiss hook (cave_dialog.c's pattern) --
 *      shaiolib_crk.c already owns MessageBoxA/W suppression for this game
 *      specifically (shg_MessageBoxA/W: matches "shaiolib"/"ERR" dialogs, and
 *      drives an auto-click thread so a suppressed calibration dialog doesn't
 *      leave the calibration wait-loop stuck). A second, generic
 *      dismiss-everything hook layered on the SAME MessageBoxA/W IAT slot
 *      would only add a redundant, less-informed layer with no upside.
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 / DirectSound /
 *  Direct3D9 API and this project's own prior reverse-engineering notes. No
 *  third-party loader or emulator source referenced.
 * =============================================================================
 */
#ifndef ULTRA_KONAMI_HOOK_H
#define ULTRA_KONAMI_HOOK_H

#if defined(_WIN32)

#include <windows.h>
#include <stdint.h>

/* ---- konami_plumbing.c ------------------------------------------------------
 * Generic mechanism shared by every Konami-PC-hook TU. No board-specific
 * meaning: an IAT-name/value patcher and an optional-file diagnostic logger. */
void konami_log(const char* fmt, ...);
void konami_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* ---- konami_render9.c --------------------------------------------------------
 * Finds the game's window (independent EnumWindows scan) and applies
 * conservative, opt-in window-level state -- see this file's own header for
 * why it deliberately never touches a D3D9 interface. Idempotent -- safe to
 * call repeatedly from a worker thread until the window appears. */
void konami_render9_install(void);
void konami_render9_teardown(void);   /* no-op; kept for symmetry with every other *_teardown() in this project */

/* Returns the game's window once known, or NULL. Used as the
 * ultra_bridge_window_fn accessor for Ultra Omni-Bridge. */
HWND konami_render9_get_window(void);

/* ---- konami_audio.c -----------------------------------------------------------
 * Installs the DirectSound IAT hook (DirectSoundCreate/8 + SetCooperativeLevel
 * failure-retry) and the XAudio2Create log-passthrough. Idempotent. */
void konami_audio_install(void);

#endif /* _WIN32 */
#endif /* ULTRA_KONAMI_HOOK_H */
