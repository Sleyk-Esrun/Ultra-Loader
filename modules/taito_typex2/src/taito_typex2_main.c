/*
 * taito_typex2_main.c -- Taito Type X2 spawner + hook injector.
 *
 * Same mechanism as taito_typex (suspended-launch DLL injection of the hook
 * before any game code runs). Type X2 games are 32-bit PE32 binaries with
 * D3D9.
 *
 * argv layout from the loader's dispatcher (work/src/main.c):
 *   argv[0] = ultra-loader.exe path
 *   argv[1] = game.exe path (the Type X2 game)
 *
 * Hook: this board's OWN independently compiled hook (built by
 * src/build_hook.sh from this board's own source tree, not copied from
 * modtest/ or native_x86's shared build). 2026-08-03: no longer installed as
 * a standalone file -- it's embedded as an RCDATA resource ("HOOK32") inside
 * THIS DLL (see hook_res.rc, baked in by build_module.sh) and extracted to a
 * fresh temp file right before each injection. Signing/verifying this one
 * DLL automatically covers the hook too, and no separate hook file sits on
 * disk between launches for anything to tamper with -- see
 * docs/PROJECT_LICENSING_AND_SIGNING.md. Caller must DeleteFileA() the
 * returned path once done with it. Only HOOK32 is embedded: this spawner
 * injects in-process (CreateProcess+CreateRemoteThread, both 32-bit) and has
 * no 64-bit code path -- build_hook.sh's ultra_taito_typex2_hook64.dll
 * output is unused by any code here, same as taito_typex's own board.
 */

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

static int find_hook(char *out, size_t cap)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)&find_hook, &self);
    if (!self) { fprintf(stderr, "[taito_typex2] GetModuleHandleEx(self) failed: %lu\n", GetLastError()); return 0; }

    HRSRC info = FindResourceA(self, "HOOK32", RT_RCDATA);
    if (!info) { fprintf(stderr, "[taito_typex2] embedded hook resource not found\n"); return 0; }
    DWORD sz = SizeofResource(self, info);
    HGLOBAL loaded = LoadResource(self, info);
    const void *ptr = loaded ? LockResource(loaded) : NULL;
    if (!ptr || !sz) { fprintf(stderr, "[taito_typex2] embedded hook resource empty\n"); return 0; }

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
            snprintf(out, cap, "%sultra_taito_typex2_hook32_%lu.dll", tmp_dir, GetCurrentProcessId());
            fp = fopen(out, "wb");
            if (fp) break;
            wl = strlen(why);
            snprintf(why + wl, sizeof why - wl, "%s%s", wl ? ", " : "", out);
        }
        /* Nothing took it.  Put the candidate list where the caller's existing
         * error message will print it -- "out" only means a path on success. */
        if (!fp) snprintf(out, cap, "%s", why[0] ? why : "(no candidate directory)");
    }
    if (!fp) { fprintf(stderr, "[taito_typex2] cannot write temp hook %s\n", out); return 0; }
    size_t wrote = fwrite(ptr, 1, sz, fp);
    fclose(fp);
    if (wrote != sz) { fprintf(stderr, "[taito_typex2] short write to temp hook %s\n", out); DeleteFileA(out); return 0; }
    fprintf(stderr, "[taito_typex2] extracted embedded hook (%lu bytes) -> %s\n", sz, out);
    return 1;
}

