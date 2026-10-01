/*
 * sega_europar_main.c -- Sega Europa-R spawner + hook injector.
 *
 * The loader's dispatcher (work/src/main.c) calls ul_sega_europar_run(argc, argv)
 * after detecting a Sega Europa-R .exe game with ULTRA_MODULE=sega_europar
 * (or auto-detected from the game's path / board profile). argv layout:
 *   argv[0] = ultra-loader.exe path
 *   argv[1] = game.exe path (the Europa-R game, e.g. fordracing.exe)
 *
 * Europa-R is an x86 arcade board (Sega PC-class platform, Win Embedded 2009,
 * D3D9 graphics, keychip + container protection). The board profile
 * (modules/native_x86/src/hook/board_profile.c, BP_SEGA_EUROPAR entry) marks
 * it as BP_GFX_D3D9 / BP_INJECT_SPAWN_SUSPENDED / 32-bit / BP_HOOK_D3D9 with
 * BP_BASE_SEGA_RINGEDGE for keychip & TrueCrypt reuse.
 *
 * TWO SEPARATE, INDEPENDENT payloads get put in front of the game, by two
 * separate mechanisms -- this split is deliberate (2026-07-23 controls
 * rework; see docs/STOCK_TAKE.md / docs/X86_BOARDS.md "Sega Europa-R" rows):
 *
 *   1. RENDERING (injected DLL, HOOK_DLL_NAME): a PRIVATE, board-pinned copy
 *      of the shared Type X hook -- ultra_sega_europar_hook32.dll, byte-
 *      identical to the confirmed-working ultra_typex_hook32.dll build this
 *      board tested against, just installed under a board-exclusive
 *      filename so a future rebuild of modules/native_x86/src/hook/ for some
 *      OTHER board can never again silently change what Ford Racing renders
 *      with (that is exactly how a Konami PC controller fix regressed this
 *      board earlier the same day). Provides the D3D9 present/force-
 *      windowed hook (ULTRA_TYPEX2_WINDOWED) and the keychip/inpout32
 *      DRM shims. It also happens to contain that shared codebase's JVS/IO4/
 *      DirectInput8 input machinery, but that machinery is INERT for this
 *      game either way (see point 2), so leaving it compiled in is harmless.
 *   2. CONTROLS (file swap, DINPUT8_CRK_NAME): a dedicated, Europa-R-only
 *      DirectInput8 proxy (src/hook/dinput8_europar.c) dropped into the
 *      game's OWN folder as "dinput8.dll" before CreateProcess -- NOT
 *      injected, NOT part of native_x86/src/hook/ at all. Root-caused by
 *      reading fordracing.exe's actual PE import table + its own bundled
 *      CTRLLERS.CDB controller-database data file (see that file's header
 *      comment for the full trace): the game loads dinput8.dll DYNAMICALLY
 *      (LoadLibraryA + GetProcAddress, no static IAT slot), which is exactly
 *      the one loading pattern the shared hook's IAT-patch-based
 *      DirectInput8Create hook structurally cannot intercept -- that is why
 *      no combination of ULTRA_BOARD / ULTRA_CONTROLS / ULTRA_IO4 knobs ever
 *      made Ford Racing respond to input. Win32's DLL search order checks
 *      the application directory before System32, so a same-named file
 *      swapped into the game folder is found by the game's own dynamic load
 *      with zero injection needed. This is the SAME technique
 *      konami_pc/src/dinput8_crk.c already uses (read as a reference for the
 *      technique only; this file is an independent implementation).
 *
 * What we do launch-mechanism-wise (mirrors taito_typex_main.c 1:1 with
 * WOW64-aware tweaks; UNCHANGED since before the controls rework above):
 *   1. CreateProcess(game.exe, CREATE_SUSPENDED) so the hook installs BEFORE
 *      any game code resolves D3D9 / keychip / IO4 entry points. Packed
 *      Europa-R titles do dynamic GetProcAddress sweeps during early startup
 *      -- a post-launch LoadLibrary injection consistently loses the race.
 *   2. Queue an APC for LoadLibraryA(hook_dll_path) on the main thread, so
 *      the hook's DllMain installs the moment the 32-bit WOW64 PE loader
 *      reaches its first user-APC alertable point (right after
 *      LdrpInitializeProcess maps kernel32/user32 etc). We previously used
 *      CreateRemoteThread on the suspended process; on WOW64 this loses the
 *      race -- the suspended thread is parked at the 64-bit LdrInitializeThunk
 *      and the 32-bit ntdll/kernel32 aren't mapped yet, so a remote thread
 *      created from the 64-bit loader side runs LoadLibraryA against an
 *      uninitialised WOW64 image and the game hangs in WOW64 init forever
 *      (symptom: process shows only ntdll64+wow64.dll, no kernel32/user32,
 *      CPU climbing slowly = WOW64 loader spinning on a missing IAT).
 *      QueueUserAPC is the documented WOW64-correct hook-inject path.
 *   3. ResumeThread on the game's main thread. The 32-bit loader runs,
 *      reaches the alertable point, fires the APC, LoadLibraryA(hook) runs
 *      with kernel32 fully mapped. As a belt-and-suspenders fallback we
 *      also CreateRemoteThread *after* a brief sleep that lets WOW64 init
 *      finish, in case APC dispatch was somehow delayed.
 *   4. WaitForSingleObject(INFINITE) so the loader stays alive until the
 *      game exits. Return the game's exit code.
 *
 * Launch target choice (Ford Racing dump): the Europa-R cabinet ships a thin
 * operator shell (GameShell.exe, 258 KB, Jul 2006) that historically wraps the
 * real game (fordracing.exe, 1.25 MB, Jun 2006) for keychip presence checks +
 * FFB wheel calibration. We SKIP the shell and launch the game exe directly:
 * GameShell.exe's Install.ini hard-codes Directory=D:\sega\GameShell, so on
 * any user machine without the shipping D: install image it sits at its own
 * splash forever waiting for the install verifier. fordracing.exe under
 * FordRacing/ is self-contained (3DDATA/, SHADERS/, NEWGFX/, SND/, ... all
 * alongside the exe). Setting workdir = dir(game) makes the relative asset
 * lookups resolve.
 *
 * Rendering hook DLL: HOOK_DLL_NAME (ultra_sega_europar_hook32.dll, a private
 * board-pinned copy -- see point 1 above). Search order:
 *   1. %LOCALAPPDATA%\Ultra Loader\modules\sega_europar\<HOOK_DLL_NAME>
 *   2. <loader_exe_dir>\<HOOK_DLL_NAME>
 *   3. <loader_exe_dir>\modtest\<HOOK_DLL_NAME>
 * Controls proxy DLL: DINPUT8_CRK_NAME (dinput8.CRK.dll -- see point 2
 * above), searched the same 3 locations, then copied into the GAME's own
 * folder as "dinput8.dll" (any pre-existing one backed up to .orig once).
 *
 * Env vars set BEFORE CreateProcess (all read by the rendering hook at
 * injection time; all default off when unset, so they only turn on for
 * Europa-R). NOTE: the input-mapping knobs the shared hook's controls.c /
 * JVS layer would read (ULTRA_CONTROLS, ULTRA_BOARD, ULTRA_IO4,
 * ULTRA_JVS_ANALOG) are DELIBERATELY NOT SET here any more -- controls now
 * come entirely from the independent dinput8.dll proxy (point 2 above), and
 * that shared layer is not to be depended on for this board at all going
 * forward (2026-07-23 decision; a controller fix made in it for one board
 * regressed a different board that also shared the same compiled hook):
 *   ULTRA_BOARD_HINT    = "sega_europar"  -- selects Europa-R branch in hook
 *   ULTRA_SYSTEM_ID     = "sega_europar"  -- boot console + downstream hooks
 *   ULTRA_LAUNCH_MODULE = "sega_europar"  -- dispatcher / log tagging
 *   ULTRA_SEGA_WINDOWED = "1"             -- force windowed-mode rendering so
 *                                             the D3D9 present hook can upscale
 *                                             the native back-buffer to user's
 *                                             chosen output res (FSR/framegen).
 *                                             Without this Europa-R runs
 *                                             fullscreen-exclusive and the user
 *                                             sees a black 1920x1080 with the
 *                                             tiny game hidden.
 *   ULTRA_SEGA_KEYCHIP  = "1"             -- enables the hook's mxparallel
 *                                             keychip-presence emulator so the
 *                                             game's keychip check passes
 *                                             without real hardware.
 *   ULTRA_INPOUT32_BYPASS = "1"           -- tell the hook to neutralize
 *                                             inpout32.dll (Europa-R titles
 *                                             ship inpout32.dll for parallel-
 *                                             port keychip I/O; its DllMain
 *                                             opens a privileged kernel
 *                                             driver handle that does not
 *                                             exist on user machines, which
 *                                             can crash WOW64 init).
 */

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

