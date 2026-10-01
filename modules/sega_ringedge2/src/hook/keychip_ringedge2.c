/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  keychip_ringedge2.c -- software Sega "mxparallel" keychip PRESENCE shim for
 *                         the Sega RingEdge 2 board (this board's own copy).
 * =============================================================================
 *
 *  RingEdge 2 gates boot AND the game container on the SAME PIC parallel
 *  keychip family as RingEdge/RingWide: a dongle on \\.\mxparallel. The
 *  user-space service mxkeychip.exe is the single broker every process talks
 *  to; it does AES-128-ECB framed I/O (per-direction keys), a DS28CN01 SHA-1
 *  challenge-response, a region check against the board EEPROM, and it hands
 *  SegaBoot the TrueCrypt keyfile (keychip.appboot.seed, itself keychip-
 *  decrypted).
 *
 *  WHAT THIS FILE HONESTLY IS:
 *    A SCAFFOLD that intercepts the keychip *transport* (CreateFile
 *    ("\\\\.\\mxparallel")) and answers the NON-SECRET parts of the protocol --
 *    region match, presence, a benign session-open ack, and the mxkeychip
 *    text-command presence/identity surface -- so a board with NO physical
 *    keychip can be carried toward its boot screen for a title whose
 *    container is ALREADY decrypted / unprotected (operator-decrypted image,
 *    or a free/location-test build).
 *
 *  WHAT IT IS NOT, AND CANNOT BE:
 *    It does NOT and CANNOT manufacture the per-game TrueCrypt key for a
 *    still-encrypted RETAIL container. That key is derived inside the
 *    keychip silicon from secrets that never appear in any dump (the SHA-1
 *    challenge table, the appboot.seed decrypt, the ds.compute proof) -- the
 *    same wall every honest analysis of this family hits. So a real retail
 *    RingEdge 2 game stays HARDWARE-GATED: you need the operator keychip, or
 *    an image whose container was already unlocked.
 *
 *  This scaffold ships NO keys, NO challenge tables, NO game data. It is
 *  clean-room Ultra Loader code over the public Win32 device/IOCTL API and
 *  PUBLIC protocol descriptions only. The mxparallel transport intercept +
 *  presence answer are ALWAYS armed by this board's own hook (this DLL exists
 *  for RingEdge 2 alone, so there is no cross-board contamination risk the
 *  way there would be in a hook shared across many boards); the ethical
 *  content-key boundary below is what actually enforces the line, not an env
 *  gate.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ringedge2_hook.h"

/* A keychip "region" the board EEPROM must agree with. Public boards use single
 * letters (e.g. region codes A/B/C...); the user's profile supplies theirs via
 * ULTRA_SEGA_REGION so we never bake an assumption in. Default is a neutral 'A'. */
static char g_region = 'A';

/* RingEdge/RingEdge2 mxkeychip region is a BITMASK (1=Japan, 2=USA, 4=Export,
 * 8=China). The user pins theirs via ULTRA_SEGA_REGION_MASK; default is
 * Japan|USA|Export|China so a board-region match succeeds for any region the
 * title was built for. */
static unsigned char g_region_mask = 0x0F;

/* Metadata the presence path may report (pure board identity, never a secret):
 * the 4-char game id + a keychip/version string. Defaults are neutral
 * placeholders, overridable so a specific title's profile can match exactly. */
static char g_game_id[8]      = "SXXX";  /* 4-char title id from the keychip info */
static char g_keychip_ver[8]  = "0104";  /* mxkeychip version string              */
static char g_platform_id[8]  = "AAV";   /* hardware platform id                  */

/* ---------------------------------------------------------------------------
 * (1) mxparallel transport intercept.
 *
 * Games/mxkeychip open the parallel keychip as the device path "\\.\mxparallel"
 * and exchange fixed 16-byte AES-framed packets. We intercept CreateFileA/W so
 * that, when there is no real device, the open SUCCEEDS against a synthetic
 * handle our answer surface below services. This lets a board with no
 * physical parallel keychip pass the *presence* gate.
 *
 * The cryptographic framing (AES-128-ECB, the documented bootstrap keys,
 * SetKeyS/SetKeyR rekey) is NOT reproduced here -- this scaffold only owns the
 * transport + presence, never the secret key material.
 * ------------------------------------------------------------------------- */
typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static pCreateFileA Real_CreateFileA = NULL;
static pCreateFileW Real_CreateFileW = NULL;

/* sentinel handle value handed back for a synthetic keychip device. */
#define RE2_KEYCHIP_SENTINEL ((HANDLE)(ULONG_PTR)0x5E6A0002)

static int is_mxparallel_a(LPCSTR n) {
    return n && (strstr(n, "mxparallel") || strstr(n, "MXPARALLEL"));
}
static int is_mxparallel_w(LPCWSTR n) {
    if (!n) return 0;
    char buf[64]; int i = 0;
    for (; n[i] && i < 63; ++i) buf[i] = (char)n[i];
    buf[i] = 0;
    return is_mxparallel_a(buf);
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (is_mxparallel_a(name)) {
        HANDLE real = Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                                       : INVALID_HANDLE_VALUE;
        if (real != INVALID_HANDLE_VALUE) return real;     /* real keychip present */
        ul_re2_log("[keychip] synthesizing mxparallel presence (no physical keychip)");
        return RE2_KEYCHIP_SENTINEL;
    }
    return Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (is_mxparallel_w(name)) {
        HANDLE real = Real_CreateFileW ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
                                       : INVALID_HANDLE_VALUE;
        if (real != INVALID_HANDLE_VALUE) return real;
        ul_re2_log("[keychip] synthesizing mxparallel presence (W) (no physical keychip)");
        return RE2_KEYCHIP_SENTINEL;
    }
    return Real_CreateFileW ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}

