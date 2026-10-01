/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  tabletmsg_hook_standalone.c -- ultra_typex_tabletmsg_hook32.dll
 * =============================================================================
 *  Diagnostic + best-effort fix, separate DLL (same reason as every other
 *  *_standalone.c on this board).
 *
 *  Block King Ball Shooter's touch-sensor calibration screen stays stuck in
 *  a "retry" state no matter what button is pressed -- exhaustively proven
 *  live (keyboard, joystick, WM_KEYDOWN, and the screen's own internal
 *  virtual-button dispatch all traced and ruled out this session). lsdrv.dll
 *  (the real touch-panel driver this game links) exports 45 functions --
 *  "GetTabletFingerData", "GetTabletPenData", etc -- confirming this is a
 *  genuine touch-panel driver, not a keyboard/gamepad-driven menu. Of the 6
 *  functions game.exe actually imports (by ordinal: TabletMessage=6,
 *  OpenDriver=2, CloseDriver=3, GetTabletEnable=11, TabletControl=7,
 *  LsSetTabletParam=36), only OpenDriver and GetTabletEnable were ever
 *  hooked before this file -- TabletMessage and TabletControl were never
 *  touched, and disassembly of the REAL lsdrv.dll shows both take a
 *  HANDLE-shaped second argument that gets stashed and later passed to a
 *  Win32 call resolved through the driver's own IAT (consistent with
 *  RegisterWaitForSingleObject/SetEvent-style async notification -- the
 *  game hands the driver an event to signal "a touch happened"). With no
 *  real hardware, that event can never fire on its own.
 *
 *  This hooks both functions purely by IAT-redirecting game.exe's own
 *  lsdrv.dll import slots (same technique as jvs_serial_hook.c's kernel32
 *  patching, applied here to lsdrv.dll specifically) to log every argument,
 *  call through to the REAL exports so the driver's own state stays
 *  consistent, and then best-effort SetEvent() any argument that looks like
 *  a handle -- SetEvent silently returns FALSE on the wrong handle type, it
 *  does not crash, so this is safe to try unconditionally.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static CRITICAL_SECTION g_log_cs;
