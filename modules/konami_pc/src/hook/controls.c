/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  controls.c -- the native_x86 player input-mapping ORCHESTRATOR + SINK ADAPTER.
 * =============================================================================
 *
 *  Owns the per-frame ultra_intent. Each frame:
 *    1. clears the intent (carrying coin counters forward),
 *    2. runs every AVAILABLE source, lowest priority first, so a higher-priority
 *       source's active analog overwrites a lower one's (last-active-writer-wins),
 *    3. renders the intent into ultra_jvs_input via intent_to_jvs_input().
 *
 *  Pure logic + the source list; no board sink is touched. The sink adapter is a
 *  pure function (no OS deps) so the unit test can round-trip intent->jvs without
 *  a game.
 *
 *  DEFAULT-OFF: ultra_input_map_active() is true only when a board profile/config
 *  resolved or ULTRA_CONTROLS=1; otherwise hook_dllmain stays on its legacy
 *  producer and the playable games are byte-identical.
 * =============================================================================
 */
#include "controls.h"
#include "input_sources.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#if defined(_WIN32)
#  include <windows.h>
extern void ultra_d3d8_log(const char* fmt, ...);
#  define cl_log ultra_d3d8_log
#else
#  define cl_log(...) do {} while (0)
#endif

/* the per-board default profile table + the config-file overlay live in
 * input_profiles.c; controls.c only consumes them. */
const board_binding* board_profiles_resolve(const char* system_id);
/* apply a config file (ULTRA_CONTROLS_CONFIG / <game>/ultra_controls.ini /
 * bundled default) ON TOP of the compiled default, returning the merged binding
 * (or the unchanged default if no file). Lives in input_profiles.c. */
const board_binding* board_profiles_load_config(const board_binding* base,
                                                const char* system_id);

/* ===========================================================================
 *  Intent helpers
 * =========================================================================== */
void ui_intent_clear(ultra_intent* it) {
    memset(it->btn, 0, sizeof it->btn);
    it->sys = 0;
    memset(it->ax_active, 0, sizeof it->ax_active);
    for (int p = 0; p < UI_MAX_PLAYERS; ++p)
        for (int a = 0; a < UI_AX__COUNT; ++a) it->ax[p][a] = 0.0f;
    /* coin[] is intentionally NOT cleared here -- it is a monotonic counter that
     * the orchestrator owns across frames; sources only ADD to it. */
}

/* ===========================================================================
 *  SINK ADAPTER -- intent -> ultra_jvs_input.  Pure; unit-tested.
 *
 *  Digital: the JVS switch wire layout from jvs_core.h:
 *    player[]: bit15 START bit14 SERVICE bit13 UP bit12 DOWN bit11 LEFT
 *              bit10 RIGHT bit9 PUSH1 bit8 PUSH2 bit7 PUSH3 bit6 PUSH4
 *              bit5 PUSH5 bit4 PUSH6 bit3 PUSH7 bit2 PUSH8
 *    system:   bit7 TEST bit6 TILT
 *  Analog: float -> uint16 into the profile's [out_min,out_max] window. Bipolar
 *  (-1..+1) centers at the window midpoint; unipolar (0..1) spans floor..ceil.
 *  GUN_X/Y with jvs_channel==-1 render to screen_x/screen_y (0..65535).
 * =========================================================================== */

/* JVS wire bit for each ui_button (in the 16-bit player word). */
static uint16_t jvs_btn_bit(ui_button b) {
    switch (b) {
        case UI_BTN_START:       return 0x8000;
        case UI_BTN_SERVICE:     return 0x4000;
        case UI_BTN_UP:          return 0x2000;
        case UI_BTN_DOWN:        return 0x1000;
        case UI_BTN_LEFT:        return 0x0800;
        case UI_BTN_RIGHT:       return 0x0400;
        case UI_BTN_PUSH1:       return 0x0200;
        case UI_BTN_PUSH2:       return 0x0100;
        case UI_BTN_PUSH3:       return 0x0080;
        case UI_BTN_PUSH4:       return 0x0040;
        case UI_BTN_PUSH5:       return 0x0020;
        case UI_BTN_PUSH6:       return 0x0010;
        case UI_BTN_PUSH7:       return 0x0008;
        case UI_BTN_PUSH8:       return 0x0004;
        case UI_BTN_GUN_TRIGGER: return 0x0200;  /* same as PUSH1 (gun convention) */
        case UI_BTN_GUN_RELOAD:  return 0x0100;  /* same as PUSH2                  */
        default:                 return 0;
    }
}

