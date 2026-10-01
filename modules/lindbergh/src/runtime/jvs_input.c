/*
 * jvs_input.c
 * JVS (JAMMA Video Standard) Input Faker
 *
 * Lindbergh games communicate with the I/O board over a JVS serial
 * protocol (RS-485) on /dev/ttyS0 (or via a USB-JVS adapter).
 *
 * JVS packet format:
 *   SYNC (0xE0)
 *   DEST (0x01 = master, 0xFF = broadcast)
 *   LENGTH (number of bytes following, including checksum)
 *   DATA[0..n]
 *   CHECKSUM (sum of DEST+LENGTH+DATA, mod 256)
 *
 * The game sends REQUEST packets and expects REPORT packets back.
 *
 * Key request types:
 *   0x01 - Reset
 *   0x10 - Get board ID
 *   0x11 - Get command format version
 *   0x12 - Get JVS version
 *   0x13 - Get communication version
 *   0x14 - Get slave features
 *   0x20 - Read coins
 *   0x21 - Read switches (buttons)
 *   0x22 - Read analog inputs
 *
 * We intercept reads/writes on /dev/ttyS0 in the VFS layer and
 * simulate a fully compliant JVS I/O board with:
 *   - 2 coin slots
 *   - 16 buttons per player (2 players)
 *   - 8 analog axes
 *
 * Input mapping (keyboard):
 *   Player 1: WASD = directions, Z=btn1, X=btn2, C=btn3, V=btn4
 *             1=start, 5=coin
 *   Player 2: Arrow keys, Numpad 1-4 = buttons
 *             2=start, 6=coin
 *   Service:  F1, Test: F2
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "loader.h"
#include "jvs_input.h"
#include "lightgun_service.h"

#include "lindbergh_boards.h"   /* lindbergh_boards_set_switch() */

/* ------------------------------------------------------------------ */
/* JVS packet constants                                                */
/* ------------------------------------------------------------------ */
#define JVS_SYNC        0xE0
#define JVS_ESCAPE      0xD0
#define JVS_MASTER_ADDR 0x00
#define JVS_BROADCAST   0xFF
#define JVS_IO_ADDR     0x01

/* Status codes */
#define JVS_STATUS_NORMAL       0x01
#define JVS_STATUS_UNKNOWN_CMD  0x02
#define JVS_STATUS_CHECKSUM_ERR 0x03
#define JVS_STATUS_OVERFLOW     0x04

/* Report codes */
#define JVS_REPORT_NORMAL       0x01
#define JVS_REPORT_PARAM_ERR    0x02

/* Feature codes */
#define JVS_FEATURE_SWITCH      0x01
#define JVS_FEATURE_COIN        0x02
#define JVS_FEATURE_ANALOG      0x03
#define JVS_FEATURE_END         0x00

/* Standard JVS player-switch bit layout (16-bit value, returned high-byte first
 * by READ_SWITCHES). High byte: D7=Start D6=Service D5=Up D4=Down D3=Left D2=Right
 * D1=Push1 D0=Push2.  Low byte: D7=Push3 D6=Push4 D5=Push5 D4=Push6 D3=Push7 D2=Push8.
 * (The previous layout used non-standard bit positions; the game reads these.) */
#define JVS_START   (1<<15)
#define JVS_SERVICE (1<<14)
#define JVS_UP      (1<<13)
#define JVS_DOWN    (1<<12)
#define JVS_LEFT    (1<<11)
#define JVS_RIGHT   (1<<10)
#define JVS_BTN1    (1<<9)   /* fire / cannon  */
#define JVS_BTN2    (1<<8)   /* missile        */
#define JVS_BTN3    (1<<7)   /* view change    */
#define JVS_BTN4    (1<<6)
#define JVS_BTN5    (1<<5)
#define JVS_BTN6    (1<<4)
#define JVS_SYS_TEST 0x80    /* system switch byte: D7=Test */

/* JVS sense line as reported to the baseboard GET_SENSE_LINE:
 *   3 = a node is present but unaddressed (game will assign an address)
 *   1 = the node is addressed -> enumeration complete (the value amJvsInit waits
 *       for; reporting it is what lets init_io_jvs finish instead of spinning). */
static int g_jvs_sense = 3;
int jvs_sense_line(void) { return g_jvs_sense; }

uint32_t g_ul_credit_check_addr = 0;   /* amCreditCheck (coin processor), opt-in */
/* Address of the game's PLAYABLE credit byte-array (one byte per coin slot/player)
 * - this is the value the game displays as CREDIT(S) and decrements on START
 * (amCreditDec subtracts the per-slot cost from byte[base+slot]). ABC does no
 * per-frame JVS coin read, so on each inserted coin we add a credit here directly
 * and poke the per-slot "credit modified" flags so the HUD refreshes. Set per-game
 * by the patcher. 0 = disabled. */
uint32_t g_ul_credit_byte_addr = 0;
uint32_t g_ul_credit_modflag_addr = 0; /* base of the per-slot modified flags */

/* ---- Free Play -----------------------------------------------------------
 * The game's native Free Play is a single boolean in SEGA's amCredit library:
 * AM_CREDIT_CONFIG.FreePlay (=1 -> free play, 0 -> coin mode). When set,
 * amCreditGetStatus returns CREDIT_ENOUGH so START/CONTINUE always pass with
 * zero coins and nothing decrements, and the HUD renders the real "FREEPLAY".
 *   g_ul_freeplay_addr        = &amCreditInfo.FreePlay      (the gate's source of truth)
 *   g_ul_freeplay_mirror_addr = &game-config display mirror (drives the FREEPLAY text)
 * Both must be re-written EVERY FRAME: ctrl_credit/amCreditLoadConfig periodically
 * re-copy/re-seed these from NVRAM defaults (0), so a one-shot boot write is lost.
 * No CRC: plain RAM/BSS, not covered by any checksum. Addrs set per-game by the
 * patcher; g_ul_freeplay is the on/off from settings. */
int      g_ul_freeplay = 0;            /* 1 = enable Free Play (from settings) */
uint32_t g_ul_freeplay_addr = 0;       /* AM_CREDIT_CONFIG.FreePlay (master gate) */
uint32_t g_ul_freeplay_mirror_addr = 0;/* display-mirror byte (HUD "FREEPLAY") */
/* Game-side JVS node receive buffer (the upstream source the game's own input
 * merge reads each frame). The game DOES attempt a per-frame bc JVS read but its
 * ack check rejects our emulated response, so the node bytes stay zero - we fill
 * them directly here in standard JVS byte format (exactly what g_jvs holds):
 *   g_ul_jvs_node_addr +0 = system byte, +1/+2 = P1 sw bytes, +3/+4 = P2, +6.. = 8x analog u16
 *   g_ul_jvs_conn_addr +0 = node-connected flag (=1), +4 = node count (=1)
 * Set per-game by the patcher. */
uint32_t g_ul_jvs_node_addr = 0;
uint32_t g_ul_jvs_conn_addr = 0;

/* ── Ghost Squad Evolution (GSEVO) credit + PC-input injection ──────────────
 * GSEVO is a Lindbergh LIGHTGUN rail-shooter. Its per-frame JVS gun poll lives
 * in a guest thread (gun_reaction_thread_func) that is gated on a recoil
 * semaphore which never fires in attract, so under the loader the JVS input is
 * starved and the credit loop stalls on the INSERT-COIN screen. GSEVO ALSO has
 * a complete PC/"xbox-board" input path (glut_mouse/motion/keyboard callbacks ->
 * the game's own switch+gun-aim merge) that is encoding-correct but is only
 * driven from the Win32 message loop, which is dead on the SDL render path.
 *
 * The fix (all sha-gated to GSEVO; ABC/LGJ never see it):
 *   1. Assert the credit lib's FreePlay flag every frame so amCreditGetStatus/
 *      amCreditIsEnough always report CREDIT_ENOUGH -> START works with no coin
 *      (RE'd: amCreditIsEnough @0x8181489 jumps to "enough" when byte[0xae9fb87]
 *      ==1; amCreditInit memsets the struct to 0 so it must be re-poked, and the
 *      credit-config gate byte[0x85a1c27] short-circuits check_spend_credit).
 *   2. On coin insert, deposit a real credit into the slot array (so the CREDIT
 *      display increments and the machinery is exercised, not just bypassed).
 *   3. Drive GSEVO's own GLUT mouse/keyboard callbacks each frame (aim + trigger
 *      + START/confirm) so the rail stage gets gun input through the game's
 *      native, encoding-correct path with no fragile switch-byte poking.
 * All addresses are .bss runtime structs (RE'd from the unstripped vsg ELF),
 * written under IsBadWritePtr guards. Set per-game by the patcher; 0 = off. */
int      g_ul_gsevo            = 0; /* 1 = this is GSEVO (gate for the feed below) */
uint32_t g_ul_gs_freeplay_addr = 0; /* amCreditInfo.FreePlay byte (0xae9fb87) */
uint32_t g_ul_gs_creditcfg_addr= 0; /* credit-config gate byte (0x85a1c27)    */
uint32_t g_ul_gs_creditarr_addr= 0; /* per-slot credit array base (0xae9fba0, stride 5) */
uint32_t g_ul_gs_creditmod_addr= 0; /* "credit modified" flag (0xae9fbf8/0xae9fbfc) */
uint32_t g_ul_gs_xboxmode_addr = 0; /* PC/xbox input-mode flag (0x8655334)     */
uint32_t g_ul_gs_mainmode_addr = 0; /* get_main_mode current  (0x8660420)      */
uint32_t g_ul_gs_mainreq_addr  = 0; /* get_main_mode_req      (0x8660424); =6 => GAME */
/* NATURAL JVS path (ABC-style direct node write - bypasses the wedged gun
 * thread). make_switch_data@0x080e83ae reads switch bytes at io_jvs_node+4/+5/+7
 * every frame; the node-COUNT guard defaults to 0 in .bss so its loop body never
 * runs until we force it =1 (the GSEVO analog of ABC's g_ul_jvs_conn_addr+4). */
uint32_t g_ul_gs_node_addr     = 0; /* io_jvs_node base (0x8655bc0, stride 0x28) */
uint32_t g_ul_gs_nodecount_addr= 0; /* node-count loop guard (0x8655364; force=1)*/
uint32_t g_ul_gs_gunwork_addr  = 0; /* gun_work base (0x8651e60, stride 0x1a0)   */
/* The xbox/PC switch struct the title/credit/demo handlers actually read for
 * START. get_xbox_swdata@0x080e9106 returns (0x86544b4 + p*0x35c). The demo's
 * ev_advertise_ctrl tests `testb $0x4,0x135(%eax)` for START; gameplay fire goes
 * through gun_work+0x64 (is_gun_action@0x080e51c6, action 3 -> bit 0x8) gated by
 * gun_work+0x7 bit 0x10. Writing these two structs is the verified ABC-parity
 * decoded-input feed (the node-byte field can't carry the high START bit). */
uint32_t g_ul_gs_swdata_addr   = 0; /* get_xbox_swdata base (0x86544b4, stride 0x35c) */
/* The CONSOLIDATED per-player switch state the FSM's game_start_check actually
 * reads: player[0]+0xf0 == 0x08653420 (RE'd live), and game_start_check tests
 * `testb $0x1,0x8(player+0xf0)` -> START = bit0 of 0x08653428 (the +8 edge/trg
 * word). This is the single authoritative field for title->game entry; writing
 * it directly is the verified START delivery (the make_switch_data output lands
 * in a different struct @0x08653c30 and is copied/processed before this). */
uint32_t g_ul_gs_swstate_addr  = 0; /* consolidated switch state (0x08653420; +8 = edge/START) */
/* Loader START flag byte (guest .bss, 0x08653438) read by the natural-START
 * trampoline stub spliced into main_loop_func between io_ctrl and event_ctrl
 * (see patcher.c). We write 1 here when START is latched, 0 when idle; the stub
 * ORs bit0 into swstate+8 in the only window that survives to game_start_check. */
uint32_t g_ul_gs_startflag_addr = 0; /* 0x08653438 (set by patcher for GSEVO) */
unsigned g_gs_edge_seen        = 0; /* DIAG: latched OR of sw+8 (catches 1-frame START edges) */
/* Latched START/trigger/aim from gsevo_inject (runs at poll time, BEFORE the
 * guest vblank cb's make_switch_data clobbers swstate+8). gsevo_post_vblank()
 * re-applies them AFTER make_switch_data, BEFORE game_start_check reads - the
 * only window where swstate+8 bit0 survives to the gate. See gsevo_post_vblank. */
static int   s_gs_start_held   = 0;
static int   s_gs_trig_held    = 0;
static float s_gs_aim_x        = 0.5f, s_gs_aim_y = 0.5f;
/* Auto-play (ULTRA_AUTO_PLAY) rail-shooter autopilot pose/fire for GSEVO: when
 * the lightgun is not driving, the auto-play sweeps the aim across the screen
 * and holds fire during gameplay so the on-rails stage is actively played (enemies
 * engaged, score climbs, stage advances) instead of standing passive. Set by the
 * auto-play block, consumed by the GSEVO gun feed at the bottom of poll_inputs. */
static int   s_gs_auto_active  = 0;       /* 1 = auto-play is providing aim/fire   */
static float s_gs_auto_aim_x   = 0.5f, s_gs_auto_aim_y = 0.5f;
static int   s_gs_auto_fire    = 0;
/* The game's OWN internal GLUT input callbacks (guest VAs, cdecl). Calling them
 * directly feeds START/FIRE through the game's native PC-input encoding (SPACE ->
 * SWITCH_START word0 0x100; left mouse -> fire), which the node-byte arcade path
 * cannot express. GSEVO registers no glutKeyboardFunc, so the loader's GLUT stub
 * never receives keys - the game's internal callbacks are the correct entry. */
uint32_t g_ul_gs_kbcb_down     = 0; /* glut_keyboard_callback    (0x080e96de)   */
uint32_t g_ul_gs_kbcb_up       = 0; /* glut_keyboard_up_callback (0x080e964a)   */
uint32_t g_ul_gs_spcb_down     = 0; /* glut_special_callback     (0x080e95c2)   */
uint32_t g_ul_gs_spcb_up       = 0; /* glut_special_up_callback  (0x080e9538)   */
uint32_t g_ul_gs_mousecb       = 0; /* glut_mouse_callback       (0x080e93fc)   */

/* ------------------------------------------------------------------ */
/* Board state                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    uint16_t coins[2];        /* coin counts per slot */
    uint16_t switches[2];     /* button bitfields per player */
    uint8_t  system_switches; /* service, test bits */
    uint16_t analog[8];       /* analog axis values (0-0xFFFF) */
} JvsState;

static JvsState g_jvs = {0};

/* ------------------------------------------------------------------ */
/* Controller tuning - set by settings_apply() (globals, NOT env: mingw      */
/* getenv can't see a post-CRT-init SetEnvironmentVariableA). Applied to the */
/* analog axes / triggers below. Flight-stick values shape the analog the    */
/* game reads; the light-gun values are forward-looking for gun titles       */
/* (Let's Go Jungle) and are consumed once a gun read path writes screen X/Y.*/
int g_ul_stick_sens       = 100;  /* % deflection (100 = 1:1)           */
int g_ul_stick_deadzone   = 12;   /* % radial dead zone                 */
int g_ul_stick_curve      = 0;    /* 0 linear, 1 smooth, 2 aggressive   */
int g_ul_stick_invert_x   = 0;
int g_ul_stick_invert_y   = 0;
int g_ul_stick_smooth     = 40;   /* 0 = instant (raw); higher = smoother analog ramp.
                                   * Slew-rate limit that turns a clone pad's near-DIGITAL
                                   * snap-to-extreme into a graceful analog lean: a quick
                                   * tip only reaches a partial deflection, only a held
                                   * push hits full. Kills the "slight tip -> barrel roll". */
int g_ul_trigger_thresh   = 25;   /* % pull at which a trigger = button  */
int g_ul_autocal          = 1;    /* 1 = intelligent stick auto-calibration; 0 = off */
int g_ul_lightgun_enable  = 0;
int g_ul_lightgun_x_sens  = 100;  /* %  */
int g_ul_lightgun_y_sens  = 100;
int g_ul_lightgun_x_offset = 0;   /* -50..50 calibration */
int g_ul_lightgun_y_offset = 0;
int g_ul_lightgun_invert_x = 0;
int g_ul_lightgun_invert_y = 0;
/* Per-player "is this player's analog the gun pose?" flag - set per-frame by
 * poll_inputs and read by process_jvs_request case 0x22 so the standard JVS
 * READ_ANALOG reply can bypass jvs_shape_axis/jvs_slew/jvs_decal/jvs_game_cal
 * for gun channels (those transforms are tuned for sticks and warp gun pose). */
static int s_gun_active[2] = {0, 0};

/* Shape one raw analog axis (centered 0x8000) with the user's dead zone,
 * sensitivity, response curve and invert. is_y selects the X/Y invert flag
 * (even channel = X/roll, odd = Y/pitch). No libm: curve is plain multiplies. */
static uint16_t jvs_shape_axis(uint16_t raw, int is_y)
{
    float v = ((int)raw - 32768) / 32768.0f;          /* -1..1            */
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    float a = v < 0 ? -v : v;                          /* magnitude        */
    float s = v < 0 ? -1.0f : 1.0f;                    /* sign             */

    float dz = g_ul_stick_deadzone / 100.0f;
    if (dz < 0.0f) dz = 0.0f;
    if (dz > 0.95f) dz = 0.95f;
    if (a <= dz) return 32768;                         /* inside dead zone -> center */
    a = (a - dz) / (1.0f - dz);                        /* rescale 0..1 past the zone */

    if (g_ul_stick_curve == 1)      a = a * a;         /* smooth: fine near center   */
    else if (g_ul_stick_curve == 2) a = a * a * a;     /* aggressive                 */

    a *= g_ul_stick_sens / 100.0f;                     /* sensitivity      */
    if (a > 1.0f) a = 1.0f;

    v = s * a;
    if (is_y ? g_ul_stick_invert_y : g_ul_stick_invert_x) v = -v;

    int out = (int)(v * 32768.0f) + 32768;
    if (out < 0) out = 0;
    if (out > 65535) out = 65535;
    return (uint16_t)out;
}

