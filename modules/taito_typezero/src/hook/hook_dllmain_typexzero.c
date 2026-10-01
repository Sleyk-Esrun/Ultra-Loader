/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_typexzero.c -- ultra_taito_typezero_hook32/64.dll entry point
 * =============================================================================
 *
 *  Injected into a Taito Type Zero game by this board's own
 *  modules/taito_typezero/src/taito_typezero_main.c. Fully independent of the
 *  shared multi-board hook (modules/native_x86/src/hook/) and of every other
 *  board's module folder -- no shared source file, no shared compiled binary
 *  -- mirroring the fork already done for Cave PC
 *  (modules/cave_pc/src/hook/cave_hook_dllmain.c).
 *
 *  On attach it installs, synchronously (the module injects us into the
 *  SUSPENDED game and resumes right after, so hooks must be in place before
 *  the game's first thread runs):
 *    - the JVS-over-serial COM transport (jvs_serial_hook_typexzero.c), fed
 *      by this board's own clean-room JVS engine (jvs_core_typexzero.c)
 *    - the D3D9 present hook: frame limiter + force-windowed + optional
 *      upscale/rotate/sharpen/dump (d3d9_throttle_typexzero.c)
 *    - the DirectSound/XAudio2 diagnostic hook (audio_hook_typexzero.c)
 *  then starts a short re-patch worker to catch any module the game loads
 *  fractionally after our first pass (delay-loaded imports), and a local
 *  XInput gamepad thread that feeds the JVS node directly (this independent
 *  build does not wire the loader's shared-memory IPC block -- same
 *  deliberate choice cave_pc's fork made; a standalone/manual gamepad input
 *  path is what actually gets exercised while this board is dump-gated).
 *
 *  Also installs Ultra Omni-Bridge (omni_bridge.c/.h, this board's own copy
 *  of the modules/native_x86/src/hook/ reference, per the project's per-board
 *  independence rule) -- opt-in via ULTRA_BRIDGE_ENABLE (default OFF, same as
 *  the other four Omni services), it synthesizes SendInput cursor/click/key
 *  events from an XInput gamepad for a Type Zero title proven to read raw OS
 *  keyboard/mouse instead of JVS. No lightgun pose source is wired (this
 *  board has no BP_IO_LIGHTGUN / gun-bridge file), so it is gamepad-only.
 *
 *  Deliberately NOT implemented in this pass (board_profile.c lists these for
 *  taito_typexzero but they are out of scope for the JVS engine / JVS-serial
 *  transport / D3D9 hook / audio hook / DllMain rebuild requested this
 *  session -- add later against a real dump):
 *    - Taito Fast-I/O (iDmacDrv) DMAC shim (BP_IO_FAST_IO)
 *    - Touch input / NESiCA card-reader stub (BP_IO_TOUCH, BP_IO_CARD_READER)
 *    - Any per-game autopilot/credit-loop script (none extracted yet --
 *      status is SCAFFOLDED, no game booted, per docs/STOCK_TAKE.md)
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 API and the open
 *  JVS standard. No third-party loader or emulator source referenced.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "jvs_core_typexzero.h"
#include "omni_bridge.h"

#include "ultra_transport.h"   /* shared universal I/O transport layer */
#include "ultra_tracer.h"
/* from the other TUs in this DLL */
int  ttz_jvs_serial_install(void);
void ttz_audio_install(void);
void ttz_d3d9_teardown(void);
extern void ttz_log(const char* fmt, ...);
extern HWND ttz_d3d9_device_window(void);   /* d3d9_throttle_typexzero.c */

/* -------------------------------------------------------------------------- */
static ttz_jvs_node*   g_node = NULL;
static volatile LONG    g_stop = 0;

ttz_jvs_node* ttz_hook_node(void) {
    return g_node;   /* created before any hook can fire */
}

/* ---- standalone gamepad (XInput) -> JVS ------------------------------------
 * This independent build has no loader shared-memory IPC wired (see header
 * comment), so the hook reads the player's gamepad itself -- same rationale
 * as the shared hook's own standalone fallback path, just without the
 * shared-memory branch on top of it. XInput is loaded dynamically (no hard
 * dependency on a given version). */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } ttz_xpad;
