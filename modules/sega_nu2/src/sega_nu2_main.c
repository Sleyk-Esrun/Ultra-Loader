/*
 * sega_nu2_main.c -- Sega Nu2 spawner + hook injector.
 *
 * Sega Nu2 (2015+) is the Sega arcade PC platform that replaces the original
 * Nu (2011) -- 64-bit Windows Embedded 8, D3D11-only (no D3D9 fallback like
 * RingEdge2 had), IO4-over-USB inputs (NOT JVS-serial), and the "N2 / L3.1"
 * keychip variant of the Sega DRM stack (USB A7/N2 smart-card via fdd.sys;
 * GkeyEnc/Dec cmd 11/12) plus a BitLocker+TPM-sealed container (we reuse the
 * ULTRA_SEGA_TRUECRYPT knob name for compat with the RingEdge path).
 *
 * Board profile (modules/native_x86/src/hook/board_profile.c, BP_SEGA_NU2):
 *   BP_GFX_D3D11 | BP_IO_IO4_USB | 64-bit | BP_INJECT_SPAWN_SUSPENDED |
 *   BP_HOOK_D3D11 | BP_PROT_KEYCHIP | BP_PROT_CONTAINER | BP_PROT_TPM |
 *   BP_READY_DRM_GATED (needs operator keychip + decrypted container).
 *
 * argv layout from the loader's dispatcher (work/src/main.c):
 *   argv[0] = ultra-loader.exe path
 *   argv[1] = game.exe path (the Nu2 game)
 *
 * Nu2 games are 64-bit exclusively -- we PE-sniff anyway so we don't get
 * fooled by an oddball repack. Sniff picks the matching hook DLL (THIS
 * board's own independent ultra_sega_nu2_hook{32,64}.dll, built from
 * modules/sega_nu2/src/hook/ by build_nu2_hook.sh -- falls back to the
 * shared, generic ultra_typex_hook{32,64}.dll only if the dedicated one
 * isn't installed):
 *   64-bit game -> ultra_sega_nu2_hook64.dll (spawn ultra-inject64.exe helper)
 *   32-bit game -> ultra_sega_nu2_hook32.dll (in-process CreateRemoteThread inject)
 *
 * Injection pattern is the Type X3/X4 helper-process model: our 32-bit loader
 * cannot CreateRemoteThread into a 64-bit target, so we shell out to the
 * matching-bitness ultra-inject64.exe (a generic cross-bitness LoadLibrary
 * helper -- it carries no board-specific logic, unlike the hook DLL) which
 * does the WOW64-side inject.
 *
 * Env vars set BEFORE CreateProcess (all read by the hook at injection time;
 * all default off when unset, so they only turn on for Nu2 -- byte-safe
 * globally):
 *   ULTRA_LAUNCH_MODULE   = "sega_nu2"    -- dispatcher / log tagging
 *   ULTRA_BOARD_HINT      = "sega_nu2"    -- selects Nu2 branch in hook
 *   ULTRA_SYSTEM_ID       = "sega_nu2"    -- boot console + downstream hooks
 *   ULTRA_CONTROLS        = "1"           -- wakes up the input-mapping layer
 *                                            (keyboard + XInput + DInput)
 *   ULTRA_SEGA_WINDOWED   = "1"           -- forces windowed rendering so the
 *                                            DXGI present hook can upscale to
 *                                            the user's chosen output res
 *   ULTRA_SEGA_KEYCHIP    = "1"           -- N2 keychip presence emulator
 *                                            (fdd.sys USB A7/N2 smart-card
 *                                            + GkeyEnc/Dec cmd 11/12 stubs)
 *   ULTRA_SEGA_TRUECRYPT  = "1"           -- BitLocker+TPM container bypass
 *                                            (reuses the RingEdge setting
 *                                            name for hook-side switch reuse)
 *   ULTRA_SEGA_AMAUTH     = "1"           -- amauth.exe presence-fake
 *   ULTRA_SEGA_ALLNET     = "1"           -- ALL.Net server presence-fake
 *   ULTRA_SEGA_AMDAEMON   = "1"           -- amdaemon service presence-fake
 *   ULTRA_SEGA_NETWORK    = "1"           -- netmask/netconfig stubs
 *   ULTRA_IO4             = "1"           -- IO4-over-USB emulator (NOT the
 *                                            JVS-serial path)
 *   ULTRA_HOOK_D3D11      = "1"           -- DXGI present hook, no D3D9
 *   ULTRA_D3D11           = "1"           -- graphics-family hint
 *   ULTRA_JVS_ANALOG      = "3"           -- advertise 3 analog channels to
 *                                            IO4 FEATCHK (steer/accel/brake
 *                                            or aim-x/aim-y/aux depending on
 *                                            the title's input profile)
 *   ULTRA_INPOUT32_BYPASS = "1"           -- neutralize inpout32.dll if a
 *                                            title ships it
 *   ULTRA_DISMISS_DIALOGS = "1"           -- auto-dismiss modal dongle/keychip
 *                                            dialogs behind the present surface
 *   ULTRA_FPS_CAP         = "60"          -- Nu2 hardware target
 *
 * Hook DLL search order (this board's own hook first, shared Type X hook as
 * last-resort fallback ONLY -- sega_nu2 never searches or references
 * sega_nu's install folder or hook name; that would be exactly the kind of
 * cross-board runtime sharing the per-board directive forbids):
 *   1. %LOCALAPPDATA%\Ultra Loader\modules\sega_nu2\ultra_sega_nu2_hook{32,64}.dll
 *   2. <loader_exe_dir>\ultra_sega_nu2_hook{32,64}.dll
 *   3. <loader_exe_dir>\modtest\ultra_sega_nu2_hook{32,64}.dll
 *   4. <loader_exe_dir>\ultra_typex_hook{32,64}.dll             (generic fallback)
 *   5. <loader_exe_dir>\modtest\ultra_typex_hook{32,64}.dll     (generic fallback)
 */

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#define LOG(...) do { fprintf(stderr, "[sega_nu2] " __VA_ARGS__); fflush(stderr); } while (0)

