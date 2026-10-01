/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  jvs_serial_hook_it.c  --  Win32 COM-port interception feeding the
 *                            Incredible Technologies board's own JVS engine
 * =============================================================================
 *
 *  IT's "nowboard" I/O (per this module's own research note) is generic JVS,
 *  the same RS-485/COM-port transport several other boards in this project
 *  use. This intercepts the game's serial API so that, instead of a real
 *  port, its command bytes flow into THIS board's own clean-room JVS engine
 *  (jvs_core_it.c) and the engine's replies flow back -- with controller
 *  state sourced from hook_dllmain_it.c's trackball(mouse)/keyboard poll
 *  thread via ul_it_node()/ul_it_pull_input().
 *
 *  Mechanism: IAT patching (no inline trampolines, no third-party hooking
 *  lib). We point every loaded module's import-address-table slot for the
 *  relevant kernel32 functions at our shims, and intercept GetProcAddress so
 *  late/dynamic resolution is caught too. Original pointers come straight
 *  from kernel32, so pass-through for non-COM handles is exact.
 *
 *  Independence: this file, jvs_core_it.c/h and it_hook.h are this board's
 *  complete, self-contained copy. modules/native_x86/src/hook/jvs_serial_hook.c
 *  was read for research only (structure/technique) -- no source or link-time
 *  dependency on it, or on any other board's copy, remains.
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

#include "it_hook.h"

/* -------------------------------------------------------------------------- */
/*  Fake COM handle table (one JVS bus; small table covers re-opens)           */
/* -------------------------------------------------------------------------- */
typedef struct {
    HANDLE   h;
    int      port;
    DWORD    evtmask;
    uint8_t  resp[4096];
    size_t   resp_pos, resp_len;
} com_slot;

#define COM_SLOTS 8
static com_slot        g_slots[COM_SLOTS];
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
typedef FARPROC(WINAPI *pGetProcAddress)(HMODULE,LPCSTR);
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
static pExitProcess        Real_ExitProcess;

/* -------------------------------------------------------------------------- */
/*  The shims                                                                   */
/* -------------------------------------------------------------------------- */
static HANDLE open_fake_com(int port) {
    HANDLE h = CreateEventA(NULL, FALSE /*auto-reset*/, FALSE, NULL);
    if (!h) return INVALID_HANDLE_VALUE;
    EnterCriticalSection(&g_lock);
    com_slot* s = slot_alloc(h, port);
    LeaveCriticalSection(&g_lock);
    if (!s) {
        Real_CloseHandle ? Real_CloseHandle(h) : (void)CloseHandle(h);
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return INVALID_HANDLE_VALUE;
    }
    ul_it_log("OPEN COM%d -> JVS fake handle (slot ok)\n", port);
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
        ul_it_log("CreateFileA: %s\n", name);
        int p = port_number_from_path(name);
        if (we_claim_port(p)) return open_fake_com(p);
    }
    HANDLE h = Real_CreateFileA(name, acc, share, sa, disp, flags, tmpl);
    if (h == INVALID_HANDLE_VALUE && name) {
        /* Flattened-dump path fixup: a game built for a deeper directory tree
         * (e.g. bin/x86/) uses ../../Data/ relative paths; when the dump puts
         * the exe at the root, strip leading ../ segments and retry. */
        const char *p = name;
        while (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\\'))
            p += 3;
        if (p != name) {
            HANDLE h2 = Real_CreateFileA(p, acc, share, sa, disp, flags, tmpl);
            if (h2 != INVALID_HANDLE_VALUE) {
                ul_it_log("  -> fixup '%s' -> '%s'\n", name, p);
                return h2;
            }
        }
        ul_it_log("  -> FAILED (err %lu)\n", (unsigned long)GetLastError());
    }
    return h;
}
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD share,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (name) {
        char a[256]; int n = WideCharToMultiByte(CP_ACP, 0, name, -1, a, sizeof(a)-1, NULL, NULL);
        if (n > 0) {
            a[n] = 0; ul_it_log("CreateFileW: %s\n", a);
            int p = port_number_from_path(a);
            if (we_claim_port(p)) return open_fake_com(p);
        }
    }
    return Real_CreateFileW(name, acc, share, sa, disp, flags, tmpl);
}

