/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  pause_menu_wire_typex.c  --  native_x86 hook <-> shared pause_menu_service bridge
 * =============================================================================
 *
 *  The shared pause menu (work/src/pause_menu_service.c) is a GDI layered-window
 *  overlay that draws on top of the host's rendering. To reuse it inside the
 *  injected Type X hook DLL we:
 *
 *    1. Provide LOCAL definitions of the two loader-side symbols the service
 *       depends on -- `g_settings` (UltraSettings struct, zero-initialized so the
 *       menu has sane defaults) and `loader_log` (forwards to OutputDebugString).
 *       The hook lives inside the GAME PROCESS and can't see the loader's real
 *       g_settings, so the pause menu edits its own in-process copy for the
 *       CURRENT session. Persistence (save-to-ini) is handled the next time the
 *       user opens the loader's settings dialog.
 *
 *    2. Register a pause_menu_host_t with the callbacks the hook can honor:
 *         get_hwnd           -> the D3D device window (typex2_gun.c tracks it)
 *         audio_pause        -> NULL (no in-hook audio engine to pause)
 *         audio_set_volume   -> NULL (see above)
 *         settings_apply     -> local re-apply (window mode / resolution)
 *         restart_game       -> ExitProcess(2) so the loader relaunches
 *         exit_to_menu       -> ExitProcess(0) so the loader returns to the menu
 *         snapshot_*         -> NULL (no snapshot integration in the hook yet)
 *         get_title/get_frame-> best-effort (title from ULTRA_TITLE env, frame
 *                              from a shared counter the present hooks bump)
 *
 *    3. Expose ultra_pausemenu_edge_check(a, b) so the input poll can pass in
 *       "combo held now / prev" masks and this TU owns the edge detection +
 *       pause_menu_request() call. Keeps combo logic out of the poll thread.
 *
 *  Ported from taito_typex2/src/pause_menu_wire_typex2.c (the reference consumer) --
 *  same source of truth every other native_x86-style board wires this way.
 *  Note: the shared service header pause_menu_service.h is on the -I path via
 *  $PROJ/work/include (see build_hook.sh's LOADER_INC).
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "loader.h"                /* LOG_* levels; loader_log defined below */
#include "settings.h"               /* UltraSettings struct */
#include "pause_menu_service.h"    /* public API of the shared service */

/* ---- local g_settings + loader_log so pause_menu_service.c links standalone --- */

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

/* ---- host callbacks the pause_menu service invokes ---------------------------- */

extern HWND ultra_typex2_device_window(void);
static HWND pm_get_hwnd(void) {
    HWND h = ultra_typex2_device_window();
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

static void pm_restart_game(void) {
    ExitProcess(2);
}
static void pm_exit_to_menu(void) {
    ExitProcess(0);
}

static const char *pm_get_title(void) {
    const char *t = getenv("ULTRA_TITLE");
    if (!t || !*t) t = getenv("ULTRA_GAME");
    if (!t || !*t) t = "Type X / PC Arcade";
    return t;
}
static volatile LONG g_pm_frame = 0;
static int pm_get_frame(void) { return (int)InterlockedCompareExchange(&g_pm_frame, 0, 0); }

void ultra_pausemenu_tick_frame(void) { InterlockedIncrement(&g_pm_frame); }

/* ---- lazy init (idempotent, safe from any thread) ----------------------------- */

static volatile LONG g_pm_inited = 0;

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
    h.audio_pause      = NULL;
    h.audio_set_volume = NULL;
    h.settings_apply   = pm_settings_apply;
    h.restart_game     = pm_restart_game;
    h.exit_to_menu     = pm_exit_to_menu;
    h.get_title        = pm_get_title;
    h.get_frame        = pm_get_frame;
    pause_menu_init(&h);
    loader_log(LOG_INFO, "[pausemenu] registered (native_x86 hook)");
}

/* ---- public entry points other TUs call --------------------------------------- */

int ultra_pausemenu_pump(void) {
    pausemenu_lazy_register();
    ultra_pausemenu_tick_frame();
    return pause_menu_pump();
}

void ultra_pausemenu_edge_check(WORD held, WORD prev) {
    /* START + SELECT only.
     *
     * LB+RB used to open the menu as well, but those are live game buttons on
     * an arcade panel (shift paddles on a racer, PUSH5/PUSH6 on a fighter), so
     * the menu kept appearing mid-play. Start+Select is a deliberate two-hand
     * gesture that no cabinet uses as a game input. */
    const WORD C1 = 0x0010 | 0x0020;  /* Start + Select */
    int now1 = ((held & C1) == C1);
    int was1 = ((prev & C1) == C1);
    if (now1 && !was1) {
        pausemenu_lazy_register();
        pause_menu_request(0);
    }
}

void ultra_pausemenu_kbd_edge_check(void) {
    static int prev = 0;
    int now = 0;
    if (GetAsyncKeyState(VK_F1)     & 0x8000) now |= 1;
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) now |= 2;
    if (GetAsyncKeyState('P')       & 0x8000) now |= 4;
    if (now && !(prev & now)) {
        pausemenu_lazy_register();
        pause_menu_request(0);
    }
    prev = now;
}

void ultra_pausemenu_shutdown(void) {
    pause_menu_shutdown();
}

#endif /* _WIN32 */
