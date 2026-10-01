/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_ringwide.c  --  ultra_sega_ringwide_hook{32,64}.dll entry point.
 *                               Owns the JVS engine, the shared diagnostic
 *                               logger, the standalone-gamepad input source,
 *                               and installs every subsystem hook for the
 *                               sega_ringwide board.
 * =============================================================================
 *
 *  Injected into the Sega RingWide game process by modules/sega_ringwide's
 *  own launcher (src/sega_ringwide_main.c). On attach it:
 *    - creates one clean-room JVS node (jvs_core_ringwide.c),
 *    - installs the JVS-over-serial transport hook (jvs_serial_hook_ringwide.c),
 *    - installs the presence-only mxparallel keychip shim (env-gated, OFF by
 *      default -- sega_keychip_ringwide.c),
 *    - installs the D3D9 present/throttle hook (d3d9_throttle_ringwide.c),
 *    - installs the DirectSound/XAudio2 diagnostic audio hook
 *      (audio_hook_ringwide.c),
 *    - starts a standalone XInput gamepad reader so the board is playable
 *      without a physical JVS panel (fed into the same JVS node).
 *
 *  THIS FILE, AND EVERY FILE IT LINKS AGAINST IN THIS FOLDER, IS SELF-CONTAINED
 *  to the sega_ringwide board module: no #include, no link-time or run-time
 *  dependency on modules/native_x86/src/hook's compiled output or on
 *  I:\Ultra Loader\modtest\. (modules/native_x86/src/hook/hook_dllmain.c was
 *  read as REFERENCE ONLY for the general shape of an Ultra Loader hook
 *  DllMain; its per-game autopilot scripts, pause-menu wiring, and board_io_ops
 *  plug-in system are native_x86-only extras this board does not copy.)
 *
 *  Clean-room: Ultra Loader's own work over the public Win32 API + the open
 *  JVS spec.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "jvs_core_ringwide.h"
#include "omni_bridge.h"

#include "ultra_transport.h"   /* shared universal I/O transport layer */
/* from the other hook translation units in this board's own tree */
int  ultra_ringwide_jvs_serial_install(void);
void ultra_ringwide_keychip_install(void);      /* presence-only mxparallel shim (env-gated, OFF) */
void ultra_ringwide_audio_install(void);        /* DirectSound + XAudio2 diagnostic hook */
HWND ultra_ringwide_d3d9_window(void);          /* current D3D9 device window, NULL until CreateDevice (d3d9_throttle_ringwide.c) */

/* -------------------------------------------------------------------------- */
/*  Shared diagnostic logger (ULTRA_JVS_LOG=<path>). Every file in this board's
 *  hook tree calls this instead of writing its own log sink. */
/* -------------------------------------------------------------------------- */
void ultra_ringwide_log(const char* fmt, ...) {
    const char* path = getenv("ULTRA_JVS_LOG");
    if (!path || !*path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

/* -------------------------------------------------------------------------- */
static ultra_jvs_node*    g_node   = NULL;
static volatile LONG      g_stop   = 0;

ultra_jvs_node* ultra_ringwide_hook_node(void) {
    return g_node;   /* created before any hook can fire */
}

/* ---- standalone gamepad (XInput) ------------------------------------------
 * The sega_ringwide module launches the game directly (no shared-memory input
 * block from a richer loader front-end today -- see sega_ringwide_main.c), so
 * this hook reads the player's gamepad itself and feeds it straight into the
 * JVS node, exactly the way a real RingWide JVS panel would report switches. */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } ul_xpad;
typedef struct { DWORD packet; ul_xpad pad; } ul_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, ul_xstate*);
static p_xinput_get   s_xget = NULL;
static ultra_jvs_input g_xinput_in;
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
    for (int a = 0; a < JVS_MAX_ANALOG; ++a) g_xinput_in.analog[a] = 0x8000;
    g_xinput_in.screen_x = g_xinput_in.screen_y = 0x8000;
    if (!s_xget) return 0;   /* no XInput available on this host -- nothing to poll */

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
static void ultra_xinput_start(void) { CreateThread(NULL, 0, xinput_thread, NULL, 0, NULL); }

/* Pull the latest input into the JVS node + drain coin edges. Called from
 * jvs_serial_hook_ringwide.c's WriteFile hook every time the game polls. */
