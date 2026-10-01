/*
 * taito_nesica2_main.c -- Taito NESiCAxLive2 (gen 2) spawner + hook injector.
 *
 * NESiCAxLive2 is Taito's 2013+ arcade content-distribution + DRM layer,
 * running on top of Taito Type X2 / Type X3 hardware for the 64-bit era.
 * Games are D3D11 64-bit PE fighters/shooters (Persona 4 Arena Ultimax
 * NLX2 rev, BlazBlue CP Extend, KOF XIII Climax, Under Night In-Birth,
 * Nitroplus Blasterz IH, Chaos Code -New Sign of Catastrophe-, etc.). NLX2
 * adds a per-title container mount, a stronger network-heartbeat check, and
 * a keychip-derived license blob.
 *
 * NLX and NLX2 share ONE unified board_profile entry in native_x86's
 * SHARED multi-board hook (see native_x86/src/hook/board_profile.c:
 * BP_HOOK_D3D9_AND_D3D11, BP_PROT_NESICA | BP_PROT_CONTAINER |
 * BP_PROT_NETWORK) -- but that entry is irrelevant to THIS module: this
 * board's own hook DLL (below) never reads board_profile.c at all. NOTE FOR
 * THE PROJECT OWNER: that table's ONLY Taito-NESiCA row is keyed by the
 * literal system_id string "taito_nesica" (singular) -- its display name
 * happens to say "NESiCAxLive / NESiCAxLive2", but the lookup KEY doesn't
 * match "taito_nesica2" (the exact string this module sets in
 * ULTRA_SYSTEM_ID below). Harmless to this fully-independent module, but
 * any FUTURE native_x86-based consumer that keys off ULTRA_SYSTEM_ID would
 * silently miss this board. Not fixed here (out of this module's scope --
 * native_x86/src/hook/ is never touched by this rebuild).
 *
 * INDEPENDENCE (2026-08-01 rebuild): this module used to stage and inject
 * modtest/ultra_typex_hook{32,64}.dll -- the SHARED Type X hook DLL also
 * used by taito_nesica, taito_typex, taito_typex2, etc. Per the project
 * owner's per-board directive (every modern arcade board gets its own
 * fully independent, self-contained hook DLL -- no shared runtime binary
 * with any other board), this module now builds and injects its OWN hook,
 * compiled from modules/taito_nesica2/src/hook/ (see that directory's
 * taito_nesica2_hook.h for the full file-by-file breakdown: its own
 * clean-room JVS engine, its own JVS-over-serial transport, its own D3D11 +
 * D3D9 present hooks, its own audio hook, its own NESiCA card-reader
 * presence stub). find_hook() below therefore only ever looks in THIS
 * module's own install location -- never in modtest\ or in another board's
 * module/appdata folder.
 *
 * argv layout from the loader's dispatcher (work/src/main.c):
 *   argv[0] = ultra-loader.exe path
 *   argv[1] = game.exe path (the NESiCA2 game)
 *
 * We PE-sniff the game and pick the matching hook DLL bitness:
 *   64-bit game -> ultra_taito_nesica2_hook64.dll (spawn ultra-inject64.exe helper) [NORMAL]
 *   32-bit game -> ultra_taito_nesica2_hook32.dll (in-process CreateRemoteThread inject) [rare 32-bit repack]
 *
 * Ethical line: we ship NO NESiCA keys, we refuse to touch encrypted
 * containers, and we only run user-supplied decrypted dumps. The
 * ULTRA_NESICA_OFFLINE=1 knob neutralizes residual ALL.Net / NESYS network
 * checks so a stand-alone dump won't hang on server heartbeats -- it does NOT
 * impersonate the auth server. ULTRA_CARD_READER=1 (now defaulted on below)
 * arms a PRESENCE-only NESiCA card-reader stub in the hook: no real card
 * data, no player profiles, no network/e-amusement handshake -- it only
 * keeps a title's local reader poll from hanging forever.
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
    fseek(f, pe_off + 4, SEEK_SET);
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
 * docs/PROJECT_LICENSING_AND_SIGNING.md's hook-DLL note (2026-08-02).
 * Caller must DeleteFileA() the returned path once done with it. */
