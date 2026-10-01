/*
 * sega_nu_main.c -- Sega Nu spawner + hook injector.
 *
 * The loader's dispatcher (work/src/main.c) calls ul_sega_nu_run(argc, argv)
 * after detecting a Sega Nu .exe game with ULTRA_MODULE=sega_nu (or auto-
 * detected via the board profile). argv layout:
 *   argv[0] = ultra-loader.exe path
 *   argv[1] = game.exe path (the Nu game)
 *
 * Sega Nu is an x86 arcade board (Sega PC-class platform, Windows Embedded 8,
 * 64-bit-only, D3D11 graphics, IO4-over-USB inputs, keychip (A7/N2 USB smart-
 * card via fdd.sys, GkeyEnc/Dec cmds 11/12) + BitLocker+TPM container
 * protection). The board profile (board_profile.c BP_SEGA_NU entry, lines
 * 123-136) marks it as:
 *   BP_GFX_D3D11 | BP_IO_IO4_USB | BP_INJECT_SPAWN_SUSPENDED | BP_HOOK_D3D11
 *   BP_PROT_KEYCHIP | BP_PROT_CONTAINER | BP_PROT_TPM
 *   BP_READY_DRM_GATED (needs operator keychip / decrypted container)
 *   64-bit.
 * Nu2 is the N2 (L3.1) keychip variant of the same DRM stack; no D3D9
 * fallback needed on either.
 *
 * We mirror sega_europar_main.c's spawn+inject pattern, with the WOW64 tweaks
 * dropped because Nu is 64-bit-only (both loader-side dispatcher and the
 * child game are 64-bit -- no 32-bit ntdll shim comes into play, and the
 * suspended thread parks at the normal 64-bit LdrInitializeThunk with kernel32
 * already partially resolved, so CreateRemoteThread(LoadLibraryA) is safe.
 * We still queue an APC as the PRIMARY inject path because it wins the race
 * against the game's early dynamic-import sweep just like on Europa-R, and
 * keep the CreateRemoteThread path as fallback in case the APC is delayed).
 *
 * Hook: this board's own independent hook (build_hook.sh, at the module
 * root), with D3D11 present + IO4-USB + keychip/truecrypt/amauth/allnet/
 * amdaemon stubs tailored to Nu. 2026-08-03: no longer installed as a
 * standalone file -- it's embedded as an RCDATA resource ("HOOK32"/"HOOK64")
 * inside THIS DLL (see hook_res.rc, baked in by build_module.sh AFTER
 * build_hook.sh produces the binaries) and extracted to a fresh temp file
 * right before each injection. Signing/verifying this one DLL automatically
 * covers the hook too, and no separate hook file sits on disk between
 * launches for anything to tamper with -- see
 * docs/PROJECT_LICENSING_AND_SIGNING.md. No shared runtime binary, no
 * fallback to any other module's compiled output (the modtest/
 * ultra_typex_hook{32,64}.dll staging this board's build script used to do
 * is gone too -- this board's own find_hook() never looked for that
 * filename in the first place, so those loose files were dead weight).
 *
 * Env vars set BEFORE CreateProcess (all default off when unset, so they
 * only turn on for Sega Nu -- byte-safe globally):
 *   ULTRA_LAUNCH_MODULE = "sega_nu"
 *   ULTRA_BOARD_HINT    = "sega_nu"
 *   ULTRA_SYSTEM_ID     = "sega_nu"
 *   ULTRA_SEGA_WINDOWED = "1"   -- force windowed rendering so the D3D11 hook
 *                                  can upscale the native back-buffer to the
 *                                  user's chosen output res (FSR/framegen).
 *   ULTRA_SEGA_KEYCHIP  = "1"   -- enables the USB A7/N2 keychip smart-card
 *                                  emulator (GkeyEnc/Dec cmd 11/12) so the
 *                                  game's keychip check can pass without real
 *                                  hardware once an operator image is
 *                                  provided.
 *   ULTRA_SEGA_TRUECRYPT= "1"   -- Nu ships BitLocker+TPM container (the
 *                                  setting name is preserved from the older
 *                                  RingEdge TrueCrypt stack, per project
 *                                  convention -- board profile marks it as
 *                                  BP_PROT_CONTAINER | BP_PROT_TPM).
 *   ULTRA_SEGA_AMAUTH   = "1"   -- ALL.Net auth stub
 *   ULTRA_SEGA_ALLNET   = "1"   -- ALL.Net network gate stub
 *   ULTRA_SEGA_AMDAEMON = "1"   -- amdaemon local IPC stub
 *   ULTRA_SEGA_NETWORK  = "1"   -- generic Sega network layer stub
 *   ULTRA_IO4           = "1"   -- IO4-over-USB (NOT JVS-serial like RingEdge)
 *   ULTRA_HOOK_D3D11    = "1"   -- DXGI present hook, no D3D9 needed
 *   ULTRA_JVS_ANALOG    = "3"   -- advertise 3 analog channels to the game's
 *                                  IO4 FEATCHK probe (racing/steering titles).
 *   ULTRA_CONTROLS      = "1"   -- master input-mapping enable (racer profile
 *                                  wakes up: keyboard 1=START, 5=COIN, F2=TEST,
 *                                  etc.; XInput; DInput wheel priority).
 *   ULTRA_INPOUT32_BYPASS = "1" -- neutralize inpout32.dll if the game ever
 *                                  tries to load it (unlikely on Nu but
 *                                  harmless as a belt-and-braces).
 */

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#define LOG(...) do { fprintf(stderr, "[sega_nu] " __VA_ARGS__); fflush(stderr); } while (0)

