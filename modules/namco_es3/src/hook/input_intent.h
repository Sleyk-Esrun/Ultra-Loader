/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  input_intent.h -- Ultra Loader native_x86 board-agnostic CONTROL-INTENT model.
 * =============================================================================
 *
 *  The native_x86 hook already has ONE normalized input struct (ultra_jvs_input in
 *  jvs_core.h) and ONE producer funnel (ultra_hook_current_input in hook_dllmain.c);
 *  every board I/O sink -- jvs_core, sega_io4, konami_io, taito_fastio, namco_io,
 *  dinput8_emu -- pulls from it. Today that producer picks between the loader shared
 *  block and a local gamepad struct that the autopilots write into.
 *
 *  This header defines the layer that sits IN FRONT of that producer: a richer,
 *  human-named CONTROL-INTENT model (semantic buttons + float axes), the SOURCE
 *  backend interface (keyboard / XInput / DirectInput8 reader / mouse / Ultra
 *  Ultra Omni-Track gun / autopilot), and the per-board PROFILE binding that maps a
 *  physical input to an intent. controls.c renders the intent into ultra_jvs_input
 *  once, in one place, so the sinks see exactly the struct they always have.
 *
 *  Float ranges are the CANONICAL contract: bipolar axes are -1..+1 (0 = center),
 *  unipolar axes are 0..1. A source never has to know the JVS 0x8000-center
 *  convention; the single sink render step (intent_to_jvs_input) does the
 *  float->uint16 conversion using the profile's per-axis calibration window.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the public Win32 input APIs + the open
 *  JVS switch map. No third-party loader source, profile, or config was read.
 * =============================================================================
 */
#ifndef ULTRA_INPUT_INTENT_H
#define ULTRA_INPUT_INTENT_H

#include <stdint.h>

#include "jvs_core.h"   /* ultra_jvs_input, JVS_MAX_PLAYERS, JVS_MAX_ANALOG */

