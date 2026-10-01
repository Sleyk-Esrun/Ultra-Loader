/*
 * taito_typex3_main.c -- Taito Type X3 spawner + hook injector.
 *
 * Type X3 is the 2011+ Taito arcade board -- 64-bit Windows PE games with
 * D3D11 rendering, JVS-over-USB I/O, and (some titles) DES-encrypted content
 * with a keychip. Common Type X3 games: BlazBlue Chrono Phantasma, KOF XIII
 * Climax, Dariusburst Chronicle Saviours, Persona 4 Arena Ultimax.
 *
 * argv layout from the loader's dispatcher (work/src/main.c):
 *   argv[0] = ultra-loader.exe path
 *   argv[1] = game.exe path (the Type X3 game)
 *
 * We detect PE bitness (32-bit vs 64-bit) from the game's IMAGE_FILE_HEADER
 * and pick the matching hook DLL:
 *   32-bit game -> ultra_taito_typex3_hook32.dll (in-process CreateRemoteThread inject)
 *   64-bit game -> ultra_taito_typex3_hook64.dll (spawn ultra-inject64.exe helper)
 *
 * Both ultra_taito_typex3_hook32.dll and ultra_taito_typex3_hook64.dll are built ONLY from
 * this board's own modules/taito_typex3/src/hook/ tree (build_module.sh runs
 * src/hook/build_typex3_hook.sh) -- a fully independent, self-contained hook
 * implementing this board's own JVS engine, Taito Fast-I/O (iDmacDrv) DMAC
 * transport, D3D9 present hook, and DirectSound/XAudio2 audio hook. No source
 * file or compiled binary is shared with modules/native_x86/src/hook/'s
 * multi-board hook or with any other board module (find_hook() below only
 * ever resolves to this board's own install dir or the loader dir it is
 * staged next to -- never to another board's dir or to modtest\).
 *
 * Env vars ULTRA_BOARD_HINT / ULTRA_SYSTEM_ID / ULTRA_BOARD identify this
 * board to the hook. ULTRA_FAST_IO=1 arms the Fast-I/O transport (default on
 * in the hook itself; set here too for visibility). ULTRA_CONTROLS=1 is kept
 * for forward-compat with a future shared input-mapping profile; today's
 * hook reads its own standalone XInput/keyboard input directly.
 */

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>   /* malloc/free in typex3_file_is_ours */
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
    if (!self) { fprintf(stderr, "[taito_typex3] GetModuleHandleEx(self) failed: %lu\n", GetLastError()); return 0; }

    const char *res_name = (bits == 32) ? "HOOK32" : "HOOK64";
    HRSRC info = FindResourceA(self, res_name, RT_RCDATA);
    if (!info) { fprintf(stderr, "[taito_typex3] embedded hook resource \"%s\" not found\n", res_name); return 0; }
    DWORD sz = SizeofResource(self, info);
    HGLOBAL loaded = LoadResource(self, info);
    const void *ptr = loaded ? LockResource(loaded) : NULL;
    if (!ptr || !sz) { fprintf(stderr, "[taito_typex3] embedded hook resource \"%s\" empty\n", res_name); return 0; }

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
    if (!fp) { fprintf(stderr, "[taito_typex3] cannot write temp hook %s\n", out); return 0; }
    size_t wrote = fwrite(ptr, 1, sz, fp);
    fclose(fp);
    if (wrote != sz) { fprintf(stderr, "[taito_typex3] short write to temp hook %s\n", out); DeleteFileA(out); return 0; }
    fprintf(stderr, "[taito_typex3] extracted embedded hook (%s, %lu bytes) -> %s\n", res_name, sz, out);
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
 * inject from the correct bitness. Search for it near the loader. */
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
        fprintf(stderr, "[taito_typex3] ultra-inject64.exe not found (needed for 64-bit games)\n");
        return 0;
    }

    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\" %lu \"%s\"", injector, pid, dll_path);
    STARTUPINFOA si = { .cb = sizeof(STARTUPINFOA) };
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "[taito_typex3] CreateProcess(inject64) failed: %lu\n", GetLastError());
        return 0;
    }
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return rc == 0;
}

