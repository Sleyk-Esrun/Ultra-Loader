/* SPDX-License-Identifier: MIT
 * NOTE: independent namco_es3 board copy -- adapted from native_x86/src/hook/jvs_serial_hook.c (reference only; no build-time or runtime dependency on native_x86 remains).
 * =============================================================================
 *  jvs_serial_hook_es3.c  --  Win32 COM-port interception feeding the JVS engine
 * =============================================================================
 *
 *  Type X / Sega Nu / Namco ES boards talk to their JVS I/O board over a serial
 *  port (RS-485 on COM2/COM3). This intercepts the game's serial API so that,
 *  instead of a real port, the game's command bytes flow into Ultra Loader's
 *  clean-room JVS engine (jvs_core.c) and the engine's replies flow back -- with
 *  controller state sourced from the loader via shared memory.
 *
 *  Mechanism: IAT patching (no inline trampolines, no third-party hooking lib).
 *  We point every loaded module's import-address-table slot for the relevant
 *  kernel32 functions at our shims, and intercept GetProcAddress so late/dynamic
 *  resolution is caught too. Original pointers come straight from kernel32, so
 *  pass-through for non-COM handles is exact.
 *
 *  Clean-room: this is Ultra Loader's own code, built on the public Win32 API and
 *  the open JVS standard. No third-party loader source.
 * =============================================================================
 */
#if defined(_WIN32)

/* winsock2.h must come before windows.h, and _WIN32_WINNT must be at least
 * 0x0501, or iphlpapi.h will not declare IP_ADAPTER_ADDRESSES. These are here
 * for the adapter STRUCT LAYOUTS only - see the offsets below. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stddef.h>
#include <psapi.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#include "jvs_core.h"
#include "board_io_ops.h"

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

/* Optional diagnostic trace (set ULTRA_JVS_LOG=<file>). Logs COM opens + each
 * JVS exchange so a test run shows exactly how far the handshake gets -- e.g.
 * whether a game reaches its security check after a clean JVS handshake. */
/* THREAD SAFETY IS NOT OPTIONAL HERE, AND THIS USED TO CRASH GAMES.
 *
 * The module sets ULTRA_JVS_LOG on every launch, and Hook_CreateFileA/W call
 * this for EVERY file the game opens. A modern title streams its assets from
 * several worker threads at once, so this function runs concurrently at very
 * high rates -- and the old body did an unsynchronised fopen/vfprintf/fclose
 * per line, with a "static int checked" one-time init that has no barrier at
 * all (one thread could set checked=1 before path was stored, and another
 * would then read an unpublished path).
 *
 * Measured on Mario Kart Arcade GP DX, 2026-09-10: roughly one launch in two
 * died with ACCESS_VIOLATION inside MSVCR100.dll at a fixed address, during
 * the sound/asset load burst. A/B over four runs with an unrelated feature
 * toggled showed the crash tracked nothing but timing.
 *
 * One critical section, one properly-published init. The lock is held only
 * around the file work, so a thread that finds logging disabled never blocks. */
static CRITICAL_SECTION g_diag_cs;
static volatile LONG    g_diag_state;      /* 0 = untouched, 1 = init, 2 = ready */
static const char      *g_diag_path;

static void jvs_diag_init_once(void) {
    if (InterlockedCompareExchange(&g_diag_state, 2, 2) == 2) return;
    if (InterlockedCompareExchange(&g_diag_state, 1, 0) == 0) {
        InitializeCriticalSection(&g_diag_cs);
        g_diag_path = getenv("ULTRA_JVS_LOG");
        InterlockedExchange(&g_diag_state, 2);   /* publishes g_diag_path */
        return;
    }
    /* Another thread is initializing. Spin briefly rather than take a lock we
     * may not have constructed yet; this window is a few instructions long. */
    while (InterlockedCompareExchange(&g_diag_state, 2, 2) != 2)
        Sleep(0);
}

static void jvs_diag(const char* fmt, ...) {
    FILE* f;
    va_list ap;
    jvs_diag_init_once();
    if (!g_diag_path || !*g_diag_path) return;
    EnterCriticalSection(&g_diag_cs);
    f = fopen(g_diag_path, "a");
    if (f) {
        va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f); fclose(f);
    }
    LeaveCriticalSection(&g_diag_cs);
}
static void jvs_diag_hex(const char* tag, const unsigned char* b, size_t n) {
    /* Was getenv() on EVERY hex line -- see jvs_diag_init_once(). */
    jvs_diag_init_once();
    if (!g_diag_path || !*g_diag_path) return;
    char line[512]; size_t o = 0;
    o += (size_t)snprintf(line, sizeof(line), "%s [%zu]:", tag, n);
    for (size_t i = 0; i < n && i < 96 && o + 4 < sizeof(line); ++i)
        o += (size_t)snprintf(line + o, sizeof(line) - o, " %02X", b[i]);
    jvs_diag("%s", line);
}

/* Provided by hook_dllmain.c */
ultra_jvs_node* ultra_hook_node(void);
void            ultra_hook_pull_input(ultra_jvs_node* n);

/* -------------------------------------------------------------------------- */
/*  Fake COM handle table (one JVS bus; small table covers re-opens)           */
/* -------------------------------------------------------------------------- */
typedef struct {
    HANDLE   h;
    int      port;          /* COM port number this slot stands in for */
    DWORD    evtmask;       /* last SetCommMask value (for WaitCommEvent) */
    uint8_t  resp[4096];
    size_t   resp_pos, resp_len;
} com_slot;

#define COM_SLOTS 8
static com_slot       g_slots[COM_SLOTS];
static int            g_slot_used[COM_SLOTS];
static CRITICAL_SECTION g_lock;
static int            g_lock_init = 0;

static com_slot* slot_for(HANDLE h) {
    for (int i = 0; i < COM_SLOTS; ++i)
        if (g_slot_used[i] && g_slots[i].h == h) return &g_slots[i];
    return NULL;
}
static com_slot* slot_alloc(HANDLE h, int port) {
    for (int i = 0; i < COM_SLOTS; ++i)
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
    for (int i = 0; i < COM_SLOTS; ++i)
        if (g_slot_used[i] && g_slots[i].h == h) { g_slot_used[i] = 0; g_slots[i].h = NULL; }
}

