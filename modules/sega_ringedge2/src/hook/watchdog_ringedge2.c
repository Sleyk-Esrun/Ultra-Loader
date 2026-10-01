/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  watchdog_ringedge2.c -- "where did it stop?", answered without a debugger
 * =============================================================================
 *
 *  WHY THIS FILE EXISTS.
 *
 *  Caladrius AC reaches Present, answers one PCP exchange on 40190, and then
 *  goes silent. Every hook in this tree logs on an EVENT; a game that stops
 *  raising events produces no further lines, so the log cannot distinguish
 *  between the three things it could be doing:
 *
 *      (a) presenting frame after frame into a window nobody can see,
 *      (b) parked forever inside one call in frame 1,
 *      (c) gone -- the process exited and nothing said so.
 *
 *  Those have completely different fixes. This file is the instrument that
 *  separates them, and it does it from INSIDE the process so no debugger has
 *  to be attached to a live cabinet run.
 *
 *  WHAT IT MEASURES, and why each number is here.
 *
 *    present9 / present11  the D3D9 and D3D11 Present hooks' running counts.
 *                          Frame 2 existing at all is the single fact the log
 *                          could not state before: throttle() logs only on its
 *                          FIRST call (d3d9_present_ringedge2.c:167-176), so
 *                          "armed @ 60.00 fps" proves frame 1 and nothing more.
 *
 *    peekmsg / getmsg      the game's own main loop turning over. Caladrius'
 *                          loop head at 0x0041E268 calls PeekMessageA through
 *                          [0x5B62B0] every iteration (docs/CALADRIUS_NO_WINDOW.md
 *                          section 3), so this counter IS the loop counter --
 *                          and it is upstream of Present. peekmsg climbing
 *                          while present9 is frozen means the game is looping
 *                          and something inside the frame returns early;
 *                          BOTH frozen means the thread is parked in a call.
 *
 *    sleep / wait          how much of the silence is a deliberate wait.
 *
 *    jvs_w / jvs_r         JVS frames written/read on our fake COM handle
 *                          (jvs_serial_ringedge2.c). This bus was completely
 *                          dark in the log before -- Hook_ReadFile/WriteFile
 *                          logged nothing at all, so "no JVS traffic" and
 *                          "JVS traffic nobody logged" looked identical.
 *
 *    recv / send           winsock calls made BY THE GAME. Filtered by return
 *                          address so this hook's own PCP daemon, which lives
 *                          in this same process and talks on the same sockets,
 *                          cannot inflate the game's numbers.
 *
 *  WHAT IT DOES WHEN NOTHING MOVES.
 *
 *  If a whole interval passes with no Present, no PeekMessage and no GetMessage,
 *  the process is not making progress, and the watchdog samples every thread:
 *  program counter, stack pointer, and a conservative return-address scan of
 *  the top of that thread's stack, each resolved to module+RVA.
 *
 *  This is deliberately the same information `cdb -p <pid> -c "~*kv; q"` would
 *  print, for the same reason it was asked for: SILVER_AMP_R_ROM.exe has
 *  DYNAMICBASE=0 and ImageBase 0x400000, so every printed VA maps directly onto
 *  the addresses already worked out in docs/CALADRIUS_NO_WINDOW.md -- the main
 *  loop head 0x0041E268, 0x0042F290 -> amAuthGetCurrentStatus 0x0054E840, the
 *  pcpaIsBusy spin at 0x0054E920, and the frame-1 callees 0x0041D110 /
 *  0x004141B0. A PC landing in any of those names the stall outright.
 *
 *  SAFETY -- the rules this sampler obeys, because a diagnostic that deadlocks
 *  the thing it is diagnosing is worse than no diagnostic:
 *
 *    1. ONE THREAD AT A TIME. Suspend, capture, resume, move on. Never two
 *       threads suspended at once, so no lock-ordering deadlock is possible.
 *    2. NOTHING BUT GetThreadContext AND ReadProcessMemory RUNS WHILE A THREAD
 *       IS SUSPENDED. No logging, no fopen, no allocation, nothing that could
 *       need a lock the suspended thread is holding. The stack bytes are copied
 *       out first and decoded afterwards, once everyone is running again.
 *    3. ReadProcessMemory ON SELF, never a raw dereference: an unmapped stack
 *       or code page returns FALSE instead of faulting, and mingw has no SEH.
 *    4. ONLY WHEN STALLED. A healthy game is never suspended -- the sampler is
 *       gated on an interval with zero observed progress, and capped
 *       (ULTRA_RE2_WD_SAMPLES, default 6) so a permanently hung game cannot
 *       turn its own log into a flood.
 *
 *  IAT OWNERSHIP. This file hooks ONLY slots no other TU in this tree and no
 *  part of work/src/ultra_tracer.c already owns. Already taken, and therefore
 *  deliberately absent here: CreateFileA/W, ReadFile, WriteFile, CloseHandle,
 *  the whole SetCommState/... serial family, GetProcAddress, LoadLibraryA/W
 *  (jvs_serial_ringedge2.c); Direct3DCreate9, D3D11CreateDeviceAndSwapChain,
 *  CreateDXGIFactory/1 (the two present hooks); DirectSoundCreate/8,
 *  XAudio2Create (audio_ringedge2.c); WaitForSingleObject, GetLocalTime,
 *  GetSystemTime, TerminateProcess, RegOpenKeyExA, RegQueryValueExA,
 *  CoCreateInstance, connect (ultra_tracer.c). Re-hooking any of those is the
 *  IAT-churn DEP bug and the GetProcAddress self-recursion bug at once.
 *
 *  Every real function here is resolved by WALKING THE EXPORT TABLE, never
 *  through GetProcAddress -- GetProcAddress is itself hooked in this tree, and
 *  a wrapper that resolves itself recurses until the stack dies.
 *
 *  ENV
 *      ULTRA_RE2_WATCHDOG=0     disable this file entirely (default: on)
 *      ULTRA_RE2_WD_SEC=n       heartbeat interval in seconds (default 5)
 *      ULTRA_RE2_WD_SAMPLES=n   max stall samples to take (default 6)
 *      ULTRA_RE2_WD_QUIET=1     heartbeat only while something is moving
 * =============================================================================
 */
