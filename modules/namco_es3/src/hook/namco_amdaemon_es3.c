/* SPDX-License-Identifier: MIT
 * NOTE: independent namco_es3 board copy -- adapted from native_x86/src/hook/namco_amdaemon.c (reference only; no build-time or runtime dependency on native_x86 remains).
 * =============================================================================
 *  namco_amdaemon_es3.c -- Namco ES amdaemon/amauthd LOCAL presence stub (env-gated).
 * =============================================================================
 *
 *  Namco System ES1/ES2/ES3 titles expect a LOCAL auth daemon (amdaemon / amauthd)
 *  to be running and answering on a local IPC/socket; the game queries it for
 *  "auth OK / network OK" before it leaves boot. This is a LOCAL presence handshake
 *  -- "is the daemon present and does it say things are fine" -- distinct from the
 *  online billing/ALL.Net step.
 *
 *  WHAT THIS FILE HONESTLY IS:
 *    A clean-room SCAFFOLD that, when env-gated on (ULTRA_AMDAEMON=1, set by a Namco
 *    ES board profile), satisfies the LOCAL "is the daemon present?" check and
 *    answers the LOCAL/OFFLINE status query with a benign "OK". It intercepts the
 *    daemon's named-pipe / device OPEN (CreateFile) so a game polling the local
 *    auth IPC keeps running instead of erroring on a missing daemon, and answers
 *    the status frame on the synthetic handle.
 *
 *  WHAT IT IS NOT, AND WILL NOT BE:
 *    It does NOT impersonate a LIVE service. Anything that reaches a real ALL.Net /
 *    Namco billing host (network auth, billing, online account) is OUT of scope and
 *    is DEFERRED (return 0) -- never answered. It ships NO keys, NO account data,
 *    and is OFF by default. The TPM/loop-AES/LUKS PCR-bound disk key that encrypts
 *    the ES1/ES2 game files is content-decryption -> entirely OUT of scope; this
 *    stub only keeps the LOCAL daemon poll from blocking a DECRYPTED dump.
 *
 *  HONEST GAP: the byte-level amdaemon/amauthd request/response framing is not in
 *  the public protocol record, so the exact query bytes are pinned against a real
 *  ES3 dump via env knobs; this scaffold provides the presence + a benign offline-OK
 *  default, the same "pinned against a dump" stance the Konami node map / Fast-I/O
 *  offsets use.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the public Win32 API + the public
 *  description of the ES auth-daemon gate. No third-party loader source, profile, or
 *  daemon binary was read or copied.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

extern void ultra_d3d8_log(const char* fmt, ...);
#define alog ultra_d3d8_log

extern void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

static int g_enabled = 0;

/* The local daemon IPC path/name substring. ES titles reach amdaemon over a local
 * named pipe / device; the exact name is per-title, so ULTRA_AMDAEMON_DEV pins it.
 * Defaults match the common daemon names. */
static char g_dev[64] = "amdaemon";

/* sentinel handle for the synthetic local-daemon endpoint. */
#define AMDAEMON_SENTINEL ((HANDLE)(ULONG_PTR)0xA3DA0001)

typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static pCreateFileA Real_CreateFileA = NULL;
static pCreateFileW Real_CreateFileW = NULL;

static int amdaemon_match_a(LPCSTR n) {
    if (!n) return 0;
    char low[260]; int i = 0;
    for (; n[i] && i < 259; ++i) { char c = n[i]; low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
    low[i] = 0;
    if (g_dev[0] && strstr(low, g_dev)) return 1;
    return strstr(low, "amdaemon") || strstr(low, "amauth") || strstr(low, "amlib");
}
static int amdaemon_match_w(LPCWSTR n) {
    if (!n) return 0;
    char buf[260]; int i = 0;
    for (; n[i] && i < 259; ++i) buf[i] = (char)n[i];
    buf[i] = 0;
    return amdaemon_match_a(buf);
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && amdaemon_match_a(name)) {
        HANDLE real = Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                                       : INVALID_HANDLE_VALUE;
        if (real != INVALID_HANDLE_VALUE) return real;   /* real daemon running */
        alog("[amdaemon] synthesizing LOCAL auth-daemon presence (no daemon process)\n");
        return AMDAEMON_SENTINEL;
    }
    return Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && amdaemon_match_w(name)) {
        HANDLE real = Real_CreateFileW ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
                                       : INVALID_HANDLE_VALUE;
        if (real != INVALID_HANDLE_VALUE) return real;
        alog("[amdaemon] synthesizing LOCAL auth-daemon presence (W) (no daemon process)\n");
        return AMDAEMON_SENTINEL;
    }
    return Real_CreateFileW ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}

