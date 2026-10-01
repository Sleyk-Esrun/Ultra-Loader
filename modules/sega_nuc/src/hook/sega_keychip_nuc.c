/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  sega_keychip_nuc.c -- Sega Nu Compact board's OWN copy of the software
 *                        Sega keychip / ALL.Net shim scaffold (env-gated).
 * =============================================================================
 *
 *  The Sega x86 boards gate boot AND the game container on a physical keychip.
 *  This board (Nu-family "Compact") uses the Nu/ALLS style: a USB smart-card
 *  keychip (A7001AG JavaCard "A7", or "N2" L3.1) reached through fdd.sys ->
 *  hdd.sys/ssd.sys. It exposes GkeyEnc/GkeyDec (cmd 11/12, 16-byte AES blocks
 *  with IV-reset), an ALL.Net Auth-2.0 key write (cmd 32), and gates a
 *  BitLocker(TPM+recovery) game container.
 *
 *  WHAT THIS FILE HONESTLY IS:
 *    A SCAFFOLD that, when env-gated on, intercepts the keychip *transport* a
 *    board uses and answers the NON-SECRET parts of the protocol -- region
 *    match, presence, challenge-response framing, session open -- so a board
 *    with NO physical keychip can be carried toward its boot screen for a
 *    title whose container is ALREADY decrypted / unprotected (operator-
 *    decrypted image, or a free/location-test build).
 *
 *  WHAT IT IS NOT, AND CANNOT BE:
 *    It does NOT and CANNOT manufacture the per-game encryption key for a
 *    still-encrypted RETAIL container. That key is derived inside the
 *    keychip silicon from secrets that never appear in any dump. So a real
 *    retail Nu title stays HARDWARE-GATED: you need the operator keychip, or
 *    an image whose container was already unlocked.
 *
 *  This scaffold ships NO keys, NO challenge tables, NO game data, and is OFF
 *  unless ULTRA_SEGA_KEYCHIP=1. This ethical boundary (answer presence/
 *  identity/region only; DEFER every key-derivation/content-transform command)
 *  is preserved EXACTLY from modules/native_x86/src/hook/sega_keychip.c, which
 *  was read for REFERENCE ONLY per this board's build directive -- nothing
 *  was linked from it; this is an independent copy with its own transport
 *  wiring (see the note below).
 *
 *  TRANSPORT NOTE (own-module design): unlike the native_x86 reference (where
 *  this file IAT-patches CreateFileA/W itself), this module centralizes
 *  CreateFileA/W/ReadFile/WriteFile hooking in hook_dllmain_nuc.c so this
 *  board's OWN keychip shim and this board's OWN IO4 shim (sega_io4_nuc.c)
 *  share the same patched IAT slot without one overwriting the other. This
 *  file only exposes the pure match-predicate + protocol-answer logic;
 *  hook_dllmain_nuc.c owns the actual hook functions and dispatches into these.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void ultra_d3d8_log(const char* fmt, ...);
#define klog ultra_d3d8_log

static int g_enabled = 0;

/* A keychip "region" the board EEPROM must agree with. The user's profile
 * supplies theirs via ULTRA_SEGA_REGION so we never bake an assumption in.
 * Default is a neutral 'A'. */
static char g_region = 'A';

/* Nu/ALLS keychip region is a BITMASK (1=Japan, 2=USA, 4=Export, 8=China). The
 * user pins theirs via ULTRA_SEGA_REGION_MASK; default is every region so a
 * board-region match succeeds for any region the title was built for. */
static unsigned char g_region_mask = 0x0F;

/* Metadata the presence path may report (pure board identity, never a secret):
 * the 4-char game id + a keychip/version string. Defaults are neutral
 * placeholders, overridable so a real board's identity can be pinned. */
static char g_game_id[8] = "SXXX";
static char g_keychip_ver[8] = "0104";
static char g_platform_id[8] = "AAV";

/* ---------------------------------------------------------------------------
 * (1) Device-open match predicate for the keychip's legacy parallel-style
 * transport path some Sega titles probe for even on USB-keychip boards
 * (mxparallel-shaped device name). Pure logic; hook_dllmain_nuc.c owns the
 * actual CreateFileA/W hook and calls this to decide routing.
 * ------------------------------------------------------------------------- */
#define SEGA_KEYCHIP_SENTINEL ((HANDLE)(ULONG_PTR)0x5E6A0001)

static int is_mxparallel_a(const char* n) {
    return n && (strstr(n, "mxparallel") || strstr(n, "MXPARALLEL"));
}