static int inject_dll(HANDLE proc, const char *dll_path)
{
    size_t n = strlen(dll_path) + 1;
    LPVOID remote = VirtualAllocEx(proc, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        fprintf(stderr, "[taito_typex2] VirtualAllocEx failed: %lu\n", GetLastError());
        return 0;
    }
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(proc, remote, dll_path, n, &wrote) || wrote != n) {
        fprintf(stderr, "[taito_typex2] WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    HMODULE kern = GetModuleHandleA("kernel32.dll");
    LPTHREAD_START_ROUTINE loadfn =
        (LPTHREAD_START_ROUTINE)(uintptr_t)GetProcAddress(kern, "LoadLibraryA");
    if (!loadfn) { VirtualFreeEx(proc, remote, 0, MEM_RELEASE); return 0; }
    HANDLE th = CreateRemoteThread(proc, NULL, 0, loadfn, remote, 0, NULL);
    if (!th) {
        fprintf(stderr, "[taito_typex2] CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    DWORD wr = WaitForSingleObject(th, 15000);
    DWORD remote_load_rc = 0;
    GetExitCodeThread(th, &remote_load_rc);
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    if (wr != WAIT_OBJECT_0) {
        fprintf(stderr, "[taito_typex2] remote LoadLibraryA timed out\n");
        return 0;
    }
    if (remote_load_rc == 0) {
        fprintf(stderr, "[taito_typex2] remote LoadLibraryA returned NULL\n");
        return 0;
    }
    fprintf(stderr, "[taito_typex2] remote LoadLibraryA OK (HMODULE=0x%lX)\n", remote_load_rc);
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

__declspec(dllexport) int ul_taito_typex2_run(int argc, char **argv)
{
    if (argc < 2 || !argv[1] || !*argv[1]) {
        fprintf(stderr, "[taito_typex2] ERROR: no game path (argc=%d)\n", argc);
        return 1;
    }

    /* Per-game gating for titles already proven on this board.
     * NOTE: if this board's own env setup below would overwrite a locked
     * key, move this call to AFTER that block (namco_es3_main.c does). */
    ul_apply_game_locks(argv[1]);
    const char *game = argv[1];
    fprintf(stderr, "[taito_typex2] launching %s\n", game);

    /* Derive workdir = dir(game). Must handle BOTH '\\' (typical Win32) and '/'
     * (forward-slash paths the loader CLI/dispatcher may hand us, e.g.
     * "I:/Modern Arcade Games/Taito Type X2 Dumps/..."). Splitting on '\\' only
     * left workdir equal to the full game path (including "game.exe") whenever
     * the incoming path used '/', so CreateProcessA's lpCurrentDirectory pointed
     * at a FILE, not a directory, and Win32 rejected it with ERROR_DIRECTORY
     * (267) -- the hook never got a chance to inject. Same bug, same fix as
     * taito_typex_main.c (fixed there first; this is the Type X2 copy of it). */
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
        fprintf(stderr, "[taito_typex2] ERROR: embedded hook could not be extracted\n");
        MessageBoxA(NULL,
            "Taito Type X2 hook could not be extracted from this module.\n\n"
            "The installed ultra_taito_typex2.dll may be corrupt -- reinstall it via\n"
            "Settings -> Install Module.",
            "Ultra Loader -- Type X2 Hook Missing",
            MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }
    fprintf(stderr, "[taito_typex2] hook = %s\n", hook);

    /* ULTRA_TYPEX2_WINDOWED=1 -- per the previous Haunted Museum / Wacky
     * Race bring-up, fullscreen-exclusive triggers a D3D9 device-LOST
     * stall; the hook upscales via a windowed render target instead.
     * ULTRA_SEGA_WINDOWED kept as a fallback for any Type X2 game whose
     * detection logic falls back to the Sega-family code path. */
    SetEnvironmentVariableA("ULTRA_BOARD_HINT", "taito_typex2");
    SetEnvironmentVariableA("ULTRA_SYSTEM_ID", "taito_typex2");
    /* ULTRA_BOARD -- the knob controls.c's ultra_input_map_install() actually
     * resolves the per-board control profile from (input_profiles.c ->
     * board_profiles_resolve() -> default_for_board()). This is DIFFERENT from
     * ULTRA_BOARD_HINT / ULTRA_SYSTEM_ID above -- controls.c never reads either
     * of those. Root-caused 2026-07-23 (same gap found in sega_europar_main.c):
     * this spawner never set it, so ultra_input_map_install() always fell
     * through to "generic_pc" (input_profiles.c g_generic: naxes=0, digital
     * buttons only). Wacky Race's steering/pedals never reached the JVS analog
     * channels regardless of wheel/pad/keyboard state -- only START/COIN/TEST
     * and the legacy xinput_thread demo (arrows/space/enter) worked, which is
     * how this board's "playable" verification was historically done.
     * Setting ULTRA_BOARD="taito_typex2" makes default_for_board() resolve the
     * racer genre (input_profiles.c) -- correct for Wacky Race. Type X2 also
     * hosts Haunted Museum (gun cab); since this board defaults to racer, a
     * gun-genre game needs a per-game ultra_controls.ini with
     * "[meta] genre = gun" + "screenpos = 1" dropped next to its exe (see
     * docs/X86_CONTROLS.md §5 and the bundled profiles/sega_alls.ini template)
     * to override the default -- the board-level env var alone can't express
     * "this specific game is the gun cab, not the racer". */
    SetEnvironmentVariableA("ULTRA_BOARD", "taito_typex2");
    SetEnvironmentVariableA("ULTRA_TYPEX2_WINDOWED", "1");
    SetEnvironmentVariableA("ULTRA_SEGA_WINDOWED", "1");
    /* MASTER input enable -- wakes up controls.c's full input-mapping layer
     * (XInput + DInput + full keyboard mapping). Without this only the
     * legacy demo path fires: arrows, space, enter. */
    SetEnvironmentVariableA("ULTRA_CONTROLS", "1");
    /* 2026-08-02: dinput8_emu.c's IAT hook on DirectInput8Create exists to
     * unblock games that refuse to boot without a working input device (see
     * its own header comment) -- Haunted Museum isn't one of those; its own
     * MUSEUM.ini has DINPUTENABLE=0 and it reads raw OS mouse/keyboard
     * directly, and it already boots and plays fine without any DirectInput
     * shim (confirmed on the pinned-binary baseline). Live test found that
     * installing it anyway (this board's from-source build compiles it in,
     * since hook_dllmain.c calls ultra_dinput8_install() unconditionally)
     * broke mouse aim and most keyboard input entirely -- our synthetic
     * device feeds empty/mapped data in place of whatever real input path
     * was actually working. Disabled by default here; a future title on this
     * board that genuinely needs the DirectInput8 shim to boot can set
     * ULTRA_NO_DINPUT8=0 to re-enable it. */
    if (!GetEnvironmentVariableA("ULTRA_NO_DINPUT8", NULL, 0))
        SetEnvironmentVariableA("ULTRA_NO_DINPUT8", "1");
    /* 2026-08-01: taito_typex (D3D8, same hook lineage) was confirmed via
     * live playtest to render a solid BLACK backbuffer whenever the
     * upscale/sharpen post-process path is active (game logic/audio/JVS all
     * ran fine; zero pixels) -- caused by the loader Settings dialog's
     * default Upscaler=FSR silently exporting ULTRA_SHARPEN/ULTRA_RES_W/H,
     * which nobody explicitly asked for. taito_typex2 wasn't independently
     * live-tested against this same failure mode this session, but it goes
     * through the same settings.c default-export path and d3d9_throttle.c
     * has the analogous sharpen/upscale post-process pass, so apply the same
     * defensive override here rather than risk shipping the same silent
     * black-screen trap. This does NOT touch ULTRA_TYPEX2_WINDOWED/
     * ULTRA_SEGA_WINDOWED above (that forced-windowed behavior is separately
     * proven correct for HM/Wacky Race's D3D9 device-LOST issue) -- it only
     * disables the sharpen/upscale post-process pass riding on top of it.
     * Opt out via ULTRA_TYPEX_UPSCALE=1 (same knob taito_typex uses). */
    if (!GetEnvironmentVariableA("ULTRA_TYPEX_UPSCALE", NULL, 0)) {
        SetEnvironmentVariableA("ULTRA_RES", "0x0");
        SetEnvironmentVariableA("ULTRA_RES_W", "");
        SetEnvironmentVariableA("ULTRA_RES_H", "");
        SetEnvironmentVariableA("ULTRA_SHARPEN", "0");
        SetEnvironmentVariableA("ULTRA_ROTATE", "0");
        SetEnvironmentVariableA("ULTRA_AA", "0");
    }
    /* 2026-08-02: this board now ships its from-source hook build (see
     * build_module.sh -- superseded the frozen pinned binary once the render
     * bug above was root-caused and fixed), which compiles typex2_gun.c's
     * OWN current feed_gun() -- fixed the same day to go fully inert (never
     * touch the cursor) whenever no live Omni-Track pose is present, so a
     * real mouse/gamepad-fed cursor is never fought. Default the lightgun
     * bridge ON: harmless with no lightgun connected, and typex2_gun.c also
     * now carries a gamepad->keyboard/mouse bridge (ULTRA_TYPEX2_GAMEPAD,
     * default ON there too) since Haunted Museum's own MUSEUM.ini disables
     * JVS/Gun/DirectInput and only reads raw OS mouse+keyboard -- a real
     * lightgun or XInput pad can only reach it by synthesizing that same raw
     * input, which is exactly what both bridges do. Opt out per-cabinet with
     * ULTRA_TYPEX2_GUN=0 / ULTRA_TYPEX2_GAMEPAD=0. */
    if (!GetEnvironmentVariableA("ULTRA_TYPEX2_GUN", NULL, 0))
        SetEnvironmentVariableA("ULTRA_TYPEX2_GUN", "1");

    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\"", game);
    /* Inherit handles so the child's stderr lands in our ultra.log. */
    if (!CreateProcessA(game, cmd, NULL, NULL, TRUE,
                        CREATE_SUSPENDED, NULL,
                        workdir[0] ? workdir : NULL, &si, &pi)) {
        fprintf(stderr, "[taito_typex2] CreateProcess(%s) failed: %lu\n",
                game, GetLastError());
        DeleteFileA(hook);
        return 1;
    }
    fprintf(stderr, "[taito_typex2] spawned pid=%lu (suspended)\n", pi.dwProcessId);

    if (!inject_dll(pi.hProcess, hook)) {
        fprintf(stderr, "[taito_typex2] hook inject failed; terminating game\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        DeleteFileA(hook);
        return 1;
    }

    ResumeThread(pi.hThread);
    fprintf(stderr, "[taito_typex2] hook injected, game resumed -- waiting for exit\n");

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0; GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    DeleteFileA(hook);
    fprintf(stderr, "[taito_typex2] game exited with code %lu\n", rc);
    return (int)rc;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h; (void)reason; (void)r;
    return TRUE;
}