typedef struct { DWORD packet; ttz_xpad pad; } ttz_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, ttz_xstate*);
static p_xinput_get    s_xget = NULL;
static ttz_jvs_input   g_xinput_in;
static volatile LONG   g_local_coin = 0;
static long            g_local_coin_seen = 0;

/* This board has NO controls-profile system - there is no controls_*.c or
 * input_profiles_*.c here at all. The coin-gate below was pasted in from a
 * board that has one, so this call never even linked and the board has not
 * built since. No profile can be active, so the honest answer is 0 and the
 * legacy coin path keeps ownership, which is the behavior that predates
 * the paste. */
static int ultra_input_map_active(void) { return 0; }

static DWORD WINAPI xinput_thread(LPVOID arg) {
    (void)arg;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
    for (int i = 0; dlls[i] && !s_xget; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) s_xget = (p_xinput_get)(void*)GetProcAddress(h, "XInputGetState");
    }
    for (int a = 0; a < TTZ_JVS_MAX_ANALOG; ++a) g_xinput_in.analog[a] = 0x8000;
    g_xinput_in.screen_x = g_xinput_in.screen_y = 0x8000;
    if (!s_xget) return 0;   /* no XInput available on this host: nothing to poll */
    int coin_was = 0;
    while (!g_stop) {
        ttz_xstate st; memset(&st, 0, sizeof st);
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
            ttz_jvs_input in; memset(&in, 0, sizeof in);
            in.player[0] = (uint16_t)((byte1<<8)|byte2);
            in.system    = test ? 0x80 : 0;         /* TEST switch */
            in.analog[0] = (uint16_t)((int)st.pad.lx + 0x8000);
            in.analog[1] = (uint16_t)(0x7FFF - (int)st.pad.ly);
            for (int a = 2; a < TTZ_JVS_MAX_ANALOG; ++a) in.analog[a] = 0x8000;
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
            /* This board has neither the controls-profile system nor the
             * ULTRA_IO_SHM block (no ultra_io_shared.h is included and no
             * g_shared exists here), so neither can own the coin line - the
             * legacy path does. The original two-term condition was pasted
             * from a board that has both and referenced symbols that do not
             * exist here, which is why this board stopped building.
             * NOTE: it also means the loader's input never reaches this
             * board at all. That is a real gap, tracked separately. */
            int legacy_owns_coin = 1;
            int coin_now = legacy_owns_coin && !!(b & 0x0020) /* BACK/SELECT */;
            if (coin_now && !coin_was) InterlockedIncrement(&g_local_coin);
            coin_was = coin_now;
        }
        Sleep(8);
    }
    return 0;
}
static void ttz_xinput_start(void) { CreateThread(NULL, 0, xinput_thread, NULL, 0, NULL); }

/* Snapshot the current input for the JVS engine. */
static void ttz_hook_current_input(ttz_jvs_input* out) {
    memset(out, 0, sizeof *out);
    for (int a = 0; a < TTZ_JVS_MAX_ANALOG; ++a) out->analog[a] = 0x8000;
    out->screen_x = out->screen_y = 0x8000;
    *out = g_xinput_in;
}

/* Pull input into the JVS node + inject any coin edges. Called from
 * jvs_serial_hook_typexzero.c's WriteFile shim on every JVS poll. */
void ttz_hook_pull_input(ttz_jvs_node* n) {
    if (!n) return;
    ttz_jvs_input in;
    ttz_hook_current_input(&in);
    ttz_jvs_set_input(n, &in);
    long lc = g_local_coin;
    if (lc != g_local_coin_seen) {
        /* Slot 0: COIN_DECREASE maps 1-based wire slot 1 -> our array index 0
         * (jvs_core_typexzero.c), so a credit inserted here is the one the
         * game spends. */
        int delta = (int)(lc - g_local_coin_seen);
        ttz_jvs_insert_coin(n, 0, delta);
        g_local_coin_seen = lc;
    }
}

