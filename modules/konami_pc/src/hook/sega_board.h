/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  sega_board.h  --  Ultra Loader clean-room framework for the Sega x86 arcade
 *                    PC boards (RingEdge / RingWide / Nu / ALLS).
 * =============================================================================
 *
 *  These boards are the modern Sega answer to the Lindbergh: commodity x86 PC
 *  motherboards running embedded Windows, with the GAME shipped as an ENCRYPTED
 *  container that a hardware keychip (RingEdge: parallel "mxparallel" PIC dongle;
 *  Nu/ALLS: USB smart-card dongle) unlocks at boot, plus a custom Sega USB I/O
 *  board ("IO4", 837-15257) carrying the cabinet inputs.
 *
 *  This header declares the per-board descriptor the native_x86 module fills in,
 *  so each Sega board is "wired and ready for a dump" in the same shape the Type X
 *  / Type X2 hooks already use. NOTHING here decrypts or ships a game -- it is the
 *  framework + honest readiness record. The heavy DRM (TrueCrypt/BitLocker game
 *  containers keyed off a physical keychip) is HARDWARE-GATED; without the operator
 *  keychip + an unencrypted (or operator-decrypted) game image, none of these run.
 *
 *  ABSOLUTE CLEAN-ROOM: every field below is derived from PUBLIC hardware/protocol
 *  facts (board spec sheets, the JVS spec, the Direct3D/DXGI SDK, public
 *  descriptions of Sega's keychip/ALL.Net protection). No third-party loader's
 *  source, config, or game profile was read or copied.
 * =============================================================================
 */
#ifndef ULTRA_SEGA_BOARD_H
#define ULTRA_SEGA_BOARD_H

#include <stdint.h>

/* ---- board generation ----------------------------------------------------- */
typedef enum {
    SEGA_BOARD_UNKNOWN = 0,
    SEGA_BOARD_RINGEDGE,        /* 2009, NVIDIA 8800GS, Win Embedded Std 2009    */
    SEGA_BOARD_RINGWIDE,        /* 2009, ATI Radeon HD2000, CompactFlash, 1xDVI  */
    SEGA_BOARD_RINGEDGE2,       /* 2011, Core i3-540 + GT545, Win Embedded Std   */
    SEGA_BOARD_NU,              /* 2013, i3-3220 + GTX650Ti, Win Embedded 8 Std  */
    SEGA_BOARD_NU2,             /* Nu rev (Nu 1.1/2/SX), same family             */
    SEGA_BOARD_ALLS_HX,         /* 2018, low-end ALLS, GeForce 10xx, Win10 IoT   */
    SEGA_BOARD_ALLS_UX,         /* 2018, high-end ALLS (GTX1070-class)           */
    SEGA_BOARD_ALLS_UX2,        /* 2nd rev high-end                              */
    SEGA_BOARD_ALLS_MX2,        /* 2nd rev mid                                   */
} sega_board_kind;

/* ---- graphics API the title renders with (drives which present-hook to arm) */
typedef enum {
    SEGA_GFX_UNKNOWN = 0,
    SEGA_GFX_D3D9,              /* RingEdge/RingWide/Nu-era titles               */
    SEGA_GFX_D3D11,            /* Nu (DX11.1-capable) and all ALLS titles        */
} sega_gfx_api;

/* ---- keychip / DRM transport ---------------------------------------------- *
 * RingEdge: a PIC-based parallel keychip on \\.\mxparallel, AES-128-ECB bus
 *   (per-direction keys), DS28CN01 SHA-1 challenge-response, region vs EEPROM,
 *   TrueCrypt game container whose keyfile (keychip.appboot.seed) is decrypted by
 *   the keychip; mxkeychip.exe is the user-space service every process talks to.
 * Nu/ALLS: a USB smart-card keychip (A7001AG JavaCard "A7", or "N2" L3.1), driven
 *   by fdd.sys -> hdd.sys/ssd.sys; GkeyEnc/GkeyDec (cmd 11/12) AES the game key;
 *   game container is BitLocker (TPM + recovery key) on Nu/ALLS; ALL.Net Auth 2.0
 *   key (cmd 32) for the online billing/auth handshake.                          */
typedef enum {
    SEGA_KEYCHIP_NONE = 0,
    SEGA_KEYCHIP_MXPARALLEL,   /* RingEdge/RingWide PIC parallel dongle          */
    SEGA_KEYCHIP_USB_A7,       /* Nu/ALLS NXP A7001AG JavaCard smart card        */
    SEGA_KEYCHIP_USB_N2,       /* Nu/ALLS "N2" L3.1 secure processor             */
} sega_keychip_kind;

/* game-container protection the board uses to keep the title encrypted at rest. */
typedef enum {
    SEGA_PROT_NONE = 0,
    SEGA_PROT_TRUECRYPT,       /* RingEdge: TC partitions O:/P: merged -> X:     */
    SEGA_PROT_BITLOCKER_TPM,   /* Nu/ALLS: BitLocker + TPM + recovery key        */
} sega_container_prot;

/* ---- which hooks a board needs to be brought up (bitmask) ------------------ */
#define SEGA_HOOK_IO4_USB     0x0001u  /* Sega IO4 (837-15257) USB I/O emulation */
#define SEGA_HOOK_JVS         0x0002u  /* classic JVS-over-serial (older titles)  */
#define SEGA_HOOK_D3D9        0x0004u  /* d3d9_throttle present path              */
#define SEGA_HOOK_D3D11       0x0008u  /* d3d11_throttle (DXGI present) path      */
#define SEGA_HOOK_KEYCHIP     0x0010u  /* software keychip / mxkeychip shim       */
#define SEGA_HOOK_ALLNET      0x0020u  /* ALL.Net / billing server stub          */
#define SEGA_HOOK_AMAUTH      0x0040u  /* amDongle / amBackup AmLib bypass        */
#define SEGA_HOOK_CONTAINER   0x0080u  /* TrueCrypt/BitLocker container mount     */
#define SEGA_HOOK_NETWORK     0x0100u  /* network/DNS redirect for ALL.Net hosts  */
#define SEGA_HOOK_LIGHTGUN    0x0200u  /* lightgun bridge (gun titles)            */

/* readiness: how far Ultra Loader can take this board WITHOUT a game dump. */
typedef enum {
    SEGA_READY_FRAMEWORK = 0,  /* descriptor only; no hook armed yet             */
    SEGA_READY_SCAFFOLDED,     /* hooks compiled + wired, lifecycle-validated    */
    SEGA_READY_DUMP_GATED,     /* fully wired; blocked only on a game image      */
    SEGA_READY_DRM_GATED,      /* blocked on a physical keychip / decrypt key    */
    SEGA_READY_PLAYABLE,       /* verified end-to-end with a real dump           */
} sega_readiness;

typedef struct sega_board_desc {
    sega_board_kind     kind;
    const char*         name;          /* "Sega RingEdge", "Sega ALLS UX", ...   */
    sega_gfx_api        gfx;
    sega_keychip_kind   keychip;
    sega_container_prot container;
    uint32_t            hooks;         /* SEGA_HOOK_* bitmask                     */
    int                 bits;          /* 32 or 64 (ALLS/Nu DX11 mostly 64-bit)  */
    sega_readiness      ready;
    const char*         notes;         /* honest one-liner on what's gated       */
} sega_board_desc;

/* The native_x86 module looks a board up by name/kind and arms exactly the hooks
 * in .hooks. Definitions live in sega_board.c (the framework table). */
const sega_board_desc* sega_board_lookup(sega_board_kind kind);
const sega_board_desc* sega_board_lookup_name(const char* name);
int                    sega_board_count(void);
const sega_board_desc* sega_board_at(int index);

/* ---- present-throttle hook entrypoints (armed per board.gfx) --------------- */
void ultra_d3d11_install(void);   /* DXGI/IDXGISwapChain::Present throttle       */
void ultra_d3d11_teardown(void);

/* ---- software keychip emulator (env-gated; see sega_keychip.c) ------------- *
 * Answers the keychip query surface (region, challenge-response, GkeyEnc/Dec,
 * appboot.seed) IN SOFTWARE so a board with NO physical keychip can be brought to
 * the boot screen for a title whose container is already decrypted/unprotected.
 * It does NOT and CANNOT manufacture the per-game encryption key for a still-
 * encrypted retail container -- that key lives in the physical keychip silicon.   */
void ultra_sega_keychip_install(void);

#endif /* ULTRA_SEGA_BOARD_H */
