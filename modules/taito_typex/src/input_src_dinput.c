/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  input_src_dinput.c -- DirectInput8 READER SOURCE backend (wheels/HOTAS/sticks).
 * =============================================================================
 *
 *  This reads the USER's physical DirectInput device (a racing wheel, flight stick,
 *  HOTAS). It is DISTINCT from dinput8_emu.c, which is the synthetic device the
 *  GAME sees. To avoid reading our own synthetic device, it enumerates real attached
 *  game controllers and acquires the first one, in background-cooperative mode.
 *
 *  Axis bindings use DIJOYSTATE field offsets in ui_axis_bind.code:
 *     UI_DI_X/Y/Z/RX/RY/RZ map to lX..lRz (each 0..65535, 32767 = center).
 *  Button bindings use the DI button index (0-based) in ui_button_bind.code.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the public DirectInput8 API.
 * =============================================================================
 */
#if defined(_WIN32)

#define DIRECTINPUT_VERSION 0x0800
#define COBJMACROS
#include <windows.h>
#include <dinput.h>
#include "input_sources.h"
#include "controls.h"
/* UI_DI_* axis selectors come from input_intent.h (shared with the config loader). */

static IDirectInput8A*       s_di  = NULL;
static IDirectInputDevice8A* s_dev = NULL;
static int  s_inited  = 0;
static int  s_present = 0;
static int  g_coin_was[UI_MAX_PLAYERS];

/* Is this DI device actually an XInput pad?
 *
 * Microsoft's documented test: the device interface path of an XInput device
 * contains "IG_". Such a pad is ALREADY handled by the XInput source, and
 * letting DirectInput bind it too is actively harmful, not merely redundant:
 * both sources write the SAME per-player intent, so whichever polls last wins.
 *
 * Live case (Wacky Race, 2026-09-13): the racer profile binds accel to
 * dinput:y "for a wheel". With only a gamepad attached, that bound the pad's
 * LEFT STICK Y, which rests at center = 0.5, and drove the accel intent to 0.5
 * every poll - stomping the value the right trigger had just written. The game
 * received a fixed 35000 on its accel channel (26000 + 0.5*18000 = exactly half
 * throttle), so the car accelerated to half speed and stayed there no matter
 * what the trigger did. Same class of bug as the Omni services' DI/XInput
 * double-claim fixed on the loader side. */
static int di_is_xinput_device(IDirectInputDevice8A* dev) {
    DIPROPGUIDANDPATH p;
    int i;
    memset(&p, 0, sizeof p);
    p.diph.dwSize = sizeof p;
    p.diph.dwHeaderSize = sizeof p.diph;
    p.diph.dwObj = 0;
    p.diph.dwHow = DIPH_DEVICE;
    if (FAILED(IDirectInputDevice8_GetProperty(dev, DIPROP_GUIDANDPATH, &p.diph)))
        return 0;                       /* unknown -> fail safe, keep the device */
    for (i = 0; i < MAX_PATH - 3 && p.wszPath[i]; ++i)
        if ((p.wszPath[i] == 'i' || p.wszPath[i] == 'I') &&
            (p.wszPath[i+1] == 'g' || p.wszPath[i+1] == 'G') &&
             p.wszPath[i+2] == '_')
            return 1;
    return 0;
}

static BOOL CALLBACK enum_cb(LPCDIDEVICEINSTANCEA inst, LPVOID ctx) {
    (void)ctx;
    if (!s_di) return DIENUM_STOP;
    IDirectInputDevice8A* dev = NULL;
    if (FAILED(IDirectInput8_CreateDevice(s_di, &inst->guidInstance, &dev, NULL)) || !dev)
        return DIENUM_CONTINUE;
    if (di_is_xinput_device(dev)) {     /* the XInput source already owns it */
        IDirectInputDevice8_Release(dev); return DIENUM_CONTINUE;
    }
    if (FAILED(IDirectInputDevice8_SetDataFormat(dev, &c_dfDIJoystick))) {
        IDirectInputDevice8_Release(dev); return DIENUM_CONTINUE;
    }
    IDirectInputDevice8_SetCooperativeLevel(dev, NULL,
                                            DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    IDirectInputDevice8_Acquire(dev);
    s_dev = dev;
    return DIENUM_STOP;   /* take the first real game controller */
}

static void di_try_init(void) {
    if (s_inited) return;
    s_inited = 1;
    HRESULT hr = DirectInput8Create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION,
                                    &IID_IDirectInput8A, (void**)&s_di, NULL);
    if (FAILED(hr) || !s_di) { s_di = NULL; return; }
    /* Note: this DirectInput8Create reaches the REAL dinput8 export, not our
     * synthetic factory -- dinput8_emu.c only patches the GAME's import slots, and
     * the hook DLL itself does not import dinput8 statically. We enumerate only
     * attached game controllers, so the synthetic in-game device (which is never a
     * real OS HID) is not enumerated here. */
    IDirectInput8_EnumDevices(s_di, DI8DEVCLASS_GAMECTRL, enum_cb, NULL,
                              DIEDFL_ATTACHEDONLY);
    s_present = (s_dev != NULL);
}

