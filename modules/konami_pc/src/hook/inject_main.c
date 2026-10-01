/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  inject_main.c  --  ultra-inject{32,64}.exe : standalone LoadLibrary injector
 * =============================================================================
 *
 *  A tiny same-bitness DLL injector. The native_x86 module runs inside the
 *  64-bit module host; most Type X / PC-arcade games are 32-bit, so the host
 *  spawns the bitness-matched copy of this helper to inject the I/O hook (a
 *  64-bit process cannot reliably CreateRemoteThread+LoadLibrary into a 32-bit
 *  one). Built twice: ultra-inject32.exe (x86) and ultra-inject64.exe (x64).
 *
 *      usage:  ultra-inject <pid> <dllpath>
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 API.
 * =============================================================================
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boot_console.h"   /* shared Ultra Loader boot console (path B) */

/* Show the UNIFIED boot console before a suspended-launch spawn (path B): if the
 * parent (the native_x86 module, path A) already drew one it sets
 * ULTRA_BOOT_CONSOLE_SHOWN, so we skip to avoid a double screen. The board SYSTEM
 * + game TITLE are passed down via ULTRA_BOOT_SYSTEM / ULTRA_BOOT_TITLE; the asset
 * dir (Orbitron.ttf / jingle.wav) is this injector's own exe folder. Honors
 * ULTRA_NO_SPLASH (also checked inside ultra_boot_console_run). */
static void inject_show_boot_console(void) {
    if (getenv("ULTRA_BOOT_CONSOLE_SHOWN")) return;
    if (getenv("ULTRA_NO_SPLASH")) return;
    const char *sys   = getenv("ULTRA_BOOT_SYSTEM");
    const char *title = getenv("ULTRA_BOOT_TITLE");
    char dir[MAX_PATH]; GetModuleFileNameA(NULL, dir, MAX_PATH);
    { char *sl = strrchr(dir, '\\'); if (sl) *sl = 0; }
    UltraBootIdentity bid = {
        (sys && *sys) ? sys : "Arcade",
        (title && *title) ? title : "the cabinet",
        NULL, NULL, NULL, 0, 0
    };
    ultra_boot_console_run(&bid, dir, NULL, NULL);
    SetEnvironmentVariableA("ULTRA_BOOT_CONSOLE_SHOWN", "1");
}

/* Inject `dll` into the already-open process handle `proc` via a remote
 * LoadLibraryA thread. Returns 0 on success. Does not close `proc`. */
