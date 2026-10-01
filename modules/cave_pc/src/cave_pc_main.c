/*
 * cave_pc_main.c -- Cave PC (Ver.A/Ver.B, Windows Embedded) spawner + hook
 * injector.
 *
 * Cave PC is the "shooters on a PC" arcade platform Cave shipped from ~2010
 * onward for their bullet-hell catalog. Known dominant titles (all shipping
 * as 32-bit PEs):
 *   - Deathsmiles II
 *   - DoDonPachi SaiDaiOuJou (original release, D3D9)
 *   - Akai Katana Shin
 *   - Espgaluda II Black Label
 *   - Muchi Muchi Pork!
 *   - Ketsui: Kizuna Jigoku Tachi (Cave PC port)
 * Only DoDonPachi SaiDaiOuJou "Peak" (the D3D11 remaster) ships as 64-bit;
 * that variant would need the Type X3-style ultra-inject64.exe helper (an
 * out-of-process 64->64 injector). Not required for the mainline catalog.
 *
 * Cave PC I/O posture:
 *   - JVS-over-serial (COM3 typical), digital 8-way stick + 3 buttons per side
 *   - NO analog, NO wheel, NO lightgun (shmups don't need any of those)
 *   - Presence-check USB dongle (SafeNet-class HASP-style)
 *   - ATA-locked HDD (skip that check via ULTRA_CAVE_NOATALOCK=1)
 *
 * The loader's dispatcher (work/src/main.c) calls ul_cave_pc_run(argc, argv)
 * after detecting a Cave PC .exe game with ULTRA_MODULE=cave_pc (or auto-
 * detected from the game's path). argv layout:
 *   argv[0] = ultra-loader.exe path
 *   argv[1] = game.exe path (the Cave PC game)
 *
 * What we do (identical mechanics to taito_typex_main.c):
 *   1. CreateProcess(game.exe, CREATE_SUSPENDED) so we can inject the hook
 *      DLL BEFORE any game code runs.
 *   2. Write the hook DLL path into the game's VA via VirtualAllocEx +
 *      WriteProcessMemory, then CreateRemoteThread(LoadLibraryA, dll_path)
 *      so the hook's DllMain installs first.
 *   3. Resume the game's main thread.
 *   4. WaitForSingleObject on the game process so the loader stays alive
 *      until the game exits. Return the game's exit code.
 *
 * Hook DLL: ultra_cave_pc_hook32.dll -- Cave PC's OWN, independent hook build
 * (modules/cave_pc/src/hook/), NOT the shared Type X hook. Until 2026-07-23
 * this module reused ultra_typex_hook32.dll (the multi-board hook also used
 * by Type X / Type X2 / Sega Europa-R / Konami PC); a controller fix made for
 * a different board on that shared binary regressed another board, so Cave
 * PC's COM3/CVKEY/coin-credit/keyboard-XInput input path -- plus the D3D9
 * present hook and dialog-dismiss it depends on to launch/render -- was
 * forked into modules/cave_pc/src/hook/ and now ships as its own dedicated
 * DLL. It shares no source file and no compiled binary with the Type X hook,
 * so a change made for any other board can never again affect Cave PC.
 *
 * 2026-08-02: the hook's INTERNAL architecture was rebuilt a third time --
 * back to the proven game_dsii.c + board_io_ops-based I/O engine (ported
 * SOURCE only, from modules/native_x86/src/hook/'s 2026-07-31 tree; still no
 * shared compiled binary -- see src/hook/build_cave_hook.sh's header for the
 * full timeline). This does not change anything in THIS file's own contract
 * with the hook (same DLL name, same env vars, same injection mechanics).
 *
 * Search order (same pattern as before, just a different filename):
 *   1. %LOCALAPPDATA%\Ultra Loader\modules\cave_pc\ultra_cave_pc_hook32.dll
 *   2. %LOCALAPPDATA%\Ultra Loader\modules\<any other installed module>\ultra_cave_pc_hook32.dll
 *   3. <loader_exe_dir>\ultra_cave_pc_hook32.dll
 *   4. <loader_exe_dir>\modtest\ultra_cave_pc_hook32.dll
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
    if (!self) { fprintf(stderr, "[cave_pc] GetModuleHandleEx(self) failed: %lu\n", GetLastError()); return 0; }

    HRSRC info = FindResourceA(self, "HOOK32", RT_RCDATA);
    if (!info) { fprintf(stderr, "[cave_pc] embedded hook resource not found\n"); return 0; }
    DWORD sz = SizeofResource(self, info);
    HGLOBAL loaded = LoadResource(self, info);
    const void *ptr = loaded ? LockResource(loaded) : NULL;
    if (!ptr || !sz) { fprintf(stderr, "[cave_pc] embedded hook resource empty\n"); return 0; }

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
            snprintf(out, cap, "%sultra_cave_pc_hook32_%lu.dll", tmp_dir, GetCurrentProcessId());
            fp = fopen(out, "wb");
            if (fp) break;
            wl = strlen(why);
            snprintf(why + wl, sizeof why - wl, "%s%s", wl ? ", " : "", out);
        }
        /* Nothing took it.  Put the candidate list where the caller's existing
         * error message will print it -- "out" only means a path on success. */
        if (!fp) snprintf(out, cap, "%s", why[0] ? why : "(no candidate directory)");
    }
    if (!fp) { fprintf(stderr, "[cave_pc] cannot write temp hook %s\n", out); return 0; }
    size_t wrote = fwrite(ptr, 1, sz, fp);
    fclose(fp);
    if (wrote != sz) { fprintf(stderr, "[cave_pc] short write to temp hook %s\n", out); DeleteFileA(out); return 0; }
    fprintf(stderr, "[cave_pc] extracted embedded hook (%lu bytes) -> %s\n", sz, out);
    return 1;
}

