/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  jvs_serial_hook_examu.c  --  Examu eX-BOARD board's OWN Win32 COM-port
 *                                interception + IAT-patch machinery.
 * =============================================================================
 *
 *  Independent copy for modules/examu_exboard/src/hook/ only. Adapted from the
 *  PATTERN in modules/native_x86/src/hook/jvs_serial_hook.c (read as reference
 *  only -- no #include, no link, no runtime dependency on that file or on
 *  modules/native_x86/src/hook/'s compiled output). Own file, own symbols,
 *  own COM-slot table, own IAT patcher (`ultra_iat_patch_all_examu`).
 *
 *  eX-BOARD (Arcana Heart family) is a generic JVS-over-serial (RS-485 on
 *  COM2/COM3) PC arcade board, so unlike Sega Europa-R's board, this transport
 *  IS this board's real coin/service/TEST/player-switch path (not an inert
 *  scaffold) -- see hook_dllmain_examu.c for the keyboard/XInput source that
 *  feeds this engine.
 *
 *  This file is also the ONE place that owns kernel32 IAT patching for this
 *  board's hook DLL: every other file in this board's hook
 *  (audio_hook_examu.c, dongle_bypass_examu.c, dinput8_emu_examu.c) calls
 *  ultra_iat_patch_all_examu() to install its own shims.
 *
 *  A generic USB/parallel security-dongle CreateFile bypass is folded in here
 *  (gated by ULTRA_DONGLE_BYPASS=1, set by examu_exboard_main.c), matching the
 *  reference's is_dongle_device() behavior in Hook_CreateFileA: common dongle
 *  device names (Hardlock / HASP / SentinelSuperPro / SafeNet / Aladdin) get a
 *  synthetic handle instead of a real open, so a title whose only obstacle is
 *  "dongle driver not installed" can get past that presence check. This does
 *  NOT and cannot derive any vendor's actual protected secret.
 *
 *  Mechanism: IAT patching (no inline trampolines, no third-party hooking
 *  lib). Point every loaded module's import-address-table slot for the
 *  relevant kernel32 functions at our shims, and intercept GetProcAddress so
 *  late/dynamic resolution is caught too.
 *
 *  Clean-room: Ultra Loader's own code, built on the public Win32 API and the
 *  open JVS standard.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#include "jvs_core_examu.h"

/* ---- IS THIS MODULE STILL MAPPED? -----------------------------------------
 * EnumProcessModules hands back a SNAPSHOT. A DLL freed between that call and
 * our read of its PE headers leaves an unmapped base, and reading it is an
 * unhandled access violation that kills the game -- measured on Caladrius AC,
 * where the faulting address was exactly the base of a d3dcompiler_42.dll that
 * D3DX9 had just unloaded. mingw32 has no __try, so ask the memory manager.
 * This narrows the race rather than closing it, and that is acceptable because
 * it can only SUBTRACT: it refuses memory we could not have read anyway. */
static const unsigned char* g_ulpe_lo;
static const unsigned char* g_ulpe_hi;

static int ul_pe_readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    const unsigned char* a = (const unsigned char*)p;
    const unsigned char* end;
    if (!a) return 0;
    end = a + n;
    if (a >= g_ulpe_lo && end <= g_ulpe_hi) return 1;      /* cached fast path */
    while (a < end) {
        if (!VirtualQuery(a, &mbi, sizeof mbi))   return 0;
        if (mbi.State != MEM_COMMIT)              return 0;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        if (mbi.Protect & PAGE_EXECUTE)           return 0;  /* execute-only */
        g_ulpe_lo = (const unsigned char*)mbi.BaseAddress;
        g_ulpe_hi = g_ulpe_lo + mbi.RegionSize;
        a = g_ulpe_hi;
    }
    return 1;
}

