/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_bna1.c  --  ultra_namco_bna1_hook{32,64}.dll entry point.
 * =============================================================================
 *
 *  Injected into a Namco System BNA1 game by modules/namco_bna1's own
 *  namco_bna1_main.c spawner (see namco_bna1_main.c's find_hook()/inject_dll_*
 *  -- this DLL is the "board-native hook" it prefers over the shared Type X
 *  fallback). On attach it:
 *    - opens the (optional) shared-memory input block a future loader-side
 *      writer may create (ULTRA_IO_SHM),
 *    - creates one clean-room JVS node (jvs_core_bna1.c),
 *    - installs the JVS-over-USB + amauthd-presence transport (namco_io_bna1.c),
 *    - installs the synthetic DirectInput8 device (dinput8_emu_bna1.c),
 *    - installs the DirectSound/XAudio2 diagnostic hook (audio_hook_bna1.c),
 *    - installs the D3D11/DXGI present throttle when armed (d3d11_throttle_bna1.c),
 *    - falls back to reading a local XInput gamepad when no loader block is
 *      mapped, so a standalone/manual run is still playable.
 *
 *  SELF-CONTAINED: every symbol this file needs comes from THIS board's own
 *  src/hook/ tree (jvs_core_bna1.*, ultra_io_shared_bna1.h, iat_patch_bna1.*,
 *  namco_io_bna1.c, d3d11_throttle_bna1.c, dinput8_emu_bna1.c,
 *  audio_hook_bna1.c) -- zero link dependency on native_x86's compiled output
 *  or any other board module.
 *
 *  Clean-room: Ultra Loader's own work over the public Win32 API + the open
 *  JVS spec. No game code, no third-party loader code.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "jvs_core_bna1.h"
#include "ultra_io_shared_bna1.h"
#include "iat_patch_bna1.h"
#include "omni_bridge.h"
#include "ultra_transport.h"   /* shared universal I/O transport layer */

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


/* from the other hook translation units in this board's own tree */
void ultra_bna1_namco_io_install(void);   /* namco_io_bna1.c   -- JVS-over-USB + amauthd */
void ultra_bna1_dinput8_install(void);    /* dinput8_emu_bna1.c -- synthetic DInput8 device */
void ultra_bna1_audio_install(void);      /* audio_hook_bna1.c -- DirectSound/XAudio2 diag */
void ultra_bna1_d3d11_install(void);      /* d3d11_throttle_bna1.c -- DXGI present throttle */
void ultra_bna1_d3d11_teardown(void);     /* pair the d3d11 limiter's timeBeginPeriod */
HWND ultra_bna1_get_render_window(void);  /* d3d11_throttle_bna1.c -- current swap-chain HWND */

/* -------------------------------------------------------------------------- */
static ultra_jvs_node*    g_node   = NULL;
static HANDLE             g_map    = NULL;
static ultra_io_shared*   g_shared = NULL;
static uint32_t           g_coin_seen[ULTRA_IO_COINS] = {0};
static volatile LONG      g_stop   = 0;

ultra_jvs_node* ultra_bna1_hook_node(void) {
    return g_node;   /* created before any hook can fire (DllMain installs synchronously) */
}

/* Raw shared input block (or NULL). */
ultra_io_shared* ultra_bna1_hook_shared(void) { return g_shared; }