static int di_available(void) {
    di_try_init();
    return s_present;
}

static int read_state(DIJOYSTATE* js) {
    if (!s_dev) return 0;
    if (FAILED(IDirectInputDevice8_Poll(s_dev))) {
        if (FAILED(IDirectInputDevice8_Acquire(s_dev))) return 0;
        IDirectInputDevice8_Poll(s_dev);
    }
    if (FAILED(IDirectInputDevice8_GetDeviceState(s_dev, sizeof *js, js))) {
        IDirectInputDevice8_Acquire(s_dev);
        return 0;
    }
    return 1;
}

/* a DI axis value (0..65535) -> normalized for the intent.
 * bipolar axes (steer/stick): center 32767 -> 0, ends -> -1/+1.
 * unipolar axes (pedals): we treat the raw 0..65535 as 0..1 directly so a pedal
 *   that rests at 0 reads 0 (most wheels expose pedals as 0..65535 rising). */
static float di_axis_norm(LONG raw, int bipolar) {
    if (bipolar) return ((float)raw - 32767.0f) / 32767.0f;
    return (float)raw / 65535.0f;
}

static LONG di_field(const DIJOYSTATE* js, uint32_t id) {
    switch (id) {
        case UI_DI_X:  return js->lX;   case UI_DI_Y:  return js->lY;
        case UI_DI_Z:  return js->lZ;   case UI_DI_RX: return js->lRx;
        case UI_DI_RY: return js->lRy;  case UI_DI_RZ: return js->lRz;
        case UI_DI_SLIDER0: return js->rglSlider[0];
        case UI_DI_SLIDER1: return js->rglSlider[1];
        default: return 32767;
    }
}

static void di_poll(ultra_intent* out, const struct board_binding* bind) {
    if (!bind || !s_dev) return;
    DIJOYSTATE js;
    if (!read_state(&js)) return;

    for (int i = 0; i < bind->nbtns; ++i) {
        const ui_button_bind* bb = &bind->btns[i];
        if (bb->src != UI_SRC_DINPUT) continue;
        int idx = (int)bb->code;
        int pressed = (idx >= 0 && idx < 128) && (js.rgbButtons[idx] & 0x80);
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

    for (int i = 0; i < bind->naxes; ++i) {
        const ui_axis_bind* ab = &bind->axes[i];
        if (ab->src != UI_SRC_DINPUT) continue;
        int bipolar = (ab->intent == UI_AX_STEER || ab->intent == UI_AX_STICK_X ||
                       ab->intent == UI_AX_STICK_Y);
        float v = di_axis_norm(di_field(&js, ab->code), bipolar);
        /* a wheel/HOTAS source is "active" whenever it is off the rest position by
         * more than a small slop, so it overrides keyboard-synthesized axes. */
        float mag = v < 0 ? -v : v;
        if (bipolar ? (mag > 0.04f) : (v > 0.02f))
            ui_ax_drive(out, ab->player, (ui_axis)ab->intent, v);
    }
}

static const ultra_input_source g_src = {
    "dinput", di_available, di_poll, /*priority=*/30
};
const ultra_input_source* ultra_src_dinput_get(void) { return &g_src; }

#else
#include "input_sources.h"
const ultra_input_source* ultra_src_dinput_get(void) { return 0; }
#endif /* _WIN32 */