#if defined(_WIN32)

/* winsock2.h FIRST and BEFORE windows.h -- windows.h pulls in the ancient
 * winsock.h otherwise and SOCKET/WSAAPI end up defined the wrong way round.
 * pcp_daemon_ringedge2.c orders its includes the same way for the same
 * reason. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "ringedge2_hook.h"

/* ---------------------------------------------------------------------------
 *  Export-table resolver. Own copy, same reason d3d9_present_ringedge2.c has
 *  one: GetProcAddress is hooked in this tree.
 * ------------------------------------------------------------------------- */
static FARPROC wd_raw_export(HMODULE mod, const char* want) {
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos; IMAGE_NT_HEADERS* nt; IMAGE_DATA_DIRECTORY d;
    IMAGE_EXPORT_DIRECTORY* ed; DWORD *names, *funcs; WORD* ords; DWORD i;
    if (!base || !want) return NULL;
    dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!d.VirtualAddress || !d.Size) return NULL;
    ed    = (IMAGE_EXPORT_DIRECTORY*)(base + d.VirtualAddress);
    names = (DWORD*)(base + ed->AddressOfNames);
    ords  = (WORD*) (base + ed->AddressOfNameOrdinals);
    funcs = (DWORD*)(base + ed->AddressOfFunctions);
    for (i = 0; i < ed->NumberOfNames; ++i) {
        const char* nm = (const char*)(base + names[i]);
        if (strcmp(nm, want) == 0) {
            DWORD rva = funcs[ords[i]];
            if (rva >= d.VirtualAddress && rva < d.VirtualAddress + d.Size)
                return NULL;                      /* forwarded export */
            return (FARPROC)(void*)(base + rva);
        }
    }
    return NULL;
}

/* kernel32!GetProcAddress is never a forwarder; taking it raw gives us a
 * genuine resolver for the handful of Win10 exports that ARE forwarders
 * (the synch family lives in api-ms-win-core-synch-* on some builds). */
typedef FARPROC (WINAPI *pGetProcAddress)(HMODULE, LPCSTR);
static pGetProcAddress wd_gpa = NULL;

static FARPROC wd_resolve(HMODULE mod, const char* want) {
    FARPROC f = wd_raw_export(mod, want);
    if (f) return f;
    if (wd_gpa && mod) return wd_gpa(mod, want);
    return NULL;
}

/* ---------------------------------------------------------------------------
 *  Main-module geometry. No ASLR on this title, but read it anyway: a probe
 *  that assumes a base and is wrong logs nothing for a whole run while looking
 *  perfectly healthy.
 * ------------------------------------------------------------------------- */
static uintptr_t g_main_base = 0, g_main_end = 0;
static uintptr_t g_text_base = 0, g_text_end = 0;
static char      g_main_name[64] = "(main)";

static void wd_learn_main_module(void) {
    HMODULE m = GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER* dos; IMAGE_NT_HEADERS* nt; IMAGE_SECTION_HEADER* sec;
    char path[MAX_PATH]; const char* b;
    WORD i;
    if (!m) return;
    g_main_base = (uintptr_t)m;
    dos = (IMAGE_DOS_HEADER*)m;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    nt = (IMAGE_NT_HEADERS*)((BYTE*)m + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    g_main_end = g_main_base + nt->OptionalHeader.SizeOfImage;
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            uintptr_t s = g_main_base + sec->VirtualAddress;
            uintptr_t e = s + (sec->Misc.VirtualSize ? sec->Misc.VirtualSize
                                                     : sec->SizeOfRawData);
            if (!g_text_base || s < g_text_base) g_text_base = s;
            if (e > g_text_end) g_text_end = e;
        }
    }
    if (GetModuleFileNameA(m, path, sizeof path)) {
        b = strrchr(path, '\\'); b = b ? b + 1 : path;
        strncpy(g_main_name, b, sizeof g_main_name - 1);
        g_main_name[sizeof g_main_name - 1] = 0;
    }
}

