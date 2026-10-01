/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  sega_keychip_allsux.c -- Sega ALLS UX board's OWN software keychip /
 *                           ALL.Net Auth-2.0 (ALKey) presence shim.
 * =============================================================================
 *
 *  ENGINE FACTS (public, from the documented Nu/ALLS protection stack -- see
 *  sega_board.c's SEGA_KEYCHIP_USB_N2 entry for the family this board uses):
 *    Sega Nu/ALLS titles reach their keychip through a USB smart-card (A7001AG
 *    JavaCard "A7", or "N2" L3.1) driven by fdd.sys -> hdd.sys/ssd.sys, and/or
 *    the legacy RingEdge-era `\\.\mxparallel` parallel transport some shared
 *    runtime libraries still probe for compatibility. It exposes a 36-command
 *    driver surface (SessionOpen/Close/AuthStart, LvGet, ErrorGet, VerGet,
 *    KeychipInfoRead [96-byte region+game_id+platform_id blob], PlayCountRead,
 *    ALStart/ALPacket Auth-2.0 LOCAL framing) and gates a BitLocker(TPM +
 *    recovery) game container.
 *
 *  WHAT THIS FILE HONESTLY IS:
 *    A clean-room SCAFFOLD that intercepts the keychip *transport* (CreateFile
 *    on the known device names, then the ReadFile/WriteFile request/reply
 *    exchange) and answers ONLY the non-secret PRESENCE / IDENTITY / SESSION /
 *    AUTH-STATE command classes -- region match, session-open ack, firmware
 *    version, the KeychipInfoRead metadata blob, play-count -- so a board with
 *    NO physical keychip can be carried toward its boot screen for a title
 *    whose container is ALREADY decrypted/unprotected (operator-decrypted
 *    image, or a free/location-test build).
 *
 *  WHAT IT IS NOT, AND WILL NEVER BE (non-negotiable, per the project owner):
 *    It does NOT and CANNOT manufacture the per-game encryption key. The
 *    KEY-DERIVATION / CONTENT-TRANSFORM command classes:
 *      cmd 3  HmacChange, cmd 4  CKeyChange   -- secret comm-key rekey
 *      cmd 10 GkeyWrite                       -- writes the secret game key+IV
 *      cmd 11 GkeyEnc,   cmd 12 GkeyDec       -- AES the game data with it
 *      cmd 30-33 ALKey*                        -- ALL.Net Auth-2.0 key material
 *    are DEFERRED: this file NEVER computes or synthesizes real key material
 *    for them, and NEVER impersonates the ALL.Net live-service. Those secrets
 *    live in keychip silicon and never appear in any dump -- the same wall
 *    every honest analysis hits. Because the transport must still hand SOME
 *    bytes back (a hung ReadFile would stall the game), a deferred command
 *    gets an ack-only frame with a ZEROED payload -- never a fabricated key.
 *    That is the exact same line drawn by modules/native_x86/src/hook/
 *    sega_keychip.c (read for reference only; no code or data copied), just
 *    adapted so the frame-completion contract of a WIRED transport still holds.
 *
 *  Env gate: ULTRA_SEGA_KEYCHIP (default ON -- this DLL only ever loads for
 *  the sega_alls_ux board; set to "0" to disable the presence shim entirely).
 *  Env knobs (read once at install):
 *    ULTRA_SEGA_REGION       -- "JPN"/"USA"/"EXP"/"OCE"/"ASI" (default "USA")
 *    ULTRA_SEGA_REGION_MASK  -- override mask byte directly
 *    ULTRA_SEGA_GAME_ID      -- 4-char game id (default "SXXX")
 *    ULTRA_SEGA_PLATFORM_ID  -- platform id (default "AUX1" for ALLS UX)
 *    ULTRA_SEGA_KEYCHIP_VER  -- firmware version string (default "3.14")
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "allsux_hook.h"

/* ---------------------------------------------------------------------------
 * (1) Pure command-classification logic (unit-provable independent of any
 *     transport): given a Nu/ALLS driver command number, produce the
 *     presence-class reply, or signal DEFER (0) for anything secret.
 *
 *     Returns the reply length written to resp (>=1), or 0 if `cmd` falls in
 *     the key-derivation/content-transform DEFER set.
 * ------------------------------------------------------------------------- */
static uint8_t  g_region_mask  = 0x02;      /* USA default */
static char     g_game_id[8]     = "SXXX";
static char     g_platform_id[8] = "AUX1";  /* ALLS UX */
static char     g_ver[16]         = "3.14";

int ul_ax_keychip_nu_cmd(int cmd, const unsigned char* req, int req_len,
                         unsigned char* resp, int resp_cap) {
    (void)req; (void)req_len;
    if (!resp || resp_cap < 1) return 0;
    memset(resp, 0, (size_t)resp_cap);
    switch (cmd) {
        /* ---- PRESENCE / IDENTITY / SESSION / AUTH-STATE (answered) ---- */
        case 0:    /* SessionOpen  */
        case 1:    /* SessionClose */
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
            int n = (int)strlen(g_ver);
            if (n > resp_cap) n = resp_cap;
            memcpy(resp, g_ver, (size_t)n);
            return n;
        }
        case 9: {  /* KeychipInfoRead -- 96-byte info blob */
            if (resp_cap < 16) return resp_cap;
            resp[0] = g_region_mask;
            memcpy(&resp[4], g_game_id, 4);
            memcpy(&resp[8], g_platform_id, 3);
            return resp_cap >= 96 ? 96 : resp_cap;
        }
        case 24:   /* PlayCountRead -- a counter (benign 0) */
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
         * These need keychip-silicon secrets that never appear in any dump. We
         * do NOT and CANNOT fabricate them. Return 0: the caller (the wired
         * transport below) still completes the I/O with a zero-payload ack so
         * the game's request/reply cadence doesn't stall, but writes NO key
         * material here or anywhere else in this file. */
        default:
            return 0;
    }
}