/* -------------------------------------------------------------------------- */
/*  Which COM ports we claim (default COM2+COM3; env ULTRA_JVS_COM overrides)   */
/* -------------------------------------------------------------------------- */
static int port_number_from_path(const char* p) {
    /* find "COM" (case-insensitive) followed by digits; return the number or -1 */
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
/*  Originals (resolved from kernel32) + typedefs                              */
/* -------------------------------------------------------------------------- */
typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
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
typedef BOOL   (WINAPI *pDeviceIoControl)(HANDLE,DWORD,LPVOID,DWORD,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef FARPROC(WINAPI *pGetProcAddress)(HMODULE,LPCSTR);
typedef HMODULE(WINAPI *pLoadLibraryA)(LPCSTR);
typedef HMODULE(WINAPI *pLoadLibraryW)(LPCWSTR);

static pCreateFileA        Real_CreateFileA;
static pCreateFileW        Real_CreateFileW;
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
static pDeviceIoControl    Real_DeviceIoControl;
static pGetProcAddress     Real_GetProcAddress;
static pLoadLibraryA       Real_LoadLibraryA;
static pLoadLibraryW       Real_LoadLibraryW;

/* --- early-init diagnostics (ULTRA_JVS_LOG) -------------------------------- */
typedef void (WINAPI *pExitProcess)(UINT);
typedef void* (WINAPI *pD3DCreate8)(UINT);
static pExitProcess Real_ExitProcess;
static pD3DCreate8  Real_Direct3DCreate8;

/* -------------------------------------------------------------------------- */
/*  The shims                                                                   */
/* -------------------------------------------------------------------------- */
/* A fake COM handle is a real, auto-reset Event the game can wait on, close, and
 * that we signal on I/O completion (overlapped serial code waits on it). */
/* non-static: game_dsii.c's cave_direct_write_thread() force-init path calls
 * this directly so a forced handle lands in THIS file's shared g_slots table
 * (open_fake_com's only job), keeping it recognized by every generic
 * slot_for(h) check here -- a per-game file cannot safely duplicate this
 * table without fragmenting it and losing those other generic behaviors for
 * the forced handle. */
HANDLE open_fake_com(int port) {
    HANDLE h = CreateEventA(NULL, FALSE /*auto-reset*/, FALSE, NULL);
    if (!h) return INVALID_HANDLE_VALUE;
    EnterCriticalSection(&g_lock);
    com_slot* s = slot_alloc(h, port);
    LeaveCriticalSection(&g_lock);
    if (!s) {                       /* table full: fail deterministically, no zombie */
        Real_CloseHandle ? Real_CloseHandle(h) : (void)CloseHandle(h);
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return INVALID_HANDLE_VALUE;
    }
    jvs_diag("OPEN COM%d -> JVS fake handle (slot ok)", port);
    return h;
}

/* Signal completion for a synchronous-but-overlapped call: fill the OVERLAPPED
 * status and wake whatever the game waits on (its own hEvent, else the handle). */
static void complete_overlapped(HANDLE h, LPOVERLAPPED ov, DWORD bytes) {
    if (!ov) return;
    ov->Internal = 0;               /* STATUS_SUCCESS */
    ov->InternalHigh = bytes;
    SetEvent(ov->hEvent ? ov->hEvent : h);
}

/* Create every parent directory in a path (so a redirected write can be created). */
static void ensure_parent_dirs(const char* path) {
    char tmp[1024]; strncpy(tmp, path, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
    for (char* p = tmp + 1; *p; ++p)
        if (*p == '\\' || *p == '/') { char c = *p; *p = 0; CreateDirectoryA(tmp, NULL); *p = c; }
}
/* Virtual D: drive -- arcade boards keep a writable data/backup partition on D:
 * (d:\game.dat, d:\history.dat ...). On a PC there is no such drive, so redirect
 * "d:\X" to a local writable folder (ULTRA_DDRIVE, default ".\ul_vdrive_d"). */
static const char* redirect_d_drive(const char* name, char* buf, size_t cap) {
    if (name && (name[0]=='d'||name[0]=='D') && name[1]==':' && (name[2]=='\\'||name[2]=='/')) {
        const char* base = getenv("ULTRA_DDRIVE"); if (!base || !*base) base = "ul_vdrive_d";
        snprintf(buf, cap, "%s%s", base, name + 2);   /* base + "\game.dat" */
        ensure_parent_dirs(buf);
        jvs_diag("D: redirect %s -> %s", name, buf);
        return buf;
    }
    return name;
}

static int is_dongle_device(const char* name) {
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

/* Namco ES3 USB dongle bypass (namco_dongle.c) */
extern FARPROC ultra_namco_dongle_getproc(HMODULE mod, LPCSTR name);
extern int     ultra_namco_dongle_is_device_path(const char* path);
extern HANDLE  ultra_namco_dongle_open_device(void);
extern int     ultra_namco_dongle_is_handle(HANDLE h);
extern BOOL    ultra_namco_dongle_ioctl(HANDLE h, DWORD code, LPVOID in_buf, DWORD in_sz,
                   LPVOID out_buf, DWORD out_sz, LPDWORD bytes_ret, LPOVERLAPPED ov);


/* ---------------------------------------------------------------------------
 *  PASSIVE COM TAP  --  observe a port we deliberately did NOT take over
 *
 *  Mario Kart GP DX drives a second serial device on COM1: the STR PCB, its
 *  steering / force-feedback board. We must NOT stand in for it -- measured
 *  2026-09-10, replacing COM1 with a JVS node makes the game fail EARLIER than
 *  leaving it alone, because our fake WaitCommEvent returns immediately with no
 *  event where a real port with nothing attached blocks until its timeout, so
 *  the game's steering loop spins and gives up before it ever opens COM3.
 *
 *  This tap is therefore an OBSERVER and nothing else: the real CreateFile
 *  happens, the real handle is returned to the game, every read and write is
 *  forwarded to the real API unchanged, and we only copy the bytes into the
 *  trace on the way past. It cannot alter timing beyond the cost of a log line,
 *  and it is the only way to learn a protocol we do not yet speak.
 *
 *  Enabled with ULTRA_JVS_TAP=1 (off by default -- it is a diagnostic).
 * ------------------------------------------------------------------------- */
#define COM_TAPS 8
static HANDLE g_tap_h[COM_TAPS];
static int    g_tap_port[COM_TAPS];

static int com_tap_enabled(void) {
    static int checked, on;
    if (!checked) {
        const char* e = getenv("ULTRA_JVS_TAP");
        char buf[8] = {0};
        if (!e && GetEnvironmentVariableA("ULTRA_JVS_TAP", buf, sizeof buf) > 0) e = buf;
        on = (e && e[0] == '1');
        checked = 1;
    }
    return on;
}
static void com_tap_add(HANDLE h, int port) {
    int i;
    if (!com_tap_enabled() || h == INVALID_HANDLE_VALUE || port < 0) return;
    for (i = 0; i < COM_TAPS; ++i)
        if (!g_tap_h[i]) { g_tap_h[i] = h; g_tap_port[i] = port;
                           jvs_diag("TAP: observing real COM%d (handle %p)", port, (void*)h);
                           return; }
}
static int com_tap_port_of(HANDLE h) {
    int i;
    if (!h) return -1;
    for (i = 0; i < COM_TAPS; ++i) if (g_tap_h[i] == h) return g_tap_port[i];
    return -1;
}
static void com_tap_remove(HANDLE h) {
    int i;
    for (i = 0; i < COM_TAPS; ++i) if (g_tap_h[i] == h) { g_tap_h[i] = NULL; return; }
}

/* ---------------------------------------------------------------------------
 *  CABINET DATA DRIVE REDIRECT  (F: -> the game folder)
 * ---------------------------------------------------------------------------
 * A real ES3 cabinet has a writable data partition mounted at F: and the game
 * writes its bookkeeping there:
 *
 *     F:/Bookkeeping/bookkeeping_<unix>.bin
 *     F:/Bookkeeping/bookkeeping_<unix>.bin.tmp
 *
 * On a normal PC F: is somebody's own drive. On this machine it holds the
 * user's personal folders, and the game was creating files next to them.
 *
 * A game has no business writing outside its own folder, so every absolute
 * path on the cabinet's data drive is rewritten to sit under the game
 * directory instead. This is also why no USB dongle emulation is needed for
 * it: the dongle is READ-only identity (PreprocessAmAuth.exe turns it into
 * netID/serialID, which the loader now writes directly); the F: writes are
 * just file I/O, and file I/O is something we already intercept here.
 *
 * ULTRA_ES3_DATA_DIR overrides the destination; ULTRA_ES3_REDIRECT_F=0 turns
 * the whole thing off. */
static int ultra_redirect_cabinet_path(const char* in, char* out, size_t cap) {
    const char* e;
    static char base[MAX_PATH];
    static int  base_ready = 0;

    if (!in || !out) return 0;
    /* Any absolute path on a nominated cabinet drive - NOT just F:. This
     * machine has personal data on BOTH E: and F:, and other boards mount
     * their writable partition elsewhere again. 92 = backslash, 47 = slash. */
    if (in[1] != 58 || (in[2] != 92 && in[2] != 47)) return 0;
    {   const char* dl = getenv("ULTRA_CABINET_DRIVES");
        char c = (char)toupper((unsigned char)in[0]);
        const char* q; int hit = 0; char self[MAX_PATH];
        if (!dl || !*dl) dl = "DEFGHJ";   /* the cabinet init.ps1 lists D E F G H J */
        for (q = dl; *q; ++q) if ((char)toupper((unsigned char)*q) == c) { hit = 1; break; }
        if (!hit) return 0;
        self[0] = 0;
        GetModuleFileNameA(NULL, self, (DWORD)sizeof self);
        if (self[0] && (char)toupper((unsigned char)self[0]) == c) return 0;
    }

    e = getenv("ULTRA_REDIRECT_CABINET");
    if (e && e[0] == '0') return 0;

    if (!base_ready) {
        /* The loader resolves ONE writable folder per game and publishes it
         * as ULTRA_GAMEDATA (see work/include/ultra_gamedata.h). Using it
         * keeps the game dump read-only and puts every game's state in one
         * place. Fall back beside the exe only if the loader did not set it,
         * e.g. when the hook is injected by something else. */
        e = getenv("ULTRA_GAMEDATA");
        if (!e || !*e) e = getenv("ULTRA_ES3_DATA_DIR");
        if (e && *e) {
            _snprintf(base, sizeof base, "%s", e);
        } else {
            char exe[MAX_PATH]; char* slash;
            GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
            slash = strrchr(exe, 92);
            if (slash) *slash = 0;
            _snprintf(base, sizeof base, "%s%cultra_gamedata", exe, 92);
        }
        base[sizeof base - 1] = 0;
        CreateDirectoryA(base, NULL);
        base_ready = 1;
    }

    _snprintf(out, cap, "%s%c%s", base, 92, in + 3);
    out[cap - 1] = 0;
    /* Create the intermediate directory so the game's fopen succeeds first
     * time - it does not expect to have to mkdir on a cabinet. */
    {   char* p = out;
        for (; *p; ++p) {
            if ((*p == 92 || *p == 47) && p > out + strlen(base)) {
                char c = *p; *p = 0; CreateDirectoryA(out, NULL); *p = c;
            }
        }
    }
    return 1;
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD acc, DWORD share,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    char rbuf[1024];
    const char* rn = name;
    if (name) {
        jvs_diag("CreateFileA: %s", name);
        /* LANGUAGE FALLBACK: a Japanese-only dump asked for English text.
         *
         * Pokken builds its text path from "/%%TEXT%%/resident_ntx_%s" and in
         * this dump the %s resolves to "en", but text/master/ holds only _ja
         * files - Battle_ja, System_ja, Title_ja, resident_ntx_ja. The open
         * fails and the game has no strings to render.
         *
         * Serving the Japanese file for the English request is honest: the
         * Japanese text is the only text that exists, and a missing file is
         * strictly worse than text in the wrong language. It substitutes ONLY
         * when the requested file is genuinely absent and the fallback is
         * genuinely present, so a real English dump is never touched.
         *
         * ULTRA_NAMCO_TEXT_FALLBACK=0 disables. */
        {
            static int nsub = 0;
            size_t L = strlen(name);
            if (L > 7 && L < sizeof rbuf &&
                _stricmp(name + L - 7, "_en.drp") == 0 &&
                GetFileAttributesA(name) == INVALID_FILE_ATTRIBUTES) {
                char envv[8];
                DWORD ev = GetEnvironmentVariableA("ULTRA_NAMCO_TEXT_FALLBACK",
                                                   envv, sizeof envv);
                /* FIRST: a text bank the LOADER supplies, never the game folder.
                 *
                 * This dump is a North American build - the version string on
                 * its own SYSTEM CHECK screen reads PKF200-1-NA-MPRO-A - and it
                 * asks for *_en.drp accordingly. Not one English bank shipped
                 * with it: text/master holds 19 files and every one is _ja. So
                 * the beginner prompt draws its Yes/No buttons with no question
                 * above them, and the operator dialog shows the raw key
                 * ERROR_MSG_001, because the game looks up English strings in a
                 * table that has none.
                 *
                 * The Japanese fallback below keeps the game running but does
                 * not fix that, so the real answer is to GENERATE the English
                 * banks and serve them from here. They live under the loader's
                 * own gamedata folder:
                 *
                 *     %ULTRA_GAMEDATA%\text\resident_ntx_en.drp
                 *
                 * Nothing is ever written into the game folder - the dump stays
                 * byte-identical and read-only, which is the whole point.
                 *
                 *     ULTRA_NAMCO_TEXT_DIR=<dir>   override the location */
                if (!(ev && ev < sizeof envv && envv[0] == '0')) {
                    char tdir[MAX_PATH];
                    if (!GetEnvironmentVariableA("ULTRA_NAMCO_TEXT_DIR", tdir, sizeof tdir)
                        || !tdir[0]) {
                        if (GetEnvironmentVariableA("ULTRA_GAMEDATA", tdir, sizeof tdir) && tdir[0]) {
                            size_t t = strlen(tdir);
                            _snprintf(tdir + t, sizeof tdir - t, "%ctext", 92); /* 92 = backslash */
                            tdir[sizeof tdir - 1] = 0;
                        } else tdir[0] = 0;
                    }
                    if (tdir[0]) {
                        const char* leaf = name; const char* q;
                        for (q = name; *q; q++) if (*q == 92 || *q == '/') leaf = q + 1;
                        _snprintf(rbuf, sizeof rbuf, "%s%c%s", tdir, 92, leaf);
                        rbuf[sizeof rbuf - 1] = 0;
                        if (GetFileAttributesA(rbuf) != INVALID_FILE_ATTRIBUTES) {
                            if (nsub < 6) { ++nsub;
                                jvs_diag("TEXT: loader-supplied bank %s -> %s", leaf, rbuf); }
                            return Real_CreateFileA(rbuf, acc, share, sa, disp, flags, tmpl);
                        }
                    }
                }
                if (!(ev && ev < sizeof envv && envv[0] == '0')) {
                    memcpy(rbuf, name, L + 1);
                    rbuf[L - 6] = 'j'; rbuf[L - 5] = 'a';
                    if (GetFileAttributesA(rbuf) != INVALID_FILE_ATTRIBUTES) {
                        if (nsub < 6) { ++nsub;
                            jvs_diag("TEXT FALLBACK: %s absent -> serving %s",
                                     name, rbuf); }
                        return Real_CreateFileA(rbuf, acc, share, sa, disp, flags, tmpl);
                    }
                }
            }
        }
        /* Keep the game's writes inside its own folder - see above. */
        if (ultra_redirect_cabinet_path(name, rbuf, sizeof rbuf)) {
            static int said = 0;
            if (said < 4) { jvs_diag("REDIRECT: %s -> %s", name, rbuf); said++; }
            return Real_CreateFileA(rbuf, acc, share, sa, disp, flags, tmpl);
        }
        /* A registered per-game module gets first refusal on a path it cares
         * about (e.g. Cave PC/DSII's CVKEY.222) -- game-specific always wins
         * over the generic fake-COM/dongle/D-drive guesses below. */
        { const board_io_ops* ops = ultra_board_ops_get();
          if (ops && ops->on_create_file) {
              HANDLE oh;
              if (ops->on_create_file(name, &oh)) return oh;
          } }
        int p = port_number_from_path(name);
        if (we_claim_port(p)) return open_fake_com(p);
        if (p >= 0 && com_tap_enabled()) {
            /* A COM port we chose not to take over: hand back the REAL handle
               and merely watch what crosses it. */
            HANDLE rh = Real_CreateFileA(name, acc, share, sa, disp, flags, tmpl);
            if (rh != INVALID_HANDLE_VALUE) com_tap_add(rh, p);
            return rh;
        }
        if (is_dongle_device(name)) {
            const char* e = getenv("ULTRA_DONGLE_BYPASS");
            if (e && e[0]=='1') {
                HANDLE fk = CreateEventA(NULL, TRUE, FALSE, NULL);
                jvs_diag("  -> dongle device '%s' bypassed (fake handle=%p)", name, (void*)fk);
                return fk ? fk : (HANDLE)(uintptr_t)0xDEAD0001;
            }
        }
        if (ultra_namco_dongle_is_device_path(name))
            return ultra_namco_dongle_open_device();
        rn = redirect_d_drive(name, rbuf, sizeof rbuf);   /* d:\ -> local folder */
    }
    HANDLE h = Real_CreateFileA(rn, acc, share, sa, disp, flags, tmpl);
    if (h == INVALID_HANDLE_VALUE && rn) {
        /* Flattened-dump path fixup: arcade games built for a deeper directory
         * tree (e.g. bin/x86/) use ../../Data/ relative paths. When the dump
         * puts the exe at the root, strip leading ../ segments and retry. */
        const char *p = rn;
        while (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\\'))
            p += 3;
        if (p != rn) {
            HANDLE h2 = Real_CreateFileA(p, acc, share, sa, disp, flags, tmpl);
            if (h2 != INVALID_HANDLE_VALUE) {
                jvs_diag("  -> fixup '%s' -> '%s'", rn, p);
                return h2;
            }
        }
        jvs_diag("  -> FAILED (err %lu)", (unsigned long)GetLastError());
    }
    return h;
}
/* ---------------------------------------------------------------------------
 *  ENGLISH TEXT REDIRECT  (Mario Kart GP DX and friends)
 * ---------------------------------------------------------------------------
 * The dump ships English assets but the build is JP, so every path is
 * "Data/TxtMessage/data_jp/...". Patching that string in .rdata would be one
 * byte pair - and would BREAK THE GAME, because the English set is INCOMPLETE:
 *
 *     TxtMessage/data_jp   66 files    TxtMessage/data_en   29 files
 *     flash/data_jp       484 files    flash/data_en         0 files  (EMPTY)
 *
 * data_en has all the text banks (title, chara, kart, item, name entry, test
 * mode, error list) but NOT the Japanese glyph pages font_02 and font_04 that the
 * game actually loads, nor race_tips/template. flash/data_en does not exist at
 * all, so redirecting that would lose every menu graphic.
 *
 * So redirect PER FILE, and only when the English file is really there:
 * swap "data_jp" -> "data_en", test it, and fall back to the original path
 * otherwise. Nothing in the game folder is modified, missing English assets
 * degrade to Japanese instead of failing, and "flash" is never touched.
 *
 * ULTRA_NAMCO_ENGLISH=0 disables; default ON. */
static int ultra_en_redirect(LPCWSTR in, wchar_t* out, size_t cap) {
    static int on = -1;
    const wchar_t* jp;
    size_t pre;
    DWORD at;

    if (on < 0) {
        /* Driven by the loader's "English (If game supports it)" checkbox,
         * which sets this explicitly to "1" or "0". Default when the variable
         * is absent entirely (hook run outside the loader) is OFF, so the
         * game's shipped language is never changed behind the user's back. */
        char b[8]; b[0] = 0;
        GetEnvironmentVariableA("ULTRA_NAMCO_ENGLISH", b, sizeof b);
        on = (b[0] == '1');
    }
    if (!on || !in) return 0;

    /* Only the text-message tree. "flash/data_en" is empty in every dump seen,
     * so touching it would lose the menus entirely. */
    jp = wcsstr(in, L"TxtMessage/data_jp");
    if (!jp) jp = wcsstr(in, L"TxtMessage" L"\\" L"data_jp");
    if (!jp) return 0;

    /* TEXT BANKS ONLY - never the fonts. data_en ships its own font_01 with
     * different glyph metrics, and redirecting those made the boot screen
     * render in oversized characters. The .txbn banks are the actual strings;
     * the fonts must stay the Japanese ones the game was laid out against. */
    if (wcsstr(in, L"/font/") || wcsstr(in, L"\font\\")) return 0;

    pre = (size_t)(jp - in) + 11;      /* keep "TxtMessage/" or "TxtMessage\" */
    if (pre + 8 >= cap) return 0;
    wcsncpy(out, in, pre);
    out[pre] = 0;
    wcscat(out, L"data_en");
    wcscat(out, jp + 11 + 7);          /* the remainder after "data_jp" */

    at = GetFileAttributesW(out);
    if (at == INVALID_FILE_ATTRIBUTES) return 0;       /* no English copy */
    if (at & FILE_ATTRIBUTE_DIRECTORY) return 0;
    return 1;
}

static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD share,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (name) {
        char a[256]; int n = WideCharToMultiByte(CP_ACP, 0, name, -1, a, sizeof(a)-1, NULL, NULL);
        if (n > 0) { a[n] = 0; jvs_diag("CreateFileW: %s", a);
                     { const board_io_ops* ops = ultra_board_ops_get();
                       if (ops && ops->on_create_file) {
                           HANDLE oh;
                           if (ops->on_create_file(a, &oh)) return oh;
                       } }
                     int p = port_number_from_path(a); if (we_claim_port(p)) return open_fake_com(p);
                     if (ultra_namco_dongle_is_device_path(a))
                         return ultra_namco_dongle_open_device(); }
    }
    {   wchar_t en[512];
        if (ultra_en_redirect(name, en, 512)) {
            static int said = 0;
            if (!said) { said = 1; jvs_diag("TEXT: English redirect ACTIVE (data_jp -> data_en, per file, jp fallback)"); }
            return Real_CreateFileW(en, acc, share, sa, disp, flags, tmpl);
        }
    }
    return Real_CreateFileW(name, acc, share, sa, disp, flags, tmpl);
}

static BOOL WINAPI Hook_DeviceIoControl(HANDLE dev, DWORD code,
        LPVOID in_buf, DWORD in_sz, LPVOID out_buf, DWORD out_sz,
        LPDWORD bytes_ret, LPOVERLAPPED ov) {
    if (ultra_namco_dongle_is_handle(dev))
        return ultra_namco_dongle_ioctl(dev, code, in_buf, in_sz,
                                        out_buf, out_sz, bytes_ret, ov);
    /* OUR FAKE COM HANDLE IS AN EVENT, NOT A DEVICE.
     * Passing it to the real DeviceIoControl can only fail (invalid handle /
     * invalid function), and until now that happened SILENTLY -- no trace line
     * at all -- so a game driving its board through IOCTL_SERIAL_* instead of
     * ReadFile/WriteFile looked, in the log, exactly like a game that never
     * touched the port. Log it, loudly, before deciding how to answer. */
    {
        EnterCriticalSection(&g_lock);
        int ours = slot_for(dev) != NULL;
        LeaveCriticalSection(&g_lock);
        if (ours) {
            jvs_diag("DeviceIoControl(COM slot) code=0x%08lX in=%lu out=%lu",
                     (unsigned long)code, (unsigned long)in_sz, (unsigned long)out_sz);
            if (in_buf && in_sz)
                jvs_diag_hex("  ioctl-in", (const unsigned char*)in_buf,
                             in_sz > 64 ? 64 : in_sz);
            /* Behavior deliberately unchanged for this diagnostic pass: report
             * what the game asks for first, then answer it correctly. */
        }
    }
    return Real_DeviceIoControl(dev, code, in_buf, in_sz,
                                out_buf, out_sz, bytes_ret, ov);
}

/* Diagnostics: where does the game die, and does D3D8 init succeed? */
static void WINAPI Hook_ExitProcess(UINT code) {
    /* Name the CALLER, not just the code.
     *
     * A clean exit with code 0 is the hardest kind of quit to diagnose: no
     * fault, no error text, and every log line above it looks healthy. Pokken
     * renders at 60fps, answers JVS, and then simply leaves. The return address
     * is what turns that into an address to disassemble. */
    HMODULE m = NULL;
    char path[MAX_PATH];
    void* ra = __builtin_return_address(0);
    const char* leaf = "?";
    unsigned long long off = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)ra, &m) && m &&
        GetModuleFileNameA(m, path, sizeof path)) {
        const char* p = strrchr(path, 92);   /* 92 = backslash, as a code */
        leaf = p ? p + 1 : path;
        off  = (unsigned long long)((char*)ra - (char*)m);
    }
    jvs_diag("EXITPROCESS code=%u  called from %s+0x%llX (abs %p)",
             code, leaf, off, ra);
    Real_ExitProcess(code);
}
static void* WINAPI Hook_Direct3DCreate8(UINT ver) {
    if (!Real_Direct3DCreate8) {
        HMODULE d = GetModuleHandleA("d3d8.dll");
        if (d) Real_Direct3DCreate8 = (pD3DCreate8)(void*)GetProcAddress(d, "Direct3DCreate8");
    }
    void* r = Real_Direct3DCreate8 ? Real_Direct3DCreate8(ver) : NULL;
    jvs_diag("Direct3DCreate8(0x%X) -> %p %s", ver, r, r ? "OK" : "NULL(D3D8 init failed)");
    /* Install the frame limiter: hook CreateDevice->Present to cap to 60fps so the
     * game doesn't run fast-forwarded (windowed D3D8 can't vsync). */
    extern void ultra_d3d8_install_throttle(void* d3d8);
    if (r) ultra_d3d8_install_throttle(r);
    return r;
}

static BOOL WINAPI Hook_WriteFile(HANDLE h, LPCVOID buf, DWORD len,
                                  LPDWORD written, LPOVERLAPPED ov) {
    EnterCriticalSection(&g_lock);
    com_slot* s = slot_for(h);
    if (s) {
        ultra_jvs_node* node = ultra_hook_node();
        ultra_hook_pull_input(node);                       /* latest controller state */
        /* compact any already-consumed response bytes */
        if (s->resp_pos) {
            memmove(s->resp, s->resp + s->resp_pos, s->resp_len - s->resp_pos);
            s->resp_len -= s->resp_pos; s->resp_pos = 0;
        }
        /* A registered per-game module's write handler takes priority (e.g. Cave
         * PC/DSII's command-byte protocol on COM3). If it fully handles the
         * write (remembers whatever state its own on_read_file needs to answer
         * next), we're done -- no generic jvs_host_bytes framing for this game. */
        const board_io_ops* ops = ultra_board_ops_get();
        if (ops && ops->on_write_file && ops->on_write_file(h, buf, len)) {
            { char tg[16]; snprintf(tg, sizeof tg, "TX COM%d", s->port);
              jvs_diag_hex(tg, (const unsigned char*)buf, len); }
            LeaveCriticalSection(&g_lock);
            if (written) *written = len;
            complete_overlapped(h, ov, len);
            return TRUE;
        }
        uint8_t tmp[2048];
        size_t got = jvs_host_bytes(node, (const uint8_t*)buf, len, tmp, sizeof(tmp));
        size_t room = sizeof(s->resp) - s->resp_len;
        if (got > room) got = room;
        memcpy(s->resp + s->resp_len, tmp, got);
        s->resp_len += got;
        { char tg[16];
          snprintf(tg, sizeof tg, "TX COM%d", s->port); jvs_diag_hex(tg, (const unsigned char*)buf, len);
          snprintf(tg, sizeof tg, "RX COM%d", s->port); jvs_diag_hex(tg, tmp, got); }
        LeaveCriticalSection(&g_lock);
        if (written) *written = len;                       /* game wrote all of it */
        complete_overlapped(h, ov, len);                   /* signal async waiters */
        return TRUE;
    }
    LeaveCriticalSection(&g_lock);
    {   int tp = com_tap_port_of(h);
        if (tp >= 0 && buf && len) {
            char tg[24]; snprintf(tg, sizeof tg, "TAP-TX COM%d", tp);
            jvs_diag_hex(tg, (const unsigned char*)buf, len > 64 ? 64 : len);
        } }
    return Real_WriteFile(h, buf, len, written, ov);
}

static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD len,
                                 LPDWORD read, LPOVERLAPPED ov) {
    /* A registered per-game module gets first refusal on a handle it owns that
     * ISN'T one of our generic fake-COM com_slots (e.g. Cave PC/DSII's CVKEY.222
     * synthetic file handle). It must internally recognize its own handle(s) and
     * return 0 for anything else so we fall through to the generic path below. */
    { const board_io_ops* ops = ultra_board_ops_get();
      if (ops && ops->on_read_file) {
          DWORD got = 0;
          if (ops->on_read_file(h, buf, len, &got)) {
              if (read) *read = got;
              if (ov) complete_overlapped(h, ov, got);
              return TRUE;
          }
      }
    }
    EnterCriticalSection(&g_lock);
    com_slot* s = slot_for(h);
    if (s) {
        size_t avail = s->resp_len - s->resp_pos;
        if (avail) {
            /* Return queued data from a prior WriteFile exchange first. */
            size_t give = (len < avail) ? len : avail;
            memcpy(buf, s->resp + s->resp_pos, give); s->resp_pos += give;
            jvs_diag("ReadFile COM%d req=%lu give=%zu (queued=%zu)", s->port, (unsigned long)len, give, avail);
            LeaveCriticalSection(&g_lock);
            if (read) *read = (DWORD)give;
            complete_overlapped(h, ov, (DWORD)give);
            return TRUE;
        }
        /* Push-mode board (e.g. Cave PC/DSII on COM3): after the handshake the
         * game reads without writing. A registered ops' on_read_file recognizes
         * its own com_slot handle here (the SAME callback used above for a
         * non-com_slot handle like CVKEY.222 -- it must check `h` itself) and
         * generates a fresh frame from live input on every call. */
        { const board_io_ops* ops2 = ultra_board_ops_get();
          if (ops2 && ops2->on_read_file && len >= 1) {
              DWORD got = 0;
              if (ops2->on_read_file(h, buf, len, &got)) {
                  LeaveCriticalSection(&g_lock);
                  if (read) *read = got;
                  complete_overlapped(h, ov, got);
                  return TRUE;
              }
          }
        }
        jvs_diag("ReadFile req=%lu give=0 (queued=0)", (unsigned long)len);
        LeaveCriticalSection(&g_lock);
        if (read) *read = 0;
        complete_overlapped(h, ov, 0);
        return TRUE;
    }
    LeaveCriticalSection(&g_lock);
    {   int tp = com_tap_port_of(h);
        if (tp >= 0) {
            BOOL r = Real_ReadFile(h, buf, len, read, ov);
            DWORD got = (read && *read) ? *read : 0;
            if (got) {
                char tg[24]; snprintf(tg, sizeof tg, "TAP-RX COM%d", tp);
                jvs_diag_hex(tg, (const unsigned char*)buf, got > 64 ? 64 : got);
            } else {
                jvs_diag("TAP-RX COM%d: 0 bytes (rc=%d err=%lu)", tp, (int)r,
                         (unsigned long)GetLastError());
            }
            return r;
        } }
    return Real_ReadFile(h, buf, len, read, ov);
}