/* Dedicated, independent DirectInput8 controls proxy -- see src/hook/
 * dinput8_europar.c. File-swapped into the game's own folder as
 * "dinput8.dll", NOT injected -- see header for why. Still a real on-disk
 * search (find_module_file() below): this is a genuinely different
 * mechanism from the injected rendering hook (point 2 in the file header),
 * not covered by the embedded-resource change below. */
#define DINPUT8_CRK_NAME "dinput8.CRK.dll"

/* Tag every diagnostic line "[sega_europar] ..." then flush, so the lines
 * reach ultra.log immediately rather than sitting in stderr's buffer until
 * the child eventually flushes it on exit. The verifier reported seeing
 * "[sega_europar] launching ..." and "[sega_europar] hook = ..." but nothing
 * from inject_dll -- that's pure stderr-buffer drag; the inject WAS failing
 * but the diagnostic line never made it out. */
#define LOG(...) do { fprintf(stderr, "[sega_europar] " __VA_ARGS__); fflush(stderr); } while (0)

/* Generalized module-file finder (was find_hook(), hardcoded to HOOK_DLL_NAME
 * -- parameterized so the same proven search order also locates
 * DINPUT8_CRK_NAME). Search order:
 *   1. %LOCALAPPDATA%\Ultra Loader\modules\sega_europar\<filename>
 *   2. <loader_exe_dir>\<filename>
 *   3. <loader_exe_dir>\modtest\<filename>
 *
 * FIXED 2026-08-04 (real, live bug -- not theoretical): step 1 used to scan
 * EVERY subdirectory under %LOCALAPPDATA%\Ultra Loader\modules\ for a file
 * named `filename`, not just this board's own. That's safe for a uniquely-
 * named file like ultra_sega_europar_hook32.dll, but DINPUT8_CRK_NAME
 * ("dinput8.CRK.dll") is a GENERIC name several boards' own CRK-swap
 * mechanisms reuse (konami_pc among them) -- FindFirstFileA/FindNextFileA
 * enumerate subdirectories in whatever order the filesystem returns (roughly
 * alphabetical on NTFS), and "konami_pc" sorts before "sega_europar", so this
 * board was silently swapping KONAMI PC's Silent Hill: The Arcade input
 * proxy into Ford Racing's folder instead of its own -- confirmed live:
 * byte-for-byte identical to %LOCALAPPDATA%\...\modules\konami_pc\
 * dinput8.CRK.dll, wrong device entirely, explaining "renders and plays
 * sound, zero controls work" independent of anything in dinput8_europar.c
 * itself. Scoped to this board's own subfolder only; no other board's
 * same-named file can ever be picked up again. */
