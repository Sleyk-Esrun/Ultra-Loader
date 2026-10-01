/*
 * glut_stub.c
 * GLUT (OpenGL Utility Toolkit) Stub Layer
 *
 * After Burner Climax uses GLUT for window creation and the main loop.
 * We already have a Win32 window and WGL context from glx_bridge.c.
 * GLUT stubs just redirect to that existing window.
 *
 * Key GLUT functions ABC calls:
 *   glutInit()                  - initialize GLUT
 *   glutInitDisplayMode()       - set display mode (RGBA, double, depth)
 *   glutInitWindowSize(w,h)     - set window size
 *   glutInitWindowPosition(x,y) - set window position
 *   glutCreateWindow(title)     - create the window -> returns window ID
 *   glutMainLoop()              - enter event loop
 *   glutDisplayFunc(fn)         - register display callback
 *   glutReshapeFunc(fn)         - register reshape callback
 *   glutKeyboardFunc(fn)        - register keyboard callback
 *   glutIdleFunc(fn)            - register idle callback
 *   glutSwapBuffers()           - swap front/back buffers
 *   glutPostRedisplay()         - mark window for redisplay
 *   glutGet(GLUT_WINDOW_WIDTH)  - query window properties
 *   glutEnterGameMode()         - fullscreen game mode
 *   glutGameModeString("1280x720:32@60") - set game mode
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "loader.h"
#include "glut_stub.h"
#include "glx_bridge.h"

/* ------------------------------------------------------------------ */
/* GLUT callback table                                                 */
/* ------------------------------------------------------------------ */
static void (*g_display_cb)(void)              = NULL;
static void (*g_reshape_cb)(int,int)           = NULL;
static void (*g_keyboard_cb)(unsigned char,int,int) = NULL;
static void (*g_keyboard_up_cb)(unsigned char,int,int) = NULL;
static void (*g_special_cb)(int,int,int)       = NULL;
static void (*g_special_up_cb)(int,int,int)    = NULL;
static void (*g_mouse_cb)(int,int,int,int)     = NULL;
static void (*g_motion_cb)(int,int)            = NULL;
static void (*g_passive_cb)(int,int)           = NULL;
static void (*g_idle_cb)(void)                 = NULL;
static void (*g_entry_cb)(int)                 = NULL;
static void (*g_visibility_cb)(int)            = NULL;
/* GSEVO (Ghost Squad Evolution) io_glut_init() registers a joystick poll
 * callback via glutJoystickFunc(func, pollInterval). ABC never imports it, so
 * it was never stubbed -> GSEVO's JMP_SLOT bound to 0 -> CALL 0 crash inside
 * io_glut_init. Store the cb (joystick polling isn't needed to render; real
 * pad input still flows through the JVS path). */
static void (*g_joystick_cb)(unsigned int,int,int,int) = NULL;
static int    g_joystick_poll_ms                = 0;

/* The size the GAME renders at = its native design resolution (640x480).  The
 * loader's OUTPUT window is independent (1280x720, see sdl_window.c) and the
 * frame is up-scaled to fill it at present time.  Reporting the native size to
 * the game keeps every screen self-consistent (UI, attract, and 3D all compose
 * at 640x480) instead of mixing a forced 1280x720 with hardcoded-640 UI. */
static int  g_win_width  = 640;
static int  g_win_height = 480;
static bool g_game_mode  = false;
static bool g_initialized = false;

/* GLUT special key codes */
#define GLUT_KEY_F1   1
#define GLUT_KEY_F2   2
#define GLUT_KEY_F12  12
#define GLUT_KEY_LEFT  100
#define GLUT_KEY_UP    101
#define GLUT_KEY_RIGHT 102
#define GLUT_KEY_DOWN  103

/* GLUT Get constants */
#define GLUT_WINDOW_WIDTH         102
#define GLUT_WINDOW_HEIGHT        103
#define GLUT_WINDOW_X             100
#define GLUT_WINDOW_Y             101
#define GLUT_SCREEN_WIDTH         200
#define GLUT_SCREEN_HEIGHT        201
#define GLUT_ELAPSED_TIME         700
#define GLUT_GAME_MODE_ACTIVE      (0<<0)
#define GLUT_DISPLAY_MODE_POSSIBLE 400

