/* SPDX-License-Identifier: MIT
 * NOTE: independent namco_es3 board copy -- adapted from native_x86/src/hook/input_src_mouse.c (reference only; no build-time or runtime dependency on native_x86 remains).
 * =============================================================================
 *  input_src_mouse_es3.c -- mouse SOURCE backend (cursor absolute + buttons).
 * =============================================================================
 *
 *  Maps the OS cursor (absolute, over the game's device window when known, else the
 *  primary screen) to the gun X/Y intent axes (0..1), and the mouse buttons to
 *  intent buttons (left=trigger by convention). For a mouse-aimed shooter this lets
 *  the player aim with the mouse without the Ultra Omni-Track hardware.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the public Win32 cursor/button APIs.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include "input_sources.h"
#include "controls.h"

/* the D3D9/D3D11/GL present hooks hand the gun bridge the device window; reuse it
 * so the mouse maps over the game's client rect when known. */
HWND ultra_typex2_device_window(void);

static int g_coin_was[UI_MAX_PLAYERS];

static int mouse_available(void) { return 1; }

/* current cursor as a 0..1 position over the target rect. */
static int cursor_norm(float* nx, float* ny) {
    POINT pt;
    if (!GetCursorPos(&pt)) return 0;
    HWND w = ultra_typex2_device_window();
    RECT rc;
    if (w && GetClientRect(w, &rc)) {
        POINT tl = { 0, 0 };
        ClientToScreen(w, &tl);
        int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
        if (cw <= 0 || ch <= 0) return 0;
        float x = (float)(pt.x - tl.x) / (float)cw;
        float y = (float)(pt.y - tl.y) / (float)ch;
        *nx = x < 0 ? 0 : (x > 1 ? 1 : x);
        *ny = y < 0 ? 0 : (y > 1 ? 1 : y);
        return 1;
    }
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw <= 0 || sh <= 0) return 0;
    *nx = (float)pt.x / (float)sw;
    *ny = (float)pt.y / (float)sh;
    return 1;
}

static int mouse_btn_down(uint32_t which) {
    int vk = (which == 1) ? VK_RBUTTON : (which == 2) ? VK_MBUTTON : VK_LBUTTON;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void mouse_poll(ultra_intent* out, const struct board_binding* bind) {
    if (!bind) return;

    for (int i = 0; i < bind->nbtns; ++i) {
        const ui_button_bind* bb = &bind->btns[i];
        if (bb->src != UI_SRC_MOUSE) continue;
        int pressed = mouse_btn_down(bb->code);
        if (!pressed) {
            if (bb->player == 0xFD && bb->intent >= 0 && bb->intent < UI_MAX_PLAYERS)
                g_coin_was[bb->intent] = 0;
            continue;
        }
        if (bb->player == 0xFE) {
            if (bb->intent < UI_SYS__COUNT) ui_sys_set(out, (ui_sysbit)bb->intent);
        } else if (bb->player == 0xFD) {
            int slot = bb->intent;
            if (slot >= 0 && slot < UI_MAX_PLAYERS) {
                if (!g_coin_was[slot]) out->coin[slot]++;
                g_coin_was[slot] = 1;
            }
        } else if (bb->intent < UI_BTN__COUNT) {
            ui_btn_set(out, bb->player, (ui_button)bb->intent);
        }
    }

    int want_axis = 0;
    for (int i = 0; i < bind->naxes; ++i)
        if (bind->axes[i].src == UI_SRC_MOUSE) { want_axis = 1; break; }
    if (!want_axis) return;

    float nx, ny;
    if (!cursor_norm(&nx, &ny)) return;
    for (int i = 0; i < bind->naxes; ++i) {
        const ui_axis_bind* ab = &bind->axes[i];
        if (ab->src != UI_SRC_MOUSE) continue;
        float v = (ab->code == UI_MAX_CURSOR_Y) ? ny : nx;
        ui_ax_drive(out, ab->player, (ui_axis)ab->intent, v);
    }
}

static const ultra_input_source g_src = {
    "mouse", mouse_available, mouse_poll, /*priority=*/25
};
const ultra_input_source* ultra_src_mouse_get(void) { return &g_src; }

#else
#include "input_sources.h"
const ultra_input_source* ultra_src_mouse_get(void) { return 0; }
#endif /* _WIN32 */
