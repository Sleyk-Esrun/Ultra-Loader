/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  codepage_spoof_standalone.c -- ultra_typex_codepage_spoof.dll
 * =============================================================================
 *  A SEPARATE, self-contained DLL from this board's main hook -- deliberately.
 *
 *  This board's shipped hook (ultra_taito_typex_hook32.dll, embedded by
 *  build_module.sh) is a frozen prebuilt binary, pinned because Chaos Breaker
 *  -- a real, verified-playable game on this exact board -- was found to
 *  black-screen when a from-source rebuild of that hook was substituted (see
 *  build_module.sh's header comment). Folding a new hook into that shared
 *  binary would re-risk the one game already proven on this board, for every
 *  title on it, not just the one that needs the new fix.
 *
 *  So this fix ships as its own independently injected DLL instead: the
 *  spawner (taito_typex_main.c) loads it ADDITIONALLY, alongside the frozen
 *  hook, and ONLY for a title whose provisional-fix entry asks for it. Chaos
 *  Breaker's injection is byte-for-byte unchanged.
 *
 *  What it does: makes GetACP()/GetOEMCP() report Shift-JIS (932) inside the
 *  target process. See codepage_spoof.c (this board's src/ copy meant for a
 *  future from-source hook rebuild) for the full rationale -- this file is
 *  that same fix, plus its own copy of the IAT patcher so it needs nothing
 *  from the other hook's translation units. The patcher body (iat_patch_module
 *  / ul_pe_readable / ultra_iat_patch_all) is copied verbatim from
 *  jvs_serial_hook.c -- this is proven, DEP-crash-hardened code (see the
 *  "ALREADY OURS" comment below), not a rewrite.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <stdlib.h>
#include <string.h>

/* ---- IAT patcher: verbatim copy of jvs_serial_hook.c's, see that file for
 * the history behind each guard here (module-unloaded-under-us, truncated
 * import table, the DEP-crash no-op-write fix). --------------------------- */
static const unsigned char* g_ulpe_lo;
static const unsigned char* g_ulpe_hi;

static int ul_pe_readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    const unsigned char* a = (const unsigned char*)p;
    const unsigned char* end;
    if (!a) return 0;
    end = a + n;
    if (a >= g_ulpe_lo && end <= g_ulpe_hi) return 1;
    while (a < end) {
        if (!VirtualQuery(a, &mbi, sizeof mbi))   return 0;
        if (mbi.State != MEM_COMMIT)              return 0;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        if (mbi.Protect & PAGE_EXECUTE)           return 0;
        g_ulpe_lo = (const unsigned char*)mbi.BaseAddress;
        g_ulpe_hi = g_ulpe_lo + mbi.RegionSize;
        a = g_ulpe_hi;
    }
    return 1;
}

static int rva_ok(DWORD rva, DWORD isize) { return rva != 0 && rva < isize; }

/* CONFIRMED via Windows' own crash report this session (codepage_spoof32.dll
 * faulting): this copy of the IAT patcher was missing a fix jvs_serial_hook.c
 * already has -- a GPU vendor driver DLL (e.g. aticfx32.dll, AMD's D3D
 * shader compiler) can unload while we're mid-scan of its import table,
 * and writing to a freed page crashes. Same fix, ported verbatim. */
static int is_gpu_driver_module(HMODULE mod) {
    char path[MAX_PATH];
    const char *base;
    static const char *const prefixes[] = {
        "aticfx", "atidxx", "atiumd", "atig6pxx",           /* AMD/ATI */
        "nvd3dum", "nvwgf2um", "nvoglv", "nvcuda",          /* NVIDIA */
        "igdumd", "igd10umd", "igd11umd", "igd12umd", "igdgfx", /* Intel */
        NULL
    };
    if (!GetModuleFileNameA(mod, path, sizeof path)) return 0;
    base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    for (int i = 0; prefixes[i]; ++i)
        if (_strnicmp(base, prefixes[i], strlen(prefixes[i])) == 0) return 1;
    return 0;
}

static void iat_patch_module(HMODULE mod, const char* impdll,
                             const char* impfn, void* newfn, void* realfn) {
    if (is_gpu_driver_module(mod)) return;
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (!ul_pe_readable(dos, sizeof *dos)) return;
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
                if ((void*)(uintptr_t)iat->u1.Function == newfn) continue;
                if (VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                    iat->u1.Function = (ULONG_PTR)(uintptr_t)newfn;
                    VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
                }
            }
        }
    }
}

static void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn) {
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

/* ---- the actual fix ------------------------------------------------------ */
#define ULTRA_JP_CODEPAGE 932u
static int g_enabled = 0;

static UINT WINAPI Hook_GetACP(void)   { return g_enabled ? ULTRA_JP_CODEPAGE : GetACP(); }
static UINT WINAPI Hook_GetOEMCP(void) { return g_enabled ? ULTRA_JP_CODEPAGE : GetOEMCP(); }

static void install(void) {
    const char* e = getenv("ULTRA_FORCE_JP_CODEPAGE");
    g_enabled = e && e[0] == '1';
    if (!g_enabled) return;
    ultra_iat_patch_all("kernel32.dll", "GetACP",   (void*)Hook_GetACP);
    ultra_iat_patch_all("kernel32.dll", "GetOEMCP", (void*)Hook_GetOEMCP);
}

/* Re-patch briefly for modules the game loads after us (mirrors the pattern
 * this board's main hook uses for the same reason -- delay-loaded imports). */
static volatile LONG g_stop;
static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 15 && !g_stop; ++i) { Sleep(1000); install(); }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        install();
        CreateThread(NULL, 0, worker, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_stop, 1);
    }
    return TRUE;
}

__declspec(dllexport) const char* ultra_codepage_spoof_id(void) {
    return "Ultra Loader codepage spoof (GetACP/GetOEMCP -> 932) v1";
}

#endif /* _WIN32 */