/* This board's hook is embedded as an RCDATA resource ("HOOK32"/"HOOK64")
 * inside THIS DLL and extracted to a fresh temp file right before each
 * injection -- see the file header for why. Caller must DeleteFileA() the
 * returned path once done with it. */
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

/* Peek the game's PE header and return 64 (PE32+), 32 (PE32), or 64 on error.
 * Nu-family titles are 64-bit but Nu-adjacent helper binaries in the wild are
 * 32-bit; we pick the matching DLL bitness so injection can't fail on Wow64. */
static int game_pe_bits(const char *exe)
{
    HANDLE fh = CreateFileA(exe, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return 64;
    IMAGE_DOS_HEADER dos; DWORD got = 0;
    if (!ReadFile(fh, &dos, sizeof dos, &got, NULL) || got != sizeof dos
        || dos.e_magic != IMAGE_DOS_SIGNATURE) { CloseHandle(fh); return 64; }
    if (SetFilePointer(fh, dos.e_lfanew, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        CloseHandle(fh); return 64;
    }
    DWORD sig = 0;
    IMAGE_FILE_HEADER fhdr;
    if (!ReadFile(fh, &sig, sizeof sig, &got, NULL) || sig != IMAGE_NT_SIGNATURE
        || !ReadFile(fh, &fhdr, sizeof fhdr, &got, NULL) || got != sizeof fhdr) {
        CloseHandle(fh); return 64;
    }
    CloseHandle(fh);
    if (fhdr.Machine == IMAGE_FILE_MACHINE_I386) return 32;
    return 64;
}

/* Primary inject path: queue a user APC for LoadLibraryA on the suspended
 * main thread. Fires the moment the thread reaches its first alertable point
 * inside the loader with kernel32 mapped. This is the WOW64-correct path;
 * on native 64-bit Nu it's not strictly required but it still wins the race
 * against the game's early dynamic-import sweep, so we keep it as primary. */
/* 64-bit inject via the ultra-inject64.exe helper.
 * This module is 32-bit and MUST be: ultra-loader.exe is 32-bit, and
 * LoadLibrary of a 64-bit DLL fails with 193 ERROR_BAD_EXE_FORMAT -- that is
 * exactly what stopped Luigi's Mansion Arcade from launching. Nu games are
 * 64-bit, and a 32-bit process cannot CreateRemoteThread/QueueUserAPC into a
 * 64-bit one, so injection is delegated to the standalone 64-bit injector --
 * the same approach namco_es3_main.c already uses for its 64-bit titles. */
static int inject_dll_64(DWORD pid, const char *dll_path)
{
    char loader_exe[MAX_PATH];
    GetModuleFileNameA(NULL, loader_exe, MAX_PATH);
    char *bs = strrchr(loader_exe, '\\'); if (bs) *bs = 0;
    char injector[MAX_PATH]; int found = 0;
    snprintf(injector, sizeof injector, "%s\\ultra-inject64.exe", loader_exe);
    if (GetFileAttributesA(injector) != INVALID_FILE_ATTRIBUTES) found = 1;
    if (!found) {
        snprintf(injector, sizeof injector, "%s\\modtest\\ultra-inject64.exe", loader_exe);
        if (GetFileAttributesA(injector) != INVALID_FILE_ATTRIBUTES) found = 1;
    }
    if (!found) { LOG("ultra-inject64.exe not found (needed for 64-bit Nu games)\n"); return 0; }
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\" %lu \"%s\"", injector, pid, dll_path);
    STARTUPINFOA si; PROCESS_INFORMATION pj;
    memset(&si, 0, sizeof si); si.cb = sizeof si; memset(&pj, 0, sizeof pj);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pj)) {
        LOG("CreateProcess(ultra-inject64) failed: %lu\n", GetLastError()); return 0;
    }
    WaitForSingleObject(pj.hProcess, 15000);
    DWORD irc = 1; GetExitCodeProcess(pj.hProcess, &irc);
    CloseHandle(pj.hThread); CloseHandle(pj.hProcess);
    return irc == 0;
}

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
    LOG("APC queued for LoadLibraryA(%s) remote=%p\n", dll_path, remote);
    /* Deliberate: leak `remote` (one small page) so the string stays valid
     * when LoadLibraryA reads it after our return. */
    return 1;
}

