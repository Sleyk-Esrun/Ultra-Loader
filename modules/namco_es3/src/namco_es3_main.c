/*
 * namco_es3_main.c -- Namco ES3 spawner + hook injector.
 *
 * The loader's dispatcher (work/src/main.c) calls ul_namco_es3_run(argc, argv)
 * after resolving a Namco ES3 .exe game (or when the operator forces
 * ULTRA_MODULE=namco_es3). argv layout:
 *   argv[0] = ultra-loader.exe path
 *   argv[1] = game.exe path (the ES3 game)
 *
 * Namco ES3 (System ES3 / "Namco ES3") is the mid/late-2010s Namco arcade PC
 * platform. Titles cover both bitnesses:
 *   64-bit (D3D11, UE3/UE4): Tekken 7, Pokken Tournament
 *   32-bit (D3D9):           Mario Kart Arcade GP DX (MKDX), Wangan Midnight
 *                            Maximum Tune 5, Time Crisis 5, Dark Escape 4D
 * The board's I/O box is ESIF2 -- a JVS-over-USB transport lineage-related to
 * Sega's IO4-USB. Authentication uses amdaemon + amauthd, both descended from
 * the same lineage as Sega's amdaemon/amauth (which is why the Sega Nu hook's
 * amdaemon stub already accepts the generic ULTRA_AMDAEMON=1 gate; the
 * BootID/OK handshake shape is vendor-agnostic). Namco ES3 has NO keychip
 * (unlike Sega Nu's A7/N2 smart-card) and NO BitLocker/TPM container.
 *
 * Bitness auto-detect via PE header (copied verbatim from sega_nu_main.c).
 * Hook DLL selection:
 *   Preferred (per-board): ultra_namco_es3_hook{32,64}.dll
 *   Fallback (shared):     ultra_sega_nu_hook{32,64}.dll  (has amdaemon stub)
 *   Last resort:           ultra_typex_hook{32,64}.dll    (D3D9/11 + JVS)
 *
 * Env vars set BEFORE CreateProcess (all default off when unset -- byte-safe
 * globally, only turn on for Namco ES3):
 *   ULTRA_LAUNCH_MODULE = "namco_es3"
 *   ULTRA_BOARD_HINT    = "namco_es3"
 *   ULTRA_SYSTEM_ID     = "namco_es3"
 *   ULTRA_D3D9          = "1"   -- MKDX / WMMT5 / TC5 / DE4D
 *   ULTRA_D3D11         = "1"   -- Tekken 7 / Pokken; both armed, first-wins
 *                                  runtime detect per project memory
 *   ULTRA_CONTROLS      = "1"   -- wake input-mapping layer (kbd/XInput/DInput)
 *   ULTRA_JVS_ANALOG    = "4"   -- Tekken 7 fighter reports more analog than
 *                                  the default (steering + brake/gas + LP/RP
 *                                  pressure axes); 4 covers ES3 racers too
 *   ULTRA_NAMCO_IO      = "1"   -- ESIF2 JVS-over-USB (Namco's IO4 cousin)
 *   ULTRA_SEGA_AMDAEMON = "1"   -- reuse Sega Nu's amdaemon stub for Namco;
 *                                  the auth-daemon BootID/OK handshake is
 *                                  vendor-agnostic and the Nu stub already
 *                                  documents "the existing taito_typex
 *                                  namco_amdaemon.c" as its lineage
 *   ULTRA_AMAUTH        = "1"   -- amauthd (Namco lineage of ALL.Net amauth)
 *   ULTRA_SEGA_WINDOWED = "1"   -- force windowed rendering so the D3D9/11
 *                                  hook can upscale to the user's chosen
 *                                  output res (FSR/framegen)
 *   ULTRA_DISMISS_DIALOGS = "1" -- auto-close dongle/error/config popups
 *   ULTRA_FPS_CAP       = "60"  -- ES3 runs 60 Hz natively
 *   ULTRA_INPOUT32_BYPASS = "1" -- neutralize inpout32.dll if the game loads it
 *   ULTRA_HOOK_D3D9     = "1"   -- both hooks armed, first-wins
 *   ULTRA_HOOK_D3D11    = "1"
 *
 * NOT set (Sega-Nu-only): ULTRA_SEGA_KEYCHIP, ULTRA_SEGA_TRUECRYPT,
 * ULTRA_SEGA_ALLNET, ULTRA_SEGA_NETWORK.
 *
 * Status: SCAFFOLDED, content-gated. No ES3 title dumps in-tree; module can
 * spawn+inject today, but nothing to launch until an operator supplies a
 * game .exe.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define LOG(...) do { fprintf(stderr, "[namco_es3] " __VA_ARGS__); fflush(stderr); } while (0)

/* Peek game PE header. Returns 32 (PE32) or 64 (PE32+). Defaults to 64 on
 * error since Tekken 7 is the flagship ES3 title. */
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

