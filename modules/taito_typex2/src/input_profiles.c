/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  input_profiles.c -- per-board DEFAULT control profiles + INI config loader.
 * =============================================================================
 *
 *  Two responsibilities:
 *    1. board_profiles_resolve(system_id): return the compiled-in DEFAULT binding
 *       for a board, keyed by the SAME system_id the board_profile table + the
 *       module SYSTEMS[] use. Built from PUBLIC cabinet control-panel facts + the
 *       JVS switch map -- a fight profile, a racer profile, a gun profile, and a
 *       generic fall-through.
 *    2. board_profiles_load_config(base, system_id): merge a user INI on top of the
 *       compiled default. Resolution order (first found wins as the file):
 *         a) ULTRA_CONTROLS_CONFIG=<path>
 *         b) <game_dir>/ultra_controls.ini   (ULTRA_NATIVE_EXE / cwd)
 *         c) bundled  .../native_x86/profiles/<system_id>.ini
 *       A partial file is valid: unspecified bindings keep the compiled default.
 *
 *  The INI tokenizer is a ~self-contained clean-room parser (no external dep),
 *  matching the existing WR0.CFG / MUSEUM.ini ecosystem in spirit. Unknown keys are
 *  ignored (logged under ULTRA_JVS_LOG).
 *
 *  Bindings encode sys/coin pseudo-targets with sentinel players:
 *    player 0xFE = a system bit  (intent = ui_sysbit)
 *    player 0xFD = a coin slot    (intent = slot index)
 *  so the source backends route them correctly without a second array.
 * =============================================================================
 */
#include "controls.h"
#include "input_intent.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#if defined(_WIN32)
#  include <windows.h>
#endif

/* sentinel players for sys/coin bindings */
#define UI_PLAYER_SYS  0xFE
#define UI_PLAYER_COIN 0xFD

/* convenience binding-table macros */
#define BTN(pl, it, sr, cd)  { (pl), (uint8_t)(it), (uint8_t)(sr), 0, (uint32_t)(cd) }
#define AX(pl, it, sr, inv, ch, omin, omax, cd, dz, cv) \
    { (uint8_t)(pl), (uint8_t)(it), (uint8_t)(sr), (uint8_t)(inv), (int16_t)(ch), \
      (uint16_t)(omin), (uint16_t)(omax), (uint32_t)(cd), (uint8_t)(dz), (uint8_t)(cv), 0 }

/* ===========================================================================
 *  DEFAULT PROFILE: FIGHT (taito_typex / Chaos Breaker class).
 *  Player 1 on keyboard (arrows + ZXCV) and an XInput pad; coin/start/test mapped.
 * =========================================================================== */
static const ui_button_bind g_fight_btns[] = {
    /* keyboard P1 */
    BTN(0, UI_BTN_UP,     UI_SRC_KEYBOARD, VK_UP),
    BTN(0, UI_BTN_DOWN,   UI_SRC_KEYBOARD, VK_DOWN),
    BTN(0, UI_BTN_LEFT,   UI_SRC_KEYBOARD, VK_LEFT),
    BTN(0, UI_BTN_RIGHT,  UI_SRC_KEYBOARD, VK_RIGHT),
    BTN(0, UI_BTN_PUSH1,  UI_SRC_KEYBOARD, 'Z'),
    BTN(0, UI_BTN_PUSH2,  UI_SRC_KEYBOARD, 'X'),
    BTN(0, UI_BTN_PUSH3,  UI_SRC_KEYBOARD, 'C'),
    BTN(0, UI_BTN_PUSH4,  UI_SRC_KEYBOARD, 'V'),
    BTN(0, UI_BTN_START,  UI_SRC_KEYBOARD, '1'),
    BTN(0, UI_BTN_SERVICE,UI_SRC_KEYBOARD, '9'),
    BTN(UI_PLAYER_SYS,  UI_SYS_TEST, UI_SRC_KEYBOARD, VK_F2),
    BTN(UI_PLAYER_COIN, 0,           UI_SRC_KEYBOARD, '5'),
    /* XInput P1 */
    BTN(0, UI_BTN_UP,     UI_SRC_XINPUT, 0x0001),
    BTN(0, UI_BTN_DOWN,   UI_SRC_XINPUT, 0x0002),
    BTN(0, UI_BTN_LEFT,   UI_SRC_XINPUT, 0x0004),
    BTN(0, UI_BTN_RIGHT,  UI_SRC_XINPUT, 0x0008),
    BTN(0, UI_BTN_START,  UI_SRC_XINPUT, 0x0010),  /* START */
    BTN(0, UI_BTN_PUSH1,  UI_SRC_XINPUT, 0x1000),  /* A */
    BTN(0, UI_BTN_PUSH2,  UI_SRC_XINPUT, 0x2000),  /* B */
    BTN(0, UI_BTN_PUSH3,  UI_SRC_XINPUT, 0x4000),  /* X */
    BTN(0, UI_BTN_PUSH4,  UI_SRC_XINPUT, 0x8000),  /* Y */
    /* UNIVERSAL: TEST on L3, SERVICE on R3 - every board, every game.
     *
     * SERVICE was on LB, a button a thumb rests near. TEST was on BACK, which is
     * also the CREDIT button: since a JVS test/service switch adds a credit by
     * itself, one press of SELECT inserted a coin AND flipped the test switch,
     * which is where the double credit came from. */
    BTN(0, UI_BTN_SERVICE,UI_SRC_XINPUT, 0x0080),  /* R3 */
    BTN(UI_PLAYER_SYS,  UI_SYS_TEST, UI_SRC_XINPUT, 0x0040), /* L3 */
    BTN(UI_PLAYER_COIN, 0,           UI_SRC_XINPUT, 0x0040), /* L3 */
};
/* stick as an alternate d-pad: left stick -> STEER-like dirs are not used in a
 * fighter; we expose lx/ly as STICK axes for games that read analog dirs. */
