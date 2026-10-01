/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  sega_keychip_alls.c -- Sega ALLS keychip presence stub (ALKey framing).
 *                         Board-owned, standalone. PRESENCE-ONLY.
 * =============================================================================
 *
 *  ENGINE FACTS (public/documented protocol shape, no third-party source):
 *    Sega ALLS (HX/UX/UX2/MX2) gates boot AND the game container on a physical
 *    USB smart-card keychip -- the N2 (L3.1) part, reached through the driver
 *    chain fdd.sys -> hdd.sys/ssd.sys, the same USB smart-card command family
 *    Sega Nu/Nu2 use (36 driver commands, 0..35). It exposes:
 *      - SessionOpen/Close/AuthStart, LvGet, ErrorGet, VerGet (cmds 0,1,2,5,6,7)
 *      - KeychipInfoRead (cmd 9): 96-byte blob (region/game-id/platform id)
 *      - PlayCountRead / TraceDataInfoRead / StorageSizeGet (24,26,27)
 *      - HmacChange/CKeyChange (cmd 3/4): secret comm-key rekey
 *      - GkeyWrite/GkeyEnc/GkeyDec (cmd 10/11/12): the per-game AES-128 content
 *        key + block cipher over the game data
 *      - ALStart/ALPacket (cmd 34/35): ALL.Net Auth 2.0 LOCAL framing shell
 *      - ALKey* (cmd 30..33): ALL.Net Auth 2.0 KEY MATERIAL
 *
 *  WHAT THIS FILE HONESTLY IS:
 *    A PRESENCE-ONLY scaffold that, when env-gated on (ULTRA_SEGA_KEYCHIP=1,
 *    set by sega_alls_main.c), intercepts the keychip device open + read/write
 *    and answers ONLY the non-secret identity/session/status surface -- so a
 *    board with no physical keychip can reach its boot screen for a title
 *    whose container is ALREADY decrypted / unprotected (an operator-decrypted
 *    image, or a free/location-test build).
 *
 *  WHAT IT IS NOT, AND WILL NEVER BE (project ethical line, non-negotiable):
 *    It does NOT and CANNOT manufacture the per-game AES content key for a
 *    still-encrypted retail container. That key is derived inside the N2
 *    keychip silicon from secrets that never appear in any dump. So:
 *
 *      *** cmd 3 (HmacChange), cmd 4 (CKeyChange), cmd 10 (GkeyWrite),      ***
 *      *** cmd 11 (GkeyEnc), cmd 12 (GkeyDec), and cmd 30-33 (ALKey* --     ***
 *      *** ALL.Net Auth 2.0 key material) ARE ALWAYS DEFERRED HERE.         ***
 *      *** This function returns 0 (not-implemented) for every one of      ***
 *      *** them -- it never derives, fabricates, or forwards a synthesized ***
 *      *** answer for any of the six. A real retail ALLS game therefore    ***
 *      *** stays HARDWARE-GATED: an operator keychip or an already-        ***
 *      *** unlocked image is required. This is deliberate and permanent.   ***
 *
 *  Env gate:  ULTRA_SEGA_KEYCHIP=1  (set by sega_alls_main.c).
 *  Env knobs (read once at install; all pure identity metadata, never secret):
 *    ULTRA_SEGA_REGION_MASK  -- region bitmask byte (default 0x02 = USA)
 *    ULTRA_SEGA_GAME_ID      -- 4-char game id (default "SXXX")
 *    ULTRA_SEGA_PLATFORM_ID  -- platform id string (default "ACA1", ALLS)
 *    ULTRA_SEGA_KEYCHIP_VER  -- firmware version string (default "4.06")
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "sega_alls_hook.h"

#define ALLS_KEYCHIP_SENTINEL ((HANDLE)(ULONG_PTR)0x5E6A0002)   /* "SEga\2" */

typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef BOOL   (WINAPI *pReadFile)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL   (WINAPI *pWriteFile)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL   (WINAPI *pCloseHandle)(HANDLE);
typedef BOOL   (WINAPI *pDeviceIoControl)(HANDLE,DWORD,LPVOID,DWORD,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);

static pCreateFileA     Real_CreateFileA     = NULL;
static pCreateFileW     Real_CreateFileW     = NULL;
static pReadFile        Real_ReadFile        = NULL;
static pWriteFile       Real_WriteFile       = NULL;
static pCloseHandle     Real_CloseHandle     = NULL;
static pDeviceIoControl Real_DeviceIoControl = NULL;

static int g_enabled = 0;
static uint8_t g_region_mask   = 0x02;    /* USA default */
static char    g_game_id[8]    = "SXXX";
static char    g_platform_id[8]= "ACA1";  /* ALLS platform id */
static char    g_ver[16]       = "4.06";

/* ---------------------------------------------------------------------------
 * Device-path match: the N2 keychip reaches the process through the fdd.sys ->
 * hdd.sys/ssd.sys driver chain. We match on the class of device-object names
 * that chain exposes (case-insensitive substring), plus the legacy mxparallel/
 * mxkeychip names some tooling still probes for backward compatibility.
 * ------------------------------------------------------------------------- */