/* TRUE-ANALOG SIMULATION (slew-rate limiter).  Clone/knock-off pads frequently
 * report a near-DIGITAL analog axis: a slight tip jumps straight to (almost)
 * full deflection, which the flight model reads as "slam the stick" -> instant
 * barrel-roll. We can't add resolution the pad never sent, but we CAN shape the
 * TIME response: move the emitted value toward the raw target by at most a fixed
 * step per frame. A brief tip then only climbs partway before you release (a
 * gentle lean); only a sustained push ramps to full. g_ul_stick_smooth picks the
 * ramp time (0 = off/instant). Per-channel state; center is 32768. */
static uint16_t jvs_slew(int c, uint16_t target)
{
    static int prev[8];
    static int init = 0;
    if (!init) { for (int i = 0; i < 8; i++) prev[i] = 32768; init = 1; }
    if (c < 0 || c >= 8) return target;
    int sm = g_ul_stick_smooth;
    if (sm <= 0) { prev[c] = target; return target; }    /* instant */
    if (sm > 100) sm = 100;
    /* frames-to-full ~= 1 + sm/7  -> sm=40 ~6.7 frames (~0.11s), sm=100 ~15 frames */
    float ftf = 1.0f + (float)sm / 7.0f;
    int step = (int)(65535.0f / ftf);
    if (step < 1) step = 1;
    int d = (int)target - prev[c];
    if (d >  step) d =  step;
    if (d < -step) d = -step;
    prev[c] += d;
    if (prev[c] < 0) prev[c] = 0;
    if (prev[c] > 65535) prev[c] = 65535;
    return (uint16_t)prev[c];
}

/* ── Clone-controller drift auto-calibration (stick HELD INERT until calibrated) ─
 * Cheap knock-off XInput pads rarely rest at true center, so the plane drifts and
 * the off-center rest makes one side reach the barrel-roll extreme too easily. We
 * learn the resting offset and subtract it so the stick reads true-center. KEY: the
 * analog axes are forced to center (the stick does NOTHING) until calibration
 * completes - so a player who ignores "don't touch it" simply has an inert stick
 * during boot rather than moving the game or corrupting the center. Calibration
 * locks only when it sees a STABLE reading NEAR center (i.e. the stick is at rest):
 * if the player holds/wiggles it, cal just WAITS for release, then the stick goes
 * live. Offset clamped to +-25%. Disable entirely with ULTRA_NO_AUTOCAL=1. */
static int  s_cal_done   = 0;
static int  s_cal_off[8] = {0};
static int  s_cal_dis    = -1;
static int  s_cal_n      = 0;
static int  s_cal_stable = 0;
static int  s_cal_prev[8];
static int  s_cal_have_prev = 0;
static int  jvs_calibrated(void) { return s_cal_done; }   /* 0 until the center is locked */
static void jvs_autocal_tick(void)   /* once per poll, AFTER all sources set g_jvs.analog */
{
    if (s_cal_dis < 0) { const char *e = getenv("ULTRA_NO_AUTOCAL");
        s_cal_dis = ((e && atoi(e)) || !g_ul_autocal) ? 1 : 0; }   /* env OR the settings toggle */
    if (s_cal_dis) { s_cal_done = 1; return; }            /* disabled -> stick always live, no offset */
    if (s_cal_done) {
        /* CONTINUOUS re-centering: after the one-time lock a clone stick that
         * slowly WANDERS still gets zeroed. VERY conservative so it can never eat
         * intentional input: only when the already-decalibrated reading sits within
         * ~5% of center AND is dead-stable for ~1.5s do we slowly nudge the offset
         * toward the new rest. Any motion or a real hold (>5%) pauses it. */
        static int rc_n = 0, rc_prev[8], rc_have = 0;
        int mv = 0;
        if (rc_have) for (int c = 0; c < 8; c++) { int d = g_jvs.analog[c] - rc_prev[c]; if (d<0) d=-d; if (d>mv) mv=d; }
        for (int c = 0; c < 8; c++) rc_prev[c] = g_jvs.analog[c];
        rc_have = 1;
        int dx = (g_jvs.analog[0]-s_cal_off[0]) - 32768, dy = (g_jvs.analog[1]-s_cal_off[1]) - 32768;
        if (dx<0) dx=-dx; if (dy<0) dy=-dy;
        if (mv < 300 && dx < 1640 && dy < 1640) {         /* dead-stable, <~5% of center */
            if (++rc_n > 90) {                            /* ~1.5s settled -> drift, not a hold */
                for (int c = 0; c < 8; c++) { int tgt = g_jvs.analog[c] - 32768;
                    if (tgt > 8192) tgt = 8192; if (tgt < -8192) tgt = -8192;
                    s_cal_off[c] += (tgt - s_cal_off[c]) / 16; }   /* slow nudge toward the new rest */
            }
        } else rc_n = 0;
        return;
    }
    s_cal_n++;

    /* frame-to-frame movement: small across all axes == the stick is at rest */
    int maxd = 0;
    if (s_cal_have_prev) for (int c = 0; c < 8; c++) { int d = g_jvs.analog[c] - s_cal_prev[c]; if (d < 0) d = -d; if (d > maxd) maxd = d; }
    for (int c = 0; c < 8; c++) s_cal_prev[c] = g_jvs.analog[c];
    s_cal_have_prev = 1;

    if (s_cal_n > 2 && maxd < 1024) s_cal_stable++;       /* <~1.5%/frame == settled */
    else s_cal_stable = 0;

    if (s_cal_stable >= 16) {
        /* Stable - but only accept it as the resting center if the main stick axes
         * are within +-25% of center. If they sit far out, the player is HOLDING the
         * stick; keep waiting (stick stays inert) until they let go. */
        int ox = g_jvs.analog[0] - 32768, oy = g_jvs.analog[1] - 32768;
        if (ox > -8193 && ox < 8193 && oy > -8193 && oy < 8193) {
            for (int c = 0; c < 8; c++) {
                int off = g_jvs.analog[c] - 32768;
                if (off >  8192) off =  8192;
                if (off < -8192) off = -8192;
                if (off > -256 && off < 256) off = 0;      /* ignore trivial noise */
                s_cal_off[c] = off;
            }
            s_cal_done = 1;
            loader_log(LOG_INFO, "[INPUT] analog auto-cal locked after %d polls: offset x=%d y=%d (stick LIVE)",
                       s_cal_n, s_cal_off[0], s_cal_off[1]);
        } else {
            s_cal_stable = 0;                              /* held off-center -> wait for release */
        }
    } else if (s_cal_n > 180) {                            /* safety: never settled -> go live, no offset */
        for (int c = 0; c < 8; c++) s_cal_off[c] = 0;
        s_cal_done = 1;
        loader_log(LOG_WARN, "[INPUT] analog auto-cal timed out (stick never settled), enabling without offset");
    }
}
static uint16_t jvs_decal(int c, uint16_t raw)   /* remove the learned resting drift */
{
    int v = (int)raw - s_cal_off[c];
    if (v < 0) v = 0;
    if (v > 65535) v = 65535;
    return (uint16_t)v;
}

/* ── Game analog-calibration remap (After Burner Climax) ────────────────────
 * GROUND TRUTH from disassembling the decoder make_switch_data @0x8094f7a:
 *   roll = clamp( (int8_t)(high_byte + 0x80) / 127.0,  -1.0, +1.0 )
 *   (movzbl hi; add $0x80,%cl; movsbl; cvtsi2ss; divss [0x82b1928]=127.0;
 *    clamp to [0x82025a0]=-1.0 .. [0x820248c]=+1.0).
 * It reads only the HIGH byte and re-centers at 0x80 - there is NO NVRAM
 * min/max scaling. So the game's NEUTRAL is high-byte 0x80 = value 0x8000
 * (32768), full LEFT = high 0x00 (value 0), full RIGHT = high 0xFF (value
 * 0xFF00). That is exactly our already-0x8000-centered shaped value, so the
 * correct remap is IDENTITY (center 32768, half 32768 -> out == v).
 * The old center=57200/half=8335 remap (based on an earlier mis-measurement
 * where the probed float was the aircraft's evolving roll state, not the raw
 * input) pushed every output high-byte to 190..255 - all >0x80 - so BOTH stick
 * directions read as positive: left rolled right, pitch-down read as up.
 * Resting-stick drift is handled upstream by autocal (jvs_decal), not here.
 * Per-axis & env-tunable (ULTRA_CAL_C#/H#); half==0 passes through unchanged;
 * g_ul_game_cal master-gates it (1 = on; the native JVS path is ABC-specific). */
int g_ul_game_cal = 1;
static int s_cal_center[8] = { 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768 };
static int s_cal_half[8]   = { 32768, 32768, 32768,     0,     0,     0,     0,     0 };
static int s_gcal_env = 0;
static uint16_t jvs_game_cal(int c, uint16_t v)
{
    if (!s_gcal_env) { s_gcal_env = 1; const char *e;
        if ((e=getenv("ULTRA_GAME_CAL"))&&*e) g_ul_game_cal = atoi(e);
        char k[20];
        for (int i=0;i<8;i++){ snprintf(k,sizeof k,"ULTRA_CAL_C%d",i); if((e=getenv(k))&&*e) s_cal_center[i]=atoi(e);
                               snprintf(k,sizeof k,"ULTRA_CAL_H%d",i); if((e=getenv(k))&&*e) s_cal_half[i]=atoi(e); }
    }
    if (g_ul_game_cal != 1 || s_cal_half[c] == 0) return v;    /* mode 1 (feed-side remap); 0 = off, feed raw */
    int n = (int)v - 32768;                                    /* -32768..+32767 */
    int out = s_cal_center[c] + (int)((long long)n * s_cal_half[c] / 32768);
    if (out < 0) out = 0; if (out > 65535) out = 65535;
    return (uint16_t)out;
}

/* ── GSEVO per-frame credit + PC-input feed ─────────────────────────────────
 * Called at the END of poll_inputs (per-frame, on the render/SDL thread where
 * the GLUT callbacks are safe to invoke). Inert unless g_ul_gsevo is set.
 *   aim_x/aim_y  : normalized gun pose [0..1] (top-left origin)
 *   trigger      : 1 = fire held
 *   start        : 1 = START/confirm held (advances attract -> game)
 *   coin_pulse   : 1 on the rising edge of a coin insert (deposit one credit)
 * NATURAL PATH (primary): START/trigger are delivered by writing the game's
 * decoded JVS node receive bytes (io_jvs_node+4/+5/+7) every frame - the SAME
 * source make_switch_data@0x080e83ae reads unconditionally - so the input flows
 * make_switch_data -> make_switch_action -> player[].SWDATA+8 -> the real
 * game_start_check@0x081083fe gate, exactly like ABC's node feed and with NO
 * dependence on the wedged gun thread or on ULTRA_GS_FORCE_GAME. The gun pose is
 * written straight into gun_work[p]+0x14/+0x18 (the struct get_gun_position
 * reads), with the valid bit cleared and the trigger flag OR'd via set_gun_flags
 * semantics. The legacy GLUT-callback path (glut_feed_*) is kept as a fallback. */
