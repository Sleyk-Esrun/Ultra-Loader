/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  omni_bridge.c -- Ultra Omni-Bridge reference implementation
 * =============================================================================
 *
 *  Generalized from this board's OWN typex2_gun.c gamepad+lightgun bridge
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

static void gamepad_feed_one(int idx) {
    if (!s_xig) return;
    ul_bridge_xstate st; ZeroMemory(&st, sizeof st);
    if (s_xig((DWORD)idx, &st) != 0) return;   /* not connected */

    HWND w = g_get_window ? g_get_window() : NULL;
    if (!w) return;

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
    mouse_force_release();
    if (g_gun_th) { CloseHandle(g_gun_th); g_gun_th = NULL; }
    if (g_pad_th) { CloseHandle(g_pad_th); g_pad_th = NULL; }
}

#endif /* _WIN32 */