static const ui_axis_bind g_fight_axes[] = {
    AX(0, UI_AX_STICK_X, UI_SRC_XINPUT, 0, 0, 0x0000, 0xFFFF, UI_XAX_LX, 18, UI_CURVE_LINEAR),
    AX(0, UI_AX_STICK_Y, UI_SRC_XINPUT, 1, 1, 0x0000, 0xFFFF, UI_XAX_LY, 18, UI_CURVE_LINEAR),
};

/* ===========================================================================
 *  DEFAULT PROFILE: RACER (taito_typex2 Wacky Race / Europa-R / RAW Thrills).
 *  Wheel/pedals from DirectInput, keyboard fallback, START/coin. The out_min/max
 *  windows are the proven NVRAM calibration ranges (WR0.CFG):
 *    STEER ch0 [4000,64000], ACCEL ch1 [26000,44000], BRAKE ch2 [28000,47000].
 * =========================================================================== */
static const ui_button_bind g_racer_btns[] = {
    BTN(0, UI_BTN_START,  UI_SRC_KEYBOARD, '1'),
    BTN(0, UI_BTN_PUSH1,  UI_SRC_KEYBOARD, VK_SPACE),   /* VIEW/horn */
    BTN(0, UI_BTN_SERVICE,UI_SRC_KEYBOARD, '9'),
    BTN(UI_PLAYER_SYS,  UI_SYS_TEST, UI_SRC_KEYBOARD, VK_F2),
    BTN(UI_PLAYER_COIN, 0,           UI_SRC_KEYBOARD, '5'),
    /* XInput fallbacks for the digital actions */
    BTN(0, UI_BTN_START,  UI_SRC_XINPUT, 0x0010),
    BTN(0, UI_BTN_PUSH1,  UI_SRC_XINPUT, 0x1000),       /* A = VIEW */
    BTN(UI_PLAYER_COIN, 0, UI_SRC_XINPUT, 0x0040),      /* L3 = coin */
    /* DI wheel buttons (shifters): paddle up/down -> PUSH2/PUSH3 */
    BTN(0, UI_BTN_PUSH2,  UI_SRC_DINPUT, 4),
    BTN(0, UI_BTN_PUSH3,  UI_SRC_DINPUT, 5),
};
static const ui_axis_bind g_racer_axes[] = {
    /* DirectInput wheel/pedals (primary) */
    AX(0, UI_AX_STEER, UI_SRC_DINPUT, 0, 0,  4000, 64000, UI_DI_X,  6, UI_CURVE_LINEAR),
    AX(0, UI_AX_ACCEL, UI_SRC_DINPUT, 0, 1, 26000, 44000, UI_DI_Y,  0, UI_CURVE_EXPO),
    AX(0, UI_AX_BRAKE, UI_SRC_DINPUT, 0, 2, 28000, 47000, UI_DI_RZ, 0, UI_CURVE_LINEAR),
    /* XInput stick/triggers (fallback) */
    AX(0, UI_AX_STEER, UI_SRC_XINPUT, 0, 0,  4000, 64000, UI_XAX_LX, 12, UI_CURVE_LINEAR),
    AX(0, UI_AX_ACCEL, UI_SRC_XINPUT, 0, 1, 26000, 44000, UI_XAX_RT, 0,  UI_CURVE_LINEAR),
    AX(0, UI_AX_BRAKE, UI_SRC_XINPUT, 0, 2, 28000, 47000, UI_XAX_LT, 0,  UI_CURVE_LINEAR),
    /* keyboard arrows as a last-resort steer/accel. code packs (hi=positive,
     * lo=negative) per ui_keypair() in input_intent.h. NOTE: this was
     * previously `code=0` ("set below", never actually set) -- input_src_keyboard.c
     * treats a 0 key-pair as neg=pos=0, GetAsyncKeyState(0) always reads not-down,
     * so the binding existed but silently never drove anything (the same bug
     * found + fixed in the SHARED native_x86 copy of this file for Sega Europa-R,
     * 2026-07-23 -- this is Type X2's own independent copy, unfixed until now). A
     * static const table can't call the inline ui_keypair() helper (not a constant
     * expression), so the pack is inlined here instead. */
    AX(0, UI_AX_STEER, UI_SRC_KEYBOARD, 0, 0, 4000, 64000,
       ((uint32_t)VK_RIGHT << 16) | (uint32_t)VK_LEFT, 0, UI_CURVE_LINEAR),
};

