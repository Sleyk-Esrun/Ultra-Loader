/* SPDX-License-Identifier: MIT
 * NOTE: independent namco_es3 board copy -- adapted from native_x86/src/hook/board_profile.c (reference only; no build-time or runtime dependency on native_x86 remains).
 * =============================================================================
 *  board_profile_es3.c  --  the UNIFIED clean-room board-profile table + arming.
 * =============================================================================
 *
 *  One descriptor per x86 arcade PC board, recording (from PUBLIC
 *  hardware/protocol facts only) which graphics API its titles render with, which
 *  I/O transport carries its inputs, which protection scheme gates it, how the
 *  clean-room hook is injected, which present-throttle hook arms, and HONESTLY how
 *  far the board can be taken without a game dump + the physical dongle/keychip.
 *
 *  The native_x86 module resolves a profile by system_id at init and arms exactly
 *  the hooks the board needs, exactly like the proven Type X / Type X2 path. No
 *  game runs from this table alone -- it is the scaffolding + readiness record.
 *
 *  ORDERING: Taito Type X / X2 (the WORKING lane) and the Taito Type X3/X4/Zero/
 *  NESiCA priority targets come FIRST, then Sega, Namco, Konami, exA, UNICO.
 * =============================================================================
 */
#include "board_profile.h"
#include "sega_board.h"   /* link Sega entries to the keychip/container detail */

#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <stdlib.h>
#endif

/* The Type X (D3D8) and Type X2 (D3D9) entries describe the ALREADY-PLAYABLE lane
 * (BP_READY_PLAYABLE). Everything past Type X2 is scaffolding wired and ready for a
 * dump. The two playable entries arm exactly what the current lane arms, so
 * resolving them is byte-faithful to the existing behavior. */

