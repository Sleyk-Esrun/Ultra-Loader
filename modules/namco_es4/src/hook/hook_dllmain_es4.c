/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_es4.c  --  ultra_namco_es4_hook{32,64}.dll entry point.
 * =============================================================================
 *
 *  THIS BOARD'S OWN FULLY INDEPENDENT, SELF-CONTAINED HOOK. Per the project
 *  directive ("every board MUST have its OWN fully independent, self-contained
 *  hook DLL -- NO shared runtime binary with any other board, NO dependency on
 *  native_x86/src/hook/'s compiled output"), this file and its siblings in
 *  this folder (jvs_core_es4.*, ultra_io_shared_es4.h, namco_io_es4.c,
 *  dinput8_emu_es4.c, d3d11_throttle_es4.c, audio_hook_es4.c) are compiled
 *  ONLY into ultra_namco_es4_hook{32,64}.dll and never link against, load, or
 *  otherwise depend on modules/native_x86/src/hook/'s compiled output or any
 *  other board's module folder. The logic (IAT patcher, node lifecycle,
 *  standalone-gamepad fallback, install sequence) was read from
 *  modules/native_x86/src/hook/{jvs_serial_hook.c,hook_dllmain.c} as REFERENCE
 *  ONLY and re-implemented here as this board's own compiled copy.
 *
 *  Injected into the Namco System ES4 game by namco_es4_main.c (this board's
 *  own module launcher). On attach it:
 *    - opens the shared-memory input block the loader may have created
 *      (ULTRA_IO_SHM), else falls back to a local XInput gamepad so a
 *      standalone run is still playable,
 *    - creates one clean-room JVS node (jvs_core_es4.c),
 *    - installs the JVS-over-USB + amauthd-presence transport
 *      (namco_io_es4.c), the DirectInput8 emulation (dinput8_emu_es4.c), the
 *      D3D11/DXGI present throttle+capture (d3d11_throttle_es4.c), and the
 *      DirectSound/XAudio2 diagnostic hook (audio_hook_es4.c),
 *    - re-applies the IAT patches for a short window so DLLs the game loads
 *      late (delay-loaded imports, plugins) are caught too.
 *
 *  No game code, no third-party loader code -- Ultra Loader's own work over
 *  the public Win32 API + the open JVS spec.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "jvs_core_es4.h"
#include "ultra_io_shared_es4.h"
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
/* from the other translation units in this board's own tree */
/* controls.c. native_x86's hook_dllmain.c declares this; the declaration was
 * not carried over when this board was derived, so the call at the
 * coin-ownership test below was an implicit declaration and namco_es4 would
 * not build at all. Pre-existing - one of the boards already known broken. */
/* controls.c's input-mapping layer is NOT carried by this board.
 *
 * native_x86 defines ultra_input_map_active() in controls.c; namco_es4 was
 * derived without controls.c, input_profiles.c or any input_src_* file - 8
 * hook files against native_x86's 47 - yet still CALLS this function at the
 * coin-ownership test below. That is why the board did not build: implicit
 * declaration, then an undefined reference at link. Pre-existing, not caused
 * by the tracer rollout.
 *
 * Answering 'no map on this board' is correct for what the board actually
 * carries: with no profile layer present, the legacy path must own the coin
 * input, which is exactly what a 0 here selects.
 *
 * This is the same stripped-derivation defect that left konami_pc's games
 * with no controls at all. namco_es4 has no game in the dump collection to
 * verify against, so it is deliberately NOT given a speculative input stack
 * here - when a Namco ES4 title arrives, derive the input files the way
 * konami_pc was, and delete this. */