/* ------------------------------------------------------------------ */
/* Win32 key -> GLUT translation                                       */
/* ------------------------------------------------------------------ */
static int vk_to_glut_special(int vk)
{
    switch (vk) {
    case VK_F1:  return GLUT_KEY_F1;
    case VK_F2:  return GLUT_KEY_F2;
    case VK_LEFT:  return GLUT_KEY_LEFT;
    case VK_UP:    return GLUT_KEY_UP;
    case VK_RIGHT: return GLUT_KEY_RIGHT;
    case VK_DOWN:  return GLUT_KEY_DOWN;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* GLUT stubs                                                          */
/* ------------------------------------------------------------------ */
void stub_glutInit(int *argc, char **argv)
{
    (void)argc; (void)argv;
    loader_log(LOG_INFO, "glutInit()");
    g_initialized = true;
    /* Window is created lazily on glutCreateWindow */
}

void stub_glutInitDisplayMode(unsigned int mode)
{
    loader_log(LOG_DEBUG, "glutInitDisplayMode(0x%x)", mode);
    /* We always use RGBA double-buffered with depth - ignore mode */
}

void stub_glutInitWindowSize(int w, int h)
{
    /* Honor the game's native request (640x480).  The output window stays
     * 1280x720 and the frame is up-scaled to fill it; reporting native keeps
     * every screen self-consistent. */
    if (w > 0 && h > 0) { g_win_width = w; g_win_height = h; }
    loader_log(LOG_INFO, "glutInitWindowSize(%d, %d) -> game renders at %dx%d (output up-scaled)",
               w, h, g_win_width, g_win_height);
}

void stub_glutInitWindowPosition(int x, int y)
{
    (void)x; (void)y;
    loader_log(LOG_DEBUG, "glutInitWindowPosition(%d,%d)", x, y);
}

int stub_glutCreateWindow(const char *title)
{
    loader_log(LOG_INFO, "glutCreateWindow(\"%s\") %dx%d",
               title ? title : "", g_win_width, g_win_height);
    /* SDL3 path: create a real SDL window + Mesa GL context (ported from the
     * SDL3 branch). This replaces the old Win32+WGL fake-X11 path. */
    extern int  sdl_window_create(const char *title);
    extern void sdl_window_set_size(int w, int h);
    sdl_window_set_size(g_win_width, g_win_height);
    if (sdl_window_create(title)) {
        loader_log(LOG_INFO, "glutCreateWindow -> SDL3 window created");
        return 1;
    }
    /* Fallback to the legacy WGL path if SDL isn't available. */
    loader_log(LOG_WARN, "glutCreateWindow -> SDL unavailable, falling back to WGL path");
    extern bool glx_ensure_init_size(int w, int h, const char *title);
    glx_ensure_init_size(g_win_width, g_win_height, title);
    return 1; /* window ID 1 */
}

void stub_glutDestroyWindow(int win) { (void)win; }

void stub_glutSetWindowTitle(const char *title)
{
    HWND hwnd = glx_get_hwnd();
    if (hwnd && title) SetWindowTextA(hwnd, title);
}

/* Callback registration */
void stub_glutDisplayFunc(void (*fn)(void))        { g_display_cb = fn;
    extern void sdl_window_set_display_cb(void *fn); sdl_window_set_display_cb((void*)fn); }
void stub_glutReshapeFunc(void (*fn)(int,int))     { g_reshape_cb     = fn; }
void stub_glutKeyboardFunc(void (*fn)(unsigned char,int,int)) { g_keyboard_cb = fn; }
void stub_glutKeyboardUpFunc(void (*fn)(unsigned char,int,int)) { g_keyboard_up_cb = fn; }
void stub_glutSpecialFunc(void (*fn)(int,int,int)) { g_special_cb     = fn; }
void stub_glutSpecialUpFunc(void (*fn)(int,int,int)){ g_special_up_cb = fn; }
void stub_glutMouseFunc(void (*fn)(int,int,int,int)){ g_mouse_cb      = fn; }
void stub_glutMotionFunc(void (*fn)(int,int))      { g_motion_cb      = fn; }
void stub_glutPassiveMotionFunc(void (*fn)(int,int)){ g_passive_cb    = fn; }
void stub_glutIdleFunc(void (*fn)(void))           { g_idle_cb        = fn; }
void stub_glutEntryFunc(void (*fn)(int))           { g_entry_cb       = fn; }
void stub_glutVisibilityFunc(void (*fn)(int))      { g_visibility_cb  = fn; }
void stub_glutJoystickFunc(void (*fn)(unsigned int,int,int,int), int poll_ms)
{ g_joystick_cb = fn; g_joystick_poll_ms = poll_ms; }

/* ------------------------------------------------------------------ */
/* GSEVO PC-input feed (drive the game's OWN GLUT callbacks)           */
/* ------------------------------------------------------------------ */
/* Ghost Squad Evolution has a complete PC/"xbox-board" input path that the
 * arcade JVS gun thread normally shadows. Under the loader the JVS gun thread
 * is starved (its 60Hz poll is gated on a recoil semaphore that never fires in
 * attract), so the credit loop stalls on the attract screen. GSEVO's GLUT
 * mouse/keyboard callbacks (glut_mouse_callback / glut_motion_callback /
 * glut_keyboard_callback) feed the SAME decoded switch + gun-aim state via the
 * game's native, encoding-correct path - but they are only invoked from the
 * Win32 message loop, which is DEAD when GSEVO runs on the SDL render path
 * (sdl_window_pump). So we synthesize those callback invocations once per frame
 * from the loader's input (lightgun service / auto-play). This delivers
 * aim + trigger + START through the game's own input merge with zero fragile
 * struct-poking - the switch bit encoding is whatever the game itself produces.
 *
 * These thin wrappers let jvs_input.c (which owns the input merge) call the
 * stored GSEVO callbacks without exposing the static pointers. All are no-ops
 * until the game has registered the corresponding callback, so they are inert
 * for every other title (ABC/LGJ register a display cb but no mouse/keyboard
 * gun callbacks). gx/gy are window-pixel coords (GSEVO normalizes them itself
 * against its reported window size). */
void glut_feed_motion(int gx, int gy)
{
    if (g_motion_cb)      g_motion_cb(gx, gy);
    else if (g_passive_cb) g_passive_cb(gx, gy);
}
void glut_feed_mouse(int button, int state, int gx, int gy)
{
    if (g_mouse_cb) g_mouse_cb(button, state, gx, gy);
}
void glut_feed_key(unsigned char key, int down)
{
    if (down) { if (g_keyboard_cb)    g_keyboard_cb(key, 0, 0); }
    else      { if (g_keyboard_up_cb) g_keyboard_up_cb(key, 0, 0); }
}
/* True only once the game has registered a mouse OR keyboard callback - the
 * signature that it actually drives a PC input path worth feeding. */
int glut_has_pc_input_cbs(void) { return (g_mouse_cb || g_keyboard_cb) ? 1 : 0; }

void stub_glutSwapBuffers(void)
{
    /* SDL path: signal the render loop to present. */
    extern void sdl_window_request_swap(void);
    sdl_window_request_swap();
    /* Legacy WGL fallback (only matters if SDL window wasn't created). */
    HDC hdc = glx_get_hdc();
    if (hdc) SwapBuffers(hdc);
}

void stub_glutPostRedisplay(void)
{
    HWND hwnd = glx_get_hwnd();
    if (hwnd) InvalidateRect(hwnd, NULL, FALSE);
}

void stub_glutSetCursor(int cursor) { (void)cursor; }

/* Game mode */
void stub_glutGameModeString(const char *str)
{
    loader_log(LOG_INFO, "glutGameModeString(\"%s\")", str ? str : "");
    /* Parse "WxH:BPP@HZ" */
    if (str) {
        int w=0, h=0, bpp=0, hz=0;
        if (sscanf(str, "%dx%d:%d@%d", &w, &h, &bpp, &hz) >= 2) {
            if (w > 0 && h > 0) {
                g_win_width  = w;
                g_win_height = h;
            }
        }
    }
}

int stub_glutEnterGameMode(void)
{
    loader_log(LOG_INFO, "glutEnterGameMode() %dx%d", g_win_width, g_win_height);
    g_game_mode = true;

    /* GSEVO (and other GLUT game-mode titles) enter here INSTEAD of
     * glutCreateWindow.  The legacy glx_ensure_init_size() path only resizes the
     * hidden 1x1 'LindberghPreInit' popup that glx_preinit() created - it never
     * brings up a real on-screen render window, never makes a healthy
     * render-target context current, never rebinds the GL GOT to the FBO-aware
     * shims (bind_gl_got_generic/patch_game_gl_got), and never runs
     * glx_emu_init().  The game's GOT therefore still points at the bare native
     * AMD opengl32 FBO/texture entrypoints, and the current context belongs to
     * the degenerate 1x1 preinit surface - so the very first FBO + depth
     * glTexImage2D render-target (init_viewport -> GLCreateFBuffer ->
     * GLCreateTexture) BLOCKS in the driver, with no [SDL3] render loop and no
     * present.
     *
     * Route game-mode entry through the SAME machinery the working SDL path
     * uses (exactly like stub_glutCreateWindow): create a real SDL OpenGL window
     * + context made current, rebind the GL GOT to FBO-aware shims, bring the
     * offscreen-FBO/GLX emulation online, and set g_sdl_active=1 so
     * stub_glutMainLoop takes the [SDL3] render loop (context current + event
     * pump around the display cb + present on glutSwapBuffers).  This is
     * additive and ABC-safe: ABC reaches glutCreateWindow, not
     * glutEnterGameMode, so its byte-identical path is untouched. */
    extern int  sdl_window_create(const char *title);
    extern void sdl_window_set_size(int w, int h);
    sdl_window_set_size(g_win_width, g_win_height);
    if (sdl_window_create("UltraLoader")) {
        loader_log(LOG_INFO, "glutEnterGameMode -> SDL3 window created");
    } else {
        /* Fallback to the legacy WGL path if SDL isn't available. */
        loader_log(LOG_WARN, "glutEnterGameMode -> SDL unavailable, falling back to WGL path");
        extern bool glx_ensure_init_size(int w, int h, const char *title);
        glx_ensure_init_size(g_win_width, g_win_height, "UltraLoader");
    }

    /* Call reshape callback with new size */
    if (g_reshape_cb) g_reshape_cb(g_win_width, g_win_height);
    return 1;
}

void stub_glutLeaveGameMode(void)
{
    g_game_mode = false;
}

int stub_glutGet(int type)
{
    switch (type) {
    case GLUT_WINDOW_WIDTH:    return g_win_width;
    case GLUT_WINDOW_HEIGHT:   return g_win_height;
    case GLUT_WINDOW_X:        return 0;
    case GLUT_WINDOW_Y:        return 0;
    /* Report the game's render surface, NOT the physical monitor - otherwise an
     * ultrawide desktop (e.g. 2560x1080) makes the game size its 3D pass to the
     * monitor and the frame composes inconsistently. */
    case GLUT_SCREEN_WIDTH:    return g_win_width;
    case GLUT_SCREEN_HEIGHT:   return g_win_height;
    case GLUT_ELAPSED_TIME:    { extern uint64_t vtime_ns(void); return (int)(vtime_ns()/1000000ULL); }  /* virtual ms (Snapshot-rewindable) */
    case GLUT_DISPLAY_MODE_POSSIBLE: return 1;
    default:                   return 0;
    }
}

/* Expose the GAME's logical render size (the size reported to the game via
 * glutGet / the reshape callback). GSEVO composites its frame into a
 * g_win_width x g_win_height sub-rectangle of the larger native offscreen FBO
 * (640x480 of 1360x768) and parks it in the bottom-left corner, so the present
 * path needs this to crop+stretch that corner to fill the window. ABC never
 * calls this (it uses the SDL/glutCreateWindow path with width==offscreen). */
void glut_get_logical_size(int *w, int *h) {
    if (w) *w = g_win_width;
    if (h) *h = g_win_height;
}

int stub_glutGetModifiers(void)
{
    int mods = 0;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= 1;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= 2;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= 4;
    return mods;
}

int stub_glutExtensionSupported(const char *ext)
{
    (void)ext;
    return 0; /* conservative */
}

/* GLUT bitmap font pointers - game passes these as void* */
static int g_glut_bitmap_9x15    = 9;
static int g_glut_bitmap_8x13    = 8;
static int g_glut_bitmap_times10 = 10;
static int g_glut_bitmap_times24 = 24;
static int g_glut_bitmap_helv10  = 110;
static int g_glut_bitmap_helv12  = 112;
static int g_glut_bitmap_helv18  = 118;

void *GLUT_BITMAP_9_BY_15    = &g_glut_bitmap_9x15;
void *GLUT_BITMAP_8_BY_13    = &g_glut_bitmap_8x13;
void *GLUT_BITMAP_TIMES_10   = &g_glut_bitmap_times10;
void *GLUT_BITMAP_TIMES_24   = &g_glut_bitmap_times24;
void *GLUT_BITMAP_HELVETICA_10 = &g_glut_bitmap_helv10;
void *GLUT_BITMAP_HELVETICA_12 = &g_glut_bitmap_helv12;
void *GLUT_BITMAP_HELVETICA_18 = &g_glut_bitmap_helv18;

void stub_glutBitmapCharacter(void *font, int c) { (void)font; (void)c; }
int  stub_glutBitmapWidth(void *font, int c)     { (void)font; (void)c; return 9; }

/* Geometric shapes */
void stub_glutSolidSphere(double r, int sl, int st) { (void)r;(void)sl;(void)st; }
void stub_glutWireSphere(double r, int sl, int st)  { (void)r;(void)sl;(void)st; }
void stub_glutSolidCube(double s)   { (void)s; }
void stub_glutWireCube(double s)    { (void)s; }
void stub_glutSolidCone(double b, double h, int sl, int st) { (void)b;(void)h;(void)sl;(void)st; }
void stub_glutWireCone(double b, double h, int sl, int st)  { (void)b;(void)h;(void)sl;(void)st; }
void stub_glutSolidTeapot(double s) { (void)s; }
void stub_glutWireTeapot(double s)  { (void)s; }

/* ------------------------------------------------------------------ */
/* glutMainLoop - the heart of it                                      */
/* The game calls this and expects it never to return.                 */
/* We run our own event loop that drives the game's display callback.  */
/* ------------------------------------------------------------------ */
void stub_glutMainLoop(void)
{
    loader_log(LOG_INFO, "glutMainLoop(), entering game loop");

    /* Call reshape once with initial size */
    if (g_reshape_cb) g_reshape_cb(g_win_width, g_win_height);

    /* SDL path: if a real SDL window was created, drive the SDL render loop
     * (pump events, call the game's display callback, present on swap). This
     * mirrors the SDL3 branch's working glutMainLoop. */
    {
        extern int g_sdl_ready_flag(void);
        extern int sdl_window_pump(void);
        extern int g_sdl_active;
        if (g_sdl_active) {
            loader_log(LOG_WARN, "[SDL3] entering render loop (idle_cb=%p display_cb=%p)",
                       (void*)g_idle_cb, (void*)g_display_cb);
            while (1) {
                if (g_idle_cb) g_idle_cb();
                if (!sdl_window_pump()) break;
            }
            loader_log(LOG_WARN, "[SDL3] render loop exited");
            ExitProcess(0);
        }
    }

    DWORD last_frame = GetTickCount();
    int   frame      = 0;

    while (1) {
        /* Pump Win32 messages */
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) ExitProcess(0);
            TranslateMessage(&msg);
            DispatchMessageA(&msg);

            /* Deliver keyboard events to game */
            if (msg.message == WM_KEYDOWN && g_keyboard_cb) {
                BYTE state[256];
                GetKeyboardState(state);
                char ch[2] = {0};
                if (ToAscii(msg.wParam, MapVirtualKey(msg.wParam,0),
                             state, (LPWORD)ch, 0) == 1) {
                    g_keyboard_cb((unsigned char)ch[0], 0, 0);
                }
            }
            if (msg.message == WM_KEYUP && g_keyboard_up_cb) {
                BYTE state[256];
                GetKeyboardState(state);
                char ch[2] = {0};
                if (ToAscii(msg.wParam, MapVirtualKey(msg.wParam,0),
                             state, (LPWORD)ch, 0) == 1) {
                    g_keyboard_up_cb((unsigned char)ch[0], 0, 0);
                }
            }
            if ((msg.message == WM_KEYDOWN || msg.message == WM_KEYUP) &&
                g_special_cb) {
                int key = vk_to_glut_special((int)msg.wParam);
                if (key) {
                    if (msg.message == WM_KEYDOWN) g_special_cb(key, 0, 0);
                    else if (g_special_up_cb) g_special_up_cb(key, 0, 0);
                }
            }
            if (msg.message == WM_SIZE && g_reshape_cb) {
                int w = LOWORD(msg.lParam);
                int h = HIWORD(msg.lParam);
                if (w > 0 && h > 0) {
                    g_win_width  = w;
                    g_win_height = h;
                    g_reshape_cb(w, h);
                }
            }
            if (msg.message == WM_LBUTTONDOWN && g_mouse_cb)
                g_mouse_cb(0, 0, LOWORD(msg.lParam), HIWORD(msg.lParam));
            if (msg.message == WM_LBUTTONUP && g_mouse_cb)
                g_mouse_cb(0, 1, LOWORD(msg.lParam), HIWORD(msg.lParam));
            if (msg.message == WM_MOUSEMOVE) {
                if (GetAsyncKeyState(VK_LBUTTON) && g_motion_cb)
                    g_motion_cb(LOWORD(msg.lParam), HIWORD(msg.lParam));
                else if (g_passive_cb)
                    g_passive_cb(LOWORD(msg.lParam), HIWORD(msg.lParam));
            }
        }

        /* Run idle callback if set */
        if (g_idle_cb) g_idle_cb();

        /* Run display callback */
        if (g_display_cb) g_display_cb();

        /* Cap at ~60fps if no vsync */
        DWORD now = GetTickCount();
        DWORD elapsed = now - last_frame;
        if (elapsed < 16) Sleep(16 - elapsed);
        last_frame = GetTickCount();
        frame++;
    }
}

