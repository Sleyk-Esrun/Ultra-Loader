/*
 * examu_exboard_main.c -- Examu eX-BOARD spawner + hook injector.
 *
 * The loader's dispatcher (work/src/main.c) calls ul_examu_exboard_run(argc, argv)
 * after detecting an eX-BOARD .exe game with ULTRA_MODULE=examu_exboard (or
 * auto-detected from the board profile). argv layout:
 *   argv[0] = ultra-loader.exe path
 *   argv[1] = game.exe path (the eX-BOARD game)
 *
 * Examu eX-BOARD is a 32-bit Windows-based PC arcade platform (Intel + AMD
 * Radeon HD). Its flagship title family is Arcana Heart (2/3/LOVEMAX). All
 * eX-BOARD games ship as 32-bit executables, so this module unconditionally
 * uses the 32-bit hook (ultra_examu_exboard_hook32.dll).
 *
 * This board now has its OWN fully independent, self-contained hook DLL,
 * built from src/hook/*_examu.c (own clean-room JVS engine, own kernel32 IAT
 * patcher, own D3D9 present/throttle hook, own synthetic DirectInput8
 * device, own DirectSound/XAudio2 diagnostic hook, own dialog-dismiss +
 * byte-patch seam). It shares NO source file and NO compiled binary with
 * modules/native_x86/src/hook/'s ultra_typex_hook32.dll or with any other
 * board's own hook -- see src/hook/build_examu_hook.sh and that directory's
 * per-file header comments for the full rationale. (Earlier revisions of
 * this module copied the shared native_x86 hook; that dependency is gone.)
 *
 * What we do:
 *   1. CreateProcess(game.exe, CREATE_SUSPENDED) so we can inject the hook
 *      DLL BEFORE any game code runs. Packed / DRM'd eX-BOARD games resolve
 *      Direct3D and dongle/IO entry points dynamically during early startup
 *      -- a post-launch LoadLibrary injection loses the race.
 *   2. Write the hook DLL path into the game's VA via VirtualAllocEx +
 *      WriteProcessMemory, then CreateRemoteThread(LoadLibraryA, dll_path)
 *      so the hook's DllMain installs first.
 *   3. Resume the game's main thread.
 *   4. WaitForSingleObject on the game process so the loader stays alive
 *      until the game exits. Return the game's exit code.
 *
 * Hook DLL: ultra_examu_exboard_hook32.dll (this board's own independent
 * hook -- see src/hook/). Search order:
 *   1. %LOCALAPPDATA%\Ultra Loader\modules\examu_exboard\ultra_examu_exboard_hook32.dll
 *   2. <loader_exe_dir>\ultra_examu_exboard_hook32.dll
 *   3. <loader_exe_dir>\modtest\ultra_examu_exboard_hook32.dll
 */

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

/* This board's hook is no longer installed as a standalone file. It's
 * embedded as an RCDATA resource ("HOOK32") inside THIS DLL (see
 * hook_res.rc, baked in by build_module.sh) and extracted to a fresh temp
 * file right before each injection. This means signing/verifying this one
 * DLL automatically covers the hook too, and there's no separate hook file
 * sitting on disk between launches for anything to tamper with -- see
 * docs/PROJECT_LICENSING_AND_SIGNING.md's hook-DLL note (2026-08-02).
 * Caller must DeleteFileA() the returned path once done with it. */
static int find_hook(char *out, size_t cap)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)&find_hook, &self);
    if (!self) { fprintf(stderr, "[examu_exboard] GetModuleHandleEx(self) failed: %lu\n", GetLastError()); return 0; }

    HRSRC info = FindResourceA(self, "HOOK32", RT_RCDATA);
    if (!info) { fprintf(stderr, "[examu_exboard] embedded hook resource not found\n"); return 0; }
    DWORD sz = SizeofResource(self, info);
    HGLOBAL loaded = LoadResource(self, info);
    const void *ptr = loaded ? LockResource(loaded) : NULL;
    if (!ptr || !sz) { fprintf(stderr, "[examu_exboard] embedded hook resource empty\n"); return 0; }

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
            snprintf(out, cap, "%sultra_examu_exboard_hook32_%lu.dll", tmp_dir, GetCurrentProcessId());
            fp = fopen(out, "wb");
            if (fp) break;
            wl = strlen(why);
            snprintf(why + wl, sizeof why - wl, "%s%s", wl ? ", " : "", out);
        }
        /* Nothing took it.  Put the candidate list where the caller's existing
         * error message will print it -- "out" only means a path on success. */
        if (!fp) snprintf(out, cap, "%s", why[0] ? why : "(no candidate directory)");
    }
    if (!fp) { fprintf(stderr, "[examu_exboard] cannot write temp hook %s\n", out); return 0; }
    size_t wrote = fwrite(ptr, 1, sz, fp);
    fclose(fp);
    if (wrote != sz) { fprintf(stderr, "[examu_exboard] short write to temp hook %s\n", out); DeleteFileA(out); return 0; }
    fprintf(stderr, "[examu_exboard] extracted embedded hook (%lu bytes) -> %s\n", sz, out);
    return 1;
}