/* -------------------------------------------------------------------------- */
/*  Diagnostics (ULTRA_JVS_LOG=<file>; same env-var convention every Ultra     */
/*  Loader hook uses so an existing diagnostic workflow keeps working)         */
/* -------------------------------------------------------------------------- */
static void jvs_diag(const char* fmt, ...) {
    static const char* path = NULL; static int checked = 0;
    if (!checked) { path = getenv("ULTRA_JVS_LOG"); checked = 1; }
    if (!path || !*path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fflush(f); fclose(f);   /* durable per line: never lose the last line to an abrupt exit */
}
static void jvs_diag_hex(const char* tag, const unsigned char* b, size_t n) {
    if (!getenv("ULTRA_JVS_LOG")) return;
    char line[512]; size_t o = 0;
    o += (size_t)snprintf(line, sizeof(line), "%s [%zu]:", tag, n);
    for (size_t i = 0; i < n && i < 96 && o + 4 < sizeof(line); ++i)
        o += (size_t)snprintf(line + o, sizeof(line) - o, " %02X", b[i]);
    jvs_diag("%s", line);
}

/* shared, generic diagnostic sink other files in this board's hook also use */
void ultra_examu_log(const char* fmt, ...) {
    static const char* path = NULL; static int checked = 0;
    if (!checked) { path = getenv("ULTRA_JVS_LOG"); checked = 1; }
    if (!path || !*path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

/* Provided by hook_dllmain_examu.c */
ultra_jvs_node_examu* ultra_hook_node_examu(void);
void                  ultra_hook_pull_input_examu(ultra_jvs_node_examu* n);

/* -------------------------------------------------------------------------- */
/*  Fake COM handle table (one JVS bus; small table covers re-opens)           */
/* -------------------------------------------------------------------------- */
typedef struct {
    HANDLE   h;
    int      port;
    DWORD    evtmask;
    uint8_t  resp[4096];
    size_t   resp_pos, resp_len;
} examu_com_slot;

#define EXAMU_COM_SLOTS 8
static examu_com_slot   g_slots[EXAMU_COM_SLOTS];
static int              g_slot_used[EXAMU_COM_SLOTS];
static CRITICAL_SECTION g_lock;
static int              g_lock_init = 0;

static examu_com_slot* slot_for(HANDLE h) {
    for (int i = 0; i < EXAMU_COM_SLOTS; ++i)
        if (g_slot_used[i] && g_slots[i].h == h) return &g_slots[i];
    return NULL;
}
static examu_com_slot* slot_alloc(HANDLE h, int port) {
    for (int i = 0; i < EXAMU_COM_SLOTS; ++i)
        if (!g_slot_used[i]) {
            g_slot_used[i] = 1;
            memset(&g_slots[i], 0, sizeof(g_slots[i]));
            g_slots[i].h = h;
            g_slots[i].port = port;
            return &g_slots[i];
        }
    return NULL;
}
static void slot_free(HANDLE h) {
    for (int i = 0; i < EXAMU_COM_SLOTS; ++i)
        if (g_slot_used[i] && g_slots[i].h == h) { g_slot_used[i] = 0; g_slots[i].h = NULL; }
}

/* -------------------------------------------------------------------------- */
/*  Which COM ports we claim (default COM2+COM3; ULTRA_EXAMU_JVS_COM overrides) */
/* -------------------------------------------------------------------------- */
static int port_number_from_path(const char* p) {
    for (const char* s = p; *s; ++s) {
        if ((s[0]=='C'||s[0]=='c') && (s[1]=='O'||s[1]=='o') && (s[2]=='M'||s[2]=='m')
            && isdigit((unsigned char)s[3])) {
            return atoi(s + 3);
        }
    }
    return -1;
}
/* QueryDosDeviceA resolves a DOS device name only for a device the OS really
 * has, so this distinguishes "the cabinet's JVS port, which does not exist on
 * a desktop PC" from "a COM port the user genuinely owns". Cached: this runs
 * inside CreateFile, which a game calls thousands of times. */
static int com_port_physically_exists(int num) {
    static signed char cache[64];      /* 0 = unknown, 1 = exists, -1 = absent */
    char name[16], target[512];
    int r;
    if (num < 0) return 0;
    if (num < 64 && cache[num]) return cache[num] > 0;
    snprintf(name, sizeof name, "COM%d", num);
    r = QueryDosDeviceA(name, target, (DWORD)sizeof target) ? 1 : 0;
    if (num < 64) cache[num] = (signed char)(r ? 1 : -1);
    return r;
}

/* Do we stand in for this COM port?
 *
 * DEFAULT IS "ONLY IF THE PORT IS NOT REALLY THERE", and the reason is measured.
 * Mario Kart Arcade GP DX uses TWO serial devices: COM3 is its JVS I/O board and
 * COM1 is the STR PCB, the steering/force-feedback board whose protocol we do
 * not speak. Claiming everything -- which an earlier version of this file did --
 * hands the game a JVS node on its steering port, and that is strictly worse
 * than leaving it alone:
 *
 *   claim COM2+COM3   -> E03-02  I/O board type abnormal
 *   claim EVERYTHING  -> E22-12  STR PCB comms error   (broke the wrong port)
 *   claim nothing     -> E03-01  I/O board NOT FOUND   (no COM3 on this PC)
 *   claim COM3 only   -> full JVS handshake, thousands of poll frames, and the
 *                        game's own boot screen prints "I/O check OK JVI/O connected"
 *
 * Probing the port reproduces that winning case with no per-game configuration:
 * this PC really has COM1 so the steering port passes through to the real
 * driver, and it has no COM3 so the JVS bus gets our node.
 *
 *   ULTRA_JVS_COM=3      claim ONLY COM3 (comma/space separated list)
 *   ULTRA_JVS_COM=any    claim every port, real hardware included
 *   ULTRA_JVS_COM=none   never claim; always use the real driver
 */
static int com_port_openable(int num) {
    /* Cached: CreateFile runs thousands of times and we must not re-probe the
     * hardware on each one. -1 unknown, 1 openable, 0 not. */
    static signed char cache[64];
    char path[24];
    HANDLE h;
    int r;
    if (num < 0) return 0;
    if (num < 64 && cache[num]) return cache[num] > 0;
    snprintf(path, sizeof path, "\\\\.\\COM%d", num);
    /* Plain CreateFileA on purpose: ultra_iat_patch_all() deliberately skips our
     * own module, so this call is already the genuine kernel32 one and never
     * re-enters our own hook. (Real_CreateFileA is not declared until further
     * down this file, and reaching forward for it buys nothing.) */
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                    OPEN_EXISTING, 0, NULL);
    r = (h != INVALID_HANDLE_VALUE);
    if (r) CloseHandle(h);
    if (num < 64) cache[num] = (signed char)(r ? 1 : -1);
    return r;
}

static int we_claim_port(int num) {
    const char* env;
    if (num < 0) return 0;
    env = getenv("ULTRA_JVS_COM");
    if (env && *env) {
        char buf[64]; char* tk;
        if (_stricmp(env, "none") == 0) return 0;
        if (_stricmp(env, "any")  == 0) return 1;
        strncpy(buf, env, sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
        for (tk = strtok(buf, ", "); tk; tk = strtok(NULL, ", "))
            if (atoi(tk) == num) return 1;
        return 0;
    }
    (void)com_port_physically_exists;   /* kept as a documented probe */
    /* DEFAULT = the legacy JVS ports, PLUS any port this PC does not have.
     *
     * Both halves are needed, each learned the hard way on 2026-09-10:
     *
     *  - Chaos Breaker opens COM2, and this desktop REALLY HAS COM1 and COM2
     *    (\\Device\\Serial0/1) with nothing attached. A "stand in only for a
     *    port that will not open" rule therefore handed the game a live but
     *    DEAD port: no JVS node, and every control silently stopped working
     *    while the game itself ran fine. Claiming 2 and 3 is what this board
     *    always did and what its games expect.
     *
     *  - Mario Kart GP DX opens \\\\.\\COM1 for its STEERING board and COM3
     *    for its JVS I/O. COM3 does not exist here, so the absent-port half is
     *    what gives it a JVS bus at all; and because COM1 is neither 2 nor 3 it
     *    is correctly left alone -- claiming it answers JVS at the steering
     *    board and makes that title fail EARLIER.
     *
     * So: always claim 2 and 3, also claim anything the OS cannot give us, and
     * leave a real port the user actually owns alone.
     *   ULTRA_JVS_COM=<list>  claim exactly these
     *   ULTRA_JVS_COM=any     claim everything, real hardware included
     *   ULTRA_JVS_COM=none    never claim                                */
    if (num == 2 || num == 3) return 1;
    return !com_port_openable(num);
}
/* -------------------------------------------------------------------------- */
/*  Generic USB/parallel security-dongle name match (ULTRA_DONGLE_BYPASS)      */
/* -------------------------------------------------------------------------- */
static int is_dongle_device_examu(const char* name) {
    if (!name) return 0;
    const char* n = name;
    if (n[0]=='\\' && n[1]=='\\' && n[2]=='.' && n[3]=='\\') n += 4;
    else if (n[0]=='\\' && n[1]=='\\' && n[2]=='?' && n[3]=='\\') n += 4;
    else return 0;
    return (_strnicmp(n, "Hardlock", 8) == 0 ||
            _strnicmp(n, "HASP", 4) == 0 ||
            _strnicmp(n, "SentinelSuperPro", 16) == 0 ||
            _strnicmp(n, "SafeNet", 7) == 0 ||
            _strnicmp(n, "Aladdin", 7) == 0);
}

/* -------------------------------------------------------------------------- */
/*  WinIo kernel-driver emulation (Daemon Bride / IpgExKey.dll dongle path)     */
/* --------------------------------------------------------------------------
 * Daemon Bride's dongle library IpgExKey.dll (the ONLY module in this dump that
 * imports DeviceIoControl -- game.exe does not) opens \\.\WINIO and drives the
 * eX-BOARD hardware key through DIRECT PORT I/O via the WinIo (Yariv Kaplan)
 * kernel driver. WinIo.sys is an old unsigned 32-bit driver that will not load
 * on 64-bit Windows, so the real open fails and the game stalls at I/O init --
 * every run's dedicated log ends at exactly "CreateFileA: \\.\WINIO".
 *
 * We stand in for the driver: CreateFile("\\.\WINIO") returns a synthetic
 * handle and DeviceIoControl on that handle is serviced here instead of going
 * to a driver that cannot load. WinIo's client library performs byte/word/dword
 * port reads+writes (and physical-memory mapping) through METHOD_BUFFERED
 * IOCTLs. The EXACT control codes, struct layout, and the ports IpgExKey reads
 * are not assumed: ultra_winio_ioctl() logs every field of every call FIRST
 * (gated on ULTRA_JVS_LOG) so a single diagnostic run reveals the real protocol
 * before any port value is pinned to make the key check pass.
 *
 * ADDITIVE + SAFE: only the "WINIO" device name is intercepted, so no other
 * eX-BOARD title (the JVS-over-serial fighters) is affected at all. Opt out
 * entirely with ULTRA_WINIO_EMU=0.
 * -------------------------------------------------------------------------- */
#define EXAMU_WINIO_SLOTS 8
static HANDLE g_winio_h[EXAMU_WINIO_SLOTS];

static int winio_emu_enabled(void) {
    const char* e = getenv("ULTRA_WINIO_EMU");
    return !(e && e[0] == '0');          /* default ON; only affects \\.\WINIO */
}
static int is_winio_device(const char* name) {
    if (!name) return 0;
    const char* n = name;
    if (n[0]=='\\' && n[1]=='\\' && (n[2]=='.'||n[2]=='?') && n[3]=='\\') n += 4;
    return _stricmp(n, "WINIO") == 0;
}
static int is_winio_handle(HANDLE h) {
    int r = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < EXAMU_WINIO_SLOTS; ++i) if (g_winio_h[i] == h) { r = 1; break; }
    LeaveCriticalSection(&g_lock);
    return r;
}
static HANDLE winio_open(void) {
    HANDLE h = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!h || h == INVALID_HANDLE_VALUE) h = (HANDLE)(uintptr_t)0xF17E0001;
    int placed = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < EXAMU_WINIO_SLOTS; ++i)
        if (!g_winio_h[i]) { g_winio_h[i] = h; placed = 1; break; }
    LeaveCriticalSection(&g_lock);
    jvs_diag("OPEN \\\\.\\WINIO -> eX-BOARD WinIo driver emulation (fake handle=%p, slot=%s)",
             (void*)h, placed ? "ok" : "FULL(untracked)");
    return h;
}
static void winio_close(HANDLE h) {
    for (int i = 0; i < EXAMU_WINIO_SLOTS; ++i) if (g_winio_h[i] == h) g_winio_h[i] = NULL;
}

/* Shadow of port space so a value written by the game reads back consistently.
 * Reads of an untouched port return ULTRA_WINIO_READ_DEFAULT (default 0x00). */
static unsigned char g_winio_port[0x10000];
static int           g_winio_port_init = 0;
static unsigned      g_winio_read_default = 0;

