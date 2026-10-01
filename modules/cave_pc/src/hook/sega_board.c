/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  sega_board.c  --  the clean-room Sega-board framework table.
 * =============================================================================
 *
 *  One descriptor per Sega x86 PC arcade board, recording (from PUBLIC hardware/
 *  protocol facts only) which graphics API the titles render with, which keychip /
 *  container DRM gates them, which Ultra Loader hooks must be armed, and HONESTLY
 *  how far the board can be taken without a game dump + the physical keychip.
 *
 *  The native_x86 module reads this table to arm the right present-throttle + I/O
 *  + keychip-shim for the board it is launching, exactly like the Type X / X2 path.
 *  No game runs from this table alone; it is the scaffolding + readiness record.
 * =============================================================================
 */
#include "sega_board.h"
#include <string.h>

/* All Sega PC boards carry the Sega IO4 (837-15257) USB I/O board for cabinet
 * inputs, so SEGA_HOOK_IO4_USB is set on every entry; older RingEdge titles can
 * also use classic serial JVS, so those add SEGA_HOOK_JVS. AmLib (amDongle/
 * amBackup/ALL.Net Auth) sits on top everywhere, so SEGA_HOOK_AMAUTH|ALLNET too. */
#define COMMON_IO   (SEGA_HOOK_IO4_USB | SEGA_HOOK_AMAUTH | SEGA_HOOK_ALLNET | SEGA_HOOK_NETWORK)