/* ===========================================================================
 *  DEFAULT PROFILE: GUN (Haunted Museum / Sega UX / Time Crisis class).
 *  Ultra Omni-Track gun primary; mouse fallback; trigger -> PUSH1; coin/start.
 *  GUN_X/Y render to JVS screen_x/y (jvs_channel = -1) for screen-pos titles.
 * =========================================================================== */
static const ui_button_bind g_gun_btns[] = {
    BTN(0, UI_BTN_GUN_TRIGGER, UI_SRC_GUN,   0),
    BTN(0, UI_BTN_GUN_RELOAD,  UI_SRC_GUN,   1),
    BTN(0, UI_BTN_GUN_TRIGGER, UI_SRC_MOUSE, 0),   /* left mouse = trigger fallback */
    BTN(0, UI_BTN_GUN_RELOAD,  UI_SRC_MOUSE, 1),   /* right mouse = reload          */
    BTN(0, UI_BTN_START,  UI_SRC_KEYBOARD, '1'),
    BTN(UI_PLAYER_COIN, 0, UI_SRC_KEYBOARD, '5'),
    BTN(UI_PLAYER_SYS, UI_SYS_TEST, UI_SRC_KEYBOARD, VK_F2),
};
static const ui_axis_bind g_gun_axes[] = {
    /* gun pose -> screen position (channel -1 = screen_x/y). Omni-Track primary. */
    AX(0, UI_AX_GUN_X, UI_SRC_GUN,   0, -1, 0, 65535, UI_GUN_X, 0, UI_CURVE_LINEAR),
    AX(0, UI_AX_GUN_Y, UI_SRC_GUN,   0, -1, 0, 65535, UI_GUN_Y, 0, UI_CURVE_LINEAR),
    /* mouse fallback over the device window */
    AX(0, UI_AX_GUN_X, UI_SRC_MOUSE, 0, -1, 0, 65535, UI_MAX_CURSOR_X, 0, UI_CURVE_LINEAR),
    AX(0, UI_AX_GUN_Y, UI_SRC_MOUSE, 0, -1, 0, 65535, UI_MAX_CURSOR_Y, 0, UI_CURVE_LINEAR),
};

/* ===========================================================================
 *  DEFAULT PROFILE: GENERIC (any board) -- keyboard + pad, no analog cab.
 * =========================================================================== */
static const ui_button_bind g_generic_btns[] = {
    BTN(0, UI_BTN_UP,     UI_SRC_KEYBOARD, VK_UP),
    BTN(0, UI_BTN_DOWN,   UI_SRC_KEYBOARD, VK_DOWN),
    BTN(0, UI_BTN_LEFT,   UI_SRC_KEYBOARD, VK_LEFT),
    BTN(0, UI_BTN_RIGHT,  UI_SRC_KEYBOARD, VK_RIGHT),
    BTN(0, UI_BTN_PUSH1,  UI_SRC_KEYBOARD, 'Z'),
    BTN(0, UI_BTN_PUSH2,  UI_SRC_KEYBOARD, 'X'),
    BTN(0, UI_BTN_START,  UI_SRC_KEYBOARD, '1'),
    BTN(0, UI_BTN_SERVICE,UI_SRC_KEYBOARD, '9'),
    BTN(UI_PLAYER_SYS,  UI_SYS_TEST, UI_SRC_KEYBOARD, VK_F2),
    BTN(UI_PLAYER_COIN, 0,           UI_SRC_KEYBOARD, '5'),
    BTN(0, UI_BTN_START,  UI_SRC_XINPUT, 0x0010),
    BTN(0, UI_BTN_PUSH1,  UI_SRC_XINPUT, 0x1000),
    BTN(0, UI_BTN_PUSH2,  UI_SRC_XINPUT, 0x2000),
    BTN(UI_PLAYER_COIN, 0, UI_SRC_XINPUT, 0x0040),
};

