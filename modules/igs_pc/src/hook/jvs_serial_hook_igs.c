/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  jvs_serial_hook_igs.c  --  Win32 COM-port interception feeding the JVS
 *                              engine (IGS PC board's OWN independent copy)
 * =============================================================================
 *
 *  PROVENANCE: forked from modules/native_x86/src/hook/jvs_serial_hook.c into
 *  IGS PC's own independent hook tree (see igs_hook.h for the full rationale).
 *  No shared source file, no shared compiled binary with the multi-board hook
 *  or any other board's per-board hook. Trimmed of everything that file carries
 *  ONLY because it is shared across several boards with per-game quirks (the
 *  board_io_ops.h per-game seam, the Namco ES3 USB dongle bypass, the Taito
 *  host-cmd-0x04 poll ack that now lives in jvs_core_igs.c's own comment, the
 *  D3D11/DXGI/OpenGL present paths) -- none of that has evidence behind it for
 *  the IGS PC family (framework-only, no dump yet per docs/STOCK_TAKE.md). What
 *  IS kept is the generic, protocol-agnostic mechanism: fake-COM-port JVS
 *  transport, dongle-device / D:-drive path bypass, and the packed-game
 *  GetProcAddress interception for the D3D9 present hook this board DOES ship
 *  (d3d9_throttle_igs.c).
 *
 *  IGS PC games (generic JVS I/O family, distinct from IGS's PGM3/KOV3 HD
 *  board) are expected to talk to their I/O board over a serial port (RS-485),
 *  the same posture as Type X / Sega Nu / Namco ES boards. This intercepts the
 *  game's serial API so that, instead of a real port, the game's command bytes
 *  flow into Ultra Loader's clean-room JVS engine (jvs_core_igs.c) and the
 *  engine's replies flow back -- with controller state sourced from the loader
 *  via shared memory (or the standalone XInput fallback in hook_dllmain_igs.c).
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

#include <windows.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

#include "igs_hook.h"

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
static com_slot         g_slots[COM_SLOTS];
static int              g_slot_used[COM_SLOTS];
static CRITICAL_SECTION g_lock;
static int              g_lock_init = 0;

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
static pDeviceIoControl    Real_DeviceIoControl;
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
    igs_log("OPEN COM%d -> JVS fake handle (slot ok)", port);
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
        igs_log("D: redirect %s -> %s", name, buf);
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
        igs_log("CreateFileA: %s", name);
        int p = port_number_from_path(name);
        if (we_claim_port(p)) {
            HANDLE fh = open_fake_com(p);
            return fh;
        }
        if (is_dongle_device(name)) {
            const char* e = getenv("ULTRA_DONGLE_BYPASS");
            if (e && e[0]=='1') {
                HANDLE fk = CreateEventA(NULL, TRUE, FALSE, NULL);
                igs_log("  -> dongle device '%s' bypassed (fake handle=%p)", name, (void*)fk);
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
                igs_log("  -> fixup '%s' -> '%s'", rn, p);
                return h2;
            }
        }
        igs_log("  -> FAILED (err %lu)", (unsigned long)GetLastError());
    }
    return h;
}
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD share,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (name) {
        char a[256]; int n = WideCharToMultiByte(CP_ACP, 0, name, -1, a, sizeof(a)-1, NULL, NULL);
        if (n > 0) {
            a[n] = 0; igs_log("CreateFileW: %s", a);
            int p = port_number_from_path(a); if (we_claim_port(p)) return open_fake_com(p);
        }
    }
    return Real_CreateFileW(name, acc, share, sa, disp, flags, tmpl);
}

static BOOL WINAPI Hook_DeviceIoControl(HANDLE dev, DWORD code,
        LPVOID in_buf, DWORD in_sz, LPVOID out_buf, DWORD out_sz,
        LPDWORD bytes_ret, LPOVERLAPPED ov) {
    return Real_DeviceIoControl(dev, code, in_buf, in_sz,
                                out_buf, out_sz, bytes_ret, ov);
}

/* Diagnostics: where does the game die? */
static void WINAPI Hook_ExitProcess(UINT code) {
    igs_log("EXITPROCESS code=%u", code);
    Real_ExitProcess(code);
}

static BOOL WINAPI Hook_WriteFile(HANDLE h, LPCVOID buf, DWORD len,
                                  LPDWORD written, LPOVERLAPPED ov) {
    EnterCriticalSection(&g_lock);
    com_slot* s = slot_for(h);
    if (s) {
        ultra_jvs_node* node = igs_hook_node();
        igs_hook_pull_input(node);                       /* latest controller state */
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
        igs_log_hex("TX", (const unsigned char*)buf, len);
        igs_log_hex("RX", tmp, got);
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
            igs_log("ReadFile req=%lu give=%zu (queued=%zu)", (unsigned long)len, give, avail);
            LeaveCriticalSection(&g_lock);
            if (read) *read = (DWORD)give;
            complete_overlapped(h, ov, (DWORD)give);
            return TRUE;
        }
        igs_log("ReadFile req=%lu give=0 (queued=0)", (unsigned long)len);
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
    if (slot_for(h)) slot_free(h);
    LeaveCriticalSection(&g_lock);
    return Real_CloseHandle(h);   /* it is a real event handle either way */
}

/* comm-config shims: succeed silently for our fake port, pass through otherwise.
 * All log (ULTRA_JVS_LOG) so we can see the exact handshake sequence. */
