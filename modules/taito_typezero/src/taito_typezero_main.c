/*
 * taito_typezero_main.c -- Taito Type Zero spawner + hook injector.
 *
 * Type Zero is Taito's low-power Atom-based arcade board (Win Embedded
 * Standard 7 / Atom 230, 32-bit) built as the low-cost NESiCAxLive download
 * host -- Direct3D 9 rendering, identical graphics path to Type X2 (already
 * working, see docs/X86_BOARDS.md). Real hardware I/O is JVS-over-USB per
 * Taito's own documentation, but this build's hook (src/hook/, this board's
 * own independent tree) implements JVS-over-serial -- the same transport
 * already proven on Type X2 -- per this rebuild's explicit board spec.
 * Common catalog: the Groove Coaster series and other NESiCAxLive 2D
 * download titles (lighter fighters/puzzle games).
 * FIX (this rebuild pass): the comment block this replaced described a
 * 64-bit/D3D11/JVS-over-USB board and named BlazBlue Chrono Phantasma / KOF
 * XIII Climax / Dariusburst / Persona 4 Arena Ultimax as "Type Zero" titles --
 * those are actually Type X3/X4-era Taito games; this block had the wrong
 * board's characteristics copied in. Corrected here to match board_profile.c's
 * actual taito_typexzero entry (BP_GFX_D3D9, 32-bit, BP_IO_JVS_SERIAL) and
 * docs/X86_BOARDS.md's Type X Zero row.
 * ALSO ON RECORD: modules/native_x86/src/hook/board_profile.c's table entry
 * (~line 75) is keyed "taito_typexzero" (note the extra "x") with an inline
 * comment "/* module id: taito_typexzero *\/", but this module's own folder
 * and every env var below have always been "taito_typezero" (no x) -- so
 * that profile lookup silently fails today. This board no longer resolves
 * via native_x86's board_profile.c at all (own independent hook below), so
 * the mismatch is moot for this build, but it was never fixed at the source
 * and remains a live bug for anything that still keys off that table.
 *
 * argv layout from the loader's dispatcher (work/src/main.c):
 *   argv[0] = ultra-loader.exe path
 *   argv[1] = game.exe path (the Type Zero game)
 *
 * We detect PE bitness (32-bit vs 64-bit) from the game's IMAGE_FILE_HEADER
 * and pick the matching hook DLL -- this board's OWN independent build
 * (modules/taito_typezero/src/hook/, built by build_typexzero_hook.sh), NOT
 * the shared multi-board ultra_typex_hook{32,64}.dll from native_x86:
 *   32-bit game -> ultra_taito_typezero_hook32.dll (in-process CreateRemoteThread inject)
 *   64-bit game -> ultra_taito_typezero_hook64.dll (spawn ultra-inject64.exe helper)
 * The real board only ever runs 32-bit games (board_profile.c bitness=32);
 * the 64-bit branch is kept for defensiveness only and is untested.
 *
 * Env vars ULTRA_BOARD_HINT / ULTRA_SYSTEM_ID / ULTRA_BOARD are set below for
 * diagnostics/consistency, though this board's own hook does not branch on
 * them (it has exactly one board to serve, unlike the shared multi-board
 * hook). ULTRA_CONTROLS=1 is set for consistency with every other board's
 * launch contract; this board's own hook reads its gamepad directly.
 */

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

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

static int try_path(const char *p, char *out, size_t cap)
{
    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) return 0;
    strncpy(out, p, cap - 1); out[cap - 1] = 0;
    return 1;
}

/* This board's hook is no longer installed as a standalone file. It's
 * embedded as an RCDATA resource ("HOOK32"/"HOOK64") inside THIS DLL (see
 * hook_res.rc, baked in by build_module.sh) and extracted to a fresh temp
 * file right before each injection. This means signing/verifying this one
 * DLL automatically covers the hook too, and there's no separate hook file
 * sitting on disk between launches for anything to tamper with -- see
 * docs/PROJECT_LICENSING_AND_SIGNING.md's hook-DLL note (2026-08-02).
 * Caller must DeleteFileA() the returned path once done with it. */