int ultra_sega_keychip_matches_a(const char* name) {
    return g_enabled && is_mxparallel_a(name);
}
int ultra_sega_keychip_matches_w(const wchar_t* name) {
    if (!g_enabled || !name) return 0;
    char buf[64]; int i = 0;
    for (; name[i] && i < 63; ++i) buf[i] = (char)name[i];
    buf[i] = 0;
    return is_mxparallel_a(buf);
}
HANDLE ultra_sega_keychip_sentinel(void) { return SEGA_KEYCHIP_SENTINEL; }

/* ---------------------------------------------------------------------------
 * (2) The protocol answer surface (region / session / challenge-response).
 *
 * The ONLY thing this scaffold answers is unconditional, non-secret metadata:
 * region match + a benign "session open" ack. Anything that needs a secret
 * (game-key derivation, the SHA-1 challenge table, appboot.seed decrypt) is
 * left undone -- and CANNOT be synthesized for an encrypted retail container.
 *
 * Returns 1 if the scaffold answered the frame, 0 if it must defer (i.e. the
 * request needs a secret we deliberately do not have).
 * ------------------------------------------------------------------------- */
int ultra_sega_keychip_answer(const unsigned char req[16], unsigned char resp[16]) {
    if (!g_enabled || !req || !resp) return 0;
    memset(resp, 0, 16);
    switch (req[0]) {
        case 0x00:                 /* SessionOpen / presence ping */
            resp[0] = 0x00;
            return 1;
        case 0x01:                 /* GetRegion */
            resp[0] = 0x00;
            resp[1] = (unsigned char)g_region;
            return 1;
        default:
            /* GkeyEnc/GkeyDec (11/12), challenge-response, appboot.seed, etc.
             * all need keychip secrets -> defer (cannot and will not fabricate). */
            return 0;
    }
}

/* ---------------------------------------------------------------------------
 * (3) Nu/ALLS USB smart-card keychip command map (cmds 0..35).
 *
 * The PRESENCE/IDENTITY/SESSION/AUTH-STATE commands report WHO the board is
 * and that a session is open -- pure metadata, no secret -- and we answer
 * those. The KEY-DERIVATION/CONTENT-TRANSFORM commands (GkeyEnc/Dec 11/12,
 * GkeyWrite 10, the comm-key changes 3/4, the Auth-2.0 key material 30..33)
 * operate the per-game key that lives in keychip silicon and never ships --
 * those we DEFER (return 0), exactly the forbidden line.
 * ------------------------------------------------------------------------- */
int ultra_sega_keychip_nu_cmd(int cmd, const unsigned char* req, int req_len,
                              unsigned char* resp, int resp_cap) {
    (void)req; (void)req_len;
    if (!g_enabled || !resp || resp_cap < 1) return 0;
    memset(resp, 0, resp_cap);
    switch (cmd) {
        /* ---- PRESENCE / IDENTITY / SESSION / AUTH-STATE (answered) ---- */
        case 0:    /* SessionOpen   */
        case 1:    /* SessionClose  */
        case 2:    /* AuthStart (state-only handshake begin) */
            resp[0] = 0x00;
            return 1;
        case 5:    /* LvGet -- report auth level (0..3); benign "authorized" */
            resp[0] = 0x03;
            return 1;
        case 6:    /* ErrorGet -- no error */
            resp[0] = 0x00;
            return 1;
        case 7: {  /* VerGet -- firmware version string */
            int n = (int)strlen(g_keychip_ver);
            if (n > resp_cap) n = resp_cap;
            memcpy(resp, g_keychip_ver, n);
            return n;
        }
        case 9: {  /* KeychipInfoRead -- 96-byte info blob (id/billing/region/
                    * gameid/platform). Pure metadata; non-secret fields laid
                    * out, rest zeroed. */
            if (resp_cap < 16) return 0;
            resp[0] = g_region_mask;
            memcpy(&resp[4], g_game_id, 4);
            memcpy(&resp[8], g_platform_id, 3);
            return resp_cap >= 96 ? 96 : resp_cap;
        }
        case 24:   /* PlayCountRead -- a counter (benign 0) */
            resp[0] = 0x00; resp[1] = 0x00; resp[2] = 0x00; resp[3] = 0x00;
            return resp_cap >= 4 ? 4 : resp_cap;
        case 26:   /* TraceDataInfoRead -- metadata (benign empty) */
        case 27:   /* StorageSizeGet   -- metadata (benign zero)   */
            resp[0] = 0x00;
            return 1;
        case 34:   /* ALStart  -- Auth-2.0 LOCAL framing shell (handshake begin) */
        case 35:   /* ALPacket -- Auth-2.0 LOCAL framing shell (packet ack)      */
            resp[0] = 0x00;
            return 1;

        /* ---- KEY DERIVATION / CONTENT TRANSFORM -> DEFER (forbidden line) ---- *
         * cmd 3 HmacChange, 4 CKeyChange     -- secret comm keys
         * cmd 10 GkeyWrite                   -- writes the secret game key + IV
         * cmd 11 GkeyEnc, 12 GkeyDec         -- AES the game data with the game key
         * cmd 30..33 ALKey*                  -- Auth-2.0 key material
         * These need keychip silicon secrets that never appear in any dump. We
         * do NOT and CANNOT fabricate them; the content key is user/scene-
         * supplied. */
        default:
            return 0;
    }
}

