/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  ringedge2_hook_main.c  --  ultra_sega_ringedge2_hook{32,64}.dll entry point.
 * =============================================================================
 *
 *  Owns the JVS engine + the loader<->game input bridge, and fires every
 *  subsystem installer on DLL_PROCESS_ATTACH. Injected into the game process
 *  by modules/sega_ringedge2/src/sega_ringedge2_main.c via a suspended-launch +
 *  QueueUserAPC(LoadLibraryA) (primary) with a CreateRemoteThread(LoadLibraryA)
 *  fallback -- so hooks must be installed SYNCHRONOUSLY here before the game's
 *  first thread runs (else its first CreateFile(COM)/Direct3DCreateN is missed).
 *
 *  This file provides ul_re2_jvs_node()/ul_re2_pull_input() so
 *  jvs_serial_ringedge2.c can answer the game's JVS polls with live input --
 *  the loader's shared-memory block (ULTRA_IO_SHM) when a loader drives it,
 *  else a local XInput gamepad so a standalone test run is still controllable.
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 API + the open
 *  JVS spec. No third-party loader code, no dependency on any other board's
 *  hook or on modules/native_x86/src/hook/'s compiled output.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "ringedge2_hook.h"
#include "jvs_core_ringedge2.h"
#include "ultra_io_shared.h"
#include "omni_bridge.h"
#include "ultra_tracer.h"

/* Shared pause/settings overlay (pause_menu_wire_ringedge2.c -> work/src/
 * pause_menu_service.c). Self-contained poll thread, see that file's header. */
void ultra_pausemenu_install(HWND (*get_hwnd)(void));
void ultra_pausemenu_shutdown(void);

/* -------------------------------------------------------------------------- */
/*  Diagnostics (ULTRA_JVS_LOG=<path>; every hook TU in this tree logs here)   */
/* -------------------------------------------------------------------------- */
void ul_re2_log(const char* fmt, ...) {
    const char* path = getenv("ULTRA_JVS_LOG");
    if (!path || !*path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    char buf[768];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    fprintf(f, "[ringedge2] %s\n", buf);
    fclose(f);
}

/* -------------------------------------------------------------------------- */
/*  Env helpers                                                                */
/* -------------------------------------------------------------------------- */
const char* ul_re2_env(const char* name) {
    static char buf[16][512];
    static int slot = 0;
    char* b = buf[slot]; slot = (slot + 1) % 16;
    DWORD n = GetEnvironmentVariableA(name, b, 512);
    if (n == 0 || n >= 512) return NULL;
    return b;
}
int ul_re2_env_on(const char* name) {
    const char* v = ul_re2_env(name);
    return v && v[0] == '1' && v[1] == 0;
}

/* -------------------------------------------------------------------------- */
/*  IAT patcher (RVA-bounded; patches by name AND by real-pointer value so a    *
 *  re-install / bound import is idempotent). Shared by every TU in this tree. */
/* -------------------------------------------------------------------------- */
static int rva_ok(DWORD rva, DWORD isize) { return rva != 0 && rva < isize; }

/* ---- IS THIS MEMORY ACTUALLY THERE? ---------------------------------------
 *
 * THE CRASH THIS EXISTS TO PREVENT (Caladrius AC, 2026-09-18, pid 17852):
 *
 *   HOOK32+0x154e  cmp word ptr [ebp],5A4Dh   ss:002b:6d210000=????
 *   Unloaded modules:  6d210000 6d3f7000   d3dcompiler_42.dll
 *
 * 0x6d210000 was EXACTLY the base of d3dcompiler_42.dll. D3DX9 loads that DLL
 * during shader bring-up and FreeLibrary's it again moments later. We had taken
 * the module list from EnumProcessModules, and by the time we read the 'MZ' at
 * one of those bases the DLL was gone -- so we read unmapped address space and
 * the process died. An unhandled AV: both VEHs in this tree are observe-only and
 * return EXCEPTION_CONTINUE_SEARCH, and there is no SEH to catch it.
 *
 * The handle was NOT garbage. The count clamp above is correct -- every slot we
 * read had been written by the API -- so this is a pure TOCTOU: the list is a
 * SNAPSHOT, and a snapshot of loaded modules starts going stale immediately.
 *
 * mingw32 has no __try/__except, so we ask the memory manager instead. This is
 * the same helper the tracer has carried since it was written, for exactly this
 * reason -- see tr_readable() in work/src/ultra_tracer.c:126. Keeping a local
 * copy rather than sharing a header is deliberate: this file is one of 41
 * near-identical board copies, and in this tree a new shared include has broken
 * locked games before.
 *
 * It narrows the race rather than closing it (a module can still unload between
 * the query and the read), and that is acceptable BECAUSE IT CAN ONLY SUBTRACT:
 * it refuses memory we could not have read anyway, so on a module whose headers
 * are mapped it is a provable no-op. That is what makes it safe to roll out to
 * boards carrying locked games. Closing the race properly needs the module
 * PINNED (GetModuleHandleExA + FreeLibrary), which must NOT be done on the
 * DllMain path -- FreeLibrary under the PE loader lock is prohibited, and under
 * that lock the list cannot change anyway.                                    */
static const unsigned char* g_rd_lo;    /* one-region cache: these walks are    */
static const unsigned char* g_rd_hi;    /* overwhelmingly sequential            */

static int mem_readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    const unsigned char* a = (const unsigned char*)p;
    const unsigned char* end;
    if (!a) return 0;
    end = a + n;
    if (a >= g_rd_lo && end <= g_rd_hi) return 1;          /* cached fast path */
    while (a < end) {
        if (!VirtualQuery(a, &mbi, sizeof mbi))   return 0;
        if (mbi.State != MEM_COMMIT)              return 0;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        if (mbi.Protect & PAGE_EXECUTE)           return 0;  /* execute-only */
        g_rd_lo = (const unsigned char*)mbi.BaseAddress;
        g_rd_hi = g_rd_lo + mbi.RegionSize;
        a = g_rd_hi;
    }
    return 1;
}

