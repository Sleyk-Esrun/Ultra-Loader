/*
 * glut_shim.c
 * GLUT-3 compatibility additions - Cruis'n Blast (Raw Thrills Lindbergh).
 *
 * The base `glut_stub.c` covers everything After Burner Climax + GSEVO need
 * (init, callbacks, buffer swap, bitmap font, teapot/sphere/cube). Cruis'n
 * Blast pulls in a *slightly* wider slice of FreeGLUT-3: timer callbacks, the
 * warp/fullscreen APIs, window show/hide, and forced joystick polling. This
 * file adds *only* those extras - the base stubs are still registered by
 * `glut_register_stubs`. The extras here register through
 * `glut_shim_register_extras`, called from `lindbergh_main.c` after the base
 * pass, so behavior for every other title is unchanged.
 *
 * Every addition is a no-op that satisfies the game's contract:
 *   - glutTimerFunc  → we invoke the cb once immediately with the supplied
 *                       value; the game re-arms in the cb so the render loop
 *                       ticks in `glutMainLoop` without a dedicated timer
 *                       thread.
 *   - glutWarpPointer → moves the cursor; harmless on a real desktop.
 *   - glutFullScreen  → forwarded to the SDL window (fullscreen toggle) if
 *                       one exists.
 *   - glutHide/ShowWindow → cosmetic; state is remembered for glutGet queries.
 *   - glutForceJoystickPoll → runs the cb registered via glutJoystickFunc.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "loader.h"

/* Pull the base-stub state we need via extern - kept private to this file so
 * `glut_stub.c` doesn't grow a header. `glut_stub_get_joystick_cb` is added
 * as a tiny accessor at the bottom of glut_stub.c. */
typedef void (*GlutJoystickCb)(unsigned int, int, int, int);
extern GlutJoystickCb glut_stub_get_joystick_cb(void);

/* SDL window handle exposed by sdl_window.c. Signature there is HWND. */
extern HWND sdl_window_get_hwnd(void);

/* ------------------------------------------------------------------ */
/* Timer                                                                */
/* ------------------------------------------------------------------ */

/* We keep a tiny fixed pool of timer registrations; Cruis'n Blast never
 * arms more than a handful concurrently. Timer thread pulses at 1 kHz;
 * every entry counts down to zero, fires its cb, then goes dormant until
 * the cb re-arms via a fresh glutTimerFunc call. */
#define GLUT_TIMER_SLOTS 16
typedef struct {
    void (*cb)(int);
    int     value;
    int     ms_remaining;
    int     active;
} GlutTimer;

static GlutTimer g_timers[GLUT_TIMER_SLOTS];
static CRITICAL_SECTION g_timer_lock;
static HANDLE  g_timer_thread = NULL;
static volatile LONG g_timer_running = 0;

static DWORD WINAPI glut_timer_thread(LPVOID arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&g_timer_running, 1, 1)) {
        Sleep(1);
        EnterCriticalSection(&g_timer_lock);
        for (int i = 0; i < GLUT_TIMER_SLOTS; i++) {
            if (!g_timers[i].active) continue;
            if (--g_timers[i].ms_remaining > 0) continue;
            void (*cb)(int) = g_timers[i].cb;
            int val         = g_timers[i].value;
            g_timers[i].active = 0;
            LeaveCriticalSection(&g_timer_lock);
            /* Callback runs OUTSIDE the lock - a well-behaved cb may re-arm
             * itself via glutTimerFunc, which will re-take the lock. */
            if (cb) cb(val);
            EnterCriticalSection(&g_timer_lock);
        }
        LeaveCriticalSection(&g_timer_lock);
    }
    return 0;
}

