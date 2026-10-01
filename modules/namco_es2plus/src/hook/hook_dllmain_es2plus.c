/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_es2plus.c  --  ultra_namco_es2plus_hook{32,64}.dll entry point.
 *                              Owns the JVS engine + the loader<->game input
 *                              bridge, and installs every subsystem hook.
 * =============================================================================
 *
 *  This is namco_es2plus's own, fully self-contained hook DLL -- it is injected
 *  into the game process by namco_es2plus_main.c (this board's spawner) via a
 *  suspended-launch + QueueUserAPC(LoadLibraryA) (primary) + a fallback
 *  CreateRemoteThread(LoadLibraryA). On attach it:
 *
 *    - opens the (optional) shared-memory input block a future loader
 *      integration may create (ULTRA_IO_SHM); when absent, falls back to a
 *      standalone XInput gamepad so the board is playable stand-alone,
 *    - creates one clean-room JVS node (jvs_core_es2plus.c),
 *    - installs the JVS-over-USB + amauthd LOCAL-presence transport
 *      (namco_io_es2plus.c) -- the ES2+ board has no confirmed COM-JVS path,
 *      so unlike Type X this hook does not install a serial transport,
 *    - installs the D3D9 present/frame-limiter hook (d3d9_throttle_es2plus.c),
 *    - installs the clean-room DirectInput8 emulation (dinput8_emu_es2plus.c),
 *    - installs the DirectSound/XAudio2 diagnostic hook (audio_hook_es2plus.c).
 *
 *  INDEPENDENCE: this file, and every other file in this src/hook/ tree, is
 *  namco_es2plus's OWN independent copy. It does not include, call, or link
 *  against anything compiled from modules/native_x86/src/hook/ or any other
 *  board's module folder. Reference read-only from
 *  modules/native_x86/src/hook/hook_dllmain.c; logic reproduced (adapted to
 *  this board's own JVS-over-USB transport and trimmed of Type-X-specific
 *  per-game autopilot scripts, which do not apply to this board), not shared
 *  at build or link time.
 *
 *  ETHICAL BOUNDARY (namco_io_es2plus.c): the amauthd/amdaemon LOCAL presence
 *  answer surface answers ONLY presence/status/version/region/play-count.
 *  Every online-auth/billing/account/key/content-decryption class is
 *  DEFERRED (returns 0), exactly like ES1's TPM/LUKS+HASP presence-only wall.
 *  This hook NEVER implements real key unwrap/decrypt.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "jvs_core_es2plus.h"
#include "ultra_io_shared_es2plus.h"
#include "ultra_transport.h"   /* shared universal I/O transport layer */
#include "omni_bridge.h"                 /* Ultra Omni-Bridge (5th Omni service) -- own copy */

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

/* ---------------------------------------------------------------------------
 *  ultra_input_map_active -- this board has no controls-profile layer.
 *
 *  The coin guard below asks whether a profile owns the coin line. This board
 *  never gained the controls/ subsystem, so the honest answer is "no": there is
 *  no profile, ownership falls through to the shared block, which is precisely
 *  what the expression is written to do. Boards that DO carry the layer get the
 *  real implementation from their own controls.c instead of this.
 * ------------------------------------------------------------------------- */
static int ultra_input_map_active(void) { return 0; }


/* from the other TUs in this board's own tree */
void es2p_namco_io_install(void);        /* JVS-over-USB + amauthd presence (namco_io_es2plus.c) */
void es2p_dinput8_install(void);         /* clean-room DirectInput8 emulation */
void es2p_audio_install(void);           /* DirectSound + XAudio2 diagnostic hook */
void* __stdcall es2p_Hook_Direct3DCreate9(UINT ver);   /* d3d9_throttle_es2plus.c */
void  es2p_d3d9_teardown(void);          /* pair timeBeginPeriod with timeEndPeriod (below) */
HWND  es2p_d3d9_get_window(void);        /* d3d9_throttle_es2plus.c -- current device window */

/* -------------------------------------------------------------------------- */
/*  Diagnostic sink -- stderr (inherited by namco_es2plus_main.c's CreateProcess
 *  with bInheritHandles=TRUE, so it flows straight into ultra.log, exactly like
 *  the board spawner's own LOG() macro). */
void es2p_log(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    fprintf(stderr, "[namco_es2plus_hook] %s", buf);
    if (!buf[0] || buf[strlen(buf) - 1] != '\n') fputc('\n', stderr);
    fflush(stderr);
}

/* -------------------------------------------------------------------------- */
/*  Generic RVA-bounded IAT walker -- this board's own copy (proven shape,
 *  matches every other per-board hook in this project). All header/thunk reads
 *  are bounds-checked against the module's mapped image size (mingw has no SEH
 *  __try/__except), so a malformed import directory is skipped instead of
 *  crashing the host game. Patches by name AND by value (the value pass
 *  catches bound imports whose names were stripped, and is naturally
 *  idempotent on re-patch). */
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

void es2p_iat_patch_all(const char* impdll, const char* impfn, void* newfn) {
    void* realfn = NULL;
    HMODULE imp = GetModuleHandleA(impdll);
    if (imp) realfn = (void*)GetProcAddress(imp, impfn);   /* for the value-based pass */

    HMODULE mods[512]; DWORD need = 0;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&es2p_iat_patch_all, &self);
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        DWORD count = need / sizeof(HMODULE);
        if (count > 512) count = 512;
        for (DWORD i = 0; i < count; ++i)
            if (mods[i] != self)                 /* don't patch our own imports */
                iat_patch_module(mods[i], impdll, impfn, newfn, realfn);
    }
}