static BOOL WINAPI Hook_CloseHandle(HANDLE h) {
    com_tap_remove(h);
    EnterCriticalSection(&g_lock);
    int ours = slot_for(h) != NULL;
    if (ours) slot_free(h);
    LeaveCriticalSection(&g_lock);
    /* Notify a registered per-game module on EVERY close so it can clear its own
     * handle-value sentinels the moment they're freed (Windows recycles handle
     * numbers almost immediately -- this is the fix for the documented Cave PC/
     * DSII CVKEY.222 handle-aliasing crash: a stale sentinel must be cleared
     * here, not left for the next unrelated CreateFile to alias). Must no-op
     * cleanly on handles it doesn't own. */
    { const board_io_ops* ops = ultra_board_ops_get();
      if (ops && ops->on_close_handle) ops->on_close_handle(h); }
    return Real_CloseHandle(h);   /* it is a real event handle either way */
}

/* comm-config shims: succeed silently for our fake port, pass through otherwise.
 * All log (ULTRA_JVS_LOG) so we can see the exact handshake sequence. */
#define COM_NOOP_TRUE(NAME, REAL, SIG, ARGS, TAG) \
    static BOOL WINAPI NAME SIG { \
        EnterCriticalSection(&g_lock); int ours = slot_for(h) != NULL; LeaveCriticalSection(&g_lock); \
        if (ours) { jvs_diag(TAG); return TRUE; } return REAL ARGS; }

