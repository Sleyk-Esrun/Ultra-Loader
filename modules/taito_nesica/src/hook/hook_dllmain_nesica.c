/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_nesica.c  --  ultra_taito_nesica_hook{32,64}.dll entry: owns
 *                             the JVS engine + the loader<->game input bridge,
 *                             and installs the hooks.
 * =============================================================================
 *
 *  Injected into the NESiCAxLive game by this board's spawner module
 *  (src/taito_nesica_main.c). On attach it:
 *    - opens the shared-memory input block the loader created (ULTRA_IO_SHM),
 *    - creates one clean-room JVS node (analog channel count from
 *      ULTRA_JVS_ANALOG, set by taito_nesica_main.c),
 *    - installs the serial-transport hook, the clean-room DirectInput8
 *      emulation, the D3D9 present hook, the audio diagnostic hook, and the
 *      (env-gated, OFF by default) card-reader presence stub,
 *    - re-applies the IAT patches for a short window so DLLs the game loads
 *      late (delay-loaded imports, plugins) are caught too.
 *
 *  The serial hook calls ultra_hook_node() / ultra_hook_pull_input() here to
 *  get the node and the latest controller state. No game code, no third-party
 *  loader code -- Ultra Loader's own work over the public Win32 API + the open
 *  JVS spec.
 *
 *  INDEPENDENCE: this file replaces native_x86/src/hook/hook_dllmain.c's much
 *  larger glue (board_io_ops per-game override layer, Namco/Sega/Konami/Cave
 *  interop scaffolds, the shared controls.c input-mapping framework, the
 *  Wacky Race / Chaos Breaker scripted auto-pilots, the pause-menu overlay
 *  wiring) with a single self-contained standalone-gamepad fallback suited to
 *  a NESiCA fighter's control scheme (digital 8-way + up to 6 buttons). None
 *  of the board/game-specific pieces above apply to this board, and this file
 *  does not call into, link against, or depend on any of them.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "jvs_core_nesica.h"
#include "ultra_io_shared_nesica.h"
#include "omni_bridge.h"

#include "ultra_transport.h"   /* shared universal I/O transport layer */
/* from the other translation units in this board's own hook tree */
int  ultra_jvs_serial_install(void);
void ultra_dinput8_install(void);         /* clean-room DirectInput8 emulation */
void ultra_audio_install(void);           /* DirectSound + XAudio2 diagnostic hook */
void ultra_card_reader_install(void);     /* NESiCA/Aime card-reader presence stub (env-gated, OFF) */
void ultra_d3d9_teardown_nesica(void);    /* pair the D3D9 limiter's timeBeginPeriod */
HWND ultra_d3d9_device_window_nesica(void); /* d3d9_throttle_nesica.c -- current device window */
void ultra_nesica_log(const char* fmt, ...);

/* -------------------------------------------------------------------------- */
static ultra_jvs_node*    g_node   = NULL;
static HANDLE             g_map    = NULL;
static ultra_io_shared*   g_shared = NULL;
static uint32_t           g_coin_seen[ULTRA_IO_COINS] = {0};
static volatile LONG      g_stop   = 0;

ultra_jvs_node* ultra_hook_node(void) {
    return g_node;   /* created synchronously in DllMain before any hook can fire */
}

/* Raw shared input block (or NULL), for the DirectInput8 emulation to read. */
ultra_io_shared* ultra_hook_shared(void) { return g_shared; }

/* ---- Ultra Omni-Track lightgun pose source --------------------------------
 * Ultra Omni-Track resolves a calibrated, already screen-normalized gun pose
 * and lands it in the loader's shared block (screen_x/screen_y, 0..65535),
 * flagging the trigger in the player[0] switch mask. This adapter hands that
 * pose to Ultra Omni-Bridge: the bridge's 2nd install argument IS its gun-pose
 * source, and with NULL there it never even creates its gun thread -- so
 * Omni-Track could not reach this board at all.
 *
 * Signature is exactly ultra_bridge_pose_fn (omni_bridge.h), so it drops
 * straight into ultra_omni_bridge_install() below.
 *
 * WHY THE LOADER_ALIVE GATE (and why a dead loader must mean "no pose", never
 * "last known pose"): the loader clears ULTRA_IO_FLAG_LOADER_ALIVE when it
 * exits or when the gun goes inactive, but the mapping itself stays alive with
 * the LAST pose it wrote still sitting in it. Reporting that stale pose as
 * live would have the bridge warp the OS cursor to wherever the gun last
 * pointed -- 120x a second, forever -- and re-assert a trigger nobody is
 * pulling, fighting a real mouse for the rest of the session. Returning 0
 * instead lets the bridge fall back to the gamepad path / leave the mouse
 * alone, which is the correct behavior for "no gun right now".
 *
 * The three fields are read under the loader's seqlock (it holds seq odd while
 * writing) with bounded retries. Unlike ultra_hook_current_input() above -- a
 * per-frame JVS poll, where a torn switch mask is harmless and a stale frame is
 * worse -- a torn read here is reported as NO pose: half of frame N's X with
 * frame N+1's trigger is an aim point that never existed, and firing at it is
 * worse than not aiming at all. */