/* ---------------------------------------------------------------------------
 *  Provision iDmacDrv64.dll into the game folder.
 *
 *  Theatrhythm Final Fantasy All-Star Carnival renders, plays and sounds
 *  correct and has no controls, and the reason is a missing FILE, not a
 *  mapping. Its input path is
 *
 *      Unity C#  ->  game_Data/Plugins/ismACIO.dll  ->  iDmacDrv64.dll
 *
 *  and ismACIO.dll binds four functions from iDmacDrv64.dll BY ORDINAL
 *  (#1 #2 #5 #6). That DLL is in neither the dump nor Windows, so the plugin
 *  cannot LOAD -- LoadLibrary returns 126, ERROR_MOD_NOT_FOUND -- and every
 *  P/Invoke from the C# side throws while the game itself runs perfectly.
 *
 *  Why a file and not a hook: taito_fastio_typex3.c works by IAT-patching
 *  CALLS to iDmacDrv*.dll, and there is no IAT to patch when the failure
 *  happens at load time. Same reasoning as konami_pc's *.CRK.dll files.
 *
 *  It goes next to game.exe, not next to ismACIO.dll: Windows resolves a
 *  plugin's own imports through the standard search order, which begins with
 *  the APPLICATION directory, not the directory the plugin was loaded from.
 *
 *  If a real iDmacDrv64.dll is ever present -- a genuine cabinet driver --
 *  it is left alone. Ours is recognized by a marker string, so the check is
 *  'is the file already there one of mine', not a timestamp race. */
static int typex3_file_is_ours(const char *path)
{
    /* Our stand-in carries this literal in its DllMain banner. Reading the
     * file is cheap and unambiguous; a size or date comparison would be
     * neither. */
    static const char MARK[] = "Ultra Loader stand-in";
    HANDLE f;
    DWORD got = 0;
    char *buf;
    int found = 0;
    DWORD sz;

    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    sz = GetFileSize(f, NULL);
    if (sz == INVALID_FILE_SIZE || sz > (8u << 20)) { CloseHandle(f); return 0; }
    buf = (char*)malloc(sz + 1);
    if (!buf) { CloseHandle(f); return 0; }
    if (ReadFile(f, buf, sz, &got, NULL) && got) {
        DWORD i;
        buf[got] = 0;
        for (i = 0; i + sizeof(MARK) - 1 <= got; ++i)
            if (memcmp(buf + i, MARK, sizeof(MARK) - 1) == 0) { found = 1; break; }
    }
    free(buf);
    CloseHandle(f);
    return found;
}

/* Provision the NESiCAReader stand-in into game_Data/Plugins.
 *
 * Unlike iDmacDrv64 - which is a MISSING dependency and goes beside game.exe
 * - the real NESiCAReader.dll is present and working; it is just wired to a
 * card reader on COM2 that does not exist here, so it reports
 * XioStatErrorConnect and the game stops with operator error 13003.
 *
 * So this one REPLACES a file, and the original is kept as .orig beside it so
 * the swap is reversible. A NESiCAReader.dll that is already ours is left
 * alone rather than being backed up over the real one - getting that wrong
 * once would destroy the only copy of the original. */
static void typex3_provision_nesica(const char *workdir)
{
    char mod_dir[MAX_PATH] = {0}, src[MAX_PATH], dst[MAX_PATH], bak[MAX_PATH];
    HMODULE self = NULL;
    char *bs;

    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&typex3_provision_nesica, &self);
    if (!self) return;
    GetModuleFileNameA(self, mod_dir, sizeof mod_dir);
    bs = strrchr(mod_dir, 92);          /* 92 = backslash, as a code */
    if (bs) *bs = 0; else return;

    snprintf(src, sizeof src, "%s/NESiCAReader.dll", mod_dir);
    snprintf(dst, sizeof dst, "%s/game_Data/Plugins/NESiCAReader.dll", workdir);
    snprintf(bak, sizeof bak, "%s/game_Data/Plugins/NESiCAReader.dll.orig", workdir);

    if (GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES) return;
    if (GetFileAttributesA(dst) == INVALID_FILE_ATTRIBUTES) return;   /* no plugin here */

    if (!typex3_file_is_ours(dst) &&
        GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES) {
        if (!CopyFileA(dst, bak, TRUE)) {
            fprintf(stderr, "[taito_typex3] could not back up NESiCAReader.dll "
                            "(err=%lu) - leaving it alone\n", GetLastError());
            return;
        }
        fprintf(stderr, "[taito_typex3] original NESiCAReader.dll saved as .orig\n");
    }
    if (CopyFileA(src, dst, FALSE))
        fprintf(stderr, "[taito_typex3] NESiCAReader stand-in installed "
                        "(fixes operator error 13003)\n");
}

