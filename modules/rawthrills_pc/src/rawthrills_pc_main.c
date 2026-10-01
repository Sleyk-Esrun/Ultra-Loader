/*
 * rawthrills_pc_main.c -- Raw Thrills PC spawner + hook injector.
 *
 * The loader's dispatcher (work/src/main.c) calls ul_rawthrills_pc_run(argc,
 * argv) after detecting a Raw Thrills PC arcade .exe game (The Fast & The
 * Furious Super Bikes, Big Buck Hunter Pro/HD/Safari, Target Terror, etc.)
 * with ULTRA_MODULE=rawthrills_pc (or auto-detected from the game's path).
 *
 * argv layout:
 *   argv[0] = ultra-loader.exe path
 *   argv[1] = game.exe path (typically sdaemon.exe -- Raw Thrills' RawApp
 *             generation packs the entire game into a single sdaemon.exe;
 *             newer cabs use a per-title .exe but the spawn shape is the
 *             same)
 *
 * What we do (same shape as taito_typex_main.c):
 *   1. CreateProcess(game.exe, CREATE_SUSPENDED) so we can inject the hook
 *      DLL BEFORE any game code runs. RawApp resolves D3D8/DirectInput8/
 *      JammaUSB/USBWD/PortTalk endpoints dynamically during early startup --
 *      a post-launch LoadLibrary injection consistently loses the race.
 *   2. Write the hook DLL path into the game's VA via VirtualAllocEx +
 *      WriteProcessMemory, then CreateRemoteThread(LoadLibraryA, dll_path)
 *      so the hook's DllMain (and its IAT/GetProcAddress interception)
 *      installs first.
 *   3. Resume the game's main thread.
 *   4. WaitForSingleObject on the game process so the loader stays alive
 *      until the game exits. Return the game's exit code.
 *
 * Hook DLL: ultra_rawthrills_hook32.dll. Search order:
 *   1. %LOCALAPPDATA%\Ultra Loader\modules\rawthrills_pc\ultra_rawthrills_hook32.dll
 *   2. <loader_exe_dir>\ultra_rawthrills_hook32.dll
 *   3. <loader_exe_dir>\modtest\ultra_rawthrills_hook32.dll
 *
 * Engine facts (verified from sdaemon.exe / TFAFSB dump strings, NOT the
 * stale board_profile guess of "D3D9+D3D11/serial-JVS"):
 *   - Graphics: D3D8 primary (Direct3DCreate8) with cg.dll + cgD3D9.dll for
 *     movies/UI. Newer Raw Thrills cabs use D3D9/D3D11 -- detect at runtime
 *     via ULTRA_D3D11=auto so the hook arms the right present chain.
 *   - Input: DirectInput8.
 *   - Arcade I/O: JammaUSB (Raw Thrills proprietary USB-JAMMA board) --
 *     NOT serial JVS. Strings: JUSB_WHEEL, JUSB_GetReport, "JAMMAUSB
 *     DISCONNECTED!!!".
 *   - Watchdog: USBWD board ("WAITING FOR USBWD BOARD").
 *   - Direct port I/O: PortTalk.SYS ring-0 driver -- WinXP-Embedded era.
 *   - Hardcoded path: c:\rawart (asset pack).
 *   - OS target: Windows XP Embedded (EWF + PortTalk).
 *
 * Env vars set for the hook DLL:
 *   ULTRA_BOARD_HINT=rawthrills_pc
 *   ULTRA_SYSTEM_ID=rawthrills_pc
 *   ULTRA_RAWTHRILLS_TITLE=<title or empty>
 *   ULTRA_SEGA_WINDOWED=1        -- force windowed so the hook can upscale
 *                                   the native back-buffer to 1080p/4K
 *   ULTRA_D3D8=1                 -- arm D3D8 present hook (TFAFSB-class)
 *   ULTRA_D3D11=auto             -- let the hook also detect D3D9/D3D11 at
 *                                   runtime for newer Raw Thrills cabs
 *   ULTRA_DINPUT8=1              -- arm DirectInput8 emu
 *   ULTRA_JAMMAUSB=1             -- arm JammaUSB transport stub
 *   ULTRA_USBWD=1                -- arm USB watchdog presence stub
 *   ULTRA_PORTTALK_BYPASS=1      -- fake PortTalk.SYS + redirect inb/outb
 *   ULTRA_EWF_STUB=1             -- stub ewfapi.dll (commit/checkpoint
 *                                   no-ops) for non-XP-Embedded hosts
 *   ULTRA_LIGHTGUN=auto          -- universal lightgun bridge (BBH family)
 *   ULTRA_RAWART_REDIRECT=<dir>  -- redirect c:\rawart to game folder
 *   ULTRA_RT_AUDIO=1             -- arm DirectSound/XAudio2 diagnostic hook
 *                                   (log every create + SetCooperativeLevel
 *                                   retry-on-failure; added 2026-08-01)
 */

