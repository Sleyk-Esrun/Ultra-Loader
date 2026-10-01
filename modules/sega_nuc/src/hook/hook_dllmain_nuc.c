/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_nuc.c -- Sega Nu Compact board's OWN injected hook: DllMain,
 *                        IAT patcher, device-open dispatch, and the live
 *                        cabinet-input source every other file in this DLL
 *                        (sega_io4_nuc.c) reads from.
 * =============================================================================
 *
 *  This DLL (ultra_sega_nuc_hook{32,64}.dll) is injected by sega_nuc_main.c
 *  into a suspended Sega Nu Compact game process. It is fully self-contained:
 *  no shared runtime binary with any other board, no dependency on
 *  native_x86/src/hook's compiled output, and no dependency on any other
 *  board's private fork. modules/native_x86/src/hook/{jvs_serial_hook.c,
 *  hook_dllmain.c} were read for REFERENCE ONLY (per this board's build
 *  directive) to model the IAT-patch mechanism and the DllMain install
 *  contract; nothing is linked from them.
 *
 *  ARCHITECTURE NOTE -- why CreateFileA/W/ReadFile/WriteFile/GetProcAddress
 *  hooking is centralized HERE rather than inside sega_keychip_nuc.c /
 *  sega_io4_nuc.c (unlike the native_x86 reference, where each board-specific
 *  file IAT-patches those slots itself): this board needs BOTH a keychip
 *  presence shim (CreateFileA/W on an mxparallel-shaped path) AND an IO4-USB
 *  presence shim (CreateFileA/W + ReadFile/WriteFile on a VID/PID-shaped
 *  path) at the same time. IAT-patching is a last-write-wins operation on one
 *  function-pointer slot -- if each subsystem patched CreateFileA itself,
 *  whichever install() ran last would silently steal the slot from the other,
 *  and only one of the two device shims would ever actually fire. So this
 *  file owns the ONE set of CreateFileA/W/ReadFile/WriteFile hooks and
 *  dispatches to whichever subsystem's (pure, non-hooking) match predicate
 *  says it owns the call; each subsystem still owns 100% of its own protocol
 *  logic and its own ethical presence-only boundary.
 *
 *  INSTALL SEQUENCE (DLL_PROCESS_ATTACH):
 *    1. Resolve the real (unpatched) kernel32 functions we're about to hook.
 *    2. Create + configure this board's own JVS node (diagnostics /
 *       introspection; see jvs_core_nuc.h for why it's kept even though the
 *       live transport to the game is IO4-USB, not JVS-serial).
 *    3. ultra_sega_keychip_install() / ultra_sega_io4_install() -- each reads
 *       its own env gate and, if OFF, is a no-op (byte-identical run).
 *    4. ultra_d3d11_install() if ULTRA_D3D11 or ULTRA_SEGA_D3D11 is set.
 *    5. ultra_audio_install() -- always on, diagnostic passthrough.
 *    6. IAT-patch CreateFileA/W, ReadFile/WriteFile, GetProcAddress once
 *       synchronously (the game must not run a frame before these are live),
 *       then start a worker thread that re-patches periodically to catch
 *       modules the game loads AFTER us (delay-loaded imports, plugins).
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include "jvs_core_nuc.h"
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

/* Shared pause/settings overlay -- own self-contained poll thread. File-scope
 * (not inline) since DLL_PROCESS_ATTACH's case body below uses its own nested
 * { } block, which would otherwise scope an inline extern decl away from the
 * DLL_PROCESS_DETACH case's shutdown() call. */
extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));
extern void ultra_pausemenu_shutdown(void);

/* ---------------------------------------------------------------------------
 * Subsystems this DLL links (own independent copies, this board's folder only)
 * ------------------------------------------------------------------------- */
extern void ultra_sega_keychip_install(void);
extern int  ultra_sega_keychip_matches_a(const char* name);
extern int  ultra_sega_keychip_matches_w(const wchar_t* name);
extern HANDLE ultra_sega_keychip_sentinel(void);