/* ---- observed IOCTL function numbers for THIS dump's WinIo variant ----------
 * From the first Daemon Bride capture (2026-09-30), the driver is a custom WinIo
 * build with DeviceType 0x8010 and METHOD_BUFFERED:
 *   func 0x812  (code 0x80102048)  no buffers            -> init/ping (return TRUE)
 *   func 0x810  (code 0x80102040)  16-byte in==out descr -> MapPhysToLin
 * The 0x810 descriptor is four little-endian DWORDs:
 *   [0] magic/stale (observed 0x80102048, a don't-care)
 *   [1] size in bytes        (observed 0x0E = 14)
 *   [2] physical address     (observed 0x000FE020, BIOS/ROM segment)
 *   [3] OUT: linear pointer the caller then dereferences (observed 0 on entry)
 * IpgExKey.dll bailed with -1 because [3] came back NULL. We hand back a real
 * readable window so the map "succeeds" and it reads the region. */
#define WINIO_FUNC_MAPPHYS   0x810u

/* One readable window we hand back for every MapPhysToLin. Grown on demand and
 * (re)seeded each call. Default content is ULTRA_WINIO_MAP_FILL (one byte, def
 * 0x00); ULTRA_WINIO_MAP_BYTES seeds the leading bytes with a known hex string
 * once the expected hardware-key value is learned -- no rebuild needed. */
static void*  g_winio_map_base = NULL;
static size_t g_winio_map_cap  = 0;

/* The eX-BOARD hardware key Daemon Bride's IpgExKey.dll validates. Extracted by
 * static RE of IpgExKey.dll: export GetKeyLicense@0 calls MapPhysToLin(phys=
 * 0xFE020, size=14), reads the 14 mapped bytes, then strcmp's them against the
 * fixed 14-char string "dlsxjvkzmrpdla" at IpgExKey .data 0x10007054. MapPhysToLin
 * returns exactly the linear pointer we place in the descriptor's DWORD[3], with
 * no page-offset applied, so the key must sit at window offset 0. Seeding these
 * bytes makes the dongle check return TRUE. ULTRA_WINIO_MAP_BYTES overrides. */
static const unsigned char EXAMU_EXBOARD_KEY_FE020[14] = {
    'd','l','s','x','j','v','k','z','m','r','p','d','l','a'
};