#include <windows.h>
#include <winioctl.h>
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
    if (!self) { fprintf(stderr, "[rawthrills_pc] GetModuleHandleEx(self) failed: %lu\n", GetLastError()); return 0; }

    HRSRC info = FindResourceA(self, "HOOK32", RT_RCDATA);
    if (!info) { fprintf(stderr, "[rawthrills_pc] embedded hook resource not found\n"); return 0; }
    DWORD sz = SizeofResource(self, info);
    HGLOBAL loaded = LoadResource(self, info);
    const void *ptr = loaded ? LockResource(loaded) : NULL;
    if (!ptr || !sz) { fprintf(stderr, "[rawthrills_pc] embedded hook resource empty\n"); return 0; }

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
            snprintf(out, cap, "%sultra_rawthrills_hook32_%lu.dll", tmp_dir, GetCurrentProcessId());
            fp = fopen(out, "wb");
            if (fp) break;
            wl = strlen(why);
            snprintf(why + wl, sizeof why - wl, "%s%s", wl ? ", " : "", out);
        }
        /* Nothing took it.  Put the candidate list where the caller's existing
         * error message will print it -- "out" only means a path on success. */
        if (!fp) snprintf(out, cap, "%s", why[0] ? why : "(no candidate directory)");
    }
    if (!fp) { fprintf(stderr, "[rawthrills_pc] cannot write temp hook %s\n", out); return 0; }
    size_t wrote = fwrite(ptr, 1, sz, fp);
    fclose(fp);
    if (wrote != sz) { fprintf(stderr, "[rawthrills_pc] short write to temp hook %s\n", out); DeleteFileA(out); return 0; }
    fprintf(stderr, "[rawthrills_pc] extracted embedded hook (%lu bytes) -> %s\n", sz, out);
    return 1;
}