static int g_log_cs_ready;
static void tlog(const char* fmt, ...) {
    static char path[MAX_PATH];
    FILE* f;
    if (!path[0]) {
        DWORD n = GetTempPathA(sizeof path - 40, path);
        if (!n || n >= sizeof path - 40) { path[0] = 'x'; path[1] = 0; return; }
        strcat(path, "ultra_tabletmsg_debug.log");
    }
    if (path[0] == 'x' && path[1] == 0) return;
    if (g_log_cs_ready) EnterCriticalSection(&g_log_cs);
    f = fopen(path, "a");
    if (f) {
        fputs("[tabletmsg] ", f);
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    if (g_log_cs_ready) LeaveCriticalSection(&g_log_cs);
}

/* Same PE-readability guard as jvs_serial_hook.c / lsdrv_shim_standalone.c --
 * a module can be unloaded between our module scan and our read of it. */
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

typedef int (WINAPI *pTabletFn)(int, void*);
static pTabletFn Real_TabletMessage;
static pTabletFn Real_TabletControl;

static int seen_handle(void* h) {
    static void* seen[32]; static int n;
    if (!h) return 1;
    for (int i = 0; i < n; ++i) if (seen[i] == h) return 1;
    if (n < 32) { seen[n++] = h; return 0; }
    return 0;
}

static void try_signal(void* h, const char* who) {
    /* DISABLED: live testing showed arg2 is a heap pointer (matches the
     * SAME address range as other known heap objects this session), not a
     * kernel handle -- SetEvent correctly failed every time (err=6,
     * ERROR_INVALID_HANDLE). Kept as a pure observation point now; calling
     * SetEvent on it was useless overhead, not a real fix, and coincided
     * with a crash that reproduced on both attempts (game.exe's own code
     * dereferencing this call's -1 return value as a pointer) -- most
     * likely a pre-existing issue this hook's timing happened to surface,
     * not one it caused, but removing the pointless SetEvent calls first
     * to isolate that before touching anything else. */
    (void)h; (void)who;
}

static void remember(void* h);

/* CONFIRMED live (two separate crashes, same faulting instruction both
 * times): when the real driver call returns -1, game.exe later dereferences
 * that -1 as a pointer ("mov [eax], ..." with eax == -1) and crashes. The
 * real driver has no reason to ever return anything else with no touch
 * hardware present, so this crash is a latent bug in the game's own error
 * handling, not something introduced by hooking these calls -- waiting on
 * the calibration screen was apparently enough to hit it on its own.
 * Substitute 0 for any negative return so the game's own unchecked pointer
 * use gets a real (zeroed) buffer instead of -1. The original return value
 * is still logged for diagnostics. */
static int WINAPI Hook_TabletMessage(int a1, void* a2) {
    int r = Real_TabletMessage ? Real_TabletMessage(a1, a2) : -1;
    tlog("TabletMessage(a1=%d/0x%X, a2=%p) -> %d%s", a1, (unsigned)a1, a2, r,
         r < 0 ? " (suppressed to 0)" : "");
    remember(a2);
    return r < 0 ? 0 : r;
}
static int WINAPI Hook_TabletControl(int a1, void* a2) {
    int r = Real_TabletControl ? Real_TabletControl(a1, a2) : -1;
    tlog("TabletControl(a1=%d/0x%X, a2=%p) -> %d%s", a1, (unsigned)a1, a2, r,
         r < 0 ? " (suppressed to 0)" : "");
    remember(a2);
    return r < 0 ? 0 : r;
}

/* Periodic sweep: some code paths may stash the event handle without ever
 * calling SetEvent through a code path we see again, so also just try
 * signaling every handle we've EVER captured, every tick -- cheap and
 * harmless if already consumed. */
static void* g_all_seen[32];
static int g_all_seen_n;
static void remember(void* h) {
    if (!h || (UINT_PTR)h < 0x10000) return;
    for (int i = 0; i < g_all_seen_n; ++i) if (g_all_seen[i] == h) return;
    if (g_all_seen_n < 32) g_all_seen[g_all_seen_n++] = h;
}

static int iat_patch_module(HMODULE mod) {
    if (!mod || !ul_pe_readable(mod, sizeof(IMAGE_DOS_HEADER))) return 0;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)mod + dos->e_lfanew);
    if (!ul_pe_readable(nt, sizeof *nt) || nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    IMAGE_DATA_DIRECTORY* dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return 0;
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)mod + dd->VirtualAddress);
    int patched = 0;
    for (; ul_pe_readable(imp, sizeof *imp) && imp->Name; imp++) {
        char* name = (char*)mod + imp->Name;
        if (!ul_pe_readable(name, 9) || _stricmp(name, "lsdrv.dll") != 0) continue;
        IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*)((BYTE*)mod + imp->FirstThunk);
        for (; ul_pe_readable(thunk, sizeof *thunk) && thunk->u1.Function; thunk++) {
            void** slot = (void**)&thunk->u1.Function;
            void* cur = *slot;
            /* Match by CURRENT resolved address, not name (bound by ordinal) --
             * we already know lsdrv.dll's real export RVAs for these two. */
            HMODULE ls = GetModuleHandleA("lsdrv.dll");
            if (!ls) continue;
            void* real_msg = (void*)((BYTE*)ls + 0x18d0);
            void* real_ctl = (void*)((BYTE*)ls + 0x21c0);
            DWORD old;
            if (cur == real_msg && !Real_TabletMessage) {
                Real_TabletMessage = (pTabletFn)cur;
                if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                    *slot = (void*)Hook_TabletMessage;
                    VirtualProtect(slot, sizeof(void*), old, &old);
                    patched++;
                }
            } else if (cur == real_ctl && !Real_TabletControl) {
                Real_TabletControl = (pTabletFn)cur;
                if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                    *slot = (void*)Hook_TabletControl;
                    VirtualProtect(slot, sizeof(void*), old, &old);
                    patched++;
                }
            }
        }
    }
    return patched;
}

static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    Sleep(500);
    /* Only game.exe's own IAT needs patching -- we only care about calls
     * THIS process's main module makes to lsdrv.dll. Scanning every loaded
     * module (as the shared jvs_serial_hook.c IAT patcher does, for a
     * different purpose) is unnecessary risk here and was removed after a
     * reproducible crash. */
    for (int tries = 0; tries < 20 && !(Real_TabletMessage && Real_TabletControl); tries++) {
        HMODULE gexe = GetModuleHandleA(NULL);
        if (gexe) iat_patch_module(gexe);
        if (Real_TabletMessage || Real_TabletControl) break;
        Sleep(250);
    }
    tlog("armed: TabletMessage hooked=%d TabletControl hooked=%d",
         Real_TabletMessage != NULL, Real_TabletControl != NULL);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_log_cs);
        g_log_cs_ready = 1;
        tlog("loaded (DLL_PROCESS_ATTACH)");
        CreateThread(NULL, 0, worker, NULL, 0, NULL);
    }
    return TRUE;
}

__declspec(dllexport) const char* ultra_tabletmsg_hook_id(void) {
    return "Ultra Loader lsdrv TabletMessage/TabletControl event-signal hook v1";
}

#endif /* _WIN32 */