extern void ultra_sega_io4_install(void);
extern int  ultra_sega_io4_enabled(void);
extern int  ultra_sega_io4_matches_a(const char* name);
extern int  ultra_sega_io4_matches_w(const wchar_t* name);
extern HANDLE ultra_sega_io4_sentinel(void);
extern int  ultra_sega_io4_fill_in(void* buf, int cap);
extern int  ultra_sega_io4_consume_out(const void* buf, int len);
extern void ultra_sega_io4_insert_coin(int slot, int count);

extern void ultra_d3d11_install(void);
extern void ultra_d3d11_teardown(void);
/* Loose-prototype externs so this file can take their address for the
 * GetProcAddress packed-game dispatch, exactly as the reference pattern does
 * (the real, concrete-typed definitions live in d3d11_throttle_nuc.c). */
extern HRESULT __stdcall ultra_Hook_D3D11CreateDeviceAndSwapChain(
    void*, int, void*, UINT, const void*, UINT, UINT,
    const void*, void**, void**, void*, void**);
extern HRESULT __stdcall ultra_Hook_CreateDXGIFactory(const void*, void**);
extern HRESULT __stdcall ultra_Hook_CreateDXGIFactory1(const void*, void**);
/* Device/render window seam (set from the swap chain's own OutputWindow the
 * moment d3d11_throttle_nuc.c hooks it; NULL until then). Passed to
 * ultra_omni_bridge_install() below as the window-getter accessor. */
extern HWND ultra_sega_nuc_device_window(void);

extern void ultra_audio_install(void);

/* ---------------------------------------------------------------------------
 * Shared trace sink. Every subsystem in this DLL calls this same symbol
 * (extern void ultra_d3d8_log(const char*, ...)) so every log line lands in
 * ONE file. Kept under the same name/env-var contract as the rest of the
 * project's boards purely for operator familiarity (ULTRA_JVS_LOG); this is
 * an independent definition private to this DLL, not a link against any
 * other board's binary.
 * ------------------------------------------------------------------------- */
void ultra_d3d8_log(const char* fmt, ...) {
    const char* p = getenv("ULTRA_JVS_LOG");
    if (!p || !*p) return;
    FILE* f = fopen(p, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}
#define nuclog ultra_d3d8_log

/* ---------------------------------------------------------------------------
 * IAT patcher -- ported (adapted, independent copy) from the generic PE-import
 * walk pattern this project uses project-wide. Pure Win32/PE-header logic, no
 * board-specific behavior. All header/thunk reads are bounds-checked against
 * the module's mapped image size (no SEH __try/__except under mingw), so a
 * malformed import directory is skipped instead of crashing the host game.
 * ------------------------------------------------------------------------- */
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

void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn) {
    void* realfn = NULL;
    HMODULE imp = GetModuleHandleA(impdll);
    if (imp) realfn = (void*)GetProcAddress(imp, impfn);

    HMODULE mods[512]; DWORD need = 0;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ultra_iat_patch_all, &self);
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        DWORD count = need / sizeof(HMODULE);
        if (count > 512) count = 512;
        for (DWORD i = 0; i < count; ++i)
            if (mods[i] != self)
                iat_patch_module(mods[i], impdll, impfn, newfn, realfn);
    }
}

/* ---------------------------------------------------------------------------
 * Live cabinet input. sega_io4_nuc.c's fill_in() calls this to build the IO4
 * IN report -- exactly the seam sega_io4.c's reference version reads from
 * (ultra_hook_current_input). This board's OWN source: keyboard (always
 * available, a standard bench-test mapping) OR'd with an XInput gamepad
 * (dynamically resolved -- no link-time xinput import-lib dependency, and no
 * force-load of a DLL the process never asked for beyond one lazy attempt).
 * ------------------------------------------------------------------------- */
typedef DWORD (WINAPI *pXInputGetState)(DWORD, XINPUT_STATE*);
static pXInputGetState Real_XInputGetState;
static int             g_xinput_tried;