static int inject_dll(HANDLE proc, const char *dll_path)
{
    size_t n = strlen(dll_path) + 1;
    LPVOID remote = VirtualAllocEx(proc, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        fprintf(stderr, "[rawthrills_pc] VirtualAllocEx failed: %lu\n", GetLastError());
        return 0;
    }
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(proc, remote, dll_path, n, &wrote) || wrote != n) {
        fprintf(stderr, "[rawthrills_pc] WriteProcessMemory failed: %lu\n", GetLastError());
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
        fprintf(stderr, "[rawthrills_pc] CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    DWORD wr = WaitForSingleObject(th, 15000);
    DWORD remote_load_rc = 0;
    GetExitCodeThread(th, &remote_load_rc);
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    if (wr != WAIT_OBJECT_0) {
        fprintf(stderr, "[rawthrills_pc] remote LoadLibraryA timed out (wait=0x%lX)\n", wr);
        return 0;
    }
    if (remote_load_rc == 0) {
        fprintf(stderr, "[rawthrills_pc] remote LoadLibraryA returned NULL (hook DllMain refused or DLL bad)\n");
        return 0;
    }
    fprintf(stderr, "[rawthrills_pc] remote LoadLibraryA OK (HMODULE=0x%lX)\n", remote_load_rc);
    return 1;
}

/* Detect a per-title hint from the game's exe name / path. The dispatcher
 * may already set ULTRA_RAWTHRILLS_TITLE; if not, we make a best-effort
 * guess so the hook can pick the right per-title profile (e.g. tfafsb,
 * bbhpro, bbhhd, bbhsafari, target_terror, etc.).
 *
 * We look for distinctive directory-name tokens. sdaemon.exe is shared
 * across many RawApp titles -- the title comes from the FOLDER name. */
static void hint_title_from_path(const char *game_path)
{
    char existing[64] = {0};
    GetEnvironmentVariableA("ULTRA_RAWTHRILLS_TITLE", existing, sizeof existing);
    if (existing[0]) return;  /* dispatcher already set it; respect that */

    char buf[MAX_PATH];
    strncpy(buf, game_path, MAX_PATH - 1); buf[MAX_PATH - 1] = 0;

    /* lowercase a working copy for substring matching */
    char lower[MAX_PATH];
    size_t i;
    for (i = 0; buf[i] && i < MAX_PATH - 1; i++) {
        char c = buf[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        lower[i] = c;
    }
    lower[i] = 0;

    const char *title = NULL;
    if      (strstr(lower, "super bikes") || strstr(lower, "superbikes"))      title = "tfafsb";
    else if (strstr(lower, "fast and the furious drift") ||
             strstr(lower, "fast & the furious drift") ||
             strstr(lower, "fnf drift"))                                       title = "fnf_drift";
    else if (strstr(lower, "fast and the furious") ||
             strstr(lower, "fast & the furious"))                              title = "fnf";
    else if (strstr(lower, "big buck hunter pro") || strstr(lower, "bbh pro")) title = "bbhpro";
    else if (strstr(lower, "big buck hunter hd")  || strstr(lower, "bbh hd"))  title = "bbhhd";
    else if (strstr(lower, "big buck safari")     || strstr(lower, "safari")) title = "bbhsafari";
    else if (strstr(lower, "target terror"))                                   title = "target_terror";
    else if (strstr(lower, "nascar"))                                          title = "nascar";
    else if (strstr(lower, "h2overdrive"))                                     title = "h2overdrive";
    else if (strstr(lower, "nitro"))                                           title = "nitro";

    if (title) {
        SetEnvironmentVariableA("ULTRA_RAWTHRILLS_TITLE", title);
        fprintf(stderr, "[rawthrills_pc] title hint: %s\n", title);
    } else {
        fprintf(stderr, "[rawthrills_pc] title hint: <unknown>\n");
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

/* ---- short-path launch --------------------------------------------------- *
 * RawApp titles build data paths (e.g. "<gamedir>\mp.pdt") into FIXED ~64-byte
 * stack buffers.  A long install path -- e.g.
 *   H:\Modern Arcade Games\Raw Thrills Dumps\The Fast and the Furious Super Bikes\
 * overruns that buffer: the path string overwrites the saved return address
 * and the game dies with FAST_FAIL_INVALID_EXCEPTION_CHAIN (confirmed via WER
 * dump on TFAFSB -- eip landed inside the path text).  Real cabinets run from a
 * short root like C:\game, so the bug never shows there.
 *
 * Fix: junction a SHORT link on the game's OWN volume to the game directory and
 * launch the exe through it.  GetModuleFileName() then returns the short link
 * path (junctions are not resolved in the returned string -- verified), so the
 * game's own path math stays inside its buffers.  Same FSCTL_SET_REPARSE_POINT
 * primitive the loader's junction.c uses: standard-user, no admin, no reboot.
 * Best-effort -- on any failure we fall back to the original long path (no worse
 * than before). */

#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003L
#endif
#ifndef FSCTL_SET_REPARSE_POINT
#define FSCTL_SET_REPARSE_POINT CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 41, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
#endif
#ifndef MAXIMUM_REPARSE_DATA_BUFFER_SIZE
#define MAXIMUM_REPARSE_DATA_BUFFER_SIZE 16384
#endif

typedef struct _UL_RT_REPARSE {
    DWORD ReparseTag; WORD ReparseDataLength; WORD Reserved;
    WORD SubstituteNameOffset; WORD SubstituteNameLength;
    WORD PrintNameOffset; WORD PrintNameLength;
    WCHAR PathBuffer[1];
} UL_RT_REPARSE;

/* Create (or re-point) a directory junction `link` -> `target`. Returns 1 on
 * success, 0 on any failure. Removes a stale reparse point at `link` first;
 * refuses to touch a real (non-reparse) directory or file already there. */
static int ul_rt_make_junction(const char *link, const char *target)
{
    char abs_target[MAX_PATH] = {0};
    HANDLE h;
    BYTE raw[MAXIMUM_REPARSE_DATA_BUFFER_SIZE] = {0};
    UL_RT_REPARSE *rb = (UL_RT_REPARSE *)raw;
    WCHAR subst[MAX_PATH + 8], print[MAX_PATH];
    int sn, subst_len, print_len;
    WORD subst_len_b, print_off, print_len_b, data_len;
    DWORD total, ret = 0, lattr;
    size_t al;
    BOOL ok;

    if (!GetFullPathNameA(target, MAX_PATH, abs_target, NULL)) return 0;
    al = strlen(abs_target);
    while (al && abs_target[al-1] == '\\') abs_target[--al] = 0;

    lattr = GetFileAttributesA(link);
    if (lattr != INVALID_FILE_ATTRIBUTES) {
        if (lattr & FILE_ATTRIBUTE_REPARSE_POINT) {
            if (!RemoveDirectoryA(link)) return 0;   /* drop stale junction */
        } else {
            return 0;                                /* real dir/file -- do not clobber */
        }
    }
    if (!CreateDirectoryA(link, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return 0;

    h = CreateFileA(link, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) { RemoveDirectoryA(link); return 0; }

    sn = MultiByteToWideChar(CP_ACP, 0, abs_target, -1, print, MAX_PATH);
    if (sn <= 0) { CloseHandle(h); RemoveDirectoryA(link); return 0; }
    sn--;                                            /* drop NUL from count */

    subst[0] = L'\\'; subst[1] = L'?'; subst[2] = L'?'; subst[3] = L'\\';
    memcpy(&subst[4], print, sn * sizeof(WCHAR));
    subst_len = 4 + sn; print_len = sn;

    subst_len_b = (WORD)(subst_len * sizeof(WCHAR));
    print_off   = (WORD)(subst_len_b + sizeof(WCHAR));
    print_len_b = (WORD)(print_len * sizeof(WCHAR));

    memcpy((BYTE *)rb->PathBuffer, subst, subst_len_b);
    *(WCHAR *)((BYTE *)rb->PathBuffer + subst_len_b) = 0;
    memcpy((BYTE *)rb->PathBuffer + print_off, print, print_len_b);
    *(WCHAR *)((BYTE *)rb->PathBuffer + print_off + print_len_b) = 0;

    rb->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    rb->SubstituteNameOffset = 0;
    rb->SubstituteNameLength = subst_len_b;
    rb->PrintNameOffset      = print_off;
    rb->PrintNameLength      = print_len_b;
    data_len = (WORD)(4 * sizeof(WORD) + subst_len_b + sizeof(WCHAR)
                      + print_len_b + sizeof(WCHAR));
    rb->ReparseDataLength = data_len;
    total = (DWORD)(sizeof(DWORD) + 2 * sizeof(WORD)) + data_len;

    ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, raw, total, NULL, 0, &ret, NULL);
    CloseHandle(h);
    if (!ok) { RemoveDirectoryA(link); return 0; }
    return 1;
}

/* Given the (long) game exe path, junction a short link on the same volume to
 * the game dir and write the short exe path to out_exe. Returns 1 if a short
 * path is ready, 0 to keep using the original. */
static int ul_rt_short_launch(const char *game, char *out_exe, size_t cap)
{
    char gamedir[MAX_PATH], link[16];
    const char *base;
    size_t need;

    if (!game || !game[0] || game[1] != ':') return 0;

    /* Already short enough? RawApp's buffer is ~64 bytes for <dir>\file; keep a
     * wide margin -- if the dir is already <= 24 chars, leave it alone. */
    strncpy(gamedir, game, MAX_PATH - 1); gamedir[MAX_PATH - 1] = 0;
    { char *bs = strrchr(gamedir, '\\'); if (!bs) return 0; *bs = 0; }
    if (strlen(gamedir) <= 24) return 0;

    /* Short link "<vol>:\_ULRT" on the game's own volume (same-volume junction). */
    link[0] = game[0]; link[1] = ':'; link[2] = '\\';
    link[3] = '_'; link[4] = 'U'; link[5] = 'L'; link[6] = 'R'; link[7] = 'T';
    link[8] = 0;

    if (!ul_rt_make_junction(link, gamedir)) {
        fprintf(stderr, "[rawthrills_pc] short-path junction %s -> %s failed (gle=%lu); "
                        "launching long path\n", link, gamedir, GetLastError());
        return 0;
    }

    base = strrchr(game, '\\'); base = base ? base + 1 : game;
    need = strlen(link) + 1 + strlen(base) + 1;
    if (need > cap) return 0;
    snprintf(out_exe, cap, "%s\\%s", link, base);
    fprintf(stderr, "[rawthrills_pc] short-path launch: %s (junction -> %s)\n",
            out_exe, gamedir);
    return 1;
}

/* Force ULTRA_JVS_LOG to a per-title file in the loader's own log\ dir when it
 * is unset or still points at the shared default debug log. The hook tree's
 * ul_rt_log() tees into ULTRA_JVS_LOG, so without this the whole I/O trace lands
 * in the shared ultra_jvs_debug.log mixed with every other title's serial JVS
 * chatter (observed 2026-09-30). A user who deliberately set a custom path keeps
 * it. loader_exe = argv[0]; the log dir is <dir(loader_exe)>\log. */
static void ul_rt_force_title_log(const char *loader_exe)
{
    char cur[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("ULTRA_JVS_LOG", cur, sizeof cur);
    int is_default = (n == 0) || !!strstr(cur, "jvs_debug") || !!strstr(cur, "jvs_dbg");
    if (n != 0 && !is_default) return;   /* respect a deliberate custom path */

    char dir[MAX_PATH];
    strncpy(dir, loader_exe ? loader_exe : "", MAX_PATH - 1); dir[MAX_PATH - 1] = 0;
    for (char *p = dir; *p; p++) if (*p == '/') *p = '\\';
    { char *bs = strrchr(dir, '\\'); if (!bs) return; *bs = 0; }

    /* Map the internal title hint to the log basename the containers expect. */
    char title[64] = {0};
    GetEnvironmentVariableA("ULTRA_RAWTHRILLS_TITLE", title, sizeof title);
    const char *name = title[0] ? title : "rawthrills_pc";
    if (strcmp(name, "tfafsb") == 0) name = "superbikes";

    char logdir[MAX_PATH], path[MAX_PATH];
    snprintf(logdir, sizeof logdir, "%s\\log", dir);
    CreateDirectoryA(logdir, NULL);
    snprintf(path, sizeof path, "%s\\%s.log", logdir, name);
    SetEnvironmentVariableA("ULTRA_JVS_LOG", path);
    fprintf(stderr, "[rawthrills_pc] ULTRA_JVS_LOG -> %s\n", path);
}

__declspec(dllexport) int ul_rawthrills_pc_run(int argc, char **argv)
{
    if (argc < 2 || !argv[1] || !*argv[1]) {
        fprintf(stderr, "[rawthrills_pc] ERROR: no game path (argc=%d)\n", argc);
        return 1;
    }

    /* Per-game gating for titles already proven on this board.
     * NOTE: if this board's own env setup below would overwrite a locked
     * key, move this call to AFTER that block (namco_es3_main.c does). */
    ul_apply_game_locks(argv[1]);
    /* Normalize forward slashes to backslashes -- the loader's dispatcher
     * passes paths in unix style (I:/Raw Thrills/...), but CreateProcessA's
     * lpApplicationName needs Windows-style separators or it returns 267
     * (ERROR_DIRECTORY). */
    char game_buf[MAX_PATH];
    strncpy(game_buf, argv[1], MAX_PATH - 1); game_buf[MAX_PATH - 1] = 0;
    for (char *p = game_buf; *p; p++) if (*p == '/') *p = '\\';
    const char *game = game_buf;
    fprintf(stderr, "[rawthrills_pc] launching %s\n", game);

    /* Derive workdir = dir(game). RawApp titles read every asset via paths
     * relative to the launch exe (rez/pvs/r2c/prefs) -- if the CWD isn't the
     * game folder the first asset open fails and the game exits silently. */
    char workdir[MAX_PATH];
    strncpy(workdir, game, MAX_PATH - 1); workdir[MAX_PATH - 1] = 0;
    {
        char *bs = strrchr(workdir, '\\');
        if (bs) *bs = 0;
    }

    char hook[MAX_PATH];
    if (!find_hook(hook, sizeof hook)) {
        fprintf(stderr, "[rawthrills_pc] ERROR: embedded hook could not be extracted\n");
        MessageBoxA(NULL,
            "Raw Thrills hook could not be extracted from this module.\n\n"
            "The installed ultra_rawthrills_pc.dll may be corrupt -- reinstall it\n"
            "via Settings -> Install Module.",
            "Ultra Loader -- Raw Thrills Hook Missing",
            MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }
    fprintf(stderr, "[rawthrills_pc] hook = %s\n", hook);

    /* Env vars the hook DLL reads at injection time.
     *
     * ULTRA_D3D11=auto -- the hook detects D3D9/D3D11 at runtime; RawApp/
     *   TFAFSB-class titles use D3D8 (set explicitly via ULTRA_D3D8) while
     *   newer Raw Thrills cabs (Nitro / H2Overdrive era) use D3D9 or D3D11.
     *   "auto" lets one hook DLL serve both generations.
     *
     * ULTRA_LIGHTGUN=auto -- enables the public lightgun service for gun
     *   titles in the family (Big Buck Hunter, Target Terror). Inert for
     *   wheel/bike cabs like TFAFSB.
     *
     * ULTRA_SEGA_WINDOWED=1 -- force windowed-mode rendering so the hook
     *   can upscale the native back-buffer to the user's chosen output
     *   resolution. Same trick that unblocked Haunted Museum on Type X2. */
    SetEnvironmentVariableA("ULTRA_BOARD_HINT", "rawthrills_pc");
    SetEnvironmentVariableA("ULTRA_SYSTEM_ID",  "rawthrills_pc");
    /* controls.c's ultra_input_map_install() resolves the per-board input
     * profile from THIS var specifically (not BOARD_HINT/SYSTEM_ID) -- see
     * the Europa-R fix in X86_BOARDS.md S7 for the bug this closes. */
    SetEnvironmentVariableA("ULTRA_BOARD",      "rawthrills_pc");
    hint_title_from_path(game);
    ul_rt_force_title_log(argv[0]);
    SetEnvironmentVariableA("ULTRA_SEGA_WINDOWED",    "1");
    SetEnvironmentVariableA("ULTRA_D3D8",             "1");
    SetEnvironmentVariableA("ULTRA_D3D11",            "auto");
    SetEnvironmentVariableA("ULTRA_DINPUT8",          "1");
    SetEnvironmentVariableA("ULTRA_JAMMAUSB",         "1");
    SetEnvironmentVariableA("ULTRA_USBWD",            "1");
    SetEnvironmentVariableA("ULTRA_PORTTALK_BYPASS",  "1");
    SetEnvironmentVariableA("ULTRA_EWF_STUB",         "1");
    SetEnvironmentVariableA("ULTRA_LIGHTGUN",         "auto");
    SetEnvironmentVariableA("ULTRA_RT_AUDIO",         "1");
    if (workdir[0])
        SetEnvironmentVariableA("ULTRA_RAWART_REDIRECT", workdir);

    /* Launch through a short junction path if the install path is long enough
     * to overflow RawApp's fixed path buffers. Done AFTER title detection and
     * ULTRA_RAWART_REDIRECT above (those want the real long dir); the junction
     * resolves to the same files, so assets still load. */
    char shortexe[MAX_PATH];
    if (ul_rt_short_launch(game, shortexe, sizeof shortexe)) {
        game = shortexe;
        strncpy(workdir, shortexe, MAX_PATH - 1); workdir[MAX_PATH - 1] = 0;
        { char *bs = strrchr(workdir, '\\'); if (bs) *bs = 0; }
    }

    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\"", game);
    /* bInheritHandles=TRUE so the child's CRT inherits our STD_ERROR_HANDLE
     * (the freopen'd ultra.log). The hook's fprintf(stderr,...) diagnostic
     * lines (D3D8 CreateDevice / JammaUSB armed / USBWD ack / etc.) will
     * then land in the same log file the loader writes, which is the only
     * way to see what the hook is doing inside the child without a console. */
    if (!CreateProcessA(game, cmd, NULL, NULL, TRUE,
                        CREATE_SUSPENDED, NULL,
                        workdir[0] ? workdir : NULL, &si, &pi)) {
        fprintf(stderr, "[rawthrills_pc] CreateProcess(%s) failed: %lu\n",
                game, GetLastError());
        DeleteFileA(hook);
        return 1;
    }
    fprintf(stderr, "[rawthrills_pc] spawned pid=%lu (suspended)\n", pi.dwProcessId);

    if (!inject_dll(pi.hProcess, hook)) {
        fprintf(stderr, "[rawthrills_pc] hook inject failed; terminating game\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        DeleteFileA(hook);
        return 1;
    }

    ResumeThread(pi.hThread);
    fprintf(stderr, "[rawthrills_pc] hook injected, game resumed -- waiting for exit\n");

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0; GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    DeleteFileA(hook);
    fprintf(stderr, "[rawthrills_pc] game exited with code %lu\n", rc);
    return (int)rc;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)hinst; (void)reason; (void)reserved;
    return TRUE;
}