static void gsevo_inject(float aim_x, float aim_y, int trigger, int start, int coin_pulse)
{
    if (!g_ul_gsevo) return;

    /* Latch this frame's START/trigger/aim for gsevo_post_vblank() to re-apply
     * AFTER the guest's make_switch_data has rebuilt swstate+8 (which runs in the
     * vblank cb, between this poll and game_start_check). */
    s_gs_start_held = start;
    s_gs_trig_held  = trigger;
    s_gs_aim_x      = aim_x;
    s_gs_aim_y      = aim_y;

    /* DIAG: latch sw+8 (edge word) every frame so 1-frame START edges produced
     * by the game's own edge detector are caught even though GSDIAG2 samples
     * sparsely. Read-only; inert effect on the game. */
    if (g_ul_gs_swstate_addr) {
        volatile uint32_t *e8 = (volatile uint32_t*)((uintptr_t)g_ul_gs_swstate_addr + 8);
        if (!IsBadReadPtr((void*)e8, 4)) g_gs_edge_seen |= *e8;
    }

    /* ── State-transition diagnostic (ULTRA_GS_DIAG=1) ──────────────────────
     * Read-only probe of the game's own state words (RE'd):
     *   [0x8651e54] : game-state bitfield (bit1=is_game_start, bit2=is_game_over)
     *   [0x86604a4] : movie state (0=idle 1=start 2=playing 3=finished)
     *   [0x0ae9fba0]: credit slot 0
     * Log only on change so the loop (attract->coin->start->play->over->attract)
     * is visible in the log without a debugger. Inert unless the env is set. */
    {
        static int s_diag = -1;
        if (s_diag < 0) { const char *e = getenv("ULTRA_GS_DIAG"); s_diag = (e && atoi(e)) ? 1 : 0; }
        if (s_diag) {
            static int s_pg=-1, s_po=-1, s_pmov=-1, s_pcr=-1, s_pmm=-2, s_pmr=-2;
            volatile uint8_t *gs = (volatile uint8_t*)0x8651e54;
            volatile int     *mv = (volatile int*)0x86604a4;
            volatile int     *mm = (volatile int*)0x8660420; /* get_main_mode      */
            volatile int     *mr = (volatile int*)0x8660424; /* get_main_mode_req  */
            volatile uint8_t *cr = (volatile uint8_t*)(uintptr_t)g_ul_gs_creditarr_addr;
            int gstart=0, gover=0, mov=-1, cred=-1, mainm=-99, mainr=-99;
            if (!IsBadReadPtr((void*)gs,1)) { gstart=(*gs>>1)&1; gover=(*gs>>2)&1; }
            if (!IsBadReadPtr((void*)mv,4)) mov = *mv;
            if (!IsBadReadPtr((void*)mm,4)) mainm = *mm;
            if (!IsBadReadPtr((void*)mr,4)) mainr = *mr;
            if (cr && !IsBadReadPtr((void*)cr,1)) cred = *cr;
            if (gstart!=s_pg||gover!=s_po||mov!=s_pmov||cred!=s_pcr||mainm!=s_pmm||mainr!=s_pmr) {
                extern int g_ul_swap_frame;
                loader_log(LOG_INFO, "[GSDIAG] f=%d mainmode=%d req=%d game_start=%d game_over=%d movie=%d credit=%d",
                           g_ul_swap_frame, mainm, mainr, gstart, gover, mov, cred);
                s_pg=gstart; s_po=gover; s_pmov=mov; s_pcr=cred; s_pmm=mainm; s_pmr=mainr;
            }
            /* START-propagation probe (ULTRA_GS_DIAG=2): periodic dump of the
             * input arg + the two START detectors (xbox swdata+0x135 and the
             * player[0]+0xf0 -> +8 SWDATA the FSM's game_start_check reads). */
            static int s_diag2 = -1;
            if (s_diag2 < 0) { const char *e = getenv("ULTRA_GS_DIAG"); s_diag2 = (e && atoi(e)>=2) ? 1 : 0; }
            if (s_diag2) {
                extern int g_ul_swap_frame; int f = g_ul_swap_frame;
                if ((f % 60) == 0) {
                    int sw135=-1, pf0=0, pf0_8=-1;
                    if (g_ul_gs_swdata_addr) { volatile uint8_t *sw=(volatile uint8_t*)(uintptr_t)g_ul_gs_swdata_addr;
                        if(!IsBadReadPtr((void*)(sw+0x135),1)) sw135=sw[0x135]; }
                    volatile uint32_t *pp = (volatile uint32_t*)0x086617e0; /* player[0]+0xf0 */
                    if(!IsBadReadPtr((void*)(((uint8_t*)pp)+0xf0),4)) { pf0=*(uint32_t*)(((uint8_t*)pp)+0xf0);
                        if(pf0 && !IsBadReadPtr((void*)(uintptr_t)(pf0+8),1)) pf0_8=*(uint8_t*)(uintptr_t)(pf0+8); }
                    unsigned st4=0, st8=0;
                    if (g_ul_gs_swstate_addr) { volatile uint32_t *st=(volatile uint32_t*)(uintptr_t)g_ul_gs_swstate_addr;
                        if(!IsBadReadPtr((void*)st,0xc)){ st4=st[1]; st8=st[2]; } }
                    extern unsigned g_gs_edge_seen; /* latched OR of sw+8 across all frames */
                    /* player[0] join state: +0x4 (bit4 0x10 = joined-mission flag that
                     * is_player_join_mission reads) and +0x8 (state, =2 after join). */
                    uint32_t pj4 = 0; int pj8 = -1;
                    volatile uint8_t *pl0 = (volatile uint8_t*)0x086617e0u;
                    if (!IsBadReadPtr((void*)(pl0+4),4)) pj4 = *(volatile uint32_t*)(pl0+4);
                    if (!IsBadReadPtr((void*)(pl0+8),4)) pj8 = *(volatile int*)(pl0+8);
                    loader_log(LOG_INFO, "[GSDIAG2] f=%d start=%d trig=%d swdata135=%02x f0=%08x +8=%02x | sw+4=%08x sw+8=%08x edge=%08x | pl0+4=%08x join=%d pl0+8=%d",
                               f, start, trigger, sw135&0xff, pf0, pf0_8&0xff, st4, st8, g_gs_edge_seen,
                               pj4, (pj4>>4)&1, pj8);
                }
            }
        }
    }

    /* 1. FreePlay + credit-config gate: assert every frame (the credit lib
     *    re-seeds these to 0 from amCreditInit/LoadConfig). =1 makes
     *    amCreditIsEnough/check_spend_credit report "enough" so START works
     *    with zero coins and nothing has to spin the wedged JVS gun thread. */
    if (g_ul_gs_freeplay_addr) {
        volatile uint8_t *f = (volatile uint8_t*)(uintptr_t)g_ul_gs_freeplay_addr;
        if (!IsBadWritePtr((void*)f, 1)) *f = 1;
    }
    if (g_ul_gs_creditcfg_addr) {
        volatile uint8_t *g = (volatile uint8_t*)(uintptr_t)g_ul_gs_creditcfg_addr;
        if (!IsBadWritePtr((void*)g, 1)) *g = 1;
    }
    /* 2. Force PC/xbox input mode so gun_ctrl_internal reads the mouse/keyboard
     *    path instead of the (starved) arcade JVS gun thread. io_read/io_ctrl
     *    can toggle this from a dipsw float each frame, so re-assert it. */
    if (g_ul_gs_xboxmode_addr) {
        volatile uint32_t *x = (volatile uint32_t*)(uintptr_t)g_ul_gs_xboxmode_addr;
        if (!IsBadWritePtr((void*)x, 4)) *x = 1;
    }
    /* 2b. (REVERTED) Forcing the "xbox-board arcade" flag [0x8655c60]=1 every
     *     frame did NOT make the natural press-start path consume START, and it
     *     destabilised the attract loop (the game took a clean-exit path around
     *     the title-movie transition, frame ~3300-4400, non-deterministically).
     *     Left out so the default attract loop stays rock-stable. The credited
     *     game is reachable via ULTRA_GS_FORCE_GAME instead (see below). */

    /* 3. Coin -> deposit one credit into the per-slot credit array (slot 0).
     *    amCreditIsEnough reads byte[slot*5 + 0xae9fba0]; bump it + set the
     *    "credit modified" flag so the HUD refreshes (credit display ticks up,
     *    proving the credit path is live even with FreePlay on). */
    if (coin_pulse && g_ul_gs_creditarr_addr) {
        volatile uint8_t *cr = (volatile uint8_t*)(uintptr_t)g_ul_gs_creditarr_addr; /* slot 0 */
        if (!IsBadWritePtr((void*)cr, 1) && *cr < 99) (*cr)++;
        if (g_ul_gs_creditmod_addr) {
            volatile uint32_t *m = (volatile uint32_t*)(uintptr_t)g_ul_gs_creditmod_addr;
            if (!IsBadWritePtr((void*)m, 4)) *m = 1;
        }
        loader_log(LOG_INFO, "[GSEVO] coin -> credit deposited (slot 0)");
    }

    /* ── NATURAL JVS NODE WRITE (the ABC model) ─────────────────────────────
     * Write the game's decoded JVS node switch bytes every frame so START and
     * the trigger reach game_start_check / make_switch_action through the game's
     * OWN per-frame decoder (make_switch_data@0x080e83ae), with no reliance on
     * the wedged gun_reaction_thread JVS poll.
     *
     * Disasm of make_switch_data's loop body (per node, stride 0x28):
     *   node+4 (movzbl): bit2 (0x4) set -> asserts switch bit 0x8
     *   node+5 (movzbl): bit6 (0x40) set -> asserts switch bit 0x8
     *   node+7 (movzbl): bit6 (0x40) set -> asserts switch bit 0x8
     * (bits 0x4/0x10/0x20 are OR'd in unconditionally each iteration; the only
     * input-derived bits are the three above + the dead `cmpg $0xff` all-on
     * paths, which never fire for a movzbl byte.) Writing 0xFF to +4/+5/+7
     * therefore asserts every input-derived switch - including the START bit
     * make_switch_action maps into player[].SWDATA+8 - which is the robust first
     * cut the bring-up plan calls for. We drive +4/+5/+7 from the SAME merged
     * sources ABC uses (g_jvs.switches[0] JVS_START/JVS_BTN1, which auto-play
     * drives, plus the lightgun START/trigger folded in by the caller via the
     * `start`/`trigger` args). Idle (no input) -> bytes 0 -> no switch asserted,
     * so the attract loop is never over-pressed.
     *
     * The node-COUNT guard @0x8655364 is zero-init .bss, so make_switch_data's
     * loop body never runs until we force count=1 (ABC: g_ul_jvs_conn_addr+4). */
    if (g_ul_gs_nodecount_addr) {
        volatile int *nc = (volatile int*)(uintptr_t)g_ul_gs_nodecount_addr;
        if (!IsBadWritePtr((void*)nc, 4) && *nc < 1) *nc = 1; /* enable node0 decode */
    }
    if (g_ul_gs_node_addr) {
        /* node0 base; switch bytes live at +4/+5/+7 (make_switch_data reads
         * io_jvs_node+4/+5/+7 -> 0x8655bc4/bc5/bc7). */
        volatile uint8_t *nd = (volatile uint8_t*)(uintptr_t)g_ul_gs_node_addr;
        if (!IsBadWritePtr((void*)nd, 0x28)) {
            uint8_t b4 = 0, b5 = 0, b7 = 0;
            /* START + trigger both route through bit 0x8; assert the carrying
             * bits when either is held. node+4 bit2 carries START; node+5/+7
             * bit6 carry the secondary switches (trigger/confirm). Use 0xFF on
             * the active byte so any permuted START/fire bit the game expects is
             * covered (all-on first cut), 0 when idle. */
            if (start)   { b4 |= 0x04; }           /* START -> node+4 bit2 */
            if (trigger) { b5 |= 0x40; b7 |= 0x40; }/* FIRE  -> node+5/+7 bit6 */
            nd[4] = b4;
            nd[5] = b5;
            nd[7] = b7;
        }
    }

    /* ── START -> xbox/PC switch struct (the field the title/demo READS) ──────
     * get_xbox_swdata@0x080e9106 = 0x86544b4 + p*0x35c. The "PRESS START" demo
     * handler ev_advertise_ctrl does `call get_xbox_swdata; testb $0x4,0x135(eax)`
     * - i.e. START = bit 0x4 of swdata+0x135 (the edge/trg byte). The node-byte
     * field decoded by make_switch_data can't carry this bit, so we set it here
     * directly (and the +0x4 "level" mask bit 0x1 that the title-select path at
     * 0x805213e reads). Player 0. Idle clears it so the demo loop isn't held. */
    if (g_ul_gs_swdata_addr) {
        volatile uint8_t *sw = (volatile uint8_t*)(uintptr_t)g_ul_gs_swdata_addr; /* player 0 */
        if (!IsBadWritePtr((void*)sw, 0x140)) {
            if (start) { sw[0x135] |= 0x04; *(volatile uint32_t*)(sw + 0x04) |= 0x1u; }
            else       { sw[0x135] &= (uint8_t)~0x04; *(volatile uint32_t*)(sw + 0x04) &= ~0x1u; }
        }
    }

    /* ── START -> the CONSOLIDATED switch word game_start_check ACTUALLY reads ──
     * AUTHORITATIVE START GATE (disasm-pinned, vsg unstripped):
     *   game_start_check@0x081083fe walks player[] (base 0x86617e0, stride 0x1ac)
     *   and for each does:  mov 0xf0(%ebx),%eax ; testb $0x1,0x8(%eax)
     *   -> i.e. it derefs player[p]+0xf0 (a POINTER) and tests bit0 (0x1) of
     *      that target's +8. Live-RE'd: player[0]+0xf0 == 0x08653420
     *      (g_ul_gs_swstate_addr), so the tested byte is 0x08653428 (swstate+8).
     *   GSDIAG2 confirmed: when START is auto-pressed, swstate+8 transiently holds
     *      0x100 (bit 8) but NEVER bit 0x1 -> game_start_check's `testb $0x1` is
     *      false every frame -> the credited game is never entered. The arcade
     *      node decode (make_switch_data) maps every input to switch bit 0x8/0x100,
     *      so it structurally cannot raise bit 0x1 here. The xbox-swdata write above
     *      lands in a different struct (0x86544b4) that this gate does not read.
     * The field is an int array indexed by player, stride 4 (disasm shows the
     * sibling reads `...0x8653428(,%ecx,4)` and several `testl $0x101,...` consumers
     * - the title/menu accept START as bits 0x1 OR 0x100). So we set BOTH bits:
     *   bit 0x1   -> satisfies game_start_check@0x081083fe (the start gate)
     *   bit 0x100 -> satisfies the title/menu `testl $0x101` readers (same press)
     * directly in 0x08653428 (player-0 slot) when START is held, cleared when idle.
     * This is the GSEVO analog of writing ABC's decoded node bytes - the single
     * authoritative decoded-input field, reached WITHOUT the wedged gun thread or
     * ULTRA_GS_FORCE_GAME. game_continue_check@0x081084bf reads the identical
     * field, so CONTINUE works through the same write, closing the credit loop.
     *
     * TIMING: the ACTUAL swstate+8 write happens in gsevo_post_vblank() - called
     * by sdl_window_pump AFTER the guest vblank cb (where make_switch_data rebuilds
     * swstate+8) and BEFORE the display cb (where game_start_check reads it). A
     * write HERE (poll time, step 1) is clobbered by make_switch_data (step 2)
     * before game_start_check (step 3) runs - verified: it never fired. So we only
     * LATCH the desired START (s_gs_start_held, set at the top of this function)
     * for gsevo_post_vblank to apply in the surviving window. */

    /* ── START + FIRE via the game's OWN PC-input callbacks (authoritative) ───
     * The node-byte arcade path (above) can only ever assert switch field bit
     * 0x8 (all of node+4/+5/+7's input bits collapse to it - disasm-proven), so
     * it CANNOT express the distinct START bit. The game's internal GLUT input
     * callbacks, however, encode every button correctly:
     *   glut_keyboard_callback@0x080e96de(key,x,y): SPACE(0x20) -> word0 bit 0x100
     *     = SWITCH_START (the SPACE jump-table entry is `orl $0x100,(%edx)`), the
     *     exact bit the title/credit path consumes; it ORs into the per-player
     *     xbox swdata struct + the persistent key mask 0x86546b8.
     *   glut_keyboard_up_callback@0x080e964a(key,x,y): clears it.
     *   glut_mouse_callback@0x080e93fc(button,state,x,y): left button = fire.
     * These run guest code in-process (same as the vblank/display cb the loader
     * already calls). GSEVO registers no glutKeyboardFunc, so the loader's GLUT
     * stub never sees keys - calling the game's internal callbacks directly is
     * the correct, game-native START/fire delivery with the right bit encoding.
     * Edge-detected: one down/up pair per press. The function pointers are the
     * fixed guest VAs (cdecl). SHA-gated via g_ul_gs_kbcb_* (set only for GSEVO). */
    if (g_ul_gs_kbcb_down && g_ul_gs_kbcb_up) {
        typedef void (*gs_kbcb_t)(unsigned char, int, int);
        gs_kbcb_t kdown = (gs_kbcb_t)(uintptr_t)g_ul_gs_kbcb_down;
        gs_kbcb_t kup   = (gs_kbcb_t)(uintptr_t)g_ul_gs_kbcb_up;
        /* SPACE for START is the documented bit (jump-table entry orl $0x100). */
        static int s_prev_kb_start = 0;
        /* START is GLUT SPECIAL F1, not SPACE. SPACE/ENTER/'z'/LMB all decode to bit
         * 0x100 = PUSH1 = the trigger, so the old kdown(0x20) fired a shot on every
         * START press. The game's real START bit (0x1) is produced ONLY by the
         * special-key callback, which nothing called until now. */
        if (g_ul_gs_spcb_down && g_ul_gs_spcb_up) {
            typedef void (*gs_spcb_t)(int, int, int);
            gs_spcb_t sdown = (gs_spcb_t)(uintptr_t)g_ul_gs_spcb_down;
            gs_spcb_t sup   = (gs_spcb_t)(uintptr_t)g_ul_gs_spcb_up;
            if (start && !s_prev_kb_start)      sdown(1, 0, 0);   /* GLUT_KEY_F1 */
            else if (!start && s_prev_kb_start) sup(1, 0, 0);
        }
        s_prev_kb_start = start;

        /* UNIVERSAL KEY FORWARDER: GSEVO reads input through its OWN GLUT
         * keyboard callback (not JVS), so directional/face-button keys never
         * reach the game even when we set the JVS switch bits. Forward every
         * user keypress through the game's native callback as the lowercase
         * ASCII the game expects. The callback is a jump-table on the key
         * byte; entries the game doesn't recognize are silently ignored, so
         * over-forwarding is byte-safe. Covers:
         *   - direction (WASD) + arrow-key alt
         *   - face buttons (Z/X/C/V)
         *   - coin/start (1/5) as raw ASCII for games that map them there
         *   - ENTER/ESC for menus
         *   - SHIFT/CTRL/ALT for service combinations
         *   - XInput-derived bits below also drop into this same forwarder. */
        struct kb_map { int vk; unsigned char ascii; };
        static const struct kb_map km[] = {
            { 'W',          'w'  },
            { 'A',          'a'  },
            { 'S',          's'  },
            { 'D',          'd'  },
            { 'Z',          'z'  },
            { 'X',          'x'  },
            { 'C',          'c'  },
            { 'V',          'v'  },
            { '1',          '1'  },
            { '2',          '2'  },
            { '3',          '3'  },
            { '4',          '4'  },
            { '5',          '5'  },
            { '6',          '6'  },
            { VK_RETURN,    0x0d },
            { VK_ESCAPE,    0x1b },
            { VK_TAB,       0x09 },
            { VK_BACK,      0x08 },
            /* arrow keys: GLUT uses glutSpecialFunc for these so they may not
             * land here, but mirror them as ASCII WASD anyway in case the
             * game's keyboard callback duplicates the mapping. */
            { VK_UP,        'w'  },
            { VK_DOWN,      's'  },
            { VK_LEFT,      'a'  },
            { VK_RIGHT,     'd'  },
        };
        static unsigned char s_prev[256] = {0};
        for (size_t i = 0; i < sizeof km / sizeof km[0]; i++) {
            int down = (GetAsyncKeyState(km[i].vk) & 0x8000) ? 1 : 0;
            unsigned char a = km[i].ascii;
            if (down && !s_prev[a]) {
                kdown(a, 0, 0);
                loader_log(LOG_INFO, "[INPUT/KEY] DOWN '%c' (vk=0x%02x) -> guest cb @%08x",
                           (a >= 0x20 && a < 0x7f) ? a : '?', km[i].vk,
                           (unsigned)g_ul_gs_kbcb_down);
            }
            else if (!down && s_prev[a]) {
                kup(a, 0, 0);
                loader_log(LOG_INFO, "[INPUT/KEY] UP   '%c' (vk=0x%02x)",
                           (a >= 0x20 && a < 0x7f) ? a : '?', km[i].vk);
            }
            s_prev[a] = down;
        }
    }
    if (g_ul_gs_mousecb) {
        typedef void (*gs_mousecb_t)(int, int, int, int);
        gs_mousecb_t mcb = (gs_mousecb_t)(uintptr_t)g_ul_gs_mousecb;
        static int s_prev_kb_trig = 0;
        int gx = (int)(aim_x * 640.0f), gy = (int)(aim_y * 480.0f);
        if (trigger && !s_prev_kb_trig)      mcb(0 /*left*/, 0 /*GLUT_DOWN*/, gx, gy);
        else if (!trigger && s_prev_kb_trig) mcb(0,          1 /*GLUT_UP  */, gx, gy);
        s_prev_kb_trig = trigger;
    }

    /* ── Gun pose + trigger -> gun_work[player0] (the struct get_gun_position
     *    and is_gun_action read) ───────────────────────────────────────────
     * get_gun_position@0x080e506e: gun_work base 0x8651e60, stride 0x1a0;
     * X@+0x14, Y@+0x18, valid = (byte[+0xd] & 0x4)==0. get_gun_raster_position
     * shows the stored coords are CENTERED: raster_x = stored_x + 0x140 (320),
     * raster_y = 0xf0 (240) - stored_y. So stored_x ∈ [-320..+320] spans the
     * screen width and stored_y ∈ [-240..+240] spans the height (Y inverted).
     * Map the normalized [0..1] pose (top-left origin) accordingly:
     *   stored_x = (aim_x - 0.5) * 640   ;  stored_y = (0.5 - aim_y) * 480
     * TRIGGER: is_gun_action@0x080e51c6 reads (1<<action) of gun_work+0x64,
     * gated by gun_work+0x7 bit 0x10 (gun enabled). Gameplay fire is action 3
     * -> bit 0x8. So set +0x7 bit 0x10 (enable) + +0x64 bit 0x8 (fire) when the
     * trigger is held, clear bit 0x8 when released. We also OR the legacy +0x4
     * trigger flag (set_gun_flags semantics) for any consumer using that path. */
    if (g_ul_gs_gunwork_addr) {
        volatile uint8_t *gw = (volatile uint8_t*)(uintptr_t)g_ul_gs_gunwork_addr; /* player 0 */
        if (!IsBadWritePtr((void*)gw, 0x1a0)) {
            int sx = (int)((aim_x - 0.5f) * 640.0f);
            int sy = (int)((0.5f - aim_y) * 480.0f);
            *(volatile int*)(gw + 0x14) = sx;
            *(volatile int*)(gw + 0x18) = sy;
            gw[0x0d] &= (uint8_t)~0x04;                 /* mark pose valid */
            gw[0x07] |= 0x10;                           /* gun-action enabled (is_gun_action gate) */
            volatile uint32_t *act   = (volatile uint32_t*)(gw + 0x64); /* is_gun_action bitfield */
            volatile uint32_t *flags = (volatile uint32_t*)(gw + 0x04); /* legacy set_gun_flags    */
            if (trigger) { *act |= (1u << 3); *flags |= 0x00000001u; }  /* action 3 = fire */
            else         { *act &= ~(1u << 3); *flags &= ~0x00000001u; }
            /* DIAG: log every 60 frames so user sees gun pose is being written. */
            {
                extern int g_ul_swap_frame;
                static int s_last_logf = -1;
                if (g_ul_swap_frame != s_last_logf && (g_ul_swap_frame % 60) == 0) {
                    loader_log(LOG_INFO,
                        "[INPUT/GUN] f=%d aim=(%.2f,%.2f) -> stored=(%d,%d) trig=%d gun_work=%08x",
                        g_ul_swap_frame, aim_x, aim_y, sx, sy, trigger,
                        (unsigned)g_ul_gs_gunwork_addr);
                    s_last_logf = g_ul_swap_frame;
                }
            }
        }
    }

    /* 3b. START -> request the credited GAME main-mode (6 = mm_game_init).
     *     The game's attract FSM (RE'd) cycles main-mode [0x8660420] through
     *     2(segalogo) -> 3(advertise/demo) -> 4(title) -> 2 ... on timers, and
     *     NEVER enters the credited game on its own. mm_title_ctrl starts a
     *     game by writing [0x8660424]=6 once game_start_check() succeeds (which,
     *     with FreePlay on, only needs a player START). Under the SDL path the
     *     title's START read is dead, so we issue the SAME documented request:
     *     when START is pressed during an attract mode (2/3/4) with a credit,
     *     set main-mode-req=6. mode_ctrl then runs mm_game_init -> the real
     *     game (GameInit..GameLoop..GameOver), and on game-over the game's own
     *     code requests an attract mode again, closing the credit loop.
     *     Edge-triggered + guarded so we request once per START press and never
     *     fight the FSM once mode 6 (or its req) is already set. */
    /* OPT-IN FALLBACK (ULTRA_GS_FORCE_GAME=1). This FSM override is no longer
     * the primary start path - the natural JVS node write above now delivers
     * START to game_start_check@0x081083fe directly, so the game enters
     * mm_game_init on its own (heap-0 64MB patch lets the card sprite load). The
     * override is retained only as a diagnostic shortcut to force mode 6 without
     * waiting for game_start_check, edge-triggered + guarded so it never fights
     * the FSM once mode 6 (or its req) is already set. Default OFF. */
    {
        static int s_force = -1;
        if (s_force < 0) { const char *e = getenv("ULTRA_GS_FORCE_GAME"); s_force = (e && atoi(e)) ? 1 : 0; }
        if (s_force && g_ul_gs_mainmode_addr && g_ul_gs_mainreq_addr) {
            volatile int *mm = (volatile int*)(uintptr_t)g_ul_gs_mainmode_addr;
            volatile int *mr = (volatile int*)(uintptr_t)g_ul_gs_mainreq_addr;
            static int s_prev_start6 = 0;
            if (!IsBadReadPtr((void*)mm,4) && !IsBadWritePtr((void*)mr,4)) {
                int cur = *mm, req = *mr;
                int in_attract = (cur == 2 || cur == 3 || cur == 4);
                int already_game = (cur == 6 || req == 6);
                if (start && !s_prev_start6 && in_attract && !already_game) {
                    *mr = 6;   /* request the credited game */
                    loader_log(LOG_INFO, "[GSEVO] START in attract(mode=%d) -> request GAME mode 6", cur);
                }
            }
            s_prev_start6 = start;
        }
    }

    /* ── In-game PLAYER JOIN bridge (disasm-pinned) ─────────────────────────
     * The title gate game_start_check@0x081083fe sets player[0]+8 = 2 on the
     * START press that requests mode 6. But the in-game player-join handler
     * check_game_join@0x081087d2 (run every frame in game_mode_ctrl) only joins
     * players whose +8 == 0 (`mov 0x8(%ebx),%ecx; test; jne skip`) and is the ONLY
     * writer of the join flag player+4 bit4 (`orl $0x10,0x4`) + the per-player
     * score/card init. So a player who entered mode 6 via the title gate sits in
     * the game-mode standby ("PRESS START BUTTON" overlay, rail frozen) FOREVER:
     * +8 is already 2, so check_game_join skips it and never joins it into the
     * active mission. ev_game_player_init@0x810a8be even CLEARS the join flag
     * (`andl $0xf7ffffef,0x86617e4`) on game-mode entry, confirming the join must
     * be (re)asserted by check_game_join in mode 6.
     * FIX: once we are in mode 6 and player[0] is in the title-reserved state
     * (+8==2) but NOT yet joined (player+4 bit4 clear), clear player[0]+8 back to
     * 0 so the very next auto-play/lightgun START tap is consumed by
     * check_game_join - which joins the player (sets bit4, inits score, spends the
     * credit) and starts the rail. Done once per game entry (re-armed when +8
     * leaves 2), edge-guarded so we never fight the game once it has joined.
     * SHA-gated to GSEVO. */
    {
        volatile int     *mm  = (volatile int*)(uintptr_t)g_ul_gs_mainmode_addr;
        volatile uint8_t *pl0 = (volatile uint8_t*)0x086617e0u;
        if (mm && !IsBadReadPtr((void*)mm,4) && *mm == 6 &&
            !IsBadWritePtr((void*)pl0, 0xc)) {
            uint32_t p4 = *(volatile uint32_t*)(pl0 + 4);
            int      p8 = *(volatile int*)(pl0 + 8);
            int joined = (p4 >> 4) & 1;
            if (p8 == 2 && !joined) {
                *(volatile int*)(pl0 + 8) = 0;   /* re-open the join gate for check_game_join */
                static int s_logged = 0;
                if (s_logged < 3) {
                    loader_log(LOG_INFO, "[GSEVO] mode6: reopen player0 join gate (+8 2->0) so check_game_join joins on next START");
                    s_logged++;
                }
            }
        }
    }

    /* 4. Gun aim + trigger + START via the game's own GLUT callbacks. Only
     *    fires once the game has registered them (glut_has_pc_input_cbs). */
    extern int  glut_has_pc_input_cbs(void);
    extern void glut_feed_motion(int gx, int gy);
    extern void glut_feed_mouse(int button, int state, int gx, int gy);
    extern void glut_feed_key(unsigned char key, int down);
    if (!glut_has_pc_input_cbs()) return;

    /* Window-pixel aim. GSEVO renders 1360x768; clamp normalized pose to it.
     * (GSEVO normalizes against its own reported size, so exact match isn't
     * required, but staying in-range keeps the aim on-screen.) */
    int gx = (int)(aim_x * 1360.0f); if (gx < 0) gx = 0; if (gx > 1359) gx = 1359;
    int gy = (int)(aim_y * 768.0f);  if (gy < 0) gy = 0; if (gy > 767)  gy = 767;
    glut_feed_motion(gx, gy);

    /* Left mouse button = fire. Edge-detect so each pull is one down/up pair
     * the game's switch differ (rising edge) registers as a shot. */
    static int s_prev_trig = 0;
    if (trigger && !s_prev_trig)      glut_feed_mouse(0, 0, gx, gy);  /* GLUT_DOWN */
    else if (!trigger && s_prev_trig) glut_feed_mouse(0, 1, gx, gy);  /* GLUT_UP   */
    s_prev_trig = trigger;

    /* SPACE = primary/START/confirm. Edge-detect down/up so the game sees a
     * clean key press (its keyboard map sets switch bit 0x100). */
    static int s_prev_start = 0;
    if (start && !s_prev_start)      glut_feed_key(' ', 1);
    else if (!start && s_prev_start) glut_feed_key(' ', 0);
    s_prev_start = start;
}

