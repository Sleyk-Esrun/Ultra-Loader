/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  game_wackyrace.c  --  Taito Type X2 "Wacky Race" racing auto-pilot
 * =============================================================================
 *
 *  Extracted out of hook_dllmain.c's shared xinput_thread body (the
 *  ULTRA_WR_AUTO branch) into its OWN isolated game_*.c module. See
 *  board_io_ops.h for the full rationale: a per-game hack (this one) living
 *  directly inside a shared dispatcher function could -- and did -- regress
 *  OTHER, unrelated boards built from the same source tree (Cave PC, Konami
 *  PC, ...). Moving it here means a board module that does not link this file
 *  cannot possibly run this code or be affected by a bug in it; the isolation
 *  is enforced at the source/build level.
 *
 *  A passive auto-throttle racer that drives the whole credit loop over JVS:
 *    coin -> START through credit/car/stage select -> race with accel held +
 *    gentle steer -> finish/game-over -> attract.
 *  The INPUT CHECK screen (game.exe strings) lists the inputs as:
 *    digital: TEST / SERVICE / VIEW / START    (START advances every menu)
 *    analog : BRAKE / ACCEL / STEERING         (READ_ANALOG channels)
 *  Coin is the JVS coin counter (slot 0). Channel order is wheel/accel/brake
 *  on the first analog channels (overridable via ULTRA_WR_CH_*).
 *
 *  Gating: opt-in ONLY. Today there is no exe-identity sniff for Wacky Race
 *  (unlike Chaos Breaker's AOE_CHAOS_BREAKER AddressOfEntryPoint gate in
 *  gamestate_probe.c) -- ultra_board_ops_autoregister() below simply checks
 *  ULTRA_WR_AUTO=1, exactly what the original inline branch in hook_dllmain.c
 *  checked. Default-off, so a plain render/loader run is unchanged.
 *
 *  autopilot_tick() is called once per tick (~8ms / 125Hz) by the generic
 *  xinput_thread in hook_dllmain.c, which still owns pacing (Sleep) and
 *  shutdown (g_stop) -- see board_io_ops.h. This callback only decides WHAT
 *  the game should see this tick and pushes it via ultra_board_ops_set_input()
 *  / ultra_board_ops_insert_coin(); it never touches g_xinput_in/g_local_coin
 *  directly (board_io_ops.c forwards those two setters into hook_dllmain.c's
 *  statics on our behalf).
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "jvs_core.h"
#include "board_io_ops.h"

/* Window handle getter owned by typex2_gun.c. Not declared in any shared
 * header -- every existing caller (hook_dllmain.c, pause_menu_wire.c) extern-
 * declares it locally at the point of use, so we follow the same convention
 * here rather than inventing a new header dependency. Used only for the
 * optional ULTRA_WR_FOCUS foreground poke. */
extern HWND ultra_typex2_device_window(void);

/* ---- one-time setup (channel assignment + NVRAM calibration) --------------
 * In the original inline branch these were read ONCE before the xinput_thread
 * while-loop started (not per-tick). board_io_ops.h's on_install() is the
 * documented seam for that ("called once ... before ultra_jvs_serial_install()"),
 * so we wire it there -- but ALSO make wr_setup_once() self-triggering from the
 * first autopilot_tick() call, because nothing in the current tree actually
 * invokes ops->on_install() yet (grepped: board_io_ops.h declares the member,
 * hook_dllmain.c's DllMain calls ultra_board_ops_autoregister() but never
 * ops->on_install()). Idempotent, so it is harmless if that gap gets fixed
 * later and on_install() starts firing too. */
static int      g_wr_setup_done = 0;

/* channel assignment (defaults: steer=0, accel=1, brake=2; tune per probe) */
static int      g_ch_steer = 0, g_ch_accel = 1, g_ch_brake = 2;