static void iat_patch_module(HMODULE mod, const char* impdll,
                             const char* impfn, void* newfn, void* realfn) {
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (!mem_readable(dos, sizeof *dos)) return;      /* module unloaded under us */
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (!mem_readable(nt, sizeof *nt)) return;
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    DWORD isize = nt->OptionalHeader.SizeOfImage;
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!rva_ok(dir.VirtualAddress, isize)) return;
    IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    for (; mem_readable(desc, sizeof *desc) && rva_ok(desc->Name, isize); ++desc) {
        const char* dllname = (const char*)(base + desc->Name);
        if (!mem_readable(dllname, 1)) continue;
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

void ul_re2_iat_patch_all(const char* impdll, const char* impfn, void* newfn) {
    void* realfn = NULL;
    HMODULE imp = GetModuleHandleA(impdll);
    if (imp) realfn = (void*)GetProcAddress(imp, impfn);

    HMODULE mods[512]; DWORD need = 0;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ul_re2_iat_patch_all, &self);
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        DWORD count = need / sizeof(HMODULE);
        if (count > 512) count = 512;   /* `need` may exceed our buffer; clamp */
        for (DWORD i = 0; i < count; ++i)
            if (mods[i] != self)                 /* don't patch our own imports */
                iat_patch_module(mods[i], impdll, impfn, newfn, realfn);
    }
}

/* -------------------------------------------------------------------------- */
/*  JVS node + loader shared-memory                                            */
/* -------------------------------------------------------------------------- */
static ultra_jvs_node_ringedge2* g_node   = NULL;
static HANDLE                    g_map    = NULL;
static ultra_io_shared*          g_shared = NULL;
static uint32_t                  g_coin_seen[ULTRA_IO_COINS] = {0};
static volatile LONG             g_stop   = 0;

ultra_jvs_node_ringedge2* ul_re2_jvs_node(void) { return g_node; }

/* ---- standalone gamepad (XInput) fallback ---------------------------------
 * When no loader is feeding the shared block (e.g. a standalone test launch),
 * read the player's gamepad directly so the JVS node is actually controllable.
 * A generic switch/axis mapping only -- no per-game autopilot scripting (that
 * class of Type-X-specific hack belongs to native_x86, not this board). */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } ul_xpad;
typedef struct { DWORD packet; ul_xpad pad; } ul_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, ul_xstate*);
static p_xinput_get    s_xget = NULL;
static ultra_jvs_input_ringedge2 g_xinput_in;
static volatile LONG   g_local_coin = 0;
static long            g_local_coin_seen = 0;