static void winio_seed_hex(unsigned char* buf, size_t cap, const char* hex) {
    size_t o = 0;
    for (const char* p = hex; *p && o < cap; ) {
        while (*p==' '||*p=='\t'||*p==','||*p=='\r'||*p=='\n') ++p;
        if (!*p) break;
        if (p[0]=='0' && (p[1]=='x'||p[1]=='X')) p += 2;
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) break;
        char b[3] = { p[0], p[1], 0 };
        buf[o++] = (unsigned char)strtoul(b, NULL, 16);
        p += 2;
    }
}
static void* winio_map_window(DWORD phys, DWORD size) {
    size_t need = size ? (size_t)size : 16;
    if (need < 0x1000) need = 0x1000;
    if (!g_winio_map_base || g_winio_map_cap < need) {
        if (g_winio_map_base) VirtualFree(g_winio_map_base, 0, MEM_RELEASE);
        g_winio_map_base = VirtualAlloc(NULL, need, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_winio_map_cap  = g_winio_map_base ? need : 0;
    }
    if (g_winio_map_base) {
        const char* fe = getenv("ULTRA_WINIO_MAP_FILL");
        int fill = (fe && *fe) ? (int)strtoul(fe, NULL, 0) : 0x00;
        memset(g_winio_map_base, fill & 0xFF, g_winio_map_cap);
        const char* sb = getenv("ULTRA_WINIO_MAP_BYTES");
        if (sb && *sb) {
            winio_seed_hex((unsigned char*)g_winio_map_base, g_winio_map_cap, sb);
        } else if (phys == 0xFE020u && g_winio_map_cap > sizeof EXAMU_EXBOARD_KEY_FE020) {
            memcpy(g_winio_map_base, EXAMU_EXBOARD_KEY_FE020, sizeof EXAMU_EXBOARD_KEY_FE020);
            ((unsigned char*)g_winio_map_base)[sizeof EXAMU_EXBOARD_KEY_FE020] = 0; /* strcmp terminator */
        }
    }
    return g_winio_map_base;
}

/* Service one DeviceIoControl aimed at our fake \\.\WINIO handle.
 * Every field is logged FIRST (ULTRA_JVS_LOG) so each run's capture is complete,
 * then the known functions are emulated and anything unknown falls back to the
 * Phase-1 echo+succeed behavior (still logged, still lets the game continue). */
static BOOL ultra_winio_ioctl(HANDLE h, DWORD code, LPVOID inb, DWORD inlen,
                              LPVOID outb, DWORD outlen, LPDWORD ret) {
    DWORD dev    = (code >> 16) & 0xFFFF;
    DWORD func   = (code >> 2)  & 0xFFF;
    DWORD method = code & 3;
    DWORD access = (code >> 14) & 3;
    (void)dev;

    if (!g_winio_port_init) {
        const char* e = getenv("ULTRA_WINIO_READ_DEFAULT");
        g_winio_read_default = (e && *e) ? (unsigned)strtoul(e, NULL, 0) : 0x00u;
        memset(g_winio_port, (int)(g_winio_read_default & 0xFF), sizeof g_winio_port);
        g_winio_port_init = 1;
    }

    jvs_diag("WINIO ioctl code=0x%08lX (dev=0x%lX func=0x%lX method=%lu access=%lu) inSize=%lu outSize=%lu inBuf=%p outBuf=%p",
             (unsigned long)code, (unsigned long)(code>>16)&0xFFFF, (unsigned long)func,
             (unsigned long)method, (unsigned long)access,
             (unsigned long)inlen, (unsigned long)outlen, inb, outb);
    if (inb  && inlen)  jvs_diag_hex("WINIO IN ", (const unsigned char*)inb,  inlen);
    if (outb && outlen && outb != inb) jvs_diag_hex("WINIO OUT(pre)", (const unsigned char*)outb, outlen);

    /* ---- MapPhysToLin (func 0x810): return a real readable linear pointer ---- */
    if (func == WINIO_FUNC_MAPPHYS && inb && inlen >= 16) {
        const DWORD* din = (const DWORD*)inb;
        DWORD magic = din[0], size = din[1], phys = din[2];
        void* lin = winio_map_window(phys, size);

        /* METHOD_BUFFERED: if in and out are distinct, mirror the descriptor
         * across first; then write the linear pointer into DWORD[3]. When the
         * client reuses one buffer (observed here), inb==outb and DWORD[1]/[2]
         * are left intact -- we only set the previously-NULL out slot. */
        DWORD* dout = (DWORD*)(outb ? outb : inb);
        DWORD  ocap = outb ? outlen : inlen;
        if (outb && outb != inb && outlen >= 16) memcpy(outb, inb, 16);
        if (ocap >= 16) dout[3] = (DWORD)(uintptr_t)lin;

        int keyed = (phys == 0xFE020u) && !getenv("ULTRA_WINIO_MAP_BYTES");
        jvs_diag("WINIO MAPPHYS func=0x810 magic=0x%lX size=%lu phys=0x%lX -> lin=%p (cap=%zu, %s)",
                 (unsigned long)magic, (unsigned long)size, (unsigned long)phys,
                 lin, g_winio_map_cap,
                 keyed ? "seeded eX-BOARD key 'dlsxjvkzmrpdla'"
                       : (getenv("ULTRA_WINIO_MAP_BYTES") ? "seeded ULTRA_WINIO_MAP_BYTES" : "fill only"));
        if (outb && ocap) jvs_diag_hex("WINIO OUT", (const unsigned char*)dout, ocap < 16 ? ocap : 16);
        if (ret) *ret = ocap ? (ocap < 16 ? ocap : 16) : 16;
        return TRUE;
    }

    /* ---- default: echo the client's struct back + succeed (still logged) ----
     * Covers func 0x812 (init/ping, no buffers) and any not-yet-identified call
     * (e.g. UnmapPhysAddr). Strictly better than a failed real open and keeps
     * the game moving so the next run's capture shows the following step. */
    if (outb && outlen && inb && outb != inb) {
        DWORD n = (inlen < outlen) ? inlen : outlen;
        memcpy(outb, inb, n);
    }
    if (ret) *ret = outlen ? outlen : inlen;
    return TRUE;
}

/* -------------------------------------------------------------------------- */
/*  Originals (resolved from kernel32) + typedefs                              */
/* -------------------------------------------------------------------------- */
typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef BOOL   (WINAPI *pDeviceIoControl)(HANDLE,DWORD,LPVOID,DWORD,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL   (WINAPI *pReadFile)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL   (WINAPI *pWriteFile)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL   (WINAPI *pCloseHandle)(HANDLE);
typedef BOOL   (WINAPI *pSetCommState)(HANDLE,LPDCB);
typedef BOOL   (WINAPI *pGetCommState)(HANDLE,LPDCB);
typedef BOOL   (WINAPI *pSetCommTimeouts)(HANDLE,LPCOMMTIMEOUTS);
typedef BOOL   (WINAPI *pGetCommTimeouts)(HANDLE,LPCOMMTIMEOUTS);
typedef BOOL   (WINAPI *pPurgeComm)(HANDLE,DWORD);
typedef BOOL   (WINAPI *pSetupComm)(HANDLE,DWORD,DWORD);
typedef BOOL   (WINAPI *pClearCommError)(HANDLE,LPDWORD,LPCOMSTAT);
typedef BOOL   (WINAPI *pGetCommModemStatus)(HANDLE,LPDWORD);
typedef BOOL   (WINAPI *pEscapeCommFunction)(HANDLE,DWORD);
typedef BOOL   (WINAPI *pGetOverlappedResult)(HANDLE,LPOVERLAPPED,LPDWORD,BOOL);
typedef BOOL   (WINAPI *pWaitCommEvent)(HANDLE,LPDWORD,LPOVERLAPPED);
typedef BOOL   (WINAPI *pSetCommMask)(HANDLE,DWORD);
typedef FARPROC(WINAPI *pGetProcAddress)(HMODULE,LPCSTR);
typedef HMODULE(WINAPI *pLoadLibraryA)(LPCSTR);
typedef HMODULE(WINAPI *pLoadLibraryW)(LPCWSTR);

static pCreateFileA        Real_CreateFileA;
static pCreateFileW        Real_CreateFileW;
static pDeviceIoControl    Real_DeviceIoControl;
static pReadFile           Real_ReadFile;
static pWriteFile          Real_WriteFile;
static pCloseHandle        Real_CloseHandle;
static pSetCommState       Real_SetCommState;
static pGetCommState       Real_GetCommState;
static pSetCommTimeouts    Real_SetCommTimeouts;
static pGetCommTimeouts    Real_GetCommTimeouts;
static pPurgeComm          Real_PurgeComm;
static pSetupComm          Real_SetupComm;
static pClearCommError     Real_ClearCommError;
static pGetCommModemStatus Real_GetCommModemStatus;
static pEscapeCommFunction Real_EscapeCommFunction;
static pGetOverlappedResult Real_GetOverlappedResult;
static pWaitCommEvent      Real_WaitCommEvent;
static pSetCommMask        Real_SetCommMask;
static pGetProcAddress     Real_GetProcAddress;
static pLoadLibraryA       Real_LoadLibraryA;
static pLoadLibraryW       Real_LoadLibraryW;

/* -------------------------------------------------------------------------- */
/*  The shims                                                                   */
/* -------------------------------------------------------------------------- */
HANDLE open_fake_com_examu(int port) {
    HANDLE h = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!h) return INVALID_HANDLE_VALUE;
    EnterCriticalSection(&g_lock);
    examu_com_slot* s = slot_alloc(h, port);
    LeaveCriticalSection(&g_lock);
    if (!s) {
        Real_CloseHandle ? Real_CloseHandle(h) : (void)CloseHandle(h);
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return INVALID_HANDLE_VALUE;
    }
    jvs_diag("OPEN COM%d -> eX-BOARD JVS fake handle (slot ok)", port);
    return h;
}

static void complete_overlapped(HANDLE h, LPOVERLAPPED ov, DWORD bytes) {
    if (!ov) return;
    ov->Internal = 0;
    ov->InternalHigh = bytes;
    SetEvent(ov->hEvent ? ov->hEvent : h);
}

/* ---------------------------------------------------------------------------
 *  CABINET DATA DRIVE REDIRECT
 * ---------------------------------------------------------------------------
 * Arcade games write to whatever absolute path their cabinet used for its
 * writable partition - typically D:, E: or F:. On a PC those are somebody's
 * own drives. Mario Kart GP DX was found creating Bookkeeping, TestMode,
 * playdata and DongleData folders directly on a user's personal drive, next
 * to their documents, for months before anyone noticed.
 *
 * Every such path is rewritten under the ONE writable folder the loader
 * resolves for this game and publishes as ULTRA_GAMEDATA, keeping the
 * sub-path so the game's own layout survives:
 *
 *     F:/Bookkeeping/bookkeeping_123.bin
 *       -> %LOCALAPPDATA%/Ultra Loader/gamedata/<game>/Bookkeeping/...
 *
 * Game dumps stay read-only and every game's state sits in one place the user
 * can back up or delete. See work/include/ultra_gamedata.h.
 *
 * Inlined per board rather than shared from work/include, because board hooks
 * build with only their own -I include directory and each board's hook is
 * deliberately self-contained.
 *
 * ULTRA_CABINET_DRIVES is the drive list (default "DEFGHJ");
 * ULTRA_REDIRECT_CABINET=0 disables it. The drive the game itself runs from
 * is NEVER redirected - that is its install, which it reads rather than
 * writes, and rewriting it would break loading. */
static int ultra_redirect_cabinet_path(const char* in, char* out, size_t cap) {
    static char base[MAX_PATH];
    static int  base_state = 0;   /* 0 unknown, 1 ready, -1 unavailable */
    const char* e;
    const char* drives;
    char c, self[MAX_PATH];
    const char* q;
    int hit = 0;

    if (!in || !out || !in[0]) return 0;
    if (in[1] != ':' || (in[2] != 92 && in[2] != 47)) return 0;
    if (base_state < 0) return 0;

    e = getenv("ULTRA_REDIRECT_CABINET");
    if (e && e[0] == '0') return 0;

    drives = getenv("ULTRA_CABINET_DRIVES");
    if (!drives || !*drives) drives = "DEFGHJ";
    c = (char)toupper((unsigned char)in[0]);
    for (q = drives; *q; ++q) if ((char)toupper((unsigned char)*q) == c) { hit = 1; break; }
    if (!hit) return 0;

    self[0] = 0;
    GetModuleFileNameA(NULL, self, (DWORD)sizeof self);
    if (self[0] && (char)toupper((unsigned char)self[0]) == c) return 0;

    if (!base_state) {
        e = getenv("ULTRA_GAMEDATA");
        if (!e || !*e) { base_state = -1; return 0; }   /* loader set none */
        _snprintf(base, sizeof base, "%s", e);
        base[sizeof base - 1] = 0;
        CreateDirectoryA(base, NULL);
        base_state = 1;
    }

    _snprintf(out, cap, "%s%c%s", base, 92, in + 3);
    out[cap - 1] = 0;
    {   char* p;
        size_t blen = strlen(base);
        for (p = out + blen; *p; ++p)
            if (*p == 92 || *p == 47) { char t = *p; *p = 0; CreateDirectoryA(out, NULL); *p = t; }
    }
    return 1;
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD acc, DWORD share,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (name) {
        /* Keep the game's writes inside its own data folder - see above. */
        {   char ul_rdb[1024];
            if (ultra_redirect_cabinet_path(name, ul_rdb, sizeof ul_rdb)) {
                return Real_CreateFileA(ul_rdb, acc, share, sa, disp, flags, tmpl);
            }
        }
        jvs_diag("CreateFileA: %s", name);
        if (is_winio_device(name) && winio_emu_enabled())
            return winio_open();
        int p = port_number_from_path(name);
        if (we_claim_port(p)) return open_fake_com_examu(p);
        if (is_dongle_device_examu(name)) {
            const char* e = getenv("ULTRA_DONGLE_BYPASS");
            if (e && e[0] == '1') {
                HANDLE fk = CreateEventA(NULL, TRUE, FALSE, NULL);
                jvs_diag("  -> dongle device '%s' bypassed (fake handle=%p)", name, (void*)fk);
                return fk ? fk : (HANDLE)(uintptr_t)0xDEAD0001;
            }
        }
        /* Asset-open visibility (bring-up): the log used to show only the NAME,
         * so a FlashRom asset that failed to open looked identical to one that
         * opened fine. Log the actual result for real file opens (not devices)
         * so a missing/unreadable/locked asset that the loader treats as fatal
         * is unmistakable. Only names with a path separator, to skip devices. */
        if (strpbrk(name, "\\/")) {
            HANDLE h = Real_CreateFileA(name, acc, share, sa, disp, flags, tmpl);
            if (h == INVALID_HANDLE_VALUE)
                jvs_diag("  -> OPEN FAILED err=%lu  '%s'", (unsigned long)GetLastError(), name);
            return h;
        }
    }
    return Real_CreateFileA(name, acc, share, sa, disp, flags, tmpl);
}
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD share,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (name) {
        char a[256]; int n = WideCharToMultiByte(CP_ACP, 0, name, -1, a, sizeof(a)-1, NULL, NULL);
        if (n > 0) {
            a[n] = 0; jvs_diag("CreateFileW: %s", a);
            if (is_winio_device(a) && winio_emu_enabled())
                return winio_open();
            int p = port_number_from_path(a);
            if (we_claim_port(p)) return open_fake_com_examu(p);
            if (is_dongle_device_examu(a)) {
                const char* e = getenv("ULTRA_DONGLE_BYPASS");
                if (e && e[0] == '1') {
                    HANDLE fk = CreateEventA(NULL, TRUE, FALSE, NULL);
                    jvs_diag("  -> dongle device '%s' bypassed (fake handle=%p)", a, (void*)fk);
                    return fk ? fk : (HANDLE)(uintptr_t)0xDEAD0001;
                }
            }
        }
    }
    return Real_CreateFileW(name, acc, share, sa, disp, flags, tmpl);
}

