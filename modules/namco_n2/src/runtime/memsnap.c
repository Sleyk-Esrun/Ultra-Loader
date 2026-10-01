/* ------------------------------------------------------------------ *
 * memsnap.c - periodic guest-memory snapshotter.
 *
 * When the Cruis'n Blast packer decrypts the `main` region in-memory at
 * runtime, static extraction sees only ciphertext (all-zeros in the
 * unmapped-until-touched page). To capture the unpacked payload we run
 * a watchdog thread that snapshots the ELF span every ~100ms while the
 * guest is alive. Later snapshots will contain the fully-unpacked code.
 *
 * Written pages: [0x08040000, 0x0A400000)  = 36 MiB max
 * Output: %ULTRA_MEMSNAP_DIR%\cb_snap_<ms>.bin   (defaults to %TEMP%)
 * Tunable env:
 *   ULTRA_MEMSNAP=1          - enable (also enabled auto for Cruis'n Blast)
 *   ULTRA_MEMSNAP_DIR=<path> - output dir (default: %TEMP% or C:\tmp)
 *   ULTRA_MEMSNAP_MS=100     - interval in milliseconds
 *   ULTRA_MEMSNAP_LO=0x08040000
 *   ULTRA_MEMSNAP_HI=0x0A400000
 *   ULTRA_MEMSNAP_MAX=64     - cap on number of snapshots (rotates)
 *
 * The dumper only writes pages that are MEM_COMMIT + readable; unmapped
 * pages are written as zeros (keeping file offset == guest VA - LO).
 * ------------------------------------------------------------------ */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loader.h"

static volatile LONG g_ms_stop = 0;

/* Shared state so exit() hook and the periodic thread agree on where/what. */
static uintptr_t g_ms_lo = 0, g_ms_hi = 0;
static char      g_ms_outdir[MAX_PATH] = {0};
static int       g_ms_armed = 0;

static int ms_readable_page(uintptr_t a)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)a, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return 1;
}

/* Dump one snapshot of [lo, hi) to `path`. Returns bytes written or -1. */
static long ms_dump_one(const char *path, uintptr_t lo, uintptr_t hi)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    const size_t PG = 0x1000;
    static uint8_t zero_page[0x1000] = {0};
    uintptr_t a = lo & ~(PG - 1);
    long total = 0;

    while (a < hi) {
        size_t chunk = PG;
        if (a + chunk > hi) chunk = hi - a;
        if (ms_readable_page(a)) {
            /* try to read safely; SEH-style wrap via IsBadReadPtr is
             * unreliable, but ms_readable_page already checked commit +
             * no-guard so this is safe on Windows page granularity. */
            fwrite((const void*)a, 1, chunk, f);
        } else {
            fwrite(zero_page, 1, chunk, f);
        }
        total += (long)chunk;
        a += chunk;
    }
    fclose(f);
    return total;
}

static void ms_get_outdir(char *dst, size_t dstsz)
{
    const char *e = getenv("ULTRA_MEMSNAP_DIR");
    if (e && *e) { snprintf(dst, dstsz, "%s", e); return; }
    const char *t = getenv("TEMP");
    if (!t || !*t) t = getenv("TMP");
    if (!t || !*t) t = "C:\\tmp";
    snprintf(dst, dstsz, "%s", t);
    /* best-effort mkdir */
    CreateDirectoryA(dst, NULL);
}

/* rotating ring of snapshots so the disk doesn't fill up. */
typedef struct { char path[MAX_PATH]; } SnapEnt;

static DWORD WINAPI ms_thread(LPVOID arg)
{
    (void)arg;
    uintptr_t lo = g_ms_lo, hi = g_ms_hi;
    int interval_ms = 100;
    int max_snaps   = 64;

    const char *ems = getenv("ULTRA_MEMSNAP_MS");
    const char *emx = getenv("ULTRA_MEMSNAP_MAX");
    if (ems && *ems) interval_ms = atoi(ems);
    if (emx && *emx) max_snaps    = atoi(emx);
    if (interval_ms < 10)  interval_ms = 10;
    if (max_snaps   < 2 )  max_snaps   = 2;
    if (max_snaps   > 512) max_snaps   = 512;

    const char *outdir = g_ms_outdir;

    loader_log(LOG_INFO, "[MEMSNAP] thread running: range=[0x%08x, 0x%08x) interval=%dms max=%d dir=%s",
               (unsigned)lo, (unsigned)hi, interval_ms, max_snaps, outdir);

    SnapEnt *ring = (SnapEnt*)calloc((size_t)max_snaps, sizeof(SnapEnt));
    if (!ring) return 0;
    int head = 0, count = 0;

    DWORD t0 = GetTickCount();
    int  fires = 0;
    /* Take an IMMEDIATE first snapshot before Sleep - the guest can call
     * exit(1) within milliseconds and our first Sleep would race it. */
    int first = 1;
    while (!InterlockedCompareExchange(&g_ms_stop, 0, 0)) {
        if (!first) Sleep(interval_ms);
        first = 0;
        DWORD now = GetTickCount() - t0;

        /* If the current head slot has a file, delete it first (rotate). */
        if (count == max_snaps && ring[head].path[0]) {
            DeleteFileA(ring[head].path);
            ring[head].path[0] = 0;
        }

        snprintf(ring[head].path, sizeof ring[head].path,
                 "%s\\cb_snap_%08u.bin", outdir, (unsigned)now);
        long n = ms_dump_one(ring[head].path, lo, hi);
        if (n < 0) {
            /* couldn't open - silence after first complaint */
            if (fires == 0) {
                loader_log(LOG_ERROR, "[MEMSNAP] fopen failed for %s (errno=%d)",
                           ring[head].path, errno);
            }
        } else if ((fires % 20) == 0) {
            loader_log(LOG_INFO, "[MEMSNAP] wrote %s (%ld bytes)", ring[head].path, n);
        }

        head = (head + 1) % max_snaps;
        if (count < max_snaps) count++;
        fires++;
    }

    loader_log(LOG_INFO, "[MEMSNAP] stopped after %d snapshots (%d retained in ring)",
               fires, count);
    free(ring);
    return 0;
}