/* ---------------------------------------------------------------------------
 * (2) Wired transport: CreateFileA/W on the known keychip device names ->
 *     synthetic handle; WriteFile = the game's request (cmd = first byte);
 *     ReadFile = drains the reply this presence shim (or the zero-ack for a
 *     deferred command) produced.
 * ------------------------------------------------------------------------- */
#define KEYCHIP_SENTINEL ((HANDLE)(ULONG_PTR)0x5E6A0002)   /* "SEga\2" (allsux) */

typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef BOOL   (WINAPI *pReadFile)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL   (WINAPI *pWriteFile)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL   (WINAPI *pCloseHandle)(HANDLE);

static pCreateFileA  Real_CreateFileA  = NULL;
static pCreateFileW  Real_CreateFileW  = NULL;
static pReadFile     Real_ReadFile     = NULL;
static pWriteFile    Real_WriteFile    = NULL;
static pCloseHandle  Real_CloseHandle  = NULL;

static int g_enabled = 0;

/* Per-handle reply FIFO (single node -- one keychip per process). */
static uint8_t g_reply_buf[128];
static DWORD   g_reply_len = 0;
static DWORD   g_reply_pos = 0;

static int name_matches(const char* p) {
    if (!p) return 0;
    if (p[0] == '\\' && p[1] == '\\' && (p[2] == '.' || p[2] == '?') && p[3] == '\\') p += 4;
    static const char* const names[] = { "mxparallel", "mxkeychip", "mxusb", "keychip", NULL };
    for (int i = 0; names[i]; ++i) {
        size_t n = strlen(names[i]);
        int match = 1;
        for (size_t j = 0; j < n; ++j) {
            char a = p[j], b = names[i][j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}
static int name_matches_w(LPCWSTR p) {
    if (!p) return 0;
    char buf[64]; int i = 0;
    for (; p[i] && i < 63; ++i) buf[i] = (char)p[i];
    buf[i] = 0;
    return name_matches(buf);
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && name_matches(name)) {
        ul_ax_log("keychip: synthesizing handle for '%s'\n", name);
        return KEYCHIP_SENTINEL;
    }
    return Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && name_matches_w(name)) {
        ul_ax_log("keychip: synthesizing handle (W)\n");
        return KEYCHIP_SENTINEL;
    }
    return Real_CreateFileW ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}
static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    (void)ov;
    if (h == KEYCHIP_SENTINEL) {
        DWORD have = g_reply_len - g_reply_pos;
        DWORD copy = (n < have) ? n : have;
        if (buf && copy) memcpy(buf, &g_reply_buf[g_reply_pos], copy);
        g_reply_pos += copy;
        if (got) *got = copy;
        return TRUE;
    }
    return Real_ReadFile ? Real_ReadFile(h, buf, n, got, ov) : FALSE;
}
static BOOL WINAPI Hook_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote, LPOVERLAPPED ov) {
    (void)ov;
    if (h == KEYCHIP_SENTINEL) {
        int cmd = 0;
        if (buf && n) cmd = ((const unsigned char*)buf)[0];
        unsigned char payload[96];
        int len = ul_ax_keychip_nu_cmd(cmd, (const unsigned char*)buf, (int)n, payload, (int)sizeof payload);
        /* DEFER (len==0): still complete the frame with a zeroed ack so the
         * game's request/reply cadence doesn't stall -- NO key material. */
        if (len <= 0) { memset(payload, 0, sizeof payload); len = 16; }
        if ((size_t)len > sizeof g_reply_buf) len = (int)sizeof g_reply_buf;
        memcpy(g_reply_buf, payload, (size_t)len);
        g_reply_len = (DWORD)len;
        g_reply_pos = 0;
        if (wrote) *wrote = n;
        return TRUE;
    }
    return Real_WriteFile ? Real_WriteFile(h, buf, n, wrote, ov) : FALSE;
}
static BOOL WINAPI Hook_CloseHandle(HANDLE h) {
    if (h == KEYCHIP_SENTINEL) return TRUE;
    return Real_CloseHandle ? Real_CloseHandle(h) : FALSE;
}