/* This board's hook is no longer installed as a standalone file. It's
 * embedded as an RCDATA resource ("HOOK32"/"HOOK64") inside THIS DLL (see
 * hook_res.rc, baked in by build_module.sh) and extracted to a fresh temp
 * file right before each injection. This means signing/verifying this one
 * DLL automatically covers the hook too, and there's no separate hook file
 * sitting on disk between launches for anything to tamper with -- see
 * docs/PROJECT_LICENSING_AND_SIGNING.md's hook-DLL note (2026-08-02).
 * Caller must DeleteFileA() the returned path once done with it. Works the
 * same for both bitnesses: the 64-bit inject path (ultra-inject64.exe) just
 * takes a file path, and doesn't care that it's a fresh temp copy. */
static int find_hook(char *out, size_t cap, int bits, const char **selected_name)
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
     * ONLY ONE.  The old code called GetTempPathA() once and gave up if the
     * write failed, reporting "hook could not be extracted ... the module may
     * be corrupt", which is a wrong and unactionable diagnosis for the two
     * ways this actually fails:
     *   1. Launched from a shell with no TMP/TEMP (an MSYS/git-bash harness,
     *      a service, a scheduler), GetTempPathA falls all the way back to
     *      C:\WINDOWS -- which is not writable, so every launch died there.
     *   2. Writing a DLL into %TEMP% and immediately injecting it is exactly
     *      the shape of a dropper, so a behavior shield can veto that one
     *      write and nothing else on the machine.
     * Try several directories in order and, if they all refuse, say which
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

        /* Ours, always writable, and a path a shield is far less likely to
         * treat as a drop site than the shared %TEMP%. */
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
        /* Nothing took it.  Put the list where the caller's message can see
         * it -- "out" is only meaningful on success. */
        if (!fp) snprintf(out, cap, "%s", why[0] ? why : "(no candidate directory)");
    }
    if (!fp) { LOG("cannot write temp hook %s\n", out); return 0; }
    size_t wrote = fwrite(ptr, 1, sz, fp);
    fclose(fp);
    if (wrote != sz) { LOG("short write to temp hook %s\n", out); DeleteFileA(out); return 0; }
    LOG("extracted embedded hook (%s, %lu bytes) -> %s\n", res_name, sz, out);
    if (selected_name) *selected_name = res_name;
    return 1;
}