/* Called by stub_exit() right before ExitProcess - grabs the last, fullest snap. */
void memsnap_final(const char *reason)
{
    if (!g_ms_armed) return;
    DWORD now = GetTickCount();
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\cb_snap_FINAL_%s_%08u.bin",
             g_ms_outdir, (reason ? reason : "exit"), (unsigned)now);
    long n = ms_dump_one(path, g_ms_lo, g_ms_hi);
    loader_log(LOG_ERROR, "[MEMSNAP] FINAL snap on %s -> %s (%ld bytes)",
               reason ? reason : "exit", path, n);
}

/* Public entrypoint - called from lindbergh_main.c right before elf32_launch. */
void memsnap_start(void)
{
    /* opt-in: env var, or auto-arm for Cruis'n Blast (env ULTRA_GAME=cruisnblast) */
    const char *e = getenv("ULTRA_MEMSNAP");
    const char *g = getenv("ULTRA_GAME");
    int enabled = 0;
    if (e && *e && *e != '0') enabled = 1;
    if (g && (strstr(g, "cruis") || strstr(g, "blast"))) enabled = 1;
    if (!enabled) return;

    /* Populate globals so memsnap_final() and ms_thread() share them. */
    g_ms_lo = 0x08040000;
    g_ms_hi = 0x0A400000;
    const char *elo = getenv("ULTRA_MEMSNAP_LO");
    const char *ehi = getenv("ULTRA_MEMSNAP_HI");
    if (elo && *elo) g_ms_lo = (uintptr_t)strtoul(elo, NULL, 0);
    if (ehi && *ehi) g_ms_hi = (uintptr_t)strtoul(ehi, NULL, 0);
    ms_get_outdir(g_ms_outdir, sizeof g_ms_outdir);
    g_ms_armed = 1;

    loader_log(LOG_INFO, "[MEMSNAP] armed: range=[0x%08x, 0x%08x) dir=%s",
               (unsigned)g_ms_lo, (unsigned)g_ms_hi, g_ms_outdir);

    HANDLE h = CreateThread(NULL, 0, ms_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);

    /* Auto-arm VEH first-touch trap on the CB main page (single page
     * containing main at 0x08061b58). The substrate handler will call
     * memsnap_veh_check() first - on the initial packer/main touch we
     * snapshot the entire memsnap window BEFORE it can be clobbered. */
    if (g && (strstr(g, "cruis") || strstr(g, "blast"))) {
        memsnap_veh_capture_arm((uintptr_t)0x08061000, (size_t)0x1000);
    } else if (getenv("ULTRA_MEMSNAP_VEH")) {
        /* env-driven manual arm - ULTRA_MEMSNAP_VEH_BASE / _SIZE */
        const char *eb = getenv("ULTRA_MEMSNAP_VEH_BASE");
        const char *es = getenv("ULTRA_MEMSNAP_VEH_SIZE");
        uintptr_t vb = (uintptr_t)0x08061000;
        size_t    vs = (size_t)0x1000;
        if (eb && *eb) vb = (uintptr_t)strtoul(eb, NULL, 0);
        if (es && *es) vs = (size_t)strtoul(es, NULL, 0);
        memsnap_veh_capture_arm(vb, vs);
    }
}

void memsnap_stop(void)
{
    InterlockedExchange(&g_ms_stop, 1);
}

/* ================================================================== *
 * VEH-based FIRST-WRITE / FIRST-TOUCH capture on the "main matures" page.
 *
 * The Cruis'n Blast packer decrypts its `main` function block in-place
 * shortly before jumping to it. We want to catch the instant the packer
 * finishes materializing that page (rather than sampling on a 100ms timer
 * and possibly missing the window).
 *
 * Strategy: arm a page range by flipping it to PAGE_NOACCESS. When the
 * packer (or main-thunk) touches that page, the VEH substrate handler in
 * lindbergh_main.c catches the AV. We hook in FIRST (memsnap_veh_check())
 * so that when the fault address lands inside the armed range we:
 *   (1) restore the page's protection to PAGE_EXECUTE_READWRITE,
 *   (2) take a full-range snapshot of the memsnap window to disk,
 *   (3) DEARM (one-shot), then resume execution.
 *
 * NOTE: the substrate handler would otherwise VirtualAlloc-COMMIT the
 * page (overwriting the packer's plaintext with zeros!). By intercepting
 * before the substrate path we prevent that clobber.
 * ================================================================== */

