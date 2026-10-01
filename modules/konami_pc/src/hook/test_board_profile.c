/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  test_board_profile.c -- lifecycle/structural test for the UNIFIED board table.
 * =============================================================================
 *  Validates the cross-vendor board-profile table (every entry internally
 *  consistent), the lookup/arming helpers, and the Fast-I/O + card-reader scaffolds'
 *  buffer/answer surfaces. No network, no device, no game -- pure structural
 *  validation of the scaffolding, exactly like test_sega_board.c.
 * =============================================================================
 */
#include "board_profile.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
/* the Fast-I/O + card-reader test seams (built with the shims). */
void ultra_taito_fastio_install(void);
void ultra_taito_fastio_test_fill(void* buf, int cap);
int  ultra_taito_fastio_enabled(void);
void ultra_card_reader_install(void);
int  ultra_card_reader_answer(const unsigned char* req, int req_len,
                              unsigned char* resp, int resp_cap);
int  ultra_card_reader_enabled(void);
/* the shim that ultra_hook_current_input would call -- the test stubs it below. */
#endif

static int g_pass = 0, g_fail = 0;
#define CHECK(c, msg) do { if (c) { g_pass++; } else { g_fail++; \
    printf("FAIL: %s\n", msg); } } while (0)

int main(void) {
    /* 1) table non-empty + every entry internally consistent. */
    int n = board_profile_count();
    CHECK(n >= 18, "expected >=18 board entries across all vendors");

    int saw_taito = 0, saw_sega = 0, saw_namco = 0, saw_konami = 0, saw_exa = 0, saw_unico = 0;
    int saw_d3d8 = 0, saw_d3d9 = 0, saw_d3d11 = 0, saw_gl = 0;
    int saw_playable = 0, saw_drm = 0, saw_fastio = 0, saw_card = 0, saw_lightgun = 0;
    for (int i = 0; i < n; ++i) {
        const board_profile* b = board_profile_at(i);
        CHECK(b != NULL, "board_at in range");
        if (!b) continue;
        CHECK(b->system_id && b->system_id[0], "board has a system_id");
        CHECK(b->name && b->name[0], "board has a name");
        CHECK(b->notes && b->notes[0], "board has an honest notes line");
        CHECK(b->bits == 32 || b->bits == 64, "board bitness 32 or 64");
        /* present hook must agree with gfx api. */
        if (b->gfx == BP_GFX_D3D8)  { CHECK(b->hook == BP_HOOK_D3D8,  "d3d8 gfx -> d3d8 hook");  saw_d3d8 = 1; }
        if (b->gfx == BP_GFX_D3D9)  { CHECK(b->hook == BP_HOOK_D3D9 || b->hook == BP_HOOK_GL,  "d3d9 gfx -> d3d9 hook"); saw_d3d9 = 1; }
        if (b->gfx == BP_GFX_D3D11) { CHECK(b->hook == BP_HOOK_D3D11, "d3d11 gfx -> d3d11 hook"); saw_d3d11 = 1; }
        if (b->gfx == BP_GFX_GL)    { CHECK(b->hook == BP_HOOK_GL,    "gl gfx -> gl hook"); saw_gl = 1; }
        /* MIXED boards arm both present hooks -- except the generic fallback, which
         * deliberately arms NONE (the user picks the renderer via ULTRA_* knobs). */
        if (b->gfx == BP_GFX_MIXED) {
            CHECK(b->hook == BP_HOOK_D3D9_AND_D3D11 || b->hook == BP_HOOK_NONE,
                  "mixed gfx -> both hooks (or NONE for the generic fallback)");
        }
        /* honesty invariant: only the proven Type X / X2 lane may be PLAYABLE; every
         * other board is at most DUMP/DRM/FRAMEWORK gated. */
        if (b->ready == BP_READY_PLAYABLE) {
            CHECK(b->vendor == BP_VENDOR_TAITO || b->vendor == BP_VENDOR_SEGA,
                  "PLAYABLE only on the proven Taito lane (or Lindbergh ELF lane)");
            saw_playable = 1;
        }
        if (b->ready == BP_READY_DRM_GATED || b->ready == BP_READY_DUMP_GATED) saw_drm = 1;
        if (b->io & BP_IO_FAST_IO)  saw_fastio = 1;
        if (b->io & BP_IO_CARD_READER) saw_card = 1;
        if (b->io & BP_IO_LIGHTGUN) saw_lightgun = 1;
        /* injection strategy sane. */
        CHECK(b->inject != BP_INJECT_NONE || b->vendor == BP_VENDOR_OTHER,
              "real board declares an injection strategy");
        switch (b->vendor) {
            case BP_VENDOR_TAITO:  saw_taito = 1; break;
            case BP_VENDOR_SEGA:   saw_sega = 1; break;
            case BP_VENDOR_NAMCO:  saw_namco = 1; break;
            case BP_VENDOR_KONAMI: saw_konami = 1; break;
            case BP_VENDOR_EXA:    saw_exa = 1; break;
            case BP_VENDOR_UNICO:  saw_unico = 1; break;
            default: break;
        }
    }
    CHECK(saw_taito && saw_sega && saw_namco && saw_konami && saw_exa && saw_unico,
          "all six researched vendors present");
    CHECK(saw_d3d8 && saw_d3d9 && saw_d3d11 && saw_gl, "all four graphics APIs represented");
    CHECK(saw_playable, "the proven playable lane is present");
    CHECK(saw_drm, "DRM/dump-gated boards present (honest scaffolding)");
    CHECK(saw_fastio, "a Fast-I/O board present (Type X3/X4)");
    CHECK(saw_card, "a card-reader board present (NESiCA/ALLS/Bemani)");
    CHECK(saw_lightgun, "a lightgun board present");

    /* 1b) FULL-WIRING INVARIANT: every real, in-scope board has reached at least
     *     DUMP/DRM-gated (= present-hook + I/O + presence + injection all wired).
     *     The ONLY allowed exceptions are (a) exA-Arcadia, the ethics-gated FRAMEWORK
     *     slot that is deliberately left un-wired, and (b) the generic_pc fallback
     *     (no present hook auto-armed -- the user picks the renderer). Nothing else
     *     may sit at BP_READY_FRAMEWORK. This is the "every non-exA board at full
     *     wiring" gate. */
    for (int i = 0; i < n; ++i) {
        const board_profile* b = board_profile_at(i);
        if (!b) continue;
        int is_exa     = (b->vendor == BP_VENDOR_EXA);
        int is_generic = (b->system_id && strcmp(b->system_id, "generic_pc") == 0);
        if (is_exa || is_generic) continue;
        char msg[128];
        snprintf(msg, sizeof msg, "%s is fully wired (>= DUMP/DRM-gated, not FRAMEWORK)",
                 b->system_id ? b->system_id : "?");
        CHECK(b->ready != BP_READY_FRAMEWORK, msg);
    }
    /* exA stays the untouched framework slot: present, DRM-gated, but NEVER armed by
     * board_profile_arm (no ULTRA_* knob set for it). */
    {
        const board_profile* exa = board_profile_lookup("exa_arcadia");
        CHECK(exa && exa->vendor == BP_VENDOR_EXA, "exa_arcadia present as the framework slot");
    }

    /* 2) the PRIORITY Taito boards resolve with the right hook. */
    const board_profile* x = board_profile_lookup("taito_typex");
    CHECK(x && x->hook == BP_HOOK_D3D8 && x->ready == BP_READY_PLAYABLE, "Type X = D3D8/PLAYABLE");
    const board_profile* x2 = board_profile_lookup("taito_typex2");
    CHECK(x2 && x2->hook == BP_HOOK_D3D9 && x2->ready == BP_READY_PLAYABLE, "Type X2 = D3D9/PLAYABLE");
    const board_profile* x3 = board_profile_lookup("taito_typex3");
    CHECK(x3 && x3->hook == BP_HOOK_D3D9 && (x3->io & BP_IO_FAST_IO), "Type X3 = D3D9 + Fast-I/O");
    const board_profile* x4 = board_profile_lookup("taito_typex4");
    CHECK(x4 && x4->hook == BP_HOOK_D3D11 && x4->bits == 64, "Type X4 = D3D11 + 64-bit");
    const board_profile* xz = board_profile_lookup("taito_typexzero");
    CHECK(xz && xz->hook == BP_HOOK_D3D9, "Type X Zero = D3D9");
    const board_profile* nes = board_profile_lookup("taito_nesica");
    CHECK(nes && (nes->io & BP_IO_CARD_READER) && (nes->prot & 0x80u/*NESICA*/), "NESiCA = card-reader + NESiCA prot");

    /* 3) Sega/Namco/Konami/exA/UNICO resolve. */
    CHECK(board_profile_lookup("sega_alls") != NULL, "sega_alls resolves");
    CHECK(board_profile_lookup("namco_es3") != NULL, "namco_es3 resolves");
    CHECK(board_profile_lookup("konami_pc") != NULL, "konami_pc resolves");
    CHECK(board_profile_lookup("exa_arcadia") != NULL, "exa_arcadia resolves");
    CHECK(board_profile_lookup("unico_pc") != NULL, "unico_pc resolves");
    CHECK(board_profile_lookup("nope_board") == NULL, "unknown id -> NULL");
    CHECK(board_profile_at(-1) == NULL && board_profile_at(99999) == NULL, "OOB index -> NULL");

    /* 4) string accessors don't crash + return non-empty. */
    CHECK(strcmp(bp_gfx_str(BP_GFX_D3D11), "D3D11") == 0, "gfx str D3D11");
    CHECK(strcmp(bp_hook_str(BP_HOOK_D3D9), "d3d9") == 0, "hook str d3d9");
    CHECK(strcmp(bp_ready_str(BP_READY_PLAYABLE), "PLAYABLE") == 0, "ready str PLAYABLE");
    CHECK(strcmp(bp_vendor_str(BP_VENDOR_TAITO), "Taito") == 0, "vendor str Taito");

    /* 5) arming: resolve-only (apply_env=0) returns 1 for known, 0 for unknown. */
    CHECK(board_profile_arm("taito_typex4", 0) == 1, "arm known board (resolve-only)");
    CHECK(board_profile_arm("nope_board", 0) == 0, "arm unknown board -> 0");

#if defined(_WIN32)
    /* 6) arming the byte-safe invariant: a D3D8/D3D9 board must NOT set ULTRA_D3D11
     *    (so the Type X / X2 lanes stay byte-identical). Clear, arm, observe. */
    SetEnvironmentVariableA("ULTRA_D3D11", NULL);
    SetEnvironmentVariableA("ULTRA_SEGA_D3D11", NULL);
    SetEnvironmentVariableA("ULTRA_FAST_IO", NULL);
    SetEnvironmentVariableA("ULTRA_IO4", NULL);
    SetEnvironmentVariableA("ULTRA_KONAMI_IO", NULL);
    SetEnvironmentVariableA("ULTRA_AMDAEMON", NULL);
    board_profile_arm("taito_typex2", 1);              /* D3D9 board */
    {
        char v[8] = {0};
        DWORD g = GetEnvironmentVariableA("ULTRA_D3D11", v, sizeof v);
        CHECK(g == 0, "D3D9 board does NOT arm ULTRA_D3D11 (Type X2 byte-safe)");
        char f[8] = {0};
        DWORD gf = GetEnvironmentVariableA("ULTRA_FAST_IO", f, sizeof f);
        CHECK(gf == 0, "Type X2 does NOT arm Fast-I/O (byte-safe)");
        /* the new interop shims must also stay OFF for the proven Taito lane. */
        char io4[8] = {0}, kon[8] = {0}, amd[8] = {0};
        CHECK(GetEnvironmentVariableA("ULTRA_IO4", io4, sizeof io4) == 0,
              "Type X2 does NOT arm ULTRA_IO4 (byte-safe)");
        CHECK(GetEnvironmentVariableA("ULTRA_KONAMI_IO", kon, sizeof kon) == 0,
              "Type X2 does NOT arm ULTRA_KONAMI_IO (byte-safe)");
        CHECK(GetEnvironmentVariableA("ULTRA_AMDAEMON", amd, sizeof amd) == 0,
              "Type X2 does NOT arm ULTRA_AMDAEMON (byte-safe)");
    }
    /* the new interop knobs ARE armed by the boards that need them. */
    SetEnvironmentVariableA("ULTRA_IO4", NULL);
    board_profile_arm("sega_alls", 1);                 /* IO4-USB board */
    {
        char io4[8] = {0};
        CHECK(GetEnvironmentVariableA("ULTRA_IO4", io4, sizeof io4) >= 1 && io4[0] == '1',
              "Sega ALLS (IO4-USB board) arms ULTRA_IO4");
        char amd[8] = {0};
        CHECK(GetEnvironmentVariableA("ULTRA_AMDAEMON", amd, sizeof amd) >= 1 && amd[0] == '1',
              "Sega ALLS (amdaemon-gated) arms ULTRA_AMDAEMON");
    }
    SetEnvironmentVariableA("ULTRA_KONAMI_IO", NULL);
    board_profile_arm("konami_pc", 1);                 /* Konami ACIO board */
    {
        char kon[8] = {0};
        CHECK(GetEnvironmentVariableA("ULTRA_KONAMI_IO", kon, sizeof kon) >= 1 && kon[0] == '1',
              "Konami PC (ACIO board) arms ULTRA_KONAMI_IO");
    }
    SetEnvironmentVariableA("ULTRA_AMDAEMON", NULL);
    board_profile_arm("namco_es3", 1);                 /* Namco ES amdaemon board */
    {
        char amd[8] = {0};
        CHECK(GetEnvironmentVariableA("ULTRA_AMDAEMON", amd, sizeof amd) >= 1 && amd[0] == '1',
              "Namco ES3 (amdaemon-gated) arms ULTRA_AMDAEMON");
    }

    /* 6b) Namco JVS-over-USB boards arm ULTRA_NAMCO_IO (the cross-board I/O seam). */
    {
        const char* jvs_usb_boards[] = { "namco_es1","namco_es2plus","namco_es3",
                                         "namco_es4","namco_bna1", NULL };
        for (int i = 0; jvs_usb_boards[i]; ++i) {
            SetEnvironmentVariableA("ULTRA_NAMCO_IO", NULL);
            board_profile_arm(jvs_usb_boards[i], 1);
            char nio[8] = {0};
            char msg[96]; snprintf(msg, sizeof msg, "%s (JVS-USB) arms ULTRA_NAMCO_IO", jvs_usb_boards[i]);
            CHECK(GetEnvironmentVariableA("ULTRA_NAMCO_IO", nio, sizeof nio) >= 1 && nio[0] == '1', msg);
        }
        SetEnvironmentVariableA("ULTRA_NAMCO_IO", NULL);
    }

    /* 6c) GL board (Namco N2) arms ULTRA_GL_PRESENT + ULTRA_GL_BOARD; a non-GL board
     *     never does (byte-safe for the Direct3D lanes). */
    {
        SetEnvironmentVariableA("ULTRA_GL_PRESENT", NULL);
        SetEnvironmentVariableA("ULTRA_GL_BOARD", NULL);
        board_profile_arm("namco_n2", 1);
        char gp[8] = {0}, gb[8] = {0};
        CHECK(GetEnvironmentVariableA("ULTRA_GL_PRESENT", gp, sizeof gp) >= 1 && gp[0] == '1',
              "Namco N2 (GL board) arms ULTRA_GL_PRESENT");
        CHECK(GetEnvironmentVariableA("ULTRA_GL_BOARD", gb, sizeof gb) >= 1 && gb[0] == '1',
              "Namco N2 (GL board) arms ULTRA_GL_BOARD");
        /* a D3D11 board must NOT arm the GL knobs */
        SetEnvironmentVariableA("ULTRA_GL_PRESENT", NULL);
        board_profile_arm("taito_typex4", 1);
        char gp2[8] = {0};
        CHECK(GetEnvironmentVariableA("ULTRA_GL_PRESENT", gp2, sizeof gp2) == 0,
              "D3D11 board does NOT arm ULTRA_GL_PRESENT (byte-safe)");
        SetEnvironmentVariableA("ULTRA_GL_PRESENT", NULL);
        SetEnvironmentVariableA("ULTRA_GL_BOARD", NULL);
    }

    /* 6d) The Lindbergh GL board routes to the ELF lane, so it must NOT arm the Win32
     *     GL hook (the standalone ELF loader presents it, not this hook). */
    {
        SetEnvironmentVariableA("ULTRA_GL_PRESENT", NULL);
        board_profile_arm("sega_lindbergh", 1);
        char gp[8] = {0};
        CHECK(GetEnvironmentVariableA("ULTRA_GL_PRESENT", gp, sizeof gp) == 0,
              "Lindbergh (ELF GL board) does NOT arm the Win32 GL hook");
        SetEnvironmentVariableA("ULTRA_GL_PRESENT", NULL);
    }

    /* 6e) ETHICS GATE: exA-Arcadia is the untouched framework slot. board_profile_arm
     *     resolves it (returns 1 = known) but sets NO ULTRA_* knob -- not even its
     *     D3D11 present hook. It must NOT wire ULTRA_D3D11/ULTRA_BOARD/anything. */
    {
        SetEnvironmentVariableA("ULTRA_D3D11", NULL);
        SetEnvironmentVariableA("ULTRA_BOARD", NULL);
        CHECK(board_profile_arm("exa_arcadia", 1) == 1, "exa_arcadia resolves (known board)");
        char d11[8] = {0}, brd[64] = {0};
        CHECK(GetEnvironmentVariableA("ULTRA_D3D11", d11, sizeof d11) == 0,
              "exA arms NO present hook (ULTRA_D3D11 unset) -- framework slot untouched");
        CHECK(GetEnvironmentVariableA("ULTRA_BOARD", brd, sizeof brd) == 0,
              "exA sets NO ULTRA_BOARD -- not wired at all");
    }
    /* a D3D11 board DOES arm ULTRA_D3D11. */
    SetEnvironmentVariableA("ULTRA_D3D11", NULL);
    board_profile_arm("taito_typex4", 1);              /* D3D11 board */
    {
        char v[8] = {0};
        DWORD g = GetEnvironmentVariableA("ULTRA_D3D11", v, sizeof v);
        CHECK(g >= 1 && v[0] == '1', "D3D11 board arms ULTRA_D3D11");
        char f[8] = {0};
        DWORD gf = GetEnvironmentVariableA("ULTRA_FAST_IO", f, sizeof f);
        CHECK(gf >= 1 && f[0] == '1', "Type X4 (Fast-I/O board) arms ULTRA_FAST_IO");
    }

    /* 7) Fast-I/O scaffold: OFF by default answers/fills nothing meaningful; ON,
     *    the buffer-fill produces a centered-analog default (proves the seam). The
     *    install() functions read getenv (the CRT env), so set via _putenv here --
     *    SetEnvironmentVariableA above is for verifying what board_profile_arm sets
     *    for the CHILD process (Win32 block), a separate store from the CRT env. */
    _putenv("ULTRA_FAST_IO=1");
    ultra_taito_fastio_install();
    CHECK(ultra_taito_fastio_enabled() == 1, "Fast-I/O enabled when armed");
    {
        unsigned char buf[64];
        memset(buf, 0xAA, sizeof buf);
        ultra_taito_fastio_test_fill(buf, sizeof buf);
        /* default analog offset 8, channel 0 = 0x8000 center (no input). */
        CHECK(buf[8] == 0x80 && buf[9] == 0x00, "Fast-I/O fills analog ch0 = 0x8000 center");
    }

    /* 8) card-reader scaffold: OFF answers nothing; ON, a status poll acks and a
     *    no-card read returns the benign idle. */
    {
        unsigned char req[4] = {0}, resp[32] = {0};
        CHECK(ultra_card_reader_answer(req, 1, resp, sizeof resp) == 0,
              "card reader answers nothing while disabled (default-off)");
        _putenv("ULTRA_CARD_READER=1");
        ultra_card_reader_install();
        CHECK(ultra_card_reader_enabled() == 1, "card reader enabled when armed");
        req[0] = 0x00;                                   /* status poll */
        CHECK(ultra_card_reader_answer(req, 1, resp, sizeof resp) == 2 && resp[0] == 0x00,
              "card reader acks a status poll when enabled");
        req[0] = 0x01;                                   /* read card -> no card idle */
        CHECK(ultra_card_reader_answer(req, 1, resp, sizeof resp) == 2 && resp[0] == 0x00,
              "card reader returns no-card idle when no ULTRA_CARD_ID set");
    }
#endif

    printf("==== Unified board-profile framework: %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