static int wd_in_main(uintptr_t a) {
    return g_main_base && a >= g_main_base && a < g_main_end;
}
static int wd_in_main_text(uintptr_t a) {
    return g_text_base && a >= g_text_base && a < g_text_end;
}

/* address -> "module+0xRVA", or the raw VA when no module owns it. */
static void wd_where(uintptr_t a, char* out, size_t n) {
    HMODULE mod = NULL;
    char path[MAX_PATH];
    const char* b;
    if (!a) { _snprintf(out, n, "(null)"); out[n-1] = 0; return; }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &mod) && mod &&
        GetModuleFileNameA(mod, path, sizeof path)) {
        b = strrchr(path, '\\'); b = b ? b + 1 : path;
        _snprintf(out, n, "%s+0x%llX", b, (unsigned long long)(a - (uintptr_t)mod));
    } else {
        _snprintf(out, n, "0x%llX", (unsigned long long)a);
    }
    out[n-1] = 0;
}

/* ---------------------------------------------------------------------------
 *  Counters. Written from hot paths, so: interlocked, never logged there.
 * ------------------------------------------------------------------------- */
static volatile LONG g_c_peek = 0, g_c_getmsg = 0, g_c_sleep = 0;
static volatile LONG g_c_wait = 0, g_c_recv = 0, g_c_send = 0;
static volatile LONG g_c_thread = 0;
static volatile LONG g_sleep_ms = 0;

/* jvs_serial_ringedge2.c bumps these on the fake-COM path. */
volatile LONG ul_re2_jvs_writes = 0;
volatile LONG ul_re2_jvs_reads  = 0;

/* ---------------------------------------------------------------------------
 *  Observe-only hooks on slots nobody else in this process owns.
 * ------------------------------------------------------------------------- */
typedef void  (WINAPI *pSleep)(DWORD);
typedef DWORD (WINAPI *pSleepEx)(DWORD, BOOL);
typedef DWORD (WINAPI *pWFMO)(DWORD, const HANDLE*, BOOL, DWORD);
typedef HANDLE(WINAPI *pCreateThread)(LPSECURITY_ATTRIBUTES, SIZE_T,
                                      LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
typedef void  (WINAPI *pExitProcess)(UINT);
typedef BOOL  (WINAPI *pPeekMessageA)(LPMSG, HWND, UINT, UINT, UINT);
typedef BOOL  (WINAPI *pGetMessageA)(LPMSG, HWND, UINT, UINT);
typedef int   (WSAAPI *pRecv)(SOCKET, char*, int, int);
typedef int   (WSAAPI *pSend)(SOCKET, const char*, int, int);

static pSleep        Real_Sleep        = NULL;
static pSleepEx      Real_SleepEx      = NULL;
static pWFMO         Real_WFMO         = NULL;
static pCreateThread Real_CreateThread = NULL;
static pExitProcess  Real_ExitProcess  = NULL;
static pPeekMessageA Real_PeekMessageA = NULL;
static pPeekMessageA Real_PeekMessageW = NULL;
static pGetMessageA  Real_GetMessageA  = NULL;
static pGetMessageA  Real_GetMessageW  = NULL;
static pRecv         Real_recv         = NULL;
static pSend         Real_send         = NULL;

#define WD_RETADDR() ((uintptr_t)__builtin_return_address(0))

/* EVERY COUNTER BELOW IS FILTERED BY CALLER, and that is not a refinement --
 * it is what makes the numbers mean anything. This DLL runs six threads of its
 * own inside the game process (PCP daemon + its per-connection threads, the
 * gamepad fallback, the IAT re-patch worker, the pause-menu poll loop, this
 * watchdog), ul_re2_iat_patch_all patches EVERY loaded module including our
 * own, and several of those threads sleep, wait and pump messages constantly.
 * An unfiltered "peekmsg" would climb steadily while the game was stone dead --
 * i.e. the one measurement this file exists to make would read backwards. */
static void WINAPI Wd_Sleep(DWORD ms) {
    static long n = 0;
    if (wd_in_main(WD_RETADDR())) {
        InterlockedIncrement(&g_c_sleep);
        InterlockedExchangeAdd(&g_sleep_ms, (LONG)(ms > 60000 ? 60000 : ms));
        if (ms >= 100 && n < 24) {
            char w[MAX_PATH + 32];
            ++n;
            wd_where(WD_RETADDR(), w, sizeof w);
            ul_re2_log("[wd] Sleep(%lu ms) from %s", (unsigned long)ms, w);
        }
    }
    if (Real_Sleep) Real_Sleep(ms);
}

static DWORD WINAPI Wd_SleepEx(DWORD ms, BOOL alertable) {
    if (wd_in_main(WD_RETADDR())) {
        InterlockedIncrement(&g_c_sleep);
        InterlockedExchangeAdd(&g_sleep_ms, (LONG)(ms > 60000 ? 60000 : ms));
    }
    return Real_SleepEx ? Real_SleepEx(ms, alertable) : 0;
}

static DWORD WINAPI Wd_WaitForMultipleObjects(DWORD n, const HANDLE* h,
                                              BOOL all, DWORD ms) {
    static long logged = 0;
    DWORD t0 = GetTickCount(), rc, took;
    uintptr_t ra = WD_RETADDR();
    if (wd_in_main(ra)) InterlockedIncrement(&g_c_wait);
    rc = Real_WFMO ? Real_WFMO(n, h, all, ms) : WAIT_FAILED;
    took = GetTickCount() - t0;
    if (logged < 24 && wd_in_main(ra) && (took >= 1000 || (ms == INFINITE && took >= 250))) {
        char w[MAX_PATH + 32];
        ++logged;
        wd_where(ra, w, sizeof w);
        ul_re2_log("[wd] WaitForMultipleObjects(n=%lu,all=%d) blocked %lu ms "
                   "(timeout=%s) from %s -> %s",
                   (unsigned long)n, (int)all, (unsigned long)took,
                   ms == INFINITE ? "INFINITE" : "finite", w,
                   rc == WAIT_TIMEOUT ? "TIMEOUT" : "signaled/other");
    }
    return rc;
}

static HANDLE WINAPI Wd_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack,
                                     LPTHREAD_START_ROUTINE start, LPVOID arg,
                                     DWORD flags, LPDWORD tid) {
    HANDLE h;
    LONG n = InterlockedIncrement(&g_c_thread);
    h = Real_CreateThread ? Real_CreateThread(sa, stack, start, arg, flags, tid)
                          : NULL;
    if (n <= 24) {
        char ws[MAX_PATH + 32], wc[MAX_PATH + 32];
        wd_where((uintptr_t)start, ws, sizeof ws);
        wd_where(WD_RETADDR(), wc, sizeof wc);
        ul_re2_log("[wd] CreateThread #%ld start=%s tid=%lu (from %s)",
                   (long)n, ws, tid ? (unsigned long)*tid : 0ul, wc);
    }
    return h;
}

