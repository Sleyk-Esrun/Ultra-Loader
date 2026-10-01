/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  taito_nesica2_hook.h -- shared declarations for Taito NESiCAxLive2's OWN,
 *                          fully independent hook DLL (ultra_taito_nesica2_
 *                          hook32.dll / ...hook64.dll, built from THIS
 *                          directory's own sources only).
 * =============================================================================
 *
 *  Everything under modules/taito_nesica2/src/hook/ builds into this board's
 *  own hook DLL. It shares NO source file and NO compiled binary with the
 *  multi-board hook at modules/native_x86/src/hook/ (the "Type X hook"), with
 *  I:\Ultra Loader\modtest\'s staged ultra_typex_hook{32,64}.dll, or with any
 *  other board module's hook tree (including taito_nesica, the gen-1 sibling,
 *  which is a SEPARATE board being handled elsewhere). Per the project
 *  owner's per-board directive: every modern arcade board gets its own fully
 *  independent, self-contained hook DLL, built from its own sources only,
 *  with no #include/link/runtime dependency on native_x86/src/hook or on
 *  modtest/.
 *
 *  NESiCAxLive2 posture (Taito's 2013+ 64-bit D3D11 arcade content-
 *  distribution / DRM layer, running on top of Type X2 / Type X3 hardware;
 *  common titles: BlazBlue CP Extend, KOF XIII Climax, Persona 4 Arena
 *  Ultimax NLX2, Under Night In-Birth, Nitroplus Blasterz IH, Chaos Code
 *  NSC -- all D3D11 64-bit PE fighters, with a rare 32-bit D3D9 repack
 *  possible per taito_nesica2_main.c's fall-through):
 *    - jvs_core_nesica2.{c,h}     : the canonical in-process cabinet
 *                                    input/coin model (clean-room JVS engine).
 *    - jvs_serial_hook_nesica2.c  : the JVS-over-serial (COM2/COM3) transport
 *                                    shim -- this board's actual cabinet I/O,
 *                                    same transport this project already
 *                                    proved out on Taito Type X2 (NLX/NLX2
 *                                    run on that hardware generation).
 *    - d3d11_throttle_nesica2.c   : DXGI/Direct3D 11 swap-chain present hook
 *                                    (frame limiter + force-windowed +
 *                                    diagnostic backbuffer dump) -- NLX2's
 *                                    PRIMARY render path (64-bit titles).
 *    - d3d9_throttle_nesica2.c    : Direct3D 9 present hook -- fall-through
 *                                    for the rare 32-bit NLX2 repack. A clean
 *                                    no-op IAT patch for a title that never
 *                                    imports d3d9.dll. Both D3D hooks are
 *                                    armed unconditionally ("first wins"):
 *                                    whichever API the title actually calls
 *                                    fires, the other never does.
 *    - audio_hook_nesica2.c       : DirectSound(8) + XAudio2 diagnostic hook
 *                                    (SetCooperativeLevel retry-with-
 *                                    fallback-window, same generic fix class
 *                                    as every other board's audio hook).
 *    - card_reader_nesica2.c      : NESiCA IC-card-reader PRESENCE stub.
 *                                    ETHICAL LINE (preserved from
 *                                    native_x86/src/hook/card_reader.c, the
 *                                    reference this was adapted from): ships
 *                                    NO real card data, NO player profiles,
 *                                    NO network/e-amusement handshake. It
 *                                    only answers the LOCAL reader's
 *                                    poll/read-card transport with a benign
 *                                    "no card" idle (or, if the user opts in
 *                                    with a synthetic ULTRA_CARD_ID, a
 *                                    synthetic card-present reply) so a title
 *                                    that gates on a card touch doesn't hang
 *                                    forever polling a reader that doesn't
 *                                    physically exist. Off by default
 *                                    (ULTRA_CARD_READER=1 arms it); this is
 *                                    injection/presence/detection ONLY --
 *                                    never online-auth or decrypt logic.
 *    - hook_dllmain_nesica2.c     : DllMain, the IAT-patch mechanism, the
 *                                    file logger, the standalone XInput/
 *                                    keyboard input reader (no shared-memory
 *                                    IPC contract exists for this board today
 *                                    -- see the module's own
 *                                    taito_nesica2_main.c), and the JVS node
 *                                    owner.
 *    - omni_bridge.{c,h}          : Ultra Omni-Bridge (5th Omni service):
 *                                    synthesizes raw OS cursor/click/key
 *                                    input from a gamepad (and, if a board
 *                                    has one, a live lightgun pose) for a
 *                                    title that reads raw input directly
 *                                    instead of JVS/DirectInput -- this
 *                                    board's own unchanged copy of the
 *                                    reference implementation at
 *                                    native_x86/src/hook/omni_bridge.c, per
 *                                    this project's per-board independence
 *                                    rule. Config comes from the shared,
 *                                    header-only work/include/omni_client.h
 *                                    (omni_client_read_bridge()); opt-in via
 *                                    ULTRA_BRIDGE_ENABLE, default OFF, same
 *                                    as the other 4 Omni services. Wired in
 *                                    hook_dllmain_nesica2.c's DllMain with
 *                                    get_pose=NULL (NLX2 fighters have no
 *                                    lightgun/pose source -- gamepad-to-
 *                                    cursor only).
 *
 *  Deliberately NOT ported into this independent tree (dump-gated board; add
 *  later once a real NESiCAxLive2 dump confirms it's needed -- same
 *  discipline the taito_typex3 / taito_typezero independent builds already
 *  applied to their own boards):
 *    - Synthetic DirectInput8 device emulation (native_x86/src/hook/
 *      dinput8_emu.c was read for reference; no evidence a real NLX2 title
 *      needs it over JVS-over-serial + the standalone XInput/keyboard
 *      reader below -- add later against a real dump).
 *    - Taito "Fast I/O" (iDmacDrv32/64) DMAC transport (that's Type X3's
 *      native cabinet-I/O path, not NLX2's -- NLX2 runs on Type X2-class
 *      hardware, which talks JVS-over-serial).
 *    - USB keychip / dongle byte-patch scaffolding (native_x86/src/hook/
 *      dongle_bypass.c's profile-driven patch mechanism) and MessageBox
 *      auto-dismiss -- out of this pass's requested reference set; the
 *      module's ULTRA_DONGLE_BYPASS / ULTRA_DISMISS_DIALOGS env vars are
 *      still set by taito_nesica2_main.c for forward compatibility, but no
 *      consumer exists in this independent build yet.
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 / Direct3D 9 /
 *  Direct3D 11 / DXGI / DirectSound / XAudio2 API and the published JVS spec.
 *  No third-party loader or emulator source referenced. native_x86/src/hook/
 *  was read ONLY as a working-pattern reference (per the project owner's
 *  explicit instruction), same as the independent builds already done for
 *  taito_typex3 and taito_typezero -- every non-static symbol here is
 *  renamed with a tn2_ prefix (or is a fresh board-specific name) and no
 *  object file or binary is shared.
 * =============================================================================
 */
#ifndef ULTRA_TAITO_NESICA2_HOOK_H
#define ULTRA_TAITO_NESICA2_HOOK_H

#if defined(_WIN32)

#include <windows.h>
#include <stdint.h>

#include "jvs_core_nesica2.h"

/* ---- hook_dllmain_nesica2.c -------------------------------------------------
 * Generic mechanism every other TU in this DLL uses. No board-specific
 * meaning: an optional-file diagnostic logger (ULTRA_JVS_LOG, the same env
 * var name convention Ultra Loader's other boards use, so one log file can
 * cover a whole run) and an IAT-name-and-value patcher. */
void tn2_log(const char* fmt, ...);
void tn2_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* The one JVS node this process owns (created in DllMain before any hook can
 * fire) and the live snapshot of what the cabinet looks like right now.
 * jvs_serial_hook_nesica2.c, d3d9/d3d11_throttle_nesica2.c may read these. */
ultra_jvs_node* tn2_hook_node(void);
void            tn2_current_input(ultra_jvs_input* out);

/* ---- jvs_serial_hook_nesica2.c ----------------------------------------------
 * Installs the JVS-over-serial (COM2/COM3 by default; ULTRA_JVS_COM
 * overrides) transport shim -- THIS board's cabinet I/O. Also installs the
 * kernel32 IAT hooks + GetProcAddress interception needed to catch a packed
 * title resolving CreateFileA/Direct3DCreate9/D3D11CreateDeviceAndSwapChain
 * dynamically. Idempotent; safe to call again from the re-patch worker. */
int tn2_jvs_serial_install(void);

/* ---- d3d11_throttle_nesica2.c ------------------------------------------------
 * Installs the DXGI/D3D11 swap-chain present hook (frame limiter +
 * force-windowed + optional diagnostic backbuffer dump). NLX2's PRIMARY
 * render path (64-bit titles). Idempotent. tn2_d3d11_window() returns the
 * captured device/render window (NULL until the first swap chain is hooked)
 * -- read by Ultra Omni-Bridge via hook_dllmain_nesica2.c's combined getter. */
void tn2_d3d11_install(void);
void tn2_d3d11_teardown(void);
HWND tn2_d3d11_window(void);

/* ---- d3d9_throttle_nesica2.c --------------------------------------------------
 * Installs the D3D9 present hook. Fall-through for the rare 32-bit NLX2
 * repack; a clean no-op if the game doesn't import d3d9.dll. Idempotent.
 * tn2_d3d9_window() returns the captured device window (NULL until the first
 * CreateDevice, or always NULL for the common D3D11-only NLX2 title) -- same
 * consumer as tn2_d3d11_window() above. */
void tn2_d3d9_install(void);
void tn2_d3d9_teardown(void);   /* pair timeBeginPeriod on DLL_PROCESS_DETACH */
HWND tn2_d3d9_window(void);

/* ---- audio_hook_nesica2.c -----------------------------------------------------
 * Installs the DirectSound(8) + XAudio2 diagnostic hook. Always-on
 * passthrough with one concrete fix (SetCooperativeLevel retry). Idempotent. */
void tn2_audio_install(void);

/* ---- card_reader_nesica2.c ---------------------------------------------------
 * Installs the NESiCA IC-card-reader PRESENCE stub (see the ethical-line
 * note above and the file's own header). OFF by default; ULTRA_CARD_READER=1
 * arms it. Idempotent. */
void tn2_card_reader_install(void);

#endif /* _WIN32 */
#endif /* ULTRA_TAITO_NESICA2_HOOK_H */