static const board_profile g_profiles[] = {

  /* ===================== TAITO (priority lane) =========================== */

  /* --- the two PROVEN, PLAYABLE boards (Chaos Breaker / Haunted Museum / Wacky Race) --- */
  { "taito_typex", "Taito Type X", BP_VENDOR_TAITO, BP_GFX_D3D8, BP_OS_WINXP_EMBEDDED,
    BP_IO_JVS_SERIAL | BP_IO_DINPUT8,
    BP_PROT_USB_DONGLE,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D8, 32, BP_READY_PLAYABLE, 0,
    "PLAYABLE: Chaos Breaker end-to-end (JVS + D3D8 throttle/sharpen + dongle/dialog "
    "bypass). The original working lane; unchanged by this framework." },

  { "taito_typex2", "Taito Type X2", BP_VENDOR_TAITO, BP_GFX_D3D9, BP_OS_WINXP_EMBEDDED,
    BP_IO_JVS_SERIAL | BP_IO_DINPUT8 | BP_IO_LIGHTGUN | BP_IO_ANALOG_WHEEL,
    BP_PROT_USB_DONGLE,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D9, 32, BP_READY_PLAYABLE, 0,
    "PLAYABLE: Haunted Museum (lightgun, full credit loop) + Wacky Race (D3D9 racing, "
    "JVS analog wheel). D3D9 present-throttle + force-windowed. The working lane." },

  /* --- Type X3: D3D9-predominant Sandy-Bridge board; closest to Type X2 ---- */
  { "taito_typex3", "Taito Type X3", BP_VENDOR_TAITO, BP_GFX_D3D9, BP_OS_WIN7_EMBEDDED,
    BP_IO_JVS_SERIAL | BP_IO_FAST_IO | BP_IO_DINPUT8 | BP_IO_LIGHTGUN | BP_IO_CARD_READER,
    BP_PROT_USB_DONGLE | BP_PROT_ATA_LOCK | BP_PROT_CONTAINER | BP_PROT_NESICA,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D9, 64, BP_READY_DRM_GATED, 0,
    "Reuses the Type X2 D3D9 + JVS lane; adds the iDmacDrv 'Fast I/O' DMAC shim + a "
    "board profile. Gunslinger Stratos = lightgun. ATA-lock + USB dongle handled "
    "OUTSIDE the hook (user supplies a decrypted dump); in-binary check via "
    "ULTRA_PATCH_FILE. SCAFFOLDED." },

  /* --- Type X4: the D3D11/UE4 board -- the headline new capability -------- */
  { "taito_typex4", "Taito Type X4", BP_VENDOR_TAITO, BP_GFX_D3D11, BP_OS_WIN8_EMBEDDED,
    BP_IO_JVS_SERIAL | BP_IO_FAST_IO | BP_IO_XINPUT | BP_IO_DINPUT8 | BP_IO_LIGHTGUN | BP_IO_CARD_READER,
    BP_PROT_USB_DONGLE | BP_PROT_ATA_LOCK | BP_PROT_CONTAINER | BP_PROT_NESICA,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D11, 64, BP_READY_DRM_GATED, 0,
    "Arms the new clean-room D3D11/DXGI present hook (SFV/SF6 Type Arcade, Densha de "
    "Go!! UE4, KOF XV). 64-bit lane + 64-bit Fast-I/O shim. UE4 titles may need "
    "windowed coercion like the Type X2 device-LOST fix. DRM (dongle+ATA+container) "
    "outside the hook. SCAFFOLDED." },

  /* --- Type X Zero: low-power Atom D3D9 board ----------------------------- */
  { "taito_typexzero", "Taito Type X Zero", BP_VENDOR_TAITO, BP_GFX_D3D9, BP_OS_WIN7_EMBEDDED, /* module id: taito_typexzero */
    BP_IO_JVS_SERIAL | BP_IO_FAST_IO | BP_IO_TOUCH | BP_IO_CARD_READER,
    BP_PROT_ATA_LOCK | BP_PROT_NESICA,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D9, 32, BP_READY_DRM_GATED, 0,
    "Graphics path identical to Type X2 (D3D9, already working); weak Atom CPU makes "
    "the 60Hz frame-limiter matter more (already implemented). Most of its catalog "
    "arrives via NESiCAxLive (see taito_nesica). SCAFFOLDED." },

  /* --- NESiCAxLive: a DRM/distribution LAYER, not a board ----------------- */
  { "taito_nesica", "Taito NESiCAxLive / NESiCAxLive2", BP_VENDOR_TAITO, BP_GFX_MIXED, BP_OS_WIN7_EMBEDDED,
    BP_IO_JVS_SERIAL | BP_IO_CARD_READER,
    BP_PROT_NESICA | BP_PROT_CONTAINER | BP_PROT_NETWORK,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D9_AND_D3D11, 64, BP_READY_DRM_GATED, 0,
    "A digital-distribution + per-title-encryption layer on Type X2/Zero/X3/X4 (inherits "
    "the host board's graphics). Ultra Loader ships NO keys/crypto: runs only a "
    "user-supplied DECRYPTED dump, presents a STUB NESiCA card so a card-touch start "
    "proceeds (env-gated, no card data), neutralizes residual online checks via "
    "ULTRA_PATCH_FILE + dialog-dismiss. Crypto/online is key-gated, out of clean-room "
    "scope. SCAFFOLDED (policy/content-gated)." },

  /* ===================== SEGA ============================================ */
  /* These mirror the Sega-specific sega_board.{h,c} detail layer; sega_kind links
   * to the keychip/container kind there. The module's SYSTEMS[] uses coarse Sega
   * ids (sega_ringedge/sega_nu/sega_alls/...); finer revs live in sega_board.c. */

  { "sega_ringedge", "Sega RingEdge", BP_VENDOR_SEGA, BP_GFX_D3D9, BP_OS_WIN_EMBEDDED_2009,
    BP_IO_IO4_USB | BP_IO_JVS_SERIAL | BP_IO_LIGHTGUN | BP_IO_ANALOG_WHEEL,
    BP_PROT_KEYCHIP | BP_PROT_CONTAINER | BP_PROT_AMAUTH | BP_PROT_ALLNET | BP_PROT_NETWORK,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D9, 32, BP_READY_DRM_GATED, SEGA_BOARD_RINGEDGE,
    "D3D9 + IO4-USB/JVS (both solved on Type X). PIC mxparallel keychip unlocks a "
    "TrueCrypt container -> needs an operator keychip or a decrypted image. Best "
    "near-term Sega target (Border Break, gun games). SCAFFOLDED." },

  { "sega_ringwide", "Sega RingWide", BP_VENDOR_SEGA, BP_GFX_D3D9, BP_OS_WIN_EMBEDDED_2009,
    BP_IO_IO4_USB | BP_IO_JVS_SERIAL,
    BP_PROT_KEYCHIP | BP_PROT_CONTAINER | BP_PROT_AMAUTH | BP_PROT_ALLNET | BP_PROT_NETWORK,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D9, 32, BP_READY_DRM_GATED, SEGA_BOARD_RINGWIDE,
    "Low-end RingEdge sibling; identical mxparallel keychip + TrueCrypt DRM path. "
    "SCAFFOLDED." },

  { "sega_ringedge2", "Sega RingEdge 2", BP_VENDOR_SEGA, BP_GFX_MIXED, BP_OS_WIN_EMBEDDED_2009,
    BP_IO_IO4_USB | BP_IO_JVS_SERIAL,
    BP_PROT_KEYCHIP | BP_PROT_CONTAINER | BP_PROT_AMAUTH | BP_PROT_ALLNET | BP_PROT_NETWORK,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D9_AND_D3D11, 32, BP_READY_DRM_GATED, SEGA_BOARD_RINGEDGE2,
    "i3-540 + GT545; later titles edge toward D3D11 (both hooks armed). Same "
    "TrueCrypt/keychip wall; best-documented NoKey/ReKey softmod exists for "
    "operator-owned units. SCAFFOLDED." },

  { "sega_nu", "Sega Nu", BP_VENDOR_SEGA, BP_GFX_D3D11, BP_OS_WIN8_EMBEDDED,
    BP_IO_IO4_USB,
    BP_PROT_KEYCHIP | BP_PROT_CONTAINER | BP_PROT_TPM | BP_PROT_AMAUTH | BP_PROT_ALLNET | BP_PROT_AMDAEMON | BP_PROT_NETWORK,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D11, 64, BP_READY_DRM_GATED, SEGA_BOARD_NU,
    "DX11.1 DXGI present hook + IO4-USB. USB A7/N2 smart-card keychip via fdd.sys + "
    "BitLocker(TPM) container; GkeyEnc/Dec (cmd 11/12) gate the game key. Per-game key "
    "lives in keychip silicon, off every dump. SCAFFOLDED." },

  { "sega_nu2", "Sega Nu 2 (Nu 1.1/2/SX)", BP_VENDOR_SEGA, BP_GFX_D3D11, BP_OS_WIN8_EMBEDDED,
    BP_IO_IO4_USB,
    BP_PROT_KEYCHIP | BP_PROT_CONTAINER | BP_PROT_TPM | BP_PROT_AMAUTH | BP_PROT_ALLNET | BP_PROT_AMDAEMON | BP_PROT_NETWORK,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D11, 64, BP_READY_DRM_GATED, SEGA_BOARD_NU2,
    "Nu revision; N2 (L3.1) keychip variant. Same USB keychip + BitLocker DRM as Nu. "
    "SCAFFOLDED." },

  { "sega_alls", "Sega ALLS (HX/UX/UX2/MX2)", BP_VENDOR_SEGA, BP_GFX_D3D11, BP_OS_WIN10_IOT,
    BP_IO_IO4_USB | BP_IO_LIGHTGUN | BP_IO_TOUCH,
    BP_PROT_KEYCHIP | BP_PROT_CONTAINER | BP_PROT_TPM | BP_PROT_AMAUTH | BP_PROT_ALLNET | BP_PROT_AMDAEMON | BP_PROT_NETWORK,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D11, 64, BP_READY_DRM_GATED, SEGA_BOARD_ALLS_HX,
    "Win10 IoT, D3D11. The hardest/most-current Sega DRM: USB N2 (L3.1) keychip via "
    "fdd.sys->ssd.sys + BitLocker(TPM+recovery). maimai DX / CHUNITHM / Initial D THE "
    "ARCADE / gun cabs (UX wires the lightgun bridge). Per-game custom I/O (slider/"
    "touch/IR) is a per-title shim. SCAFFOLDED." },

  /* --- Sega aliases / framework slots so every module system_id resolves --- */
  { "sega_lindbergh", "Sega Lindbergh (Linux/x86)", BP_VENDOR_SEGA, BP_GFX_GL, BP_OS_LINUX,
    BP_IO_JVS_SERIAL,
    BP_PROT_USB_DONGLE | BP_PROT_CONTAINER,
    BP_INJECT_LINUX_ELF, BP_HOOK_GL, 32, BP_READY_PLAYABLE, 0,
    "Linux/ELF Sega board run by Ultra Loader's OWN standalone Lindbergh ELF32 lane "
    "(ABC playable, GSEVO credit loop) -- NOT this Win32 hook. Listed here for the "
    "unified table; the native_x86 module routes it to the ELF loader." },

  { "sega_europar", "Sega Europa-R (Windows/x86)", BP_VENDOR_SEGA, BP_GFX_D3D9, BP_OS_WIN_EMBEDDED_2009,
    BP_IO_IO4_USB | BP_IO_JVS_SERIAL | BP_IO_ANALOG_WHEEL,
    BP_PROT_KEYCHIP | BP_PROT_CONTAINER | BP_PROT_AMAUTH | BP_PROT_ALLNET | BP_PROT_NETWORK,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D9, 32, BP_READY_DRM_GATED, SEGA_BOARD_RINGEDGE,
    "RingEdge-class Sega driving board (Europa-R). FULLY WIRED at RingEdge parity: "
    "D3D9 present hook + IO4-USB/JVS I/O (ULTRA_IO4) + analog wheel + mxparallel "
    "keychip presence (ULTRA_SEGA_KEYCHIP) + container/dongle bypass + spawn-suspended "
    "inject. Reuses SEGA_BOARD_RINGEDGE for the keychip/TrueCrypt detail. DRM-gated "
    "(operator keychip or a decrypted container image)." },

  /* NB: the canonical "sega_nu2" entry is above (the Nu/Nu2/SX revision line); the
   * module's coarse id resolves to it. No second alias entry -- lookup returns the
   * FIRST match, so a duplicate would be dead/divergent. */

  /* ===================== NAMCO ========================================== */

  { "namco_es3", "Namco System ES3 / ES3X", BP_VENDOR_NAMCO, BP_GFX_MIXED, BP_OS_WIN7_EMBEDDED,
    BP_IO_JVS_USB | BP_IO_ANALOG_WHEEL | BP_IO_LIGHTGUN | BP_IO_XINPUT,
    BP_PROT_TPM | BP_PROT_AMDAEMON | BP_PROT_NETWORK,
    BP_INJECT_INTO_DAEMON, BP_HOOK_D3D9_AND_D3D11, 64, BP_READY_DUMP_GATED, 0,
    "Most-wanted Namco family. Wheel titles use a D3D9 wrapper; the UE titles are D3D11 (both "
    "hooks armed). ESIF2 JVS-over-USB I/O. Protection = the amdaemon auth daemon + I/O "
    "presence (no heavy in-binary packer on leaked titles), so it is DUMP-gated rather "
    "than key-gated. amdaemon stub answers the auth handshake (BootID/OK). Time Crisis "
    "5 = lightgun. SCAFFOLDED, content-gated." },

  { "namco_es1", "Namco System ES1 / ES2 (Windows variant)", BP_VENDOR_NAMCO, BP_GFX_D3D9, BP_OS_WIN7_EMBEDDED,
    BP_IO_JVS_USB | BP_IO_ANALOG_WHEEL,
    BP_PROT_TPM | BP_PROT_CONTAINER | BP_PROT_USB_DONGLE | BP_PROT_AMDAEMON | BP_PROT_NETWORK,
    BP_INJECT_INTO_DAEMON, BP_HOOK_D3D9, 64, BP_READY_DRM_GATED, 0,
    "Only the Windows-Embedded ES1/ES2+ titles are in-scope (Linux titles need a "
    "separate loader). 3-stage DRM: TPM trusted-boot + loop-AES/LUKS game files bound "
    "to PCR values + HASP HL Max USB dongle + custom HDD firmware. The per-machine "
    "PCR-bound key makes even a dump unusable -- hardest in the family. SCAFFOLDED." },

  { "namco_n2", "Namco System N2 (Windows variant)", BP_VENDOR_NAMCO, BP_GFX_GL, BP_OS_WIN7_EMBEDDED,
    BP_IO_JVS_SERIAL,
    BP_PROT_USB_DONGLE,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_GL, 32, BP_READY_DRM_GATED, 0,
    "Pre-ES board, OpenGL pipeline (GeForce). V337 RC/RS + RATOC PCI604 (USB + RS232 "
    "serial -> the RS232 leg rides the JVS serial hook). FULLY WIRED for the WINDOWS "
    "N2 variant: the real clean-room GL present hook (gl_present.c: wglSwapBuffers/"
    "SwapBuffers frame-limit + glReadPixels capture + force-windowed; ULTRA_GL_PRESENT/"
    "ULTRA_GL_BOARD) + JVS-serial I/O + per-game USB-dongle presence + spawn-suspended "
    "inject. NB this is the Windows/PE N2; the Linux/ELF N2 image routes to the "
    "standalone ELF lane instead (board_is_linux selects by image, not id). DRM-gated "
    "(per-game serial dongle)." },

  /* --- Namco aliases / framework slots so every module system_id resolves --- */
  { "namco_es2plus", "Namco System ES2 Plus (Windows variant)", BP_VENDOR_NAMCO, BP_GFX_D3D9, BP_OS_WIN7_EMBEDDED,
    BP_IO_JVS_USB | BP_IO_ANALOG_WHEEL,
    BP_PROT_TPM | BP_PROT_CONTAINER | BP_PROT_USB_DONGLE | BP_PROT_AMDAEMON | BP_PROT_NETWORK,
    BP_INJECT_INTO_DAEMON, BP_HOOK_D3D9, 64, BP_READY_DRM_GATED, 0,
    "Windows-Embedded ES2+ variant; D3D9 + ESIF JVS-over-USB. Same TPM/AES + HASP + "
    "amdaemon family as ES1. SCAFFOLDED." },

  { "namco_es4", "Namco System ES4 (Windows/x86)", BP_VENDOR_NAMCO, BP_GFX_D3D11, BP_OS_WIN8_EMBEDDED,
    BP_IO_JVS_USB | BP_IO_XINPUT,
    BP_PROT_TPM | BP_PROT_AMDAEMON | BP_PROT_NETWORK,
    BP_INJECT_INTO_DAEMON, BP_HOOK_D3D11, 64, BP_READY_DUMP_GATED, 0,
    "Later Namco PC board (D3D11). Fully wired: D3D11 DXGI present hook + JVS-over-USB "
    "I/O (namco_io.c, ULTRA_NAMCO_IO) + amdaemon/amauthd LOCAL presence (presence/auth-"
    "state only; online/key/content DEFERRED). Case-A virtual-COM bridges fall to the "
    "serial hook; case-B WinUSB bridges arm on ULTRA_NAMCO_IO_VIDPID. DUMP-gated." },

  { "namco_bna1", "Namco System BNA1 / Lite (Win 10 IoT)", BP_VENDOR_NAMCO, BP_GFX_D3D11, BP_OS_WIN10_IOT,
    BP_IO_JVS_USB | BP_IO_XINPUT | BP_IO_TOUCH,
    BP_PROT_TPM | BP_PROT_AMDAEMON | BP_PROT_NETWORK,
    BP_INJECT_INTO_DAEMON, BP_HOOK_D3D11, 64, BP_READY_DUMP_GATED, 0,
    "Newest Namco PC board (Win10 IoT, D3D11). Fully wired: D3D11 DXGI present hook + "
    "JVS-over-USB I/O (namco_io.c) + amdaemon/amauthd LOCAL presence. Same Namco "
    "transport + ethical line as ES4 (online/key/content DEFERRED). DUMP-gated." },

  /* ===================== OTHER ========================================== */
  { "rawthrills_pc", "RAW Thrills PC-based", BP_VENDOR_OTHER, BP_GFX_MIXED, BP_OS_WIN7_EMBEDDED,
    BP_IO_JVS_SERIAL | BP_IO_ANALOG_WHEEL | BP_IO_LIGHTGUN,
    BP_PROT_USB_DONGLE,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D9_AND_D3D11, 64, BP_READY_DUMP_GATED, 0,
    "RAW Thrills PC arcade (racing/gun titles). FULLY WIRED: mixed D3D9 + D3D11 present "
    "hooks both armed (ULTRA_D3D11; first create-fn wins = runtime detect) + JVS-serial "
    "I/O + analog wheel + universal lightgun bridge (ULTRA_LIGHTGUN) + USB-dongle/dialog "
    "bypass + spawn-suspended inject. (A newer JVS-over-USB cabinet variant would arm "
    "ULTRA_NAMCO_IO instead via BP_IO_JVS_USB.) DUMP-gated -- runs a user-supplied "
    "decrypted dump." },

  /* ===================== KONAMI ========================================= */

  { "konami_pc", "Konami Bemani PC / ADE-704 / ADE-6291", BP_VENDOR_KONAMI, BP_GFX_MIXED, BP_OS_WINXP_EMBEDDED,
    BP_IO_KONAMI_IO | BP_IO_TOUCH | BP_IO_CARD_READER,
    BP_PROT_USB_DONGLE | BP_PROT_DATA_CRYPT | BP_PROT_NETWORK,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D9_AND_D3D11, 32, BP_READY_DRM_GATED, 0,
    "D3D9 on XP-Embedded Bemani PC / early ADE; modern IIDX Lightning shifts to DX11 "
    "(both hooks armed). NOT JVS: custom Konami P3IO/P4IO/KFCA/BIO2 bit-protocol per "
    "game (turntable/keys/slider) -> a per-game clean-room I/O shim. iKey 2032 dongle "
    "presence stub. RSA/AES-encrypted game data + Paseli/e-amusement is the real wall "
    "(keys content-gated, may be infeasible). SCAFFOLD + I/O framework only." },

  /* ===================== exA-Arcadia ==================================== */

  { "exa_arcadia", "exA-Arcadia", BP_VENDOR_EXA, BP_GFX_D3D11, BP_OS_WIN10_IOT,
    BP_IO_JVS_SERIAL,
    BP_PROT_CART_HASP | BP_PROT_CONTAINER,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D11, 64, BP_READY_DRM_GATED, 0,
    "FRAMEWORK SLOT ONLY -- a LIVE, actively-sold commercial platform (2019+). Cartridge "
    "= encrypted SSD + HASP USB dongle. D3D11/DXGI + JVS posture documented, but Ultra "
    "Loader does NOT target retail titles: no dumps, no playability claim. Ethics-gated." },

  /* ===================== CAVE =========================================== */

  { "cave_pc", "Cave PC (Ver.A/B)", BP_VENDOR_OTHER, BP_GFX_MIXED, BP_OS_WINXP_EMBEDDED,
    BP_IO_JVS_SERIAL,
    BP_PROT_USB_DONGLE | BP_PROT_ATA_LOCK,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D9_AND_D3D11, 32, BP_READY_DUMP_GATED, 0,
    "Cave bullet-hell shmup platform (Deathsmiles II, DoDonPachi SaiDaiOuJou, Akai Katana "
    "Shin, Espgaluda II, Ketsui, Muchi Muchi Pork). D3D9 mainline + D3D11 for SDOJ Peak. "
    "JVS digital 8-way + 3 buttons. SafeNet HASP dongle + ATA-locked HDD. DUMP-GATED." },

  /* ===================== UNICO ========================================== */

  { "unico_pc", "UNICO (Korea) PC boards", BP_VENDOR_UNICO, BP_GFX_MIXED, BP_OS_WIN7_EMBEDDED,
    BP_IO_JVS_SERIAL | BP_IO_ANALOG_WHEEL,
    BP_PROT_USB_DONGLE,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_D3D9_AND_D3D11, 64, BP_READY_DUMP_GATED, 0,
    "Korean PC racing/redemption boards. FULLY WIRED: mixed D3D9 + D3D11 present hooks "
    "both armed (ULTRA_D3D11; detect-at-runtime, first create-fn wins) + JVS-serial I/O "
    "+ wheel/pedal analog + USB-dongle/dialog bypass + spawn-suspended inject. DUMP-"
    "gated -- runs a user-supplied decrypted dump (board shape inferred from public PC-"
    "arcade facts; finer per-title tuning waits on a real image)." },

  /* ===================== generic fallback =============================== */
  { "generic_pc", "Generic PC-based arcade board", BP_VENDOR_OTHER, BP_GFX_MIXED, BP_OS_UNKNOWN,
    BP_IO_JVS_SERIAL | BP_IO_DINPUT8,
    BP_PROT_USB_DONGLE,
    BP_INJECT_SPAWN_SUSPENDED, BP_HOOK_NONE, 32, BP_READY_FRAMEWORK, 0,
    "Default profile for an unrecognised PC arcade dump: JVS + dongle/dialog bypass, no "
    "present hook auto-armed (the user picks the renderer via ULTRA_* knobs). The "
    "host/user refines to a specific board at init." },
};

