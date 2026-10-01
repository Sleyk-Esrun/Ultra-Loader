/*
 * incredible_technologies_main.c -- entry-point + spawner+injector for the per-board Incredible Technologies (Golden Tee PC) module.
 *
 * The loader's dispatcher (work/src/main.c) calls ul_incredible_technologies_run(argc, argv)
 * after resolving a Incredible Technologies (Golden Tee PC) .exe game to board="IT_PCCORE" via the unified
 * board profile.
 *
 * argv layout:
 *   argv[0] = ultra-loader.exe path
 *   argv[1] = game.exe path
 *
 * PATTERN: mirrors sega_nu_main.c / rawthrills_pc_main.c (medium template):
 *   1. Sniff game PE bitness (32 vs 64) to pick matching hook DLL.
 *   2. Search for a board-native hook (ultra_incredible_technologies_hook{32,64}.dll); fall
 *      back to shared Type X hook (ultra_typex_hook{32,64}.dll). Both accept
 *      ULTRA_BOARD_HINT=IT_PCCORE and branch per-board internally. Fallback is
 *      byte-safe because board-specific stubs are env-gated (see notes in
 *      per-module dict).
 *   3. Set ULTRA_LAUNCH_MODULE / ULTRA_BOARD_HINT / ULTRA_SYSTEM_ID = incredible_technologies
 *      + board-specific ULTRA_* subsystem flags (per-module override below).
 *   4. CreateProcessA(..., CREATE_SUSPENDED, workdir=dir(game), bInheritHandles=TRUE)
 *      so hook stderr flows into ultra.log.
 *   5. Primary inject: QueueUserAPC(LoadLibraryA) on suspended thread.
 *   6. Fallback inject: ResumeThread + Sleep(750) + CreateRemoteThread(LoadLibraryA).
 *   7. WaitForSingleObject on game, propagate exit code.
 *
 * BOARD-SPECIFIC ENV VARS: see the per-module BOARD_ENV block below. When the
 * board_profile.c entry for "IT_PCCORE" also sets the same var via board_profile_arm(),
 * that path uses set_if_unset semantics so a user override always wins.
 *
 * UNTESTED status: this module has never been playtested against a real game
 * dump on real hardware; see UNTESTED.txt in this folder.
 */

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

/* Preferred board-native hook DLLs; fall back to the shared Type X hook. Both
 * bitnesses are selected at runtime from the game's PE header. */
#define LOG(...) do { fprintf(stderr, "[incredible_technologies] " __VA_ARGS__); fflush(stderr); } while (0)

/* This board's hook is no longer installed as a standalone file. It's
 * embedded as an RCDATA resource ("HOOK32"/"HOOK64") inside THIS DLL (see
 * hook_res.rc, baked in by build_module.sh) and extracted to a fresh temp
 * file right before each injection. This means signing/verifying this one
 * DLL automatically covers the hook too, and there's no separate hook file
 * sitting on disk between launches for anything to tamper with -- see
 * docs/PROJECT_LICENSING_AND_SIGNING.md's hook-DLL note (2026-08-02).
 * Caller must DeleteFileA() the returned path once done with it. */