static int path_is_keychip_a(LPCSTR p) {
    if (!p) return 0;
    if (p[0] == '\\' && p[1] == '\\' && (p[2] == '.' || p[2] == '?') && p[3] == '\\') p += 4;
    static const char* const names[] = {
        "fdd", "ssd", "hdd", "keychip", "mxparallel", "mxkeychip", "mxusb", NULL
    };
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
static int path_is_keychip_w(LPCWSTR p) {
    if (!p) return 0;
    char narrow[260]; int i = 0;
    for (; p[i] && i < 259; ++i) narrow[i] = (char)p[i];
    narrow[i] = 0;
    return path_is_keychip_a(narrow);
}

/* ---------------------------------------------------------------------------
 * (1) USB smart-card command map (cmds 0..35) -- the Nu/ALLS-class protocol.
 *
 * PRESENCE/IDENTITY/SESSION/AUTH-STATE commands report WHO the board is and
 * that a session is open -- pure metadata, no secret -- and we answer those.
 * KEY-DERIVATION/CONTENT-TRANSFORM commands operate the per-game key that
 * lives in keychip silicon and never ships -- those are DEFERRED (return 0),
 * exactly the forbidden line the project directive draws.
 *
 * Returns the response length on a presence command (resp filled), or 0 on a
 * deferred/unknown command (the caller must not treat 0 as "answered").
 * ------------------------------------------------------------------------- */
int ultra_alls_keychip_cmd(int cmd, const unsigned char* req, int req_len,
                           unsigned char* resp, int resp_cap) {
    (void)req; (void)req_len;
    if (!g_enabled || !resp || resp_cap < 1) return 0;
    memset(resp, 0, resp_cap);
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
            memcpy(resp, g_ver, n);
            return n;
        }
        case 9: {  /* KeychipInfoRead -- 96-byte info blob (region/game-id/platform) */
            if (resp_cap < 16) return 0;
            resp[0] = g_region_mask;
            memcpy(&resp[4], g_game_id, 4);
            memcpy(&resp[8], g_platform_id, 4);
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
         * cmd 3  HmacChange, 4  CKeyChange   -- secret comm keys
         * cmd 10 GkeyWrite                   -- writes the secret game key + IV
         * cmd 11 GkeyEnc, 12 GkeyDec         -- AES the game data with the game key
         * cmd 30..33 ALKey*                  -- Auth-2.0 key material
         * These need keychip silicon secrets that never appear in any dump. We
         * do NOT and CANNOT fabricate them; the content key is user/scene-
         * supplied (an already-decrypted image), never synthesized here. */
        default:
            return 0;
    }
}

/* Compose the wire-framed 16 (or 96)-byte reply for a WriteFile-received
 * command byte, into the per-handle reply FIFO drained by ReadFile. Frame
 * layout: [0]=cmd echo, [1]=status(0=OK), [2..]=payload, remainder zero --
 * same convention the rest of this project's Sega keychip stubs use. */
static uint8_t g_reply_buf[128];
static DWORD   g_reply_len = 0;
static DWORD   g_reply_pos = 0;

static void build_reply(uint8_t cmd) {
    unsigned char payload[96];
    int n = ultra_alls_keychip_cmd(cmd, NULL, 0, payload, sizeof payload);
    memset(g_reply_buf, 0, sizeof g_reply_buf);
    g_reply_buf[0] = cmd;
    g_reply_buf[1] = 0x00;   /* status OK -- even a deferred cmd ACKs the frame;
                              * the payload after it stays zero, which is the
                              * correct behavior for "cannot supply this secret"
                              * without ever synthesizing key material. */
    if (n > 0) {
        int cap = (int)sizeof(g_reply_buf) - 2;
        if (n > cap) n = cap;
        memcpy(&g_reply_buf[2], payload, n);
    }
    g_reply_len = (cmd == 9) ? 96 : 16;
    if (g_reply_len > sizeof g_reply_buf) g_reply_len = sizeof g_reply_buf;
    g_reply_pos = 0;
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && path_is_keychip_a(name)) {
        ul_alls_log("[keychip] synthesizing presence for '%s'\n", name);
        return ALLS_KEYCHIP_SENTINEL;
    }
    return Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && path_is_keychip_w(name)) {
        ul_alls_log("[keychip] synthesizing presence (W)\n");
        return ALLS_KEYCHIP_SENTINEL;
    }
    return Real_CreateFileW ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}