#define BP_N ((int)(sizeof(g_profiles)/sizeof(g_profiles[0])))

const board_profile* board_profile_lookup(const char* system_id) {
    if (!system_id) return 0;
    for (int i = 0; i < BP_N; ++i)
        if (g_profiles[i].system_id && strcmp(g_profiles[i].system_id, system_id) == 0)
            return &g_profiles[i];
    return 0;
}
int board_profile_count(void) { return BP_N; }
const board_profile* board_profile_at(int index) {
    if (index < 0 || index >= BP_N) return 0;
    return &g_profiles[index];
}

const char* bp_gfx_str(bp_gfx_api a) {
    switch (a) {
        case BP_GFX_D3D8:  return "D3D8";
        case BP_GFX_D3D9:  return "D3D9";
        case BP_GFX_D3D11: return "D3D11";
        case BP_GFX_GL:    return "OpenGL";
        case BP_GFX_MIXED: return "D3D9+D3D11(mixed)";
        default:           return "none";
    }
}
const char* bp_inject_str(bp_inject i) {
    switch (i) {
        case BP_INJECT_SPAWN_SUSPENDED: return "spawn-suspended";
        case BP_INJECT_INTO_DAEMON:     return "inject-into-daemon";
        case BP_INJECT_LINUX_ELF:       return "linux-elf";
        default:                        return "none";
    }
}
const char* bp_hook_str(bp_present_hook h) {
    switch (h) {
        case BP_HOOK_D3D8:          return "d3d8";
        case BP_HOOK_D3D9:          return "d3d9";
        case BP_HOOK_D3D11:         return "d3d11";
        case BP_HOOK_GL:            return "gl";
        case BP_HOOK_D3D9_AND_D3D11:return "d3d9+d3d11";
        default:                    return "none";
    }
}
const char* bp_ready_str(bp_readiness r) {
    switch (r) {
        case BP_READY_SCAFFOLDED: return "SCAFFOLDED";
        case BP_READY_DUMP_GATED: return "DUMP-GATED";
        case BP_READY_DRM_GATED:  return "DRM-GATED";
        case BP_READY_PLAYABLE:   return "PLAYABLE";
        default:                  return "FRAMEWORK";
    }
}
const char* bp_vendor_str(bp_vendor v) {
    switch (v) {
        case BP_VENDOR_TAITO:  return "Taito";
        case BP_VENDOR_SEGA:   return "Sega";
        case BP_VENDOR_NAMCO:  return "Namco";
        case BP_VENDOR_KONAMI: return "Konami";
        case BP_VENDOR_EXA:    return "exA-Arcadia";
        case BP_VENDOR_UNICO:  return "UNICO";
        default:               return "various";
    }
}

