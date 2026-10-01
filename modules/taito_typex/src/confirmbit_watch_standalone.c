/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  confirmbit_watch_standalone.c -- ultra_typex_confirmbit_watch32.dll
 * =============================================================================
 *  Diagnostic-only, separate DLL (same reason as every other *_standalone.c in
 *  this board: never touch the shared hook while chasing a single title's
 *  bug).
 *
 *  Block King Ball Shooter's touch-sensor calibration screen ignores our
 *  correctly-working synthetic ENTER key. Live tracing (dinput8_emu.c +
 *  lsdrv_shim_standalone.c's keyboard-object vtable hook) already proved:
 *    - the driver-level "IsKeyDown(Enter)" (vtable slot 2) fires constantly
 *      and correctly reports pressed/released, called from EXACTLY ONE site:
 *      game.exe+0x639F9 (RVA), inside a generic keyboard-shortcut scanner
 *      that ORs a per-key bit into a per-player virtual-button accumulator.
 *    - the driver-level "WasKeyPressed(Enter)" (vtable slot 3) NEVER fires
 *      with keyId 0x1C -- the two "confirm-button" helper functions that use
 *      it (game.exe+0x4392C0 and +0x43945A) are simply not on the active code
 *      path for this screen.
 *  Static disassembly further shows the bit the keyboard scanner ORs in
 *  (loaded from a fixed global at RVA 0x20060C, paired with a second at
 *  0x200610) is tested by ~30 other call sites scattered across the exe,
 *  most densely clustered around RVA 0x1F2FCF..0x1F935A -- almost certainly
 *  the test-menu / UI screen state machine. Manually reading all ~30 sites
 *  and their surrounding logic to guess which one is live during THIS screen
 *  would take far longer than just asking the running process directly.
 *
 *  This DLL patches a one-byte INT3 (0xCC) over each candidate site, and a
 *  vectored exception handler logs every hit (site name + eax/ebx/ecx/edx/
 *  esi/edi), restores the original byte, single-steps past it, and re-arms --
 *  so it observes forever without ever missing an access. Whichever handful
 *  of sites actually fire while the calibration screen is up and ENTER is
 *  pressed are the real candidates -- directly, with zero guessing.
 *
 *  RVAs below (site VA minus 0x400000) are computed at install time against
 *  this process's actual game.exe base, so a relocated load (ASLR) is still
 *  handled correctly even though this build has never observed one.
 *
 *  Env-gated: ULTRA_CONFIRMBIT_WATCH=1 (provisional-fix table, Block King
 *  only). Diagnostic-only -- never shipped as a permanent fix.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static CRITICAL_SECTION g_log_cs;
static int g_log_cs_ready;
static void wlog(const char* fmt, ...) {
    static char path[MAX_PATH];
    FILE* f;
    if (!path[0]) {
        DWORD n = GetTempPathA(sizeof path - 40, path);
        if (!n || n >= sizeof path - 40) { path[0] = 'x'; path[1] = 0; return; }
        strcat(path, "ultra_confirmbit_debug.log");
    }
    if (path[0] == 'x' && path[1] == 0) return;
    if (g_log_cs_ready) EnterCriticalSection(&g_log_cs);
    f = fopen(path, "a");
    if (f) {
        fputs("[confirmbit] ", f);
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    if (g_log_cs_ready) LeaveCriticalSection(&g_log_cs);
}

/* RVA (site VA - 0x400000), human label, hit-log cap. All sites are 6-byte
 * instructions (opcode + modrm + disp32), confirmed from static disassembly. */
typedef struct { DWORD_PTR rva; const char* label; } site_t;
static const site_t SITES[] = {
    { 0x0392d6, "confirmfnA_gamepad(0x4392d6)" },
    { 0x03946c, "confirmfnB_gamepad(0x43946c)" },
    { 0x060094, "site_460094" },
    { 0x0600ad, "site_4600ad" },
    { 0x06012a, "site_46012a" },
    { 0x060143, "site_460143" },
    { 0x0604a3, "site_4604a3" },
    { 0x0604c7, "site_4604c7" },
    { 0x06059f, "site_46059f" },
    { 0x061ebb, "site_461ebb" },
    { 0x1cf22e, "site_5cf22e_mov" },
    { 0x1d2966, "site_5d2966" },
    { 0x1e9fa8, "site_5e9fa8" },
    { 0x1f2fcf, "site_5f2fcf" },
    { 0x1f300f, "site_5f300f" },
    { 0x1f3036, "site_5f3036" },
    { 0x1f3f03, "site_5f3f03" },
    { 0x1f3f3f, "site_5f3f3f" },
    { 0x1f489d, "site_5f489d" },
    { 0x1f537d, "site_5f537d" },
    { 0x1f5432, "site_5f5432" },
    { 0x1f558c, "site_5f558c" },
    { 0x1f5694, "site_5f5694" },
    { 0x1f59af, "site_5f59af" },
    { 0x1f5d67, "site_5f5d67" },
    { 0x1f61f9, "site_5f61f9" },
    { 0x1f6238, "site_5f6238" },
    { 0x1f6273, "site_5f6273" },
    { 0x1f8a63, "site_5f8a63" },
    { 0x1f915f, "site_5f915f" },
    { 0x1f930e, "site_5f930e" },
    { 0x1f935a, "site_5f935a" },
    { 0x24dfe5, "site_64dfe5" },
};
#define NSITES (int)(sizeof(SITES) / sizeof(SITES[0]))

static BYTE* g_addr[NSITES];
static BYTE  g_orig[NSITES][6];
static int   g_hits[NSITES];
static int   g_armed;

static __declspec(thread) int t_pending_idx = -1;

static LONG CALLBACK confirmbit_veh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT) {
        void* addr = ep->ExceptionRecord->ExceptionAddress;
        for (int i = 0; i < NSITES; i++) {
            if (g_addr[i] == (BYTE*)addr) {
                g_hits[i]++;
                if (g_hits[i] <= 8) {
                    CONTEXT* c = ep->ContextRecord;
                    wlog("HIT %s (#%d) eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX",
                         SITES[i].label, g_hits[i],
                         c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);
                } else if (g_hits[i] == 9) {
                    wlog("%s: further hits suppressed (>8)", SITES[i].label);
                }
                /* restore original bytes, rewind eip, single-step, re-arm after */
                DWORD old;
                VirtualProtect(g_addr[i], 6, PAGE_EXECUTE_READWRITE, &old);
                memcpy(g_addr[i], g_orig[i], 6);
                ep->ContextRecord->Eip -= 1;
                ep->ContextRecord->EFlags |= 0x100; /* trap flag */
                t_pending_idx = i;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == EXCEPTION_SINGLE_STEP && t_pending_idx >= 0) {
        int i = t_pending_idx;
        t_pending_idx = -1;
        DWORD old;
        VirtualProtect(g_addr[i], 6, PAGE_EXECUTE_READWRITE, &old);
        g_addr[i][0] = 0xCC;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void install_confirmbit_watch(void) {
    if (g_armed) return;
    HMODULE gexe = GetModuleHandleA(NULL);
    if (!gexe) return;
    BYTE* base = (BYTE*)gexe;
    int ok = 0;
    for (int i = 0; i < NSITES; i++) {
        BYTE* p = base + SITES[i].rva;
        DWORD old;
        if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) continue;
        memcpy(g_orig[i], p, 6);
        p[0] = 0xCC;
        g_addr[i] = p;
        ok++;
    }
    AddVectoredExceptionHandler(1, confirmbit_veh);
    g_armed = 1;
    wlog("armed: %d/%d sites patched, game.exe base=%p", ok, NSITES, (void*)base);
}

static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    Sleep(500);
    install_confirmbit_watch();
    /* periodic summary so a long calibration-screen session still leaves a
     * readable trail even if the process is killed instead of exited cleanly */
    while (1) {
        Sleep(5000);
        char buf[2048]; int n = 0;
        buf[0] = 0;
        for (int i = 0; i < NSITES; i++) {
            if (g_hits[i] > 0 && n < (int)sizeof buf - 64) {
                n += _snprintf(buf + n, sizeof buf - n - 1, "%s=%d ", SITES[i].label, g_hits[i]);
            }
        }
        if (n > 0) wlog("summary: %s", buf);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_log_cs);
        g_log_cs_ready = 1;
        wlog("loaded (DLL_PROCESS_ATTACH)");
        CreateThread(NULL, 0, worker, NULL, 0, NULL);
    }
    return TRUE;
}

__declspec(dllexport) const char* ultra_confirmbit_watch_id(void) {
    return "Ultra Loader diagnostic-only confirm-bit site watcher v1";
}

#endif /* _WIN32 */
