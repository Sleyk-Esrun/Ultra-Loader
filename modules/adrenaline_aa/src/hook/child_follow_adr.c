/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  child_follow_adr.c -- keep the board hook present across a self-relaunching
 *                        Adrenaline Amusements launcher (GameLauncher.exe)
 * =============================================================================
 *
 *  WHY
 *  ---
 *  GameLauncher.exe (the Adrenaline .NET launcher/watchdog for Tomb Raider et al)
 *  bootstraps and then spawns a fresh GameLauncher.exe instance for the real
 *  pre-flight UI (the "Adrenaline logo" screen that does the FE Adrio serial
 *  I/O-board handshake). The loader injects the FIRST (bootstrap) instance at
 *  CREATE_SUSPENDED, but that instance exits after spawning the child, so the
 *  child -- the one that actually opens the serial port -- runs with no hook.
 *  With no hook faking the I/O board, it waits forever at the logo.
 *
 *  Late poll-injection into the running child does not help: by the time a
 *  poll injector loads the DLL, the child has already opened the COM port and
 *  blocked on a read; a serial hook installed after that read is in flight
 *  cannot retroactively complete it (observed: the child stays frozen).
 *
 *  FIX
 *  ---
 *  Follow the child at creation. This hook (already inside the bootstrap
 *  instance, courtesy of the loader) intercepts CreateProcessW/A. When the
 *  process being spawned is another GameLauncher.exe, it is created
 *  CREATE_SUSPENDED, this same hook DLL is injected before the child runs a
 *  single instruction, and then the child's main thread is resumed (unless the
 *  caller itself asked for a suspended start). The board hook is therefore
 *  present from instruction zero of every relaunched instance -- its serial
 *  hooks are installed before the child ever opens the COM port.
 *
 *  Only GameLauncher.exe children are followed. Game.exe (the 64-bit Unity
 *  process) is deliberately NOT followed here: it is a different bitness (a
 *  32-bit parent cannot inject a 64-bit child anyway) and it receives its mods
 *  the file-based way (UnityDoorstop winhttp.dll + on-disk managed patches),
 *  so it needs nothing injected.
 * =============================================================================
 */
#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <string.h>
#include <wchar.h>

extern void ultra_adr_log(const char* fmt, ...);
extern void ultra_iat_patch_all_adr(const char* impdll, const char* impfn, void* newfn);

typedef BOOL (WINAPI *pCreateProcessW)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
        LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
typedef BOOL (WINAPI *pCreateProcessA)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES,
        LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
typedef BOOL (WINAPI *pShellExecuteExW)(SHELLEXECUTEINFOW*);

static pCreateProcessW  Real_CreateProcessW;
static pCreateProcessA  Real_CreateProcessA;
static pShellExecuteExW Real_ShellExecuteExW;

/* This hook DLL's own on-disk path -- what we load into each followed child. */
static char g_self_path[MAX_PATH];

static void resolve_self_path(void) {
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&resolve_self_path, &self);
    if (self) GetModuleFileNameA(self, g_self_path, sizeof g_self_path);
}

/* case-insensitive "does this string contain GameLauncher.exe" -- ASCII */
static int has_gl_a(const char* s) {
    static const char* needle = "gamelauncher.exe";
    if (!s) return 0;
    for (; *s; ++s) {
        const char* a = s; const char* b = needle;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (ca != cb) break;
            ++a; ++b;
        }
        if (!*b) return 1;
    }
    return 0;
}

/* case-insensitive "contains GameLauncher.exe" -- wide */
static int has_gl_w(const wchar_t* s) {
    static const wchar_t* needle = L"gamelauncher.exe";
    if (!s) return 0;
    for (; *s; ++s) {
        const wchar_t* a = s; const wchar_t* b = needle;
        while (*a && *b) {
            wchar_t ca = *a, cb = *b;
            if (ca >= L'A' && ca <= L'Z') ca += 32;
            if (ca != cb) break;
            ++a; ++b;
        }
        if (!*b) return 1;
    }
    return 0;
}

/* Load this hook DLL into a (suspended) child via a remote LoadLibraryA thread.
 * The remote thread runs even while the child's MAIN thread stays suspended, so
 * DllMain -- and therefore the child's own serial hooks -- installs before the
 * child executes any of its own code. */
static void inject_self_into(HANDLE hProc) {
    if (!g_self_path[0]) { ultra_adr_log("child-follow: self path unknown, cannot inject child"); return; }
    size_t n = strlen(g_self_path) + 1;
    LPVOID rbuf = VirtualAllocEx(hProc, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!rbuf) { ultra_adr_log("child-follow: VirtualAllocEx failed (%lu)", GetLastError()); return; }
    SIZE_T wr = 0;
    if (WriteProcessMemory(hProc, rbuf, g_self_path, n, &wr) && wr == n) {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        LPTHREAD_START_ROUTINE ll = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryA");
        HANDLE th = ll ? CreateRemoteThread(hProc, NULL, 0, ll, rbuf, 0, NULL) : NULL;
        if (th) {
            WaitForSingleObject(th, 10000);
            DWORD ec = 0; GetExitCodeThread(th, &ec);
            ultra_adr_log("child-follow: injected hook into child -> LoadLibraryA=0x%08lX %s",
                          ec, ec ? "(ok)" : "(NULL!)");
            CloseHandle(th);
        } else {
            ultra_adr_log("child-follow: CreateRemoteThread failed (%lu)", GetLastError());
        }
    } else {
        ultra_adr_log("child-follow: WriteProcessMemory failed (%lu)", GetLastError());
    }
    VirtualFreeEx(hProc, rbuf, 0, MEM_RELEASE);
}