/* "The process exited silently" is one of the three outcomes this whole file
 * exists to tell apart, and it is the only one that leaves no other trace. */
static void WINAPI Wd_ExitProcess(UINT code) {
    char w[MAX_PATH + 32];
    wd_where(WD_RETADDR(), w, sizeof w);
    ul_re2_log("[wd] *** ExitProcess(%u) from %s -- the game is ending itself",
               code, w);
    if (Real_ExitProcess) Real_ExitProcess(code);
}

/* THE LOOP COUNTER. Caladrius' main loop head at 0x0041E268 calls PeekMessageA
 * through [0x5B62B0] on every iteration, so this counter IS that loop's
 * iteration count -- and it sits UPSTREAM of Present, which is what makes it
 * able to say something Present cannot: whether the game is still going round
 * at all when no frame comes out the other end.
 *
 * Count only, never log: this is called thousands of times a second and one
 * fopen here would cost more than the frame it is measuring. The caller test
 * is two compares against globals, which is nothing next to the syscall. */
static BOOL WINAPI Wd_PeekMessageA(LPMSG m, HWND h, UINT a, UINT b, UINT r) {
    if (wd_in_main(WD_RETADDR())) InterlockedIncrement(&g_c_peek);
    return Real_PeekMessageA ? Real_PeekMessageA(m, h, a, b, r) : FALSE;
}
static BOOL WINAPI Wd_PeekMessageW(LPMSG m, HWND h, UINT a, UINT b, UINT r) {
    if (wd_in_main(WD_RETADDR())) InterlockedIncrement(&g_c_peek);
    return Real_PeekMessageW ? Real_PeekMessageW(m, h, a, b, r) : FALSE;
}
static BOOL WINAPI Wd_GetMessageA(LPMSG m, HWND h, UINT a, UINT b) {
    if (wd_in_main(WD_RETADDR())) InterlockedIncrement(&g_c_getmsg);
    return Real_GetMessageA ? Real_GetMessageA(m, h, a, b) : FALSE;
}
static BOOL WINAPI Wd_GetMessageW(LPMSG m, HWND h, UINT a, UINT b) {
    if (wd_in_main(WD_RETADDR())) InterlockedIncrement(&g_c_getmsg);
    return Real_GetMessageW ? Real_GetMessageW(m, h, a, b) : FALSE;
}

/* Winsock, filtered by CALLER. pcp_daemon_ringedge2.c runs in this same
 * process and talks on these same sockets; without the filter its traffic
 * would be indistinguishable from the game's and the counter would be a lie. */
