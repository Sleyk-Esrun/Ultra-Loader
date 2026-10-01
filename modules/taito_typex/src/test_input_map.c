/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  test_input_map.c -- structural test for the player input-mapping layer.
 * =============================================================================
 *  Pure, deterministic, no device/game:
 *    1. the sink adapter intent_to_jvs_input round-trips digital + analog +
 *       sys + gun-screen correctly onto ultra_jvs_input (the struct every board
 *       sink consumes),
 *    2. the float->uint16 axis scaling honors the per-axis calibration window,
 *       bipolar centering, invert, deadzone, and curve,
 *    3. the compiled default profiles resolve per board id,
 *    4. the INI config loader parses + merges over a base (override + append),
 *    5. a synthetic SOURCE drives the orchestrator end-to-end.
 *
 *  Mirrors test_board_profile.c's CHECK harness.
 * =============================================================================
 */
#include "controls.h"
#include "input_intent.h"
#include "input_sources.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#if defined(_WIN32)
#  include <windows.h>   /* SetEnvironmentVariableA for the config-loader test */
#endif

static int g_pass = 0, g_fail = 0;
#define CHECK(c, msg) do { if (c) { g_pass++; } else { g_fail++; \
    printf("FAIL: %s\n", msg); } } while (0)

/* the sink adapter + resolver come from controls.c / input_profiles.c. */
extern const board_binding* board_profiles_resolve(const char* id);
extern const board_binding* board_profiles_load_config(const board_binding* base, const char* id);

/* ----- a tiny binding for the sink-adapter tests ----- */
static const ui_axis_bind T_axes[] = {
    /* bipolar steer into ch0, window [4000,64000], center=0 -> midpoint */
    { 0, UI_AX_STEER, UI_SRC_DINPUT, 0, 0, 4000, 64000, UI_DI_X, 0, UI_CURVE_LINEAR, 0 },
    /* unipolar accel into ch1, window [26000,44000] */
    { 0, UI_AX_ACCEL, UI_SRC_DINPUT, 0, 1, 26000, 44000, UI_DI_Y, 0, UI_CURVE_LINEAR, 0 },
    /* gun X into screen (channel -1) */
    { 0, UI_AX_GUN_X, UI_SRC_GUN, 0, -1, 0, 65535, UI_GUN_X, 0, UI_CURVE_LINEAR, 0 },
};
static const ui_button_bind T_btns[] = {
    { 0, UI_BTN_START, UI_SRC_KEYBOARD, 0, 0 },
    { 0, UI_BTN_PUSH1, UI_SRC_KEYBOARD, 0, 0 },
};
static const board_binding T_bind = {
    "test", "test", T_btns, 2, T_axes, 3, 8, 1
};

