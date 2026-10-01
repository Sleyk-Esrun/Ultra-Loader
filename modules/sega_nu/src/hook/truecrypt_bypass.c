/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  truecrypt_bypass.c -- Sega Nu container-drive (Y:\, X:\) redirect +
 *                        no-op the veracrypt/truecrypt/manage-bde mount tool.
 * =============================================================================
 *
 *  ENGINE FACTS:
 *    Sega Nu mounts the encrypted game container at a FIXED drive letter:
 *      - Y:\  -- early Nu / Nu1.1 (TrueCrypt container "mount.tc",
 *                mounted by am_launcher/amdaemon).
 *      - X:\  -- later Nu1.2 / Nu2 BitLocker variants (some titles use X:\).
 *    Belt-and-braces we redirect ANY access to Y:\* or X:\* to
 *    <ULTRA_SEGA_CONTENT_DIR>\* when those drives don't exist on the host, AND
 *    we no-op the CreateProcess spawn of any mount tool (veracrypt.exe,
 *    truecrypt.exe, manage-bde.exe, mountvol.exe) with a synthesized
 *    PROCESS_INFORMATION so the launcher's WaitForSingleObject completes with
 *    exit code 0.
 *
 *  API surface hooked (all in kernel32.dll unless noted):
 *    - CreateFileA / CreateFileW
 *    - GetFileAttributesA / GetFileAttributesW / GetFileAttributesExA/W
 *    - FindFirstFileA / FindFirstFileW / FindFirstFileExA/W
 *    - SetCurrentDirectoryA / SetCurrentDirectoryW
 *    - GetDiskFreeSpaceExA/W  -- return plausible ~10 GB free
 *    - GetDriveTypeA/W        -- return DRIVE_FIXED for Y:\ / X:\
 *    - GetVolumeInformationA/W -- return label "SEGA"
 *    - CreateProcessA / CreateProcessW -- swallow mount-tool spawns
 *
 *  Env gate:
 *    ULTRA_SEGA_TRUECRYPT_BYPASS=1  (or shorter ULTRA_SEGA_TRUECRYPT=1)
 *  Env knobs:
 *    ULTRA_SEGA_CONTENT_DIR   -- redirect target (required)
 *    ULTRA_SEGA_CONTENT_DRIVE -- override "Y" default; when set only that
 *                                drive is redirected, otherwise both Y and X
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "sega_nu_hook.h"

typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef DWORD  (WINAPI *pGetFileAttributesA)(LPCSTR);
typedef DWORD  (WINAPI *pGetFileAttributesW)(LPCWSTR);
typedef BOOL   (WINAPI *pGetFileAttributesExA)(LPCSTR,GET_FILEEX_INFO_LEVELS,LPVOID);
typedef BOOL   (WINAPI *pGetFileAttributesExW)(LPCWSTR,GET_FILEEX_INFO_LEVELS,LPVOID);
typedef HANDLE (WINAPI *pFindFirstFileA)(LPCSTR,LPWIN32_FIND_DATAA);
typedef HANDLE (WINAPI *pFindFirstFileW)(LPCWSTR,LPWIN32_FIND_DATAW);
typedef HANDLE (WINAPI *pFindFirstFileExA)(LPCSTR,FINDEX_INFO_LEVELS,LPVOID,FINDEX_SEARCH_OPS,LPVOID,DWORD);
typedef HANDLE (WINAPI *pFindFirstFileExW)(LPCWSTR,FINDEX_INFO_LEVELS,LPVOID,FINDEX_SEARCH_OPS,LPVOID,DWORD);
typedef BOOL   (WINAPI *pSetCurrentDirectoryA)(LPCSTR);
typedef BOOL   (WINAPI *pSetCurrentDirectoryW)(LPCWSTR);
typedef BOOL   (WINAPI *pGetDiskFreeSpaceExA)(LPCSTR,PULARGE_INTEGER,PULARGE_INTEGER,PULARGE_INTEGER);
typedef BOOL   (WINAPI *pGetDiskFreeSpaceExW)(LPCWSTR,PULARGE_INTEGER,PULARGE_INTEGER,PULARGE_INTEGER);
typedef UINT   (WINAPI *pGetDriveTypeA)(LPCSTR);
typedef UINT   (WINAPI *pGetDriveTypeW)(LPCWSTR);
typedef BOOL   (WINAPI *pGetVolumeInformationA)(LPCSTR,LPSTR,DWORD,LPDWORD,LPDWORD,LPDWORD,LPSTR,DWORD);
typedef BOOL   (WINAPI *pGetVolumeInformationW)(LPCWSTR,LPWSTR,DWORD,LPDWORD,LPDWORD,LPDWORD,LPWSTR,DWORD);
typedef BOOL   (WINAPI *pCreateProcessA)(LPCSTR,LPSTR,LPSECURITY_ATTRIBUTES,LPSECURITY_ATTRIBUTES,BOOL,DWORD,LPVOID,LPCSTR,LPSTARTUPINFOA,LPPROCESS_INFORMATION);
typedef BOOL   (WINAPI *pCreateProcessW)(LPCWSTR,LPWSTR,LPSECURITY_ATTRIBUTES,LPSECURITY_ATTRIBUTES,BOOL,DWORD,LPVOID,LPCWSTR,LPSTARTUPINFOW,LPPROCESS_INFORMATION);

