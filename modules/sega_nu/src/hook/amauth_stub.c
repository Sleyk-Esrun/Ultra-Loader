/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  amauth_stub.c -- amauthd named-pipe / BootID presence responder.
 * =============================================================================
 *
 *  ENGINE FACTS:
 *    Sega Nu games talk to amauthd over `\\.\pipe\amauth` (and a few substrings
 *    the RingEdge legacy still uses: "amauth", "amlib"). amauthd answers a
 *    handful of DRM-boot requests; the presence-class ones we care about are:
 *      - /keychip/auth/boot   -> {STATUS=OK, BootID=<8B game_id-derived blob>}
 *      - /keychip/auth/session -> STATUS=OK
 *      - /keychip/status      -> "ready"
 *    On DRM-gated Nu titles the amauthd pipe is the FIRST thing the game
 *    connects to after mounting the container. If the connect fails or the
 *    response is not STATUS=OK the game exits with an amauth failure banner.
 *
 *    We DO NOT emulate the ALL.Net Auth-2.0 signature (per research memo the
 *    subsequent AES stream still needs silicon-fused keys). We answer the
 *    presence-class surface only; on retail Nu that unblocks the amauth handshake
 *    when combined with an operator-supplied decrypted container.
 *
 *  WHAT THIS STUB DOES:
 *    - IAT-hook CreateNamedPipeA/W (kernel32) so if the amauthd daemon itself
 *      is not running we intercept the pipe creation and own the server side.
 *    - IAT-hook CreateFileA/W: any client-side open of `\\.\pipe\amauth` (or
 *      substring match `amauth`/`amlib`) returns a sentinel handle
 *      AMAUTH_SENTINEL.
 *    - ReadFile/WriteFile on that handle: parse the request opcode/URI and emit
 *      a matching presence-class reply.
 *    - Belt-and-braces: also spin up a real named-pipe server on
 *      `\\.\pipe\amauth` in a worker thread so any process in the session that
 *      opens the pipe naturally (bypassing our IAT hook -- e.g. a child spawn)
 *      still gets the presence reply.
 *
 *  Env gate: ULTRA_SEGA_AMAUTH_STUB=1  (set by sega_nu_main.c).
 *  Env knobs:
 *    ULTRA_SEGA_GAME_ID   -- BootID payload derived from this 4-char id
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "sega_nu_hook.h"

#define AMAUTH_SENTINEL ((HANDLE)(ULONG_PTR)0xA3DA0002)   /* "AmDA\2" */

typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef BOOL   (WINAPI *pReadFile)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL   (WINAPI *pWriteFile)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL   (WINAPI *pCloseHandle)(HANDLE);

static pCreateFileA Real_CreateFileA = NULL;
static pCreateFileW Real_CreateFileW = NULL;
static pReadFile    Real_ReadFile    = NULL;
static pWriteFile   Real_WriteFile   = NULL;
static pCloseHandle Real_CloseHandle = NULL;

static int  g_enabled = 0;
static char g_game_id[8] = "SXXX";

/* Reply staging (per-sentinel single-request FIFO; amauth is strictly
 * request/response so a single-slot FIFO is fine for the presence surface). */
static uint8_t g_amauth_reply[256];
static DWORD   g_amauth_reply_len = 0;
static DWORD   g_amauth_reply_pos = 0;