static int find_module_file(const char *filename, char *out, size_t cap)
{
    char loader_exe[MAX_PATH];
    GetModuleFileNameA(NULL, loader_exe, MAX_PATH);
    char loader_dir[MAX_PATH];
    strncpy(loader_dir, loader_exe, MAX_PATH - 1); loader_dir[MAX_PATH - 1] = 0;
    char *bs = strrchr(loader_dir, '\\'); if (bs) *bs = 0;

    char appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appdata))) {
        snprintf(out, cap, "%s\\Ultra Loader\\modules\\sega_europar\\%s", appdata, filename);
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 1;
    }
    snprintf(out, cap, "%s\\%s", loader_dir, filename);
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 1;
    snprintf(out, cap, "%s\\modtest\\%s", loader_dir, filename);
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 1;
    return 0;
}

/* The RENDERING hook (point 1 in the file header) is no longer installed as
 * a standalone file. It's embedded as an RCDATA resource ("HOOK32") inside
 * THIS DLL (see hook_res.rc, baked in by build_module.sh) and extracted to a
 * fresh temp file right before each injection. This means signing/verifying
 * this one DLL automatically covers the hook too, and there's no separate
 * hook file sitting on disk between launches for anything to tamper with --
 * see docs/PROJECT_LICENSING_AND_SIGNING.md's hook-DLL note (2026-08-02).
 * Caller must DeleteFileA() the returned path once done with it. The
 * CONTROLS proxy (point 2, DINPUT8_CRK_NAME) is a completely separate
 * mechanism -- see find_module_file() above -- and is NOT affected by this. */
static int extract_hook_to_temp(char *out, size_t cap)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)&extract_hook_to_temp, &self);
    if (!self) { LOG("GetModuleHandleEx(self) failed: %lu\n", GetLastError()); return 0; }

    HRSRC info = FindResourceA(self, "HOOK32", RT_RCDATA);
    if (!info) { LOG("embedded hook resource not found\n"); return 0; }
    DWORD sz = SizeofResource(self, info);
    HGLOBAL loaded = LoadResource(self, info);
    const void *ptr = loaded ? LockResource(loaded) : NULL;
    if (!ptr || !sz) { LOG("embedded hook resource empty\n"); return 0; }

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
            snprintf(out, cap, "%sultra_sega_europar_hook32_%lu.dll", tmp_dir, GetCurrentProcessId());
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
    LOG("extracted embedded hook (%lu bytes) -> %s\n", sz, out);
    return 1;
}

/* Controls: copy the dedicated dinput8.dll proxy (src/hook/dinput8_europar.c,
 * built as DINPUT8_CRK_NAME) into the GAME's own folder as "dinput8.dll" --
 * NOT injected. Win32's DLL search order checks the application directory
 * before System32, so the game's own dynamic LoadLibraryA("dinput8.dll")
 * (Ford Racing never has a static IAT slot for it -- see the .c file header)
 * finds our proxy without any process injection at all. Mirrors
 * konami_pc/src/konami_pc_main.c's konami_di8_swap_in() pattern: a
 * pre-existing dinput8.dll (there isn't one in the Ford Racing dump today)
 * is backed up once to dinput8.dll.orig and never clobbered again. Safe to
 * call even when DINPUT8_CRK_NAME isn't installed -- logs a warning and
 * leaves the game to fall back to the real system DirectInput8 (which only
 * sees a real, physically-attached, game-recognized wheel/pad). */