static pCreateFileA           Real_CreateFileA           = NULL;
static pCreateFileW           Real_CreateFileW           = NULL;
static pGetFileAttributesA    Real_GetFileAttributesA    = NULL;
static pGetFileAttributesW    Real_GetFileAttributesW    = NULL;
static pGetFileAttributesExA  Real_GetFileAttributesExA  = NULL;
static pGetFileAttributesExW  Real_GetFileAttributesExW  = NULL;
static pFindFirstFileA        Real_FindFirstFileA        = NULL;
static pFindFirstFileW        Real_FindFirstFileW        = NULL;
static pFindFirstFileExA      Real_FindFirstFileExA      = NULL;
static pFindFirstFileExW      Real_FindFirstFileExW      = NULL;
static pSetCurrentDirectoryA  Real_SetCurrentDirectoryA  = NULL;
static pSetCurrentDirectoryW  Real_SetCurrentDirectoryW  = NULL;
static pGetDiskFreeSpaceExA   Real_GetDiskFreeSpaceExA   = NULL;
static pGetDiskFreeSpaceExW   Real_GetDiskFreeSpaceExW   = NULL;
static pGetDriveTypeA         Real_GetDriveTypeA         = NULL;
static pGetDriveTypeW         Real_GetDriveTypeW         = NULL;
static pGetVolumeInformationA Real_GetVolumeInformationA = NULL;
static pGetVolumeInformationW Real_GetVolumeInformationW = NULL;
static pCreateProcessA        Real_CreateProcessA        = NULL;
static pCreateProcessW        Real_CreateProcessW        = NULL;

static int  g_enabled = 0;
static char g_target[MAX_PATH] = {0};
static char g_drive_letters[4] = "YX";  /* redirect these letters when unset host */

static int drive_letter_matched(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (const char* p = g_drive_letters; *p; ++p) {
        char q = *p;
        if (q >= 'a' && q <= 'z') q = (char)(q - 32);
        if (c == q) return 1;
    }
    return 0;
}

static int starts_with_container_a(LPCSTR p) {
    if (!p) return 0;
    if (p[0] == '\\' && p[1] == '\\' && (p[2] == '.' || p[2] == '?') && p[3] == '\\') p += 4;
    if (!p[0] || p[1] != ':') return 0;
    if (p[2] && p[2] != '\\' && p[2] != '/') return 0;
    return drive_letter_matched(p[0]);
}
static int starts_with_container_w(LPCWSTR p) {
    if (!p) return 0;
    if (p[0] == L'\\' && p[1] == L'\\' && (p[2] == L'.' || p[2] == L'?') && p[3] == L'\\') p += 4;
    if (!p[0] || p[1] != L':') return 0;
    if (p[2] && p[2] != L'\\' && p[2] != L'/') return 0;
    return drive_letter_matched((char)p[0]);
}