static int xinput_poll(XINPUT_STATE* st) {
    if (!Real_XInputGetState) {
        if (g_xinput_tried) return 0;
        g_xinput_tried = 1;
        static const char* versions[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        for (size_t i = 0; i < sizeof(versions)/sizeof(versions[0]); ++i) {
            HMODULE m = LoadLibraryA(versions[i]);
            if (!m) continue;
            Real_XInputGetState = (pXInputGetState)(void*)GetProcAddress(m, "XInputGetState");
            if (Real_XInputGetState) break;
        }
        if (!Real_XInputGetState) return 0;
    }
    memset(st, 0, sizeof *st);
    return Real_XInputGetState(0, st) == ERROR_SUCCESS;
}

static volatile LONG g_local_coin = 0;

void ultra_hook_current_input(ultra_jvs_input* out) {
    memset(out, 0, sizeof *out);
    for (int a = 0; a < JVS_MAX_ANALOG; ++a) out->analog[a] = 0x8000;
    out->screen_x = out->screen_y = 0x8000;

    /* keyboard: standard bench-test mapping (arrows + ctrl/alt/space/shift +
     * z/x, 1=start, 9=service, tab=test, 5=coin). Harmless when nothing is
     * pressed -- this only ever ADDS bits, never subtracts a real input. */
    int up    = (GetAsyncKeyState(VK_UP)      & 0x8000) != 0;
    int down  = (GetAsyncKeyState(VK_DOWN)    & 0x8000) != 0;
    int left  = (GetAsyncKeyState(VK_LEFT)    & 0x8000) != 0;
    int right = (GetAsyncKeyState(VK_RIGHT)   & 0x8000) != 0;
    int p1    = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    int p2    = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
    int p3    = (GetAsyncKeyState(VK_SPACE)   & 0x8000) != 0;
    int p4    = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
    int p5    = (GetAsyncKeyState('Z')        & 0x8000) != 0;
    int p6    = (GetAsyncKeyState('X')        & 0x8000) != 0;
    int start = (GetAsyncKeyState('1')        & 0x8000) != 0;
    int svc   = (GetAsyncKeyState('9')        & 0x8000) != 0;
    int test  = (GetAsyncKeyState(VK_TAB)     & 0x8000) != 0;
    int coin_edge = (GetAsyncKeyState('5')    & 0x8000) != 0;

    /* XInput gamepad, merged in (OR'd) when present. */
    XINPUT_STATE st;
    if (xinput_poll(&st)) {
        WORD b = st.Gamepad.wButtons;
        up    |= !!(b & XINPUT_GAMEPAD_DPAD_UP);
        down  |= !!(b & XINPUT_GAMEPAD_DPAD_DOWN);
        left  |= !!(b & XINPUT_GAMEPAD_DPAD_LEFT);
        right |= !!(b & XINPUT_GAMEPAD_DPAD_RIGHT);
        p1    |= !!(b & XINPUT_GAMEPAD_A);
        p2    |= !!(b & XINPUT_GAMEPAD_B);
        p3    |= !!(b & XINPUT_GAMEPAD_X);
        p4    |= !!(b & XINPUT_GAMEPAD_Y);
        p5    |= !!(b & XINPUT_GAMEPAD_LEFT_SHOULDER);
        p6    |= !!(b & XINPUT_GAMEPAD_RIGHT_SHOULDER);
        start |= !!(b & XINPUT_GAMEPAD_START);
        svc   |= !!(b & XINPUT_GAMEPAD_BACK);
        coin_edge |= (st.Gamepad.bRightTrigger > 128);
        if (st.Gamepad.sThumbLX < -8000 || st.Gamepad.sThumbLX > 8000
            || st.Gamepad.sThumbLY < -8000 || st.Gamepad.sThumbLY > 8000) {
            out->analog[0] = (uint16_t)((int)st.Gamepad.sThumbLX + 0x8000);
            out->analog[1] = (uint16_t)(0x7FFF - (int)st.Gamepad.sThumbLY);
        }
    }

    uint8_t byte1 = (uint8_t)((start<<7)|(svc<<6)|(up<<5)|(down<<4)|(left<<3)|(right<<2)|(p1<<1)|p2);
    uint8_t byte2 = (uint8_t)((p3<<7)|(p4<<6)|(p5<<5)|(p6<<4));
    out->player[0] = (uint16_t)(((uint16_t)byte1 << 8) | byte2);
    out->system = (uint8_t)(test ? 0x80 : 0);

    static int coin_was = 0;
    if (coin_edge && !coin_was) InterlockedIncrement(&g_local_coin);
    coin_was = coin_edge;
}

/* ---------------------------------------------------------------------------
 * This board's own JVS node -- kept for diagnostics/introspection + a future
 * serial fallback (see jvs_core_nuc.h). Configured from the same env vars the
 * rest of the project uses (ULTRA_JVS_ID / ULTRA_JVS_ANALOG /
 * ULTRA_JVS_SCREENPOS) so operator tooling behaves consistently board-to-board.
 * ------------------------------------------------------------------------- */
static ultra_jvs_node* g_node;
static volatile LONG   g_stop;
static long            g_coin_seen;

static void configure_node(void) {
    g_node = jvs_create();
    if (!g_node) return;
    const char* id = getenv("ULTRA_JVS_ID");
    if (id && *id) jvs_set_id(g_node, id);
    int screenpos = 0;
    const char* sp = getenv("ULTRA_JVS_SCREENPOS");
    if (sp) screenpos = (sp[0] == '1');
    uint8_t analog_ch = 0;
    const char* ach = getenv("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= JVS_MAX_ANALOG) analog_ch = (uint8_t)v; }
    jvs_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* Pull current input into the JVS node + drain the local coin edge into BOTH
 * the JVS node's counter (diagnostics) and the IO4 counters (what the game
 * actually reads, when the IO4 shim is armed). */
static void pull_input(void) {
    ultra_jvs_input in;
    ultra_hook_current_input(&in);
    if (g_node) jvs_set_input(g_node, &in);
    long lc = g_local_coin;
    if (lc != g_coin_seen) {
        int delta = (int)(lc - g_coin_seen);
        if (g_node) jvs_insert_coin(g_node, 0, delta);
        if (ultra_sega_io4_enabled()) ultra_sega_io4_insert_coin(0, delta);
        g_coin_seen = lc;
    }
}

/* ---------------------------------------------------------------------------
 * Device-open / report I/O dispatch. ONE hook per kernel32 function, shared by
 * every subsystem in this DLL (see the architecture note at file top).
 * ------------------------------------------------------------------------- */
typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef BOOL   (WINAPI *pReadFile)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL   (WINAPI *pWriteFile)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef FARPROC(WINAPI *pGetProcAddress)(HMODULE,LPCSTR);

static pCreateFileA     Real_CreateFileA;
static pCreateFileW     Real_CreateFileW;
static pReadFile        Real_ReadFile;
static pWriteFile       Real_WriteFile;
static pGetProcAddress  Real_GetProcAddress;

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    HANDLE real = Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                                   : INVALID_HANDLE_VALUE;
    if (real != INVALID_HANDLE_VALUE) return real;    /* real device present: use it */
    if (ultra_sega_keychip_matches_a(name)) {
        nuclog("[hook_nuc] synthesizing keychip presence (no physical keychip): %s", name);
        return ultra_sega_keychip_sentinel();
    }
    if (ultra_sega_io4_matches_a(name)) {
        nuclog("[hook_nuc] synthesizing IO4-USB presence (no physical 837-15257): %s", name);
        return ultra_sega_io4_sentinel();
    }
    return real;   /* INVALID_HANDLE_VALUE: not ours, not real -- pass the failure through */
}
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    HANDLE real = Real_CreateFileW ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
                                   : INVALID_HANDLE_VALUE;
    if (real != INVALID_HANDLE_VALUE) return real;
    if (ultra_sega_keychip_matches_w(name)) {
        nuclog("[hook_nuc] synthesizing keychip presence (W, no physical keychip)");
        return ultra_sega_keychip_sentinel();
    }
    if (ultra_sega_io4_matches_w(name)) {
        nuclog("[hook_nuc] synthesizing IO4-USB presence (W, no physical 837-15257)");
        return ultra_sega_io4_sentinel();
    }
    return real;
}