/* ---- the genre table + per-board mapping ---------------------------------- */
static const board_binding g_fight = {
    "taito_typex", "fight", g_fight_btns, (int)(sizeof g_fight_btns/sizeof g_fight_btns[0]),
    g_fight_axes, (int)(sizeof g_fight_axes/sizeof g_fight_axes[0]), 8, 0
};
static const board_binding g_racer = {
    "taito_typex2", "racer", g_racer_btns, (int)(sizeof g_racer_btns/sizeof g_racer_btns[0]),
    g_racer_axes, (int)(sizeof g_racer_axes/sizeof g_racer_axes[0]), 10, 0
};
static const board_binding g_gun = {
    "gun", "gun", g_gun_btns, (int)(sizeof g_gun_btns/sizeof g_gun_btns[0]),
    g_gun_axes, (int)(sizeof g_gun_axes/sizeof g_gun_axes[0]), 8, 1
};
static const board_binding g_generic = {
    "generic_pc", "generic", g_generic_btns, (int)(sizeof g_generic_btns/sizeof g_generic_btns[0]),
    NULL, 0, 8, 0
};

/* Map a board system_id to its default genre profile. A board that ships several
 * genres (e.g. Type X2 has both Wacky Race racing AND Haunted Museum gun) defaults
 * to the most-common cab; a per-game ultra_controls.ini overrides the genre. */
static const board_binding* default_for_board(const char* id) {
    if (!id) return &g_generic;
    /* fighters / generic Taito Type X */
    if (!strcmp(id, "taito_typex"))      return &g_fight;
    /* racing-predominant boards */
    if (!strcmp(id, "taito_typex2")  || !strcmp(id, "taito_typex3") ||
        !strcmp(id, "taito_typexzero")|| !strcmp(id, "sega_europar") ||
        !strcmp(id, "sega_ringedge") || !strcmp(id, "sega_ringwide") ||
        !strcmp(id, "rawthrills_pc") || !strcmp(id, "unico_pc"))
        return &g_racer;
    /* gun-cab predominant */
    if (!strcmp(id, "sega_alls"))        return &g_gun;
    return &g_generic;
}

/* the merged binding the config loader builds is held in a static so the returned
 * pointer stays valid for the process lifetime (single board per run). */
static board_binding     g_merged;
static ui_button_bind    g_merged_btns[96];
static ui_axis_bind      g_merged_axes[48];
static int               g_have_merged = 0;
static int               g_config_found = 0;   /* a real config FILE was loaded */

int board_profiles_config_found(void) { return g_config_found; }

const board_binding* board_profiles_resolve(const char* system_id) {
    return default_for_board(system_id);
}

/* ---- logging ------------------------------------------------------------- */
static void plog(const char* msg) {
#if defined(_WIN32)
    const char* p = getenv("ULTRA_JVS_LOG"); if (!p || !*p) return;
    FILE* f = fopen(p, "a"); if (!f) return; fprintf(f, "CONTROLS: %s\n", msg); fclose(f);
#else
    (void)msg;
#endif
}

/* =============================================================================
 *  INI tokenizer.
 *
 *  Sections: [meta] [buttons] [axes].
 *  [buttons]  p1.start = src:code [, src:code ...]
 *             sys.test = key:0x71 ; coin: p1.coin = key:0x35
 *  [axes]     p1.steer = src:code  jvs_ch  out_min out_max [inv] [dz%] [curve]
 * ============================================================================= */

static char* trim(char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char* e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}
static void lower(char* s) { for (; *s; ++s) if (*s >= 'A' && *s <= 'Z') *s += 32; }