static int extract_hook_to_temp(const char *res_name, char *out, size_t cap)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)&extract_hook_to_temp, &self);
    if (!self) { LOG("GetModuleHandleEx(self) failed: %lu\n", GetLastError()); return 0; }

    HRSRC info = FindResourceA(self, res_name, RT_RCDATA);
    if (!info) { LOG("embedded hook resource \"%s\" not found\n", res_name); return 0; }
    DWORD sz = SizeofResource(self, info);
    HGLOBAL loaded = LoadResource(self, info);
    const void *ptr = loaded ? LockResource(loaded) : NULL;
    if (!ptr || !sz) { LOG("embedded hook resource \"%s\" empty\n", res_name); return 0; }

    /* WHERE THE HOOK GETS EXTRACTED TO -- %TEMP% IS THE FIRST CHOICE, NOT THE
     * ONLY ONE.  The old code called GetTempPathA() once and returned 0 if the
     * write failed, which surfaced to the user as "hook could not be extracted
     * ... the module may be corrupt".  That diagnosis is wrong for both ways
     * this actually fails:
     *   1. Launched from a shell with no TMP/TEMP (an MSYS/git-bash harness, a
     *      service, a scheduler), GetTempPathA falls all the way back to
     *      C:\WINDOWS -- not writable -- so every launch died right there.
     *   2. Writing a DLL into %TEMP% and injecting it moments later is the
     *      exact shape of a dropper, so a behavior shield can veto that one
     *      write and nothing else on the machine.
     * Try several directories in order, and if they all refuse, report WHICH
     * ones were tried instead of blaming the module file. */
    char tmp_dir[MAX_PATH];
    FILE *fp = NULL;
    {
        char cands[4][MAX_PATH];
        char base[MAX_PATH];
        char why[600];
        int  ncand = 0, ci;
        DWORD n;

        why[0] = 0;

        n = GetTempPathA(MAX_PATH, cands[ncand]);
        if (n > 0 && n < MAX_PATH) ncand++;

        /* Ours, always writable, and far less likely than the shared %TEMP%
         * to be treated as a drop site. */
        n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
        if (n > 0 && n < MAX_PATH - 24) {
            snprintf(cands[ncand], MAX_PATH, "%s\\Ultra Loader", base);
            CreateDirectoryA(cands[ncand], NULL);
            snprintf(cands[ncand], MAX_PATH, "%s\\Ultra Loader\\tmp", base);
            CreateDirectoryA(cands[ncand], NULL);
            snprintf(cands[ncand], MAX_PATH, "%s\\Ultra Loader\\tmp\\", base);
            ncand++;
        }

        /* The loader's own working directory. */
        n = GetCurrentDirectoryA(MAX_PATH, base);
        if (n > 0 && n < MAX_PATH - 2) {
            snprintf(cands[ncand], MAX_PATH, "%s%s", base,
                     (base[n - 1] == '\\' || base[n - 1] == '/') ? "" : "\\");
            ncand++;
        }

        n = GetEnvironmentVariableA("USERPROFILE", base, MAX_PATH);
        if (n > 0 && n < MAX_PATH - 2) {
            snprintf(cands[ncand], MAX_PATH, "%s\\", base);
            ncand++;
        }

        for (ci = 0; ci < ncand; ci++) {
            size_t wl;
            memcpy(tmp_dir, cands[ci], MAX_PATH);
            snprintf(out, cap, "%s%s_%lu.dll", tmp_dir, res_name, GetCurrentProcessId());
            fp = fopen(out, "wb");
            if (fp) break;
            wl = strlen(why);
            snprintf(why + wl, sizeof why - wl, "%s%s", wl ? ", " : "", out);
        }
        /* Nothing took it.  Put the candidate list where the caller's existing
         * error message will print it -- "out" only means a path on success. */
        if (!fp) snprintf(out, cap, "%s", why[0] ? why : "(no candidate directory)");
    }
    if (!fp) { LOG("cannot write temp hook %s\n", out); return 0; }
    size_t wrote = fwrite(ptr, 1, sz, fp);
    fclose(fp);
    if (wrote != sz) { LOG("short write to temp hook %s\n", out); DeleteFileA(out); return 0; }
    LOG("extracted embedded hook (%s, %lu bytes) -> %s\n", res_name, sz, out);
    return 1;
}