static void rewrite_a(char* dst, size_t cap, LPCSTR src) {
    const char* p = src;
    if (p[0] == '\\' && p[1] == '\\' && (p[2] == '.' || p[2] == '?') && p[3] == '\\') p += 4;
    /* tail = everything after "X:\" (or "X:" if bare) */
    const char* tail = p + 2;
    if (tail[0] == '\\' || tail[0] == '/') ++tail;
    size_t tn = strlen(g_target);
    if (tn >= cap) tn = cap - 1;
    memcpy(dst, g_target, tn);
    if (tn < cap - 1) {
        dst[tn++] = '\\';
        size_t rem = cap - tn - 1;
        strncpy(dst + tn, tail, rem);
        dst[tn + rem] = 0;
    } else {
        dst[cap - 1] = 0;
    }
}
static void rewrite_w(wchar_t* dst, size_t cap, LPCWSTR src) {
    const wchar_t* p = src;
    if (p[0] == L'\\' && p[1] == L'\\' && (p[2] == L'.' || p[2] == L'?') && p[3] == L'\\') p += 4;
    const wchar_t* tail = p + 2;
    if (tail[0] == L'\\' || tail[0] == L'/') ++tail;
    size_t i = 0;
    for (const char* t = g_target; *t && i < cap - 1; ++t, ++i)
        dst[i] = (wchar_t)(unsigned char)*t;
    if (i < cap - 1) dst[i++] = L'\\';
    for (const wchar_t* t = tail; *t && i < cap - 1; ++t, ++i) dst[i] = *t;
    dst[i] = 0;
}

static int cmdline_is_mount_tool_a(LPCSTR app, LPCSTR cmd) {
    static const char* const tools[] = {
        "veracrypt", "truecrypt", "manage-bde", "mountvol", "diskpart", NULL
    };
    const char* hay[2] = { app, cmd };
    for (int h = 0; h < 2; ++h) {
        if (!hay[h]) continue;
        for (int i = 0; tools[i]; ++i) {
            const char* needle = tools[i];
            size_t n = strlen(needle);
            for (const char* q = hay[h]; *q; ++q) {
                int match = 1;
                for (size_t k = 0; k < n; ++k) {
                    char a = q[k]; char b = needle[k];
                    if (!a) { match = 0; break; }
                    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                    if (a != b) { match = 0; break; }
                }
                if (match) return 1;
            }
        }
    }
    return 0;
}
static int cmdline_is_mount_tool_w(LPCWSTR app, LPCWSTR cmd) {
    char sa[MAX_PATH*2] = {0}, sc[MAX_PATH*2] = {0};
    if (app) WideCharToMultiByte(CP_UTF8, 0, app, -1, sa, sizeof sa - 1, NULL, NULL);
    if (cmd) WideCharToMultiByte(CP_UTF8, 0, cmd, -1, sc, sizeof sc - 1, NULL, NULL);
    return cmdline_is_mount_tool_a(app?sa:NULL, cmd?sc:NULL);
}

/* -------------------- hook implementations -------------------- */