static DWORD WINAPI gamepad_thread(LPVOID arg) {
    (void)arg;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
    for (int i = 0; dlls[i] && !s_xget; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) s_xget = (p_xinput_get)(void*)GetProcAddress(h, "XInputGetState");
    }
    for (int a = 0; a < JVS_RINGEDGE2_MAX_ANALOG; ++a) g_xinput_in.analog[a] = 0x8000;
    g_xinput_in.screen_x = g_xinput_in.screen_y = 0x8000;
    if (!s_xget) return 0;   /* no XInput available on this host: nothing to poll */
    int coin_was = 0;
    while (!g_stop) {
        ul_xstate st; memset(&st, 0, sizeof st);
        if (s_xget(0, &st) == 0) {                 /* controller 0 present */
            WORD b = st.pad.wButtons;
            int up=!!(b&0x0001), dn=!!(b&0x0002), lf=!!(b&0x0004), rt=!!(b&0x0008);
            int start=!!(b&0x0010), test=!!(b&0x0020) /*BACK*/, svc=!!(b&0x0100) /*LB*/;
            int p1=!!(b&0x1000)/*A*/, p2=!!(b&0x2000)/*B*/, p3=!!(b&0x4000)/*X*/,
                p4=!!(b&0x8000)/*Y*/, p5=!!(b&0x0200)/*RB*/, p6=!!(b&0x0080)/*R3*/;
            if (st.pad.lx < -10000) lf = 1; else if (st.pad.lx > 10000) rt = 1;
            if (st.pad.ly >  10000) up = 1; else if (st.pad.ly < -10000) dn = 1;
            uint8_t byte1 = (uint8_t)((start<<7)|(svc<<6)|(up<<5)|(dn<<4)|(lf<<3)|(rt<<2)|(p1<<1)|p2);
            uint8_t byte2 = (uint8_t)((p3<<7)|(p4<<6)|(p5<<5)|(p6<<4));
            ultra_jvs_input_ringedge2 in; memset(&in, 0, sizeof in);
            in.player[0] = (uint16_t)((byte1<<8)|byte2);
            in.system    = test ? 0x80 : 0;        /* TEST switch */
            in.analog[0] = (uint16_t)((int)st.pad.lx + 0x8000);
            in.analog[1] = (uint16_t)(0x7FFF - (int)st.pad.ly);
            for (int a = 2; a < JVS_RINGEDGE2_MAX_ANALOG; ++a) in.analog[a] = 0x8000;
            in.screen_x = in.screen_y = 0x8000;
            g_xinput_in = in;
            int coin_now = (st.pad.bRT > 128) || !!(b&0x0040) /*L3*/;
            if (coin_now && !coin_was) InterlockedIncrement(&g_local_coin);
            coin_was = coin_now;
        }
        Sleep(8);
    }
    return 0;
}

/* The input the game should see: the loader's shared block when a loader
 * drives it, otherwise the local gamepad. */
void ul_re2_current_input(ultra_jvs_input_ringedge2* out) {
    memset(out, 0, sizeof *out);
    for (int a = 0; a < JVS_RINGEDGE2_MAX_ANALOG; ++a) out->analog[a] = 0x8000;
    out->screen_x = out->screen_y = 0x8000;
    ultra_io_shared* s = g_shared;
    if (s && s->magic == ULTRA_IO_MAGIC) {
        ultra_io_shared snap; int got = 0;
        for (int attempt = 0; attempt < 8; ++attempt) {
            uint32_t s1 = s->seq; MemoryBarrier(); snap = *s; MemoryBarrier();
            uint32_t s2 = s->seq; if (s1 == s2 && !(s1 & 1u)) { got = 1; break; }
        }
        if (!got) snap = *s;
        out->system = snap.system;
        for (int p = 0; p < ULTRA_IO_PLAYERS && p < JVS_RINGEDGE2_MAX_PLAYERS; ++p) out->player[p] = snap.player[p];
        for (int a = 0; a < ULTRA_IO_ANALOG && a < JVS_RINGEDGE2_MAX_ANALOG; ++a) out->analog[a] = snap.analog[a];
        out->screen_x = snap.screen_x; out->screen_y = snap.screen_y;
    } else {
        *out = g_xinput_in;                        /* standalone gamepad fallback */
    }
}

