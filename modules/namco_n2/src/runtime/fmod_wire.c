/*
 * fmod_wire.c
 * FMOD (libfmod.so.10) extractor + no-op audio surface for Cruis'n Blast.
 *
 * WHY EXTRACT.
 *
 * The game dump ships its own custom FMOD build as `libfmod.so.10`. That .so
 * is a Linux ELF and can't be loaded by our Windows PE runtime, so we can't
 * actually *link* to it. But we still want it on disk next to the game (in a
 * runtime scratch dir) so an offline analysis pass - or a future
 * arm64/wine-lite lane - can pick it up without re-extracting from the ISO.
 * On Linux native builds this same file goes on LD_LIBRARY_PATH so the ELF's
 * DT_NEEDED("libfmod.so.10") resolves.
 *
 * WHY NO-OP FOR NOW.
 *
 * FMOD Ex / FMOD Studio speak a big surface (FMOD_System_*, FMOD::Studio,
 * bank loading, streaming). Wiring a real audio backend behind that surface
 * is a substantial project on the order of segaapi_stub.c (see
 * `ultra-loader-segaapi-audio.md`). Per the Cruis'n Blast plan this is a
 * KNOWN GAP marked SOUND_STATUS=silent - the goal here is boot-to-attract
 * with wheel input, not full audio parity.
 *
 * So this file does two things:
 *
 *   1. On core_init, walk the mounted ISO / game dir looking for
 *      libfmod.so.10 (or libfmodex.so.4 for older Raw Thrills builds), copy
 *      it out to `%TEMP%\UltraLoader\runtime\fmod\`, and record that dir.
 *      This is the "extraction helper" the plan asked for.
 *
 *   2. Register `fmod_wire_dlopen` / `fmod_wire_dlsym` so that when the ELF
 *      dlopens libfmod.so.10 we hand back a marker handle and every symbol
 *      lookup routes to a shared "return 0 (FMOD_OK)" thunk. The game's
 *      audio path returns without error; it just plays nothing.
 *
 * When the real audio bring-up happens, this file is the drop-in seam:
 * replace the no-op thunk with a per-symbol dispatch table, similar to how
 * `cri_shim.c` handles Sega CRI middleware for GSEVO.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "loader.h"

/* Marker handle handed back for libfmod.so.10 / libfmodex.so.4 dlopens. */
#define FMOD_WIRE_HANDLE ((void*)0xDEADF00D)

/* Where we drop the extracted .so. Populated on first extraction; queried by
 * the boot console. */
static char g_fmod_dir [MAX_PATH]  = "";
static char g_fmod_path[MAX_PATH]  = "";  /* full path to the extracted .so */
static int  g_fmod_extracted       = 0;

/* Optional VFS reader - the Lindbergh runtime exposes vfs_open/read/close
 * over the mounted ISO. Declared extern so we don't grow header deps. */
extern int      vfs_open (const char *path, int flags);
extern int64_t  vfs_read (int fd, void *buf, size_t count);
extern int      vfs_close(int fd);

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int fmod_mkdir_p(const char *path)
{
    /* Recursive mkdir - the shortest working `mkdir -p` for Win32. */
    char tmp[MAX_PATH];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    for (char *p = tmp + 3; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            CreateDirectoryA(tmp, NULL);
            *p = c;
        }
    }
    return CreateDirectoryA(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
}

static int fmod_copy_from_vfs(const char *src_path, const char *dst_path)
{
    int fd = vfs_open(src_path, 0);
    if (fd < 0) return -1;

    FILE *out = fopen(dst_path, "wb");
    if (!out) { vfs_close(fd); return -1; }

    uint8_t buf[64 * 1024];
    int64_t n;
    size_t  total = 0;
    while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t)n, out);
        total += (size_t)n;
    }
    fclose(out);
    vfs_close(fd);
    loader_log(LOG_INFO, "[FMOD] extracted %s -> %s (%zu bytes)",
               src_path, dst_path, total);
    return total > 0 ? 0 : -1;
}