/* Hook_SetCommState is deliberately NOT built from COM_NOOP_TRUE like its two
 * neighbors below: this is the one call that carries the actual line
 * parameters (baud/bytesize/parity/stopbits) a game asked for, and per this
 * board's own bring-up notes for Mario Kart Arcade GP DX, nothing anywhere in
 * this project has ever tied a specific baud rate to COM2 (the vendor) as
 * opposed to COM3 (JVS) or COM4 (steering) -- the exe's own generic
 * SerialSettingWindows dumper lists candidates (115200/57600/.../9600) but not
 * which port gets which. Logging the DCB here (still gated by the same
 * ULTRA_JVS_LOG env var every other line in this file uses, still a pure
 * no-op returning TRUE exactly as before) is a zero-risk way to finally
 * answer that for real on the next test run, for every port this file claims
 * -- not just COM2, so this also documents COM3/COM4 for comparison. */
static BOOL WINAPI Hook_SetCommState(HANDLE h, LPDCB d) {
    EnterCriticalSection(&g_lock); com_slot* s = slot_for(h); LeaveCriticalSection(&g_lock);
    if (s) {
        if (d) {
            jvs_diag("SetCommState COM%d: baud=%lu bytesize=%u parity=%u(%s) "
                     "stopbits=%u(%s) fBinary=%d fParity=%d fOutxCtsFlow=%d "
                     "fDtrControl=%d fRtsControl=%d",
                     s->port, (unsigned long)d->BaudRate, (unsigned)d->ByteSize,
                     (unsigned)d->Parity,
                     d->Parity==NOPARITY?"N":d->Parity==ODDPARITY?"O":d->Parity==EVENPARITY?"E":
                     d->Parity==MARKPARITY?"M":d->Parity==SPACEPARITY?"S":"?",
                     (unsigned)d->StopBits,
                     d->StopBits==ONESTOPBIT?"1":d->StopBits==ONE5STOPBITS?"1.5":
                     d->StopBits==TWOSTOPBITS?"2":"?",
                     d->fBinary, d->fParity, d->fOutxCtsFlow, d->fDtrControl, d->fRtsControl);
        } else {
            jvs_diag("SetCommState COM%d: (NULL DCB)", s->port);
        }
        return TRUE;
    }
    return Real_SetCommState(h, d);
}
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
    EnterCriticalSection(&g_lock); com_slot* s = slot_for(h);
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
        /* The JVS SENSE line is read via modem status and is DYNAMIC: while our
         * node is unaddressed the host must see "node present, address me", and
         * once we've taken an address it must see "chain fully addressed" so the
         * host stops walking SET_ADDRESS. Empirically (Chaos Breaker/jvs.lib):
         * all lines clear = "address me", a set line = "done". Tunable via
         * ULTRA_JVS_SENSE0 (unaddressed) / ULTRA_JVS_SENSE1 (addressed). */
        ultra_jvs_node* node = ultra_hook_node();
        /* The whole CHAIN, not just node 1: with a second node enabled we must
         * keep saying "not done" until it too has an address, or the host
         * stops enumerating and the STR PCB never exists. */
        int addressed = (node && jvs_chain_addressed(node));
        DWORD v;
        /* serialsense() in jvs.lib = (GetCommModemStatus >> 4) & 1 = the CTS bit
         * (0x10). The host addresses nodes while serialsense()==0 and stops at 1.
         * So: unaddressed -> CTS clear (0); addressed -> CTS set (0x10). (Proven
         * by disassembling Chaos Breaker's jvs.lib ?serialsense@@/?Proc@@.) */
        if (addressed) { const char* e = getenv("ULTRA_JVS_SENSE1"); v = (e && *e) ? (DWORD)strtoul(e,NULL,0) : (DWORD)MS_CTS_ON; }
        else           { const char* e = getenv("ULTRA_JVS_SENSE0"); v = (e && *e) ? (DWORD)strtoul(e,NULL,0) : 0u; }
        if (st) *st = v;
        jvs_diag("GetCommModemStatus -> 0x%lX (addressed=%d)", (unsigned long)v, addressed);
        return TRUE;
    }
    return Real_GetCommModemStatus(h,st);
}
static BOOL WINAPI Hook_SetCommMask(HANDLE h, DWORD mask) {
    EnterCriticalSection(&g_lock); com_slot* s = slot_for(h); if (s) s->evtmask = mask;
    int ours = s != NULL; LeaveCriticalSection(&g_lock);
    if (ours) { jvs_diag("SetCommMask(0x%lX)", (unsigned long)mask); return TRUE; }
    return Real_SetCommMask(h, mask);
}
/* Event-driven readers wait here for EV_RXCHAR. Our writes produce the response
 * synchronously, so a poll-then-wait pattern has data ready; report it at once. */
static BOOL WINAPI Hook_WaitCommEvent(HANDLE h, LPDWORD evt, LPOVERLAPPED ov) {
    EnterCriticalSection(&g_lock); com_slot* s = slot_for(h);
    if (s) {
        DWORD mask = s->evtmask ? s->evtmask : (DWORD)EV_RXCHAR;
        /* A push-mode board (e.g. Cave PC/DSII) has no pre-queued data
         * (resp_len==0 always), so a registered ops must be able to say
         * "signal ready anyway" so the game's WaitCommEvent loop advances to
         * ReadFile, where on_read_file generates the frame live. */
        const board_io_ops* ops = ultra_board_ops_get();
        int pending_anyway = ops && ops->on_wait_comm_event_pending && ops->on_wait_comm_event_pending();
        DWORD ev = (pending_anyway || (s->resp_len > s->resp_pos)) ? (mask & EV_RXCHAR) : 0;
        LeaveCriticalSection(&g_lock);
        if (evt) *evt = ev;
        jvs_diag("WaitCommEvent -> ev=0x%lX (queued=%d, ov=%p)",
                 (unsigned long)ev, (int)(s ? (int)(s->resp_len - s->resp_pos) : -1),
                 (void*)ov);
        complete_overlapped(h, ov, 0);
        return TRUE;
    }
    LeaveCriticalSection(&g_lock);
    return Real_WaitCommEvent(h, evt, ov);
}
static BOOL WINAPI Hook_GetOverlappedResult(HANDLE h, LPOVERLAPPED ov,
                                            LPDWORD cnt, BOOL wait) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h) != NULL; LeaveCriticalSection(&g_lock);
    if (ours) {                              /* our I/O always completed synchronously */
        if (cnt && ov) *cnt = (DWORD)ov->InternalHigh;
        jvs_diag("GetOverlappedResult -> %lu bytes",
                 (unsigned long)((cnt && ov) ? *cnt : 0));
        return TRUE;
    }
    return Real_GetOverlappedResult(h, ov, cnt, wait);
}

/* GetProcAddress interception: hand back our shim for the names we own so that
 * code resolving kernel32 functions dynamically still hits our I/O. */
extern void ultra_d3d8_log(const char* fmt, ...);  /* shared trace sink (d3d8_throttle.c) */

static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR name);
static HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR name);

/* A registered per-game module can answer a dynamic GetProcAddress lookup for
 * a name it owns even when there's no static import to IAT-patch (e.g. Cave
 * PC/DSII resolves GetIfTable dynamically). Weakly declared so this file
 * links whether or not any linked game_*.c file actually defines it. */
int __attribute__((weak)) game_dsii_hook_getproctable(const char* name, void** out_fn) {
    (void)name; (void)out_fn; return 0;
}

