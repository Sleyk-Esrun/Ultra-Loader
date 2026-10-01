/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_typex4.c -- Taito Type X4 hook DLL entry point.
 * =============================================================================
 *
 *  ultra_taito_typex4_hook{32,64}.dll is THIS board's own, fully independent
 *  hook: built ONLY from files in modules/taito_typex4/src/hook/, with no
 *  #include, link, or runtime dependency on modules/native_x86/src/hook/ or
 *  on modtest/. It is injected only by this board's own spawner
 *  (../taito_typex4_main.c), which launches a Type X4 game CREATE_SUSPENDED
 *  and either CreateRemoteThread(LoadLibraryA)'s this DLL in directly (rare
 *  32-bit Type X4 tooling) or hands the job to ultra-module-host.exe's
 *  `--inject` CLI (the project's shared, board-agnostic 64-bit injector --
 *  Type X4 titles are 64-bit).
 *
 *  On DLL_PROCESS_ATTACH this file:
 *    - creates the one clean-room JVS node (jvs_core_typex4.c) that both a
 *      legacy JVS-over-serial title and (far more commonly for this board)
 *      the Fast-I/O DMAC shim answer questions from,
 *    - starts a standalone input-capture thread (XInput first controller,
 *      keyboard fallback) since this board's own spawner does not (yet) set
 *      up a loader-side shared-memory input feed -- so a solo run is still
 *      actually controllable,
 *    - installs the Fast-I/O DMAC shim, the D3D11 present/capture hook, and
 *      the DirectSound/XAudio2 diagnostic hook.
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 API. No game
 *  data, no third-party loader source.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "jvs_core_typex4.h"
#include "hook_dllmain_typex4.h"
#include "omni_bridge.h"