/* Pull input into the JVS node + inject coin edges (shared block + gamepad). */
void ul_re2_pull_input(ultra_jvs_node_ringedge2* n) {
    if (!n) return;
    ultra_jvs_input_ringedge2 in;
    ul_re2_current_input(&in);
    jvs_ringedge2_set_input(n, &in);
    ultra_io_shared* s = g_shared;
    if (s && s->magic == ULTRA_IO_MAGIC) {
        for (int c = 0; c < ULTRA_IO_COINS; ++c) {
            uint32_t pulse = s->coin_pulse[c];
            if (pulse != g_coin_seen[c]) {
                int delta = (int)(pulse - g_coin_seen[c]);
                jvs_ringedge2_insert_coin(n, c, delta);
                g_coin_seen[c] = pulse; s->coin_ack[c] = pulse;
            }
        }
        s->jvs_address = jvs_ringedge2_address(n); s->poll_count++;
    }
    long lc = g_local_coin;                        /* gamepad coin (standalone) */
    if (lc != g_local_coin_seen) {
        int delta = (int)(lc - g_local_coin_seen);
        jvs_ringedge2_insert_coin(n, 0, delta);
        g_local_coin_seen = lc;
    }
}

/* ---- Ultra Omni-Track light-gun pose source (ultra_bridge_pose_fn) ---------
 * Coherent snapshot of the loader's seqlocked aiming pose (screen_x/screen_y +
 * the player[0] trigger bit), handed to the Ultra Omni-Bridge so a real
 * light-gun drives the OS cursor on a RingEdge 2 title that reads the raw
 * mouse instead of JVS. Signature matches ultra_bridge_pose_fn exactly
 * (int*,int*,int*)->0/1, so it drops straight into ultra_omni_bridge_install().
 *
 * Two gates, both meaning "no pose right now" rather than "reuse the last one":
 *
 *  1. LOADER_ALIVE. The loader clears this flag when it exits (or when its gun
 *     channel goes inactive), and the mapped view keeps its LAST-WRITTEN bytes
 *     afterwards -- the shared block is not zeroed on the writer's death. If we
 *     ignored the flag we would keep reporting that stale pose forever: the
 *     bridge would go on warping the cursor to wherever the gun last pointed
 *     and, if the trigger bit happened to be set in that final frame, hold the
 *     mouse button down permanently. Returning 0 instead makes the bridge fall
 *     back to the gamepad/real mouse, which is the correct behavior for a
 *     loader that is no longer there. (magic alone can't tell us this -- it
 *     stays valid in the stale bytes.)
 *
 *  2. The seqlock. The loader writes the pose fields inside an odd/even seq
 *     window; a raw read can latch a torn pose (X from frame N, trigger from
 *     N+1). Retry a bounded number of times, and if we never catch an even,
 *     unchanged window, report "no pose" -- unlike ul_re2_current_input()
 *     above, which must always produce a JVS frame and so accepts a torn read
 *     as a last resort, an aiming pose is better skipped than misaimed.
 *
 * Trigger bit is player[0] bit9 = PUSH1 (see jvs_core_ringedge2.h), which is
 * what the loader sets for a gun trigger. */
int ul_re2_gun_pose(int* out_x, int* out_y, int* out_trig) {
    ultra_io_shared* s = g_shared;
    if (!s || s->magic != ULTRA_IO_MAGIC) return 0;
    if (!(s->flags & ULTRA_IO_FLAG_LOADER_ALIVE)) return 0;
    uint16_t sx = 0x8000, sy = 0x8000, p0 = 0; int got = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t s1 = s->seq; MemoryBarrier();
        sx = s->screen_x; sy = s->screen_y; p0 = s->player[0];
        MemoryBarrier();
        uint32_t s2 = s->seq;
        if (s1 == s2 && !(s1 & 1u)) { got = 1; break; }
    }
    if (!got) return 0;
    *out_x = sx; *out_y = sy;                  /* already 0..65535 screen-normalized */
    *out_trig = (p0 & 0x0200) != 0;            /* bit9 = PUSH1 = gun trigger */
    return 1;
}