static FARPROC WINAPI Hook_GetProcAddress(HMODULE mod, LPCSTR name) {
    if ((((uintptr_t)name) >> 16) != 0) {   /* by name, not ordinal */
        { const board_io_ops* ops = ultra_board_ops_get();
          void* fn;
          if (ops && game_dsii_hook_getproctable(name, &fn)) return (FARPROC)fn; }
        struct { const char* n; FARPROC f; } map[] = {
            {"CreateFileA",(FARPROC)Hook_CreateFileA},{"CreateFileW",(FARPROC)Hook_CreateFileW},
            {"ReadFile",(FARPROC)Hook_ReadFile},{"WriteFile",(FARPROC)Hook_WriteFile},
            {"CloseHandle",(FARPROC)Hook_CloseHandle},{"SetCommState",(FARPROC)Hook_SetCommState},
            {"GetCommState",(FARPROC)Hook_GetCommState},{"SetCommTimeouts",(FARPROC)Hook_SetCommTimeouts},
            {"PurgeComm",(FARPROC)Hook_PurgeComm},{"SetupComm",(FARPROC)Hook_SetupComm},
            {"ClearCommError",(FARPROC)Hook_ClearCommError},
            {"GetCommModemStatus",(FARPROC)Hook_GetCommModemStatus},
            {"EscapeCommFunction",(FARPROC)Hook_EscapeCommFunction},
            {"SetCommMask",(FARPROC)Hook_SetCommMask},{"WaitCommEvent",(FARPROC)Hook_WaitCommEvent},
            {"GetOverlappedResult",(FARPROC)Hook_GetOverlappedResult},
            {"DeviceIoControl",(FARPROC)Hook_DeviceIoControl},
            {"GetProcAddress",(FARPROC)Hook_GetProcAddress},
            {"LoadLibraryA",(FARPROC)Hook_LoadLibraryA},
            {"LoadLibraryW",(FARPROC)Hook_LoadLibraryW},
        };
        for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); ++i)
            if (strcmp(name, map[i].n) == 0) return map[i].f;

        /* PACKED Type X2 games (e.g. Haunted Museum) have NO static d3d9 IAT slot --
         * the unpacker resolves the renderer create-fn at runtime via
         * LoadLibrary("d3d9.dll")+GetProcAddress(...,"Direct3DCreate9"). The IAT
         * patch can't catch that (no slot exists), so intercept it HERE and hand
         * back our wrapper. Same for the D3D11/GL siblings. This is the canonical
         * packed-game present-hook entry point. */
        {
            extern void*   __stdcall ultra_Hook_Direct3DCreate9(UINT);
            extern HRESULT __stdcall ultra_Hook_D3D11CreateDeviceAndSwapChain(
                void*,int,void*,UINT,const void*,UINT,UINT,const void*,void**,void**,void*,void**);
            extern HRESULT __stdcall ultra_Hook_CreateDXGIFactory(const void*,void**);
            extern HRESULT __stdcall ultra_Hook_CreateDXGIFactory1(const void*,void**);
            extern BOOL    __stdcall ultra_Hook_wglSwapBuffers(HDC);
            extern BOOL    __stdcall ultra_Hook_SwapBuffers(HDC);
            if (strcmp(name, "Direct3DCreate9") == 0) {
                ultra_d3d8_log("D3D9: GetProcAddress(\"Direct3DCreate9\") intercepted -> hook armed (packed path)");
                return (FARPROC)ultra_Hook_Direct3DCreate9;
            }
            if (strcmp(name, "D3D11CreateDeviceAndSwapChain") == 0)
                return (FARPROC)ultra_Hook_D3D11CreateDeviceAndSwapChain;
            /* DXGI-factory render path (bare device + IDXGIFactory::CreateSwapChain) */
            if (strcmp(name, "CreateDXGIFactory") == 0)
                return (FARPROC)ultra_Hook_CreateDXGIFactory;
            if (strcmp(name, "CreateDXGIFactory1") == 0)
                return (FARPROC)ultra_Hook_CreateDXGIFactory1;
            if (strcmp(name, "wglSwapBuffers") == 0)
                return (FARPROC)ultra_Hook_wglSwapBuffers;
            /* GL titles that swap via gdi32!SwapBuffers (wgl tail-calls it) */
            if (strcmp(name, "SwapBuffers") == 0)
                return (FARPROC)ultra_Hook_SwapBuffers;
        }

        /* Namco ES3 dongle bypass: intercept SetupDi* from setupapi.dll */
        { FARPROC nd = ultra_namco_dongle_getproc(mod, name);
          if (nd) return nd; }
    }
    return Real_GetProcAddress(mod, name);
}

/* -------------------------------------------------------------------------- */
/*  IAT patching across all loaded modules                                     */
/* -------------------------------------------------------------------------- */
/* All header/thunk reads are bounds-checked against the module's mapped image
 * size (mingw has no SEH __try/__except), so a malformed import directory is
 * skipped instead of crashing the host game. We patch by name AND by value:
 * the value pass (slot already == the real kernel32 export) catches bound
 * imports whose names were stripped, and is naturally idempotent on re-patch. */
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
        /* Bound EACH thunk read against the mapped image: a malformed/packed import
         * whose IAT array is not NULL-terminated within the image would otherwise
         * walk off the end and fault (no SEH guard under mingw). Stop if the next
         * iat (or orig, when present) would read past isize. iat/orig are always
         * >= base, so the byte offset is non-negative -> a clean size_t compare. */
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
void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn) {
    void* realfn = NULL;
    HMODULE imp = GetModuleHandleA(impdll);
    if (imp) realfn = (void*)GetProcAddress(imp, impfn);   /* for the value-based pass */

    HMODULE mods[512]; DWORD need = 0;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ultra_iat_patch_all, &self);
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        /* `need` is the size required for ALL modules; if it exceeds our fixed
         * buffer only the first 512 entries were actually written, so clamp the
         * iteration to the buffer capacity (else we'd read past mods[] and deref
         * garbage HMODULEs as PE images). */
        DWORD count = need / sizeof(HMODULE);
        if (count > 512) count = 512;
        for (DWORD i = 0; i < count; ++i)
            if (mods[i] != self)                 /* don't patch our own imports */
                iat_patch_module(mods[i], impdll, impfn, newfn, realfn);
    }
}

static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR name) {
    /* A registered per-game module can redirect a bare-name (no path separator)
     * DLL load -- e.g. Cave PC/DSII's dinput8.dll -> game-folder-copy redirect.
     * The callback owns the "which name do I care about" decision. */
    if (name && strchr(name, '\\') == NULL && strchr(name, '/') == NULL) {
        const board_io_ops* ops = ultra_board_ops_get();
        if (ops && ops->on_load_library_redirect) {
            const char* redirect = ops->on_load_library_redirect(name);
            if (redirect) {
                HMODULE h = Real_LoadLibraryA(redirect);
                jvs_diag("LoadLibraryA(\"%s\") -> redirected to \"%s\" (h=%p)", name, redirect, (void*)h);
                return h;
            }
        }
    }
    return Real_LoadLibraryA(name);
}

static HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR name) {
    if (name && wcschr(name, L'\\') == NULL && wcschr(name, L'/') == NULL) {
        char narrow[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, MAX_PATH, NULL, NULL);
        const board_io_ops* ops = ultra_board_ops_get();
        if (ops && ops->on_load_library_redirect) {
            const char* redirect = ops->on_load_library_redirect(narrow);
            if (redirect) {
                HMODULE h = Real_LoadLibraryA(redirect);
                jvs_diag("LoadLibraryW(\"%s\") -> redirected to \"%s\" (h=%p)", narrow, redirect, (void*)h);
                return h;
            }
        }
    }
    return Real_LoadLibraryW(name);
}

/* ---------------------------------------------------------------------------
 *  LAN ERROR 1 -- make the live adapter count as real hardware
 * ---------------------------------------------------------------------------
 *
 *  The game calls GetIfTable2 and wants one interface that is BOTH
 *
 *      hardware   -- bit 0 of InterfaceAndOperStatusFlags (row + 0x480)
 *      connected  -- MediaConnectState == 1                (row + 0x48C)
 *
 *  On a cabinet that is the onboard NIC with a cable in it. On this machine
 *  the two halves land on different adapters and neither one satisfies both:
 *
 *      Realtek 1G      hardware,  DISCONNECTED   -> fails the link test
 *      Mellanox 10G    CONNECTED, Virtual=true   -> fails the hardware test
 *
 *  so no row qualifies and 4-1 is raised. It is also perfectly correct: the
 *  game is right that no physical link exists. But plugging a cable into a port
 *  the owner does not want to use is not a fix, it is a workaround with a
 *  shopping list, and we already spoof ALL.Net for exactly the same reason.
 *
 *  So: promote. A row that is genuinely UP AND CONNECTED gets the hardware bit
 *  set. A disconnected adapter is NOT reported as connected - the truth we
 *  assert is "this working link counts as a real one", which it is, rather than
 *  "that dead port is alive", which it is not. If the machine really has no
 *  network, no row qualifies and the error stands, correctly.
 *
 *  PROMOTE EXACTLY ONE ROW, AND ONLY ONE THAT OWNS AN IP ADDRESS.
 *
 *  The first cut of this promoted every up+connected row, and that was wrong in
 *  a way the log showed immediately. Windows lists a filter/scheduler
 *  pseudo-interface for each real NIC, and they are indistinguishable from the
 *  real one by every obvious field - same 6-byte MAC, same type 6, same name
 *  with a suffix:
 *
 *      ifIndex 22  Mellanox ConnectX-3 ...-WFP Native MAC Layer LightWeight Filter-0000
 *      ifIndex 23  Mellanox ConnectX-3 ...-QoS Packet Scheduler-0000
 *      ifIndex 24  Mellanox ConnectX-3 ...-WFP 802.3 MAC Layer LightWeight Filter-0000
 *      ifIndex  4  Mellanox ConnectX-3 Ethernet Adapter          <-- owns 192.168.0.194
 *
 *  The game took a promoted pseudo-interface, asked it for an address, and got
 *  nothing: its own log went from "cable disconnected" to "address acquisition
 *  NG". One step further, still stuck.
 *
 *  The discriminator is the address table. Only ifIndex 4 appears in it, so
 *  "has a non-loopback IPv4" picks the real adapter and nothing else - no
 *  name-matching on "WFP"/"QoS" substrings, which would be a guess about how
 *  Microsoft spells things this year.
 *
 *      ULTRA_ES3_FAKE_LINK=0    leave GetIfTable2 exactly as Windows returns it
 */
#define IFROW_SZ     0x548     /* sizeof(MIB_IF_ROW2), from the SDK headers  */
#define IFROW_IDX    0x8       /* NET_IFINDEX InterfaceIndex                 */
#define IFROW_DESC   0x21E     /* WCHAR Description[257]                     */
#define IFROW_MACLEN 0x420     /* ULONG PhysicalAddressLength                */
#define IFROW_FLAGS  0x480     /* InterfaceAndOperStatusFlags, bit0=hardware */
#define IFROW_OPER   0x484     /* IF_OPER_STATUS, 1 = IfOperStatusUp         */
#define IFROW_MEDIA  0x48C     /* NET_IF_MEDIA_CONNECT_STATE, 1 = Connected  */
#define IFTAB_ROWS   0x8       /* MIB_IF_TABLE2::Table                       */
#define IPTAB_ROWS   0x4       /* MIB_IPADDRTABLE::table                     */
#define IPROW_SZ     0x18      /* sizeof(MIB_IPADDRROW)                      */

typedef ULONG (WINAPI *pGetIfTable2)(void**);
typedef ULONG (WINAPI *pGetIpAddrTable)(void*, ULONG*, BOOL);
static pGetIfTable2    Real_GetIfTable2;
static pGetIpAddrTable Real_GetIpAddrTable;
/* Which interface the link hook chose. The address hooks below rewrite that
 * one and no other, so the machine's real networking is never touched. */
static ULONG g_promoted_idx;

/* Does this interface index own a usable IPv4 address? Loopback does not count
 * - 127.0.0.1 is always there and would make every guess look right. */
static int iface_ipv4(ULONG want_idx, ULONG* out_ip)
{
    unsigned char stackbuf[4096];
    unsigned char* buf = stackbuf;
    ULONG sz = sizeof stackbuf, n, i;
    int found = 0;

    if (!Real_GetIpAddrTable) return 0;
    if (Real_GetIpAddrTable(buf, &sz, FALSE) != 0) {
        if (sz <= sizeof stackbuf || sz > (1u << 20)) return 0;
        buf = (unsigned char*)malloc(sz);
        if (!buf) return 0;
        if (Real_GetIpAddrTable(buf, &sz, FALSE) != 0) { free(buf); return 0; }
    }
    n = *(ULONG*)buf;
    for (i = 0; i < n && !found; i++) {
        unsigned char* r = buf + IPTAB_ROWS + (size_t)i * IPROW_SZ;
        ULONG ip  = *(ULONG*)(r + 0);
        ULONG idx = *(ULONG*)(r + 4);
        if (idx == want_idx && ip && (ip & 0xFF) != 127) {
            if (out_ip) *out_ip = ip;
            found = 1;
        }
    }
    if (buf != stackbuf) free(buf);
    return found;
}

