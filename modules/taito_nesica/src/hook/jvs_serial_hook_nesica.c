/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  jvs_serial_hook_nesica.c  --  Win32 COM-port interception feeding the JVS
 *                                 engine, taito_nesica's own independent copy.
 * =============================================================================
 *
 *  Taito NESiCAxLive (gen 1) runs on Type X / Type X Zero-class PC hardware and
 *  talks to its JVS I/O board over a serial port (RS-485 on COM2/COM3), same as
 *  the rest of the Taito PC-arcade family. This intercepts the game's serial API
 *  so that, instead of a real port, the game's command bytes flow into Ultra
 *  Loader's clean-room JVS engine (jvs_core_nesica.c) and the engine's replies
 *  flow back -- with controller state sourced from the loader via shared memory
 *  (or the standalone gamepad fallback in hook_dllmain_nesica.c).
 *
 *  Mechanism: IAT patching (no inline trampolines, no third-party hooking lib).
 *  We point every loaded module's import-address-table slot for the relevant
 *  kernel32 functions at our shims, and intercept GetProcAddress so late/dynamic
 *  resolution is caught too. Original pointers come straight from kernel32, so
 *  pass-through for non-COM handles is exact.
 *
 *  INDEPENDENCE: this file, and every other file in this src/hook/ tree, is
 *  taito_nesica's OWN copy -- compiled only into ultra_taito_nesica_hook{32,64}
 *  .dll. It does not link against, share an object file with, or depend on the
 *  output of native_x86/src/hook/ (the legacy shared ultra_typex_hook{32,64}
 *  .dll) or any other board module. Per-board/per-game logic that lived in the
 *  shared tree (Namco ES3 dongle bypass, Cave PC serial protocol, the
 *  board_io_ops per-game override layer, etc.) is deliberately NOT carried over
 *  here -- none of it applies to a NESiCAxLive title. The one addition specific
 *  to this board is the single, ordered "who owns this CreateFile" call into
 *  card_reader_nesica.c (see Hook_CreateFileA below) so the IC-card-reader
 *  presence stub and the JVS fake-COM path can coexist on ONE hook chain
 *  without one silently overwriting the other's IAT patch.
 *
 *  Clean-room: Ultra Loader's own code, built on the public Win32 API and the
 *  open JVS standard. No third-party loader source.
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

#include "jvs_core_nesica.h"

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
/*  Shared diagnostic sink (ULTRA_JVS_LOG=<file>). Every _nesica.c file in this
 *  tree externs this instead of each keeping its own logger, matching the
 *  shared-trace-sink pattern the rest of the project uses (there it is called
 *  ultra_d3d8_log; here, since this board carries no D3D8, it is simply named
 *  for what it is). */