static int ultra_input_map_active(void) { return 0; }
void ultra_namco_io_install(void);        /* namco_io_es4.c  -- JVS-over-USB + amauthd (env-gated) */
void ultra_dinput8_install(void);         /* dinput8_emu_es4.c -- clean-room DirectInput8 emulation */
void ultra_d3d11_install(void);           /* d3d11_throttle_es4.c -- DXGI present throttle+capture */
void ultra_d3d11_teardown(void);          /* pair the d3d11 limiter's timeBeginPeriod */
HWND ultra_es4_get_window(void);          /* d3d11_throttle_es4.c -- device window (Omni-Bridge) */
void ultra_audio_install(void);           /* audio_hook_es4.c -- DirectSound + XAudio2 diagnostic hook */
/* omni_bridge.c -- Ultra Omni-Bridge (5th Omni service): keyboard/mouse
 * emulation for raw-input-only games. This board DOES have a live-pose
 * source: the loader's Ultra Omni-Track engine lands a calibrated lightgun
 * pose in ultra_io_shared::screen_x/screen_y (0..65535) with the trigger in
 * player[0], and ULTRA_IO_FLAG_LOADER_ALIVE (ultra_io_shared_es4.h) is the
 * liveness signal that says the pose is current rather than stale. So
 * get_pose is namco_es4_gun_pose() below, not NULL -- gun path + gamepad
 * path, both opt-in via ULTRA_BRIDGE_ENABLE (default off, matching the other
 * 4 Omni services). */

/* =============================================================================
 *  Shared trace sink -- kept as `ultra_d3d8_log` for consistency with the
 *  codebase-wide convention (every per-board hook tree in this project uses
 *  this exact name for its own local diagnostic log sink, gated by
 *  ULTRA_JVS_LOG), even though this board has no D3D8 renderer at all (it is
 *  D3D11-only). This is THIS board's own local definition -- it is not shared
 *  with, and does not call into, any other board's copy of the same name.
 * ============================================================================= */
void ultra_d3d8_log(const char* fmt, ...) {
    const char* p = getenv("ULTRA_JVS_LOG"); if (!p || !*p) return;
    FILE* f = fopen(p, "a"); if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

/* =============================================================================
 *  IAT patching across all loaded modules -- this board's OWN copy of the
 *  clean-room PE-import-table patcher (ported from
 *  modules/native_x86/src/hook/jvs_serial_hook.c, read as REFERENCE ONLY; pure
 *  public-PE-format code, not game- or board-specific logic).
 * ============================================================================= */
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
        /* Bound EACH thunk read against the mapped image: a malformed/packed import
         * whose IAT array is not NULL-terminated within the image would otherwise
         * walk off the end and fault (no SEH guard under mingw). Stop if the next
         * iat (or orig, when present) would read past isize. */
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
    if (imp) realfn = (void*)GetProcAddress(imp, impfn);   /* for the value-based pass */

    HMODULE mods[512]; DWORD need = 0;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ultra_iat_patch_all, &self);
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        /* `need` is the size required for ALL modules; if it exceeds our fixed
         * buffer only the first 512 entries were actually written, so clamp the
         * iteration to the buffer capacity. */
        DWORD count = need / sizeof(HMODULE);
        if (count > 512) count = 512;
        for (DWORD i = 0; i < count; ++i)
            if (mods[i] != self)                 /* don't patch our own imports */
                iat_patch_module(mods[i], impdll, impfn, newfn, realfn);
    }
}

/* =============================================================================
 *  JVS node lifecycle + the loader<->game input bridge
 * ============================================================================= */
static ultra_jvs_node*    g_node   = NULL;
static HANDLE             g_map    = NULL;
static ultra_io_shared*   g_shared = NULL;
static uint32_t           g_coin_seen[ULTRA_IO_COINS] = {0};
static volatile LONG      g_stop   = 0;

ultra_jvs_node* ultra_hook_node(void) {
    return g_node;   /* created before any hook can fire (DllMain is synchronous) */
}

/* Raw shared input block (or NULL), for the DirectInput8 emulation to read. */
ultra_io_shared* ultra_hook_shared(void) { return g_shared; }

