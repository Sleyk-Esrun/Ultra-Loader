/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  boot_trace.c -- maximum-information bring-up trace for a single run
 * =============================================================================
 *
 *  WHY THIS EXISTS. Bringing a game up has been costing one playtest per
 *  question, because each run answered exactly one thing and every answer
 *  raised the next question. This file exists to make ONE run answer as many
 *  questions as it can, so the owner's time stops being the bottleneck.
 *
 *  Four things it adds that nothing else in this hook provides:
 *
 *   1. A STACK WALK on any exception. The existing crash handler prints the
 *      faulting address and the registers, which names the instruction but not
 *      the CALLER - and on Silent Hill the faulting address was inside ntdll,
 *      where "who called us" is the entire question. Each frame is resolved to
 *      module+offset so the log names the culprit DLL by itself.
 *
 *   2. SHAIOLIB CALL TRACING. This board's game talks to the GHL security/IO
 *      library through a handful of imports. With the game folder back to
 *      stock, what that library says is the bring-up question - not something
 *      to be inferred. Every call is logged with its arguments and its RETURN
 *      VALUE, and passed straight through to the real function: observe-only,
 *      nothing is faked.
 *
 *      (Note the standing warning in this project about observe-only tracers:
 *      one that cannot resolve the real export must NOT invent a return value.
 *      If resolution fails we do not install the hook at all.)
 *
 *   3. A TIMELINE. Every line carries milliseconds-since-attach and a thread
 *      id, so "it stopped at t=640ms on the render thread" is readable off the
 *      log instead of reconstructed.
 *
 *   4. THE EXIT PATH. ExitProcess is traced so a clean exit is distinguishable
 *      from a crash, with the code and the thread that called it.
 *
 *  Everything here is gated on ULTRA_BOOT_TRACE=1 so a normal run pays nothing.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GCC spells the caller's address __builtin_return_address(0). */
#define _ReturnAddress() __builtin_return_address(0)

extern void ultra_d3d8_log(const char* fmt, ...);
extern void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

#define BT_LOG ultra_d3d8_log

static DWORD  g_t0;
static int    g_on;

static DWORD bt_ms(void) { return GetTickCount() - g_t0; }

/* module+offset for an address, so a bare pointer names something. */
static void bt_where(void* addr, char* out, size_t cap)
{
    HMODULE m = NULL;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &m) && m &&
        GetModuleFileNameA(m, path, sizeof path)) {
        const char* base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        snprintf(out, cap, "%s+0x%lX", base,
                 (unsigned long)((char*)addr - (char*)m));
    } else {
        snprintf(out, cap, "%p (no module)", addr);
    }
}

/* ---- 1. the stack walk -----------------------------------------------------
 * RtlCaptureStackBackTrace is resolved at run time: it is present on every
 * Windows this project targets, but resolving it dynamically means a missing
 * export degrades to "no stack" rather than a load failure. */
typedef USHORT (WINAPI *bt_pCapture)(ULONG, ULONG, PVOID*, PULONG);
static bt_pCapture s_capture;

static void bt_stack(const char* tag)
{
    void* frames[24];
    USHORT n, i;
    char w[MAX_PATH + 32];
    if (!s_capture) return;
    n = s_capture(0, 24, frames, NULL);
    for (i = 0; i < n; ++i) {
        bt_where(frames[i], w, sizeof w);
        BT_LOG("[%6lu] %s stack[%02u] %s", bt_ms(), tag, (unsigned)i, w);
    }
    if (!n) BT_LOG("[%6lu] %s stack: (empty)", bt_ms(), tag);
}

/* Public: resolve any address to module+offset, for the crash handler's own
 * frame walk (it walks the FAULTING thread, which this file cannot reach). */
void ultra_boot_trace_where(void* addr, char* out, unsigned cap)
{ bt_where(addr, out, (size_t)cap); }

/* Public: the existing crash handler calls this to add what it was missing. */
void ultra_boot_trace_stack(const char* tag) { if (g_on) bt_stack(tag); }

/* ---- 2. shaiolib, observed not replaced ------------------------------------
 * The game's own imports, traced with their return values. These are the calls
 * that decide whether it believes it has a GHL I/O board. */