/* 32-bit in-process CreateRemoteThread inject (Type X pattern) */
static int inject_dll_32(HANDLE proc, const char *dll_path)
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
    LPTHREAD_START_ROUTINE loadfn =
        (LPTHREAD_START_ROUTINE)(uintptr_t)GetProcAddress(kern, "LoadLibraryA");
    if (!loadfn) { VirtualFreeEx(proc, remote, 0, MEM_RELEASE); return 0; }
    HANDLE th = CreateRemoteThread(proc, NULL, 0, loadfn, remote, 0, NULL);
    if (!th) {
        LOG("CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    WaitForSingleObject(th, 15000);
    DWORD rc = 0; GetExitCodeThread(th, &rc);
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return rc != 0;
}

/* 64-bit inject via ultra-inject64.exe helper. Our 32-bit loader can't
 * CreateRemoteThread into a 64-bit process, so we spawn the stand-alone
 * injector which does the WOW64 inject from the correct bitness. */
static int inject_dll_64(DWORD pid, const char *dll_path)
{
    char loader_exe[MAX_PATH];
    GetModuleFileNameA(NULL, loader_exe, MAX_PATH);
    char *bs = strrchr(loader_exe, '\\'); if (bs) *bs = 0;

    char injector[MAX_PATH];
    int found = 0;
    snprintf(injector, sizeof injector, "%s\\ultra-inject64.exe", loader_exe);
    if (GetFileAttributesA(injector) != INVALID_FILE_ATTRIBUTES) found = 1;
    if (!found) {
        snprintf(injector, sizeof injector, "%s\\modtest\\ultra-inject64.exe", loader_exe);
        if (GetFileAttributesA(injector) != INVALID_FILE_ATTRIBUTES) found = 1;
    }
    if (!found) {
        LOG("ultra-inject64.exe not found (needed for 64-bit ES3 games)\n");
        return 0;
    }

    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\" %lu \"%s\"", injector, pid, dll_path);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        LOG("CreateProcess(ultra-inject64) failed: %lu\n", GetLastError());
        return 0;
    }
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return rc == 0;
}