/* ---- Ultra Omni-Track lightgun pose source (Omni-Bridge get_pose) ---------
 * Signature is EXACTLY ultra_bridge_pose_fn (omni_bridge.h:30), so it drops
 * straight into ultra_omni_bridge_install()'s 2nd argument. Returns 1 with a
 * coherent, LIVE aiming pose (x/y 0..65535 screen-normalized, trigger 0/1);
 * 0 means "no pose right now" and the bridge falls back to its gamepad path
 * (or stays fully inert), leaving a real mouse alone.
 *
 * Why the LOADER_ALIVE gate and not just the magic word: the shared block
 * outlives the loader. If the loader process exits -- or its Omni-Track gun
 * goes inactive -- the mapping keeps its magic and its LAST written pose
 * forever, so a magic-only check would keep reporting that frozen pose as if
 * it were live. The bridge would then warp the cursor to wherever the gun was
 * last pointing, ~120x/sec, and re-assert a trigger bit that was set at the
 * moment the loader died -- a stuck, phantom gun. The loader clears
 * ULTRA_IO_FLAG_LOADER_ALIVE on the way out, so gating on it makes a dead
 * loader mean "no pose" (hands off) rather than "last known pose".
 *
 * Deliberately NOT built on ultra_hook_current_input(): that function is the
 * JVS path and is intentionally lenient -- it checks only magic and, when the
 * seqlock retries are exhausted, falls through to a TORN read. Torn is fine
 * for a JVS switch poll; for an aim point it means X from one frame and the
 * trigger from another, i.e. a shot fired at the wrong place. Here an
 * exhausted retry is a hard `return 0`. */
int namco_es4_gun_pose(int* out_x, int* out_y, int* out_trig) {
    ultra_io_shared* s = g_shared;
    if (!s || s->magic != ULTRA_IO_MAGIC) return 0;
    if (!(s->flags & ULTRA_IO_FLAG_LOADER_ALIVE)) return 0;   /* see banner: dead loader == no pose */

    uint16_t sx = 0x8000, sy = 0x8000, p0 = 0; int got = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t s1 = s->seq; MemoryBarrier();
        sx = s->screen_x; sy = s->screen_y; p0 = s->player[0];
        MemoryBarrier();
        uint32_t s2 = s->seq;
        if (s1 == s2 && !(s1 & 1u)) { got = 1; break; }       /* even + unchanged == coherent */
    }
    if (!got) return 0;                                       /* never hand back a torn aim point */

    *out_x = sx; *out_y = sy;
    *out_trig = (p0 & 0x0200) != 0;   /* player[0] bit9 = PUSH1 (jvs_core_es4.h) = gun trigger */
    return 1;
}

/* ---- standalone gamepad (XInput) ------------------------------------------
 * When no loader is feeding the shared block (e.g. a standalone test launch,
 * or this board has not yet been wired to create ULTRA_IO_SHM), the hook reads
 * the player's gamepad itself so the game is actually playable. XInput is
 * loaded dynamically (no hard dependency on a given version). No per-game
 * autopilot scripts are included here -- this board has no dump / no known
 * game quirks yet (framework-only status), so only the generic pad->JVS
 * mapping is provided. */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } ul_xpad;
typedef struct { DWORD packet; ul_xpad pad; } ul_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, ul_xstate*);
static p_xinput_get   s_xget = NULL;
static ultra_jvs_input g_xinput_in;          /* updated by the XInput thread */
static volatile LONG   g_local_coin = 0;     /* coin pulses from the gamepad */
static long            g_local_coin_seen = 0;

static DWORD WINAPI xinput_thread(LPVOID arg) {
    (void)arg;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
    for (int i = 0; dlls[i] && !s_xget; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) s_xget = (p_xinput_get)(void*)GetProcAddress(h, "XInputGetState");
    }
    for (int a = 0; a < JVS_MAX_ANALOG; ++a) g_xinput_in.analog[a] = 0x8000;
    g_xinput_in.screen_x = g_xinput_in.screen_y = 0x8000;
    if (!s_xget) return 0;
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
            ultra_jvs_input in; memset(&in, 0, sizeof in);
            in.player[0] = (uint16_t)((byte1<<8)|byte2);
            in.system    = test ? 0x80 : 0;        /* TEST switch */
            in.analog[0] = (uint16_t)((int)st.pad.lx + 0x8000);
            in.analog[1] = (uint16_t)(0x7FFF - (int)st.pad.ly);
            for (int a = 2; a < JVS_MAX_ANALOG; ++a) in.analog[a] = 0x8000;
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
            int legacy_owns_coin = !ultra_input_map_active() &&
                                   !(g_shared && g_shared->magic == ULTRA_IO_MAGIC);
            int coin_now = legacy_owns_coin && !!(b & 0x0020) /* BACK/SELECT */;
            if (coin_now && !coin_was) InterlockedIncrement(&g_local_coin);
            coin_was = coin_now;
        }
        Sleep(8);
    }
    return 0;
}
static void ultra_xinput_start(void) { CreateThread(NULL, 0, xinput_thread, NULL, 0, NULL); }

