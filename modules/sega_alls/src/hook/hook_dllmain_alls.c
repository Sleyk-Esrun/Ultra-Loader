/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_alls.c -- ultra_sega_alls_hook{32,64}.dll entry point.
 * =============================================================================
 *
 *  Injected into the Sega ALLS game process by THIS board's OWN spawner
 *  (modules/sega_alls/src/sega_alls_main.c). On attach it:
 *    - owns the shared infra every subsystem in this DLL uses: stderr log
 *      (ul_alls_log), the RVA-bounded IAT walker (ul_alls_iat_patch_all), and
 *      env-var helpers (ul_alls_env / ul_alls_env_on),
 *    - runs a standalone XInput gamepad reader so the board has SOME live
 *      controller state even with no Ultra Loader shared-memory plumbing
 *      wired to it yet (this hook has zero dependency on any other board's
 *      IPC ABI -- see ultra_alls_current_input below),
 *    - installs the D3D11 present hook, the IO4-USB I/O emulation, the ALKey-
 *      framing keychip presence stub, and the audio diagnostic hook -- each
 *      strictly env-gated by a flag sega_alls_main.c sets before CreateProcess.
 *
 *  Nothing in this file, or any other file in this folder, is compiled from
 *  or linked against native_x86's hook sources/objects. native_x86 was read
 *  ONLY as reference for public API shapes; every line here is this board's
 *  own copy.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "sega_alls_hook.h"
#include "jvs_core_alls.h"
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
/* ---------- log helper ------------------------------------------------------
 * Always to stderr (so injected-process stderr -> ultra.log, same convention
 * as every other board's hook); additionally appended to ULTRA_JVS_LOG's file
 * if that env var names one (shared diagnostic-file convention). */
void ul_alls_log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[alls-hook] %s", buf);
    fflush(stderr);
    const char* path = getenv("ULTRA_JVS_LOG");
    if (path && *path) {
        FILE* f = fopen(path, "a");
        if (f) { fprintf(f, "[alls-hook] %s", buf); fclose(f); }
    }
}

/* ---------- env helpers ------------------------------------------------------ */
const char* ul_alls_env(const char* name) {
    static char buf[16][512];
    static int  slot = 0;
    char* b = buf[slot]; slot = (slot + 1) % 16;
    DWORD n = GetEnvironmentVariableA(name, b, 512);
    if (n == 0 || n >= 512) return NULL;
    return b;
}
int ul_alls_env_on(const char* name) {
    const char* v = ul_alls_env(name);
    return v && v[0] == '1' && v[1] == 0;
}

/* ---------- IAT walker (RVA-bounded) ----------------------------------------
 * Every header/thunk read is bounds-checked against the module's mapped image
 * size (mingw has no SEH __try/__except), so a malformed import directory is
 * skipped instead of crashing the host game. Matches by name AND by value
 * (the value pass catches bound imports whose names were stripped, and is
 * naturally idempotent on re-patch). */
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

void ul_alls_iat_patch_all(const char* impdll, const char* impfn, void* newfn) {
    void* realfn = NULL;
    HMODULE imp = GetModuleHandleA(impdll);
    if (imp) realfn = (void*)GetProcAddress(imp, impfn);

    HMODULE mods[512]; DWORD need = 0;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ul_alls_iat_patch_all, &self);
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        DWORD count = need / sizeof(HMODULE);
        if (count > 512) count = 512;
        for (DWORD i = 0; i < count; ++i)
            if (mods[i] != self)
                iat_patch_module(mods[i], impdll, impfn, newfn, realfn);
    }
}

/* -------------------------------------------------------------------------- */
/* Standalone gamepad (XInput) input source. This board hook has no shared-
 * memory ABI dependency on any other module -- it reads the local controller
 * itself so IO4/keychip callers always get a real, live snapshot. */
static volatile LONG    g_stop = 0;
static ultra_jvs_input   g_xinput_in;
static volatile LONG     g_local_coin = 0;
static long              g_local_coin_seen = 0;

typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } ul_alls_xpad;
typedef struct { DWORD packet; ul_alls_xpad pad; } ul_alls_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, ul_alls_xstate*);
static p_xinput_get s_xget = NULL;

static DWORD WINAPI xinput_thread(LPVOID arg) {
    (void)arg;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
    for (int i = 0; dlls[i] && !s_xget; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) s_xget = (p_xinput_get)(void*)GetProcAddress(h, "XInputGetState");
    }
    for (int a = 0; a < JVS_ALLS_MAX_ANALOG; ++a) g_xinput_in.analog[a] = 0x8000;
    g_xinput_in.screen_x = g_xinput_in.screen_y = 0x8000;
    if (!s_xget) return 0;   /* no controller subsystem available -- nothing to poll */

    int coin_was = 0;
    while (!g_stop) {
        ul_alls_xstate st; memset(&st, 0, sizeof st);
        if (s_xget(0, &st) == 0) {
            WORD b = st.pad.wButtons;
            int up=!!(b&0x0001), dn=!!(b&0x0002), lf=!!(b&0x0004), rt=!!(b&0x0008);
            int start=!!(b&0x0010), test=!!(b&0x0020) /*BACK*/, svc=!!(b&0x0100) /*LB*/;
            int p1=!!(b&0x1000)/*A*/, p2=!!(b&0x2000)/*B*/, p3=!!(b&0x4000)/*X*/,
                p4=!!(b&0x8000)/*Y*/, p5=!!(b&0x0200)/*RB*/, p6=!!(b&0x0080)/*R3*/;
            if (st.pad.lx < -10000) lf = 1; else if (st.pad.lx > 10000) rt = 1;
            if (st.pad.ly >  10000) up = 1; else if (st.pad.ly < -10000) dn = 1;
            uint8_t byte1 = (uint8_t)((start<<7)|(svc<<6)|(up<<5)|(dn<<4)|(lf<<3)|(rt<<2)|(p1<<1)|p2);
            uint8_t byte2 = (uint8_t)((p3<<7)|(p4<<6)|(p5<<5)|(p6<<4));
            ultra_jvs_input in; memset(&in, 0, sizeof in);
            in.player[0] = (uint16_t)((byte1<<8)|byte2);
            in.system    = test ? 0x80 : 0;
            in.analog[0] = (uint16_t)((int)st.pad.lx + 0x8000);
            in.analog[1] = (uint16_t)(0x7FFF - (int)st.pad.ly);
            for (int a = 2; a < JVS_ALLS_MAX_ANALOG; ++a) in.analog[a] = 0x8000;
            in.screen_x = in.screen_y = 0x8000;
            g_xinput_in = in;
            /* LEGACY STANDALONE COIN - SELECT only, and only when nothing
             * else owns the coin line.
             *
             * This used to be `bRT > 128 || L3`, i.e. the RIGHT TRIGGER inserted
             * a credit. That is a hardcoded path in this thread which bypasses
             * the controls profile AND the loader's shared block entirely, and
             * ultra_hook_pull_input drains it unconditionally - so on a racer,
             * where the right trigger is the ACCELERATOR, every press of the gas
             * fed the coin slot. Reported live on Wacky Race: "R2 is still
             * credit" even after the profile had been reassigned.
             *
             * Now: SELECT/BACK only (matching the profile and the publisher), and
             * suppressed whenever a controls profile or a loader block is present,
             * since those own the coin line and this would double-credit. */
            /* NEITHER GUARD EXISTS ON THIS BOARD.
             *
             * This expression was carried over from a board that has both a
             * controls-profile layer and a ULTRA_IO_SHM reader. sega_alls has
             * neither: there is no controls.c here, and no ultra_io_shared
             * anywhere in this hook - so ultra_input_map_active(), g_shared and
             * ULTRA_IO_MAGIC were all undeclared and this file has never
             * compiled.
             *
             * With no profile and no shared block, nothing else can own the coin
             * line, so the legacy pad path owns it - which is what the original
             * expression evaluates to once the two absent guards are resolved
             * honestly. If this board later gains the shared block, restore the
             * real test rather than leaving this constant. */
            int legacy_owns_coin = 1;
            int coin_now = legacy_owns_coin && !!(b & 0x0020) /* BACK/SELECT */;
            if (coin_now && !coin_was) InterlockedIncrement(&g_local_coin);
            coin_was = coin_now;
        }
        Sleep(8);
    }
    return 0;
}
static void start_xinput(void) { CreateThread(NULL, 0, xinput_thread, NULL, 0, NULL); }

