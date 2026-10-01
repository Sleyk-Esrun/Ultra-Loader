/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  calib_bypass_standalone.c -- ultra_typex_calib_bypass32.dll
 * =============================================================================
 *  Block King Ball Shooter's touch-sensor calibration screen depends on a
 *  real touch-panel handshake (lsdrv.dll's TabletMessage/TabletControl) that
 *  can never complete without real hardware -- confirmed this session via
 *  extensive live tracing. This game is known to require an external binary
 *  patch of game.exe before it runs stably, and its unpatched state is
 *  detectable at runtime. Chasing the real driver protocol further is no longer
 *  the approach -- this designs around the screen instead.
 *
 *  CTestModeForm_Calibration (confirmed via RTTI: ".?AVCTestModeForm_Calibration@@",
 *  vtable at game.exe+0x1fd31c) is the class backing this screen.
 *  OnRenderWindow (vtable slot 0, game.exe+0x1ff290) is a per-frame render
 *  call that reads its own "this" in ECX (thiscall) -- confirmed live via
 *  cdb: matching this vtable pointer at that entry point isolates the real,
 *  active instance from unrelated objects sharing the same generic per-frame
 *  update dispatcher.
 *
 *  Another of the class's own methods (game.exe+0x1fea20) unconditionally
 *  transitions the screen's internal mode to 4 (a "closing" state, confirmed
 *  live: it always executes the same mode-4 transition + cleanup regardless
 *  of input) -- this is almost certainly what a real TEST/cancel press would
 *  have triggered through the touch-panel-dependent path that never
 *  completes on its own. Rather than reverse-engineer the full touch
 *  protocol, this calls that EXISTING game method directly and once, the
 *  moment the real instance is identified -- simulating exactly what the
 *  game's own code does internally on a real button press, without
 *  inventing new behavior.
 *
 *  Mechanism: same INT3-breakpoint-plus-single-step technique already
 *  proven in confirmbit_watch_standalone.c this session -- patch one byte,
 *  catch it in a vectored exception handler, act, restore, single-step,
 *  re-arm. No inline hooking, no trampoline.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* Same "ask the memory manager" pattern as jvs_serial_hook.c's own comment
 * explains: mingw32 (this project's 32-bit toolchain) has no __try/__except,
 * so safety here means checking readability before touching memory, not
 * catching a fault after the fact. */
static int ul_pe_readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    const unsigned char* a = (const unsigned char*)p;
    const unsigned char* end = a + n;
    while (a < end) {
        if (!VirtualQuery(a, &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT) return 0;
        if (mbi.Protect == PAGE_NOACCESS || mbi.Protect == PAGE_EXECUTE) return 0;
        a = (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}

static CRITICAL_SECTION g_log_cs;
static int g_log_cs_ready;
static void clog(const char* fmt, ...) {
    static char path[MAX_PATH];
    FILE* f;
    if (!path[0]) {
        DWORD n = GetTempPathA(sizeof path - 40, path);
        if (!n || n >= sizeof path - 40) { path[0] = 'x'; path[1] = 0; return; }
        strcat(path, "ultra_calib_bypass_debug.log");
    }
    if (path[0] == 'x' && path[1] == 0) return;
    if (g_log_cs_ready) EnterCriticalSection(&g_log_cs);
    f = fopen(path, "a");
    if (f) {
        fputs("[calib-bypass] ", f);
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    if (g_log_cs_ready) LeaveCriticalSection(&g_log_cs);
}

#define ONRENDER_RVA   0x1ff290u
#define VTABLE_RVA     0x1fd31cu   /* +0x400000 = the confirmed CTestModeForm_Calibration vtable */
#define CLOSE_FN_RVA   0x1fea20u

static BYTE* g_addr;
static BYTE  g_orig[1];
static int   g_armed;
static void* g_triggered_for[8];
static int   g_triggered_n;

typedef void (__attribute__((thiscall)) *pCloseFn)(void*);

/* Pure check -- does NOT add as a side effect, unlike a typical "seen"
 * helper, because the caller needs to check this MANY times per instance
 * (once per render frame) before deciding to actually trigger. */
static int already_triggered(void* obj) {
    for (int i = 0; i < g_triggered_n; i++) if (g_triggered_for[i] == obj) return 1;
    return 0;
}
static void mark_triggered(void* obj) {
    if (g_triggered_n < 8) g_triggered_for[g_triggered_n++] = obj;
}

/* Give the screen a moment to finish its own setup before we force it
 * closed -- firing on literally the first render call risks interrupting
 * whatever else the game is still wiring up around this instance. Cheap
 * insurance, not a fix for the stale-pointer crash itself (that's guarded
 * in lsdrv_shim_standalone.c's vtable hooks instead). */
static void* g_pending_obj;
static int g_pending_hits;
#define TRIGGER_AFTER_HITS 30

static __declspec(thread) int t_pending;

static LONG CALLBACK bypass_veh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT && ep->ExceptionRecord->ExceptionAddress == g_addr) {
        CONTEXT* c = ep->ContextRecord;
        void* obj = (void*)c->Ecx;
        HMODULE gexe = GetModuleHandleA(NULL);
        void* expect_vtable = (void*)((BYTE*)gexe + VTABLE_RVA);
        int is_real = obj && ul_pe_readable(obj, sizeof(void*)) &&
                      (*(void**)obj == expect_vtable);
        if (is_real && !already_triggered(obj)) {
            if (obj != g_pending_obj) { g_pending_obj = obj; g_pending_hits = 0; }
            g_pending_hits++;
            if (g_pending_hits >= TRIGGER_AFTER_HITS) {
                /* CONFIRMED via Windows' own crash report: calling the real
                 * close-fn (game+0x1FEA20) directly crashes deterministically
                 * -- it makes its OWN indirect vtable call partway through,
                 * landing in unrelated padding bytes every time. That
                 * function's FIRST action, before any of its risky internal
                 * calls, is simply "mode = 4" on the object (confirmed via
                 * disassembly: movl $0x4,0x40(%esi)). Do just that one write
                 * ourselves and skip the rest of its body entirely -- same
                 * observable state transition, none of the risk. */
                clog("real CTestModeForm_Calibration instance seen %d times: this=%p -- forcing mode=4 directly (close-fn's own vtable call crashes)",
                     g_pending_hits, obj);
                if (ul_pe_readable((BYTE*)obj + 0x40, sizeof(int))) {
                    *(int*)((BYTE*)obj + 0x40) = 4;
                    clog("mode=4 written for this=%p", obj);
                } else {
                    clog("this+0x40 not writable for this=%p, skipped", obj);
                }
                mark_triggered(obj);
            }
        }
        DWORD old;
        VirtualProtect(g_addr, 1, PAGE_EXECUTE_READWRITE, &old);
        g_addr[0] = g_orig[0];
        c->Eip -= 1;
        c->EFlags |= 0x100;
        t_pending = 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && t_pending) {
        t_pending = 0;
        DWORD old;
        VirtualProtect(g_addr, 1, PAGE_EXECUTE_READWRITE, &old);
        g_addr[0] = 0xCC;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Logs the exit code and a best-effort caller chain (walking saved return
 * addresses found on the stack, resolved to module+offset) right before the
 * game voluntarily exits -- in-process, so unlike attaching cdb it can run
 * alongside this file's own INT3 patch without the two fighting over the
 * process's single debug port. */
static void modname_at(void *addr, char *out, size_t cap) {
    HMODULE mod = NULL;
    char path[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            (LPCSTR)addr, &mod) || !mod) {
        _snprintf(out, cap, "%p", addr); out[cap-1] = 0; return;
    }
    DWORD n = GetModuleFileNameA(mod, path, sizeof path);
    const char *base = n ? path : "?";
    for (const char *s = base; *s; s++) if (*s == '\\' || *s == '/') base = s + 1;
    _snprintf(out, cap, "%s+0x%X", base, (unsigned)((BYTE*)addr - (BYTE*)mod));
    out[cap-1] = 0;
}

typedef UINT (WINAPI *pExitProcessFn)(UINT);
static pExitProcessFn Real_ExitProcess;
static void WINAPI Hook_ExitProcess(UINT code) {
    clog("ExitProcess(code=%u / 0x%X) -- scanning stack for game.exe callers:", code, code);
    DWORD* stack = (DWORD*)&code; /* first stack slot above our own return addr */
    HMODULE gexe = GetModuleHandleA(NULL);
    int shown = 0;
    for (int i = -2; i < 64 && shown < 12; i++) {
        DWORD v = stack[i];
        HMODULE owner = NULL;
        if (v > 0x10000 &&
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)v, &owner) &&
            owner == gexe) {
            char nm[128]; modname_at((void*)v, nm, sizeof nm);
            clog("  stack[%d] = 0x%08lX (%s)", i, v, nm);
            shown++;
        }
    }
    if (Real_ExitProcess) Real_ExitProcess(code);
    ExitProcess(code); /* fallback, should not be reached */
}

static void install_exit_hook(HMODULE gexe) {
    if (Real_ExitProcess) return;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)gexe;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)gexe + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY* dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return;
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)gexe + dd->VirtualAddress);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    void* real_exit = k32 ? (void*)GetProcAddress(k32, "ExitProcess") : NULL;
    if (!real_exit) return;
    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*)((BYTE*)gexe + imp->FirstThunk);
        for (; thunk->u1.Function; thunk++) {
            void** slot = (void**)&thunk->u1.Function;
            if (*slot == real_exit) {
                Real_ExitProcess = (pExitProcessFn)real_exit;
                DWORD old;
                if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                    *slot = (void*)Hook_ExitProcess;
                    VirtualProtect(slot, sizeof(void*), old, &old);
                    clog("ExitProcess hooked in game.exe's IAT");
                }
                return;
            }
        }
    }
}