/* ── GSEVO START re-assert - runs AFTER the guest vblank cb, BEFORE display cb ──
 * THE TIMING FIX. game_start_check@0x081083fe gates the credited game on
 *   mov 0xf0(%ebx),%eax ; testb $0x1,0x8(%eax)
 * i.e. bit0 of swstate+8 (0x08653428, player[0]+0xf0 -> +8). Disasm of
 * make_switch_data@0x080e83ae proved swstate+8 is the RISING-EDGE word
 * (`edi = current & ~prev ; mov %edi,0x8(...)`), and NONE of its node-byte
 * (node+4/+5/+7 -> bits 0x4/0x8/0x10/0x20) or xbox-key (0x86544b8 -> bits
 * 0x4/0x8) decode paths can ever raise bit 0x1 - so the natural decode can
 * NEVER pass game_start_check's `testb $0x1`. (GSDIAG2 confirmed swstate+8 only
 * ever held 0x100, never 0x1.)
 *
 * The per-frame order under the loader is:
 *   (1) jvs_poll -> gsevo_inject  : our START write to swstate+8
 *   (2) guest vblank cb           : make_switch_data REBUILDS swstate+8 (clobbers (1))
 *   (3) guest display cb          : the mode handler calls game_start_check (reads swstate+8)
 * So a write at (1) is always overwritten at (2) before (3) reads it. This
 * function is invoked by sdl_window_pump BETWEEN (2) and (3): it ORs bit0 (the
 * exact gate bit) - and bit8 0x100 for the title/menu `testl $0x101` readers -
 * straight into the rebuilt swstate+8 of the player-0 slot, so game_start_check
 * sees START on the same frame and the mode handler writes main-mode-req=6
 * (mm_rating_ctrl@0x080f5d93: `je; movl $0x6,0x8660424`) ENTIRELY on the game's
 * own path. No FSM override, no wedged gun thread. Edge-detected so it's a clean
 * one-frame press (the rising-edge word must drop to 0 between presses or the
 * gate, being edge-based, won't refire - and to avoid jamming menus). Cleared
 * when START is released. game_continue_check reads the identical field, so
 * CONTINUE works through this too. Inert unless g_ul_gsevo. SHA-gated.            */
void gsevo_post_vblank(void)
{
    /* Drive the loader START flag byte (0x08653438) that the in-guest natural-START
     * trampoline reads (between io_ctrl and event_ctrl, the only window where the
     * START bit survives to game_start_check). The stub ORs bit0 into swstate+8
     * when this flag is 1. We pulse it on the RISING edge of START for a short,
     * fixed window so it's a clean single press: game_start_check latches
     * player+8=2 on first success, and pulsing avoids holding the gate bit across
     * later menu screens. This runs from sdl_window_pump just before the guest
     * display cb (main_loop_func); the flag is consumed inside that same cb.
     * Inert unless g_ul_gsevo + the flag addr is set (GSEVO only). */
    if (!g_ul_gsevo || !g_ul_gs_startflag_addr) return;
    volatile uint8_t *flag = (volatile uint8_t*)(uintptr_t)g_ul_gs_startflag_addr;
    if (IsBadWritePtr((void*)flag, 1)) return;
    /* ONE-SHOT PATCH-VERIFY (ULTRA_GS_DIAG>=2): dump the loaded bytes at the
     * call-redirect site (0x80eea5b) and the stub (0x81ad363) so we can see if
     * the trampoline patches are actually present in the EXECUTED image. */
    {
        static int s_pvd = -1;
        static int s_pv_dumped = 0;
        if (s_pvd < 0) { const char *e = getenv("ULTRA_GS_DIAG"); s_pvd = (e && atoi(e)>=2) ? 1 : 0; }
        if (s_pvd && !s_pv_dumped) {
            volatile uint8_t *call = (volatile uint8_t*)0x080eea5bu;
            volatile uint8_t *stub = (volatile uint8_t*)0x081ad363u;
            if (!IsBadReadPtr((void*)call,5) && !IsBadReadPtr((void*)stub,16)) {
                loader_log(LOG_INFO, "[GSPATCH] call@80eea5b: %02x %02x %02x %02x %02x  (want e8 03 e9 0b 00)",
                           call[0],call[1],call[2],call[3],call[4]);
                loader_log(LOG_INFO, "[GSPATCH] stub@81ad363: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x  (want a0 18 34 65 08 ...)",
                           stub[0],stub[1],stub[2],stub[3],stub[4],stub[5],stub[6],stub[7],
                           stub[8],stub[9],stub[10],stub[11],stub[12],stub[13],stub[14],stub[15]);
                s_pv_dumped = 1;
            }
        }
        /* Per-frame branch probe: log [[0x8273348]+0x10] (the js gate that decides
         * whether `call event_ctrl` - and thus the stub - runs) when START is held. */
        if (s_pvd && s_gs_start_held) {
            static int s_logged = 0;
            if (s_logged < 6) {
                volatile uint32_t *pp = (volatile uint32_t*)0x08273348u;
                int32_t gate = 0x7fffffff; uint32_t ptr = 0;
                if (!IsBadReadPtr((void*)pp,4)) { ptr = *pp;
                    if (ptr && !IsBadReadPtr((void*)(uintptr_t)(ptr+0x10),4))
                        gate = *(int32_t*)(uintptr_t)(ptr+0x10); }
                volatile uint8_t *gb = (volatile uint8_t*)0x08653428u;
                int g8 = (gb && !IsBadReadPtr((void*)gb,1)) ? *gb : -1;
                extern int g_ul_swap_frame;
                loader_log(LOG_INFO, "[GSBR] f=%d evctrl_gate(ptr=%08x +0x10=%d %s) swstate+8=%02x",
                           g_ul_swap_frame, ptr, gate, (gate<0?"SKIP":"call"), g8);
                s_logged++;
            }
        }
    }
    static int s_prev = 0, s_pulse = 0;
    static int s_dbg = -1;
    if (s_dbg < 0) { const char *e = getenv("ULTRA_GS_DIAG"); s_dbg = (e && atoi(e)>=2) ? 1 : 0; }
    if (s_gs_start_held && !s_prev) {
        s_pulse = 4;   /* assert START for ~4 frames so the gate is sampled reliably */
        if (s_dbg) { extern int g_ul_swap_frame;
            loader_log(LOG_INFO, "[GSPV] f=%d START rising edge -> flag pulse", g_ul_swap_frame); }
    }
    /* DIAG ISOLATION (ULTRA_GS_FORCE_STARTBIT=1): hold the gate flag asserted every
     * frame to prove the stub->game_start_check path independent of input timing. */
    static int s_forcebit = -1;
    if (s_forcebit < 0) { const char *e = getenv("ULTRA_GS_FORCE_STARTBIT"); s_forcebit = (e && atoi(e)) ? 1 : 0; }
    if (s_forcebit)       { *flag = 1; }
    else if (s_pulse > 0) { *flag = 1; s_pulse--; }
    else                  { *flag = 0; }
    s_prev = s_gs_start_held;
}

/* Response buffer */
static uint8_t g_resp_buf[256];
static int     g_resp_len = 0;
static int     g_resp_pos = 0;

/* ------------------------------------------------------------------ */
/* XInput (gamepad) support                                            */
/* ------------------------------------------------------------------ */
typedef DWORD (WINAPI *pfn_XInputGetState)(DWORD, XINPUT_STATE*);
static pfn_XInputGetState _XInputGetState = NULL;

static void xinput_init(void)
{
    HMODULE xi = LoadLibraryA("xinput1_4.dll");
    if (!xi) xi = LoadLibraryA("xinput1_3.dll");
    if (xi) _XInputGetState = (pfn_XInputGetState)
        GetProcAddress(xi, "XInputGetState");
}

/* ------------------------------------------------------------------ */
/* SDL3 gamepad support - zero-config for ALL controllers.            */
/* SDL3 ships a per-vendor mapping DB, so Xbox / PlayStation (DS4/DS5) */
/* / Nintendo (Switch Pro/JoyCon) / generic pads all map to the same  */
/* position-based layout (SOUTH/EAST/WEST/NORTH, LEFTX/Y...) with no   */
/* user setup. We poll up to 2 open gamepads as players 1-2. SDL3 is   */
/* already loaded for the window; we just bring up the GAMEPAD subsys. */
/* ------------------------------------------------------------------ */
#define SDL_INIT_GAMEPAD_      0x00002000u
/* SDL_GamepadButton */
#define SDLB_SOUTH 0  /* A / Cross    */
#define SDLB_EAST  1  /* B / Circle   */
#define SDLB_WEST  2  /* X / Square   */
#define SDLB_NORTH 3  /* Y / Triangle */
#define SDLB_BACK  4
#define SDLB_START 6
#define SDLB_LSHOULDER 9
#define SDLB_RSHOULDER 10
#define SDLB_DPAD_UP    11
#define SDLB_DPAD_DOWN  12
#define SDLB_DPAD_LEFT  13
#define SDLB_DPAD_RIGHT 14
/* SDL_GamepadAxis */
#define SDLA_LEFTX 0
#define SDLA_LEFTY 1
#define SDLA_RIGHT_TRIGGER 5
#define SDLA_LEFT_TRIGGER  4

typedef int      (*pfn_SDL_InitSubSystem)(uint32_t);
typedef uint32_t*(*pfn_SDL_GetGamepads)(int*);
typedef void*    (*pfn_SDL_OpenGamepad)(uint32_t);
typedef int      (*pfn_SDL_GetGamepadButton)(void*, int);   /* SDL3 bool == int */
typedef int16_t  (*pfn_SDL_GetGamepadAxis)(void*, int);
typedef void     (*pfn_SDL_UpdateGamepads)(void);
typedef void     (*pfn_SDL_free)(void*);

static pfn_SDL_InitSubSystem    _SDL_InitSubSystem;
static pfn_SDL_GetGamepads      _SDL_GetGamepads;
static pfn_SDL_OpenGamepad      _SDL_OpenGamepad;
static pfn_SDL_GetGamepadButton _SDL_GetGamepadButton;
static pfn_SDL_GetGamepadAxis   _SDL_GetGamepadAxis;
static pfn_SDL_UpdateGamepads   _SDL_UpdateGamepads;
static pfn_SDL_free             _SDL_free;
static int   g_sdl_pad_ready = 0;
static void *g_pad[2] = {0,0};       /* SDL_Gamepad* for players 1-2 */