/* ---------------------------------------------------------------------------
 * (2) The protocol answer surface (region / session / challenge-response).
 *
 * Given a 16-byte request frame the board wrote to the keychip, produce the
 * 16-byte response. The ONLY thing this scaffold answers is the
 * unconditional, non-secret metadata: region match + a benign "session open"
 * ack, so a presence/region gate is satisfied. Anything that needs a secret
 * (game-key derivation, the SHA-1 challenge table, appboot.seed decrypt) is
 * left unanswered -- and CANNOT be synthesized for an encrypted retail
 * container.
 *
 * Returns 1 if this answered the frame, 0 if it must defer (i.e. the request
 * needs a secret we deliberately do not have).
 * ------------------------------------------------------------------------- */
int ul_re2_keychip_answer(const unsigned char req[16], unsigned char resp[16]) {
    if (!req || !resp) return 0;
    memset(resp, 0, 16);
    /* The first request byte is the command class on the documented framing.
     * We answer only two non-secret classes here; everything else defers. */
    switch (req[0]) {
        case 0x00:                 /* SessionOpen / presence ping */
            resp[0] = 0x00;        /* ack/ok */
            return 1;
        case 0x01:                 /* GetRegion */
            resp[0] = 0x00;
            resp[1] = (unsigned char)g_region;
            return 1;
        default:
            /* Challenge-response, appboot.seed, etc. all need keychip secrets
             * -> defer (cannot and will not fabricate). */
            return 0;
    }
}

/* ---------------------------------------------------------------------------
 * (3) mxkeychip text-command surface (PCP/data ports 40106/40107).
 *
 * mxkeychip.exe answers text commands like "keychip.status", "keychip.version",
 * "keychip.appboot.gameid". The PRESENCE/IDENTITY/REGION commands are non-secret
 * metadata and we answer them. The CONTENT-KEY commands -- "keychip.decrypt",
 * "keychip.appboot.seed" (the 16-byte value that derives the TrueCrypt keyfile),
 * "keychip.ds.compute" (DS28CN01 SHA-1 challenge math), "keychip.ssd.proof" --
 * are the key-derivation boundary; we DEFER them (the response math is a
 * documented unknown and would derive the per-game key, which is forbidden
 * and impossible without the physical keychip).
 *
 * `cmd` is the request text. On a presence command we write the text reply
 * into `resp` and return its length; on a content-key command we return 0
 * (defer).
 * ------------------------------------------------------------------------- */
static int kc_match(const char* cmd, const char* key) {
    return cmd && strstr(cmd, key) != NULL;
}
int ul_re2_keychip_mx_cmd(const char* cmd, char* resp, int resp_cap) {
    if (!cmd || !resp || resp_cap < 2) return 0;
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
    /* anything else (incl. any other secret) -> defer. */
    return 0;
}

/* ---------------------------------------------------------------------------
 * Install. Always armed (this DLL exists for RingEdge 2 alone -- see file
 * header). ULTRA_SEGA_REGION / _REGION_MASK / _GAME_ID / _KEYCHIP_VER /
 * _PLATFORM_ID let the user pin exact metadata for a specific title.
 * ------------------------------------------------------------------------- */
void ul_re2_keychip_install(void) {
    const char* r = ul_re2_env("ULTRA_SEGA_REGION");
    if (r && r[0]) g_region = r[0];
    const char* rm = ul_re2_env("ULTRA_SEGA_REGION_MASK");
    if (rm && rm[0]) g_region_mask = (unsigned char)strtoul(rm, NULL, 0);
    const char* gid = ul_re2_env("ULTRA_SEGA_GAME_ID");
    if (gid && gid[0]) { strncpy(g_game_id, gid, sizeof(g_game_id)-1); g_game_id[sizeof(g_game_id)-1] = 0; }
    const char* kv = ul_re2_env("ULTRA_SEGA_KEYCHIP_VER");
    if (kv && kv[0]) { strncpy(g_keychip_ver, kv, sizeof(g_keychip_ver)-1); g_keychip_ver[sizeof(g_keychip_ver)-1] = 0; }
    const char* pid = ul_re2_env("ULTRA_SEGA_PLATFORM_ID");
    if (pid && pid[0]) { strncpy(g_platform_id, pid, sizeof(g_platform_id)-1); g_platform_id[sizeof(g_platform_id)-1] = 0; }

    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (k) {
        Real_CreateFileA = (pCreateFileA)(void*)GetProcAddress(k, "CreateFileA");
        Real_CreateFileW = (pCreateFileW)(void*)GetProcAddress(k, "CreateFileW");
    }
    ul_re2_iat_patch_all("kernel32.dll", "CreateFileA", (void*)Hook_CreateFileA);
    ul_re2_iat_patch_all("kernel32.dll", "CreateFileW", (void*)Hook_CreateFileW);
    ul_re2_log("[keychip] mxparallel presence-only shim armed (region=%c mask=0x%02X). "
              "NOTE: cannot decrypt a retail container; needs an unprotected/"
              "operator-decrypted image.", g_region, g_region_mask);
}

#endif /* _WIN32 */