static int WSAAPI Wd_recv(SOCKET s, char* b, int n, int f) {
    int rc;
    uintptr_t ra = WD_RETADDR();
    DWORD t0;
    if (!wd_in_main(ra)) return Real_recv ? Real_recv(s, b, n, f) : -1;
    InterlockedIncrement(&g_c_recv);
    t0 = GetTickCount();
    rc = Real_recv ? Real_recv(s, b, n, f) : -1;
    {
        static long logged = 0;
        DWORD took = GetTickCount() - t0;
        if (logged < 24) {
            ++logged;
            ul_re2_log("[wd] game recv(%d) took %lu ms -> %d%s", n,
                       (unsigned long)took, rc,
                       rc == 0 ? "  (peer closed)" : "");
        }
    }
    return rc;
}
static int WSAAPI Wd_send(SOCKET s, const char* b, int n, int f) {
    uintptr_t ra = WD_RETADDR();
    if (wd_in_main(ra)) {
        static long logged = 0;
        InterlockedIncrement(&g_c_send);
        if (logged < 24) {
            int show = n > 96 ? 96 : n;
            ++logged;
            ul_re2_log("[wd] game send(%d) '%.*s'", n, show, b ? b : "");
        }
    }
    return Real_send ? Real_send(s, b, n, f) : -1;
}

/* ---------------------------------------------------------------------------
 *  Window state. "Presenting into a window you cannot see" and "stopped after
 *  one frame" look the same in a log that never says where the window is.
 * ------------------------------------------------------------------------- */
typedef BOOL (WINAPI *pIsHungAppWindow)(HWND);
static pIsHungAppWindow Real_IsHungAppWindow = NULL;

static void wd_log_window(void) {
    HWND w = ul_re2_d3d9_window();
    HWND fg;
    RECT r;
    DWORD_PTR res = 0;
    int responded;
    if (!w) w = ul_re2_d3d11_window();
    if (!w) { ul_re2_log("[wd] window: NONE yet (no D3D9/D3D11 device window)"); return; }
    if (!IsWindow(w)) { ul_re2_log("[wd] window: hwnd=%p IS NO LONGER A WINDOW", (void*)w); return; }
    memset(&r, 0, sizeof r);
    GetWindowRect(w, &r);
    fg = GetForegroundWindow();
    /* SMTO_ABORTIFHUNG returns 0 immediately when the owning thread is not
     * pumping messages -- a direct read of "is the UI thread alive". */
    responded = SendMessageTimeoutA(w, WM_NULL, 0, 0,
                                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &res) ? 1 : 0;
    ul_re2_log("[wd] window: hwnd=%p rect=(%ld,%ld)-(%ld,%ld) %ldx%ld "
               "visible=%d iconic=%d zoomed=%d foreground=%d pumping=%d hung=%d "
               "style=0x%08lX exstyle=0x%08lX",
               (void*)w, r.left, r.top, r.right, r.bottom,
               r.right - r.left, r.bottom - r.top,
               IsWindowVisible(w) ? 1 : 0, IsIconic(w) ? 1 : 0,
               IsZoomed(w) ? 1 : 0, (fg == w) ? 1 : 0, responded,
               Real_IsHungAppWindow ? (Real_IsHungAppWindow(w) ? 1 : 0) : -1,
               (unsigned long)GetWindowLongA(w, GWL_STYLE),
               (unsigned long)GetWindowLongA(w, GWL_EXSTYLE));
}

/* ---------------------------------------------------------------------------
 *  The stall sampler.
 * ------------------------------------------------------------------------- */
#define WD_MAXTHREADS 48
#define WD_STACKCOPY  2048
#define WD_MAXFRAMES  10

typedef struct {
    DWORD     tid;
    int       got;
    uintptr_t pc, sp;
    BYTE      stack[WD_STACKCOPY];
    SIZE_T    stack_got;
} wd_sample;

/* Does `a` look like a return address -- i.e. is it preceded by a call?
 * Conservative on purpose: a false frame in a stall report costs more than a
 * missing one. Reads code bytes through ReadProcessMemory so an unmapped page
 * is a FALSE, not a fault (mingw has no SEH to catch one). */
static int wd_looks_like_retaddr(uintptr_t a) {
    BYTE b[8];
    SIZE_T got = 0;
    if (!wd_in_main_text(a)) return 0;
    if (a < 8) return 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(a - 8), b, 8, &got) || got != 8)
        return 0;
    if (b[3] == 0xE8) return 1;                       /* call rel32          */
    if (b[2] == 0xFF && b[3] == 0x15) return 1;       /* call dword ptr [m32]*/
    if (b[6] == 0xFF && (b[7] & 0x38) == 0x10) return 1;  /* call r/m, 2-byte */
    if (b[5] == 0xFF && (b[6] & 0x38) == 0x10) return 1;  /* call r/m, 3-byte */
    return 0;
}