static void sdlpad_init(void)
{
    HMODULE s = GetModuleHandleA("SDL3.dll");
    if (!s) s = LoadLibraryA("SDL3.dll");
    if (!s) return;
    _SDL_InitSubSystem    = (pfn_SDL_InitSubSystem)   (void*)GetProcAddress(s, "SDL_InitSubSystem");
    _SDL_GetGamepads      = (pfn_SDL_GetGamepads)     (void*)GetProcAddress(s, "SDL_GetGamepads");
    _SDL_OpenGamepad      = (pfn_SDL_OpenGamepad)     (void*)GetProcAddress(s, "SDL_OpenGamepad");
    _SDL_GetGamepadButton = (pfn_SDL_GetGamepadButton)(void*)GetProcAddress(s, "SDL_GetGamepadButton");
    _SDL_GetGamepadAxis   = (pfn_SDL_GetGamepadAxis)  (void*)GetProcAddress(s, "SDL_GetGamepadAxis");
    _SDL_UpdateGamepads   = (pfn_SDL_UpdateGamepads)  (void*)GetProcAddress(s, "SDL_UpdateGamepads");
    _SDL_free             = (pfn_SDL_free)            (void*)GetProcAddress(s, "SDL_free");
    if (_SDL_InitSubSystem && _SDL_GetGamepads && _SDL_OpenGamepad &&
        _SDL_GetGamepadButton && _SDL_GetGamepadAxis) {
        _SDL_InitSubSystem(SDL_INIT_GAMEPAD_);
        g_sdl_pad_ready = 1;
        loader_log(LOG_INFO, "[JVS] SDL3 gamepad layer ready (Xbox/PlayStation/Nintendo zero-config)");
    }
}

/* Open the first <=2 connected gamepads and refresh their state. */
static void sdlpad_refresh_open(void)
{
    if (_SDL_UpdateGamepads) _SDL_UpdateGamepads();
    if (g_pad[0] && g_pad[1]) return;            /* already have two */
    int n = 0;
    uint32_t *ids = _SDL_GetGamepads ? _SDL_GetGamepads(&n) : NULL;
    if (ids) {
        for (int i = 0, slot = 0; i < n && slot < 2; i++) {
            if (g_pad[slot]) { slot++; if (slot >= 2) break; }
            if (!g_pad[slot]) { g_pad[slot] = _SDL_OpenGamepad(ids[i]); if (g_pad[slot]) slot++; }
        }
        if (_SDL_free) _SDL_free(ids);
    }
}

/* Map an open SDL gamepad to player p's JVS bits + analog. After Burner is a
 * flight game: left stick = the flight stick (analog 0/1), face buttons & the
 * triggers = fire/missile, START = start, BACK = service. */
static void sdlpad_poll_player(int p, int *coin_down)
{
    void *g = g_pad[p];
    if (!g) return;
    #define BTN(b) (_SDL_GetGamepadButton(g, (b)))
    if (BTN(SDLB_DPAD_UP))    g_jvs.switches[p] |= JVS_UP;
    if (BTN(SDLB_DPAD_DOWN))  g_jvs.switches[p] |= JVS_DOWN;
    if (BTN(SDLB_DPAD_LEFT))  g_jvs.switches[p] |= JVS_LEFT;
    if (BTN(SDLB_DPAD_RIGHT)) g_jvs.switches[p] |= JVS_RIGHT;
    /* Fire / missile: any face button OR the triggers/shoulders so it "just works". */
    if (BTN(SDLB_SOUTH) || (!g_ul_gsevo && BTN(SDLB_RSHOULDER))) g_jvs.switches[p] |= JVS_BTN1; /* fire    */
    if (BTN(SDLB_WEST)  || BTN(SDLB_LSHOULDER)) g_jvs.switches[p] |= JVS_BTN2; /* missile */
    if (BTN(SDLB_EAST))  g_jvs.switches[p] |= JVS_BTN3; /* view change */
    if (BTN(SDLB_NORTH)) g_jvs.switches[p] |= JVS_BTN4;
    if (BTN(SDLB_START)) g_jvs.switches[p] |= JVS_START;
    if (BTN(SDLB_BACK))  coin_down[p] = 1;             /* Select/Back = insert coin */
    { int tt = 32767 * g_ul_trigger_thresh / 100; if (tt < 1) tt = 1; if (tt > 32766) tt = 32766;
      if (_SDL_GetGamepadAxis(g, SDLA_RIGHT_TRIGGER) > tt) g_jvs.switches[p] |= JVS_BTN1; /* trigger = fire    */
      if (_SDL_GetGamepadAxis(g, SDLA_LEFT_TRIGGER)  > tt) g_jvs.switches[p] |= JVS_BTN2; } /* trigger = missile */
    /* Left stick -> analog flight stick (centered at 0x8000) + digital fallback.
     * Use int (not int16_t) so negating the -32768 extreme can't overflow. */
    int lx = _SDL_GetGamepadAxis(g, SDLA_LEFTX);
    int ly = _SDL_GetGamepadAxis(g, SDLA_LEFTY);
    int ny = -ly; if (ny > 32767) ny = 32767; if (ny < -32768) ny = -32768;
    g_jvs.analog[p*2]   = (uint16_t)(lx + 32768);
    g_jvs.analog[p*2+1] = (uint16_t)(ny + 32768);    /* JVS up = +; SDL up = - */
    if (lx < -12000) g_jvs.switches[p] |= JVS_LEFT;
    if (lx >  12000) g_jvs.switches[p] |= JVS_RIGHT;
    if (ly < -12000) g_jvs.switches[p] |= JVS_UP;
    if (ly >  12000) g_jvs.switches[p] |= JVS_DOWN;
    #undef BTN
}