/* Analog levels must fall INSIDE the game's NVRAM calibration ranges (WR0.CFG):
 *   ACCEL  ioac[min,max] = [26000,44000]  -> full gas = ioacmax = 44000 (0xABE0)
 *   BRAKE  iobr[min,max] = [28000,47000]  -> brake off = iobrmin = 28000 (0x6D60)
 *   STEER  iost[min,max] = [ 4000,64000]  -> center    = midpoint = 34000 (0x84D0)
 * Feeding a raw outside the range (e.g. 0xFFFF accel = 65535 > 44000) reads as an
 * out-of-range / uncalibrated axis, which (with the calib flags) the game rejects.
 * All overridable so a different cabinet/NVRAM can be matched. */
static unsigned g_wr_accel_raw = 44000;   /* ioacmax: full throttle */
static unsigned g_wr_steer_raw = 34000;   /* (iostmin+iostmax)/2: center */
static unsigned g_wr_brake_raw = 28000;   /* iobrmin: brake released */

static void wr_setup_once(void) {
    if (g_wr_setup_done) return;
    g_wr_setup_done = 1;
    const char* e;
    if ((e = getenv("ULTRA_WR_CH_STEER")) && *e) g_ch_steer = atoi(e);
    if ((e = getenv("ULTRA_WR_CH_ACCEL")) && *e) g_ch_accel = atoi(e);
    if ((e = getenv("ULTRA_WR_CH_BRAKE")) && *e) g_ch_brake = atoi(e);
    if ((e = getenv("ULTRA_WR_ACCEL")) && *e) g_wr_accel_raw = (unsigned)strtoul(e, NULL, 0) & 0xFFFF;
    if ((e = getenv("ULTRA_WR_STEER")) && *e) g_wr_steer_raw = (unsigned)strtoul(e, NULL, 0) & 0xFFFF;
    if ((e = getenv("ULTRA_WR_BRAKE")) && *e) g_wr_brake_raw = (unsigned)strtoul(e, NULL, 0) & 0xFFFF;
}

/* ---- per-tick state --------------------------------------------------------
 * Were function-local statics inside the shared xinput_thread's wr_auto
 * branch (lazily initialized the first tick each guard was reached); now
 * file-scope statics of this isolated module, same lazy-init pattern. */
static unsigned g_tick       = 0;    /* our own tick counter (~8ms/tick, 125Hz) */
static int      g_coin_amt   = 0;    /* ULTRA_WR_COIN_AMT, default 2 (see below) */
static int      g_focus      = -1;   /* ULTRA_WR_FOCUS, -1 = not yet resolved */
static int      g_rotate_mode = -1;  /* ULTRA_WR_ROTATE, -1 = not yet resolved */
static int      g_forced = -1, g_hold = 0, g_gap = 0;  /* rotate-mode overrides */

/* candidate action bits for rotate mode: START then the VIEW/DECIDE bits */
static const uint8_t WR_ROTATE_CAND[] = { 0x80, 0x20, 0x10, 0x08, 0x04, 0x02 };

/* -----------------------------------------------------------------------------
 * autopilot_tick: builds the ultra_jvs_input for THIS tick and pushes it.
 * Tick is ~8ms (125 Hz), driven by the generic xinput_thread loop, which owns
 * Sleep(8)/g_stop -- we only decide values and push them.
 *
 * The board converts an inserted coin straight to a credit + auto-debits the
 * income meter, so a single one-shot coin can be gone before START is pressed.
 * So we keep a credit topped up: insert a coin every ~3s (until START commits a
 * race), and pulse START on a steady cadence from boot. VIEW (PUSH1, byte1
 * bit1) is pulsed out of phase with START to confirm car/stage selects. Driving
 * axes held: accel full, steer center, brake off -- a passive auto-throttle
 * racer that plays a stage through to the finish/game-over and back to attract.
 * -----------------------------------------------------------------------------
 */