/* clamp helper */
static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Is a ui_axis bipolar (center at 0) or unipolar (floor at 0)? */
static int axis_is_bipolar(ui_axis a) {
    return a == UI_AX_STEER || a == UI_AX_STICK_X || a == UI_AX_STICK_Y;
}

/* Apply a per-axis binding's shaping (deadzone, curve, invert) to a normalized
 * value, then scale into the [out_min,out_max] uint16 window. */
static uint16_t shape_and_scale(const ui_axis_bind* ab, float v) {
    int bipolar = axis_is_bipolar((ui_axis)ab->intent);
    if (ab->invert) v = bipolar ? -v : (1.0f - v);

    /* deadzone */
    float dz = (float)ab->deadzone_pct / 100.0f;
    if (dz > 0.0f) {
        if (bipolar) {
            if (v > -dz && v < dz) v = 0.0f;
            else {
                float s = (v > 0 ? 1.0f : -1.0f);
                v = s * (fabsf(v) - dz) / (1.0f - dz);
            }
        } else {
            if (v < dz) v = 0.0f;
            else        v = (v - dz) / (1.0f - dz);
        }
    }

    /* curve */
    if (ab->curve == UI_CURVE_EXPO) {
        if (bipolar) { float s = (v >= 0 ? 1.0f : -1.0f); v = s * v * v; }
        else         v = v * v;
    }

    /* normalize into 0..1 over the window, then scale into [out_min,out_max]. */
    float u;   /* 0..1 position in the window */
    if (bipolar) {
        v = clampf(v, -1.0f, 1.0f);
        u = (v + 1.0f) * 0.5f;     /* -1 -> 0, 0 -> 0.5, +1 -> 1 */
    } else {
        u = clampf(v, 0.0f, 1.0f); /* 0 -> floor, 1 -> ceil */
    }
    long span = (long)ab->out_max - (long)ab->out_min;
    long out  = (long)ab->out_min + (long)(u * (float)span + 0.5f);
    if (out < 0)      out = 0;
    if (out > 0xFFFF) out = 0xFFFF;
    return (uint16_t)out;
}

void intent_to_jvs_input(const ultra_intent* it, const board_binding* bind,
                         ultra_jvs_input* out) {
    memset(out, 0, sizeof *out);
    for (int a = 0; a < JVS_MAX_ANALOG; ++a) out->analog[a] = 0x8000;
    out->screen_x = out->screen_y = 0x8000;

    /* digital switches per player */
    for (int p = 0; p < UI_MAX_PLAYERS && p < JVS_MAX_PLAYERS; ++p) {
        uint16_t w = 0;
        for (int b = 0; b < UI_BTN__COUNT; ++b)
            if (it->btn[p] & (1u << b)) w |= jvs_btn_bit((ui_button)b);
        out->player[p] = w;
    }
    /* system bits */
    if (it->sys & (1u << UI_SYS_TEST)) out->system |= 0x80;
    if (it->sys & (1u << UI_SYS_TILT)) out->system |= 0x40;

    /* analog: only render the axes the profile actually binds, so unbound JVS
     * channels stay centered (0x8000) -- a 0 there reads as full deflection. */
    if (bind && bind->axes) {
        for (int i = 0; i < bind->naxes; ++i) {
            const ui_axis_bind* ab = &bind->axes[i];
            int p = ab->player;
            if (p < 0 || p >= UI_MAX_PLAYERS) continue;
            float v = it->ax[p][ab->intent];
            uint16_t raw = shape_and_scale(ab, v);
            if (ab->jvs_channel < 0) {
                /* gun/screen position */
                if (ab->intent == UI_AX_GUN_X) out->screen_x = raw;
                else if (ab->intent == UI_AX_GUN_Y) out->screen_y = raw;
            } else if (ab->jvs_channel < JVS_MAX_ANALOG) {
                out->analog[ab->jvs_channel] = raw;
            }
        }
    }
}

/* ===========================================================================
 *  ORCHESTRATOR
 * =========================================================================== */
