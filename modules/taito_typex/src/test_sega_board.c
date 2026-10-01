/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  test_sega_board.c -- lifecycle/structural test for the Sega-board framework.
 * =============================================================================
 *  Validates the clean-room board table (every entry consistent) and the keychip
 *  answer surface (answers only the non-secret classes, defers the rest). No
 *  network, no device, no game -- pure structural validation of the scaffolding.
 * =============================================================================
 */
#include "sega_board.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* the keychip shim's answer fn is built with the shim; declare it for the test. */
int  ultra_sega_keychip_answer(const unsigned char req[16], unsigned char resp[16]);
void ultra_sega_keychip_install(void);   /* arms g_enabled from ULTRA_SEGA_KEYCHIP */

static int g_pass = 0, g_fail = 0;
#define CHECK(c, msg) do { if (c) { g_pass++; } else { g_fail++; \
    printf("FAIL: %s\n", msg); } } while (0)

int main(void) {
    /* 1) table is non-empty and every entry is internally consistent. */
    int n = sega_board_count();
    CHECK(n >= 9, "expected >=9 Sega board entries");

    int saw_d3d9 = 0, saw_d3d11 = 0, saw_mxpar = 0, saw_usb = 0;
    for (int i = 0; i < n; ++i) {
        const sega_board_desc* b = sega_board_at(i);
        CHECK(b != NULL, "board_at returned NULL in range");
        if (!b) continue;
        CHECK(b->name && b->name[0], "board has a name");
        CHECK(b->kind != SEGA_BOARD_UNKNOWN, "board kind set");
        CHECK(b->gfx != SEGA_GFX_UNKNOWN, "board gfx api set");
        CHECK(b->bits == 32 || b->bits == 64, "board bitness is 32 or 64");
        /* honesty invariant: every retail Sega board is DRM-gated (keychip), so no
         * entry should claim PLAYABLE without a dump. */
        CHECK(b->ready != SEGA_READY_PLAYABLE, "no board falsely marked PLAYABLE");
        CHECK(b->keychip != SEGA_KEYCHIP_NONE, "every Sega board has a keychip");
        /* gfx must match the armed present hook. */
        if (b->gfx == SEGA_GFX_D3D9)  { CHECK(b->hooks & SEGA_HOOK_D3D9,  "d3d9 board arms d3d9 hook");  saw_d3d9 = 1; }
        if (b->gfx == SEGA_GFX_D3D11) { CHECK(b->hooks & SEGA_HOOK_D3D11, "d3d11 board arms d3d11 hook"); saw_d3d11 = 1; }
        /* keychip transport must match the container/keychip kind. */
        if (b->keychip == SEGA_KEYCHIP_MXPARALLEL) { CHECK(b->container == SEGA_PROT_TRUECRYPT, "mxparallel -> TrueCrypt"); saw_mxpar = 1; }
        if (b->keychip == SEGA_KEYCHIP_USB_A7 || b->keychip == SEGA_KEYCHIP_USB_N2) {
            CHECK(b->container == SEGA_PROT_BITLOCKER_TPM, "usb keychip -> BitLocker"); saw_usb = 1; }
        CHECK(b->notes && b->notes[0], "board has an honest notes line");
    }
    CHECK(saw_d3d9 && saw_d3d11, "both D3D9 and D3D11 boards present");
    CHECK(saw_mxpar && saw_usb, "both mxparallel and USB keychips present");

    /* 2) lookup helpers. */
    const sega_board_desc* re = sega_board_lookup(SEGA_BOARD_RINGEDGE);
    CHECK(re && re->gfx == SEGA_GFX_D3D9 && re->bits == 32, "RingEdge = D3D9/32-bit");
    const sega_board_desc* alls = sega_board_lookup_name("Sega ALLS UX");
    CHECK(alls && alls->gfx == SEGA_GFX_D3D11 && alls->bits == 64, "ALLS UX = D3D11/64-bit");
    CHECK(sega_board_lookup_name("nope") == NULL, "unknown name -> NULL");
    CHECK(sega_board_at(-1) == NULL && sega_board_at(99999) == NULL, "OOB index -> NULL");

    /* 3) keychip answer surface: env-gated OFF by default -> answers nothing.
     *    (ULTRA_SEGA_KEYCHIP unset here, so install() never ran and g_enabled=0.) */
    unsigned char req[16] = {0}, resp[16] = {0};
    req[0] = 0x00;  /* SessionOpen */
    int r = ultra_sega_keychip_answer(req, resp);
    CHECK(r == 0, "keychip answers nothing while shim is disabled (default-off)");

    /* 4) enable the shim and verify it answers ONLY the non-secret classes. */
    _putenv("ULTRA_SEGA_KEYCHIP=1");
    _putenv("ULTRA_SEGA_REGION=B");
    ultra_sega_keychip_install();              /* sets g_enabled + region (IAT patch is a no-op stub) */
    memset(resp, 0xAA, sizeof resp);
    req[0] = 0x00;                              /* SessionOpen -> non-secret ack */
    CHECK(ultra_sega_keychip_answer(req, resp) == 1 && resp[0] == 0x00, "SessionOpen acked when enabled");
    memset(resp, 0xAA, sizeof resp);
    req[0] = 0x01;                              /* GetRegion -> non-secret region byte */
    CHECK(ultra_sega_keychip_answer(req, resp) == 1 && resp[1] == 'B', "GetRegion returns configured region");
    req[0] = 0x0B;                              /* GkeyEnc (11) -> needs a secret -> MUST defer */
    CHECK(ultra_sega_keychip_answer(req, resp) == 0, "GkeyEnc deferred (no secret fabricated)");
    req[0] = 0x0C;                              /* GkeyDec (12) -> needs a secret -> MUST defer */
    CHECK(ultra_sega_keychip_answer(req, resp) == 0, "GkeyDec deferred (no secret fabricated)");

    printf("==== Sega board framework: %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