static int fmod_copy_from_disk(const char *src_path, const char *dst_path)
{
    /* Fallback if the game files live on the host FS (dev builds) rather
     * than inside a mounted ISO. */
    if (!CopyFileA(src_path, dst_path, FALSE)) return -1;
    loader_log(LOG_INFO, "[FMOD] copied %s -> %s", src_path, dst_path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: extraction (called from rt_g5_init / autoprofile)           */
/* ------------------------------------------------------------------ */

int fmod_wire_extract(void)
{
    if (g_fmod_extracted) return 0;

    /* Destination: %TEMP%\UltraLoader\runtime\fmod\ */
    char tmp_root[MAX_PATH];
    if (!GetTempPathA(sizeof(tmp_root), tmp_root))
        strncpy(tmp_root, "C:\\Temp\\", sizeof(tmp_root));

    _snprintf(g_fmod_dir, sizeof(g_fmod_dir) - 1,
              "%sUltraLoader\\runtime\\fmod", tmp_root);
    fmod_mkdir_p(g_fmod_dir);

    /* Candidate .so names - .10 is Cruis'n Blast; .4 is older Raw Thrills. */
    static const char *candidates[] = {
        "libfmod.so.10", "libfmod.so", "libfmodex.so.4", "libfmodex.so",
        NULL
    };
    /* Search paths inside the game tree. The ISO mount root is "/" for us. */
    static const char *search_dirs[] = {
        "/", "/lib/", "/lib32/", "/usr/lib/", "/usr/lib32/",
        "/opt/game/", "/opt/game/lib/",
        NULL
    };

    for (int si = 0; search_dirs[si]; si++) {
        for (int ci = 0; candidates[ci]; ci++) {
            char src[MAX_PATH];
            _snprintf(src, sizeof(src) - 1, "%s%s",
                      search_dirs[si], candidates[ci]);
            _snprintf(g_fmod_path, sizeof(g_fmod_path) - 1,
                      "%s\\%s", g_fmod_dir, candidates[ci]);

            /* Try VFS first (ISO-backed), then host disk. */
            if (fmod_copy_from_vfs(src, g_fmod_path) == 0 ||
                fmod_copy_from_disk(src, g_fmod_path) == 0) {
                g_fmod_extracted = 1;
                /* Preserve the extraction dir on PATH so any tool that later
                 * comes along (e.g. objdump for symbols) can find it. */
                char env_buf[2 * MAX_PATH];
                DWORD have = GetEnvironmentVariableA("ULTRA_LIBFMOD_DIR",
                                                     env_buf, sizeof(env_buf));
                if (have == 0)
                    SetEnvironmentVariableA("ULTRA_LIBFMOD_DIR", g_fmod_dir);
                return 0;
            }
        }
    }

    loader_log(LOG_WARN,
        "[FMOD] libfmod.so.10 not found in game dump; audio stays silent");
    g_fmod_extracted = 1;  /* don't retry every frame */
    return -1;
}

const char *fmod_wire_extracted_path(void) { return g_fmod_path; }
const char *fmod_wire_extracted_dir (void) { return g_fmod_dir;  }

/* ------------------------------------------------------------------ */
/* dlopen / dlsym surface                                              */
/* ------------------------------------------------------------------ */

bool fmod_wire_is_lib(const char *filename)
{
    if (!filename) return false;
    return strstr(filename, "libfmod")   != NULL
        || strstr(filename, "libfmodex") != NULL
        || strstr(filename, "libfmodstudio") != NULL;
}

void *fmod_wire_dlopen(const char *filename)
{
    /* Trigger extraction lazily - safe to call before core_init if needed. */
    if (!g_fmod_extracted) fmod_wire_extract();
    loader_log(LOG_INFO, "[FMOD] dlopen(%s) -> silent stub handle (SOUND_STATUS=silent)",
               filename ? filename : "(null)");
    return FMOD_WIRE_HANDLE;
}

/* Executable "return 0" thunk - FMOD_OK == 0, so every FMOD_System_* call
 * we don't yet implement reports success. */
static uintptr_t fmod_ok_thunk(void)
{
    static uint8_t *arena = NULL;
    if (arena) return (uintptr_t)arena;
    arena = (uint8_t*)VirtualAlloc(NULL, 16,
                                   MEM_COMMIT|MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
    if (!arena) return 0;
    /* xor eax,eax ; ret */
    arena[0] = 0x31;
    arena[1] = 0xC0;
    arena[2] = 0xC3;
    return (uintptr_t)arena;
}

void *fmod_wire_dlsym(void *handle, const char *symbol)
{
    (void)handle;
    if (!symbol) return NULL;
    /* One shared shim for every FMOD entry point. Enough to reach attract. */
    uintptr_t p = fmod_ok_thunk();
    loader_log(LOG_DEBUG, "[FMOD] dlsym(%s) -> silent stub", symbol);
    return (void*)p;
}