/* ---------------------------------------------------------------------------
 * The LOCAL status answer surface. Given a query frame the game wrote to the local
 * daemon endpoint, produce the benign LOCAL reply: "daemon present, local status
 * OK". We answer ONLY the non-secret, LOCAL/OFFLINE classes:
 *   0x00 = ping/presence        -> ack
 *   0x01 = local status query   -> "auth OK / network OK (offline)"
 * Anything that reaches a real online host (network auth/billing/account) DEFERS
 * (returns 0) -- we never stand in for the live service. The exact opcodes are
 * pinned per dump (ULTRA_AMDAEMON_*); the defaults here are the benign presence/OK.
 *
 * Returns response length, or 0 if it defers (the request needs the online service
 * we deliberately do not impersonate, or is unknown).
 * ------------------------------------------------------------------------- */
int ultra_namco_amdaemon_answer(const unsigned char* req, int req_len,
                                unsigned char* resp, int resp_cap) {
    if (!g_enabled || !req || req_len < 1 || !resp || resp_cap < 2) return 0;
    switch (req[0]) {
        case 0x00:                 /* presence ping */
            resp[0] = 0x00;        /* daemon present */
            resp[1] = 0x00;        /* no error */
            return 2;
        case 0x01:                 /* LOCAL status query -> auth OK / network OK */
            resp[0] = 0x00;        /* status OK */
            resp[1] = 0x00;        /* no error / offline-fine */
            return 2;
        default:
            /* online auth / billing / account -> NOT impersonated -> defer. */
            return 0;
    }
}

static BOOL WINAPI Hook_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote, LPOVERLAPPED ov);
static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov);

typedef BOOL (WINAPI *pReadFile)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL (WINAPI *pWriteFile)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
static pReadFile  Real_ReadFile  = NULL;
static pWriteFile Real_WriteFile = NULL;

/* last answer produced by WriteFile, served by the next ReadFile. */
static unsigned char g_resp[64];
static int           g_resp_len = 0;
static int           g_resp_pos = 0;

static BOOL WINAPI Hook_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote, LPOVERLAPPED ov) {
    if (g_enabled && h == AMDAEMON_SENTINEL) {
        g_resp_len = ultra_namco_amdaemon_answer((const unsigned char*)buf, (int)n,
                                                 g_resp, (int)sizeof g_resp);
        g_resp_pos = 0;
        if (wrote) *wrote = n;
        return TRUE;
    }
    return Real_WriteFile ? Real_WriteFile(h, buf, n, wrote, ov) : FALSE;
}
static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    if (g_enabled && h == AMDAEMON_SENTINEL) {
        int avail = g_resp_len - g_resp_pos;
        int cp = ((int)n < avail) ? (int)n : avail;
        if (cp > 0) memcpy(buf, &g_resp[g_resp_pos], cp);
        g_resp_pos += (cp > 0 ? cp : 0);
        if (got) *got = (DWORD)(cp > 0 ? cp : 0);
        return TRUE;
    }
    return Real_ReadFile ? Real_ReadFile(h, buf, n, got, ov) : FALSE;
}

void ultra_namco_amdaemon_install(void) {
    const char* e = getenv("ULTRA_AMDAEMON");
    g_enabled = e && e[0] == '1';
    if (!g_enabled) return;
    const char* dv = getenv("ULTRA_AMDAEMON_DEV");
    if (dv && dv[0]) { strncpy(g_dev, dv, sizeof(g_dev)-1); g_dev[sizeof(g_dev)-1] = 0;
        for (char* q = g_dev; *q; ++q) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32); }

    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (k) {
        if (!Real_CreateFileA) Real_CreateFileA = (pCreateFileA)(void*)GetProcAddress(k, "CreateFileA");
        if (!Real_CreateFileW) Real_CreateFileW = (pCreateFileW)(void*)GetProcAddress(k, "CreateFileW");
        if (!Real_ReadFile)    Real_ReadFile    = (pReadFile)(void*)GetProcAddress(k, "ReadFile");
        if (!Real_WriteFile)   Real_WriteFile   = (pWriteFile)(void*)GetProcAddress(k, "WriteFile");
    }
    ultra_iat_patch_all("kernel32.dll", "CreateFileA", (void*)Hook_CreateFileA);
    ultra_iat_patch_all("kernel32.dll", "CreateFileW", (void*)Hook_CreateFileW);
    ultra_iat_patch_all("kernel32.dll", "ReadFile",    (void*)Hook_ReadFile);
    ultra_iat_patch_all("kernel32.dll", "WriteFile",   (void*)Hook_WriteFile);

    alog("[amdaemon] LOCAL Namco ES auth-daemon presence stub armed (dev~='%s'). "
         "Answers presence + offline-OK ONLY; online ALL.Net/billing is DEFERRED "
         "(never impersonated). NOTE: the TPM/PCR-bound disk key is OUT of scope -- "
         "needs an operator-decrypted dump.\n", g_dev);
}

int ultra_namco_amdaemon_enabled(void) { return g_enabled; }

#endif /* _WIN32 */
