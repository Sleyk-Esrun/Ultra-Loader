/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  crk_swap.cpp -- put the GHL replacement DLLs in place before the game starts
 * =============================================================================
 *
 *  WHY THIS IS NOT OPTIONAL.
 *
 *  Konami's real shaiolib.dll will not initialize without its GHL board. A bare
 *  LoadLibrary of the stock file returns ERROR_DLL_INIT_FAILED (1114), proven
 *  directly rather than inferred, and because KSHG.exe imports it statically the
 *  Windows loader destroys the process with 0xC0000142 before the game's first
 *  instruction - and before any hook we inject can run or log. That is a
 *  hardware gate, not a bug, and swapping the library is the only way past it.
 *
 *  WHAT IT DOES, AND WHAT IT REFUSES TO DO.
 *
 *  For each pair it copies <name>.CRK.dll from beside this module into the game
 *  folder as <name>.dll. Before the FIRST overwrite it preserves the game's own
 *  file as <name>.dll.orig, once, and never again - so the backup can only ever
 *  hold the original, never one of ours.
 *
 *  It will NOT copy a replacement it cannot verify:
 *    - the source must exist beside the module
 *    - the destination must not already BE that file (byte-compare, so a rerun
 *      is a no-op and cannot churn the game folder)
 *  and it logs every decision, because the previous version of this logic
 *  wrote a diagnostic file into the game directory and nobody noticed for weeks.
 *
 *  NOTE ON THE GAME FOLDER. I:\Modern Arcade Games is read-only source material
 *  by project rule, and these five files are the one deliberate exception -
 *  without them the title cannot run at all. Every file written here is
 *  recorded in the log so the folder can always be returned to stock.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

namespace loader_glue {
    void hglog_public(int lvl, const char* msg);
}

/* THIS LAYER MUST NOT BE ABLE TO FAIL QUIETLY.
 *
 * 2026-09-17: a stale KSHG.exe held shaiolib.dll loaded, so CopyFileA failed
 * with a sharing violation and this file dutifully logged
 * "crk: FAILED to install shaiolib.dll (err=32)" -- to hglog_public, which
 * writes to STDERR. Nothing captures stderr during a playtest. The message went
 * nowhere, the game kept running a six-hour-old module, and every fix built on
 * top of that was invisible. Hours were spent debugging code that was never
 * loaded.
 *
 * That is the second time in one session a silent diagnostic has cost this
 * project a night; the first was ULTRA_SHAIOLIB_LOG being unset. So every line
 * this file emits now also goes to the board log, where the rest of the
 * evidence already lives. Same pattern as audio_hook.c's alog(). */
static void crk_log(int lvl, const char* msg)
{
    loader_glue::hglog_public(lvl, msg);   /* the original sink: stderr */

    /* ULTRA_JVS_LOG FIRST, BUT IT IS USUALLY NOT SET YET.
     *
     * The swap runs BEFORE the container's [env] block is published (see
     * ufw_launch.c: keys are applied early, [env] deliberately later so it can
     * beat the loader's own publish). So at this moment ULTRA_JVS_LOG is
     * normally empty, and a fix that only wrote there would look correct and
     * still record nothing -- which is exactly the failure this whole helper
     * exists to prevent. Hence the unconditional fallback: a fixed path under
     * LOCALAPPDATA that needs no configuration at all and is always writable. */
    char path[MAX_PATH] = {0};
    const char* p = getenv("ULTRA_JVS_LOG");
    if (p && *p) {
        std::snprintf(path, sizeof path, "%s", p);
    } else {
        char local[MAX_PATH] = {0};
        DWORD got = GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof local);
        if (got && got < sizeof local)
            std::snprintf(path, sizeof path, "%s\\Ultra Loader\\crk_swap.log", local);
    }
    if (path[0]) {
        FILE* f = fopen(path, "a");
        if (f) {
            SYSTEMTIME st; GetLocalTime(&st);
            std::fprintf(f, "[%02u:%02u:%02u] %s\n",
                         st.wHour, st.wMinute, st.wSecond, msg);
            fclose(f);
        }
    }
}

/* The five, in load order. shaiolib is the one that matters; the rest remove
 * secondary blockers (an offline BMS stub, a neutral DI8 proxy, a board-detect
 * stub, and the game's own XInput path). */
static const char* const CRK_NAMES[] = {
    "shaiolib", "libbms", "libutil", "shaerror", "dinput8"
};