/* Non-static + ultra_adr_ prefix: the GetProcAddress shim in
 * jvs_serial_hook_adr.c hands these back for dynamically-resolved calls (the
 * .NET CLR caches CreateProcessW/ShellExecuteExW pointers, bypassing the IAT). */
BOOL WINAPI ultra_adr_Hook_CreateProcessW(LPCWSTR app, LPWSTR cmd,
        LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inh, DWORD flags,
        LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    int follow = has_gl_w(app) || has_gl_w(cmd);
    DWORD want_suspended = flags & CREATE_SUSPENDED;
    DWORD f = follow ? (flags | CREATE_SUSPENDED) : flags;
    BOOL ok = Real_CreateProcessW ?
        Real_CreateProcessW(app, cmd, pa, ta, inh, f, env, dir, si, pi) :
        CreateProcessW(app, cmd, pa, ta, inh, f, env, dir, si, pi);
    if (ok && follow && pi && pi->hProcess) {
        ultra_adr_log("child-follow: GameLauncher child pid=%lu spawned (CreateProcessW) -> following", pi->dwProcessId);
        inject_self_into(pi->hProcess);
        if (!want_suspended && pi->hThread) ResumeThread(pi->hThread);
    }
    return ok;
}

BOOL WINAPI ultra_adr_Hook_CreateProcessA(LPCSTR app, LPSTR cmd,
        LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inh, DWORD flags,
        LPVOID env, LPCSTR dir, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi) {
    int follow = has_gl_a(app) || has_gl_a(cmd);
    DWORD want_suspended = flags & CREATE_SUSPENDED;
    DWORD f = follow ? (flags | CREATE_SUSPENDED) : flags;
    BOOL ok = Real_CreateProcessA ?
        Real_CreateProcessA(app, cmd, pa, ta, inh, f, env, dir, si, pi) :
        CreateProcessA(app, cmd, pa, ta, inh, f, env, dir, si, pi);
    if (ok && follow && pi && pi->hProcess) {
        ultra_adr_log("child-follow: GameLauncher child pid=%lu spawned (CreateProcessA) -> following", pi->dwProcessId);
        inject_self_into(pi->hProcess);
        if (!want_suspended && pi->hThread) ResumeThread(pi->hThread);
    }
    return ok;
}

/* .NET Framework Process.Start defaults to UseShellExecute=true -> the launcher
 * relaunches itself through ShellExecuteExW, which our CreateProcess hooks never
 * see. Intercept it: for a GameLauncher.exe target, spawn it ourselves suspended
 * (via CreateProcessW), inject, resume -- so the relaunched instance is hooked
 * from instruction zero just like the CreateProcess path. */
BOOL WINAPI ultra_adr_Hook_ShellExecuteExW(void* pv) {
    SHELLEXECUTEINFOW* ei = (SHELLEXECUTEINFOW*)pv;
    if (ei && ei->lpFile && has_gl_w(ei->lpFile)) {
        /* Build "\"file\" params" command line. */
        static wchar_t cmd[1024];
        const wchar_t* params = ei->lpParameters ? ei->lpParameters : L"";
        _snwprintf(cmd, 1024, L"\"%s\" %s", ei->lpFile, params);
        cmd[1023] = 0;

        STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
        if (ei->fMask & SEE_MASK_NO_CONSOLE) {}
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = (WORD)ei->nShow;
        PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);

        BOOL ok = (Real_CreateProcessW ? Real_CreateProcessW : CreateProcessW)(
            NULL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL,
            ei->lpDirectory, &si, &pi);
        if (ok) {
            ultra_adr_log("child-follow: GameLauncher child pid=%lu spawned (ShellExecuteExW) -> following", pi.dwProcessId);
            inject_self_into(pi.hProcess);
            ResumeThread(pi.hThread);
            if (ei->fMask & SEE_MASK_NOCLOSEPROCESS) ei->hProcess = pi.hProcess;
            else CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            ei->hInstApp = (HINSTANCE)42;   /* > 32 == success per ShellExecute contract */
            return TRUE;
        }
        ultra_adr_log("child-follow: ShellExecuteExW self-spawn FAILED (%lu) -- falling through to real", GetLastError());
    }
    return Real_ShellExecuteExW ? Real_ShellExecuteExW(ei) : ShellExecuteExW(ei);
}

void ultra_child_follow_install_adr(void) {
    resolve_self_path();
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
        Real_CreateProcessW = (pCreateProcessW)GetProcAddress(k32, "CreateProcessW");
        Real_CreateProcessA = (pCreateProcessA)GetProcAddress(k32, "CreateProcessA");
    }
    HMODULE sh = LoadLibraryA("shell32.dll");
    if (sh) Real_ShellExecuteExW = (pShellExecuteExW)GetProcAddress(sh, "ShellExecuteExW");

    ultra_iat_patch_all_adr("kernel32.dll", "CreateProcessW", (void*)ultra_adr_Hook_CreateProcessW);
    ultra_iat_patch_all_adr("kernel32.dll", "CreateProcessA", (void*)ultra_adr_Hook_CreateProcessA);
    ultra_iat_patch_all_adr("shell32.dll",  "ShellExecuteExW", (void*)ultra_adr_Hook_ShellExecuteExW);
    ultra_adr_log("child-follow: CreateProcess+ShellExecuteEx hooks armed (self=%s)",
                  g_self_path[0] ? g_self_path : "?");
}

#endif /* _WIN32 */