static void sega_europar_dinput8_swap_in(const char *workdir)
{
    /* PER-GAME OFF SWITCH, added 2026-09-20 for Sega Rally 3.
     *
     * This proxy is FORD RACING's. It answers EnumDevices with one synthetic
     * Formula-GP-Wheel joystick and never falls through to the real devices, so
     * dropping it into another game's folder HIDES every physical DirectInput
     * device from that game. Rally.exe additionally STATICALLY imports
     * DINPUT8.dll!DirectInput8Create, and dinput8.dll is not a KnownDLL, so
     * Windows resolves it from the application directory -- ours wins before
     * System32 is ever consulted. Rally drives from raw HID (see the per-game
     * identity block in happio_europar.c), so this proxy buys it nothing and
     * actively blinds it.
     *
     * When disabled we also CLEAN UP, because the swap is sticky: a copy left
     * behind by an earlier launch keeps being loaded forever afterwards. Both
     * files in the Rally dump were verified to be OUR OWN builds (its .orig is
     * our 2026-08-13 output, not a vendor file), so removing them returns the
     * game to the real system DirectInput8 rather than destroying an original.
     *
     *   ULTRA_EUROPAR_DINPUT8 = 0   do not swap, and undo a previous swap */
    {
        /* GetEnvironmentVariableA, NOT getenv(). The loader publishes container keys
         * with SetEnvironmentVariableA, which writes the OS environment block but NOT
         * the CRT's own cached copy -- and getenv() reads the cache. This code runs in
         * the LOADER process, whose CRT snapshotted that cache long before the
         * container was applied, so a getenv() here NEVER sees a container key. It
         * silently read empty and the swap went ahead anyway, with the log line
         * '[UFW] env ULTRA_EUROPAR_DINPUT8=0' printed immediately above it.
         * Hook code gets away with getenv() because the game process INHERITS a
         * finished environment block at CreateProcess time; loader-process code does
         * not. Every other env read in this file already uses the Win32 call. */
        char dis[16] = {0};
        GetEnvironmentVariableA("ULTRA_EUROPAR_DINPUT8", dis, sizeof dis);
        if (dis[0] && atoi(dis) == 0) {
            char d[MAX_PATH], o[MAX_PATH];
            snprintf(d, sizeof d, "%s\\dinput8.dll",      workdir);
            snprintf(o, sizeof o, "%s\\dinput8.dll.orig", workdir);
            if (GetFileAttributesA(d) != INVALID_FILE_ATTRIBUTES && DeleteFileA(d))
                LOG("controls: ULTRA_EUROPAR_DINPUT8=0 -- removed a previously swapped dinput8.dll\n");
            if (GetFileAttributesA(o) != INVALID_FILE_ATTRIBUTES && DeleteFileA(o))
                LOG("controls: removed dinput8.dll.orig (verified to be our own earlier build)\n");
            LOG("controls: dinput8 proxy DISABLED here; the game sees the real system DirectInput8\n");
            return;
        }
    }
    char src[MAX_PATH];
    if (!find_module_file(DINPUT8_CRK_NAME, src, sizeof src)) {
        LOG("WARNING: %s not found (checked %%LOCALAPPDATA%%\\Ultra Loader\\modules\\*, "
            "loader dir, loader dir\\modtest) -- controls will not work; the game will "
            "see the real system DirectInput8 instead\n", DINPUT8_CRK_NAME);
        return;
    }

    char dll[MAX_PATH], bak[MAX_PATH];
    snprintf(dll, sizeof dll, "%s\\dinput8.dll",      workdir);
    snprintf(bak, sizeof bak, "%s\\dinput8.dll.orig", workdir);

    if (GetFileAttributesA(dll) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES) {
        if (!CopyFileA(dll, bak, TRUE))
            LOG("warn: could not back up dinput8.dll -> .orig (err=%lu)\n", GetLastError());
        else
            LOG("backed up pre-existing dinput8.dll -> dinput8.dll.orig\n");
    }

    if (!CopyFileA(src, dll, FALSE)) {
        LOG("ERROR: %s -> %s failed (err=%lu) -- controls will not work\n",
            src, dll, GetLastError());
        return;
    }
    LOG("controls: %s -> %s (dedicated Europa-R wheel+keyboard proxy, "
        "independent of the shared JVS/IO4 layer)\n", src, dll);
}