static HANDLE WINAPI Hook_tc_CreateFileA(LPCSTR n, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && starts_with_container_a(n)) {
        char buf[MAX_PATH]; rewrite_a(buf, sizeof buf, n);
        return Real_CreateFileA ? Real_CreateFileA(buf, acc, sh, sa, disp, flags, tmpl)
                                : CreateFileA(buf, acc, sh, sa, disp, flags, tmpl);
    }
    return Real_CreateFileA ? Real_CreateFileA(n, acc, sh, sa, disp, flags, tmpl)
                            : CreateFileA(n, acc, sh, sa, disp, flags, tmpl);
}
static HANDLE WINAPI Hook_tc_CreateFileW(LPCWSTR n, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && starts_with_container_w(n)) {
        wchar_t buf[MAX_PATH]; rewrite_w(buf, MAX_PATH, n);
        return Real_CreateFileW ? Real_CreateFileW(buf, acc, sh, sa, disp, flags, tmpl)
                                : CreateFileW(buf, acc, sh, sa, disp, flags, tmpl);
    }
    return Real_CreateFileW ? Real_CreateFileW(n, acc, sh, sa, disp, flags, tmpl)
                            : CreateFileW(n, acc, sh, sa, disp, flags, tmpl);
}
static DWORD WINAPI Hook_tc_GetFileAttributesA(LPCSTR n) {
    if (g_enabled && starts_with_container_a(n)) {
        char buf[MAX_PATH]; rewrite_a(buf, sizeof buf, n);
        return Real_GetFileAttributesA ? Real_GetFileAttributesA(buf)
                                       : GetFileAttributesA(buf);
    }
    return Real_GetFileAttributesA ? Real_GetFileAttributesA(n)
                                   : GetFileAttributesA(n);
}
static DWORD WINAPI Hook_tc_GetFileAttributesW(LPCWSTR n) {
    if (g_enabled && starts_with_container_w(n)) {
        wchar_t buf[MAX_PATH]; rewrite_w(buf, MAX_PATH, n);
        return Real_GetFileAttributesW ? Real_GetFileAttributesW(buf)
                                       : GetFileAttributesW(buf);
    }
    return Real_GetFileAttributesW ? Real_GetFileAttributesW(n)
                                   : GetFileAttributesW(n);
}
static BOOL WINAPI Hook_tc_GetFileAttributesExA(LPCSTR n, GET_FILEEX_INFO_LEVELS lvl, LPVOID info) {
    if (g_enabled && starts_with_container_a(n)) {
        char buf[MAX_PATH]; rewrite_a(buf, sizeof buf, n);
        return Real_GetFileAttributesExA ? Real_GetFileAttributesExA(buf, lvl, info)
                                         : GetFileAttributesExA(buf, lvl, info);
    }
    return Real_GetFileAttributesExA ? Real_GetFileAttributesExA(n, lvl, info)
                                     : GetFileAttributesExA(n, lvl, info);
}
static BOOL WINAPI Hook_tc_GetFileAttributesExW(LPCWSTR n, GET_FILEEX_INFO_LEVELS lvl, LPVOID info) {
    if (g_enabled && starts_with_container_w(n)) {
        wchar_t buf[MAX_PATH]; rewrite_w(buf, MAX_PATH, n);
        return Real_GetFileAttributesExW ? Real_GetFileAttributesExW(buf, lvl, info)
                                         : GetFileAttributesExW(buf, lvl, info);
    }
    return Real_GetFileAttributesExW ? Real_GetFileAttributesExW(n, lvl, info)
                                     : GetFileAttributesExW(n, lvl, info);
}
static HANDLE WINAPI Hook_tc_FindFirstFileA(LPCSTR n, LPWIN32_FIND_DATAA fd) {
    if (g_enabled && starts_with_container_a(n)) {
        char buf[MAX_PATH]; rewrite_a(buf, sizeof buf, n);
        return Real_FindFirstFileA ? Real_FindFirstFileA(buf, fd)
                                   : FindFirstFileA(buf, fd);
    }
    return Real_FindFirstFileA ? Real_FindFirstFileA(n, fd) : FindFirstFileA(n, fd);
}
static HANDLE WINAPI Hook_tc_FindFirstFileW(LPCWSTR n, LPWIN32_FIND_DATAW fd) {
    if (g_enabled && starts_with_container_w(n)) {
        wchar_t buf[MAX_PATH]; rewrite_w(buf, MAX_PATH, n);
        return Real_FindFirstFileW ? Real_FindFirstFileW(buf, fd)
                                   : FindFirstFileW(buf, fd);
    }
    return Real_FindFirstFileW ? Real_FindFirstFileW(n, fd) : FindFirstFileW(n, fd);
}
static HANDLE WINAPI Hook_tc_FindFirstFileExA(LPCSTR n, FINDEX_INFO_LEVELS lvl, LPVOID fd,
        FINDEX_SEARCH_OPS ops, LPVOID reserved, DWORD flags) {
    if (g_enabled && starts_with_container_a(n)) {
        char buf[MAX_PATH]; rewrite_a(buf, sizeof buf, n);
        return Real_FindFirstFileExA ? Real_FindFirstFileExA(buf, lvl, fd, ops, reserved, flags)
                                     : FindFirstFileExA(buf, lvl, fd, ops, reserved, flags);
    }
    return Real_FindFirstFileExA ? Real_FindFirstFileExA(n, lvl, fd, ops, reserved, flags)
                                 : FindFirstFileExA(n, lvl, fd, ops, reserved, flags);
}
static HANDLE WINAPI Hook_tc_FindFirstFileExW(LPCWSTR n, FINDEX_INFO_LEVELS lvl, LPVOID fd,
        FINDEX_SEARCH_OPS ops, LPVOID reserved, DWORD flags) {
    if (g_enabled && starts_with_container_w(n)) {
        wchar_t buf[MAX_PATH]; rewrite_w(buf, MAX_PATH, n);
        return Real_FindFirstFileExW ? Real_FindFirstFileExW(buf, lvl, fd, ops, reserved, flags)
                                     : FindFirstFileExW(buf, lvl, fd, ops, reserved, flags);
    }
    return Real_FindFirstFileExW ? Real_FindFirstFileExW(n, lvl, fd, ops, reserved, flags)
                                 : FindFirstFileExW(n, lvl, fd, ops, reserved, flags);
}
static BOOL WINAPI Hook_tc_SetCurrentDirectoryA(LPCSTR n) {
    if (g_enabled && starts_with_container_a(n)) {
        char buf[MAX_PATH]; rewrite_a(buf, sizeof buf, n);
        return Real_SetCurrentDirectoryA ? Real_SetCurrentDirectoryA(buf)
                                         : SetCurrentDirectoryA(buf);
    }
    return Real_SetCurrentDirectoryA ? Real_SetCurrentDirectoryA(n)
                                     : SetCurrentDirectoryA(n);
}
static BOOL WINAPI Hook_tc_SetCurrentDirectoryW(LPCWSTR n) {
    if (g_enabled && starts_with_container_w(n)) {
        wchar_t buf[MAX_PATH]; rewrite_w(buf, MAX_PATH, n);
        return Real_SetCurrentDirectoryW ? Real_SetCurrentDirectoryW(buf)
                                         : SetCurrentDirectoryW(buf);
    }
    return Real_SetCurrentDirectoryW ? Real_SetCurrentDirectoryW(n)
                                     : SetCurrentDirectoryW(n);
}
static BOOL WINAPI Hook_tc_GetDiskFreeSpaceExA(LPCSTR root,
        PULARGE_INTEGER avail, PULARGE_INTEGER total, PULARGE_INTEGER freeT) {
    if (g_enabled && starts_with_container_a(root)) {
        ULONGLONG plausible_free  = (ULONGLONG)10 * 1024 * 1024 * 1024;
        ULONGLONG plausible_total = (ULONGLONG)32 * 1024 * 1024 * 1024;
        if (avail) avail->QuadPart = plausible_free;
        if (total) total->QuadPart = plausible_total;
        if (freeT) freeT->QuadPart = plausible_free;
        return TRUE;
    }
    return Real_GetDiskFreeSpaceExA ? Real_GetDiskFreeSpaceExA(root, avail, total, freeT)
                                    : GetDiskFreeSpaceExA(root, avail, total, freeT);
}
static BOOL WINAPI Hook_tc_GetDiskFreeSpaceExW(LPCWSTR root,
        PULARGE_INTEGER avail, PULARGE_INTEGER total, PULARGE_INTEGER freeT) {
    if (g_enabled && starts_with_container_w(root)) {
        ULONGLONG plausible_free  = (ULONGLONG)10 * 1024 * 1024 * 1024;
        ULONGLONG plausible_total = (ULONGLONG)32 * 1024 * 1024 * 1024;
        if (avail) avail->QuadPart = plausible_free;
        if (total) total->QuadPart = plausible_total;
        if (freeT) freeT->QuadPart = plausible_free;
        return TRUE;
    }
    return Real_GetDiskFreeSpaceExW ? Real_GetDiskFreeSpaceExW(root, avail, total, freeT)
                                    : GetDiskFreeSpaceExW(root, avail, total, freeT);
}
static UINT WINAPI Hook_tc_GetDriveTypeA(LPCSTR root) {
    if (g_enabled && starts_with_container_a(root)) return DRIVE_FIXED;
    return Real_GetDriveTypeA ? Real_GetDriveTypeA(root) : GetDriveTypeA(root);
}
static UINT WINAPI Hook_tc_GetDriveTypeW(LPCWSTR root) {
    if (g_enabled && starts_with_container_w(root)) return DRIVE_FIXED;
    return Real_GetDriveTypeW ? Real_GetDriveTypeW(root) : GetDriveTypeW(root);
}
static BOOL WINAPI Hook_tc_GetVolumeInformationA(LPCSTR root,
        LPSTR name, DWORD name_sz, LPDWORD serial, LPDWORD max_comp,
        LPDWORD fs_flags, LPSTR fs_name, DWORD fs_sz) {
    if (g_enabled && starts_with_container_a(root)) {
        if (name && name_sz) { strncpy(name, "SEGA", name_sz-1); name[name_sz-1] = 0; }
        if (serial)   *serial   = 0xC0DECAFE;
        if (max_comp) *max_comp = 255;
        if (fs_flags) *fs_flags = 0x00000003;
        if (fs_name && fs_sz) { strncpy(fs_name, "NTFS", fs_sz-1); fs_name[fs_sz-1] = 0; }
        return TRUE;
    }
    return Real_GetVolumeInformationA ? Real_GetVolumeInformationA(root, name, name_sz, serial, max_comp, fs_flags, fs_name, fs_sz)
                                      : GetVolumeInformationA(root, name, name_sz, serial, max_comp, fs_flags, fs_name, fs_sz);
}
static BOOL WINAPI Hook_tc_GetVolumeInformationW(LPCWSTR root,
        LPWSTR name, DWORD name_sz, LPDWORD serial, LPDWORD max_comp,
        LPDWORD fs_flags, LPWSTR fs_name, DWORD fs_sz) {
    if (g_enabled && starts_with_container_w(root)) {
        if (name && name_sz >= 5) { wcscpy(name, L"SEGA"); }
        if (serial)   *serial   = 0xC0DECAFE;
        if (max_comp) *max_comp = 255;
        if (fs_flags) *fs_flags = 0x00000003;
        if (fs_name && fs_sz >= 5) { wcscpy(fs_name, L"NTFS"); }
        return TRUE;
    }
    return Real_GetVolumeInformationW ? Real_GetVolumeInformationW(root, name, name_sz, serial, max_comp, fs_flags, fs_name, fs_sz)
                                      : GetVolumeInformationW(root, name, name_sz, serial, max_comp, fs_flags, fs_name, fs_sz);
}

