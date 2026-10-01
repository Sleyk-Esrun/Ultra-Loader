/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  pause_menu_wire_wpc.c  --  native_x86 hook <-> shared pause_menu_service
 * =============================================================================
 *
 *  The shared pause menu (work/src/pause_menu_service.c) is a GDI layered-window
 *  overlay that draws on top of the host's rendering. This board's own
 *  hook_dllmain_wpc.c has no continuous input-poll loop to hook into (the
 *  installers are one-shot IAT patches), so this file owns a
 *  SELF-CONTAINED thread: one poll loop that does its own minimal XInput read
 *  for the pause combo, checks the keyboard fallback, and pumps the overlay --
 *  decoupled from this board's own I/O stubs. Same pattern as omni_bridge.c's
 *  own gun_thread/gamepad_thread.
 *
 *  Local definitions of g_settings / loader_log so pause_menu_service.c links
 *  standalone inside the injected game process.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "loader.h"
#include "settings.h"
#include "pause_menu_service.h"
#include "scene_probe.h"        /* runtime scene-state detector */

UltraSettings g_settings;

void loader_log(int level, const char *fmt, ...) {
    char buf[1024];
    const char *lv = (level >= LOG_ERROR) ? "ERR"
                   : (level >= LOG_WARN)  ? "WRN"
                   : (level >= LOG_INFO)  ? "INF" : "DBG";
    int n = snprintf(buf, sizeof buf, "[hook][%s] ", lv);
    if (n < 0 || n >= (int)sizeof buf) n = 0;
    va_list ap; va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    if ((size_t)(n + m) >= sizeof buf - 2) m = (int)sizeof buf - 2 - n;
    buf[n + m] = '\n'; buf[n + m + 1] = 0;
    OutputDebugStringA(buf);
    fputs(buf, stderr);
}

/* ---- host callbacks --------------------------------------------------- */

static HWND (*g_get_hwnd)(void) = NULL;
static HWND pm_get_hwnd(void) {
    HWND h = g_get_hwnd ? g_get_hwnd() : NULL;
    if (!h) h = GetActiveWindow();
    if (!h) h = GetForegroundWindow();
    return h;
}