/* -------------------------------------------------------------------------- */
void ultra_nesica_log(const char* fmt, ...) {
    static const char* path = NULL; static int checked = 0;
    if (!checked) { path = getenv("ULTRA_JVS_LOG"); checked = 1; }
    if (!path || !*path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}
static void nesica_log_hex(const char* tag, const unsigned char* b, size_t n) {
    if (!getenv("ULTRA_JVS_LOG")) return;
    char line[512]; size_t o = 0;
    o += (size_t)snprintf(line, sizeof(line), "%s [%zu]:", tag, n);
    for (size_t i = 0; i < n && i < 96 && o + 4 < sizeof(line); ++i)
        o += (size_t)snprintf(line + o, sizeof(line) - o, " %02X", b[i]);
    ultra_nesica_log("%s", line);
}

/* Provided by hook_dllmain_nesica.c */
ultra_jvs_node* ultra_hook_node(void);
void            ultra_hook_pull_input(ultra_jvs_node* n);

/* Provided by card_reader_nesica.c. Gets first refusal on any CreateFile* path
 * so the NESiCA/Aime card-reader stub and the JVS fake-COM path below share
 * ONE IAT hook chain instead of two independent installs racing to own the
 * same kernel32 slot (which is what would happen if card_reader_nesica.c
 * patched CreateFileA a second time on its own -- see the file header there
 * for why that pattern is deliberately avoided in this independent build). */
int ultra_card_reader_try_open(const char* name, HANDLE* out);

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
typedef FARPROC(WINAPI *pGetProcAddress)(HMODULE,LPCSTR);
typedef HMODULE(WINAPI *pLoadLibraryA)(LPCSTR);
typedef HMODULE(WINAPI *pLoadLibraryW)(LPCWSTR);
typedef void   (WINAPI *pExitProcess)(UINT);

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
static pGetProcAddress     Real_GetProcAddress;
static pLoadLibraryA       Real_LoadLibraryA;
static pLoadLibraryW       Real_LoadLibraryW;
static pExitProcess        Real_ExitProcess;

/* -------------------------------------------------------------------------- */
/*  The shims                                                                   */
/* -------------------------------------------------------------------------- */
/* A fake COM handle is a real, auto-reset Event the game can wait on, close, and
 * that we signal on I/O completion (overlapped serial code waits on it). */
static HANDLE open_fake_com(int port) {
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
    ultra_nesica_log("OPEN COM%d -> JVS fake handle (slot ok)", port);
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
/* Virtual D: drive -- Type X-family boards keep a writable data/backup
 * partition on D: (d:\game.dat, d:\history.dat ...). On a PC there is no such
 * drive, so redirect "d:\X" to a local writable folder (ULTRA_DDRIVE, default
 * ".\ul_vdrive_d"). NESiCA titles' per-title save data would live here too. */
static const char* redirect_d_drive(const char* name, char* buf, size_t cap) {
    if (name && (name[0]=='d'||name[0]=='D') && name[1]==':' && (name[2]=='\\'||name[2]=='/')) {
        const char* base = getenv("ULTRA_DDRIVE"); if (!base || !*base) base = "ul_vdrive_d";
        snprintf(buf, cap, "%s%s", base, name + 2);   /* base + "\game.dat" */
        ensure_parent_dirs(buf);
        ultra_nesica_log("D: redirect %s -> %s", name, buf);
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
    char rbuf[1024];
    const char* rn = name;
    if (name) {
        /* Keep the game's writes inside its own data folder - see above. */
        {   char ul_rdb[1024];
            if (ultra_redirect_cabinet_path(name, ul_rdb, sizeof ul_rdb)) {
                return Real_CreateFileA(ul_rdb, acc, share, sa, disp, flags, tmpl);
            }
        }
        ultra_nesica_log("CreateFileA: %s", name);
        /* The card-reader stub gets first refusal (env-gated, OFF by default --
         * see card_reader_nesica.c for the ethical DEFER boundary). */
        { HANDLE oh;
          if (ultra_card_reader_try_open(name, &oh)) return oh; }
        int p = port_number_from_path(name);
        if (we_claim_port(p)) {
            HANDLE fh = open_fake_com(p);
            return fh;
        }
        if (is_dongle_device(name)) {
            const char* e = getenv("ULTRA_DONGLE_BYPASS");
            if (e && e[0]=='1') {
                HANDLE fk = CreateEventA(NULL, TRUE, FALSE, NULL);
                ultra_nesica_log("  -> dongle device '%s' bypassed (fake handle=%p)", name, (void*)fk);
                return fk ? fk : (HANDLE)(uintptr_t)0xDEAD0001;
            }
        }
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
                ultra_nesica_log("  -> fixup '%s' -> '%s'", rn, p);
                return h2;
            }
        }
        ultra_nesica_log("  -> FAILED (err %lu)", (unsigned long)GetLastError());
    }
    return h;
}
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD share,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (name) {
        char a[256]; int n = WideCharToMultiByte(CP_ACP, 0, name, -1, a, sizeof(a)-1, NULL, NULL);
        if (n > 0) {
            a[n] = 0; ultra_nesica_log("CreateFileW: %s", a);
            HANDLE oh;
            if (ultra_card_reader_try_open(a, &oh)) return oh;
            int p = port_number_from_path(a); if (we_claim_port(p)) return open_fake_com(p);
        }
    }
    return Real_CreateFileW(name, acc, share, sa, disp, flags, tmpl);
}