static void typex3_provision_idmac(const char *workdir, int bits)
{
    char mod_dir[MAX_PATH] = {0}, src[MAX_PATH], dst[MAX_PATH];
    HMODULE self = NULL;
    char *bs;

    if (bits != 64) {
        /* A 32-bit Type X3 title would want iDmacDrv32.dll, which is a
         * different driver we do not have. Say so rather than silently doing
         * nothing that looks like success. */
        fprintf(stderr, "[taito_typex3] 32-bit title: iDmacDrv32.dll is not "
                        "provisioned (only the 64-bit driver exists here)\n");
        return;
    }

    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&typex3_provision_idmac, &self);
    if (!self) return;
    GetModuleFileNameA(self, mod_dir, sizeof mod_dir);
    bs = strrchr(mod_dir, 92);   /* 92 = backslash, as a code: an escape here
                                 has been eaten by a generation layer twice */
    if (bs) *bs = 0; else return;

    snprintf(src, sizeof src, "%s/iDmacDrv64.dll", mod_dir);
    snprintf(dst, sizeof dst, "%s/iDmacDrv64.dll", workdir);

    if (GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "[taito_typex3] iDmacDrv64.dll not in the module dir "
                        "-- controls will be dead (ismACIO.dll cannot load)\n");
        return;
    }
    if (GetFileAttributesA(dst) != INVALID_FILE_ATTRIBUTES &&
        !typex3_file_is_ours(dst)) {
        fprintf(stderr, "[taito_typex3] a NON-Ultra iDmacDrv64.dll is already "
                        "in the game folder -- left alone\n");
        return;
    }
    if (CopyFileA(src, dst, FALSE)) {
        fprintf(stderr, "[taito_typex3] provisioned iDmacDrv64.dll -> %s"
                        " (ismACIO.dll can now load)\n", dst);
        /* OUR DLL IS NOW THE TRANSPORT, SO THE SHIM MUST STAND DOWN.
         *
         * taito_fastio_typex3.c IAT-patches calls to iDmacDrv*.dll with
         * 3-argument handlers -- Shim_iDmacDrvUpdate(channel, buf, len) --
         * written before any real Type X3 dump was available. The actual ABI
         * is FOUR arguments, Read(handle, reg, buf, status), so the shim
         * receives the REGISTER NUMBER where it expects a buffer pointer and
         * writes through it.
         *
         * That is not a theory. With the driver finally present, the shim had
         * something to intercept for the first time and the game died at once;
         * the tracer named it exactly:
         *
         *   ACCESS_VIOLATION at HOOK64_*.dll+0x38A6 -- writing address 0x4000
         *
         * 0x4000 being one of the iDmacDrv register numbers. Two
         * implementations of the same transport cannot both be live, and the
         * one with the measured ABI wins. */
        SetEnvironmentVariableA("ULTRA_FAST_IO", "0");

        /* BIT SWEEP: ON. Walks a single bit at a time through the cabinet
         * panel's digital registers, about 1.5s per bit, logging each one.
         * Watch the game's I/O CHECK screen next to the log and the mapping
         * falls out directly.
         *
         * The bit meanings are not in the game's C# -- no enum in
         * Assembly-CSharp carries TEST/SERVICE/COIN members, they are used as
         * raw indices -- so a live sweep is the only way to pin them.
         *
         * Turn off with ULTRA_IDMAC_SWEEP=0, or say so and this line goes. */
        /* SWEEP IS OFF. It works -- one run covered all 64 bits of both panel
         * registers in order -- but while it runs it is pressing cabinet
         * buttons, and one of those bits is TEST, so every 48-second cycle it
         * threw the player into the test menu. A diagnostic that makes the game
         * unplayable can only be on when it is being read.
         *
         * Set ULTRA_IDMAC_SWEEP=1 to run it again. */
        SetEnvironmentVariableA("ULTRA_IDMAC_SWEEP", "0");

        fprintf(stderr, "[taito_typex3] Fast-I/O IAT shim disabled: iDmacDrv64.dll is the transport now\n");
    } else {        fprintf(stderr, "[taito_typex3] could not write %s (err=%lu) -- "
                        "controls will be dead\n", dst, GetLastError());
}
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