/* Peek the game's PE header; returns 32 for PE32, 64 for PE32+/error. */
static int game_pe_bits(const char *exe)
{
    HANDLE fh = CreateFileA(exe, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return 32;
    IMAGE_DOS_HEADER dos; DWORD got = 0;
    if (!ReadFile(fh, &dos, sizeof dos, &got, NULL) || got != sizeof dos
        || dos.e_magic != IMAGE_DOS_SIGNATURE) { CloseHandle(fh); return 32; }
    if (SetFilePointer(fh, dos.e_lfanew, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        CloseHandle(fh); return 32;
    }
    DWORD sig = 0;
    IMAGE_FILE_HEADER fhdr;
    if (!ReadFile(fh, &sig, sizeof sig, &got, NULL) || sig != IMAGE_NT_SIGNATURE
        || !ReadFile(fh, &fhdr, sizeof fhdr, &got, NULL) || got != sizeof fhdr) {
        CloseHandle(fh); return 32;
    }
    CloseHandle(fh);
    if (fhdr.Machine == IMAGE_FILE_MACHINE_I386) return 32;
    return 64;
}

static int find_hook(char *out, size_t cap, int bits)
{
    const char *res = (bits == 32) ? "HOOK32" : "HOOK64";
    if (extract_hook_to_temp(res, out, cap)) {
        LOG("using board-native (embedded) hook: %s\n", res);
        return 1;
    }
    return 0;
}

/* Primary inject: QueueUserAPC(LoadLibraryA) on suspended thread. */
static int inject_dll_apc(HANDLE proc, HANDLE thread, const char *dll_path)
{
    size_t n = strlen(dll_path) + 1;
    LPVOID remote = VirtualAllocEx(proc, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { LOG("VirtualAllocEx failed: %lu\n", GetLastError()); return 0; }
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(proc, remote, dll_path, n, &wrote) || wrote != n) {
        LOG("WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    HMODULE kern = GetModuleHandleA("kernel32.dll");
    PAPCFUNC loadfn = (PAPCFUNC)(uintptr_t)GetProcAddress(kern, "LoadLibraryA");
    if (!loadfn) {
        LOG("GetProcAddress(LoadLibraryA) failed\n");
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    if (!QueueUserAPC(loadfn, thread, (ULONG_PTR)remote)) {
        LOG("QueueUserAPC failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    LOG("APC queued for LoadLibraryA(%s) remote=%p\n", dll_path, remote);
    return 1;
}

/* Fallback inject: CreateRemoteThread(LoadLibraryA) after resume. */
static int inject_dll_remote_thread(HANDLE proc, const char *dll_path)
{
    size_t n = strlen(dll_path) + 1;
    LPVOID remote = VirtualAllocEx(proc, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return 0;
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(proc, remote, dll_path, n, &wrote) || wrote != n) {
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE); return 0;
    }
    HMODULE kern = GetModuleHandleA("kernel32.dll");
    LPTHREAD_START_ROUTINE loadfn =
        (LPTHREAD_START_ROUTINE)(uintptr_t)GetProcAddress(kern, "LoadLibraryA");
    if (!loadfn) { VirtualFreeEx(proc, remote, 0, MEM_RELEASE); return 0; }
    HANDLE th = CreateRemoteThread(proc, NULL, 0, loadfn, remote, 0, NULL);
    if (!th) { VirtualFreeEx(proc, remote, 0, MEM_RELEASE); return 0; }
    DWORD wr = WaitForSingleObject(th, 15000);
    DWORD rc = 0; GetExitCodeThread(th, &rc);
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    if (wr != WAIT_OBJECT_0 || rc == 0) {
        LOG("[fallback] remote LoadLibraryA failed (wait=0x%lX rc=0x%lX)\n", wr, rc);
        return 0;
    }
    LOG("[fallback] remote LoadLibraryA OK (HMODULE=0x%lX)\n", rc);
    return 1;
}


/* ===========================================================================
 *  ULTRA GAME LOCKS -- per-game gating, owned by THIS board's module.
 *
 *  Owner's standing rule (2026-09-14): once a game is playable and signed off,
 *  the settings it depends on are pinned HERE, so later work cannot silently
 *  take the win away - a toggle flipped in the loader UI, a feature that starts
 *  defaulting on, a fix aimed at a different title.
 *
 *  PER MODULE, deliberately. A lock in this file can never affect a game on
 *  another board, because another board's module never executes this code.
 *  That containment is what makes locking one title safe.
 *
 *  A lock OVERWRITES what the loader published from ultra.ini. It has to: the
 *  loader already turned the ini into environment, so "is this env set?" cannot
 *  tell an operator's deliberate override from an ini value that is simply
 *  wrong for this title. A lock that yielded to a set env would never fire.
 *  The escape hatch is explicit instead: ULTRA_NO_GAME_LOCKS=1 disables every
 *  lock, on every board, for a debugging run.
 *
 *  TO LOCK A TITLE (only after the owner confirms it plays):
 *      static const ul_lock_env LOCK_FOO[] = {
 *          { "ULTRA_BRIDGE_ENABLE", "0" },
 *      };
 *  then add, ABOVE the terminator:
 *      { "foo", "Foo Game", LOCK_FOO, (int)(sizeof LOCK_FOO/sizeof LOCK_FOO[0]) },
 *  The needle is a lowercased substring of the game path.
 * =========================================================================== */
typedef struct { const char *env, *val; } ul_lock_env;
typedef struct { const char *needle, *title; const ul_lock_env *env; int nenv; } ul_game_lock;

static const ul_game_lock UL_GAME_LOCKS[] = {
    { NULL, NULL, NULL, 0 }   /* terminator -- add locked titles ABOVE this */
};

static void ul_apply_game_locks(const char *game_path)
{
    char lc[MAX_PATH];
    size_t n, i;
    unsigned g;

    if (!game_path || !*game_path) return;
    if (GetEnvironmentVariableA("ULTRA_NO_GAME_LOCKS", NULL, 0)) {
        fprintf(stderr, "[gamelock] disabled by ULTRA_NO_GAME_LOCKS\n");
        fflush(stderr);
        return;
    }
    n = strlen(game_path);
    if (n >= sizeof lc) n = sizeof lc - 1;
    for (i = 0; i < n; i++) {
        char c = game_path[i];
        lc[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lc[n] = 0;

    for (g = 0; g < sizeof UL_GAME_LOCKS / sizeof UL_GAME_LOCKS[0]; g++) {
        const ul_game_lock *L = &UL_GAME_LOCKS[g];
        int k;
        if (!L->needle || !strstr(lc, L->needle)) continue;
        fprintf(stderr, "[gamelock] %s: pinning %d setting(s) proven for this title\n",
                L->title, L->nenv);
        for (k = 0; k < L->nenv; k++) {
            SetEnvironmentVariableA(L->env[k].env, L->env[k].val);
            fprintf(stderr, "[gamelock]   %s=%s\n", L->env[k].env, L->env[k].val);
        }
        fflush(stderr);
    }
}

__declspec(dllexport) int ul_incredible_technologies_run(int argc, char **argv)
{
    if (argc < 2 || !argv[1] || !*argv[1]) {
        LOG("ERROR: no game path (argc=%d)\n", argc);
        return 1;
    }

    /* Per-game gating for titles already proven on this board.
     * NOTE: if this board's own env setup below would overwrite a locked
     * key, move this call to AFTER that block (namco_es3_main.c does). */
    ul_apply_game_locks(argv[1]);

    /* Normalize / -> \ so CreateProcessA accepts the path. */
    char game_buf[MAX_PATH];
    strncpy(game_buf, argv[1], MAX_PATH - 1); game_buf[MAX_PATH - 1] = 0;
    for (char *p = game_buf; *p; p++) if (*p == '/') *p = '\\';
    const char *game = game_buf;
    LOG("launching %s\n", game);

    /* workdir = dir(game); many arcade titles require CWD = game folder. */
    char workdir[MAX_PATH];
    strncpy(workdir, game, MAX_PATH - 1); workdir[MAX_PATH - 1] = 0;
    { char *bs = strrchr(workdir, '\\'); if (bs) *bs = 0; }

    int bits = game_pe_bits(game);
    LOG("game PE bitness = %d\n", bits);

    char hook[MAX_PATH];
    if (!find_hook(hook, sizeof hook, bits)) {
        LOG("ERROR: board-native hook not found\n");
        MessageBoxA(NULL,
            "Incredible Technologies (Golden Tee PC) hook DLL not installed.\n\n"
            "Place ultra_incredible_technologies_hook<bits>.dll\n"
            "next to ultra-loader.exe, or install it via Settings -> Install Module.",
            "Ultra Loader -- Incredible Technologies (Golden Tee PC) Hook Missing",
            MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }
    LOG("hook = %s\n", hook);
    LOG("workdir = %s\n", workdir);

    /* Universal env vars every module sets. */
    SetEnvironmentVariableA("ULTRA_LAUNCH_MODULE", "incredible_technologies");
    SetEnvironmentVariableA("ULTRA_BOARD_HINT",    "incredible_technologies");
    SetEnvironmentVariableA("ULTRA_SYSTEM_ID",     "incredible_technologies");
    SetEnvironmentVariableA("ULTRA_CONTROLS",      "1");

    /* Board-specific ULTRA_* flags -- customize per module. Each is safe when
     * absent; the shared hook branches on ULTRA_BOARD_HINT so the fallback
     * path still does useful board-aware work. */
    /* BEGIN BOARD_ENV_BLOCK -- see per-module dict "notes" for the exact list */
    /* END BOARD_ENV_BLOCK */

    /* Shared frontend knobs (default on if user hasn't set them). */
    if (!GetEnvironmentVariableA("ULTRA_SEGA_WINDOWED", NULL, 0))
        SetEnvironmentVariableA("ULTRA_SEGA_WINDOWED", "1");
    if (!GetEnvironmentVariableA("ULTRA_DISMISS_DIALOGS", NULL, 0))
        SetEnvironmentVariableA("ULTRA_DISMISS_DIALOGS", "1");
    if (!GetEnvironmentVariableA("ULTRA_FPS_CAP", NULL, 0))
        SetEnvironmentVariableA("ULTRA_FPS_CAP", "60");

    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\"", game);
    if (!CreateProcessA(game, cmd, NULL, NULL, TRUE,
                        CREATE_SUSPENDED, NULL,
                        workdir[0] ? workdir : NULL, &si, &pi)) {
        LOG("CreateProcess(%s) failed: %lu\n", game, GetLastError());
        DeleteFileA(hook);
        return 1;
    }
    LOG("spawned pid=%lu tid=%lu (suspended)\n", pi.dwProcessId, pi.dwThreadId);

    int apc_ok = inject_dll_apc(pi.hProcess, pi.hThread, hook);
    if (!apc_ok) LOG("APC inject failed; falling back to CreateRemoteThread\n");

    ResumeThread(pi.hThread);
    Sleep(750);
    int rt_ok = inject_dll_remote_thread(pi.hProcess, hook);
    if (!apc_ok && !rt_ok) {
        LOG("BOTH inject paths failed; terminating game\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        DeleteFileA(hook);
        return 1;
    }
    LOG("hook injected (apc=%d rt=%d), waiting for game exit\n", apc_ok, rt_ok);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0; GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    DeleteFileA(hook);
    LOG("game exited with code %lu\n", rc);
    return (int)rc;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)hinst; (void)reason; (void)reserved;
    return TRUE;
}