int taito_nesica_gun_pose(int* out_x, int* out_y, int* out_trig) {
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
    *out_x = sx; *out_y = sy;
    /* bit9 of player[0] == byte1 bit1 == PUSH1 (button 1), the switch the
     * loader sets for a gun trigger -- same packing xinput_thread() below and
     * dinput8_emu_nesica.c decode. */
    *out_trig = (p0 & 0x0200) != 0;
    return 1;
}

/* ---- standalone gamepad (XInput) ------------------------------------------
 * When no loader is feeding the shared block (e.g. a direct test launch), the
 * hook reads the player's gamepad itself so the game is actually playable.
 * XInput is loaded dynamically (no hard dependency on a given version). Maps
 * to a NESiCA fighter's control scheme: 8-way digital stick + up to 6 attack
 * buttons + START/SERVICE/TEST, plus a coin-insert edge on RT/L3. */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } ul_xpad;
typedef struct { DWORD packet; ul_xpad pad; } ul_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, ul_xstate*);
static p_xinput_get   s_xget = NULL;
static ultra_jvs_input g_xinput_in;          /* updated by the XInput thread */
static volatile LONG   g_local_coin = 0;     /* coin pulses from the gamepad */
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
    if (!s_xget) return 0;   /* no XInput available on this host -- shared-memory input only */

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
            for (int a = 0; a < JVS_MAX_ANALOG; ++a) in.analog[a] = 0x8000;
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
        /* Credit the first chute (slot 0). READ_COIN reads slot 0 and its
         * COIN_DECREASE slot=1 maps to coin[0] via the 1-based->0-based fix in
         * jvs_core_nesica.c, so a credit inserted here is the one spent. */
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
    /* Advertised analog-channel count. taito_nesica_main.c sets
     * ULTRA_JVS_ANALOG=2 for the NLX1 fighter default (sticks + buttons are
     * digital JVS switches; 2 keeps FEATCHK modest). 0 = keep node default (8). */
    uint8_t analog_ch = 0;
    const char* ach = getenv("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= JVS_MAX_ANALOG) analog_ch = (uint8_t)v; }
    /* defaults (2p/13sw/2coin/8analog@16/2gpo) + optional light-gun feature
     * (unused by a NESiCA fighter, kept for engine symmetry) */
    jvs_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 * imports, render/audio plugins). The first install already happened
 * synchronously in DllMain; this only re-points IAT slots periodically. */
static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        ultra_jvs_serial_install();   /* idempotent (name + value based) */
        ultra_dinput8_install();      /* idempotent */
    }
    return 0;
}

/* ---- ULTRA_DISMISS_DIALOGS: auto-dismiss modal error dialogs ---------------
 * taito_nesica_main.c defaults ULTRA_DISMISS_DIALOGS=1 so an unattended run
 * (missing asset, DirectX warning, etc.) doesn't sit blocked on a MessageBox.
 * We only ever touch a top-level window that: (a) belongs to THIS process,
 * and (b) is a standard dialog class ("#32770") -- never another process's
 * window, never the game's own render window. Clicking IDOK/IDCANCEL (or
 * WM_CLOSE if neither control exists) is the same action a human tester would
 * take to get past an informational/error box. */