/* =============================================================================
 *  Arming.  Map the resolved profile -> the ULTRA_* env knobs the injected hook
 *  reads on attach. ADDITIVE + byte-safe: a knob is set ONLY when the profile
 *  needs it, and the two PLAYABLE Taito entries arm exactly the (default) set the
 *  current lane already uses -- so resolving them changes nothing.
 *
 *  We DO NOT set any input/autopilot/dump/cosmetic knob here (ULTRA_WR_*,
 *  ULTRA_TYPEX2_*, ULTRA_FPS_CAP, ULTRA_ROTATE, ...). Those stay user/profile
 *  driven and default-off. We set only the *board-shape* knobs: which present
 *  hook to arm and which protection shim to enable. The D3D8/D3D9 present paths
 *  are always-on diagnostics in the hook already (they no-op unless a knob arms
 *  the limiter), so they need no env knob -- only D3D11 + the Sega keychip do.
 * ============================================================================= */
#if defined(_WIN32)
/* set an env var for the child ONLY if the user has not already set it, so an
 * explicit user override always wins over the profile default. */
static void set_if_unset(const char* k, const char* v) {
    char tmp[8];
    DWORD n = GetEnvironmentVariableA(k, tmp, sizeof(tmp));
    if (n == 0) SetEnvironmentVariableA(k, v);   /* unset -> apply profile default */
}
#endif