/* ---- standalone gamepad (XInput) ------------------------------------------
 * When no loader is feeding the shared block (e.g. a direct/manual test run),
 * the hook reads the player's gamepad itself so the game is actually playable.
 * XInput is loaded dynamically (no hard dependency on a given version). */
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
void ultra_bna1_hook_current_input(ultra_jvs_input* out) {
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

/* ---- Ultra Omni-Track lightgun pose -> Omni-Bridge ------------------------
 * Signature matches ultra_bridge_pose_fn (omni_bridge.h) exactly, so it drops
 * straight into ultra_omni_bridge_install() as the 2nd argument. The loader's
 * lightgun engine lands an already-calibrated, screen-normalized pose in
 * screen_x/screen_y (0..65535) and flags the trigger as PUSH1 (player[0] bit 9,
 * mask 0x0200 -- see jvs_core_bna1.h's switch bit map), so no translation is
 * needed here beyond a coherent read.
 *
 * WHY THE LOADER_ALIVE GATE: magic alone only proves the mapping was created
 * once; it stays valid in the page long after the loader process exits. The
 * loader clears ULTRA_IO_FLAG_LOADER_ALIVE when it goes away (or when the gun
 * goes inactive), and the last pose it wrote stays frozen in the block. If we
 * kept reporting that stale pose as "live", the bridge would keep warping the
 * OS cursor to a dead aim point every poll -- fighting a real mouse -- and, if
 * the trigger bit happened to be set at the moment the loader died, would hold
 * the mouse button down forever. Returning 0 instead makes the bridge fall
 * back to the gamepad/real mouse. A dead loader means "NO pose", never "last
 * known pose".
 *
 * Returns 1 with a coherent pose captured, 0 otherwise (no block, wrong magic,
 * loader gone, or the seqlock never settled within the retry budget). */
int ultra_bna1_gun_pose(int* out_x, int* out_y, int* out_trig) {
    ultra_io_shared* s = ultra_bna1_hook_shared();
    if (!s || s->magic != ULTRA_IO_MAGIC) return 0;
    if (!(s->flags & ULTRA_IO_FLAG_LOADER_ALIVE)) return 0;
    /* Same bounded odd/equal seqlock retry ultra_bna1_hook_current_input()
     * uses: reading the three fields raw can latch a torn pose (X from frame
     * N, trigger from frame N+1). Unlike that reader we do NOT fall back to a
     * torn copy -- an un-settled pose reports "no pose" so the bridge leaves
     * the cursor alone rather than aiming at a half-updated point. */
    uint16_t sx = 0x8000, sy = 0x8000, p0 = 0; int got = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t s1 = s->seq; MemoryBarrier();
        sx = s->screen_x; sy = s->screen_y; p0 = s->player[0];
        MemoryBarrier();
        uint32_t s2 = s->seq;
        if (s1 == s2 && !(s1 & 1u)) { got = 1; break; }
    }
    if (!got) return 0;
    *out_x = sx; *out_y = sy; *out_trig = (p0 & 0x0200) != 0;   /* bit9 = PUSH1 = trigger */
    return 1;
}

/* Pull input into the JVS node + inject coin edges (shared block + gamepad). */
void ultra_bna1_hook_pull_input(ultra_jvs_node* n) {
    if (!n) return;
    ultra_jvs_input in;
    ultra_bna1_hook_current_input(&in);
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
        /* Credit the first chute (slot 0). The game's READ_COIN reads slot 0 and
         * its COIN_DECREASE slot=1 maps to coin[0] via the 1-based->0-based fix
         * in jvs_core_bna1.c, so a credit inserted here is the one the game
         * spends. */
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
    /* Advertised analog-channel count. Default 8; a touch/rhythm cabinet may
     * poll more, so let the loader bump it via ULTRA_JVS_ANALOG so FEATCHK
     * matches the game's READ_ANALOG count. */
    uint8_t analog_ch = 0;   /* 0 = keep node default (8) */
    const char* ach = getenv("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= JVS_MAX_ANALOG) analog_ch = (uint8_t)v; }
    jvs_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 * imports, render/audio plugins, winusb.dll loaded late for the JVS-USB
 * bridge). The first install already happened synchronously in DllMain; this
 * just re-arms the idempotent installers periodically for a short window. */
static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        ultra_bna1_namco_io_install();   /* idempotent (env-gated + name/value based) */
        ultra_bna1_dinput8_install();    /* idempotent */
    }
    return 0;
}

/* ---- crash diagnostics (VEH) ---------------------------------------------- */
static LONG WINAPI ultra_bna1_crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "(unknown)";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
#ifdef _WIN64
        ultra_bna1_log("CRASH(bna1): ACCESS_VIOLATION at RIP=%p in %s", addr, modname);
        ultra_bna1_log("CRASH(bna1): RAX=%p RBX=%p RCX=%p RDX=%p",
            (void*)ep->ContextRecord->Rax, (void*)ep->ContextRecord->Rbx,
            (void*)ep->ContextRecord->Rcx, (void*)ep->ContextRecord->Rdx);
