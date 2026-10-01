/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain.c  --  ultra_typex_hook.dll entry: owns the JVS engine + the
 *                      loader<->game input bridge, and installs the hooks.
 * =============================================================================
 *
 *  Injected into the native arcade game by the native_x86 module. On attach it:
 *    - opens the shared-memory input block the loader created (ULTRA_IO_SHM),
 *    - creates one clean-room JVS node,
 *    - installs the serial-transport hook + the dongle/dialog bypass,
 *    - re-applies the IAT patches for a short window so DLLs the game loads late
 *      (delay-loaded imports, plugins) are caught too.
 *
 *  The serial hook calls ultra_hook_node() / ultra_hook_pull_input() here to get
 *  the node and the latest controller state. No game code, no third-party loader
 *  code -- Ultra Loader's own work over the public Win32 API + the open JVS spec.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "jvs_core.h"
#include "ultra_io_shared.h"
#include "board_io_ops.h"
#include "ultra_transport.h"   /* shared universal I/O transport layer */
#include "ultra_tracer.h"

/* from the other hook translation units */
int  ultra_jvs_serial_install(void);
void ultra_dongle_install(void);
void ultra_dinput8_install(void);   /* clean-room DirectInput8 emulation */
void ultra_gamestate_probe_start(void);  /* per-game exit-gate probe/poke */
void ultra_typex2_gun_install(void);     /* Type X2 lightgun->cursor bridge (env-gated) */
void ultra_typex2_gun_stop(void);        /* stop the gun/auto threads + release mouse btn */
void ultra_d3d8_teardown(void);          /* pair timeBeginPeriod with timeEndPeriod */
void ultra_d3d11_install(void);          /* Sega Nu/ALLS DXGI present throttle (env-gated) */
void ultra_d3d11_teardown(void);         /* pair the d3d11 timeBeginPeriod */
void ultra_gl_install(void);             /* Namco N2 / GL board wglSwapBuffers present throttle (env-gated) */
void ultra_gl_teardown(void);            /* pair the gl timeBeginPeriod */
void ultra_sega_keychip_install(void);   /* Sega keychip software shim (env-gated, OFF) */
void ultra_taito_fastio_install(void);   /* Taito Type X3/X4 Fast-I/O (iDmacDrv) shim (env-gated, OFF) */
void ultra_card_reader_install(void);    /* NESiCA/Aime card-reader presence stub (env-gated, OFF) */
void ultra_sega_io4_install(void);       /* Sega IO4-USB (837-15257) I/O emulation (env-gated, OFF) */
void ultra_sega_io4_insert_coin(int slot, int count); /* feed coin pulses to the IO4 counters */
int  ultra_sega_io4_enabled(void);       /* whether the IO4 emulation is armed */
void ultra_konami_io_install(void);      /* Konami Bemani-PC ACIO I/O scaffold (env-gated, OFF) */
void ultra_namco_amdaemon_install(void); /* Namco ES LOCAL amdaemon presence stub (env-gated, OFF) */
void ultra_namco_io_install(void);       /* Namco ES JVS-over-USB I/O + amauthd presence (env-gated, OFF) */
void ultra_namco_dongle_install(void);   /* Namco ES3 USB dongle bypass (env-gated, OFF) */
void ultra_rawthrills_io_install(void);  /* Raw Thrills JammaUSB factory patch (env-gated, OFF) */
void ultra_audio_install(void);          /* DirectSound + XAudio2 diagnostic hook (audio_hook.c) */
/* THIS BOARD'S OWN render + audio layers (konami_render9.c / konami_audio.c).
 * They exist because shaiolib_crk.c owns this game's Direct3DCreate9 hook:
 * konami_render9 controls the output WINDOW only and never touches the
 * device. The generic d3d9_throttle.c is deliberately NOT built for this
 * board -- see the restore note in build_hook.sh. */
void konami_render9_install(void);
void konami_audio_install(void);

/* Shared pause/settings menu (work/src/pause_menu_service.c) via pause_menu_wire.c */
void ultra_pausemenu_edge_check(WORD held, WORD prev);
void ultra_pausemenu_kbd_edge_check(void);
int  ultra_pausemenu_pump(void);
void ultra_pausemenu_shutdown(void);

/* Ultra Omni-Bridge (omni_bridge.c) -- present in this tree but was never
 * actually wired up (file existed, install() was never called). */
void ultra_omni_bridge_install(HWND (*get_window)(void), int (*get_pose)(int*, int*, int*));
void ultra_omni_bridge_stop(void);
HWND ultra_typex2_device_window(void);
int  ultra_typex2_gun_pose(int* out_x, int* out_y, int* out_trig);

/* Player input-mapping layer (controls.c). DEFAULT-OFF: active only when a board
 * controls profile/config resolves or ULTRA_CONTROLS=1. When inactive, the legacy
 * gamepad/autopilot path below is byte-identical. */
void     ultra_input_map_install(void);
int      ultra_input_map_active(void);
void     ultra_input_map_current(ultra_jvs_input* out);
int      ultra_input_map_channel_range(int channel, uint16_t* omin, uint16_t* omax, int* bipolar);  /* profile's calibrated floor/ceiling for a JVS analog channel */
uint32_t ultra_input_map_coin(int slot);

/* -------------------------------------------------------------------------- */
static ultra_jvs_node*    g_node   = NULL;
static HANDLE             g_map    = NULL;
static ultra_io_shared*   g_shared = NULL;
static uint32_t           g_coin_seen[ULTRA_IO_COINS] = {0};
static volatile LONG      g_stop   = 0;
static volatile LONG      g_node_ready = 0;

ultra_jvs_node* ultra_hook_node(void) {
    return g_node;   /* created by the worker before any hook can fire */
}

/* Raw shared input block (or NULL), for the DirectInput8 emulation to read. */
ultra_io_shared* ultra_hook_shared(void) { return g_shared; }

/* ---- standalone gamepad (XInput) ------------------------------------------
 * When no loader is feeding the shared block (e.g. the standalone test launcher),
 * the hook reads the player's gamepad itself so the game is actually playable.
 * XInput is loaded dynamically (no hard dependency on a given version). */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } ul_xpad;
typedef struct { DWORD packet; ul_xpad pad; } ul_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, ul_xstate*);
static p_xinput_get   s_xget = NULL;
static ultra_jvs_input g_xinput_in;          /* updated by the XInput thread */
static volatile LONG   g_local_coin = 0;     /* coin pulses from the gamepad */
static long            g_local_coin_seen = 0;
static uint32_t        g_map_coin_seen[ULTRA_IO_COINS] = {0};  /* input-map coin counters */