static void wr_autopilot_tick(void) {
    wr_setup_once();

    uint8_t b1 = 0, b2 = 0, sysb = 0;
    unsigned t = g_tick;

    /* Wacky Race charges 2 coins per game (COIN_DECREASE amount=2 = GAME START
     * CREDIT). Topping up only 1 coin per cycle never reached the 2-coin
     * threshold after the first debit, so the game could never start. Insert a
     * PAIR of coins each top-up, every ~2s, for the first ~30s, so a full
     * credit is always available for START to commit. (ULTRA_WR_COIN_AMT
     * overrides the pair count.) */
    if (!g_coin_amt) {
        const char* e = getenv("ULTRA_WR_COIN_AMT");
        g_coin_amt = (e && *e) ? atoi(e) : 2;
        if (g_coin_amt < 1) g_coin_amt = 1;
    }
    if (t >= 250u && t < 3800u && (t % 250u) == 0u)
        for (int k = 0; k < g_coin_amt; ++k) ultra_board_ops_insert_coin(0);

    /* Optional: foreground the game so the front-buffer dump is bright (the
     * capture grabs the whole desktop; a backgrounded window dims). DEFAULT
     * OFF -- repeatedly stealing foreground can disturb the run, and a clean
     * capture is better achieved by closing other windows. ULTRA_WR_FOCUS=1
     * enables it (every ~2s) for a manual brightness check. */
    if ((t % 250u) == 30u) {
        if (g_focus < 0) { const char* e = getenv("ULTRA_WR_FOCUS"); g_focus = (e && e[0] == '1'); }
        if (g_focus) {
            HWND w = ultra_typex2_device_window();
            if (w) SetForegroundWindow(w);
        }
    }

    /* NOTE on the original's "Cave PC: always bring the game window to front"
     * block (it lived right here, gated on getenv("ULTRA_CAVE_IO") at
     * (t%250u)==31u): DELIBERATELY DROPPED, not carried over. ULTRA_CAVE_IO is
     * Cave PC's own env var (set by cave_pc_main.c to switch jvs_serial_hook.c
     * onto Cave's proprietary COM3 protocol) -- it has no connection to Wacky
     * Race / Type X2 and is read nowhere else in this file. Wacky Race and Cave
     * PC are different boards/modules that are never both live in the same
     * injected process, so the check was always inert in practice; keeping it
     * here would just re-plant, in the NEW isolated file, the exact kind of
     * cross-board cross-reference this whole board_io_ops extraction exists to
     * eliminate. Confirmed independently: input_autopilot.c's separate
     * from-scratch re-implementation of this same WR racer (used when the
     * input-mapping layer is active) never picked this check up either. If a
     * "Wacky Race board should always foreground" requirement is real, it
     * should be a new ULTRA_WR_*-named knob, not a borrowed Cave PC one. */

    /* DEFAULT DRIVER = the full bit-sweep. PROVEN end-to-end: it walks
     * attract -> COURSE SELECT -> RACE (timer counts down) -> RESULT ->
     * attract, crash-free. It cycles EACH candidate switch bit (byte1 b7..b0
     * then byte2 b7..b0), ~2.5s each, pulsed ~150ms on / ~350ms off, so
     * whatever the current screen's start/decide/confirm button is gets a
     * long deliberate press within each cycle. (Shorter taps did NOT latch
     * the transitions.) ULTRA_WR_ROTATE=1 selects the leaner 6-action-bit
     * rotation instead (byte1 only). */
    if (g_rotate_mode < 0) { const char* e = getenv("ULTRA_WR_ROTATE"); g_rotate_mode = (e && e[0] == '1'); }
    if (!g_rotate_mode) {
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
        if (g_forced < 0) {
            const char* e = getenv("ULTRA_WR_BTN");
            g_forced = (e && *e) ? (int)strtol(e, NULL, 0) : 0;
            /* default hold ~2.5s -- the bit-sweep that reached COURSE SELECT
             * held each candidate this long; shorter taps did not latch. */
            const char* h = getenv("ULTRA_WR_HOLD"); g_hold = (h && *h) ? atoi(h) : 300;  /* ~2.4s */
            const char* g = getenv("ULTRA_WR_GAP");  g_gap  = (g && *g) ? atoi(g) : 50;    /* ~0.4s */
        }
        unsigned period = (unsigned)(g_hold + g_gap);
        unsigned cyc = (t - 300u) % period;
        if ((int)cyc < g_hold) {                      /* long press */
            if (g_forced) b1 = (uint8_t)g_forced;
            else {
                unsigned idx = ((t - 300u) / period) % (sizeof WR_ROTATE_CAND / sizeof WR_ROTATE_CAND[0]);
                b1 = WR_ROTATE_CAND[idx];
            }
        }
    }

    ultra_jvs_input in; memset(&in, 0, sizeof in);
    in.player[0] = (uint16_t)((b1 << 8) | b2);
    in.system = sysb;
    for (int a = 0; a < JVS_MAX_ANALOG; ++a) in.analog[a] = (uint16_t)g_wr_steer_raw; /* center-ish */
    /* drive: accel held (in-range), steering centered, brake released -- all
     * within the NVRAM calibration ranges so the game accepts them. */
    if (g_ch_accel >= 0 && g_ch_accel < JVS_MAX_ANALOG) in.analog[g_ch_accel] = (uint16_t)g_wr_accel_raw;
    if (g_ch_steer >= 0 && g_ch_steer < JVS_MAX_ANALOG) in.analog[g_ch_steer] = (uint16_t)g_wr_steer_raw;
    if (g_ch_brake >= 0 && g_ch_brake < JVS_MAX_ANALOG) in.analog[g_ch_brake] = (uint16_t)g_wr_brake_raw;
    in.screen_x = in.screen_y = 0x8000;

    ultra_board_ops_set_input(&in);
    g_tick++;
}