/* parse a named intent button ("start","push1","gun_trigger","up"...) -> id, or -1 */
static int parse_btn_name(const char* n) {
    static const struct { const char* k; int v; } M[] = {
        {"start",UI_BTN_START},{"service",UI_BTN_SERVICE},{"up",UI_BTN_UP},
        {"down",UI_BTN_DOWN},{"left",UI_BTN_LEFT},{"right",UI_BTN_RIGHT},
        {"push1",UI_BTN_PUSH1},{"push2",UI_BTN_PUSH2},{"push3",UI_BTN_PUSH3},
        {"push4",UI_BTN_PUSH4},{"push5",UI_BTN_PUSH5},{"push6",UI_BTN_PUSH6},
        {"push7",UI_BTN_PUSH7},{"push8",UI_BTN_PUSH8},
        {"trigger",UI_BTN_GUN_TRIGGER},{"gun_trigger",UI_BTN_GUN_TRIGGER},
        {"reload",UI_BTN_GUN_RELOAD},{"gun_reload",UI_BTN_GUN_RELOAD},
    };
    for (size_t i = 0; i < sizeof M/sizeof M[0]; ++i) if (!strcmp(n, M[i].k)) return M[i].v;
    return -1;
}
static int parse_axis_name(const char* n) {
    static const struct { const char* k; int v; } M[] = {
        {"steer",UI_AX_STEER},{"accel",UI_AX_ACCEL},{"brake",UI_AX_BRAKE},
        {"stick_x",UI_AX_STICK_X},{"stick_y",UI_AX_STICK_Y},
        {"gun_x",UI_AX_GUN_X},{"gun_y",UI_AX_GUN_Y},
        {"aux0",UI_AX_AUX0},{"aux1",UI_AX_AUX1},
    };
    for (size_t i = 0; i < sizeof M/sizeof M[0]; ++i) if (!strcmp(n, M[i].k)) return M[i].v;
    return -1;
}
static int parse_sys_name(const char* n) {
    if (!strcmp(n, "test")) return UI_SYS_TEST;
    if (!strcmp(n, "tilt")) return UI_SYS_TILT;
    return -1;
}
/* "key:0x31" / "xinput:A" / "dinput:axisX" / "dinput:btn4" / "mouse:left" /
 * "gun:trigger" / "gun:x". Returns src kind, fills code; -1 on parse error. */
