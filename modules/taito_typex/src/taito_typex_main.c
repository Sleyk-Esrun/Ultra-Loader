/*
 * taito_typex_main.c -- Taito Type X spawner + hook injector.
 *
 * The loader's dispatcher (work/src/main.c) calls ul_taito_typex_run(argc, argv)
 * after detecting a Type X .exe game with ULTRA_MODULE=taito_typex (or
 * auto-detected from the game's path). argv layout:
 *   argv[0] = ultra-loader.exe path
 *   argv[1] = game.exe path (the Type X game)
 *
 * What we do:
 *   1. CreateProcess(game.exe, CREATE_SUSPENDED) so we can inject the hook
 *      DLL BEFORE any game code runs. Packed Taito Type X games resolve
 *      Direct3D/dongle/IO entry points dynamically during early startup --
 *      a post-launch LoadLibrary injection consistently loses the race.
 *   2. Write the hook DLL path into the game's VA via VirtualAllocEx +
 *      WriteProcessMemory, then CreateRemoteThread(LoadLibraryA, dll_path)
 *      so the hook's DllMain (and its IAT/GetProcAddress interception)
 *      installs first.
 *   3. Resume the game's main thread.
 *   4. WaitForSingleObject on the game process so the loader stays alive
 *      until the game exits. Return the game's exit code.
 *
 * Hook DLL: ultra_taito_typex_hook32.dll -- this board's OWN independently
 * compiled hook (built by src/build_hook.sh from this board's own source
 * tree, not copied from modtest/ or native_x86's shared build -- fixed
 * 2026-08-01 per the standing no-shared-binary directive). Search order:
 *   1. %LOCALAPPDATA%\Ultra Loader\modules\taito_typex\ultra_taito_typex_hook32.dll
 *   2. <loader_exe_dir>\ultra_taito_typex_hook32.dll
 * (no modtest\ fallback anymore -- this board never depends on the shared
 * hook build again.)
 */

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

/* This board's hook is no longer installed as a standalone file. It's
 * embedded as an RCDATA resource ("HOOK32") inside THIS DLL (see
 * hook_res.rc, baked in by build_module.sh -- from the SAME pinned-prebuilt
 * binary this board has always shipped, see that script's header for why)
 * and extracted to a fresh temp file right before each injection. This
 * means signing/verifying this one DLL automatically covers the hook too,
 * and there's no separate hook file sitting on disk between launches for
 * anything to tamper with -- see docs/PROJECT_LICENSING_AND_SIGNING.md's
 * hook-DLL note (2026-08-02). Caller must DeleteFileA() the returned path
 * once done with it. */
static int find_hook(char *out, size_t cap)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)&find_hook, &self);
    if (!self) { fprintf(stderr, "[taito_typex] GetModuleHandleEx(self) failed: %lu\n", GetLastError()); return 0; }

    HRSRC info = FindResourceA(self, "HOOK32", RT_RCDATA);
    if (!info) { fprintf(stderr, "[taito_typex] embedded hook resource not found\n"); return 0; }
    DWORD sz = SizeofResource(self, info);
    HGLOBAL loaded = LoadResource(self, info);
    const void *ptr = loaded ? LockResource(loaded) : NULL;
    if (!ptr || !sz) { fprintf(stderr, "[taito_typex] embedded hook resource empty\n"); return 0; }

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
            snprintf(out, cap, "%sultra_taito_typex_hook32_%lu.dll", tmp_dir, GetCurrentProcessId());
            fp = fopen(out, "wb");
            if (fp) break;
            wl = strlen(why);
            snprintf(why + wl, sizeof why - wl, "%s%s", wl ? ", " : "", out);
        }
        /* Nothing took it.  Put the candidate list where the caller's existing
         * error message will print it -- "out" only means a path on success. */
        if (!fp) snprintf(out, cap, "%s", why[0] ? why : "(no candidate directory)");
    }
    if (!fp) { fprintf(stderr, "[taito_typex] cannot write temp hook %s\n", out); return 0; }
    size_t wrote = fwrite(ptr, 1, sz, fp);
    fclose(fp);
    if (wrote != sz) { fprintf(stderr, "[taito_typex] short write to temp hook %s\n", out); DeleteFileA(out); return 0; }
    fprintf(stderr, "[taito_typex] extracted embedded hook (%lu bytes) -> %s\n", sz, out);
    return 1;
}