/* EnumWindows callback to find the game's main window by PID. */
typedef struct { DWORD pid; HWND result; } es3_find_ctx;
static BOOL CALLBACK es3_enum_wnd(HWND h, LPARAM lp) {
    es3_find_ctx* ctx = (es3_find_ctx*)lp;
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
 *  GAME LOCKS -- REMOVED 2026-09-15. They live in each game's container now.
 *
 *  This board used to carry two hard-coded tables, LOCK_POKKEN and LOCK_MKDX,
 *  pinning the env each title was proven with. They did their job: they are
 *  why Pokken's analog stick survived a fix aimed at Mario Kart's menus, and
 *  the two tables held the SAME setting at OPPOSITE values on purpose.
 *
 *  What replaced them is better in one specific way. A lock was a game's NAME
 *  compiled into a board's module: to read it you had to know this file
 *  existed, every board needed its own copy of the mechanism, and a title
 *  could only be locked by someone able to rebuild and re-sign the module.
 *  The same values now travel as DATA in that game's own signed container,
 *  where the person who packaged it can read them, and where no other game can
 *  see them at all.
 *
 *  So this file no longer knows any game's name -- which was always the part
 *  that did not belong in a board module.
 *
 *  Nothing was weakened: a container's [env] block is applied after the loader
 *  publishes its settings, exactly as a lock was, so it still wins over
 *  ultra.ini. See docs/ULTRA_FIRMWARE_SPEC.md sections 8b and 9.
 * =========================================================================== */

__declspec(dllexport) int ul_namco_es3_run(int argc, char **argv)
{
    if (argc < 2 || !argv[1] || !*argv[1]) {
        LOG("ERROR: no game path (argc=%d)\n", argc);
        return 1;
    }
    const char *game = argv[1];
    LOG("launching %s\n", game);

    /* Derive workdir = dir(game). ES3 games expect to run from their own
     * folder for relative asset lookups. Handle both '\\' and '/'. */
    char workdir[MAX_PATH];
    strncpy(workdir, game, MAX_PATH - 1); workdir[MAX_PATH - 1] = 0;
    {
        char *b1 = strrchr(workdir, '\\');
        char *f1 = strrchr(workdir, '/');
        char *sep = (b1 && f1) ? (b1 > f1 ? b1 : f1) : (b1 ? b1 : f1);
        if (sep) *sep = 0;
    }

    int bits = game_pe_bits(game);
    LOG("game PE bitness = %d\n", bits);

    char hook[MAX_PATH];
    const char *hook_name = NULL;
    if (!find_hook(hook, sizeof hook, bits, &hook_name)) {
        LOG("ERROR: embedded hook for %d-bit ES3 game could not be extracted\n", bits);
        MessageBoxA(NULL,
            "Namco ES3 hook could not be extracted from this module.\n\n"
            "The installed ultra_namco_es3.dll may be corrupt -- reinstall it via\n"
            "Settings -> Install Module.",
            "Ultra Loader -- Namco ES3 Hook Missing",
            MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }
    LOG("hook = %s\n", hook);
    LOG("workdir = %s\n", workdir);

    /* Env vars the hook DLL reads at injection time. All default off when
     * unset, so they turn on per-ES3 only -- byte-safe globally. */
    SetEnvironmentVariableA("ULTRA_LAUNCH_MODULE",    "namco_es3");
    SetEnvironmentVariableA("ULTRA_BOARD_HINT",       "namco_es3");
    SetEnvironmentVariableA("ULTRA_SYSTEM_ID",        "namco_es3");
    /* controls.c's ultra_input_map_install() resolves the per-board input
     * profile from THIS var specifically (not BOARD_HINT/SYSTEM_ID) -- see
     * the Europa-R fix in X86_BOARDS.md S7 for the bug this closes. */
    SetEnvironmentVariableA("ULTRA_BOARD",            "namco_es3");
    /* Both graphics APIs armed; the hook's first-wins runtime detect picks
     * whichever CreateDevice fires first (D3D11 for T7/Pokken, D3D9 for the
     * rest). Per project memory: "both armed, first-wins runtime detect". */
    SetEnvironmentVariableA("ULTRA_D3D9",             "1");
    SetEnvironmentVariableA("ULTRA_D3D11",            "1");
    SetEnvironmentVariableA("ULTRA_HOOK_D3D9",        "1");
    SetEnvironmentVariableA("ULTRA_HOOK_D3D11",       "1");
    SetEnvironmentVariableA("ULTRA_CONTROLS",         "1");
    /* Runtime scene-state detector, which is what makes "Return to Game Title"
     * work without a hand-mapped per-game address. It is OBSERVE-ONLY while it
     * learns - it watches the game write its own screen-request field and never
     * writes anything itself - and the only write happens when the user picks
     * the menu item. Armed per-board here rather than by flipping the default
     * in the shared scene_probe.h, so enabling it for ES3 cannot change
     * behavior on the other 33 boards. An explicit env still wins. */
    /* NOT armed by default (reverted 2026-09-14).
     *
     * Arming it is the one thing that changed between the build where the pause
     * menu opened (11:57, probe off) and every build after, where it never
     * opened again. scene_probe_tick() runs from ultra_pausemenu_tick_frame(),
     * which is now reached from BOTH the input poll thread and the render
     * thread, with no locking, and it sweeps memory at frames 60/240/600.
     * The poll loop's heartbeat fired exactly ONCE in a multi-minute session,
     * so that loop is stalling - and that loop is what reads F1 and the pad.
     *
     * Return to Game Title is worth less than the menu that reaches it, so the
     * probe goes back to explicit opt-in until it is proven not to be the
     * stall: ULTRA_SCENE_PROBE=1 still turns it on for a test. */
    SetEnvironmentVariableA("ULTRA_JVS_ANALOG",       "4");
    SetEnvironmentVariableA("ULTRA_NAMCO_IO",         "1");
    /* Also set ULTRA_IO4=1 so the shared IO4-USB emulator (used as the base
     * for ESIF2 per memory) wakes up in the Nu/Type X fallback hooks. */
    SetEnvironmentVariableA("ULTRA_IO4",              "1");

    /* Reuse Sega Nu's amdaemon stub -- the BootID/OK handshake is
     * vendor-agnostic. Setting the Sega-flavored gate is what the shared
     * hook checks. */
    SetEnvironmentVariableA("ULTRA_SEGA_AMDAEMON",    "1");
    SetEnvironmentVariableA("ULTRA_AMDAEMON",         "1");
    SetEnvironmentVariableA("ULTRA_AMAUTH",           "1");
    SetEnvironmentVariableA("ULTRA_SEGA_AMAUTH",      "1");
    SetEnvironmentVariableA("ULTRA_SEGA_WINDOWED",    "1");
    SetEnvironmentVariableA("ULTRA_WINDOWED",         "1");
    SetEnvironmentVariableA("ULTRA_INPOUT32_BYPASS",  "1");
    if (!GetEnvironmentVariableA("ULTRA_DISMISS_DIALOGS", NULL, 0))
        SetEnvironmentVariableA("ULTRA_DISMISS_DIALOGS", "1");
    SetEnvironmentVariableA("ULTRA_DONGLE_BYPASS",    "1");
    SetEnvironmentVariableA("ULTRA_NAMCO_DONGLE",     "1");
    if (!GetEnvironmentVariableA("ULTRA_FPS_CAP", NULL, 0))
        SetEnvironmentVariableA("ULTRA_FPS_CAP", "60");


    /* Mario Kart GP DX uses THREE serial devices:
     *   COM3  JVS I/O board
     *   COM4  drive / steering board
     *   COM2  IC card vendor (the card dispenser)
     * This desktop really has COM1 and COM2, so the hook's default rule
     * ("claim any port the OS cannot give us") leaves COM2 alone and the
     * game's open passes through to real hardware with nothing attached.
     * Name the ports explicitly so the vendor open reaches our emulation.
     * COM1 is deliberately NOT claimed - it exists and something else opens
     * it. Operator override: set ULTRA_JVS_COM yourself (list, "any", "none"). */
    /* The per-title I/O and render quirks that used to live here - the COM
     * ports for one game's I/O boards, and the size it authors its frames at -
     * moved into that game's own container [env] on 2026-09-16. They were
     * matched by hard-coding its executable name in this file, which is the
     * same mistake the lock tables were: a board module has no business knowing
     * which games run on it. A container publishes them after the loader's
     * settings, exactly as this block did, so nothing about the behavior
     * changed. */

    /* JVS / dongle diagnostic log next to the game exe.
     *
     * A CONTAINER MAY OVERRIDE OR SILENCE THIS. It used to be set
     * unconditionally, which meant a finished game had no way to decline it -
     * and it is not free: a single session wrote 9.6 MB here, every COM TX/RX
     * and every asset open, synchronously, into what is meant to be read-only
     * source material. "off" or "0" disables it; any other value is used as the
     * path; unset keeps the old behavior. */
    {
        /* LATCHED, because deleting the variable is self-defeating.
         *
         * ul_run_with_restart() calls this function again, and the first pass
         * had already REMOVED ULTRA_JVS_LOG - so the second pass saw it unset,
         * fell through to the default below and switched the log back on. The
         * container says "off" once; it stays off for the life of the process. */
        static int s_jvs_log_off = 0;
        char logpath[MAX_PATH];
        char want[MAX_PATH];
        DWORD wn;
        if (s_jvs_log_off) goto jvs_log_done;
        wn = GetEnvironmentVariableA("ULTRA_JVS_LOG", want, sizeof want);
        if (wn > 0 && wn < sizeof want) {
            if (!_stricmp(want, "off") || !_stricmp(want, "0")) {
                s_jvs_log_off = 1;
                SetEnvironmentVariableA("ULTRA_JVS_LOG", NULL);
                LOG("JVS diagnostic log: disabled by the container\n");
                goto jvs_log_done;
            }
            LOG("JVS diagnostic log: %s (from the container)\n", want);
            goto jvs_log_done;
        }
        snprintf(logpath, sizeof logpath, "%s\\ultra_namco_jvs.log",
                 workdir[0] ? workdir : ".");
        SetEnvironmentVariableA("ULTRA_JVS_LOG", logpath);
        /* TRUNCATE per launch. The hook opens this file in append mode, so
         * without this a new run's capture lands directly behind the previous
         * one with nothing to separate them. On 2026-09-13 that cost two wrong
         * diagnoses of the STR board: the head of the file was the PREVIOUS
         * build's behavior, and the run being analyzed did not start until
         * 95% of the way in, at its JVS RESET. */
        {
            FILE* lf = fopen(logpath, "w");
            if (lf) fclose(lf);
        }
        LOG("JVS log = %s\n", logpath);
    }
jvs_log_done:

    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\"", game);
    if (!CreateProcessA(game, cmd, NULL, NULL, TRUE, CREATE_SUSPENDED,
                        NULL, workdir[0] ? workdir : NULL, &si, &pi)) {
        LOG("CreateProcess(%s) failed: %lu\n", game, GetLastError());
        DeleteFileA(hook);
        return 1;
    }
    LOG("spawned pid=%lu tid=%lu (suspended)\n",
        pi.dwProcessId, pi.dwThreadId);

    int ok = (bits == 64)
        ? inject_dll_64(pi.dwProcessId, hook)
        : inject_dll_32(pi.hProcess, hook);
    if (!ok) {
        LOG("inject failed; terminating game\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        DeleteFileA(hook);
        return 1;
    }

    ResumeThread(pi.hThread);
    LOG("hook injected (%s), game resumed\n", hook_name ? hook_name : "?");

    /* Window handling.
     *
     * WHAT THIS USED TO DO, AND WHY IT WAS WRONG:
     * unconditionally rewrite the game window's style to WS_POPUP and resize
     * it to the full desktop, from THIS process, a few seconds after the game
     * started.  Two things break because of that:
     *
     *  1. It fights our own hook.  The D3D11 hook installs with windowed=1 and
     *     calls SetFullscreenState(FALSE) on the swap chain it just caught.
     *     This block then yanks the same window to WS_POPUP + desktop size, so
     *     DXGI sees a resize the game never asked for while its device is
     *     still being set up.  Mario Kart GP DX renders at 1360x768 (see its
     *     own AMConfig.ini amucfg-h_resol/v_resol) and does not survive it --
     *     bare it runs indefinitely, under this block it exits with code 1.
     *  2. SetForegroundWindow + BringWindowToTop + SWP_SHOWWINDOW drags the
     *     window onto the real screen even when a test harness deliberately
     *     parked it off-screen.  cave_pc hit exactly this and grew
     *     ULTRA_CAVE_TEST_HEADLESS for it; this module had no such escape.
     *
     * So the DEFAULT is now "leave the game's own window alone" -- we only
     * raise it.  Set ULTRA_ES3_FULLSCREEN=1 to get the old borderless-desktop
     * behavior back, and ULTRA_ES3_TEST_HEADLESS=1 to skip the whole block. */
    {
        const char *hl_e = getenv("ULTRA_ES3_TEST_HEADLESS");
        const char *fs_e = getenv("ULTRA_ES3_FULLSCREEN");
        int headless = (hl_e && hl_e[0] == '1');
        int want_fs  = (fs_e && fs_e[0] == '1');
        if (headless) {
            LOG("headless test mode -- SKIPPING window takeover entirely\n");
        } else {
            DWORD dw = (DWORD)GetSystemMetrics(SM_CXSCREEN);
            DWORD dh = (DWORD)GetSystemMetrics(SM_CYSCREEN);
            for (int i = 0; i < 40; ++i) {
                Sleep(500);
                /* Stop hunting for a window the moment the game is gone --
                 * the old loop kept enumerating for a full 20s after exit. */
                if (WaitForSingleObject(pi.hProcess, 0) != WAIT_TIMEOUT) {
                    LOG("game exited before a window appeared\n");
                    break;
                }
                es3_find_ctx fctx = { pi.dwProcessId, NULL };
                EnumWindows(es3_enum_wnd, (LPARAM)&fctx);
                if (fctx.result) {
                    if (want_fs) {
                        SetWindowLongA(fctx.result, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                        SetWindowPos(fctx.result, HWND_TOP, 0, 0, (int)dw, (int)dh,
                                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                        LOG("game window %p -> forced fullscreen %lux%lu"
                            " (ULTRA_ES3_FULLSCREEN=1)\n",
                            (void*)fctx.result, dw, dh);
                    } else {
                        LOG("game window %p -> left at its own style/size"
                            " (set ULTRA_ES3_FULLSCREEN=1 to force %lux%lu)\n",
                            (void*)fctx.result, dw, dh);
                    }
                    SetForegroundWindow(fctx.result);
                    BringWindowToTop(fctx.result);
                    break;
                }
            }
        }
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0; GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    DeleteFileA(hook);
    LOG("game exited with code %lu\n", rc);
    /* Exit code 2 (pause-menu restart) is handled by the LOADER, once, for
     * every board - see ul_run_with_restart() in work/src/main.c. Doing it here
     * too would just be a second mechanism to keep in step. */
    return (int)rc;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)hinst; (void)reason; (void)reserved;
    return TRUE;
}