static int find_hook(int bits, char *out, size_t cap)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)&find_hook, &self);
    if (!self) { fprintf(stderr, "[taito_typezero] GetModuleHandleEx(self) failed: %lu\n", GetLastError()); return 0; }

    const char *res_name = (bits == 32) ? "HOOK32" : "HOOK64";
    HRSRC info = FindResourceA(self, res_name, RT_RCDATA);
    if (!info) { fprintf(stderr, "[taito_typezero] embedded hook resource \"%s\" not found\n", res_name); return 0; }
    DWORD sz = SizeofResource(self, info);
    HGLOBAL loaded = LoadResource(self, info);
    const void *ptr = loaded ? LockResource(loaded) : NULL;
    if (!ptr || !sz) { fprintf(stderr, "[taito_typezero] embedded hook resource \"%s\" empty\n", res_name); return 0; }

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
    if (!fp) { fprintf(stderr, "[taito_typezero] cannot write temp hook %s\n", out); return 0; }
    size_t wrote = fwrite(ptr, 1, sz, fp);
    fclose(fp);
    if (wrote != sz) { fprintf(stderr, "[taito_typezero] short write to temp hook %s\n", out); DeleteFileA(out); return 0; }
    fprintf(stderr, "[taito_typezero] extracted embedded hook (%s, %lu bytes) -> %s\n", res_name, sz, out);
    return 1;
}