/* The input the game should see: the loader's shared block when a loader
 * drives it, otherwise the local gamepad (XInput). Used by the JVS engine AND
 * the DirectInput8 emulation so both report the same controller state. */
void ultra_hook_current_input(ultra_jvs_input* out) {
    memset(out, 0, sizeof *out);
    for (int a = 0; a < JVS_MAX_ANALOG; ++a) out->analog[a] = 0x8000;
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
        for (int p = 0; p < ULTRA_IO_PLAYERS && p < JVS_MAX_PLAYERS; ++p) out->player[p] = snap.player[p];
        for (int a = 0; a < ULTRA_IO_ANALOG && a < JVS_MAX_ANALOG; ++a) out->analog[a] = snap.analog[a];
        out->screen_x = snap.screen_x; out->screen_y = snap.screen_y;
    } else {
        *out = g_xinput_in;                        /* standalone gamepad */
    }
}

/* Pull input into the JVS node + inject coin edges (shared block + gamepad). */
void ultra_hook_pull_input(ultra_jvs_node* n) {
    if (!n) return;
    ultra_jvs_input in;
    ultra_hook_current_input(&in);
    jvs_set_input(n, &in);
    ultra_io_shared* s = g_shared;
    if (s && s->magic == ULTRA_IO_MAGIC) {
        for (int c = 0; c < ULTRA_IO_COINS; ++c) {
            uint32_t pulse = s->coin_pulse[c];
            if (pulse != g_coin_seen[c]) {
                int delta = (int)(pulse - g_coin_seen[c]);
                jvs_insert_coin(n, c, delta);
                g_coin_seen[c] = pulse; s->coin_ack[c] = pulse;
            }
        }
        s->jvs_address = jvs_address(n); s->poll_count++;
    }
    long lc = g_local_coin;                        /* gamepad coin (standalone) */
    if (lc != g_local_coin_seen) {
        int delta = (int)(lc - g_local_coin_seen);
        jvs_insert_coin(n, 0, delta);
        g_local_coin_seen = lc;
    }
}

/* -------------------------------------------------------------------------- */
static void open_shared(void) {
    const char* name = getenv(ULTRA_IO_ENVVAR);
    if (!name || !*name) return;
    g_map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!g_map) return;
    g_shared = (ultra_io_shared*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0,
                                               sizeof(ultra_io_shared));
    if (g_shared && g_shared->magic == ULTRA_IO_MAGIC)
        g_shared->flags |= ULTRA_IO_FLAG_HOOK_ATTACHED;
}

static void configure_node(void) {
    g_node = jvs_create();
    if (!g_node) return;
    const char* id = getenv("ULTRA_JVS_ID");
    if (id && *id) jvs_set_id(g_node, id);
    int screenpos = 0;
    const char* sp = getenv("ULTRA_JVS_SCREENPOS");
    if (sp) screenpos = (sp[0] == '1');
    uint8_t analog_ch = 0;   /* 0 = keep node default (8) */
    const char* ach = getenv("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= JVS_MAX_ANALOG) analog_ch = (uint8_t)v; }
    /* defaults (2p/13sw/2coin/8analog@16/2gpo) + optional light-gun feature */
    jvs_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* set an env var only if the user/launcher hasn't already set it -- mirrors
 * namco_es4_main.c's own set_if_unset pattern for shared frontend knobs. */
static void set_if_unset(const char* name, const char* value) {
    if (!GetEnvironmentVariableA(name, NULL, 0))
        SetEnvironmentVariableA(name, value);
}

/* Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 * imports, render/audio plugins). The first install already happened
 * synchronously in DllMain, so this only re-arms idempotent installers
 * periodically for a short window after attach. */
static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        ultra_dinput8_install();      /* idempotent */
        ultra_namco_io_install();     /* idempotent */
    }
    return 0;
}