static int                  g_active   = 0;
static const board_binding* g_bind     = NULL;
static ultra_intent         g_intent;          /* persistent (coin counters live here) */
static const ultra_input_source* g_sources[8];
static int                  g_nsources = 0;
#if defined(_WIN32)
static CRITICAL_SECTION     g_lock;
static int                  g_lock_init = 0;
#endif

/* insertion sort the source list ascending by priority so we poll lowest first
 * (highest priority writes its analog last = wins). */
static void add_source(const ultra_input_source* s) {
    if (!s || g_nsources >= (int)(sizeof g_sources / sizeof g_sources[0])) return;
    int i = g_nsources++;
    while (i > 0 && g_sources[i-1]->priority > s->priority) {
        g_sources[i] = g_sources[i-1]; --i;
    }
    g_sources[i] = s;
}

void ultra_input_map_install(void) {
#if defined(_WIN32)
    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = 1; }
#endif
    memset(&g_intent, 0, sizeof g_intent);

    /* resolve the board id the board profile recorded (ULTRA_BOARD), else fall
     * back to the explicit controls system id, else "generic_pc". */
    const char* board = NULL;
#if defined(_WIN32)
    char buf[64];
    DWORD n = GetEnvironmentVariableA("ULTRA_BOARD", buf, sizeof buf);
    if (n > 0 && n < sizeof buf) board = buf;
#endif
    if (!board || !*board) board = getenv("ULTRA_CONTROLS_BOARD");
    if (!board || !*board) board = "generic_pc";

    const board_binding* base = board_profiles_resolve(board);
    g_bind = board_profiles_load_config(base, board);

    /* DEFAULT-OFF gate. The compiled-in default profile resolves for EVERY board,
     * so it can never be the activation trigger -- otherwise every native run would
     * change. The layer goes active ONLY on an explicit opt-in:
     *   - ULTRA_CONTROLS=1               (force real input on this run), OR
     *   - ULTRA_CONTROLS_CONFIG=<path>   (an explicit config path), OR
     *   - a real ultra_controls.ini / bundled <id>.ini was actually found on disk.
     * With none of those, g_active stays 0 and hook_dllmain keeps the legacy
     * gamepad/autopilot producer -- so the three playable games are byte-identical. */
    int forced = 0, explicit_cfg = 0;
#if defined(_WIN32)
    /* read these via the Win32 environment (not getenv) so a value the launch path
     * set with SetEnvironmentVariableA is honored even if the CRT env cache differs.
     * The board id above is read the same way, keeping the whole gate consistent. */
    char eb[8], cb[8];
    DWORD fn = GetEnvironmentVariableA("ULTRA_CONTROLS", eb, sizeof eb);
    forced = (fn > 0 && fn < sizeof eb && eb[0] == '1');
    DWORD cn = GetEnvironmentVariableA("ULTRA_CONTROLS_CONFIG", cb, sizeof cb);
    explicit_cfg = (cn > 0);   /* any non-empty value = an explicit config path */
#else
    const char* force = getenv("ULTRA_CONTROLS");
    forced = force && force[0] == '1';
    const char* cfg = getenv("ULTRA_CONTROLS_CONFIG");
    explicit_cfg = cfg && *cfg;
#endif
    int file_found = board_profiles_config_found();
    g_active = (forced || explicit_cfg || file_found) ? 1 : 0;

    /* assemble the source list (priority order set per source). Each source is
     * compiled in but only contributes when available() AND the profile binds
     * something to it -- so an absent pad / DI device is silently skipped. */
    g_nsources = 0;
    add_source(ultra_src_keyboard_get());
    add_source(ultra_src_mouse_get());
    add_source(ultra_src_xinput_get());
    add_source(ultra_src_dinput_get());
    add_source(ultra_src_gun_get());
    add_source(ultra_src_autopilot_get());

    if (g_active)
        cl_log("[controls] input-map ACTIVE (board='%s' genre='%s' btns=%d axes=%d "
               "sources=%d)\n", board, g_bind ? g_bind->genre : "?",
               g_bind ? g_bind->nbtns : 0, g_bind ? g_bind->naxes : 0, g_nsources);
    else
        cl_log("[controls] input-map wired but inert (set ULTRA_CONTROLS=1 or supply "
               "a profile/config to activate)\n");

    /* Print the per-channel calibration the shared-block path will now map onto.
     * This is the number that was silently wrong: the loader publishes a generic
     * 0x8000 center, and until 2026-09-10 the hook fed that straight to the game
     * regardless of what the cabinet's own NVRAM said the center was. Wacky Race
     * calibrates steering 4000..64000 (center 34000), so 0x8000 was a permanent
     * 1232-count left pull. Seeing "center=" match the game's own calibration is
     * the proof the mapping is live. */
    if (g_active && g_bind && g_bind->axes) {
        for (int i = 0; i < g_bind->naxes; ++i) {
            const ui_axis_bind* a = &g_bind->axes[i];
            if (a->jvs_channel < 0 || a->out_min == a->out_max) continue;
            cl_log("[controls] analog ch%d range=%u..%u center=%u (SHM 0x8000 -> %u)\n",
                   a->jvs_channel, (unsigned)a->out_min, (unsigned)a->out_max,
                   (unsigned)((a->out_min + a->out_max) / 2),
                   (unsigned)(a->out_min + 0x8000u * (unsigned)(a->out_max - a->out_min) / 65535u));
        }
    }

}