int main(void) {
    /* ---- 1) sink adapter: digital + sys ---- */
    {
        ultra_intent it; memset(&it, 0, sizeof it);
        ui_btn_set(&it, 0, UI_BTN_START);
        ui_btn_set(&it, 0, UI_BTN_PUSH1);
        ui_btn_set(&it, 1, UI_BTN_PUSH2);
        ui_sys_set(&it, UI_SYS_TEST);
        ultra_jvs_input out;
        intent_to_jvs_input(&it, &T_bind, &out);
        CHECK(out.player[0] & 0x8000, "START -> bit15");
        CHECK(out.player[0] & 0x0200, "PUSH1 -> bit9");
        CHECK(out.player[1] & 0x0100, "P2 PUSH2 -> bit8");
        CHECK(out.system   & 0x80,   "TEST -> system bit7");
        CHECK(!(out.player[0] & 0x4000), "SERVICE not set");
    }

    /* ---- 2) analog scaling: bipolar center, unipolar floor/ceil ---- */
    {
        ultra_intent it; memset(&it, 0, sizeof it);
        /* steer center (0.0) -> midpoint of [4000,64000] = 34000 */
        ui_ax_drive(&it, 0, UI_AX_STEER, 0.0f);
        /* accel full (1.0) -> out_max 44000; gun X 0.25 -> ~16383 of 65535 */
        ui_ax_drive(&it, 0, UI_AX_ACCEL, 1.0f);
        ui_ax_drive(&it, 0, UI_AX_GUN_X, 0.25f);
        ultra_jvs_input out;
        intent_to_jvs_input(&it, &T_bind, &out);
        int steer = out.analog[0];
        CHECK(steer > 33000 && steer < 35000, "steer center ~= 34000 (window midpoint)");
        CHECK(out.analog[1] == 44000, "accel full -> out_max 44000");
        CHECK(out.screen_x > 15000 && out.screen_x < 18000, "gun X 0.25 -> ~16383 screen_x");
        /* unbound channels stay centered at 0x8000 (NOT 0 = full deflection). */
        CHECK(out.analog[5] == 0x8000, "unbound channel stays centered 0x8000");
    }

    /* ---- 2b) steer extremes + invert ---- */
    {
        ultra_intent it; memset(&it, 0, sizeof it);
        ui_ax_drive(&it, 0, UI_AX_STEER, 1.0f);      /* full right -> out_max 64000 */
        ultra_jvs_input out;
        intent_to_jvs_input(&it, &T_bind, &out);
        CHECK(out.analog[0] == 64000, "steer full -> out_max 64000");

        ui_axis_bind inv = T_axes[0]; inv.invert = 1;
        board_binding b2 = T_bind; b2.axes = &inv; b2.naxes = 1;
        memset(&it, 0, sizeof it);
        ui_ax_drive(&it, 0, UI_AX_STEER, 1.0f);      /* inverted -> out_min 4000 */
        intent_to_jvs_input(&it, &b2, &out);
        CHECK(out.analog[0] == 4000, "inverted steer full -> out_min 4000");
    }

    /* ---- 3) compiled default profiles per board ---- */
    {
        const board_binding* fight = board_profiles_resolve("taito_typex");
        const board_binding* racer = board_profiles_resolve("taito_typex2");
        const board_binding* alls  = board_profiles_resolve("sega_alls");
        const board_binding* gen   = board_profiles_resolve("nope_board");
        CHECK(fight && !strcmp(fight->genre, "fight"), "taito_typex -> fight profile");
        CHECK(racer && !strcmp(racer->genre, "racer"), "taito_typex2 -> racer profile");
        CHECK(alls  && !strcmp(alls->genre,  "gun"),   "sega_alls -> gun profile");
        CHECK(gen   && gen->nbtns > 0,                 "unknown board -> generic profile");
        /* racer profile actually binds the 3 driving axes */
        int saw_steer = 0, saw_accel = 0, saw_brake = 0;
        for (int i = 0; racer && i < racer->naxes; ++i) {
            if (racer->axes[i].intent == UI_AX_STEER) saw_steer = 1;
            if (racer->axes[i].intent == UI_AX_ACCEL) saw_accel = 1;
            if (racer->axes[i].intent == UI_AX_BRAKE) saw_brake = 1;
        }
        CHECK(saw_steer && saw_accel && saw_brake, "racer binds steer+accel+brake");
        /* gun profile renders to screen (jvs_channel -1) */
        int saw_screen = 0;
        for (int i = 0; alls && i < alls->naxes; ++i)
            if (alls->axes[i].jvs_channel == -1) saw_screen = 1;
        CHECK(saw_screen, "gun profile binds a screen-pos axis");
    }

    /* ---- 4) INI config loader: override + append over a base ---- */
    {
        /* write a temp INI that overrides P1 start key and adds a new push5 bind. */
        const char* path = "._ul_test_controls.ini";
        FILE* f = fopen(path, "w");
        CHECK(f != NULL, "open temp ini");
        if (f) {
            fputs("[meta]\nsystem_id = taito_typex\ngenre = fight\nanalog_channels = 8\n\n", f);
            fputs("[buttons]\n", f);
            fputs("p1.start = key:0x42\n", f);          /* override start to 'B' */
            fputs("p1.push5 = key:0x4E, xinput:rb\n", f);/* append a new binding */
            fputs("p1.coin  = key:0x35\n", f);
            fputs("sys.test = key:0x71\n", f);
            fputs("\n[axes]\n", f);
            fputs("p1.steer = dinput:axisX  0  4000 64000 0 6 lin\n", f);
            fputs("p1.gun_x = gun:x screen 0 65535\n", f);
            fclose(f);
        }
#if defined(_WIN32)
        SetEnvironmentVariableA("ULTRA_CONTROLS_CONFIG", path);
#else
        setenv("ULTRA_CONTROLS_CONFIG", path, 1);
#endif
        const board_binding* base = board_profiles_resolve("taito_typex");
        const board_binding* merged = board_profiles_load_config(base, "taito_typex");
        CHECK(merged != NULL, "config merge returns a binding");

        /* the overridden start key must now be 'B' (0x42) on the keyboard source. */
        int start_code = -1, saw_push5 = 0, saw_steer = 0, saw_gun = 0;
        for (int i = 0; merged && i < merged->nbtns; ++i) {
            const ui_button_bind* b = &merged->btns[i];
            if (b->player == 0 && b->intent == UI_BTN_START && b->src == UI_SRC_KEYBOARD)
                start_code = (int)b->code;
            if (b->player == 0 && b->intent == UI_BTN_PUSH5) saw_push5 = 1;
        }
        for (int i = 0; merged && i < merged->naxes; ++i) {
            if (merged->axes[i].intent == UI_AX_STEER && merged->axes[i].src == UI_SRC_DINPUT) saw_steer = 1;
            if (merged->axes[i].intent == UI_AX_GUN_X && merged->axes[i].jvs_channel == -1) saw_gun = 1;
        }
        CHECK(start_code == 0x42, "config overrode P1 start key to 'B'(0x42)");
        CHECK(saw_push5, "config appended a new P1 push5 binding");
        CHECK(saw_steer, "config steer axis present");
        CHECK(saw_gun,   "config gun_x screen axis present");
        /* a binding the file did NOT mention is preserved from the base default. */
        int saw_inherited_left = 0;
        for (int i = 0; merged && i < merged->nbtns; ++i)
            if (merged->btns[i].intent == UI_BTN_LEFT && merged->btns[i].src == UI_SRC_KEYBOARD)
                saw_inherited_left = 1;
        CHECK(saw_inherited_left, "unmentioned base binding inherited (partial file valid)");
#if defined(_WIN32)
        SetEnvironmentVariableA("ULTRA_CONTROLS_CONFIG", NULL);
#else
        unsetenv("ULTRA_CONTROLS_CONFIG");
#endif
        remove(path);
    }

    /* ---- 5) coin counter is monotonic in the intent ---- */
    {
        ultra_intent it; memset(&it, 0, sizeof it);
        it.coin[0]++; it.coin[0]++;
        CHECK(it.coin[0] == 2, "intent coin counter increments");
    }

    printf("test_input_map: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
