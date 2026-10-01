/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  sega_keychip_nu2.c -- Sega Nu2 keychip PRESENCE stub (N2/L3.1 USB
 *                        smart-card + mxparallel legacy transport).
 * =============================================================================
 *
 *  ENGINE FACTS (research summary):
 *    Sega Nu2 / ALLS-family titles talk to the keychip via the same family
 *    of transports as the original Nu, on the "N2 / L3.1" keychip variant:
 *      1. mxparallel  -- `\\.\mxparallel` device (RingEdge-legacy path;
 *                        16-byte framed answer, cmd 0x00 SessionOpen +
 *                        cmd 0x01 GetRegion).
 *      2. USB A7/N2 smart-card (36-cmd map) -- SessionOpen/Close/AuthStart,
 *                        LvGet, ErrorGet, VerGet, KeychipInfoRead (96-byte
 *                        blob with region-mask + game_id + platform_id),
 *                        PlayCountRead, ALStart/ALPacket framing.
 *      3. mxkeychip text port -- keychip.status/version/gameid/platform/region.
 *
 *    DELIBERATE DEFERRALS (per project ethical line -- fabricating these does
 *    not unlock content, so we do NOT synthesize):
 *      - cmd 3 HmacChange / 4 CKeyChange (comm-key rekey)
 *      - cmd 10 GkeyWrite / 11 GkeyEnc / 12 GkeyDec  (AES-128 game-key ops)
 *      - cmds 30-33 ALKey* (ALL.Net Auth-2.0 key material)
 *      - mxkeychip decrypt / appboot.seed / ds.compute / ssd.proof
 *    The correct posture is "presence stub + operator-supplied decrypted
 *    container"; a synthesized signature would not unblock the subsequent AES
 *    stream (silicon-fused per-title keys are off-dump by construction). This
 *    is the SAME ethical boundary the board owner's directive requires:
 *    presence-only, never real key derivation or BitLocker/TPM unlock.
 *
 *  WHAT THIS STUB DOES:
 *    - IAT-hook CreateFileA/W in kernel32: any open of `\\.\mxparallel` (or
 *      the alternates `\\.\mxkeychip` / `\\.\mxusb` / `\\.\keychip` seen on
 *      later Nu2/ALLS titles) returns a sentinel handle KEYCHIP_SENTINEL.
 *      amauthd's device-open gate passes.
 *    - IAT-hook ReadFile/WriteFile on that handle: emit a 16-byte framed
 *      "presence + region OK" answer. Cmd 0x00 SessionOpen -> {0x00,0x00,...};
 *      cmd 0x01 GetRegion -> {0x01, region_mask, ...}.
 *
 *  Env gate:  ULTRA_SEGA_KEYCHIP=1  (set by sega_nu2_main.c).
 *  Env knobs (read once at install):
 *    ULTRA_SEGA_REGION       -- "JPN" / "USA" / "EXP" (default "USA")
 *    ULTRA_SEGA_REGION_MASK  -- override mask byte (default derived from above)
 *    ULTRA_SEGA_GAME_ID      -- 4-char game id (default "SXXX")
 *    ULTRA_SEGA_PLATFORM_ID  -- platform id (default "AAV2" for Nu2)
 *    ULTRA_SEGA_KEYCHIP_VER  -- version blob (default "3.14")
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "sega_nu2_hook.h"

#define KEYCHIP_SENTINEL ((HANDLE)(ULONG_PTR)0x5E6B0001)   /* "SEgb\1" -- Nu2's own sentinel */

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
static uint8_t g_region_mask = 0x02;              /* USA default */
static char g_game_id[8]     = "SXXX";
static char g_platform_id[8] = "AAV2";             /* Nu2 platform id default */
static char g_ver[16]        = "3.14";

/* Per-sentinel small answer FIFO. Filled by WriteFile (client sends a request),
 * drained by ReadFile (client reads the reply). 128 bytes is more than enough
 * for the presence-class replies (16-byte frames + a 96-byte KeychipInfoRead
 * response). */
static uint8_t g_reply_buf[128];
static DWORD   g_reply_len = 0;
static DWORD   g_reply_pos = 0;