/* Belt-and-suspenders second inject path: post-resume, after the game's
 * loader has had a moment to bring the process up. If the APC already
 * loaded the hook, this is a no-op (LoadLibraryA returns the existing
 * HMODULE and DllMain isn't re-entered). */
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

__declspec(dllexport) int ul_sega_nu_run(int argc, char **argv)
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

    /* Derive workdir = dir(game). Nu games expect to run from their own
     * folder for relative asset lookups. Handle both '\\' and '/'. */
    char workdir[MAX_PATH];
    strncpy(workdir, game, MAX_PATH - 1); workdir[MAX_PATH - 1] = 0;
    {
        char *bs = strrchr(workdir, '\\');
        char *fs = strrchr(workdir, '/');
        char *sep = (bs && fs) ? (bs > fs ? bs : fs) : (bs ? bs : fs);
        if (sep) *sep = 0;
    }

    int bits = game_pe_bits(game);
    LOG("game PE bitness = %d\n", bits);
    char hook[MAX_PATH];
    if (!find_hook(bits, hook, sizeof hook)) {
        LOG("ERROR: embedded hook for %d-bit could not be extracted\n", bits);
        MessageBoxA(NULL,
            "Sega Nu hook could not be extracted from this module.\n\n"
            "The installed ultra_sega_nu.dll may be corrupt -- reinstall it via\n"
            "Settings -> Install Module.",
            "Ultra Loader -- Sega Nu Hook Missing",
            MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }
    LOG("hook = %s\n", hook);
    LOG("workdir = %s\n", workdir);

    /* Env vars the hook DLL reads at injection time. All default off when
     * unset, so they turn on per-Nu only -- byte-safe globally. */
    SetEnvironmentVariableA("ULTRA_LAUNCH_MODULE",    "sega_nu");
    SetEnvironmentVariableA("ULTRA_BOARD_HINT",       "sega_nu");
    SetEnvironmentVariableA("ULTRA_SYSTEM_ID",        "sega_nu");
    /* controls.c's ultra_input_map_install() resolves the per-board input
     * profile from THIS var specifically (not BOARD_HINT/SYSTEM_ID) -- see
     * the Europa-R fix in X86_BOARDS.md S7 for the bug this closes. */
    SetEnvironmentVariableA("ULTRA_BOARD",            "sega_nu");
    SetEnvironmentVariableA("ULTRA_SEGA_WINDOWED",    "1");
    SetEnvironmentVariableA("ULTRA_SEGA_KEYCHIP",     "1");
    SetEnvironmentVariableA("ULTRA_SEGA_TRUECRYPT",   "1");
    SetEnvironmentVariableA("ULTRA_SEGA_AMAUTH",      "1");
    SetEnvironmentVariableA("ULTRA_SEGA_ALLNET",      "1");
    SetEnvironmentVariableA("ULTRA_SEGA_AMDAEMON",    "1");
    SetEnvironmentVariableA("ULTRA_SEGA_NETWORK",     "1");
    SetEnvironmentVariableA("ULTRA_IO4",              "1");
    SetEnvironmentVariableA("ULTRA_HOOK_D3D11",       "1");
    SetEnvironmentVariableA("ULTRA_JVS_ANALOG",       "3");
    SetEnvironmentVariableA("ULTRA_CONTROLS",         "1");
    SetEnvironmentVariableA("ULTRA_INPOUT32_BYPASS",  "1");
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
    LOG("spawned pid=%lu tid=%lu (suspended)\n",
        pi.dwProcessId, pi.dwThreadId);

    /* 64-bit game: only the out-of-process injector can reach it. Handled
     * fully here so the 32-bit APC/CreateRemoteThread path below is untouched. */
    if (bits == 64) {
        if (!inject_dll_64(pi.dwProcessId, hook)) {
            LOG("64-bit inject failed; terminating game\n");
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            DeleteFileA(hook);
            return 1;
        }
        ResumeThread(pi.hThread);
        LOG("hook injected (inject64), game resumed\n");
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD rc64 = 0; GetExitCodeProcess(pi.hProcess, &rc64);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        DeleteFileA(hook);
        LOG("game exited with code %lu\n", rc64);
        return (int)rc64;
    }

    int apc_ok = inject_dll_apc(pi.hProcess, pi.hThread, hook);
    if (!apc_ok) {
        LOG("APC inject failed; will rely on fallback CreateRemoteThread\n");
    }

    ResumeThread(pi.hThread);
    LOG("game resumed; brief sleep before fallback inject\n");
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