/* Same extraction shape as find_hook(), for the SEPARATE, independently
 * injected fix DLL(s) -- see codepage_spoof_standalone.c's header for why
 * this rides alongside the frozen hook instead of being folded into it.
 * Only called for a title whose provisional-fix entry actually needs it, so
 * this costs every other title on this board nothing. */
static int find_extra_dll(const char *resname, const char *basename, char *out, size_t cap)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)&find_extra_dll, &self);
    if (!self) { fprintf(stderr, "[taito_typex] GetModuleHandleEx(self) failed: %lu\n", GetLastError()); return 0; }

    HRSRC info = FindResourceA(self, resname, RT_RCDATA);
    if (!info) { fprintf(stderr, "[taito_typex] embedded resource '%s' not found\n", resname); return 0; }
    DWORD sz = SizeofResource(self, info);
    HGLOBAL loaded = LoadResource(self, info);
    const void *ptr = loaded ? LockResource(loaded) : NULL;
    if (!ptr || !sz) { fprintf(stderr, "[taito_typex] embedded resource '%s' empty\n", resname); return 0; }

    char tmp_dir[MAX_PATH];
    FILE *fp = NULL;
    {
        char cands[4][MAX_PATH];
        char base[MAX_PATH];
        int  ncand = 0, ci;
        DWORD n;

        n = GetTempPathA(MAX_PATH, cands[ncand]);
        if (n > 0 && n < MAX_PATH) ncand++;

        n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
        if (n > 0 && n < MAX_PATH - 24) {
            snprintf(cands[ncand], MAX_PATH, "%s\\Ultra Loader\\tmp\\", base);
            ncand++;
        }

        n = GetCurrentDirectoryA(MAX_PATH, base);
        if (n > 0 && n < MAX_PATH - 2) {
            snprintf(cands[ncand], MAX_PATH, "%s%s", base,
                     (base[n - 1] == '\\' || base[n - 1] == '/') ? "" : "\\");
            ncand++;
        }

        for (ci = 0; ci < ncand; ci++) {
            memcpy(tmp_dir, cands[ci], MAX_PATH);
            snprintf(out, cap, "%s%s_%lu.dll", tmp_dir, basename, GetCurrentProcessId());
            fp = fopen(out, "wb");
            if (fp) break;
        }
    }
    if (!fp) { fprintf(stderr, "[taito_typex] cannot write temp file for '%s'\n", resname); return 0; }
    size_t wrote = fwrite(ptr, 1, sz, fp);
    fclose(fp);
    if (wrote != sz) { fprintf(stderr, "[taito_typex] short write for '%s'\n", resname); DeleteFileA(out); return 0; }
    fprintf(stderr, "[taito_typex] extracted '%s' (%lu bytes) -> %s\n", resname, sz, out);
    return 1;
}

