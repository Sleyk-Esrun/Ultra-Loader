/* SPDX-License-Identifier: MIT
 * NOTE: independent namco_es3 board copy -- adapted from native_x86/src/hook/input_autopilot.c (reference only; no build-time or runtime dependency on native_x86 remains).
 * =============================================================================
 *  input_autopilot_es3.c -- the default-off TEST autopilots, as a SOURCE backend.
 * =============================================================================
 *
 *  The three scripted drivers (Wacky Race racer, DEMO/Chaos-Breaker nav, and a
 *  generic gun auto-fire) wrapped behind the source interface so they flow through
 *  the same intent->profile->sink path as real input -- but ONLY when their exact
 *  env gate is set:
 *     ULTRA_WR_AUTO=1     -> racer autopilot   (accel held, steer center, START sweep)
 *     ULTRA_DEMO_INPUT=1  -> menu/fight nav    (coin, START, then mash play buttons)
 *     ULTRA_GUN_AUTO=1    -> gun auto-fire     (center + periodic trigger)
 *
 *  This is the lowest-priority source, so the moment a human touches a real control
 *  (keyboard/pad/wheel/gun) that source's higher-priority active analog/digital
 *  overrides the autopilot. With no ULTRA_*_AUTO gate set, available() returns 0 and
 *  the source contributes nothing -- the autopilots stay a deliberate test mode.
 *
 *  NB: these wrappers preserve the env-knob CONTRACT of the originals. The legacy
 *  copies inside hook_dllmain.c's xinput_thread / typex2_gun.c are UNCHANGED and
 *  remain the path used when the input-map layer is inactive, so nothing regresses.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include "input_sources.h"
#include "controls.h"

static int   s_checked = 0;
static int   s_wr = 0, s_demo = 0, s_gun = 0;
static DWORD s_t0 = 0;

/* Read an env var from the LIVE Win32 environment (GetEnvironmentVariableA), NOT
 * mingw's startup-cached CRT block (getenv) -- so a gate set at runtime via
 * SetEnvironmentVariableA AFTER CRT init is still honored. Matches controls.c's
 * env reads (ULTRA_CONTROLS / ULTRA_BOARD). Returns buf iff the var exists + fits. */
static const char* ap_env(const char* name, char* buf, DWORD cap) {
    DWORD n = GetEnvironmentVariableA(name, buf, cap);
    return (n > 0 && n < cap) ? buf : NULL;
}

static void check_env(void) {
    if (s_checked) return;
    s_checked = 1;
    char b[32]; const char* e;
    e = ap_env("ULTRA_WR_AUTO",    b, sizeof b); s_wr   = e && e[0] == '1';
    e = ap_env("ULTRA_DEMO_INPUT", b, sizeof b); s_demo = e && e[0] == '1';
    e = ap_env("ULTRA_GUN_AUTO",   b, sizeof b); s_gun  = e && e[0] == '1';
    s_t0 = GetTickCount();
}

static int auto_available(void) {
    check_env();
    return s_wr || s_demo || s_gun;
}

/* a ~8ms tick count from arm time, so the scripts run on the same cadence the
 * originals used (they slept 8ms/loop). */
static unsigned cur_tick(void) {
    DWORD dt = GetTickCount() - s_t0;
    return (unsigned)(dt / 8u);
}

/* coin slot 0, edge-managed by a one-shot counter the script bumps. */
static unsigned s_coin_emitted = 0;

