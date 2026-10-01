/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  typex2_gun.c -- Taito Type X2 lightgun bridge + auto-pilot (Haunted Museum).
 * =============================================================================
 *
 *  Haunted Museum aims by reading the Windows MOUSE CURSOR and fires on the
 *  left mouse button; it takes COIN on the 'S' key and START on '1' (its
 *  MUSEUM.ini ships GUNENABLE/JVSENABLE/DINPUTENABLE=0, so it does NOT read the
 *  JVS screen-position or DirectInput -- it reads the OS mouse + keyboard).
 *
 *  Two cooperating, env-gated facilities, both DEFAULT-OFF (so a plain render
 *  test is byte-identical to the native path):
 *
 *   1. ULTRA_TYPEX2_GUN=1  -- LIGHTGUN BRIDGE.
 *      The universal Ultra Lightgun engine lands a calibrated pose in the
 *      loader's shared block (ultra_io_shared::screen_x/screen_y, 0..65535) and
 *      flags the trigger in the player button mask. This maps that pose into
 *      client pixels and feeds it as the OS cursor (SetCursorPos), and presses
 *      the left mouse button for the trigger -- so the gun aims and fires the
 *      game exactly as a physical lightgun would. When no loader is mapped, the
 *      gun rests at screen center (a centered "gun" still plays a rail shooter).
 *
 *   2. ULTRA_TYPEX2_AUTO=1 -- CREDIT-LOOP AUTO-PILOT (probe / unattended demo).
 *      A self-contained state machine that drives the whole credit loop without
 *      a loader or a human: it waits for the device window, taps 'S' a few times
 *      (insert coins), taps '1' (start), then -- for a rail shooter -- keeps the
 *      gun centered and auto-fires so a stage plays through to game-over and back
 *      to attract. This is what lets the loop CLOSE in an automated frame-dump
 *      probe. It uses the same cursor + mouse-button feed as the bridge.
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 API + the loader's
 *  own shared-memory I/O block. No game code, no proprietary input source.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>

#include "ultra_io_shared.h"

extern void ultra_d3d8_log(const char* fmt, ...);
#define glog ultra_d3d8_log

/* provided by hook_dllmain.c */
ultra_io_shared* ultra_hook_shared(void);

static volatile LONG g_gun_stop = 0;
/* Written once by the D3D9 CreateDevice hook (render thread), read on the gun /
 * auto threads -- volatile so the readers see the store without a barrier race. */
static HWND volatile  g_gun_hwnd = NULL;   /* device window (set by the D3D9 hook) */

/* The D3D9 CreateDevice hook hands us the game's device window so the cursor
 * maps to the right client rect. Until it fires, g_gun_hwnd is NULL and the
 * bridge stays inert (we do NOT grab an arbitrary foreground window -- during
 * boot that would be the loader/launcher/console). */
void ultra_typex2_gun_set_window(HWND hwnd) { g_gun_hwnd = hwnd; }

/* The device window the D3D hook captured (for foregrounding from other modules,
 * e.g. the racing auto-pilot). Returns NULL until the real device window is known
 * so callers can skip the foreground poke rather than acting on a stray window. */
HWND ultra_typex2_device_window(void) {
    return g_gun_hwnd;
}

/* The gun bridge's target: ONLY the real device window. NULL = not ready yet
 * (the caller skips the cursor/trigger feed until CreateDevice supplies it). */
static HWND gun_target_window(void) {
    return g_gun_hwnd;
}

/* ---- low-level input feeders (SendInput: drives the real input queue, so it
 *      reaches GetAsyncKeyState/GetKeyState AND WM_KEYDOWN / DirectInput
 *      readers) ------------------------------------------------------------- */

