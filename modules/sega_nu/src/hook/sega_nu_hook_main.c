/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  sega_nu_hook_main.c -- DllMain + shared helpers for the Sega Nu hook DLL
 *                         (ultra_sega_nu_hook{32,64}.dll).
 * =============================================================================
 *
 *  This TU owns:
 *    - DllMain (installs all 8 stubs + Omni-Bridge on DLL_PROCESS_ATTACH,
 *               stops Omni-Bridge on DLL_PROCESS_DETACH),
 *    - ul_sn_log       -- stderr diagnostic sink,
 *    - ul_sn_iat_patch_all -- IAT walker (RVA-bounded, copied from the
 *                             Raw Thrills hook's proven walker),
 *    - ul_sn_env / ul_sn_env_on -- env helpers so every stub is env-gated.
 *
 *  The first 5 subsystems installed here are the Sega Nu / ALL.Net / DRM
 *  abstractions identified by static analysis of amauthd.exe / amdaemon.exe /
 *  the Nu game binaries:
 *    1. keychip_stub      -- USB smart-card (A7/N2) + mxparallel + mxkeychip
 *    2. amauth_stub       -- amauthd named-pipe / TCP presence + BootID
 *    3. allnet_stub       -- allnetd loopback + naomitest.hostname DNS
 *    4. truecrypt_bypass  -- Y:\ / X:\ container-drive redirect + no-op mount
 *    5. amdaemon_stub     -- amdaemon local IPC presence stub
 *
 *  3 more subsystems cover render/audio/input so a Nu title is a complete
 *  render+sound+controls experience, not just a DRM-boot pass-through:
 *    6. d3d11_throttle_nu -- DXGI/D3D11 present hook (frame cap, force-
 *                            windowed, optional back-buffer BMP dump)
 *    7. audio_hook_nu     -- DirectSound/XAudio2 diagnostic + SetCooperative-
 *                            Level fallback (always-on passthrough)
 *    8. sega_io4_nu       -- IO4-USB (837-15257) cabinet I/O emulation,
 *                            keyboard + XInput backed
 *
 *  This DLL is injected into the game process by sega_nu_main.c via a
 *  suspended-launch + QueueUserAPC(LoadLibraryA) (primary) + a fallback
 *  CreateRemoteThread(LoadLibraryA). Each install() self-checks its ULTRA_*
 *  env flag; the ones whose flag isn't set for this title no-op.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "sega_nu_hook.h"
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

/* Shared pause/settings overlay (pause_menu_wire_nu.c -> work/src/
 * pause_menu_service.c). Self-contained poll thread, see that file's header. */
void ultra_pausemenu_install(HWND (*get_hwnd)(void));
void ultra_pausemenu_shutdown(void);

/* ---------- log helper ----------------------------------------------------- */
void ul_sn_log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[sn-hook] %s", buf);
    fflush(stderr);
}

/* ---------- env helpers ---------------------------------------------------- */
const char* ul_sn_env(const char* name) {
    static char buf[512];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
    if (n == 0 || n >= sizeof buf) return NULL;
    return buf;
}
int ul_sn_env_on(const char* name) {
    const char* v = ul_sn_env(name);
    return v && v[0] == '1' && v[1] == 0;
}

/* ---------- IAT walker (RVA-bounded, same shape as the Raw Thrills hook) --- */
static int rva_ok(DWORD rva, DWORD isize) { return rva && rva < isize; }

static void patch_module(HMODULE mod, const char* impdll, const char* impfn,
                         void* newfn, void* realfn) {
    if (!mod) return;
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (!ul_pe_readable(dos, sizeof *dos)) return;   /* module unloaded under us */
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (!ul_pe_readable(nt, sizeof *nt)) return;
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    DWORD isize = nt->OptionalHeader.SizeOfImage;
    IMAGE_DATA_DIRECTORY* dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!rva_ok(dir->VirtualAddress, isize) || dir->Size == 0) return;
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
                    iat->u1.Function = (ULONG_PTR)(uintptr_t)newfn;
                    VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
                }
            }
        }
    }
}

void ul_sn_iat_patch_all(const char* impdll, const char* impfn, void* newfn) {
    void* realfn = NULL;
    HMODULE imp = GetModuleHandleA(impdll);
    if (imp) realfn = (void*)GetProcAddress(imp, impfn);

    HMODULE mods[512]; DWORD need = 0;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ul_sn_iat_patch_all, &self);
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        DWORD count = need / sizeof(HMODULE);
        if (count > 512) count = 512;
        for (DWORD i = 0; i < count; ++i)
            if (mods[i] != self)
                patch_module(mods[i], impdll, impfn, newfn, realfn);
    }
}

/* ---------- DllMain -------------------------------------------------------- */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)hinst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        ul_sn_log("attach: ultra_sega_nu_hook loaded pid=%lu (%d-bit)\n",
                  GetCurrentProcessId(), (int)(sizeof(void*) * 8));
        /* Fire all 8 installers. Order:
         *   - truecrypt-bypass FIRST so the container-drive redirect is armed
         *     before the game's early asset opens (chdir Y:\ ...) run;
         *   - keychip next -- amauthd opens the keychip transport before it
         *     starts serving the ALL.Net auth handshake;
         *   - amauthd + allnet + amdaemon next (network/IPC surfaces the
         *     game hits after DRM boot);
         *   - d3d11 present + audio + IO4-USB last: these IAT-patch the
         *     render/sound/device-open surfaces the game reaches once it's
         *     past the DRM boot sequence above, so they can install any time
         *     before the game's own init runs (order among themselves does
         *     not matter -- they touch disjoint import names). */
        ul_sn_install_truecrypt_bypass();
        ul_sn_install_keychip();
        ul_sn_install_amauth();
        ul_sn_install_allnet();
        ul_sn_install_amdaemon();
        ul_sn_install_d3d11();
        ul_sn_install_audio();
        ul_sn_install_io4();
        ul_sn_log("attach: all 8 stub installers fired\n");

        /* Ultra Omni-Bridge (KBM emulation for raw-input-only game dumps).
         * Opt-in (ULTRA_BRIDGE_ENABLE, default OFF -- see
         * omni_client_read_bridge()), so this is a no-op unless a user/
         * future session turns it on for a confirmed raw-keyboard/mouse
         * Sega Nu title. Installed AFTER ul_sn_install_d3d11() above so the
         * DXGI present hook the window getter depends on is already armed.
         * No board-specific live lightgun-pose reader exists for this board
         * (Nu's only I/O class is IO4-USB, read directly from keyboard +
         * XInput in sega_io4_nu.c -- there is no shared-memory pose block to
         * adapt), so get_pose is NULL: gamepad-only. */
        ultra_omni_bridge_install(ul_sn_get_render_window, NULL);
        /* Shared pause/settings overlay -- Start+Select / LB+RB (pad) or
         * F1/Esc/P (keyboard) to open. Own self-contained poll thread. */
        ultra_pausemenu_install(ul_sn_get_render_window);
    } else if (reason == DLL_PROCESS_DETACH) {
        ultra_omni_bridge_stop();
        ultra_pausemenu_shutdown();
    }
    return TRUE;
}

#endif /* _WIN32 */