static void wd_take_samples(int sample_no) {
    HANDLE snap;
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    static wd_sample s[WD_MAXTHREADS];
    static uintptr_t prev_pc[WD_MAXTHREADS];
    static DWORD     prev_tid[WD_MAXTHREADS];
    static int       prev_n = 0;
    int n = 0, i, j;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        ul_re2_log("[wd] stall sample #%d: CreateToolhelp32Snapshot failed err=%lu",
                   sample_no, (unsigned long)GetLastError());
        return;
    }

    memset(&te, 0, sizeof te);
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            HANDLE h;
            CONTEXT ctx;
            if (te.th32OwnerProcessID != pid) continue;
            if (te.th32ThreadID == me) continue;
            if (n >= WD_MAXTHREADS) break;

            h = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                           THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;

            s[n].tid = te.th32ThreadID;
            s[n].got = 0;
            s[n].pc = s[n].sp = 0;
            s[n].stack_got = 0;

            /* ---- SUSPENDED WINDOW BEGINS. Nothing but GetThreadContext and
             * ReadProcessMemory may run between here and ResumeThread: a
             * suspended thread may hold the CRT, heap or loader lock, and a
             * single fopen here would deadlock the game for good. ---- */
            if (SuspendThread(h) != (DWORD)-1) {
                memset(&ctx, 0, sizeof ctx);
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(h, &ctx)) {
#ifdef _WIN64
                    s[n].pc = (uintptr_t)ctx.Rip;
                    s[n].sp = (uintptr_t)ctx.Rsp;
#else
                    s[n].pc = (uintptr_t)ctx.Eip;
                    s[n].sp = (uintptr_t)ctx.Esp;
#endif
                    s[n].got = 1;
                    if (s[n].sp)
                        ReadProcessMemory(GetCurrentProcess(), (LPCVOID)s[n].sp,
                                          s[n].stack, WD_STACKCOPY, &s[n].stack_got);
                }
                ResumeThread(h);
            }
            /* ---- SUSPENDED WINDOW ENDS. Logging is safe again. ---- */

            CloseHandle(h);
            if (s[n].got) ++n;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);

    ul_re2_log("[wd] ==== STALL SAMPLE #%d: %d thread(s), main module %s at 0x%llX "
               "(code 0x%llX-0x%llX) ====",
               sample_no, n, g_main_name, (unsigned long long)g_main_base,
               (unsigned long long)g_text_base, (unsigned long long)g_text_end);

    for (i = 0; i < n; ++i) {
        char wpc[MAX_PATH + 32];
        const char* same = "";
        wd_where(s[i].pc, wpc, sizeof wpc);
        for (j = 0; j < prev_n; ++j)
            if (prev_tid[j] == s[i].tid && prev_pc[j] == s[i].pc) { same = "  [UNCHANGED since last sample]"; break; }
        ul_re2_log("[wd]   tid=%-6lu pc=%s%s%s",
                   (unsigned long)s[i].tid, wpc,
                   wd_in_main(s[i].pc) ? "   <== GAME CODE" : "", same);

        /* Conservative return-address scan of the top of this thread's stack.
         * Only addresses inside the main module's executable sections and
         * preceded by a call opcode are printed, so this is a list of the
         * game's own frames, not a hex dump. */
        {
            int shown = 0;
            SIZE_T off;
            for (off = 0; off + sizeof(uintptr_t) <= s[i].stack_got &&
                          shown < WD_MAXFRAMES; off += sizeof(uintptr_t)) {
                uintptr_t v;
                char wf[MAX_PATH + 32];
                memcpy(&v, s[i].stack + off, sizeof v);
                if (!wd_looks_like_retaddr(v)) continue;
                wd_where(v, wf, sizeof wf);
                ++shown;
                ul_re2_log("[wd]       frame %2d  sp+0x%03llX  %s   (VA 0x%llX)",
                           shown, (unsigned long long)off, wf,
                           (unsigned long long)v);
            }
            if (!shown && wd_in_main(s[i].pc))
                ul_re2_log("[wd]       (no game return addresses on the top "
                           "%d bytes of its stack)", (int)s[i].stack_got);
        }
    }

    for (i = 0; i < n; ++i) { prev_tid[i] = s[i].tid; prev_pc[i] = s[i].pc; }
    prev_n = n;
}

/* ---------------------------------------------------------------------------
 *  The heartbeat thread.
 * ------------------------------------------------------------------------- */
static volatile LONG g_wd_started = 0;

static int wd_env_int(const char* name, int dflt, int lo, int hi) {
    const char* v = ul_re2_env(name);
    int n;
    if (!v || !*v) return dflt;
    n = atoi(v);
    if (n < lo || n > hi) return dflt;
    return n;
}