static BOOL CALLBACK dismiss_enum(HWND hwnd, LPARAM lp) {
    (void)lp;
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    char cls[32];
    if (!GetClassNameA(hwnd, cls, sizeof cls) || strcmp(cls, "#32770") != 0) return TRUE;
    HWND ok = GetDlgItem(hwnd, IDOK);
    HWND cancel = GetDlgItem(hwnd, IDCANCEL);
    if (ok)          PostMessageA(hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)ok);
    else if (cancel) PostMessageA(hwnd, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), (LPARAM)cancel);
    else             PostMessageA(hwnd, WM_CLOSE, 0, 0);
    ultra_nesica_log("[dialog] auto-dismissed hwnd=%p class=%s", (void*)hwnd, cls);
    return TRUE;
}
static DWORD WINAPI dismiss_thread(LPVOID arg) {
    (void)arg;
    while (!g_stop) { EnumWindows(dismiss_enum, 0); Sleep(500); }
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
        ultra_nesica_log("CRASH: ACCESS_VIOLATION at RIP=%p in %s", addr, modname);
        ultra_nesica_log("CRASH: RAX=%p RBX=%p RCX=%p RDX=%p",
            (void*)ep->ContextRecord->Rax, (void*)ep->ContextRecord->Rbx,
            (void*)ep->ContextRecord->Rcx, (void*)ep->ContextRecord->Rdx);
#else
        ultra_nesica_log("CRASH: ACCESS_VIOLATION at EIP=%p in %s", addr, modname);
        ultra_nesica_log("CRASH: EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX",
            ep->ContextRecord->Eax, ep->ContextRecord->Ebx,
            ep->ContextRecord->Ecx, ep->ContextRecord->Edx);
        ultra_nesica_log("CRASH: ESP=%08lX EBP=%08lX ESI=%08lX EDI=%08lX",
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
            utxp.board        = "taito_nesica";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        AddVectoredExceptionHandler(1, ultra_crash_handler);
        /* Install SYNCHRONOUSLY here: the spawner injects us into the
         * SUSPENDED game and resumes right after, so the hooks must be in
         * place before the game's first thread runs (else its first
         * CreateFile(COM) is missed). */
        open_shared();
        configure_node();
        ultra_jvs_serial_install();
        ultra_dinput8_install();                          /* synthetic DInput8 device */
        ultra_audio_install();                            /* DirectSound + XAudio2 diagnostic (always-on, passthrough) */
        ultra_card_reader_install();                      /* internally env-gated (OFF by default) */
        { const char* e = getenv("ULTRA_DISMISS_DIALOGS");
          if (e && e[0] == '1') CreateThread(NULL, 0, dismiss_thread, NULL, 0, NULL); }
        ultra_xinput_start();                             /* standalone gamepad -> JVS/DInput fallback */
        CreateThread(NULL, 0, worker, NULL, 0, NULL);      /* periodic re-patch only */
        /* Ultra Omni-Bridge (5th Omni service, opt-in via ULTRA_BRIDGE_ENABLE,
         * default OFF): synthesizes raw cursor/click/key input for a NESiCA
         * title that reads OS input directly instead of JVS/DirectInput.
         * Called after ultra_jvs_serial_install() above, which is what arms
         * the D3D9 Direct3DCreate9 IAT hook -- so the window accessor has a
         * live hook to observe (it just returns NULL until CreateDevice
         * actually fires, so this is safe either way).
         * get_pose = taito_nesica_gun_pose (above): NESiCAxLive is not only
         * fighters -- the platform carries lightgun titles too, and the loader
         * already writes an Omni-Track pose into screen_x/screen_y of the very
         * block this hook maps. Passing NULL here left the bridge's gun thread
         * uncreated, so Omni-Track could not reach this board at any setting.
         * The adapter is self-gating (no live pose -> returns 0 -> the bridge
         * falls back to gamepad-to-cursor exactly as before), so a non-gun
         * NESiCA title behaves identically to the old NULL. */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(ultra_d3d9_device_window_nesica, taito_nesica_gun_pose);
ultra_pausemenu_install(ultra_d3d9_device_window_nesica);
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();
ultra_pausemenu_shutdown();
        ultra_d3d9_teardown_nesica();   /* timeEndPeriod(1) if the limiter raised it */
        if (g_shared) { UnmapViewOfFile(g_shared); g_shared = NULL; }
        if (g_map)    { CloseHandle(g_map); g_map = NULL; }
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the spawner can confirm the right DLL was injected. */
__declspec(dllexport) const char* ultra_taito_nesica_hook_id(void) {
    return "Ultra Loader Taito NESiCAxLive I/O hook (clean-room JVS, independent build) v1";
}

#endif /* _WIN32 */