static BOOL WINAPI Hook_WriteFile(HANDLE h, LPCVOID buf, DWORD len,
                                  LPDWORD written, LPOVERLAPPED ov) {
    EnterCriticalSection(&g_lock);
    examu_com_slot* s = slot_for(h);
    if (s) {
        ultra_jvs_node_examu* node = ultra_hook_node_examu();
        ultra_hook_pull_input_examu(node);
        if (s->resp_pos) {
            memmove(s->resp, s->resp + s->resp_pos, s->resp_len - s->resp_pos);
            s->resp_len -= s->resp_pos; s->resp_pos = 0;
        }
        uint8_t tmp[2048];
        size_t got = jvs_examu_host_bytes(node, (const uint8_t*)buf, len, tmp, sizeof(tmp));
        size_t room = sizeof(s->resp) - s->resp_len;
        if (got > room) got = room;
        memcpy(s->resp + s->resp_len, tmp, got);
        s->resp_len += got;
        jvs_diag_hex("TX", (const unsigned char*)buf, len);
        jvs_diag_hex("RX", tmp, got);
        LeaveCriticalSection(&g_lock);
        if (written) *written = len;
        complete_overlapped(h, ov, len);
        return TRUE;
    }
    LeaveCriticalSection(&g_lock);
    return Real_WriteFile(h, buf, len, written, ov);
}

static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD len,
                                 LPDWORD read, LPOVERLAPPED ov) {
    EnterCriticalSection(&g_lock);
    examu_com_slot* s = slot_for(h);
    if (s) {
        size_t avail = s->resp_len - s->resp_pos;
        size_t give = (len < avail) ? len : avail;
        if (give) memcpy(buf, s->resp + s->resp_pos, give);
        s->resp_pos += give;
        jvs_diag("ReadFile req=%lu give=%zu (queued=%zu)", (unsigned long)len, give, avail);
        LeaveCriticalSection(&g_lock);
        if (read) *read = (DWORD)give;
        complete_overlapped(h, ov, (DWORD)give);
        return TRUE;
    }
    LeaveCriticalSection(&g_lock);
    return Real_ReadFile(h, buf, len, read, ov);
}

static BOOL WINAPI Hook_CloseHandle(HANDLE h) {
    EnterCriticalSection(&g_lock);
    int ours = slot_for(h) != NULL;
    if (ours) slot_free(h);
    winio_close(h);                 /* also releases a fake \\.\WINIO handle */
    LeaveCriticalSection(&g_lock);
    return Real_CloseHandle(h);
}

/* DeviceIoControl: only our fake \\.\WINIO handle is serviced here; everything
 * else passes straight through to the real kernel32 export. */
static BOOL WINAPI Hook_DeviceIoControl(HANDLE h, DWORD code,
        LPVOID inb, DWORD inlen, LPVOID outb, DWORD outlen,
        LPDWORD ret, LPOVERLAPPED ov) {
    if (is_winio_handle(h))
        return ultra_winio_ioctl(h, code, inb, inlen, outb, outlen, ret);
    return Real_DeviceIoControl(h, code, inb, inlen, outb, outlen, ret, ov);
}

#define COM_NOOP_TRUE(NAME, REAL, SIG, ARGS, TAG) \
    static BOOL WINAPI NAME SIG { \
        EnterCriticalSection(&g_lock); int ours = slot_for(h) != NULL; LeaveCriticalSection(&g_lock); \
        if (ours) { jvs_diag(TAG); return TRUE; } return REAL ARGS; }

COM_NOOP_TRUE(Hook_SetCommState,    Real_SetCommState,    (HANDLE h, LPDCB d),          (h,d), "SetCommState")
COM_NOOP_TRUE(Hook_SetCommTimeouts, Real_SetCommTimeouts, (HANDLE h, LPCOMMTIMEOUTS t), (h,t), "SetCommTimeouts")
COM_NOOP_TRUE(Hook_PurgeComm,       Real_PurgeComm,       (HANDLE h, DWORD f),          (h,f), "PurgeComm")

static BOOL WINAPI Hook_EscapeCommFunction(HANDLE h, DWORD f) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) { jvs_diag("EscapeCommFunction(%lu)", (unsigned long)f); return TRUE; }
    return Real_EscapeCommFunction(h,f);
}
static BOOL WINAPI Hook_GetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS t) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) { if (t) memset(t,0,sizeof(*t)); jvs_diag("GetCommTimeouts"); return TRUE; }
    return Real_GetCommTimeouts(h,t);
}
static BOOL WINAPI Hook_SetupComm(HANDLE h, DWORD in, DWORD out) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) { jvs_diag("SetupComm"); return TRUE; } return Real_SetupComm(h,in,out);
}
static BOOL WINAPI Hook_GetCommState(HANDLE h, LPDCB d) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) { if (d) { memset(d,0,sizeof(*d)); d->DCBlength=sizeof(*d); d->BaudRate=115200;
                         d->ByteSize=8; } jvs_diag("GetCommState"); return TRUE; }
    return Real_GetCommState(h,d);
}
static BOOL WINAPI Hook_ClearCommError(HANDLE h, LPDWORD err, LPCOMSTAT st) {
    EnterCriticalSection(&g_lock); examu_com_slot* s = slot_for(h);
    if (s) { if (err) *err = 0;
             DWORD q = (DWORD)(s->resp_len - s->resp_pos);
             if (st) { memset(st,0,sizeof(*st)); st->cbInQue = q; }
             LeaveCriticalSection(&g_lock); jvs_diag("ClearCommError inQ=%lu", (unsigned long)q); return TRUE; }
    LeaveCriticalSection(&g_lock);
    return Real_ClearCommError(h,err,st);
}
static BOOL WINAPI Hook_GetCommModemStatus(HANDLE h, LPDWORD st) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) {
        /* JVS SENSE line is dynamic: while unaddressed the host must see
         * "node present, address me"; once addressed, "chain fully addressed".
         * serialsense() maps to the CTS bit (0x10) on the public protocol
         * every JVS-over-serial board (Type X family included) shares. */
        ultra_jvs_node_examu* node = ultra_hook_node_examu();
        int addressed = (node && jvs_examu_address(node) != 0);
        DWORD v;
        if (addressed) { const char* e = getenv("ULTRA_EXAMU_JVS_SENSE1"); v = (e && *e) ? (DWORD)strtoul(e,NULL,0) : (DWORD)MS_CTS_ON; }
        else           { const char* e = getenv("ULTRA_EXAMU_JVS_SENSE0"); v = (e && *e) ? (DWORD)strtoul(e,NULL,0) : 0u; }
        if (st) *st = v;
        jvs_diag("GetCommModemStatus -> 0x%lX (addressed=%d)", (unsigned long)v, addressed);
        return TRUE;
    }
    return Real_GetCommModemStatus(h,st);
}
static BOOL WINAPI Hook_SetCommMask(HANDLE h, DWORD mask) {
    EnterCriticalSection(&g_lock); examu_com_slot* s = slot_for(h); if (s) s->evtmask = mask;
    int ours = s != NULL; LeaveCriticalSection(&g_lock);
    if (ours) { return TRUE; } return Real_SetCommMask(h, mask);
}
static BOOL WINAPI Hook_WaitCommEvent(HANDLE h, LPDWORD evt, LPOVERLAPPED ov) {
    EnterCriticalSection(&g_lock); examu_com_slot* s = slot_for(h);
    if (s) {
        DWORD mask = s->evtmask ? s->evtmask : (DWORD)EV_RXCHAR;
        DWORD ev = (s->resp_len > s->resp_pos) ? (mask & EV_RXCHAR) : 0;
        LeaveCriticalSection(&g_lock);
        if (evt) *evt = ev;
        complete_overlapped(h, ov, 0);
        return TRUE;
    }
    LeaveCriticalSection(&g_lock);
    return Real_WaitCommEvent(h, evt, ov);
}
static BOOL WINAPI Hook_GetOverlappedResult(HANDLE h, LPOVERLAPPED ov,
                                            LPDWORD cnt, BOOL wait) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h) != NULL; LeaveCriticalSection(&g_lock);
    if (ours) {
        if (cnt && ov) *cnt = (DWORD)ov->InternalHigh;
        return TRUE;
    }
    return Real_GetOverlappedResult(h, ov, cnt, wait);
}