/* ------------------------------------------------------------------ */
/* Register all GLUT stubs                                             */
/* ------------------------------------------------------------------ */
void glut_register_stubs(LoaderCtx *ctx)
{
    loader_log(LOG_INFO, "Registering GLUT stubs");

    #define R(name) sym_register(ctx, #name, (uintptr_t)stub_##name)
    #define RD(name, val) sym_register(ctx, #name, (uintptr_t)(val))

    R(glutInit);
    R(glutInitDisplayMode);
    R(glutInitWindowSize);
    R(glutInitWindowPosition);
    R(glutCreateWindow);
    R(glutDestroyWindow);
    R(glutSetWindowTitle);
    R(glutDisplayFunc);
    R(glutReshapeFunc);
    R(glutKeyboardFunc);
    R(glutKeyboardUpFunc);
    R(glutSpecialFunc);
    R(glutSpecialUpFunc);
    R(glutMouseFunc);
    R(glutMotionFunc);
    R(glutPassiveMotionFunc);
    R(glutIdleFunc);
    R(glutEntryFunc);
    R(glutVisibilityFunc);
    R(glutJoystickFunc);   /* GSEVO io_glut_init registers a joystick poll cb */
    R(glutSwapBuffers);
    R(glutPostRedisplay);
    R(glutSetCursor);
    R(glutGameModeString);
    R(glutEnterGameMode);
    R(glutLeaveGameMode);
    R(glutGet);
    R(glutGetModifiers);
    R(glutExtensionSupported);
    R(glutMainLoop);
    R(glutBitmapCharacter);
    R(glutBitmapWidth);
    R(glutSolidSphere); R(glutWireSphere);
    R(glutSolidCube);   R(glutWireCube);
    R(glutSolidCone);   R(glutWireCone);
    R(glutSolidTeapot); R(glutWireTeapot);

    /* Bitmap font data symbols */
    RD(glutBitmap9By15, &g_glut_bitmap_9x15);
    RD(glutBitmap8By13, &g_glut_bitmap_8x13);

    #undef R
    #undef RD
}

/* ------------------------------------------------------------------ */
/* Accessor used by glut_shim.c::stub_glutForceJoystickPoll to reach the
 * cb the game registered via glutJoystickFunc. Keeps g_joystick_cb file-
 * local while letting the shim's extras avoid a second dispatch table. */
typedef void (*GlutJoystickCb)(unsigned int, int, int, int);
GlutJoystickCb glut_stub_get_joystick_cb(void) { return g_joystick_cb; }
