/*
 * glx_bridge.c
 * OpenGL / GLX Semantic Bridge
 *
 * Lindbergh games use OpenGL via GLX (X11's OpenGL binding).
 * On Windows we have WGL. The semantic gap is significant:
 *
 *   GLX                    WGL
 *   ----                   ---
 *   Display*               HDC (device context)
 *   GLXDrawable            HWND / HDC
 *   GLXContext             HGLRC
 *   glXChooseVisual        ChoosePixelFormat
 *   glXCreateContext       wglCreateContext
 *   glXMakeCurrent         wglMakeCurrent
 *   glXSwapBuffers         SwapBuffers
 *   glXGetProcAddress      wglGetProcAddress / GetProcAddress
 *   XOpenDisplay           (we create a hidden Win32 window)
 *   XCreateWindow          (we use our hidden window)
 *
 * Strategy:
 *   1. At first glXChooseVisual/glXCreateContext call, create a hidden
 *      Win32 HWND and a WGL context on it.
 *   2. The game's window creation (via X11 XCreateWindow stub) maps to
 *      our visible Win32 window.
 *   3. All gl* function calls are forwarded to opengl32.dll via
 *      GetProcAddress / wglGetProcAddress.
 *   4. Extension strings are faked to report what the game needs.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "loader.h"
#include "glx_bridge.h"

/* ------------------------------------------------------------------ */
/* WGL function pointers (loaded dynamically from opengl32.dll)       */
/* ------------------------------------------------------------------ */
static HMODULE g_opengl32 = NULL;

typedef HGLRC  (WINAPI *pfn_wglCreateContext)(HDC);
typedef BOOL   (WINAPI *pfn_wglMakeCurrent)(HDC, HGLRC);
typedef BOOL   (WINAPI *pfn_wglDeleteContext)(HGLRC);
typedef PROC   (WINAPI *pfn_wglGetProcAddress)(LPCSTR);
typedef BOOL   (WINAPI *pfn_wglSwapIntervalEXT)(int);

static pfn_wglCreateContext    _wglCreateContext    = NULL;
static pfn_wglMakeCurrent      _wglMakeCurrent      = NULL;
static pfn_wglDeleteContext    _wglDeleteContext     = NULL;
static pfn_wglGetProcAddress   _wglGetProcAddress   = NULL;
static pfn_wglSwapIntervalEXT  _wglSwapIntervalEXT  = NULL;

/* ------------------------------------------------------------------ */
/* Window state                                                        */
/* ------------------------------------------------------------------ */
static HWND  g_hwnd   = NULL;
static HDC   g_hdc    = NULL;
static HGLRC g_hglrc  = NULL;
static int   g_width  = 1280;
static int   g_height = 720;


/* X11 resource id (XID: Window/Drawable/Colormap/Pixmap/Cursor/Atom/Time/Font)
 * is `unsigned long` in Xlib. THIS LOADER IS A 32-BIT PE (-m32), and the guest
 * is 32-bit i386 Linux where `unsigned long` == 4 bytes. The stubs originally
 * typed these params/returns as uint64_t (correct only for a 64-bit host),
 * which on a 32-bit cdecl call SHIFTS every following stack arg by 4 bytes.
 * Harmless for stubs that ignore their args, but FATAL for any stub that
 * dereferences a pointer arg - XQueryPointer wrote through a 4-byte-shifted
 * out-pointer (=> EAX=8 bad write at sWinProc_ChangeFullScreen's XQueryPointer
 * call, ret 0x08411BBC). Use a 32-bit XID so the stack layout matches the game.
 * Additive: ABC is GLUT (its only X import is XFree) - it calls none of these. */
typedef uint32_t XID;

/* Fake GLXContext/Drawable - page-mapped backing memory instead of bare
 * sentinels or raw HWND numerics. Root cause of the previous intermittency
 * (task #12): handing out (XID)g_hwnd meant any guest/ICD code that treated
 * the XID as a memory pointer read from kernel-handle bits. Some HWND values
 * land in mapped memory (spurious success), most don't (fault). A stable,
 * process-lifetime VirtualAlloc'd zero page is always mapped; any probe at
 * any offset within it returns zero. The emu-shim's IsBadReadPtr + GLX_EMU_MAGIC
 * check falls through cleanly (magic mismatch → NULL → default SDL swap),
 * and the NVIDIA ICD's "GLXE" probe at +0x3C reads zero (unrecognized → treat
 * as opaque, native path). Guaranteed no-fault on any deref. */