static int inject_dll(HANDLE proc, const char *dll_path)
{
    size_t n = strlen(dll_path) + 1;
    LPVOID remote = VirtualAllocEx(proc, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        fprintf(stderr, "[taito_typex] VirtualAllocEx failed: %lu\n", GetLastError());
        return 0;
    }
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(proc, remote, dll_path, n, &wrote) || wrote != n) {
        fprintf(stderr, "[taito_typex] WriteProcessMemory failed: %lu\n", GetLastError());
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
        fprintf(stderr, "[taito_typex] CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    DWORD wr = WaitForSingleObject(th, 15000);
    DWORD remote_load_rc = 0;
    GetExitCodeThread(th, &remote_load_rc);
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    if (wr != WAIT_OBJECT_0) {
        fprintf(stderr, "[taito_typex] remote LoadLibraryA timed out (wait=0x%lX)\n", wr);
        return 0;
    }
    if (remote_load_rc == 0) {
        fprintf(stderr, "[taito_typex] remote LoadLibraryA returned NULL (hook DllMain refused or DLL bad)\n");
        return 0;
    }
    fprintf(stderr, "[taito_typex] remote LoadLibraryA OK (HMODULE=0x%lX)\n", remote_load_rc);
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

/* ===========================================================================
 *  PROVISIONAL per-title fixes -- same needle-match mechanism as the game
 *  locks above, but for a fix that has NOT yet been confirmed by playtest.
 *  Once an owner confirms a title plays with one of these on, move its entry
 *  up into UL_GAME_LOCKS instead (that table's own comment explains why:
 *  locks are for proven settings). Kept separate so an unconfirmed guess is
 *  never mistaken for a proven one.
 * =========================================================================== */
/* Block King Ball Shooter's boot/attract text mixes clean ASCII with garbled
 * Shift-JIS -- this dump was built for a Japanese-locale Windows
 * (GetACP()==932) and its own text code decides byte-pair-vs-single-byte
 * from that; see codepage_spoof.c. */
static const ul_lock_env FIX_BLOCK_KING[] = {
    { "ULTRA_FORCE_JP_CODEPAGE", "1" },
    /* "Touch sensor error (0xffffffff)" is lsdrv.dll's own OpenDriver() timing
     * out waiting for a companion vendor service that publishes three named
     * shared-memory mappings ("LightTabletMap"/"ConMap"/"ExMap") plus a mutex
     * ("LSDRVAPP") -- confirmed by disassembly, not a guess. See
     * lsdrv_shim_standalone.c for the fix: create those objects ourselves so
     * the real, unmodified driver code succeeds against zeroed ("no touch")
     * data. */
    { "ULTRA_LSDRV_SHIM", "1" },
    /* Past that gate, the game's own test-menu screens (confirmed live: the
     * touch-sensor calibration prompt) read the keyboard directly for
     * "[ENTER]"/"[TEST]" -- see keybridge_standalone.c for the gamepad ->
     * keyboard bridge (A -> Enter, left-thumb-click -> F2). */
    { "ULTRA_KEYBRIDGE", "1" },
    /* SETTLED 2026-09-28: the calibration screen depends on a real
     * touch-panel handshake (lsdrv.dll's TabletMessage/TabletControl) that
     * cannot complete without real hardware -- exhaustively proven this
     * session (keyboard, joystick, WM_KEYDOWN, and the screen's own message
     * dispatch all traced and ruled out). This game is known to require an
     * external binary patch before it even runs stably. No longer trying to
     * satisfy that handshake --
     * see calib_bypass_standalone.c, which calls the screen's own existing
     * close/cancel method directly once its real instance is identified,
     * the same call a working touch panel would have triggered. */
    { "ULTRA_CALIB_BYPASS", "1" },
};

static const ul_game_lock UL_PROVISIONAL_FIXES[] = {
    { "block king", "Block King Ball Shooter",
      FIX_BLOCK_KING, (int)(sizeof FIX_BLOCK_KING / sizeof FIX_BLOCK_KING[0]) },
    { NULL, NULL, NULL, 0 }   /* terminator */
};

static void ul_apply_provisional_fixes(const char *game_path)
{
    char lc[MAX_PATH];
    size_t n, i;
    unsigned g;

    if (!game_path || !*game_path) return;
    if (GetEnvironmentVariableA("ULTRA_NO_GAME_LOCKS", NULL, 0)) return;
    n = strlen(game_path);
    if (n >= sizeof lc) n = sizeof lc - 1;
    for (i = 0; i < n; i++) {
        char c = game_path[i];
        lc[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lc[n] = 0;

    for (g = 0; g < sizeof UL_PROVISIONAL_FIXES / sizeof UL_PROVISIONAL_FIXES[0]; g++) {
        const ul_game_lock *L = &UL_PROVISIONAL_FIXES[g];
        int k;
        if (!L->needle || !strstr(lc, L->needle)) continue;
        fprintf(stderr, "[gamelock] %s: applying %d UNCONFIRMED fix(es) awaiting playtest\n",
                L->title, L->nenv);
        for (k = 0; k < L->nenv; k++) {
            SetEnvironmentVariableA(L->env[k].env, L->env[k].val);
            fprintf(stderr, "[gamelock]   %s=%s\n", L->env[k].env, L->env[k].val);
        }
        fflush(stderr);
    }
}

__declspec(dllexport) int ul_taito_typex_run(int argc, char **argv)
{
    if (argc < 2 || !argv[1] || !*argv[1]) {
        fprintf(stderr, "[taito_typex] ERROR: no game path (argc=%d)\n", argc);
        return 1;
    }

    /* Per-game gating for titles already proven on this board.
     * NOTE: if this board's own env setup below would overwrite a locked
     * key, move this call to AFTER that block (namco_es3_main.c does). */
    ul_apply_game_locks(argv[1]);
    ul_apply_provisional_fixes(argv[1]);
    const char *game = argv[1];
    fprintf(stderr, "[taito_typex] launching %s\n", game);

    /* Derive workdir = dir(game). Games typically expect to run from their
     * own folder for relative-path data file lookups.
     *
     * Must handle BOTH '\\' (typical Win32) and '/' (forward-slash paths the
     * loader CLI/dispatcher may hand us, e.g. "I:/Taito Type X Dumps/...").
     * If we only split on '\\' and the path uses '/', strrchr returns NULL,
     * workdir stays equal to the full path including "game.exe", and Win32
     * CreateProcessA then rejects lpCurrentDirectory with ERROR_DIRECTORY
     * (267) so the hook never injects. */
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

    char hook[MAX_PATH];
    if (!find_hook(hook, sizeof hook)) {
        fprintf(stderr, "[taito_typex] ERROR: embedded hook could not be extracted\n");
        MessageBoxA(NULL,
            "Taito Type X hook could not be extracted from this module.\n\n"
            "The installed ultra_taito_typex.dll may be corrupt -- reinstall it via\n"
            "Settings -> Install Module.",
            "Ultra Loader -- Type X Hook Missing",
            MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }
    fprintf(stderr, "[taito_typex] hook = %s\n", hook);

    /* Env vars the hook DLL reads at injection time.
     *
     * IMPORTANT: ULTRA_SEGA_WINDOWED is read by the D3D9/D3D11/GL hook lanes
     * (d3d9_throttle.c, d3d11_throttle.c, gl_present.c) but NOT by the D3D8
     * lane (d3d8_throttle.c). Chaos Breaker is a D3D8 title; setting
     * ULTRA_SEGA_WINDOWED=1 here would be a no-op for that game.
     *
     * 2026-08-01 LIVE-PLAYTEST FINDING (this is now enforced, not just
     * documented -- a prior version of this comment only recommended it,
     * and that was not enough): on real hardware tested this session, the
     * upscale/sharpen post-process path (g_up_w>0 via ULTRA_RES/ULTRA_RES_W,
     * or ULTRA_SHARPEN>0) forces Hook_CreateDevice to flip the device
     * Windowed=TRUE (d3d8_throttle.c's "forced WINDOWED for upscale" path).
     * That path DOES create a stable device -- the game logic, JVS I/O, and
     * audio all run correctly inside it -- but it presents a solid BLACK
     * backbuffer for the entire session (confirmed via ULTRA_ROT_DUMP frame
     * captures, not just visual impression). True native fullscreen
     * (g_up_w==0, do_sharpen==0) is the ONLY path that actually painted a
     * picture on this hardware. The problem: the loader's Settings dialog
     * defaults the Upscaler to FSR, and settings.c silently exports
     * ULTRA_SHARPEN/ULTRA_RES_W/ULTRA_RES_H from that default whenever they
     * aren't already set -- so a fresh install/launch was ALWAYS silently
     * routed into the black-screen path, even though nobody explicitly
     * asked for upscaling. Force the native path here, unconditionally,
     * overriding whatever settings.c already exported -- Chaos Breaker (and
     * any other D3D8 Type X title sharing this hook) needs it to boot with
     * a picture at all. A user who explicitly wants FSR/upscale for a title
     * that handles it fine can still get it by setting ULTRA_TYPEX_UPSCALE=1
     * before launch (checked below) -- opt-in, not a silent default.
     *
     * DO arm the dialog-dismisser (the dongle modal can sit over the
     * fullscreen surface and look like a black screen) and the 60-fps cap.
     * ULTRA_TYPEX_POKE=1 is now also forced on: without it the JVS I/O
     * watchdog (gamestate_probe.c) is free to trip and the game silently
     * calls ExitProcess after ~20-90s of normal-looking play (confirmed via
     * live test: EXITPROCESS logged, process gone, no crash dialog) -- the
     * poke keeps the watchdog fed. It only ever writes memory when the host
     * image's AddressOfEntryPoint matches the real Chaos Breaker binary
     * (gamestate_probe.c's own safety gate), so it's a silent no-op on any
     * other Type X title. */
    SetEnvironmentVariableA("ULTRA_BOARD_HINT", "taito_typex");
    SetEnvironmentVariableA("ULTRA_SYSTEM_ID", "taito_typex");
    /* controls.c's ultra_input_map_install() resolves the per-board input
     * profile from THIS var specifically (not BOARD_HINT/SYSTEM_ID) -- see
     * the Europa-R fix in X86_BOARDS.md S7 for the bug this closes. */
    SetEnvironmentVariableA("ULTRA_BOARD", "taito_typex");
    /* Dismiss dongle/error modals that the game pops behind the present
     * surface; without this the game can sit on a black screen waiting
     * for an OK click that never comes. dongle_bypass.c:47 reads this. */
    if (!GetEnvironmentVariableA("ULTRA_DISMISS_DIALOGS", NULL, 0))
        SetEnvironmentVariableA("ULTRA_DISMISS_DIALOGS", "1");
    /* 60 fps cap (the Type X hardware target). d3d8_throttle.c:122 reads
     * this; default to 60 if the loader didn't set it. */
    /* OVERRIDE the loader's global cap, for the same reason as ULTRA_WINDOWED
     * above: "only if unset" loses, because settings.c has already exported the
     * user's global fps_cap by the time we get here. With that global at 120 the
     * limiter armed at 120 and Chaos Breaker ran at 120.7 fps -- exactly DOUBLE
     * speed, because these titles are fixed-timestep against a 60Hz Type X
     * cabinet and take their game clock from the present rate.
     * ULTRA_TYPEX_FPS_CAP=<n> sets a different cap deliberately. */
    {
        char want[16];
        DWORD n = GetEnvironmentVariableA("ULTRA_TYPEX_FPS_CAP", want, sizeof want);
        const char *cap = (n && n < sizeof want) ? want : "60";
        char cur[16] = {0};
        GetEnvironmentVariableA("ULTRA_FPS_CAP", cur, sizeof cur);
        if (strcmp(cur, cap) != 0) {
            SetEnvironmentVariableA("ULTRA_FPS_CAP", cap);
            fprintf(stderr, "[taito_typex] fps cap %s -> %s (Type X cabinets are 60Hz; "
                            "a higher cap runs these games fast)\n",
                    cur[0] ? cur : "(unset)", cap);
        }
    }
    /* MASTER input enable -- without this, controls.c's input-mapping layer
     * is DEFAULT-OFF and the hook only delivers a legacy demo path (arrows,
     * space, enter). With ULTRA_CONTROLS=1 the fighter/racer profile in
     * input_profiles.c wakes up: keyboard 1=START, 5=COIN, F2=TEST + full
     * XInput / DInput fallback chain. */
    SetEnvironmentVariableA("ULTRA_CONTROLS", "1");
    /* Force the proven-working native D3D8 path unless the user explicitly
     * opts into upscale/sharpen (see the long comment above). */
    if (!GetEnvironmentVariableA("ULTRA_TYPEX_UPSCALE", NULL, 0)) {
        SetEnvironmentVariableA("ULTRA_RES", "0x0");     /* invalid -> parsed as "unset" */
        SetEnvironmentVariableA("ULTRA_RES_W", "");
        SetEnvironmentVariableA("ULTRA_RES_H", "");
        SetEnvironmentVariableA("ULTRA_SHARPEN", "0");
        SetEnvironmentVariableA("ULTRA_ROTATE", "0");
        SetEnvironmentVariableA("ULTRA_AA", "0");
    }
    /* ★ FORCE WINDOWED -- without this Chaos Breaker cannot create a device at
     * all on Windows 10.
     *
     * Measured 2026-09-10, straight off the wire:
     *     D3D8: CreateDevice bb=640x480 fmt=23 swapeffect=3 windowed=0
     * fmt 23 is D3DFMT_R5G6B5, i.e. a 16-BIT FULLSCREEN-EXCLUSIVE device. WDDM
     * stopped exposing 16-bit exclusive display modes, so CreateDevice answers
     * D3DERR_INVALIDCALL (0x8876086C); the game retries six times, prints
     *     CreateDevice error 8876086c / Error:DrawBase Initalize
     * and exits -1. It does this with NO loader and NO hook present as well, so
     * it is the display stack rather than anything of ours -- and that is
     * precisely why it has to be fixed here, in the only place that sits
     * between the game and D3D8.
     *
     * The hook already knows how to rewrite the parameters (windowed + a
     * desktop-safe X8R8G8B8 backbuffer, same size, so Present just stretches
     * it); it simply needed to be told to. Verified live: with this set the
     * game reaches its attract/title screen and renders correctly.
     *
     * NOT the upscale path: ULTRA_TYPEX_UPSCALE stays opt-in, because that is
     * the one that went black on this hardware (see the long comment above).
     * Set ULTRA_WINDOWED=0 before launch to opt out of this. */
    /* This OVERRIDES the loader's global display setting on purpose. settings.c
     * exports ULTRA_WINDOWED=0 whenever the user's display mode is anything but
     * "windowed", and deferring to that is what kept this broken: the game then
     * asks for 16-bit exclusive fullscreen and simply cannot get a device. The
     * board knows something the global setting cannot -- its titles are D3D8 and
     * 16-bit exclusive is gone from this OS -- so it wins here.
     * ULTRA_TYPEX_ALLOW_FULLSCREEN=1 disables the override for anyone who wants
     * to try exclusive fullscreen on hardware/OS where it still exists. */
    {
        char cur[8] = {0};
        DWORD n = GetEnvironmentVariableA("ULTRA_WINDOWED", cur, sizeof cur);
        int allow_fs = GetEnvironmentVariableA("ULTRA_TYPEX_ALLOW_FULLSCREEN", NULL, 0) != 0;
        if (!allow_fs && !(n && cur[0] == '1')) {
            SetEnvironmentVariableA("ULTRA_WINDOWED", "1");
            fprintf(stderr, "[taito_typex] forcing ULTRA_WINDOWED=1 (was '%s'): "
                            "16-bit exclusive fullscreen is unavailable on this OS\n",
                    n ? cur : "(unset)");
        }
    }

    /* Keep the JVS I/O watchdog fed for the real Chaos Breaker binary; a
     * no-op on any other Type X title (gated on AddressOfEntryPoint match
     * inside gamestate_probe.c itself). */
    SetEnvironmentVariableA("ULTRA_TYPEX_POKE", "1");

    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\"", game);
    /* bInheritHandles=TRUE so the child's CRT inherits our STD_ERROR_HANDLE
     * (the freopen'd ultra.log). The hook's fprintf(stderr,...) diagnostic
     * lines (D3D8 CreateDevice / D3D9 hook armed / etc.) will then land in
     * the same log file the loader writes, which is the only way to see
     * what the hook is doing inside the child without a console. */
    if (!CreateProcessA(game, cmd, NULL, NULL, TRUE,
                        CREATE_SUSPENDED, NULL,
                        workdir[0] ? workdir : NULL, &si, &pi)) {
        fprintf(stderr, "[taito_typex] CreateProcess(%s) failed: %lu\n",
                game, GetLastError());
        DeleteFileA(hook);
        return 1;
    }
    fprintf(stderr, "[taito_typex] spawned pid=%lu (suspended)\n", pi.dwProcessId);

    if (!inject_dll(pi.hProcess, hook)) {
        fprintf(stderr, "[taito_typex] hook inject failed; terminating game\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        DeleteFileA(hook);
        return 1;
    }

    /* SEPARATE, additional fix DLL(s) -- only for a title whose provisional
     * fix asked for it (ul_apply_provisional_fixes above). Never folded into
     * the frozen hook above; see codepage_spoof_standalone.c's header. A
     * failure here is non-fatal (the game still runs, just without this one
     * fix) -- unlike the primary hook, none of these are required for the
     * game to launch at all. */
    typedef struct { const char *env, *res, *base; } ul_extra_dll;
    static const ul_extra_dll EXTRAS[] = {
        { "ULTRA_FORCE_JP_CODEPAGE", "CPSPOOF32",   "ultra_typex_codepage_spoof32" },
        { "ULTRA_LSDRV_SHIM",        "LSDRVSHIM32", "ultra_typex_lsdrv_shim32" },
        { "ULTRA_KEYBRIDGE",         "KEYBRIDGE32", "ultra_typex_keybridge32" },
        { "ULTRA_CALIB_BYPASS",      "CALIBBYPASS32", "ultra_typex_calib_bypass32" },
    };
    char extra[8][MAX_PATH]; int nextra = 0;
    for (unsigned e = 0; e < sizeof EXTRAS / sizeof EXTRAS[0]; e++) {
        if (!GetEnvironmentVariableA(EXTRAS[e].env, NULL, 0)) continue;
        if (nextra >= (int)(sizeof extra / sizeof extra[0])) break;
        if (find_extra_dll(EXTRAS[e].res, EXTRAS[e].base, extra[nextra], sizeof extra[nextra])) {
            if (!inject_dll(pi.hProcess, extra[nextra]))
                fprintf(stderr, "[taito_typex] %s inject failed (non-fatal)\n", EXTRAS[e].res);
            nextra++;
        }
    }

    ResumeThread(pi.hThread);
    fprintf(stderr, "[taito_typex] hook injected, game resumed -- waiting for exit\n");

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0; GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    DeleteFileA(hook);
    for (int e = 0; e < nextra; e++) DeleteFileA(extra[e]);
    fprintf(stderr, "[taito_typex] game exited with code %lu\n", rc);
    return (int)rc;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)hinst; (void)reason; (void)reserved;
    return TRUE;
}