static void wd_arm_hooks(void) {
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    HMODULE u32 = GetModuleHandleA("user32.dll");
    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");

    if (!wd_gpa && k32) wd_gpa = (pGetProcAddress)(void*)wd_raw_export(k32, "GetProcAddress");

    if (k32) {
        if (!Real_Sleep)        Real_Sleep        = (pSleep)(void*)wd_resolve(k32, "Sleep");
        if (!Real_SleepEx)      Real_SleepEx      = (pSleepEx)(void*)wd_resolve(k32, "SleepEx");
        if (!Real_WFMO)         Real_WFMO         = (pWFMO)(void*)wd_resolve(k32, "WaitForMultipleObjects");
        if (!Real_CreateThread) Real_CreateThread = (pCreateThread)(void*)wd_resolve(k32, "CreateThread");
        if (!Real_ExitProcess)  Real_ExitProcess  = (pExitProcess)(void*)wd_resolve(k32, "ExitProcess");
        if (Real_Sleep)        ul_re2_iat_patch_all("kernel32.dll", "Sleep",        (void*)Wd_Sleep);
        if (Real_SleepEx)      ul_re2_iat_patch_all("kernel32.dll", "SleepEx",      (void*)Wd_SleepEx);
        if (Real_WFMO)         ul_re2_iat_patch_all("kernel32.dll", "WaitForMultipleObjects", (void*)Wd_WaitForMultipleObjects);
        if (Real_CreateThread) ul_re2_iat_patch_all("kernel32.dll", "CreateThread", (void*)Wd_CreateThread);
        if (Real_ExitProcess)  ul_re2_iat_patch_all("kernel32.dll", "ExitProcess",  (void*)Wd_ExitProcess);
    }
    if (u32) {
        if (!Real_IsHungAppWindow)
            Real_IsHungAppWindow = (pIsHungAppWindow)(void*)wd_resolve(u32, "IsHungAppWindow");
        if (!Real_PeekMessageA) Real_PeekMessageA = (pPeekMessageA)(void*)wd_resolve(u32, "PeekMessageA");
        if (!Real_PeekMessageW) Real_PeekMessageW = (pPeekMessageA)(void*)wd_resolve(u32, "PeekMessageW");
        if (!Real_GetMessageA)  Real_GetMessageA  = (pGetMessageA)(void*)wd_resolve(u32, "GetMessageA");
        if (!Real_GetMessageW)  Real_GetMessageW  = (pGetMessageA)(void*)wd_resolve(u32, "GetMessageW");
        if (Real_PeekMessageA) ul_re2_iat_patch_all("user32.dll", "PeekMessageA", (void*)Wd_PeekMessageA);
        if (Real_PeekMessageW) ul_re2_iat_patch_all("user32.dll", "PeekMessageW", (void*)Wd_PeekMessageW);
        if (Real_GetMessageA)  ul_re2_iat_patch_all("user32.dll", "GetMessageA",  (void*)Wd_GetMessageA);
        if (Real_GetMessageW)  ul_re2_iat_patch_all("user32.dll", "GetMessageW",  (void*)Wd_GetMessageW);
    }
    if (ws2) {
        if (!Real_recv) Real_recv = (pRecv)(void*)wd_resolve(ws2, "recv");
        if (!Real_send) Real_send = (pSend)(void*)wd_resolve(ws2, "send");
        if (Real_recv) ul_re2_iat_patch_all("ws2_32.dll", "recv", (void*)Wd_recv);
        if (Real_send) ul_re2_iat_patch_all("ws2_32.dll", "send", (void*)Wd_send);
    }
}

