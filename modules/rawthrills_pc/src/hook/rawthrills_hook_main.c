/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  rawthrills_hook_main.c -- DllMain + shared helpers for the Raw Thrills
 *                            PC hook (ultra_rawthrills_hook32.dll).
 * =============================================================================
 *
 *  This TU owns:
 *    - DllMain (installs all 9 stubs on DLL_PROCESS_ATTACH, plus the Omni-
 *      Bridge framework install/stop pair described below),
 *    - ul_rt_log (stderr diagnostic sink for every stub),
 *    - ul_rt_iat_patch_all (IAT walker that rewrites named imports across every
 *      loaded module -- copied from the Type X hook's proven walker),
 *    - ul_rt_env / ul_rt_env_on (env helpers so every stub is env-gated).
 *
 *  The 9 subsystems installed here are the Raw Thrills PC arcade abstractions
 *  identified by static analysis of sdaemon.exe (the TFAFSB import list), plus
 *  the present + audio hooks added 2026-08-01:
 *    1. JAMMAUSB (HID device -- Raw Thrills' USB-JAMMA I/O board)
 *    2. USBWD    (HID device -- Raw Thrills' USB watchdog board)
 *    3. PortTalk (kernel driver \\.\PortTalk + parallel-port outb/inb thunks)
 *    4. EWFAPI   (Enhanced Write Filter -- 21 named exports, 4 used)
 *    5. RawArt   (hardcoded c:\rawart asset root)
 *    6. D3D8     (present-hook -- RawApp/TFAFSB-class titles, ULTRA_D3D8=1)
 *    7. D3D9     (present-hook -- newer Raw Thrills cabs, ULTRA_D3D11=<any>)
 *    8. D3D11    (present-hook -- newer Raw Thrills cabs, ULTRA_D3D11=<any>)
 *    9. Audio    (DirectSound + XAudio2 diagnostic hook, ULTRA_RT_AUDIO=1)
 *
 *  Plus, added 2026-08-02: Ultra Omni-Bridge (omni_bridge.c/h, this board's own
 *  copy of the reference implementation at
 *  modules/native_x86/src/hook/omni_bridge.c -- see that file's header for the
 *  full design rationale). It synthesizes SendInput cursor/click/key traffic
 *  from an XInput gamepad for the raw-keyboard/mouse-reading titles this
 *  family may include (mirrors what proved necessary on Taito Type X2's
 *  Haunted Museum). Installed unconditionally like the 9 stubs above, but
 *  stays fully inert unless ULTRA_BRIDGE_ENABLE=1 is set (default OFF, same
 *  opt-in convention as the other four Omni services -- rawthrills_pc_main.c
 *  does NOT set this flag; a user/future session enables it per-game once
 *  verified). This board has no board-specific lightgun-pose reader of its
 *  own (see d3d9_throttle_rawthrills.c's own header note: the universal
 *  lightgun bridge for the Big Buck Hunter family is a separate, not-yet-
 *  wired integration point), so get_pose is NULL here -- the bridge drives
 *  the cursor from a gamepad stick only, never a lightgun pose, so it cannot
 *  race any future lightgun integration for this board.
 *
 *  See rawthrills_hook.h for the shared ABI.
 *  See each .c for the exact hook surface + fallback semantics.
 *
 *  This DLL is loaded into the game process by rawthrills_pc_main.c via a
 *  suspended-launch + CreateRemoteThread(LoadLibraryA) inject. On attach we
 *  fire every install() (each self-checks the ULTRA_* env flag and no-ops if
 *  its subsystem isn't wanted for this title). No global constructors; every
 *  install() is idempotent so a repeat-attach is safe.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "rawthrills_hook.h"
#include "omni_bridge.h"

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

/* Shared pause/settings overlay (pause_menu_wire_rawthrills.c -> work/src/
 * pause_menu_service.c). Self-contained poll thread, see that file's header. */
void ultra_pausemenu_install(HWND (*get_hwnd)(void));
void ultra_pausemenu_shutdown(void);

/* ---------- log helper -----------------------------------------------------
 * Writes to BOTH stderr (inherited by the loader's ultra.log) AND, when
 * ULTRA_JVS_LOG is set, appends to that file. Every I/O stub in this hook tree
 * (jammausb / porttalk / usbwd / ewf / d3d8 / d3d9 / d3d11 / audio) logs through
 * here, so teeing into ULTRA_JVS_LOG is what finally makes the live I/O init
 * visible in the operator's designated per-title diagnostic file (the golden
 * layer pins ULTRA_JVS_LOG = log\<title>.log for exactly this purpose). Before
 * this, only omni_bridge.c's blog() reached that file, so a run showed the
 * overlay but nothing about JammaUSB / PortTalk / D3D -- the run looked empty of
 * I/O even though the stubs were talking. Purely additive: stderr output is
 * unchanged; the file append is skipped entirely when ULTRA_JVS_LOG is unset. */
void ul_rt_log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[rt-hook] %s", buf);
    fflush(stderr);

    /* Tee into the per-title diagnostic file if the container pinned one. */
    {
        char path[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("ULTRA_JVS_LOG", path, sizeof path);
        if (n > 0 && n < sizeof path) {
            FILE* f = fopen(path, "a");
            if (f) { fputs("[rt-hook] ", f); fputs(buf, f); fclose(f); }
        }
    }
}

/* ---------- env helpers ---------------------------------------------------- */
const char* ul_rt_env(const char* name) {
    static char buf[512];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
    if (n == 0 || n >= sizeof buf) return NULL;
    return buf;
}
int ul_rt_env_on(const char* name) {
    const char* v = ul_rt_env(name);
    return v && v[0] == '1' && v[1] == 0;
}

/* ---------- IAT walker (RVA-bounded, hardened -- same shape as the Type X
 *            hook's ultra_iat_patch_all in jvs_serial_hook.c) ----------------- */
static int rva_ok(DWORD rva, DWORD isize) { return rva && rva < isize; }

/* Returns the IAT entry value that was REPLACED (the previous target of that
 * import in this module), or NULL if nothing was patched. Used to chain hooks:
 * a second stub that patches the same import gets the first stub's hook as its
 * "real", so both run in order regardless of install order. Without this, two
 * stubs that both patch CreateFileA by name would have the last-installed one
 * shadow the earlier one (its real = raw kernel32), silently bypassing it --
 * that is the bug that made the JammaUSB synthetic HID open never reach the
 * jammausb sentinel because rawart installed after it. */
static void* patch_module(HMODULE mod, const char* impdll, const char* impfn,
                          void* newfn, void* realfn) {
    void* replaced = NULL;
    if (!mod) return NULL;
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (!ul_pe_readable(dos, sizeof *dos)) return NULL;   /* module unloaded under us */
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (!ul_pe_readable(nt, sizeof *nt)) return NULL;
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    DWORD isize = nt->OptionalHeader.SizeOfImage;
    IMAGE_DATA_DIRECTORY* dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!rva_ok(dir->VirtualAddress, isize) || dir->Size == 0) return NULL;
    IMAGE_IMPORT_DESCRIPTOR* desc =
        (IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress);
    for (; (size_t)((BYTE*)desc - base) + sizeof(*desc) <= (size_t)isize
           && desc->Name; ++desc) {
        if (!rva_ok(desc->Name, isize)) continue;
        const char* dllname = (const char*)(base + desc->Name);
        if (_stricmp(dllname, impdll) != 0) continue;
        DWORD orig_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                  : desc->FirstThunk;
        IMAGE_THUNK_DATA* orig = rva_ok(orig_rva, isize)
                                 ? (IMAGE_THUNK_DATA*)(base + orig_rva) : NULL;
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);
        for (; (size_t)((BYTE*)iat - base) + sizeof(*iat) <= (size_t)isize
               && (!orig || (size_t)((BYTE*)orig - base) + sizeof(*orig) <= (size_t)isize)
               && iat->u1.Function; ++iat, orig = orig ? orig + 1 : NULL) {
            int match = 0;
            if (orig && !(orig->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                && rva_ok((DWORD)orig->u1.AddressOfData, isize)) {
                IMAGE_IMPORT_BY_NAME* ibn =
                    (IMAGE_IMPORT_BY_NAME*)(base + orig->u1.AddressOfData);
                if (strcmp((const char*)ibn->Name, impfn) == 0) match = 1;
            }
            if (!match && realfn && (void*)(uintptr_t)iat->u1.Function == realfn)
                match = 1;
            if (match) {
                DWORD old;
                if (VirtualProtect(&iat->u1.Function, sizeof(void*),
                                   PAGE_READWRITE, &old)) {
                    void* prev = (void*)(uintptr_t)iat->u1.Function;
                    iat->u1.Function = (ULONG_PTR)(uintptr_t)newfn;
                    VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
                    if (prev && prev != newfn) replaced = prev;
                }
            }
        }
    }
    return replaced;
}

/* Returns the previous IAT target (for chaining) -- see patch_module. */
void* ul_rt_iat_patch_all(const char* impdll, const char* impfn, void* newfn) {
    void* realfn = NULL;
    void* prev_chain = NULL;
    HMODULE imp = GetModuleHandleA(impdll);
    if (imp) realfn = (void*)GetProcAddress(imp, impfn);

    HMODULE mods[512]; DWORD need = 0;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ul_rt_iat_patch_all, &self);
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        DWORD count = need / sizeof(HMODULE);
        if (count > 512) count = 512;
        for (DWORD i = 0; i < count; ++i)
            if (mods[i] != self) {
                void* p = patch_module(mods[i], impdll, impfn, newfn, realfn);
                /* Prefer a chain target that is another hook (not raw kernel32),
                 * so a later stub calls the earlier stub's hook. */
                if (p && (!prev_chain || p != realfn)) prev_chain = p;
            }
    }
    return prev_chain ? prev_chain : realfn;
}

/* ---------- Omni-Bridge window-getter adapter ------------------------------
 * omni_bridge.c needs a single "current render window" accessor but this
 * board's window state is split across three independent present-hooks (only
 * one of which is ever live for a given title -- see rawthrills_hook.h).
 * Falls back D3D9 -> D3D11 -> D3D8 (arbitrary but stable order among the
 * "only one is ever non-NULL" set); returns NULL (bridge stays fully inert)
 * until whichever hook actually fires for this title creates its device. */
static HWND rt_omni_bridge_get_window(void) {
    HWND w = ul_rt_d3d9_window();
    if (w) return w;
    w = ul_rt_d3d11_window();
    if (w) return w;
    return ul_rt_d3d8_window();
}

/* Fast & Furious Super Bikes (sdaemon.exe): the device-object factory at
 * 0x4E6C20 returns NULL in our environment (its inner config lookup at
 * 0x4BFF90 fails), so the caller stores NULL into the singleton global at
 * 0x57EC40 and the whole field-init sequence (0x44B54C, 0x44B563, ...) faults
 * on a NULL base.  Everything the game actually needs (JammaUSB / PortTalk /
 * etc.) is already emulated, so make the factory hand back a fresh, zeroed
 * buffer and let init complete.  The factory is __cdecl (returns 0xC3, the
 * caller cleans its own args) and is reached from five call sites, so we patch
 * the factory ENTRY (not a call site) to a tiny code cave that VirtualAllocs a
 * fresh zeroed block per call -- distinct objects, stack balanced for every
 * caller.  Gated by the exact factory prologue AND the 0x44B542 call bytes so
 * no other Raw Thrills title is ever touched (this exe has no ASLR, base
 * 0x400000, VAs are static). */
static void ul_rt_superbikes_patch(void) {
    unsigned char *fac  = (unsigned char *)0x004E6C20;
    unsigned char *site = (unsigned char *)0x0044B542;
    static const unsigned char fac_sig[16] =
        { 0x81,0xEC,0x00,0x01,0x00,0x00,0x53,0x8B,
          0x9C,0x24,0x0C,0x01,0x00,0x00,0x56,0x57 };
    static const unsigned char site_sig[5] = { 0xE8,0xD9,0xB6,0x09,0x00 };
    FARPROC pVA;
    unsigned char *cave;
    DWORD old;
    int k = 0;

    if (IsBadReadPtr(fac, 16) || memcmp(fac, fac_sig, 16) != 0) return;
    if (IsBadReadPtr(site, 5) || memcmp(site, site_sig, 5) != 0) return;

    pVA = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "VirtualAlloc");
    if (!pVA) return;

    cave = (unsigned char *)VirtualAlloc(NULL, 0x1000,
               MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) return;

    /* VirtualAlloc(NULL, 0x40000, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE):
     * stdcall args pushed right-to-left; VirtualAlloc cleans its own 4 args;
     * the zeroed block is returned in EAX; C3 returns to the factory's caller
     * (which then cleans the factory's args, __cdecl). */
    cave[k++] = 0x6A; cave[k++] = 0x04;                                    /* push 4       */
    cave[k++] = 0x68; *(DWORD *)(cave + k) = 0x00003000; k += 4;           /* push 0x3000  */
    cave[k++] = 0x68; *(DWORD *)(cave + k) = 0x00040000; k += 4;           /* push 0x40000 */
    cave[k++] = 0x6A; cave[k++] = 0x00;                                    /* push 0       */
    cave[k++] = 0xB8; *(DWORD *)(cave + k) = (DWORD)(DWORD_PTR)pVA; k += 4; /* mov eax,VA  */
    cave[k++] = 0xFF; cave[k++] = 0xD0;                                    /* call eax     */
    cave[k++] = 0xC3;                                                      /* ret          */
    FlushInstructionCache(GetCurrentProcess(), cave, k);

    if (VirtualProtect(fac, 5, PAGE_EXECUTE_READWRITE, &old)) {
        fac[0] = 0xE9;                                                     /* jmp rel32    */
        *(DWORD *)(fac + 1) = (DWORD)((DWORD_PTR)cave - (DWORD_PTR)fac - 5);
        VirtualProtect(fac, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), fac, 5);
        ul_rt_log("superbikes: factory 0x4E6C20 -> fresh-zeroed-object cave %p\n",
                  (void *)cave);
    }
}