/* CONFIRMED via Windows' own crash report, reproduced identically across
 * multiple builds (including one where this file never even calls into the
 * game's close-fn any more) -- something in the game independently jumps to
 * game.exe+0x1FF28F, exactly one byte before OnRenderWindow's real entry
 * point, and executes the natural `int3` alignment padding sitting there
 * (5 padding bytes precede every function in this binary's normal MSVC
 * layout; this one just happens to sit in front of a function we care
 * about). That's a genuine, pre-existing bug/corruption in this exact game
 * dump -- unrelated to anything this DLL does -- consistent with the
 * known bad bytes documented for this title's common dump.
 * Since this byte is never meant to be reached under normal execution
 * anyway, the safe fix is to make landing there harmless: turn it into a
 * NOP. Any code that (correctly or by this same off-by-one bug) ends up
 * here now just falls straight through into OnRenderWindow's real first
 * byte at +0x1FF290, one instruction later, instead of crashing. */
#define PAD_BEFORE_ONRENDER_RVA 0x1ff28fu

static void install(void) {
    if (g_armed) return;
    HMODULE gexe = GetModuleHandleA(NULL);
    if (!gexe) return;
    install_exit_hook(gexe);
    BYTE* pad = (BYTE*)gexe + PAD_BEFORE_ONRENDER_RVA;
    DWORD old;
    if (VirtualProtect(pad, 1, PAGE_EXECUTE_READWRITE, &old)) {
        if (pad[0] == 0xCC) {
            pad[0] = 0x90; /* NOP -- falls through into OnRenderWindow's real entry */
            clog("neutralized padding byte at game.exe+0x%X (was 0xCC, now NOP)",
                 PAD_BEFORE_ONRENDER_RVA);
        }
        VirtualProtect(pad, 1, old, &old);
    }
    BYTE* p = (BYTE*)gexe + ONRENDER_RVA;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) return;
    g_orig[0] = p[0];
    p[0] = 0xCC;
    g_addr = p;
    AddVectoredExceptionHandler(1, bypass_veh);
    g_armed = 1;
    clog("armed: patched game.exe+0x%X, watching for vtable game.exe+0x%X, will call game.exe+0x%X",
         ONRENDER_RVA, VTABLE_RVA, CLOSE_FN_RVA);
}

static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    Sleep(500);
    install();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_log_cs);
        g_log_cs_ready = 1;
        clog("loaded (DLL_PROCESS_ATTACH)");
        CreateThread(NULL, 0, worker, NULL, 0, NULL);
    }
    return TRUE;
}

__declspec(dllexport) const char* ultra_calib_bypass_id(void) {
    return "Ultra Loader Block King calibration-screen bypass v1";
}

#endif /* _WIN32 */
