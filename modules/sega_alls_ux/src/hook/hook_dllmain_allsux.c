/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_allsux.c -- DllMain + shared plumbing for the Sega ALLS UX
 *                           board's own hook DLL (ultra_sega_alls_ux_hook{32,64}.dll)
 * =============================================================================
 *
 *  This TU owns:
 *    - DllMain (fires every subsystem installer on DLL_PROCESS_ATTACH),
 *    - ul_ax_log             -- stderr (+ optional ULTRA_JVS_LOG file) sink,
 *    - ul_ax_iat_patch_all   -- the RVA-bounded IAT walker every other TU in
 *                               this tree uses to hook a game import,
 *    - ul_ax_env / ul_ax_env_on / ul_ax_env_or -- env helpers,
 *    - ul_ax_current_input / ul_ax_drain_coin  -- the one cabinet-input source
 *      (a local XInput gamepad poll -- this module has NO shared-memory
 *      dependency on the loader or on native_x86, it is fully self-contained).
 *
 *  Injected into the game by sega_alls_ux_main.c via a suspended-launch +
 *  QueueUserAPC(LoadLibraryA) (primary) with a CreateRemoteThread(LoadLibraryA)
 *  fallback (see that file). Every installer below is idempotent and safe to
 *  call unconditionally -- this DLL is ONLY ever loaded into a game already
 *  resolved to the sega_alls_ux board.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "allsux_hook.h"
#include "omni_bridge.h"

#include "ultra_transport.h"   /* shared universal I/O transport layer */

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
/*  Log sink                                                                    */
/* -------------------------------------------------------------------------- */
void ul_ax_log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[allsux-hook] %s", buf);
    if (buf[0] && buf[strlen(buf) - 1] != '\n') fputc('\n', stderr);
    fflush(stderr);

    /* optional mirror to a diagnostic file (same knob every other board's
     * hook in this project honors) so a test run can be compared side by side. */
    const char* p = getenv("ULTRA_JVS_LOG");
    if (p && *p) {
        FILE* f = fopen(p, "a");
        if (f) {
            fputs("[allsux-hook] ", f);
            fputs(buf, f);
            if (buf[0] && buf[strlen(buf) - 1] != '\n') fputc('\n', f);
            fclose(f);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  Env helpers                                                                 */
/* -------------------------------------------------------------------------- */
const char* ul_ax_env(const char* name) {
    static char buf[8][512];
    static int  slot = 0;
    char* out = buf[slot]; slot = (slot + 1) % 8;
    DWORD n = GetEnvironmentVariableA(name, out, 512);
    if (n == 0 || n >= 512) return NULL;
    return out;
}
int ul_ax_env_on(const char* name) {
    const char* v = ul_ax_env(name);
    return v && v[0] == '1' && v[1] == 0;
}
const char* ul_ax_env_or(const char* name, const char* dflt) {
    const char* v = ul_ax_env(name);
    return (v && *v) ? v : dflt;
}

/* -------------------------------------------------------------------------- */
/*  Generic IAT patcher (RVA-bounded; same proven shape used across every      */
/*  independent per-board hook in this project).                               */
/* -------------------------------------------------------------------------- */
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

void ul_ax_iat_patch_all(const char* impdll, const char* impfn, void* newfn) {
    void* realfn = NULL;
    HMODULE imp = GetModuleHandleA(impdll);
    if (imp) realfn = (void*)GetProcAddress(imp, impfn);

    HMODULE mods[512]; DWORD need = 0;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ul_ax_iat_patch_all, &self);
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        DWORD count = need / sizeof(HMODULE);
        if (count > 512) count = 512;
        for (DWORD i = 0; i < count; ++i)
            if (mods[i] != self)
                iat_patch_module(mods[i], impdll, impfn, newfn, realfn);
    }
}

/* -------------------------------------------------------------------------- */
/*  Cabinet input source: local XInput gamepad poll.                           */
/*                                                                              */
/*  This module has no loader shared-memory block (unlike native_x86's fuller  */
/*  architecture) -- by design, per the independence directive, so it reads    */
/*  the player's own gamepad directly. Mapped onto the JVS switch/analog       */
/*  layout every subsystem here shares:                                        */
/*    A/B/X/Y -> PUSH1..4, LB/RB -> PUSH5/6, Start -> START, Back -> TEST,      */
/*    D-pad/left-stick -> UP/DOWN/LEFT/RIGHT, right trigger -> coin insert.     */
/* -------------------------------------------------------------------------- */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } ul_ax_xpad;
typedef struct { DWORD packet; ul_ax_xpad pad; } ul_ax_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, ul_ax_xstate*);

static p_xinput_get    s_xget = NULL;
static ultra_jvs_input g_input;                 /* updated by the poll thread   */
static CRITICAL_SECTION g_input_lock;
static volatile LONG   g_coin_pulse = 0;        /* monotonic coin-edge counter  */
static volatile LONG   g_stop = 0;

void ul_ax_current_input(ultra_jvs_input* out) {
    if (!out) return;
    EnterCriticalSection(&g_input_lock);
    *out = g_input;
    LeaveCriticalSection(&g_input_lock);
}

unsigned ul_ax_drain_coin(void) {
    static LONG seen = 0;
    LONG now = g_coin_pulse;
    unsigned delta = (unsigned)(now - seen);
    seen = now;
    return delta;
}