/* Forwarders for board_io_ops.c: a per-game autopilot_tick callback writes
 * through these instead of reaching into g_xinput_in/g_local_coin directly, so
 * write-access to those two statics stays centralized in this file even though
 * the per-tick VALUES are decided by an isolated game_*.c module. */
void ultra_hook_set_local_input(const ultra_jvs_input* in) {
    if (in) g_xinput_in = *in;
}
void ultra_hook_local_coin_insert(int slot) {
    if (slot == 0) InterlockedIncrement(&g_local_coin);
    /* Non-zero slots: today's autopilot scripts only ever credit slot 0 (the
     * single coin chute every extracted game module drives); if a future game
     * module needs a second slot, extend this forwarder then rather than
     * guess at a second counter now. */
}

static DWORD WINAPI xinput_thread(LPVOID arg) {
    (void)arg;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
    for (int i = 0; dlls[i] && !s_xget; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) s_xget = (p_xinput_get)(void*)GetProcAddress(h, "XInputGetState");
    }
    for (int a = 0; a < JVS_MAX_ANALOG; ++a) g_xinput_in.analog[a] = 0x8000;
    g_xinput_in.screen_x = g_xinput_in.screen_y = 0x8000;
    /* demo mode: drive a scripted input sequence so a standalone test run is
     * visibly controllable (proves input reaches the game) without a physical
     * pad. Env-gated; never on for a real loader run. */
    const char* dm = getenv("ULTRA_DEMO_INPUT");
    const int demo = dm && dm[0] == '1';
    /* Wacky Race racing auto-pilot: a passive auto-throttle racer that drives the
     * whole credit loop over JVS (coin -> START through credit/car/stage select ->
     * race with accel held + gentle steer -> finish/game-over -> attract). The
     * INPUT CHECK screen (game.exe strings) lists the inputs as:
     *   digital: TEST / SERVICE / VIEW / START   (START advances every menu)
     *   analog : BRAKE / ACCEL / STEERING        (READ_ANALOG channels)
     * Coin is the JVS coin counter (g_local_coin). Channel order is wheel/accel/
     * brake on the first analog channels (overridable via ULTRA_WR_CH_*). Gated by
     * ULTRA_WR_AUTO=1; default-off so a plain render/loader run is unchanged. */
    const char* wm = getenv("ULTRA_WR_AUTO");
    const int wr_auto = wm && wm[0] == '1';
    /* channel assignment (defaults: steer=0, accel=1, brake=2; tune per probe) */
    int ch_steer = 0, ch_accel = 1, ch_brake = 2;
    { const char* e;
      if ((e=getenv("ULTRA_WR_CH_STEER")) && *e) ch_steer = atoi(e);
      if ((e=getenv("ULTRA_WR_CH_ACCEL")) && *e) ch_accel = atoi(e);
      if ((e=getenv("ULTRA_WR_CH_BRAKE")) && *e) ch_brake = atoi(e);
    }
    /* Analog levels must fall INSIDE the game's NVRAM calibration ranges (WR0.CFG):
     *   ACCEL  ioac[min,max] = [26000,44000]  -> full gas = ioacmax = 44000 (0xABE0)
     *   BRAKE  iobr[min,max] = [28000,47000]  -> brake off = iobrmin = 28000 (0x6D60)
     *   STEER  iost[min,max] = [ 4000,64000]  -> center    = midpoint = 34000 (0x84D0)
     * Feeding a raw outside the range (e.g. 0xFFFF accel = 65535 > 44000) reads as an
     * out-of-range / uncalibrated axis, which (with the calib flags) the game rejects.
     * All overridable so a different cabinet/NVRAM can be matched. */
    unsigned wr_accel_raw = 44000;   /* ioacmax: full throttle */
    unsigned wr_steer_raw = 34000;   /* (iostmin+iostmax)/2: center */
    unsigned wr_brake_raw = 28000;   /* iobrmin: brake released */
    { const char* e;
      if ((e=getenv("ULTRA_WR_ACCEL")) && *e) wr_accel_raw = (unsigned)strtoul(e,NULL,0) & 0xFFFF;
      if ((e=getenv("ULTRA_WR_STEER")) && *e) wr_steer_raw = (unsigned)strtoul(e,NULL,0) & 0xFFFF;
      if ((e=getenv("ULTRA_WR_BRAKE")) && *e) wr_brake_raw = (unsigned)strtoul(e,NULL,0) & 0xFFFF;
    }
    unsigned tick = 0;
    { const board_io_ops* o = ultra_board_ops_get();
      if (!s_xget && !demo && !wr_auto && !(o && o->autopilot_tick)) return 0; }
    int coin_was = 0;
    while (!g_stop) {
        /* A registered per-game module (game_wackyrace.c, game_chaosbreaker.c, ...)
         * takes priority over the inline legacy branches below -- once one is
         * registered, it is the ONLY thing driving g_xinput_in/coin for this
         * process, so the old inline scripts never run for that game (avoids
         * double-driving input from two places at once). Boards with no
         * extracted module (nothing registered) fall through unchanged. */
        const board_io_ops* ops = ultra_board_ops_get();
        if (ops && ops->autopilot_tick) {
            ops->autopilot_tick();
            tick++;
            Sleep(8);
            continue;
        }
        if (wr_auto) {
            /* Tick is ~8ms (125 Hz). The board converts an inserted coin straight to
             * a credit + auto-debits the income meter, so a single one-shot coin can
             * be gone before START is pressed. So we keep a credit topped up: insert
             * a coin every ~3s (until START commits a race), and pulse START on a
             * steady ~0.8s cadence from boot. VIEW (PUSH1, byte1 bit1) is pulsed out
             * of phase with START to confirm car/stage selects. Driving axes held:
             * accel full, steer center, brake off -- a passive auto-throttle racer
             * that plays a stage through to the finish/game-over and back to attract.
             * START tapped as a clean ~150ms edge so each press = one menu action. */
            uint8_t b1 = 0, b2 = 0, sysb = 0;
            unsigned t = tick;
            /* Wacky Race charges 2 coins per game (COIN_DECREASE amount=2 =
             * GAME START CREDIT). Topping up only 1 coin per cycle never reached the
             * 2-coin threshold after the first debit, so the game could never start.
             * Insert a PAIR of coins each top-up, every ~2s, for the first ~30s, so a
             * full credit is always available for START to commit. (ULTRA_WR_COIN_AMT
             * overrides the pair count.) */
            static int coin_amt = 0;
            if (!coin_amt) { const char* e=getenv("ULTRA_WR_COIN_AMT"); coin_amt = (e&&*e)?atoi(e):2; if(coin_amt<1)coin_amt=1; }
            if (t >= 250u && t < 3800u && (t % 250u) == 0u)
                for (int k=0;k<coin_amt;++k) InterlockedIncrement(&g_local_coin);
            /* Optional: foreground the game so the front-buffer dump is bright (the
             * capture grabs the whole desktop; a backgrounded window dims). DEFAULT
             * OFF -- repeatedly stealing foreground can disturb the run, and a clean
             * capture is better achieved by closing other windows. ULTRA_WR_FOCUS=1
             * enables it (every ~2s) for a manual brightness check. */
            if ((t % 250u) == 30u) {
                static int focus = -1;
                if (focus < 0) { const char* e=getenv("ULTRA_WR_FOCUS"); focus = (e&&e[0]=='1'); }
                if (focus) { extern HWND ultra_typex2_device_window(void);
                    HWND w = ultra_typex2_device_window(); if (w) SetForegroundWindow(w); }
            }
            /* Cave PC: always bring the game window to front (user requirement) */
            if ((t % 250u) == 31u) {
                static int cave_focus = -1;
                if (cave_focus < 0) { const char* e=getenv("ULTRA_CAVE_IO"); cave_focus=(e&&e[0]=='1'); }
                if (cave_focus) { extern HWND ultra_typex2_device_window(void);
                    HWND w = ultra_typex2_device_window(); if (w) SetForegroundWindow(w); }
            }
            /* DEFAULT DRIVER = the full bit-sweep. PROVEN end-to-end: it walks
             * attract -> COURSE SELECT -> RACE (timer counts down) -> RESULT ->
             * attract, crash-free. It cycles EACH candidate switch bit (byte1 b7..b0
             * then byte2 b7..b0), ~2.5s each, pulsed ~150ms on / ~350ms off, so
             * whatever the current screen's start/decide/confirm button is gets a
             * long deliberate press within each cycle. (Shorter taps did NOT latch
             * the transitions.) ULTRA_WR_ROTATE=1 selects the leaner 6-action-bit
             * rotation instead (byte1 only). */
            static int rotate_mode = -1;
            if (rotate_mode < 0) { const char* e=getenv("ULTRA_WR_ROTATE"); rotate_mode = (e&&e[0]=='1'); }
            if (!rotate_mode) {
                unsigned slot = (t / 312u) % 16u;   /* 2.5s per candidate bit */
                unsigned ph2  = t % 62u;            /* ~0.5s press/gap cycle */
                if (ph2 < 18u) {
                    if (slot < 8u) b1 = (uint8_t)(1u << (7u - slot));
                    else           b2 = (uint8_t)(1u << (7u - (slot - 8u)));
                }
            } else if (t >= 300u) {
                /* The bit-sweep proved Wacky Race advances attract -> COURSE SELECT
                 * -> race via the byte1 action buttons (START 0x80 + the
                 * VIEW/DECIDE bits 0x20/0x10/0x08/0x04/0x02). A short 150ms tap did
                 * NOT latch the transition -- the game needs a LONGER, deliberate
                 * press (the sweep held each bit ~2.5s when it worked). So rotate
                 * through the candidate action bits with a ~1.2s HOLD + ~0.3s gap
                 * each, so whatever the current screen's confirm/start/decide button
                 * is gets a long press every pass. This walks credit -> course select
                 * -> car select -> race -> results -> attract. (ULTRA_WR_BTN forces a
                 * single bit; ULTRA_WR_HOLD/_GAP tune the press/gap in ticks.) */
                static int forced = -1, hold = 0, gap = 0;
                if (forced < 0) {
                    const char* e=getenv("ULTRA_WR_BTN"); forced = (e&&*e)?(int)strtol(e,NULL,0):0;
                    /* default hold ~2.5s -- the bit-sweep that reached COURSE SELECT
                     * held each candidate this long; shorter taps did not latch. */
                    const char* h=getenv("ULTRA_WR_HOLD"); hold = (h&&*h)?atoi(h):300;  /* ~2.4s */
                    const char* g=getenv("ULTRA_WR_GAP");  gap  = (g&&*g)?atoi(g):50;   /* ~0.4s */
                }
                static const uint8_t cand[] = { 0x80, 0x20, 0x10, 0x08, 0x04, 0x02 };
                unsigned period = (unsigned)(hold + gap);
                unsigned cyc = (t - 300u) % period;
                if ((int)cyc < hold) {                      /* long press */
                    if (forced) b1 = (uint8_t)forced;
                    else {
                        unsigned idx = ((t - 300u) / period) % (sizeof cand / sizeof cand[0]);
                        b1 = cand[idx];
                    }
                }
            }
            ultra_jvs_input in; memset(&in, 0, sizeof in);
            in.player[0] = (uint16_t)((b1 << 8) | b2);
            in.system = sysb;
            for (int a=0;a<JVS_MAX_ANALOG;++a) in.analog[a]=(uint16_t)wr_steer_raw; /* center-ish */
            /* drive: accel held (in-range), steering centered, brake released -- all
             * within the NVRAM calibration ranges so the game accepts them. */
            if (ch_accel>=0 && ch_accel<JVS_MAX_ANALOG) in.analog[ch_accel]=(uint16_t)wr_accel_raw;
            if (ch_steer>=0 && ch_steer<JVS_MAX_ANALOG) in.analog[ch_steer]=(uint16_t)wr_steer_raw;
            if (ch_brake>=0 && ch_brake<JVS_MAX_ANALOG) in.analog[ch_brake]=(uint16_t)wr_brake_raw;
            in.screen_x = in.screen_y = 0x8000;
            g_xinput_in = in;
            tick++;
            Sleep(8);
            continue;
        }
        if (demo) {
            /* Chaos Breaker DEBUG MENU navigation via JVS Player-1 BYTE2 (RE-confirmed:
             * the menu reads the game's internal word @0x473726, fed from JVS node+0x185
             * = P1 byte2, where DOWN=bit4 0x10, UP=bit5 0x20, SELECT=bit2 0x04). byte2 is
             * the LOW byte of ultra_jvs_input.player[0]. Auto-repeat is bypassed in the
             * boot state, so drive DISCRETE one-frame edges. Sequence: 11x DOWN to reach
             * DEMO TEST, then SELECT, then idle (let the demo render) -- then repeat.
             * ULTRA_DEMO_SELECT=1 enables the SELECT press (off = just scroll, proves nav). */
            /* Drive clean menu DOWN: byte1 bit4 (0x10) -> internal 0x01 = cursor DOWN.
             * One-frame edges (~32ms press, ~0.35s gap) so each press = one move. The
             * probe logs cursor=[menuObj+0xc] -> watch it increment. byte1 = HIGH byte. */
            /* Auto-play: insert a coin, press START, then mash PUSH1 + move so the
             * game goes attract -> credit -> character select -> fight (gameplay).
             * Standard JVS byte1: START 0x80, UP 0x20, DOWN 0x10, LEFT 0x08, RIGHT 0x04,
             * PUSH1 0x02, PUSH2 0x01. Coin via g_local_coin (pull_input inserts it). */
            uint8_t b1 = 0;
            unsigned t = tick;
            if (t == 240u || t == 245u) InterlockedIncrement(&g_local_coin);   /* 2 coins ~2s */
            if (t >= 360u && t < 368u) b1 = 0x80;                               /* START ~3s */
            else if (t > 420u) {                                               /* then play */
                unsigned ph = (t / 45u) % 6u;
                if ((t % 45u) < 6u) {
                    if      (ph == 0u) b1 = 0x02;   /* PUSH1 (confirm/attack) */
                    else if (ph == 1u) b1 = 0x04;   /* RIGHT */
                    else if (ph == 2u) b1 = 0x02;   /* PUSH1 */
                    else if (ph == 3u) b1 = 0x08;   /* LEFT  */
                    else if (ph == 4u) b1 = 0x01;   /* PUSH2 */
                    else               b1 = 0x80;   /* START (advance) */
                }
            }
            ultra_jvs_input in; memset(&in, 0, sizeof in);
            in.player[0] = (uint16_t)(b1 << 8);
            for (int a=0;a<JVS_MAX_ANALOG;++a) in.analog[a]=0x8000;
            in.screen_x = in.screen_y = 0x8000;
            g_xinput_in = in;
            tick++;
            Sleep(8);
            continue;
        }
        ul_xstate st; memset(&st, 0, sizeof st);
        static WORD s_pm_prev_btn = 0;
        /* Keyboard fallback (F1 / Esc / P) fires regardless of controller presence;
         * cheap edge-detect owned by pause_menu_wire.c. */
        ultra_pausemenu_kbd_edge_check();
        /* Service the overlay's own message pump on THIS thread every tick --
         * pause_menu_pump() lazily creates the overlay window on first request
         * and PeekMessage's the CALLING thread's queue, so it must be pumped
         * from the same thread consistently. Was declared/defined but never
         * actually called anywhere in this tree until now -- the overlay could
         * be requested and torn down but never drew or took input. */
        ultra_pausemenu_pump();
        if (s_xget(0, &st) == 0) {                 /* controller 0 present */
            WORD b = st.pad.wButtons;
            /* pause-menu combo detect BEFORE we map the bits into JVS byte1/byte2:
             * on Start+Select or LB+RB edge, request the shared overlay. */
            ultra_pausemenu_edge_check(b, s_pm_prev_btn);
            s_pm_prev_btn = b;
            int up=!!(b&0x0001), dn=!!(b&0x0002), lf=!!(b&0x0004), rt=!!(b&0x0008);
            int start=!!(b&0x0010), test=!!(b&0x0020) /*BACK*/, svc=!!(b&0x0100) /*LB*/;
            int p1=!!(b&0x1000)/*A*/, p2=!!(b&0x2000)/*B*/, p3=!!(b&0x4000)/*X*/,
                p4=!!(b&0x8000)/*Y*/, p5=!!(b&0x0200)/*RB*/, p6=!!(b&0x0080)/*R3*/;
            if (st.pad.lx < -10000) lf = 1; else if (st.pad.lx > 10000) rt = 1;
            if (st.pad.ly >  10000) up = 1; else if (st.pad.ly < -10000) dn = 1;
            uint8_t byte1 = (uint8_t)((start<<7)|(svc<<6)|(up<<5)|(dn<<4)|(lf<<3)|(rt<<2)|(p1<<1)|p2);
            /* byte2: standard extra buttons (p3..p6) PLUS Chaos Breaker DEBUG-MENU nav
             * bits so the gamepad also drives that menu (RE: byte2 0x20=up/0x10=down/
             * 0x04=select read at node+0x185). dpad up->0x20, down->0x10, A(p1)->0x04. */
            uint8_t byte2 = (uint8_t)((p3<<7)|(p4<<6)|(p5<<5)|(p6<<4)
                                      | (up?0x20:0) | (dn?0x10:0) | (p1?0x04:0));
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

/* The input the game should see: the loader's shared block when a loader drives
 * it, otherwise the local gamepad (XInput). Used by the JVS engine AND the
 * DirectInput8 emulation so both report the same controller state. */
/* Put a loader-published GENERIC axis onto the range THIS game calibrates.
 *
 * The loader cannot know which game is about to run, so ULTRA_IO_SHM carries a
 * plain full-scale axis: 0..65535 with 0x8000 as center. Real cabinets are not
 * full-scale. Wacky Race's own NVRAM (sv/WR0.CFG) calibrates steering as
 * iostmin=4000 iostmax=64000, so its center is 34000 -- handing the game a raw
 * 0x8000 (32768) is 1232 counts left of center on EVERY frame. The game has no
 * way to null that out, and in play it reads as a wheel permanently turning
 * left. The board profile already carries the correct floor/ceiling per channel;
 * this just puts the published value through them.
 *
 * `raw` == the channel's own rest position maps to the range's rest position,
 * so a centered wheel lands mid-range and a released pedal lands on the floor. */
static uint16_t shm_axis_to_profile(int ch, uint16_t raw) {
    uint16_t lo = 0, hi = 0; int bip = 1;
    if (!ultra_input_map_channel_range(ch, &lo, &hi, &bip)) return raw;  /* unbound */
    /* 65535*65535 still fits in uint32, so this needs no 64-bit maths. */
    return (uint16_t)(lo + (uint32_t)raw * (uint32_t)(hi - lo) / 65535u);
}

/* Where a channel sits when NOTHING is driving it.
 *
 * The loader publishes 0x8000 as a placeholder on every channel it has no
 * device for, which is indistinguishable from a real centered wheel. For a
 * steering axis that placeholder is right; for a pedal it is a half-pressed
 * accelerator, and a driving game will pull away on its own. So an unowned
 * channel takes the profile's REST end of its own range instead. */
static uint16_t profile_rest_axis(int ch) {
    uint16_t lo = 0, hi = 0; int bip = 1;
    if (!ultra_input_map_channel_range(ch, &lo, &hi, &bip)) return 0x8000;
    return bip ? (uint16_t)((lo + hi) / 2) : lo;
}

/* Periodic snapshot of what the game is ACTUALLY being handed.
 *
 * Reading the analog values back out of the JVS wire log means re-deriving the
 * frame layout from the request, and a single mis-parsed command shifts every
 * channel by a byte - which produced three separate wrong diagnoses of the
 * Wacky Race steering. Logging the values at the point we set them removes the
 * guesswork entirely. ~1 line/sec at a 60Hz poll; costs nothing. */
static void ultra_log_input_snapshot(const ultra_jvs_input* in) {
    static unsigned n = 0;
    if ((n++ % 60) != 0) return;
    {   /* jvs_diag() is file-static in jvs_serial_hook.c, so write to the same
         * ULTRA_JVS_LOG file directly rather than exporting it. */
        const char* p = getenv("ULTRA_JVS_LOG");
        FILE* f;
        if (!p || !*p) return;
        f = fopen(p, "a");
        if (!f) return;
        fprintf(f, "INPUT: sys=%02X p1=%04X | ch0=%u ch1=%u ch2=%u ch3=%u ch4=%u ch5=%u\n",
                in->system, in->player[0],
                in->analog[0], in->analog[1], in->analog[2],
                in->analog[3], in->analog[4], in->analog[5]);
        fclose(f);
    }
}

void ultra_hook_current_input(ultra_jvs_input* out) {
    memset(out, 0, sizeof *out);
    for (int a = 0; a < JVS_MAX_ANALOG; ++a) out->analog[a] = 0x8000;
    out->screen_x = out->screen_y = 0x8000;

    /* The profile, when one is active, is the ONLY thing that knows which JVS
     * channel each control belongs on for THIS game, so it drives both the
     * switch bits and the axes it binds.
     *
     * The shared block cannot do that job: it publishes analog[0]=steer,
     * [1]=accel, [2]=brake and the hook copies analog[a] onto channel a
     * positionally. Wacky Race reads its STEERING from channel 2, ACCEL from
     * channel 3 and BRAKE from channel 4 (its READ_ANALOG handler stores the
     * channels at obj+0x34.. and the copy at 0x49BC47 shifts them two places:
     * [+0x0C]<-[+0x38], [+0x0E]<-[+0x3A], [+0x10]<-[+0x3C]). Positionally
     * copied, our steering landed on channel 0 where the game never looks,
     * while its steering channel received our BRAKE value - a constant. That is
     * why the wheel did not respond, and why it sat at full lock back when that
     * channel still carried 0x0000. */
    int have_map = ultra_input_map_active();
    ultra_jvs_input pm;
    if (have_map) ultra_input_map_current(&pm);

    ultra_io_shared* s = g_shared;
    if (s && s->magic == ULTRA_IO_MAGIC) {
        ultra_io_shared snap; int got = 0;
        for (int attempt = 0; attempt < 8; ++attempt) {
            uint32_t s1 = s->seq; MemoryBarrier(); snap = *s; MemoryBarrier();
            uint32_t s2 = s->seq; if (s1 == s2 && !(s1 & 1u)) { got = 1; break; }
        }
        if (!got) snap = *s;
        out->system = snap.system;
        for (int p = 0; p < ULTRA_IO_PLAYERS && p < JVS_MAX_PLAYERS; ++p)
            out->player[p] = snap.player[p];
        uint32_t owned = ULTRA_IO_ANALOG_OWNED(&snap);
        if (!owned) owned = ~0u;
        for (int a = 0; a < ULTRA_IO_ANALOG && a < JVS_MAX_ANALOG; ++a) {
            uint16_t lo = 0, hi = 0; int bip = 1;
            if (have_map && ultra_input_map_channel_range(a, &lo, &hi, &bip))
                out->analog[a] = pm.analog[a];          /* profile owns this channel */
            else if (owned & (1u << a))
                out->analog[a] = shm_axis_to_profile(a, snap.analog[a]);
            else
                out->analog[a] = profile_rest_axis(a);
        }
        out->screen_x = snap.screen_x; out->screen_y = snap.screen_y;
        if (have_map) {
            /* The profile is authoritative for GAME buttons, but it must not
             * throw away the loader's DIRECTIONS.
             *
             * 2026-09-13: this used to be a plain overwrite, which discarded
             * everything the loader published into ULTRA_IO_SHM. Profile
             * tables commonly bind up/down/left/right from the keyboard
             * only, so on a gamepad the d-pad was dead - no menu navigation
             * at all - even though the loader published it correctly. Found
             * on namco_es3 with Mario Kart GP DX; this file has the same
             * code, so it has the same bug.
             *
             * Only the four direction bits are merged. A blanket OR is NOT
             * safe: the loader maps L3 to SERVICE while the generic profile
             * maps the same physical button to COIN, so OR-ing everything
             * would insert a credit AND poke service on one press.
             * UP 1<<13  DOWN 1<<12  LEFT 1<<11  RIGHT 1<<10 (JVS switch word) */
            const uint16_t UL_DIRS = (1u << 13) | (1u << 12) | (1u << 11) | (1u << 10);
            for (int p = 0; p < ULTRA_IO_PLAYERS && p < JVS_MAX_PLAYERS; ++p)
                out->player[p] = pm.player[p] | (snap.player[p] & UL_DIRS);
            /* ONE OWNER FOR THE WHOLE INPUT, NOT HALF OF IT: the profile can
             * express both system bits it could collide on (controls.c:
             * TEST 0x80, TILT 0x40), so it ASSIGNS the byte instead of OR-ing
             * it. OR-ing let the loader's TEST survive on top of whatever the
             * profile put on that button - R3 rang SERVICE and TEST at once
             * and the test/service menu was unreachable. Gated on
             * ultra_input_map_active(): a title with no container map takes
             * have_map == 0 and behaves exactly as before. */
            out->system = pm.system;
        }
    } else if (have_map) {
        *out = pm;
    } else {
        *out = g_xinput_in;                        /* standalone gamepad (legacy) */
    }
    ultra_log_input_snapshot(out);
}

/* Name every credit as it is inserted, with the path that produced it.
 * There are three independent coin sources in this file and a double-credit
 * is otherwise indistinguishable from the game counting one coin twice. */
static void ultra_log_coin(const char* src, int slot, int delta, unsigned total) {
    const char* p = getenv("ULTRA_JVS_LOG");
    FILE* f;
    if (!p || !*p) return;
    f = fopen(p, "a");
    if (!f) return;
    fprintf(f, "COIN: +%d slot=%d total=%u via %s\n", delta, slot, total, src);
    fclose(f);
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
                ultra_log_coin("SHM/coin_pulse", c, delta,
                               (unsigned)jvs_coin_count(n, c));
                /* mirror the coin to the Sega IO4 counters when that path is armed
                 * (the IO4 reports its own counters; a Sega title reads those, not
                 * the JVS node). No-op + linked-out when the IO4 shim is OFF. */
                if (ultra_sega_io4_enabled()) ultra_sega_io4_insert_coin(c, delta);
                g_coin_seen[c] = pulse; s->coin_ack[c] = pulse;
            }
        }
        s->jvs_address = jvs_address(n); s->poll_count++;
    }
    if (ultra_input_map_active() && !(s && s->magic == ULTRA_IO_MAGIC)) {
        /* ONE owner per coin slot, never two.
         *
         * This layer keeps its own monotonic per-slot coin counters, and the
         * shared block above keeps another. When a loader is driving, BOTH were
         * draining into jvs_insert_coin for the same physical press, so a single
         * tap on the credit button could register as several credits. The block
         * above already owns the coin path whenever a loader is present, so this
         * one only runs standalone. */
        for (int slot = 0; slot < ULTRA_IO_COINS && slot < JVS_MAX_PLAYERS; ++slot) {
            uint32_t cm = ultra_input_map_coin(slot);
            if (cm != g_map_coin_seen[slot]) {
                int delta = (int)(cm - g_map_coin_seen[slot]);
                jvs_insert_coin(n, slot, delta);
                ultra_log_coin("profile/map_coin", slot, delta,
                               (unsigned)jvs_coin_count(n, slot));
                if (ultra_sega_io4_enabled()) ultra_sega_io4_insert_coin(slot, delta);
                g_map_coin_seen[slot] = cm;
            }
        }
    }
    long lc = g_local_coin;                        /* gamepad / auto-pilot coin (standalone) */
    if (lc != g_local_coin_seen) {
        /* Credit the first chute (slot 0). The game READ_COIN reads slot 0 and its
         * COIN_DECREASE slot=1 maps to coin[0] via the 1-based->0-based fix in
         * jvs_core.c, so a credit inserted here is the one the game spends. */
        int delta = (int)(lc - g_local_coin_seen);
        jvs_insert_coin(n, 0, delta);
        ultra_log_coin("legacy/g_local_coin", 0, delta,
                       (unsigned)jvs_coin_count(n, 0));
        if (ultra_sega_io4_enabled()) ultra_sega_io4_insert_coin(0, delta);
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
    /* Advertised analog-channel count. Default 8 (fighters/shooters); driving
     * cabinets poll more (Wacky Race = 10), so let the auto-pilot / loader bump it
     * via ULTRA_JVS_ANALOG so FEATCHK matches the game's READ_ANALOG count. */
    uint8_t analog_ch = 0;   /* 0 = keep node default (8) */
    const char* ach = getenv("ULTRA_JVS_ANALOG");
    if (ach && *ach) { int v = atoi(ach); if (v > 0 && v <= JVS_MAX_ANALOG) analog_ch = (uint8_t)v; }
    /* defaults (2p/13sw/2coin/8analog@16/2gpo) + optional light-gun feature */
    jvs_set_features(g_node, 0, 0, 0, analog_ch, 0, 0, (uint8_t)screenpos);
}