static void WINAPI Hook_ExitProcess(UINT code) {
    ul_it_log("EXITPROCESS code=%u\n", code);
    Real_ExitProcess(code);
}

static BOOL WINAPI Hook_WriteFile(HANDLE h, LPCVOID buf, DWORD len,
                                  LPDWORD written, LPOVERLAPPED ov) {
    EnterCriticalSection(&g_lock);
    com_slot* s = slot_for(h);
    if (s) {
        ultra_jvs_node* node = ul_it_node();
        ul_it_pull_input(node);                              /* latest trackball/keys */
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
    com_slot* s = slot_for(h);
    if (s) {
        size_t avail = s->resp_len - s->resp_pos;
        if (avail) {
            size_t give = (len < avail) ? len : avail;
            memcpy(buf, s->resp + s->resp_pos, give); s->resp_pos += give;
            LeaveCriticalSection(&g_lock);
            if (read) *read = (DWORD)give;
            complete_overlapped(h, ov, (DWORD)give);
            return TRUE;
        }
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

/* comm-config shims: succeed silently for our fake port, pass through otherwise. */
#define COM_NOOP_TRUE(NAME, REAL, SIG, ARGS) \
    static BOOL WINAPI NAME SIG { \
        EnterCriticalSection(&g_lock); int ours = slot_for(h) != NULL; LeaveCriticalSection(&g_lock); \
        if (ours) { return TRUE; } \
        return REAL ARGS; }

COM_NOOP_TRUE(Hook_SetCommState,    Real_SetCommState,    (HANDLE h, LPDCB d),          (h,d))
COM_NOOP_TRUE(Hook_SetCommTimeouts, Real_SetCommTimeouts, (HANDLE h, LPCOMMTIMEOUTS t), (h,t))
COM_NOOP_TRUE(Hook_PurgeComm,       Real_PurgeComm,       (HANDLE h, DWORD f),          (h,f))

static BOOL WINAPI Hook_EscapeCommFunction(HANDLE h, DWORD f) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) return TRUE;
    return Real_EscapeCommFunction(h,f);
}
static BOOL WINAPI Hook_GetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS t) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) { if (t) memset(t,0,sizeof(*t)); return TRUE; }
    return Real_GetCommTimeouts(h,t);
}
static BOOL WINAPI Hook_SetupComm(HANDLE h, DWORD in, DWORD out) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) return TRUE;
    return Real_SetupComm(h,in,out);
}
static BOOL WINAPI Hook_GetCommState(HANDLE h, LPDCB d) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) { if (d) { memset(d,0,sizeof(*d)); d->DCBlength=sizeof(*d); d->BaudRate=115200; d->ByteSize=8; } return TRUE; }
    return Real_GetCommState(h,d);
}
static BOOL WINAPI Hook_ClearCommError(HANDLE h, LPDWORD err, LPCOMSTAT st) {
    EnterCriticalSection(&g_lock); com_slot* s = slot_for(h);
    if (s) { if (err) *err = 0;
             DWORD q = (DWORD)(s->resp_len - s->resp_pos);
             if (st) { memset(st,0,sizeof(*st)); st->cbInQue = q; }
             LeaveCriticalSection(&g_lock); return TRUE; }
    LeaveCriticalSection(&g_lock);
    return Real_ClearCommError(h,err,st);
}
static BOOL WINAPI Hook_GetCommModemStatus(HANDLE h, LPDWORD st) {
    EnterCriticalSection(&g_lock); int ours = slot_for(h)!=NULL; LeaveCriticalSection(&g_lock);
    if (ours) {
        /* The JVS SENSE line is read via modem status and is DYNAMIC: while
         * our node is unaddressed the host must see "node present, address
         * me", and once addressed it must see "chain fully addressed" so the
         * host stops walking SET_ADDRESS. All lines clear = "address me", a
         * set line = "done" (same convention proven on other boards in this
         * project). Tunable via ULTRA_JVS_SENSE0 / ULTRA_JVS_SENSE1. */
        ultra_jvs_node* node = ul_it_node();
        int addressed = (node && jvs_address(node) != 0);
        DWORD v;
        if (addressed) { const char* e = ul_it_env("ULTRA_JVS_SENSE1"); v = (e && *e) ? (DWORD)strtoul(e,NULL,0) : (DWORD)MS_CTS_ON; }
        else           { const char* e = ul_it_env("ULTRA_JVS_SENSE0"); v = (e && *e) ? (DWORD)strtoul(e,NULL,0) : 0u; }
        if (st) *st = v;
        return TRUE;
    }
    return Real_GetCommModemStatus(h,st);
}
static BOOL WINAPI Hook_SetCommMask(HANDLE h, DWORD mask) {
    EnterCriticalSection(&g_lock); com_slot* s = slot_for(h); if (s) s->evtmask = mask;
    int ours = s != NULL; LeaveCriticalSection(&g_lock);
    if (ours) return TRUE;
    return Real_SetCommMask(h, mask);
}
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
    if (ours) {
        if (cnt && ov) *cnt = (DWORD)ov->InternalHigh;
        return TRUE;
    }
    return Real_GetOverlappedResult(h, ov, cnt, wait);
}