/* Inject by queuing a user APC for LoadLibraryA on the suspended main
 * thread. The APC fires the first time the thread enters an alertable wait
 * inside the 32-bit loader -- by then ntdll32+kernel32 are mapped and
 * LoadLibraryA resolves correctly. This is the WOW64-correct path; the
 * old CreateRemoteThread-on-CREATE_SUSPENDED variant hangs Europa-R
 * because fordracing.exe + inpout32.dll never reach an alertable state
 * before the remote thread's LoadLibraryA dereferences un-mapped IATs.
 *
 * As a belt-and-suspenders fallback, after we ResumeThread we also try a
 * post-init CreateRemoteThread once we've slept long enough for WOW64 to
 * have brought the 32-bit loader up. */
static int inject_dll_apc(HANDLE proc, HANDLE thread, const char *dll_path)
{
    size_t n = strlen(dll_path) + 1;
    LPVOID remote = VirtualAllocEx(proc, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        LOG("VirtualAllocEx failed: %lu\n", GetLastError());
        return 0;
    }
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(proc, remote, dll_path, n, &wrote) || wrote != n) {
        LOG("WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    HMODULE kern = GetModuleHandleA("kernel32.dll");
    PAPCFUNC loadfn = (PAPCFUNC)(uintptr_t)GetProcAddress(kern, "LoadLibraryA");
    if (!loadfn) {
        LOG("GetProcAddress(LoadLibraryA) failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    DWORD qrc = QueueUserAPC(loadfn, thread, (ULONG_PTR)remote);
    if (!qrc) {
        LOG("QueueUserAPC failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    LOG("APC queued for LoadLibraryA(%s) on tid=?? remote=%p\n", dll_path, remote);
    /* Note: we deliberately do NOT VirtualFreeEx here -- the APC fires after
     * we return, and the string at `remote` must still be valid when
     * LoadLibraryA reads it. Leak is 1 small page, lives until process exit. */
    return 1;
}

/* Belt-and-suspenders second inject path: post-resume, after WOW64 has had
 * a moment to map the 32-bit loader, fire a CreateRemoteThread(LoadLibraryA).
 * If the APC already loaded the hook, this is a no-op (LoadLibraryA returns
 * the existing HMODULE and DllMain isn't re-entered). If the APC was eaten
 * by some race, this catches it. */
static int inject_dll_remote_thread(HANDLE proc, const char *dll_path)
{
    size_t n = strlen(dll_path) + 1;
    LPVOID remote = VirtualAllocEx(proc, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        LOG("[fallback] VirtualAllocEx failed: %lu\n", GetLastError());
        return 0;
    }
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(proc, remote, dll_path, n, &wrote) || wrote != n) {
        LOG("[fallback] WriteProcessMemory failed: %lu\n", GetLastError());
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
        LOG("[fallback] CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    DWORD wr = WaitForSingleObject(th, 15000);
    DWORD remote_load_rc = 0;
    GetExitCodeThread(th, &remote_load_rc);
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    if (wr != WAIT_OBJECT_0) {
        LOG("[fallback] remote LoadLibraryA timed out (wait=0x%lX)\n", wr);
        return 0;
    }
    if (remote_load_rc == 0) {
        LOG("[fallback] remote LoadLibraryA returned NULL (hook DllMain refused or DLL bad)\n");
        return 0;
    }
    LOG("[fallback] remote LoadLibraryA OK (HMODULE=0x%lX)\n", remote_load_rc);
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

/* Put the screen back after we KILL a game.
 *
 * This is what actually held the owner's desktop on 2026-09-20, three times.
 * The watchdog was working -- the log shows 'game exited with code 4', which is
 * our own TerminateProcess -- but GameShell changes the display mode on startup,
 * and a TERMINATED process never restores it. Windows only puts the mode back
 * when a process exits cleanly, so the change outlives the kill, explorer is
 * stranded in it, and the taskbar disappears. Restarting explorer fixed it every
 * time, which is the tell.
 *
 * ChangeDisplaySettings(NULL, 0) is the documented way to return to the mode in
 * the registry. Harmless when nothing changed it, so it is safe to call on every
 * kill path. We do NOT call it when a game exits on its own -- that game already
 * restored its own mode, and a redundant switch would just make the screen blink.
 */
static void europar_restore_display(void)
{
    ChangeDisplaySettingsA(NULL, 0);
}

__declspec(dllexport) int ul_sega_europar_run(int argc, char **argv)
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

    /* ---- LAUNCH THE OPERATOR SHELL INSTEAD OF THE GAME -------------------
     *
     * Ford Racing IS a credits game, but fordracing.exe holds NO coin input and
     * NO credit arithmetic. The coin mech, the credit arithmetic, the persisted
     * counters and the operator/service menu all live in GameShell.exe, which
     * drives the game over WM_COPYDATA. With no shell running, the game's own
     * sega_init() at 0x00467C1C unconditionally writes FreePlay=1 and 99
     * credits -- which is why the cabinet only ever showed FREE PLAY.
     *
     * So to get real credits and a real service menu, the shell has to run.
     * ULTRA_EUROPAR_GAMESHELL=1 swaps the launch target to GameShell.exe in the
     * SAME dump, and the shell then starts the game itself.
     *
     * The workdir follows automatically (it is derived from dir(game) below),
     * and that matters more than it looks: GameShell's bitmap paths are patched
     * from absolute d:\\sega\\... to RELATIVE resources\\graphics\\..., so they
     * resolve against the working directory. Point it anywhere else and the
     * shell loads no graphics at all.
     *
     * OFF BY DEFAULT. Unset, the direct-launch path is byte-for-byte what it
     * was, so Ford Racing's proven free-play route is untouched. */
    char gs_path[MAX_PATH];
    {
        char want[8] = {0};
        GetEnvironmentVariableA("ULTRA_EUROPAR_GAMESHELL", want, sizeof want);
        if (want[0] && atoi(want) != 0) {
            char root[MAX_PATH];
            char *cut;
            size_t n = strlen(game);
            if (n < sizeof root) {
                memcpy(root, game, n + 1);
                /* game is <dump>\\FordRacing\\fordracing.exe -- climb two levels */
                cut = strrchr(root, '\\'); if (cut) *cut = 0;
                cut = strrchr(root, '\\'); if (cut) *cut = 0;
                snprintf(gs_path, sizeof gs_path, "%s\\GameShell\\GameShell.exe", root);
                if (GetFileAttributesA(gs_path) != INVALID_FILE_ATTRIBUTES) {
                    LOG("gameshell: launching the operator shell instead of the game -> %s\n", gs_path);
                    game = gs_path;
                    /* THE SHELL LAUNCH PROTECTS ITSELF. Set in the CHILD's environment, which it
                     * inherits at CreateProcess -- the container does not have to know.
                     *
                     * HAPPIO OFF: on 2026-09-20 the shell froze the DESKTOP the instant it opened
                     * our synthetic HAPP device -- the log's last line was 'opened synthetic HAPP
                     * UGCI device' and nothing followed. GameShell's coin input is a SEPARATE
                     * 4-byte UGCI report, not the 8-byte wheel report we serve, so its coin thread
                     * blocked on a read that was never going to be satisfied. The shell does not
                     * need a wheel; it needs a coin mech. Give it neither until that report is
                     * mapped, rather than something that wedges it.
                     *
                     * PANIC KEY ON: Ctrl+Alt+Q always kills it, from a thread that does not need a
                     * message pump. The Ultra overlay could not help there -- it is pumped by the
                     * game's own loop, which was the thing that was stuck. */
                    SetEnvironmentVariableA("ULTRA_EUROPAR_HAPPIO", "0");
                    SetEnvironmentVariableA("ULTRA_PANIC_KEY", "1");
                    {   char pt[16] = {0};
                        if (!GetEnvironmentVariableA("ULTRA_PANIC_TIMEOUT", pt, sizeof pt) || !pt[0])
                            SetEnvironmentVariableA("ULTRA_PANIC_TIMEOUT", "20");
                    }
                    LOG("gameshell: HAPPIO disabled, panic key armed (Ctrl+Alt+Q), watchdog 20s\n");
                } else {
                    LOG("gameshell: ULTRA_EUROPAR_GAMESHELL=1 but %s does not exist -- launching the game directly\n", gs_path);
                }
            }
        }
    }
    LOG("launching %s\n", game);

    /* Derive workdir = dir(game). Europa-R games expect to run from their own
     * folder for relative asset lookups (3DDATA/, SHADERS/, NEWGFX/, SND/...).
     *
     * Must handle BOTH '\\' (typical Win32) and '/' (forward-slash paths the
     * loader CLI/dispatcher may hand us, e.g. "I:/Sega Europa R Dumps/...").
     * If we only split on '\\' and the path uses '/', strrchr returns NULL,
     * workdir stays equal to the full path including "fordracing.exe", and
     * Win32 CreateProcessA then rejects lpCurrentDirectory with
     * ERROR_DIRECTORY (267) so the hook never injects. The verifier
     * reported this exact failure -- see Test 1 / "267" in the report. */
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
    if (!extract_hook_to_temp(hook, sizeof hook)) {
        LOG("ERROR: embedded rendering hook could not be extracted\n");
        MessageBoxA(NULL,
            "Sega Europa-R rendering hook could not be extracted from this module.\n\n"
            "The installed ultra_sega_europar.dll may be corrupt -- reinstall it via\n"
            "Settings -> Install Module.",
            "Ultra Loader -- Europa-R Hook Missing",
            MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }
    LOG("hook = %s\n", hook);
    LOG("workdir = %s\n", workdir);

    /* Env vars the rendering hook DLL reads at injection time. All default
     * off when unset, so they turn on per-Europa-R only -- byte-safe
     * globally.
     *   ULTRA_SEGA_WINDOWED -- forces windowed-mode rendering so the present
     *     hook can upscale the native back-buffer to the user's output res.
     *   ULTRA_SEGA_KEYCHIP  -- enables mxparallel keychip-presence emulator
     *     (Europa-R has BP_PROT_KEYCHIP per board_profile).
     * NOTE: no ULTRA_CONTROLS / ULTRA_BOARD / ULTRA_IO4 / ULTRA_JVS_ANALOG
     * here any more -- those only ever armed the shared hook's JVS/IO4/
     * controls.c input-mapping layer, which this board no longer uses AT ALL
     * (see file header, point 2). Leaving them unset means that layer's
     * xinput/dinput polling threads simply don't start for this board;
     * controls come entirely from sega_europar_dinput8_swap_in() below. */
    SetEnvironmentVariableA("ULTRA_LAUNCH_MODULE", "sega_europar");
    SetEnvironmentVariableA("ULTRA_BOARD_HINT",    "sega_europar");
    SetEnvironmentVariableA("ULTRA_SYSTEM_ID",     "sega_europar");
    /* ULTRA_SEGA_WINDOWED only gates the D3D11 (d3d11_throttle.c) and GL
     * (gl_present.c) present hooks -- Europa-R/Ford Racing is a D3D9 title
     * (board_profile.c: BP_GFX_D3D9), whose present hook is d3d9_throttle.c.
     * That file's force_windowed() reads a COMPLETELY DIFFERENT var,
     * ULTRA_TYPEX2_WINDOWED (plus the universal ULTRA_WINDOWED/
     * ULTRA_BORDERLESS, which settings_apply() explicitly sets to "0" when
     * the user's display_mode is DISP_FULLSCREEN(2) -- NOT merely "unset").
     * Root-caused 2026-07-23 (live test): with only ULTRA_SEGA_WINDOWED set,
     * force_windowed() returns 0, so Europa-R ran fullscreen-exclusive --
     * exactly the failure mode this comment block already predicted below
     * ("without this Europa-R runs fullscreen-exclusive and the user sees a
     * black 1920x1080 with the tiny game hidden"), just via the wrong knob.
     * Confirmed live: solid black 1920x1080 window, process alive/responding/
     * consuming CPU (a real render loop, not a hang), foreground + correctly
     * positioned -- the classic "hidden tiny game inside an exclusive
     * fullscreen swapchain" symptom. Set BOTH vars so every present-hook
     * variant (D3D9 today; D3D11/GL if Europa-R titles ever use them) forces
     * windowed rendering the same way sega_ringedge-class boards need. */
    SetEnvironmentVariableA("ULTRA_SEGA_WINDOWED", "1");
    SetEnvironmentVariableA("ULTRA_TYPEX2_WINDOWED", "1");
    SetEnvironmentVariableA("ULTRA_SEGA_KEYCHIP",  "1");
    /* Tell the hook to neutralize inpout32.dll if the game tries to load it.
     * Europa-R titles ship inpout32.dll for parallel-port keychip I/O; on a
     * user machine without the inpout32 kernel driver installed, inpout32's
     * DllMain crashes during DLL_PROCESS_ATTACH (it CreateFile()s the driver
     * handle unconditionally). That crash trips inside WOW64 init and the
     * process hangs with only ntdll64+wow64.dll mapped -- which is exactly
     * the symptom the verifier reported (process hung in WOW64 init at
     * Test 2, no hook loaded). The hook's load_library hook treats this
     * env var as "if inpout32.dll LoadLibraryA, return a fake HMODULE
     * pointing at our stub". */
    SetEnvironmentVariableA("ULTRA_INPOUT32_BYPASS", "1");
    /* Also force-dismiss any modal dongle/keychip dialogs the game might
     * pop behind the present surface. */
    if (!GetEnvironmentVariableA("ULTRA_DISMISS_DIALOGS", NULL, 0))
        SetEnvironmentVariableA("ULTRA_DISMISS_DIALOGS", "1");
    /* 60 fps cap (Europa-R hardware target). */
    if (!GetEnvironmentVariableA("ULTRA_FPS_CAP", NULL, 0))
        SetEnvironmentVariableA("ULTRA_FPS_CAP", "60");

    /* Controls: swap our dedicated dinput8.dll proxy into the GAME's own
     * folder BEFORE CreateProcess -- it must already be sitting there when
     * the game's own dynamic LoadLibraryA("dinput8.dll") runs during its
     * Input System init. See sega_europar_dinput8_swap_in()'s own comment
     * and the file header (point 2) for why this is a file swap and not an
     * injected hook. */
    sega_europar_dinput8_swap_in(workdir);

    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\"", game);
    /* bInheritHandles=TRUE so the child's CRT inherits our STD_ERROR_HANDLE
     * (the freopen'd ultra.log). The hook's fprintf(stderr,...) diagnostic
     * lines (D3D9 hook armed / keychip emulated / IO4 wired / etc.) will
     * then land in the same log file the loader writes, which is the only
     * way to see what the hook is doing inside the child without a console. */
    if (!CreateProcessA(game, cmd, NULL, NULL, TRUE,
                        CREATE_SUSPENDED, NULL,
                        workdir[0] ? workdir : NULL, &si, &pi)) {
        LOG("CreateProcess(%s) failed: %lu\n", game, GetLastError());
        DeleteFileA(hook);
        return 1;
    }
    LOG("spawned pid=%lu tid=%lu (suspended)\n",
        pi.dwProcessId, pi.dwThreadId);

    /* Primary inject path: queue a user APC for LoadLibraryA on the main
     * thread. Fires when WOW64 hits its first alertable point with the
     * 32-bit loader fully up. */
    int apc_ok = inject_dll_apc(pi.hProcess, pi.hThread, hook);
    if (!apc_ok) {
        LOG("APC inject failed; will rely on fallback CreateRemoteThread\n");
    }

    ResumeThread(pi.hThread);
    LOG("game resumed; waiting briefly for WOW64 32-bit loader to come up\n");

    /* Give WOW64 a moment to map the 32-bit ntdll/kernel32 so the fallback
     * CreateRemoteThread can land safely if the APC missed. 750 ms is well
     * past WOW64 init on any modern machine (<50 ms typical) but small
     * enough that the user doesn't perceive a delay. */
    Sleep(750);

    /* Fallback inject path. If the APC already mapped the hook this is a
     * harmless no-op (LoadLibraryA returns the existing HMODULE). */
    int rt_ok = inject_dll_remote_thread(pi.hProcess, hook);
    if (!apc_ok && !rt_ok) {
        LOG("BOTH inject paths failed; terminating game\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        DeleteFileA(hook);
        return 1;
    }
    LOG("hook injected (apc=%d rt=%d), waiting for game exit\n", apc_ok, rt_ok);

    /* ---- THE ESCAPE HATCH LIVES HERE, IN THE LOADER --------------------------
     *
     * IT WAS IN THE HOOK AND IT COULD NOT WORK. A first attempt put a panic-key
     * thread inside the injected DLL. Windows will not let a thread created in
     * DllMain execute until DllMain RETURNS -- the new thread needs the loader
     * lock that DllMain is holding. So when GameShell wedged during startup on
     * 2026-09-20 the thread existed and never ran one instruction, and the owner's
     * desktop was held twice. ultra_panic.log was never written, which is what
     * proved it.
     *
     * The loader is a SEPARATE, HEALTHY process that is already sitting here doing
     * nothing but waiting. Nothing the game does can wedge it. So the guard
     * belongs on this side of the process boundary, not inside the patient.
     *
     *   Ctrl+Alt+Q held ~0.4s        -> kill the game
     *   ULTRA_PANIC_TIMEOUT seconds  -> kill it if it never opens a window
     *
     * 'Progress' is judged from outside with GetProcessTimes: a process that is
     * genuinely loading burns CPU, while a wedged one sits at a flat kernel+user
     * total. That distinguishes 'slow' from 'stuck' without the game's help. */
    {
        char pt[16] = {0};
        int  timeout_s = 0;
        int  held_ms = 0;
        DWORD t0 = GetTickCount();
        ULONGLONG last_cpu = 0;
        DWORD last_cpu_move = t0;
        GetEnvironmentVariableA("ULTRA_PANIC_TIMEOUT", pt, sizeof pt);
        if (pt[0]) timeout_s = atoi(pt);
        if (timeout_s < 0) timeout_s = 0;
    
        for (;;) {
            DWORD w = WaitForSingleObject(pi.hProcess, 100);
            if (w != WAIT_TIMEOUT) break;          /* game exited on its own */
    
            /* panic key -- a short hold so a stray chord cannot kill a good run */
            if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                (GetAsyncKeyState(VK_MENU)    & 0x8000) &&
                (GetAsyncKeyState('Q')        & 0x8000)) {
                held_ms += 100;
                if (held_ms >= 400) {
                    LOG("PANIC: Ctrl+Alt+Q -- terminating the game\n");
                    TerminateProcess(pi.hProcess, 3);
                    europar_restore_display();   /* the kill does not undo a mode change */
                    break;
                }
            } else {
                held_ms = 0;
            }
    
            /* watchdog -- only while the game has never shown a window */
            if (timeout_s > 0) {
                FILETIME c, e, k, u;
                DWORD now = GetTickCount();
                if (GetProcessTimes(pi.hProcess, &c, &e, &k, &u)) {
                    ULONGLONG cpu =
                        (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                        (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
                    /* 10 ms of CPU counts as progress (FILETIME is 100 ns units) */
                    if (cpu > last_cpu + 100000ULL) { last_cpu = cpu; last_cpu_move = now; }
                }
                if ((int)((now - last_cpu_move) / 1000) >= timeout_s) {
                    LOG("WATCHDOG: no CPU progress for %d s -- terminating so the desktop is not held\n", timeout_s);
                    TerminateProcess(pi.hProcess, 4);
                    europar_restore_display();   /* the kill does not undo a mode change */
                    break;
                }
            }
        }
    }
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