static int path_is_keychip_a(LPCSTR p) {
    if (!p) return 0;
    /* Strip leading "\\.\" or "\\?\" prefix. */
    if (p[0] == '\\' && p[1] == '\\'
        && (p[2] == '.' || p[2] == '?') && p[3] == '\\') p += 4;
    static const char* const names[] = {
        "mxparallel", "mxkeychip", "mxusb", "keychip", NULL
    };
    for (int i = 0; names[i]; ++i) {
        size_t n = strlen(names[i]);
        int match = 1;
        for (size_t j = 0; j < n; ++j) {
            char a = p[j];
            char b = names[i][j];
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
    if (p[0]==L'\\' && p[1]==L'\\'
        && (p[2]==L'.' || p[2]==L'?') && p[3]==L'\\') p += 4;
    static const wchar_t* const names[] = {
        L"mxparallel", L"mxkeychip", L"mxusb", L"keychip", NULL
    };
    for (int i = 0; names[i]; ++i) {
        size_t n = wcslen(names[i]);
        int match = 1;
        for (size_t j = 0; j < n; ++j) {
            wchar_t a = p[j];
            wchar_t b = names[i][j];
            if (a >= L'A' && a <= L'Z') a = (wchar_t)(a + 32);
            if (b >= L'A' && b <= L'Z') b = (wchar_t)(b + 32);
            if (a != b) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

/* Compose a 16-byte framed answer for the given command byte. Frame layout:
 *   [0]=cmd echo, [1]=status(0=OK), [2..]=payload, remainder zero. */
static void build_reply(uint8_t cmd) {
    memset(g_reply_buf, 0, sizeof g_reply_buf);
    switch (cmd) {
    case 0x00: /* SessionOpen */
        g_reply_buf[0] = 0x00;
        g_reply_buf[1] = 0x00;
        g_reply_len = 16;
        break;
    case 0x01: /* GetRegion */
        g_reply_buf[0] = 0x01;
        g_reply_buf[1] = 0x00;
        g_reply_buf[2] = g_region_mask;
        g_reply_len = 16;
        break;
    case 0x05: /* LvGet -- return level 3 (retail unlock) */
        g_reply_buf[0] = 0x05;
        g_reply_buf[1] = 0x00;
        g_reply_buf[2] = 0x03;
        g_reply_len = 16;
        break;
    case 0x06: /* ErrorGet -- no error */
        g_reply_buf[0] = 0x06;
        g_reply_buf[1] = 0x00;
        g_reply_len = 16;
        break;
    case 0x07: /* VerGet -- version blob */
        g_reply_buf[0] = 0x07;
        g_reply_buf[1] = 0x00;
        {
            size_t n = strlen(g_ver);
            if (n > 12) n = 12;
            memcpy(&g_reply_buf[2], g_ver, n);
        }
        g_reply_len = 16;
        break;
    case 0x09: /* KeychipInfoRead -- 96-byte blob: game_id + platform + region */
        g_reply_buf[0] = 0x09;
        g_reply_buf[1] = 0x00;
        /* offset 8: game_id (4B) */
        memcpy(&g_reply_buf[8],  g_game_id, 4);
        /* offset 16: platform_id (4B) */
        memcpy(&g_reply_buf[16], g_platform_id, 4);
        /* offset 24: region_mask */
        g_reply_buf[24] = g_region_mask;
        g_reply_len = 96;
        break;
    case 0x18: /* PlayCountRead -- return 0 plays */
        g_reply_buf[0] = 0x18;
        g_reply_buf[1] = 0x00;
        g_reply_len = 16;
        break;
    default:
        /* Any deferred cmd (3/4, 10/11/12, 30-33): answer OK with no payload.
         * This lets the game continue past the handshake -- content will still
         * be garbage if it depends on real GkeyEnc/Dec, which is the correct
         * posture for a presence-only stub. */
        g_reply_buf[0] = cmd;
        g_reply_buf[1] = 0x00;
        g_reply_len = 16;
        break;
    }
    g_reply_pos = 0;
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && path_is_keychip_a(name)) {
        ul_sn2_log("keychip: synthesizing handle for %s\n", name);
        return KEYCHIP_SENTINEL;
    }
    return Real_CreateFileA
        ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
        : CreateFileA(name, acc, sh, sa, disp, flags, tmpl);
}
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && path_is_keychip_w(name)) {
        ul_sn2_log("keychip: synthesizing handle for keychip device (W)\n");
        return KEYCHIP_SENTINEL;
    }
    return Real_CreateFileW
        ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
        : CreateFileW(name, acc, sh, sa, disp, flags, tmpl);
}
static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got,
                                 LPOVERLAPPED ov) {
    if (h == KEYCHIP_SENTINEL) {
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
static BOOL WINAPI Hook_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote,
                                  LPOVERLAPPED ov) {
    if (h == KEYCHIP_SENTINEL) {
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
    if (h == KEYCHIP_SENTINEL) return TRUE;
    return Real_CloseHandle ? Real_CloseHandle(h) : FALSE;
}

void ul_sn2_install_keychip(void) {
    if (!ul_sn2_env_on("ULTRA_SEGA_KEYCHIP")) {
        ul_sn2_log("keychip: skipped (ULTRA_SEGA_KEYCHIP!=1)\n");
        return;
    }
    if (g_enabled) return;
    g_enabled = 1;

    const char* region = ul_sn2_env("ULTRA_SEGA_REGION");
    const char* mask   = ul_sn2_env("ULTRA_SEGA_REGION_MASK");
    const char* gid    = ul_sn2_env("ULTRA_SEGA_GAME_ID");
    const char* plat   = ul_sn2_env("ULTRA_SEGA_PLATFORM_ID");
    const char* ver    = ul_sn2_env("ULTRA_SEGA_KEYCHIP_VER");
    if (region) {
        if      (!_stricmp(region, "JPN")) g_region_mask = 0x01;
        else if (!_stricmp(region, "USA")) g_region_mask = 0x02;
        else if (!_stricmp(region, "EXP")) g_region_mask = 0x04;
        else if (!_stricmp(region, "OCE")) g_region_mask = 0x08;
        else if (!_stricmp(region, "ASI")) g_region_mask = 0x10;
    }
    if (mask) g_region_mask = (uint8_t)strtoul(mask, NULL, 0);
    if (gid) { strncpy(g_game_id,     gid,  sizeof(g_game_id)-1);     g_game_id[sizeof(g_game_id)-1]=0; }
    if (plat){ strncpy(g_platform_id, plat, sizeof(g_platform_id)-1); g_platform_id[sizeof(g_platform_id)-1]=0; }
    if (ver) { strncpy(g_ver,         ver,  sizeof(g_ver)-1);         g_ver[sizeof(g_ver)-1]=0; }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
        Real_CreateFileA = (pCreateFileA)(void*)GetProcAddress(k32, "CreateFileA");
        Real_CreateFileW = (pCreateFileW)(void*)GetProcAddress(k32, "CreateFileW");
        Real_ReadFile    = (pReadFile)   (void*)GetProcAddress(k32, "ReadFile");
        Real_WriteFile   = (pWriteFile)  (void*)GetProcAddress(k32, "WriteFile");
        Real_CloseHandle = (pCloseHandle)(void*)GetProcAddress(k32, "CloseHandle");
    }
    ul_sn2_iat_patch_all("kernel32.dll", "CreateFileA", (void*)Hook_CreateFileA);
    ul_sn2_iat_patch_all("kernel32.dll", "CreateFileW", (void*)Hook_CreateFileW);
    ul_sn2_iat_patch_all("kernel32.dll", "ReadFile",    (void*)Hook_ReadFile);
    ul_sn2_iat_patch_all("kernel32.dll", "WriteFile",   (void*)Hook_WriteFile);
    ul_sn2_iat_patch_all("kernel32.dll", "CloseHandle", (void*)Hook_CloseHandle);
    ul_sn2_log("keychip: armed (region_mask=0x%02X game=%s plat=%s ver=%s) -- PRESENCE ONLY\n",
              g_region_mask, g_game_id, g_platform_id, g_ver);
}

#endif /* _WIN32 */