#include "ultra_transport.h"   /* shared universal I/O transport layer */
#include "ultra_tracer.h"

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
/*  Diagnostic logger -- ULTRA_JVS_LOG=<path> arms it (same env var every      */
/*  other board in this project uses, so a user's existing log tooling works  */
/*  unmodified against this board too). No-op otherwise.                      */
/* -------------------------------------------------------------------------- */
void ultra_tx4_log(const char* fmt, ...) {
    const char* p = getenv("ULTRA_JVS_LOG");
    if (!p || !*p) return;
    FILE* f = fopen(p, "a");
    if (!f) return;
    fprintf(f, "[typex4] ");
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* -------------------------------------------------------------------------- */
/*  Generic IAT patcher -- this board's OWN copy. Walks every loaded module's  */
/*  import directory and redirects impdll!impfn to newfn, both by name (the   */
/*  normal case) and by value (catches an import whose name got stripped, and */
/*  makes a re-patch idempotent). All reads are bounds-checked against the    */
/*  module's mapped image size (mingw has no SEH __try/__except), so a        */
/*  malformed import directory is skipped rather than faulted on.             */
/* -------------------------------------------------------------------------- */
static int tx4_rva_ok(DWORD rva, DWORD isize) { return rva != 0 && rva < isize; }

static void tx4_iat_patch_module(HMODULE mod, const char* impdll,
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
    if (!tx4_rva_ok(dir.VirtualAddress, isize)) return;
    IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    for (; tx4_rva_ok(desc->Name, isize); ++desc) {
        const char* dllname = (const char*)(base + desc->Name);
        if (_stricmp(dllname, impdll) != 0) continue;
        DWORD orig_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        IMAGE_THUNK_DATA* orig = tx4_rva_ok(orig_rva, isize) ? (IMAGE_THUNK_DATA*)(base + orig_rva) : NULL;
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);
        for (; (size_t)((BYTE*)iat - base) + sizeof(*iat) <= (size_t)isize
               && (!orig || (size_t)((BYTE*)orig - base) + sizeof(*orig) <= (size_t)isize)
               && iat->u1.Function; ++iat, orig = orig ? orig + 1 : NULL) {
            int match = 0;
            if (orig && !(orig->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                && tx4_rva_ok((DWORD)orig->u1.AddressOfData, isize)) {
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

void ultra_tx4_iat_patch_all(const char* impdll, const char* impfn, void* newfn) {
    void* realfn = NULL;
    HMODULE imp = GetModuleHandleA(impdll);
    if (imp) realfn = (void*)GetProcAddress(imp, impfn);

    HMODULE mods[512]; DWORD need = 0;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ultra_tx4_iat_patch_all, &self);
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        DWORD count = need / sizeof(HMODULE);
        if (count > 512) count = 512;
        for (DWORD i = 0; i < count; ++i)
            if (mods[i] != self)
                tx4_iat_patch_module(mods[i], impdll, impfn, newfn, realfn);
    }
}

/* -------------------------------------------------------------------------- */
/*  The one JVS node + the live input snapshot every transport reads from.     */
/* -------------------------------------------------------------------------- */
static ultra_jvs_node_tx4* g_node = NULL;
static volatile LONG       g_stop = 0;

ultra_jvs_node_tx4* ultra_tx4_node(void) { return g_node; }

/* ---- standalone input capture (XInput controller 0, keyboard fallback) ---
 * This board's own spawner does not create a loader-side shared-memory input
 * feed (unlike native_x86's ultra_io_shared.h IPC, which is private to that
 * substrate's own launch path) -- so this hook reads the local controller
 * itself, same "standalone gamepad" fallback pattern proven project-wide,
 * which makes even a solo test launch actually controllable. */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } tx4_xpad;
typedef struct { DWORD packet; tx4_xpad pad; } tx4_xstate;
typedef DWORD (WINAPI *tx4_p_xinput_get)(DWORD, tx4_xstate*);
static tx4_p_xinput_get s_xget = NULL;
static ultra_jvs_input_tx4 g_live_in;
static volatile LONG        g_local_coin = 0;
static long                 g_local_coin_seen = 0;

static DWORD WINAPI tx4_input_thread(LPVOID arg) {
    (void)arg;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
    for (int i = 0; dlls[i] && !s_xget; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) s_xget = (tx4_p_xinput_get)(void*)GetProcAddress(h, "XInputGetState");
    }
    for (int a = 0; a < JVS_MAX_ANALOG; ++a) g_live_in.analog[a] = 0x8000;
    g_live_in.screen_x = g_live_in.screen_y = 0x8000;

    int coin_was = 0;
    while (!g_stop) {
        /* Keyboard fallback: TEST=F2, SERVICE=F1, coin=5, start=1, dirs=arrows,
         * two action buttons = Z/X. Works even with no controller attached. */
        SHORT vk_test = GetAsyncKeyState(VK_F2);
        SHORT vk_svc  = GetAsyncKeyState(VK_F1);
        SHORT vk_coin = GetAsyncKeyState('5');
        SHORT vk_start= GetAsyncKeyState('1');
        SHORT vk_up   = GetAsyncKeyState(VK_UP);
        SHORT vk_dn   = GetAsyncKeyState(VK_DOWN);
        SHORT vk_lf   = GetAsyncKeyState(VK_LEFT);
        SHORT vk_rt   = GetAsyncKeyState(VK_RIGHT);
        SHORT vk_p1   = GetAsyncKeyState('Z');
        SHORT vk_p2   = GetAsyncKeyState('X');

        int test=0, svc=0, up=0, dn=0, lf=0, rt=0, start=0, p1=0, p2=0;
        int coin_now = (vk_coin & 0x8000) != 0;
        if (vk_test  & 0x8000) test  = 1;
        if (vk_svc   & 0x8000) svc   = 1;
        if (vk_start & 0x8000) start = 1;
        if (vk_up    & 0x8000) up    = 1;
        if (vk_dn    & 0x8000) dn    = 1;
        if (vk_lf    & 0x8000) lf    = 1;
        if (vk_rt    & 0x8000) rt    = 1;
        if (vk_p1    & 0x8000) p1    = 1;
        if (vk_p2    & 0x8000) p2    = 1;

        int ax = 0x8000, ay = 0x8000;
        if (s_xget) {
            tx4_xstate st; memset(&st, 0, sizeof st);
            if (s_xget(0, &st) == 0) {
                WORD b = st.pad.wButtons;
                if (b & 0x0001) up = 1;      /* DPAD_UP    */
                if (b & 0x0002) dn = 1;      /* DPAD_DOWN  */
                if (b & 0x0004) lf = 1;      /* DPAD_LEFT  */
                if (b & 0x0008) rt = 1;      /* DPAD_RIGHT */
                if (b & 0x0010) start = 1;   /* START      */
                if (b & 0x0020) test  = 1;   /* BACK       */
                if (b & 0x0100) svc   = 1;   /* LB         */
                if (b & 0x1000) p1    = 1;   /* A          */
                if (b & 0x2000) p2    = 1;   /* B          */
                if (st.pad.lx < -10000) lf = 1; else if (st.pad.lx > 10000) rt = 1;
                if (st.pad.ly >  10000) up = 1; else if (st.pad.ly < -10000) dn = 1;
                ax = (int)st.pad.lx + 0x8000;
                ay = 0x7FFF - (int)st.pad.ly;
                if (st.pad.bRT > 128 || (b & 0x0040)) coin_now = 1;   /* RT or L3 */
            }
        }

        uint8_t byte1 = (uint8_t)((start<<7)|(svc<<6)|(up<<5)|(dn<<4)|(lf<<3)|(rt<<2)|(p1<<1)|p2);
        ultra_jvs_input_tx4 in; memset(&in, 0, sizeof in);
        in.player[0] = (uint16_t)(byte1 << 8);
        in.system    = test ? 0x80 : 0;
        in.analog[0] = (uint16_t)ax; in.analog[1] = (uint16_t)ay;
        for (int a = 2; a < JVS_MAX_ANALOG; ++a) in.analog[a] = 0x8000;
        in.screen_x = in.screen_y = 0x8000;
        g_live_in = in;

        if (coin_now && !coin_was) InterlockedIncrement(&g_local_coin);
        coin_was = coin_now;

        /* Keep the JVS node's snapshot + coin counters in sync too, so a
         * future legacy JVS-over-serial transport (or diagnostics reading
         * the node directly) sees the same live state Fast-I/O already
         * reads via ultra_tx4_current_input(). */
        ultra_tx4_pull_input();

        Sleep(8);
    }
    return 0;
}

void ultra_tx4_current_input(ultra_jvs_input_tx4* out) {
    if (!out) return;
    *out = g_live_in;
}

void ultra_tx4_pull_input(void) {
    if (!g_node) return;
    ultra_jvs_input_tx4 in;
    ultra_tx4_current_input(&in);
    jvs_tx4_set_input(g_node, &in);

    long lc = g_local_coin;
    if (lc != g_local_coin_seen) {
        int delta = (int)(lc - g_local_coin_seen);
        jvs_tx4_insert_coin(g_node, 0, delta);
        g_local_coin_seen = lc;
    }
}

/* -------------------------------------------------------------------------- */
static void tx4_configure_node(void) {
    g_node = jvs_tx4_create();
    if (!g_node) return;
    const char* id = getenv("ULTRA_JVS_ID");
    if (id && *id) jvs_tx4_set_id(g_node, id);
    int screenpos = 0;
    const char* sp = getenv("ULTRA_JVS_SCREENPOS");
    if (sp) screenpos = (sp[0] == '1');
    uint8_t analog_ch = 0;
    const char* ach = getenv("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= JVS_MAX_ANALOG) analog_ch = (uint8_t)v; }
    jvs_tx4_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* Periodic re-patch: catches modules the game loads AFTER our synchronous
 * DllMain install (delay-loaded imports, render/audio plugins). */
static DWORD WINAPI tx4_repatch_worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        ultra_tx4_fastio_install();   /* idempotent */
        ultra_tx4_audio_install();    /* idempotent */
        ultra_tx4_felica_stub_install(); /* idempotent (env-gated, OFF by default) */
    }
    return 0;
}

/* ---- crash diagnostics (VEH), same shape as the project's other boards --- */
static LONG WINAPI ultra_tx4_crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "(unknown)";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
#ifdef _WIN64
        ultra_tx4_log("CRASH: ACCESS_VIOLATION at RIP=%p in %s", addr, modname);
        ultra_tx4_log("CRASH: RAX=%p RBX=%p RCX=%p RDX=%p",
            (void*)ep->ContextRecord->Rax, (void*)ep->ContextRecord->Rbx,
            (void*)ep->ContextRecord->Rcx, (void*)ep->ContextRecord->Rdx);
#else
        ultra_tx4_log("CRASH: ACCESS_VIOLATION at EIP=%p in %s", addr, modname);
        ultra_tx4_log("CRASH: EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX",
            ep->ContextRecord->Eax, ep->ContextRecord->Ebx,
            ep->ContextRecord->Ecx, ep->ContextRecord->Edx);
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


    {   /* Universal transport layer: declare this board's shape so the shared
         * pacing/range/codec core is live and self-describing in the log.
         * Serial boards are REQUEST/RESPONSE -- their reply queue already ends
         * the game's read loop, so pacing is deliberately inert here; injecting
         * empty reads would truncate a reply frame. Behavior is unchanged. */
        static int s_utx_once;
        if (!s_utx_once) { static utx_profile utxp;
            s_utx_once = 1;
            memset(&utxp, 0, sizeof utxp);
            utxp.board        = "taito_typex4";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        AddVectoredExceptionHandler(1, ultra_tx4_crash_handler);
        ultra_tx4_log("DLL_PROCESS_ATTACH -- Taito Type X4 hook (independent build)");

        tx4_configure_node();
        CreateThread(NULL, 0, tx4_input_thread, NULL, 0, NULL);

        ultra_tx4_fastio_install();    /* Taito Fast-I/O DMAC shim (this board's transport) */
        ultra_tx4_d3d11_install();     /* D3D11/DXGI present + capture (this board is D3D11-only) */
        ultra_tx4_audio_install();     /* DirectSound + XAudio2 diagnostic hook */
        ultra_tx4_felica_stub_install(); /* Sony FeliCa NFC library stand-in (env-gated, OFF by default) */

        /* Ultra Omni-Bridge: opt-in gamepad/lightgun -> synthesized OS
         * cursor+click+key bridge for a Type X4 title that reads raw
         * mouse/keyboard instead of JVS/DirectInput directly (proven pattern
         * on taito_typex2 / Haunted Museum). Installed AFTER the D3D11 hook
         * above so ultra_tx4_device_window() has a real swap chain to report
         * once one is created; the bridge itself polls that getter and stays
         * fully inert until a window shows up. This board has no lightgun/
         * shared-pose source of its own, so get_pose is NULL -- the bridge
         * only ever drives the cursor from an XInput gamepad. Still defaults
         * OFF (ULTRA_BRIDGE_ENABLE=0) via omni_client_read_bridge(); this
         * call just arms the option for the same per-game opt-in a user
         * enables through the other four Omni services. */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(ultra_tx4_device_window, NULL);
ultra_pausemenu_install(ultra_tx4_device_window);

        CreateThread(NULL, 0, tx4_repatch_worker, NULL, 0, NULL);
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();
ultra_pausemenu_shutdown();
        ultra_tx4_d3d11_teardown();
        if (g_node) { jvs_tx4_destroy(g_node); g_node = NULL; }
        break;
    default: break;
    }
    return TRUE;
}

/* Explicit diagnostic export so the spawner/user can confirm the right DLL
 * was injected. */
__declspec(dllexport) const char* ultra_taito_typex4_hook_id(void) {
    return "Ultra Loader Taito Type X4 hook (independent, clean-room JVS + Fast-I/O + D3D11) v1";
}

#endif /* _WIN32 */