/* Move the OS cursor to a client-pixel (px,py) inside window w. */
static void feed_cursor_px(HWND w, int px, int py) {
    if (!w) return;
    POINT pt = { px, py };
    ClientToScreen(w, &pt);
    /* Never warp the pointer off the visible desktop -- see the long note on
     * the matching guard in omni_bridge.c. ClientToScreen maps through the
     * game window's current position, so an off-screen window turns this
     * ~125Hz call into a pointer the user cannot move. */
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

/* Center the cursor in the window's client rect. */
static void feed_cursor_center(HWND w) {
    RECT rc; if (!w || !GetClientRect(w, &rc)) return;
    feed_cursor_px(w, (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2);
}

/* Single shared left-button state so the GUN bridge and the AUTO auto-pilot (which
 * may run at the same time) never fight over the OS mouse button -- without this
 * each thread tracked its own local down/up and the interleaving left the button
 * in an indeterminate (possibly stuck-DOWN) state. All toggles go through here. */
static volatile LONG g_mouse_down = 0;   /* current emitted left-button state (0/1) */
static void mouse_emit(int down) {
    INPUT in; ZeroMemory(&in, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof in);
}
/* Set the shared button state; only emits an event on an actual edge. */
static void mouse_set(int down) {
    down = down ? 1 : 0;
    if (InterlockedExchange(&g_mouse_down, down) != down) mouse_emit(down);
}
/* Guarantee the OS left button is released (called on stop / thread exit so a
 * trigger held at teardown can't latch the system-wide mouse button down). */
static void mouse_force_release(void) {
    if (InterlockedExchange(&g_mouse_down, 0) != 0) mouse_emit(0);
}

/* Coherent snapshot of the loader's seqlocked pose (screen_x/screen_y/player[0]).
 * The loader writes these three fields inside an odd/even seq window; reading them
 * raw can latch a torn pose (X from frame N, trigger from frame N+1). Mirror the
 * engine's ultra_hook_current_input() retry loop. Returns 1 if a live, coherent
 * aiming pose was captured; 0 = no live loader -> caller rests at center. */
static int gun_pose_snapshot(ultra_io_shared* s, int* out_x, int* out_y, int* out_trig) {
    if (!s || s->magic != ULTRA_IO_MAGIC) return 0;
    /* gate on loader liveness, not just magic: a loader that exited (or whose gun
     * went inactive) clears LOADER_ALIVE, and we must fall back to center rather
     * than warping / re-firing from a stale pose. */
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
    *out_x = sx; *out_y = sy; *out_trig = (p0 & 0x0200) != 0;
    return 1;
}

/* Ultra Omni-Bridge pose adapter -- signature (int*,int*,int*)->0/1 is EXACTLY
 * ultra_bridge_pose_fn (omni_bridge.h), so it drops straight into
 * ultra_omni_bridge_install()'s 2nd argument with no translation. It is simply
 * gun_pose_snapshot() over this board's own mapped block (ultra_hook_shared()).
 *
 * Why the LOADER_ALIVE gate inside matters here: returning 0 is how the bridge
 * is told "there is NO pose right now", which makes it fall back to the gamepad
 * ->cursor path and leave the real mouse alone. If we instead returned the last
 * values we happened to read, a loader that exited (or an Omni-Track gun that
 * went inactive) would leave the bridge warping the cursor to a frozen aim point
 * and re-asserting a stale trigger forever -- a stuck, un-clearable phantom shot.
 * So: dead loader == no pose, never "last known pose".
 *
 * Cave PC caveat (kept honest for the next reader): Deathsmiles II itself has no
 * cursor/mouse input path -- it reads raw-input keyboard, XInput, and the
 * emulated COM3 JVS responder, which takes only player[0]. So this pose fn does
 * NOT by itself make a lightgun aim DSII; Omni-Track's real reach on this board
 * is player[0] through the shared block. It is still correct to install: it makes
 * a live gun take priority over the pad->cursor path (omni_bridge.c's
 * gamepad_feed_one() yields whenever get_pose() reports live), and it is the
 * right source for any future Cave PC title that does read a cursor. */
int ultra_cave_pc_gun_pose(int* out_x, int* out_y, int* out_trig) {
    return gun_pose_snapshot(ultra_hook_shared(), out_x, out_y, out_trig);
}

/* Tap a virtual key, sending BOTH the VK event and its hardware scan code so it
 * registers no matter how the game samples the keyboard. */
static void key_tap(WORD vk, DWORD hold_ms) {
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    INPUT in[2];
    /* key down: scancode + vk in one event each */
    ZeroMemory(in, sizeof in);
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = vk;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = 0; in[1].ki.wScan = (WORD)sc;
    in[1].ki.dwFlags = KEYEVENTF_SCANCODE;
    SendInput(2, in, sizeof(INPUT));
    Sleep(hold_ms ? hold_ms : 40);
    ZeroMemory(in, sizeof in);
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = vk; in[0].ki.dwFlags = KEYEVENTF_KEYUP;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = 0; in[1].ki.wScan = (WORD)sc;
    in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

/* ---- lightgun bridge (ULTRA_TYPEX2_GUN) ----------------------------------- */
/* Map a shared-block pose (0..65535 over the screen) to a client pixel and feed
 * it as the OS cursor. Trigger (gun button 0) presses the left mouse button.
 * With no loader mapped (probe runs), rest at center. */
static void feed_gun(ultra_io_shared* s) {
    HWND w = gun_target_window();
    if (!w) return;                 /* device window not known yet -> stay inert */
    RECT rc; if (!GetClientRect(w, &rc)) return;
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;

    int px = cw / 2, py = ch / 2;   /* default: rest at center (no live pose) */
    int trigger = 0;
    int sx = 0, sy = 0, t = 0;
    if (gun_pose_snapshot(s, &sx, &sy, &t)) {
        /* map the 0..65535 pose to a client pixel and CLAMP to [0, cw-1]/[0, ch-1]
         * so screen_x==65535 lands on the last in-rect column, not one past it
         * (which would put the aim on the window border / next monitor). */
        px = (int)((long)sx * cw / 65535); if (px > cw - 1) px = cw - 1; if (px < 0) px = 0;
        py = (int)((long)sy * ch / 65535); if (py > ch - 1) py = ch - 1; if (py < 0) py = 0;
        trigger = t;
    }
    feed_cursor_px(w, px, py);
    mouse_set(trigger);             /* shared edge-tracked button (no torn state) */
}

static DWORD WINAPI gun_thread(LPVOID arg) {
    (void)arg;
    glog("TYPEX2 GUN: lightgun bridge active (shared pose -> cursor)");
    while (!g_gun_stop) {
        feed_gun(ultra_hook_shared());
        Sleep(8);   /* ~120 Hz cursor feed */
    }
    mouse_force_release();   /* never leave the OS button latched down on stop */
    return 0;
}

/* Robustly bring the target window to the foreground so SendInput key/mouse events
 * reach the game. A background thread's SetForegroundWindow is frequently REJECTED
 * by Windows' foreground-lock rules (returns FALSE, window only flashes), which
 * would silently mis-route coins/start/fire to whatever window IS focused. Verify
 * the grant and try a stronger AttachThreadInput sequence on failure. */
static int focus_window(HWND w) {
    if (!w) return 0;
    if (SetForegroundWindow(w) && GetForegroundWindow() == w) return 1;
    ShowWindow(w, SW_SHOW);
    BringWindowToTop(w);
    DWORD me  = GetCurrentThreadId();
    DWORD tgt = GetWindowThreadProcessId(w, NULL);
    if (tgt && tgt != me && AttachThreadInput(me, tgt, TRUE)) {
        SetForegroundWindow(w);
        BringWindowToTop(w);
        AttachThreadInput(me, tgt, FALSE);
    }
    return GetForegroundWindow() == w;
}

/* ---- credit-loop auto-pilot (ULTRA_TYPEX2_AUTO) --------------------------- */
/* Self-contained: insert coins ('S'), start ('1'), then center + auto-fire so a
 * rail-shooter stage plays to game-over -> attract, closing the loop. Tunables:
 *   ULTRA_TYPEX2_COINS   coins to insert            (default 2)
 *   ULTRA_TYPEX2_BOOT_MS settle before first coin   (default 9000)
 *   ULTRA_TYPEX2_FIRE_MS auto-fire period in ms     (default 250; 0 = no fire)
 *   ULTRA_TYPEX2_COIN_VK / _START_VK  override keys (default 'S'=0x53 / '1'=0x31)
 */
static int env_int(const char* name, int dflt) {
    const char* e = getenv(name);
    if (!e || !*e) return dflt;
    return (int)strtol(e, NULL, 0);
}

static DWORD WINAPI auto_thread(LPVOID arg) {
    (void)arg;
    int coins    = env_int("ULTRA_TYPEX2_COINS", 2);
    int boot_ms  = env_int("ULTRA_TYPEX2_BOOT_MS", 9000);
    int fire_ms  = env_int("ULTRA_TYPEX2_FIRE_MS", 250);
    WORD coin_vk  = (WORD)env_int("ULTRA_TYPEX2_COIN_VK",  0x53); /* 'S' */
    WORD start_vk = (WORD)env_int("ULTRA_TYPEX2_START_VK", 0x31); /* '1' */
    glog("TYPEX2 AUTO: credit-loop auto-pilot armed (coins=%d boot=%dms fire=%dms)",
         coins, boot_ms, fire_ms);

    /* 1) wait specifically for the REAL device window (D3D9 CreateDevice sets it).
     *    Don't accept an arbitrary foreground window -- during boot that would be
     *    the loader/launcher/console, and we'd warp the user's cursor / foreground
     *    the wrong app. */
    for (int i = 0; i < 600 && !g_gun_stop; ++i) {
        if (gun_target_window()) break;
        Sleep(50);
    }
    HWND w = gun_target_window();
    if (!w) { glog("TYPEX2 AUTO: no device window, giving up"); return 0; }

    /* bring the game to the foreground so SendInput key/mouse reach it. */
    if (!focus_window(w)) glog("TYPEX2 AUTO: foreground grab not confirmed (continuing)");

    /* 2) settle through the attract intro to the INSERT COIN screen. */
    for (int t = 0; t < boot_ms && !g_gun_stop; t += 100) Sleep(100);

    /* 3) insert coins ('S'), then start ('1'). Re-foreground before each so the
     *    keystroke is delivered to the game even if focus wandered. */
    for (int c = 0; c < coins && !g_gun_stop; ++c) {
        w = gun_target_window(); if (!focus_window(w)) glog("TYPEX2 AUTO: coin focus not confirmed");
        key_tap(coin_vk, 60);
        glog("TYPEX2 AUTO: coin %d ('S')", c + 1);
        Sleep(700);
    }
    if (!g_gun_stop) {
        w = gun_target_window(); if (!focus_window(w)) glog("TYPEX2 AUTO: start focus not confirmed");
        key_tap(start_vk, 60);
        glog("TYPEX2 AUTO: start ('1')");
        Sleep(1500);
    }

    /* 4) gameplay: keep the gun centered and auto-fire. A centered rail gun lets a
     *    stage play through to game-over (and back to attract) unattended. */
    int last_fire = 0, btn = 0;
    DWORD t0 = GetTickCount();
    while (!g_gun_stop) {
        w = gun_target_window();
        feed_cursor_center(w);
        if (fire_ms > 0) {
            int now = (int)(GetTickCount() - t0);
            if (now - last_fire >= fire_ms) {
                btn = !btn; mouse_set(btn);   /* shared edge-tracked button */
                last_fire = now;
            }
        }
        Sleep(16);
    }
    mouse_force_release();   /* never leave the OS button latched down on stop */
    return 0;
}

/* Install the Type X2 bridges. Both WIRED, both DEFAULT-OFF:
 *   ULTRA_TYPEX2_GUN=1   -> lightgun-pose -> cursor bridge
 *   ULTRA_TYPEX2_AUTO=1  -> credit-loop auto-pilot (coin/start/auto-fire)
 * With neither set, aiming uses the real mouse, exactly as the dump expects. */
static HANDLE g_gun_th  = NULL;   /* gun_thread handle  (closed on stop)  */
static HANDLE g_auto_th = NULL;   /* auto_thread handle (closed on stop)  */

void ultra_typex2_gun_install(void) {
    const char* g = getenv("ULTRA_TYPEX2_GUN");
    const char* a = getenv("ULTRA_TYPEX2_AUTO");
    int gun  = (g && g[0] == '1');
    int automode = (a && a[0] == '1');
    if (!gun && !automode) {
        glog("TYPEX2 GUN: wired but inert (set ULTRA_TYPEX2_GUN=1 / ULTRA_TYPEX2_AUTO=1)");
        return;
    }
    /* GUN and AUTO both drive the OS cursor + left button; running both lets the
     * live-pose bridge and the centered auto-fire fight over the same mouse. Make
     * them mutually exclusive -- AUTO (the unattended probe) wins if both are set. */
    if (gun && automode) {
        glog("TYPEX2 GUN: both ULTRA_TYPEX2_GUN and _AUTO set -- AUTO wins (gun bridge disabled)");
        gun = 0;
    }
    if (gun)      g_gun_th  = CreateThread(NULL, 0, gun_thread,  NULL, 0, NULL);
    if (automode) g_auto_th = CreateThread(NULL, 0, auto_thread, NULL, 0, NULL);
}

/* Signal the bridge/auto threads to exit and guarantee the OS mouse button is
 * released. Called from DllMain DLL_PROCESS_DETACH before the shared block is
 * unmapped. We do NOT WaitForSingleObject on the threads here -- DETACH runs under
 * the loader lock and joining could deadlock; we just request stop, force-release
 * the button, and close the handles. */
void ultra_typex2_gun_stop(void) {
    InterlockedExchange(&g_gun_stop, 1);
    mouse_force_release();
    if (g_gun_th)  { CloseHandle(g_gun_th);  g_gun_th  = NULL; }
    if (g_auto_th) { CloseHandle(g_auto_th); g_auto_th = NULL; }
}

#endif /* _WIN32 */