#ifdef __cplusplus
extern "C" {
#endif

/* The intent model is sized to the same 2-player limit the JVS node + the loader
 * shared block (ULTRA_IO_PLAYERS) both use, so nothing is ever truncated. */
#define UI_MAX_PLAYERS  JVS_MAX_PLAYERS

/* ---- per-player DIGITAL intents (bit indices into a u32 per player) ---------
 * Named in human terms so a config file can bind "p1.push1 = key:0x20". The sink
 * render step maps each to the JVS switch-bit the wire layout expects. */
typedef enum {
    UI_BTN_START = 0,
    UI_BTN_SERVICE,
    UI_BTN_UP,
    UI_BTN_DOWN,
    UI_BTN_LEFT,
    UI_BTN_RIGHT,
    UI_BTN_PUSH1,
    UI_BTN_PUSH2,
    UI_BTN_PUSH3,
    UI_BTN_PUSH4,
    UI_BTN_PUSH5,
    UI_BTN_PUSH6,
    UI_BTN_PUSH7,
    UI_BTN_PUSH8,
    UI_BTN_PUSH9,         /* JVS word bit 0x0002 - the 15th switch. Mario Kart GP DX
                           * puts its MARIO button here; most cabinets stop at PUSH8 */
    UI_BTN_GUN_TRIGGER,   /* gun cabs: maps to PUSH1 wire bit (gun-bridge convention) */
    UI_BTN_GUN_RELOAD,    /* gun cabs: off-screen reload (maps to PUSH2 by default)    */
    UI_BTN__COUNT
} ui_button;

/* ---- per-player ANALOG intents (normalized float; see the range column) ------ */
typedef enum {
    UI_AX_STEER = 0,    /* -1..+1, 0 = center   (wheel)                 */
    UI_AX_ACCEL,        /*  0..1                (gas pedal)             */
    UI_AX_BRAKE,        /*  0..1                (brake pedal)           */
    UI_AX_STICK_X,      /* -1..+1, 0 = center   (flight/twin-stick X)   */
    UI_AX_STICK_Y,      /* -1..+1, 0 = center   (flight/twin-stick Y)   */
    UI_AX_GUN_X,        /*  0..1                (gun X over screen)     */
    UI_AX_GUN_Y,        /*  0..1                (gun Y over screen)     */
    UI_AX_AUX0,         /* spare (shifter/throttle)                     */
    UI_AX_AUX1,         /* spare                                        */
    UI_AX__COUNT
} ui_axis;

/* ---- system-level DIGITAL intents (not per-player) -------------------------- */
typedef enum {
    UI_SYS_TEST = 0,    /* JVS system bit7 */
    UI_SYS_TILT,        /* JVS system bit6 */
    UI_SYS__COUNT
} ui_sysbit;

/* ---- physical SOURCE kinds (which backend a binding draws from) ------------- */
typedef enum {
    UI_SRC_NONE = 0,
    UI_SRC_KEYBOARD,    /* GetAsyncKeyState VK            */
    UI_SRC_XINPUT,      /* XInput pad button mask / axis  */
    UI_SRC_DINPUT,      /* DirectInput8 device object     */
    UI_SRC_MOUSE,       /* mouse buttons / cursor         */
    UI_SRC_GUN,         /* Ultra Omni-Track lightgun pose */
    UI_SRC_AUTOPILOT,   /* the default-off test driver    */
    UI_SRC__COUNT
} ui_src_kind;

/* ---- axis shaping curve --------------------------------------------------- */
typedef enum { UI_CURVE_LINEAR = 0, UI_CURVE_EXPO = 1 } ui_curve;

/* =============================================================================
 *  THE CONTROL-INTENT struct -- one per frame. SOURCES fill it; the sink render
 *  step collapses it to ultra_jvs_input.
 * ============================================================================= */
typedef struct ultra_intent {
    uint32_t btn[UI_MAX_PLAYERS];               /* bit (1u<<ui_button) = pressed   */
    float    ax [UI_MAX_PLAYERS][UI_AX__COUNT]; /* normalized, per the range above */
    uint32_t sys;                               /* bit (1u<<ui_sysbit)             */
    uint32_t coin[UI_MAX_PLAYERS];              /* monotonic coin-insert counter   */
    /* which analog channels a SOURCE actually drove this frame (bit a = ui_axis a),
     * so the orchestrator can let a moved wheel override a synthesized key-steer
     * (last active writer wins for analog). */
    uint32_t ax_active[UI_MAX_PLAYERS];
} ultra_intent;

/* Reset an intent to the neutral pose (all buttons up, all axes centered/zero,
 * coin counters preserved by the caller as needed). */
void ui_intent_clear(ultra_intent* it);

/* convenience bit helpers (header-inline so sources stay terse) */
static inline void ui_btn_set(ultra_intent* it, int player, ui_button b) {
    if (player >= 0 && player < UI_MAX_PLAYERS) it->btn[player] |= (1u << (int)b);
}
static inline int ui_btn_get(const ultra_intent* it, int player, ui_button b) {
    return (player >= 0 && player < UI_MAX_PLAYERS) && (it->btn[player] & (1u << (int)b));
}
static inline void ui_sys_set(ultra_intent* it, ui_sysbit s) { it->sys |= (1u << (int)s); }

/* Mark an analog channel as actively driven this frame and store its value. */
static inline void ui_ax_drive(ultra_intent* it, int player, ui_axis ax, float v) {
    if (player < 0 || player >= UI_MAX_PLAYERS) return;
    it->ax[player][ax] = v;
    it->ax_active[player] |= (1u << (int)ax);
}

/* =============================================================================
 *  BINDINGS -- one physical input -> one intent. A profile is two arrays of these.
 * ============================================================================= */

/* digital: source `src`/`code` -> button `intent` on `player`. */
typedef struct ui_button_bind {
    uint8_t  player;     /* 0-based                                    */
    uint8_t  intent;     /* ui_button                                  */
    uint8_t  src;        /* ui_src_kind                                */
    uint8_t  _pad;
    uint32_t code;       /* VK, XInput button mask, DI button index,
                          *   mouse-button id (0=L,1=R,2=M)            */
} ui_button_bind;

/* analog: source `src`/`code` -> axis `intent` on `player`, landing in JVS
 * `jvs_channel`, scaled into [out_min,out_max] (the NVRAM calibration window). */
typedef struct ui_axis_bind {
    uint8_t  player;
    uint8_t  intent;       /* ui_axis                                  */
    uint8_t  src;          /* ui_src_kind                              */
    uint8_t  invert;       /* 1 = invert the normalized value          */
    int16_t  jvs_channel;  /* analog[] channel it renders into; -1 = screen_x/y */
    uint16_t out_min;      /* uint16 the axis floor maps to            */
    uint16_t out_max;      /* uint16 the axis ceil maps to             */
    uint32_t code;         /* DI axis offset / XInput axis id / key-pair packed */
    uint8_t  deadzone_pct; /* 0..100 dead zone around center/floor     */
    uint8_t  curve;        /* ui_curve                                 */
    uint16_t _pad;
} ui_axis_bind;

/* XInput axis ids used in ui_axis_bind.code when src==UI_SRC_XINPUT. */
enum {
    UI_XAX_LX = 1, UI_XAX_LY, UI_XAX_RX, UI_XAX_RY, UI_XAX_LT, UI_XAX_RT
};
/* A keyboard analog "axis" is a pair of VK codes packed lo=negative, hi=positive
 * (for a bipolar axis) or hi=0 (unipolar, code low byte is the press key). */
static inline uint32_t ui_keypair(uint16_t neg_vk, uint16_t pos_vk) {
    return ((uint32_t)pos_vk << 16) | (uint32_t)neg_vk;
}

/* gun axis selectors used in ui_axis_bind.code when src==UI_SRC_GUN. */
enum { UI_GUN_X = 1, UI_GUN_Y = 2 };
/* mouse axis selectors used in ui_axis_bind.code when src==UI_SRC_MOUSE. */
enum { UI_MAX_CURSOR_X = 1, UI_MAX_CURSOR_Y = 2 };
/* DirectInput8 axis selectors used in ui_axis_bind.code when src==UI_SRC_DINPUT. */
enum { UI_DI_X = 1, UI_DI_Y, UI_DI_Z, UI_DI_RX, UI_DI_RY, UI_DI_RZ,
       UI_DI_SLIDER0, UI_DI_SLIDER1 };

/* =============================================================================
 *  A BOARD PROFILE -- the per-board default binding set, keyed by the SAME
 *  system_id the board_profile table + the module SYSTEMS[] use.
 * ============================================================================= */
typedef struct board_binding {
    const char*           system_id;   /* "taito_typex2", ...                    */
    const char*           genre;       /* "fight","racer","shooter","gun","music"*/
    const ui_button_bind* btns;
    int                   nbtns;
    const ui_axis_bind*   axes;
    int                   naxes;
    uint8_t analog_channels;           /* feeds ULTRA_JVS_ANALOG / FEATCHK count  */
    uint8_t want_screenpos;            /* gun cab -> JVS screen-pos feature       */
} board_binding;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ULTRA_INPUT_INTENT_H */