/* ------------------------------------------------------------------ */
/* Read current input state from keyboard + XInput                    */
/* ------------------------------------------------------------------ */
static void poll_inputs(void)
{
    /* Clear state */
    g_jvs.switches[0] = 0;
    g_jvs.switches[1] = 0;
    g_jvs.system_switches = 0;
    /* Center the analog axes each frame so a missing/idle stick reads NEUTRAL
     * (0x8000) instead of a stuck full-deflection. A connected pad overwrites
     * the channels it drives below. */
    for (int i = 0; i < 8; i++) g_jvs.analog[i] = 32768;

    /*
     * JVS switch bit layout (standard):
     *   Bit 15: Test
     *   Bit 14: Tilt1
     *   Bit 13: Tilt2
     *   Bit 12: Tilt3
     *   Bit 11: (unused)
     *   Bit 10: Start
     *   Bit 9:  Service
     *   Bit 8:  Up
     *   Bit 7:  Down
     *   Bit 6:  Left
     *   Bit 5:  Right
     *   Bit 4:  Button1
     *   Bit 3:  Button2
     *   Bit 2:  Button3
     *   Bit 1:  Button4
     *   Bit 0:  Button5
     */
    #define KEY(vk) (GetAsyncKeyState(vk) & 0x8000)
    static int s_coin_prev[2] = {0, 0};
    int coin_down[2] = {0, 0};

    /* Player 1 - keyboard */
    if (KEY('W'))           g_jvs.switches[0] |= JVS_UP;
    if (KEY('S'))           g_jvs.switches[0] |= JVS_DOWN;
    if (KEY('A'))           g_jvs.switches[0] |= JVS_LEFT;
    if (KEY('D'))           g_jvs.switches[0] |= JVS_RIGHT;
    if (KEY('Z'))           g_jvs.switches[0] |= JVS_BTN1;
    if (KEY('X'))           g_jvs.switches[0] |= JVS_BTN2;
    if (KEY('C'))           g_jvs.switches[0] |= JVS_BTN3;
    if (KEY('V'))           g_jvs.switches[0] |= JVS_BTN4;
    if (KEY('1'))           g_jvs.switches[0] |= JVS_START;
    if (KEY('5'))           coin_down[0] = 1;

    /* Player 2 - arrow keys */
    if (KEY(VK_UP))         g_jvs.switches[1] |= JVS_UP;
    if (KEY(VK_DOWN))       g_jvs.switches[1] |= JVS_DOWN;
    if (KEY(VK_LEFT))       g_jvs.switches[1] |= JVS_LEFT;
    if (KEY(VK_RIGHT))      g_jvs.switches[1] |= JVS_RIGHT;
    if (KEY(VK_NUMPAD1))    g_jvs.switches[1] |= JVS_BTN1;
    if (KEY(VK_NUMPAD2))    g_jvs.switches[1] |= JVS_BTN2;
    if (KEY(VK_NUMPAD3))    g_jvs.switches[1] |= JVS_BTN3;
    if (KEY(VK_NUMPAD4))    g_jvs.switches[1] |= JVS_BTN4;
    if (KEY('2'))           g_jvs.switches[1] |= JVS_START;
    if (KEY('6'))           coin_down[1] = 1;

    /* Keyboard flight stick: RAMP the digital directions (WASD / arrows) into the
     * ANALOG axes so a keyboard banks SMOOTHLY - a gentle tap nudges the stick (a
     * shallow bank), and only a sustained full push reaches the edge that triggers
     * a barrel roll, exactly like the cabinet's analog stick. Without this the
     * keyboard slammed the axis to full instantly (instant barrel roll / could only
     * pin one way). A connected analog pad overwrites these axes further below, so
     * this affects keyboard play only. Ramps in ~0.7s, recenters fast on release. */
    {
        static float kx[2] = {0,0}, ky[2] = {0,0};
        for (int p = 0; p < 2; p++) {
            float tx = ((g_jvs.switches[p]&JVS_RIGHT)?1.0f:0.0f) - ((g_jvs.switches[p]&JVS_LEFT)?1.0f:0.0f);
            float ty = ((g_jvs.switches[p]&JVS_UP)?1.0f:0.0f)    - ((g_jvs.switches[p]&JVS_DOWN)?1.0f:0.0f);
            float sx = (tx!=0.0f)?0.05f:0.14f, sy = (ty!=0.0f)?0.05f:0.14f;
            if (kx[p]<tx){ kx[p]+=sx; if(kx[p]>tx)kx[p]=tx; } else if(kx[p]>tx){ kx[p]-=sx; if(kx[p]<tx)kx[p]=tx; }
            if (ky[p]<ty){ ky[p]+=sy; if(ky[p]>ty)ky[p]=ty; } else if(ky[p]>ty){ ky[p]-=sy; if(ky[p]<ty)ky[p]=ty; }
            if (kx[p]!=0.0f || ky[p]!=0.0f) {
                int ax = 32768 + (int)(kx[p]*32767.0f);
                int ay = 32768 + (int)(ky[p]*32767.0f);
                if(ax<0)ax=0; if(ax>65535)ax=65535;
                if(ay<0)ay=0; if(ay>65535)ay=65535;
                g_jvs.analog[p*2]   = (uint16_t)ax;
                g_jvs.analog[p*2+1] = (uint16_t)ay;
            }
        }
    }

    /* Service / Test */
    /* F1/F2 moved into the TEST/SERVICE block below, which also drives the
     * security-board front panel -- the only route GSEVO actually reads. */

    /* XInput gamepad (pad 0 = player 1, pad 1 = player 2) */
    if (_XInputGetState) {
        for (int p = 0; p < 2; p++) {
            XINPUT_STATE xs;
            if (_XInputGetState(p, &xs) != ERROR_SUCCESS) continue;
            WORD btn = xs.Gamepad.wButtons;

            /* In-game overlay combos (player 1 only): Start+Select opens the
             * pause/settings panel, LB+RB opens it on the exit options. Suppress
             * the individual Start/Coin on the combo so opening the menu doesn't
             * also start/coin the game. Edge-detected. */
            if (p == 0) {
                extern void ingame_overlay_request(int exit_screen);
                static WORD s_prevcombo = 0;
                int sel   = (btn & XINPUT_GAMEPAD_START) && (btn & XINPUT_GAMEPAD_BACK);
                int exitc = (btn & XINPUT_GAMEPAD_LEFT_SHOULDER) && (btn & XINPUT_GAMEPAD_RIGHT_SHOULDER);
                WORD now = (sel ? 1 : 0) | (exitc ? 2 : 0);
                if (sel   && !(s_prevcombo & 1)) ingame_overlay_request(0);
                if (exitc && !(s_prevcombo & 2)) ingame_overlay_request(1);
                s_prevcombo = now;
                if (sel) btn &= ~(XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK);  /* don't leak to game */
            }

            if (btn & XINPUT_GAMEPAD_DPAD_UP)    g_jvs.switches[p] |= JVS_UP;
            if (btn & XINPUT_GAMEPAD_DPAD_DOWN)  g_jvs.switches[p] |= JVS_DOWN;
            if (btn & XINPUT_GAMEPAD_DPAD_LEFT)  g_jvs.switches[p] |= JVS_LEFT;
            if (btn & XINPUT_GAMEPAD_DPAD_RIGHT) g_jvs.switches[p] |= JVS_RIGHT;
            if (btn & XINPUT_GAMEPAD_A)          g_jvs.switches[p] |= JVS_BTN1;
            if (btn & XINPUT_GAMEPAD_B)          g_jvs.switches[p] |= JVS_BTN2;
            if (btn & XINPUT_GAMEPAD_X)          g_jvs.switches[p] |= JVS_BTN3;
            if (btn & XINPUT_GAMEPAD_Y)          g_jvs.switches[p] |= JVS_BTN4;
            if (btn & XINPUT_GAMEPAD_START)      g_jvs.switches[p] |= JVS_START;
            if (btn & XINPUT_GAMEPAD_BACK)       coin_down[p] = 1;   /* Back = coin */
            { int tt = 255 * g_ul_trigger_thresh / 100; if (tt < 1) tt = 1; if (tt > 254) tt = 254;
              if (xs.Gamepad.bRightTrigger > tt) g_jvs.switches[p] |= JVS_BTN1;
              if (xs.Gamepad.bLeftTrigger  > tt) g_jvs.switches[p] |= JVS_BTN2; }

            g_jvs.analog[p*2]   = (uint16_t)(xs.Gamepad.sThumbLX + 32768);
            g_jvs.analog[p*2+1] = (uint16_t)(xs.Gamepad.sThumbLY + 32768);

            /* GSEVO bridge: also forward XInput buttons through the game's
             * native GLUT keyboard callback so the pad reaches games that
             * only listen to GLUT input (GSEVO). P1 only - P2 uses the
             * separate gamepad slot's own bindings. SHA-gated. */
            if (p == 0 && g_ul_gs_kbcb_down && g_ul_gs_kbcb_up) {
                typedef void (*gs_kbcb_t)(unsigned char, int, int);
                gs_kbcb_t kdown = (gs_kbcb_t)(uintptr_t)g_ul_gs_kbcb_down;
                gs_kbcb_t kup   = (gs_kbcb_t)(uintptr_t)g_ul_gs_kbcb_up;
                /* DPAD -> wasd, ABXY -> zxcv, START -> SPACE, BACK -> 5 (coin) */
                struct xb_map { int xbtn; unsigned char ascii; };
                static const struct xb_map xm[] = {
                    { XINPUT_GAMEPAD_DPAD_UP,     'w'  },
                    { XINPUT_GAMEPAD_DPAD_DOWN,   's'  },
                    { XINPUT_GAMEPAD_DPAD_LEFT,   'a'  },
                    { XINPUT_GAMEPAD_DPAD_RIGHT,  'd'  },
                    { XINPUT_GAMEPAD_A,           'z'  },
                    { XINPUT_GAMEPAD_B,           'x'  },
                    { XINPUT_GAMEPAD_X,           'c'  },
                    { XINPUT_GAMEPAD_Y,           'v'  },
                    /* START is NOT in this table any more: SPACE decodes to bit 0x100, which is
                     * PUSH1 -- the TRIGGER, not START. Pad START was firing a shot every press.
                     * Real START is a GLUT SPECIAL key (F1), driven in gsevo_inject instead. */
                    { XINPUT_GAMEPAD_BACK,        '5'  }, /* BACK = coin     */
                    /* LB 'q' and RB 'e' removed: 'q' is a useless PUSH6 alias and 'e' is
                     * PUSH8, the camera/demo selector -- see the correction in the pad-aim
                     * block. LB is the reload button now; RB is fire via RT/A only. */
                };
                static unsigned char s_pxb[256] = {0};
                for (size_t i = 0; i < sizeof xm / sizeof xm[0]; i++) {
                    int down = (btn & xm[i].xbtn) ? 1 : 0;
                    unsigned char a = xm[i].ascii;
                    if (down && !s_pxb[a])       kdown(a, 0, 0);
                    else if (!down && s_pxb[a])  kup(a, 0, 0);
                    s_pxb[a] = down;
                }
            }
        }
    }

    /* SDL3 gamepads - covers PlayStation/Nintendo/generic (and Xbox too) with
     * zero config. Polled after XInput so a controller on either path works. */
    if (g_sdl_pad_ready) {
        sdlpad_refresh_open();
        sdlpad_poll_player(0, coin_down);
        sdlpad_poll_player(1, coin_down);

        /* ---- TEST = L3, SERVICE = R3 -------------------------------------------
         *
         * THE ROUTE MATTERS MORE THAN THE BUTTON. Until now the only TEST/SERVICE
         * source on this board was F1/F2 above, and both went out on the JVS reply
         * path -- which GSEVO never reads, because it never issues READ_SWITCHES. So
         * TEST and SERVICE were unreachable on this title from ANY input, pad or
         * keyboard.
         *
         * The route that does work was already written and never called. The runtime
         * emulates the security board's front panel (lindbergh_boards.c, port 0x38)
         * with exactly the bit polarity the games decode -- lindbergh_boards.c:119-120
         * clears 0x004 for TEST and 0x008 for SERVICE, and GSEVO's amDipswGetData
         * inverts and tests those same two bits. The setter for it,
         * lindbergh_boards_set_switch(), had ZERO callers anywhere in the tree, so
         * g_testSw/g_serviceSw sat at 0 for the whole life of every Lindbergh process.
         * That is the real reason there was no Test or Service button on this board.
         *
         * HOLD, DO NOT TAP. The TEST line is LEVEL-triggered end to end and there is
         * no debounce anywhere: a single true frame is enough to trip it, and the game
         * never re-polls the level afterwards. A stick that gets bumped mid-game would
         * otherwise be indistinguishable from a deliberate press. On After Burner
         * Climax that would drop the player into its test screen; on Ghost Squad it is
         * worse, because GSEVO's TEST branch calls exit_application() -- the game
         * QUITS rather than opening a menu. So both need a deliberate hold.
         *
         *   ULTRA_LIND_TESTSVC        0 = no pad route to TEST/SERVICE at all
         *   ULTRA_LIND_TESTSVC_HOLD   hold time in ms (default 900, 0 = instant) */
        {
            static int   s_ts_on = -1, s_ts_hold = 900;
            static DWORD s_l3_since = 0, s_r3_since = 0;
            int sw_test = 0, sw_service = 0;
            if (s_ts_on < 0) {
                const char *e = getenv("ULTRA_LIND_TESTSVC");
                s_ts_on = (e && *e) ? atoi(e) : 1;
                if ((e = getenv("ULTRA_LIND_TESTSVC_HOLD")) && *e) s_ts_hold = atoi(e);
                if (s_ts_hold < 0)     s_ts_hold = 0;
                if (s_ts_hold > 10000) s_ts_hold = 10000;
            }
            /* the keyboard route keeps its original meaning, with no hold */
            if (KEY(VK_F1)) sw_service = 1;
            if (KEY(VK_F2)) sw_test    = 1;
            /* GATED TO GSEVO, at the owner's instruction 2026-09-20. After Burner Climax
             * is a finished, locked title on this same shared file and its TEST branch
             * enters ITS test screen -- reachable, from a control ABC's player is already
             * holding. Ungated that is a locked-game regression for no gain, since ABC's
             * test screen was never the thing being asked for. The keyboard route (F1/F2)
             * and the front-panel call below are NOT gated: they behave for every title
             * exactly as they did before, so nothing is taken away from ABC either. */
            if (s_ts_on && g_ul_gsevo && _XInputGetState) {
                XINPUT_STATE xt;
                DWORD now = GetTickCount();
                if (_XInputGetState(0, &xt) == ERROR_SUCCESS) {
                    WORD b = xt.Gamepad.wButtons;
                    if (b & 0x0040) {                 /* L3 = TEST    */
                        if (!s_l3_since) s_l3_since = now;
                        if ((int)(now - s_l3_since) >= s_ts_hold) sw_test = 1;
                    } else s_l3_since = 0;
                    if (b & 0x0080) {                 /* R3 = SERVICE */
                        if (!s_r3_since) s_r3_since = now;
                        if ((int)(now - s_r3_since) >= s_ts_hold) sw_service = 1;
                    } else s_r3_since = 0;
                } else { s_l3_since = 0; s_r3_since = 0; }
            }
            /* the front panel: the route that reaches the game */
            lindbergh_boards_set_switch(sw_test, sw_service);
            /* and the JVS wire, which is what After Burner Climax's node write reads */
            if (sw_service) g_jvs.switches[0]     |= JVS_SERVICE;
            if (sw_test)    g_jvs.system_switches |= JVS_SYS_TEST;
        }
    }

    /* ── Auto-play test driver (ULTRA_AUTO_PLAY) ─────────────────────────
     * Deterministically walk boot → coin → START → confirm so the actual
     * gameplay render path can be reached headlessly for diagnostics, with
     * no window focus or manual input. Off unless the env var is set. After
     * dropping one coin it cycles START then the fire/confirm button, which
     * brute-forces through aircraft-select / briefing into the mission. */
    {
        static int s_auto = -1;
        if (s_auto < 0) { const char *e = getenv("ULTRA_AUTO_PLAY"); s_auto = (e && atoi(e)) ? 1 : 0; }
        if (s_auto) {
            static unsigned af = 0; af++;
            static int s_stop = -2;     /* ULTRA_AUTO_STOP=N: cease all auto-input at frame N
                                         * (so a "press START to skip" cutscene plays unskipped) */
            if (s_stop == -2) { const char *e = getenv("ULTRA_AUTO_STOP"); s_stop = e ? atoi(e) : -1; }
            if (s_stop < 0 || (int)af < s_stop) {
                if (af == 150) coin_down[0] = 1;                 /* drop exactly one coin */
                if (af > 210) {
                    unsigned ph = (af - 210) % 80;
                    if (ph < 12)                  g_jvs.switches[0] |= JVS_START; /* tap START  */
                    else if (ph >= 28 && ph < 40) g_jvs.switches[0] |= JVS_BTN1;  /* tap CONFIRM/FIRE */
                }
                /* GSEVO rail-shooter autopilot: once in the credited game (mode 6),
                 * actively PLAY - sweep the gun aim in a lissajous pattern across
                 * the screen and hold fire most of the time so the on-rails stage
                 * is engaged (enemies shot, score climbs, the rail advances toward
                 * its end / the player eventually dies), driving the loop forward to
                 * game-over -> attract. Without active play a rail shooter just
                 * stalls on its first standby frame. Inert unless GSEVO + mode 6. */
                if (g_ul_gsevo && g_ul_gs_mainmode_addr) {
                    volatile int *mm = (volatile int*)(uintptr_t)g_ul_gs_mainmode_addr;
                    if (!IsBadReadPtr((void*)mm,4) && *mm == 6) {
                        float t = (float)af;
                        /* lissajous sweep, comfortably inside the screen */
                        s_gs_auto_aim_x = 0.5f + 0.34f * sinf(t * 0.061f);
                        s_gs_auto_aim_y = 0.5f + 0.26f * sinf(t * 0.043f + 1.1f);
                        /* fire ~75% duty so menus that need a START tap still get
                         * the gaps, but the rail gets near-continuous fire */
                        s_gs_auto_fire  = ((af % 8) < 6) ? 1 : 0;
                        s_gs_auto_active = 1;
                        /* also tap START periodically to clear briefings/skip prompts */
                        if ((af % 40) < 6) g_jvs.switches[0] |= JVS_START;
                    } else {
                        s_gs_auto_active = 0;
                    }
                }
            } else {
                s_gs_auto_active = 0;
            }
        }
    }

    /* Coin insert on the rising edge only (so one press = one credit). ABC does
     * no per-frame JVS coin read, so on each inserted coin we deposit a credit
     * straight into the game's credit counters via amCreditAdd(slot,1,1). */
    int gs_coin_pulse = 0;   /* set on slot-0 coin rising edge (for GSEVO deposit) */
    for (int p = 0; p < 2; p++) {
        if (coin_down[p] && !s_coin_prev[p]) {
            g_jvs.coins[p]++;
            if (p == 0) gs_coin_pulse = 1;
            /* Deposit a credit into the game's playable credit array (the value it
             * displays + spends on START). ABC does no per-frame JVS coin read, so
             * we do it directly; then poke the modified flags so the HUD refreshes. */
            if (g_ul_credit_byte_addr) {
                volatile uint8_t *c = (volatile uint8_t*)(uintptr_t)(g_ul_credit_byte_addr + p);
                if (!IsBadWritePtr((void*)c, 1) && *c < 99) (*c)++;
                if (g_ul_credit_modflag_addr) {
                    for (int s = 0; s < 3; s++) {
                        volatile uint32_t *f = (volatile uint32_t*)(uintptr_t)(g_ul_credit_modflag_addr + s*4);
                        if (!IsBadWritePtr((void*)f, 4)) *f = 1;
                    }
                }
            }
            loader_log(LOG_INFO, "[COIN] slot %d inserted (count=%u, credit++)", p, g_jvs.coins[p]);
        }
        s_coin_prev[p] = coin_down[p];
    }

    /* ── Free Play ───────────────────────────────────────────────────────
     * Hold the game's native FreePlay flag (and its HUD mirror) asserted every
     * frame. =1 makes amCreditGetStatus report CREDIT_ENOUGH so START works with
     * no coins and nothing is spent, and the credit HUD reads "FREEPLAY". Must be
     * per-frame: the game re-seeds these from NVRAM defaults (0) periodically.
     * No-op until the game's credit lib has mapped the bytes (IsBadWritePtr-guarded). */
    if (g_ul_freeplay && g_ul_freeplay_addr) {
        volatile uint8_t *m = (volatile uint8_t*)(uintptr_t)g_ul_freeplay_addr;
        if (!IsBadWritePtr((void*)m, 1)) *m = 1;
        if (g_ul_freeplay_mirror_addr) {
            volatile uint8_t *d = (volatile uint8_t*)(uintptr_t)g_ul_freeplay_mirror_addr;
            if (!IsBadWritePtr((void*)d, 1)) *d = 1;
        }
    }

    /* Learn/remove clone-controller resting drift (uses the final per-source
     * analog values just gathered above). Runs every poll until calibrated. */
    jvs_autocal_tick();

    /* ── NVRAM flight-stick calibration fix (After Burner Climax) ───────────────
     * ROOT CAUSE (RE'd): the flight model reads a per-axis cal block @0x958bdb8
     * derived from a CalibrateConfig in the NVRAM-backed config struct via the
     * game's apply_calibration_parameter() @0x08095132 - which ONLY runs from the
     * test-menu calibration screen. With blank/degenerate NVRAM and no calibration
     * ever run, the block stays zero -> RANGE=0 -> divide-by-0 -> roll clamps to
     * -1.0 (full left). FIX: seed the game's FACTORY-DEFAULT CalibrateConfig (rodata
     * 0x082c3714 = 36 77 b4 57 98 d8 52 80 ac 80 18 18) into the active config
     * (0x9f3bb9c/a0/a4) and run the game's own apply_calibration_parameter() once,
     * exactly reproducing a factory-calibrated cabinet for ALL THREE axes. Then we
     * feed a true 0x8000-centered stick (no feed-side remap). One-shot after boot. */
    /* NOTE: mode 2 (direct calibration-block write at 0x958bdb8) was fully
     * implemented + RE'd (formula roll=-(RANGE-cfg)/(RANGE-(trimA+Cw)) etc.) but
     * the game's normalizer is CHAOTIC outside a narrow band: a written
     * calibration only yields a clean monotonic map over cfg∈[H..3H] -> roll ±0.5
     * (half-throw), with non-monotonic glitches at the edges + a hard glitch at
     * cfg=4H. So it CANNOT give clean full ±1 control. The DEGENERATE formula
     * (no calibration written, mode 1) instead has a full-range monotonic region
     * (raw 49152..65535 -> -1..+1, center 57200) - so the feed-side remap (mode 1)
     * is the robust full-throw fix. Mode 2 removed; findings in memory.            */

    /* DEBUG (gated): read the game's OWN normalized analog/roll floats so we can see
     * what value our neutral 0x8000 actually produces inside the flight model.
     * Candidates from the FINDER: player-input struct 0x958bcfc/d00/d04 and IO node
     * 0x834791c/20/24. ULTRA_READ_FLOATS=1. */
    if (getenv("ULTRA_READ_FLOATS")) { static unsigned rf=0; if(((rf++)%60)==0){
        static const uint32_t A[]={0x958bcfc,0x958bd00,0x958bd04,0x834791c,0x8347920,0x8347924};
        float v[6];
        for(int i=0;i<6;i++) v[i]=IsBadReadPtr((void*)(uintptr_t)A[i],4)?-999.f:*(volatile float*)(uintptr_t)A[i];
        loader_log(LOG_WARN,"[RDFLOAT] pi[%.4f %.4f %.4f] io[%.4f %.4f %.4f]",v[0],v[1],v[2],v[3],v[4],v[5]);
        /* ctrl_io_jvs roll calibration block (player+0xdf8..): cur@+2 max@+6
         * min@+8 off@+a, all signed words. + config @0x958bcec. */
        volatile int16_t *cb=(volatile int16_t*)(uintptr_t)0x958bdb8; /* player+0xdf8 = 0x958afc0+0xdf8 */
        if(!IsBadReadPtr((void*)cb,0x20))
          loader_log(LOG_WARN,"[RDBLK] +0:%d +2:%d +4:%d +6:%d +8:%d +a:%d +c:%d +e:%d +10:%d +12:%d +16:%d +18:%d +1a:%d",
            cb[0],cb[1],cb[2],cb[3],cb[4],cb[5],cb[6],cb[7],cb[8],cb[9],cb[0xb],cb[0xc],cb[0xd]);
        volatile uint32_t *cc=(volatile uint32_t*)(uintptr_t)0x09f3bb9c;
        if(!IsBadReadPtr((void*)cc,12))
          loader_log(LOG_WARN,"[RDCFG] active CalibrateConfig = %08x %08x %08x  liveCfg(roll)=%d",
            cc[0],cc[1],cc[2], *(volatile int16_t*)(uintptr_t)0x958bcec); } }

    /* DEBUG (gated): inject RAW analog (pre-pipeline) to verify the whole shape->
     * slew->game_cal->byte-swap chain end to end. Unlike ULTRA_FORCE_AXIS (which
     * overwrites the post-pipeline node word) this sets g_jvs.analog so the full
     * path runs. Pair with ULTRA_NO_AUTOCAL=1 so decal stays pass-through. Format
     * "c:v[,c:v]" v=0..65535. Inert without env. */
    { const char *fr = getenv("ULTRA_FORCE_RAW");
      if (fr && *fr) { char b[64]; strncpy(b,fr,sizeof b-1); b[sizeof b-1]=0;
          char *t=strtok(b,","); while(t){ int c=-1,v=-1;
              if(sscanf(t,"%d:%d",&c,&v)==2 && c>=0 && c<8 && v>=0 && v<=65535) g_jvs.analog[c]=(uint16_t)v;
              t=strtok(NULL,","); } } }

    /* DIAG (temp, gated): the FULL roll-axis (ch0) chain so a single line pins where
     * "right" dies - raw from the pad -> de-cal (drift removed) -> shaped -> game-cal
     * remap (the value the game reads), plus its high byte (the game samples that).
     * Push the stick fully LEFT then fully RIGHT and compare. ULTRA_DIAG_ANALOG=1. */
    if (getenv("ULTRA_DIAG_ANALOG")) { static unsigned df=0; if(((df++)%30)==0){
        uint16_t rx = g_jvs.analog[0];
        uint16_t dx = jvs_decal(0, rx);
        uint16_t shx = jvs_calibrated()? jvs_shape_axis(dx,0) : 32768;
        uint16_t gx = jvs_game_cal(0, shx);
        loader_log(LOG_WARN,"[DIAG-ANALOG] roll: raw=%u off=%d -> decal=%u -> shaped=%u -> game=%u (hi=%u)  cal_locked=%d",
            (unsigned)rx, s_cal_off[0], (unsigned)dx, (unsigned)shx, (unsigned)gx, (unsigned)(gx>>8), jvs_calibrated()); } }

    /* Universal lightgun engine: tick & overlay screen pos onto JVS analog 0/1
     * (player 0) and 2/3 (player 1) when the gun is enabled. Wins over the
     * keyboard/XInput/SDL merge for those channels. Trigger -> JVS_BTN1,
     * offscreen/reload -> JVS_BTN2. s_gun_active[] (file-static so the
     * READ_ANALOG reply in process_jvs_request can also see it) signals BOTH
     * the ABC node-buffer writer and the standard JVS analog reply to bypass
     * stick shaping/slew/decal/game_cal for gun axes. */
    s_gun_active[0] = 0; s_gun_active[1] = 0;
    int *gun_active = s_gun_active;
    lightgun_tick();
    if (g_ul_lightgun_enable) {
        for (int p = 0; p < 2; p++) {
            ul_event_t ev;
            if (lightgun_get_state((unsigned)p, &ev)) {
                gun_active[p] = 1;
                float gx = ev.offscreen ? 0.5f : ev.x;
                float gy = ev.offscreen ? 0.5f : ev.y;
                unsigned vx = (unsigned)(gx * 65535.0f); if (vx > 65535) vx = 65535;
                unsigned vy = (unsigned)(gy * 65535.0f); if (vy > 65535) vy = 65535;
                g_jvs.analog[p*2]   = (uint16_t)vx;
                g_jvs.analog[p*2+1] = (uint16_t)vy;
                if (ev.trigger)                                                       g_jvs.switches[p] |= JVS_BTN1;
                /* Reload semantic: explicit RELOAD button, OR the classic
                 * "trigger pulled while pointed off-screen" gesture. Just
                 * being off-screen (no trigger) is NOT a reload - that caused
                 * spurious continuous reloads in the v1 integration. */
                if ((ev.offscreen && ev.trigger) || (ev.buttons & UL_BTN_RELOAD)) g_jvs.switches[p] |= JVS_BTN2;
                if (ev.buttons & UL_BTN_START)                    g_jvs.switches[p] |= JVS_START;
                if (ev.buttons & UL_BTN_A)                        g_jvs.switches[p] |= JVS_BTN3;
                if (ev.buttons & UL_BTN_B)                        g_jvs.switches[p] |= JVS_BTN4;
            }
        }
    }

    /* Deliver switches + analog into the game's JVS node receive buffer - the
     * upstream source its own per-frame input merge consumes. Standard JVS byte
     * format: g_jvs.switches[p] high byte = JVS byte1 (bit7=Start,6=Service,5=Up,
     * 4=Down,3=Left,2=Right,1=Push1,0=Push2), low byte = byte2. analog = 8x u16
     * centered 0x8000 (game uses the high byte). */
    if (g_ul_jvs_node_addr) {
        if (g_ul_jvs_conn_addr) {
            *(volatile uint8_t *)(uintptr_t)(g_ul_jvs_conn_addr)     = 1;  /* node connected */
            *(volatile uint32_t*)(uintptr_t)(g_ul_jvs_conn_addr + 4) = 1;  /* node count */
        }
        volatile uint8_t *n = (volatile uint8_t*)(uintptr_t)g_ul_jvs_node_addr;
        if (!IsBadWritePtr((void*)n, 0x18)) {
            n[0] = g_jvs.system_switches;
            n[1] = (uint8_t)(g_jvs.switches[0] >> 8);
            n[2] = (uint8_t)(g_jvs.switches[0]);
            n[3] = (uint8_t)(g_jvs.switches[1] >> 8);
            n[4] = (uint8_t)(g_jvs.switches[1]);
            /* Shape every analog axis with the user's dead zone / sensitivity /
             * curve / invert just before the game consumes it - the single point
             * where all input sources (XInput, SDL pad, future flight stick) have
             * converged. (Light-gun titles will write screen X/Y into analog[0/1]
             * and apply the g_ul_lightgun_* tuning on their own gun-read path.) */
            /* Stick stays INERT (forced to center) until auto-cal locks the resting
             * center - see jvs_autocal_tick. Once live, remove drift then shape. */
            volatile uint16_t *an = (volatile uint16_t*)(n + 6);
            for (int c = 0; c < 8; c++) {
                uint16_t sv;
                int pl = c / 2;
                if (pl < 2 && gun_active[pl]) {
                    /* Gun pose: bypass stick shaping. The raw normalized analog
                     * value from the lightgun service IS the calibrated screen
                     * pos (ul_calib already applied invert/sens/offset). Decal/
                     * shape/slew/game_cal would distort that.
                     * BUT we still call jvs_slew with the center value to keep
                     * its per-channel prev[] up-to-date, so when the user
                     * switches mid-session from gun back to stick the slew
                     * starts ramping from center rather than from a stale
                     * gun-pose snapshot. (Otherwise: hard stick jumps on
                     * mode-switch.) */
                    (void)jvs_slew(c, 32768);
                    sv = g_jvs.analog[c];
                } else {
                    sv = jvs_calibrated()
                          ? jvs_game_cal(c, jvs_slew(c, jvs_shape_axis(jvs_decal(c, g_jvs.analog[c]), c & 1)))
                          : jvs_game_cal(c, (jvs_slew(c, 32768), 32768));  /* hold game-center + keep slew at rest */
                }
                /* The decoder make_switch_data (@0x8094f7a) reads the BYTE at the
                 * channel's BASE address: roll = (int8_t)(byte + 0x80)/127, clamp +-1.
                 * JVS analog is big-endian (MSB first) so the game wants our
                 * 0x8000-centered value's HIGH byte at the low address; we're little-
                 * endian, so byte-swap on the way out. Neutral 0x8000 -> read byte
                 * 0x80 (level), full left 0x0000 -> 0x00 (-1), full right 0xFFFF ->
                 * 0xFF (+1). Writing native LE was the bug: the game saw our LOW byte
                 * (~0x00 for any half-throw) so the whole negative half collapsed to
                 * full-left and only the +ve extreme ever registered -> "left rolls
                 * right, pitch-down reads as up". Verified exact: u16 0xEA60 -> read
                 * 0x60 -> -0.252; 0xF5C2 -> 0xC2 -> +0.520 (matches probed floats). */
                an[c] = (uint16_t)((sv >> 8) | (sv << 8));
            }
            /* DEBUG (gated): force raw channel values to probe which axis is roll and
             * whether injected input moves the plane. Format "c:val,c:val" (val 0..65535),
             * e.g. ULTRA_FORCE_AXIS=0:65535 = full deflection on ch0. Inert without env. */
            { const char *fa = getenv("ULTRA_FORCE_AXIS");
              if (fa && *fa) { char buf[64]; strncpy(buf,fa,sizeof buf-1); buf[sizeof buf-1]=0;
                  char *tok = strtok(buf, ",");
                  while (tok) { int c=-1, v=-1; if (sscanf(tok,"%d:%d",&c,&v)==2 && c>=0 && c<8 && v>=0 && v<=65535) an[c]=(uint16_t)v; tok=strtok(NULL,","); } } }
            /* DEBUG (gated): boot-sweep ch0/1/2 to both extremes so the game's
             * adaptive min/max calibration learns the full range, then neutral
             * 0x8000 should map to center. ULTRA_CAL_SWEEP=1 (window 700..900). */
            if (getenv("ULTRA_CAL_SWEEP")) { extern int g_ul_swap_frame; int f=g_ul_swap_frame;
                if (f>=700 && f<900) { uint16_t v = ((f/20)&1)?0xFFFF:0x0000; an[0]=an[1]=an[2]=v; } }
        }
    }

    /* ── GSEVO: credit + PC-input feed (sha-gated; inert for ABC/LGJ) ────────
     * Derive the gun pose + trigger + START from the universal lightgun service
     * when enabled, otherwise from the merged switch state (auto-play drives
     * JVS_START/JVS_BTN1; a passive player in a rail shooter still dies -> game
     * over -> attract, closing the credit loop). Aim defaults to screen center
     * so the rail stage always has a valid pointer. */
    if (g_ul_gsevo) {
        float aim_x = 0.5f, aim_y = 0.5f;
        int   reload_now = 0;   /* hoisted: the pad-aim block below must not clobber a
                                 * reload pose, and the pad's own reload sets it too. */
        int   trig  = 0, start = 0;
        if (g_ul_lightgun_enable) {
            ul_event_t ev;
            if (lightgun_get_state(0, &ev)) {
                if (!ev.offscreen) { aim_x = ev.x; aim_y = ev.y; }
                trig  = ev.trigger ? 1 : 0;
                if (ev.buttons & UL_BTN_START) start = 1;
            }
        } else {
            /* GSEVO is a lightgun shooter -- when the user hasn't explicitly
             * enabled the Ultra Omni-Track lightgun service, fall back to the
             * OS mouse cursor as the aim source + left mouse button as fire.
             *
             *   Mouse on window  + left-click       -> shoot
             *   Mouse off window + left-click       -> reload (off-screen shot)
             *   Right-click anywhere                -> reload
             *   R key                               -> reload
             *
             * gun_work coordinate model (RE'd at gun_work+0x14/+0x18):
             *   stored_x = (aim_x - 0.5) * 640      in [-320..+320]
             *   stored_y = (0.5 - aim_y) * 480      in [-240..+240]   (Y flipped)
             * Sending stored_x = -10000 (aim_x = -15) registers as "way off the
             * CRT" -> the game's gun-pose check classifies the shot as off-screen
             * -> reload, without us needing a separate reload bit. */
            extern HWND glut_get_hwnd(void);
            HWND hw = glut_get_hwnd();
            static float s_last_on_x = 0.5f, s_last_on_y = 0.5f;
            /* NO foreground-window check - mouse must reach the game even if
             * focus is briefly elsewhere (overlay, debugger, etc.). The
             * GetAsyncKeyState calls below are inherently global state too. */
            POINT pt; RECT rc;
            int offscreen = 1;
            float fx = 0.5f, fy = 0.5f;
            int have_rect = 0;
            if (hw && GetClientRect(hw, &rc)) {
                /* Window-relative path: cursor coords mapped into the GL window. */
                if (GetCursorPos(&pt) && ScreenToClient(hw, &pt)) {
                    /* AIM AGAINST THE PICTURE, NOT THE WINDOW.
                     *
                     * The game's aim space is its own rendered picture. That picture is NOT the
                     * client rect: in Fit mode (g_ul_disp_mode == 1 -- what ultra.ini selects)
                     * it sits inside letterbox/pillarbox bars. Mapping the cursor across the
                     * whole window therefore shifts the crosshair away from the pointer by the
                     * bar size, which is exactly the 'crosshair is so far from my mouse' report
                     * on Ghost Squad Evolution (2026-09-19).
                     *
                     * glx_emu publishes the rect it actually presents into. The present blits
                     * source->destination one-to-one, so a cursor at fraction f across that
                     * destination is at fraction f across the visible picture -- the game's own
                     * aim space. Fall back to the client rect only before the first frame, when
                     * nothing has been presented yet. */
                    extern int g_ul_present_dx0, g_ul_present_dy0;
                    extern int g_ul_present_dx1, g_ul_present_dy1;
                    extern int g_ul_present_valid;
                    int px0 = 0, py0 = 0, px1 = rc.right - rc.left, py1 = rc.bottom - rc.top;
                    if (g_ul_present_valid &&
                        g_ul_present_dx1 > g_ul_present_dx0 && g_ul_present_dy1 > g_ul_present_dy0) {
                        px0 = g_ul_present_dx0; py0 = g_ul_present_dy0;
                        px1 = g_ul_present_dx1; py1 = g_ul_present_dy1;
                    }
                    pt.x -= px0; pt.y -= py0;
                    int w = px1 - px0, h = py1 - py0;
                    if (w > 0 && h > 0) {
                        fx = (float)pt.x / (float)w;
                        fy = (float)pt.y / (float)h;
                        offscreen = (fx < 0.0f || fx > 1.0f || fy < 0.0f || fy > 1.0f);
                        have_rect = 1;
                    }
                }
            }
            if (!have_rect) {
                /* SCREEN-RELATIVE FALLBACK: glut_get_hwnd returned NULL (SDL3 +
                 * Mesa-llvmpipe doesn't route through wglGetCurrentDC), or the
                 * client rect couldn't be queried. Map the cursor against the
                 * primary monitor so the gun is still drivable. Loses the
                 * per-window mapping (off-screen reload only triggers when the
                 * cursor leaves the desktop), but it's strictly better than the
                 * old NULL-HWND silent failure that made the gun aim freeze at
                 * (0.5, 0.5) and the player passively die -> "YOU GAVE UP". */
                if (GetCursorPos(&pt)) {
                    int sw = GetSystemMetrics(SM_CXSCREEN);
                    int sh = GetSystemMetrics(SM_CYSCREEN);
                    if (sw > 0 && sh > 0) {
                        fx = (float)pt.x / (float)sw;
                        fy = (float)pt.y / (float)sh;
                        offscreen = (fx < 0.0f || fx > 1.0f || fy < 0.0f || fy > 1.0f);
                    }
                }
            }
            int lmb   = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            int rmb   = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            int r_key = (GetAsyncKeyState('R')        & 0x8000) != 0;
            reload_now = (lmb && offscreen) || rmb || r_key;

            if (reload_now) {
                /* Force gun pose far off the CRT + pull the trigger so the
                 * game's own off-screen-shot reload path fires naturally. */
                aim_x = -15.0f;
                aim_y = -15.0f;
                trig  = 1;
            } else if (!offscreen) {
                aim_x = fx;
                aim_y = fy;
                s_last_on_x = fx;
                s_last_on_y = fy;
                if (lmb) trig = 1;
            } else {
                /* Mouse drifted off the window but no reload -> hold last
                 * on-screen aim so the crosshair doesn't snap to center. */
                aim_x = s_last_on_x;
                aim_y = s_last_on_y;
            }
        }
        /* ── PAD AIM: a stick-driven crosshair for a lightgun game ───────────
         *
         * GSEVO had exactly two aim sources: the Ultra Omni-Track lightgun service,
         * or the OS mouse. On a GAMEPAD there was no aim at all -- the player got
         * START and a trigger (RT already sets JVS_BTN1 above) and nothing to point
         * with. Owner-reported 2026-09-19: "it doesn't have controls".
         *
         * A lightgun game wants an ABSOLUTE pointer, but a stick is a RELATIVE
         * device, so the stick drives the crosshair like a trackball: deflection
         * sets velocity, the position integrates and clamps to the screen. That is
         * how pad-on-lightgun is normally done and it stays usable at any speed.
         *
         * The mouse is NOT disabled by this. The stick only takes over on the frames
         * it is actually deflected past its deadzone; the moment it is released the
         * mouse is authoritative again, and the pad position re-seeds from wherever
         * the mouse left the crosshair so there is no jump either way.
         *
         *   ULTRA_GS_PAD_AIM=0     turn the stick off entirely (mouse only)
         *   ULTRA_GS_PAD_SPEED=N   crosshair travel, screens/second (default 1.6)
         *   ULTRA_GS_PAD_STICK=l   aim with the LEFT stick instead of the right
         *   ULTRA_GS_PAD_DEAD=N    stick deadzone 0..32767 (default 7000) */
        if (!g_ul_lightgun_enable && _XInputGetState) {
            static int   s_pa_on = -1, s_pa_dead = 7000, s_pa_left = 0;
            static float s_pa_speed = 1.6f;
            static float s_pa_x = 0.5f, s_pa_y = 0.5f;
            static DWORD s_pa_last = 0;
            if (s_pa_on < 0) {
                const char *e;
                s_pa_on = ((e = getenv("ULTRA_GS_PAD_AIM")) && *e) ? atoi(e) : 1;
                if ((e = getenv("ULTRA_GS_PAD_SPEED")) && *e) s_pa_speed = (float)atof(e);
                if ((e = getenv("ULTRA_GS_PAD_DEAD"))  && *e) s_pa_dead  = atoi(e);
                if ((e = getenv("ULTRA_GS_PAD_STICK")) && (*e=='l'||*e=='L')) s_pa_left = 1;
                if (s_pa_speed <= 0.0f) s_pa_speed = 1.6f;
                if (s_pa_dead  <  0)    s_pa_dead  = 0;
                if (s_pa_dead  > 30000) s_pa_dead  = 30000;
            }
            if (s_pa_on) {
                XINPUT_STATE xs;
                if (_XInputGetState(0, &xs) == ERROR_SUCCESS) {
                    int rx = s_pa_left ? xs.Gamepad.sThumbLX : xs.Gamepad.sThumbRX;
                    int ry = s_pa_left ? xs.Gamepad.sThumbLY : xs.Gamepad.sThumbRY;
                    int mag = (rx < 0 ? -rx : rx) > (ry < 0 ? -ry : ry)
                            ? (rx < 0 ? -rx : rx) : (ry < 0 ? -ry : ry);
                    DWORD now = GetTickCount();
                    float dt = s_pa_last ? (float)(now - s_pa_last) / 1000.0f : 0.0f;
                    s_pa_last = now;
                    if (dt > 0.10f) dt = 0.10f;          /* a stall must not teleport it */
                    if (mag > s_pa_dead) {
                        /* Rescale past the deadzone so the first movement is gentle
                         * rather than a jump, then square it for fine aim near center
                         * and quick travel at full deflection. */
                        float sx = (float)rx / 32767.0f, sy = (float)ry / 32767.0f;
                        float dz = (float)s_pa_dead / 32767.0f;
                        float ax = sx < 0 ? -sx : sx, ay = sy < 0 ? -sy : sy;
                        if (ax > dz) ax = (ax - dz) / (1.0f - dz); else ax = 0.0f;
                        if (ay > dz) ay = (ay - dz) / (1.0f - dz); else ay = 0.0f;
                        s_pa_x += (sx < 0 ? -1.0f : 1.0f) * ax * ax * s_pa_speed * dt;
                        s_pa_y -= (sy < 0 ? -1.0f : 1.0f) * ay * ay * s_pa_speed * dt;  /* stick up = screen up */
                        if (s_pa_x < 0.0f) s_pa_x = 0.0f; if (s_pa_x > 1.0f) s_pa_x = 1.0f;
                        if (s_pa_y < 0.0f) s_pa_y = 0.0f; if (s_pa_y > 1.0f) s_pa_y = 1.0f;
                        if (!reload_now) { aim_x = s_pa_x; aim_y = s_pa_y; }   /* the stick is driving */
                    } else {
                        /* Stick idle. Re-seed from the mouse ONLY when the mouse is actually
                         * being moved. Following it unconditionally would snap the crosshair
                         * back to wherever the cursor is parked the instant the stick is
                         * released -- on a pad-only cabinet that is a corner of the screen. */
                        POINT cp;
                        static POINT s_pa_prev; static int s_pa_seen = 0;
                        int moved = 0;
                        if (GetCursorPos(&cp)) {
                            if (s_pa_seen && (cp.x != s_pa_prev.x || cp.y != s_pa_prev.y)) moved = 1;
                            s_pa_prev = cp; s_pa_seen = 1;
                        }
                        if (moved) { s_pa_x = aim_x; s_pa_y = aim_y; }  /* the mouse takes over */
                        else if (!reload_now) { aim_x = s_pa_x; aim_y = s_pa_y; }  /* the pad holds its aim */
                    }
                    /* CORRECTION 2026-09-20. An earlier note here claimed fire was not bound to
                     * RB and that binding it would shoot whenever the exit overlay opened. That
                     * was WRONG on this board: sdlpad_poll_player already bound the right
                     * shoulder to JVS_BTN1 (jvs_input.c:1135), so RB HAS been pulling the
                     * trigger the whole time -- and the XInput GLUT bridge additionally sent 'e',
                     * which the game decodes as PUSH8 and feeds to its camera/demo selector. That
                     * double action is what the owner saw as "I hit R1 and a green crosshair
                     * shows up". Both are removed for this title; fire stays on RT and A. */
                }
            }
        }
        /* PAD RELOAD. GSEVO reloads by shooting OFF-SCREEN -- there is no reload
         * button, on the cabinet or in the code. The mouse gets there three ways
         * (off-window left click, right click, R key) and a pad had NONE, which made
         * the game unplayable past the first clip on a controller. LB alone does it;
         * the !RB term keeps the LB+RB exit-overlay combo from also firing a shot. */
        if (!g_ul_lightgun_enable && _XInputGetState) {
            XINPUT_STATE xr;
            if (_XInputGetState(0, &xr) == ERROR_SUCCESS &&
                (xr.Gamepad.wButtons & 0x0100) && !(xr.Gamepad.wButtons & 0x0200)) {
                aim_x = -15.0f; aim_y = -15.0f; trig = 1;   /* off-screen shot = reload */
            }
        }
        /* Fold in the merged switch state (keyboard/pad/auto-play): START bit and
         * the fire button both feed the rail-shooter. */
        if (g_jvs.switches[0] & JVS_START) start = 1;
        if (g_jvs.switches[0] & JVS_BTN1)  trig  = 1;
        /* Auto-play rail-shooter autopilot pose/fire (only when the real lightgun
         * isn't driving): sweep the aim and hold fire so the credited stage is
         * actively played and the loop advances to game-over -> attract. */
        if (s_gs_auto_active && !g_ul_lightgun_enable) {
            aim_x = s_gs_auto_aim_x;
            aim_y = s_gs_auto_aim_y;
            if (s_gs_auto_fire) trig = 1;
        }
        gsevo_inject(aim_x, aim_y, trig, start, gs_coin_pulse);
    }

    #undef KEY
}