static int parse_src_code(const char* tok, int is_axis, uint32_t* code) {
    char buf[64]; strncpy(buf, tok, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    char* colon = strchr(buf, ':');
    if (!colon) return -1;
    *colon = 0;
    char* src = trim(buf); lower(src);
    char* val = trim(colon + 1); lower(val);

    if (!strcmp(src, "key")) {
        *code = (uint32_t)strtoul(val, NULL, 0);
        return UI_SRC_KEYBOARD;
    }
    if (!strcmp(src, "xinput") || !strcmp(src, "pad")) {
        if (is_axis) {
            if (!strcmp(val,"lx")) *code = UI_XAX_LX; else if (!strcmp(val,"ly")) *code = UI_XAX_LY;
            else if (!strcmp(val,"rx")) *code = UI_XAX_RX; else if (!strcmp(val,"ry")) *code = UI_XAX_RY;
            else if (!strcmp(val,"lt")) *code = UI_XAX_LT; else if (!strcmp(val,"rt")) *code = UI_XAX_RT;
            else return -1;
        } else {
            static const struct { const char* k; uint32_t m; } B[] = {
                {"up",0x0001},{"down",0x0002},{"left",0x0004},{"right",0x0008},
                {"start",0x0010},{"back",0x0020},{"l3",0x0040},{"r3",0x0080},
                {"lb",0x0100},{"rb",0x0200},{"a",0x1000},{"b",0x2000},{"x",0x4000},{"y",0x8000},
            };
            uint32_t m = 0;
            for (size_t i=0;i<sizeof B/sizeof B[0];++i) if (!strcmp(val,B[i].k)) { m=B[i].m; break; }
            if (!m) m = (uint32_t)strtoul(val, NULL, 0);   /* raw mask allowed */
            *code = m;
        }
        return UI_SRC_XINPUT;
    }
    if (!strcmp(src, "dinput") || !strcmp(src, "di")) {
        if (is_axis) {
            if (!strcmp(val,"axisx")||!strcmp(val,"x")) *code = UI_DI_X;
            else if (!strcmp(val,"axisy")||!strcmp(val,"y")) *code = UI_DI_Y;
            else if (!strcmp(val,"axisz")||!strcmp(val,"z")) *code = UI_DI_Z;
            else if (!strcmp(val,"axisrx")||!strcmp(val,"rx")) *code = UI_DI_RX;
            else if (!strcmp(val,"axisry")||!strcmp(val,"ry")) *code = UI_DI_RY;
            else if (!strcmp(val,"axisrz")||!strcmp(val,"rz")) *code = UI_DI_RZ;
            else if (!strcmp(val,"slider0")) *code = UI_DI_SLIDER0;
            else if (!strcmp(val,"slider1")) *code = UI_DI_SLIDER1;
            else return -1;
        } else {
            /* "btn4" or a raw index */
            const char* p = val; if (!strncmp(p,"btn",3)) p += 3;
            *code = (uint32_t)strtoul(p, NULL, 0);
        }
        return UI_SRC_DINPUT;
    }
    if (!strcmp(src, "mouse")) {
        if (is_axis) { *code = (!strcmp(val,"y")) ? UI_MAX_CURSOR_Y : UI_MAX_CURSOR_X; }
        else { *code = (!strcmp(val,"right")||!strcmp(val,"r")) ? 1u
                      : (!strcmp(val,"middle")||!strcmp(val,"m")) ? 2u : 0u; }
        return UI_SRC_MOUSE;
    }
    if (!strcmp(src, "gun")) {
        if (is_axis) { *code = (!strcmp(val,"y")) ? UI_GUN_Y : UI_GUN_X; }
        else { *code = (!strcmp(val,"reload")) ? 1u : 0u; }
        return UI_SRC_GUN;
    }
    return -1;
}

/* parse "p1.start" / "sys.test" / "p1.coin" -> (player, intent-or-slot, is_axis).
 * Returns 1 button, 2 axis, 3 sys, 4 coin, 0 on error. */
static int parse_target(const char* key, int* player, int* intent) {
    char buf[64]; strncpy(buf, key, sizeof buf - 1); buf[sizeof buf - 1] = 0; lower(buf);
    char* dot = strchr(buf, '.');
    if (!dot) return 0;
    *dot = 0;
    char* who = buf; char* what = dot + 1;
    if (!strcmp(who, "sys")) {
        int s = parse_sys_name(what); if (s < 0) return 0;
        *player = UI_PLAYER_SYS; *intent = s; return 3;
    }
    int p = 0;
    if (who[0] == 'p' && isdigit((unsigned char)who[1])) p = who[1] - '1';
    if (p < 0 || p >= UI_MAX_PLAYERS) return 0;
    if (!strcmp(what, "coin")) { *player = UI_PLAYER_COIN; *intent = p; return 4; }
    int b = parse_btn_name(what);
    if (b >= 0) { *player = p; *intent = b; return 1; }
    int a = parse_axis_name(what);
    if (a >= 0) { *player = p; *intent = a; return 2; }
    return 0;
}

/* find/replace an existing button binding for (player,intent,src) or append. */
static void upsert_btn(int player, int intent, int src, uint32_t code) {
    if (g_merged.nbtns >= (int)(sizeof g_merged_btns/sizeof g_merged_btns[0])) return;
    for (int i = 0; i < g_merged.nbtns; ++i) {
        ui_button_bind* b = &g_merged_btns[i];
        if (b->player == player && b->intent == intent && b->src == src) {
            b->code = code; return;
        }
    }
    ui_button_bind nb = BTN(player, intent, src, code);
    g_merged_btns[g_merged.nbtns++] = nb;
}
static void upsert_axis(int player, int intent, int src, uint32_t code,
                        int jvs_ch, int omin, int omax, int inv, int dz, int curve) {
    if (g_merged.naxes >= (int)(sizeof g_merged_axes/sizeof g_merged_axes[0])) return;
    for (int i = 0; i < g_merged.naxes; ++i) {
        ui_axis_bind* a = &g_merged_axes[i];
        if (a->player == player && a->intent == intent && a->src == src) {
            a->code = code; a->jvs_channel = (int16_t)jvs_ch;
            a->out_min = (uint16_t)omin; a->out_max = (uint16_t)omax;
            a->invert = (uint8_t)inv; a->deadzone_pct = (uint8_t)dz; a->curve = (uint8_t)curve;
            return;
        }
    }
    ui_axis_bind na = AX(player, intent, src, inv, jvs_ch, omin, omax, code, dz, curve);
    g_merged_axes[g_merged.naxes++] = na;
}

/* apply one [buttons] line: key = src:code[, src:code...] */
static void apply_button_line(const char* key, char* value) {
    int player, intent, kind = parse_target(key, &player, &intent);
    if (kind != 1 && kind != 3 && kind != 4) { plog("ignoring non-button key in [buttons]"); return; }
    char* save = NULL;
    for (char* tok = strtok_r(value, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        tok = trim(tok); if (!*tok) continue;
        uint32_t code; int src = parse_src_code(tok, /*is_axis=*/0, &code);
        if (src < 0) { plog("bad button source token"); continue; }
        upsert_btn(player, intent, src, code);
    }
}

/* apply one [axes] line: key = src:code  jvs_ch out_min out_max [inv dz curve] */
static void apply_axis_line(const char* key, char* value) {
    int player, intent, kind = parse_target(key, &player, &intent);
    if (kind != 2) { plog("ignoring non-axis key in [axes]"); return; }
    /* split off the first whitespace-delimited token = src:code, rest = numbers. */
    char vbuf[160]; strncpy(vbuf, value, sizeof vbuf - 1); vbuf[sizeof vbuf - 1] = 0;
    char* save = NULL;
    char* srctok = strtok_r(vbuf, " \t", &save);
    if (!srctok) return;
    uint32_t code; int src = parse_src_code(srctok, /*is_axis=*/1, &code);
    if (src < 0) { plog("bad axis source token"); return; }
    /* numbers: jvs_ch out_min out_max [inv dz curve]; "screen" jvs_ch = -1. */
    char* a_ch  = strtok_r(NULL, " \t", &save);
    char* a_min = strtok_r(NULL, " \t", &save);
    char* a_max = strtok_r(NULL, " \t", &save);
    char* a_inv = strtok_r(NULL, " \t", &save);
    char* a_dz  = strtok_r(NULL, " \t", &save);
    char* a_cv  = strtok_r(NULL, " \t", &save);
    int jvs_ch = 0;
    if (a_ch) { char low[16]; strncpy(low,a_ch,sizeof low-1); low[sizeof low-1]=0; lower(low);
                jvs_ch = (!strcmp(low,"screen")) ? -1 : (int)strtol(a_ch,NULL,0); }
    int omin = a_min ? (int)strtol(a_min,NULL,0) : 0x8000;
    int omax = a_max ? (int)strtol(a_max,NULL,0) : 0x8000;
    int inv  = a_inv ? (int)strtol(a_inv,NULL,0) : 0;
    int dz   = a_dz  ? (int)strtol(a_dz,NULL,0)  : 0;
    int cv   = UI_CURVE_LINEAR;
    if (a_cv) { char low[16]; strncpy(low,a_cv,sizeof low-1); low[sizeof low-1]=0; lower(low);
                cv = (!strcmp(low,"expo")||!strcmp(low,"1")) ? UI_CURVE_EXPO : UI_CURVE_LINEAR; }
    upsert_axis(player, intent, src, code, jvs_ch, omin, omax, inv, dz, cv);
}

/* parse an opened INI stream into the merged tables (pre-seeded from base). */
static int parse_ini(FILE* f) {
    char line[256];
    int section = 0;   /* 0 none, 1 buttons, 2 axes, 3 meta */
    int applied = 0;
    while (fgets(line, sizeof line, f)) {
        /* strip comments (; or #) */
        char* c = line; for (; *c; ++c) { if (*c == ';' || *c == '#') { *c = 0; break; } }
        char* s = trim(line);
        if (!*s) continue;
        if (*s == '[') {
            char* end = strchr(s, ']'); if (end) *end = 0;
            char* name = trim(s + 1); lower(name);
            if      (!strcmp(name, "buttons")) section = 1;
            else if (!strcmp(name, "axes"))    section = 2;
            else if (!strcmp(name, "meta"))    section = 3;
            else section = 0;
            continue;
        }
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = trim(s); char* val = trim(eq + 1);
        if (section == 1) { apply_button_line(key, val); applied = 1; }
        else if (section == 2) { apply_axis_line(key, val); applied = 1; }
        else if (section == 3) {
            char low[64]; strncpy(low,key,sizeof low-1); low[sizeof low-1]=0; lower(low);
            if (!strcmp(low, "analog_channels")) g_merged.analog_channels = (uint8_t)atoi(val);
            else if (!strcmp(low, "screenpos")) g_merged.want_screenpos = (uint8_t)atoi(val);
            else if (!strcmp(low, "genre")) g_merged.genre = "config";
        }
    }
    return applied;
}

/* seed the merged tables from a base binding so a partial file keeps defaults. */
static void seed_from_base(const board_binding* base) {
    memset(&g_merged, 0, sizeof g_merged);
    g_merged.system_id = base ? base->system_id : "config";
    g_merged.genre     = base ? base->genre : "config";
    g_merged.analog_channels = base ? base->analog_channels : 8;
    g_merged.want_screenpos  = base ? base->want_screenpos : 0;
    g_merged.nbtns = 0; g_merged.naxes = 0;
    if (base) {
        for (int i = 0; i < base->nbtns && g_merged.nbtns < (int)(sizeof g_merged_btns/sizeof g_merged_btns[0]); ++i)
            g_merged_btns[g_merged.nbtns++] = base->btns[i];
        for (int i = 0; i < base->naxes && g_merged.naxes < (int)(sizeof g_merged_axes/sizeof g_merged_axes[0]); ++i)
            g_merged_axes[g_merged.naxes++] = base->axes[i];
    }
    g_merged.btns = g_merged_btns;
    g_merged.axes = g_merged_axes;
}

#if defined(_WIN32)
/* resolve the bundled default INI path: <dir of this DLL>/profiles/<id>.ini. */
static int bundled_ini_path(const char* system_id, char* out, int cap) {
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void*)&bundled_ini_path, &self);
    char dir[MAX_PATH]; DWORD n = GetModuleFileNameA(self, dir, sizeof dir);
    if (n == 0 || n >= sizeof dir) return 0;
    char* slash = strrchr(dir, '\\'); if (!slash) slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    _snprintf(out, cap, "%s\\profiles\\%s.ini", dir, system_id);
    out[cap-1] = 0;
    return 1;
}
/* <game_dir>/ultra_controls.ini from ULTRA_NATIVE_EXE (or cwd). */
static int game_ini_path(char* out, int cap) {
    char exe[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableA("ULTRA_NATIVE_EXE", exe, sizeof exe);
    if (n > 0 && n < sizeof exe) {
        char* slash = strrchr(exe, '\\'); if (!slash) slash = strrchr(exe, '/');
        if (slash) { *slash = 0; _snprintf(out, cap, "%s\\ultra_controls.ini", exe); out[cap-1]=0; return 1; }
    }
    _snprintf(out, cap, "ultra_controls.ini"); out[cap-1] = 0;
    return 1;
}
#endif

const board_binding* board_profiles_load_config(const board_binding* base,
                                                const char* system_id) {
    /* always seed from the compiled default so a missing/partial file keeps it. */
    seed_from_base(base);
    g_config_found = 0;

    const char* path = NULL;
    /* `is_optin`: an explicit ULTRA_CONTROLS_CONFIG path or a per-game
     * ultra_controls.ini is a deliberate user opt-in -> it ACTIVATES the layer. A
     * BUNDLED .../profiles/<id>.ini is shipped documentation/template: it refines
     * the bindings when the user opts in some OTHER way (ULTRA_CONTROLS=1), but does
     * NOT by itself flip the layer on -- otherwise the playable Type X2 games would
     * silently switch off their proven autopilot/native path. */
    int is_optin = 0;
#if defined(_WIN32)
    char p_explicit[MAX_PATH] = {0}, p_game[MAX_PATH] = {0}, p_bundled[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableA("ULTRA_CONTROLS_CONFIG", p_explicit, sizeof p_explicit);
    FILE* f = NULL;
    if (n > 0 && n < sizeof p_explicit) { f = fopen(p_explicit, "r"); if (f) { path = p_explicit; is_optin = 1; } }
    if (!f && game_ini_path(p_game, sizeof p_game))   { f = fopen(p_game, "r");   if (f) { path = p_game; is_optin = 1; } }
    if (!f && bundled_ini_path(system_id, p_bundled, sizeof p_bundled)) { f = fopen(p_bundled, "r"); if (f) { path = p_bundled; is_optin = 0; } }
#else
    FILE* f = NULL;
    const char* env = getenv("ULTRA_CONTROLS_CONFIG");
    if (env && *env) { f = fopen(env, "r"); if (f) { path = env; is_optin = 1; } }
#endif
    if (!f) {
        g_have_merged = 1;   /* merged == compiled default */
        return &g_merged;
    }
    int applied = parse_ini(f);
    fclose(f);
    g_config_found = is_optin;   /* only an explicit/per-game file auto-activates */
    if (path) { char m[300]; _snprintf(m, sizeof m, "loaded config '%s' (applied=%d optin=%d)", path, applied, is_optin); m[sizeof m-1]=0; plog(m); }
    g_have_merged = 1;
    return &g_merged;
}

/* table accessors for the unit test (the genre defaults). */
const board_binding* board_binding_at(int index) {
    static const board_binding* T[] = { &g_fight, &g_racer, &g_gun, &g_generic };
    if (index < 0 || index >= (int)(sizeof T/sizeof T[0])) return 0;
    return T[index];
}
int board_binding_count(void) { return 4; }