/* ---------- DllMain -------------------------------------------------------- */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)hinst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        ul_rt_log("attach: ultra_rawthrills_hook loaded pid=%lu\n",
                  GetCurrentProcessId());
        /* Fire all 9 installers. Each is env-gated and idempotent -- the ones
         * whose ULTRA_* flag isn't set for this title become no-ops. Order:
         * porttalk + ewf go BEFORE jammausb/usbwd because the game may call
         * ewfMgr* / open \\.\PortTalk during static-init before ever touching
         * HID; rawart next because it hooks SetCurrentDirectory* which the
         * game calls in main(); present + audio hooks last -- they only IAT-
         * patch d3d8/d3d9/d3d11/dxgi/dsound/xaudio2, none of which are touched
         * before the game's own main() runs, so ordering relative to the I/O
         * stubs above does not matter. */
        ul_rt_porttalk_install();
        ul_rt_ewf_install();
        ul_rt_jammausb_install();
        ul_rt_usbwd_install();
        ul_rt_rawart_install();
        ul_rt_d3d8_install();
        ul_rt_d3d9_install();
        ul_rt_d3d11_install();
        ul_rt_audio_install();
        ul_rt_log("attach: all 9 stub installers fired\n");
        /* Super Bikes factory cave: DEFAULT OFF as of 2026-09-30. Static RE +
         * the live errorlog.txt proved the cave is now the DIRECT cause of the
         * crash: it makes the 0x4E6C20 factory return a VirtualAlloc'd ZEROED
         * block, but the real factory stores its config object into [obj+0x30]
         * (0x4E6C81), and the video-mode-ratio routine 0x4E6D20 does
         * `mov edi,[edx+0x30]; fild [edi+0x30]` (0x4E6D82/0x4E6D8C) -- with our
         * zero at +0x30, edi=0 -> read of 0x00000030 -> AV at 0x4E6D8C (exactly
         * what errorlog.txt records every run). The real factory returns a valid
         * object on its own (prior dump: page-aligned, +0x30 populated), so
         * letting it run fixes the crash. Opt back IN only for diagnosis with
         * ULTRA_RT_SB_FACTORY_CAVE=1. */
        if (ul_rt_env_on("ULTRA_RT_SB_FACTORY_CAVE"))
            ul_rt_superbikes_patch();
        /* Omni-Bridge last: its window-getter adapter above reads whichever
         * present-hook's static HWND, so it must install after those three
         * are armed (the getter itself just returns NULL until a device is
         * actually created, so ordering only matters for readability here --
         * matches the task's own "after the D3D present hook installs" note).
         * get_pose is NULL: this board has no lightgun-pose reader of its own
         * (see the file header). omni_client_read_bridge() inside gates the
         * whole thing on ULTRA_BRIDGE_ENABLE, which nothing in this board's
         * spawner sets -- opt-in only. */
        ultra_omni_bridge_install(rt_omni_bridge_get_window, NULL);
        /* Shared pause/settings overlay -- Start+Select / LB+RB (pad) or
         * F1/Esc/P (keyboard) to open. Own self-contained poll thread. */
        ultra_pausemenu_install(rt_omni_bridge_get_window);
    } else if (reason == DLL_PROCESS_DETACH) {
        ultra_omni_bridge_stop();
        ultra_pausemenu_shutdown();
    }
    return TRUE;
}

#endif /* _WIN32 */