int ultra_input_map_active(void) { return g_active; }

/* The active profile's JVS output range for an analog channel.
 *
 * WHY THIS EXISTS. The loader publishes a GENERIC axis into ULTRA_IO_SHM:
 * 0..65535 with 0x8000 as center, because the loader has no idea which game is
 * about to run. A real cabinet's axis is not generic -- Wacky Race's own NVRAM
 * (sv/WR0.CFG) calibrates steering as iostmin=4000 iostmax=64000, so ITS center
 * is 34000, not 32768. The hook used to copy the shared block's analog straight
 * through, which fed the game a value 1232 counts left of center on every single
 * frame: a permanent, constant left bias that reads in-game as "the wheel is
 * stuck turning left". The profile already carries the right numbers per game
 * (ui_axis_bind.out_min/out_max); this hands them to the shared-block path so a
 * loader-driven axis lands in the same range a locally-driven one does.
 *
 * Returns 1 and fills *omin/*omax if some binding renders into `channel`,
 * 0 if nothing is bound there (caller keeps the raw value). */
int ultra_input_map_channel_range(int channel, uint16_t* omin, uint16_t* omax,
                                  int* bipolar) {
    if (!g_bind || !g_bind->axes || channel < 0) return 0;
    for (int i = 0; i < g_bind->naxes; ++i) {
        const ui_axis_bind* a = &g_bind->axes[i];
        if (a->jvs_channel != channel) continue;
        if (a->out_min == a->out_max) continue;      /* degenerate, ignore */
        if (omin) *omin = a->out_min;
        if (omax) *omax = a->out_max;
        /* Which end of the range is REST? A wheel or stick rests in the middle;
         * a pedal or a gun axis rests at the floor. Getting this backwards is
         * not cosmetic -- resting a throttle at mid-range is a car that drives
         * off on its own before the player touches anything. */
        if (bipolar) *bipolar = (a->intent == UI_AX_STEER ||
                                 a->intent == UI_AX_STICK_X ||
                                 a->intent == UI_AX_STICK_Y);
        return 1;
    }
    return 0;
}


void ultra_input_map_current(ultra_jvs_input* out) {
    if (!out) return;
#if defined(_WIN32)
    if (g_lock_init) EnterCriticalSection(&g_lock);
#endif
    /* clear digital/analog, preserve coin counters */
    ui_intent_clear(&g_intent);
    for (int i = 0; i < g_nsources; ++i) {
        const ultra_input_source* s = g_sources[i];
        if (s->available && !s->available()) continue;
        if (s->poll) s->poll(&g_intent, g_bind);
    }
    intent_to_jvs_input(&g_intent, g_bind, out);
#if defined(_WIN32)
    if (g_lock_init) LeaveCriticalSection(&g_lock);
#endif
}

uint32_t ultra_input_map_coin(int slot) {
    if (slot < 0 || slot >= UI_MAX_PLAYERS) return 0;
    uint32_t v;
#if defined(_WIN32)
    if (g_lock_init) EnterCriticalSection(&g_lock);
#endif
    v = g_intent.coin[slot];
#if defined(_WIN32)
    if (g_lock_init) LeaveCriticalSection(&g_lock);
#endif
    return v;
}

/* expose the lookup table accessors (defined in input_profiles.c) through the
 * controls.h surface so the test + the producer share one resolver. */
const board_binding* board_binding_lookup(const char* system_id) {
    return board_profiles_resolve(system_id);
}