/* ------------------------------------------------------------------ */
/* JVS packet builder                                                  */
/* ------------------------------------------------------------------ */
static uint8_t jvs_checksum(const uint8_t *data, int len)
{
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) sum += data[i];
    return sum;
}

static void build_response(const uint8_t *payload, int payload_len)
{
    /*
     * Response frame:
     *   SYNC   (0xE0)
     *   DEST   (0x00 = master)
     *   LENGTH (payload_len + 1 for checksum)
     *   payload[0..n]
     *   CHECKSUM
     */
    g_resp_buf[0] = JVS_SYNC;
    g_resp_buf[1] = JVS_MASTER_ADDR;
    g_resp_buf[2] = (uint8_t)(payload_len + 1);
    memcpy(&g_resp_buf[3], payload, payload_len);

    /* Checksum covers DEST + LENGTH + payload */
    uint8_t cs = g_resp_buf[1] + g_resp_buf[2];
    for (int i = 0; i < payload_len; i++) cs += payload[i];
    g_resp_buf[3 + payload_len] = cs;

    g_resp_len = 4 + payload_len;
    g_resp_pos = 0;
}

/* ------------------------------------------------------------------ */
/* Process a JVS request and generate a response                      */
/* ------------------------------------------------------------------ */
static void process_jvs_request(const uint8_t *req, int req_len)
{
    uint8_t resp[128];
    int     rpos = 0;

    /* Input is polled on the render thread (jvs_poll) where SDL gamepad calls are
     * safe; here we just read the latest g_jvs snapshot. (Calling poll_inputs from
     * this guest ioctl/serial thread raced SDL and crashed non-deterministically.) */

    /* Status byte always first */
    resp[rpos++] = JVS_STATUS_NORMAL;

    int i = 0; /* index into request DATA field (after DEST/LEN) */
    while (i < req_len) {
        uint8_t cmd = req[i++];

        switch (cmd) {

        case 0xF0: /* RESET (broadcast): consumes arg 0xD9, no report */
            if (i < req_len) i++;            /* swallow 0xD9 */
            g_jvs_sense = 3;                 /* node present, awaiting address */
            break;

        case 0xF1: /* SET ADDRESS: arg = node address */
            if (i < req_len) i++;            /* swallow the address */
            resp[rpos++] = JVS_REPORT_NORMAL;
            g_jvs_sense = 1;                 /* addressed -> enumeration complete */
            break;

        case 0x10: /* Get board ID */
            resp[rpos++] = JVS_REPORT_NORMAL;
            {
                const char *id = "SEGA CORPORATION;I/O BD JVS;837-14572;Ver1.00;2005/10";
                int len = strlen(id) + 1;
                memcpy(&resp[rpos], id, len);
                rpos += len;
            }
            break;

        case 0x11: /* Command format version */
            resp[rpos++] = JVS_REPORT_NORMAL;
            resp[rpos++] = 0x13;
            break;

        case 0x12: /* JVS version */
            resp[rpos++] = JVS_REPORT_NORMAL;
            resp[rpos++] = 0x30;
            break;

        case 0x13: /* Communication version */
            resp[rpos++] = JVS_REPORT_NORMAL;
            resp[rpos++] = 0x10;
            break;

        case 0x14: /* Get features (CAP code + 3 param bytes each, CAP_END terminates) */
            resp[rpos++] = JVS_REPORT_NORMAL;
            resp[rpos++] = JVS_FEATURE_SWITCH; resp[rpos++] = 2;  resp[rpos++] = 13; resp[rpos++] = 0;
            resp[rpos++] = JVS_FEATURE_COIN;   resp[rpos++] = 2;  resp[rpos++] = 0;  resp[rpos++] = 0;
            resp[rpos++] = JVS_FEATURE_ANALOG; resp[rpos++] = 8;  resp[rpos++] = 16; resp[rpos++] = 0;
            resp[rpos++] = JVS_FEATURE_END;
            break;

        case 0x20: { /* Read SWITCHES: #players, #bytes/player */
            uint8_t players = req[i++];
            uint8_t bytes   = req[i++];
            resp[rpos++] = JVS_REPORT_NORMAL;
            resp[rpos++] = g_jvs.system_switches;      /* system switch byte (D7=Test) */
            for (int p = 0; p < players; p++) {
                uint16_t sw = (p < 2) ? g_jvs.switches[p] : 0;
                for (int b = 0; b < bytes; b++)
                    resp[rpos++] = (b == 0) ? (uint8_t)(sw >> 8)
                                 : (b == 1) ? (uint8_t)(sw & 0xFF) : 0;
            }
            break;
        }

        case 0x21: { /* Read COINS: #slots */
            uint8_t slots = req[i++];
            resp[rpos++] = JVS_REPORT_NORMAL;
            for (int s = 0; s < slots; s++) {
                uint16_t val = (s < 2) ? (g_jvs.coins[s] & 0x3FFF) : 0; /* cond=00(normal)+count */
                resp[rpos++] = (uint8_t)(val >> 8);
                resp[rpos++] = (uint8_t)(val & 0xFF);
            }
            break;
        }

        case 0x22: { /* Read ANALOG: #channels (16-bit, hi byte first) */
            uint8_t channels = req[i++];
            resp[rpos++] = JVS_REPORT_NORMAL;
            for (int c = 0; c < channels; c++) {
                uint16_t v;
                if (c >= 8) v = 0x8000;
                else {
                    int pl = c / 2;
                    /* Gun channels: write the raw (already-calibrated by ul)
                     * gun pose; shaping/slew/decal/game_cal would warp it.
                     * Stick channels: apply the same pipeline the ABC writer
                     * uses, so the standard JVS REPLY and the ABC node-buffer
                     * path agree byte-for-byte. */
                    if (pl < 2 && s_gun_active[pl]) {
                        v = g_jvs.analog[c];
                    } else {
                        v = jvs_calibrated()
                              ? jvs_game_cal(c, jvs_slew(c, jvs_shape_axis(jvs_decal(c, g_jvs.analog[c]), c & 1)))
                              : jvs_game_cal(c, (jvs_slew(c, 32768), 32768));
                    }
                }
                resp[rpos++] = (uint8_t)(v >> 8);
                resp[rpos++] = (uint8_t)(v & 0xFF);
            }
            break;
        }

        case 0x30: { /* Decrease coins: slot, hi, lo */
            uint8_t slot  = req[i++];
            uint8_t hi    = req[i++];
            uint8_t lo    = req[i++];
            uint16_t n    = (hi << 8) | lo;
            if (slot >= 1 && slot <= 2 && g_jvs.coins[slot-1] >= n)
                g_jvs.coins[slot-1] -= n;
            resp[rpos++] = JVS_REPORT_NORMAL;
            break;
        }

        case 0x32: { /* Write GPO (lamps): #bytes, then bytes - acknowledge */
            uint8_t outputs = req[i++];
            i += outputs;
            resp[rpos++] = JVS_REPORT_NORMAL;
            break;
        }

        default:
            loader_log(LOG_DEBUG, "JVS: unknown cmd 0x%02x", cmd);
            goto done;
        }
    }
done:
    build_response(resp, rpos);
}