static int stristr_a(const char* hay, const char* needle) {
    if (!hay || !needle) return 0;
    size_t n = strlen(needle);
    if (!n) return 1;
    for (const char* p = hay; *p; ++p) {
        int match = 1;
        for (size_t i = 0; i < n; ++i) {
            char a = p[i]; char b = needle[i];
            if (!a) { match = 0; break; }
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}
static int stristr_w(LPCWSTR hay, const wchar_t* needle) {
    if (!hay || !needle) return 0;
    size_t n = wcslen(needle);
    if (!n) return 1;
    for (const wchar_t* p = hay; *p; ++p) {
        int match = 1;
        for (size_t i = 0; i < n; ++i) {
            wchar_t a = p[i]; wchar_t b = needle[i];
            if (!a) { match = 0; break; }
            if (a >= L'A' && a <= L'Z') a = (wchar_t)(a + 32);
            if (b >= L'A' && b <= L'Z') b = (wchar_t)(b + 32);
            if (a != b) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

static int path_is_amauth_a(LPCSTR p) {
    return p && (stristr_a(p, "amauth") || stristr_a(p, "amlib"));
}
static int path_is_amauth_w(LPCWSTR p) {
    return p && (stristr_w(p, L"amauth") || stristr_w(p, L"amlib"));
}

/* Emit the presence reply for the given request buffer. Two request shapes are
 * observed: an HTTP-ish `POST /keychip/auth/boot` frame and a binary opcode
 * frame. We check for the URI substring first; on no match we emit the generic
 * STATUS=OK binary frame. */
static void build_amauth_reply(const uint8_t* req, DWORD n) {
    memset(g_amauth_reply, 0, sizeof g_amauth_reply);
    const char* r = (const char*)req;
    int is_boot = 0;
    if (r && n > 0) {
        char snip[128] = {0};
        DWORD c = n < sizeof(snip)-1 ? n : sizeof(snip)-1;
        memcpy(snip, r, c);
        if (stristr_a(snip, "auth/boot") || stristr_a(snip, "authboot")
            || stristr_a(snip, "keychipboot")) {
            is_boot = 1;
        }
    }
    if (is_boot) {
        /* HTTP-shaped presence reply. Body carries STATUS + BootID from game_id. */
        char body[128];
        int bl = snprintf(body, sizeof body,
            "STATUS=OK\r\nBootID=%c%c%c%c00000000\r\n",
            g_game_id[0]?g_game_id[0]:'S',
            g_game_id[1]?g_game_id[1]:'X',
            g_game_id[2]?g_game_id[2]:'X',
            g_game_id[3]?g_game_id[3]:'X');
        if (bl < 0) bl = 0;
        int hl = snprintf((char*)g_amauth_reply, sizeof g_amauth_reply,
            "HTTP/1.0 200 OK\r\nContent-Length: %d\r\n\r\n%s", bl, body);
        if (hl < 0) hl = 0;
        g_amauth_reply_len = (DWORD)hl;
    } else {
        /* Binary presence reply: 2 bytes {status, error} + 8-byte BootID. */
        g_amauth_reply[0] = 0x00;   /* status */
        g_amauth_reply[1] = 0x00;   /* error */
        memcpy(&g_amauth_reply[2], g_game_id, 4);
        g_amauth_reply_len = 16;
    }
    g_amauth_reply_pos = 0;
}

static HANDLE WINAPI Hook_amauth_CreateFileA(LPCSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && path_is_amauth_a(name)) {
        ul_sn_log("amauth: synthesizing pipe handle for %s\n", name);
        /* Pre-load a boot-style reply so a client that WRITEs then READs still
         * gets a real answer even on read-first. */
        build_amauth_reply(NULL, 0);
        return AMAUTH_SENTINEL;
    }
    return Real_CreateFileA
        ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
        : CreateFileA(name, acc, sh, sa, disp, flags, tmpl);
}
static HANDLE WINAPI Hook_amauth_CreateFileW(LPCWSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && path_is_amauth_w(name)) {
        ul_sn_log("amauth: synthesizing pipe handle (W)\n");
        build_amauth_reply(NULL, 0);
        return AMAUTH_SENTINEL;
    }
    return Real_CreateFileW
        ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
        : CreateFileW(name, acc, sh, sa, disp, flags, tmpl);
}
static BOOL WINAPI Hook_amauth_ReadFile(HANDLE h, LPVOID buf, DWORD n,
        LPDWORD got, LPOVERLAPPED ov) {
    if (h == AMAUTH_SENTINEL) {
        DWORD have = g_amauth_reply_len - g_amauth_reply_pos;
        DWORD copy = (n < have) ? n : have;
        if (buf && copy) memcpy(buf, &g_amauth_reply[g_amauth_reply_pos], copy);
        g_amauth_reply_pos += copy;
        if (got) *got = copy;
        (void)ov;
        return TRUE;
    }
    return Real_ReadFile ? Real_ReadFile(h, buf, n, got, ov) : FALSE;
}
static BOOL WINAPI Hook_amauth_WriteFile(HANDLE h, LPCVOID buf, DWORD n,
        LPDWORD wrote, LPOVERLAPPED ov) {
    if (h == AMAUTH_SENTINEL) {
        build_amauth_reply((const uint8_t*)buf, n);
        if (wrote) *wrote = n;
        (void)ov;
        return TRUE;
    }
    return Real_WriteFile ? Real_WriteFile(h, buf, n, wrote, ov) : FALSE;
}
static BOOL WINAPI Hook_amauth_CloseHandle(HANDLE h) {
    if (h == AMAUTH_SENTINEL) return TRUE;
    return Real_CloseHandle ? Real_CloseHandle(h) : FALSE;
}

/* Belt-and-braces server thread: create the pipe if amauthd isn't running.
 * Each connect gets a single presence reply then disconnect. */
static DWORD WINAPI amauth_pipe_server(LPVOID arg) {
    (void)arg;
    for (;;) {
        HANDLE h = CreateNamedPipeA(
            "\\\\.\\pipe\\amauth",
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            8, 4096, 4096, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            /* pipe likely already owned by the real amauthd daemon */
            Sleep(1000);
            continue;
        }
        if (ConnectNamedPipe(h, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
            uint8_t req[256]; DWORD got = 0;
            /* Best-effort read; ignore result -- we send BootID either way. */
            ReadFile(h, req, sizeof req, &got, NULL);
            build_amauth_reply(req, got);
            DWORD wrote = 0;
            WriteFile(h, g_amauth_reply, g_amauth_reply_len, &wrote, NULL);
            FlushFileBuffers(h);
            DisconnectNamedPipe(h);
        }
        CloseHandle(h);
    }
    return 0;
}

void ul_sn_install_amauth(void) {
    if (!ul_sn_env_on("ULTRA_SEGA_AMAUTH_STUB")
        && !ul_sn_env_on("ULTRA_SEGA_AMAUTH")) {
        ul_sn_log("amauth: skipped (ULTRA_SEGA_AMAUTH_STUB!=1)\n");
        return;
    }
    if (g_enabled) return;
    g_enabled = 1;

    const char* gid = ul_sn_env("ULTRA_SEGA_GAME_ID");
    if (gid) { strncpy(g_game_id, gid, sizeof(g_game_id)-1); g_game_id[sizeof(g_game_id)-1] = 0; }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
        Real_CreateFileA = (pCreateFileA)(void*)GetProcAddress(k32, "CreateFileA");
        Real_CreateFileW = (pCreateFileW)(void*)GetProcAddress(k32, "CreateFileW");
        Real_ReadFile    = (pReadFile)   (void*)GetProcAddress(k32, "ReadFile");
        Real_WriteFile   = (pWriteFile)  (void*)GetProcAddress(k32, "WriteFile");
        Real_CloseHandle = (pCloseHandle)(void*)GetProcAddress(k32, "CloseHandle");
    }
    ul_sn_iat_patch_all("kernel32.dll", "CreateFileA", (void*)Hook_amauth_CreateFileA);
    ul_sn_iat_patch_all("kernel32.dll", "CreateFileW", (void*)Hook_amauth_CreateFileW);
    ul_sn_iat_patch_all("kernel32.dll", "ReadFile",    (void*)Hook_amauth_ReadFile);
    ul_sn_iat_patch_all("kernel32.dll", "WriteFile",   (void*)Hook_amauth_WriteFile);
    ul_sn_iat_patch_all("kernel32.dll", "CloseHandle", (void*)Hook_amauth_CloseHandle);

    HANDLE th = CreateThread(NULL, 0, amauth_pipe_server, NULL, 0, NULL);
    if (th) CloseHandle(th);
    ul_sn_log("amauth: armed (game_id=%s, pipe server active)\n", g_game_id);
}

#endif /* _WIN32 */