/* -------------------------------------------------------------------------- */
static ultra_jvs_node*    g_node   = NULL;
static HANDLE             g_map    = NULL;
static ultra_io_shared*   g_shared = NULL;
static uint32_t           g_coin_seen[ULTRA_IO_COINS] = {0};
static volatile LONG      g_stop   = 0;

ultra_jvs_node* es2p_hook_node(void) { return g_node; }
ultra_io_shared* es2p_hook_shared(void) { return g_shared; }

/* ---- standalone gamepad (XInput) ------------------------------------------
 * When no loader is feeding the shared block, the hook reads the player's
 * gamepad itself so a standalone run is actually playable/testable. XInput is
 * loaded dynamically (no hard dependency on a given version). No per-game
 * autopilot scripting here -- ES2+ has no confirmed game dump to script
 * against; this is deliberately generic. */
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
static void es2p_xinput_start(void) { CreateThread(NULL, 0, xinput_thread, NULL, 0, NULL); }

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
void es2p_hook_pull_input(ultra_jvs_node* n) {
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
        /* Credit the first chute (slot 0). The game READ_COIN reads slot 0 and its
         * COIN_DECREASE slot=1 maps to coin[0] via the 1-based->0-based fix in
         * jvs_core_es2plus.c, so a credit inserted here is the one the game spends. */
        int delta = (int)(lc - g_local_coin_seen);
        jvs_insert_coin(n, 0, delta);
        g_local_coin_seen = lc;
    }
}

/* ---- Ultra Omni-Bridge pose adapter ----------------------------------------
 * ES2+'s own shared-memory block (ultra_io_shared, opened above in
 * open_shared()) already carries a light-gun pose in screen_x/screen_y for the
 * normal JVS path (jvs_core_es2plus.c answers READ_SCRPOS from it) -- this
 * adapter exposes that SAME coherent pose to Omni-Bridge as its
 * ultra_bridge_pose_fn, for the (currently unconfirmed) case of an ES2+ title
 * that reads the raw OS cursor instead of/in addition to JVS screen position.
 * Mirrors taito_typex2's own gun_pose_snapshot(): a seqlocked read gated on
 * ULTRA_IO_FLAG_LOADER_ALIVE so a loader that exited (or whose gun went
 * inactive) reports "no live pose" rather than replaying a stale snapshot.
 * Trigger = player[0] PUSH1 (bit9/0x0200), the same bit Type X2 uses. */