#define COM_NOOP_TRUE(NAME, REAL, SIG, ARGS, TAG) \
    static BOOL WINAPI NAME SIG { \
        EnterCriticalSection(&g_lock); int ours = slot_for(h) != NULL; LeaveCriticalSection(&g_lock); \
        if (ours) { igs_log(TAG); return TRUE; } return REAL ARGS; }

COM_NOOP_TRUE(Hook_SetCommState,    Real_SetCommState,    (HANDLE h, LPDCB d),          (h,d), "SetCommState")
COM_NOOP_TRUE(Hook_SetCommTimeouts, Real_SetCommTimeouts, (HANDLE h, LPCOMMTIMEOUTS t), (h,t), "SetCommTimeouts")
COM_NOOP_TRUE(Hook_PurgeComm,       Real_PurgeComm,       (HANDLE h, DWORD f),          (h,f), "PurgeComm")

static BOOL WINAPI Hook_EscapeCommFunction(HANDLE h, DWORD f) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) { igs_log("EscapeCommFunction(%lu)", (unsigned long)f); return TRUE; }
    return Real_EscapeCommFunction(h,f);
}
static BOOL WINAPI Hook_GetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS t) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) { if (t) memset(t,0,sizeof(*t)); igs_log("GetCommTimeouts"); return TRUE; }
    return Real_GetCommTimeouts(h,t);
}
static BOOL WINAPI Hook_SetupComm(HANDLE h, DWORD in, DWORD out) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) { igs_log("SetupComm"); return TRUE; } return Real_SetupComm(h,in,out);
}
static BOOL WINAPI Hook_GetCommState(HANDLE h, LPDCB d) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) { if (d) { memset(d,0,sizeof(*d)); d->DCBlength=sizeof(*d); d->BaudRate=115200;
                         d->ByteSize=8; } igs_log("GetCommState"); return TRUE; }
    return Real_GetCommState(h,d);
}
static BOOL WINAPI Hook_ClearCommError(HANDLE h, LPDWORD err, LPCOMSTAT st) {
    EnterCriticalSection(&g_lock); com_slot* s = slot_for(h);
    if (s) { if (err) *err = 0;
             DWORD q = (DWORD)(s->resp_len - s->resp_pos);
             if (st) { memset(st,0,sizeof(*st)); st->cbInQue = q; }
             LeaveCriticalSection(&g_lock); igs_log("ClearCommError inQ=%lu", (unsigned long)q); return TRUE; }
    LeaveCriticalSection(&g_lock);
    return Real_ClearCommError(h,err,st);
}
static BOOL WINAPI Hook_GetCommModemStatus(HANDLE h, LPDWORD st) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) {
        /* The JVS SENSE line is read via modem status and is DYNAMIC: while our
         * node is unaddressed the host must see "node present, address me", and
         * once we've taken an address it must see "chain fully addressed" so the
         * host stops walking SET_ADDRESS. Same wiring proven on the Type X JVS
         * engine this was forked from (all lines clear = "address me", a set
         * line = "done"). Tunable via ULTRA_JVS_SENSE0 / ULTRA_JVS_SENSE1. */
        ultra_jvs_node* node = igs_hook_node();
        int addressed = (node && jvs_address(node) != 0);
        DWORD v;
        if (addressed) { const char* e = getenv("ULTRA_JVS_SENSE1"); v = (e && *e) ? (DWORD)strtoul(e,NULL,0) : (DWORD)MS_CTS_ON; }
        else           { const char* e = getenv("ULTRA_JVS_SENSE0"); v = (e && *e) ? (DWORD)strtoul(e,NULL,0) : 0u; }
        if (st) *st = v;
        igs_log("GetCommModemStatus -> 0x%lX (addressed=%d)", (unsigned long)v, addressed);
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
 * code resolving kernel32 functions dynamically still hits our I/O, and so a
 * PACKED game that resolves Direct3DCreate9 at runtime (LoadLibrary+GetProcAddress
 * instead of a static import) still gets the D3D9 present hook armed. */
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
            {"DeviceIoControl",(FARPROC)Hook_DeviceIoControl},
            {"GetProcAddress",(FARPROC)Hook_GetProcAddress},
            {"LoadLibraryA",(FARPROC)Hook_LoadLibraryA},
            {"LoadLibraryW",(FARPROC)Hook_LoadLibraryW},
        };
        for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); ++i)
            if (strcmp(name, map[i].n) == 0) return map[i].f;

        /* Packed-game present-hook entry point: a title with NO static d3d9 IAT
         * slot resolves the renderer create-fn at runtime via
         * LoadLibrary("d3d9.dll")+GetProcAddress(...,"Direct3DCreate9"). The
         * static IAT patch can't catch that (no slot exists), so intercept it
         * HERE and hand back our wrapper (d3d9_throttle_igs.c). */
        if (strcmp(name, "Direct3DCreate9") == 0) {
            extern void* __stdcall igs_Hook_Direct3DCreate9(UINT);
            igs_log("D3D9: GetProcAddress(\"Direct3DCreate9\") intercepted -> hook armed (packed path)");
            return (FARPROC)igs_Hook_Direct3DCreate9;
        }
    }
    return Real_GetProcAddress(mod, name);
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
int igs_jvs_serial_install(void) {
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
        igs_iat_patch_all("kernel32.dll", H[i].fn, H[i].hook);

    return 1;
}

#endif /* _WIN32 */
