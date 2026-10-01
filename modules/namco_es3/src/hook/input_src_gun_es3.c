/* SPDX-License-Identifier: MIT
 * NOTE: independent namco_es3 board copy -- adapted from native_x86/src/hook/input_src_gun.c (reference only; no build-time or runtime dependency on native_x86 remains).
 * =============================================================================
 *  input_src_gun_es3.c -- Ultra Omni-Track lightgun SOURCE backend.
 * =============================================================================
 *
 *  Brings the Ultra Omni-Track lightgun pose into the intent as a first-class
 *  source for ALL boards (not just the Type X2 cursor bridge). The universal
 *  lightgun engine lands a calibrated pose in the loader's shared block
 *  (ultra_io_shared::screen_x/screen_y, 0..65535, + the trigger bit in player[0]);
 *  this reads that pose with the SAME seqlock snapshot the typex2 gun bridge uses,
 *  and exposes it as UI_AX_GUN_X/Y (0..1) + UI_BTN_GUN_TRIGGER.
 *
 *  When no loader/gun pose is live the source is "unavailable" so it does not stomp
 *  a mouse-aimed fallback; the gun cab's mouse binding (if present) takes over.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the loader's own shared I/O block.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include "input_sources.h"
#include "controls.h"
#include "ultra_io_shared.h"

/* provided by hook_dllmain.c */
ultra_io_shared* ultra_hook_shared(void);

/* coherent seqlock snapshot of the gun pose (mirrors typex2_gun.c). */
static int gun_snapshot(int* x, int* y, int* trig) {
    ultra_io_shared* s = ultra_hook_shared();
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
    *x = sx; *y = sy; *trig = (p0 & 0x0200) != 0;   /* 0x0200 = PUSH1/trigger bit */
    return 1;
}

static int gun_available(void) {
    int x, y, t;
    return gun_snapshot(&x, &y, &t);
}

static void gun_poll(ultra_intent* out, const struct board_binding* bind) {
    if (!bind) return;
    int x, y, t;
    if (!gun_snapshot(&x, &y, &t)) return;
    float nx = (float)x / 65535.0f, ny = (float)y / 65535.0f;

    for (int i = 0; i < bind->naxes; ++i) {
        const ui_axis_bind* ab = &bind->axes[i];
        if (ab->src != UI_SRC_GUN) continue;
        float v = (ab->code == UI_GUN_Y) ? ny : nx;
        ui_ax_drive(out, ab->player, (ui_axis)ab->intent, v);
    }
    for (int i = 0; i < bind->nbtns; ++i) {
        const ui_button_bind* bb = &bind->btns[i];
        if (bb->src != UI_SRC_GUN) continue;
        /* code: 0 = trigger, 1 = reload (off-screen). */
        int pressed = (bb->code == 1) ? 0 : t;   /* reload edge handled by engine */
        if (pressed && bb->intent < UI_BTN__COUNT)
            ui_btn_set(out, bb->player, (ui_button)bb->intent);
    }
}

static const ultra_input_source g_src = {
    "gun", gun_available, gun_poll, /*priority=*/35
};
const ultra_input_source* ultra_src_gun_get(void) { return &g_src; }

#else
#include "input_sources.h"
const ultra_input_source* ultra_src_gun_get(void) { return 0; }
#endif /* _WIN32 */