/* 32-bit in-process inject (Type X pattern) */
static int inject_dll_32(HANDLE proc, const char *dll_path)
{
    size_t n = strlen(dll_path) + 1;
    LPVOID remote = VirtualAllocEx(proc, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return 0;
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(proc, remote, dll_path, n, &wrote) || wrote != n) {
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
 * inject from the correct bitness. Search only next to the loader -- the
 * "modtest\" fallback that used to sit here was removed this rebuild pass
 * (independence: this board must not depend on modtest at runtime). NOTE for
 * whoever revisits this: per memory ultra-loader-embedded-assets.md#inject,
 * the project has since moved to a centralized ul_inject_pid/spawn/handle
 * API (ultra-module-host.exe --inject / --spawn-inject) and no longer ships
 * per-module ultra-inject32/64.exe helpers in Ship -- this 64-bit branch is
 * untested (the real Type Zero board is 32-bit only) and still targets the
 * older helper-exe convention; migrating it to the centralized API is a
 * reasonable follow-up but is out of scope for this pass (only the JVS
 * engine / JVS-serial transport / D3D9 hook / audio hook / DllMain rebuild
 * was requested). */
static int inject_dll_64(DWORD pid, const char *dll_path)
{
    char loader_exe[MAX_PATH];
    GetModuleFileNameA(NULL, loader_exe, MAX_PATH);
    char *bs = strrchr(loader_exe, '\\'); if (bs) *bs = 0;

    char injector[MAX_PATH];
    snprintf(injector, sizeof injector, "%s\\ultra-inject64.exe", loader_exe);
    int found = (GetFileAttributesA(injector) != INVALID_FILE_ATTRIBUTES);
    if (!found) {
        fprintf(stderr, "[taito_typezero] ultra-inject64.exe not found (needed for 64-bit games)\n");
        return 0;
    }

    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\" %lu \"%s\"", injector, pid, dll_path);
    STARTUPINFOA si = { .cb = sizeof(STARTUPINFOA) };
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "[taito_typezero] CreateProcess(inject64) failed: %lu\n", GetLastError());
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

__declspec(dllexport) int ul_taito_typezero_run(int argc, char **argv)
{
    if (argc < 2 || !argv[1] || !*argv[1]) {
        fprintf(stderr, "[taito_typezero] ERROR: no game path\n");
        return 1;
    }

    /* Per-game gating for titles already proven on this board.
     * NOTE: if this board's own env setup below would overwrite a locked
     * key, move this call to AFTER that block (namco_es3_main.c does). */
    ul_apply_game_locks(argv[1]);
    const char *game = argv[1];
    fprintf(stderr, "[taito_typezero] launching %s\n", game);

    int bits = pe_bitness(game);
    if (bits == 0) { fprintf(stderr, "[taito_typezero] not a PE file\n"); return 1; }
    fprintf(stderr, "[taito_typezero] PE bitness = %d\n", bits);

    char hook[MAX_PATH];
    if (!find_hook(bits, hook, sizeof hook)) {
        fprintf(stderr, "[taito_typezero] embedded hook for %d-bit could not be extracted\n", bits);
        return 1;
    }
    fprintf(stderr, "[taito_typezero] hook = %s\n", hook);

    char workdir[MAX_PATH];
    strncpy(workdir, game, MAX_PATH - 1); workdir[MAX_PATH - 1] = 0;
    {
        char *b1 = strrchr(workdir, '\\');
        char *f1 = strrchr(workdir, '/');
        char *sep = (b1 && f1) ? (b1 > f1 ? b1 : f1) : (b1 ? b1 : f1);
        if (sep) *sep = 0;
    }

    /* Diagnostic/consistency env vars -- this board's OWN independent hook
     * (modules/taito_typezero/src/hook/) does not branch on ULTRA_BOARD_HINT/
     * ULTRA_SYSTEM_ID/ULTRA_BOARD (unlike the shared multi-board hook's
     * controls.c, it serves exactly one board, so there is nothing to
     * select), but they are still set for logging/diagnostics and in case a
     * future shared tool inspects them. FIX (this rebuild pass): the old
     * ULTRA_D3D11=1 set here was simply wrong -- Type Zero is a D3D9 board
     * (board_profile.c: BP_GFX_D3D9; docs/X86_BOARDS.md; graphics path
     * identical to Type X2) -- removed. This board's own hook always installs
     * its D3D9 present hook unconditionally (no env gate needed: unlike the
     * shared hook it never has to choose among several graphics APIs). */
    SetEnvironmentVariableA("ULTRA_BOARD_HINT",  "taito_typezero");
    SetEnvironmentVariableA("ULTRA_SYSTEM_ID",   "taito_typezero");
    SetEnvironmentVariableA("ULTRA_BOARD",       "taito_typezero");
    SetEnvironmentVariableA("ULTRA_CONTROLS",    "1");     /* wake up input layer */
    if (!GetEnvironmentVariableA("ULTRA_DISMISS_DIALOGS", NULL, 0))
        SetEnvironmentVariableA("ULTRA_DISMISS_DIALOGS", "1");
    if (!GetEnvironmentVariableA("ULTRA_FPS_CAP", NULL, 0))
        SetEnvironmentVariableA("ULTRA_FPS_CAP", "60");

    STARTUPINFOA si = { .cb = sizeof(STARTUPINFOA) };
    PROCESS_INFORMATION pi = {0};
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\"", game);
    if (!CreateProcessA(game, cmd, NULL, NULL, TRUE, CREATE_SUSPENDED,
                        NULL, workdir[0] ? workdir : NULL, &si, &pi)) {
        fprintf(stderr, "[taito_typezero] CreateProcess failed: %lu\n", GetLastError());
        DeleteFileA(hook);
        return 1;
    }
    fprintf(stderr, "[taito_typezero] spawned pid=%lu (suspended)\n", pi.dwProcessId);

    int ok = (bits == 64)
        ? inject_dll_64(pi.dwProcessId, hook)
        : inject_dll_32(pi.hProcess, hook);
    if (!ok) {
        fprintf(stderr, "[taito_typezero] inject failed\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        DeleteFileA(hook);
        return 1;
    }

    ResumeThread(pi.hThread);
    fprintf(stderr, "[taito_typezero] hook injected, game resumed\n");
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0; GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    DeleteFileA(hook);
    return (int)rc;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID x) { (void)h;(void)r;(void)x; return TRUE; }
