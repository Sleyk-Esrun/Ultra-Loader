/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  pause_menu_wire.c  --  native_x86 hook <-> shared pause_menu_service bridge
 * =============================================================================
 *
 *  The shared pause menu (work/src/pause_menu_service.c) is a GDI layered-window
 *  overlay that draws on top of the host's rendering. To reuse it inside the
 *  injected Type X / Type X2 / Sega Europa-R hook DLL we:
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
 *  Note: the shared service header pause_menu_service.h is on the -I path via
 *  ../../../../../../work/include (see build_hook.sh).
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "loader.h"                /* LOG_* levels; loader_log defined below */
#include "settings.h"              /* UltraSettings struct */
#include "pause_menu_service.h"
#include "scene_probe.h"        /* runtime scene-state detector */    /* public API of the shared service */

/* ---- local g_settings + loader_log so pause_menu_service.c links standalone --- */

/* Zero-initialized session copy. The pause menu reads/writes fields directly;
 * un-set fields stay at their zero-value defaults (which are the same defaults
 * settings_load() applies before ultra.ini is read on the loader side). We set a
 * few obviously-non-zero defaults below on first init so the menu is usable. */
UltraSettings g_settings;

/* loader_log stub -- forward to OutputDebugString + stderr so the messages the
 * service already emits ("[OVERLAY] in-game menu opened", etc.) still show up in
 * the debug view / a captured stderr log. Cheap; no external file, no crash risk
 * inside a foreign process. */
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
    /* Tee into the board log as well. Without this, every overlay event --
     * when the menu opened, which item was chosen, whether an exit was even
     * attempted -- is invisible unless someone is watching a debugger, which
     * nobody is during a playtest. Same pattern as audio_hook.c's alog(). */
    {
        const char *p = getenv("ULTRA_JVS_LOG");
        if (p && *p) {
            FILE *f = fopen(p, "a");
            if (f) { fputs(buf, f); fclose(f); }
        }
    }
}

/* ---- host callbacks the pause_menu service invokes ---------------------------- */

/* typex2_gun.c publishes the D3D9 device HWND once CreateDevice has fired.
 * For D3D11 / GL boards we fall back to GetActiveWindow() (usually the same
 * window; the overlay will re-parent on the first draw). */
extern HWND ultra_typex2_device_window(void);
static HWND pm_get_hwnd(void) {
    HWND h = ultra_typex2_device_window();
    if (!h) h = GetActiveWindow();
    if (!h) h = GetForegroundWindow();
    return h;
}

/* Re-apply live settings changes (e.g. resolution / window-mode toggles). The
 * hook can't tear down and recreate the game's device, but a size change picked
 * from the menu should at least resize the window; the D3D9 upscale path
 * (d3d9_throttle.c!resize_output_window9) will pick up the new size on the next
 * Present. */