static int es2p_bridge_get_pose(int *x, int *y, int *trigger) {
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
    *x = sx; *y = sy; *trigger = (p0 & 0x0200) != 0;
    return 1;
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
    /* Advertised analog-channel count. Default 8; bump via ULTRA_JVS_ANALOG if a
     * real ES2+ dump's driving-style cabinet polls more. */
    uint8_t analog_ch = 0;   /* 0 = keep node default (8) */
    const char* ach = getenv("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= JVS_MAX_ANALOG) analog_ch = (uint8_t)v; }
    jvs_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* ---- crash diagnostics (VEH) ----------------------------------------------- */
static LONG WINAPI es2p_crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "(unknown)";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
#ifdef _WIN64
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
        es2p_log("CRASH: ACCESS_VIOLATION at RIP=%p in %s\n", addr, modname);
#else
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
        es2p_log("CRASH: ACCESS_VIOLATION at EIP=%p in %s\n", addr, modname);
#endif
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 * imports, render/audio plugins). The first install already happened
 * synchronously in DllMain; this only re-points IAT slots periodically. */
static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        es2p_dinput8_install();      /* idempotent */
        es2p_iat_patch_all("d3d9.dll", "Direct3DCreate9", (void*)es2p_Hook_Direct3DCreate9);
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
            utxp.board        = "namco_es2plus";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        AddVectoredExceptionHandler(1, es2p_crash_handler);
        es2p_log("attach: ultra_namco_es2plus_hook loaded pid=%lu (%d-bit)\n",
                 GetCurrentProcessId(), (int)(sizeof(void*) * 8));

        /* Install SYNCHRONOUSLY here: the module injects us into the SUSPENDED
         * game and resumes right after, so the hooks must be in place before
         * the game's first thread runs. */
        open_shared();
        configure_node();

        es2p_namco_io_install();     /* JVS-over-USB + amauthd LOCAL presence */
        es2p_dinput8_install();      /* synthetic DirectInput8 device */
        es2p_audio_install();        /* DirectSound + XAudio2 diagnostic hook */
        /* D3D9 present hook: IAT-patch the static import, AND catch a packed
         * title's dynamic GetProcAddress("Direct3DCreate9") lookup via the
         * kernel32 GetProcAddress hook below. */
        es2p_iat_patch_all("d3d9.dll", "Direct3DCreate9", (void*)es2p_Hook_Direct3DCreate9);

        es2p_xinput_start();         /* standalone gamepad -> JVS/DInput fallback */
        CreateThread(NULL, 0, worker, NULL, 0, NULL);   /* periodic re-patch only */

        /* Ultra Omni-Bridge (5th Omni service): keyboard/mouse-emulation
         * bridge for any ES2+ title found (in the future) to read raw OS
         * input instead of JVS/DirectInput. Installed AFTER the D3D9 present
         * hook above so its window-getter (es2p_d3d9_get_window) has a real
         * CreateDevice hook armed to source HWND from. Off by default
         * (ULTRA_BRIDGE_ENABLE=0, see omni_client.h) -- this is a framework
         * rollout, not a default-behavior change; opt in per game once a
         * real ES2+ dump is confirmed to need it. */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(es2p_d3d9_get_window, es2p_bridge_get_pose);
ultra_pausemenu_install(es2p_d3d9_get_window);

        es2p_log("attach: all subsystems armed (JVS-over-USB, D3D9 throttle, "
                  "DirectInput8 emu, DirectSound/XAudio2 diagnostic, Omni-Bridge)\n");
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();
ultra_pausemenu_shutdown();
        es2p_d3d9_teardown();
        if (g_shared) { UnmapViewOfFile(g_shared); g_shared = NULL; }
        if (g_map)    { CloseHandle(g_map); g_map = NULL; }
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader can confirm the right DLL was injected. */
__declspec(dllexport) const char* ultra_namco_es2plus_hook_id(void) {
    return "Ultra Loader Namco System ES2+ I/O hook (clean-room JVS-over-USB, "
           "board-native, self-contained) v1";
}

/* d3d9_throttle_es2plus.c raises the multimedia timer period (timeBeginPeriod)
 * the first time its frame limiter runs; pair it with timeEndPeriod on detach.
 * Kept here (not in d3d9_throttle_es2plus.c) so it's a single, obviously-paired
 * call site regardless of which subsystem armed the timer. */
void es2p_d3d9_teardown(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    timeEndPeriod(1);
}

#endif /* _WIN32 */