/* Diagnostics: where does the game die? */
static void WINAPI Hook_ExitProcess(UINT code) {
    ultra_nesica_log("EXITPROCESS code=%u", code);
    Real_ExitProcess(code);
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
        uint8_t tmp[2048];
        size_t got = jvs_host_bytes(node, (const uint8_t*)buf, len, tmp, sizeof(tmp));
        size_t room = sizeof(s->resp) - s->resp_len;
        if (got > room) got = room;
        memcpy(s->resp + s->resp_len, tmp, got);
        s->resp_len += got;
        nesica_log_hex("TX", (const unsigned char*)buf, len);
        nesica_log_hex("RX", tmp, got);
        LeaveCriticalSection(&g_lock);
        if (written) *written = len;                       /* game wrote all of it */
        complete_overlapped(h, ov, len);                   /* signal async waiters */
        return TRUE;
    }
    LeaveCriticalSection(&g_lock);
    return Real_WriteFile(h, buf, len, written, ov);
}

static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD len,
                                 LPDWORD read, LPOVERLAPPED ov) {
    EnterCriticalSection(&g_lock);
    com_slot* s = slot_for(h);
    if (s) {
        size_t avail = s->resp_len - s->resp_pos;
        if (avail) {
            /* Return queued data from a prior WriteFile exchange first. */
            size_t give = (len < avail) ? len : avail;
            memcpy(buf, s->resp + s->resp_pos, give); s->resp_pos += give;
            ultra_nesica_log("ReadFile req=%lu give=%zu (queued=%zu)", (unsigned long)len, give, avail);
            LeaveCriticalSection(&g_lock);
            if (read) *read = (DWORD)give;
            complete_overlapped(h, ov, (DWORD)give);
            return TRUE;
        }
        ultra_nesica_log("ReadFile req=%lu give=0 (queued=0)", (unsigned long)len);
        LeaveCriticalSection(&g_lock);
        if (read) *read = 0;
        complete_overlapped(h, ov, 0);
        return TRUE;
    }
    LeaveCriticalSection(&g_lock);
    return Real_ReadFile(h, buf, len, read, ov);
}

static BOOL WINAPI Hook_CloseHandle(HANDLE h) {
    EnterCriticalSection(&g_lock);
    int ours = slot_for(h) != NULL;
    if (ours) slot_free(h);
    LeaveCriticalSection(&g_lock);
    return Real_CloseHandle(h);   /* it is a real event handle either way */
}

/* comm-config shims: succeed silently for our fake port, pass through otherwise.
 * All log (ULTRA_JVS_LOG) so we can see the exact handshake sequence. */
#define COM_NOOP_TRUE(NAME, REAL, SIG, ARGS, TAG) \
    static BOOL WINAPI NAME SIG { \
        EnterCriticalSection(&g_lock); int ours = slot_for(h) != NULL; LeaveCriticalSection(&g_lock); \
        if (ours) { ultra_nesica_log(TAG); return TRUE; } return REAL ARGS; }

COM_NOOP_TRUE(Hook_SetCommState,    Real_SetCommState,    (HANDLE h, LPDCB d),          (h,d), "SetCommState")
COM_NOOP_TRUE(Hook_SetCommTimeouts, Real_SetCommTimeouts, (HANDLE h, LPCOMMTIMEOUTS t), (h,t), "SetCommTimeouts")
COM_NOOP_TRUE(Hook_PurgeComm,       Real_PurgeComm,       (HANDLE h, DWORD f),          (h,f), "PurgeComm")