static DWORD WINAPI xinput_thread(LPVOID arg) {
    (void)arg;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
    for (int i = 0; dlls[i] && !s_xget; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) s_xget = (p_xinput_get)(void*)GetProcAddress(h, "XInputGetState");
    }
    if (s_xget) ul_ax_log("input: XInput resolved, gamepad 0 will drive the cabinet\n");
    else        ul_ax_log("input: no XInput DLL found -- cabinet reads neutral/centered\n");

    int coin_was = 0;
    while (!g_stop) {
        ultra_jvs_input in; memset(&in, 0, sizeof in);
        for (int a = 0; a < JVS_MAX_ANALOG; ++a) in.analog[a] = 0x8000;
        in.screen_x = in.screen_y = 0x8000;

        if (s_xget) {
            ul_ax_xstate st; memset(&st, 0, sizeof st);
            if (s_xget(0, &st) == 0) {
                WORD b = st.pad.wButtons;
                int up=!!(b&0x0001), dn=!!(b&0x0002), lf=!!(b&0x0004), rt=!!(b&0x0008);
                int start=!!(b&0x0010), test=!!(b&0x0020) /*BACK*/, svc=!!(b&0x0100) /*LB*/;
                int p1=!!(b&0x1000)/*A*/, p2=!!(b&0x2000)/*B*/, p3=!!(b&0x4000)/*X*/,
                    p4=!!(b&0x8000)/*Y*/, p5=!!(b&0x0200)/*RB*/;
                if (st.pad.lx < -10000) lf = 1; else if (st.pad.lx > 10000) rt = 1;
                if (st.pad.ly >  10000) up = 1; else if (st.pad.ly < -10000) dn = 1;
                uint8_t byte1 = (uint8_t)((start<<7)|(svc<<6)|(up<<5)|(dn<<4)|(lf<<3)|(rt<<2)|(p1<<1)|p2);
                uint8_t byte2 = (uint8_t)((p3<<7)|(p4<<6)|(p5<<5));
                in.player[0] = (uint16_t)((byte1<<8)|byte2);
                in.system    = test ? 0x80 : 0;
                in.analog[0] = (uint16_t)((int)st.pad.lx + 0x8000);
                in.analog[1] = (uint16_t)(0x7FFF - (int)st.pad.ly);

                int coin_now = (st.pad.bRT > 128);
                if (coin_now && !coin_was) InterlockedIncrement(&g_coin_pulse);
                coin_was = coin_now;
            }
        }

        EnterCriticalSection(&g_input_lock);
        g_input = in;
        LeaveCriticalSection(&g_input_lock);
        Sleep(8);   /* ~125 Hz poll, matches the cadence used across this project */
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  DllMain                                                                     */
/* -------------------------------------------------------------------------- */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:

    {   /* Universal transport layer: declare this board's shape so the shared
         * pacing/range/codec core is live and self-describing in the log.
         * Serial boards are REQUEST/RESPONSE -- their reply queue already ends
         * the game's read loop, so pacing is deliberately inert here; injecting
         * empty reads would truncate a reply frame. Behavior is unchanged. */
        static int s_utx_once;
        if (!s_utx_once) { static utx_profile utxp;
            s_utx_once = 1;
            memset(&utxp, 0, sizeof utxp);
            utxp.board        = "sega_alls_ux";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(hinst);
        InitializeCriticalSection(&g_input_lock);
        for (int a = 0; a < JVS_MAX_ANALOG; ++a) g_input.analog[a] = 0x8000;
        g_input.screen_x = g_input.screen_y = 0x8000;

        ul_ax_log("attach: ultra_sega_alls_ux_hook loaded pid=%lu (%d-bit)\n",
                  GetCurrentProcessId(), (int)(sizeof(void*) * 8));

        /* Order: D3D11 present first (so a splash/first-frame present is
         * already capped/windowed), then the I/O surfaces, then keychip
         * presence (device-open gates usually come after the render device
         * in a Win10 IoT title's boot path), then start the input thread. */
        ul_ax_install_d3d11();
        ul_ax_install_io4();
        ul_ax_install_dinput8();
        ul_ax_install_keychip();
        ul_ax_install_audio();
        CreateThread(NULL, 0, xinput_thread, NULL, 0, NULL);

        /* Ultra Omni-Bridge (5th Omni service): raw-input (SendInput cursor/
         * click/key) synthesis for a game that reads OS keyboard+mouse
         * directly instead of JVS/DirectInput. Installed last, after the
         * D3D11 present hook is armed (its window-getter needs a captured
         * swap-chain HWND). This board has no live lightgun/shared-pose
         * source of its own (screen_x/screen_y in ultra_jvs_input stay
         * neutral -- no gun-bridge TU exists in this tree), so get_pose is
         * NULL: gamepad-only cursor/button/key synthesis. Opt-in only --
         * omni_client_read_bridge() defaults ULTRA_BRIDGE_ENABLE off, and
         * nothing in this module's env block turns it on. */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(ul_ax_get_device_window, NULL);
ultra_pausemenu_install(ul_ax_get_device_window);

        ul_ax_log("attach: all subsystem installers fired\n");
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();
ultra_pausemenu_shutdown();
        ul_ax_teardown_d3d11();
        break;
    default: break;
    }
    return TRUE;
}

/* Explicit export so the loader/diagnostics can confirm the right DLL loaded. */
__declspec(dllexport) const char* ultra_sega_alls_ux_hook_id(void) {
    return "Ultra Loader Sega ALLS UX board hook (clean-room, independent) v1";
}

#endif /* _WIN32 */