/* -------------------------------------------------------------------------- */
static void open_shared(void) {
    const char* name = ul_re2_env(ULTRA_IO_ENVVAR);
    if (!name || !*name) return;
    g_map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!g_map) return;
    g_shared = (ultra_io_shared*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0,
                                               sizeof(ultra_io_shared));
    if (g_shared && g_shared->magic == ULTRA_IO_MAGIC) {
        g_shared->flags |= ULTRA_IO_FLAG_HOOK_ATTACHED;
        ul_re2_log("shared input block attached (%s)", name);
    }
}

static void configure_node(void) {
    g_node = jvs_ringedge2_create();
    if (!g_node) return;
    const char* id = ul_re2_env("ULTRA_JVS_ID");
    if (id && *id) jvs_ringedge2_set_id(g_node, id);
    int screenpos = ul_re2_env_on("ULTRA_JVS_SCREENPOS");
    uint8_t analog_ch = 0;   /* 0 = keep node default (8) */
    const char* ach = ul_re2_env("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= JVS_RINGEDGE2_MAX_ANALOG) analog_ch = (uint8_t)v; }
    /* defaults (2p/13sw/2coin/8analog@16/2gpo) + optional light-gun feature */
    jvs_ringedge2_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* Ultra Omni-Bridge window-getter accessor: this board is mixed D3D9/D3D11
 * (BP_HOOK_D3D9_AND_D3D11, see ringedge2_hook.h), so whichever present hook
 * actually got exercised by the running title is the one with a live device
 * window -- try D3D9 first, then D3D11, same "first create-fn wins" rule the
 * rest of this board's mixed-graphics handling already uses. */
static HWND ul_re2_bridge_get_window(void) {
    HWND w = ul_re2_d3d9_window();
    if (w) return w;
    return ul_re2_d3d11_window();
}

/* Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 * imports, render/audio plugins). The first install already happened
 * synchronously in DllMain; this only re-points IAT slots periodically. */
/* Has the set of loaded modules changed since we last looked?
 *
 * WHY THIS GATE EXISTS. ul_re2_jvs_serial_install() calls ul_re2_iat_patch_all
 * about 24 times (once per hooked name), and EACH of those takes its own
 * EnumProcessModules snapshot and walks every module in it. Re-installing blindly
 * once a second for 30 seconds was therefore ~720 full sweeps -- tens of thousands
 * of module-header reads -- during exactly the window in which D3D9/D3DX are
 * loading and unloading helper DLLs. That is what turned a narrow race into a
 * crash on 2026-09-18 (d3dcompiler_42.dll, unloaded mid-walk).
 *
 * mem_readable() above makes that walk survivable; this makes it rare. The sweep
 * only exists to catch DLLs the game loads AFTER us, so when nothing has loaded
 * or unloaded there is by definition nothing new to patch and the whole round is
 * pure waste -- waste that also perturbs startup timing, which is the one
 * variable we most want stable between runs.
 *
 * Cheap and deliberately approximate: count plus a sum of the bases. It cannot
 * miss a load (any new base changes both), and a false POSITIVE merely costs one
 * re-install, which is idempotent. */
static int modules_changed(void) {
    static DWORD    s_count;
    static uintptr_t s_sum;
    static int      s_have;
    HMODULE mods[512]; DWORD need = 0, count, i;
    uintptr_t sum = 0;

    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need))
        return 1;                      /* cannot tell -> assume yes, as before */
    count = need / sizeof(HMODULE);
    if (count > 512) count = 512;
    for (i = 0; i < count; ++i) sum += (uintptr_t)mods[i];

    if (s_have && count == s_count && sum == s_sum) return 0;
    s_count = count; s_sum = sum; s_have = 1;
    return 1;
}

static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        if (!modules_changed()) continue;     /* nothing loaded -> nothing to patch */
        ul_re2_jvs_serial_install();   /* idempotent (name + value based) */
    }
    return 0;
}

/* ---- crash diagnostics (VEH) ------------------------------------------- */
static LONG WINAPI ul_re2_crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "(unknown)";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
#ifdef _WIN64
        ul_re2_log("CRASH: ACCESS_VIOLATION at RIP=%p in %s", addr, modname);
