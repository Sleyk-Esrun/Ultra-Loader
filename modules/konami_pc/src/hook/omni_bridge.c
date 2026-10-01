/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  omni_bridge.c -- Ultra Omni-Bridge reference implementation
 * =============================================================================
 *
 *  Generalized from taito_typex2's own typex2_gun.c gamepad+lightgun bridge
 *  (proven live 2026-08-02 on Haunted Museum: mouse aim, left-click=P1,
 *  right-click=P2, left stick aims, A/RT fires, START/BACK -> game keys).
 *  This file makes that pattern reusable across boards via two caller-
 *  supplied accessors (ultra_bridge_window_fn / ultra_bridge_pose_fn) instead
 *  of reaching into one board's specific hook globals, and reads its tuning
 *  from the shared omni_client.h config (omni_client_read_bridge()) instead
 *  of ad-hoc per-board env vars.
 *
 *  Per this project's per-board independence rule this file is a REFERENCE --
 *  copy it into each board's own hook source tree (adapting only the
 *  #include path to omni_client.h if that board's include dirs differ), do
 *  not link against a shared compiled copy.
 *
 * -----------------------------------------------------------------------------
 *  2026-09-18 ADDITION: THE GENERAL PAD -> KEYBOARD MAP  (opt-in, per game)
 * -----------------------------------------------------------------------------
 *
 *  WHY. Some games create our synthetic DirectInput devices and then never read
 *  them, so every pad profile written for them is invisible. Contra Evolution HD
 *  is one: it creates three devices per DirectInput8Create and has no located
 *  consumer for the joystick, but it ships a COMPLETE native keyboard map
 *  (AMContra.exe file 0x15B6A8, 37 entries of {DIK scancode, action id} --
 *  decoded in docs/CONTRA_KEYBOARD_MAP.md). For such a game the shortest real
 *  path to the game's own action ids is to press the game's own keys.
 *
 *  SendInput is the right instrument for that: it drives the real system input
 *  queue, so the keystroke is seen by GetAsyncKeyState (which is how our own
 *  dinput8_emu.c fills the synthetic DI8 keyboard buffer -- dinput8_emu.c:102),
 *  by WM_KEYDOWN, and by a real DirectInput keyboard alike.
 *
 *  THIS IS OFF UNLESS A CONTAINER TURNS IT ON. With ULTRA_BRIDGE_KEYS and
 *  ULTRA_BRIDGE_KEYS2 unset or empty, not one statement below runs and this
 *  file behaves exactly as it did before -- same five buttons, same lightgun
 *  path, same log lines. Silent Hill: The Arcade, which shares this module and
 *  is a locked playable title, sets neither key (and in fact never reaches this
 *  code at all: it runs with ULTRA_BRIDGE_ENABLE=0, so install() returns at the
 *  g_cfg.enabled gate before any thread is created).
 *
 *  CONFIG (all read from the game's environment, i.e. a .ufw container's [env])
 *
 *    ULTRA_BRIDGE_KEYS    pad 0's map.  Empty/unset = feature off for pad 0.
 *    ULTRA_BRIDGE_KEYS2   pad 1's map.  Empty/unset = feature off for pad 1.
 *
 *        Format: a list of  control:key  pairs, separated by commas,
 *        semicolons or spaces. Case-insensitive. Example:
 *
 *            ULTRA_BRIDGE_KEYS = up:W, down:S, left:A, right:D, a:J, b:K,
 *                                x:U, y:I, start:RETURN, back:SPACE
 *
 *        CONTROLS
 *          d-pad ........ up down left right
 *          face ......... a b x y
 *          shoulders .... lb rb
 *          triggers ..... lt rt          (analog, past ULTRA_BRIDGE_KEYS_TRIG)
 *          sticks ....... l3 r3          (the click)
 *          left stick ... sup sdown sleft sright     (as a d-pad)
 *          right stick .. rsup rsdown rsleft rsright (as a d-pad)
 *          aliases ...... select = back, menu = start
 *
 *        KEYS
 *          a single character ........ W  A  S  D  J  K  U  I  7
 *          a name .................... RETURN ENTER SPACE ESC TAB BACKSPACE
 *                                      SHIFT CTRL ALT
 *                                      UP DOWN LEFT RIGHT
 *                                      INSERT DELETE HOME END PGUP PGDN
 *                                      F1 .. F12
 *                                      NUM0 .. NUM9 NUMPLUS NUMMINUS
 *                                      NUMMUL NUMDIV NUMDOT
 *          a raw virtual key ......... 0x0D   13
 *          NONE ...................... parsed and ignored (documents a gap)
 *
 *        NOT SUPPORTED: numpad ENTER. It is VK_RETURN with the extended bit,
 *        indistinguishable here from the main RETURN, so a game whose P2 START
 *        is numpad ENTER (Contra is one) cannot have that one action bound from
 *        a pad. Everything else in Contra's P2 column (arrows, NUM1/NUM2,
 *        NUMPLUS/NUMMINUS) is reachable.
 *
 *    ULTRA_BRIDGE_KEYS_STICK   stick-as-d-pad threshold, 0.0..1.0  (default .50)
 *    ULTRA_BRIDGE_KEYS_TRIG    trigger threshold, 0..255           (default 128)
 *    ULTRA_BRIDGE_KEYS_FOCUS   1 = only type while the game's own process owns
 *                              the foreground window (default 1). Leave this on:
 *                              at 0 a resting pad can type W A S D into whatever
 *                              application the user alt-tabbed to.
 *    ULTRA_BRIDGE_KEYS_MOUSE   1 = ALSO keep the legacy A/RT -> mouse click on a
 *                              pad that has a map (default 0). A game that reads
 *                              the keyboard normally has no mouse role, and a
 *                              stray click into its window can steal focus or
 *                              hit a UI element.
 *    ULTRA_BRIDGE_KEYS_PAUSEGUARD
 *                              1 = while START and BACK are held TOGETHER (the
 *                              pause overlay's own combo), hold off whatever
 *                              those two are bound to (default 1). Without it,
 *                              opening the overlay also posts both keys into the
 *                              game -- and where BACK is the SERVICE key that is
 *                              a free credit every time.
 *
 *  SEMANTICS. Strictly edge-triggered: one key-down when the control goes down,
 *  one key-up when it comes back up, nothing in between -- so a held direction
 *  is a held key, which is what a run-and-gun needs, and there is no auto-repeat
 *  storm. Several controls may share one key (binding both the d-pad and the
 *  left stick to W is the normal case); a reference count per virtual key means
 *  the key is only released when the LAST control holding it lets go.
 *
 *  Each press emits ONE INPUT carrying both wVk and wScan (plus the extended
 *  flag where the key needs it) rather than the two separate events key_tap()
 *  sends. That is the canonical form and it avoids a real bug in the two-event
 *  shape: a bare-scancode event for an arrow key carries the numpad scancode
 *  without the extended bit, so KEYEVENTF_SCANCODE alone would deliver
 *  VK_LEFT as VK_NUMPAD4.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "omni_bridge.h"
#include "omni_client.h"

static void blog(const char* fmt, ...) {
    const char* p = getenv("ULTRA_JVS_LOG");
    if (!p || !*p) return;
    FILE* f = fopen(p, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

static volatile LONG g_stop = 0;
static volatile LONG g_km_drained = 0;  /* poll thread released its own keys */
static ultra_bridge_window_fn g_get_window = NULL;
static ultra_bridge_pose_fn   g_get_pose   = NULL;
static omni_client_bridge_t   g_cfg;

/* ---- low-level input feeders (SendInput drives the real input queue, so it
 *      reaches GetAsyncKeyState / WM_KEYDOWN / a raw-input reader alike) --- */
static void feed_cursor_px(HWND w, int px, int py) {
    if (!w) return;
    POINT pt = { px, py };
    ClientToScreen(w, &pt);
    /* Never warp the pointer to a coordinate that is not on any monitor.
     *
     * ClientToScreen maps through the game window's CURRENT position, so if
     * that window is off-screen -- minimized to a phantom position, on another
     * desktop, moved by a test harness, or restored from a stale multi-monitor
     * layout -- every point maps somewhere the user cannot see. This function
     * runs ~125x/second, so the effect is not a glitch: the real pointer is
     * dragged off the desktop and pinned there, and the machine looks frozen.
     * (Observed 2026-09-10: a headless test parked the window at -32000,-32000
     * and the user's mouse was held in the top-left corner until they killed
     * the game from Task Manager.) A warp outside the virtual desktop can never
     * be what the caller wanted, so drop it and leave the pointer alone. */
    {
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (vw > 0 && vh > 0 &&
            (pt.x < vx || pt.y < vy || pt.x >= vx + vw || pt.y >= vy + vh))
            return;
    }
    SetCursorPos(pt.x, pt.y);
}

/* Independent left/right button state so a two-player raw-mouse game (P1 =
 * left click, P2 = right click) never gets a torn/stuck button from two
 * threads racing on one shared flag. */
static volatile LONG g_mouse_down_l = 0;
static volatile LONG g_mouse_down_r = 0;
static void mouse_emit(int down, int right) {
    INPUT in; ZeroMemory(&in, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = right ? (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP)
                          : (down ? MOUSEEVENTF_LEFTDOWN  : MOUSEEVENTF_LEFTUP);
    SendInput(1, &in, sizeof in);
}
static void mouse_set(int down, int right) {
    down = down ? 1 : 0;
    volatile LONG* state = right ? &g_mouse_down_r : &g_mouse_down_l;
    if (InterlockedExchange(state, down) != down) mouse_emit(down, right);
}
static void mouse_force_release(void) {
    if (InterlockedExchange(&g_mouse_down_l, 0) != 0) mouse_emit(0, 0);
    if (InterlockedExchange(&g_mouse_down_r, 0) != 0) mouse_emit(0, 1);
}

static void key_tap(int vk, DWORD hold_ms) {
    if (vk <= 0) return;
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    INPUT in[2];
    ZeroMemory(in, sizeof in);
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = (WORD)vk;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = 0; in[1].ki.wScan = (WORD)sc;
    in[1].ki.dwFlags = KEYEVENTF_SCANCODE;
    SendInput(2, in, sizeof(INPUT));
    Sleep(hold_ms ? hold_ms : 40);
    ZeroMemory(in, sizeof in);
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = (WORD)vk; in[0].ki.dwFlags = KEYEVENTF_KEYUP;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = 0; in[1].ki.wScan = (WORD)sc;
    in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

/* ---- lightgun pose -> cursor (P1 slot only; a live pose always wins over
 *      the gamepad, see gamepad_feed_one()) --------------------------------- */
static DWORD WINAPI gun_thread(LPVOID arg) {
    (void)arg;
    blog("OMNI-BRIDGE: lightgun pose bridge active (pose -> cursor, P1 trigger)");
    while (!g_stop) {
        HWND w = g_get_window ? g_get_window() : NULL;
        if (w && g_get_pose) {
            int x, y, t;
            if (g_get_pose(&x, &y, &t)) {
                RECT rc;
                if (GetClientRect(w, &rc)) {
                    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
                    if (cw > 0 && ch > 0) {
                        int px = (int)((long)x * cw / 65535); if (px > cw - 1) px = cw - 1; if (px < 0) px = 0;
                        int py = (int)((long)y * ch / 65535); if (py > ch - 1) py = ch - 1; if (py < 0) py = 0;
                        feed_cursor_px(w, px, py);
                        mouse_set(t, 0);   /* lightgun always drives P1 (left click) */
                    }
                }
            }
        }
        Sleep(8);
    }
    return 0;
}

/* ---- XInput gamepad -> cursor + buttons + keys ---------------------------- */
typedef BOOL (WINAPI *pfnXInputGetState)(DWORD, void*);
static pfnXInputGetState s_xig;

/* Local mirror of XINPUT_STATE/XINPUT_GAMEPAD so this file needs no -lxinput
 * import library (same trick used elsewhere in this project's hooks). */
#pragma pack(push, 1)
typedef struct { WORD wButtons; BYTE bLeftTrigger; BYTE bRightTrigger;
                 SHORT sThumbLX; SHORT sThumbLY; SHORT sThumbRX; SHORT sThumbRY; } ul_bridge_gamepad;
typedef struct { DWORD dwPacketNumber; ul_bridge_gamepad Gamepad; } ul_bridge_xstate;
#pragma pack(pop)
#define UXB_START 0x0010
#define UXB_BACK  0x0020
#define UXB_A     0x1000
#define UXB_LB    0x0100
#define UXB_RB    0x0200

typedef struct { int accum_x, accum_y; int start_prev, coin_prev, svc_prev, test_prev; } pad_slot_state;
static pad_slot_state g_pad[2];

/* ===========================================================================
 *  PAD -> KEYBOARD MAP  (see the file header for the whole contract)
 *
 *  EVERYTHING IN THIS SECTION IS INERT UNLESS A CONTAINER SETS
 *  ULTRA_BRIDGE_KEYS / ULTRA_BRIDGE_KEYS2. Both default to unset, which leaves
 *  g_km[idx].n == 0, which is the only condition any of it is entered under.
 * =========================================================================== */

enum {
    KC_UP = 0, KC_DOWN, KC_LEFT, KC_RIGHT, KC_START, KC_BACK,
    KC_L3, KC_R3, KC_LB, KC_RB, KC_A, KC_B, KC_X, KC_Y,   /* .. KC_Y = last wButtons bit */
    KC_LT, KC_RT,
    KC_SUP, KC_SDOWN, KC_SLEFT, KC_SRIGHT,
    KC_RSUP, KC_RSDOWN, KC_RSLEFT, KC_RSRIGHT,
    KC__COUNT
};

/* name -> control. The first 14 carry the XINPUT_GAMEPAD bit they test; the
 * rest are derived (analog triggers, sticks used as a d-pad) and carry 0. */
static const struct { const char* name; WORD mask; } g_ctl[KC__COUNT] = {
    { "up",     0x0001 }, { "down",   0x0002 }, { "left",  0x0004 }, { "right", 0x0008 },
    { "start",  0x0010 }, { "back",   0x0020 }, { "l3",    0x0040 }, { "r3",    0x0080 },
    { "lb",     0x0100 }, { "rb",     0x0200 }, { "a",     0x1000 }, { "b",     0x2000 },
    { "x",      0x4000 }, { "y",      0x8000 },
    { "lt",     0 },      { "rt",     0 },
    { "sup",    0 },      { "sdown",  0 },      { "sleft",  0 },     { "sright",  0 },
    { "rsup",   0 },      { "rsdown", 0 },      { "rsleft", 0 },     { "rsright", 0 }
};
static const struct { const char* name; int id; } g_ctl_alias[] = {
    { "select", KC_BACK }, { "menu", KC_START }, { NULL, 0 }
};

static const struct { const char* name; int vk; } g_keyname[] = {
    { "return", 0x0D }, { "enter", 0x0D }, { "space", 0x20 },
    { "escape", 0x1B }, { "esc", 0x1B },   { "tab", 0x09 },
    { "backspace", 0x08 }, { "shift", 0x10 }, { "ctrl", 0x11 },
    { "control", 0x11 }, { "alt", 0x12 },
    { "up", 0x26 }, { "down", 0x28 }, { "left", 0x25 }, { "right", 0x27 },
    { "insert", 0x2D }, { "delete", 0x2E }, { "home", 0x24 }, { "end", 0x23 },
    { "pgup", 0x21 }, { "pgdn", 0x22 },
    { "f1", 0x70 }, { "f2", 0x71 }, { "f3", 0x72 }, { "f4", 0x73 },
    { "f5", 0x74 }, { "f6", 0x75 }, { "f7", 0x76 }, { "f8", 0x77 },
    { "f9", 0x78 }, { "f10", 0x79 }, { "f11", 0x7A }, { "f12", 0x7B },
    { "num0", 0x60 }, { "num1", 0x61 }, { "num2", 0x62 }, { "num3", 0x63 },
    { "num4", 0x64 }, { "num5", 0x65 }, { "num6", 0x66 }, { "num7", 0x67 },
    { "num8", 0x68 }, { "num9", 0x69 },
    { "nummul", 0x6A }, { "numplus", 0x6B }, { "numadd", 0x6B },
    { "numminus", 0x6D }, { "numsub", 0x6D }, { "numdot", 0x6E }, { "numdiv", 0x6F },
    { "none", 0 },
    { NULL, 0 }
};

typedef struct { int ctl; int vk; int prev; } km_bind;
typedef struct { int n; km_bind b[KC__COUNT]; } km_map;
static km_map g_km[2];                 /* both zeroed: the feature is OFF */
static int    g_km_trig  = 128;
static int    g_km_stick = 16383;      /* 0.50 * 32767 */
static int    g_km_focus  = 1;
static int    g_km_mouse  = 0;
static int    g_km_pguard = 1;

/* Reference count per virtual key, so two controls bound to the same key (the
 * d-pad and the stick both on W, say) do not release it out from under each
 * other. Only gamepad_thread touches this -- it feeds pad 0 then pad 1 on one
 * thread -- so no interlock is needed. */
static int g_km_held[256];

static int km_is_extended(int vk) {
    switch (vk) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT:
        case VK_RCONTROL: case VK_RMENU:
        case VK_NUMLOCK: case VK_DIVIDE:
            return 1;
        default: return 0;
    }
}

/* One INPUT carrying wVk AND wScan (see the header for why not two). */
static void km_emit(int vk, int down) {
    INPUT in;
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    ZeroMemory(&in, sizeof in);
    in.type     = INPUT_KEYBOARD;
    in.ki.wVk   = (WORD)vk;
    in.ki.wScan = (WORD)sc;
    in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP)
                  | (km_is_extended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(1, &in, sizeof(INPUT));
}
static void km_key_down(int vk) {
    if (vk <= 0 || vk > 255) return;
    if (g_km_held[vk]++ == 0) km_emit(vk, 1);
}
static void km_key_up(int vk) {
    if (vk <= 0 || vk > 255) return;
    if (g_km_held[vk] <= 0) { g_km_held[vk] = 0; return; }
    if (--g_km_held[vk] == 0) km_emit(vk, 0);
}

/* ---- parsing -------------------------------------------------------------- */
static int km_lower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }
static int km_ieq(const char* a, const char* b) {
    while (*a && *b) { if (km_lower((unsigned char)*a) != km_lower((unsigned char)*b)) return 0; ++a; ++b; }
    return *a == 0 && *b == 0;
}
static int km_find_ctl(const char* s) {
    int i;
    for (i = 0; i < KC__COUNT; ++i) if (km_ieq(s, g_ctl[i].name)) return i;
    for (i = 0; g_ctl_alias[i].name; ++i) if (km_ieq(s, g_ctl_alias[i].name)) return g_ctl_alias[i].id;
    return -1;
}
/* Returns a virtual key, 0 for an explicit "none", or -1 if unparseable. */
static int km_parse_key(const char* s) {
    int i;
    if (!s || !*s) return -1;
    if (!s[1]) {                                 /* one character: W, J, 7 */
        int c = (unsigned char)s[0];
        if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
        if (c >= 'A' && c <= 'Z') return c;
        if (c >= '0' && c <= '9') return c;
        return -1;
    }
    for (i = 0; g_keyname[i].name; ++i) if (km_ieq(s, g_keyname[i].name)) return g_keyname[i].vk;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        long v = strtol(s + 2, NULL, 16);
        return (v > 0 && v < 256) ? (int)v : -1;
    }
    if (s[0] >= '0' && s[0] <= '9') {
        long v = strtol(s, NULL, 10);
        return (v > 0 && v < 256) ? (int)v : -1;
    }
    return -1;
}
static int km_is_sep(int c)  { return c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int km_is_pair(int c) { return c == ':' || c == '='; }

/* Fills *m from a "control:key, control:key" string. Returns the bind count.
 * An unparseable pair is logged and skipped -- a typo must never take the
 * whole map down with it. */
static int km_parse(km_map* m, const char* src, const char* who) {
    char ctl[32], key[32];
    const char* p = src;
    m->n = 0;
    if (!src) return 0;
    while (*p) {
        size_t ci = 0, ki = 0;
        int id, vk;
        while (*p && km_is_sep((unsigned char)*p)) ++p;
        if (!*p) break;
        while (*p && !km_is_sep((unsigned char)*p) && !km_is_pair((unsigned char)*p)) {
            if (ci + 1 < sizeof ctl) ctl[ci++] = *p;
            ++p;
        }
        ctl[ci] = 0;
        if (!km_is_pair((unsigned char)*p)) { blog("OMNI-BRIDGE: %s: '%s' has no key, skipped", who, ctl); continue; }
        ++p;
        while (*p && !km_is_sep((unsigned char)*p)) {
            if (ki + 1 < sizeof key) key[ki++] = *p;
            ++p;
        }
        key[ki] = 0;
        id = km_find_ctl(ctl);
        vk = km_parse_key(key);
        if (id < 0)  { blog("OMNI-BRIDGE: %s: unknown control '%s', skipped", who, ctl); continue; }
        if (vk < 0)  { blog("OMNI-BRIDGE: %s: unknown key '%s' for '%s', skipped", who, key, ctl); continue; }
        if (vk == 0) { blog("OMNI-BRIDGE: %s: '%s' bound to NONE", who, ctl); continue; }
        if (m->n < KC__COUNT) {
            m->b[m->n].ctl = id; m->b[m->n].vk = vk; m->b[m->n].prev = 0;
            ++m->n;
        }
    }
    return m->n;
}

/* FAIL CLOSED, and understand the word forms.
 *
 * strtol returns 0 for anything non-numeric -- and 0 is also OFF. So
 * ULTRA_BRIDGE_KEYS_FOCUS = on silently DISABLED the focus guard, and the pad
 * would then type W/A/S/D/J/K/Enter into every other application on the desktop
 * for the rest of the session. The header for that key literally says "leave
 * this on", and sibling keys in the same container already use word forms
 * (ULTRA_PAUSE_KEYS = none), so the word form is what a user will write. */
static int km_envi(const char* name, int dflt) {
    const char* v = getenv(name);
    char* end = NULL;
    long n;
    if (!v || !*v) return dflt;
    if (km_ieq(v, "on")  || km_ieq(v, "true")  || km_ieq(v, "yes")) return 1;
    if (km_ieq(v, "off") || km_ieq(v, "false") || km_ieq(v, "no"))  return 0;
    n = strtol(v, &end, 0);
    if (end == v) {
        blog("OMNI-BRIDGE: %s='%s' not understood -- using default %d", name, v, dflt);
        return dflt;                       /* guards stay ON */
    }
    return (int)n;
}

/* Read the whole feature's configuration. Called once, from gamepad_thread. */
static void km_init(void) {
    const char* k0 = getenv("ULTRA_BRIDGE_KEYS");
    const char* k1 = getenv("ULTRA_BRIDGE_KEYS2");
    const char* st;
    if ((!k0 || !*k0) && (!k1 || !*k1)) return;   /* OFF: nothing below runs */

    g_km_trig  = km_envi("ULTRA_BRIDGE_KEYS_TRIG", 128);
    g_km_focus = km_envi("ULTRA_BRIDGE_KEYS_FOCUS", 1);
    g_km_mouse  = km_envi("ULTRA_BRIDGE_KEYS_MOUSE", 0);
    g_km_pguard = km_envi("ULTRA_BRIDGE_KEYS_PAUSEGUARD", 1);
    st = getenv("ULTRA_BRIDGE_KEYS_STICK");
    if (st && *st) {
        double f = atof(st);
        if (f > 0.0 && f < 1.0) g_km_stick = (int)(f * 32767.0);
    }
    if (k0 && *k0) km_parse(&g_km[0], k0, "ULTRA_BRIDGE_KEYS");
    if (k1 && *k1) km_parse(&g_km[1], k1, "ULTRA_BRIDGE_KEYS2");
    blog("OMNI-BRIDGE: pad->key map ACTIVE (pad0 %d binds, pad1 %d binds, "
         "trig=%d stick=%d focus=%d mouse=%d pauseguard=%d)",
         g_km[0].n, g_km[1].n, g_km_trig, g_km_stick, g_km_focus, g_km_mouse, g_km_pguard);
}

static int km_ctl_down(int id, const ul_bridge_gamepad* g) {
    if (id <= KC_Y) return (g->wButtons & g_ctl[id].mask) != 0;
    switch (id) {
        case KC_LT:      return g->bLeftTrigger  > g_km_trig;
        case KC_RT:      return g->bRightTrigger > g_km_trig;
        case KC_SUP:     return g->sThumbLY >  g_km_stick;
        case KC_SDOWN:   return g->sThumbLY < -g_km_stick;
        case KC_SLEFT:   return g->sThumbLX < -g_km_stick;
        case KC_SRIGHT:  return g->sThumbLX >  g_km_stick;
        case KC_RSUP:    return g->sThumbRY >  g_km_stick;
        case KC_RSDOWN:  return g->sThumbRY < -g_km_stick;
        case KC_RSLEFT:  return g->sThumbRX < -g_km_stick;
        case KC_RSRIGHT: return g->sThumbRX >  g_km_stick;
        default: return 0;
    }
}

/* Release every key this pad is still holding. A no-op when the pad has no map
 * (m->n == 0) or is holding nothing, which is why it is safe to call from the
 * shared paths below. */
static void km_release_pad(int idx) {
    km_map* m = &g_km[idx];
    int i;
    for (i = 0; i < m->n; ++i) {
        if (m->b[i].prev) { km_key_up(m->b[i].vk); m->b[i].prev = 0; }
    }
}

/* Is the foreground window one of OUR game process's windows? SendInput posts
 * to whatever has focus, so without this a pad resting past its dead zone types
 * into the user's other applications. */
static int km_focus_ok(void) {
    HWND fg;
    DWORD pid = 0;
    if (!g_km_focus) return 1;
    fg = GetForegroundWindow();
    if (!fg) return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* CHORD STATE for the pause combo. One per pad.
 *
 * START + BACK together is the pause overlay's own combo (pause_menu_wire.c,
 * C1 = 0x0010 | 0x0020), and on Contra BACK is bound to SPACE = SERVICE = a
 * real credit. The first version of this guard simply suppressed both binds
 * while both buttons were down, and that LEAKED A FREE CREDIT ON BOTH EDGES:
 *
 *   PRESS   -- thumbs land 20-80 ms apart, so whichever button arrives first
 *              posts its key on the very next 8 ms poll, before the combo has
 *              ever existed. Suppressing "while both are held" is too late.
 *   RELEASE -- while suppressed, prev was forced to 0. Let go of START first
 *              and the combo is no longer true, but BACK is still held: the
 *              next poll sees now=1, prev=0, and fires a FRESH key-down.
 *
 * So one overlay visit was worth 0, 1 or 2 free credits depending on timing.
 *
 * This version handles it the way chords have to be handled:
 *   ARM DELAY  -- a combo button must be held ALONE for KM_ARM_POLLS before its
 *                 key is posted at all, which covers the thumb spread.
 *   LATCH      -- once both have been seen down together, both stay suppressed
 *                 until BOTH are released, so there is no rising edge to catch.
 * The cost is a ~120 ms delay on START and the credit button only. Neither is a
 * twitch control, and every other bind is untouched and still instant. */
#define KM_ARM_POLLS 15            /* x 8 ms poll = ~120 ms */
typedef struct { int st_arm, bk_arm, latch; } km_chord;
static km_chord g_chord[2];

static void km_chord_reset(int idx) {
    g_chord[idx].st_arm = g_chord[idx].bk_arm = g_chord[idx].latch = 0;
}

static void km_feed_pad(int idx, const ul_bridge_gamepad* g) {
    km_map* m = &g_km[idx];
    km_chord* cs = &g_chord[idx];
    int st_raw = (g->wButtons & UXB_START) != 0;
    int bk_raw = (g->wButtons & UXB_BACK)  != 0;
    int i;

    if (!km_focus_ok()) { km_release_pad(idx); km_chord_reset(idx); return; }

    if (g_km_pguard) {
        if (st_raw && bk_raw)        cs->latch = 1;   /* chord seen */
        else if (!st_raw && !bk_raw) cs->latch = 0;   /* only a FULL release clears it */
        if (!st_raw) cs->st_arm = 0; else if (cs->st_arm < KM_ARM_POLLS) cs->st_arm++;
        if (!bk_raw) cs->bk_arm = 0; else if (cs->bk_arm < KM_ARM_POLLS) cs->bk_arm++;
    }

    for (i = 0; i < m->n; ++i) {
        int ctl = m->b[i].ctl;
        int now = km_ctl_down(ctl, g) ? 1 : 0;
        if (g_km_pguard && ctl == KC_START)
            now = (!cs->latch && cs->st_arm >= KM_ARM_POLLS) ? now : 0;
        else if (g_km_pguard && ctl == KC_BACK)
            now = (!cs->latch && cs->bk_arm >= KM_ARM_POLLS) ? now : 0;
        if (now == m->b[i].prev) continue;          /* edge-triggered: no repeat */
        if (now) km_key_down(m->b[i].vk); else km_key_up(m->b[i].vk);
        m->b[i].prev = now;
    }
}

static void gamepad_feed_one(int idx) {
    if (!s_xig) return;
    ul_bridge_xstate st; ZeroMemory(&st, sizeof st);
    if (s_xig((DWORD)idx, &st) != 0) { km_release_pad(idx); return; }   /* not connected */

    HWND w = g_get_window ? g_get_window() : NULL;
    if (!w) { km_release_pad(idx); return; }

    if (g_cfg.gamepad_source) {
        SHORT sx = g_cfg.stick_right ? st.Gamepad.sThumbRX : st.Gamepad.sThumbLX;
        SHORT sy = g_cfg.stick_right ? st.Gamepad.sThumbRY : st.Gamepad.sThumbLY;
        int dz = (int)(g_cfg.deadzone * 32767.0f);
        int speed = (int)(14.0f * (g_cfg.sensitivity > 0 ? g_cfg.sensitivity : 1.0f));
        int moved = 0;
        if (sx > dz || sx < -dz) { g_pad[idx].accum_x += ((int)sx * speed) / 32768; moved = 1; }
        if (sy > dz || sy < -dz) { g_pad[idx].accum_y -= ((int)sy * speed) / 32768; moved = 1; }   /* Y inverted: up = positive on the stick */

        /* Idle stick -> don't touch the cursor at all (never fight a real
         * mouse, or the lightgun bridge if it's already driving this frame). */
        if (moved) {
            RECT rc;
            if (GetClientRect(w, &rc)) {
                int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
                if (cw > 0 && ch > 0) {
                    if (g_pad[idx].accum_x < 0) g_pad[idx].accum_x = 0;
                    if (g_pad[idx].accum_x > cw - 1) g_pad[idx].accum_x = cw - 1;
                    if (g_pad[idx].accum_y < 0) g_pad[idx].accum_y = 0;
                    if (g_pad[idx].accum_y > ch - 1) g_pad[idx].accum_y = ch - 1;

                    int have_pose = 0;
                    if (g_get_pose) { int gx, gy, gt; have_pose = g_get_pose(&gx, &gy, &gt); }
                    if (!have_pose) feed_cursor_px(w, g_pad[idx].accum_x, g_pad[idx].accum_y);
                }
            }
        }
    }

    /* THE OPT-IN PAD -> KEY MAP. g_km[idx].n is zero unless a container set
     * ULTRA_BRIDGE_KEYS / ULTRA_BRIDGE_KEYS2, so with no map configured this
     * whole branch is skipped and the legacy block below runs untouched. When a
     * map IS configured for this pad it owns the pad completely: the five-button
     * legacy path would otherwise fire its own vk_start/vk_coin on the same
     * physical buttons the map has just bound. */
    if (g_km[idx].n > 0) {
        km_feed_pad(idx, &st.Gamepad);
        if (g_km_mouse) {
            int kfire = (st.Gamepad.wButtons & UXB_A) || (st.Gamepad.bRightTrigger > 128);
            mouse_set(kfire, idx == 1 ? g_cfg.p2_button_right : 0);
        }
        return;
    }

    /* THE LEGACY PATH POSTS REAL SYSTEM KEYS TOO, so it needs the same focus
     * gate the mapped path gets. km_focus_ok() was only checked inside the map
     * branch above, but this block calls key_tap() for START/COIN/SERVICE/TEST
     * -- so a pad falling through here (which pad 1 does whenever
     * ULTRA_BRIDGE_KEYS2 fails to parse, since km_parse skips bad pairs
     * silently and leaves n == 0) would type into whatever the user alt-tabbed
     * to. Clearing the edge state as we bail means returning to the game does
     * not fire a phantom press from a button that was held while away. */
    if (!km_focus_ok()) {
        mouse_set(0, idx == 1 ? g_cfg.p2_button_right : 0);
        g_pad[idx].start_prev = g_pad[idx].coin_prev = 0;
        g_pad[idx].svc_prev   = g_pad[idx].test_prev = 0;
        return;
    }

    /* idx 0 = P1 (left click always); idx 1 = P2 (button per config,
     * default right -- matches the live-confirmed Haunted Museum mapping). */
    int fire = (st.Gamepad.wButtons & UXB_A) || (st.Gamepad.bRightTrigger > 128);
    mouse_set(fire, idx == 1 ? g_cfg.p2_button_right : 0);

    int start_now = (st.Gamepad.wButtons & UXB_START) != 0;
    if (start_now && !g_pad[idx].start_prev) key_tap(g_cfg.vk_start, 40);
    g_pad[idx].start_prev = start_now;

    int coin_now = (st.Gamepad.wButtons & UXB_BACK) != 0;
    if (coin_now && !g_pad[idx].coin_prev) key_tap(g_cfg.vk_coin, 40);
    g_pad[idx].coin_prev = coin_now;

    int svc_now = (st.Gamepad.wButtons & UXB_LB) != 0;
    if (svc_now && !g_pad[idx].svc_prev) key_tap(g_cfg.vk_service, 40);
    g_pad[idx].svc_prev = svc_now;

    int test_now = (st.Gamepad.wButtons & UXB_RB) != 0;
    if (test_now && !g_pad[idx].test_prev) key_tap(g_cfg.vk_test, 40);
    g_pad[idx].test_prev = test_now;
}

static DWORD WINAPI gamepad_thread(LPVOID arg) {
    (void)arg;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
    for (int i = 0; dlls[i] && !s_xig; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) s_xig = (pfnXInputGetState)(void*)GetProcAddress(h, "XInputGetState");
    }
    if (!s_xig) { blog("OMNI-BRIDGE: XInput not available -- gamepad bridge inert"); return 0; }

    /* Reads ULTRA_BRIDGE_KEYS / ULTRA_BRIDGE_KEYS2 and returns immediately if
     * neither is set, leaving g_km[] all-zero. */
    km_init();

    HWND w0 = NULL;
    for (int i = 0; i < 300 && !g_stop && !(w0 = (g_get_window ? g_get_window() : NULL)); ++i) Sleep(50);
    if (w0) {
        RECT rc;
        if (GetClientRect(w0, &rc))
            for (int i = 0; i < 2; ++i) {
                g_pad[i].accum_x = (rc.right - rc.left) / 2;
                g_pad[i].accum_y = (rc.bottom - rc.top) / 2;
            }
    }

    blog("OMNI-BRIDGE: gamepad bridge active (%s stick aims, A/RT fires, "
         "START/BACK/LB/RB -> mapped keys; pad0=P1 left-click, pad1=P2 %s-click)",
         g_cfg.stick_right ? "right" : "left", g_cfg.p2_button_right ? "right" : "left");
    while (!g_stop) {
        gamepad_feed_one(0);
        gamepad_feed_one(1);
        Sleep(8);
    }
    /* RELEASE ON THE THREAD THAT PRESSED.
     *
     * g_stop is only tested at the TOP of this loop, so the pass already in
     * flight when it is set runs to completion. If the DETACH thread released
     * the keys itself, that in-flight pass would see prev==0 for a control the
     * player is still holding and post a FRESH key-down after the release --
     * and the process then exits with that key down on the whole desktop, with
     * nothing left alive to send the key-up. g_km_held[] and b[].prev are also
     * documented as owned by this thread alone.
     *
     * So the owner drains, and ultra_omni_bridge_stop() only waits. */
    km_release_pad(0);
    km_release_pad(1);
    InterlockedExchange(&g_km_drained, 1);
    return 0;
}

static HANDLE g_gun_th = NULL, g_pad_th = NULL;
static int    g_installed = 0;

void ultra_omni_bridge_install(ultra_bridge_window_fn get_window, ultra_bridge_pose_fn get_pose) {
    if (g_installed) return;
    g_installed = 1;
    g_get_window = get_window;
    g_get_pose   = get_pose;
    omni_client_read_bridge(&g_cfg);
    if (!g_cfg.enabled) {
        blog("OMNI-BRIDGE: disabled (ULTRA_BRIDGE_ENABLE=0)");
        return;
    }
    if (g_get_pose) g_gun_th = CreateThread(NULL, 0, gun_thread, NULL, 0, NULL);
    g_pad_th = CreateThread(NULL, 0, gamepad_thread, NULL, 0, NULL);
}

void ultra_omni_bridge_stop(void) {
    InterlockedExchange(&g_stop, 1);
    /* BOUNDED wait, never a join: DLL_PROCESS_DETACH runs under the PE loader
     * lock, so blocking here indefinitely would deadlock the process we are
     * trying to let exit. ~2 poll periods is enough for the in-flight pass to
     * finish and drain its own keys. */
    if (g_pad_th) WaitForSingleObject(g_pad_th, 200);
    if (g_gun_th) WaitForSingleObject(g_gun_th, 200);
    mouse_force_release();
    /* Never leave a key latched down on the way out. Both calls are no-ops
     * unless a container configured a map AND that pad is holding something.
     * Only needed if the poll thread never reached its own drain. */
    if (!InterlockedCompareExchange(&g_km_drained, 0, 0)) {
        km_release_pad(0);
        km_release_pad(1);
    }
    if (g_gun_th) { CloseHandle(g_gun_th); g_gun_th = NULL; }
    if (g_pad_th) { CloseHandle(g_pad_th); g_pad_th = NULL; }
}

#endif /* _WIN32 */