/* Re-patch worker: catches modules the game loads AFTER us (delay-loaded
 * imports, render/audio plugins). The first install already happened
 * synchronously in DllMain, so this only re-points IAT slots periodically. */
static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        ultra_jvs_serial_install();   /* idempotent (name + value based) */
        ultra_dinput8_install();      /* idempotent */
        konami_render9_install();     /* idempotent; retries until the game window exists */
    }
    return 0;
}

/* ---- crash diagnostics (VEH) -------------------------------------------- */
/* Is this range safe to read? Inside a VEH we must never provoke a second
 * fault, and mingw GCC has no __try, so ask the memory manager instead. */
static int ultra_readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p) return 0;
    if (VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    /* must not run past the end of the committed region */
    return ((const char *)p + n) <=
           ((const char *)mbi.BaseAddress + mbi.RegionSize);
}

/* Name the fatal exception codes we care about. An access violation is not
 * the only way a game dies: Silent Hill executes heap data and raises
 * 0xC0000096, which this handler used to ignore completely. */
static const char *ultra_exc_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
    case EXCEPTION_PRIV_INSTRUCTION:      return "PRIVILEGED_INSTRUCTION";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
    case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    default:                              return 0;
    }
}

/* How many fault reports this handler will write before it goes quiet.
 *
 * This is not tidiness. Silent Hill's render path faults ONCE PER FRAME by
 * design -- shaiolib_crk.c carries recovery handlers for the exact addresses
 * and longjmps back into the render loop, so a fault here is an expected,
 * handled event, not a death. Reporting every one of them wrote a 227 MB,
 * 4,360,778-line log in a single run: 28,557 identical copies of one fault,
 * plus the disk churn of writing them, on the render thread's critical path.
 * The log was so large it buried the 130 lines that actually mattered and made
 * the hang measurably worse.
 *
 * A fault we have already described N times tells us nothing the first one did
 * not. Report the first few in full, then say so once and stop. */
