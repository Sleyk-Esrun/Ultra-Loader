/* SPDX-License-Identifier: MIT
 * NOTE: independent namco_es3 board copy -- adapted from native_x86/src/hook/pause_menu_wire.c (reference only; no build-time or runtime dependency on native_x86 remains).
 * =============================================================================
 *  pause_menu_wire_es3.c  --  native_x86 hook <-> shared pause_menu_service bridge
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
 *         exit_to_menu       -> ExitProcess(3) so the loader returns to its menu
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
#include "pause_menu_service.h"    /* public API of the shared service */
#include "scene_probe.h"           /* runtime scene-state detector (observe-only) */
/* home_state.h is deliberately NOT included any more.
 *
 * It implemented a whole-process memory rollback as the generic fallback for
 * boards with no known scene trigger. That approach is a DEAD END for live
 * multi-threaded Win32 games and the header now documents why in full. Short
 * version, both proven by measurement:
 *   - A CRITICAL_SECTION half lives in the kernel. Rolling back its user-mode
 *     counters while the kernel keyed-event wait queue persists deadlocks the
 *     game. Confirmed by parking the CS in .bss so it is never rolled back -
 *     it still deadlocks.
 *   - SuspendThread does not park a thread at a user-mode boundary. Under load
 *     every worker sat in an ntdll WOW64 syscall thunk, and rewinding EIP/ESP
 *     under an in-flight syscall is not sound.
 * With 100%% of regions restored and none skipped, every worker thread still
 * stopped forever. A frozen cabinet is strictly worse than the reboot it was
 * meant to replace.
 *
 * Including it also cost ~240 KB of .bss per board for a struct nobody calls.
 * Return to Game Menu is driven by the game itself instead - see
 * ultra_es3_return_to_game_menu() in jvs_core_es3.c. */

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

    /* AND the board's own log, because otherwise we are blind on this.
     *
     * These two sinks reach a debugger and a captured stderr, and the board log
     * is neither. So ultra_namco_jvs.log contained ZERO "[OVERLAY]" and ZERO
     * "[pausemenu]" lines for the whole Pokken session - and the lines that DID
     * appear, in ultra.log, came from a different game entirely on another
     * board. Reasoning about whether the overlay ran from those was reasoning
     * about the wrong process.
     *
     * Same file the rest of the hook writes, opened per line: this is a handful
     * of messages when a human presses a key, not a per-frame path. */
    {
        char path[MAX_PATH];
        if (GetEnvironmentVariableA("ULTRA_JVS_LOG", path, sizeof path) && path[0]) {
            FILE *f = fopen(path, "a");
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

/* Restart: the module host relaunched the game with the same args; we exit here
 * with a marker code the module can spot and re-launch. Exit code 2 = "restart
 * same game" by convention (unused elsewhere in the hook). */
static void pm_restart_game(void) {
    /* ExitProcess is the safe choice from inside a random game thread; TerminateProcess
     * has been observed to leak GDI + audio device state. The module runs the game as
     * a child and re-launches when it exits, so this is enough. */
    ExitProcess(2);
}
/* Exit code 3 = "Return to Ultra Loader Menu". 0 was indistinguishable from the
 * game simply closing, so the loader exited along with it instead of bringing
 * its picker back. The module forwards the child exit code verbatim and
 * main() loops on 3. See UL_RC_TO_MENU in work/src/main.c. */
static void pm_exit_to_menu(void) {
    ExitProcess(3);
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

/* ---- "Return to Game Menu" ---------------------------------------------------
 * A native Win32 arcade game has no machine to reset, so we rewind its memory to
 * the frame it first reached its own title screen. See work/include/home_state.h
 * for why a private-pages rollback is safe from a Present boundary. */
/* MKDX drives its own screens through clSequenceManager, so "Return to Game
 * Menu" is a single store to that manager rather than a memory rollback. See
 * jvs_core_es3.c. It needs no setup and no captured state, which is why
 * home_capture is left NULL for this board - there is nothing to capture.
 *
 * home_state.h stays included as the generic fallback for boards where no
 * such trigger has been traced yet; it is disabled by default (ULHOME_MEMSNAP)
 * because a whole-process rollback is not proven safe on a live game. */
extern int ultra_es3_return_to_game_menu(void);
extern int ultra_es3_home_available(void);

/* RETURN TO TITLE - hand-mapped first, LEARNED second.
 *
 * The hand-mapped triggers (MKDX's clSequenceManager, Pokken's scene latch)
 * are instant and exact, but they are per-title work and there are hundreds of
 * titles. So when there is no hand-mapped trigger we fall back to what the
 * game told us about ITSELF during boot.
 *
 * scene_probe.h watches the game trace its own start-up - Boot -> Logo ->
 * Demo -> Title is the game writing its own request field and consuming it -
 * and locks onto that field once the behavior is proven: three full
 * sentinel->value->sentinel cycles, two of them adopted by the same partner
 * field, across at least two different screen ids. Nothing else in a game's
 * memory behaves like that, which is why the detector can be trusted without
 * knowing a single thing about the engine.
 *
 * The value the partner holds while the game idles at boot IS that game's home
 * screen. We never learn what the enum means and never need to. Writing it
 * back is the same store the game makes on itself.
 *
 * This is why the menu item appears a few seconds in rather than immediately:
 * it unlocks the moment the probe has seen enough, which is about when the
 * game reaches its title screen. Until then home_available() says no and the
 * item stays out of the way. */
/* SCENE PROBE ONLY - the hand-mapped Pokken path is deliberately not used.
 *
 * ultra_es3_return_to_game_menu() tries ultra_es3_pkn_return_to_title() first,
 * and that writes 1 into errmgr+0x38 - the game's FATAL LATCH, the exact byte
 * whose being set is what puts the cabinet into scene 0x11, the scene that
 * draws nothing. Driving the black-screen mechanism on purpose to force a
 * scene change was too clever: it depends on a hard-coded RVA, and its
 * withdrawal depended on a per-frame tick that also poked that byte.
 *
 * The probe reaches the same place by watching the game write its OWN screen
 * field and then writing that same value back - no hard-coded address, and
 * nothing touched that the game does not touch itself. */
static int pm_home_restore(void)
{
    if (scene_probe_ready()) {
        uintptr_t req  = scene_probe_req();
        int       home = scene_probe_home();
        if (req && home >= 0) {
            *(volatile uint32_t *)req = (uint32_t)home;
            loader_log(LOG_INFO, "[OVERLAY] return to title: learned request field "
                                 "%p <- screen id %d (was on %d)",
                       (void *)req, home, scene_probe_current());
            return 0;
        }
    }
    return -1;
}

static int pm_home_available(void)
{
    return scene_probe_ready();   /* probe only - see pm_home_restore */
}

/* Called by the present-hook TUs so the pause menu status bar advances. Cheap,
 * lock-free; fine on the render thread.
 *
 * This is also the one call site every board already has on its frame path, so
 * it is where the home-state auto-capture is armed. booted = -1 means "this
 * board does not report boot-complete", which leaves the frame-count heuristic
 * in charge. */
void ultra_pausemenu_tick_frame(void)
{
    /* The Pokken scene-latch tick is gone with the PKN path (see
     * pm_home_restore): nothing writes errmgr+0x38 any more, so there is
     * nothing to withdraw. */
    LONG f = InterlockedIncrement(&g_pm_frame);

    /* Runtime scene-state detector. OBSERVE-ONLY and opt-in
     * (ULTRA_SCENE_PROBE=1); it never writes to the game.
     *
     * This board already knows its answer by hand - clSequenceManager at
     * *0x00E2FDD8 +0x3C/+0x40 - which is exactly why the detector runs here
     * first: MKDX is the ground truth to check it against. If it independently
     * reports request=...+0x40 with home id 5, it can be trusted on the titles
     * nobody has traced. Hand-tracing 390 games does not scale, and the generic
     * memory-rollback alternative is a proven dead end (see home_state.h). */
    scene_probe_tick((int)f);
}

/* ---- lazy init (idempotent, safe from any thread) ----------------------------- */

static volatile LONG g_pm_inited = 0;

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
    h.home_restore     = pm_home_restore;
    h.home_capture     = NULL;   /* nothing to capture: the trigger is native */
    h.home_available   = pm_home_available;
    /* snapshots left NULL -- Snapshots sub-menu hides itself gracefully */
    h.get_title        = pm_get_title;
    h.get_frame        = pm_get_frame;
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
    /* A 400ms WINDOW, not a single sample.
     *
     * This used to require both bits in the SAME 8ms poll. Pressing Start and
     * Select a tenth of a second apart - which is what a two-handed gesture on
     * a hand-held pad actually looks like - produced nothing, and nothing in
     * the log said why. Either button now arms the window and the other one
     * inside it opens the menu, in either order. Holding both is unchanged. */
#define COMBO_WINDOW_MS 400u
    const WORD B_START = 0x0010, B_SELECT = 0x0020;
    const WORD C1 = B_START | B_SELECT;
    static DWORD t_start = 0, t_select = 0;
    const DWORD now_ms = GetTickCount();
    int now1, was1 = ((prev & C1) == C1);

    if ((held & B_START)  && !(prev & B_START))  t_start  = now_ms ? now_ms : 1;
    if ((held & B_SELECT) && !(prev & B_SELECT)) t_select = now_ms ? now_ms : 1;

    now1 = ((held & C1) == C1)
        || (t_start && t_select
            && (now_ms - t_start)  < COMBO_WINDOW_MS
            && (now_ms - t_select) < COMBO_WINDOW_MS);

    /* Say when only half of it arrives, so a press that did not open the menu
     * is visible in the log instead of silently doing nothing. */
    if (!now1) {
        if (t_start && !t_select && (now_ms - t_start) < COMBO_WINDOW_MS) {
            static DWORD said; if (said != t_start) { said = t_start;
                loader_log(LOG_INFO, "[pausemenu] START seen - waiting for SELECT"); }
        } else if (t_select && !t_start && (now_ms - t_select) < COMBO_WINDOW_MS) {
            static DWORD said2; if (said2 != t_select) { said2 = t_select;
                loader_log(LOG_INFO, "[pausemenu] SELECT seen - waiting for START"); }
        }
    }
    /* DIAGNOSTIC: report the raw pad mask the first time each new combination
     * of buttons is seen, so the log shows whether START (0x10) and BACK (0x20)
     * are reaching this function at all. */
    { static WORD seen = 0;
      if (held && held != seen) { seen = held;
          loader_log(LOG_INFO, "[pausemenu] pad mask 0x%04X (START=0x10 BACK=0x20)", held); } }
    if (now1 && !was1) {
        t_start = t_select = 0;          /* consume the window: one-shot */
        pausemenu_lazy_register();
        pause_menu_request(0);
        loader_log(LOG_INFO, "[pausemenu] request raised from pad");
    }
}

/* Keyboard fallback: any of F1 / Esc / P (edge-triggered). Called from the same
 * poll thread; uses GetAsyncKeyState so it works without a message pump. */
void ultra_pausemenu_kbd_edge_check(void) {
    static int prev = 0;
    static unsigned beats = 0;
    int now = 0;
    if (GetAsyncKeyState(VK_F1)     & 0x8000) now |= 1;
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) now |= 2;
    if (GetAsyncKeyState('P')       & 0x8000) now |= 4;

    /* DIAGNOSTIC. The menu never opened across several sessions and the log
     * could not distinguish "this check never runs" from "it runs but never
     * sees a key" - two completely different bugs. So: a heartbeat proving the
     * check is live, and a line the moment any watched key is actually seen. */
    if ((beats++ % 60u) == 0u)             /* ~0.5 s: fine-grained so a stall is visible */
        loader_log(LOG_INFO, "[pausemenu] kbd check alive (tick %u, keys=0x%X)",
                   beats, now);
    if (now && !prev)
        loader_log(LOG_INFO, "[pausemenu] KEY SEEN 0x%X (F1=1 Esc=2 P=4)", now);

    /* fire on any bit going 0->1 */
    if (now && !(prev & now)) {
        pausemenu_lazy_register();
        pause_menu_request(0);
        loader_log(LOG_INFO, "[pausemenu] request raised from keyboard");
    }
    prev = now;
}

/* Tear-down from DllMain DLL_PROCESS_DETACH. Safe even if pause_menu_init was
 * never called (the service checks its own g_host_set flag). */
void ultra_pausemenu_shutdown(void) {
    pause_menu_shutdown();
}

#endif /* _WIN32 */