/* ------------------------------------------------------------------ */
/* Baseboard-command (bc) entry point: After Burner Climax and other   */
/* Lindbergh games talk JVS over /dev/lbb's PROCESS_JVS command rather  */
/* than the /dev/ttyS0 serial path. The full JVS frame is handed to us  */
/* in one shot (from baseboard shared memory). Parse it, run it through */
/* the same emulator, and return the response frame. Mirrors whether    */
/* the request carried the 0xE0 SYNC byte.                              */
/* ------------------------------------------------------------------ */
int jvs_process_frame(const uint8_t *frame, int frame_len, uint8_t *out, int out_max)
{
    if (!frame || frame_len < 3 || !out) return 0;

    int has_sync = (frame[0] == JVS_SYNC);
    const uint8_t *p = has_sync ? frame + 1 : frame;   /* p[0]=DEST p[1]=LEN p[2..]=data+checksum */
    int avail = has_sync ? frame_len - 1 : frame_len;
    if (avail < 2) return 0;

    int len     = p[1];                 /* bytes after LEN, incl. checksum */
    int dataLen = len - 1;              /* data bytes (exclude checksum) */
    if (dataLen < 0) dataLen = 0;
    if (dataLen > avail - 2) dataLen = avail - 2;

    process_jvs_request(&p[2], dataLen);   /* fills g_resp_buf (full frame w/ SYNC) */

    const uint8_t *rsrc = g_resp_buf;
    int rlen = g_resp_len;
    if (!has_sync) { rsrc++; rlen--; }     /* strip SYNC to match request framing */
    if (rlen < 0) rlen = 0;
    if (rlen > out_max) rlen = out_max;
    memcpy(out, rsrc, rlen);
    return rlen;
}

/* ------------------------------------------------------------------ */
/* Request accumulation buffer                                         */
/* ------------------------------------------------------------------ */
static uint8_t g_req_buf[256];
static int     g_req_pos = 0;

/* ------------------------------------------------------------------ */
/* VFS read hook - called when game reads from /dev/ttyS0             */
/* ------------------------------------------------------------------ */
static int g_jvs_rd_log = 12;
int64_t jvs_read(void *buf, size_t len)
{
    if (g_jvs_rd_log > 0) {
        loader_log(LOG_INFO, "[JVS/ttyS] read req len=%u (pending=%d)", (unsigned)len, g_resp_len - g_resp_pos);
        g_jvs_rd_log--;
    }
    if (g_resp_pos >= g_resp_len) {
        /* No pending response - return 0 bytes (would block) */
        return 0;
    }

    size_t avail = (size_t)(g_resp_len - g_resp_pos);
    size_t n     = (len < avail) ? len : avail;
    memcpy(buf, &g_resp_buf[g_resp_pos], n);
    g_resp_pos += (int)n;
    return (int64_t)n;
}

/* ------------------------------------------------------------------ */
/* VFS write hook - called when game writes to /dev/ttyS0             */
/* ------------------------------------------------------------------ */
static int g_jvs_serial_log = 16;
int64_t jvs_write(const void *buf, size_t len)
{
    const uint8_t *data = (const uint8_t*)buf;
    if (g_jvs_serial_log > 0) {
        char hx[3*24+1]; int n = len < 24 ? (int)len : 24, o = 0;
        for (int k = 0; k < n; k++) o += snprintf(hx+o, sizeof(hx)-o, "%02x ", data[k]);
        loader_log(LOG_INFO, "[JVS/ttyS] write %u: %s", (unsigned)len, hx);
        g_jvs_serial_log--;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        if (b == JVS_SYNC) {
            g_req_pos = 0; /* Start of new frame */
            continue;
        }

        if (g_req_pos < (int)sizeof(g_req_buf))
            g_req_buf[g_req_pos++] = b;

        /*
         * Frame complete when we have enough bytes:
         * g_req_buf[0] = DEST
         * g_req_buf[1] = LENGTH
         * g_req_buf[2..LENGTH] = DATA + CHECKSUM
         */
        if (g_req_pos >= 2) {
            int expected = g_req_buf[1]; /* LENGTH = data bytes + checksum */
            /* Full frame after SYNC = DEST(1) + LEN(1) + LEN bytes = LEN + 2. */
            if (g_req_pos >= expected + 2) {
                /* Validate checksum */
                uint8_t cs = 0;
                for (int j = 0; j < g_req_pos - 1; j++) cs += g_req_buf[j];
                uint8_t recv_cs = g_req_buf[g_req_pos - 1];

                if (cs == recv_cs) {
                    /* Process: data starts at byte 2, length-1 bytes */
                    process_jvs_request(&g_req_buf[2], expected - 1);
                } else {
                    loader_log(LOG_WARN, "JVS checksum error: calc=0x%02x recv=0x%02x",
                               cs, recv_cs);
                    uint8_t err[] = { JVS_STATUS_CHECKSUM_ERR };
                    build_response(err, 1);
                }
                g_req_pos = 0;
            }
        }
    }

    return (int64_t)len;
}

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */
/* Poll all input sources into g_jvs. MUST be called from the render/window
 * thread (sdl_window_pump) - SDL gamepad enumeration/open is only safe there. */
static void input_diag_tick(void);
void jvs_poll(void) { poll_inputs(); input_diag_tick(); }

/* INPUT DIAGNOSTIC - env-gated, ALWAYS-on for GSEVO so user can verify what
 * we register. Edge-triggered: logs only when state changes, so the log is
 * readable. */
static void input_diag_tick(void)
{
    static int   s_armed = -1;
    if (s_armed < 0) {
        const char *e = getenv("ULTRA_INPUT_DIAG");
        s_armed = (e && atoi(e)) ? 1 : (g_ul_gsevo ? 1 : 0);
    }
    if (!s_armed) return;

    static uint16_t s_prev_sw0   = 0;
    static int      s_prev_xb_ok = -1;
    static int      s_prev_lmb   = -1, s_prev_rmb = -1;
    static int      s_prev_kb1   = -1, s_prev_kb5 = -1, s_prev_kbW = -1,
                     s_prev_kbA = -1, s_prev_kbS = -1, s_prev_kbD = -1,
                     s_prev_kbZ = -1;
    static int      s_prev_fg    = -1;
    extern HWND glut_get_hwnd(void);

    HWND hw = glut_get_hwnd();
    int  fg = (hw && hw == GetForegroundWindow()) ? 1 : 0;
    int  xb_ok = 0;
    if (_XInputGetState) {
        XINPUT_STATE xs;
        xb_ok = (_XInputGetState(0, &xs) == ERROR_SUCCESS) ? 1 : 0;
    }
    int kb1 = (GetAsyncKeyState('1') & 0x8000) ? 1 : 0;
    int kb5 = (GetAsyncKeyState('5') & 0x8000) ? 1 : 0;
    int kbW = (GetAsyncKeyState('W') & 0x8000) ? 1 : 0;
    int kbA = (GetAsyncKeyState('A') & 0x8000) ? 1 : 0;
    int kbS = (GetAsyncKeyState('S') & 0x8000) ? 1 : 0;
    int kbD = (GetAsyncKeyState('D') & 0x8000) ? 1 : 0;
    int kbZ = (GetAsyncKeyState('Z') & 0x8000) ? 1 : 0;
    int lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
    int rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 1 : 0;
    uint16_t sw0 = g_jvs.switches[0];

    int changed = (s_prev_sw0 != sw0) || (s_prev_xb_ok != xb_ok) ||
                  (s_prev_lmb != lmb) || (s_prev_rmb != rmb) ||
                  (s_prev_kb1 != kb1) || (s_prev_kb5 != kb5) ||
                  (s_prev_kbW != kbW) || (s_prev_kbA != kbA) ||
                  (s_prev_kbS != kbS) || (s_prev_kbD != kbD) ||
                  (s_prev_kbZ != kbZ) || (s_prev_fg  != fg);
    if (changed) {
        loader_log(LOG_INFO,
            "[INPUT] fg=%d hwnd=%p xinput=%s kb=%s%s%s%s%s%s%s mouse=%s%s jvs_sw0=%04x kbcb_set=%d",
            fg, (void*)hw, xb_ok ? "ON" : "off",
            kb1 ? "1 " : "", kb5 ? "5 " : "",
            kbW ? "W " : "", kbA ? "A " : "", kbS ? "S " : "", kbD ? "D " : "",
            kbZ ? "Z " : "",
            lmb ? "L" : "-", rmb ? "R" : "-",
            sw0,
            (g_ul_gs_kbcb_down && g_ul_gs_kbcb_up) ? 1 : 0);
        s_prev_sw0 = sw0; s_prev_xb_ok = xb_ok;
        s_prev_lmb = lmb; s_prev_rmb = rmb;
        s_prev_kb1 = kb1; s_prev_kb5 = kb5;
        s_prev_kbW = kbW; s_prev_kbA = kbA; s_prev_kbS = kbS; s_prev_kbD = kbD;
        s_prev_kbZ = kbZ; s_prev_fg = fg;
    }
}

void jvs_init(void)
{
    xinput_init();
    sdlpad_init();   /* SDL3 gamepads: Xbox/PlayStation/Nintendo, zero config */
    memset(&g_jvs, 0, sizeof(g_jvs));
    loader_log(LOG_INFO, "JVS input faker initialized (keyboard + XInput + SDL3 gamepad)");
    loader_log(LOG_INFO, "  P1: WASD=dirs Z/X/C/V=btns 1=start 5=coin");
    loader_log(LOG_INFO, "  P2: Arrows Num1-4=btns 2=start 6=coin");
    loader_log(LOG_INFO, "  F1=service F2=test");
}