#define ULTRA_FAULT_REPORT_MAX 12
static LONG ultra_fault_reports = 0;

static LONG WINAPI ultra_crash_handler(EXCEPTION_POINTERS *ep) {
    const char *exc_name = ultra_exc_name(ep->ExceptionRecord->ExceptionCode);
    if (exc_name) {
        LONG seen = InterlockedIncrement(&ultra_fault_reports);
        if (seen > ULTRA_FAULT_REPORT_MAX) {
            /* Announce the cut-off exactly once, then never write again. */
            if (seen == ULTRA_FAULT_REPORT_MAX + 1) {
                extern void ultra_d3d8_log(const char* fmt, ...);
                ultra_d3d8_log("FAULT: %d reports written -- suppressing the rest. "
                               "A repeating fault is the render loop recovering, "
                               "not new information; see the shaiolib log for the "
                               "per-frame view.", ULTRA_FAULT_REPORT_MAX);
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    if (exc_name) {
        extern void ultra_d3d8_log(const char* fmt, ...);
        extern void ultra_boot_trace_stack(const char*);
        void *fa = ep->ExceptionRecord->ExceptionAddress;
        ultra_d3d8_log("FAULT: %s (0x%08lX) at %p",
                       exc_name,
                       (unsigned long)ep->ExceptionRecord->ExceptionCode, fa);

        /* WHAT the CPU was asked to execute. A privileged instruction is
         * readable straight off the opcode, and garbage reached by jumping
         * into data looks obviously unlike compiled code. */
        {
            unsigned char *p = (unsigned char *)fa;
            char hex[80]; int i, n = 0;
            if (ultra_readable(p, 16)) {
                for (i = 0; i < 16 && n < (int)sizeof hex - 4; ++i)
                    n += sprintf(hex + n, "%02X ", p[i]);
                ultra_d3d8_log("FAULT: bytes at EIP: %s", hex);
            } else {
                ultra_d3d8_log("FAULT: bytes at EIP unreadable "
                               "(not committed, or no read access)");
            }
        }

        /* WHERE that memory came from. This is what settles whether the
         * address really is inside the DirectSound object logged moments
         * earlier, or merely in the same heap neighbourhood. */
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(fa, &mbi, sizeof mbi) == sizeof mbi) {
                ultra_d3d8_log("FAULT: region base=%p size=%lu state=0x%lX "
                               "protect=0x%lX type=0x%lX (offset +0x%lX)",
                               mbi.AllocationBase,
                               (unsigned long)mbi.RegionSize,
                               (unsigned long)mbi.State,
                               (unsigned long)mbi.Protect,
                               (unsigned long)mbi.Type,
                               (unsigned long)((char*)fa - (char*)mbi.AllocationBase));
            }
        }

        /* WHO called. Walk the FAULTING thread's frame chain, not ours. */
#if !defined(_WIN64)
        {
            extern void ultra_boot_trace_where(void*, char*, unsigned);
            DWORD *fp = (DWORD *)ep->ContextRecord->Ebp;
            char w[320];
            int depth;
            ultra_d3d8_log("FAULT: EIP=%08lX ESP=%08lX EBP=%08lX",
                           (unsigned long)ep->ContextRecord->Eip,
                           (unsigned long)ep->ContextRecord->Esp,
                           (unsigned long)ep->ContextRecord->Ebp);
            /* STACK SCAN. The frame chain cannot reach the immediate caller
             * here, but its return address is still on the stack. Walk
             * upward from ESP and print every value that lands inside a
             * loaded module; the first few ARE the call path. */
            {
                DWORD *sp = (DWORD *)ep->ContextRecord->Esp;
                int i, shown = 0;
                for (i = 0; i < 128 && shown < 12; ++i) {
                    void *v;
                    HMODULE hm = NULL;
                    if (!ultra_readable(sp + i, sizeof(DWORD))) break;
                    v = (void *)sp[i];
                    if ((ULONG_PTR)v < 0x10000) continue;
                    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                            (LPCSTR)v, &hm) || !hm) continue;
                    ultra_boot_trace_where(v, w, sizeof w);
                    ultra_d3d8_log("FAULT: stack[+0x%03X] %s", (unsigned)(i * 4), w);
                    ++shown;
                }
                if (!shown)
                    ultra_d3d8_log("FAULT: no module addresses found on the stack");
            }

            for (depth = 0; depth < 12; ++depth) {
                void *ret;
                DWORD *next;
                if (!ultra_readable(fp, sizeof(DWORD) * 2)) {
                    ultra_d3d8_log("FAULT: frame chain ends at %p (unreadable)", (void*)fp);
                    break;
                }
                ret  = (void *)fp[1];
                next = (DWORD *)fp[0];
                if (!ret) break;
                ultra_boot_trace_where(ret, w, sizeof w);
                ultra_d3d8_log("FAULT: caller[%02d] %s", depth, w);
                if (next <= fp) break;          /* a frame chain must ascend */
                fp = next;
            }
        }
#endif
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        extern void ultra_d3d8_log(const char* fmt, ...);
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "(unknown)";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
#ifdef _WIN64
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
        ultra_d3d8_log("CRASH: ACCESS_VIOLATION at RIP=%p in %s", addr, modname);
        ultra_d3d8_log("CRASH: RAX=%p RBX=%p RCX=%p RDX=%p",
            (void*)ep->ContextRecord->Rax, (void*)ep->ContextRecord->Rbx,
            (void*)ep->ContextRecord->Rcx, (void*)ep->ContextRecord->Rdx);
#else
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
        ultra_d3d8_log("CRASH: ACCESS_VIOLATION at EIP=%p in %s", addr, modname);
        ultra_d3d8_log("CRASH: EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX",
            ep->ContextRecord->Eax, ep->ContextRecord->Ebx,
            ep->ContextRecord->Ecx, ep->ContextRecord->Edx);
        ultra_d3d8_log("CRASH: ESP=%08lX EBP=%08lX ESI=%08lX EDI=%08lX",
            ep->ContextRecord->Esp, ep->ContextRecord->Ebp,
            ep->ContextRecord->Esi, ep->ContextRecord->Edi);
        { extern void ultra_boot_trace_stack(const char*);
          ultra_boot_trace_stack("CRASH"); }
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
            utxp.board        = "native_x86";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        AddVectoredExceptionHandler(1, ultra_crash_handler);
        /* Maximum-information bring-up trace (ULTRA_BOOT_TRACE=1). Installed
         * AFTER the crash handler so a fault during its own install is still
         * caught, and it costs nothing when the env var is unset. */
        { extern void ultra_boot_trace_install(void);
          extern void ultra_boot_trace_start_beat(void);
          ultra_boot_trace_install();
          ultra_boot_trace_start_beat(); }
        /* Per-game module claim happens automatically before we even get here:
         * each linked game_*.c file's own uniquely-named __attribute__((constructor))
         * function runs at DLL load (before the CRT calls this DllMain) and
         * decides FOR ITSELF whether this process is its target game. A board
         * module that links no game_*.c file has nothing register -- byte-
         * identical to today. See board_io_ops.h for the full mechanism. */
        /* Install SYNCHRONOUSLY here: the module injects us into the SUSPENDED
         * game and resumes right after, so the hooks must be in place before the
         * game's first thread runs (else its first CreateFile(COM) is missed). */
        open_shared();
        configure_node();
        InterlockedExchange(&g_node_ready, 1);
        ultra_jvs_serial_install();
        ultra_dongle_install();
        ultra_dinput8_install();                         /* synthetic DInput8 device */
        /* Legacy exit-gate probe/poke (Chaos Breaker): only start it if NO
         * per-game module claimed this process. Once game_chaosbreaker.c is
         * verified equivalent, this call (and gamestate_probe.c) can be deleted
         * outright; until then this guard just prevents it from double-poking
         * the same memory alongside a registered ops' on_install thread. */
        if (!ultra_board_ops_get()) ultra_gamestate_probe_start();
        ultra_typex2_gun_install();                      /* Type X2 gun->cursor bridge (env-gated) */
        /* Ultra Omni-Bridge: present in this tree (omni_bridge.c) but never
         * actually installed. get_pose is the real adapter (typex2_gun.c
         * already tracks a live Omni-Track pose); stays fully inert unless
         * ULTRA_BRIDGE_ENABLE=1. */
        ultra_omni_bridge_install(ultra_typex2_device_window, ultra_typex2_gun_pose);
        /* Unified board-profile scaffolding -- all DEFAULT-OFF so the Type X / X2
         * path is byte-identical. The board profile (board_profile.c) sets the
         * ULTRA_* knobs below for the resolved board; nothing here arms unless a
         * knob is set. No dump = these are inert scaffolding.
         *   - D3D11 present-throttle: ULTRA_D3D11 (vendor-neutral, set by any D3D11
         *     board profile) OR ULTRA_SEGA_D3D11 (legacy alias). Either arms it.
         *   - Sega keychip shim: ULTRA_SEGA_KEYCHIP (Sega keychip boards).
         *   - Taito Fast-I/O (iDmacDrv) shim: ULTRA_FAST_IO (Type X3/X4 profiles).
         *   - NESiCA/Aime card-reader stub: ULTRA_CARD_READER (NESiCA/ALLS/Bemani). */
        { const char* d  = getenv("ULTRA_D3D11");
          const char* sd = getenv("ULTRA_SEGA_D3D11");
          if ((d && d[0]=='1') || (sd && sd[0]=='1')) ultra_d3d11_install(); }
        /* OpenGL present-throttle (Namco N2 + any GL board): ULTRA_GL_PRESENT
         * (vendor-neutral) OR ULTRA_GL_BOARD (board-profile alias) arms it. We do NOT
         * arm it on the shared ULTRA_TYPEX2_AUTO flag: a Type X2 title is D3D9 and
         * handles its own force-windowed via the D3D9 PresentationParameters path, so
         * arming the GL ChangeDisplaySettings swallow there could perturb the proven
         * Type X2 lane. ULTRA_SEGA_WINDOWED is the deliberate GL force-windowed knob. */
        { const char* g  = getenv("ULTRA_GL_PRESENT");
          const char* gb = getenv("ULTRA_GL_BOARD");
          const char* sw = getenv("ULTRA_SEGA_WINDOWED");
          if ((g && g[0]=='1') || (gb && gb[0]=='1') || (sw && sw[0]=='1'))
              ultra_gl_install(); }
        ultra_sega_keychip_install();                    /* internally env-gated (OFF by default) */
        ultra_taito_fastio_install();                    /* internally env-gated (OFF by default) */
        ultra_card_reader_install();                     /* internally env-gated (OFF by default) */
        ultra_sega_io4_install();                        /* Sega IO4-USB I/O (env-gated, OFF) */
        ultra_konami_io_install();                       /* Konami ACIO I/O (env-gated, OFF) */
        ultra_namco_amdaemon_install();                  /* Namco ES local amdaemon (env-gated, OFF) */
        ultra_namco_io_install();                        /* Namco ES JVS-over-USB I/O + amauthd (env-gated, OFF) */
        ultra_namco_dongle_install();                    /* Namco ES3 USB dongle bypass (env-gated, OFF) */
        ultra_rawthrills_io_install();                   /* Raw Thrills JammaUSB factory (env-gated, OFF) */
        ultra_audio_install();                           /* DirectSound + XAudio2 diagnostic hook (always-on, passthrough) */
        konami_audio_install();                          /* this board's own DirectSound copy */
        konami_render9_install();                        /* output-window control only; no device hook */
        ultra_input_map_install();                       /* player input-mapping layer (controls.c); active only on a profile/config or ULTRA_CONTROLS=1 */
        ultra_xinput_start();                            /* standalone gamepad -> JVS/DInput (legacy fallback + autopilot host) */
        CreateThread(NULL, 0, worker, NULL, 0, NULL);   /* periodic re-patch only */
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        /* Stop the Type X2 gun/auto threads BEFORE unmapping the shared block they
         * read (and force-release the OS mouse button) so an explicit FreeLibrary
         * unload can't leave them spinning against an unmapped page or a latched
         * button. Process-teardown DETACH (reserved!=NULL) tears the whole address
         * space down at once, so this is the belt for the rare orderly-unload. */
        ultra_typex2_gun_stop();
        ultra_omni_bridge_stop();
        ultra_pausemenu_shutdown();  /* destroy the pause-menu overlay window if any */
        ultra_d3d8_teardown();   /* timeEndPeriod(1) if the limiter raised it */
        ultra_d3d11_teardown();  /* pair the d3d11 limiter's timeBeginPeriod, if armed */
        if (g_shared) { UnmapViewOfFile(g_shared); g_shared = NULL; }
        if (g_map)    { CloseHandle(g_map); g_map = NULL; }
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader can confirm the right DLL was injected. */
__declspec(dllexport) const char* ultra_typex_hook_id(void) {
    return "Ultra Loader Type X / PC-arcade I/O hook (clean-room JVS) v1";
}

#endif /* _WIN32 */