static int inject_dll(HANDLE proc, const char *dll_path)
{
    size_t n = strlen(dll_path) + 1;
    LPVOID remote = VirtualAllocEx(proc, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        fprintf(stderr, "[examu_exboard] VirtualAllocEx failed: %lu\n", GetLastError());
        return 0;
    }
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(proc, remote, dll_path, n, &wrote) || wrote != n) {
        fprintf(stderr, "[examu_exboard] WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    HMODULE kern = GetModuleHandleA("kernel32.dll");
    LPTHREAD_START_ROUTINE loadfn =
        (LPTHREAD_START_ROUTINE)(uintptr_t)GetProcAddress(kern, "LoadLibraryA");
    if (!loadfn) {
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    HANDLE th = CreateRemoteThread(proc, NULL, 0, loadfn, remote, 0, NULL);
    if (!th) {
        fprintf(stderr, "[examu_exboard] CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    DWORD wr = WaitForSingleObject(th, 15000);
    DWORD remote_load_rc = 0;
    GetExitCodeThread(th, &remote_load_rc);
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    if (wr != WAIT_OBJECT_0) {
        fprintf(stderr, "[examu_exboard] remote LoadLibraryA timed out (wait=0x%lX)\n", wr);
        return 0;
    }
    if (remote_load_rc == 0) {
        fprintf(stderr, "[examu_exboard] remote LoadLibraryA returned NULL (hook DllMain refused or DLL bad)\n");
        return 0;
    }
    fprintf(stderr, "[examu_exboard] remote LoadLibraryA OK (HMODULE=0x%lX)\n", remote_load_rc);
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

__declspec(dllexport) int ul_examu_exboard_run(int argc, char **argv)
{
    if (argc < 2 || !argv[1] || !*argv[1]) {
        fprintf(stderr, "[examu_exboard] ERROR: no game path (argc=%d)\n", argc);
        return 1;
    }

    /* Per-game gating for titles already proven on this board.
     * NOTE: if this board's own env setup below would overwrite a locked
     * key, move this call to AFTER that block (namco_es3_main.c does). */
    ul_apply_game_locks(argv[1]);
    const char *game = argv[1];
    fprintf(stderr, "[examu_exboard] launching %s\n", game);

    /* Derive workdir = dir(game). Handle both '\\' and '/' separators (the
     * loader CLI may hand us either). */
    char workdir[MAX_PATH];
    strncpy(workdir, game, MAX_PATH - 1); workdir[MAX_PATH - 1] = 0;
    {
        char *bs = strrchr(workdir, '\\');
        char *fs = strrchr(workdir, '/');
        char *sep = (bs && fs) ? (bs > fs ? bs : fs) : (bs ? bs : fs);
        if (sep) *sep = 0;
    }

    /* Tell the in-game controls layer where the game lives, so it can find a
     * per-game <game_dir>/ultra_controls.ini. Without this the layer falls back
     * to a path relative to the working directory -- which happens to be right
     * today only because we pass workdir to CreateProcess below, and would break
     * silently the moment a game chdir()s at startup. The child inherits this. */
    SetEnvironmentVariableA("ULTRA_NATIVE_EXE", game);

    /* Turn the hook's own logging ON by default.
     *
     * Every diagnostic in the hook (JVS traffic, the controls layer, the D3D
     * present hooks, the dongle bypass) is gated behind ULTRA_JVS_LOG, and
     * NOTHING in the tree ever set it - not the loader, not this module. So a
     * normal run produced no hook log at all, and every problem report had to
     * be diagnosed by inference instead of evidence. Default it to a file in
     * the game's own folder; an explicit ULTRA_JVS_LOG still wins. */
    if (!GetEnvironmentVariableA("ULTRA_JVS_LOG", NULL, 0)) {
        char lg[MAX_PATH];
        _snprintf(lg, sizeof lg, "%s\\ultra_hook.log", workdir);
        lg[sizeof lg - 1] = 0;
        SetEnvironmentVariableA("ULTRA_JVS_LOG", lg);
    }

    /* Daemon Bride bring-up: FORCE the trace to the dedicated project log so a
     * capture is unambiguous. The container's golden.ini asks for this path, but
     * the loader's global ULTRA_JVS_LOG (the shared ultra_jvs_debug.log, which
     * interleaves every other running game) was winning, so the WinIo capture
     * landed in the wrong file. Only for this one title (needle match, so no
     * other eX-BOARD game is affected), only if the log dir already exists, and
     * ULTRA_JVS_LOG_FORCE=0 opts out. Forward slashes: fopen accepts them and
     * there is no backslash to collapse. */
    {
        char lc[MAX_PATH]; size_t gi, gn = strlen(game);
        const char* nf = NULL; char nfbuf[8] = {0};
        if (GetEnvironmentVariableA("ULTRA_JVS_LOG_FORCE", nfbuf, sizeof nfbuf)) nf = nfbuf;
        if (gn >= sizeof lc) gn = sizeof lc - 1;
        for (gi = 0; gi < gn; ++gi) {
            char c = game[gi];
            lc[gi] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        lc[gn] = 0;
        if (strstr(lc, "daemon bride") && !(nf && nf[0] == '0')) {
            const char* dbdir = "I:/Ultra Loader/log";
            if (GetFileAttributesA(dbdir) != INVALID_FILE_ATTRIBUTES) {
                SetEnvironmentVariableA("ULTRA_JVS_LOG", "I:/Ultra Loader/log/daemonbride.log");
                fprintf(stderr, "[examu_exboard] ULTRA_JVS_LOG forced -> I:/Ultra Loader/log/daemonbride.log (Daemon Bride)\n");
            }
            /* BRING-UP DIAGNOSTIC (Daemon Bride only): the title now reaches its
             * FlashRom asset loader and then exits with a non-standard code
             * (0xCFFFFFFF) with NO WER dump -- a raised/thrown exception the game
             * catches in its own SEH and turns into ExitProcess. The tracer VEH
             * only logs a fixed set of "interesting" codes by default, so force
             * verbose first-chance logging to capture the EXACT exception code +
             * faulting module/offset of that fatal raise. ULTRA_TRACE_VERBOSE=0
             * (or _FORCE=0 above) opts out. Remove once the wall is past. */
            if (!GetEnvironmentVariableA("ULTRA_TRACE_VERBOSE", NULL, 0))
                SetEnvironmentVariableA("ULTRA_TRACE_VERBOSE", "1");
            if (!GetEnvironmentVariableA("ULTRA_TRACE_MAXEXC", NULL, 0))
                SetEnvironmentVariableA("ULTRA_TRACE_MAXEXC", "200");
        }
    }

    char hook[MAX_PATH];
    if (!find_hook(hook, sizeof hook)) {
        fprintf(stderr, "[examu_exboard] ERROR: embedded hook could not be extracted\n");
        MessageBoxA(NULL,
            "Examu eX-BOARD hook could not be extracted from this module.\n\n"
            "The installed ultra_examu_exboard.dll may be corrupt -- reinstall it\n"
            "via Settings -> Install Module.",
            "Ultra Loader -- eX-BOARD Hook Missing",
            MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }
    fprintf(stderr, "[examu_exboard] hook = %s\n", hook);

    /* Env vars this board's OWN independent hook DLL (src/hook/*_examu.c)
     * reads at injection time.
     *
     * All knobs default off / byte-safe. Rationale per knob:
     *   ULTRA_LAUNCH_MODULE / ULTRA_BOARD_HINT / ULTRA_SYSTEM_ID -- boot
     *     console + downstream hook resolution.
     *   ULTRA_D3D9=1 -- Arcana Heart family is D3D9. Arms the D3D9 present
     *     hook (d3d9_throttle_examu.c) for windowed coercion + FPS cap +
     *     upscale.
     *   ULTRA_CONTROLS=1 -- kept for parity with every other board's launch
     *     contract; this board's own hook does NOT read it. Unlike
     *     native_x86 (whose controls.c/input_profiles.c apparatus is
     *     gated on this var), hook_dllmain_examu.c's keyboard+XInput input
     *     source (1=START, 5=COIN, F1=SERVICE, F2=TEST, arrows=directions,
     *     Z/X/C/A/S/D=6 buttons) is ALWAYS active -- see that file's header.
     *   ULTRA_JVS_ANALOG=2 -- fighter (2 sticks, no wheel/pedals). Tells
     *     the JVS engine to publish 2 analog channels; Arcana Heart reads
     *     digital directions but the JVS FEATCHK still needs a truthful
     *     analog count.
     *   ULTRA_SEGA_WINDOWED=1 -- force windowed. Mirrors the Haunted
     *     Museum fix: fullscreen-exclusive D3D9 device can device-LOST
     *     stall and present a black screen even with audio + input alive.
     *     d3d9_throttle_examu.c reads this.
     *   ULTRA_DONGLE_BYPASS=1 -- eX-BOARD games check for a dongle at
     *     boot; without a bypass they refuse to run. The generic USB/
     *     parallel dongle-name CreateFile bypass lives in
     *     jvs_serial_hook_examu.c (is_dongle_device_examu()).
     *   ULTRA_DISMISS_DIALOGS=1 -- kill any modal that sits over the
     *     present surface. dongle_bypass_examu.c reads this.
     *   ULTRA_FPS_CAP=60 -- eX-BOARD target rate.
     *
     * Every SetEnvironmentVariableA below only writes if the caller (loader
     * settings) hasn't already set the var, so user overrides stick. */
    SetEnvironmentVariableA("ULTRA_LAUNCH_MODULE", "examu_exboard");
    SetEnvironmentVariableA("ULTRA_BOARD_HINT",   "examu_exboard");
    SetEnvironmentVariableA("ULTRA_SYSTEM_ID",    "examu_exboard");
    SetEnvironmentVariableA("ULTRA_BOARD",        "examu_exboard");
    if (!GetEnvironmentVariableA("ULTRA_D3D9", NULL, 0))
        SetEnvironmentVariableA("ULTRA_D3D9", "1");
    if (!GetEnvironmentVariableA("ULTRA_CONTROLS", NULL, 0))
        SetEnvironmentVariableA("ULTRA_CONTROLS", "1");
    if (!GetEnvironmentVariableA("ULTRA_JVS_ANALOG", NULL, 0))
        SetEnvironmentVariableA("ULTRA_JVS_ANALOG", "2");
    if (!GetEnvironmentVariableA("ULTRA_SEGA_WINDOWED", NULL, 0))
        SetEnvironmentVariableA("ULTRA_SEGA_WINDOWED", "1");
    if (!GetEnvironmentVariableA("ULTRA_DONGLE_BYPASS", NULL, 0))
        SetEnvironmentVariableA("ULTRA_DONGLE_BYPASS", "1");
    /* WinIo kernel-driver emulation. Daemon Bride's dongle library IpgExKey.dll
     * opens \\.\WINIO and does direct port I/O through the WinIo driver, which
     * cannot load on 64-bit Windows -- so the hook stands in for it (see
     * jvs_serial_hook_examu.c's ultra_winio_ioctl). Only the "WINIO" device name
     * is intercepted, so this is inert for any title that never opens it.
     * ULTRA_WINIO_EMU=0 disables it. */
    if (!GetEnvironmentVariableA("ULTRA_WINIO_EMU", NULL, 0))
        SetEnvironmentVariableA("ULTRA_WINIO_EMU", "1");
    if (!GetEnvironmentVariableA("ULTRA_DISMISS_DIALOGS", NULL, 0))
        SetEnvironmentVariableA("ULTRA_DISMISS_DIALOGS", "1");
    if (!GetEnvironmentVariableA("ULTRA_FPS_CAP", NULL, 0))
        SetEnvironmentVariableA("ULTRA_FPS_CAP", "60");

    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\"", game);
    /* bInheritHandles=TRUE so the child's CRT inherits STD_ERROR_HANDLE
     * (the freopen'd ultra.log). Hook fprintf(stderr,...) lines land in
     * the same log the loader writes. */
    if (!CreateProcessA(game, cmd, NULL, NULL, TRUE,
                        CREATE_SUSPENDED, NULL,
                        workdir[0] ? workdir : NULL, &si, &pi)) {
        fprintf(stderr, "[examu_exboard] CreateProcess(%s) failed: %lu\n",
                game, GetLastError());
        DeleteFileA(hook);
        return 1;
    }
    fprintf(stderr, "[examu_exboard] spawned pid=%lu (suspended)\n", pi.dwProcessId);

    if (!inject_dll(pi.hProcess, hook)) {
        fprintf(stderr, "[examu_exboard] hook inject failed; terminating game\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        DeleteFileA(hook);
        return 1;
    }

    ResumeThread(pi.hThread);
    fprintf(stderr, "[examu_exboard] hook injected, game resumed -- waiting for exit\n");

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0; GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    DeleteFileA(hook);
    fprintf(stderr, "[examu_exboard] game exited with code %lu\n", rc);
    return (int)rc;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)hinst; (void)reason; (void)reserved;
    return TRUE;
}