/* __cdecl, NOT WINAPI. THIS IS LOAD-BEARING -- see the block below.
 *
 * shaiolib is a __cdecl library. KSHG.exe at VA 0x004D1448 does
 *   FF 15 D8 04 68 00   call dword ptr [0x6804D8]   ; shaiolib_GUN_status
 *   83 C4 04            add  esp,4                  ; CALLER cleans up
 * and seven further call sites agree (LED_change cleans 0xC). Our own export
 * at shaiolib_crk.c is plain mingw __cdecl too.
 *
 * Declaring these tracer shims WINAPI wrecked ESP TWICE in one call: the
 * stdcall epilogue popped the argument that the game ALSO pops, and because
 * the forwarding pointer was typed stdcall, GCC emitted a compensating
 * "sub esp,4" after the call that the real cdecl callee never earned. The
 * epilogue then landed one slot low: "pop ebp" took the game's saved EDI and
 * "ret" took its address out of the saved-EBP slot.
 *
 * That predicted EBP = the cached hook pointer the game holds in EDI, and a
 * stale high stack value for EIP. The 2026-09-17 fault reported
 *   EBP=6C7662B0  EIP=0019FF76  ESP=0019FD08
 * and 0x6C7662B0 is exactly the pointer rgate_keeper logs at boot. Three
 * matches, no fitting.
 *
 * The damage was not a wrong value: the main thread returned to its own stack,
 * our VEH called that a "high fault" and ran ExitThread on it, the game's
 * window died with it, and because ExitThread on a main thread does not end a
 * Win32 process, the owner was left with a topmost overlay over a process only
 * Task Manager could kill.
 *
 * Same class as the shg_EaxManagerCreate arity bug fixed hours earlier. See
 * the project memory note on the stub-DLL ABI trap. */
typedef int  (*p_i_v)(void);
typedef int  (*p_i_p)(void*);
typedef int  (*p_i_i)(int);

static p_i_v  s_real_init, s_real_ready;
static p_i_p  s_real_gun;
static p_i_i  s_real_en, s_real_dis;

