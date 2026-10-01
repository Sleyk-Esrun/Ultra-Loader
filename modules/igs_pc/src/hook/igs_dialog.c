/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  igs_dialog.c -- MessageBox auto-dismiss for the IGS-PC-only hook DLL
 * =============================================================================
 *
 *  PROVENANCE: forked from cave_pc/src/hook/cave_dialog.c (itself trimmed from
 *  modules/native_x86/src/hook/dongle_bypass.c's MessageBoxA/W auto-dismiss
 *  mechanism) into IGS PC's own independent hook tree. See igs_hook.h for the
 *  full rationale.
 *
 *  PC-arcade games commonly gate behind hardware-error dialogs (e.g. a
 *  missing/failed dongle or I/O-board presence check) that would otherwise sit
 *  as a blocking MessageBox over the render surface. Off by default;
 *  ULTRA_DISMISS_DIALOGS=1 (set unconditionally by igs_pc_main.c) enables it.
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 API.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>

#include "igs_hook.h"

typedef int (WINAPI *pMessageBoxA)(HWND,LPCSTR,LPCSTR,UINT);
typedef int (WINAPI *pMessageBoxW)(HWND,LPCWSTR,LPCWSTR,UINT);
static pMessageBoxA Real_MessageBoxA;
static pMessageBoxW Real_MessageBoxW;

static int dialogs_dismissed(void) {
    const char* e = getenv("ULTRA_DISMISS_DIALOGS");
    return e && e[0] == '1';
}
static int default_button(UINT type) {
    /* Answer so the game gets PAST a blocking error and keeps running: dismiss
     * (don't retry forever, don't abort, don't dive into a test menu). */
    switch (type & 0xF) {
        case MB_OK:                return IDOK;
        case MB_OKCANCEL:          return IDCANCEL;
        case MB_YESNO:             return IDNO;
        case MB_YESNOCANCEL:       return IDNO;
        case MB_RETRYCANCEL:       return IDCANCEL;
        case MB_ABORTRETRYIGNORE:  return IDIGNORE;
        case MB_CANCELTRYCONTINUE: return IDCONTINUE;
        default:                   return IDOK;
    }
}
static int WINAPI Hook_MessageBoxA(HWND w, LPCSTR t, LPCSTR c, UINT type) {
    igs_log("MESSAGEBOX title=\"%s\" text=\"%s\"", c ? c : "", t ? t : "");
    if (dialogs_dismissed()) return default_button(type);
    return Real_MessageBoxA(w, t, c, type);
}
static int WINAPI Hook_MessageBoxW(HWND w, LPCWSTR t, LPCWSTR c, UINT type) {
    char tt[256] = {0}, cc[256] = {0};
    if (t) WideCharToMultiByte(CP_ACP, 0, t, -1, tt, sizeof(tt)-1, NULL, NULL);
    if (c) WideCharToMultiByte(CP_ACP, 0, c, -1, cc, sizeof(cc)-1, NULL, NULL);
    igs_log("MESSAGEBOX title=\"%s\" text=\"%s\"", cc, tt);
    if (dialogs_dismissed()) return default_button(type);
    return Real_MessageBoxW(w, t, c, type);
}

void igs_dialog_install(void) {
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (!u32) return;
    Real_MessageBoxA = (pMessageBoxA)GetProcAddress(u32, "MessageBoxA");
    Real_MessageBoxW = (pMessageBoxW)GetProcAddress(u32, "MessageBoxW");
    if (Real_MessageBoxA) igs_iat_patch_all("user32.dll", "MessageBoxA", (void*)Hook_MessageBoxA);
    if (Real_MessageBoxW) igs_iat_patch_all("user32.dll", "MessageBoxW", (void*)Hook_MessageBoxW);
}

#endif /* _WIN32 */
