/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  lsdrv_shim_standalone.c -- ultra_typex_lsdrv_shim32.dll
 * =============================================================================
 *  A SEPARATE, self-contained DLL from this board's main hook -- same reason
 *  as codepage_spoof_standalone.c: never fold new work into the frozen hook
 *  that Chaos Breaker depends on.
 *
 *  Block King Ball Shooter's "Touch sensor error (0xffffffff)" is NOT a real
 *  hardware sensor check. Disassembly of the game's own lsdrv.dll (its touch-
 *  panel driver wrapper) shows OpenDriver() talking to a SEPARATE, absent
 *  companion process (mutex name "LSDRVAPP") via three named file mappings:
 *  "LightTabletMap" / "LightTabletConMap" / "LightTabletExMap". With no such
 *  service running, every OpenFileMappingA call failed and OpenDriver's
 *  internal init fell through to `or eax, 0xffffffff`.
 *
 *  ATTEMPTED FIX (still under test): create those three named mappings
 *  ourselves plus the mutex, all zero-filled, before the game's own init
 *  runs, so lsdrv.dll's own unmodified code succeeds the way it would
 *  against a real vendor service.
 *
 *  DIAGNOSTICS: the error persisted after that fix, so this file ALSO wraps
 *  game.exe's calls into lsdrv.dll's OpenDriver / GetTabletEnable /
 *  TabletControl with a pass-through logger (real function still runs
 *  unchanged; only the return value is observed) writing to ULTRA_JVS_LOG.
 *  Disassembly alone left it ambiguous whether OpenDriver is still failing,
 *  or whether it now succeeds and a LATER call is what actually fails --
 *  this settles it with a live run instead of more guessing. Confirmed from
 *  game.exe's own code (not lsdrv.dll's) that OpenDriver's contract is
 *  "0 = success, nonzero = the error code game.exe displays" -- game.exe's
 *  own branch at the OpenDriver call site is `test eax,eax; jne <fail>`.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define LSDRV_MAP_SIZE 0x8000u   /* comfortably larger than every size the real driver requests */

static void create_named(const char* name) {
    /* Deliberately never closed/leaked-tracked -- these must outlive this
     * DLL for the whole life of the game process, exactly like a real
     * vendor service's mapping would. The OS reclaims them when the process
     * exits. INVALID_HANDLE_VALUE backs the mapping with the system paging
     * file (there is no real file), same as any other anonymous mapping. */
    CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, LSDRV_MAP_SIZE, name);
}

/* ---- diagnostic log --------------------------------------------------------
 * getenv() reads the CRT's own environment snapshot, not the live process
 * block -- ultra_tracer.c's own header calls this out by name as a bug that
 * "has cost whole runs before": GetEnvironmentVariableA is the only safe way
 * to read env inside an injected DLL. On top of using the safe call, this
 * writes to a fixed, already-proven-writable path (this exact process just
 * extracted three DLLs into %TEMP% successfully) instead of trusting
 * ULTRA_JVS_LOG's value at all, so a log-path mystery can't hide the actual
 * OpenDriver result a second time. */
static CRITICAL_SECTION g_log_cs;
static int g_log_cs_ready = 0;