static int find_hook(int bits, char *out, size_t cap)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)&find_hook, &self);
    if (!self) { fprintf(stderr, "[taito_nesica2] GetModuleHandleEx(self) failed: %lu\n", GetLastError()); return 0; }

    const char *res_name = (bits == 32) ? "HOOK32" : "HOOK64";
    HRSRC info = FindResourceA(self, res_name, RT_RCDATA);
    if (!info) { fprintf(stderr, "[taito_nesica2] embedded hook resource \"%s\" not found\n", res_name); return 0; }
    DWORD sz = SizeofResource(self, info);
    HGLOBAL loaded = LoadResource(self, info);
    const void *ptr = loaded ? LockResource(loaded) : NULL;
    if (!ptr || !sz) { fprintf(stderr, "[taito_nesica2] embedded hook resource \"%s\" empty\n", res_name); return 0; }

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
    if (!fp) { fprintf(stderr, "[taito_nesica2] cannot write temp hook %s\n", out); return 0; }
    size_t wrote = fwrite(ptr, 1, sz, fp);
    fclose(fp);
    if (wrote != sz) { fprintf(stderr, "[taito_nesica2] short write to temp hook %s\n", out); DeleteFileA(out); return 0; }
    fprintf(stderr, "[taito_nesica2] extracted embedded hook (%s, %lu bytes) -> %s\n", res_name, sz, out);
    return 1;
}

/* 32-bit in-process inject */
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

/* 64-bit inject via ultra-inject64.exe helper */
static int inject_dll_64(DWORD pid, const char *dll_path)
{
    char loader_exe[MAX_PATH];
    GetModuleFileNameA(NULL, loader_exe, MAX_PATH);
    char *bs = strrchr(loader_exe, '\\'); if (bs) *bs = 0;

    char injector[MAX_PATH];
    const char *inj_candidates[] = { "ultra-inject64.exe", NULL };
    int found = 0;
    for (int i = 0; inj_candidates[i]; i++) {
        snprintf(injector, sizeof injector, "%s\\%s", loader_exe, inj_candidates[i]);
        if (GetFileAttributesA(injector) != INVALID_FILE_ATTRIBUTES) { found = 1; break; }
        snprintf(injector, sizeof injector, "%s\\modtest\\%s", loader_exe, inj_candidates[i]);
        if (GetFileAttributesA(injector) != INVALID_FILE_ATTRIBUTES) { found = 1; break; }
    }
    if (!found) {
        fprintf(stderr, "[taito_nesica2] ultra-inject64.exe not found (needed for 64-bit games)\n");
        return 0;
    }

    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\" %lu \"%s\"", injector, pid, dll_path);
    STARTUPINFOA si = { .cb = sizeof(STARTUPINFOA) };
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "[taito_nesica2] CreateProcess(inject64) failed: %lu\n", GetLastError());
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