/* -------------------------------------------------------------------------- */
static void configure_node(void) {
    g_node = ttz_jvs_create();
    if (!g_node) return;
    const char* id = getenv("ULTRA_JVS_ID");
    if (id && *id) ttz_jvs_set_id(g_node, id);
    int screenpos = 0;
    const char* sp = getenv("ULTRA_JVS_SCREENPOS");
    if (sp) screenpos = (sp[0] == '1');
    uint8_t analog_ch = 0;   /* 0 = keep node default (8) */
    const char* ach = getenv("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= TTZ_JVS_MAX_ANALOG) analog_ch = (uint8_t)v; }
    ttz_jvs_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 * imports, render/audio plugins). The first install already happened
 * synchronously in DllMain; this only re-points IAT slots periodically.
 * ttz_jvs_serial_install() is idempotent (name + value based patch). */
static DWORD WINAPI ttz_worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        ttz_jvs_serial_install();
    }
    return 0;
}

/* ---- crash diagnostics (VEH) ------------------------------------------------ */
static LONG WINAPI ttz_crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "(unknown)";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
#ifdef _WIN64
        ttz_log("CRASH: ACCESS_VIOLATION at RIP=%p in %s", addr, modname);
        ttz_log("CRASH: RAX=%p RBX=%p RCX=%p RDX=%p",
            (void*)ep->ContextRecord->Rax, (void*)ep->ContextRecord->Rbx,
            (void*)ep->ContextRecord->Rcx, (void*)ep->ContextRecord->Rdx);
#else
        ttz_log("CRASH: ACCESS_VIOLATION at EIP=%p in %s", addr, modname);
        ttz_log("CRASH: EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX",
            ep->ContextRecord->Eax, ep->ContextRecord->Ebx,
            ep->ContextRecord->Ecx, ep->ContextRecord->Edx);
        ttz_log("CRASH: ESP=%08lX EBP=%08lX ESI=%08lX EDI=%08lX",
            ep->ContextRecord->Esp, ep->ContextRecord->Ebp,
            ep->ContextRecord->Esi, ep->ContextRecord->Edi);
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
            utxp.board        = "taito_typezero";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        AddVectoredExceptionHandler(1, ttz_crash_handler);
        ttz_log("=== ultra_taito_typezero_hook%s.dll attached (Type-Zero-only, independent) ===",
#ifdef _WIN64
                "64"
#else
                "32"
#endif
        );
        configure_node();
        ttz_jvs_serial_install();   /* also arms the D3D9 Direct3DCreate9 IAT hook */
        ttz_audio_install();
        ttz_xinput_start();
        CreateThread(NULL, 0, ttz_worker, NULL, 0, NULL);
        /* Ultra Omni-Bridge (5th Omni service, opt-in via ULTRA_BRIDGE_ENABLE,
         * default OFF): synthesizes raw cursor/click/key input for a Type
         * Zero title that reads OS input directly instead of JVS. Called
         * after the D3D9 hook is armed above so the window accessor has a
         * live hook to observe; the accessor itself just returns NULL until
         * a device actually exists, so this is safe even before CreateDevice
         * fires. No pose source: Type Zero's board profile has no
         * BP_IO_LIGHTGUN (touch input is its non-JVS-serial peripheral, out
         * of scope here per this file's header comment), so pass NULL for
         * get_pose -- gamepad-to-cursor only. */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(ttz_d3d9_device_window, NULL);
ultra_pausemenu_install(ttz_d3d9_device_window);
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();
ultra_pausemenu_shutdown();
        ttz_d3d9_teardown();
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader/user can confirm the right DLL was
 * injected (and that it is the independent Type-Zero-only build, not the
 * shared Type X hook). */
__declspec(dllexport) const char* ultra_taito_typezero_hook_id(void) {
    return "Ultra Loader Taito Type Zero I/O hook (independent, clean-room) v1";
}

#endif /* _WIN32 */