static int inject_dll(HANDLE proc, const char *dll_path)
{
    size_t n = strlen(dll_path) + 1;
    LPVOID remote = VirtualAllocEx(proc, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        fprintf(stderr, "[cave_pc] VirtualAllocEx failed: %lu\n", GetLastError());
        return 0;
    }
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(proc, remote, dll_path, n, &wrote) || wrote != n) {
        fprintf(stderr, "[cave_pc] WriteProcessMemory failed: %lu\n", GetLastError());
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
        fprintf(stderr, "[cave_pc] CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    DWORD wr = WaitForSingleObject(th, 15000);
    DWORD remote_load_rc = 0;
    GetExitCodeThread(th, &remote_load_rc);
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    if (wr != WAIT_OBJECT_0) {
        fprintf(stderr, "[cave_pc] remote LoadLibraryA timed out (wait=0x%lX)\n", wr);
        return 0;
    }
    if (remote_load_rc == 0) {
        fprintf(stderr, "[cave_pc] remote LoadLibraryA returned NULL (hook DllMain refused or DLL bad)\n");
        return 0;
    }
    fprintf(stderr, "[cave_pc] remote LoadLibraryA OK (HMODULE=0x%lX)\n", remote_load_rc);
    return 1;
}

typedef struct { DWORD pid; HWND result; } cave_find_ctx;
static BOOL CALLBACK cave_enum_wnd(HWND h, LPARAM lp) {
    cave_find_ctx* ctx = (cave_find_ctx*)lp;
    DWORD wpid = 0;
    GetWindowThreadProcessId(h, &wpid);
    if (wpid == ctx->pid && IsWindowVisible(h)) {
        char cls[64]; GetClassNameA(h, cls, sizeof cls);
        if (strcmp(cls, "UltraLoaderSplash") != 0) {
            ctx->result = h;
            return FALSE;
        }
    }
    return TRUE;
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

__declspec(dllexport) int ul_cave_pc_run(int argc, char **argv)
{
    if (argc < 2 || !argv[1] || !*argv[1]) {
        fprintf(stderr, "[cave_pc] ERROR: no game path (argc=%d)\n", argc);
        return 1;
    }

    /* Per-game gating for titles already proven on this board.
     * NOTE: if this board's own env setup below would overwrite a locked
     * key, move this call to AFTER that block (namco_es3_main.c does). */
    ul_apply_game_locks(argv[1]);
    const char *game = argv[1];
    fprintf(stderr, "[cave_pc] launching %s\n", game);

    /* Derive workdir = dir(game). Cave PC games load resources by relative
     * paths (data\, sound\, shader\) from the exe's parent, so CurrentDirectory
     * must be the game folder.
     *
     * Handle BOTH '\\' (Win32) and '/' (loader/CLI-supplied paths). */
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

    char hook[MAX_PATH];
    if (!find_hook(hook, sizeof hook)) {
        fprintf(stderr, "[cave_pc] ERROR: embedded hook could not be extracted\n");
        MessageBoxA(NULL,
            "Cave PC hook could not be extracted from this module.\n\n"
            "The installed ultra_cave_pc.dll may be corrupt -- reinstall it via\n"
            "Settings -> Install Module.\n\n"
            "(Cave PC has its own independent hook -- not shared with Type X.)",
            "Ultra Loader -- Cave PC Hook Missing",
            MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }
    fprintf(stderr, "[cave_pc] hook = %s\n", hook);

    /* Env vars the hook DLL + board_profile arm layer read at injection time.
     *
     * ULTRA_BOARD_HINT / ULTRA_SYSTEM_ID / ULTRA_BOARD drive board_profile_arm
     * to set the mixed D3D9+D3D11 present hook + JVS-serial I/O + dongle
     * bypass. */
    /* Enable hook-side diagnostic logging so we can see every hooked API
     * call the game makes (CreateFileA, Direct3DCreate9, ExitProcess, etc.).
     * Written to a file next to the game exe. */
    {
        char jlog[MAX_PATH];
        snprintf(jlog, sizeof jlog, "%s\\ultra_cave_jvs.log", workdir);
        /* Truncate old log so it doesn't grow unbounded across sessions */
        FILE* trunc = fopen(jlog, "w");
        if (trunc) fclose(trunc);
        SetEnvironmentVariableA("ULTRA_JVS_LOG", jlog);
        fprintf(stderr, "[cave_pc] JVS diag log -> %s\n", jlog);
    }

    SetEnvironmentVariableA("ULTRA_BOARD_HINT", "cave_pc");
    SetEnvironmentVariableA("ULTRA_SYSTEM_ID", "cave_pc");
    SetEnvironmentVariableA("ULTRA_BOARD", "cave_pc");

    /* D3D11 hook: OFF by default. The mainline Cave PC catalog is D3D9;
     * only SDOJ Peak (64-bit) uses D3D11. Arming the DXGI hook on a D3D9
     * game adds unnecessary IAT patching that can conflict with the game's
     * own imports. The user can set ULTRA_D3D11=1 explicitly for Peak. */

    /* Cave PC uses a PROPRIETARY serial protocol on COM3 - NOT standard JVS.
     * ULTRA_CAVE_IO=1 activates the Cave protocol handler in jvs_serial_hook.c
     * which responds with button state from our controls engine instead of
     * trying to parse the requests as standard JVS frames.
     * Claim ONLY COM3 (Cave's port). */
    SetEnvironmentVariableA("ULTRA_JVS_COM", "3");
    SetEnvironmentVariableA("ULTRA_CAVE_IO", "1");

    /* Cave PC games enumerate HID devices and may read gameplay input via
     * DirectInput8 or raw HID, not COM3 serial. COM3 is the cabinet system
     * board (coin/test/service). Let the hook's DInput8 layer run so our
     * input map feeds the game. The game's bundled dinput8.dll is just an
     * old (2001-era) copy of the system DLL -- not a third-party wrapper. */

    /* Dongle presence + dialog dismiss (SafeNet-class HASP-style presence
     * check + any MessageBox that would sit over the render surface). */
    if (!GetEnvironmentVariableA("ULTRA_DONGLE_BYPASS", NULL, 0))
        SetEnvironmentVariableA("ULTRA_DONGLE_BYPASS", "1");
    if (!GetEnvironmentVariableA("ULTRA_DISMISS_DIALOGS", NULL, 0))
        SetEnvironmentVariableA("ULTRA_DISMISS_DIALOGS", "1");

    /* Cave PC ships with an ATA-locked HDD; the game's startup path reads
     * the drive serial and refuses to boot if it doesn't match. Skip that
     * check via the module knob. */
    if (!GetEnvironmentVariableA("ULTRA_CAVE_NOATALOCK", NULL, 0))
        SetEnvironmentVariableA("ULTRA_CAVE_NOATALOCK", "1");

    /* Force windowed D3D9 present (avoids fullscreen-exclusive device-LOST
     * stall). ULTRA_TYPEX2_WINDOWED is what the D3D9 throttle reads.
     * Do NOT set ULTRA_SEGA_WINDOWED - that arms the GL present hook
     * (ChangeDisplaySettings interception) which conflicts on D3D9 games. */
    if (!GetEnvironmentVariableA("ULTRA_TYPEX2_WINDOWED", NULL, 0))
        SetEnvironmentVariableA("ULTRA_TYPEX2_WINDOWED", "1");

    /* 60 fps cap -- Cave PC is a fixed-60Hz target. Don't override if the
     * loader already set a value (user preference wins). */
    if (!GetEnvironmentVariableA("ULTRA_FPS_CAP", NULL, 0))
        SetEnvironmentVariableA("ULTRA_FPS_CAP", "60");

    /* Cave PC = shmup input profile: digital 8-way stick + 3 buttons
     * (shot / bomb / hyper). As of the 2026-08-02 rebuild the hook DLL is the
     * ported native_x86 board_io_ops engine, so ULTRA_CONTROLS=1 DOES arm
     * controls.c's generic multi-source input-mapping layer (ultra_input_map_
     * install() in hook_dllmain.c) -- unlike the from-scratch cave_io.c this
     * replaced, which never read it.
     *
     * CORRECTION (2026-08-02, same day): an earlier version of this comment
     * claimed game_dsii.c's cave_io_respond()/cave_direct_write_thread() do
     * NOT consume controls.c's output. That was WRONG -- traced the actual
     * code: cave_io_respond() calls jvs_get_input(node,&in) at its top and
     * ORs in.player[0]'s mapped bits (START/SERVICE/UP/DOWN/LEFT/RIGHT/
     * PUSH1/PUSH2, exactly controls.c's jvs_btn_bit() layout) into cave_btn/
     * cave_sys ADDITIVELY alongside its own separate direct GetAsyncKeyState/
     * XInputGetState reads. So controls.c's multi-source layer (keyboard,
     * mouse, XInput, DirectInput, gun, autopilot) DOES feed DSII's COM3
     * response live, every poll -- it is not a dead/unrelated path for this
     * board. See input_src_dinput.c's 2026-08-02 POV-hat fix: that source
     * previously only read discrete buttons + continuous axes, never the POV
     * hat, so a real DirectInput device that reports its D-pad via POV (common
     * on arcade sticks) fed NOTHING into this pipeline for UP/DOWN/LEFT/RIGHT
     * regardless of profile config -- a likely real contributor to "controls
     * never worked" reports, independent of this env var. Left ULTRA_CONTROLS
     * set to match the proven upstream configuration; do not turn it off to
     * "simplify" -- doing so would remove a real, now-more-complete input path. */
    SetEnvironmentVariableA("ULTRA_CONTROLS", "1");

    /* Deathsmiles II detection: 3,616,768 byte game.exe.
     * DSII's input_poll (0x45c370) has a one-shot guard [ESI+4] that locks
     * COM3 out after the first frame - direct-write bypass required.
     * ULTRA_CAVE_DSII=1 enables the cave_direct_write_thread in the hook. */
    {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA(game, GetFileExInfoStandard, &fa) &&
            fa.nFileSizeHigh == 0 && fa.nFileSizeLow == 3616768 /* 0x374C00 */) {
            SetEnvironmentVariableA("ULTRA_CAVE_DSII", "1");
            fprintf(stderr, "[cave_pc] detected Deathsmiles II: direct-write input bypass enabled\n");
        }
    }

    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\"", game);
    /* bInheritHandles=TRUE so the child's CRT inherits our STD_ERROR_HANDLE
     * (the freopen'd ultra.log). Hook fprintf(stderr,...) lines land in the
     * same log file the loader writes. */
    if (!CreateProcessA(game, cmd, NULL, NULL, TRUE,
                        CREATE_SUSPENDED, NULL,
                        workdir[0] ? workdir : NULL, &si, &pi)) {
        fprintf(stderr, "[cave_pc] CreateProcess(%s) failed: %lu\n",
                game, GetLastError());
        DeleteFileA(hook);
        return 1;
    }
    fprintf(stderr, "[cave_pc] spawned pid=%lu (suspended)\n", pi.dwProcessId);

    if (!inject_dll(pi.hProcess, hook)) {
        fprintf(stderr, "[cave_pc] hook inject failed; terminating game\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        DeleteFileA(hook);
        return 1;
    }

    /* The game's bundled dinput8.dll is an old (2001-era) copy of the system
     * DirectInput8 DLL - NOT a third-party wrapper. The hook DLL's IAT patch for
     * DirectInput8Create feeds our input map to the game when it creates
     * DInput8 devices via its own init path. */

    ResumeThread(pi.hThread);
    fprintf(stderr, "[cave_pc] hook injected, game resumed -- waiting for exit\n");

    /* Find the game's render window and make it borderless-fullscreen +
     * foreground. This runs from the LOADER process (which owns the
     * foreground token), so SetForegroundWindow actually works. The game
     * needs a few seconds to create its D3D9 device + window, so we poll
     * with a timeout. We do this in a background thread so the main thread
     * can sit on WaitForSingleObject for the game exit.
     *
     * HEADLESS TEST MODE (2026-08-02, incident fix): this whole block runs
     * from the LOADER's own process, completely independent of the hook
     * DLL's in-process off-screen positioning (see d3d9_throttle.c's
     * ultra_cave_test_headless()). That in-process fix was landed to stop
     * automated test runs from putting a real window on the user's screen
     * -- but THIS code finds that same window via EnumWindows a few seconds
     * later and unconditionally forces it back to real (0,0) fullscreen +
     * SetForegroundWindow, undoing the in-process fix and putting a real,
     * visible (and foreground-stealing) window on the desktop regardless.
     * Live-confirmed: a headless test run still flashed a real window
     * because of this exact block. Skip this entire step when
     * ULTRA_CAVE_TEST_HEADLESS=1 -- the game's own window still exists
     * (off-screen, per the in-process fix) so CreateDevice etc. all still
     * succeed identically; we just never go looking for it to drag it back
     * onto the real screen. Off by default (unset), so normal play is
     * byte-identical to before this fix. */
    {
        const char* headless_e = getenv("ULTRA_CAVE_TEST_HEADLESS");
        int headless = (headless_e && headless_e[0] == '1');
        if (headless) {
            fprintf(stderr, "[cave_pc] headless test mode -- SKIPPING foreground/fullscreen window takeover\n");
        } else {
        HWND game_wnd = NULL;
        for (int i = 0; i < 40; ++i) {
            Sleep(500);
            if (WaitForSingleObject(pi.hProcess, 0) != WAIT_TIMEOUT)
                break;
            cave_find_ctx ctx = { pi.dwProcessId, NULL };
            EnumWindows(cave_enum_wnd, (LPARAM)&ctx);
            if (ctx.result) { game_wnd = ctx.result; break; }
        }
        if (game_wnd) {
            int dw = GetSystemMetrics(SM_CXSCREEN);
            int dh = GetSystemMetrics(SM_CYSCREEN);
            SetWindowLongA(game_wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(game_wnd, HWND_TOP, 0, 0, dw, dh,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            SetForegroundWindow(game_wnd);
            BringWindowToTop(game_wnd);
            fprintf(stderr, "[cave_pc] game window %p -> borderless %dx%d, foreground\n",
                    (void*)game_wnd, dw, dh);
        } else {
            fprintf(stderr, "[cave_pc] could not find game window (pid=%lu)\n",
                    (unsigned long)pi.dwProcessId);
        }
        }
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0; GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    DeleteFileA(hook);
    fprintf(stderr, "[cave_pc] game exited with code %lu\n", rc);
    return (int)rc;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)hinst; (void)reason; (void)reserved;
    return TRUE;
}