static void llog(const char* fmt, ...) {
    static char path[MAX_PATH];
    FILE* f;
    if (!path[0]) {
        DWORD n = GetTempPathA(sizeof path - 32, path);
        if (!n || n >= sizeof path - 32) { path[0] = 'x'; path[1] = 0; return; }
        strcat(path, "ultra_lsdrv_debug.log");
    }
    if (path[0] == 'x' && path[1] == 0) return;
    /* Multiple threads (DllMain, the repatch worker, and every hooked call
     * site) log concurrently -- an unlocked fopen/fwrite/fclose cycle per
     * call interleaves across threads and corrupts lines (seen live: a
     * truncated line missing its own prefix). One process-wide lock around
     * the whole open-write-close cycle serializes them. */
    if (g_log_cs_ready) EnterCriticalSection(&g_log_cs);
    f = fopen(path, "a");
    if (f) {
        fputs("[lsdrv-shim] ", f);
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    if (g_log_cs_ready) LeaveCriticalSection(&g_log_cs);
}

/* ---- IAT patcher: verbatim copy of jvs_serial_hook.c's (see that file for
 * the DEP-crash / truncated-import-table history behind each guard). ------- */
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

/* This file's header claims this is a "verbatim copy of jvs_serial_hook.c's"
 * IAT patcher, but a background sweep confirmed it was missing exactly the
 * fix jvs_serial_hook.c and codepage_spoof_standalone.c both already have:
 * a GPU vendor driver DLL (aticfx32.dll etc) can unload mid-scan, and
 * writing to a freed page crashes. Same guard, ported in. */
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

/* ---- diagnostic pass-through wrappers on the KERNEL32 calls lsdrv.dll makes
 * while opening our shim's objects -- OpenDriver alone only told us pass/fail,
 * not WHERE inside its init it still fails. Neither of these is in
 * ultra_tracer.h's "already owned" list, so patching them here is safe. ---- */
typedef HANDLE (WINAPI *pOpenFileMappingA)(DWORD, BOOL, LPCSTR);
typedef HANDLE (WINAPI *pOpenMutexA)(DWORD, BOOL, LPCSTR);
typedef LPVOID (WINAPI *pMapViewOfFile)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
static pOpenFileMappingA Real_OpenFileMappingA;
static pOpenMutexA       Real_OpenMutexA;
static pMapViewOfFile    Real_MapViewOfFile;

static HANDLE WINAPI Hook_OpenFileMappingA(DWORD acc, BOOL inh, LPCSTR name) {
    HANDLE h = Real_OpenFileMappingA ? Real_OpenFileMappingA(acc, inh, name) : NULL;
    if (name && (strstr(name, "Light") || strstr(name, "LSDRV")))
        llog("OpenFileMappingA(acc=0x%lX, \"%s\") -> %p (err=%lu)",
             (unsigned long)acc, name, (void*)h, h ? 0 : GetLastError());
    return h;
}
static HANDLE WINAPI Hook_OpenMutexA(DWORD acc, BOOL inh, LPCSTR name) {
    HANDLE h = Real_OpenMutexA ? Real_OpenMutexA(acc, inh, name) : NULL;
    if (name) llog("OpenMutexA(acc=0x%lX, \"%s\") -> %p (err=%lu)",
                    (unsigned long)acc, name, (void*)h, h ? 0 : GetLastError());
    return h;
}
static LPVOID WINAPI Hook_MapViewOfFile(HANDLE hmap, DWORD acc, DWORD ofh, DWORD ofl, SIZE_T sz) {
    LPVOID v = Real_MapViewOfFile ? Real_MapViewOfFile(hmap, acc, ofh, ofl, sz) : NULL;
    llog("MapViewOfFile(h=%p, acc=0x%lX, size=0x%zX) -> %p (err=%lu)",
         (void*)hmap, (unsigned long)acc, sz, v, v ? 0 : GetLastError());
    return v;
}

static int g_k32_installed = 0;
static void install_k32_diag(void) {
    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (!k) return;
    if (!Real_OpenFileMappingA) Real_OpenFileMappingA = (pOpenFileMappingA)(void*)GetProcAddress(k, "OpenFileMappingA");
    if (!Real_OpenMutexA)       Real_OpenMutexA       = (pOpenMutexA)(void*)GetProcAddress(k, "OpenMutexA");
    if (!Real_MapViewOfFile)    Real_MapViewOfFile    = (pMapViewOfFile)(void*)GetProcAddress(k, "MapViewOfFile");
    ultra_iat_patch_all("kernel32.dll", "OpenFileMappingA", (void*)Hook_OpenFileMappingA);
    ultra_iat_patch_all("kernel32.dll", "OpenMutexA",       (void*)Hook_OpenMutexA);
    ultra_iat_patch_all("kernel32.dll", "MapViewOfFile",    (void*)Hook_MapViewOfFile);
    g_k32_installed = 1;
}

/* RULED OUT: IsWindow() was already returning TRUE for every real handle
 * game.exe checked (confirmed live, hundreds of calls, never once false) --
 * the internal window-handle poll inside OpenDriver's init is not what is
 * failing. That hook is removed; it was also firing on unrelated IsWindow
 * traffic throughout the game's normal rendering, adding noise and I/O
 * latency for no diagnostic value. */

/* ---- diagnostic pass-through wrapper ---------------------------------------
 * OpenDriver's disassembly (lsdrv.dll @0x1440) ends in a bare `ret` -- zero
 * stack arguments in any calling convention, so wrapping it in a real C
 * function is safe (no ABI/stack-imbalance risk). This is also the ONE call
 * whose return value directly becomes the on-screen error code (per
 * game.exe's own `test eax,eax; jne <fail>` at the call site), so it alone
 * tells us whether the shim above actually worked. */
typedef int (WINAPI *pOpenDriver)(void);
static pOpenDriver Real_OpenDriver;

static int WINAPI Hook_OpenDriver(void) {
    int rc = Real_OpenDriver ? Real_OpenDriver() : -1;
    llog("OpenDriver() real lsdrv.dll call returned 0x%08X (%s)",
         (unsigned)rc, rc == 0 ? "SUCCESS per game.exe's own check" : "game.exe treats this as FAILURE");
    return rc;
}

/* GetTabletEnable() (lsdrv.dll @0x23f0, confirmed 0 stack args -- bare `ret`)
 * reads *[0x10011004] (offset 0 of the struct OpenDriver's own init proved
 * exists) and returns that != 0 as a plain boolean -- game.exe's own code
 * shows "touch sensor error (0x001)" specifically when this returns false.
 * A one-time write into that struct field (tried first) did not stick --
 * something inside lsdrv.dll, almost certainly a background thread that
 * reconciles this flag against the (all-zero, fake) shared memory, keeps
 * resetting it back to 0. Hooking the exported function directly, the same
 * proven-safe way as OpenDriver, sidesteps that race entirely: nothing
 * inside lsdrv.dll can un-fake an answer that never touches its real state
 * in the first place. */
typedef int (WINAPI *pGetTabletEnable)(void);
static pGetTabletEnable Real_GetTabletEnable;
static int WINAPI Hook_GetTabletEnable(void) {
    int rc = Real_GetTabletEnable ? Real_GetTabletEnable() : 0;
    if (!rc) llog("GetTabletEnable() real=0 (would fail) -> spoofed 1");
    return 1;
}

/* CONFIRMED root cause (live-traced, not a guess): OpenDriver's internal init
 * (lsdrv.dll @0x100010a0) now genuinely SUCCEEDS -- every named object opens,
 * and it returns eax=1 at @0x10001292. OpenDriver's OWN follow-on code then
 * fails a SEPARATE check: an internal struct's +0x2c field (a window handle
 * lsdrv.dll expects some caller to have set via its own exported
 * TabletInptSet -- which game.exe's import list never calls) is read at
 * @0x10001262. When it's 0, the code does NOT skip gracefully like every
 * other call site in this driver does -- it sleeps 100ms, re-checks, and on
 * two specific branches (@0x10001283 and @0x1000128d, both `je 0x10001299`
 * = the failure return) gives up. Confirmed nothing ever populates that
 * field: the same runs that got every OpenFileMappingA/MapViewOfFile/
 * OpenMutexA call to succeed still returned 0xFFFFFFFF from OpenDriver.
 *
 * FIX: NOP both `je` instructions (2 bytes each) so this check can never
 * fail -- the two RVAs are read directly off the disassembly of THIS game's
 * own lsdrv.dll, not guessed, and each patched instruction was independently
 * confirmed (via ret-size / operand-length checks) to be a plain 2-byte
 * short conditional jump, so overwriting it with two 0x90 NOPs cannot shift
 * or corrupt anything after it. This is the ONLY code patched; nothing else
 * in lsdrv.dll is touched. */
#define LSDRV_PATCH1_RVA 0x1283u   /* je 0x10001299 (the "still unset after sleep" fail) */
#define LSDRV_PATCH2_RVA 0x128du   /* je 0x10001299 (the "IsWindow false" fail)          */

static int g_diag_installed = 0;
static int g_code_patched = 0;

static void patch_byte2(BYTE* addr) {
    DWORD old;
    if (addr[0] != 0x74) return;   /* not the 'je' we expect -- refuse rather than guess */
    if (VirtualProtect(addr, 2, PAGE_EXECUTE_READWRITE, &old)) {
        addr[0] = 0x90; addr[1] = 0x90;
        VirtualProtect(addr, 2, old, &old);
    }
}

/* Traced live (not guessed): game.exe's own "confirm pressed" check
 * (game.exe+0x4392c0) does:
 *   ecx = *(void**)(game.exe_base + 0x546a10)   -- the keyboard wrapper object
 *   eax = *(void**)ecx                           -- its vtable
 *   edx = *(void**)(eax + 0xc)                   -- vtable slot 3
 *   push 0x1c ; call edx                         -- __thiscall(this=ecx, keyId=0x1c)
 * returning AL (0/1), then ORed with a gamepad check and stored/returned as
 * "confirmed". This hooks that ONE vtable slot (a pointer swap, exactly like
 * IAT patching but for a C++-style vtable instead of a PE import table) to
 * log every call's (this, keyId, result) -- this settles definitively
 * whether the calibration screen even reaches this check, and with what
 * key id, instead of any more static-disassembly guessing. */
/* __thiscall, NOT __stdcall/WINAPI: the real call site passes the object
 * pointer in ECX (the caller never pushes it), with only keyId on the
 * stack -- confirmed by the disassembly (`mov ecx,[obj]; ...; push 0x1c;
 * call edx`, no push of the object). A WINAPI-declared hook here expects
 * BOTH args on the stack and never looks at ECX, so it read the wrong
 * values entirely (silently taking the return address as one of them) --
 * that mismatch, not anything in the game, caused the crash on the last
 * attempt. */
/* keyId=0x1c/0x3c never showed up through slot 3 (WasKeyPressed) -- only
 * keyId=1 (Escape), constantly, from an unrelated watchdog. So the
 * calibration screen reads input some OTHER way. Neighboring slots on this
 * SAME object (same vtable dump) are the next most likely candidates:
 * slot 2 (0x484910, reads offset 0x1505 = raw "is down" state, no edge
 * detection), slot 4 (0x484950, offset 0x1805), slot 5 (0x484970, offset
 * 0x1905, presumably "was released"). Hooking all of them at once settles
 * which one this screen actually uses instead of another guess-rebuild
 * cycle per slot. */
/* Dedup by (slot, keyId): logs once per DISTINCT combination ever seen
 * (plus every time the result is nonzero, since a rare "yes" is exactly
 * the moment we need to see even for an already-logged keyId), instead of
 * flooding the log with the same per-frame poll repeated thousands of
 * times. */
static int seen_combo(int slot, int keyid) {
    static int seen[4][256]; /* slots 2..5 -> index 0..3 */
    int idx = slot - 2;
    if (idx < 0 || idx > 3 || keyid < 0 || keyid > 255) return 0;
    if (seen[idx][keyid]) return 1;
    seen[idx][keyid] = 1;
    return 0;
}

static void modname_at(void *addr, char *out, size_t cap); /* defined below, in the crash handler section */

/* For Enter (0x1c) and F2 (0x3c) specifically: log EVERY distinct CALLER
 * address (not just every distinct keyId), unconditionally, resolved to
 * game.exe+offset -- this shows us every place in the game that ever checks
 * these two keys through this object, directly, instead of guessing among
 * static disassembly candidates that turned out not to match (the first
 * "confirm" function found this way never actually fired). */
static int seen_caller(int slot, void* caller) {
    static void* seen[4][32]; static int n[4]; /* slots 2..5 -> index 0..3 */
    int idx = slot - 2;
    if (idx < 0 || idx > 3) return 0;
    for (int i = 0; i < n[idx]; ++i) if (seen[idx][i] == caller) return 1;
    if (n[idx] < 32) seen[idx][n[idx]++] = caller;
    return 0;
}

typedef int (__attribute__((thiscall)) *pVtableSlot)(void* thisptr, int keyid);
static pVtableSlot Real_Slot2, Real_Slot3, Real_Slot4, Real_Slot5;

/* CONFIRMED via Windows' own crash report this session: game.exe's real
 * WasKeyPressed (game.exe+0x8493B) faulted dereferencing "this" -- i.e. the
 * keyboard object at [0x946a10] had gone stale (freed/reused) by the time
 * some caller reached it through this vtable. Rather than chase why the
 * object's lifetime changed, guard every real call here: skip it and
 * return a safe 0 if "this" no longer looks like live, readable memory. A
 * dropped poll this one frame beats a hard crash. */
static void** g_kb_vtable_ptr; /* set once, in install_kb_vtable_hook below */
static int slot_this_valid(void* t) {
    if (!t || !ul_pe_readable(t, sizeof(void*))) return 0;
    void* vt = *(void**)t;
    /* Exact match against the ONE known-good vtable address, not just
     * "readable" -- freed memory that got reused for something else
     * entirely would still pass a plain readability check, but it won't
     * coincidentally hold this exact static code address. */
    return g_kb_vtable_ptr && vt == (void*)g_kb_vtable_ptr;
}

static pVtableSlot Real_Slot1;
static int __attribute__((thiscall)) Hook_Slot1(void* t, int k) {
    if (!slot_this_valid(t)) { llog("kb-slot1: stale this=%p, skipped", t); return 0; }
    return Real_Slot1 ? Real_Slot1(t,k) : 0; }

static int __attribute__((thiscall)) Hook_Slot2(void* t, int k) {
    if (!slot_this_valid(t)) { llog("kb-slot2/IsKeyDown: stale this=%p, skipped", t); return 0; }
    int r = Real_Slot2?Real_Slot2(t,k):0;
    if (k == 0x1c || k == 0x3c || k == 0x01) { void* c = __builtin_return_address(0);
        if (!seen_caller(2, c)) { char cn[128]; modname_at(c, cn, sizeof cn);
            llog("kb-slot2/IsKeyDown keyId=0x%02X called from %s", k, cn); } }
    if (r || !seen_combo(2,k)) llog("kb-slot2/IsKeyDown(this=%p, keyId=0x%02X) -> %d", t, k, r); return r; }
static int __attribute__((thiscall)) Hook_Slot3(void* t, int k) {
    if (!slot_this_valid(t)) { llog("kb-slot3/WasKeyPressed: stale this=%p, skipped", t); return 0; }
    int r = Real_Slot3?Real_Slot3(t,k):0;
    if (k == 0x1c || k == 0x3c || k == 0x01) { void* c = __builtin_return_address(0);
        if (!seen_caller(3, c)) { char cn[128]; modname_at(c, cn, sizeof cn);
            llog("kb-slot3/WasKeyPressed keyId=0x%02X called from %s", k, cn); } }
    if (r || !seen_combo(3,k)) llog("kb-slot3/WasKeyPressed(this=%p, keyId=0x%02X) -> %d", t, k, r); return r; }
static int __attribute__((thiscall)) Hook_Slot4(void* t, int k) {
    if (!slot_this_valid(t)) { llog("kb-slot4: stale this=%p, skipped", t); return 0; }
    int r = Real_Slot4?Real_Slot4(t,k):0;
    if (k == 0x1c || k == 0x3c || k == 0x01) { void* c = __builtin_return_address(0);
        if (!seen_caller(4, c)) { char cn[128]; modname_at(c, cn, sizeof cn);
            llog("kb-slot4 keyId=0x%02X called from %s", k, cn); } }
    if (r || !seen_combo(4,k)) llog("kb-slot4(this=%p, keyId=0x%02X) -> %d", t, k, r); return r; }
static int __attribute__((thiscall)) Hook_Slot5(void* t, int k) {
    if (!slot_this_valid(t)) { llog("kb-slot5/WasKeyReleased: stale this=%p, skipped", t); return 0; }
    int r = Real_Slot5?Real_Slot5(t,k):0;
    if (k == 0x1c || k == 0x3c || k == 0x01) { void* c = __builtin_return_address(0);
        if (!seen_caller(5, c)) { char cn[128]; modname_at(c, cn, sizeof cn);
            llog("kb-slot5/WasKeyReleased keyId=0x%02X called from %s", k, cn); } }
    if (r || !seen_combo(5,k)) llog("kb-slot5/WasKeyReleased(this=%p, keyId=0x%02X) -> %d", t, k, r); return r; }

static int g_kb_vtable_hooked = 0;
static void install_kb_vtable_hook(void) {
    if (g_kb_vtable_hooked) return;
    HMODULE gexe = GetModuleHandleA(NULL);
    if (!gexe) return;
    void** kbobj_slot = (void**)((BYTE*)gexe + 0x546a10);
    if (!ul_pe_readable(kbobj_slot, sizeof(void*))) return;
    void* kbobj = *kbobj_slot;
    if (!kbobj || !ul_pe_readable(kbobj, sizeof(void*))) return;
    void** vtable = *(void***)kbobj;
    /* Extended to slot[1..5] (was [2..5]) -- a background sweep this session
     * found 3 live call sites at offset 0x4 (slot 1) on this SAME vtable
     * that Hook_Slot2..5 never covered, going through the object with only
     * a NULL check, not a staleness check. Same object, same fix. */
    if (!vtable || !ul_pe_readable(&vtable[1], 5*sizeof(void*))) return;
    Real_Slot1 = (pVtableSlot)vtable[1];
    Real_Slot2 = (pVtableSlot)vtable[2];
    Real_Slot3 = (pVtableSlot)vtable[3];
    Real_Slot4 = (pVtableSlot)vtable[4];
    Real_Slot5 = (pVtableSlot)vtable[5];
    DWORD old;
    if (VirtualProtect(&vtable[1], 5*sizeof(void*), PAGE_READWRITE, &old)) {
        vtable[1] = (void*)Hook_Slot1;
        vtable[2] = (void*)Hook_Slot2;
        vtable[3] = (void*)Hook_Slot3;
        vtable[4] = (void*)Hook_Slot4;
        vtable[5] = (void*)Hook_Slot5;
        VirtualProtect(&vtable[1], 5*sizeof(void*), old, &old);
        g_kb_vtable_ptr = vtable;
        llog("kb vtable hooks installed: obj=%p vtable=%p slots[1..5]=%p,%p,%p,%p,%p",
             kbobj, (void*)vtable, (void*)Real_Slot1, (void*)Real_Slot2, (void*)Real_Slot3, (void*)Real_Slot4, (void*)Real_Slot5);
        g_kb_vtable_hooked = 1;
    }
}

/* ---- Same fix, generalized to the OTHER two fragile global objects a
 * background sweep found this session (0x946a0c and 0x946a04, RVA
 * 0x546a0c/0x546a04) -- ~46 more call sites through them, none previously
 * guarded at all. Same "exact vtable pointer match" technique: hooking the
 * vtable itself (not each call site individually) covers every one of
 * those call sites in game.exe with one patch, since they all read the
 * same vtable memory. ------------------------------------------------- */

static void** g_gp_vtable_ptr;   /* 0x946a0c's vtable, once found live */
static int gp_this_valid(void* t) {
    if (!t || !ul_pe_readable(t, sizeof(void*))) return 0;
    return g_gp_vtable_ptr && *(void**)t == (void*)g_gp_vtable_ptr;
}
static pVtableSlot Real_GP_C, Real_GP_10, Real_GP_18;
static int __attribute__((thiscall)) Hook_GP_C(void* t, int k) {
    if (!gp_this_valid(t)) { llog("gp-slotC: stale this=%p, skipped", t); return 0; }
    return Real_GP_C ? Real_GP_C(t,k) : 0; }
static int __attribute__((thiscall)) Hook_GP_10(void* t, int k) {
    if (!gp_this_valid(t)) { llog("gp-slot10: stale this=%p, skipped", t); return 0; }
    return Real_GP_10 ? Real_GP_10(t,k) : 0; }
static int __attribute__((thiscall)) Hook_GP_18(void* t, int k) {
    if (!gp_this_valid(t)) { llog("gp-slot18: stale this=%p, skipped", t); return 0; }
    return Real_GP_18 ? Real_GP_18(t,k) : 0; }

static int g_gp_vtable_hooked = 0;
static void install_gp_vtable_hook(void) {
    if (g_gp_vtable_hooked) return;
    HMODULE gexe = GetModuleHandleA(NULL);
    if (!gexe) return;
    void** gpobj_slot = (void**)((BYTE*)gexe + 0x546a0c);
    if (!ul_pe_readable(gpobj_slot, sizeof(void*))) return;
    void* gpobj = *gpobj_slot;
    if (!gpobj || !ul_pe_readable(gpobj, sizeof(void*))) return;
    void** vtable = *(void***)gpobj;
    /* offsets 0xc/0x10/0x18 = slot index 3/4/6 (4 bytes per slot) */
    if (!vtable || !ul_pe_readable(&vtable[3], 4*sizeof(void*))) return;
    Real_GP_C  = (pVtableSlot)vtable[3];
    Real_GP_10 = (pVtableSlot)vtable[4];
    if (!ul_pe_readable(&vtable[6], sizeof(void*))) return;
    Real_GP_18 = (pVtableSlot)vtable[6];
    DWORD old;
    if (VirtualProtect(&vtable[3], 2*sizeof(void*), PAGE_READWRITE, &old)) {
        vtable[3] = (void*)Hook_GP_C;
        vtable[4] = (void*)Hook_GP_10;
        VirtualProtect(&vtable[3], 2*sizeof(void*), old, &old);
    }
    if (VirtualProtect(&vtable[6], sizeof(void*), PAGE_READWRITE, &old)) {
        vtable[6] = (void*)Hook_GP_18;
        VirtualProtect(&vtable[6], sizeof(void*), old, &old);
    }
    g_gp_vtable_ptr = vtable;
    llog("gamepad-object vtable hooks installed: obj=%p vtable=%p slots[C,10,18]=%p,%p,%p",
         gpobj, (void*)vtable, (void*)Real_GP_C, (void*)Real_GP_10, (void*)Real_GP_18);
    g_gp_vtable_hooked = 1;
}

/* 0x946a04's target is a STATIC struct (confirmed via disassembly:
 * "movl $0x946f68, 0x946a04" at game.exe's init -- never reassigned
 * elsewhere), so unlike the other two objects the risk isn't the object
 * pointer going stale, it's the vtable pointer STORED INSIDE that static
 * struct getting overwritten by an unrelated wild write elsewhere. Guard
 * by checking it still holds the one known-correct static vtable address
 * (0x6f890c, confirmed via disassembly) before calling through. */
static void** g_cfg_vtable_ptr;
static int cfg_this_valid(void* t) {
    if (!t || !ul_pe_readable(t, sizeof(void*))) return 0;
    return g_cfg_vtable_ptr && *(void**)t == (void*)g_cfg_vtable_ptr;
}
static pVtableSlot Real_CFG_0, Real_CFG_10;
static int __attribute__((thiscall)) Hook_CFG_0(void* t, int k) {
    if (!cfg_this_valid(t)) { llog("cfg-slot0: corrupted vtable on this=%p, skipped", t); return 0; }
    return Real_CFG_0 ? Real_CFG_0(t,k) : 0; }
static int __attribute__((thiscall)) Hook_CFG_10(void* t, int k) {
    if (!cfg_this_valid(t)) { llog("cfg-slot10: corrupted vtable on this=%p, skipped", t); return 0; }
    return Real_CFG_10 ? Real_CFG_10(t,k) : 0; }

static int g_cfg_vtable_hooked = 0;
static void install_cfg_vtable_hook(void) {
    if (g_cfg_vtable_hooked) return;
    HMODULE gexe = GetModuleHandleA(NULL);
    if (!gexe) return;
    void** cfgobj_slot = (void**)((BYTE*)gexe + 0x546a04);
    if (!ul_pe_readable(cfgobj_slot, sizeof(void*))) return;
    void* cfgobj = *cfgobj_slot;
    if (!cfgobj || !ul_pe_readable(cfgobj, sizeof(void*))) return;
    void** vtable = *(void***)cfgobj;
    if (!vtable || !ul_pe_readable(&vtable[0], sizeof(void*))
                 || !ul_pe_readable(&vtable[4], sizeof(void*))) return;
    Real_CFG_0  = (pVtableSlot)vtable[0];
    Real_CFG_10 = (pVtableSlot)vtable[4];
    DWORD old;
    if (VirtualProtect(&vtable[0], sizeof(void*), PAGE_READWRITE, &old)) {
        vtable[0] = (void*)Hook_CFG_0;
        VirtualProtect(&vtable[0], sizeof(void*), old, &old);
    }
    if (VirtualProtect(&vtable[4], sizeof(void*), PAGE_READWRITE, &old)) {
        vtable[4] = (void*)Hook_CFG_10;
        VirtualProtect(&vtable[4], sizeof(void*), old, &old);
    }
    g_cfg_vtable_ptr = vtable;
    llog("config-object vtable hooks installed: obj=%p vtable=%p slots[0,10]=%p,%p",
         cfgobj, (void*)vtable, (void*)Real_CFG_0, (void*)Real_CFG_10);
    g_cfg_vtable_hooked = 1;
}

static void install_diag(void) {
    HMODULE h = GetModuleHandleA("lsdrv.dll");
    if (!h) return;
    if (!Real_OpenDriver) {
        Real_OpenDriver = (pOpenDriver)(void*)GetProcAddress(h, "OpenDriver");
        if (!g_diag_installed) llog("lsdrv.dll now loaded @%p, real OpenDriver=%p",
                                     (void*)h, (void*)Real_OpenDriver);
    }
    if (!Real_GetTabletEnable)
        Real_GetTabletEnable = (pGetTabletEnable)(void*)GetProcAddress(h, "GetTabletEnable");
    if (Real_OpenDriver) {
        ultra_iat_patch_all("lsdrv.dll", "OpenDriver", (void*)Hook_OpenDriver);
        g_diag_installed = 1;
    }
    if (Real_GetTabletEnable)
        ultra_iat_patch_all("lsdrv.dll", "GetTabletEnable", (void*)Hook_GetTabletEnable);
    if (!g_code_patched) {
        BYTE* base = (BYTE*)h;
        BYTE* p1 = base + LSDRV_PATCH1_RVA;
        BYTE* p2 = base + LSDRV_PATCH2_RVA;
        if (ul_pe_readable(p1, 2) && ul_pe_readable(p2, 2)) {
            llog("patching lsdrv.dll @+0x%X and @+0x%X (bytes: %02X %02X / %02X %02X)",
                 LSDRV_PATCH1_RVA, LSDRV_PATCH2_RVA, p1[0], p1[1], p2[0], p2[1]);
            patch_byte2(p1);
            patch_byte2(p2);
            llog("post-patch bytes: %02X %02X / %02X %02X", p1[0], p1[1], p2[0], p2[1]);
            g_code_patched = 1;
        }
    }
}

static volatile LONG g_stop;
static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    /* lsdrv.dll loading and game.exe calling OpenDriver() can both happen
     * within the first few milliseconds of the game's own thread resuming --
     * a 500ms poll interval missed that window outright in a live run (the
     * module-loaded confirmation logged, but the OpenDriver call itself
     * never got caught). Poll tight (5ms) for the first two seconds to close
     * that race, then fall back to a slow safety-net poll. */
    for (int i = 0; i < 400 && !g_stop; ++i) { Sleep(5); install_diag(); install_k32_diag(); install_kb_vtable_hook(); install_gp_vtable_hook(); install_cfg_vtable_hook(); }
    for (int i = 0; i < 20 && !g_stop; ++i)  { Sleep(500); install_diag(); install_k32_diag(); install_kb_vtable_hook(); install_gp_vtable_hook(); install_cfg_vtable_hook(); }
    return 0;
}