static ULONG WINAPI Hook_GetIfTable2(void** table)
{
    unsigned char* t;
    ULONG n, i;
    char env[8];

    ULONG rc = Real_GetIfTable2 ? Real_GetIfTable2(table) : (ULONG)-1;
    if (rc != 0 || !table || !*table) return rc;

    if (GetEnvironmentVariableA("ULTRA_ES3_FAKE_LINK", env, sizeof env) && env[0] == '0')
        return rc;

    t = (unsigned char*)*table;
    n = *(ULONG*)t;
    if (n == 0 || n > 256) return rc;          /* implausible: do not touch it */

    /* Prove the offsets before writing through them. Description is a UTF-16
     * adapter name; if row 0 does not hold one, this Windows build lays the
     * structure out differently and we must not scribble on it. */
    {
        const wchar_t* d0 = (const wchar_t*)(t + IFTAB_ROWS + IFROW_DESC);
        if (d0[0] < 0x20 || d0[0] > 0x7E) {
            static int said;
            if (!said) { said = 1;
                jvs_diag("LINK: MIB_IF_ROW2 layout not as expected -- GetIfTable2 left untouched"); }
            return rc;
        }
    }

    for (i = 0; i < n; i++) {
        unsigned char* row = t + IFTAB_ROWS + (size_t)i * IFROW_SZ;
        unsigned char* flags = row + IFROW_FLAGS;
        ULONG idx = *(ULONG*)(row + IFROW_IDX);
        ULONG ip  = 0;
        char name[160];

        if (*(unsigned*)(row + IFROW_OPER)  != 1) continue;   /* not up        */
        if (*(unsigned*)(row + IFROW_MEDIA) != 1) continue;   /* not connected */
        if (*(ULONG*)(row + IFROW_MACLEN)   != 6) continue;   /* no Ethernet MAC */
        if (*flags & 1)                           return rc;  /* a real one already
                                                               * qualifies: leave
                                                               * the table alone */
        if (!iface_ipv4(idx, &ip)) continue;      /* pseudo-interface, no address */

        WideCharToMultiByte(CP_ACP, 0, (const wchar_t*)(row + IFROW_DESC), -1,
                            name, sizeof name, NULL, NULL);
        *flags |= 1;
        g_promoted_idx = idx;
        {
            static int said;
            if (!said) { said = 1;
                jvs_diag("LINK: promoted ifIndex %lu \"%s\" (%lu.%lu.%lu.%lu) to hardware"
                         " -- it is up, connected and owns an address",
                         idx, name, ip & 0xFF, (ip >> 8) & 0xFF,
                         (ip >> 16) & 0xFF, (ip >> 24) & 0xFF); }
        }
        return rc;                               /* exactly one, then stop */
    }

    {
        static int said;
        if (!said) { said = 1;
            jvs_diag("LINK: no up+connected adapter owns an IPv4 address --"
                     " 4-1 stands, and it is right to"); }
    }
    return rc;
}

/* ---------------------------------------------------------------------------
 *  "address acquisition NG" -- put the cabinet on its own subnet
 * ---------------------------------------------------------------------------
 *
 *  With the link spoof in place the game stopped complaining about the cable
 *  and moved one step on, to this, in its own NWComponentLog:
 *
 *      before   ケーブル接続が切断されました      cable disconnected
 *      after    アドレス取得NG                  address acquisition failed
 *
 *  A Namco ES3 cabinet lives on a fixed private LAN. The game holds its own
 *  address as text and tests it with a std::regex built at 0x14060B8CD from
 *
 *      (192)\.(168)\.(123)\.[0-9]+
 *
 *  so nothing outside 192.168.123.0/24 is an acceptable cabinet address. This
 *  machine's adapter is 192.168.0.194, which is a perfectly good address and
 *  the wrong one.
 *
 *  The host could simply be given a second IP on that subnet, and that would
 *  work. But the owner does not want their networking rearranged to satisfy a
 *  game, which is fair, and we already answer ALL.Net, MUCHA and the JVS chain
 *  with what they expect rather than reconfiguring the machine. So answer this
 *  the same way: when the game asks Windows to enumerate addresses, the one
 *  interface we promoted reports a cabinet address.
 *
 *  All three enumeration APIs are covered because the game imports all three
 *  and static analysis could not say which one feeds the check - the addresses
 *  found by scanning for call sites turned out to be import THUNKS, not calls.
 *  Rather than guess, all three are answered consistently and each logs the
 *  first time it is used, so the next run says which one actually mattered.
 *
 *  ONLY the promoted interface is rewritten. Every other adapter is reported
 *  exactly as Windows described it, so this cannot disturb the machine's real
 *  networking, and our own ALL.Net/MUCHA traffic is on 127.0.0.1 regardless.
 *
 *      ULTRA_ES3_LAN_IP=192.168.123.10   what to report (default)
 *      ULTRA_ES3_LAN_IP=off              report the true address
 */
#define IPADDR_ROW_ADDR  0x0
#define IPADDR_ROW_INDEX 0x4
#define IPADDR_ROW_MASK  0x8
/* ASK THE COMPILER, DO NOT WRITE THE NUMBERS DOWN.
 *
 * These were once hand-written constants, measured off a 64-bit binary (see
 * the networkLanChecker address in the comment further down). This ONE source
 * file compiles into BOTH the 32-bit and the 64-bit hook, and there was no
 * bitness guard - so on a 32-bit title every pointer-bearing offset was wrong
 * by 4 to 12 bytes. Hook_GetAdaptersInfo then walked a "Next" pointer read out
 * of an ASCII address string and _snprintf'd through it, which is an access
 * violation into a read-only system image page. It cost Mario Kart GP DX every
 * launch, at the network check, while Pokken (x64) was unaffected and hid it.
 *
 * offsetof() is right for both bitnesses by construction and cannot drift. */
#define AINFO_NEXT       offsetof(IP_ADAPTER_INFO, Next)
#define AINFO_INDEX      offsetof(IP_ADAPTER_INFO, Index)
#define AINFO_IPLIST     offsetof(IP_ADAPTER_INFO, IpAddressList)
#define ASTR_NEXT        offsetof(IP_ADDR_STRING, Next)
#define ASTR_IP          offsetof(IP_ADDR_STRING, IpAddress)
#define ASTR_MASK        offsetof(IP_ADDR_STRING, IpMask)
#define ASTR_LIST_SZ     sizeof(IP_ADDR_STRING)
#define AA_IFINDEX       offsetof(IP_ADAPTER_ADDRESSES, IfIndex)
#define AA_NEXT          offsetof(IP_ADAPTER_ADDRESSES, Next)
#define AA_UNICAST       offsetof(IP_ADAPTER_ADDRESSES, FirstUnicastAddress)
#define UNI_NEXT         offsetof(IP_ADAPTER_UNICAST_ADDRESS, Next)
#define UNI_SOCKADDR     offsetof(IP_ADAPTER_UNICAST_ADDRESS, Address.lpSockaddr)

typedef ULONG (WINAPI *pGetAdaptersInfo)(void*, ULONG*);
typedef ULONG (WINAPI *pGetAdaptersAddresses)(ULONG, ULONG, void*, void*, ULONG*);
static pGetAdaptersInfo      Real_GetAdaptersInfo;
static pGetAdaptersAddresses Real_GetAdaptersAddresses;