static int bt_initialize(void) {
    int r;
    BT_LOG("[%6lu] SHAIO: initialize() called   tid=%lu", bt_ms(), GetCurrentThreadId());
    r = s_real_init ? s_real_init() : 0;
    BT_LOG("[%6lu] SHAIO: initialize() -> %d", bt_ms(), r);
    if (!r) bt_stack("SHAIO-init-fail");
    return r;
}
static int bt_is_ready(void) {
    static int told, last = -2;
    int r = s_real_ready ? s_real_ready() : 0;
    if (!told || r != last) {
        told = 1; last = r;
        BT_LOG("[%6lu] SHAIO: is_ready() -> %d   tid=%lu", bt_ms(), r, GetCurrentThreadId());
    }
    return r;
}
static int bt_gun_status(void* buf) {
    static unsigned n;
    int r = s_real_gun ? s_real_gun(buf) : 0;
    if (n == 0 || (n % 600u) == 0u) {
        const int* p = (const int*)buf;
        if (buf)
            BT_LOG("[%6lu] SHAIO: GUN_status#%u -> %d  P1(x=%d y=%d t=%d s=%d) P2(x=%d y=%d t=%d s=%d)",
                   bt_ms(), n, r, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        else
            BT_LOG("[%6lu] SHAIO: GUN_status#%u -> %d (null buffer)", bt_ms(), n, r);
    }
    n++;
    return r;
}
static int bt_gun_enable(int p)  {
    BT_LOG("[%6lu] SHAIO: GUN_enable(%d)", bt_ms(), p);
    return s_real_en ? s_real_en(p) : 0;
}
static int bt_gun_disable(int p) {
    BT_LOG("[%6lu] SHAIO: GUN_disable(%d)", bt_ms(), p);
    return s_real_dis ? s_real_dis(p) : 0;
}

/* ---- 3. the exit path ------------------------------------------------------ */
typedef void (WINAPI *p_exitproc)(UINT);
static p_exitproc s_real_exit;
static void WINAPI bt_exitprocess(UINT code) {
    BT_LOG("[%6lu] EXIT: ExitProcess(%u = 0x%08X) tid=%lu", bt_ms(), code, code,
           GetCurrentThreadId());
    bt_stack("EXIT");
    if (s_real_exit) s_real_exit(code);
    ExitProcess(code);
}

/* ---- 4. what actually loaded ---------------------------------------------- */
typedef HMODULE (WINAPI *p_loadlibA)(LPCSTR);
static p_loadlibA s_real_lla;
static HMODULE WINAPI bt_loadlibA(LPCSTR n) {
    HMODULE h = s_real_lla ? s_real_lla(n) : LoadLibraryA(n);
    BT_LOG("[%6lu] LOAD: LoadLibraryA(\"%s\") -> %p", bt_ms(), n ? n : "(null)", (void*)h);
    return h;
}

/* ---- 5. WHO STARTS A THREAD, AND ON WHAT ----------------------------------
 *
 * A thread whose start address is not inside any loaded module is either a JIT
 * (legitimate, and rare in a 2007 arcade game) or a corrupted function pointer
 * (which is what a privileged-instruction fault on the first instruction looks
 * like). Either way the creator is what we need, and the return address of the
 * CreateThread call names it.
 */
typedef HANDLE (WINAPI *p_createthread)(LPSECURITY_ATTRIBUTES, SIZE_T,
                                        LPTHREAD_START_ROUTINE, LPVOID,
                                        DWORD, LPDWORD);
static p_createthread s_real_ct;

static HANDLE WINAPI bt_createthread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack,
                                     LPTHREAD_START_ROUTINE start, LPVOID param,
                                     DWORD flags, LPDWORD tid)
{
    char w[MAX_PATH + 32], who[MAX_PATH + 32];
    HMODULE m = NULL;
    int in_module;

    bt_where((void *)start, w, sizeof w);
    in_module = GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)start, &m) && m;

    /* The immediate caller, so we know who asked. */
    bt_where(_ReturnAddress(), who, sizeof who);

    if (!in_module) {
        MEMORY_BASIC_INFORMATION mbi;
        BT_LOG("[%6lu] THREAD: *** start address %p is in NO MODULE *** called from %s",
               bt_ms(), (void *)start, who);
        if (VirtualQuery((void *)start, &mbi, sizeof mbi) == sizeof mbi)
            BT_LOG("[%6lu] THREAD: that address is heap/base=%p protect=0x%lX "
                   "state=0x%lX type=0x%lX",
                   bt_ms(), mbi.AllocationBase, (unsigned long)mbi.Protect,
                   (unsigned long)mbi.State, (unsigned long)mbi.Type);
    } else {
        BT_LOG("[%6lu] THREAD: start=%s param=%p called from %s",
               bt_ms(), w, param, who);
    }
    return s_real_ct ? s_real_ct(sa, stack, start, param, flags, tid)
                     : CreateThread(sa, stack, start, param, flags, tid);
}

/* Bind one shaiolib export, observe-only. Refuses to install a hook whose real
 * target it cannot resolve - a tracer that invents a return value has already
 * cost this project a black screen on another board. */
static void bt_bind(HMODULE lib, const char* name, void** real, void* hook)
{
    if (!lib) return;
    *real = (void*)GetProcAddress(lib, name);
    if (!*real) { BT_LOG("BOOT-TRACE: %s not exported - not hooked", name); return; }
    ultra_iat_patch_all("shaiolib.dll", name, hook);
    BT_LOG("BOOT-TRACE: tracing shaiolib!%s", name);
}

