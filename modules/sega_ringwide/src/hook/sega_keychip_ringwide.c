/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  sega_keychip_ringwide.c -- software Sega mxparallel keychip PRESENCE-ONLY
 *                             shim for the sega_ringwide board (env-gated, OFF
 *                             by default).
 * =============================================================================
 *
 *  Sega RingWide -- like RingEdge, the board family it shares this keychip
 *  lineage with -- gates boot on a physical keychip: a PIC parallel dongle on
 *  \\.\mxparallel. The user-space service mxkeychip.exe is the single broker
 *  every process talks to; it does AES-128-ECB framed I/O (per-direction keys),
 *  a DS28CN01 SHA-1 challenge-response, a region check against the board
 *  EEPROM, and it hands SegaBoot the TrueCrypt keyfile (keychip.appboot.seed,
 *  itself keychip-decrypted).
 *
 *  WHAT THIS FILE HONESTLY IS:
 *    A SCAFFOLD that, when env-gated on, intercepts the keychip *transport*
 *    (CreateFile("\\\\.\\mxparallel")) and answers the NON-SECRET parts of the
 *    protocol -- presence, region match, a benign "session open" ack -- so a
 *    board with NO physical keychip can be carried toward its boot screen for a
 *    title whose container is ALREADY decrypted / unprotected (operator-
 *    decrypted image, or a free/location-test build).
 *
 *  WHAT IT IS NOT, AND CANNOT BE:
 *    It does NOT and CANNOT manufacture the per-game encryption key for a
 *    still-encrypted RETAIL container. That key is derived inside the keychip
 *    silicon from secrets that never appear in any dump. So a real retail
 *    RingWide game stays HARDWARE-GATED: you need the operator keychip, or an
 *    image whose container was already unlocked. THIS FILE NEVER IMPLEMENTS
 *    KEY DERIVATION -- only presence / identity / session commands are
 *    answered; every command that would need a secret returns "defer" (0) so
 *    real hardware or an operator-supplied profile can answer it instead.
 *
 *  This scaffold ships NO keys, NO challenge tables, NO game data, and is OFF
 *  unless ULTRA_SEGA_KEYCHIP=1. It is clean-room Ultra Loader code over the
 *  public Win32 device API and PUBLIC protocol descriptions only.
 *
 *  THIS IS THE sega_ringwide BOARD'S OWN, SELF-CONTAINED COPY (see
 *  modules/native_x86/src/hook/sega_keychip.c, read as REFERENCE ONLY for the
 *  general shim shape -- that file also carries a Sega Nu/ALLS USB smart-card
 *  command map, which is a DIFFERENT hardware family RingWide does not use and
 *  is therefore not reproduced here).
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void ultra_ringwide_log(const char* fmt, ...);
#define klog ultra_ringwide_log

/* from jvs_serial_hook_ringwide.c -- reuse its IAT patcher (3-arg form). */
extern void ultra_ringwide_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

static int g_enabled = 0;

/* A keychip "region" the board EEPROM must agree with. Public boards use single
 * letters (e.g. region codes A/B/C...); the user's profile supplies theirs via
 * ULTRA_SEGA_REGION so we never bake an assumption in. Default is a neutral 'A'. */
static char g_region = 'A';

/* RingEdge/RingWide mxkeychip region is a BITMASK (1=Japan, 2=USA, 4=Export,
 * 8=China). The user pins theirs via ULTRA_SEGA_REGION_MASK; default
 * Japan|USA|Export|China so a board-region match succeeds for any region the
 * title was built for. (Distinct from the single-letter g_region used in the
 * legacy 16-byte mxparallel framing.) */
static unsigned char g_region_mask = 0x0F;

/* Metadata the presence path may report (pure board identity, never a secret):
 * the 4-char game id + a keychip/version string. The user's profile supplies
 * these so we ship NO assumption. Defaults are neutral placeholders. */
static char g_game_id[8] = "SXXX";     /* 4-char title id from the keychip info    */
static char g_keychip_ver[8] = "0104"; /* mxkeychip version string                 */
static char g_platform_id[8] = "AAV";  /* hardware platform id                     */