/* The address we hand out, as a packed little-endian IPv4 (0 = feature off). */
static ULONG cabinet_ip(void)
{
    static ULONG cached = (ULONG)-1;
    char v[64];
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (cached != (ULONG)-1) return cached;
    if (!GetEnvironmentVariableA("ULTRA_ES3_LAN_IP", v, sizeof v) || !v[0])
        strcpy(v, "192.168.123.10");
    if (!strcmp(v, "off") || !strcmp(v, "0")) { cached = 0; return 0; }
    if (sscanf(v, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) { cached = 0; return 0; }
    cached = (ULONG)((a & 0xFF) | ((b & 0xFF) << 8) | ((c & 0xFF) << 16) | ((d & 0xFF) << 24));
    return cached;
}

/* Which interface should carry it. Prefer the one we promoted; if the game
 * asked for addresses before it asked for interfaces, fall back to whichever
 * owns a real IPv4 -- the same rule, just computed a moment earlier. */
static ULONG cabinet_iface(void)
{
    unsigned char stackbuf[4096];
    unsigned char* buf = stackbuf;
    ULONG sz = sizeof stackbuf, n, i, pick = 0;
    if (g_promoted_idx) return g_promoted_idx;
    if (!Real_GetIpAddrTable) return 0;
    if (Real_GetIpAddrTable(buf, &sz, FALSE) != 0) return 0;
    n = *(ULONG*)buf;
    for (i = 0; i < n && !pick; i++) {
        unsigned char* r = buf + IPTAB_ROWS + (size_t)i * IPROW_SZ;
        ULONG ip = *(ULONG*)(r + IPADDR_ROW_ADDR);
        if (ip && (ip & 0xFF) != 127) pick = *(ULONG*)(r + IPADDR_ROW_INDEX);
    }
    return pick;
}

static void lan_note(const char* api, ULONG idx, ULONG ip)
{
    jvs_diag("LAN: %s -> ifIndex %lu reported as %lu.%lu.%lu.%lu"
             " (the 192.168.123.x cabinet subnet the game requires)",
             api, idx, ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
}

static ULONG WINAPI Hook_GetIpAddrTable(void* tbl, ULONG* size, BOOL order)
{
    ULONG rc = Real_GetIpAddrTable ? Real_GetIpAddrTable(tbl, size, order) : (ULONG)-1;
    ULONG ip = cabinet_ip(), idx, n, i;
    if (rc != 0 || !tbl || !ip) return rc;
    idx = cabinet_iface();
    if (!idx) return rc;
    n = *(ULONG*)tbl;
    if (n > 4096) return rc;
    for (i = 0; i < n; i++) {
        unsigned char* r = (unsigned char*)tbl + IPTAB_ROWS + (size_t)i * IPROW_SZ;
        if (*(ULONG*)(r + IPADDR_ROW_INDEX) != idx) continue;
        *(ULONG*)(r + IPADDR_ROW_ADDR) = ip;
        *(ULONG*)(r + IPADDR_ROW_MASK) = 0x00FFFFFF;      /* 255.255.255.0 */
        { static int said; if (!said) { said = 1; lan_note("GetIpAddrTable", idx, ip); } }
    }
    return rc;
}

static ULONG WINAPI Hook_GetAdaptersInfo(void* info, ULONG* size)
{
    ULONG rc = Real_GetAdaptersInfo ? Real_GetAdaptersInfo(info, size) : (ULONG)-1;
    ULONG ip = cabinet_ip(), idx;
    unsigned char* a;
    if (rc != 0 || !info || !ip) return rc;
    idx = cabinet_iface();
    if (!idx) return rc;
    /* THE GAME READS RECORD ZERO AND NOTHING ELSE.
     *
     * networkLanChecker::fetchAddress (0x14060AE70) calls GetAdaptersInfo,
     * keeps the buffer HEAD, and reads [head+0x1C0] (IpAddressList.IpAddress),
     * [head+0x1D0] (.IpMask) and [head+0x1F0] (GatewayList.IpAddress). There is
     * no Next walk anywhere in it. It then fails if the address string is the
     * literal "0.0.0.0".
     *
     * On this machine record[0] is the unplugged Realtek, which is exactly
     * "0.0.0.0"; the adapter we promoted is record[1]. So the first version of
     * this hook rewrote the record matching our chosen ifIndex - a record the
     * game never looks at - which is why it fired, logged success, and changed
     * nothing. The log said the spoof worked and the game still said
     * アドレス取得NG, and both were true.
     *
     * So rewrite the HEAD, whatever it is, and also the index-matched record so
     * anything that does walk the list sees the same answer. */
    for (a = (unsigned char*)info; a; a = *(unsigned char**)(a + AINFO_NEXT)) {
        unsigned char* s;
        int head = (a == (unsigned char*)info);
        if (!head && *(ULONG*)(a + AINFO_INDEX) != idx) continue;
        for (s = a + AINFO_IPLIST; s; s = *(unsigned char**)(s + ASTR_NEXT)) {
            _snprintf((char*)(s + ASTR_IP), 16, "%lu.%lu.%lu.%lu",
                      ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
            strcpy((char*)(s + ASTR_MASK), "255.255.255.0");
        }
        /* The gateway sits in the next IP_ADDR_STRING after the address list;
         * a cabinet's router is .1 on its own subnet. An empty gateway reads as
         * "no network" to the same checker. */
        for (s = a + AINFO_IPLIST + ASTR_LIST_SZ; s && head;
             s = *(unsigned char**)(s + ASTR_NEXT)) {
            _snprintf((char*)(s + ASTR_IP), 16, "%lu.%lu.%lu.1",
                      ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF);
            strcpy((char*)(s + ASTR_MASK), "255.255.255.0");
        }
        { static int said; if (!said) { said = 1;
            lan_note(head ? "GetAdaptersInfo[head]" : "GetAdaptersInfo", idx, ip); } }
    }
    return rc;
}

static ULONG WINAPI Hook_GetAdaptersAddresses(ULONG fam, ULONG flags, void* rsv,
                                              void* buf, ULONG* size)
{
    ULONG rc = Real_GetAdaptersAddresses
             ? Real_GetAdaptersAddresses(fam, flags, rsv, buf, size) : (ULONG)-1;
    ULONG ip = cabinet_ip(), idx;
    unsigned char* a;
    if (rc != 0 || !buf || !ip) return rc;
    idx = cabinet_iface();
    if (!idx) return rc;
    for (a = (unsigned char*)buf; a; a = *(unsigned char**)(a + AA_NEXT)) {
        unsigned char* u;
        if (*(ULONG*)(a + AA_IFINDEX) != idx) continue;
        for (u = *(unsigned char**)(a + AA_UNICAST); u;
             u = *(unsigned char**)(u + UNI_NEXT)) {
            unsigned char* sa = *(unsigned char**)(u + UNI_SOCKADDR);
            if (sa && *(unsigned short*)sa == AF_INET)
                *(ULONG*)(sa + 4) = ip;                   /* sockaddr_in::sin_addr */
        }
        { static int said; if (!said) { said = 1; lan_note("GetAdaptersAddresses", idx, ip); } }
    }
    return rc;
}

/* ---------------------------------------------------------------------------
 *  Who closes the window?
 * ---------------------------------------------------------------------------
 *
 *  Pokken exits cleanly with code 0 and nobody knows who asked it to. The chain
 *  itself is traced and is ordinary Win32:
 *
 *      WM_CLOSE -> DefWindowProc -> DestroyWindow -> WM_DESTROY
 *               -> PostQuitMessage(0) -> the game's "SYSDR SHUTDOWN" thread
 *               -> Shutdown(0) -> CRT _exit -> ExitProcess(0)
 *
 *  which is why the ExitProcess caller reads ferrum_app.exe+0x7AE352: that is
 *  the single call site inside the shared CRT common_exit, reached by exit,
 *  _exit, abort and a WinMain return alike. It says "orderly exit" and nothing
 *  whatsoever about who started it - a distinction that cost us a wrong
 *  conclusion once already.
 *
 *  So log the FIRST link instead of the last. The game's window procedure
 *  forwards to DefWindowProcA, so an IAT hook there sees WM_CLOSE arrive and
 *  can name the moment; PostQuitMessage catches the case where the game decides
 *  to quit on its own without a close message.
 *
 *  The two cases we need to tell apart, and cannot from an exit code:
 *    - the window was closed from OUTSIDE (its own X button, Alt+F4, a taskbar
 *      Close, or one of our own tools posting WM_CLOSE), or
 *    - the game asked to quit for a reason of its own.
 *  Only the first link distinguishes them. */
typedef LRESULT (WINAPI *pDefWindowProcA)(HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (WINAPI *pDefWindowProcW)(HWND, UINT, WPARAM, LPARAM);
typedef void    (WINAPI *pPostQuitMessage)(int);
static pDefWindowProcA  Real_DefWindowProcA;
static pDefWindowProcW  Real_DefWindowProcW;
static pPostQuitMessage Real_PostQuitMessage;

/* Star Wars Battle Pod flag, set by game_swbp_dongle.c's constructor once that
 * title is confirmed as the host process. */
extern int g_ultra_swbp;

/* Should we swallow the close/quit for the current process? SWBP only, and only
 * unless the operator explicitly opts back into normal closing. Cached after the
 * first call (env + flag are stable for the process lifetime). */
static int swbp_block_close(void)
{
    static int cached = -1;
    if (cached < 0) {
        int on = g_ultra_swbp;
        if (on) {
            const char* e = getenv("ULTRA_SWBP_ALLOW_CLOSE");
            if (e && e[0] == '1') on = 0;   /* escape hatch: let it close */
        }
        cached = on;
    }
    return cached;
}

static void wm_note(const char* api, HWND h, UINT m, WPARAM w)
{
    const char* what = NULL;
    if (m == WM_CLOSE) what = "WM_CLOSE";
    else if (m == WM_DESTROY) what = "WM_DESTROY";
    else if (m == WM_QUIT) what = "WM_QUIT";
    else if (m == WM_ENDSESSION) what = "WM_ENDSESSION";
    else if (m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_CLOSE) what = "WM_SYSCOMMAND/SC_CLOSE";
    if (!what) return;
    {
        HWND fg = GetForegroundWindow();
        DWORD fpid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fpid);
        jvs_diag("SHUTDOWN-TRIGGER: %s via %s  hwnd=%p  foreground=%p (pid %lu, ours=%lu)"
                 "  -- this is what ends the run; the ExitProcess line later is only the CRT",
                 what, api, (void*)h, (void*)fg, fpid, GetCurrentProcessId());
    }
}

/* Is this a message that would tear the game down? */
static int wm_is_close(UINT m, WPARAM w)
{
    return m == WM_CLOSE
        || m == WM_QUERYENDSESSION
        || m == WM_ENDSESSION
        || (m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_CLOSE);
}

static LRESULT WINAPI Hook_DefWindowProcA(HWND h, UINT m, WPARAM w, LPARAM l)
{
    wm_note("DefWindowProcA", h, m, w);
    if (swbp_block_close() && wm_is_close(m, w)) {
        jvs_diag("SWBP: swallowed close message 0x%X (game kept alive)", m);
        return 0;   /* handled -- do NOT let DefWindowProc DestroyWindow() us */
    }
    return Real_DefWindowProcA ? Real_DefWindowProcA(h, m, w, l) : DefWindowProcA(h, m, w, l);
}
static LRESULT WINAPI Hook_DefWindowProcW(HWND h, UINT m, WPARAM w, LPARAM l)
{
    wm_note("DefWindowProcW", h, m, w);
    if (swbp_block_close() && wm_is_close(m, w)) {
        jvs_diag("SWBP: swallowed close message 0x%X (game kept alive)", m);
        return 0;
    }
    return Real_DefWindowProcW ? Real_DefWindowProcW(h, m, w, l) : DefWindowProcW(h, m, w, l);
}
static void WINAPI Hook_PostQuitMessage(int code)
{
    void* ra = __builtin_return_address(0);
    HMODULE mod = NULL;
    char path[MAX_PATH];
    const char* leaf = "?";
    unsigned long long off = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)ra, &mod) && mod &&
        GetModuleFileNameA(mod, path, sizeof path)) {
        const char* p = strrchr(path, 92);   /* 92 = backslash, as a code */
        leaf = p ? p + 1 : path;
        off  = (unsigned long long)((char*)ra - (char*)mod);
    }
    jvs_diag("SHUTDOWN-TRIGGER: PostQuitMessage(%d) from %s+0x%llX", code, leaf, off);
    if (swbp_block_close()) {
        jvs_diag("SWBP: swallowed PostQuitMessage(%d) (game kept alive)", code);
        return;   /* do NOT post WM_QUIT -- the message loop stays alive */
    }
    if (Real_PostQuitMessage) Real_PostQuitMessage(code); else PostQuitMessage(code);
}

/* ---------------------------------------------------------------------------
 *  Namco ES3 operator-error gate
 * ---------------------------------------------------------------------------
 *
 *  Pokken Tournament stops on its cabinet error screen with four errors listed:
 *
 *      7-1  CARD R/W DISCONNECT     4-1 LAN ERROR 1
 *      4-5  LAN ERROR 5             19-1 SYSTEM ERROR 1
 *
 *  THREE OF THOSE FOUR NEVER STOPPED ANYTHING. The game keeps a 30-entry error
 *  table of 40-byte records, and one byte in each record decides whether the
 *  error merely appears in the corner list or actually halts the cabinet:
 *
 *      record = table + index*0x28
 *        +0x00  u32   code, (major << 8) | minor   -- 0x0701 prints as "7-1"
 *        +0x08  char* title, e.g. "CARD R/W DISCONNECT"
 *        +0x10  char* category, e.g. "CARD"
 *        +0x20  u32   auto-expire time, seconds
 *        +0x24  u8    non-zero = this error NEVER auto-expires
 *        +0x25  u8    ZERO = raising it is FATAL to the cabinet
 *
 *  THE FIRST VERSION OF THIS PATCHED +0x24, AND THAT WAS WRONG. The two bytes
 *  do nearly opposite jobs, and only one of them is the halt. Disassembly of
 *  both readers settles it:
 *
 *    raise, 0x14065E37A   cmp byte [r12+25h],0        <- the record
 *                         sete al
 *                         or  byte [rsi+38h],al       <- errmgr+0x38: FATAL LATCH
 *
 *    tick,  0x14065DF40   cmp byte [rcx+24h],0
 *                         jne  skip                    <- non-zero: skip expiry
 *                         movsxd rcx,[rcx+20h]
 *                         imul rcx,rcx,989680h         <- seconds -> 100ns units
 *
 *  So +0x25 == 0 arms a latch at errmgr+0x38 the moment the error is raised,
 *  and the scene manager reacts by destroying whatever scene is running and
 *  switching to scene 0x11 - an object whose name string is literally
 *  "fatalerror", which draws nothing and whose isFinished() tests a pointer
 *  that is never NULL, so it can never exit. That is the black screen, exactly:
 *  the game renders its SYSTEM CHECK, 19-1 is raised, the latch arms, and the
 *  cabinet switches to a scene that draws one color forever.
 *
 *  Meanwhile +0x24 only suppresses auto-expiry. Writing 1 there - what we did
 *  first - made 19-1 PERMANENT rather than harmless. It looked plausible
 *  because the values correlate across the table, and correlation is exactly
 *  how you pick the wrong byte of two.
 *
 *  Measured in this dump: 7-1, 4-2 and 4-5 all carry +0x25 = 1, so the card and
 *  LAN errors could never have halted anything and fixing them would not have
 *  produced a single visible frame. 19-1 carries +0x25 = 0 and is the only one
 *  of the four that can.
 *
 *  Suppressing an error still does NOT make its underlying check pass - 19-1 is
 *  the dongle/serial check, and S/N will keep reading NG on the check screen.
 *  This gets the cabinet past a halt whose cause is physical hardware we do not
 *  have; it is not a substitute for having it.
 *
 *      ULTRA_ES3_ERR_COSMETIC=1301,0301   comma list of hex codes to un-halt
 *      ULTRA_ES3_ERR_COSMETIC=off         patch nothing
 */
#define ES3_ERRREC_SZ   0x28
#define ES3_ERRTAB_N    30
#define ES3_GATE_OFF    0x25
#define ES3_ERRTAB_RVA  0x9AAF30u      /* ferrum_app.exe, this dump */

/* The table is found by CONTENT, not by address. A different build moves it,
 * and writing 0x01 into whatever happens to live at a stale offset is exactly
 * the kind of patch that corrupts something unrelated and gets blamed on the
 * renderer three sessions later. The known RVA is only the first guess; it has
 * to prove itself, and if it cannot, we scan for the real thing. */
