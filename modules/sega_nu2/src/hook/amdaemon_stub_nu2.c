/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  amdaemon_stub_nu2.c -- amdaemon local IPC presence stub
 *                        (Sega Nu2 board module -- fresh, independent copy).
 * =============================================================================
 *
 *  ENGINE FACTS:
 *    amdaemon.exe (a Sega-supplied local IPC daemon) owns the pipe
 *    `\\.\pipe\amdaemon`. Nu2 titles poll it for a status word ("ready\n" or
 *    the binary equivalent {0x00, 0x00}) and a couple of opcodes (get_state,
 *    heartbeat, coin_credit). Without a reply the launcher retries and
 *    eventually gives up.
 *
 *  WHAT THIS STUB DOES:
 *    - IAT-hook CreateFileA/W: any client-side open of `\\.\pipe\amdaemon`
 *      (or substring `amdaemon`) returns AMDAEMON_SENTINEL.
 *    - ReadFile/WriteFile on that handle emit "ready\n" (or the 2-byte OK
 *      framing) depending on the client's request byte.
 *    - Belt-and-braces server thread on the real pipe name (same as
 *      amauth_stub_nu2.c) so a spawn that bypasses IAT still connects.
 *
 *  Env gate: ULTRA_SEGA_AMDAEMON=1 (set by sega_nu2_main.c).
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "sega_nu2_hook.h"

#define AMDAEMON_SENTINEL ((HANDLE)(ULONG_PTR)0xA3DB0001)   /* Nu2's own sentinel */

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

static int g_enabled = 0;

static uint8_t g_reply[64];
static DWORD   g_reply_len = 0;
static DWORD   g_reply_pos = 0;

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

static void build_amdaemon_reply(const uint8_t* req, DWORD n) {
    memset(g_reply, 0, sizeof g_reply);
    /* If the request byte 0 looks like an ASCII query ('s'tatus, 'g'et,
     * 'h'eartbeat), reply "ready\n"; otherwise emit a 2-byte OK. */
    uint8_t b0 = (req && n) ? req[0] : 0;
    if (b0 >= 'a' && b0 <= 'z') {
        memcpy(g_reply, "ready\n", 6);
        g_reply_len = 6;
    } else {
        g_reply[0] = 0x00;   /* status = OK */
        g_reply[1] = 0x00;   /* error = none */
        g_reply_len = 2;
    }
    g_reply_pos = 0;
}

static HANDLE WINAPI Hook_amd_CreateFileA(LPCSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && name && stristr_a(name, "amdaemon")) {
        ul_sn2_log("amdaemon: synthesizing pipe handle for %s\n", name);
        build_amdaemon_reply(NULL, 0);
        return AMDAEMON_SENTINEL;
    }
    return Real_CreateFileA
        ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
        : CreateFileA(name, acc, sh, sa, disp, flags, tmpl);
}
static HANDLE WINAPI Hook_amd_CreateFileW(LPCWSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && name && stristr_w(name, L"amdaemon")) {
        ul_sn2_log("amdaemon: synthesizing pipe handle (W)\n");
        build_amdaemon_reply(NULL, 0);
        return AMDAEMON_SENTINEL;
    }
    return Real_CreateFileW
        ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
        : CreateFileW(name, acc, sh, sa, disp, flags, tmpl);
}
static BOOL WINAPI Hook_amd_ReadFile(HANDLE h, LPVOID buf, DWORD n,
        LPDWORD got, LPOVERLAPPED ov) {
    if (h == AMDAEMON_SENTINEL) {
        DWORD have = g_reply_len - g_reply_pos;
        DWORD copy = (n < have) ? n : have;
        if (buf && copy) memcpy(buf, &g_reply[g_reply_pos], copy);
        g_reply_pos += copy;
        if (got) *got = copy;
        (void)ov;
        return TRUE;
    }
    return Real_ReadFile ? Real_ReadFile(h, buf, n, got, ov) : FALSE;
}
static BOOL WINAPI Hook_amd_WriteFile(HANDLE h, LPCVOID buf, DWORD n,
        LPDWORD wrote, LPOVERLAPPED ov) {
    if (h == AMDAEMON_SENTINEL) {
        build_amdaemon_reply((const uint8_t*)buf, n);
        if (wrote) *wrote = n;
        (void)ov;
        return TRUE;
    }
    return Real_WriteFile ? Real_WriteFile(h, buf, n, wrote, ov) : FALSE;
}
static BOOL WINAPI Hook_amd_CloseHandle(HANDLE h) {
    if (h == AMDAEMON_SENTINEL) return TRUE;
    return Real_CloseHandle ? Real_CloseHandle(h) : FALSE;
}

static DWORD WINAPI amdaemon_pipe_server(LPVOID arg) {
    (void)arg;
    for (;;) {
        HANDLE h = CreateNamedPipeA(
            "\\\\.\\pipe\\amdaemon",
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            8, 4096, 4096, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }
        if (ConnectNamedPipe(h, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
            uint8_t req[128]; DWORD got = 0;
            ReadFile(h, req, sizeof req, &got, NULL);
            build_amdaemon_reply(req, got);
            DWORD wrote = 0;
            WriteFile(h, g_reply, g_reply_len, &wrote, NULL);
            FlushFileBuffers(h);
            DisconnectNamedPipe(h);
        }
        CloseHandle(h);
    }
    return 0;
}

void ul_sn2_install_amdaemon(void) {
    if (!ul_sn2_env_on("ULTRA_SEGA_AMDAEMON")) {
        ul_sn2_log("amdaemon: skipped (ULTRA_SEGA_AMDAEMON!=1)\n");
        return;
    }
    if (g_enabled) return;
    g_enabled = 1;

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
        Real_CreateFileA = (pCreateFileA)(void*)GetProcAddress(k32, "CreateFileA");
        Real_CreateFileW = (pCreateFileW)(void*)GetProcAddress(k32, "CreateFileW");
        Real_ReadFile    = (pReadFile)   (void*)GetProcAddress(k32, "ReadFile");
        Real_WriteFile   = (pWriteFile)  (void*)GetProcAddress(k32, "WriteFile");
        Real_CloseHandle = (pCloseHandle)(void*)GetProcAddress(k32, "CloseHandle");
    }
    ul_sn2_iat_patch_all("kernel32.dll", "CreateFileA", (void*)Hook_amd_CreateFileA);
    ul_sn2_iat_patch_all("kernel32.dll", "CreateFileW", (void*)Hook_amd_CreateFileW);
    ul_sn2_iat_patch_all("kernel32.dll", "ReadFile",    (void*)Hook_amd_ReadFile);
    ul_sn2_iat_patch_all("kernel32.dll", "WriteFile",   (void*)Hook_amd_WriteFile);
    ul_sn2_iat_patch_all("kernel32.dll", "CloseHandle", (void*)Hook_amd_CloseHandle);

    HANDLE th = CreateThread(NULL, 0, amdaemon_pipe_server, NULL, 0, NULL);
    if (th) CloseHandle(th);
    ul_sn2_log("amdaemon: armed (pipe server active)\n");
}

#endif /* _WIN32 */