static volatile LONG      g_ms_armed_veh   = 0;
static volatile uintptr_t g_ms_armed_base  = 0;
static volatile uintptr_t g_ms_armed_end   = 0;
static volatile LONG      g_ms_veh_fires   = 0;

/* Arm a range for first-touch snapshot. base+size get flipped to
 * PAGE_NOACCESS. On the FIRST fault in that range we snapshot + dearm. */
void memsnap_veh_capture_arm(uintptr_t base, size_t size)
{
    if (!g_ms_armed) {
        /* memsnap not running - arm anyway if the range is plausible,
         * but we won't have g_ms_outdir set. Populate a fallback dir. */
        if (!g_ms_outdir[0]) ms_get_outdir(g_ms_outdir, sizeof g_ms_outdir);
        if (!g_ms_lo) g_ms_lo = 0x08040000;
        if (!g_ms_hi) g_ms_hi = 0x0A400000;
    }
    const uintptr_t PG = 0x1000;
    uintptr_t lo = base & ~(PG - 1);
    uintptr_t hi = (base + size + PG - 1) & ~(PG - 1);

    DWORD old = 0;
    if (!VirtualProtect((LPVOID)lo, (SIZE_T)(hi - lo), PAGE_NOACCESS, &old)) {
        loader_log(LOG_WARN, "[MEMSNAP/VEH] arm FAILED for [0x%08x, 0x%08x) err=%lu",
                   (unsigned)lo, (unsigned)hi, GetLastError());
        return;
    }
    g_ms_armed_base = lo;
    g_ms_armed_end  = hi;
    InterlockedExchange(&g_ms_armed_veh, 1);
    loader_log(LOG_INFO,
        "[MEMSNAP/VEH] armed FIRST-TOUCH trap on [0x%08x, 0x%08x) (was prot=0x%lx)",
        (unsigned)lo, (unsigned)hi, (unsigned long)old);
}

/* Called from the VEH substrate handler in lindbergh_main.c BEFORE it
 * demand-pages. Returns 1 if we handled the fault (caller should
 * EXCEPTION_CONTINUE_EXECUTION), 0 to pass through. */
int memsnap_veh_check(uintptr_t fault_addr, uintptr_t eip)
{
    if (!InterlockedCompareExchange(&g_ms_armed_veh, 0, 0)) return 0;
    uintptr_t base = g_ms_armed_base;
    uintptr_t end  = g_ms_armed_end;
    if (fault_addr < base || fault_addr >= end) return 0;

    /* One-shot: disarm before doing work, so a nested fault (e.g. from
     * fwrite) can't re-enter us. */
    InterlockedExchange(&g_ms_armed_veh, 0);

    /* Restore the page(s) to executable+writable so the guest can proceed
     * once we resume. We do this BEFORE the snapshot: the snapshot reads
     * the pages we just made readable. */
    DWORD old = 0;
    if (!VirtualProtect((LPVOID)base, (SIZE_T)(end - base),
                        PAGE_EXECUTE_READWRITE, &old)) {
        loader_log(LOG_WARN,
            "[MEMSNAP/VEH] restore protect FAILED for [0x%08x, 0x%08x) err=%lu",
            (unsigned)base, (unsigned)end, GetLastError());
        return 0;
    }

    LONG n = InterlockedIncrement(&g_ms_veh_fires);
    DWORD now = GetTickCount();
    char path[MAX_PATH];
    /* Ensure we have an outdir; some paths may arm before memsnap_start(). */
    if (!g_ms_outdir[0]) ms_get_outdir(g_ms_outdir, sizeof g_ms_outdir);
    /* Prefer the requested C:/tmp/cb_snaps convention when it exists or can be made. */
    const char *pref = "C:\\tmp\\cb_snaps";
    CreateDirectoryA("C:\\tmp", NULL);
    if (CreateDirectoryA(pref, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
        snprintf(path, sizeof path, "%s\\cb_snap_MAIN_MAT_%08u.bin",
                 pref, (unsigned)now);
    } else {
        snprintf(path, sizeof path, "%s\\cb_snap_MAIN_MAT_%08u.bin",
                 g_ms_outdir, (unsigned)now);
    }

    uintptr_t dump_lo = g_ms_lo ? g_ms_lo : 0x08040000;
    uintptr_t dump_hi = g_ms_hi ? g_ms_hi : 0x0A400000;
    long wrote = ms_dump_one(path, dump_lo, dump_hi);
    loader_log(LOG_ERROR,
        "[MEMSNAP/VEH] FIRST-TOUCH #%ld fault=0x%08x eip=0x%08x -> %s (%ld bytes)",
        (long)n, (unsigned)fault_addr, (unsigned)eip, path, wrote);
    return 1;
}