static unsigned char* es3_find_errtab(unsigned char* base, size_t span)
{
    size_t off;
    unsigned char* p;

    /* A valid table starts with COIN ERROR 1 (0x0101) and has ascending codes
     * with in-module title pointers. Two records is enough to rule out noise. */
    #define ES3_TAB_OK(q) (                                                    \
        *(unsigned*)(q) == 0x0101u &&                                          \
        *(unsigned*)((q) + ES3_ERRREC_SZ) == 0x0102u &&                        \
        *(unsigned long long*)((q) + 8) >= (unsigned long long)(uintptr_t)base &&\
        *(unsigned long long*)((q) + 8) <                                      \
            (unsigned long long)(uintptr_t)base + span)

    p = base + ES3_ERRTAB_RVA;
    if (ES3_ERRTAB_RVA + ES3_ERRREC_SZ * 2 < span && ES3_TAB_OK(p))
        return p;

    jvs_diag("ERRGATE: table not at the expected RVA 0x%X -- scanning", ES3_ERRTAB_RVA);
    for (off = 0; off + ES3_ERRREC_SZ * ES3_ERRTAB_N < span; off += 8) {
        p = base + off;
        if (ES3_TAB_OK(p)) {
            jvs_diag("ERRGATE: found error table at RVA 0x%X", (unsigned)off);
            return p;
        }
    }
    #undef ES3_TAB_OK
    return NULL;
}

static void es3_error_gate_patch(void)
{
    char want[128];
    unsigned char *base, *tab;
    MODULEINFO mi;
    HMODULE self;
    const char* s;
    int done = 0;

    if (!GetEnvironmentVariableA("ULTRA_ES3_ERR_COSMETIC", want, sizeof want) || !want[0])
        strcpy(want, "0202,1301,1302,1303,1601,1701");  /* every +0x25==0 record */
    if (!strcmp(want, "0") || !strcmp(want, "off")) return;

    self = GetModuleHandleA(NULL);
    if (!self || !GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof mi)) return;
    base = (unsigned char*)self;

    tab = es3_find_errtab(base, (size_t)mi.SizeOfImage);
    if (!tab) { jvs_diag("ERRGATE: no ES3 error table in this image -- nothing patched"); return; }

    for (s = want; *s; ) {
        unsigned code = (unsigned)strtoul(s, (char**)&s, 16);
        int i;
        while (*s == ',' || *s == ' ') s++;
        if (!code) continue;
        for (i = 0; i < ES3_ERRTAB_N; i++) {
            unsigned char* rec = tab + (size_t)i * ES3_ERRREC_SZ;
            const char* title;
            DWORD old = 0;
            if (*(unsigned*)rec != code) continue;
            title = (const char*)(uintptr_t)(*(unsigned long long*)(rec + 8));
            if (rec[ES3_GATE_OFF]) {
                jvs_diag("ERRGATE: %u-%u already cosmetic, left alone",
                         code >> 8, code & 0xFF);
                break;
            }
            if (!VirtualProtect(rec + ES3_GATE_OFF, 1, PAGE_READWRITE, &old)) {
                jvs_diag("ERRGATE: %u-%u VirtualProtect failed (%lu)",
                         code >> 8, code & 0xFF, GetLastError());
                break;
            }
            rec[ES3_GATE_OFF] = 1;
            VirtualProtect(rec + ES3_GATE_OFF, 1, old, &old);
            done++;
            jvs_diag("ERRGATE: %u-%u \"%s\" FATAL -> cosmetic; +0x25 was 0, which arms the"
                     " errmgr fatal latch and switches the cabinet to the scene that draws"
                     " nothing (gate byte at %p)",
                     code >> 8, code & 0xFF, title ? title : "?",
                     (void*)(rec + ES3_GATE_OFF));
            /* No break: 23-1 appears TWICE in this table (DATA BACKUP ERROR 1
             * and 2 share code 0x1701), and stopping here would leave the
             * second one able to halt the cabinet on its own. */
        }
    }
    if (!done)
        jvs_diag("ERRGATE: nothing changed (already cosmetic, or code not in table)");
}

/*  Install                                                                     */
/* -------------------------------------------------------------------------- */
int ultra_jvs_serial_install(void) {
    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = 1; }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) return 0;
    #define GET(name) (p##name)GetProcAddress(k32, #name)
    Real_CreateFileA        = GET(CreateFileA);
    Real_CreateFileW        = GET(CreateFileW);
    Real_ReadFile           = GET(ReadFile);
    Real_WriteFile          = GET(WriteFile);
    Real_CloseHandle        = GET(CloseHandle);
    Real_SetCommState       = GET(SetCommState);
    Real_GetCommState       = GET(GetCommState);
    Real_SetCommTimeouts    = GET(SetCommTimeouts);
    Real_PurgeComm          = GET(PurgeComm);
    Real_SetupComm          = GET(SetupComm);
    Real_ClearCommError     = GET(ClearCommError);
    Real_GetCommModemStatus = GET(GetCommModemStatus);
    Real_EscapeCommFunction = GET(EscapeCommFunction);
    Real_GetOverlappedResult= GET(GetOverlappedResult);
    Real_WaitCommEvent      = GET(WaitCommEvent);
    Real_SetCommMask        = GET(SetCommMask);
    Real_GetCommTimeouts    = GET(GetCommTimeouts);
    Real_ExitProcess        = GET(ExitProcess);
    Real_DeviceIoControl    = GET(DeviceIoControl);
    Real_GetProcAddress     = GET(GetProcAddress);
    Real_LoadLibraryA       = GET(LoadLibraryA);
    Real_LoadLibraryW       = GET(LoadLibraryW);
    #undef GET

    /* A registered per-game module's own on_install runs here (after Real_*
     * resolve) -- this is how game_dsii.c spawns its keyboard/direct-write
     * threads, game_chaosbreaker.c starts its probe thread, etc. Guard the
     * legacy Cave-specific block below on "no ops registered" so a module
     * that HAS been migrated never double-spawns its own threads alongside
     * the old inline path. */
    { const board_io_ops* ops = ultra_board_ops_get();
      if (ops && ops->on_install) ops->on_install(); }

    struct { const char* fn; void* hook; } H[] = {
        {"ExitProcess",(void*)Hook_ExitProcess},
        {"CreateFileA",(void*)Hook_CreateFileA},{"CreateFileW",(void*)Hook_CreateFileW},
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
        {"DeviceIoControl",(void*)Hook_DeviceIoControl},
        {"GetProcAddress",(void*)Hook_GetProcAddress},
        {"LoadLibraryA",(void*)Hook_LoadLibraryA},
        {"LoadLibraryW",(void*)Hook_LoadLibraryW},
    };
    for (size_t i = 0; i < sizeof(H)/sizeof(H[0]); ++i)
        ultra_iat_patch_all("kernel32.dll", H[i].fn, H[i].hook);

    /* d3d8 diagnostic (d3d8.dll is a static import of the game, already loaded) */
    ultra_iat_patch_all("d3d8.dll", "Direct3DCreate8", (void*)Hook_Direct3DCreate8);

    /* Graceful sibling create-fn hooks for non-D3D8 renderers. Only D3D8 gets the
     * sharpen/limiter today; these stubs log "sharpen unsupported, passthrough" and
     * forward so a future D3D9/D3D11/GL Type X dump degrades gracefully. The IAT
     * patch is a no-op when the game does not import that create-fn. */
    {
        extern void*   __stdcall ultra_Hook_Direct3DCreate9(UINT);
        extern HRESULT __stdcall ultra_Hook_D3D11CreateDeviceAndSwapChain(
            void*,int,void*,UINT,const void*,UINT,UINT,const void*,void**,void**,void*,void**);
        extern HRESULT __stdcall ultra_Hook_CreateDXGIFactory(const void*,void**);
        extern HRESULT __stdcall ultra_Hook_CreateDXGIFactory1(const void*,void**);
        extern BOOL    __stdcall ultra_Hook_wglSwapBuffers(HDC);
        extern BOOL    __stdcall ultra_Hook_SwapBuffers(HDC);
        ultra_iat_patch_all("d3d9.dll",  "Direct3DCreate9", (void*)ultra_Hook_Direct3DCreate9);
        ultra_iat_patch_all("d3d11.dll", "D3D11CreateDeviceAndSwapChain", (void*)ultra_Hook_D3D11CreateDeviceAndSwapChain);
        /* DXGI-factory render path: a bare device + IDXGIFactory::CreateSwapChain
         * (the Sega Nu/ALLS route). The d3d11 hook patches the returned factory's
         * vtable so any swap chain it makes is captured. */
        ultra_iat_patch_all("dxgi.dll",  "CreateDXGIFactory",  (void*)ultra_Hook_CreateDXGIFactory);
        ultra_iat_patch_all("dxgi.dll",  "CreateDXGIFactory1", (void*)ultra_Hook_CreateDXGIFactory1);
        ultra_iat_patch_all("opengl32.dll", "wglSwapBuffers", (void*)ultra_Hook_wglSwapBuffers);
        /* GL titles that swap via gdi32!SwapBuffers directly (wgl tail-calls it). */
        ultra_iat_patch_all("gdi32.dll", "SwapBuffers", (void*)ultra_Hook_SwapBuffers);
    }

    /* Name whoever ends the run. The game's WndProc forwards to DefWindowProcA,
     * so WM_CLOSE passes through here on its way to DestroyWindow. */
    {
        HMODULE u32 = GetModuleHandleA("user32.dll");
        if (u32) {
            if (!Real_DefWindowProcA)
                Real_DefWindowProcA = (pDefWindowProcA)GetProcAddress(u32, "DefWindowProcA");
            if (!Real_DefWindowProcW)
                Real_DefWindowProcW = (pDefWindowProcW)GetProcAddress(u32, "DefWindowProcW");
            if (!Real_PostQuitMessage)
                Real_PostQuitMessage = (pPostQuitMessage)GetProcAddress(u32, "PostQuitMessage");
            ultra_iat_patch_all("user32.dll", "DefWindowProcA",  (void*)Hook_DefWindowProcA);
            ultra_iat_patch_all("user32.dll", "DefWindowProcW",  (void*)Hook_DefWindowProcW);
            ultra_iat_patch_all("user32.dll", "PostQuitMessage", (void*)Hook_PostQuitMessage);
        }
    }

    /* LAN ERROR 1: let a working link count as a real one. */
    {
        HMODULE ip = GetModuleHandleA("iphlpapi.dll");
        if (!ip) ip = Real_LoadLibraryA ? Real_LoadLibraryA("iphlpapi.dll") : NULL;
        if (ip) {
            if (!Real_GetIfTable2)
                Real_GetIfTable2 = (pGetIfTable2)GetProcAddress(ip, "GetIfTable2");
            if (!Real_GetIpAddrTable)
                Real_GetIpAddrTable = (pGetIpAddrTable)GetProcAddress(ip, "GetIpAddrTable");
            /* Both, or neither: without the address table we cannot tell a real
             * adapter from its filter pseudo-interfaces, and promoting the
             * wrong one is what produced "address acquisition NG". */
            if (!Real_GetAdaptersInfo)
                Real_GetAdaptersInfo = (pGetAdaptersInfo)GetProcAddress(ip, "GetAdaptersInfo");
            if (!Real_GetAdaptersAddresses)
                Real_GetAdaptersAddresses = (pGetAdaptersAddresses)
                    GetProcAddress(ip, "GetAdaptersAddresses");
            if (Real_GetIfTable2 && Real_GetIpAddrTable)
                ultra_iat_patch_all("iphlpapi.dll", "GetIfTable2", (void*)Hook_GetIfTable2);
            else
                jvs_diag("LINK: iphlpapi missing GetIfTable2/GetIpAddrTable -- link spoof off");
            /* The cabinet subnet. All three enumeration APIs, because static
             * analysis could not say which one the check reads. */
            if (cabinet_ip()) {
                if (Real_GetIpAddrTable)
                    ultra_iat_patch_all("iphlpapi.dll", "GetIpAddrTable", (void*)Hook_GetIpAddrTable);
                if (Real_GetAdaptersInfo)
                    ultra_iat_patch_all("iphlpapi.dll", "GetAdaptersInfo", (void*)Hook_GetAdaptersInfo);
                if (Real_GetAdaptersAddresses)
                    ultra_iat_patch_all("iphlpapi.dll", "GetAdaptersAddresses",
                                        (void*)Hook_GetAdaptersAddresses);
            } else {
                jvs_diag("LAN: ULTRA_ES3_LAN_IP is off -- true addresses reported");
            }
        }
    }

    /* Last: the game image is fully mapped by now, so its .rdata is there to
     * read and the table lookup can verify itself against real content. */
    es3_error_gate_patch();
    return 1;
}

#endif /* _WIN32 */