/* PE bitness sniff. Returns 32 or 64; 0 on error. */
static int pe_bitness(const char *exe)
{
    FILE *f = fopen(exe, "rb"); if (!f) return 0;
    unsigned char mz[2] = {0};
    if (fread(mz, 1, 2, f) != 2 || mz[0] != 'M' || mz[1] != 'Z') { fclose(f); return 0; }
    fseek(f, 0x3C, SEEK_SET);
    unsigned int pe_off = 0;
    if (fread(&pe_off, 4, 1, f) != 1) { fclose(f); return 0; }
    fseek(f, pe_off + 4, SEEK_SET);   /* skip PE signature to IMAGE_FILE_HEADER */
    unsigned short machine = 0;
    if (fread(&machine, 2, 1, f) != 1) { fclose(f); return 0; }
    fclose(f);
    if (machine == 0x014C) return 32;    /* IMAGE_FILE_MACHINE_I386 */
    if (machine == 0x8664) return 64;    /* IMAGE_FILE_MACHINE_AMD64 */
    return 0;
}

/* This board's hook is no longer installed as a standalone file. It's
 * embedded as an RCDATA resource ("HOOK32"/"HOOK64") inside THIS DLL (see
 * hook_res.rc, baked in by build_module.sh) and extracted to a fresh temp
 * file right before each injection. This means signing/verifying this one
 * DLL automatically covers the hook too, and there's no separate hook file
 * sitting on disk between launches for anything to tamper with -- see
 * docs/PROJECT_LICENSING_AND_SIGNING.md's hook-DLL note (2026-08-02). This
 * also removes the old shared-typex-hook fallback entirely: there is no
 * fallback resource, only this board's own embedded hook. Caller must
 * DeleteFileA() the returned path once done with it. */