static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    if (h == ultra_sega_io4_sentinel()) {
        int w = ultra_sega_io4_fill_in(buf, (int)n);
        if (got) *got = (DWORD)(w > 0 ? w : 0);
        return w > 0 ? TRUE : FALSE;
    }
    if (h == ultra_sega_keychip_sentinel()) {
        /* presence-only: this board never implements the secret challenge/
         * response framing (see sega_keychip_nuc.c). A title that reads raw
         * frames off this handle gets zeros -- never a fabricated secret. */
        if (buf && n) memset(buf, 0, n);
        if (got) *got = n;
        return TRUE;
    }
    return Real_ReadFile ? Real_ReadFile(h, buf, n, got, ov) : FALSE;
}
static BOOL WINAPI Hook_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote, LPOVERLAPPED ov) {
    if (h == ultra_sega_io4_sentinel()) {
        ultra_sega_io4_consume_out(buf, (int)n);
        if (wrote) *wrote = n;
        return TRUE;
    }
    if (h == ultra_sega_keychip_sentinel()) {
        if (wrote) *wrote = n;   /* accept + ignore; nothing secret to answer */
        return TRUE;
    }
    return Real_WriteFile ? Real_WriteFile(h, buf, n, wrote, ov) : FALSE;
}

/* GetProcAddress interception: PACKED Sega Nu titles can resolve
 * D3D11CreateDeviceAndSwapChain / CreateDXGIFactory[1] dynamically at runtime
 * (LoadLibrary("d3d11.dll")+GetProcAddress(...)) instead of a static IAT
 * import, which the IAT patch alone can't catch. Hand back our wrapper for
 * the names we own; everything else forwards untouched. */