static void pm_settings_apply(void) {
    HWND h = pm_get_hwnd();
    if (h && g_settings.width > 0 && g_settings.height > 0) {
        SetWindowPos(h, NULL, 0, 0, g_settings.width, g_settings.height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static void pm_restart_game(void) { ExitProcess(2); }
static void pm_exit_to_menu(void) { ExitProcess(0); }

static const char *pm_get_title(void) {
    const char *t = getenv("ULTRA_TITLE");
    if (!t || !*t) t = getenv("ULTRA_GAME");
    if (!t || !*t) t = "wahlap_wpc";
    return t;
}
static volatile LONG g_pm_frame = 0;
static int pm_get_frame(void) { return (int)InterlockedCompareExchange(&g_pm_frame, 0, 0); }

static volatile LONG g_pm_inited = 0;
/* RETURN TO GAME TITLE, without a line of per-game work.
 *
 * scene_probe.h watches the game trace its own boot - Boot -> Logo -> Demo ->
 * Title is the game writing its own screen-request field and then consuming it
 * - and locks onto that field once the behavior is proven beyond coincidence:
 * three full sentinel->value->sentinel cycles, two of them adopted by the same
 * partner field, across at least two different screen ids.
 *
 * The value the partner holds while the game idles at boot IS that game's home
 * screen. We never learn what the enum means, and never need to. Writing it
 * back is the same store the game already makes on itself.
 *
 * So the menu item lights up a few seconds into a game it has never seen
 * before - about when it reaches its title screen - and does nothing before
 * then, because home_available() honestly says no.
 */
static int pm_home_restore(void)
{
    uintptr_t req;
    int home;
    if (!scene_probe_ready()) return -1;
    req  = scene_probe_req();
    home = scene_probe_home();
    if (!req || home < 0) return -1;
    *(volatile uint32_t *)req = (uint32_t)home;
    loader_log(LOG_INFO, "[OVERLAY] return to title: learned request field %p <- "
                         "screen id %d (was on %d)",
               (void *)req, home, scene_probe_current());
    return 0;
}
static int pm_home_available(void) { return scene_probe_ready(); }

static void pausemenu_lazy_register(void) {
    if (InterlockedCompareExchange(&g_pm_inited, 1, 0) != 0) return;
    if (g_settings.width  <= 0) g_settings.width  = 1280;
    if (g_settings.height <= 0) g_settings.height = 720;
    if (g_settings.audio_volume <= 0 || g_settings.audio_volume > 100)
        g_settings.audio_volume = 80;
    g_settings.pause_game_audio = 1;

    pause_menu_host_t h;
    memset(&h, 0, sizeof h);
    h.get_hwnd         = pm_get_hwnd;
    h.win_w            = g_settings.width;
    h.win_h            = g_settings.height;
    h.settings_apply   = pm_settings_apply;
    h.restart_game     = pm_restart_game;
    h.exit_to_menu     = pm_exit_to_menu;
    h.get_title        = pm_get_title;
    h.get_frame        = pm_get_frame;
    h.home_restore     = pm_home_restore;
    h.home_available   = pm_home_available;
    pause_menu_init(&h);
    loader_log(LOG_INFO, "[pausemenu] registered (native_x86 hook)");
}

/* ---- self-contained poll thread ---------------------------------------- */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } pm_xpad;
typedef struct { DWORD packet; pm_xpad pad; } pm_xstate;
typedef DWORD (WINAPI *pm_xinput_get)(DWORD, pm_xstate*);
static pm_xinput_get s_pm_xget = NULL;

static void pm_ensure_xinput(void) {
    if (s_pm_xget) return;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (size_t i = 0; i < sizeof(dlls)/sizeof(dlls[0]) && !s_pm_xget; ++i) {
        HMODULE hm = LoadLibraryA(dlls[i]);
        if (hm) s_pm_xget = (pm_xinput_get)(void*)GetProcAddress(hm, "XInputGetState");
    }
}

static void pm_kbd_edge_check(void) {
    static int prev = 0;
    int now = 0;
    if (GetAsyncKeyState(VK_F1)     & 0x8000) now |= 1;
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) now |= 2;
    if (GetAsyncKeyState('P')       & 0x8000) now |= 4;
    if (now && !(prev & now)) { pausemenu_lazy_register(); pause_menu_request(0); }
    prev = now;
}

static void pm_pad_edge_check(void) {
    static WORD prev = 0;
    pm_ensure_xinput();
    if (!s_pm_xget) return;
    pm_xstate st; memset(&st, 0, sizeof st);
    if (s_pm_xget(0, &st) != 0) return;
    WORD held = st.pad.wButtons;
    /* START + SELECT only. LB+RB removed 2026-09-14: on an arcade panel
     * those are live game buttons (shift paddles on a racer, PUSH5/PUSH6
     * on a fighter), so the menu kept opening mid-play. Start+Select is a
     * deliberate two-hand gesture no cabinet uses as a game input. */
    const WORD C1 = 0x0010 | 0x0020;  /* Start+Select */
    int now1 = ((held & C1) == C1), was1 = ((prev & C1) == C1);
    if (now1 && !was1) { pausemenu_lazy_register(); pause_menu_request(0); }
    prev = held;
}

static volatile LONG g_pm_stop = 0;
static HANDLE        g_pm_thread = NULL;

static DWORD WINAPI pausemenu_poll_thread(LPVOID arg) {
    (void)arg;
    while (!g_pm_stop) {
        pm_kbd_edge_check();
        pm_pad_edge_check();
        pausemenu_lazy_register();
        InterlockedIncrement(&g_pm_frame);
        scene_probe_tick((int)InterlockedCompareExchange(&g_pm_frame, 0, 0));
        pause_menu_pump();
        Sleep(8);
    }
    return 0;
}

void ultra_pausemenu_install(HWND (*get_hwnd)(void)) {
    if (g_pm_thread) return;
    g_get_hwnd = get_hwnd;
    g_pm_thread = CreateThread(NULL, 0, pausemenu_poll_thread, NULL, 0, NULL);
}

void ultra_pausemenu_shutdown(void) {
    InterlockedExchange(&g_pm_stop, 1);
    pause_menu_shutdown();
    if (g_pm_thread) { CloseHandle(g_pm_thread); g_pm_thread = NULL; }
}

#endif /* _WIN32 */