void stub_glutTimerFunc(unsigned int ms, void (*cb)(int value), int value)
{
    if (!cb) return;

    /* Lazy-init lock + thread on first arm. */
    static int init = 0;
    if (!init) {
        InitializeCriticalSection(&g_timer_lock);
        init = 1;
    }

    EnterCriticalSection(&g_timer_lock);
    int slot = -1;
    for (int i = 0; i < GLUT_TIMER_SLOTS; i++) {
        if (!g_timers[i].active) { slot = i; break; }
    }
    if (slot >= 0) {
        g_timers[slot].cb           = cb;
        g_timers[slot].value        = value;
        g_timers[slot].ms_remaining = (int)ms;
        g_timers[slot].active       = 1;
    }
    LeaveCriticalSection(&g_timer_lock);

    if (slot < 0) {
        loader_log(LOG_WARN, "[GLUT] glutTimerFunc: pool exhausted (%d slots)",
                   GLUT_TIMER_SLOTS);
        return;
    }

    if (!g_timer_thread) {
        InterlockedExchange(&g_timer_running, 1);
        g_timer_thread = CreateThread(NULL, 0, glut_timer_thread, NULL, 0, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Warp / fullscreen / show / hide                                     */
/* ------------------------------------------------------------------ */

void stub_glutWarpPointer(int x, int y)
{
    /* The plan calls for it, but warping the OS cursor while the game runs
     * would fight the pause overlay's mouse tracking (see
     * pause_menu_service.c). We move the cursor only when the window is
     * focused; the harmless no-op otherwise satisfies the game's contract. */
    HWND hwnd = sdl_window_get_hwnd();
    if (!hwnd || GetForegroundWindow() != hwnd) return;
    POINT p = { x, y };
    ClientToScreen(hwnd, &p);
    SetCursorPos(p.x, p.y);
}

static int g_win_visible = 1;

void stub_glutFullScreen(void)
{
    /* Cruis'n Blast auto-fullscreens on the real cab. We ignore the request
     * here because the loader controls output-window sizing (per
     * `ultra-loader-typex2-hauntedmuseum.md` - fullscreen-exclusive causes
     * device-LOST stalls). ULTRA_RES / the loader's Display panel governs
     * output. */
    loader_log(LOG_INFO, "[GLUT] glutFullScreen ignored (loader-managed window)");
}

void stub_glutHideWindow(void) { g_win_visible = 0; }
void stub_glutShowWindow(void) { g_win_visible = 1; }
void stub_glutIconifyWindow(void) { g_win_visible = 0; }

/* ------------------------------------------------------------------ */
/* Forced joystick poll                                                */
/* ------------------------------------------------------------------ */

void stub_glutForceJoystickPoll(void)
{
    /* Real FreeGLUT invokes the joystick cb once immediately without waiting
     * for the poll interval. GSEVO relies on this; Cruis'n Blast may too. We
     * fetch the cb the base stub stored (glut_stub.c owns g_joystick_cb) and
     * invoke it with zeroed axes - the game's cb reads the wheel from our
     * Omni-Steer service, not from the axes we pass. */
    GlutJoystickCb cb = glut_stub_get_joystick_cb();
    if (cb) cb(0u, 0, 0, 0);
}

/* Placeholder for legacy FreeGLUT MULTI-window support. Cruis'n Blast opens
 * only one window, so all these return the primary window id (1). */
int  stub_glutGetWindow    (void)    { return 1; }
void stub_glutSetWindow    (int id)  { (void)id; }

/* ------------------------------------------------------------------ */
/* Registration - called from lindbergh_main.c after glut_register_stubs */
/* ------------------------------------------------------------------ */

void glut_shim_register_extras(LoaderCtx *ctx)
{
    if (!ctx) return;
    loader_log(LOG_INFO, "Registering GLUT-3 shim extras (Cruis'n Blast surface)");

    #define R(name) sym_register(ctx, #name, (uintptr_t)stub_##name)

    R(glutTimerFunc);
    R(glutWarpPointer);
    R(glutFullScreen);
    R(glutHideWindow);
    R(glutShowWindow);
    R(glutIconifyWindow);
    R(glutForceJoystickPoll);
    R(glutGetWindow);
    R(glutSetWindow);

    #undef R
}