static FARPROC WINAPI Hook_GetProcAddress(HMODULE mod, LPCSTR name) {
    if (((uintptr_t)name >> 16) != 0) {   /* by name, not ordinal */
        if (strcmp(name, "D3D11CreateDeviceAndSwapChain") == 0) {
            nuclog("[hook_nuc] GetProcAddress(D3D11CreateDeviceAndSwapChain) intercepted (packed path)");
            return (FARPROC)ultra_Hook_D3D11CreateDeviceAndSwapChain;
        }
        if (strcmp(name, "CreateDXGIFactory") == 0)  return (FARPROC)ultra_Hook_CreateDXGIFactory;
        if (strcmp(name, "CreateDXGIFactory1") == 0) return (FARPROC)ultra_Hook_CreateDXGIFactory1;
    }
    return Real_GetProcAddress ? Real_GetProcAddress(mod, name) : NULL;
}

/* Re-patch worker: catches modules the game loads AFTER the synchronous
 * DLL_PROCESS_ATTACH install (delay-loaded imports, render/audio plugins). */
static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 600 && !g_stop; ++i) {
        Sleep(1000);
        pull_input();
        if ((i % 5) == 0) {
            ultra_iat_patch_all("kernel32.dll", "CreateFileA",     (void*)Hook_CreateFileA);
            ultra_iat_patch_all("kernel32.dll", "CreateFileW",     (void*)Hook_CreateFileW);
            ultra_iat_patch_all("kernel32.dll", "ReadFile",        (void*)Hook_ReadFile);
            ultra_iat_patch_all("kernel32.dll", "WriteFile",       (void*)Hook_WriteFile);
            ultra_iat_patch_all("kernel32.dll", "GetProcAddress",  (void*)Hook_GetProcAddress);
        }
    }
    return 0;
}

/* ---- crash diagnostics (VEH) -------------------------------------------- */
static LONG WINAPI ultra_crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "(unknown)";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
#ifdef _WIN64
        nuclog("CRASH: ACCESS_VIOLATION at RIP=%p in %s", addr, modname);