/* -------------------------------------------------------------------------- */
/*  user32 display/window shims                                                */
/*                                                                             */
/*  THE POST-DONGLE WALL (Daemon Bride, statically proven 2026-09-30):         */
/*  game.exe's main "run" method (0x407890) passes GetKeyLicense (dongle now   */
/*  seeded, verified) then calls its display/window init sub_408300. That      */
/*  routine's FIRST act is ChangeDisplaySettingsA(&dm, CDS_TEST) with a        */
/*  DEVMODE of 640x480 @ 16 bpp fullscreen (dmFields=0x5C0000 =                */
/*  WIDTH|HEIGHT|BITSPERPEL|FREQ). Modern Windows exposes NO 16-bpp display    */
/*  modes, so the TEST returns DISP_CHANGE_BADMODE (a negative LONG); the game */
/*  does `jl` -> `xor eax,eax; ret` so sub_408300 returns 0; the caller at    */
/*  0x407984 then jumps straight to its epilogue (0x408135) and returns 0,     */
/*  which bubbles to WinMain -> ExitProcess(0). That is EXACTLY the user's     */
/*  "no window, exits clean (loader logs exit code 0)" report: it never even   */
/*  reaches Direct3DCreate9 (no D3D9 log line ever appears).                   */
/*                                                                             */
/*  We already force-windowed D3D9 on this board (ULTRA_SEGA_WINDOWED=1, see   */
/*  d3d9_throttle_examu.c), so the game never needs a real exclusive mode      */
/*  change. Hook ChangeDisplaySettingsA to report what the REAL call would say */
/*  (CDS_TEST only, never changes the desktop) and then return                 */
/*  DISP_CHANGE_SUCCESSFUL so init proceeds. Opt out: ULTRA_EXAMU_CDS_FAKE=0.  */
/*                                                                             */
/*  Also: on its NORMAL shutdown path (0x408112) the game calls               */
/*  ExitWindowsEx(EWX_REBOOT) -- arcade cabinet reboot-on-exit. Neutralize it  */
/*  so exiting the game can never reboot the user's PC (opt in to the real     */
/*  behavior with ULTRA_EXAMU_ALLOW_REBOOT=1). CreateWindowExA is wrapped for  */
/*  diagnostics only (pass-through) so the log shows the window actually born. */
/* -------------------------------------------------------------------------- */
typedef LONG (WINAPI *pChangeDisplaySettingsA)(DEVMODEA*, DWORD);
typedef HWND (WINAPI *pCreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
                                        HWND, HMENU, HINSTANCE, LPVOID);
typedef BOOL (WINAPI *pExitWindowsEx)(UINT, DWORD);
static pChangeDisplaySettingsA Real_ChangeDisplaySettingsA = NULL;
static pCreateWindowExA        Real_CreateWindowExA        = NULL;
static pExitWindowsEx          Real_ExitWindowsEx          = NULL;

static int examu_env_on(const char* name, int defval) {
    const char* e = getenv(name);
    if (!e || !*e) return defval;
    return !(e[0] == '0' && e[1] == '\0');
}

static const char* cds_name(LONG r) {
    switch (r) {
        case DISP_CHANGE_SUCCESSFUL: return "SUCCESSFUL";
        case DISP_CHANGE_RESTART:    return "RESTART";
        case DISP_CHANGE_FAILED:     return "FAILED";
        case DISP_CHANGE_BADMODE:    return "BADMODE(no such mode on this display -- e.g. 16bpp)";
        case DISP_CHANGE_NOTUPDATED: return "NOTUPDATED";
        case DISP_CHANGE_BADFLAGS:   return "BADFLAGS";
        case DISP_CHANGE_BADPARAM:   return "BADPARAM";
        default:                     return "other";
    }
}

static LONG WINAPI Hook_ChangeDisplaySettingsA(DEVMODEA* dm, DWORD flags) {
    if (dm) {
        jvs_diag("CHANGEDISPLAY req %lux%lu %lubpp %luHz fields=0x%lX flags=0x%lX",
                 (unsigned long)dm->dmPelsWidth,  (unsigned long)dm->dmPelsHeight,
                 (unsigned long)dm->dmBitsPerPel, (unsigned long)dm->dmDisplayFrequency,
                 (unsigned long)dm->dmFields,     (unsigned long)flags);
        /* CDS_TEST validates the mode WITHOUT touching the desktop -- safe to
         * probe so the log states outright why the stock call fails here. */
        if (Real_ChangeDisplaySettingsA) {
            LONG t = Real_ChangeDisplaySettingsA(dm, CDS_TEST);
            jvs_diag("CHANGEDISPLAY real CDS_TEST -> %ld (%s)", (long)t, cds_name(t));
        }
    } else {
        jvs_diag("CHANGEDISPLAY req <restore registry default> flags=0x%lX",
                 (unsigned long)flags);
    }
    if (examu_env_on("ULTRA_EXAMU_CDS_FAKE", 1)) {
        jvs_diag("CHANGEDISPLAY -> faked DISP_CHANGE_SUCCESSFUL "
                 "(no real mode change; rendering is force-windowed)");
        return DISP_CHANGE_SUCCESSFUL;
    }
    return Real_ChangeDisplaySettingsA ? Real_ChangeDisplaySettingsA(dm, flags)
                                       : DISP_CHANGE_FAILED;
}

static HWND WINAPI Hook_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR win, DWORD style,
                                        int x, int y, int w, int h,
                                        HWND par, HMENU menu, HINSTANCE inst, LPVOID param) {
    HWND r = Real_CreateWindowExA
             ? Real_CreateWindowExA(ex, cls, win, style, x, y, w, h, par, menu, inst, param)
             : NULL;
    char cbuf[80], wbuf[80];
    if ((((uintptr_t)cls) >> 16) && cls) snprintf(cbuf, sizeof cbuf, "%s", cls);
    else snprintf(cbuf, sizeof cbuf, "#atom%u", (unsigned)(uintptr_t)cls);
    if ((((uintptr_t)win) >> 16) && win) snprintf(wbuf, sizeof wbuf, "%s", win);
    else snprintf(wbuf, sizeof wbuf, "(atom/null)");
    jvs_diag("CREATEWINDOWEX cls='%s' name='%s' style=0x%08lX %dx%d -> hwnd=%08X%s",
             cbuf, wbuf, (unsigned long)style, w, h, (unsigned)(uintptr_t)r,
             r ? "  <<< WINDOW BORN" : "  <<< NULL (CreateWindowExA FAILED)");
    if (!r) jvs_diag("CREATEWINDOWEX GetLastError=%lu", (unsigned long)GetLastError());
    return r;
}

static BOOL WINAPI Hook_ExitWindowsEx(UINT flags, DWORD reason) {
    jvs_diag("EXITWINDOWSEX(flags=0x%X reason=0x%lX) INTERCEPTED -- NOT rebooting the "
             "host (arcade cabinet reboot-on-exit neutralized)",
             flags, (unsigned long)reason);
    if (examu_env_on("ULTRA_EXAMU_ALLOW_REBOOT", 0))
        return Real_ExitWindowsEx ? Real_ExitWindowsEx(flags, reason) : FALSE;
    return TRUE;   /* claim success, do nothing */
}