static void *g_fake_drawable_page = NULL;  /* one 4K page, zero-filled */
static void *g_fake_context_page  = NULL;  /* one 4K page, zero-filled */
static void ensure_fake_pages(void)
{
    if (!g_fake_drawable_page)
        g_fake_drawable_page = VirtualAlloc(NULL, 0x1000,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_fake_context_page)
        g_fake_context_page  = VirtualAlloc(NULL, 0x1000,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
#define FAKE_DRAWABLE  ((XID)(uintptr_t)g_fake_drawable_page)
#define FAKE_CONTEXT   (g_fake_context_page)

/* ------------------------------------------------------------------ *
 * REAL, populated fake Xlib _XDisplay (32-bit Xlib ABI).
 *
 * DIRECT-GLX titles (Let's Go Jungle) use Xlib MACROS - RootWindow,
 * DefaultScreen, DefaultDepth, DefaultVisual, ScreenCount, DefaultColormap,
 * BlackPixel/WhitePixel, DisplayWidth/Height - which expand to *direct
 * struct-field reads* off the _XDisplay / Screen / Visual the game holds.
 * The old opaque FAKE_DISPLAY (0xD15A0000) faulted the instant LGJ deref'd
 * Display->screens. We now hand back a fully-populated set of static
 * structs whose field OFFSETS exactly match the 32-bit Xlib ABI.
 *
 * Offsets verified by disassembling LGJ's sWinProc_MakeWindow /
 * sWinProc_MakeWindowSub / fgChooseVisual:
 *   _XDisplay: default_screen @ +0x84, nscreens @ +0x88, screens @ +0x8c
 *              (game does `mov 0x84(dpy),idx; lea (idx,idx,4); shl 4` =>
 *               idx * 80 => sizeof(Screen)==0x50, then `screens[idx]`)
 *   Screen:    root @ +0x08 (passed to XCreateColormap/XCreateWindow),
 *              width @ +0x0c, height @ +0x10, mwidth @ +0x14, mheight @ +0x18
 *              => sizeof(Screen) == 0x50 (80 bytes)
 *
 * ADDITIVE: ABC never imports XOpenDisplay (verified at .dynsym), so this
 * struct is only ever reachable through stub_XOpenDisplay - a dead path for
 * ABC, whose glXGetCurrentDisplay goes through glx_emu.c's own g_glx.display.
 * ------------------------------------------------------------------ */

/* 32-bit Xlib Visual ABI (sizeof == 0x20). */
typedef struct FakeXVisual {
    void    *ext_data;      /* +0x00 XExtData* */
    uint32_t visualid;      /* +0x04 VisualID  */
    int32_t  c_class;       /* +0x08 int class (4 == TrueColor) */
    uint32_t red_mask;      /* +0x0c */
    uint32_t green_mask;    /* +0x10 */
    uint32_t blue_mask;     /* +0x14 */
    int32_t  bits_per_rgb;  /* +0x18 */
    int32_t  map_entries;   /* +0x1c */
} FakeXVisual;

/* 32-bit Xlib Depth ABI (sizeof == 0x0c). */
typedef struct FakeXDepth {
    int32_t  depth;         /* +0x00 */
    int32_t  nvisuals;      /* +0x04 */
    void    *visuals;       /* +0x08 Visual* */
} FakeXDepth;

/* 32-bit Xlib Screen ABI (sizeof == 0x50). */
typedef struct FakeXScreen {
    void    *ext_data;        /* +0x00 XExtData* */
    void    *display;         /* +0x04 struct _XDisplay* */
    uint32_t root;            /* +0x08 Window  <-- read by the game */
    int32_t  width;           /* +0x0c */
    int32_t  height;          /* +0x10 */
    int32_t  mwidth;          /* +0x14 mm */
    int32_t  mheight;         /* +0x18 mm */
    int32_t  ndepths;         /* +0x1c */
    void    *depths;          /* +0x20 Depth* */
    int32_t  root_depth;      /* +0x24 */
    void    *root_visual;     /* +0x28 Visual* */
    void    *default_gc;      /* +0x2c GC */
    uint32_t cmap;            /* +0x30 Colormap */
    uint32_t white_pixel;     /* +0x34 */
    uint32_t black_pixel;     /* +0x38 */
    int32_t  max_maps;        /* +0x3c */
    int32_t  min_maps;        /* +0x40 */
    int32_t  backing_store;   /* +0x44 */
    int32_t  save_unders;     /* +0x48 Bool */
    int32_t  root_input_mask; /* +0x4c long */
} FakeXScreen;                /* sizeof == 0x50 */

/* 32-bit Xlib _XDisplay ABI. Only the fields the game's macros read need
 * correct offsets; everything else is benign zero padding. We size it to
 * 0x3C0 (well past every documented field) so any stray macro read lands on
 * valid mapped zero bytes rather than faulting. */
typedef struct FakeXDisplay {
    void    *ext_data;        /* +0x00 */
    void    *private1;        /* +0x04 free_funcs */
    int32_t  fd;              /* +0x08 */
    int32_t  private2;        /* +0x0c */
    int32_t  proto_major;     /* +0x10 */
    int32_t  proto_minor;     /* +0x14 */
    char    *vendor;          /* +0x18 */
    uint8_t  pad_1c[0x84 - 0x1c]; /* +0x1c .. +0x83 (resource ids, byte order, etc.) */
    int32_t  default_screen;  /* +0x84 */
    int32_t  nscreens;        /* +0x88 */
    void    *screens;         /* +0x8c Screen* */
    uint8_t  pad_90[0x3C0 - 0x90]; /* +0x90 .. end: motion_buffer, flags, etc. */
} FakeXDisplay;

/* The struct field offsets are LOAD-BEARING: LGJ's Xlib MACROS read these
 * exact byte offsets directly off the structs (no accessor functions). A
 * stray field edit that shifts an offset would silently re-introduce the
 * sWinProc_MakeWindow 0x08412546 fault. Pin every disassembly-anchored offset
 * with a compile-time assert so the build breaks instead of the game.
 * (offsets verified against lgj_final disassembly - see comment block above.) */
#define UL_ASSERT_OFF(type, field, off) \
    _Static_assert(__builtin_offsetof(type, field) == (off), \
                   #type "." #field " must be at offset " #off)
UL_ASSERT_OFF(FakeXDisplay, default_screen, 0x84);
UL_ASSERT_OFF(FakeXDisplay, nscreens,       0x88);
UL_ASSERT_OFF(FakeXDisplay, screens,        0x8c);
UL_ASSERT_OFF(FakeXScreen,  root,           0x08);
UL_ASSERT_OFF(FakeXScreen,  width,          0x0c);
UL_ASSERT_OFF(FakeXScreen,  height,         0x10);
UL_ASSERT_OFF(FakeXScreen,  mwidth,         0x14);
UL_ASSERT_OFF(FakeXScreen,  mheight,        0x18);
UL_ASSERT_OFF(FakeXScreen,  ndepths,        0x1c);
UL_ASSERT_OFF(FakeXScreen,  depths,         0x20);
UL_ASSERT_OFF(FakeXScreen,  root_depth,     0x24);
UL_ASSERT_OFF(FakeXScreen,  root_visual,    0x28);
UL_ASSERT_OFF(FakeXScreen,  cmap,           0x30);
UL_ASSERT_OFF(FakeXScreen,  white_pixel,    0x34);
UL_ASSERT_OFF(FakeXScreen,  black_pixel,    0x38);
_Static_assert(sizeof(FakeXScreen) == 0x50, "FakeXScreen must be 0x50 (game indexes screens[i] with i*0x50)");
UL_ASSERT_OFF(FakeXVisual,  c_class,        0x08);
UL_ASSERT_OFF(FakeXVisual,  red_mask,       0x0c);
UL_ASSERT_OFF(FakeXVisual,  green_mask,     0x10);
UL_ASSERT_OFF(FakeXVisual,  blue_mask,      0x14);

static FakeXVisual g_fake_xvisual = {
    .ext_data     = NULL,
    .visualid     = 0x21,
    .c_class      = 4,            /* TrueColor */
    .red_mask     = 0x00FF0000,
    .green_mask   = 0x0000FF00,
    .blue_mask    = 0x000000FF,
    .bits_per_rgb = 8,
    .map_entries  = 256,
};

static FakeXDepth g_fake_xdepth = {
    .depth    = 24,
    .nvisuals = 1,
    .visuals  = &g_fake_xvisual,
};

static FakeXScreen g_fake_xscreen = {
    .ext_data        = NULL,
    .display         = NULL,      /* back-patched to &g_fake_xdisplay at open */
    .root            = 0,          /* patched by fake_display_init() to the page-mapped drawable */
    .width           = 1360,      /* LGJ profile res; back-patched at open */
    .height          = 768,
    .mwidth          = 360,       /* ~96 dpi at 1360 */
    .mheight         = 203,
    .ndepths         = 1,
    .depths          = &g_fake_xdepth,
    .root_depth      = 24,
    .root_visual     = &g_fake_xvisual,
    .default_gc      = (void*)0x6C000001,
    .cmap            = 0xC010,
    .white_pixel     = 0x00FFFFFF,
    .black_pixel     = 0x00000000,
    .max_maps        = 256,
    .min_maps        = 256,
    .backing_store   = 0,         /* NotUseful */
    .save_unders     = 0,
    .root_input_mask = 0,
};

static FakeXDisplay g_fake_xdisplay = {
    .fd             = -1,
    .proto_major    = 11,
    .proto_minor    = 0,
    .vendor         = "Ultra Loader (fake X)",
    .default_screen = 0,
    .nscreens       = 1,
    .screens        = &g_fake_xscreen,
};

#define FAKE_DISPLAY   ((void*)&g_fake_xdisplay)

/* Populate the cross-links + current resolution into the fake display tree.
 * Idempotent; called from stub_XOpenDisplay (and any path that may hand the
 * display out before that). */
static void fake_display_init(void)
{
    /* Ensure the fake XID / context backing pages exist. Idempotent. */
    ensure_fake_pages();
    g_fake_xscreen.display = &g_fake_xdisplay;
    /* Mint the "root Window" as our page-mapped drawable - safe for any
     * guest/ICD deref (zeroed page, no fault possible). Falls back to a
     * bare sentinel only in the impossible case that VirtualAlloc failed. */
    g_fake_xscreen.root       = g_fake_drawable_page
        ? (uint32_t)(uintptr_t)g_fake_drawable_page
        : (uint32_t)0xD4000001;
    /* .default_gc is legitimately a *pointer* in Xlib - hand out a page
     * (the drawable page is fine to share; nothing reads it as anything
     * other than opaque) rather than a bare id. */
    g_fake_xscreen.default_gc = g_fake_drawable_page
        ? (void*)g_fake_drawable_page
        : (void*)0x6C000001;
    /* .cmap is an XID (integer). Point at the same page - same rationale. */
    g_fake_xscreen.cmap = g_fake_drawable_page
        ? (uint32_t)(uintptr_t)g_fake_drawable_page
        : (uint32_t)0xC010;
    if (g_width  > 0) g_fake_xscreen.width  = g_width;
    if (g_height > 0) g_fake_xscreen.height = g_height;
    /* keep mm size roughly ~96dpi so any DPI math the game does is sane */
    g_fake_xscreen.mwidth  = (g_fake_xscreen.width  * 254) / (96 * 10);
    g_fake_xscreen.mheight = (g_fake_xscreen.height * 254) / (96 * 10);
}

/* The game may DEREFERENCE the visual / FBConfig it gets back (reading fields
 * like depth/visualid off it), so these cannot be bare sentinel values - they
 * must point at real, mapped, mostly-zero memory. Layout mirrors X11's
 * XVisualInfo (visual*, visualid, screen, depth, class, masks, ...) with extra
 * slack so any field a GL helper reads lands on valid zero bytes, not a fault. */
typedef struct {
    void    *visual;        /* +0  */
    uint32_t visualid;      /* +4  <-- the read that was crashing (mov edx,[ebx+4]) */
    int32_t  screen;        /* +8  */
    int32_t  depth;         /* +12 */
    int32_t  c_class;       /* +16 */
    uint32_t red_mask;      /* +20 */
    uint32_t green_mask;    /* +24 */
    uint32_t blue_mask;     /* +28 */
    int32_t  colormap_size; /* +32 */
    int32_t  bits_per_rgb;  /* +36 */
    uint8_t  slack[208];    /* pad to 256 bytes total */
} FakeVisualInfo;

static FakeVisualInfo g_fake_visual = {
    .visual        = (void*)&g_fake_xvisual, /* real bare Visual (Xlib ABI) */
    .visualid      = 0x21,
    .screen        = 0,
    .depth         = 24,
    .c_class       = 4,          /* TrueColor */
    .red_mask      = 0x00FF0000,
    .green_mask    = 0x0000FF00,
    .blue_mask     = 0x000000FF,
    .colormap_size = 256,
    .bits_per_rgb  = 8,
};
#define FAKE_VISUAL    ((void*)&g_fake_visual)

/* FBConfig: the game indexes the returned list ([0]) and may deref that entry.
 * Point the single list slot at the same dereferenceable visual buffer. */
static void *g_fake_fbconfig_list[1] = { (void*)&g_fake_visual };

/* ------------------------------------------------------------------ */
/* Win32 window creation                                               */
/* ------------------------------------------------------------------ */
/* Notify the lightgun service when the game window's screen rect changes, so
 * raw-input pixel→[0,1] math stays correct for windowed/multi-monitor/DPI
 * setups. Declared in lightgun_service.h. Cheap; only fires on real changes. */
extern void lightgun_set_surface(int x0, int y0, int w, int h);

static void glx_update_lightgun_surface(HWND hwnd) {
    RECT rc; if (!GetClientRect(hwnd, &rc)) return;
    POINT tl = {0, 0}; ClientToScreen(hwnd, &tl);
    lightgun_set_surface(tl.x, tl.y, rc.right - rc.left, rc.bottom - rc.top);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) PostQuitMessage(0);
        break;
    case WM_SIZE:
        g_width  = LOWORD(lp);
        g_height = HIWORD(lp);
        glx_update_lightgun_surface(hwnd);
        break;
    case WM_MOVE:
    case WM_DPICHANGED:
        glx_update_lightgun_surface(hwnd);
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static bool create_window(int w, int h, const char *title)
{
    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "LindberghLoader";
    RegisterClassExA(&wc);

    DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    RECT r = {0, 0, w, h};
    AdjustWindowRect(&r, style, FALSE);

    g_hwnd = CreateWindowExA(0, "LindberghLoader",
                               title ? title : "Lindbergh",
                               style, CW_USEDEFAULT, CW_USEDEFAULT,
                               r.right - r.left, r.bottom - r.top,
                               NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!g_hwnd) {
        loader_log(LOG_ERROR, "CreateWindow failed: %lu", GetLastError());
        return false;
    }
    g_hdc = GetDC(g_hwnd);
    loader_log(LOG_INFO, "Win32 window created %dx%d", w, h);
    /* Seed the lightgun surface immediately so the first event uses the
     * game-window rect, not the stale primary-monitor default from init. */
    glx_update_lightgun_surface(g_hwnd);
    return true;
}

static bool setup_wgl(void)
{
    g_opengl32 = LoadLibraryA("opengl32.dll");
    if (!g_opengl32) {
        loader_log(LOG_ERROR, "opengl32.dll not found");
        return false;
    }

    _wglCreateContext  = (pfn_wglCreateContext)  GetProcAddress(g_opengl32, "wglCreateContext");
    _wglMakeCurrent    = (pfn_wglMakeCurrent)    GetProcAddress(g_opengl32, "wglMakeCurrent");
    _wglDeleteContext  = (pfn_wglDeleteContext)   GetProcAddress(g_opengl32, "wglDeleteContext");
    _wglGetProcAddress = (pfn_wglGetProcAddress)  GetProcAddress(g_opengl32, "wglGetProcAddress");

    /* Set pixel format */
    PIXELFORMATDESCRIPTOR pfd = {
        sizeof(PIXELFORMATDESCRIPTOR), 1,
        PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        PFD_TYPE_RGBA, 32,
        0,0,0,0,0,0,0,0,0,0,0,0,0,
        24, 8, 0, PFD_MAIN_PLANE, 0, 0,0,0
    };

    int fmt = ChoosePixelFormat(g_hdc, &pfd);
    if (!fmt || !SetPixelFormat(g_hdc, fmt, &pfd)) {
        loader_log(LOG_ERROR, "SetPixelFormat failed: %lu", GetLastError());
        return false;
    }

    g_hglrc = _wglCreateContext(g_hdc);
    if (!g_hglrc) {
        loader_log(LOG_ERROR, "wglCreateContext failed: %lu", GetLastError());
        return false;
    }

    if (!_wglMakeCurrent(g_hdc, g_hglrc)) {
        loader_log(LOG_ERROR, "wglMakeCurrent failed");
        return false;
    }

    /* Try to get wglSwapIntervalEXT for vsync control */
    if (_wglGetProcAddress)
        _wglSwapIntervalEXT = (pfn_wglSwapIntervalEXT)
            _wglGetProcAddress("wglSwapIntervalEXT");

    loader_log(LOG_INFO, "WGL context initialized");
    return true;
}

static bool g_initialized = false;

static bool glx_ensure_init(void)
{
    if (g_initialized) return true;
    if (!g_hwnd && !create_window(g_width, g_height, "Lindbergh")) return false;
    if (!g_hglrc && !setup_wgl()) return false;
    g_initialized = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* X11 stub layer (Lindbergh games open X11 displays)                 */
/* ------------------------------------------------------------------ */
void *stub_XOpenDisplay(const char *name)
{
    glx_ensure_init();          /* creates the Win32 window -> sets g_width/g_height */
    fake_display_init();        /* cross-link the fake _XDisplay tree at the real res */
    loader_log(LOG_INFO,
        "XOpenDisplay(\"%s\") -> fake _XDisplay %p (screen0 %dx%d root=0x%x depth=%d)",
        name ? name : ":0", FAKE_DISPLAY,
        g_fake_xscreen.width, g_fake_xscreen.height,
        g_fake_xscreen.root, g_fake_xscreen.root_depth);
    return FAKE_DISPLAY;
}

int stub_XCloseDisplay(void *display)
{
    return 0;
}

XID stub_XCreateColormap(void *dpy, XID window, void *visual, int alloc)
{
    /* Ensure the fake _XDisplay tree is real-handle-populated before we
     * hand out an id the caller may use in Xlib-macro contexts. */
    glx_ensure_init();
    fake_display_init();
    return (XID)0xC010; /* fake colormap */
}

XID stub_XCreateWindow(void *dpy, XID parent,
                              int x, int y, unsigned w, unsigned h,
                              unsigned border, int depth, unsigned cls,
                              void *visual, unsigned long mask, void *attrs)
{
    loader_log(LOG_INFO, "XCreateWindow %dx%d", w, h);
    g_width = w; g_height = h;

    /* If glx_preinit() already ran and the current window is the tiny 1x1
     * "LindberghPreInit" helper created for WGL extension resolution, tear it
     * down so glx_ensure_init() below can create a real game-sized window.
     *
     * Without this, glx_ensure_init() short-circuits (g_initialized=true) and
     * only SetWindowPos is called - the HWND is resized to game dimensions but
     * the WGL pixel format and context stay bound to the original 1x1 surface.
     * SwapBuffers on that context deadlocks the NVIDIA driver: it tries to vsync
     * a surface whose pixel format was chosen for a 1x1 window.  Hummer Extreme
     * hangs at frame 1 exactly here, watchdog fires at 40+ seconds, then crashes. */
    if (g_initialized && g_hwnd && (int)w > 1 && (int)h > 1) {
        char cls_name[64] = {0};
        GetClassNameA(g_hwnd, cls_name, sizeof(cls_name));
        if (strcmp(cls_name, "LindberghPreInit") == 0) {
            loader_log(LOG_INFO, "[GLX] XCreateWindow: replacing 1x1 pre-init window with %dx%d game window", w, h);
            if (_wglMakeCurrent)               _wglMakeCurrent(NULL, NULL);
            if (_wglDeleteContext && g_hglrc) { _wglDeleteContext(g_hglrc); g_hglrc = NULL; }
            if (g_hdc)                        { ReleaseDC(g_hwnd, g_hdc);  g_hdc   = NULL; }
            if (g_hwnd)                       { DestroyWindow(g_hwnd);      g_hwnd  = NULL; }
            g_initialized = false;
        }
    }

    /* Full init (window + WGL) so g_hwnd/g_hglrc are guaranteed non-NULL
     * on return, and the fake _XDisplay tree gets rewritten with the real
     * HWND numeric as .root. */
    glx_ensure_init();
    if (g_hwnd)
        SetWindowPos(g_hwnd, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
    fake_display_init();
    /* Return the page-mapped drawable as the "Window" XID. Safe for guest/ICD
     * dereferences (unlike raw g_hwnd, which faults when treated as pointer). */
    return FAKE_DRAWABLE;
}

int stub_XMapWindow(void *dpy, XID window)
{
    if (g_hwnd) ShowWindow(g_hwnd, SW_SHOW);
    return 0;
}

int stub_XMapRaised(void *dpy, XID window)  { return stub_XMapWindow(dpy, window); }
int stub_XUnmapWindow(void *dpy, XID window) {
    if (g_hwnd) ShowWindow(g_hwnd, SW_HIDE);
    return 0;
}

int stub_XDestroyWindow(void *dpy, XID window) { return 0; }
int stub_XFlush(void *dpy)   { return 0; }
int stub_XSync(void *dpy, int discard) { return 0; }

/* Xlib display/screen query functions (the non-macro entry points). These
 * read CONSISTENT values out of the same fake _XDisplay/Screen the game's
 * Xlib MACROS read directly, so a title that mixes macros + functions sees
 * one coherent screen. Additive: ABC imports none of these. */
uint64_t stub_XDefaultRootWindow(void *dpy) { (void)dpy; return g_fake_xscreen.root; }
int      stub_XDefaultScreen(void *dpy)     { (void)dpy; return g_fake_xdisplay.default_screen; }
uint64_t stub_XRootWindow(void *dpy, int screen)   { (void)dpy; (void)screen; return g_fake_xscreen.root; }
void    *stub_XDefaultVisual(void *dpy, int screen){ (void)dpy; (void)screen; return g_fake_xscreen.root_visual; }
int      stub_XDefaultDepth(void *dpy, int screen) { (void)dpy; (void)screen; return g_fake_xscreen.root_depth; }
uint64_t stub_XDefaultColormap(void *dpy, int scr) { (void)dpy; (void)scr; return g_fake_xscreen.cmap; }
void    *stub_XDefaultGC(void *dpy, int screen)    { (void)dpy; (void)screen; return g_fake_xscreen.default_gc; }
void    *stub_XScreenOfDisplay(void *dpy, int scr) { (void)dpy; (void)scr; return &g_fake_xscreen; }
void    *stub_XDefaultScreenOfDisplay(void *dpy)   { (void)dpy; return &g_fake_xscreen; }
int      stub_XScreenCount(void *dpy)              { (void)dpy; return g_fake_xdisplay.nscreens; }
int      stub_XDisplayWidth(void *dpy, int screen) { (void)dpy; (void)screen; return g_fake_xscreen.width; }
int      stub_XDisplayHeight(void *dpy, int screen){ (void)dpy; (void)screen; return g_fake_xscreen.height; }
int      stub_XDisplayWidthMM(void *dpy, int scr)  { (void)dpy; (void)scr; return g_fake_xscreen.mwidth; }
int      stub_XDisplayHeightMM(void *dpy, int scr) { (void)dpy; (void)scr; return g_fake_xscreen.mheight; }
uint64_t stub_XBlackPixel(void *dpy, int screen)   { (void)dpy; (void)screen; return g_fake_xscreen.black_pixel; }
uint64_t stub_XWhitePixel(void *dpy, int screen)   { (void)dpy; (void)screen; return g_fake_xscreen.white_pixel; }
int      stub_XDefaultDepthOfScreen(void *scr)     { (void)scr; return g_fake_xscreen.root_depth; }
uint64_t stub_XRootWindowOfScreen(void *scr)       { (void)scr; return g_fake_xscreen.root; }
int      stub_XConnectionNumber(void *dpy)         { (void)dpy; return g_fake_xdisplay.fd; }

int stub_XPending(void *dpy)
{
    /* Pump Win32 messages and report pending count */
    MSG msg;
    int count = 0;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_NOREMOVE)) count++;
    return count;
}

/* Fake XEvent structure (minimal) */
typedef struct {
    int type;
    uint64_t window;
    int x, y;
    unsigned int width, height;
    int keycode;
    uint64_t button;
} XEvent;

#define KeyPress        2
#define KeyRelease      3
#define ButtonPress     4
#define ButtonRelease   5
#define MotionNotify    6
#define ConfigureNotify 22
#define ClientMessage   33

int stub_XNextEvent(void *dpy, XEvent *event)
{
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (msg.message == WM_KEYDOWN) {
            event->type    = KeyPress;
            event->keycode = (int)msg.wParam;
            return 0;
        }
        if (msg.message == WM_KEYUP) {
            event->type    = KeyRelease;
            event->keycode = (int)msg.wParam;
            return 0;
        }
        if (msg.message == WM_QUIT) {
            ExitProcess(0);
        }
    }
    return 0;
}

int stub_XCheckMaskEvent(void *dpy, long mask, XEvent *event)
{
    MSG msg;
    if (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (msg.message == WM_QUIT) ExitProcess(0);
        if (msg.message == WM_KEYDOWN) {
            event->type = KeyPress; event->keycode = (int)msg.wParam; return 1;
        }
    }
    return 0;
}

int stub_XStoreName(void *dpy, XID window, const char *title)
{
    if (g_hwnd) SetWindowTextA(g_hwnd, title);
    return 0;
}

void *stub_XInternAtom(void *dpy, const char *name, int only_if_exists)
{
    glx_ensure_init();
    fake_display_init();
    return (void*)0xA00; /* fake atom */
}

int stub_XSetWMProtocols(void *dpy, XID window, void **protos, int count)
{
    return 1;
}

/* ------------------------------------------------------------------ *
 * Extended X11 stubs for DIRECT-GLX titles (Let's Go Jungle uses raw
 * Xlib + Cg, not GLUT). ABC uses GLUT and imports NONE of these (verified
 * from its .dynsym), so every stub here is purely additive for ABC.
 * The first one LGJ needed was XSetErrorHandler - sWinProc_MakeWindow
 * calls it before XOpenDisplay; its unbound GOT slot (NULL) made the
 * game call 0 during _sScene::initializeGraph -> sWinProc_MakeWindow.
 * These return benign success/sane values: we run on a hidden Win32
 * window (create_window) + WGL, so X11 server ops are no-ops, while
 * geometry/visual queries report the values our window actually uses. */

/* error handlers: store + return the previous handler (Xlib semantics). */
static void *g_x_err_handler = NULL;
static void *g_x_ioerr_handler = NULL;
void *stub_XSetErrorHandler(void *handler)   { void *p = g_x_err_handler;   g_x_err_handler   = handler; return p; }
void *stub_XSetIOErrorHandler(void *handler) { void *p = g_x_ioerr_handler; g_x_ioerr_handler = handler; return p; }
int   stub_XGetErrorText(void *dpy, int code, char *buf, int len)
{
    if (buf && len > 0) { _snprintf(buf, len, "X error %d", code); buf[len-1]=0; }
    return 0;
}
int   stub_XGetErrorDatabaseText(void *dpy, const char *name, const char *msg,
                                 const char *dflt, char *buf, int len)
{
    if (buf && len > 0) { strncpy(buf, dflt ? dflt : "", len-1); buf[len-1]=0; }
    return 0;
}

/* threading: Xlib MT init - we serialize via Win32 already; report success. */
int  stub_XInitThreads(void)            { return 1; }
void stub_XLockDisplay(void *dpy)       { (void)dpy; }
void stub_XUnlockDisplay(void *dpy)     { (void)dpy; }

/* close-down / focus / window placement (all no-ops on our single window) */
int  stub_XSetCloseDownMode(void *dpy, int mode) { (void)dpy;(void)mode; return 0; }
int  stub_XSetInputFocus(void *dpy, XID w, int revert, XID t) { return 0; }
int  stub_XMoveWindow(void *dpy, XID w, int x, int y) { return 0; }
int  stub_XMoveResizeWindow(void *dpy, XID w, int x, int y, unsigned ww, unsigned wh) { return 0; }
int  stub_XResizeWindow(void *dpy, XID w, unsigned ww, unsigned wh) { return 0; }
int  stub_XRaiseWindow(void *dpy, XID w) { return 0; }

/* pointer / keyboard grab + warp - no cursor capture in our hidden-window model */
int  stub_XGrabKeyboard(void *dpy, XID w, int oe, int pm, int km, XID t)   { return 0; /* GrabSuccess */ }
int  stub_XUngrabKeyboard(void *dpy, XID t)                                { return 0; }
int  stub_XGrabPointer(void *dpy, XID w, int oe, unsigned mask, int pm, int km,
                       XID conf, XID cur, XID t)                           { return 0; }
int  stub_XUngrabPointer(void *dpy, XID t)                                 { return 0; }
int  stub_XWarpPointer(void *dpy, XID src, XID dst, int sx, int sy,
                       unsigned sw, unsigned sh, int dx, int dy)           { return 0; }
int  stub_XAutoRepeatOn(void *dpy)  { return 0; }
int  stub_XAutoRepeatOff(void *dpy) { return 0; }

/* cursor / pixmap (hidden window: report fake non-zero ids) */
XID stub_XCreatePixmap(void *dpy, XID d, unsigned w, unsigned h, unsigned depth) {
    glx_ensure_init(); fake_display_init(); return 0xB1;
}
XID stub_XCreatePixmapFromBitmapData(void *dpy, XID d, char *data, unsigned w,
                                          unsigned h, unsigned long fg, unsigned long bg, unsigned depth) {
    glx_ensure_init(); fake_display_init(); return 0xB2;
}
XID stub_XCreatePixmapCursor(void *dpy, XID src, XID mask, void *fg,
                                  void *bg, unsigned x, unsigned y) {
    glx_ensure_init(); fake_display_init(); return 0xC0;
}
int  stub_XDefineCursor(void *dpy, XID w, XID cursor) { return 0; }
int  stub_XUndefineCursor(void *dpy, XID w) { return 0; }
int  stub_XFreeCursor(void *dpy, XID cursor) { return 0; }
int  stub_XFreePixmap(void *dpy, XID pix) { return 0; }
int  stub_XParseColor(void *dpy, XID cmap, const char *spec, void *exact) { return 1; }

/* geometry / pointer / coordinate queries - report our real window size
 * (g_width/g_height are this file's module-level statics defined at the top).
 * XID args are 4 bytes on 32-bit; the out-pointers MUST land at the right
 * stack offsets or we write through garbage (this stub was the crash site). */
int stub_XQueryPointer(void *dpy, XID w, XID *root, XID *child,
                       int *rx, int *ry, int *wx, int *wy, unsigned *mask)
{
    if (root) *root=0; if (child) *child=0;
    if (rx)*rx=0; if (ry)*ry=0; if (wx)*wx=0; if (wy)*wy=0; if (mask)*mask=0;
    return 1; /* True: pointer on same screen */
}
int stub_XTranslateCoordinates(void *dpy, XID src, XID dst, int sx, int sy,
                               int *dx, int *dy, XID *child)
{
    if (dx)*dx=sx; if (dy)*dy=sy; if (child)*child=0;
    return 1;
}
int stub_XLookupString(void *ev, char *buf, int n, void *keysym, void *status)
{
    if (buf && n>0) buf[0]=0;
    return 0;
}

/* WM property helpers (no window manager in our model) */
int  stub_XSetWMProperties(void *dpy, XID w, void *wname, void *iname, char **argv,
                           int argc, void *normal, void *wmh, void *clh) { return 0; }
int  stub_XStringListToTextProperty(char **list, int count, void *tp) { return 1; }
void stub_XMissingExtension(void *dpy, const char *name) { (void)dpy;(void)name; }

/* Xext display-list bookkeeping (used by GLX/Cg's libXext + the game's OWN
 * statically-linked libXxf86vm). The XFree86-VidMode extension funnels every
 * call through find_display(dpy) -> XextCreateExtension / XextFindDisplay /
 * XextAddDisplay, and treats a NON-NULL return as a real XExtDisplayInfo* that
 * it then DEREFERENCES (record->codes at +8, then record->codes->major_opcode
 * etc.) before emitting a real X11 wire request via _XReply.
 *
 * We are headless (no X server, fake _XDisplay), so we must make find_display
 * return NULL: that drives the game down its graceful `XMissingExtension`
 * branch (e.g. XF86VidModeGetViewPort returns 0 / fills 0,0) instead of
 * dereferencing a bogus record and/or blocking on an X protocol round-trip.
 *   - XextFindDisplay -> NULL  (no cached per-display record)
 *   - XextAddDisplay  -> NULL  (CRITICAL: returning non-NULL here made
 *                        find_display hand back e.g. 1, and the VidMode code
 *                        did `mov 0x8(eax),eax` => read addr 9 => crash at
 *                        0x084F7FCE. NULL = "extension unavailable", clean.)
 * XextCreateExtension may stay non-NULL: it's only used as the opaque handle
 * threaded into Find/Add/Remove, never dereferenced by the game on our path.
 * Additive: ABC imports none of these (GLUT title, no XF86VidMode). */
void *stub_XextCreateExtension(void) { static int e; return &e; }
int   stub_XextAddDisplay(void *ext, void *dpy, char *name, void *hooks, int nev, void *data) { return 0; }
int   stub_XextFindDisplay(void *ext, void *dpy) { return 0; }
int   stub_XextRemoveDisplay(void *ext, void *dpy) { return 1; }

/* ---- XF86VidMode: FUNCTION-form stubs -------------------------------
 * The comment above says find_display() returns NULL (via our
 * XextFindDisplay/XextAddDisplay stubs), which drives the game's static
 * libXxf86vm down its `XMissingExtension` graceful branch. But that
 * graceful path is inside the static libXxf86vm code - it only runs when
 * the guest CALLS the XF86VidMode* API entry. Some Lindbergh titles
 * (Hummer) instead import XF86VidMode* as UNDEFINED external symbols,
 * so an unresolved-symbol PLT slot faults BEFORE the graceful path can
 * observe find_display -> NULL. Providing thin `return 0` stubs at these
 * symbol names lets the guest linker resolve, then the returned "0
 * (failure)" pushes the game onto the same graceful branch as the static
 * find_display=NULL case.
 *
 * All five return 0 (X_FALSE). ViewPort/ModeLine writeouts are cleared
 * so the game reads a well-defined 0,0 viewport / zero-size mode line
 * on failure. Additive - ABC imports none of these. */
int stub_XF86VidModeGetAllModeLines(void *dpy, int screen, int *count, void ***lines) {
    (void)dpy; (void)screen;
    if (count) *count = 0;
    if (lines) *lines = NULL;
    return 0;
}
int stub_XF86VidModeGetModeLine(void *dpy, int screen, int *dotclock, void *mode) {
    (void)dpy; (void)screen;
    if (dotclock) *dotclock = 0;
    if (mode) memset(mode, 0, 48);   /* XF86VidModeModeLine is ~48 bytes */
    return 0;
}
int stub_XF86VidModeGetViewPort(void *dpy, int screen, int *x, int *y) {
    (void)dpy; (void)screen;
    if (x) *x = 0;
    if (y) *y = 0;
    return 0;
}
int stub_XF86VidModeSetViewPort(void *dpy, int screen, int x, int y) {
    (void)dpy; (void)screen; (void)x; (void)y;
    return 0;
}
int stub_XF86VidModeSwitchToMode(void *dpy, int screen, void *mode) {
    (void)dpy; (void)screen; (void)mode;
    return 0;
}

/* ---- glX: direct-render query + X-sync barrier ---------------------- *
 * glXIsDirect asks "does this context bypass the X server?" - on Windows
 * WGL there IS no X server, so returning True (1) is the honest answer
 * and matches what a real direct-DRI context returns. glXWaitX is a
 * flush barrier ("wait for all pending X requests to complete before
 * further GL calls"). We have no X server, so the barrier is a no-op. */
int stub_glXIsDirect(void *dpy, void *ctx) { (void)dpy; (void)ctx; return 1; }
void stub_glXWaitX(void) { }

/* ------------------------------------------------------------------ */
/* GLX stubs                                                           */
/* ------------------------------------------------------------------ */
void *stub_glXChooseVisual(void *dpy, int screen, int *attrib_list)
{
    loader_log(LOG_DEBUG, "glXChooseVisual");
    glx_ensure_init();
    return FAKE_VISUAL;
}

void *stub_glXCreateContext(void *dpy, void *vis, void *share, int direct)
{
    loader_log(LOG_DEBUG, "glXCreateContext");
    glx_ensure_init();
    return FAKE_CONTEXT;
}

void stub_glXDestroyContext(void *dpy, void *ctx)
{
    /* Keep our context alive; ignore destroy calls */
}

int stub_glXMakeCurrent(void *dpy, XID drawable, void *ctx)
{
    glx_ensure_init();
    /* Identity diagnostic: after this fix, valid drawables are 0, the real
     * HWND numeric, or the pre-init sentinel. Anything else is a stale
     * handle round-trip worth flagging (does not change behavior). */
    if (drawable != 0 &&
        drawable != (XID)FAKE_DRAWABLE &&
        (!g_hwnd || drawable != (XID)(uintptr_t)g_hwnd)) {
        loader_log(LOG_WARN,
            "glXMakeCurrent: unexpected drawable 0x%x (hwnd=%p ctx=%p), using g_hdc anyway",
            (unsigned)drawable, (void*)g_hwnd, ctx);
    }
    if (ctx == NULL) {
        _wglMakeCurrent(NULL, NULL);
        return 1;
    }
    return _wglMakeCurrent(g_hdc, g_hglrc) ? 1 : 0;
}

int stub_glXMakeContextCurrent(void *dpy, XID draw, XID read, void *ctx)
{
    return stub_glXMakeCurrent(dpy, draw, ctx);
}

void stub_glXSwapBuffers(void *dpy, XID drawable)
{
    /* Identity diagnostic - see stub_glXMakeCurrent for rationale. */
    if (drawable != 0 &&
        drawable != (XID)FAKE_DRAWABLE &&
        (!g_hwnd || drawable != (XID)(uintptr_t)g_hwnd)) {
        loader_log(LOG_WARN,
            "glXSwapBuffers: unexpected drawable 0x%x (hwnd=%p), swapping g_hdc anyway",
            (unsigned)drawable, (void*)g_hwnd);
    }
    if (g_hdc) SwapBuffers(g_hdc);
    /* Pump messages during swap to keep window responsive */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (msg.message == WM_QUIT) ExitProcess(0);
    }
}

void *stub_glXGetProcAddress(const uint8_t *name)
{
    return stub_glXGetProcAddressARB(name);
}

/* Forward decls for the glX* names we intercept below (stubs defined later in
 * this file). Signatures match the definitions; we only need addresses. */
void *stub_glXChooseVisual(void *dpy, int screen, int *attrib_list);
void *stub_glXCreateContext(void *dpy, void *vis, void *share, int direct);
void  stub_glXDestroyContext(void *dpy, void *ctx);
int   stub_glXMakeCurrent(void *dpy, XID drawable, void *ctx);
int   stub_glXMakeContextCurrent(void *dpy, XID draw, XID read, void *ctx);
void  stub_glXSwapBuffers(void *dpy, XID drawable);
int   stub_glXQueryExtension(void *dpy, int *error_base, int *event_base);
int   stub_glXQueryVersion(void *dpy, int *major, int *minor);
const char *stub_glXQueryExtensionsString(void *dpy, int screen);
const char *stub_glXGetClientString(void *dpy, int name);
const char *stub_glXQueryServerString(void *dpy, int screen, int name);
int   stub_glXSwapIntervalSGI(int interval);
int   stub_glXSwapIntervalEXT(void *dpy, XID draw, int interval);
void *stub_glXChooseFBConfig(void *dpy, int screen, const int *attribs, int *n);
void *stub_glXGetVisualFromFBConfig(void *dpy, void *config);
void *stub_glXCreateNewContext(void *dpy, void *config, int type, void *share, int direct);
XID   stub_glXCreateWindow(void *dpy, void *config, XID win, const int *attribs);
int   stub_glXIsDirect(void *dpy, void *ctx);
void  stub_glXWaitX(void);

void *stub_glXGetProcAddressARB(const uint8_t *name)
{
    if (!g_opengl32) g_opengl32 = LoadLibraryA("opengl32.dll");

    /* Legacy-Cg stack-overflow guard: if libCgGL resolves glGetString through
     * glXGetProcAddress (rather than its ELF GOT), hand it our curated __cdecl
     * wrapper instead of the raw driver pointer, so the multi-KB GL_EXTENSIONS
     * string can't overflow Cg's fixed buffers. ul_glGetString is __cdecl (the
     * caller calls cdecl) and returns the real string verbatim for non-Cg
     * titles, so this is ABC-safe. */
    if (name && !strcmp((const char*)name, "glGetString")) {
        extern const unsigned char * __cdecl ul_glGetString(unsigned);
        return (void*)ul_glGetString;
    }

    /* CRITICAL: NEVER hand back a raw ICD pointer for any glX* entry point.
     * Every glX* function in this bridge operates on our FAKE_DRAWABLE
     * (0xD4000001) / FAKE_CONTEXT (0xC0000001) sentinels. If we return a
     * real driver pointer here, the game will call the ICD with our fake
     * handle, and the ICD will fault on the first field-dereference (Hummer
     * seen faulting inside nvoglv32.dll @ RIP 0x693c65a5 with EDX=0xD4000001
     * where the ICD did `cmp DWORD PTR [edx+0x3c], 0x474C5845` - a driver
     * magic tag read against our sentinel). Route every glX* to our stub
     * table so the fake handle NEVER escapes to the ICD. */
    if (name && !strncmp((const char*)name, "glX", 3)) {
        const char *n = (const char*)name;
        #define GLX_MAP(sym) if (!strcmp(n, #sym)) return (void*)stub_##sym
        GLX_MAP(glXChooseVisual);
        GLX_MAP(glXCreateContext);
        GLX_MAP(glXDestroyContext);
        GLX_MAP(glXMakeCurrent);
        GLX_MAP(glXMakeContextCurrent);
        GLX_MAP(glXSwapBuffers);
        GLX_MAP(glXGetProcAddress);
        GLX_MAP(glXGetProcAddressARB);
        GLX_MAP(glXQueryExtension);
        GLX_MAP(glXQueryVersion);
        GLX_MAP(glXQueryExtensionsString);
        GLX_MAP(glXGetClientString);
        GLX_MAP(glXQueryServerString);
        GLX_MAP(glXSwapIntervalSGI);
        GLX_MAP(glXSwapIntervalEXT);
        GLX_MAP(glXChooseFBConfig);
        GLX_MAP(glXGetVisualFromFBConfig);
        GLX_MAP(glXCreateNewContext);
        GLX_MAP(glXCreateWindow);
        GLX_MAP(glXIsDirect);
        GLX_MAP(glXWaitX);
        #undef GLX_MAP
        /* Unknown glX* extension - return a harmless no-op rather than the
         * raw ICD pointer. Better silently-absent than fatal-on-fake-handle. */
        extern uint32_t gl_unresolved_noop(void);
        loader_log(LOG_WARN, "glXGetProcAddress: unknown glX ext %s -> noop", name);
        return (void*)(uintptr_t)&gl_unresolved_noop;
    }

    /* Try wglGetProcAddress first (for extensions) */
    void *p = NULL;
    if (_wglGetProcAddress)
        p = (void*)_wglGetProcAddress((const char*)name);
    if (!p && g_opengl32)
        p = (void*)GetProcAddress(g_opengl32, (const char*)name);
    if (!p)
        loader_log(LOG_DEBUG, "glXGetProcAddress: unresolved %s", name);
    return p;
}

int stub_glXQueryExtension(void *dpy, int *error_base, int *event_base)
{
    if (error_base) *error_base = 0;
    if (event_base) *event_base = 0;
    return 1;
}

int stub_glXQueryVersion(void *dpy, int *major, int *minor)
{
    if (major) *major = 1;
    if (minor) *minor = 4;
    return 1;
}

const char *stub_glXQueryExtensionsString(void *dpy, int screen)
{
    return "GLX_ARB_multisample GLX_EXT_swap_control GLX_ARB_create_context "
           "GLX_ARB_create_context_profile GLX_SGIX_fbconfig";
}

const char *stub_glXGetClientString(void *dpy, int name)
{
    switch (name) {
    case 1: return "NVIDIA Corporation";   /* GLX_VENDOR */
    case 2: return "1.4";                  /* GLX_VERSION */
    case 3: return stub_glXQueryExtensionsString(dpy, 0); /* GLX_EXTENSIONS */
    }
    return "";
}

/* glXQueryServerString(dpy, screen, name) - server-side analog of
 * glXGetClientString. LGJ imports it; the RX 580 ICD exports no glX* through
 * wglGetProcAddress, so a real stub (not the no-op, which would return a NULL
 * char* the game may strstr/strcmp) is required. Same GLX_* name enum. Never
 * return NULL. ABC imports none of glX*; additive. */
const char *stub_glXQueryServerString(void *dpy, int screen, int name)
{
    switch (name) {
    case 1: return "SGI";   /* GLX_VENDOR (server) */
    case 2: return "1.4";   /* GLX_VERSION */
    case 3: return stub_glXQueryExtensionsString(dpy, screen); /* GLX_EXTENSIONS */
    }
    return "";
}

int stub_glXSwapIntervalSGI(int interval)
{
    if (_wglSwapIntervalEXT) _wglSwapIntervalEXT(interval);
    return 0;
}

int stub_glXSwapIntervalEXT(void *dpy, XID draw, int interval)
{
    return stub_glXSwapIntervalSGI(interval);
}

/* GLXFBConfig stubs */
void *stub_glXChooseFBConfig(void *dpy, int screen, const int *attribs, int *n)
{
    glx_ensure_init();
    fake_display_init();
    if (n) *n = 1;
    /* Return a list whose [0] entry is a real, dereferenceable buffer. */
    return g_fake_fbconfig_list;
}

void *stub_glXGetVisualFromFBConfig(void *dpy, void *config)
{
    glx_ensure_init();
    fake_display_init();
    return FAKE_VISUAL;
}

void *stub_glXCreateNewContext(void *dpy, void *config, int type, void *share, int direct)
{
    return stub_glXCreateContext(dpy, FAKE_VISUAL, share, direct);
}

XID stub_glXCreateWindow(void *dpy, void *config, XID win, const int *attribs)
{
    glx_ensure_init();
    fake_display_init();
    /* Round-trip the caller's own Window XID if it matches our page-mapped
     * drawable, else hand out our page. Same page-safety rationale as
     * stub_XCreateWindow - never return the raw HWND numeric (kernel-handle
     * bits fault on deref). */
    if (win && win == FAKE_DRAWABLE) return win;
    return FAKE_DRAWABLE;
}

/* ------------------------------------------------------------------ *
 * ROUND 2 X11 stubs (Hummer Extreme Edition + siblings) - additive for
 * ABC (GLUT-only; imports none of these). Hummer's Xlib import list
 * asks for 12 additional symbols on top of the direct-GLX baseline
 * that was enough for LGJ. All no-ops except the event pump, which
 * peeks the real Win32 message queue owned by g_hwnd. */

/* Window/attrs (no-op, all report success = 1). */
int stub_XChangeWindowAttributes(void *dpy, XID w, unsigned long mask, void *attrs)
{
    (void)dpy;(void)w;(void)mask;(void)attrs;
    return 1;
}
int stub_XConfigureWindow(void *dpy, XID w, unsigned mask, void *chg)
{
    (void)dpy;(void)w;(void)mask;(void)chg;
    return 1;
}
int stub_XChangeProperty(void *dpy, XID w, XID prop, XID type, int fmt,
                         int mode, const unsigned char *data, int nelements)
{
    (void)dpy;(void)w;(void)prop;(void)type;(void)fmt;
    (void)mode;(void)data;(void)nelements;
    return 1;
}
int stub_XFreeColormap(void *dpy, XID cmap)
{
    (void)dpy;(void)cmap;
    return 1;
}
int stub_XGetGeometry(void *dpy, XID d, XID *root_return,
                      int *x, int *y, unsigned *w, unsigned *h,
                      unsigned *border_width, unsigned *depth)
{
    (void)dpy;(void)d;
    if (root_return) *root_return = g_fake_xscreen.root;
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = (unsigned)g_width;
    if (h) *h = (unsigned)g_height;
    if (border_width) *border_width = 0;
    if (depth) *depth = 24;
    return 1;
}

/* Cursor / pixmap. Reuse the same fake XID sentinels the existing
 * XCreatePixmapCursor family already returns so id-equality checks
 * behave consistently. */
XID stub_XCreateFontCursor(void *dpy, unsigned int shape)
{
    (void)dpy;(void)shape;
    glx_ensure_init(); fake_display_init();
    return (XID)0xC0;
}
XID stub_XCreateBitmapFromData(void *dpy, XID d, const char *data,
                               unsigned w, unsigned h)
{
    (void)dpy;(void)d;(void)data;(void)w;(void)h;
    glx_ensure_init(); fake_display_init();
    return (XID)0xB2;
}

/* Input / keyboard */
int stub_XChangeKeyboardControl(void *dpy, unsigned long mask, void *ctrl)
{
    (void)dpy;(void)mask;(void)ctrl;
    return 1;
}
void stub_XRefreshKeyboardMapping(void *event)
{
    (void)event;
}
unsigned long stub_XLookupKeysym(void *event, int index)
{
    (void)event;(void)index;
    return 0; /* NoSymbol */
}

/* Event queue - peek the REAL Win32 queue owned by our hidden g_hwnd.
 * XEventsQueued returns the pending message count (any count > 0 makes
 * the game's event loop dispatch); XPeekEvent hands back the next event
 * without dequeuing (we reuse the XNextEvent decoder). XSelectInput is
 * a no-op because we always pump every message class. */
int stub_XEventsQueued(void *dpy, int mode)
{
    (void)dpy;(void)mode;
    MSG m;
    return PeekMessageA(&m, NULL, 0, 0, PM_NOREMOVE) ? 1 : 0;
}
int stub_XPeekEvent(void *dpy, void *event)
{
    /* Peek reuses NextEvent's decoder - same shape, no dequeue guarantee
     * (Xlib callers using PeekEvent still do a NextEvent afterwards, so
     * the extra dispatch is harmless). */
    return stub_XNextEvent(dpy, (XEvent*)event);
}
int stub_XSelectInput(void *dpy, XID w, long mask)
{
    (void)dpy;(void)w;(void)mask;
    return 1;
}

/* WM hints - return a small zeroed heap struct the game will free. */
void *stub_XAllocWMHints(void)
{
    return calloc(1, 72);
}
int stub_XSetWMNormalHints(void *dpy, XID w, void *hints)
{
    (void)dpy;(void)w;(void)hints;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Register all GLX/X11 stubs                                         */
/* ------------------------------------------------------------------ */
void glx_register_stubs(LoaderCtx *ctx)
{
    loader_log(LOG_INFO, "Registering GLX/X11 stubs");

    #define R(name) sym_register(ctx, #name, (uintptr_t)stub_##name)

    /* X11 */
    R(XOpenDisplay); R(XCloseDisplay);
    R(XCreateColormap); R(XCreateWindow); R(XDestroyWindow);
    R(XMapWindow); R(XMapRaised); R(XUnmapWindow);
    R(XFlush); R(XSync); R(XPending);
    R(XNextEvent); R(XCheckMaskEvent);
    R(XStoreName); R(XInternAtom); R(XSetWMProtocols);
    R(XDefaultRootWindow); R(XDefaultScreen);

    /* Display/Screen query functions reading the fake _XDisplay tree (LGJ
     * direct-GLX). Additive - ABC imports none of these. */
    R(XRootWindow); R(XDefaultVisual); R(XDefaultDepth); R(XDefaultColormap);
    R(XDefaultGC); R(XScreenOfDisplay); R(XDefaultScreenOfDisplay);
    R(XScreenCount); R(XDisplayWidth); R(XDisplayHeight);
    R(XDisplayWidthMM); R(XDisplayHeightMM);
    R(XBlackPixel); R(XWhitePixel);
    R(XDefaultDepthOfScreen); R(XRootWindowOfScreen);
    R(XConnectionNumber);

    /* Extended X11 for direct-GLX titles (LGJ). Additive - ABC imports none. */
    R(XSetErrorHandler); R(XSetIOErrorHandler);
    R(XGetErrorText); R(XGetErrorDatabaseText);
    R(XInitThreads); R(XLockDisplay); R(XUnlockDisplay);
    R(XSetCloseDownMode); R(XSetInputFocus);
    R(XMoveWindow); R(XMoveResizeWindow); R(XResizeWindow); R(XRaiseWindow);
    R(XGrabKeyboard); R(XUngrabKeyboard);
    R(XGrabPointer); R(XUngrabPointer); R(XWarpPointer);
    R(XAutoRepeatOn); R(XAutoRepeatOff);
    R(XCreatePixmap); R(XCreatePixmapFromBitmapData); R(XCreatePixmapCursor);
    R(XDefineCursor); R(XUndefineCursor); R(XFreeCursor); R(XFreePixmap);
    R(XParseColor);
    R(XQueryPointer); R(XTranslateCoordinates); R(XLookupString);
    R(XSetWMProperties); R(XStringListToTextProperty); R(XMissingExtension);
    R(XextCreateExtension); R(XextAddDisplay); R(XextFindDisplay); R(XextRemoveDisplay);

    /* Round 2 X11 (Hummer Extreme Edition). Additive - ABC imports none. */
    R(XChangeWindowAttributes); R(XConfigureWindow); R(XChangeProperty);
    R(XFreeColormap); R(XGetGeometry);
    R(XCreateFontCursor); R(XCreateBitmapFromData);
    R(XChangeKeyboardControl); R(XRefreshKeyboardMapping); R(XLookupKeysym);
    R(XEventsQueued); R(XPeekEvent); R(XSelectInput);
    R(XAllocWMHints); R(XSetWMNormalHints);

    /* GLX */
    R(glXChooseVisual); R(glXCreateContext); R(glXDestroyContext);
    R(glXMakeCurrent); R(glXMakeContextCurrent);
    R(glXSwapBuffers);
    R(glXGetProcAddress); R(glXGetProcAddressARB);
    R(glXQueryExtension); R(glXQueryVersion);
    R(glXQueryExtensionsString); R(glXGetClientString);
    R(glXQueryServerString);
    R(glXSwapIntervalSGI); R(glXSwapIntervalEXT);
    R(glXChooseFBConfig); R(glXGetVisualFromFBConfig);
    R(glXCreateNewContext); R(glXCreateWindow);
    /* Round 3 additions */
    R(glXIsDirect); R(glXWaitX);
    R(XF86VidModeGetAllModeLines); R(XF86VidModeGetModeLine);
    R(XF86VidModeGetViewPort);     R(XF86VidModeSetViewPort);
    R(XF86VidModeSwitchToMode);

    #undef R
}

/* ------------------------------------------------------------------ */
/* Message pump (call from main loop if game doesn't drive events)    */
/* ------------------------------------------------------------------ */
bool glx_ensure_init_size(int w, int h, const char *title)
{
    if (w > 0) g_width  = w;
    if (h > 0) g_height = h;
    if (!g_initialized) {
        if (!g_hwnd) create_window(g_width, g_height, title ? title : "Ultra Loader");
        if (!g_hglrc) setup_wgl();
        g_initialized = true;
    } else if (g_hwnd) {
        RECT r = {0, 0, g_width, g_height};
        DWORD style = (DWORD)GetWindowLongA(g_hwnd, GWL_STYLE);
        AdjustWindowRect(&r, style, FALSE);
        SetWindowPos(g_hwnd, NULL, 0, 0, r.right-r.left, r.bottom-r.top,
                     SWP_NOMOVE|SWP_NOZORDER);
        if (title) SetWindowTextA(g_hwnd, title);
    }
    return g_initialized;
}

/* Pre-initialize WGL context so wglGetProcAddress works during extension resolution */
bool glx_preinit(void)
{
    if (g_initialized) return true;
    loader_log(LOG_INFO, "Pre-initializing WGL context for extension resolution...");
    if (!g_hwnd) {
        /* Create a hidden 1x1 window just to get a GL context */
        WNDCLASSEXA wc = {0};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_OWNDC;
        wc.lpfnWndProc   = DefWindowProcA;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.lpszClassName = "LindberghPreInit";
        RegisterClassExA(&wc);
        g_hwnd = CreateWindowExA(0, "LindberghPreInit", "PreInit",
                                  WS_POPUP, 0, 0, 1, 1,
                                  NULL, NULL, GetModuleHandleA(NULL), NULL);
        if (!g_hwnd) return false;
        g_hdc = GetDC(g_hwnd);
    }
    if (!g_hglrc) {
        if (!setup_wgl()) return false;
    }
    g_initialized = true;
    /* Seed the fake _XDisplay tree with the real HWND/HDC we just minted so
     * any guest ctor that dereferences _XDisplay->screens[0].root (via Xlib
     * client-side macros) BEFORE XOpenDisplay runs gets a live handle, not
     * the compile-time-static FAKE_DRAWABLE sentinel. Closes the last
     * sentinel-escape window on the mint-side hardening. */
    fake_display_init();
    loader_log(LOG_INFO, "WGL pre-init complete");
    return true;
}

void glx_pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) ExitProcess(0);
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

HWND  glx_get_hwnd(void)  { return g_hwnd; }
HDC   glx_get_hdc(void)   { return g_hdc; }
HGLRC glx_get_hglrc(void) { return g_hglrc; }
