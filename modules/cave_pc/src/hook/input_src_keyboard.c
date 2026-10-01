/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  input_src_keyboard.c -- keyboard SOURCE backend (GetAsyncKeyState).
 * =============================================================================
 *
 *  Maps the profile's keyboard bindings to intent buttons/sys/coin and synthesizes
 *  bipolar/unipolar axes from key pairs (e.g. Left/Right -> STEER -1/+1). Coin keys
 *  are edge-detected: a press increments the per-slot coin counter once.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the public Win32 GetAsyncKeyState.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include "input_sources.h"
#include "controls.h"

static int key_down(uint32_t vk) {
    if (!vk) return 0;
    return (GetAsyncKeyState((int)vk) & 0x8000) != 0;
}

/* per-slot coin edge state for keyboard coin bindings. */
static int g_coin_was[UI_MAX_PLAYERS];

static int kbd_available(void) { return 1; }   /* a keyboard is always present */

static void kbd_poll(ultra_intent* out, const struct board_binding* bind) {
    if (!bind) return;

    /* digital + coin + sys */
    for (int i = 0; i < bind->nbtns; ++i) {
        const ui_button_bind* bb = &bind->btns[i];
        if (bb->src != UI_SRC_KEYBOARD) continue;
        if (!key_down(bb->code)) continue;
        int p = bb->player;
        /* a binding to UI_BTN__COUNT+UI_SYS_* range is encoded as a sys binding via
         * player==0xFF; we keep sys separate below, so here only real buttons. */
        if (bb->intent < UI_BTN__COUNT)
            ui_btn_set(out, p, (ui_button)bb->intent);
    }

    /* axes from key pairs: code = (pos_vk<<16)|neg_vk. */
    for (int i = 0; i < bind->naxes; ++i) {
        const ui_axis_bind* ab = &bind->axes[i];
        if (ab->src != UI_SRC_KEYBOARD) continue;
        uint16_t neg = (uint16_t)(ab->code & 0xFFFF);
        uint16_t pos = (uint16_t)(ab->code >> 16);
        int dn = key_down(neg), up = key_down(pos);
        float v;
        if (pos == 0) {            /* unipolar: single key = 0..1 */
            v = dn ? 1.0f : 0.0f;
        } else {                   /* bipolar pair */
            v = (up ? 1.0f : 0.0f) - (dn ? 1.0f : 0.0f);
        }
        if (dn || up) ui_ax_drive(out, ab->player, (ui_axis)ab->intent, v);
    }
}

/* sys + coin bindings are encoded with a sentinel player/intent; we walk the same
 * button array but treat UI_SYS_* and the coin pseudo-intent here. To keep the
 * binding struct uniform, sys uses player==0xFE + intent in [0..UI_SYS__COUNT),
 * and coin uses player==0xFD + intent = slot. */
static void kbd_poll_sys_coin(ultra_intent* out, const struct board_binding* bind) {
    if (!bind) return;
    for (int i = 0; i < bind->nbtns; ++i) {
        const ui_button_bind* bb = &bind->btns[i];
        if (bb->src != UI_SRC_KEYBOARD) continue;
        if (bb->player == 0xFE) {                 /* system bit */
            if (key_down(bb->code) && bb->intent < UI_SYS__COUNT)
                ui_sys_set(out, (ui_sysbit)bb->intent);
        } else if (bb->player == 0xFD) {          /* coin slot */
            int slot = bb->intent;
            if (slot >= 0 && slot < UI_MAX_PLAYERS) {
                int now = key_down(bb->code);
                if (now && !g_coin_was[slot]) out->coin[slot]++;
                g_coin_was[slot] = now;
            }
        }
    }
}

static void kbd_poll_all(ultra_intent* out, const struct board_binding* bind) {
    kbd_poll(out, bind);
    kbd_poll_sys_coin(out, bind);
}

static const ultra_input_source g_src = {
    "keyboard", kbd_available, kbd_poll_all, /*priority=*/10
};
const ultra_input_source* ultra_src_keyboard_get(void) { return &g_src; }

#else
#include "input_sources.h"
const ultra_input_source* ultra_src_keyboard_get(void) { return 0; }
#endif /* _WIN32 */