static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    if (h == ALLS_KEYCHIP_SENTINEL) {
        DWORD have = g_reply_len - g_reply_pos;
        DWORD copy = (n < have) ? n : have;
        if (buf && copy) memcpy(buf, &g_reply_buf[g_reply_pos], copy);
        g_reply_pos += copy;
        if (got) *got = copy;
        (void)ov;
        return TRUE;
    }
    return Real_ReadFile ? Real_ReadFile(h, buf, n, got, ov) : FALSE;
}
static BOOL WINAPI Hook_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote, LPOVERLAPPED ov) {
    if (h == ALLS_KEYCHIP_SENTINEL) {
        uint8_t cmd = 0;
        if (buf && n) cmd = ((const uint8_t*)buf)[0];
        build_reply(cmd);
        if (wrote) *wrote = n;
        (void)ov;
        return TRUE;
    }
    return Real_WriteFile ? Real_WriteFile(h, buf, n, wrote, ov) : FALSE;
}
static BOOL WINAPI Hook_CloseHandle(HANDLE h) {
    if (h == ALLS_KEYCHIP_SENTINEL) return TRUE;
    return Real_CloseHandle ? Real_CloseHandle(h) : FALSE;
}
/* Some fdd.sys/ssd.sys-chain tooling exchanges the smart-card frames via
 * DeviceIoControl rather than Read/WriteFile; answer the same presence-only
 * way (input buffer's first byte = cmd, output buffer <- our reply), and pass
 * anything against a handle we don't own straight through. */
static BOOL WINAPI Hook_DeviceIoControl(HANDLE h, DWORD code,
        LPVOID in_buf, DWORD in_sz, LPVOID out_buf, DWORD out_sz,
        LPDWORD bytes_ret, LPOVERLAPPED ov) {
    if (h == ALLS_KEYCHIP_SENTINEL) {
        uint8_t cmd = (in_buf && in_sz) ? ((const uint8_t*)in_buf)[0] : 0;
        build_reply(cmd);
        DWORD copy = (out_sz < g_reply_len) ? out_sz : g_reply_len;
        if (out_buf && copy) memcpy(out_buf, g_reply_buf, copy);
        if (bytes_ret) *bytes_ret = copy;
        (void)code; (void)ov;
        return TRUE;
    }
    return Real_DeviceIoControl ? Real_DeviceIoControl(h, code, in_buf, in_sz,
                                                        out_buf, out_sz, bytes_ret, ov)
                                : FALSE;
}

/* ---------------------------------------------------------------------------
 * Install. Env-gated (ULTRA_SEGA_KEYCHIP=1, set by sega_alls_main.c). Idempotent.
 * ------------------------------------------------------------------------- */
void ul_alls_install_keychip(void) {
    if (!ul_alls_env_on("ULTRA_SEGA_KEYCHIP")) {
        ul_alls_log("[keychip] skipped (ULTRA_SEGA_KEYCHIP != 1)\n");
        return;
    }
    if (g_enabled) return;
    g_enabled = 1;

    const char* mask = ul_alls_env("ULTRA_SEGA_REGION_MASK");
    const char* gid  = ul_alls_env("ULTRA_SEGA_GAME_ID");
    const char* plat = ul_alls_env("ULTRA_SEGA_PLATFORM_ID");
    const char* ver  = ul_alls_env("ULTRA_SEGA_KEYCHIP_VER");
    if (mask) g_region_mask = (uint8_t)strtoul(mask, NULL, 0);
    if (gid)  { strncpy(g_game_id,     gid,  sizeof(g_game_id)-1);     g_game_id[sizeof(g_game_id)-1]=0; }
    if (plat) { strncpy(g_platform_id, plat, sizeof(g_platform_id)-1); g_platform_id[sizeof(g_platform_id)-1]=0; }
    if (ver)  { strncpy(g_ver,         ver,  sizeof(g_ver)-1);         g_ver[sizeof(g_ver)-1]=0; }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
        Real_CreateFileA     = (pCreateFileA)(void*)GetProcAddress(k32, "CreateFileA");
        Real_CreateFileW     = (pCreateFileW)(void*)GetProcAddress(k32, "CreateFileW");
        Real_ReadFile        = (pReadFile)(void*)GetProcAddress(k32, "ReadFile");
        Real_WriteFile       = (pWriteFile)(void*)GetProcAddress(k32, "WriteFile");
        Real_CloseHandle     = (pCloseHandle)(void*)GetProcAddress(k32, "CloseHandle");
        Real_DeviceIoControl = (pDeviceIoControl)(void*)GetProcAddress(k32, "DeviceIoControl");
    }
    ul_alls_iat_patch_all("kernel32.dll", "CreateFileA",    (void*)Hook_CreateFileA);
    ul_alls_iat_patch_all("kernel32.dll", "CreateFileW",    (void*)Hook_CreateFileW);
    ul_alls_iat_patch_all("kernel32.dll", "ReadFile",       (void*)Hook_ReadFile);
    ul_alls_iat_patch_all("kernel32.dll", "WriteFile",      (void*)Hook_WriteFile);
    ul_alls_iat_patch_all("kernel32.dll", "CloseHandle",    (void*)Hook_CloseHandle);
    ul_alls_iat_patch_all("kernel32.dll", "DeviceIoControl",(void*)Hook_DeviceIoControl);

    ul_alls_log("[keychip] ALKey presence shim armed (region_mask=0x%02X game=%s plat=%s ver=%s). "
                "NOTE: cmd 3/4/10/11/12/30-33 (comm-key rekey, GkeyWrite/Enc/Dec, ALKey* Auth-2.0 "
                "key material) are DEFERRED -- this shim cannot and will not decrypt a retail "
                "container; it needs an unprotected/operator-decrypted image.\n",
                g_region_mask, g_game_id, g_platform_id, g_ver);
}

#endif /* _WIN32 */