/* ---------------------------------------------------------------------------
 * (1) mxparallel transport intercept.
 *
 * Games/mxkeychip open the parallel keychip as the device path "\\.\mxparallel"
 * and exchange fixed 16-byte AES-framed packets. We intercept CreateFileA/W so
 * that, when the shim is enabled and there is no real device, the open SUCCEEDS
 * against a synthetic handle. This lets a board with no parallel keychip pass
 * the *presence* gate.
 *
 * The cryptographic framing (AES-128-ECB, the documented bootstrap keys, SetKeyS/
 * SetKeyR rekey) is implemented by the user's profile layer on top of this seam --
 * this scaffold only owns the transport + presence, never the secret key material.
 * ------------------------------------------------------------------------- */
typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static pCreateFileA Real_CreateFileA = NULL;
static pCreateFileW Real_CreateFileW = NULL;

/* sentinel handle value handed back for a synthetic keychip device. */
#define SEGA_KEYCHIP_SENTINEL ((HANDLE)(ULONG_PTR)0x5E6A0002)

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
    if (g_enabled && is_mxparallel_a(name)) {
        HANDLE real = Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                                       : INVALID_HANDLE_VALUE;
        if (real != INVALID_HANDLE_VALUE) return real;     /* real keychip present */
        klog("[keychip] synthesizing mxparallel presence (no physical keychip)");
        return SEGA_KEYCHIP_SENTINEL;
    }
    return Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && is_mxparallel_w(name)) {
        HANDLE real = Real_CreateFileW ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
                                       : INVALID_HANDLE_VALUE;
        if (real != INVALID_HANDLE_VALUE) return real;
        klog("[keychip] synthesizing mxparallel presence (W) (no physical keychip)");
        return SEGA_KEYCHIP_SENTINEL;
    }
    return Real_CreateFileW ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}

/* ---------------------------------------------------------------------------
 * (2) The protocol answer surface (region / session / challenge-response).
 *
 * This is the seam a per-title profile plugs into: given a 16-byte request
 * frame the board wrote to the keychip, produce the 16-byte response. The
 * ONLY thing this scaffold answers itself is the unconditional, non-secret
 * metadata: region match + a benign "session open" ack, so a presence/region
 * gate is satisfied. Anything that needs a secret (game-key derivation, the
 * SHA-1 challenge table, appboot.seed decrypt) is left undone -- and CANNOT be
 * synthesized for an encrypted retail container.
 *
 * Returns 1 if the scaffold answered the frame, 0 if it must defer to real
 * hardware / an operator-supplied profile (i.e. the request needs a secret we
 * deliberately do not have).
 * ------------------------------------------------------------------------- */
int ultra_ringwide_keychip_answer(const unsigned char req[16], unsigned char resp[16]) {
    if (!g_enabled || !req || !resp) return 0;
    memset(resp, 0, 16);
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
 * documented unknown and would derive the per-game key, which is forbidden and
 * impossible without the real chip).
 *
 * `cmd` is the request text. On a presence command we write the text reply
 * into `resp` and return its length; on a content-key command we return 0
 * (defer).
 * ------------------------------------------------------------------------- */
static int kc_match(const char* cmd, const char* key) {
    return cmd && strstr(cmd, key) != NULL;
}
int ultra_ringwide_keychip_mx_cmd(const char* cmd, char* resp, int resp_cap) {
    if (!g_enabled || !cmd || !resp || resp_cap < 2) return 0;
    resp[0] = 0;
    /* ---- CONTENT-KEY boundary: DEFER FIRST so a "decrypt"/"seed" substring can
     * never fall through to a presence answer. ---- */
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
 * Install. Env-gated; default OFF so a normal RingWide run is byte-identical
 * unless the operator explicitly asks for the presence shim.
 * ------------------------------------------------------------------------- */
void ultra_ringwide_keychip_install(void) {
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

    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (k) {
        Real_CreateFileA = (pCreateFileA)(void*)GetProcAddress(k, "CreateFileA");
        Real_CreateFileW = (pCreateFileW)(void*)GetProcAddress(k, "CreateFileW");
    }
    ultra_ringwide_iat_patch_all("kernel32.dll", "CreateFileA", (void*)Hook_CreateFileA);
    ultra_ringwide_iat_patch_all("kernel32.dll", "CreateFileW", (void*)Hook_CreateFileW);
    klog("[keychip] RingWide software keychip shim armed (region=%c). NOTE: cannot "
         "decrypt a retail container; needs an unprotected/operator-decrypted image.", g_region);
}

#endif /* _WIN32 */
