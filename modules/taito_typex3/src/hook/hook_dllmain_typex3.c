/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_typex3.c -- entry point for Taito Type X3's own, independent
 *                           hook DLL (ultra_typex_hook32.dll / ...hook64.dll,
 *                           built ONLY from this board's own src/hook tree).
 * =============================================================================
 *
 *  Injected into the Type X3 game by this module's own src/taito_typex3_main.c
 *  (CreateProcess CREATE_SUSPENDED + LoadLibraryA inject + ResumeThread). On
 *  attach it, synchronously (the hooks must be in place before the game's
 *  first thread runs):
 *    - creates the one JVS node this process owns (jvs_core_typex3.c) --
 *      the canonical cabinet input/coin model every other subsystem reads,
 *    - installs the Fast-I/O (iDmacDrv) shim -- this board's actual cabinet
 *      I/O transport (taito_fastio_typex3.c),
 *    - installs the D3D9 present hook (d3d9_throttle_typex3.c) -- a clean
 *      no-op for a D3D11 title that doesn't import d3d9.dll,
 *    - installs the DirectSound + XAudio2 diagnostic hook (audio_hook_typex3.c),
 *    - starts a standalone XInput + keyboard input reader so the JVS node
 *      reports real cabinet state even with no loader-side shared-memory
 *      feed (this board's own taito_typex3_main.c does not set up an IPC
 *      block today -- see its own header comment),
 *    - installs the Ultra Omni-Bridge keyboard/mouse-emulation bridge
 *      (omni_bridge.c) -- opt-in (ULTRA_BRIDGE_ENABLE, default OFF), for
 *      titles that read raw OS input directly instead of JVS/Fast-I/O; this
 *      board has no lightgun pose source, so it's wired gamepad-only,
 *  then starts a short re-patch worker to catch any module the game loads
 *  fractionally after our first pass (delay-loaded imports).
 *
 *  This TU also owns the two generic Win32 mechanisms every sibling TU in
 *  this DLL needs: t3_log() (an optional-file diagnostic logger, gated by
 *  ULTRA_JVS_LOG) and t3_iat_patch_all() (an IAT-name-and-value patcher,
 *  bounds-checked against each module's mapped image size since mingw has no
 *  SEH __try/__except). Written fresh for this board; no shared source file,
 *  no shared compiled binary with any other board's hook DLL.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "taito_typex3_hook.h"
#include "omni_bridge.h"

#include "ultra_transport.h"   /* shared universal I/O transport layer */
#include "ultra_tracer.h"
#include "ultra_touch.h"

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
/* ============================================================================
 *  Generic plumbing: diagnostic logger + IAT patcher
 * ========================================================================= */
void t3_log(const char* fmt, ...) {
    static const char* path = NULL; static int checked = 0;
    if (!checked) { path = getenv("ULTRA_JVS_LOG"); checked = 1; }
    if (!path || !*path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

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

void t3_iat_patch_all(const char* impdll, const char* impfn, void* newfn) {
    void* realfn = NULL;
    HMODULE imp = GetModuleHandleA(impdll);
    if (imp) realfn = (void*)GetProcAddress(imp, impfn);

    HMODULE mods[512]; DWORD need = 0;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&t3_iat_patch_all, &self);
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        DWORD count = need / sizeof(HMODULE);
        if (count > 512) count = 512;   /* `need` may exceed our fixed buffer -- clamp */
        for (DWORD i = 0; i < count; ++i)
            if (mods[i] != self)                 /* don't patch our own imports */
                iat_patch_module(mods[i], impdll, impfn, newfn, realfn);
    }
}

/* ============================================================================
 *  The one JVS node this process owns
 * ========================================================================= */
static ultra_jvs_node* g_node = NULL;

ultra_jvs_node* t3_hook_node(void) { return g_node; }

void t3_current_input(ultra_jvs_input* out) {
    jvs_get_input(g_node, out);
}

static void configure_node(void) {
    g_node = jvs_create();
    if (!g_node) return;
    const char* id = getenv("ULTRA_JVS_ID");
    if (id && *id) jvs_set_id(g_node, id);
    int screenpos = 0;
    const char* sp = getenv("ULTRA_JVS_SCREENPOS");
    if (sp) screenpos = (sp[0] == '1');
    uint8_t analog_ch = 0;   /* 0 = keep node default (8) */
    const char* ach = getenv("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= JVS_MAX_ANALOG) analog_ch = (uint8_t)v; }
    jvs_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* ============================================================================
 *  Standalone input reader: XInput gamepad + keyboard fallback.
 *
 *  taito_typex3_main.c (this board's spawner) sets no shared-memory IPC
 *  contract today -- unlike native_x86's boards, which read the loader's
 *  input mapping through a named file mapping (ULTRA_IO_SHM). Until that
 *  wiring exists for this board, the hook reads a physical controller
 *  itself, exactly as every other independent board hook in this project
 *  does when no loader feed is present, so the JVS node (and therefore
 *  Fast-I/O) reports real state instead of a permanently-neutral snapshot.
 *
 *  Keyboard mapping (test/attract-friendly defaults, all standard VKs):
 *    1 = P1 START, 5 = P1 COIN, F2 = TEST, F1 = SERVICE
 *  XInput mapping (pad 0): D-pad/stick = P1 UP/DOWN/LEFT/RIGHT,
 *    A/B/X/Y = PUSH1..4, Start = START, Back = COIN (edge-triggered),
 *    LB = SERVICE.
 * ========================================================================= */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } t3_xpad;
typedef struct { DWORD packet; t3_xpad pad; } t3_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, t3_xstate*);
static p_xinput_get s_xget = NULL;
static volatile LONG g_stop = 0;

static DWORD WINAPI t3_input_thread(LPVOID arg) {
    (void)arg;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
    for (int i = 0; dlls[i] && !s_xget; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) s_xget = (p_xinput_get)(void*)GetProcAddress(h, "XInputGetState");
    }
    t3_log("input: XInput %s", s_xget ? "available" : "unavailable (keyboard-only fallback)");

    int coin_was = 0;
    while (!g_stop) {
        uint8_t byte1 = 0, byte2 = 0, sysb = 0;
        uint16_t ax0 = 0x8000, ax1 = 0x8000;
        int coin_now = 0;

        /* keyboard fallback (works even with no gamepad plugged in) */
        if (GetAsyncKeyState('1')   & 0x8000) byte1 |= 0x80;   /* START   */
        if (GetAsyncKeyState(VK_F1) & 0x8000) byte1 |= 0x40;   /* SERVICE */
        if (GetAsyncKeyState(VK_F2) & 0x8000) sysb  |= 0x80;   /* TEST    */
        if (GetAsyncKeyState('5')   & 0x8000) coin_now = 1;    /* COIN    */
        if (GetAsyncKeyState(VK_UP)    & 0x8000) byte1 |= 0x20;
        if (GetAsyncKeyState(VK_DOWN)  & 0x8000) byte1 |= 0x10;
        if (GetAsyncKeyState(VK_LEFT)  & 0x8000) byte1 |= 0x08;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) byte1 |= 0x04;
        if (GetAsyncKeyState('Z') & 0x8000) byte1 |= 0x02;     /* PUSH1 */
        if (GetAsyncKeyState('X') & 0x8000) byte1 |= 0x01;     /* PUSH2 */
        if (GetAsyncKeyState('C') & 0x8000) byte2 |= 0x80;     /* PUSH3 */
        if (GetAsyncKeyState('V') & 0x8000) byte2 |= 0x40;     /* PUSH4 */

        if (s_xget) {
            t3_xstate st; memset(&st, 0, sizeof st);
            if (s_xget(0, &st) == 0) {
                WORD b = st.pad.wButtons;
                if (b & 0x0001) byte1 |= 0x20;                 /* DPAD_UP    */
                if (b & 0x0002) byte1 |= 0x10;                 /* DPAD_DOWN  */
                if (b & 0x0004) byte1 |= 0x08;                 /* DPAD_LEFT  */
                if (b & 0x0008) byte1 |= 0x04;                 /* DPAD_RIGHT */
                if (b & 0x0010) byte1 |= 0x80;                 /* START      */
                if (b & 0x0100) byte1 |= 0x40;                 /* LB->SERVICE*/
                if (b & 0x0020) sysb  |= 0x80;                 /* BACK->TEST */
                if (b & 0x1000) byte1 |= 0x02;                 /* A->PUSH1   */
                if (b & 0x2000) byte1 |= 0x01;                 /* B->PUSH2   */
                if (b & 0x4000) byte2 |= 0x80;                 /* X->PUSH3   */
                if (b & 0x8000) byte2 |= 0x40;                 /* Y->PUSH4   */
                if (st.pad.lx < -10000) byte1 |= 0x08; else if (st.pad.lx > 10000) byte1 |= 0x04;
                if (st.pad.ly >  10000) byte1 |= 0x20; else if (st.pad.ly < -10000) byte1 |= 0x10;
                ax0 = (uint16_t)((int)st.pad.lx + 0x8000);
                ax1 = (uint16_t)(0x7FFF - (int)st.pad.ly);
                if (st.pad.bRT > 128) coin_now = 1;            /* right trigger -> coin */
            }
        }

        if (g_node) {
            ultra_jvs_input in; memset(&in, 0, sizeof in);
            in.system    = sysb;
            in.player[0] = (uint16_t)((byte1 << 8) | byte2);
            in.analog[0] = ax0; in.analog[1] = ax1;
            for (int a = 2; a < JVS_MAX_ANALOG; ++a) in.analog[a] = 0x8000;
            in.screen_x = in.screen_y = 0x8000;
            jvs_set_input(g_node, &in);

            if (coin_now && !coin_was) jvs_insert_coin(g_node, 0, 1);
        }
        coin_was = coin_now;

        Sleep(8);   /* ~125 Hz poll */
    }
    return 0;
}
static void t3_input_start(void) { CreateThread(NULL, 0, t3_input_thread, NULL, 0, NULL); }