static bool files_identical(const char* a, const char* b)
{
    HANDLE ha = CreateFileA(a, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (ha == INVALID_HANDLE_VALUE) return false;
    HANDLE hb = CreateFileA(b, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hb == INVALID_HANDLE_VALUE) { CloseHandle(ha); return false; }
    LARGE_INTEGER sa, sb;
    bool same = GetFileSizeEx(ha, &sa) && GetFileSizeEx(hb, &sb) && sa.QuadPart == sb.QuadPart;
    if (same) {
        char ba[8192], bb[8192];
        DWORD ra = 0, rb = 0;
        while (same && ReadFile(ha, ba, sizeof ba, &ra, NULL) && ra) {
            if (!ReadFile(hb, bb, sizeof bb, &rb, NULL) || rb != ra ||
                memcmp(ba, bb, ra) != 0) same = false;
        }
    }
    CloseHandle(ha); CloseHandle(hb);
    return same;
}

/* Find <name>.CRK.dll: beside the running module first, then the board's
 * install directory. Returns an empty string if neither has it.
 *
 * Both are legitimate. A board launched the classic way runs from its
 * install dir; a board launched from an Ultra Firmware container is staged
 * into a cache folder that holds only the module itself. */
static std::string crk_find(const std::string& module_dir, const char* name)
{
    char line[1024];
    std::string cand = module_dir + "\\" + name + ".CRK.dll";
    if (GetFileAttributesA(cand.c_str()) != INVALID_FILE_ATTRIBUTES) return cand;

    char local[MAX_PATH] = {0};
    DWORD got = GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof local);
    if (got && got < sizeof local) {
        std::string inst = std::string(local) + "\\Ultra Loader\\modules\\konami_pc\\"
                         + name + ".CRK.dll";
        if (GetFileAttributesA(inst.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::snprintf(line, sizeof line,
                "crk: %s.CRK.dll not beside the staged module - using the installed copy",
                name);
            crk_log(0, line);
            return inst;
        }
    }
    return std::string();
}

/* extern "C": the caller declares this from inside an extern "C" export, so
 * the definition must use C linkage too or the names will not meet. */
extern "C" void ultra_konami_install_crk(const std::string& module_dir,
                                         const std::string& game_dir)
{
    char line[1024];
    if (module_dir.empty() || game_dir.empty()) {
        crk_log(2, "crk: no module or game directory - not swapping");
        return;
    }

    /* ONLY SILENT HILL. These five replacements are written for KSHG.exe's GHL
     * stack and nothing else.
     *
     * konami_pc is a BOARD, not a game, and Contra Evolution HD runs on it too.
     * Contra imports a completely different Konami stack -- libavs-win32.dll and
     * libacio.dll -- and does not import shaiolib or libbms at all. Dropping our
     * files into its folder is at best litter; dinput8.dll is the dangerous one,
     * because Contra DOES import DirectInput8 and would get a proxy built around
     * Silent Hill's needs. (Contra's folder already carries our dinput8.dll,
     * libbms.dll and their .orig backups from a September pass -- evidence that
     * this ran where it should not have.)
     *
     * Keyed on KSHG.exe in the target folder rather than on an env var: the swap
     * happens before the container publishes its [env] block, so an env gate
     * would read empty and silently do the wrong thing -- which is exactly how
     * crk_swap's own logging failed earlier today. */
    {
        std::string marker = game_dir + "\\KSHG.exe";
        if (GetFileAttributesA(marker.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::snprintf(line, sizeof line,
                "crk: no KSHG.exe in the game folder - this is not Silent Hill, "
                "leaving its DLLs alone (the GHL replacements are that title's only)");
            crk_log(0, line);
            return;
        }
    }
    for (size_t i = 0; i < sizeof CRK_NAMES / sizeof CRK_NAMES[0]; ++i) {
        const char* n = CRK_NAMES[i];
        std::string src = crk_find(module_dir, n);
        std::string dst = game_dir   + "\\" + n + ".dll";
        std::string bak = game_dir   + "\\" + n + ".dll.orig";

        if (src.empty() || GetFileAttributesA(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::snprintf(line, sizeof line,
                "crk: %s.CRK.dll found in NEITHER the staged module dir NOR the "
                "installed board dir - SKIPPED", n);
            crk_log(1, line);
            continue;
        }
        if (files_identical(src.c_str(), dst.c_str())) {
            std::snprintf(line, sizeof line, "crk: %s.dll already current", n);
            crk_log(0, line);
            continue;
        }
        /* Preserve the game's own file ONCE. If a backup already exists it is
         * the original and must never be overwritten by one of ours. */
        if (GetFileAttributesA(dst.c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesA(bak.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (CopyFileA(dst.c_str(), bak.c_str(), TRUE)) {
                std::snprintf(line, sizeof line, "crk: preserved the game's %s.dll -> %s.dll.orig", n, n);
                crk_log(0, line);
            } else {
                std::snprintf(line, sizeof line,
                    "crk: could NOT preserve %s.dll (err=%lu) - NOT overwriting it",
                    n, (unsigned long)GetLastError());
                crk_log(2, line);
                continue;                      /* never overwrite what we cannot back up */
            }
        }
        if (CopyFileA(src.c_str(), dst.c_str(), FALSE)) {
            std::snprintf(line, sizeof line, "crk: installed %s.CRK.dll -> %s.dll", n, n);
            crk_log(0, line);
        } else {
            unsigned long e = (unsigned long)GetLastError();
            std::snprintf(line, sizeof line,
                "crk: *** FAILED to install %s.dll (err=%lu)%s *** THE GAME WILL RUN A "
                "STALE %s.dll AND EVERY MODULE CHANGE WILL BE INVISIBLE",
                n, e,
                (e == 32ul || e == 5ul)
                    ? " - the file is LOCKED; a previous KSHG.exe is still running"
                    : "",
                n);
            crk_log(2, line);
        }
    }
}

#endif /* _WIN32 */