/* Convenience string accessors keyed by id (so the C++ module can log a resolved
 * profile without pulling the full struct layout across the language boundary). */
const char* bp_gfx_str_of(const char* system_id) {
    const board_profile* p = board_profile_lookup(system_id);
    return p ? bp_gfx_str(p->gfx) : "?";
}
const char* bp_hook_str_of(const char* system_id) {
    const board_profile* p = board_profile_lookup(system_id);
    return p ? bp_hook_str(p->hook) : "?";
}
const char* bp_ready_str_of(const char* system_id) {
    const board_profile* p = board_profile_lookup(system_id);
    return p ? bp_ready_str(p->ready) : "?";
}
const char* bp_vendor_str_of(const char* system_id) {
    const board_profile* p = board_profile_lookup(system_id);
    return p ? bp_vendor_str(p->vendor) : "?";
}
const char* bp_inject_str_of(const char* system_id) {
    const board_profile* p = board_profile_lookup(system_id);
    return p ? bp_inject_str(p->inject) : "?";
}

int board_profile_arm(const char* system_id, int apply_env) {
    const board_profile* p = board_profile_lookup(system_id);
    if (!p) return 0;

    /* ETHICS GATE: exA-Arcadia is a LIVE, actively-sold commercial platform. Its
     * descriptor exists ONLY so the unified table is complete; Ultra Loader does NOT
     * wire, arm, or hook it. Resolving it is fine (returns 1 so the id is "known"),
     * but NO ULTRA_* knob is ever set for it -- not even a present-hook knob. The
     * framework slot stays untouched. This is BEFORE any apply_env work. */
    if (p->vendor == BP_VENDOR_EXA) return 1;

#if defined(_WIN32)
    if (apply_env) {
        /* record the resolved board for the hook/logs */
        SetEnvironmentVariableA("ULTRA_BOARD", p->system_id);

        /* present hook: arm D3D11 when the board renders D3D11 (or mixed). The hook
         * reads BOTH ULTRA_SEGA_D3D11 (legacy) and ULTRA_D3D11 (vendor-neutral). */
        int want_d3d11 = (p->hook == BP_HOOK_D3D11 || p->hook == BP_HOOK_D3D9_AND_D3D11);
        if (want_d3d11) {
            set_if_unset("ULTRA_D3D11", "1");
            set_if_unset("ULTRA_SEGA_D3D11", "1");   /* same arming line in the hook */
        }

        /* present hook: arm the OpenGL swap-buffer throttle/capture when the board
         * renders via GL (Namco N2 + any GL board). The hook reads BOTH
         * ULTRA_GL_PRESENT (vendor-neutral) and ULTRA_GL_BOARD (the 60Hz cabinet
         * default). Only the in-host Win32 GL path is armed; the Linbergh-class
         * BP_INJECT_LINUX_ELF GL boards use the standalone ELF loader, not this hook. */
        if (p->hook == BP_HOOK_GL && p->inject != BP_INJECT_LINUX_ELF) {
            set_if_unset("ULTRA_GL_PRESENT", "1");
            set_if_unset("ULTRA_GL_BOARD", "1");
        }

        /* Sega keychip presence shim: only Sega keychip boards. Default region 'A'
         * (overridable by the user via ULTRA_SEGA_REGION; we never clobber it). */
        if (p->vendor == BP_VENDOR_SEGA && (p->prot & BP_PROT_KEYCHIP))
            set_if_unset("ULTRA_SEGA_KEYCHIP", "1");

        /* Sega IO4-USB (837-15257) I/O emulation: any board whose I/O is IO4-USB.
         * JVS-backed inputs/analog/coins; lamps accepted. Env-gated + idempotent. */
        if (p->io & BP_IO_IO4_USB)
            set_if_unset("ULTRA_IO4", "1");

        /* Konami Bemani-PC ACIO I/O scaffold: any board whose I/O is the Konami
         * custom bit-protocol. Per-game node ID + bit-map are user-pinned knobs. */
        if (p->io & BP_IO_KONAMI_IO)
            set_if_unset("ULTRA_KONAMI_IO", "1");

        /* Namco/Sega ES amdaemon LOCAL presence stub: boards gated on the local
         * auth daemon. Answers presence + offline-OK only; online is never faked. */
        if (p->prot & BP_PROT_AMDAEMON)
            set_if_unset("ULTRA_AMDAEMON", "1");

        /* Namco JVS-over-USB I/O + amauthd LOCAL presence: any Namco board whose I/O
         * is JVS-over-USB (ESIF). Bridges JVS through the shared clean-room node + a
         * LOCAL presence/auth-state surface (online/key/content always DEFERRED). The
         * raw-WinUSB (case B) device match stays inert until the user pins the bridge
         * VID/PID (ULTRA_NAMCO_IO_VIDPID); a virtual-COM (case A) bridge is handled by
         * the serial hook via ULTRA_JVS_COM. Env-gated + idempotent. */
        if (p->vendor == BP_VENDOR_NAMCO && (p->io & BP_IO_JVS_USB))
            set_if_unset("ULTRA_NAMCO_IO", "1");

        /* Taito Fast-I/O (iDmacDrv) shim hint for the boards that use it. The shim
         * is itself env-gated + idempotent; this only advertises the board needs it. */
        if (p->io & BP_IO_FAST_IO)
            set_if_unset("ULTRA_FAST_IO", "1");

        /* card-reader (NESiCA / Aime) stub hint. */
        if (p->io & BP_IO_CARD_READER)
            set_if_unset("ULTRA_CARD_READER", "1");

        /* lightgun cabinets: advertise the gun bridge so the loader wires it. */
        if (p->io & BP_IO_LIGHTGUN)
            set_if_unset("ULTRA_LIGHTGUN", "1");
    }
#else
    (void)apply_env;
#endif
    return 1;
}