#else
        ul_re2_log("CRASH: ACCESS_VIOLATION at EIP=%p in %s", addr, modname);
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

        DisableThreadLibraryCalls(inst);
        AddVectoredExceptionHandler(1, ul_re2_crash_handler);
        ul_re2_log("attach: ultra_sega_ringedge2_hook loaded pid=%lu (%d-bit)",
                  (unsigned long)GetCurrentProcessId(), (int)(sizeof(void*) * 8));
        /* Install SYNCHRONOUSLY: we're injected into the SUSPENDED game and it
         * resumes right after, so every hook must be armed before the game's
         * first thread runs (else its first CreateFile(COM)/Direct3DCreateN
         * call is missed). */
        open_shared();
        configure_node();
        /* FIRST. The game connects to the local daemon very early and blocks
         * its MAIN THREAD for 60s per attempt when nobody answers, so this has
         * to be listening before anything else is worth installing. */
        ul_re2_pcp_install();
        ul_re2_jvs_serial_install();     /* JVS-over-serial + D3D9/D3D11/DXGI seam */
        ul_re2_keychip_install();        /* mxparallel presence-only shim (internally always-armed) */
        ul_re2_d3d9_install();           /* D3D9 present hook (mixed-board: always armed) */
        ul_re2_d3d11_install();          /* D3D11/DXGI present hook (mixed-board: always armed) */
        ul_re2_audio_install();          /* DirectSound + XAudio2 diagnostic hook */
        /* Ultra Omni-Bridge (5th Omni service): opt-in (ULTRA_BRIDGE_ENABLE,
         * default OFF like the other four Omni services) SendInput keyboard/
         * mouse emulation from an XInput gamepad, for a RingEdge 2 title that
         * reads raw OS keyboard/mouse instead of JVS. Installed AFTER both
         * present hooks above so ul_re2_bridge_get_window() has a live device
         * window to read from once one shows up; install() itself is a no-op
         * until the game actually creates a device. The 2nd argument is the
         * Ultra Omni-Track pose source: ul_re2_gun_pose() reads the same
         * shared block's screen_x/y + trigger bit that the JVS engine serves
         * over READ_SCRPOS, so a real light-gun ALSO reaches a RingEdge 2
         * title that ignores JVS and reads the raw OS mouse instead. Passing
         * NULL here (as this board did before) left only the gamepad->cursor
         * path and no gun_thread, i.e. Omni-Track could not reach the board at
         * all. The JVS path is unchanged and still primary for titles that do
         * poll it -- the two are independent readers of the same fields. */
        ultra_omni_bridge_install(ul_re2_bridge_get_window, ul_re2_gun_pose);
        /* Shared pause/settings overlay -- Start+Select / LB+RB (pad) or
         * F1/Esc/P (keyboard) to open. Own self-contained poll thread. */
        ultra_pausemenu_install(ul_re2_bridge_get_window);
        CreateThread(NULL, 0, gamepad_thread, NULL, 0, NULL);  /* standalone fallback input */
        CreateThread(NULL, 0, worker, NULL, 0, NULL);          /* periodic re-patch only */
        /* LAST, on purpose. It counts what everything above installs, and its
         * own heartbeat thread must not be the first thread the game's
         * CreateThread hook ever sees. Every hook it installs is observe-only
         * and lands on IAT slots no other TU in this tree (or ultra_tracer.c)
         * owns -- see watchdog_ringedge2.c's IAT OWNERSHIP note. */
        /* Break the amCredit "drain an uninitialised queue forever"
         * deadlock before the game's first frame -- the loop that hangs
         * runs inside the per-frame credit poll, so it bites on frame 1. */
        ul_re2_amcredit_install();
        ul_re2_amcredit_coin_install();   /* opt-in virtual coin, see ULTRA_RE2_COIN_KEY */
        ul_re2_watchdog_install();
        ul_re2_log("attach: all subsystem installers fired");
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ul_re2_amcredit_shutdown();   /* stop the retry thread before we unload */
        ultra_omni_bridge_stop();
        ultra_pausemenu_shutdown();
        ul_re2_d3d11_teardown();   /* pairs the d3d11 limiter's timeBeginPeriod, if armed */
        if (g_shared) { UnmapViewOfFile(g_shared); g_shared = NULL; }
        if (g_map)    { CloseHandle(g_map); g_map = NULL; }
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader/operator can confirm the right DLL loaded. */
__declspec(dllexport) const char* ultra_sega_ringedge2_hook_id(void) {
    return "Ultra Loader Sega RingEdge 2 I/O hook (clean-room JVS, presence-only keychip) v1";
}

#endif /* _WIN32 */