__declspec(dllexport) int ul_taito_nesica2_run(int argc, char **argv)
{
    if (argc < 2 || !argv[1] || !*argv[1]) {
        fprintf(stderr, "[taito_nesica2] ERROR: no game path\n");
        return 1;
    }

    /* Per-game gating for titles already proven on this board.
     * NOTE: if this board's own env setup below would overwrite a locked
     * key, move this call to AFTER that block (namco_es3_main.c does). */
    ul_apply_game_locks(argv[1]);
    const char *game = argv[1];
    fprintf(stderr, "[taito_nesica2] launching %s\n", game);

    int bits = pe_bitness(game);
    if (bits == 0) { fprintf(stderr, "[taito_nesica2] not a PE file\n"); return 1; }
    fprintf(stderr, "[taito_nesica2] PE bitness = %d\n", bits);

    char hook[MAX_PATH];
    if (!find_hook(bits, hook, sizeof hook)) {
        fprintf(stderr, "[taito_nesica2] embedded hook for %d-bit could not be extracted\n", bits);
        return 1;
    }
    fprintf(stderr, "[taito_nesica2] hook = %s\n", hook);

    char workdir[MAX_PATH];
    strncpy(workdir, game, MAX_PATH - 1); workdir[MAX_PATH - 1] = 0;
    {
        char *b1 = strrchr(workdir, '\\');
        char *f1 = strrchr(workdir, '/');
        char *sep = (b1 && f1) ? (b1 > f1 ? b1 : f1) : (b1 ? b1 : f1);
        if (sep) *sep = 0;
    }

    /* Env vars the hook reads at injection time. NLX2 default is D3D11 64-bit;
     * we still arm D3D9 as a fall-through for the rare 32-bit NLX2 repack.
     * JVS_ANALOG=2 (fighter default: sticks + 6 buttons). */
    SetEnvironmentVariableA("ULTRA_BOARD_HINT",   "taito_nesica2");
    SetEnvironmentVariableA("ULTRA_SYSTEM_ID",    "taito_nesica2");
    /* controls.c's ultra_input_map_install() resolves the per-board input
     * profile from THIS var specifically (not BOARD_HINT/SYSTEM_ID) -- see
     * the Europa-R fix in X86_BOARDS.md S7 for the bug this closes. */
    SetEnvironmentVariableA("ULTRA_BOARD",        "taito_nesica2");
    SetEnvironmentVariableA("ULTRA_NESICA_GEN",   "2");
    SetEnvironmentVariableA("ULTRA_D3D11",        "1");     /* NLX2 = D3D11 primary */
    SetEnvironmentVariableA("ULTRA_D3D9",         "1");     /* fall-through for 32-bit NLX2 repacks */
    SetEnvironmentVariableA("ULTRA_CONTROLS",     "1");
    SetEnvironmentVariableA("ULTRA_JVS_ANALOG",   "2");     /* fighter (sticks + buttons) */
    SetEnvironmentVariableA("ULTRA_SEGA_WINDOWED","1");     /* avoid fullscreen device-lost stalls */
    SetEnvironmentVariableA("ULTRA_DONGLE_BYPASS","1");     /* Type X keychip presence */
    SetEnvironmentVariableA("ULTRA_NESICA_OFFLINE","1");    /* neutralize ALL.Net/NESYS heartbeats */
    if (!GetEnvironmentVariableA("ULTRA_CARD_READER", NULL, 0))
        SetEnvironmentVariableA("ULTRA_CARD_READER", "1");  /* NESiCA card-reader PRESENCE stub
                                                              * (card_reader_nesica2.c): benign
                                                              * "no card" idle only -- ethical
                                                              * DEFER preserved, no real card
                                                              * data/network auth. Override to
                                                              * "0" to disable. */
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
        fprintf(stderr, "[taito_nesica2] CreateProcess failed: %lu\n", GetLastError());
        DeleteFileA(hook);
        return 1;
    }
    fprintf(stderr, "[taito_nesica2] spawned pid=%lu (suspended)\n", pi.dwProcessId);

    int ok = (bits == 64)
        ? inject_dll_64(pi.dwProcessId, hook)
        : inject_dll_32(pi.hProcess, hook);
    if (!ok) {
        fprintf(stderr, "[taito_nesica2] inject failed\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        DeleteFileA(hook);
        return 1;
    }

    ResumeThread(pi.hThread);
    fprintf(stderr, "[taito_nesica2] hook injected, game resumed\n");
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0; GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    DeleteFileA(hook);
    return (int)rc;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID x) { (void)h;(void)r;(void)x; return TRUE; }