void ultra_ringwide_hook_pull_input(ultra_jvs_node* n) {
    if (!n) return;
    jvs_set_input(n, &g_xinput_in);
    long lc = g_local_coin;
    if (lc != g_local_coin_seen) {
        int delta = (int)(lc - g_local_coin_seen);
        jvs_insert_coin(n, 0, delta);
        g_local_coin_seen = lc;
    }
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
    jvs_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 * imports, render plugins). The first install already happened synchronously
 * in DllMain; this only re-points IAT slots periodically. */
static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        ultra_ringwide_jvs_serial_install();   /* idempotent (name + value based) */
    }
    return 0;
}

/* ---- crash diagnostics (VEH) ------------------------------------------- */
static LONG WINAPI ultra_ringwide_crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "(unknown)";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
#ifdef _WIN64
        ultra_ringwide_log("CRASH: ACCESS_VIOLATION at RIP=%p in %s", addr, modname);
        ultra_ringwide_log("CRASH: RAX=%p RBX=%p RCX=%p RDX=%p",
            (void*)ep->ContextRecord->Rax, (void*)ep->ContextRecord->Rbx,
            (void*)ep->ContextRecord->Rcx, (void*)ep->ContextRecord->Rdx);
#else
        ultra_ringwide_log("CRASH: ACCESS_VIOLATION at EIP=%p in %s", addr, modname);
        ultra_ringwide_log("CRASH: EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX",
            ep->ContextRecord->Eax, ep->ContextRecord->Ebx,
            ep->ContextRecord->Ecx, ep->ContextRecord->Edx);
        ultra_ringwide_log("CRASH: ESP=%08lX EBP=%08lX ESI=%08lX EDI=%08lX",
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

    {   /* Universal transport layer: declare this board's shape so the shared
         * pacing/range/codec core is live and self-describing in the log.
         * Serial boards are REQUEST/RESPONSE -- their reply queue already ends
         * the game's read loop, so pacing is deliberately inert here; injecting
         * empty reads would truncate a reply frame. Behavior is unchanged. */
        static int s_utx_once;
        if (!s_utx_once) { static utx_profile utxp;
            s_utx_once = 1;
            memset(&utxp, 0, sizeof utxp);
            utxp.board        = "sega_ringwide";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        AddVectoredExceptionHandler(1, ultra_ringwide_crash_handler);
        /* Install SYNCHRONOUSLY here: the module injects us into the SUSPENDED
         * game and resumes right after, so the hooks must be in place before
         * the game's first thread runs (else its first CreateFile(COM) or
         * Direct3DCreate9 call is missed). */
        configure_node();
        ultra_ringwide_jvs_serial_install();     /* JVS-over-serial + D3D9 present hook wiring */
        ultra_ringwide_keychip_install();        /* presence-only mxparallel shim (env-gated, OFF) */
        ultra_ringwide_audio_install();          /* DirectSound + XAudio2 diagnostic hook */
        ultra_xinput_start();                    /* standalone gamepad -> JVS */
        /* Ultra Omni-Bridge (5th Omni service, opt-in via ULTRA_BRIDGE_ENABLE --
         * off by default, see omni_client_read_bridge()): raw SendInput cursor/
         * click/key-tap synthesis for a title that reads OS keyboard+mouse
         * directly instead of this board's JVS panel. Installed after the D3D9
         * present hook above so its window-getter has something to find once
         * CreateDevice fires. sega_ringwide has no lightgun/shared-pose source
         * of its own (its standalone gamepad reader feeds the JVS node, not a
         * pose struct), so get_pose is NULL -- gamepad-only bridging. */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(ultra_ringwide_d3d9_window, NULL);
ultra_pausemenu_install(ultra_ringwide_d3d9_window);
        CreateThread(NULL, 0, worker, NULL, 0, NULL);   /* periodic re-patch only */
        ultra_ringwide_log("sega_ringwide hook attached (pid=%lu)", (unsigned long)GetCurrentProcessId());
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();
ultra_pausemenu_shutdown();
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader/operator can confirm the right DLL was
 * injected (objdump -p / a diagnostic GetProcAddress probe). */
__declspec(dllexport) const char* ultra_sega_ringwide_hook_id(void) {
    return "Ultra Loader Sega RingWide hook (clean-room JVS, presence-only keychip) v1";
}

#endif /* _WIN32 */