static BOOL WINAPI Hook_EscapeCommFunction(HANDLE h, DWORD f) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) { ultra_nesica_log("EscapeCommFunction(%lu)", (unsigned long)f); return TRUE; }
    return Real_EscapeCommFunction(h,f);
}
static BOOL WINAPI Hook_GetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS t) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) { if (t) memset(t,0,sizeof(*t)); ultra_nesica_log("GetCommTimeouts"); return TRUE; }
    return Real_GetCommTimeouts(h,t);
}
static BOOL WINAPI Hook_SetupComm(HANDLE h, DWORD in, DWORD out) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) { ultra_nesica_log("SetupComm"); return TRUE; } return Real_SetupComm(h,in,out);
}
static BOOL WINAPI Hook_GetCommState(HANDLE h, LPDCB d) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) { if (d) { memset(d,0,sizeof(*d)); d->DCBlength=sizeof(*d); d->BaudRate=115200;
                         d->ByteSize=8; } ultra_nesica_log("GetCommState"); return TRUE; }
    return Real_GetCommState(h,d);
}
static BOOL WINAPI Hook_ClearCommError(HANDLE h, LPDWORD err, LPCOMSTAT st) {
    EnterCriticalSection(&g_lock); com_slot* s = slot_for(h);
    if (s) { if (err) *err = 0;
             DWORD q = (DWORD)(s->resp_len - s->resp_pos);
             if (st) { memset(st,0,sizeof(*st)); st->cbInQue = q; }
             LeaveCriticalSection(&g_lock); ultra_nesica_log("ClearCommError inQ=%lu", (unsigned long)q); return TRUE; }
    LeaveCriticalSection(&g_lock);
    return Real_ClearCommError(h,err,st);
}
static BOOL WINAPI Hook_GetCommModemStatus(HANDLE h, LPDWORD st) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) {
        /* The JVS SENSE line is read via modem status and is DYNAMIC: while our
         * node is unaddressed the host must see "node present, address me", and
         * once we've taken an address it must see "chain fully addressed" so the
         * host stops walking SET_ADDRESS. Empirically (Taito jvs.lib, proven on
         * the Type X-family JVS lane this board shares): all lines clear =
         * "address me", a set line = "done". Tunable via ULTRA_JVS_SENSE0
         * (unaddressed) / ULTRA_JVS_SENSE1 (addressed). */
        ultra_jvs_node* node = ultra_hook_node();
        int addressed = (node && jvs_address(node) != 0);
        DWORD v;
        if (addressed) { const char* e = getenv("ULTRA_JVS_SENSE1"); v = (e && *e) ? (DWORD)strtoul(e,NULL,0) : (DWORD)MS_CTS_ON; }
        else           { const char* e = getenv("ULTRA_JVS_SENSE0"); v = (e && *e) ? (DWORD)strtoul(e,NULL,0) : 0u; }
        if (st) *st = v;
        ultra_nesica_log("GetCommModemStatus -> 0x%lX (addressed=%d)", (unsigned long)v, addressed);
        return TRUE;
    }
    return Real_GetCommModemStatus(h,st);
}
static BOOL WINAPI Hook_SetCommMask(HANDLE h, DWORD mask) {
    EnterCriticalSection(&g_lock); com_slot* s = slot_for(h); if (s) s->evtmask = mask;
    int ours = s != NULL; LeaveCriticalSection(&g_lock);
    if (ours) { return TRUE; } return Real_SetCommMask(h, mask);
}
/* Event-driven readers wait here for EV_RXCHAR. Our writes produce the response
 * synchronously, so a poll-then-wait pattern has data ready; report it at once. */
static BOOL WINAPI Hook_WaitCommEvent(HANDLE h, LPDWORD evt, LPOVERLAPPED ov) {
    EnterCriticalSection(&g_lock); com_slot* s = slot_for(h);
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
    if (ours) {                              /* our I/O always completed synchronously */
        if (cnt && ov) *cnt = (DWORD)ov->InternalHigh;
        return TRUE;
    }
    return Real_GetOverlappedResult(h, ov, cnt, wait);
}