/* RaiseException: the game reaches its FlashRom asset loader then exits with a
 * non-standard code (0xCFFFFFFF) and NO WER dump, while the tracer's first-
 * chance VEH logs NO access violation -- i.e. it is a THROWN/RAISED exception
 * the game catches in its own SEH and turns into ExitProcess, not a pointer
 * crash. Both a C++ `throw` (code 0xE06D7363) and a bare RaiseException funnel
 * through kernel32!RaiseException, so hooking it here and scanning the caller
 * stack for return addresses inside game.exe .text names the throw site with no
 * symbol-aware stack walker. Bring-up diagnostic; pass-through (never swallows).*/
typedef VOID (WINAPI *pRaiseException)(DWORD, DWORD, DWORD, const ULONG_PTR*);
static pRaiseException Real_RaiseException = NULL;

static void examu_log_game_stack(const char* tag) {
    HMODULE gm = GetModuleHandleA(NULL);
    if (!gm) return;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)gm;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)gm + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    uintptr_t lo = (uintptr_t)gm + 0x1000;
    uintptr_t hi = (uintptr_t)gm + nt->OptionalHeader.SizeOfImage;
    uintptr_t* sp;
#if defined(__GNUC__)
    sp = (uintptr_t*)__builtin_frame_address(0);
#else
    sp = (uintptr_t*)&tag;
#endif
    int printed = 0;
    for (int i = 0; i < 400 && printed < 12; ++i) {
        uintptr_t v = sp[i];
        if (v >= lo && v < hi) {
            jvs_diag("   %s stack[%d] -> game.exe+0x%zX (static %p)",
                     tag, i, (size_t)(v - (uintptr_t)gm), (void*)v);
            ++printed;
        }
    }
    if (!printed) jvs_diag("   %s (no game.exe return address found on stack)", tag);
}

static VOID WINAPI Hook_RaiseException(DWORD code, DWORD flags, DWORD nargs,
                                       const ULONG_PTR* args) {
    /* skip the benign debugger conventions so the throw we care about stands out */
    if (code != 0x406D1388 /* SetThreadName */ && code != 0x40010006 /* ODS */) {
        jvs_diag("RAISEEXCEPTION code=0x%08lX flags=0x%lX nargs=%lu%s",
                 (unsigned long)code, (unsigned long)flags, (unsigned long)nargs,
                 code == 0xE06D7363 ? "  (C++ throw)" : "");
        examu_log_game_stack("RAISE");
    }
    if (Real_RaiseException) Real_RaiseException(code, flags, nargs, args);
}

/* ExitProcess / TerminateProcess: Phase 5 proved the fatal is NOT a throw (the
 * RaiseException hook + verbose VEH saw nothing), so the game reaches a FAILED()
 * check after loading act_00.pk3 and quits DIRECTLY -- either ExitProcess(code)
 * or a WinMain return the CRT turns into ExitProcess(code). Both funnel through
 * kernel32!ExitProcess, which game.exe imports (statically-linked CRT uses the
 * same IAT slot), so hooking it and scanning the caller stack for game.exe .text
 * return addresses names the exact site that decides to quit, regardless of
 * throw-vs-return. We log (durably) and then call the real function so the exit
 * still happens. Bring-up diagnostic. */
typedef VOID (WINAPI *pExitProcess)(UINT);
typedef BOOL (WINAPI *pTerminateProcess)(HANDLE, UINT);
static pExitProcess      Real_ExitProcess      = NULL;
static pTerminateProcess Real_TerminateProcess = NULL;

static VOID WINAPI Hook_ExitProcess(UINT code) {
    jvs_diag("EXITPROCESS code=0x%08X (%d) -- naming the game.exe site that quit:",
             (unsigned)code, (int)code);
    examu_log_game_stack("EXIT");
    if (Real_ExitProcess) Real_ExitProcess(code);
    /* Real_ExitProcess never returns; guard anyway. */
    for (;;) { }
}

static BOOL WINAPI Hook_TerminateProcess(HANDLE h, UINT code) {
    /* Only self-termination is interesting; the loader legitimately terminates
     * other handles. GetCurrentProcess() returns the pseudo-handle -1. */
    if (h == GetCurrentProcess() || h == (HANDLE)(LONG_PTR)-1) {
        jvs_diag("TERMINATEPROCESS(self) code=0x%08X (%d) -- naming the game.exe site:",
                 (unsigned)code, (int)code);
        examu_log_game_stack("TERM");
    }
    return Real_TerminateProcess ? Real_TerminateProcess(h, code) : FALSE;
}

/* -------------------------------------------------------------------------- */
/*  GetProcAddress / LoadLibrary interception                                  */
/* -------------------------------------------------------------------------- */
static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR name);
static HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR name);

static FARPROC WINAPI Hook_GetProcAddress(HMODULE mod, LPCSTR name) {
    if ((((uintptr_t)name) >> 16) != 0) {   /* by name, not ordinal */
        struct { const char* n; FARPROC f; } map[] = {
            {"CreateFileA",(FARPROC)Hook_CreateFileA},{"CreateFileW",(FARPROC)Hook_CreateFileW},
            {"DeviceIoControl",(FARPROC)Hook_DeviceIoControl},
            {"ReadFile",(FARPROC)Hook_ReadFile},{"WriteFile",(FARPROC)Hook_WriteFile},
            {"CloseHandle",(FARPROC)Hook_CloseHandle},{"SetCommState",(FARPROC)Hook_SetCommState},
            {"GetCommState",(FARPROC)Hook_GetCommState},{"SetCommTimeouts",(FARPROC)Hook_SetCommTimeouts},
            {"PurgeComm",(FARPROC)Hook_PurgeComm},{"SetupComm",(FARPROC)Hook_SetupComm},
            {"ClearCommError",(FARPROC)Hook_ClearCommError},
            {"GetCommModemStatus",(FARPROC)Hook_GetCommModemStatus},
            {"EscapeCommFunction",(FARPROC)Hook_EscapeCommFunction},
            {"SetCommMask",(FARPROC)Hook_SetCommMask},{"WaitCommEvent",(FARPROC)Hook_WaitCommEvent},
            {"GetOverlappedResult",(FARPROC)Hook_GetOverlappedResult},
            {"GetProcAddress",(FARPROC)Hook_GetProcAddress},
            {"LoadLibraryA",(FARPROC)Hook_LoadLibraryA},
            {"LoadLibraryW",(FARPROC)Hook_LoadLibraryW},
            {"ChangeDisplaySettingsA",(FARPROC)Hook_ChangeDisplaySettingsA},
            {"CreateWindowExA",(FARPROC)Hook_CreateWindowExA},
            {"ExitWindowsEx",(FARPROC)Hook_ExitWindowsEx},
            {"RaiseException",(FARPROC)Hook_RaiseException},
            {"ExitProcess",(FARPROC)Hook_ExitProcess},
            {"TerminateProcess",(FARPROC)Hook_TerminateProcess},
        };
        for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); ++i)
            if (strcmp(name, map[i].n) == 0) return map[i].f;

        /* Packed-game path: some eX-BOARD titles may resolve d3d9.dll's
         * Direct3DCreate9 dynamically instead of via a static IAT slot. Catch
         * it here too so the D3D9 present hook arms either way. */
        {
            extern void* __stdcall ultra_Hook_Direct3DCreate9_examu(UINT);
            if (strcmp(name, "Direct3DCreate9") == 0)
                return (FARPROC)ultra_Hook_Direct3DCreate9_examu;
        }
    }
    return Real_GetProcAddress(mod, name);
}

/* -------------------------------------------------------------------------- */
/*  IAT patching across all loaded modules                                     */
/* -------------------------------------------------------------------------- */
static int rva_ok(DWORD rva, DWORD isize) { return rva != 0 && rva < isize; }