#else
        ultra_bna1_log("CRASH(bna1): ACCESS_VIOLATION at EIP=%p in %s", addr, modname);
        ultra_bna1_log("CRASH(bna1): EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX",
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

    {   /* Universal transport layer: declare this board's shape so the shared
         * pacing/range/codec core is live and self-describing in the log.
         * Serial boards are REQUEST/RESPONSE -- their reply queue already ends
         * the game's read loop, so pacing is deliberately inert here; injecting
         * empty reads would truncate a reply frame. Behavior is unchanged. */
        static int s_utx_once;
        if (!s_utx_once) { static utx_profile utxp;
            s_utx_once = 1;
            memset(&utxp, 0, sizeof utxp);
            utxp.board        = "namco_bna1";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        AddVectoredExceptionHandler(1, ultra_bna1_crash_handler);
        /* Install SYNCHRONOUSLY: the module injects us into the SUSPENDED game
         * and resumes right after, so the hooks must be in place before the
         * game's first thread runs. */
        open_shared();
        configure_node();
        ultra_bna1_namco_io_install();    /* JVS-over-USB + amauthd (env-gated: ULTRA_NAMCO_IO) */
        ultra_bna1_dinput8_install();     /* synthetic DInput8 device */
        ultra_bna1_audio_install();       /* DirectSound + XAudio2 diagnostic (always-on, passthrough) */
        /* D3D11 present-throttle: ULTRA_D3D11 (vendor-neutral) OR ULTRA_SEGA_D3D11
         * (legacy alias) arms it -- same knobs the loader's board_profile.c sets
         * via set_if_unset for system_id="namco_bna1" (BP_HOOK_D3D11). */
        { const char* d  = getenv("ULTRA_D3D11");
          const char* sd = getenv("ULTRA_SEGA_D3D11");
          if ((d && d[0]=='1') || (sd && sd[0]=='1')) ultra_bna1_d3d11_install(); }
        /* Ultra Omni-Bridge (5th Omni service, opt-in): keyboard/mouse
         * emulation from a live gamepad, for a BNA1 title that turns out to
         * read raw OS input directly instead of JVS (the pattern proven on
         * Type X2's Haunted Museum). Installed AFTER ultra_bna1_d3d11_install()
         * above so the DXGI present hook is already armed before the bridge's
         * poll thread starts asking ultra_bna1_get_render_window() for a
         * window -- the getter just returns NULL (bridge stays inert) if D3D11
         * was never armed for this run. get_pose is this board's own
         * ultra_bna1_gun_pose() (above), which serves the loader's Ultra
         * Omni-Track pose (screen_x/screen_y + the PUSH1 trigger bit) out of
         * the shared block -- so a real lightgun aims the cursor and a gamepad
         * only drives it when no live pose is present. It was NULL before,
         * which disabled the gun path entirely (omni_bridge.c only spawns its
         * gun thread when get_pose is non-NULL), so Omni-Track could not reach
         * this board at all. Default OFF (ULTRA_BRIDGE_ENABLE,
         * read via omni_client.h) -- this is framework rollout, not a
         * default-behavior change; a user or a future session turns it on
         * per-game once verified against a real BNA1 dump. */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(ultra_bna1_get_render_window, ultra_bna1_gun_pose);
ultra_pausemenu_install(ultra_bna1_get_render_window);
        ultra_xinput_start();             /* standalone gamepad -> JVS/DInput fallback */
        CreateThread(NULL, 0, worker, NULL, 0, NULL);   /* periodic re-patch only */
        ultra_bna1_log("hook_dllmain(bna1): attached (pid=%lu)", (unsigned long)GetCurrentProcessId());
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();        /* signal stop + force-release any held mouse buttons */
ultra_pausemenu_shutdown();
        ultra_bna1_d3d11_teardown();     /* pair the d3d11 limiter's timeBeginPeriod, if armed */
        if (g_shared) { UnmapViewOfFile(g_shared); g_shared = NULL; }
        if (g_map)    { CloseHandle(g_map); g_map = NULL; }
        if (g_node)   { jvs_destroy(g_node); g_node = NULL; }
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader/injector can confirm the right DLL landed. */
__declspec(dllexport) const char* ultra_namco_bna1_hook_id(void) {
    return "Ultra Loader Namco System BNA1 I/O hook (clean-room JVS-over-USB) v1";
}

#endif /* _WIN32 */