/* ---------------------------------------------------------------------------
 * Install. Default ON (this DLL only ever loads for sega_alls_ux); ULTRA_SEGA_
 * KEYCHIP=0 disables the shim outright. Idempotent.
 * ------------------------------------------------------------------------- */
static int g_installed = 0;

void ul_ax_install_keychip(void) {
    if (g_installed) return;
    const char* e = ul_ax_env("ULTRA_SEGA_KEYCHIP");
    g_enabled = !(e && e[0] == '0');
    if (!g_enabled) { ul_ax_log("keychip: skipped (ULTRA_SEGA_KEYCHIP=0)\n"); return; }
    g_installed = 1;

    const char* region = ul_ax_env("ULTRA_SEGA_REGION");
    const char* mask    = ul_ax_env("ULTRA_SEGA_REGION_MASK");
    const char* gid     = ul_ax_env("ULTRA_SEGA_GAME_ID");
    const char* plat    = ul_ax_env("ULTRA_SEGA_PLATFORM_ID");
    const char* ver      = ul_ax_env("ULTRA_SEGA_KEYCHIP_VER");
    if (region) {
        if      (!_stricmp(region, "JPN")) g_region_mask = 0x01;
        else if (!_stricmp(region, "USA")) g_region_mask = 0x02;
        else if (!_stricmp(region, "EXP")) g_region_mask = 0x04;
        else if (!_stricmp(region, "OCE")) g_region_mask = 0x08;
        else if (!_stricmp(region, "ASI")) g_region_mask = 0x10;
    }
    if (mask) g_region_mask = (uint8_t)strtoul(mask, NULL, 0);
    if (gid)  { strncpy(g_game_id,     gid,  sizeof(g_game_id)-1);     g_game_id[sizeof(g_game_id)-1]=0; }
    if (plat) { strncpy(g_platform_id, plat, sizeof(g_platform_id)-1); g_platform_id[sizeof(g_platform_id)-1]=0; }
    if (ver)  { strncpy(g_ver,         ver,  sizeof(g_ver)-1);         g_ver[sizeof(g_ver)-1]=0; }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
        Real_CreateFileA = (pCreateFileA)(void*)GetProcAddress(k32, "CreateFileA");
        Real_CreateFileW = (pCreateFileW)(void*)GetProcAddress(k32, "CreateFileW");
        Real_ReadFile    = (pReadFile)   (void*)GetProcAddress(k32, "ReadFile");
        Real_WriteFile   = (pWriteFile)  (void*)GetProcAddress(k32, "WriteFile");
        Real_CloseHandle = (pCloseHandle)(void*)GetProcAddress(k32, "CloseHandle");
    }
    ul_ax_iat_patch_all("kernel32.dll", "CreateFileA", (void*)Hook_CreateFileA);
    ul_ax_iat_patch_all("kernel32.dll", "CreateFileW", (void*)Hook_CreateFileW);
    ul_ax_iat_patch_all("kernel32.dll", "ReadFile",    (void*)Hook_ReadFile);
    ul_ax_iat_patch_all("kernel32.dll", "WriteFile",   (void*)Hook_WriteFile);
    ul_ax_iat_patch_all("kernel32.dll", "CloseHandle", (void*)Hook_CloseHandle);
    ul_ax_log("keychip: presence shim armed (region_mask=0x%02X game=%s plat=%s ver=%s). "
              "ALKey (cmd 30-33) + GkeyEnc/Dec-class commands stay DEFERRED -- no key "
              "material is ever synthesized.\n",
              g_region_mask, g_game_id, g_platform_id, g_ver);
}

#endif /* _WIN32 */