static void wr_drive(ultra_intent* out) {
    unsigned t = cur_tick();
    /* top up a pair of coins every ~2s for the first ~30s (mirrors the original). */
    static int coin_amt = 0;
    if (!coin_amt) { char b[32]; const char* e = ap_env("ULTRA_WR_COIN_AMT", b, sizeof b); coin_amt = (e&&*e)?atoi(e):2; if (coin_amt<1) coin_amt=1; }
    if (t >= 250u && t < 3800u && (t % 250u) == 0u) {
        /* emit the pair exactly once per 250-tick boundary */
        static unsigned last_boundary = 0xFFFFFFFFu;
        unsigned bnd = t / 250u;
        if (bnd != last_boundary) { out->coin[0] += (unsigned)coin_amt; last_boundary = bnd; }
    }
    /* START/decide bit sweep: long ~2.4s press + ~0.4s gap through candidate bits.
     * Candidates: START, then the VIEW/DECIDE buttons (PUSH1..PUSH4). */
    if (t >= 300u) {
        static const ui_button cand[] = {
            UI_BTN_START, UI_BTN_PUSH1, UI_BTN_PUSH2, UI_BTN_PUSH3, UI_BTN_PUSH4
        };
        unsigned hold = 300, gap = 50, period = hold + gap;
        unsigned cyc = (t - 300u) % period;
        if (cyc < hold) {
            unsigned idx = ((t - 300u) / period) % (sizeof cand / sizeof cand[0]);
            ui_btn_set(out, 0, cand[idx]);
        }
    }
    /* driving axes: accel held, steer center, brake released -- the profile's axis
     * window scales these. We express them in normalized intent terms. */
    ui_ax_drive(out, 0, UI_AX_ACCEL, 1.0f);
    ui_ax_drive(out, 0, UI_AX_STEER, 0.0f);
    ui_ax_drive(out, 0, UI_AX_BRAKE, 0.0f);
}

static void demo_drive(ultra_intent* out) {
    unsigned t = cur_tick();
    if (t == 240u || t == 245u) out->coin[0]++;          /* 2 coins ~2s */
    if (t >= 360u && t < 368u) { ui_btn_set(out, 0, UI_BTN_START); return; }
    if (t > 420u) {
        unsigned ph = (t / 45u) % 6u;
        if ((t % 45u) < 6u) {
            switch (ph) {
                case 0: ui_btn_set(out, 0, UI_BTN_PUSH1); break;
                case 1: ui_btn_set(out, 0, UI_BTN_RIGHT); break;
                case 2: ui_btn_set(out, 0, UI_BTN_PUSH1); break;
                case 3: ui_btn_set(out, 0, UI_BTN_LEFT);  break;
                case 4: ui_btn_set(out, 0, UI_BTN_PUSH2); break;
                default: ui_btn_set(out, 0, UI_BTN_START); break;
            }
        }
    }
}

static void gun_drive(ultra_intent* out) {
    unsigned t = cur_tick();
    int fire_ms = 250;
    { char b[32]; const char* e = ap_env("ULTRA_GUN_AUTO_FIRE_MS", b, sizeof b); if (e && *e) fire_ms = atoi(e); }
    /* aim center; pulse the trigger on the fire cadence. */
    ui_ax_drive(out, 0, UI_AX_GUN_X, 0.5f);
    ui_ax_drive(out, 0, UI_AX_GUN_Y, 0.5f);
    if (fire_ms > 0) {
        unsigned fire_ticks = (unsigned)(fire_ms / 8); if (!fire_ticks) fire_ticks = 1;
        if ((t / fire_ticks) & 1u) ui_btn_set(out, 0, UI_BTN_GUN_TRIGGER);
    }
}

static void auto_poll(ultra_intent* out, const struct board_binding* bind) {
    (void)bind;   /* the autopilot drives intent directly, independent of bindings */
    check_env();
    (void)s_coin_emitted;
    if (s_wr)   wr_drive(out);
    if (s_demo) demo_drive(out);
    if (s_gun)  gun_drive(out);
}

static const ultra_input_source g_src = {
    "autopilot", auto_available, auto_poll, /*priority=*/0
};
const ultra_input_source* ultra_src_autopilot_get(void) { return &g_src; }

#else
#include "input_sources.h"
const ultra_input_source* ultra_src_autopilot_get(void) { return 0; }
#endif /* _WIN32 */