/* ---------------------------------------------------------------------------
 * (4) Legacy text-command surface (some Sega PC boards expose a keychip
 * broker over local text commands like "keychip.status", "keychip.version",
 * "keychip.appboot.gameid"). The PRESENCE/IDENTITY/REGION commands are
 * non-secret metadata and we answer them. The CONTENT-KEY commands --
 * "keychip.decrypt", "keychip.appboot.seed", "keychip.ds.compute",
 * "keychip.ssd.proof" -- are the key-derivation boundary; we DEFER them.
 * ------------------------------------------------------------------------- */
static int kc_match(const char* cmd, const char* key) {
    return cmd && strstr(cmd, key) != NULL;
}
int ultra_sega_keychip_mx_cmd(const char* cmd, char* resp, int resp_cap) {
    if (!g_enabled || !cmd || !resp || resp_cap < 2) return 0;
    resp[0] = 0;
    /* ---- CONTENT-KEY boundary: DEFER FIRST so a "decrypt"/"seed" substring
     * can never fall through to a presence answer. ---- */
    if (kc_match(cmd, "decrypt") || kc_match(cmd, "seed") ||
        kc_match(cmd, "ds.compute") || kc_match(cmd, "ssd.proof") ||
        kc_match(cmd, "keyfile")) {
        return 0;
    }
    /* ---- PRESENCE / IDENTITY / REGION (answered) ---- */
    if (kc_match(cmd, "status")) {
        int n = (int)strlen("ok"); if (n >= resp_cap) n = resp_cap - 1;
        memcpy(resp, "ok", n); resp[n] = 0; return n;
    }
    if (kc_match(cmd, "version")) {
        int n = (int)strlen(g_keychip_ver); if (n >= resp_cap) n = resp_cap - 1;
        memcpy(resp, g_keychip_ver, n); resp[n] = 0; return n;
    }
    if (kc_match(cmd, "appboot.gameid") || kc_match(cmd, "gameid")) {
        int n = (int)strlen(g_game_id); if (n >= resp_cap) n = resp_cap - 1;
        memcpy(resp, g_game_id, n); resp[n] = 0; return n;
    }
    if (kc_match(cmd, "appboot.platform") || kc_match(cmd, "platform")) {
        int n = (int)strlen(g_platform_id); if (n >= resp_cap) n = resp_cap - 1;
        memcpy(resp, g_platform_id, n); resp[n] = 0; return n;
    }
    if (kc_match(cmd, "region")) {
        int n = _snprintf(resp, resp_cap, "%u", (unsigned)g_region_mask);
        if (n < 0 || n >= resp_cap) { n = resp_cap - 1; resp[n] = 0; }
        return n;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Install. Env-gated; default OFF so an unconfigured run touches nothing.
 * Only reads env; does NOT patch any IAT slot itself (see the transport note
 * above -- hook_dllmain_nuc.c owns that).
 * ------------------------------------------------------------------------- */
void ultra_sega_keychip_install(void) {
    const char* e = getenv("ULTRA_SEGA_KEYCHIP");
    g_enabled = e && e[0] == '1';
    if (!g_enabled) return;
    const char* r = getenv("ULTRA_SEGA_REGION");
    if (r && r[0]) g_region = r[0];
    const char* rm = getenv("ULTRA_SEGA_REGION_MASK");
    if (rm && rm[0]) g_region_mask = (unsigned char)strtoul(rm, NULL, 0);
    const char* gid = getenv("ULTRA_SEGA_GAME_ID");
    if (gid && gid[0]) { strncpy(g_game_id, gid, sizeof(g_game_id)-1); g_game_id[sizeof(g_game_id)-1] = 0; }
    const char* kv = getenv("ULTRA_SEGA_KEYCHIP_VER");
    if (kv && kv[0]) { strncpy(g_keychip_ver, kv, sizeof(g_keychip_ver)-1); g_keychip_ver[sizeof(g_keychip_ver)-1] = 0; }
    const char* pid = getenv("ULTRA_SEGA_PLATFORM_ID");
    if (pid && pid[0]) { strncpy(g_platform_id, pid, sizeof(g_platform_id)-1); g_platform_id[sizeof(g_platform_id)-1] = 0; }

    klog("[keychip_nuc] software keychip shim armed (region=%c). NOTE: cannot decrypt "
         "a retail container; needs an unprotected/operator-decrypted image.", g_region);
}

#endif /* _WIN32 */
