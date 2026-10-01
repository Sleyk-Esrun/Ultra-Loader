/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  board_profile.h  --  Ultra Loader UNIFIED clean-room board-profile framework
 *                       for the native_x86 (M3 launch-and-inject) arcade families.
 * =============================================================================
 *
 *  Ultra Loader already runs Taito Type X (D3D8) and Type X2 (D3D9) through a
 *  clean-room hook (JVS engine, D3D8/D3D9/D3D11 present-throttle, suspended-launch
 *  injection + IAT/GetProcAddress interception, lightgun bridge, dongle/dialog
 *  bypass). This header defines ONE vendor-neutral descriptor that lets every
 *  x86 arcade PC board declare, from PUBLIC hardware/protocol
 *  facts only, exactly which present-hook + I/O transport + injection strategy +
 *  protection handler it needs -- so launching a game RESOLVES its board profile
 *  and arms the right hook config.
 *
 *  ABSOLUTE CLEAN-ROOM: every field below is derived from PUBLIC hardware/protocol
 *  facts (board spec sheets, the JVS spec, the Direct3D/DXGI SDK, public
 *  descriptions of each board's protection scheme). No third-party loader's source,
 *  config, or game profile was read, copied, transliterated, or mirrored. Ultra
 *  Loader ships NO game, NO dongle dump, NO keychip secret, NO decryption key. Each
 *  board is "wired and ready for a dump", NOT verified-playable.
 *
 *  RELATIONSHIP TO sega_board.h: sega_board.{h,c} is the Sega-SPECIFIC detail layer
 *  (keychip transport, TrueCrypt/BitLocker container kinds, GkeyEnc/Dec framing).
 *  board_profile.{h,c} is the UNIFIED cross-vendor table the module launch path
 *  reads; for a Sega board it points back at the matching sega_board_kind for the
 *  finer keychip/container detail. The Type X (D3D8) + Type X2 (D3D9) entries here
 *  describe the ALREADY-WORKING lane and must stay byte-faithful to it.
 * =============================================================================
 */
#ifndef ULTRA_BOARD_PROFILE_H
#define ULTRA_BOARD_PROFILE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- graphics API the title renders with (selects which present-hook arms) -- */
typedef enum {
    BP_GFX_NONE = 0,
    BP_GFX_D3D8,        /* Taito Type X                                          */
    BP_GFX_D3D9,        /* Type X2/X Zero, RingEdge/Wide/2, many Namco ES titles */
    BP_GFX_D3D11,       /* Type X3/X4, Nu/Nu2, ALLS, ES3 UE titles, exA, RE2 late*/
    BP_GFX_GL,          /* Namco N2/ES1 (OpenGL pipeline)                        */
    BP_GFX_MIXED,       /* family ships both D3D9 and D3D11 titles (detect@run)  */
} bp_gfx_api;

/* ---- OS the board image runs (informational + Linux/ELF vs Windows/PE route) */
typedef enum {
    BP_OS_UNKNOWN = 0,
    BP_OS_WINXP_EMBEDDED,   /* Type X/X2, early Bemani PC                        */
    BP_OS_WIN7_EMBEDDED,    /* Type X3, Namco ES2+/ES3, Win Embedded Std 7       */
    BP_OS_WIN8_EMBEDDED,    /* Type X4, Sega Nu/Nu2 (Win Embedded 8 Std)         */
    BP_OS_WIN10_IOT,        /* Sega ALLS, exA-Arcadia, modern Bemani             */
    BP_OS_WIN_EMBEDDED_2009,/* Sega RingEdge/Wide/2                              */
    BP_OS_LINUX,            /* Namco N2/ES1 (Debian), Sega Lindbergh (separate)  */
} bp_os;

/* ---- input transport the board's I/O uses (bitmask; a board may set several) */
#define BP_IO_JVS_SERIAL   0x0001u  /* classic JVS over a serial COM port        */
#define BP_IO_JVS_USB      0x0002u  /* JVS framing over a USB CDC / vendor pipe   */
#define BP_IO_DINPUT8      0x0004u  /* DirectInput8 device (some Type X titles)   */
#define BP_IO_XINPUT       0x0008u  /* XInput (fighting titles on X4/ES3)         */
#define BP_IO_FAST_IO      0x0010u  /* Taito iDmacDrv "Fast I/O" DMAC path        */
#define BP_IO_IO4_USB      0x0020u  /* Sega IO4 (837-15257) USB vendor protocol   */
#define BP_IO_KONAMI_IO    0x0040u  /* Konami P3IO/P4IO/KFCA/BIO2 custom I/O      */
#define BP_IO_CARD_READER  0x0080u  /* NESiCA / Aime / e-amusement card reader    */
#define BP_IO_LIGHTGUN     0x0100u  /* universal lightgun bridge (gun cabs)       */
#define BP_IO_ANALOG_WHEEL 0x0200u  /* wheel/pedal analog (racing cabs)           */
#define BP_IO_TOUCH        0x0400u  /* touch / slider / IR (music games)          */
#define BP_IO_CAN          0x0800u  /* CAN-bus driving I/O (some Sega cabs)        */

/* ---- protection scheme the board uses (bitmask) ---------------------------- */
#define BP_PROT_NONE       0x0000u
#define BP_PROT_USB_DONGLE 0x0001u  /* presence USB dongle (HASP/SafeNet/Taito)   */
#define BP_PROT_ATA_LOCK   0x0002u  /* ATA-password / motherboard-bound HDD       */
#define BP_PROT_KEYCHIP    0x0004u  /* Sega mxparallel / USB smart-card keychip   */
#define BP_PROT_TPM        0x0008u  /* TPM trusted-boot / PCR-bound key           */
#define BP_PROT_CONTAINER  0x0010u  /* encrypted container (TrueCrypt/BitLocker/LUKS) */
#define BP_PROT_AMAUTH     0x0020u  /* Sega amDongle / amBackup AmLib auth        */
#define BP_PROT_ALLNET     0x0040u  /* Sega ALL.Net online auth / billing         */
#define BP_PROT_NESICA     0x0080u  /* Taito NESiCAxLive online + per-title key    */
#define BP_PROT_NETWORK    0x0100u  /* needs a network/DNS redirect to come up    */
#define BP_PROT_AMDAEMON   0x0200u  /* Namco/Sega amdaemon auth-daemon gate       */
#define BP_PROT_DATA_CRYPT 0x0400u  /* per-game RSA/AES-encrypted data (Konami)   */
#define BP_PROT_CART_HASP  0x0800u  /* exA-Arcadia cartridge SSD + HASP dongle    */

/* ---- injection strategy the module uses to attach the clean-room hook ------ */
typedef enum {
    BP_INJECT_NONE = 0,
    BP_INJECT_SPAWN_SUSPENDED,  /* CreateProcess(SUSPENDED) -> inject -> resume    */
    BP_INJECT_INTO_DAEMON,      /* inject into amdaemon.exe / launcher, not game   */
    BP_INJECT_LINUX_ELF,        /* hand the ELF to Ultra Loader's native ELF lane  */
} bp_inject;

/* ---- which present-throttle hook arms (single primary; MIXED arms both) ----- */
typedef enum {
    BP_HOOK_NONE = 0,
    BP_HOOK_D3D8,
    BP_HOOK_D3D9,
    BP_HOOK_D3D11,
    BP_HOOK_GL,
    BP_HOOK_D3D9_AND_D3D11,     /* family ships both; arm both, first-wins         */
} bp_present_hook;

/* ---- readiness: how far Ultra Loader takes the board WITHOUT a dump --------- */
typedef enum {
    BP_READY_FRAMEWORK = 0, /* descriptor only                                     */
    BP_READY_SCAFFOLDED,    /* hooks compiled + wired + lifecycle-validated        */
    BP_READY_DUMP_GATED,    /* fully wired; blocked only on a (decrypted) game image*/
    BP_READY_DRM_GATED,     /* blocked on a physical dongle/keychip/decrypt key    */
    BP_READY_PLAYABLE,      /* verified end-to-end with a real dump (Type X/X2)     */
} bp_readiness;

/* ---- vendor (for grouping / reporting) ------------------------------------- */
typedef enum {
    BP_VENDOR_TAITO = 0,
    BP_VENDOR_SEGA,
    BP_VENDOR_NAMCO,
    BP_VENDOR_KONAMI,
    BP_VENDOR_EXA,
    BP_VENDOR_UNICO,
    BP_VENDOR_OTHER,
} bp_vendor;

/* =============================================================================
 *  THE UNIFIED BOARD DESCRIPTOR.
 *
 *  `system_id` is the SAME string the native_x86 module's SYSTEMS[] table uses
 *  (e.g. "taito_typex3", "sega_alls", "namco_es3") so the launch path can resolve
 *  a profile by the id the host already passes to ultra_module init.
 * ============================================================================= */
typedef struct board_profile {
    const char*     system_id;      /* module system id ("taito_typex3", ...)      */
    const char*     name;           /* human label                                 */
    bp_vendor       vendor;
    bp_gfx_api      gfx;
    bp_os           os;
    uint32_t        io;             /* BP_IO_* bitmask                             */
    uint32_t        prot;           /* BP_PROT_* bitmask                           */
    bp_inject       inject;
    bp_present_hook hook;
    int             bits;           /* 32 or 64 (the dominant title bitness)       */
    bp_readiness    ready;
    /* sega_board_kind if vendor==SEGA (else 0); links to the keychip/container
     * detail in sega_board.{h,c}. Stored as int to keep this header standalone. */
    int             sega_kind;
    const char*     notes;          /* one honest line on what is gated            */
} board_profile;

/* ---- lookups (definitions in board_profile.c) ------------------------------ */
const board_profile* board_profile_lookup(const char* system_id);
int                  board_profile_count(void);
const board_profile* board_profile_at(int index);

/* Human-readable enum -> string (for logging / the readiness report). */
const char* bp_gfx_str(bp_gfx_api a);
const char* bp_inject_str(bp_inject i);
const char* bp_hook_str(bp_present_hook h);
const char* bp_ready_str(bp_readiness r);
const char* bp_vendor_str(bp_vendor v);

/* =============================================================================
 *  Arming.  The module calls board_profile_arm(system_id) at init: it resolves the
 *  profile and sets the ULTRA_* env knobs the hook reads on attach (present hook,
 *  Fast-I/O, keychip, card reader, ...). The hook stays default-OFF for everything
 *  the resolved board does NOT need, so the Type X (D3D8) + Type X2 (D3D9) lanes
 *  are byte-identical -- they resolve to profiles that arm exactly what they
 *  already armed. Returns 1 on a known board, 0 if the id is not in the table.
 *
 *  apply_env: when nonzero, actually SetEnvironmentVariable for the child; when 0,
 *  only resolves (used by the structural test, which has no child to launch).
 * ============================================================================= */
int board_profile_arm(const char* system_id, int apply_env);

/* id-keyed string accessors (so a C++ consumer can log a resolved profile without
 * pulling the struct across the language boundary). */
const char* bp_gfx_str_of(const char* system_id);
const char* bp_hook_str_of(const char* system_id);
const char* bp_ready_str_of(const char* system_id);
const char* bp_vendor_str_of(const char* system_id);
const char* bp_inject_str_of(const char* system_id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ULTRA_BOARD_PROFILE_H */
