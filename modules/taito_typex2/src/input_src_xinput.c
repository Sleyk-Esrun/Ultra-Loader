/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  input_src_xinput.c -- XInput gamepad SOURCE backend.
 * =============================================================================
 *
 *  Reads controller 0 via a dynamically-loaded XInput, maps the profile's XInput
 *  bindings (button mask -> intent button, stick/trigger axis id -> intent axis)
 *  into the shared intent. Coin via an edge-detected coin binding. This is the
 *  refactor of hook_dllmain.c's xinput_thread button/axis decode, now expressed as
 *  intent so it flows through the profile/sink like every other source.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the public XInput API.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include "input_sources.h"
#include "controls.h"

typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } ul_xpad;
typedef struct { DWORD packet; ul_xpad pad; } ul_xstate;
typedef DWORD (WINAPI *p_xinput_get)(DWORD, ul_xstate*);

static p_xinput_get s_xget    = NULL;
static int          s_tried   = 0;
static int          s_present = 0;
static int          g_coin_was[UI_MAX_PLAYERS];

static void xinput_try_load(void) {
    if (s_tried) return;
    s_tried = 1;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
    for (int i = 0; dlls[i] && !s_xget; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) s_xget = (p_xinput_get)(void*)GetProcAddress(h, "XInputGetState");
    }
}

static int xinput_available(void) {
    xinput_try_load();
    if (!s_xget) return 0;
    ul_xstate st; ZeroMemory(&st, sizeof st);
    s_present = (s_xget(0, &st) == 0);   /* controller 0 connected? */
    return s_present;
}

/* the live pad state, re-read once per poll. */
static int read_pad(ul_xstate* st) {
    if (!s_xget) return 0;
    ZeroMemory(st, sizeof *st);
    return s_xget(0, st) == 0;
}

/* normalized stick/trigger value for an XInput axis id. */
static float xinput_axis(const ul_xpad* p, uint32_t id) {
    switch (id) {
        case UI_XAX_LX: return (float)p->lx / 32767.0f;
        case UI_XAX_LY: return (float)p->ly / 32767.0f;
        case UI_XAX_RX: return (float)p->rx / 32767.0f;
        case UI_XAX_RY: return (float)p->ry / 32767.0f;
        case UI_XAX_LT: return (float)p->bLT / 255.0f;
        case UI_XAX_RT: return (float)p->bRT / 255.0f;
        default:        return 0.0f;
    }
}

static void xinput_poll(ultra_intent* out, const struct board_binding* bind) {
    if (!bind) return;
    ul_xstate st;
    if (!read_pad(&st)) return;
    const ul_xpad* p = &st.pad;
    WORD b = p->wButtons;

    for (int i = 0; i < bind->nbtns; ++i) {
        const ui_button_bind* bb = &bind->btns[i];
        if (bb->src != UI_SRC_XINPUT) continue;
        int pressed = (b & (WORD)bb->code) != 0;
        if (!pressed) continue;
        if (bb->player == 0xFE) {                       /* system bit */
            if (bb->intent < UI_SYS__COUNT) ui_sys_set(out, (ui_sysbit)bb->intent);
        } else if (bb->player == 0xFD) {                /* coin slot */
            int slot = bb->intent;
            if (slot >= 0 && slot < UI_MAX_PLAYERS) {
                if (!g_coin_was[slot]) out->coin[slot]++;
                g_coin_was[slot] = 1;
            }
        } else if (bb->intent < UI_BTN__COUNT) {
            ui_btn_set(out, bb->player, (ui_button)bb->intent);
        }
    }
    /* reset coin edge for any slot whose coin button is up this frame. */
    for (int slot = 0; slot < UI_MAX_PLAYERS; ++slot) {
        int held = 0;
        for (int i = 0; i < bind->nbtns; ++i) {
            const ui_button_bind* bb = &bind->btns[i];
            if (bb->src == UI_SRC_XINPUT && bb->player == 0xFD && bb->intent == slot
                && (b & (WORD)bb->code)) { held = 1; break; }
        }
        if (!held) g_coin_was[slot] = 0;
    }

    /* axes: stick deadzone handled by the profile/shape step; here just normalize.
     * Only drive a channel when the stick/trigger is meaningfully off-center so a
     * resting pad does not stomp a wheel/DI source. */
    for (int i = 0; i < bind->naxes; ++i) {
        const ui_axis_bind* ab = &bind->axes[i];
        if (ab->src != UI_SRC_XINPUT) continue;
        float v = xinput_axis(p, ab->code);
        /* D-PAD ALSO STEERS.
         *
         * A driving cabinet has no left/right buttons - you steer to go
         * sideways - so on a gamepad the d-pad is dead weight unless it drives
         * the steering axis. Fold it in here rather than as its own binding,
         * because upsert_axis() keys bindings on (player, intent, source): a
         * second XInput steer binding would REPLACE the stick one rather than
         * sit alongside it.
         *
         * Only for axes that actually steer, and only while the stick itself is
         * inside its deadband, so a real stick (or a wheel on another source)
         * always wins and the d-pad can never fight it. Masks are XInput's
         * DPAD_LEFT 0x0004 / DPAD_RIGHT 0x0008. */
        if (ab->intent == UI_AX_STEER || ab->intent == UI_AX_STICK_X) {
            float d = 0.0f;
            if (b & 0x0004) d -= 1.0f;          /* d-pad left  */
            if (b & 0x0008) d += 1.0f;          /* d-pad right */
            if (d != 0.0f && v > -0.18f && v < 0.18f) v = d;
        }
        int bipolar = (ab->code == UI_XAX_LX || ab->code == UI_XAX_LY ||
                       ab->code == UI_XAX_RX || ab->code == UI_XAX_RY);
        float mag = v < 0 ? -v : v;
        if (mag > 0.18f || (!bipolar && v > 0.06f))
            ui_ax_drive(out, ab->player, (ui_axis)ab->intent, v);
    }
}

static const ultra_input_source g_src = {
    "xinput", xinput_available, xinput_poll, /*priority=*/20
};
const ultra_input_source* ultra_src_xinput_get(void) { return &g_src; }

#else
#include "input_sources.h"
const ultra_input_source* ultra_src_xinput_get(void) { return 0; }
#endif /* _WIN32 */