static int find_hook(int bits, char *out, size_t cap)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)&find_hook, &self);
    if (!self) { LOG("GetModuleHandleEx(self) failed: %lu\n", GetLastError()); return 0; }

    const char *res_name = (bits == 32) ? "HOOK32" : "HOOK64";
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

/* 32-bit in-process inject (rarely used for Nu2 -- games are 64-bit -- but
 * kept for repacks / oddballs). */
static int inject_dll_32(HANDLE proc, const char *dll_path)
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
    LPTHREAD_START_ROUTINE loadfn =
        (LPTHREAD_START_ROUTINE)(uintptr_t)GetProcAddress(kern, "LoadLibraryA");
    if (!loadfn) { VirtualFreeEx(proc, remote, 0, MEM_RELEASE); return 0; }
    HANDLE th = CreateRemoteThread(proc, NULL, 0, loadfn, remote, 0, NULL);
    if (!th) { VirtualFreeEx(proc, remote, 0, MEM_RELEASE); return 0; }
    WaitForSingleObject(th, 15000);
    DWORD rc = 0; GetExitCodeThread(th, &rc);
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return rc != 0;
}

/* 64-bit inject via helper: our 32-bit loader can't CreateRemoteThread into
 * a 64-bit process, so we spawn ultra-inject64.exe which does the WOW64
 * inject from the correct bitness. Search for it near the loader. */
static int inject_dll_64(DWORD pid, const char *dll_path)
{
    char loader_exe[MAX_PATH];
    GetModuleFileNameA(NULL, loader_exe, MAX_PATH);
    char *bs = strrchr(loader_exe, '\\'); if (bs) *bs = 0;

    char injector[MAX_PATH];
    int found = 0;
    snprintf(injector, sizeof injector, "%s\\ultra-inject64.exe", loader_exe);
    if (GetFileAttributesA(injector) != INVALID_FILE_ATTRIBUTES) found = 1;
    if (!found) {
        snprintf(injector, sizeof injector, "%s\\modtest\\ultra-inject64.exe", loader_exe);
        if (GetFileAttributesA(injector) != INVALID_FILE_ATTRIBUTES) found = 1;
    }
    if (!found) {
        LOG("ultra-inject64.exe not found (needed for 64-bit Nu2 games)\n");
        return 0;
    }

    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\" %lu \"%s\"", injector, pid, dll_path);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        LOG("CreateProcess(inject64) failed: %lu\n", GetLastError());
        return 0;
    }
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return rc == 0;
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