/* ---- registration ----------------------------------------------------------
 * Opt-in ONLY: ULTRA_WR_AUTO=1 is the sole gate, exactly as the original
 * inline branch in hook_dllmain.c checked. No exe-identity sniff exists for
 * Wacky Race today (unlike Chaos Breaker's AOE_CHAOS_BREAKER gate in
 * gamestate_probe.c) -- do not add one speculatively here; if/when a real
 * signature is captured, that is a separate, deliberate change. */
static const board_io_ops g_wackyrace_ops = {
    .name          = "Taito Type X2: Wacky Race autopilot",
    .on_install    = wr_setup_once,   /* idempotent; also self-triggers from the first tick (see above) */
    .autopilot_tick = wr_autopilot_tick,
    /* on_create_file / on_read_file / on_write_file / on_close_handle /
     * on_wait_comm_event_pending / on_load_library_redirect: NULL (default-
     * zeroed). Wacky Race's autopilot only drives input over the standard JVS
     * serial engine like the generic gamepad path did -- it needs none of the
     * file/serial interception seams, so every generic dispatcher just falls
     * through to its existing behavior for this game. */
};

/* Uniquely-named + constructor-attributed: runs automatically at DLL load,
 * before the CRT calls the exported DllMain, with no explicit call site
 * anywhere in the shared generic files. MUST be uniquely named (not the
 * generic "ultra_board_ops_autoregister" this file used before) because a
 * board's build can link MORE THAN ONE game_*.c file into the same binary
 * (the shared legacy hook DLL serves both this file's board, Type X2, AND
 * game_chaosbreaker.c's board, Type X) -- a shared well-known symbol name is
 * a link-time collision the moment two game modules share a binary. See
 * board_io_ops.h's registration section. */
static void __attribute__((constructor)) game_wackyrace_autoregister(void) {
    const char* wm = getenv("ULTRA_WR_AUTO");
    if (!(wm && wm[0] == '1')) return;
    ultra_board_ops_register(&g_wackyrace_ops);
}

#endif /* _WIN32 */