#else
        nuclog("CRASH: ACCESS_VIOLATION at EIP=%p in %s", addr, modname);
#endif
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* -------------------------------------------------------------------------- */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH: {

    {   /* Universal transport layer: declare this board's shape so the shared
         * pacing/range/codec core is live and self-describing in the log.
         * Serial boards are REQUEST/RESPONSE -- their reply queue already ends
         * the game's read loop, so pacing is deliberately inert here; injecting
         * empty reads would truncate a reply frame. Behavior is unchanged. */
        static int s_utx_once;
        if (!s_utx_once) { static utx_profile utxp;
            s_utx_once = 1;
            memset(&utxp, 0, sizeof utxp);
            utxp.board        = "sega_nuc";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        AddVectoredExceptionHandler(1, ultra_crash_handler);

        HMODULE k = GetModuleHandleA("kernel32.dll");
        if (k) {
            Real_CreateFileA    = (pCreateFileA)(void*)GetProcAddress(k, "CreateFileA");
            Real_CreateFileW    = (pCreateFileW)(void*)GetProcAddress(k, "CreateFileW");
            Real_ReadFile       = (pReadFile)(void*)GetProcAddress(k, "ReadFile");
            Real_WriteFile      = (pWriteFile)(void*)GetProcAddress(k, "WriteFile");
            Real_GetProcAddress = (pGetProcAddress)(void*)GetProcAddress(k, "GetProcAddress");
        }

        configure_node();

        /* Install SYNCHRONOUSLY: the module injects us into the SUSPENDED
         * game and resumes right after, so the hooks must be live before the
         * game's first thread runs. */
        ultra_sega_keychip_install();                    /* internally env-gated (OFF by default) */
        ultra_sega_io4_install();                         /* internally env-gated (OFF by default) */
        { const char* d  = getenv("ULTRA_D3D11");
          const char* sd = getenv("ULTRA_SEGA_D3D11");
          if ((d && d[0]=='1') || (sd && sd[0]=='1')) ultra_d3d11_install(); }
        ultra_audio_install();                            /* DirectSound + XAudio2 diagnostic hook (always-on, passthrough) */

        /* Ultra Omni-Bridge (5th Omni service): SendInput keyboard/mouse
         * emulation from an XInput gamepad, for a Sega Nu-family title that
         * reads raw OS keyboard/mouse instead of JVS/DirectInput. Installed
         * AFTER the D3D11 present hook above so its window-getter
         * (ultra_sega_nuc_device_window) has a live swap chain to read from
         * once one shows up; install() itself is a no-op until the game
         * actually creates a swap chain. This board has no lightgun/shared-
         * pose source of its own, so get_pose is NULL -- gamepad-only.
         * omni_client_read_bridge() defaults ULTRA_BRIDGE_ENABLE to OFF, same
         * as the other four Omni services, so this is opt-in per game. */
        ultra_omni_bridge_install(ultra_sega_nuc_device_window, NULL);
ultra_pausemenu_install(ultra_sega_nuc_device_window);

        ultra_iat_patch_all("kernel32.dll", "CreateFileA",    (void*)Hook_CreateFileA);
        ultra_iat_patch_all("kernel32.dll", "CreateFileW",    (void*)Hook_CreateFileW);
        ultra_iat_patch_all("kernel32.dll", "ReadFile",       (void*)Hook_ReadFile);
        ultra_iat_patch_all("kernel32.dll", "WriteFile",      (void*)Hook_WriteFile);
        ultra_iat_patch_all("kernel32.dll", "GetProcAddress", (void*)Hook_GetProcAddress);

        CreateThread(NULL, 0, worker, NULL, 0, NULL);
        nuclog("[hook_nuc] sega_nuc hook installed (pid=%lu)", GetCurrentProcessId());
        break;
    }
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();
ultra_pausemenu_shutdown();
        ultra_d3d11_teardown();
        if (g_node) { jvs_destroy(g_node); g_node = NULL; }
        break;
    default: break;
    }
    return TRUE;
}

#endif /* _WIN32 */