static int inject_into(HANDLE proc, const char* dll) {
    size_t n = strlen(dll) + 1;
    void* rem = VirtualAllocEx(proc, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!rem) { fprintf(stderr, "VirtualAllocEx failed: %lu\n", GetLastError()); return 1; }
    if (!WriteProcessMemory(proc, rem, dll, n, NULL)) {
        fprintf(stderr, "WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, rem, 0, MEM_RELEASE); return 1;
    }
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    LPTHREAD_START_ROUTINE fn = (LPTHREAD_START_ROUTINE)(void*)GetProcAddress(k32, "LoadLibraryA");
    HANDLE th = fn ? CreateRemoteThread(proc, NULL, 0, fn, rem, 0, NULL) : NULL;
    if (!th) {
        fprintf(stderr, "CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, rem, 0, MEM_RELEASE); return 1;
    }
    /* A 15s TIMEOUT means the remote LoadLibraryA never returned (its DllMain
     * blocked). On timeout GetExitCodeThread reads STILL_ACTIVE (259), which is
     * non-zero -- reporting that as success would resume the game with the hook
     * only half-attached (the race the --spawn path exists to prevent). Treat any
     * non-signaled wait, and an exit code still == STILL_ACTIVE, as failure.
     * The `rem` page may still be read by the live remote thread, so DON'T free it
     * on the timeout path -- leak it deliberately and let the caller terminate. */
    DWORD w = WaitForSingleObject(th, 15000);
    if (w != WAIT_OBJECT_0) {
        fprintf(stderr, "remote LoadLibraryA did not complete (wait=0x%lX); injection failed\n", w);
        CloseHandle(th);
        return 1;                              /* leak rem: a live thread may use it */
    }
    DWORD remote_hmodule = 0;
    GetExitCodeThread(th, &remote_hmodule);
    CloseHandle(th);
    if (remote_hmodule == 0 || remote_hmodule == STILL_ACTIVE) {
        fprintf(stderr, "remote LoadLibraryA returned %s (dll failed to load)\n",
                remote_hmodule == STILL_ACTIVE ? "STILL_ACTIVE" : "NULL");
        return 1;                              /* leak rem: thread state uncertain */
    }
    VirtualFreeEx(proc, rem, 0, MEM_RELEASE);  /* safe: thread finished */
    return 0;
}

/* Suspended-launch injection: create <exe> SUSPENDED, inject <dll> so the hook's
 * DllMain (and its GetProcAddress interception) is in place before ANY game code
 * runs, then resume. This is the only reliable path for a PACKED game that
 * resolves Direct3DCreate9 dynamically during early startup -- a post-launch
 * LoadLibrary inject can lose the race. Env vars set by the parent are inherited.
 *   usage:  ultra-inject --spawn <exe> <dllpath> [workdir] */
static int spawn_inject(const char* exe, const char* dll, const char* workdir) {
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    /* UNIFIED boot console (path B) before we spawn the game cabinet. */
    inject_show_boot_console();
    /* CreateProcess wants a mutable command line */
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\"", exe);
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED,
                        NULL, (workdir && *workdir) ? workdir : NULL, &si, &pi)) {
        fprintf(stderr, "CreateProcess(%s) failed: %lu\n", exe, GetLastError());
        return 1;
    }
    fprintf(stderr, "spawned (suspended) pid=%lu\n", pi.dwProcessId);
    int rc = inject_into(pi.hProcess, dll);
    if (rc != 0) {
        fprintf(stderr, "inject failed; terminating spawned process\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return rc;
    }
    ResumeThread(pi.hThread);
    printf("ultra-inject: spawned+injected %s into pid %lu (resumed)\n", exe, pi.dwProcessId);
    /* print PID on its own line so a wrapper can capture it */
    printf("PID=%lu\n", pi.dwProcessId);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 4 && strcmp(argv[1], "--spawn") == 0)
        return spawn_inject(argv[2], argv[3], (argc >= 5) ? argv[4] : NULL);

    if (argc < 3) {
        fprintf(stderr, "usage: %s <pid> <dllpath>\n", argv[0]);
        fprintf(stderr, "       %s --spawn <exe> <dllpath> [workdir]\n", argv[0]);
        return 2;
    }
    DWORD pid = (DWORD)strtoul(argv[1], NULL, 0);
    const char* dll = argv[2];

    HANDLE proc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc) { fprintf(stderr, "OpenProcess(%lu) failed: %lu\n", pid, GetLastError()); return 1; }

    size_t n = strlen(dll) + 1;
    void* rem = VirtualAllocEx(proc, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!rem) { fprintf(stderr, "VirtualAllocEx failed: %lu\n", GetLastError()); CloseHandle(proc); return 1; }

    if (!WriteProcessMemory(proc, rem, dll, n, NULL)) {
        fprintf(stderr, "WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, rem, 0, MEM_RELEASE); CloseHandle(proc); return 1;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    LPTHREAD_START_ROUTINE fn = (LPTHREAD_START_ROUTINE)(void*)GetProcAddress(k32, "LoadLibraryA");
    HANDLE th = fn ? CreateRemoteThread(proc, NULL, 0, fn, rem, 0, NULL) : NULL;
    if (!th) {
        fprintf(stderr, "CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, rem, 0, MEM_RELEASE); CloseHandle(proc); return 1;
    }

    /* Same timeout/STILL_ACTIVE guard as inject_into(): a hung remote LoadLibraryA
     * leaves GetExitCodeThread reporting STILL_ACTIVE (259, non-zero), which must
     * NOT be mistaken for a loaded HMODULE. On a non-signaled wait the `rem` page
     * may still be referenced by the live thread, so leak it rather than free it. */
    DWORD w = WaitForSingleObject(th, 15000);
    if (w != WAIT_OBJECT_0) {
        fprintf(stderr, "remote LoadLibraryA did not complete (wait=0x%lX); injection failed\n", w);
        CloseHandle(th); CloseHandle(proc);
        return 1;                              /* leak rem: a live thread may use it */
    }
    DWORD remote_hmodule = 0;
    GetExitCodeThread(th, &remote_hmodule);   /* low 32 bits of the loaded HMODULE */
    CloseHandle(th);

    if (remote_hmodule == 0 || remote_hmodule == STILL_ACTIVE) {
        fprintf(stderr, "remote LoadLibraryA returned %s (dll failed to load)\n",
                remote_hmodule == STILL_ACTIVE ? "STILL_ACTIVE" : "NULL");
        CloseHandle(proc);
        return 1;                              /* leak rem: thread state uncertain */
    }
    VirtualFreeEx(proc, rem, 0, MEM_RELEASE);  /* safe: thread finished */
    CloseHandle(proc);
    printf("ultra-inject: loaded %s into pid %lu\n", dll, pid);
    return 0;
}