static void pm_settings_apply(void) {
    HWND h = pm_get_hwnd();
    if (h && g_settings.width > 0 && g_settings.height > 0) {
        SetWindowPos(h, NULL, 0, 0, g_settings.width, g_settings.height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* EXIT MUST ALWAYS WORK. ExitProcess is the clean path and stays the primary
 * one, but it is NOT guaranteed to return: it runs DLL_PROCESS_DETACH for every
 * loaded DLL and takes the loader lock to do it. If any thread died holding that
 * lock -- which is exactly what happens when a thread is killed mid-call --
 * ExitProcess blocks forever and the menu item silently does nothing.
 *
 * That is what the owner hit on 2026-09-17: the game's main thread had been
 * killed, the overlay was still drawing WS_EX_TOPMOST over their desktop, and
 * "Exit Ultra Loader" could not close it. They had to use Task Manager.
 *
 * So every exit arms a fallback first: if the clean path has not completed in
 * 1.5 seconds, force it. The fallback releases the mouse before terminating,
 * because TerminateProcess skips DLL_PROCESS_DETACH and a latched button would
 * otherwise be left pinned. */
static DWORD WINAPI pm_kill_fallback(LPVOID code) {
    INPUT up;
    Sleep(1500);
    memset(&up, 0, sizeof up);
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;  SendInput(1, &up, sizeof(INPUT));
    up.mi.dwFlags = MOUSEEVENTF_RIGHTUP; SendInput(1, &up, sizeof(INPUT));
    /* KEYS TOO, NOT JUST THE MOUSE.
     *
     * This backstop predates the pad->keyboard mapper. TerminateProcess skips
     * DLL_PROCESS_DETACH, so ultra_omni_bridge_stop() -- the only thing that
     * sends the key-ups -- never ran on this path. A player holding a direction
     * when they chose "Exit to menu" was left with that key DOWN across the
     * whole desktop, with the only process that could release it now dead.
     * No-op when no map is configured, so this is inert for Silent Hill. */
    { extern void ultra_omni_bridge_stop(void); ultra_omni_bridge_stop(); }
    loader_log(LOG_ERROR, "[pausemenu] ExitProcess(%u) did not complete in 1500 ms "
                          "(loader lock held by a dead thread?) -- TerminateProcess",
               (unsigned)(UINT_PTR)code);
    TerminateProcess(GetCurrentProcess(), (UINT)(UINT_PTR)code);
    return 0;
}
static void pm_arm_kill_fallback(UINT code) {
    HANDLE t = CreateThread(NULL, 0, pm_kill_fallback, (LPVOID)(UINT_PTR)code, 0, NULL);
    if (t) CloseHandle(t);
}

/* Restart: the module host relaunched the game with the same args; we exit here
 * with a marker code the module can spot and re-launch. Exit code 2 = "restart
 * same game" by convention (unused elsewhere in the hook). */
/* LET THE GAME SHUT ITSELF DOWN FIRST, SO IT SAVES.
 *
 * KSHG keeps its operator settings in Data\TestDat\setting.1 -- free play vs
 * coin, the coin blocker, volume, everything an operator sets in the service
 * menu -- and it keeps bookkeeping alongside. It writes them during its OWN
 * shutdown: the terminate chain at 0x4D1160 (kacbookkeeping_terminate,
 * shaiolib_COIN_blocker_close, tabooword_terminate).
 *
 * We never gave it the chance. Both exits called ExitProcess, which tears the
 * process down without running any of that, and the TerminateProcess backstop
 * is harsher still. The evidence is on disk: setting.1 was last written in
 * JULY, while gun.1 from the same folder was written today -- so writing works
 * fine, the game simply never reached its save. Every service-menu change the
 * owner made was discarded at exit, which is also why CREDIT(S) reads
 * uninitialised garbage on every boot.
 *
 * WM_CLOSE is the polite request: the game's own window procedure handles it,
 * runs its terminate chain, flushes, and exits by itself. If it does, our
 * ExitProcess below never runs. If it ignores us, we fall through and force it
 * exactly as before -- so this can only add a graceful path, never remove the
 * guaranteed one. */
static void pm_request_graceful_exit(void) {
    HWND h = pm_get_hwnd();
    if (!h || !IsWindow(h)) {
        loader_log(LOG_WARN, "[pausemenu] no game window to close politely -- exiting directly");
        return;
    }
    loader_log(LOG_INFO, "[pausemenu] asking the game to close (WM_CLOSE) so it can save "
                         "setting.1 and its bookkeeping");
    PostMessageA(h, WM_CLOSE, 0, 0);

    /* Give it a real chance to run its terminate chain and write. Polled rather
     * than one long sleep, so a game that closes quickly is not held up. */
    {
        int i;
        for (i = 0; i < 60; ++i) {          /* up to ~3 s, 50 ms granularity */
            Sleep(50);
            if (!IsWindow(h)) {
                loader_log(LOG_INFO, "[pausemenu] game window closed itself after %d ms -- "
                                     "its shutdown ran", (i + 1) * 50);
                return;
            }
        }
        loader_log(LOG_WARN, "[pausemenu] game did not close within 3 s of WM_CLOSE -- "
                             "forcing the exit (its settings may not have been saved)");
    }
}

static void pm_restart_game(void) {
    /* ExitProcess is the safe choice from inside a random game thread; TerminateProcess
     * has been observed to leak GDI + audio device state. The module runs the game as
     * a child and re-launches when it exits, so this is enough -- when it returns. */
    pm_request_graceful_exit();
    pm_arm_kill_fallback(2);
    ExitProcess(2);
}
static void pm_exit_to_menu(void) {
    pm_request_graceful_exit();
    pm_arm_kill_fallback(0);
    ExitProcess(0);
}

/* Title / frame for the status bar. Titles come from the module via env
 * (ULTRA_TITLE) so we don't have to reach into the boot_console TU. Frame is a
 * simple counter the present hooks bump. */
static const char *pm_get_title(void) {
    const char *t = getenv("ULTRA_TITLE");
    if (!t || !*t) t = getenv("ULTRA_GAME");
    if (!t || !*t) t = "Type X / PC Arcade";
    return t;
}
static volatile LONG g_pm_frame = 0;
static int pm_get_frame(void) { return (int)InterlockedCompareExchange(&g_pm_frame, 0, 0); }

/* Called by the present-hook TUs so the pause menu status bar advances. Cheap,
 * lock-free; fine on the render thread. */
void ultra_pausemenu_tick_frame(void) {
    scene_probe_tick((int)InterlockedCompareExchange(&g_pm_frame, 0, 0)); InterlockedIncrement(&g_pm_frame); }

/* ---- lazy init (idempotent, safe from any thread) ----------------------------- */

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

    /* Session defaults -- match the loader's settings_defaults() where obvious.
     * Everything else stays at zero (Bilinear upscaler, no rotate, etc.). */
    if (g_settings.width  <= 0) g_settings.width  = 1280;
    if (g_settings.height <= 0) g_settings.height = 720;
    if (g_settings.audio_volume <= 0 || g_settings.audio_volume > 100)
        g_settings.audio_volume = 80;
    g_settings.pause_game_audio = 1;   /* silence game audio while paused (menu-owned) */

    pause_menu_host_t h;
    memset(&h, 0, sizeof h);
    h.get_hwnd         = pm_get_hwnd;
    h.win_w            = g_settings.width;
    h.win_h            = g_settings.height;
    h.audio_pause      = NULL;         /* hook has no audio engine bridge */
    h.audio_set_volume = NULL;
    h.settings_apply   = pm_settings_apply;
    h.restart_game     = pm_restart_game;
    h.exit_to_menu     = pm_exit_to_menu;
    /* snapshots left NULL -- Snapshots sub-menu hides itself gracefully */
    h.get_title        = pm_get_title;
    h.get_frame        = pm_get_frame;
    h.home_restore     = pm_home_restore;
    h.home_available   = pm_home_available;
    pause_menu_init(&h);
    loader_log(LOG_INFO, "[pausemenu] registered (native_x86 hook)");
}

/* ---- public entry points other TUs call --------------------------------------- */

/* Called from Hook_Present9 / Hook_Present (D3D11) / swap_common (GL) BEFORE the
 * real present. Returns 1 while the overlay is visible (caller must still call
 * Real_Present so the last frame stays on screen; the game must not advance). */
int ultra_pausemenu_pump(void) {
    pausemenu_lazy_register();
    ultra_pausemenu_tick_frame();
    return pause_menu_pump();
}

/* Called from the XInput poll thread. `held` is the current XInput.wButtons +
 * synthetic keyboard bits; `prev` is the previous frame's same mask. Fires
 * pause_menu_request() on a fresh combo edge. Combos honored:
 *     Start (0x10) + Back  (0x20)
 * Keyboard combos (F1 / Esc / P) are handled by a separate helper below since
 * they don't fit in the XInput 16-bit mask. */
void ultra_pausemenu_edge_check(WORD held, WORD prev) {
    /* START + SELECT only.
     *
     * an arcade panel (shift paddles on a racer, PUSH5/PUSH6 on a fighter), so
     * the menu kept appearing mid-play. Start+Select is a deliberate two-hand
     * gesture that no cabinet uses as a game input. */
    /* START + SELECT only. LB+RB removed 2026-09-14: on an arcade panel
     * those are live game buttons (shift paddles on a racer, PUSH5/PUSH6
     * on a fighter), so the menu kept opening mid-play. Start+Select is a
     * deliberate two-hand gesture no cabinet uses as a game input. */
    const WORD C1 = 0x0010 | 0x0020;  /* Start + Select */
    int now1 = ((held & C1) == C1);
    int was1 = ((prev & C1) == C1);
    if (now1 && !was1) {
        pausemenu_lazy_register();
        pause_menu_request(0);
    }
}

/* Keyboard fallback: any of F1 / Esc / P (edge-triggered). Called from the same
 * poll thread; uses GetAsyncKeyState so it works without a message pump.
 *
 * CONFIGURABLE PER GAME, because these three keys are not free on every title.
 *
 * ULTRA_PAUSE_KEYS accepts a comma list of VK codes (decimal or 0x hex), or the
 * word "none" to switch the keyboard route off entirely and leave only the pad
 * combo. Unset keeps the historical F1/Esc/P.
 *
 * WHY IT EXISTS. Contra Evolution HD's own key table makes F1 its TEST button
 * and P a test-menu action. GetAsyncKeyState only observes, so the game does
 * still receive them -- but our overlay opens on top at the same moment, and
 * while it is open nothing gates the keys, so our menu's Enter is simultaneously
 * the game's P1 START. The result is a TEST menu you cannot use. On that title
 * the pad combo (Start+Back) is the whole answer and the keyboard route is pure
 * interference. */
static int ultra_pause_key_list(int **out_n) {
    static int  keys[8];
    static int  n = -1;
    static int  disabled = 0;
    if (n < 0) {
        const char *e = getenv("ULTRA_PAUSE_KEYS");
        n = 0;
        if (e && *e) {
            if (_stricmp(e, "none") == 0) {
                disabled = 1;
            } else {
                char buf[128], *tk;
                strncpy(buf, e, sizeof buf - 1); buf[sizeof buf - 1] = 0;
                for (tk = strtok(buf, ", "); tk && n < 8; tk = strtok(NULL, ", ")) {
                    int v = (int)strtol(tk, NULL, 0);
                    if (v > 0 && v < 256) keys[n++] = v;
                }
            }
        }
        if (!n && !disabled) {                 /* historical default */
            keys[n++] = VK_F1;
            keys[n++] = VK_ESCAPE;
            keys[n++] = 'P';
        }
        loader_log(LOG_INFO, "[pausemenu] keyboard hotkeys: %s",
                   disabled ? "DISABLED (ULTRA_PAUSE_KEYS=none) -- pad combo only"
                            : (e && *e ? "from ULTRA_PAUSE_KEYS" : "default F1/Esc/P"));
    }
    *out_n = keys;
    return disabled ? 0 : n;
}

void ultra_pausemenu_kbd_edge_check(void) {
    static int prev = 0;
    int now = 0, i, n, *keys;
    n = ultra_pause_key_list(&keys);
    for (i = 0; i < n; ++i)
        if (GetAsyncKeyState(keys[i]) & 0x8000) now |= (1 << i);
    /* fire on any bit going 0->1 */
    if (now && !(prev & now)) {
        pausemenu_lazy_register();
        pause_menu_request(0);
    }
    prev = now;
}

/* Tear-down from DllMain DLL_PROCESS_DETACH. Safe even if pause_menu_init was
 * never called (the service checks its own g_host_set flag). */
void ultra_pausemenu_shutdown(void) {
    pause_menu_shutdown();
}

#endif /* _WIN32 */