/* No-op mount-tool spawns. Synthesized PROCESS_INFORMATION: use a fresh
 * signaled event as hProcess so WaitForSingleObject completes immediately, and
 * DuplicateHandle to yield a distinct hThread. GetExitCodeProcess on the event
 * returns 0 because events don't own an exit code; the launcher only cares
 * that the wait completes. */
static BOOL WINAPI Hook_tc_CreateProcessA(LPCSTR app, LPSTR cmd,
        LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags,
        LPVOID env, LPCSTR cwd, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi) {
    if (g_enabled && cmdline_is_mount_tool_a(app, cmd)) {
        ul_sn_log("truecrypt: swallowed mount-tool spawn (%s)\n",
                  cmd ? cmd : (app ? app : "?"));
        if (pi) {
            HANDLE ev = CreateEventA(NULL, TRUE, TRUE, NULL);   /* manual-reset, signaled */
            pi->hProcess    = ev ? ev : (HANDLE)(ULONG_PTR)0xDEADBEEF;
            pi->hThread     = ev ? ev : (HANDLE)(ULONG_PTR)0xDEADBEEE;
            pi->dwProcessId = 0xF00D;
            pi->dwThreadId  = 0xF00E;
        }
        return TRUE;
    }
    return Real_CreateProcessA ? Real_CreateProcessA(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi)
                               : CreateProcessA(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
}
static BOOL WINAPI Hook_tc_CreateProcessW(LPCWSTR app, LPWSTR cmd,
        LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags,
        LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    if (g_enabled && cmdline_is_mount_tool_w(app, cmd)) {
        ul_sn_log("truecrypt: swallowed mount-tool spawn (W)\n");
        if (pi) {
            HANDLE ev = CreateEventA(NULL, TRUE, TRUE, NULL);
            pi->hProcess    = ev ? ev : (HANDLE)(ULONG_PTR)0xDEADBEEF;
            pi->hThread     = ev ? ev : (HANDLE)(ULONG_PTR)0xDEADBEEE;
            pi->dwProcessId = 0xF00D;
            pi->dwThreadId  = 0xF00E;
        }
        return TRUE;
    }
    return Real_CreateProcessW ? Real_CreateProcessW(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi)
                               : CreateProcessW(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
}

void ul_sn_install_truecrypt_bypass(void) {
    if (!ul_sn_env_on("ULTRA_SEGA_TRUECRYPT_BYPASS")
        && !ul_sn_env_on("ULTRA_SEGA_TRUECRYPT")) {
        ul_sn_log("truecrypt: skipped (ULTRA_SEGA_TRUECRYPT_BYPASS!=1)\n");
        return;
    }
    const char* tgt = ul_sn_env("ULTRA_SEGA_CONTENT_DIR");
    if (!tgt || !tgt[0]) {
        ul_sn_log("truecrypt: skipped (no ULTRA_SEGA_CONTENT_DIR)\n");
        return;
    }
    if (g_enabled) return;
    g_enabled = 1;
    strncpy(g_target, tgt, sizeof(g_target) - 1);
    g_target[sizeof(g_target) - 1] = 0;
    size_t tn = strlen(g_target);
    while (tn > 0 && (g_target[tn-1] == '\\' || g_target[tn-1] == '/')) {
        g_target[--tn] = 0;
    }
    const char* drv = ul_sn_env("ULTRA_SEGA_CONTENT_DRIVE");
    if (drv && drv[0]) {
        g_drive_letters[0] = drv[0];
        g_drive_letters[1] = 0;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
        Real_CreateFileA           = (pCreateFileA)          (void*)GetProcAddress(k32, "CreateFileA");
        Real_CreateFileW           = (pCreateFileW)          (void*)GetProcAddress(k32, "CreateFileW");
        Real_GetFileAttributesA    = (pGetFileAttributesA)   (void*)GetProcAddress(k32, "GetFileAttributesA");
        Real_GetFileAttributesW    = (pGetFileAttributesW)   (void*)GetProcAddress(k32, "GetFileAttributesW");
        Real_GetFileAttributesExA  = (pGetFileAttributesExA) (void*)GetProcAddress(k32, "GetFileAttributesExA");
        Real_GetFileAttributesExW  = (pGetFileAttributesExW) (void*)GetProcAddress(k32, "GetFileAttributesExW");
        Real_FindFirstFileA        = (pFindFirstFileA)       (void*)GetProcAddress(k32, "FindFirstFileA");
        Real_FindFirstFileW        = (pFindFirstFileW)       (void*)GetProcAddress(k32, "FindFirstFileW");
        Real_FindFirstFileExA      = (pFindFirstFileExA)     (void*)GetProcAddress(k32, "FindFirstFileExA");
        Real_FindFirstFileExW      = (pFindFirstFileExW)     (void*)GetProcAddress(k32, "FindFirstFileExW");
        Real_SetCurrentDirectoryA  = (pSetCurrentDirectoryA) (void*)GetProcAddress(k32, "SetCurrentDirectoryA");
        Real_SetCurrentDirectoryW  = (pSetCurrentDirectoryW) (void*)GetProcAddress(k32, "SetCurrentDirectoryW");
        Real_GetDiskFreeSpaceExA   = (pGetDiskFreeSpaceExA)  (void*)GetProcAddress(k32, "GetDiskFreeSpaceExA");
        Real_GetDiskFreeSpaceExW   = (pGetDiskFreeSpaceExW)  (void*)GetProcAddress(k32, "GetDiskFreeSpaceExW");
        Real_GetDriveTypeA         = (pGetDriveTypeA)        (void*)GetProcAddress(k32, "GetDriveTypeA");
        Real_GetDriveTypeW         = (pGetDriveTypeW)        (void*)GetProcAddress(k32, "GetDriveTypeW");
        Real_GetVolumeInformationA = (pGetVolumeInformationA)(void*)GetProcAddress(k32, "GetVolumeInformationA");
        Real_GetVolumeInformationW = (pGetVolumeInformationW)(void*)GetProcAddress(k32, "GetVolumeInformationW");
        Real_CreateProcessA        = (pCreateProcessA)       (void*)GetProcAddress(k32, "CreateProcessA");
        Real_CreateProcessW        = (pCreateProcessW)       (void*)GetProcAddress(k32, "CreateProcessW");
    }
    ul_sn_iat_patch_all("kernel32.dll", "CreateFileA",           (void*)Hook_tc_CreateFileA);
    ul_sn_iat_patch_all("kernel32.dll", "CreateFileW",           (void*)Hook_tc_CreateFileW);
    ul_sn_iat_patch_all("kernel32.dll", "GetFileAttributesA",    (void*)Hook_tc_GetFileAttributesA);
    ul_sn_iat_patch_all("kernel32.dll", "GetFileAttributesW",    (void*)Hook_tc_GetFileAttributesW);
    ul_sn_iat_patch_all("kernel32.dll", "GetFileAttributesExA",  (void*)Hook_tc_GetFileAttributesExA);
    ul_sn_iat_patch_all("kernel32.dll", "GetFileAttributesExW",  (void*)Hook_tc_GetFileAttributesExW);
    ul_sn_iat_patch_all("kernel32.dll", "FindFirstFileA",        (void*)Hook_tc_FindFirstFileA);
    ul_sn_iat_patch_all("kernel32.dll", "FindFirstFileW",        (void*)Hook_tc_FindFirstFileW);
    ul_sn_iat_patch_all("kernel32.dll", "FindFirstFileExA",      (void*)Hook_tc_FindFirstFileExA);
    ul_sn_iat_patch_all("kernel32.dll", "FindFirstFileExW",      (void*)Hook_tc_FindFirstFileExW);
    ul_sn_iat_patch_all("kernel32.dll", "SetCurrentDirectoryA",  (void*)Hook_tc_SetCurrentDirectoryA);
    ul_sn_iat_patch_all("kernel32.dll", "SetCurrentDirectoryW",  (void*)Hook_tc_SetCurrentDirectoryW);
    ul_sn_iat_patch_all("kernel32.dll", "GetDiskFreeSpaceExA",   (void*)Hook_tc_GetDiskFreeSpaceExA);
    ul_sn_iat_patch_all("kernel32.dll", "GetDiskFreeSpaceExW",   (void*)Hook_tc_GetDiskFreeSpaceExW);
    ul_sn_iat_patch_all("kernel32.dll", "GetDriveTypeA",         (void*)Hook_tc_GetDriveTypeA);
    ul_sn_iat_patch_all("kernel32.dll", "GetDriveTypeW",         (void*)Hook_tc_GetDriveTypeW);
    ul_sn_iat_patch_all("kernel32.dll", "GetVolumeInformationA", (void*)Hook_tc_GetVolumeInformationA);
    ul_sn_iat_patch_all("kernel32.dll", "GetVolumeInformationW", (void*)Hook_tc_GetVolumeInformationW);
    ul_sn_iat_patch_all("kernel32.dll", "CreateProcessA",        (void*)Hook_tc_CreateProcessA);
    ul_sn_iat_patch_all("kernel32.dll", "CreateProcessW",        (void*)Hook_tc_CreateProcessW);
    ul_sn_log("truecrypt: armed (redirect %s:\\ -> %s)\n", g_drive_letters, g_target);
}

#endif /* _WIN32 */