static const sega_board_desc g_boards[] = {
  { SEGA_BOARD_RINGEDGE, "Sega RingEdge", SEGA_GFX_D3D9,
    SEGA_KEYCHIP_MXPARALLEL, SEGA_PROT_TRUECRYPT,
    COMMON_IO | SEGA_HOOK_JVS | SEGA_HOOK_D3D9 | SEGA_HOOK_KEYCHIP | SEGA_HOOK_CONTAINER,
    32, SEGA_READY_DRM_GATED,
    "PIC mxparallel keychip (AES-128-ECB bus, DS28CN01 SHA-1 C/R) unlocks a "
    "TrueCrypt game container; needs operator keychip or a decrypted image." },

  { SEGA_BOARD_RINGWIDE, "Sega RingWide", SEGA_GFX_D3D9,
    SEGA_KEYCHIP_MXPARALLEL, SEGA_PROT_TRUECRYPT,
    COMMON_IO | SEGA_HOOK_JVS | SEGA_HOOK_D3D9 | SEGA_HOOK_KEYCHIP | SEGA_HOOK_CONTAINER,
    32, SEGA_READY_DRM_GATED,
    "Low-end RingEdge sibling (ATI HD2000, CompactFlash, 1xDVI); identical "
    "mxparallel keychip + TrueCrypt DRM path." },

  { SEGA_BOARD_RINGEDGE2, "Sega RingEdge 2", SEGA_GFX_D3D9,
    SEGA_KEYCHIP_MXPARALLEL, SEGA_PROT_TRUECRYPT,
    COMMON_IO | SEGA_HOOK_JVS | SEGA_HOOK_D3D9 | SEGA_HOOK_D3D11 | SEGA_HOOK_KEYCHIP | SEGA_HOOK_CONTAINER,
    32, SEGA_READY_DRM_GATED,
    "i3-540 + GT545; later titles edge toward D3D11. Same TrueCrypt/keychip wall; "
    "well-documented NoKey/ReKey softmod exists for operator-owned units." },

  { SEGA_BOARD_NU, "Sega Nu", SEGA_GFX_D3D11,
    SEGA_KEYCHIP_USB_A7, SEGA_PROT_BITLOCKER_TPM,
    COMMON_IO | SEGA_HOOK_D3D11 | SEGA_HOOK_KEYCHIP | SEGA_HOOK_CONTAINER,
    64, SEGA_READY_DRM_GATED,
    "USB A7/N2 smart-card keychip via fdd.sys; BitLocker(TPM+recovery) container; "
    "GkeyEnc/Dec (cmd 11/12) gate the game key. DX11.1 GPU." },

  { SEGA_BOARD_NU2, "Sega Nu 2", SEGA_GFX_D3D11,
    SEGA_KEYCHIP_USB_N2, SEGA_PROT_BITLOCKER_TPM,
    COMMON_IO | SEGA_HOOK_D3D11 | SEGA_HOOK_KEYCHIP | SEGA_HOOK_CONTAINER,
    64, SEGA_READY_DRM_GATED,
    "Nu revision (1.1/2/SX). N2 (L3.1) keychip variant; same USB keychip + "
    "BitLocker DRM as Nu." },

  { SEGA_BOARD_ALLS_HX, "Sega ALLS HX", SEGA_GFX_D3D11,
    SEGA_KEYCHIP_USB_N2, SEGA_PROT_BITLOCKER_TPM,
    COMMON_IO | SEGA_HOOK_D3D11 | SEGA_HOOK_KEYCHIP | SEGA_HOOK_CONTAINER,
    64, SEGA_READY_DRM_GATED,
    "Low-end ALLS, Win10 IoT Enterprise, GeForce 10xx. Same USB keychip + "
    "BitLocker container as Nu; the heaviest, most-current Sega DRM." },

  { SEGA_BOARD_ALLS_UX, "Sega ALLS UX", SEGA_GFX_D3D11,
    SEGA_KEYCHIP_USB_N2, SEGA_PROT_BITLOCKER_TPM,
    COMMON_IO | SEGA_HOOK_D3D11 | SEGA_HOOK_KEYCHIP | SEGA_HOOK_CONTAINER | SEGA_HOOK_LIGHTGUN,
    64, SEGA_READY_DRM_GATED,
    "High-end ALLS (GTX1070-class). Win10 IoT, D3D11, USB keychip + BitLocker. "
    "Some titles (gun cabs) wire the lightgun bridge." },

  { SEGA_BOARD_ALLS_UX2, "Sega ALLS UX2", SEGA_GFX_D3D11,
    SEGA_KEYCHIP_USB_N2, SEGA_PROT_BITLOCKER_TPM,
    COMMON_IO | SEGA_HOOK_D3D11 | SEGA_HOOK_KEYCHIP | SEGA_HOOK_CONTAINER | SEGA_HOOK_LIGHTGUN,
    64, SEGA_READY_DRM_GATED,
    "2nd-rev high-end ALLS. Same Win10 IoT / D3D11 / USB-keychip + BitLocker stack." },

  { SEGA_BOARD_ALLS_MX2, "Sega ALLS MX2", SEGA_GFX_D3D11,
    SEGA_KEYCHIP_USB_N2, SEGA_PROT_BITLOCKER_TPM,
    COMMON_IO | SEGA_HOOK_D3D11 | SEGA_HOOK_KEYCHIP | SEGA_HOOK_CONTAINER,
    64, SEGA_READY_DRM_GATED,
    "2nd-rev mid-grade ALLS. Same DRM/graphics stack as UX2/HX." },
};

#define SEGA_BOARD_N ((int)(sizeof(g_boards)/sizeof(g_boards[0])))

const sega_board_desc* sega_board_lookup(sega_board_kind kind) {
    for (int i = 0; i < SEGA_BOARD_N; ++i)
        if (g_boards[i].kind == kind) return &g_boards[i];
    return 0;
}

const sega_board_desc* sega_board_lookup_name(const char* name) {
    if (!name) return 0;
    for (int i = 0; i < SEGA_BOARD_N; ++i)
        if (g_boards[i].name && strcmp(g_boards[i].name, name) == 0) return &g_boards[i];
    return 0;
}

int sega_board_count(void) { return SEGA_BOARD_N; }

const sega_board_desc* sega_board_at(int index) {
    if (index < 0 || index >= SEGA_BOARD_N) return 0;
    return &g_boards[index];
}