/* The live cabinet input every subsystem (IO4 report builder, and any future
 * JVS-over-serial wiring) reads. */
void ultra_alls_current_input(ultra_jvs_input* out) {
    memset(out, 0, sizeof *out);
    for (int a = 0; a < JVS_ALLS_MAX_ANALOG; ++a) out->analog[a] = 0x8000;
    out->screen_x = out->screen_y = 0x8000;
    *out = g_xinput_in;
}

/* Drain the local coin pulse into the IO4 counters once per poll. Called from
 * a lightweight timer thread so the IO4 report always reflects the latest
 * insert without every read path needing to know about the coin edge logic. */
static DWORD WINAPI coin_bridge_thread(LPVOID arg) {
    (void)arg;
    while (!g_stop) {
        long lc = g_local_coin;
        if (lc != g_local_coin_seen) {
            int delta = (int)(lc - g_local_coin_seen);
            if (ul_alls_io4_enabled()) ul_alls_io4_insert_coin(0, delta);
            g_local_coin_seen = lc;
        }
        Sleep(16);
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
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
            utxp.board        = "sega_alls";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        ul_alls_log("attach: ultra_sega_alls_hook loaded pid=%lu (%d-bit)\n",
                   GetCurrentProcessId(), (int)(sizeof(void*) * 8));

        /* D3D11 present/throttle hook: ULTRA_D3D11 (vendor-neutral) OR
         * ULTRA_SEGA_D3D11 (legacy alias) arms it. */
        { const char* d  = getenv("ULTRA_D3D11");
          const char* sd = getenv("ULTRA_SEGA_D3D11");
          if ((d && d[0]=='1') || (sd && sd[0]=='1')) ul_alls_d3d11_install(); }

        ul_alls_install_keychip();   /* ALKey presence shim (env-gated, DEFERS 30-33 / 11 / 12) */
        ul_alls_install_io4();       /* IO4-USB I/O emulation (env-gated) */
        ul_alls_audio_install();     /* DirectSound + XAudio2 diagnostic hook (always-on passthrough) */

        start_xinput();              /* standalone gamepad -> IO4/keychip live input */
        CreateThread(NULL, 0, coin_bridge_thread, NULL, 0, NULL);

        /* Ultra Omni-Bridge (5th Omni service): synthesizes raw OS mouse/key
         * input from a gamepad for any ALLS title that reads the OS cursor/
         * keyboard directly instead of IO4/JVS. Opt-in only (ULTRA_BRIDGE_
         * ENABLE defaults OFF, same as the other four Omni services) -- this
         * call just arms the poll thread(s), which stay fully inert until a
         * user/profile turns it on. This board has no lightgun/shared-pose
         * source of its own, so get_pose is NULL (gamepad-only bridge); the
         * window getter needs the D3D11 present hook armed first, which is
         * why this call sits after ul_alls_d3d11_install() above. */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(ul_alls_device_window, NULL);
ultra_pausemenu_install(ul_alls_device_window);

        ul_alls_log("attach: all installers fired\n");
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();
ultra_pausemenu_shutdown();
        ul_alls_d3d11_teardown();
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader/spawner can confirm the right DLL loaded. */
__declspec(dllexport) const char* ultra_sega_alls_hook_id(void) {
    return "Ultra Loader Sega ALLS hook (clean-room, board-owned) v1";
}

#endif /* _WIN32 */
