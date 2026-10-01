/* SPDX-License-Identifier: MIT
 * typex_test_launcher.c -- bounded first-contact test: launch a 32-bit Type X
 * game SUSPENDED, inject the clean-room I/O hook (same-bitness), resume, run for
 * N seconds, then terminate. Env (ULTRA_JVS_LOG, ULTRA_DISMISS_DIALOGS, ...) is
 * inherited by the game so the injected hook picks it up.
 *   usage: typex_test_launcher <game.exe> <hook.dll> [seconds]
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <game.exe> <hook.dll> [seconds]\n", argv[0]); return 2; }
    const char* exe = argv[1];
    const char* dll = argv[2];
    int secs = (argc > 3) ? atoi(argv[3]) : 12;

    char dir[1024]; strncpy(dir, exe, sizeof dir - 1); dir[sizeof dir - 1] = 0;
    char* s = strrchr(dir, '\\'); if (!s) s = strrchr(dir, '/');
    if (s) *s = 0; else strcpy(dir, ".");

    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
    char cmd[1100]; snprintf(cmd, sizeof cmd, "\"%s\"", exe);

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, dir, &si, &pi)) {
        fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError()); return 1;
    }
    printf("launched pid %lu (suspended): %s\n", pi.dwProcessId, exe);

    size_t n = strlen(dll) + 1;
    void* rem = VirtualAllocEx(pi.hProcess, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int injected = 0;
    if (rem && WriteProcessMemory(pi.hProcess, rem, dll, n, NULL)) {
        HMODULE k = GetModuleHandleA("kernel32.dll");
        LPTHREAD_START_ROUTINE ll = (LPTHREAD_START_ROUTINE)(void*)GetProcAddress(k, "LoadLibraryA");
        HANDLE th = ll ? CreateRemoteThread(pi.hProcess, NULL, 0, ll, rem, 0, NULL) : NULL;
        if (th) {
            WaitForSingleObject(th, 10000);
            DWORD rc = 0; GetExitCodeThread(th, &rc);
            printf("inject: remote LoadLibraryA returned 0x%lX (%s)\n", rc, rc ? "loaded" : "FAILED");
            injected = (rc != 0);
            CloseHandle(th);
        } else printf("inject: CreateRemoteThread failed %lu\n", GetLastError());
    } else printf("inject: alloc/write failed %lu\n", GetLastError());

    ResumeThread(pi.hThread);
    printf("resumed; running for %ds (hook %sinjected)...\n", secs, injected ? "" : "NOT ");

    DWORD w = WaitForSingleObject(pi.hProcess, (DWORD)secs * 1000);
    if (w == WAIT_TIMEOUT) {
        printf("still running after %ds -> terminating\n", secs);
        TerminateProcess(pi.hProcess, 0);
    } else {
        DWORD ec = 0; GetExitCodeProcess(pi.hProcess, &ec);
        printf("game EXITED on its own, code=%lu (0x%lX) after <%ds\n", ec, ec, secs);
    }
    if (rem) VirtualFreeEx(pi.hProcess, rem, 0, MEM_RELEASE);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return 0;
}
