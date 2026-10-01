/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_it.c -- DllMain + shared plumbing for the Incredible
 *                       Technologies board's own hook DLL
 *                       (ultra_incredible_technologies_hook32.dll)
 * =============================================================================
 *
 *  This TU owns:
 *    - DllMain (fires every subsystem installer on DLL_PROCESS_ATTACH),
 *    - ul_it_log            -- stderr (+ optional ULTRA_JVS_LOG file) sink,
 *    - ul_it_iat_patch_all  -- the RVA-bounded IAT walker jvs_serial_hook_it.c,
 *                              d3d9_throttle_it.c and audio_hook_it.c all use,
 *    - ul_it_env / ul_it_env_on / ul_it_env_or -- env helpers,
 *    - ul_it_node / ul_it_pull_input -- the one JVS node + the trackball/
 *      keyboard poll thread that feeds it.
 *
 *  CABINET INPUT: per this module's own UNTESTED.txt research note, a Golden
 *  Tee-family trackball enumerates as a PS/2 or USB HID mouse. This thread
 *  reads the real Windows cursor's relative motion each tick and folds it
 *  into the JVS node's rotary[] counters (jvs_core_it.c's READ_ROTARY path),
 *  exactly the "route through as a mouse-class device" design this module's
 *  own notes call for. Two buttons close the loop: mouse LEFT = shot/swing
 *  trigger, mouse RIGHT = view/club-select -- the two buttons a Golden Tee
 *  trackball housing actually has. Keyboard supplies the cabinet's fixed
 *  panel buttons (Start/Coin/Test/Service), since those are ordinary momentary
 *  switches, not part of the trackball assembly.
 *
 *  Injected into the game by incredible_technologies_main.c via a suspended-
 *  launch + QueueUserAPC(LoadLibraryA) (primary) with a CreateRemoteThread
 *  (LoadLibraryA) fallback. Every installer below is idempotent and safe to
 *  call unconditionally -- this DLL is ONLY ever loaded into a game already
 *  resolved to the incredible_technologies board.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "it_hook.h"
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
/*  Log sink                                                                    */
/* -------------------------------------------------------------------------- */
void ul_it_log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[it-hook] %s", buf);
    if (buf[0] && buf[strlen(buf) - 1] != '\n') fputc('\n', stderr);
    fflush(stderr);

    const char* p = getenv("ULTRA_JVS_LOG");
    if (p && *p) {
        FILE* f = fopen(p, "a");
        if (f) {
            fputs("[it-hook] ", f);
            fputs(buf, f);
            if (buf[0] && buf[strlen(buf) - 1] != '\n') fputc('\n', f);
            fclose(f);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  Env helpers                                                                 */
/* -------------------------------------------------------------------------- */
const char* ul_it_env(const char* name) {
    static char buf[8][512];
    static int  slot = 0;
    char* out = buf[slot]; slot = (slot + 1) % 8;
    DWORD n = GetEnvironmentVariableA(name, out, 512);
    if (n == 0 || n >= 512) return NULL;
    return out;
}
int ul_it_env_on(const char* name) {
    const char* v = ul_it_env(name);
    return v && v[0] == '1' && v[1] == 0;
}
const char* ul_it_env_or(const char* name, const char* dflt) {
    const char* v = ul_it_env(name);
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

void ul_it_iat_patch_all(const char* impdll, const char* impfn, void* newfn) {
    void* realfn = NULL;
    HMODULE imp = GetModuleHandleA(impdll);
    if (imp) realfn = (void*)GetProcAddress(imp, impfn);

    HMODULE mods[512]; DWORD need = 0;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ul_it_iat_patch_all, &self);
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        DWORD count = need / sizeof(HMODULE);
        if (count > 512) count = 512;
        for (DWORD i = 0; i < count; ++i)
            if (mods[i] != self)
                iat_patch_module(mods[i], impdll, impfn, newfn, realfn);
    }
}

/* -------------------------------------------------------------------------- */
/*  The JVS node + trackball(mouse)/keyboard poll thread                       */
/* -------------------------------------------------------------------------- */
static ultra_jvs_node*  g_node = NULL;
static ultra_jvs_input  g_input;
static CRITICAL_SECTION g_input_lock;
static volatile LONG    g_stop = 0;

ultra_jvs_node* ul_it_node(void) { return g_node; }

void ul_it_pull_input(ultra_jvs_node* n) {
    if (!n) return;
    ultra_jvs_input in;
    EnterCriticalSection(&g_input_lock);
    in = g_input;
    LeaveCriticalSection(&g_input_lock);
    jvs_set_input(n, &in);
}

/* Whether the foreground window belongs to THIS process -- gates cursor
 * recentering so the hook never hijacks the mouse away from some other
 * foreground app (e.g. a debug console) during a test run. */
static int game_has_focus(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static DWORD WINAPI trackball_thread(LPVOID arg) {
    (void)arg;
    ul_it_log("input: trackball(mouse)+keyboard poll thread started\n");

    /* Recentering the real cursor every tick is the standard way to turn an
     * absolute mouse position into unbounded relative motion (the same trick
     * FPS mouselook uses) -- essential here since a physical trackball has no
     * positional limit but a Windows cursor is clamped to the screen. Off by
     * default only when ULTRA_IT_NO_MOUSE=1 (e.g. running headless/CI where
     * there is no real display to move a cursor on). */
    const int mouse_enabled = !ul_it_env_on("ULTRA_IT_NO_MOUSE");
    int cx = GetSystemMetrics(SM_CXSCREEN) / 2;
    int cy = GetSystemMetrics(SM_CYSCREEN) / 2;
    if (mouse_enabled) SetCursorPos(cx, cy);

    uint16_t rot_x = 0x0000, rot_y = 0x0000;
    int coin_key_was = 0;

    while (!g_stop) {
        ultra_jvs_input in; memset(&in, 0, sizeof in);
        for (int a = 0; a < JVS_MAX_ANALOG; ++a) in.analog[a] = 0x8000;
        in.screen_x = in.screen_y = 0x8000;

        if (mouse_enabled && game_has_focus()) {
            POINT p;
            if (GetCursorPos(&p)) {
                int dx = p.x - cx, dy = p.y - cy;
                if (dx || dy) {
                    /* Free-running counters: real trackball hardware wraps,
                     * and the host game only ever diffs two consecutive
                     * reads, so wraparound (uint16_t arithmetic) is correct,
                     * not a bug. */
                    rot_x = (uint16_t)(rot_x + dx);
                    rot_y = (uint16_t)(rot_y + dy);
                    SetCursorPos(cx, cy);   /* recenter so motion never clamps */
                }
            }
        }
        in.rotary[0] = rot_x;
        in.rotary[1] = rot_y;

        int shot = mouse_enabled && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
        int view = mouse_enabled && (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
        int start = (GetAsyncKeyState('1') & 0x8000) || (GetAsyncKeyState(VK_RETURN) & 0x8000);
        int service = (GetAsyncKeyState(VK_F1) & 0x8000);
        int test = (GetAsyncKeyState(VK_F2) & 0x8000);
        int coin_now = (GetAsyncKeyState('5') & 0x8000) != 0;

        uint8_t byte1 = (uint8_t)((start<<7)|(service<<6)|(shot<<1));
        uint8_t byte2 = (uint8_t)((view<<7));
        in.player[0] = (uint16_t)((byte1 << 8) | byte2);
        in.system = test ? 0x80 : 0;

        if (coin_now && !coin_key_was && g_node) jvs_insert_coin(g_node, 0, 1);
        coin_key_was = coin_now;

        EnterCriticalSection(&g_input_lock);
        g_input = in;
        LeaveCriticalSection(&g_input_lock);

        Sleep(8);   /* ~125 Hz poll -- matches the cadence used across this project */
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
            utxp.board        = "incredible_technologies";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(hinst);
        InitializeCriticalSection(&g_input_lock);
        memset(&g_input, 0, sizeof g_input);
        for (int a = 0; a < JVS_MAX_ANALOG; ++a) g_input.analog[a] = 0x8000;
        g_input.screen_x = g_input.screen_y = 0x8000;

        g_node = jvs_create();

        ul_it_log("attach: ultra_incredible_technologies_hook loaded pid=%lu (%d-bit)\n",
                  GetCurrentProcessId(), (int)(sizeof(void*) * 8));

        /* Order: D3D9 present first (so a splash/first-frame present is
         * already capped/windowed), then the JVS I/O transport, then audio,
         * then start the trackball/keyboard poll thread. */
        ul_it_install_d3d9();
        ul_it_jvs_serial_install();
        ul_it_install_audio();
        CreateThread(NULL, 0, trackball_thread, NULL, 0, NULL);

        /* Ultra Omni-Bridge (5th Omni service, opt-in): keyboard/mouse
         * emulation from a live gamepad, for an IT title that turns out to
         * read raw OS input directly instead of JVS (the pattern proven on
         * Type X2's Haunted Museum). Installed AFTER ul_it_install_d3d9()
         * above so the D3D9 present hook is already armed before the
         * bridge's poll thread starts asking ul_it_get_render_window() for a
         * window -- the getter just returns NULL (bridge stays inert) until
         * CreateDevice fires. This board's own cabinet-input path
         * (trackball_thread above) READS the real Windows mouse to feed JVS
         * rotary counters -- the opposite direction from Omni-Bridge, which
         * WRITES synthetic mouse/keyboard input for a game to read -- so
         * there is no overlap/conflict to gate around. No lightgun/shared-
         * pose source exists for this board (Golden Tee-family cabinets are
         * trackball, not lightgun), so get_pose is NULL: gamepad-to-cursor
         * only. Default OFF (ULTRA_BRIDGE_ENABLE, read via omni_client.h) --
         * this is framework rollout, not a default-behavior change; a user
         * or a future session turns it on per-game once verified against a
         * real IT dump. */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(ul_it_get_render_window, NULL);
ultra_pausemenu_install(ul_it_get_render_window);

        ul_it_log("attach: all subsystem installers fired\n");
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();   /* signal stop + force-release any held mouse buttons */
ultra_pausemenu_shutdown();
        if (g_node) { jvs_destroy(g_node); g_node = NULL; }
        break;
    default: break;
    }
    return TRUE;
}

/* Explicit export so the loader/diagnostics can confirm the right DLL loaded. */
__declspec(dllexport) const char* ultra_incredible_technologies_hook_id(void) {
    return "Ultra Loader Incredible Technologies board hook (clean-room, independent) v1";
}

#endif /* _WIN32 */