/* ============================================================================
 *  Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 *  imports). The first install already happened synchronously in DllMain;
 *  every install() below is idempotent (name+value based patch), so calling
 *  again is safe.
 * ========================================================================= */
static DWORD WINAPI t3_worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        t3_fastio_install();
        t3_d3d9_install();
        t3_audio_install();
    }
    return 0;
}

/* ---- crash diagnostics (VEH) ------------------------------------------------ */
static LONG WINAPI t3_crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "(unknown)";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
#ifdef _WIN64
        t3_log("CRASH: ACCESS_VIOLATION at RIP=%p in %s", addr, modname);
#else
        t3_log("CRASH: ACCESS_VIOLATION at EIP=%p in %s", addr, modname);
#endif
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* -------------------------------------------------------------------------- */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        /* FIRST. The hook is injected into a SUSPENDED process, so this
         * runs before the game's entry point: the tracer therefore sees the
         * very first fault and the very first failed load. Observe-only -
         * it swallows no exception and changes no return value.
         * ULTRA_TRACE=0 disables. See work/include/ultra_tracer.h. */
        ultra_tracer_install();

        /* TOUCH. This title is played on a touch PANEL, not with buttons:
         * game.exe imports RegisterTouchWindow + GetTouchInputInfo and the C#
         * uses Unity's Input.touchCount, while ismACIO carries only the cabinet
         * panel (test / service / coin). A mouse is not a substitute -- Windows
         * routes mouse and touch through separate paths and a game asking for
         * touch never sees a mouse at all.
         *
         * ultra_touch synthesises REAL touch with InjectTouchInput, so the
         * game's own RegisterTouchWindow path receives genuine WM_TOUCH and
         * nothing has to be patched. Verified against a window registered
         * exactly the way game.exe registers its own.
         *
         * It spawns a thread and does nothing else here, so it is safe under
         * the loader lock. ULTRA_TOUCH=0 disables. */
        ultra_touch_install();

        /* CURSOR. The game hides the pointer because the cabinet has a real
         * touch panel; on a PC that leaves the player aiming something they
         * cannot see. cursor_typex3.c answers WM_SETCURSOR so it stays drawn.
         * It never MOVES the pointer -- ultra_touch injects at the cursor's
         * own position, so a second mover would feed back. ULTRA_CURSOR=0. */
        ultra_cursor_install();


    {   /* Universal transport layer: declare this board's shape so the shared
         * pacing/range/codec core is live and self-describing in the log.
         * Serial boards are REQUEST/RESPONSE -- their reply queue already ends
         * the game's read loop, so pacing is deliberately inert here; injecting
         * empty reads would truncate a reply frame. Behavior is unchanged. */
        static int s_utx_once;
        if (!s_utx_once) { static utx_profile utxp;
            s_utx_once = 1;
            memset(&utxp, 0, sizeof utxp);
            utxp.board        = "taito_typex3";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        AddVectoredExceptionHandler(1, t3_crash_handler);
        t3_log("=== ultra_typex_hook{32,64}.dll attached (Taito Type X3, independent build) pid=%lu (%d-bit) ===",
               GetCurrentProcessId(), (int)(sizeof(void*) * 8));
        /* Install SYNCHRONOUSLY: the module injects us into the SUSPENDED
         * game and resumes right after, so hooks must be in place before the
         * game's first thread runs. */
        configure_node();
        t3_fastio_install();
        t3_d3d9_install();
        t3_audio_install();
        t3_input_start();
        /* Ultra Omni-Bridge: opt-in (ULTRA_BRIDGE_ENABLE, default OFF) raw
         * keyboard/mouse-emulation bridge for titles that read OS input
         * directly. Installed after t3_d3d9_install() so the window getter
         * has the D3D9 present hook armed first. This board has no live
         * lightgun/shared-pose source of its own, so get_pose is NULL --
         * the bridge only ever drives the cursor from an XInput gamepad. */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(t3_d3d9_get_window, NULL);
ultra_pausemenu_install(t3_d3d9_get_window);
        CreateThread(NULL, 0, t3_worker, NULL, 0, NULL);
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();
ultra_pausemenu_shutdown();
        t3_d3d9_teardown();
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader/user can confirm the right (independent)
 * DLL was injected, distinct from any other board's hook. */
__declspec(dllexport) const char* ultra_taito_typex3_hook_id(void) {
    return "Ultra Loader Taito Type X3 I/O hook (independent, clean-room) v1";
}

#endif /* _WIN32 */