static void iat_patch_module(HMODULE mod, const char* impdll,
                             const char* impfn, void* newfn, void* realfn) {
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (!ul_pe_readable(dos, sizeof *dos)) return;   /* module unloaded under us */
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (!ul_pe_readable(nt, sizeof *nt)) return;
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    DWORD isize = nt->OptionalHeader.SizeOfImage;
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!rva_ok(dir.VirtualAddress, isize)) return;
    IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    for (; rva_ok(desc->Name, isize); ++desc) {
        const char* dllname = (const char*)(base + desc->Name);
        if (_stricmp(dllname, impdll) != 0) continue;
        DWORD orig_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        IMAGE_THUNK_DATA* orig = rva_ok(orig_rva, isize) ? (IMAGE_THUNK_DATA*)(base + orig_rva) : NULL;
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);
        for (; (size_t)((BYTE*)iat - base) + sizeof(*iat) <= (size_t)isize
               && (!orig || (size_t)((BYTE*)orig - base) + sizeof(*orig) <= (size_t)isize)
               && iat->u1.Function; ++iat, orig = orig ? orig + 1 : NULL) {
            int match = 0;
            if (orig && !(orig->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                && rva_ok((DWORD)orig->u1.AddressOfData, isize)) {
                IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + orig->u1.AddressOfData);
                if (strcmp((const char*)ibn->Name, impfn) == 0) match = 1;
            }
            if (!match && realfn && (void*)(uintptr_t)iat->u1.Function == realfn) match = 1;
            if (match) {
                DWORD old;
                /* ALREADY OURS -> DO NOTHING. This early-out is not an
                 * optimization, it is a correctness fix.
                 *
                 * The re-patch worker calls this every second for 30 seconds
                 * over every loaded module. Without this check each pass
                 * re-ran VirtualProtect(PAGE_READWRITE) -> store -> restore on
                 * slots that already held our hook. VirtualProtect works at
                 * PAGE granularity and PAGE_READWRITE strips EXECUTE from the
                 * entire page, so any thread executing code sharing a page with
                 * a patched slot during that window takes a DEP fault.
                 *
                 * Measured on Mario Kart Arcade GP DX, 2026-09-10, 8 runs:
                 * with the periodic re-patch running, 3 of 4 launches died with
                 * EXECUTE/DEP at a valid .text address and every stack frame in
                 * the game; with it disabled, 0 of 4. Skipping the no-op writes
                 * removes the window while still catching a DLL the game loads
                 * late, which is the only reason the worker exists. */
                if ((void*)(uintptr_t)iat->u1.Function == newfn) continue;
                if (VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                    iat->u1.Function = (ULONG_PTR)(uintptr_t)newfn;
                    VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
                }
            }
        }
    }
}
void ultra_iat_patch_all_examu(const char* impdll, const char* impfn, void* newfn) {
    void* realfn = NULL;
    HMODULE imp = GetModuleHandleA(impdll);
    if (imp) realfn = (void*)GetProcAddress(imp, impfn);

    HMODULE mods[512]; DWORD need = 0;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ultra_iat_patch_all_examu, &self);
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        DWORD count = need / sizeof(HMODULE);
        if (count > 512) count = 512;
        for (DWORD i = 0; i < count; ++i)
            if (mods[i] != self)
                iat_patch_module(mods[i], impdll, impfn, newfn, realfn);
    }
}

static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR name) {
    return Real_LoadLibraryA(name);
}
static HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR name) {
    return Real_LoadLibraryW(name);
}

/* -------------------------------------------------------------------------- */
/*  Install                                                                     */
/* -------------------------------------------------------------------------- */
int ultra_jvs_serial_install_examu(void) {
    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = 1; }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) return 0;
    #define GET(name) (p##name)GetProcAddress(k32, #name)
    Real_CreateFileA        = GET(CreateFileA);
    Real_CreateFileW        = GET(CreateFileW);
    Real_DeviceIoControl    = GET(DeviceIoControl);
    Real_ReadFile           = GET(ReadFile);
    Real_WriteFile          = GET(WriteFile);
    Real_CloseHandle        = GET(CloseHandle);
    Real_SetCommState       = GET(SetCommState);
    Real_GetCommState       = GET(GetCommState);
    Real_SetCommTimeouts    = GET(SetCommTimeouts);
    Real_GetCommTimeouts    = GET(GetCommTimeouts);
    Real_PurgeComm          = GET(PurgeComm);
    Real_SetupComm          = GET(SetupComm);
    Real_ClearCommError     = GET(ClearCommError);
    Real_GetCommModemStatus = GET(GetCommModemStatus);
    Real_EscapeCommFunction = GET(EscapeCommFunction);
    Real_GetOverlappedResult= GET(GetOverlappedResult);
    Real_WaitCommEvent      = GET(WaitCommEvent);
    Real_SetCommMask        = GET(SetCommMask);
    Real_GetProcAddress     = GET(GetProcAddress);
    Real_LoadLibraryA       = GET(LoadLibraryA);
    Real_LoadLibraryW       = GET(LoadLibraryW);
    Real_RaiseException     = (pRaiseException)GetProcAddress(k32, "RaiseException");
    Real_ExitProcess        = (pExitProcess)GetProcAddress(k32, "ExitProcess");
    Real_TerminateProcess   = (pTerminateProcess)GetProcAddress(k32, "TerminateProcess");
    #undef GET

    struct { const char* fn; void* hook; } H[] = {
        {"CreateFileA",(void*)Hook_CreateFileA},{"CreateFileW",(void*)Hook_CreateFileW},
        {"DeviceIoControl",(void*)Hook_DeviceIoControl},
        {"ReadFile",(void*)Hook_ReadFile},{"WriteFile",(void*)Hook_WriteFile},
        {"CloseHandle",(void*)Hook_CloseHandle},{"SetCommState",(void*)Hook_SetCommState},
        {"GetCommState",(void*)Hook_GetCommState},{"SetCommTimeouts",(void*)Hook_SetCommTimeouts},
        {"PurgeComm",(void*)Hook_PurgeComm},{"SetupComm",(void*)Hook_SetupComm},
        {"GetCommTimeouts",(void*)Hook_GetCommTimeouts},
        {"ClearCommError",(void*)Hook_ClearCommError},
        {"GetCommModemStatus",(void*)Hook_GetCommModemStatus},
        {"EscapeCommFunction",(void*)Hook_EscapeCommFunction},
        {"SetCommMask",(void*)Hook_SetCommMask},{"WaitCommEvent",(void*)Hook_WaitCommEvent},
        {"GetOverlappedResult",(void*)Hook_GetOverlappedResult},
        {"GetProcAddress",(void*)Hook_GetProcAddress},
        {"LoadLibraryA",(void*)Hook_LoadLibraryA},
        {"LoadLibraryW",(void*)Hook_LoadLibraryW},
        {"RaiseException",(void*)Hook_RaiseException},
        {"ExitProcess",(void*)Hook_ExitProcess},
        {"TerminateProcess",(void*)Hook_TerminateProcess},
    };
    for (size_t i = 0; i < sizeof(H)/sizeof(H[0]); ++i)
        ultra_iat_patch_all_examu("kernel32.dll", H[i].fn, H[i].hook);

    /* Static D3D9 IAT patch: Arcana Heart family ships D3D9 (Windows PC
     * arcade board, per examu_exboard_main.c's ULTRA_D3D9=1 default). Catches
     * the common case; the GetProcAddress hook above covers a packed title
     * with no static slot. */
    {
        extern void* __stdcall ultra_Hook_Direct3DCreate9_examu(UINT);
        ultra_iat_patch_all_examu("d3d9.dll", "Direct3DCreate9", (void*)ultra_Hook_Direct3DCreate9_examu);
    }

    /* user32 display/window shims (see block above): let the game past the
     * 16bpp-fullscreen ChangeDisplaySettings TEST that modern Windows rejects,
     * and keep its reboot-on-exit from touching the host. */
    {
        HMODULE u32 = GetModuleHandleA("user32.dll");
        if (!u32) u32 = LoadLibraryA("user32.dll");
        if (u32) {
            Real_ChangeDisplaySettingsA = (pChangeDisplaySettingsA)GetProcAddress(u32, "ChangeDisplaySettingsA");
            Real_CreateWindowExA        = (pCreateWindowExA)GetProcAddress(u32, "CreateWindowExA");
            Real_ExitWindowsEx          = (pExitWindowsEx)GetProcAddress(u32, "ExitWindowsEx");
            ultra_iat_patch_all_examu("user32.dll", "ChangeDisplaySettingsA", (void*)Hook_ChangeDisplaySettingsA);
            ultra_iat_patch_all_examu("user32.dll", "CreateWindowExA",        (void*)Hook_CreateWindowExA);
            ultra_iat_patch_all_examu("user32.dll", "ExitWindowsEx",          (void*)Hook_ExitWindowsEx);
        }
    }

    jvs_diag("eX-BOARD hook: kernel32 IAT patched, JVS-over-serial (COM2/COM3 default) armed; "
             "user32 display/window shims armed (CDS fake=%d, reboot-on-exit blocked=%d)",
             examu_env_on("ULTRA_EXAMU_CDS_FAKE", 1),
             !examu_env_on("ULTRA_EXAMU_ALLOW_REBOOT", 0));
    return 1;
}

#endif /* _WIN32 */