/* GetProcAddress interception: hand back our shim for the names we own so that
 * code resolving kernel32 functions dynamically still hits our I/O. */
static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR name);
static HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR name);

static FARPROC WINAPI Hook_GetProcAddress(HMODULE mod, LPCSTR name) {
    if ((((uintptr_t)name) >> 16) != 0) {   /* by name, not ordinal */
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
            {"GetProcAddress",(FARPROC)Hook_GetProcAddress},
            {"LoadLibraryA",(FARPROC)Hook_LoadLibraryA},
            {"LoadLibraryW",(FARPROC)Hook_LoadLibraryW},
        };
        for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); ++i)
            if (strcmp(name, map[i].n) == 0) return map[i].f;

        /* Packed NLX1 repacks may have NO static d3d9 IAT slot -- the unpacker
         * resolves Direct3DCreate9 at runtime via LoadLibrary("d3d9.dll") +
         * GetProcAddress. The IAT patch below can't catch that (no slot exists),
         * so intercept it HERE too and hand back our present-hook wrapper
         * (d3d9_throttle_nesica.c). This is the canonical packed-game
         * present-hook entry point used across the project's D3D9 boards. */
        {
            extern void* __stdcall ultra_Hook_Direct3DCreate9_Nesica(UINT);
            if (strcmp(name, "Direct3DCreate9") == 0) {
                ultra_nesica_log("D3D9: GetProcAddress(\"Direct3DCreate9\") intercepted -> hook armed (packed path)");
                return (FARPROC)ultra_Hook_Direct3DCreate9_Nesica;
            }
        }
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
/* Non-static: card_reader_nesica.c, dinput8_emu_nesica.c, audio_hook_nesica.c
 * and d3d9_throttle_nesica.c's install path all patch their own target DLL
 * import name (dsound.dll / xaudio2_*.dll / dinput8.dll are DISTINCT import
 * names from the kernel32.dll / d3d9.dll patched here, so there is no double-
 * patch race on the same slot -- see the file header note above). */
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
    return Real_LoadLibraryA(name);
}
static HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR name) {
    return Real_LoadLibraryW(name);
}

/* -------------------------------------------------------------------------- */
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
    Real_GetProcAddress     = GET(GetProcAddress);
    Real_LoadLibraryA       = GET(LoadLibraryA);
    Real_LoadLibraryW       = GET(LoadLibraryW);
    #undef GET

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
        {"GetProcAddress",(void*)Hook_GetProcAddress},
        {"LoadLibraryA",(void*)Hook_LoadLibraryA},
        {"LoadLibraryW",(void*)Hook_LoadLibraryW},
    };
    for (size_t i = 0; i < sizeof(H)/sizeof(H[0]); ++i)
        ultra_iat_patch_all("kernel32.dll", H[i].fn, H[i].hook);

    /* D3D9 present-hook (this board's primary renderer -- NLX1 fighters are
     * 32-bit D3D9). d3d9.dll is a static import when not packed; the packed
     * fallback is the GetProcAddress interception above. IAT patch is a no-op
     * if the game doesn't import d3d9.dll!Direct3DCreate9 (e.g. a title that
     * only resolves it dynamically), which the GetProcAddress path then
     * catches instead. D3D11 (the rare NLX1 D3D11 repack) is intentionally
     * NOT present-hooked in this pass -- ULTRA_D3D11 is reserved for a future
     * extension; an unhooked D3D11 title still renders natively, it just does
     * not get the frame-limiter/upscale treatment d3d9_throttle_nesica.c
     * provides D3D9 titles. */
    {
        extern void* __stdcall ultra_Hook_Direct3DCreate9_Nesica(UINT);
        ultra_iat_patch_all("d3d9.dll", "Direct3DCreate9", (void*)ultra_Hook_Direct3DCreate9_Nesica);
    }
    return 1;
}

#endif /* _WIN32 */
