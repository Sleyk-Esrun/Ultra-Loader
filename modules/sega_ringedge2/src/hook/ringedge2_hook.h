/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  ringedge2_hook.h -- shared declarations for the Sega RingEdge 2 hook TUs.
 * =============================================================================
 *
 *  modules/sega_ringedge2/src/hook/ is this board's OWN fully independent hook
 *  tree. It is NOT #included, linked, or shared at build or run time with
 *  modules/native_x86/src/hook/ or modtest/ -- those trees were read as
 *  REFERENCE ONLY (clean-room: public JVS spec + public Win32/D3D9/D3D11/DXGI/
 *  DirectSound/XAudio2 SDK surfaces only), and every file below is this board's
 *  own compiled copy under its own symbol names.
 *
 *  Board facts (from board_profile.c / sega_board.c, read as reference only):
 *    Sega RingEdge 2 -- Core i3-540 + nVidia GT545, Windows Embedded Standard
 *    2009. I/O: JVS-over-serial (RS-485 on COM2/COM3). DRM: PIC "mxparallel"
 *    keychip family (same family as RingEdge/RingWide) gating a TrueCrypt game
 *    container. Graphics: MIXED -- earlier titles render D3D9, later titles
 *    edge toward D3D11, so BOTH present hooks are armed here and whichever API
 *    the running game actually calls wins at runtime (first-create-fn-wins
 *    detection, same idea as the canonical d3d11 dual-path -- see
 *    d3d11_present_ringedge2.c's header).
 *
 *  Ethical line (see keychip_ringedge2.c): this hook answers ONLY the
 *  non-secret PRESENCE / region / session-open surface of the mxparallel
 *  protocol. It never derives or ships the per-game TrueCrypt key -- that key
 *  is sealed inside the physical keychip silicon and is out of reach by design;
 *  a real retail RingEdge 2 title therefore stays HARDWARE-GATED (needs the
 *  operator keychip, or an already-decrypted/unprotected image).
 *
 *  TUs in this tree:
 *    ringedge2_hook_main.c      -- DllMain, log/env/IAT-patch helpers, JVS node
 *                                  lifecycle, loader-shared-memory + gamepad
 *                                  input snapshot.
 *    jvs_core_ringedge2.c/.h    -- clean-room JVS protocol engine (own copy).
 *    jvs_serial_ringedge2.c     -- COM-port transport feeding the JVS engine +
 *                                  the D3D9/D3D11/DXGI GetProcAddress seam
 *                                  (for packed titles with no static import).
 *    keychip_ringedge2.c        -- mxparallel presence-only shim.
 *    d3d9_present_ringedge2.c   -- D3D9 frame limiter + present hook.
 *    d3d11_present_ringedge2.c  -- D3D11/DXGI frame limiter + present hook
 *                                  (device+swapchain route AND bare-factory
 *                                  route; first to fire wins).
 *    audio_ringedge2.c          -- DirectSound + XAudio2 diagnostic hook.
 *    omni_bridge.c/.h           -- Ultra Omni-Bridge (5th Omni service): this
 *                                  board's own copy of the reference
 *                                  implementation at modules/native_x86/src/
 *                                  hook/omni_bridge.{c,h} (read as reference
 *                                  only, unchanged besides the omni_client.h
 *                                  include path). Opt-in (ULTRA_BRIDGE_ENABLE,
 *                                  default OFF); config from work/include/
 *                                  omni_client.h's omni_client_read_bridge().
 * =============================================================================
 */
#ifndef ULTRA_RINGEDGE2_HOOK_H
#define ULTRA_RINGEDGE2_HOOK_H

#if defined(_WIN32)
#include <windows.h>
#include <stdint.h>
#include "jvs_core_ringedge2.h"

/* ---- diagnostics (ULTRA_JVS_LOG=<path>; silent/no-op unless set) ----------- */
void ul_re2_log(const char* fmt, ...);

/* Local Sega daemon stand-in. This board cannot reach a frame without one:
 * the game parks its main thread on a 60s receive timeout waiting for a
 * loopback daemon that does not exist on a desktop PC. ULTRA_RE2_PCP=0
 * disables it in favor of an external one. */
void ul_re2_pcp_install(void);

/* ---- env helpers ------------------------------------------------------------*/
int         ul_re2_env_on(const char* name);   /* env var == "1" */
const char* ul_re2_env(const char* name);      /* raw value or NULL */

/* ---- IAT patcher: walk every loaded module, rewrite the named import ------- *
 * RVA-bounded against each module's mapped image size (no SEH under mingw), so
 * a malformed/packed import directory is skipped rather than faulted on.
 * Patches by name AND by value (idempotent re-patch, catches bound imports). */
void ul_re2_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* ---- JVS node + input snapshot (owned by ringedge2_hook_main.c) ------------ */
ultra_jvs_node_ringedge2* ul_re2_jvs_node(void);
/* Pull the latest input (loader shared-memory block, else local gamepad) into
 * the JVS node and drain any coin pulses. */
void ul_re2_pull_input(ultra_jvs_node_ringedge2* n);
/* Raw current input snapshot (used by anything that wants it directly). */
void ul_re2_current_input(ultra_jvs_input_ringedge2* out);

/* ---- per-subsystem installers -- all idempotent, called from DllMain ------- */
int  ul_re2_jvs_serial_install(void);   /* COM transport + D3D9/D3D11/DXGI seam */
void ul_re2_keychip_install(void);      /* mxparallel presence-only shim         */
void ul_re2_d3d9_install(void);         /* D3D9 present hook (armed always)      */
void ul_re2_d3d11_install(void);        /* D3D11/DXGI present hook (armed always)*/
void ul_re2_d3d11_teardown(void);       /* pairs d3d11's timeBeginPeriod          */
void ul_re2_audio_install(void);        /* DirectSound + XAudio2 diagnostic hook */

/* ---- device-window getters (this board is mixed D3D9/D3D11 -- whichever API
 * the running title actually calls is the one that ever returns non-NULL, see
 * each file's own header comment). Ultra Omni-Bridge's window-getter accessor
 * (ringedge2_hook_main.c) reads both and returns whichever is set. */
HWND ul_re2_d3d9_window(void);          /* d3d9_present_ringedge2.c  */
HWND ul_re2_d3d11_window(void);         /* d3d11_present_ringedge2.c */

/* ---- bring-up watchdog (watchdog_ringedge2.c) ------------------------------
 * A heartbeat thread that turns "the log just stops" into a measurement:
 * present counts, the game's own main-loop counter, JVS traffic, window state,
 * and -- only when a whole interval passes with NO progress -- a per-thread
 * PC + stack-frame sample resolved to module+RVA. ULTRA_RE2_WATCHDOG=0 off. */
/* amCredit deadlock breaker -- see amcredit_ringedge2.c. Signature-matched,
 * so it is inert on any image that does not carry the exact idiom. */
void ul_re2_amcredit_install(void);
void ul_re2_amcredit_state(long* init_flag, long* ring_head);
void ul_re2_amcredit_shutdown(void);
void ul_re2_amcredit_coin_install(void);
void ul_re2_watchdog_install(void);

/* Running Present counts. Whichever graphics API the running title actually
 * uses is the one that ever moves; the other stays 0 forever. Read by the
 * watchdog -- the pair of them is how "stopped after one frame" is told apart
 * from "presenting into a window you cannot see". */
unsigned long ul_re2_d3d9_present_count(void);
unsigned long ul_re2_d3d11_present_count(void);

/* JVS frames seen on the fake COM handle. Defined in watchdog_ringedge2.c,
 * bumped by jvs_serial_ringedge2.c's Hook_WriteFile/Hook_ReadFile. Before
 * these existed this bus was completely dark: the two hooks logged nothing at
 * all, so "the game never polled JVS" and "it polled 3000 times and nobody
 * wrote it down" produced the identical (empty) log. */
extern volatile LONG ul_re2_jvs_writes;
extern volatile LONG ul_re2_jvs_reads;

/* NOTE: ul_re2_Hook_Direct3DCreate9 / ul_re2_Hook_D3D11CreateDeviceAndSwapChain /
 * ul_re2_Hook_CreateDXGIFactory[1] (d3d9_present_ringedge2.c /
 * d3d11_present_ringedge2.c) are deliberately NOT prototyped here. Each is
 * defined with its own concrete D3D9/D3D11/DXGI signature in its own file, and
 * jvs_serial_ringedge2.c (the only other caller) forward-declares each locally
 * with a loose, ABI-compatible signature (same parameter slots/calling
 * convention) right where it uses them -- declaring the loose form here too
 * would conflict with the concrete definition in the same translation unit.
 * This mirrors the reference native_x86 hook's own pattern for the identical
 * reason. */

#endif /* _WIN32 */
#endif /* ULTRA_RINGEDGE2_HOOK_H */
