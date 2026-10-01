/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  rawart_redirect.c -- Raw Thrills "c:\rawart" asset-root redirect.
 * =============================================================================
 *
 *  ENGINE FACTS (from static analysis of sdaemon.exe):
 *    RawApp titles have a HARDCODED asset root: `c:\rawart`. Early in main()
 *    sdaemon calls SetCurrentDirectory("c:\\rawart") (both A and W variants
 *    appear across the RawApp generations) and then opens every rez/pvs/r2c/
 *    prefs file with a path relative to that root. If the chdir fails the
 *    first asset open fails and sdaemon exits silently.
 *
 *    The dump itself IS the contents of `c:\rawart`. On a modern host the
 *    user's Raw Thrills dump lives at some arbitrary path (e.g.
 *    `I:\Raw Thrills Dumps\The Fast and the Furious Super Bikes\`). We must
 *    make `c:\rawart` resolve to that directory.
 *
 *  WHAT THIS STUB DOES:
 *    Two paths, in preference order:
 *      1. If the target dir is passed via ULTRA_RAWART_REDIRECT (set by the
 *         loader-side spawner), we IAT-hook SetCurrentDirectoryA/W AND the
 *         common file-open functions (CreateFileA/W, fopen via GetProcAddress
 *         lookup is NOT feasible for CRT statics, so we only cover the Win32
 *         surface). When the game passes a path starting with "c:\rawart" we
 *         rewrite it to the redirect target on the fly.
 *      2. If ULTRA_RAWART_JUNCTION=1 is set (opt-in, needs write access to
 *         C:\), we CREATE a real NTFS directory-junction `C:\rawart -> target`
 *         at hook init. This is the Lindbergh disk0/disk1 junction pattern
 *         (see memory: ultra-loader-eeprom-crash) and is byte-safe for the
 *         game -- no IAT hook needed once the junction is in place.
 *
 *    The default is (1) because it does not touch C:\ and works without admin.
 *
 *  Env gate:
 *      ULTRA_RAWART_REDIRECT=<absolute dir>    -- redirect target (required)
 *      ULTRA_RAWART_JUNCTION=1                 -- optional: also create the
 *                                                 real C:\rawart junction
 *
 *  Default: OFF (no ULTRA_RAWART_REDIRECT env -> no hook).
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "rawthrills_hook.h"

typedef BOOL   (WINAPI *pSetCurrentDirectoryA)(LPCSTR);
typedef BOOL   (WINAPI *pSetCurrentDirectoryW)(LPCWSTR);
typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef DWORD  (WINAPI *pGetFileAttributesA)(LPCSTR);
typedef DWORD  (WINAPI *pGetFileAttributesW)(LPCWSTR);

static pSetCurrentDirectoryA Real_SetCurrentDirectoryA = NULL;
static pSetCurrentDirectoryW Real_SetCurrentDirectoryW = NULL;
static pCreateFileA          Real_CreateFileA          = NULL;
static pCreateFileW          Real_CreateFileW          = NULL;
static pGetFileAttributesA   Real_GetFileAttributesA   = NULL;
static pGetFileAttributesW   Real_GetFileAttributesW   = NULL;

static int  g_enabled = 0;
static char g_target[MAX_PATH] = {0};        /* the redirect destination */

/* Does path (case-insensitive) start with "c:\rawart"?  Accept forward slashes
 * too for defensive coverage; sdaemon uses backslashes but the RawApp source
 * has both. */
static int starts_with_rawart_a(LPCSTR p) {
    if (!p) return 0;
    /* Skip leading "\\?\" prefix if present. */
    if (p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\') p += 4;
    if ((p[0]|0x20) != 'c') return 0;
    if (p[1] != ':') return 0;
    if (p[2] != '\\' && p[2] != '/') return 0;
    const char* r = p + 3;
    return ((r[0]|0x20)=='r' && (r[1]|0x20)=='a' && (r[2]|0x20)=='w'
         && (r[3]|0x20)=='a' && (r[4]|0x20)=='r' && (r[5]|0x20)=='t');
}
static int starts_with_rawart_w(LPCWSTR p) {
    if (!p) return 0;
    if (p[0]==L'\\' && p[1]==L'\\' && p[2]==L'?' && p[3]==L'\\') p += 4;
    if ((p[0]|0x20) != L'c') return 0;
    if (p[1] != L':') return 0;
    if (p[2] != L'\\' && p[2] != L'/') return 0;
    const wchar_t* r = p + 3;
    return ((r[0]|0x20)==L'r' && (r[1]|0x20)==L'a' && (r[2]|0x20)==L'w'
         && (r[3]|0x20)==L'a' && (r[4]|0x20)==L'r' && (r[5]|0x20)==L't');
}

/* Rewrite "c:\rawart\<tail>" into "<g_target>\<tail>". Buf must be MAX_PATH. */
static void rewrite_a(char* dst, size_t cap, LPCSTR src) {
    /* Find the tail after "c:\rawart". */
    const char* p = src;
    if (p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\') p += 4;
    const char* tail = p + 3 + 6;   /* skip "c:\" + "rawart" */
    /* Copy target + tail; guarantee nul-term inside cap. */
    size_t tn = strlen(g_target);
    if (tn >= cap) tn = cap - 1;
    memcpy(dst, g_target, tn);
    size_t remain = cap - tn - 1;
    strncpy(dst + tn, tail, remain);
    dst[cap - 1] = 0;
}
static void rewrite_w(wchar_t* dst, size_t cap, LPCWSTR src) {
    const wchar_t* p = src;
    if (p[0]==L'\\' && p[1]==L'\\' && p[2]==L'?' && p[3]==L'\\') p += 4;
    const wchar_t* tail = p + 3 + 6;
    /* target is char -- widen. */
    size_t i = 0;
    for (const char* t = g_target; *t && i < cap - 1; ++t, ++i) dst[i] = (wchar_t)(unsigned char)*t;
    for (const wchar_t* t = tail; *t && i < cap - 1; ++t, ++i) dst[i] = *t;
    dst[i] = 0;
}

static BOOL WINAPI Hook_SetCurrentDirectoryA(LPCSTR p) {
    if (g_enabled && starts_with_rawart_a(p)) {
        char buf[MAX_PATH];
        rewrite_a(buf, sizeof buf, p);
        ul_rt_log("rawart: chdir %s -> %s\n", p, buf);
        return Real_SetCurrentDirectoryA
               ? Real_SetCurrentDirectoryA(buf)
               : SetCurrentDirectoryA(buf);
    }
    return Real_SetCurrentDirectoryA ? Real_SetCurrentDirectoryA(p) : FALSE;
}
static BOOL WINAPI Hook_SetCurrentDirectoryW(LPCWSTR p) {
    if (g_enabled && starts_with_rawart_w(p)) {
        wchar_t buf[MAX_PATH];
        rewrite_w(buf, MAX_PATH, p);
        ul_rt_log("rawart: chdirW -> redirect\n");
        return Real_SetCurrentDirectoryW
               ? Real_SetCurrentDirectoryW(buf)
               : SetCurrentDirectoryW(buf);
    }
    return Real_SetCurrentDirectoryW ? Real_SetCurrentDirectoryW(p) : FALSE;
}
static HANDLE WINAPI Hook_CreateFileA(LPCSTR n, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && starts_with_rawart_a(n)) {
        char buf[MAX_PATH];
        rewrite_a(buf, sizeof buf, n);
        return Real_CreateFileA ? Real_CreateFileA(buf, acc, sh, sa, disp, flags, tmpl)
                                : CreateFileA(buf, acc, sh, sa, disp, flags, tmpl);
    }
    return Real_CreateFileA ? Real_CreateFileA(n, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR n, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && starts_with_rawart_w(n)) {
        wchar_t buf[MAX_PATH];
        rewrite_w(buf, MAX_PATH, n);
        return Real_CreateFileW ? Real_CreateFileW(buf, acc, sh, sa, disp, flags, tmpl)
                                : CreateFileW(buf, acc, sh, sa, disp, flags, tmpl);
    }
    return Real_CreateFileW ? Real_CreateFileW(n, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}
static DWORD WINAPI Hook_GetFileAttributesA(LPCSTR n) {
    if (g_enabled && starts_with_rawart_a(n)) {
        char buf[MAX_PATH];
        rewrite_a(buf, sizeof buf, n);
        return Real_GetFileAttributesA ? Real_GetFileAttributesA(buf)
                                       : GetFileAttributesA(buf);
    }
    return Real_GetFileAttributesA ? Real_GetFileAttributesA(n) : INVALID_FILE_ATTRIBUTES;
}
static DWORD WINAPI Hook_GetFileAttributesW(LPCWSTR n) {
    if (g_enabled && starts_with_rawart_w(n)) {
        wchar_t buf[MAX_PATH];
        rewrite_w(buf, MAX_PATH, n);
        return Real_GetFileAttributesW ? Real_GetFileAttributesW(buf)
                                       : GetFileAttributesW(buf);
    }
    return Real_GetFileAttributesW ? Real_GetFileAttributesW(n) : INVALID_FILE_ATTRIBUTES;
}

void ul_rt_rawart_install(void) {
    const char* tgt = ul_rt_env("ULTRA_RAWART_REDIRECT");
    if (!tgt || !tgt[0]) {
        ul_rt_log("rawart: skipped (no ULTRA_RAWART_REDIRECT)\n");
        return;
    }
    if (g_enabled) return;
    g_enabled = 1;
    strncpy(g_target, tgt, sizeof(g_target) - 1);
    g_target[sizeof(g_target) - 1] = 0;
    /* Strip trailing slash so rewrite_a's concat is clean. */
    size_t tn = strlen(g_target);
    while (tn > 0 && (g_target[tn-1] == '\\' || g_target[tn-1] == '/')) {
        g_target[--tn] = 0;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
        Real_SetCurrentDirectoryA = (pSetCurrentDirectoryA)(void*)GetProcAddress(k32, "SetCurrentDirectoryA");
        Real_SetCurrentDirectoryW = (pSetCurrentDirectoryW)(void*)GetProcAddress(k32, "SetCurrentDirectoryW");
        Real_CreateFileA          = (pCreateFileA)         (void*)GetProcAddress(k32, "CreateFileA");
        Real_CreateFileW          = (pCreateFileW)         (void*)GetProcAddress(k32, "CreateFileW");
        Real_GetFileAttributesA   = (pGetFileAttributesA)  (void*)GetProcAddress(k32, "GetFileAttributesA");
        Real_GetFileAttributesW   = (pGetFileAttributesW)  (void*)GetProcAddress(k32, "GetFileAttributesW");
    }

    ul_rt_iat_patch_all("kernel32.dll", "SetCurrentDirectoryA", (void*)Hook_SetCurrentDirectoryA);
    ul_rt_iat_patch_all("kernel32.dll", "SetCurrentDirectoryW", (void*)Hook_SetCurrentDirectoryW);
    /* Chain: capture the previous IAT target as our Real_ so paths we do NOT
     * redirect (anything not c:\rawart, e.g. the JammaUSB synthetic HID path)
     * fall through to the CreateFile hook installed before us (jammausb) rather
     * than to raw kernel32 -- which would silently fail the synthetic board open
     * and kill controls. rawart installs LAST, so without this it shadows the
     * jammausb HID sentinel. */
    { void* pa = ul_rt_iat_patch_all("kernel32.dll", "CreateFileA", (void*)Hook_CreateFileA);
      if (pa) Real_CreateFileA = (pCreateFileA)pa; }
    { void* pw = ul_rt_iat_patch_all("kernel32.dll", "CreateFileW", (void*)Hook_CreateFileW);
      if (pw) Real_CreateFileW = (pCreateFileW)pw; }
    ul_rt_iat_patch_all("kernel32.dll", "GetFileAttributesA",   (void*)Hook_GetFileAttributesA);
    ul_rt_iat_patch_all("kernel32.dll", "GetFileAttributesW",   (void*)Hook_GetFileAttributesW);

    /* Optional: create the real C:\rawart junction as an additional safety
     * net for any CRT-static path (e.g. sdaemon calling _chdir via msvcrt). */
    if (ul_rt_env_on("ULTRA_RAWART_JUNCTION")) {
        /* CreateSymbolicLinkA + SYMBOLIC_LINK_FLAG_DIRECTORY is the modern
         * equivalent of `mklink /J`. Needs admin OR developer-mode; if it
         * fails we log and continue -- the IAT redirect above still works. */
        typedef BOOLEAN (WINAPI *pCreateSymbolicLinkA)(LPCSTR,LPCSTR,DWORD);
        pCreateSymbolicLinkA csl = (pCreateSymbolicLinkA)(void*)GetProcAddress(
            GetModuleHandleA("kernel32.dll"), "CreateSymbolicLinkA");
        if (csl) {
            if (csl("C:\\rawart", g_target, 0x1 | 0x2 /* DIR | ALLOW_UNPRIV */)) {
                ul_rt_log("rawart: created C:\\rawart junction -> %s\n", g_target);
            } else {
                ul_rt_log("rawart: junction create failed (%lu) -- IAT redirect still active\n",
                          GetLastError());
            }
        }
    }
    ul_rt_log("rawart: armed (redirect c:\\rawart -> %s)\n", g_target);
}

#endif /* _WIN32 */