static DWORD WINAPI wd_thread(LPVOID unused) {
    int sec      = wd_env_int("ULTRA_RE2_WD_SEC", 5, 1, 300);
    int maxsamp  = wd_env_int("ULTRA_RE2_WD_SAMPLES", 6, 0, 64);
    int quiet    = ul_re2_env_on("ULTRA_RE2_WD_QUIET");
    DWORD t0     = GetTickCount();
    LONG  p9 = 0, p11 = 0, peek = 0, getm = 0, jw = 0, jr = 0;
    int   samples = 0, ticks = 0, rearm = 0;
    (void)unused;
    DWORD last_ms = GetTickCount();   /* see the dt note in the sample loop */

    wd_learn_main_module();
    ul_re2_log("[wd] watchdog armed: every %d s, main module %s base=0x%llX "
               "code=0x%llX-0x%llX, max %d stall samples",
               sec, g_main_name, (unsigned long long)g_main_base,
               (unsigned long long)g_text_base, (unsigned long long)g_text_end,
               maxsamp);

    for (;;) {
        LONG n9, n11, npeek, ngetm, njw, njr;
        long amc_init, amc_head;
        DWORD now_ms; double dt_s;
        int moved;
        double t;

        if (Real_Sleep) Real_Sleep((DWORD)sec * 1000);
        else            Sleep((DWORD)sec * 1000);
        ++ticks;

        /* Catch modules the game loaded after us (delay-loaded imports, render
         * plugins). Idempotent: ul_re2_iat_patch_all leaves a slot that is
         * already ours completely alone, which is what keeps this off the
         * VirtualProtect-churn DEP path. */
        if (rearm < 12) { wd_arm_hooks(); ++rearm; }

        amc_init = -1; amc_head = -1;
        /* WALL-CLOCK GAP BETWEEN SAMPLES.
         *
         * Without this a frozen process is indistinguishable from a wedged one.
         * In the 2026-09-18 17:26 run WerFault suspended the process for ~12 s
         * to write a 290 MB crash dump, so the t=5 s and t=10 s ticks never
         * fired and the single surviving sample -- taken at wall-clock 16.3 s
         * but carrying counters frozen at about 4 s -- read as "still hung".
         * That would have sent the next session back to disassembling a stall
         * that was not there. If dt is far past the sample interval, the numbers
         * in that line describe some earlier moment, not now. */
        now_ms = GetTickCount();
        dt_s   = (double)(now_ms - last_ms) / 1000.0;
        last_ms = now_ms;
        n9    = (LONG)ul_re2_d3d9_present_count();
        n11   = (LONG)ul_re2_d3d11_present_count();
        npeek = g_c_peek;
        ngetm = g_c_getmsg;
        njw   = ul_re2_jvs_writes;
        njr   = ul_re2_jvs_reads;
        moved = (n9 != p9) || (n11 != p11) || (npeek != peek) || (ngetm != getm);
        /* The game's own I/O bring-up state, read straight out of its globals.
         * amCredit's init flag is 0 when amCreditInit() never ran, which is
         * what strands the per-frame credit poll -- see amcredit_ringedge2.c.
         * Printing it means a stall no longer has to be re-derived from a
         * disassembly every time: the log says which half is missing. */
        ul_re2_amcredit_state(&amc_init, &amc_head);
        t     = (double)(GetTickCount() - t0) / 1000.0;

        if (!quiet || moved) {
            ul_re2_log("[wd] t=%6.1fs  present9=%ld (+%ld, %.1f fps)  present11=%ld (+%ld)  "
                       "peekmsg=%ld (+%ld)  getmsg=%ld (+%ld)  jvs_w=%ld (+%ld) jvs_r=%ld (+%ld)  "
                       "sleep=%ld (%ld ms)  wait=%ld  recv=%ld send=%ld  threads_created=%ld  "
                       "amcredit_init=%ld ring_head=%ld  dt=%.1fs%s",
                       t, (long)n9, (long)(n9 - p9), (double)(n9 - p9) / (double)sec,
                       (long)n11, (long)(n11 - p11),
                       (long)npeek, (long)(npeek - peek),
                       (long)ngetm, (long)(ngetm - getm),
                       (long)njw, (long)(njw - jw), (long)njr, (long)(njr - jr),
                       (long)g_c_sleep, (long)g_sleep_ms, (long)g_c_wait,
                       (long)g_c_recv, (long)g_c_send, (long)g_c_thread,
                       amc_init, amc_head, dt_s,
                       (dt_s > (double)sec * 2.0)
                           ? "  <- PROCESS WAS SUSPENDED; counters above are stale"
                           : "");
            wd_log_window();
        }

        /* THE BUDGET FOLLOWS THE LAST STALL, NOT THE FIRST.
         *
         * Without this reset the whole sample budget is spent on the first
         * no-progress window of the run and latches forever. Measured against
         * the real log: the first Present happens EARLY -- before amdsc.ini,
         * before the mxjvs open, before ~25 HID opens and before eleven rom\*.mma
         * reads totalling roughly 190 MB off the I: drive. Those reads are a
         * legitimate synchronous load during which present9 does not advance,
         * so every sample would be taken there, all six naming ReadFile inside
         * ntdll, and the budget would be gone long before the game parks after
         * current.status -- the stall this instrument exists to catch. The
         * advice to "leave it 45 seconds" was exactly inverted by the same bug.
         *
         * Progress means the previous stall is over and did not matter. */
        if (moved)
            samples = 0;

        if (!moved && samples < maxsamp) {
            ++samples;
            ul_re2_log("[wd] NO PROGRESS in the last %d s -- present, PeekMessage and "
                       "GetMessage all frozen. Sampling every thread.", sec);
            wd_take_samples(samples);
            if (samples == maxsamp)
                ul_re2_log("[wd] stall-sample budget spent (%d). Heartbeat continues; "
                           "raise ULTRA_RE2_WD_SAMPLES for more.", maxsamp);
        }

        p9 = n9; p11 = n11; peek = npeek; getm = ngetm; jw = njw; jr = njr;
        if (ticks > 100000) break;   /* ~5.8 days at the default interval */
    }
    return 0;
}

void ul_re2_watchdog_install(void) {
    const char* e = ul_re2_env("ULTRA_RE2_WATCHDOG");
    HANDLE h;
    if (e && e[0] == '0') {
        ul_re2_log("[wd] disabled (ULTRA_RE2_WATCHDOG=0)");
        return;
    }
    if (InterlockedCompareExchange(&g_wd_started, 1, 0) != 0) return;

    wd_learn_main_module();
    wd_arm_hooks();

    /* CreateThread, not Real_CreateThread: at install time we are inside
     * DllMain and the IAT patch may not have landed yet. Either way this
     * thread is ours and the counter counting it once is harmless. */
    h = CreateThread(NULL, 0, wd_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else   ul_re2_log("[wd] CreateThread failed err=%lu", (unsigned long)GetLastError());
}

#endif /* _WIN32 */