/* ---- crash diagnostics (VEH) -------------------------------------------- */
static LONG WINAPI ultra_crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "(unknown)";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
#ifdef _WIN64
        ultra_d3d8_log("CRASH(ES4): ACCESS_VIOLATION at RIP=%p in %s", addr, modname);
        ultra_d3d8_log("CRASH(ES4): RAX=%p RBX=%p RCX=%p RDX=%p",
            (void*)ep->ContextRecord->Rax, (void*)ep->ContextRecord->Rbx,
            (void*)ep->ContextRecord->Rcx, (void*)ep->ContextRecord->Rdx);
#else
        ultra_d3d8_log("CRASH(ES4): ACCESS_VIOLATION at EIP=%p in %s", addr, modname);
        ultra_d3d8_log("CRASH(ES4): EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX",
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
            utxp.board        = "namco_es4";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        AddVectoredExceptionHandler(1, ultra_crash_handler);
        /* Install SYNCHRONOUSLY here: the module injects us into the SUSPENDED
         * game and resumes right after, so the hooks must be in place before
         * the game's first thread runs. */
        open_shared();
        configure_node();

        /* This board's default I/O + present transports. Per-board sane
         * defaults (set-if-unset, so a user/profile override always wins,
         * matching namco_es4_main.c's own set_if_unset convention for the
         * shared frontend knobs):
         *   - ULTRA_NAMCO_IO=1   : arm the JVS-over-USB + amauthd-presence
         *     transport. The VID/PID device match stays inert until
         *     ULTRA_NAMCO_IO_VIDPID is pinned against a real ES4 dump, so
         *     arming this alone changes nothing observable yet.
         *   - ULTRA_D3D11=1      : arm the DXGI present throttle/capture
         *     (ES4 renders through D3D11). */
        set_if_unset("ULTRA_NAMCO_IO", "1");
        set_if_unset("ULTRA_D3D11", "1");

        ultra_namco_io_install();                        /* JVS-over-USB + amauthd presence (env-gated) */
        ultra_dinput8_install();                         /* synthetic DInput8 device */
        ultra_d3d11_install();                           /* DXGI present throttle + capture */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        /* Omni-Bridge (opt-in, ULTRA_BRIDGE_ENABLE). 2nd arg = Ultra Omni-Track
         * lightgun pose source (namco_es4_gun_pose, defined above); NULL here
         * would disable the gun path and leave only gamepad->cursor. */
        ultra_omni_bridge_install(ultra_es4_get_window, namco_es4_gun_pose);
ultra_pausemenu_install(ultra_es4_get_window);
        ultra_audio_install();                           /* DirectSound + XAudio2 diagnostic hook */
        ultra_xinput_start();                            /* standalone gamepad -> JVS/DInput fallback */
        CreateThread(NULL, 0, worker, NULL, 0, NULL);     /* periodic re-patch only */
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();  /* stop poll thread, force-release any held mouse buttons */
ultra_pausemenu_shutdown();
        ultra_d3d11_teardown();  /* pair the d3d11 limiter's timeBeginPeriod, if armed */
        if (g_shared) { UnmapViewOfFile(g_shared); g_shared = NULL; }
        if (g_map)    { CloseHandle(g_map); g_map = NULL; }
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader/module can confirm the right DLL was
 * injected (mirrors every other per-board hook's *_hook_id() export). */
__declspec(dllexport) const char* ultra_namco_es4_hook_id(void) {
    return "Ultra Loader Namco System ES4 I/O hook (clean-room JVS-over-USB, D3D11) v1";
}

#endif /* _WIN32 */