/* GetProcAddress interception: hand back our shim for the names we own so that
 * code resolving kernel32/d3d9 functions dynamically still hits our I/O. */
extern void* __stdcall ul_it_hook_direct3dcreate9(UINT);

static FARPROC WINAPI Hook_GetProcAddress(HMODULE mod, LPCSTR name) {
    if ((((uintptr_t)name) >> 16) != 0) {   /* by name, not ordinal */
        struct { const char* n; FARPROC f; } map[] = {
            {"CreateFileA",(FARPROC)Hook_CreateFileA},{"CreateFileW",(FARPROC)Hook_CreateFileW},
            {"ReadFile",(FARPROC)Hook_ReadFile},{"WriteFile",(FARPROC)Hook_WriteFile},
            {"CloseHandle",(FARPROC)Hook_CloseHandle},{"SetCommState",(FARPROC)Hook_SetCommState},
            {"GetCommState",(FARPROC)Hook_GetCommState},{"SetCommTimeouts",(FARPROC)Hook_SetCommTimeouts},
            {"PurgeComm",(FARPROC)Hook_PurgeComm},{"SetupComm",(FARPROC)Hook_SetupComm},
            {"GetCommTimeouts",(FARPROC)Hook_GetCommTimeouts},
            {"ClearCommError",(FARPROC)Hook_ClearCommError},
            {"GetCommModemStatus",(FARPROC)Hook_GetCommModemStatus},
            {"EscapeCommFunction",(FARPROC)Hook_EscapeCommFunction},
            {"SetCommMask",(FARPROC)Hook_SetCommMask},{"WaitCommEvent",(FARPROC)Hook_WaitCommEvent},
            {"GetOverlappedResult",(FARPROC)Hook_GetOverlappedResult},
            {"GetProcAddress",(FARPROC)Hook_GetProcAddress},
            {"ExitProcess",(FARPROC)Hook_ExitProcess},
        };
        for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); ++i)
            if (strcmp(name, map[i].n) == 0) return map[i].f;

        /* A packed/unpacked-at-runtime game may resolve d3d9.dll!Direct3DCreate9
         * dynamically instead of through a static IAT slot -- catch it here so
         * the D3D9 present throttle still arms. */
        if (strcmp(name, "Direct3DCreate9") == 0)
            return (FARPROC)ul_it_hook_direct3dcreate9;
    }
    return Real_GetProcAddress(mod, name);
}

/* -------------------------------------------------------------------------- */
/*  Install                                                                     */
/* -------------------------------------------------------------------------- */
int ul_it_jvs_serial_install(void) {
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
    Real_ExitProcess        = GET(ExitProcess);
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
    };
    for (size_t i = 0; i < sizeof(H)/sizeof(H[0]); ++i)
        ul_it_iat_patch_all("kernel32.dll", H[i].fn, H[i].hook);

    /* d3d9.dll is a static import of the game when not packed; the packed
     * path is caught above in Hook_GetProcAddress. */
    ul_it_iat_patch_all("d3d9.dll", "Direct3DCreate9", (void*)ul_it_hook_direct3dcreate9);

    ul_it_log("jvs_serial: installed (COM claim=%s)\n", ul_it_env_or("ULTRA_JVS_COM", "2,3 (default)"));
    return 1;
}

#endif /* _WIN32 */