__declspec(dllexport) int ul_sega_nu2_run(int argc, char **argv)
{
    if (argc < 2 || !argv[1] || !*argv[1]) {
        LOG("ERROR: no game path (argc=%d)\n", argc);
        return 1;
    }

    /* Per-game gating for titles already proven on this board.
     * NOTE: if this board's own env setup below would overwrite a locked
     * key, move this call to AFTER that block (namco_es3_main.c does). */
    ul_apply_game_locks(argv[1]);
    const char *game = argv[1];
    LOG("launching %s\n", game);

    int bits = pe_bitness(game);
    if (bits == 0) { LOG("not a PE file: %s\n", game); return 1; }
    LOG("PE bitness = %d\n", bits);

    char hook[MAX_PATH];
    if (!find_hook(bits, hook, sizeof hook)) {
        LOG("ERROR: embedded hook for %d-bit could not be extracted\n", bits);
        MessageBoxA(NULL,
            "Sega Nu2 hook could not be extracted from this module.\n\n"
            "The installed ultra_sega_nu2.dll may be corrupt -- reinstall it via\n"
            "Settings -> Install Module. (ultra-inject64.exe is still needed next\n"
            "to ultra-loader.exe for 64-bit games.)",
            "Ultra Loader -- Nu2 Hook Missing",
            MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }
    LOG("hook = %s\n", hook);

    /* Derive workdir = dir(game). Nu2 games expect to run from their own
     * folder for relative asset lookups. Handle both '\\' and '/' separators. */
    char workdir[MAX_PATH];
    strncpy(workdir, game, MAX_PATH - 1); workdir[MAX_PATH - 1] = 0;
    {
        char *b1 = strrchr(workdir, '\\');
        char *f1 = strrchr(workdir, '/');
        char *sep = (b1 && f1) ? (b1 > f1 ? b1 : f1) : (b1 ? b1 : f1);
        if (sep) *sep = 0;
    }
    LOG("workdir = %s\n", workdir);

    /* Env vars the hook reads at injection time. See file header for
     * per-var rationale. All default off when unset, so they only turn on
     * for Nu2 -- byte-safe globally. */
    SetEnvironmentVariableA("ULTRA_LAUNCH_MODULE",   "sega_nu2");
    SetEnvironmentVariableA("ULTRA_BOARD_HINT",      "sega_nu2");
    SetEnvironmentVariableA("ULTRA_SYSTEM_ID",       "sega_nu2");
    /* controls.c's ultra_input_map_install() resolves the per-board input
     * profile from THIS var specifically (not BOARD_HINT/SYSTEM_ID) -- see
     * the Europa-R fix in X86_BOARDS.md S7 for the bug this closes. */
    SetEnvironmentVariableA("ULTRA_BOARD",           "sega_nu2");
    SetEnvironmentVariableA("ULTRA_CONTROLS",        "1");
    SetEnvironmentVariableA("ULTRA_SEGA_WINDOWED",   "1");
    SetEnvironmentVariableA("ULTRA_SEGA_KEYCHIP",    "1");
    SetEnvironmentVariableA("ULTRA_SEGA_TRUECRYPT",  "1");
    SetEnvironmentVariableA("ULTRA_SEGA_AMAUTH",     "1");
    SetEnvironmentVariableA("ULTRA_SEGA_ALLNET",     "1");
    SetEnvironmentVariableA("ULTRA_SEGA_AMDAEMON",   "1");
    SetEnvironmentVariableA("ULTRA_SEGA_NETWORK",    "1");
    SetEnvironmentVariableA("ULTRA_IO4",             "1");
    SetEnvironmentVariableA("ULTRA_HOOK_D3D11",      "1");
    SetEnvironmentVariableA("ULTRA_D3D11",           "1");
    SetEnvironmentVariableA("ULTRA_JVS_ANALOG",      "3");
    SetEnvironmentVariableA("ULTRA_INPOUT32_BYPASS", "1");
    if (!GetEnvironmentVariableA("ULTRA_DISMISS_DIALOGS", NULL, 0))
        SetEnvironmentVariableA("ULTRA_DISMISS_DIALOGS", "1");
    if (!GetEnvironmentVariableA("ULTRA_FPS_CAP", NULL, 0))
        SetEnvironmentVariableA("ULTRA_FPS_CAP", "60");

    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\"", game);
    if (!CreateProcessA(game, cmd, NULL, NULL, TRUE, CREATE_SUSPENDED,
                        NULL, workdir[0] ? workdir : NULL, &si, &pi)) {
        LOG("CreateProcess(%s) failed: %lu\n", game, GetLastError());
        DeleteFileA(hook);
        return 1;
    }
    LOG("spawned pid=%lu tid=%lu (suspended)\n", pi.dwProcessId, pi.dwThreadId);

    /* Small pause so WOW64 (or 64-bit ntdll) finishes early loader init
     * before the injector reaches in. Mirrors the Europa-R pattern; small
     * enough that the user doesn't perceive delay. */
    Sleep(200);

    int ok = (bits == 64)
        ? inject_dll_64(pi.dwProcessId, hook)
        : inject_dll_32(pi.hProcess, hook);
    if (!ok) {
        LOG("inject failed; terminating game\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        DeleteFileA(hook);
        return 1;
    }
    LOG("hook injected\n");

    ResumeThread(pi.hThread);
    LOG("game resumed; waiting for exit\n");
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