/* ---------------------------------------------------------------------------
 *  INSTALL -- AND WHY NONE OF THE REAL WORK HAPPENS IN DllMain.
 *
 *  The first version of this did its binding straight from DLL_PROCESS_ATTACH,
 *  including a LoadLibraryA("shaiolib.dll"). That is a loader-lock violation:
 *  loading a library while the loader lock is held is not allowed, and the game
 *  died at startup with 0xC0000142 (STATUS_DLL_INIT_FAILED) before a single
 *  line of this trace could be written. The whole point of this file is to make
 *  a run informative, so killing the run is the one thing it must never do.
 *
 *  So DllMain does exactly one legal thing - CreateThread, which is safe
 *  because the new thread cannot run until DllMain returns - and every bind,
 *  every resolve and every wait happens on that thread afterwards.
 *
 *  It also never calls LoadLibrary at all. It POLLS GetModuleHandleA until the
 *  game loads shaiolib itself. If the game never loads it, that silence is a
 *  finding rather than something to force.
 * ------------------------------------------------------------------------- */
static DWORD WINAPI bt_worker(LPVOID a)
{
    HMODULE k32, shaio = NULL;
    int waited = 0;
    unsigned beat = 0;
    (void)a;

    k32 = GetModuleHandleA("kernel32.dll");
    if (k32) s_capture = (bt_pCapture)(void*)GetProcAddress(k32, "RtlCaptureStackBackTrace");
    if (!s_capture) {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        if (nt) s_capture = (bt_pCapture)(void*)GetProcAddress(nt, "RtlCaptureStackBackTrace");
    }
    BT_LOG("[%6lu] BOOT-TRACE: armed on worker thread (stack walk %s)",
           bt_ms(), s_capture ? "available" : "UNAVAILABLE");

    if (k32) {
        s_real_exit = (p_exitproc)(void*)GetProcAddress(k32, "ExitProcess");
        if (s_real_exit) ultra_iat_patch_all("kernel32.dll", "ExitProcess", (void*)bt_exitprocess);
        s_real_lla = (p_loadlibA)(void*)GetProcAddress(k32, "LoadLibraryA");
        if (s_real_lla) ultra_iat_patch_all("kernel32.dll", "LoadLibraryA", (void*)bt_loadlibA);
        s_real_ct = (p_createthread)(void*)GetProcAddress(k32, "CreateThread");
        if (s_real_ct) ultra_iat_patch_all("kernel32.dll", "CreateThread", (void*)bt_createthread);
        BT_LOG("[%6lu] BOOT-TRACE: kernel32 traces installed", bt_ms());
    }

    /* Wait for the GAME to load shaiolib. Never load it ourselves. */
    for (waited = 0; waited < 200 && !shaio; ++waited) {
        shaio = GetModuleHandleA("shaiolib.dll");
        if (!shaio) Sleep(50);
    }
    if (!shaio) {
        BT_LOG("[%6lu] BOOT-TRACE: shaiolib.dll never loaded in 10s - "
               "the game did not get that far", bt_ms());
    } else {
        BT_LOG("[%6lu] BOOT-TRACE: shaiolib.dll present at %p - tracing its calls",
               bt_ms(), (void*)shaio);
        bt_bind(shaio, "shaiolib_initialize",  (void**)&s_real_init,  (void*)bt_initialize);
        bt_bind(shaio, "shaiolib_is_ready",    (void**)&s_real_ready, (void*)bt_is_ready);
        bt_bind(shaio, "shaiolib_GUN_status",  (void**)&s_real_gun,   (void*)bt_gun_status);
        bt_bind(shaio, "shaiolib_GUN_enable",  (void**)&s_real_en,    (void*)bt_gun_enable);
        bt_bind(shaio, "shaiolib_GUN_disable", (void**)&s_real_dis,   (void*)bt_gun_disable);
    }

    /* Heartbeat: a HANG is otherwise indistinguishable from a crash. */
    for (;;) {
        Sleep(2000);
        BT_LOG("[%6lu] BEAT %u: still alive", bt_ms(), ++beat);
        if (beat >= 90) break;
    }
    return 0;
}

void ultra_boot_trace_install(void)
{
    const char* e = getenv("ULTRA_BOOT_TRACE");
    HANDLE t;
    if (!e || e[0] != '1') return;
    g_on = 1;
    g_t0 = GetTickCount();
    /* The ONLY thing done under the loader lock. */
    t = CreateThread(NULL, 0, bt_worker, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* Kept so the existing call site stays valid; the worker owns the heartbeat. */
void ultra_boot_trace_start_beat(void) { }

#endif /* _WIN32 */