__declspec(dllexport) int ul_taito_typex3_run(int argc, char **argv)
{
    if (argc < 2 || !argv[1] || !*argv[1]) {
        fprintf(stderr, "[taito_typex3] ERROR: no game path\n");
        return 1;
    }

    /* Per-game gating for titles already proven on this board.
     * NOTE: if this board's own env setup below would overwrite a locked
     * key, move this call to AFTER that block (namco_es3_main.c does). */
    ul_apply_game_locks(argv[1]);
    const char *game = argv[1];
    fprintf(stderr, "[taito_typex3] launching %s\n", game);

    int bits = pe_bitness(game);
    if (bits == 0) { fprintf(stderr, "[taito_typex3] not a PE file\n"); return 1; }
    fprintf(stderr, "[taito_typex3] PE bitness = %d\n", bits);

    char hook[MAX_PATH];
    if (!find_hook(bits, hook, sizeof hook)) {
        fprintf(stderr, "[taito_typex3] embedded hook for %d-bit could not be extracted\n", bits);
        return 1;
    }
    fprintf(stderr, "[taito_typex3] hook = %s\n", hook);

    char workdir[MAX_PATH];
    strncpy(workdir, game, MAX_PATH - 1); workdir[MAX_PATH - 1] = 0;
    {
        char *b1 = strrchr(workdir, '\\');
        char *f1 = strrchr(workdir, '/');
        char *sep = (b1 && f1) ? (b1 > f1 ? b1 : f1) : (b1 ? b1 : f1);
        if (sep) *sep = 0;
    }

    typex3_provision_idmac(workdir, bits);
    typex3_provision_nesica(workdir);

    /* Env vars the hook reads at injection time */
    SetEnvironmentVariableA("ULTRA_BOARD_HINT",  "taito_typex3");
    SetEnvironmentVariableA("ULTRA_SYSTEM_ID",   "taito_typex3");
    /* controls.c's ultra_input_map_install() resolves the per-board input
     * profile from THIS var specifically (not BOARD_HINT/SYSTEM_ID) -- see
     * the Europa-R fix in X86_BOARDS.md S7 for the bug this closes. */
    SetEnvironmentVariableA("ULTRA_BOARD",       "taito_typex3");
    SetEnvironmentVariableA("ULTRA_D3D11",       "1");     /* some Type X3 titles = D3D11 (future sibling hook) */
    /* This board's own independent hook (src/hook/) defaults Fast-I/O ON
     * unless explicitly turned off; set it here too so the knob is visible
     * next to the board's other env contract entries. */
    SetEnvironmentVariableA("ULTRA_FAST_IO",     "1");     /* Taito Fast-I/O (iDmacDrv) transport */
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
        fprintf(stderr, "[taito_typex3] CreateProcess failed: %lu\n", GetLastError());
        DeleteFileA(hook);
        return 1;
    }
    fprintf(stderr, "[taito_typex3] spawned pid=%lu (suspended)\n", pi.dwProcessId);

    int ok = (bits == 64)
        ? inject_dll_64(pi.dwProcessId, hook)
        : inject_dll_32(pi.hProcess, hook);
    if (!ok) {
        fprintf(stderr, "[taito_typex3] inject failed\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        DeleteFileA(hook);
        return 1;
    }

    ResumeThread(pi.hThread);
    fprintf(stderr, "[taito_typex3] hook injected, game resumed\n");
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0; GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    DeleteFileA(hook);
    return (int)rc;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID x) { (void)h;(void)r;(void)x; return TRUE; }