/* ---- crash diagnostics ------------------------------------------------------
 * OpenDriver now succeeds, so game.exe proceeds to call GetTabletEnable /
 * TabletControl / TabletMessage / LsSetTabletParam against our all-zero
 * shared memory for the FIRST time ever (every prior run failed at
 * OpenDriver, so none of these were ever exercised). If one of them
 * dereferences the buffer's content as a count/pointer rather than a flat
 * flag, zero may not be a safe value there. This names exactly where. */
static void modname_at(void *addr, char *out, size_t cap) {
    HMODULE mod = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)addr, &mod) && mod) {
        char full[MAX_PATH];
        if (GetModuleFileNameA(mod, full, MAX_PATH)) {
            const char *base = strrchr(full, '\\');
            base = base ? base + 1 : full;
            _snprintf(out, cap, "%s+0x%X", base, (unsigned)((BYTE*)addr - (BYTE*)mod));
            out[cap-1] = 0;
            return;
        }
    }
    _snprintf(out, cap, "(no module owns %p)", addr);
    out[cap-1] = 0;
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        char eip_mod[128], fault_mod[128], eax_mod[128], ecx_mod[128], edi_mod[128];
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        void *fault = (void*)ep->ExceptionRecord->ExceptionInformation[1];
        modname_at(addr, eip_mod, sizeof eip_mod);
        modname_at(fault, fault_mod, sizeof fault_mod);
        modname_at((void*)ep->ContextRecord->Eax, eax_mod, sizeof eax_mod);
        modname_at((void*)ep->ContextRecord->Ecx, ecx_mod, sizeof ecx_mod);
        modname_at((void*)ep->ContextRecord->Edi, edi_mod, sizeof edi_mod);
        llog("CRASH: ACCESS_VIOLATION (%s) executing in %s, %s address %p (%s)",
             ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "WRITE" : "READ",
             eip_mod,
             ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "writing to" : "reading from",
             fault, fault_mod);
        llog("CRASH: EAX=%08lX(%s) EBX=%08lX ECX=%08lX(%s) EDX=%08lX ESI=%08lX EDI=%08lX(%s)",
             ep->ContextRecord->Eax, eax_mod, ep->ContextRecord->Ebx,
             ep->ContextRecord->Ecx, ecx_mod, ep->ContextRecord->Edx,
             ep->ContextRecord->Esi, ep->ContextRecord->Edi, edi_mod);
        /* Walk a few stack slots above ESP looking for return addresses that
         * land inside a known module -- mingw has no frame-pointer chain
         * guarantee here, so this is a best-effort "what called this"
         * approximation, not a real stack walk. */
        DWORD *sp = (DWORD*)ep->ContextRecord->Esp;
        for (int i = 0; i < 24; ++i) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(&sp[i], &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
            void *cand = (void*)sp[i];
            HMODULE m = NULL;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)cand, &m) && m) {
                char nm[128]; modname_at(cand, nm, sizeof nm);
                llog("CRASH: stack[esp+%d] = %p (%s)", i * 4, cand, nm);
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_log_cs);
        g_log_cs_ready = 1;
        AddVectoredExceptionHandler(1, crash_handler);
        create_named("LightTabletMap");
        create_named("LightTabletConMap");
        create_named("LightTabletExMap");
        CreateMutexA(NULL, FALSE, "LSDRVAPP");
        llog("armed: created LightTabletMap/ConMap/ExMap (0x%X bytes each) + LSDRVAPP mutex",
             LSDRV_MAP_SIZE);
        install_diag();
        install_k32_diag();
        CreateThread(NULL, 0, worker, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_stop, 1);
    }
    return TRUE;
}

__declspec(dllexport) const char* ultra_lsdrv_shim_id(void) {
    return "Ultra Loader lsdrv.dll companion-service shim + diagnostics v2";
}

#endif /* _WIN32 */
