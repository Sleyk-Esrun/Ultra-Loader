/*
 * sdl_window.c - SDL3 window + Mesa GL context for Ultra Loader.
 *
 * Ported from the SDL3 branch (lindbergh_launcher) into tonight's build.
 * Uses *dynamic* SDL3 loading (LoadLibrary/GetProcAddress) so it needs no
 * SDL dev headers and won't disturb the existing build. Creates the real
 * OpenGL window + context that init_graphics expects, then drives the
 * game's GLUT display callback in a render loop.
 *
 * Requires next to the exe: SDL3.dll  and Mesa's opengl32.dll
 * (+ libgallium_wgl.dll) - the same DLLs the SDL3 branch used.
 */

#include <stdlib.h>
#include "../include/compat32.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "loader.h"
#include "snapshot.h"

/* ── SDL3 constants (from SDL3 headers, hardcoded to avoid the dependency) ── */
#define SDL_INIT_VIDEO            0x00000020u
#define SDL_WINDOW_FULLSCREEN     0x0000000000000001ull
#define SDL_WINDOW_OPENGL         0x0000000000000002ull
#define SDL_WINDOW_BORDERLESS     0x0000000000000010ull
#define SDL_WINDOW_RESIZABLE      0x0000000000000020ull
/* SDL_GLAttr enum values (SDL3, same as SDL2) */
#define SDL_GL_RED_SIZE            0
#define SDL_GL_GREEN_SIZE          1
#define SDL_GL_BLUE_SIZE           2
#define SDL_GL_ALPHA_SIZE          3
#define SDL_GL_DEPTH_SIZE          6
#define SDL_GL_STENCIL_SIZE        7
#define SDL_GL_DOUBLEBUFFER        5
#define SDL_GL_CONTEXT_MAJOR_VERSION 17
#define SDL_GL_CONTEXT_MINOR_VERSION 18
#define SDL_GL_CONTEXT_PROFILE_MASK  21
#define SDL_GL_CONTEXT_PROFILE_COMPATIBILITY 0x0002

/* SDL3 event: SDL_Event is a union; first field is uint32_t type. We only need
 * its size (128 bytes is safe for SDL3) and the type field. */
#define SDL_EVENT_QUIT            0x100
#define SDL_EVENT_KEY_DOWN        0x300

typedef void SDL_Window;
typedef void* SDL_GLContext;

/* ── dynamic SDL3 entry points ─────────────────────────────────────── */
static HMODULE s_sdl = NULL;
typedef int          (*pSDL_Init)(uint32_t);
typedef void         (*pSDL_Quit)(void);
typedef SDL_Window*  (*pSDL_CreateWindow)(const char*, int, int, uint64_t); /* SDL3: 4 args */
typedef void         (*pSDL_DestroyWindow)(SDL_Window*);
typedef SDL_GLContext(*pSDL_GL_CreateContext)(SDL_Window*);
typedef int          (*pSDL_GL_MakeCurrent)(SDL_Window*, SDL_GLContext);
typedef void         (*pSDL_GL_SwapWindow)(SDL_Window*);
typedef int          (*pSDL_GL_SetSwapInterval)(int);
typedef int          (*pSDL_GL_SetAttribute)(int, int);
typedef void*        (*pSDL_GL_GetProcAddress)(const char*);
typedef int          (*pSDL_PollEvent)(void*);
typedef const char*  (*pSDL_GetError)(void);

static pSDL_Init               SDL_Init_;
static pSDL_Quit               SDL_Quit_;
static pSDL_CreateWindow       SDL_CreateWindow_;
static pSDL_DestroyWindow      SDL_DestroyWindow_;
static pSDL_GL_CreateContext   SDL_GL_CreateContext_;
static pSDL_GL_MakeCurrent     SDL_GL_MakeCurrent_;
static pSDL_GL_SwapWindow      SDL_GL_SwapWindow_;
static pSDL_GL_SetSwapInterval SDL_GL_SetSwapInterval_;
static pSDL_GL_SetAttribute    SDL_GL_SetAttribute_;
static pSDL_GL_GetProcAddress  SDL_GL_GetProcAddress_;
static pSDL_PollEvent          SDL_PollEvent_;
static pSDL_GetError           SDL_GetError_;

/* ── window state ──────────────────────────────────────────────────── */
static SDL_Window   *g_window = NULL;
static SDL_GLContext g_glctx  = NULL;
static int           g_sdl_ready = 0;
int                  g_sdl_active = 0;   /* 1 once the SDL window is live */
int                  g_ul_win_w = 1280;
int                  g_ul_win_h = 720;
volatile int         g_ul_swap_pending = 0;
volatile int         g_splash_dismiss  = 0;   /* set once the cabinet renders; fades the splash out */
/* Address of the guest global holding the game's vblank callback pointer
 * (set by set_vblank_callback). Per-game; the patcher fills it in. When set,
 * the render loop calls *(*g_ul_vblank_cb_addr) each frame so the vblank
 * counter the game's logic waits on actually advances. */
uint32_t             g_ul_vblank_cb_addr = 0;
/* Address of the game's per-frame "hardware ready / vblank occurred" flag byte.
 * The game's frame loop gates its whole update on this being non-zero and CLEARS
 * it after each frame, expecting the GPU vblank interrupt to re-set it. We re-set
 * it every render frame so the game actually advances. Per-game; patcher fills. */
uint32_t             g_ul_hwready_addr   = 0;
/* ptcl_ctxt_list (std::list) head + element-count globals. Its sentinel self-init
 * was NOP'd out; patching the static ctor breaks boot, so we repair the sentinel
 * at runtime each frame WHEN THE LIST IS EMPTY (count==0): self-link _M_next/_M_prev
 * to the sentinel so the post-START mission-init walk terminates instead of
 * dereferencing garbage. Per-game; patcher fills. */
uint32_t             g_ul_ptcl_head_addr  = 0;
uint32_t             g_ul_ptcl_count_addr = 0;
static void         *g_ul_display_cb = NULL;   /* game's glutDisplayFunc */

/* Default output resolution: 16:9 720p. The game requests 640x480 (4:3) via
 * glutInitWindowSize, but we present at 1280x720 - set ULTRA_RES=WxH to override
 * (resolution tooling/menu comes later). */
int g_force_w = 1280, g_force_h = 720;
int g_ul_display_mode = 0;  /* 0=windowed 1=borderless 2=fullscreen (settings.c) */
int g_ul_aspect       = 0;  /* 0=16:9 1=4:3 2=stretch */
int g_ul_vsync        = 0;
void sdl_window_set_size(int w, int h) {
    (void)w; (void)h;   /* ignore the game's request; force 16:9 720p */
    g_ul_win_w = g_force_w; g_ul_win_h = g_force_h;
    extern int g_width, g_height;   /* keep glx_emu's blit dims in sync */
    g_width = g_force_w; g_height = g_force_h;
}
void sdl_window_set_display_cb(void *fn) { g_ul_display_cb = fn; }
void sdl_window_request_swap(void) { g_ul_swap_pending = 1; }
HWND sdl_window_get_hwnd(void) { return NULL; /* not needed for basic render */ }

/* ── Accessors required by the ported glx_emu.c ────────────────────────
 * glx_emu uses these to bind to the SDL-created GL context. */
HMODULE glut_get_mesa_handle(void) { return GetModuleHandleA("opengl32.dll"); }

HDC glut_get_hdc(void) {
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (gl) {
        typedef HDC (WINAPI *fn_t)(void);
        fn_t f = (fn_t)GetProcAddress(gl, "wglGetCurrentDC");
        if (f) { HDC dc = f(); if (dc) return dc; }
    }
    return NULL;
}

HGLRC glut_get_hglrc(void) {
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (gl) {
        typedef HGLRC (WINAPI *fn_t)(void);
        fn_t f = (fn_t)GetProcAddress(gl, "wglGetCurrentContext");
        if (f) return f();
    }
    return (HGLRC)g_glctx;
}

HWND glut_get_hwnd(void) {
    /* SDL3-first path: query the real Win32 HWND from the SDL window via
     * SDL_GetWindowProperties + SDL_GetPointerProperty("SDL.window.win32.hwnd").
     * The legacy WindowFromDC(wglGetCurrentDC()) path returns NULL under SDL3 +
     * Mesa-llvmpipe (the GL context comes from SDL, not WGL), which breaks the
     * GSEVO cursor->gun-pose feed in jvs_input.c. Falling back to the DC-derived
     * HWND keeps non-SDL paths working. */
    if (g_window && s_sdl) {
        typedef uint32_t (*pfn_GetWindowProperties)(void*);
        typedef void*    (*pfn_GetPointerProperty)(uint32_t, const char*, void*);
        static pfn_GetWindowProperties p_props = NULL;
        static pfn_GetPointerProperty  p_get   = NULL;
        static int s_resolved = 0;
        if (!s_resolved) {
            p_props = (pfn_GetWindowProperties)(void*)GetProcAddress(s_sdl, "SDL_GetWindowProperties");
            p_get   = (pfn_GetPointerProperty) (void*)GetProcAddress(s_sdl, "SDL_GetPointerProperty");
            s_resolved = 1;
        }
        if (p_props && p_get) {
            uint32_t props = p_props(g_window);
            if (props) {
                HWND w = (HWND)p_get(props, "SDL.window.win32.hwnd", NULL);
                if (w) return w;
            }
        }
    }
    /* Legacy fallback: derive HWND from the current DC via WindowFromDC. */
    HDC dc = glut_get_hdc();
    if (dc) { HWND w = WindowFromDC(dc); if (w) return w; }
    /* Final fallback: foreground window when this process owns it. The aim
     * feed is byte-safe for spurious HWNDs (the cursor mapping just produces
     * a centered aim), so this is preferable to dropping input entirely. */
    HWND fg = GetForegroundWindow();
    if (fg) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) return fg;
    }
    return NULL;
}

static int load_sdl(void)
{
    if (s_sdl) return 1;
    const char *names[] = { "SDL3.dll", "SDL2.dll", "SDL2-2.0.dll", NULL };
    for (int i = 0; names[i]; i++) {
        s_sdl = GetModuleHandleA(names[i]);
        if (!s_sdl) s_sdl = LoadLibraryA(names[i]);
        if (s_sdl) { loader_log(LOG_WARN, "[SDL3] loaded %s", names[i]); break; }
    }
    if (!s_sdl) { loader_log(LOG_ERROR, "[SDL3] SDL3.dll not found, place it next to ultra-loader.exe"); return 0; }
#define GETSDL(v, n) v = (p##n)(void*)GetProcAddress(s_sdl, #n); if (!v) loader_log(LOG_WARN, "[SDL3] missing %s", #n)
    GETSDL(SDL_Init_, SDL_Init);
    GETSDL(SDL_Quit_, SDL_Quit);
    GETSDL(SDL_CreateWindow_, SDL_CreateWindow);
    GETSDL(SDL_DestroyWindow_, SDL_DestroyWindow);
    GETSDL(SDL_GL_CreateContext_, SDL_GL_CreateContext);
    GETSDL(SDL_GL_MakeCurrent_, SDL_GL_MakeCurrent);
    GETSDL(SDL_GL_SwapWindow_, SDL_GL_SwapWindow);
    GETSDL(SDL_GL_SetSwapInterval_, SDL_GL_SetSwapInterval);
    GETSDL(SDL_GL_SetAttribute_, SDL_GL_SetAttribute);
    GETSDL(SDL_GL_GetProcAddress_, SDL_GL_GetProcAddress);
    GETSDL(SDL_PollEvent_, SDL_PollEvent);
    GETSDL(SDL_GetError_, SDL_GetError);
#undef GETSDL
    return SDL_CreateWindow_ && SDL_GL_CreateContext_;
}

/* Create the SDL3 window + GL context. Returns 1 on success. */
int sdl_window_create(const char *title)
{
    if (g_sdl_ready) return 1;
    if (!load_sdl()) return 0;

    /* Output resolution is the forced 16:9 720p (or ULTRA_RES), parsed in main(). */
    g_ul_win_w = g_force_w; g_ul_win_h = g_force_h;
    { extern int g_width, g_height; g_width = g_force_w; g_height = g_force_h; }

    if (SDL_Init_(SDL_INIT_VIDEO) != 0 && SDL_Init_(SDL_INIT_VIDEO) < 0) {
        loader_log(LOG_ERROR, "[SDL3] SDL_Init failed: %s", SDL_GetError_ ? SDL_GetError_() : "?");
        /* SDL3 returns true(1)/false(0); SDL2 returns 0/neg. Be lenient. */
    }

    /* OpenGL 2.1 compat - matches Lindbergh's GeForce 7800 (same as SDL3 branch) */
    if (SDL_GL_SetAttribute_) {
        SDL_GL_SetAttribute_(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute_(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute_(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
        SDL_GL_SetAttribute_(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute_(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute_(SDL_GL_STENCIL_SIZE, 8);
        SDL_GL_SetAttribute_(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute_(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute_(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute_(SDL_GL_ALPHA_SIZE, 8);
    }

    /* Force a fixed, distinctive window title (the game passes its ELF path,
     * which the Computer-Use app matcher can't resolve). */
    (void)title;
    unsigned long long wflags = SDL_WINDOW_OPENGL;
    if (g_ul_display_mode == 1)      wflags |= SDL_WINDOW_BORDERLESS;
    else if (g_ul_display_mode == 2) wflags |= SDL_WINDOW_FULLSCREEN;
    else                             wflags |= SDL_WINDOW_RESIZABLE;
    g_window = SDL_CreateWindow_("UltraLoader",
                                 g_ul_win_w, g_ul_win_h, wflags);
    if (!g_window) {
        loader_log(LOG_ERROR, "[SDL3] SDL_CreateWindow failed: %s", SDL_GetError_ ? SDL_GetError_() : "?");
        return 0;
    }
    g_glctx = SDL_GL_CreateContext_(g_window);
    if (!g_glctx) {
        loader_log(LOG_ERROR, "[SDL3] SDL_GL_CreateContext failed: %s", SDL_GetError_ ? SDL_GetError_() : "?");
        return 0;
    }
    SDL_GL_MakeCurrent_(g_window, g_glctx);
    if (SDL_GL_SetSwapInterval_) SDL_GL_SetSwapInterval_(g_ul_vsync ? 1 : 0);

    loader_log(LOG_WARN, "[SDL3] Window %dx%d created, GL context ready", g_ul_win_w, g_ul_win_h);

    /* Log GL strings from Mesa's opengl32.dll */
    {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (gl) {
            typedef const char* (WINAPI *glGetString_t)(unsigned);
            glGetString_t gs = (glGetString_t)GetProcAddress(gl, "glGetString");
            if (gs) {
                loader_log(LOG_WARN, "[SDL3] GL_VENDOR:   %s", gs(0x1F00));
                loader_log(LOG_WARN, "[SDL3] GL_RENDERER: %s", gs(0x1F01));
                loader_log(LOG_WARN, "[SDL3] GL_VERSION:  %s", gs(0x1F02));
            }
            typedef void (WINAPI *glViewport_t)(int,int,int,int);
            glViewport_t vp = (glViewport_t)GetProcAddress(gl, "glViewport");
            if (vp) vp(0, 0, g_ul_win_w, g_ul_win_h);
        }
    }

    /* Patch the game's GOT with real Mesa GL pointers (defined in glx_emu.c).
     * Step 1: NAME-DRIVEN generic binder - walks the loaded ELF's own
     *   R_386_JUMP_SLOT relocs and binds every core/ARB/EXT gl* import (works
     *   for ANY Lindbergh title, no hardcoded address table).
     * Step 2: the verified hardcoded ABC table runs as an ADDITIVE FALLBACK -
     *   for ABC it re-writes its 128 slots byte-identically (zero regression);
     *   for other titles its writes are guarded to ABC's address space, which
     *   the generic binder has already handled by name. */
    { extern int  bind_gl_got_generic(void);
      extern void patch_game_gl_got(void);
      if (!getenv("ULTRA_NO_GENERIC_GL_BIND")) bind_gl_got_generic();
      patch_game_gl_got(); }

    /* Initialize the GLX/FBO emulation layer, bound to the SDL window's GL
     * context. The glX* SHIMS were already registered at init (before
     * relocation) so the game's GOT points at the FBO-aware versions; here we
     * just bring the emulation state online now that the context exists. */
    {
        extern int glx_emu_init(HWND, HDC);
        HWND hwnd = glut_get_hwnd();
        HDC  hdc  = glut_get_hdc();
        glx_emu_init(hwnd, hdc);
        loader_log(LOG_WARN, "[GLX] FBO/GLX emulation online (context bound)");
    }

    g_sdl_ready = 1;
    g_sdl_active = 1;
    return 1;
}

int g_ul_swap_frame = 0;   /* last presented frame index; read by tracers in other TUs */

/* Capture the current GL framebuffer to a 24-bit BMP (gated by ULTRA_DUMP_FRAME).
 * glReadPixels returns rows bottom-up and GL_BGR matches BMP byte order, so the
 * pixels drop straight in. Lets us SEE what the game drew without a screenshot. */
static void dump_frame_bmp(int frame)
{
    static void (__stdcall *p_readpixels)(int,int,int,int,unsigned,unsigned,void*) = 0;
    if (!p_readpixels) {
        HMODULE gl = LoadLibraryA("opengl32.dll");
        p_readpixels = (void*)GetProcAddress(gl, "glReadPixels");
    }
    if (!p_readpixels) return;
    /* Read the REAL window framebuffer (0), not whatever offscreen FBO the present
     * path left bound - otherwise we'd capture the game's native-res FBO instead
     * of the scaled output the user actually sees. */
    int w = g_ul_win_w, h = g_ul_win_h;
    /* ULTRA_DUMP_OFFSCREEN=1: capture the RAW offscreen FBO (pre-present) at its
     * native size instead - used to calibrate the cutscene crop rect. */
    int off_ok = 0;
    if (getenv("ULTRA_DUMP_OFFSCREEN")) {
        extern int glx_bind_read_offscreen(int*,int*);
        off_ok = glx_bind_read_offscreen(&w, &h);
    }
    if (!off_ok) { extern void glx_bind_read_window(void); glx_bind_read_window(); }
    int rowsz = (w*3 + 3) & ~3;            /* 4-byte aligned rows */
    unsigned char *px = (unsigned char*)malloc((size_t)rowsz * h);
    if (!px) return;
    p_readpixels(0, 0, w, h, 0x80E0 /*GL_BGR*/, 0x1401 /*GL_UNSIGNED_BYTE*/, px);
    /* Dump destination: ULTRA_DUMP_DIR if set, else the current working dir (the
     * loader cd's to the game dir, but frame dumps are most useful next to the
     * loader, so '.' resolves to wherever the process was launched). The old
     * hardcoded C:\Users\Sleyk\Desktop\Ultra Loader path silently failed once the
     * build moved to I:\ (fopen returned NULL on a missing dir). Diagnostic-only
     * path; does not affect any game's render output. */
    char path[MAX_PATH];
    const char *dump_dir = getenv("ULTRA_DUMP_DIR");
    if (dump_dir && *dump_dir) snprintf(path, sizeof path, "%s\\ul_frame_%d.bmp", dump_dir, frame);
    else                       snprintf(path, sizeof path, "ul_frame_%d.bmp", frame);
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t datalen = (uint32_t)rowsz * h, filelen = 54 + datalen;
        uint8_t hdr[54] = {0};
        hdr[0]='B'; hdr[1]='M';
        *(uint32_t*)(hdr+2)=filelen; *(uint32_t*)(hdr+10)=54;
        *(uint32_t*)(hdr+14)=40; *(int32_t*)(hdr+18)=w; *(int32_t*)(hdr+22)=h;
        *(uint16_t*)(hdr+26)=1; *(uint16_t*)(hdr+28)=24; *(uint32_t*)(hdr+34)=datalen;
        fwrite(hdr,1,54,f); fwrite(px,1,datalen,f); fclose(f);
        loader_log(LOG_WARN, "[SDL3] dumped framebuffer -> ul_frame_%d.bmp (%dx%d)", frame, w, h);
    }
    free(px);
}

/* Exported: the shell's boot-console thread polls this each frame to know
 * when the game has rendered past its own init/loading screen (gray frames)
 * onto real colored content. Returns 1 once detected, 0 otherwise. Same
 * underlying flag the splash uses. */
__declspec(dllexport)
int ul_lindbergh_content_seen(void) { return g_splash_dismiss ? 1 : 0; }

/* Hold the boot splash until the cabinet draws its first non-black frame (the
 * game renders black "loading" frames for several seconds before INSERT COIN /
 * the SEGA logo appear). Sample a small center patch of the freshly-rendered
 * back buffer; once it's no longer black, dismiss the splash so it fades
 * straight into live content - no black gap. */
static void splash_content_check(void)
{
    if (g_splash_dismiss) return;
    static void (__stdcall *p_readpixels)(int,int,int,int,unsigned,unsigned,void*) = 0;
    if (!p_readpixels) {
        HMODULE gl = LoadLibraryA("opengl32.dll");
        p_readpixels = (void*)GetProcAddress(gl, "glReadPixels");
        if (!p_readpixels) { g_splash_dismiss = 1; return; }   /* can't sample → don't hang the splash */
    }
    enum { N = 32 };
    unsigned char px[N*N*3];
    int cx = g_ul_win_w/2 - N/2, cy = g_ul_win_h/2 - N/2;
    if (cx < 0) cx = 0; if (cy < 0) cy = 0;
    p_readpixels(cx, cy, N, N, 0x1907 /*GL_RGB*/, 0x1401 /*GL_UNSIGNED_BYTE*/, px);
    long sum = 0, spread = 0;
    for (int i = 0; i < N*N; i++) {
        int r = px[i*3], g = px[i*3+1], b = px[i*3+2];
        sum += r + g + b;
        int mx = r>g?(r>b?r:b):(g>b?g:b), mn = r<g?(r<b?r:b):(g<b?g:b);
        spread += mx - mn;
    }
    int bright = (int)(sum / (N*N*3));
    int sat    = (int)(spread / (N*N));
    /* Dismiss when the game draws any non-black content (bright >= 30 covers
     * gray loading screens, sat >= 12 catches colored frames).  The old
     * threshold (bright >= 185) was too conservative and caused dark-loading-
     * screen titles (Hummer Extreme, etc.) to sit behind the console for the
     * full 90 s hard-cap.  Frame-count fallback at 240 frames (~4 s at 60 fps)
     * handles the edge case of pure-black loading sequences. */
    if (sat >= 12 || bright >= 30) {
        g_splash_dismiss = 1;
        loader_log(LOG_INFO, "[SPLASH] cabinet drew real content (bright=%d sat=%d), fading into game", bright, sat);
    } else if (g_ul_swap_frame >= 240) {
        g_splash_dismiss = 1;
        loader_log(LOG_INFO, "[SPLASH] game alive for %d frames (bright=%d sat=%d), auto-dismiss", g_ul_swap_frame, bright, sat);
    }
}

/* ============================================================================
 *  IN-GAME OVERLAY - DELEGATED to the shared pause_menu_service.
 *
 *  The GDI layered-window menu used to live inline in this TU; it was lifted
 *  into work/src/pause_menu_service.c so every module (Lindbergh, Type X /
 *  X2, Sega Europa-R, RawThrills PC, native libretro frontends) can share it.
 *  The native Lindbergh side now:
 *    - registers a pause_menu_host_t with Lindbergh-specific callbacks
 *      (audio pause via audio_engine, settings_apply, restart via re-exec,
 *      snapshot save/load/info);
 *    - keeps the original ingame_overlay_request / ingame_overlay_pump entry
 *      points as thin wrappers so jvs_input.c and sdl_window_pump don't change.
 * ========================================================================== */
#include "../include/settings.h"
#include "pause_menu_service.h"
extern int  g_ul_stick_sens, g_ul_stick_deadzone, g_ul_stick_curve, g_ul_stick_smooth;
extern void audio_set_master_volume(int);
extern void audio_set_pause(int);
extern void settings_apply(void);
extern int  g_ul_pause_audio;
extern int  g_ul_snapshots;
HWND glut_get_hwnd(void);

/* ── Lindbergh-side callbacks the shared service calls back into ─────────── */
static HWND lb_get_hwnd(void) { return glut_get_hwnd(); }

static void lb_audio_pause(int overlay_up)
{
    /* Honor the per-user "Pause in Menu" setting. */
    audio_set_pause(overlay_up && g_ul_pause_audio);
}

static void lb_audio_set_vol(int v) { audio_set_master_volume(v); }

static void lb_settings_apply(void) { settings_apply(); }

static void lb_reexec(int with_game)
{
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, sizeof exe);
    { char dir[MAX_PATH]; strncpy(dir, exe, MAX_PATH - 1); dir[MAX_PATH - 1] = 0;
      char *s = strrchr(dir, '\\'); if (s) *s = 0;
      extern void settings_save(const char *); settings_save(dir); }
    char cmd[1300];
    if (with_game) snprintf(cmd, sizeof cmd, "\"%s\" \"%s\"", exe, g_settings.game_path);
    else           snprintf(cmd, sizeof cmd, "\"%s\"", exe);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    ExitProcess(0);
}
static void lb_restart_game(void) { lb_reexec(1); }
static void lb_exit_to_menu(void) { lb_reexec(0); }

static int  lb_snap_save(int slot) { return snapshot_save(slot); }
static int  lb_snap_load(int slot) { return snapshot_load(slot); }
static int  lb_snap_used(int slot) { return snapshot_slot_used(slot); }
static void lb_snap_info(int slot, int *fr, int *mb) { snapshot_slot_info(slot, fr, mb); }

static const char *lb_get_title(void) { extern const char *g_ul_loaded_title; return g_ul_loaded_title; }
static int         lb_get_frame(void) { return g_ul_swap_frame; }

/* g_ul_pause_audio = mirror of g_settings.pause_game_audio, set by settings_apply.
 * Definition stays in this TU so it lives in the runtime image. */
int g_ul_pause_audio = 1;

/* Wire the shared service to Lindbergh's environment on first request. */
static void pause_menu_lazy_register(void)
{
    static int s_done = 0;
    if (s_done) return;
    s_done = 1;
    pause_menu_host_t h;
    memset(&h, 0, sizeof h);
    h.get_hwnd            = lb_get_hwnd;
    h.win_w               = g_ul_win_w;
    h.win_h               = g_ul_win_h;
    h.audio_pause         = lb_audio_pause;
    h.audio_set_volume    = lb_audio_set_vol;
    h.settings_apply      = lb_settings_apply;
    h.restart_game        = lb_restart_game;
    h.exit_to_menu        = lb_exit_to_menu;
    h.snapshot_save       = lb_snap_save;
    h.snapshot_load       = lb_snap_load;
    h.snapshot_slot_used  = lb_snap_used;
    h.snapshot_slot_info  = lb_snap_info;
    h.snapshots_enabled   = g_ul_snapshots;
    h.snapshots_num_slots = SNAP_NUM_SLOTS;
    h.get_title           = lb_get_title;
    h.get_frame           = lb_get_frame;
    pause_menu_init(&h);
}

/* Compatibility shims - jvs_input.c calls ingame_overlay_request, sdl_window_pump
 * calls ingame_overlay_pump. Both just forward to the shared service. */
void ingame_overlay_request(int exit_screen)
{
    pause_menu_lazy_register();
    pause_menu_request(exit_screen);
}
int ingame_overlay_pump(void)
{
    pause_menu_lazy_register();
    return pause_menu_pump();
}

/* Pump events + drive the game's display callback once. Returns 0 to quit. */
int sdl_window_pump(void)
{
    if (!g_sdl_ready) return 1;
    if (SDL_PollEvent_) {
        uint8_t ev[256];
        while (SDL_PollEvent_(ev)) {
            uint32_t type = *(uint32_t*)ev;
            if (type == SDL_EVENT_QUIT) return 0;
        }
    }
    static int frame = 0; frame++;
    g_ul_swap_frame = frame;  /* publish for tracers in other TUs */
    if (frame <= 3 || (frame % 30) == 0) loader_log(LOG_WARN, "[SDL3] frame %d", frame);
    { extern void jvs_poll(void); jvs_poll(); }   /* poll controllers on the SDL thread */

    /* dev hook: auto-open the overlay at frame N for headless verification */
    { const char* ao=getenv("ULTRA_OV_AUTOOPEN"); static int aof=0;
      if (ao && !pause_menu_is_active() && ++aof == atoi(ao)) ingame_overlay_request(0); }

    /* Headless snapshot self-test (frame-triggered, at this clean frame boundary
     * BEFORE the game is ticked this frame): ULTRA_SNAP_SAVE=N / ULTRA_SNAP_LOAD=M
     * use RAM slot 0; ULTRA_SNAP_SAVEFILE=N / ULTRA_SNAP_LOADFILE=M use ULTRA_SNAP_PATH. */
    { static int sv=-2, ld=-2, svf=-2, ldf=-2;
      if(sv==-2){ const char*e=getenv("ULTRA_SNAP_SAVE");     sv =e?atoi(e):-1; }
      if(ld==-2){ const char*e=getenv("ULTRA_SNAP_LOAD");     ld =e?atoi(e):-1; }
      if(svf==-2){const char*e=getenv("ULTRA_SNAP_SAVEFILE"); svf=e?atoi(e):-1; }
      if(ldf==-2){const char*e=getenv("ULTRA_SNAP_LOADFILE"); ldf=e?atoi(e):-1; }
      const char* sp=getenv("ULTRA_SNAP_PATH"); if(!sp) sp="ul_snap.uls";
      if(sv >=0 && frame==sv)  snapshot_save(0);
      if(svf>=0 && frame==svf) snapshot_save_file(sp);
      if(ld >=0 && frame==ld)  snapshot_load(0);
      if(ldf>=0 && frame==ldf) snapshot_load_file(sp); }

    /* In-game overlay (Start+Select / LB+RB). While it's up, freeze the game:
     * return before driving its vblank/display callback. Resumes on close. */
    if (ingame_overlay_pump()) return 1;

    /* Repair the ptcl_ctxt_list sentinel when empty (its self-init was NOP'd). Only
     * touches the heap-allocated sentinel pointed to by the head global, and only
     * when the element count is 0 - so a non-empty list (real particles) is never
     * disturbed. Runs after boot, before each frame's game logic. */
    if (g_ul_ptcl_head_addr && g_ul_ptcl_count_addr) {
        uint32_t cnt  = *(volatile uint32_t*)(uintptr_t)g_ul_ptcl_count_addr;
        uint32_t sent = *(volatile uint32_t*)(uintptr_t)g_ul_ptcl_head_addr;
        static int plog = 0;
        if (plog < 2) { loader_log(LOG_WARN, "[PTCL] head=0x%x count=%u frame=%d", sent, cnt, frame); plog++; }
        if (cnt == 0 && sent > 0x10000u && !IsBadWritePtr((void*)(uintptr_t)sent, 8)) {
            ((volatile uint32_t*)(uintptr_t)sent)[0] = sent;   /* _M_next = self */
            ((volatile uint32_t*)(uintptr_t)sent)[1] = sent;   /* _M_prev = self */
        }
    }

    /* Tick the game's vblank callback so its per-frame logic (which waits on the
     * vblank counter) advances and polls input - without this the game renders a
     * frozen INSERT COIN and never reacts. */
    if (g_ul_vblank_cb_addr && !getenv("ULTRA_NO_VBLANK")) {
        uint32_t fn = *(volatile uint32_t*)(uintptr_t)g_ul_vblank_cb_addr;
        static int vblog = 0;
        if (vblog < 1) { loader_log(LOG_INFO, "[VBLANK] driving game vblank cb 0x%x", fn); vblog++; }
        if (fn >= 0x08048000u && fn < 0x0a000000u && !IsBadCodePtr((FARPROC)(uintptr_t)fn)) {
            ((void(*)(void))(uintptr_t)fn)();
        }
    }

    /* Re-assert the per-frame hardware-ready/vblank flag the game's frame loop
     * gates on (and clears each frame). Without this the game freezes after the
     * first frame. Must happen before the display callback (which checks it). */
    if (g_ul_hwready_addr) {
        *(volatile uint8_t*)(uintptr_t)g_ul_hwready_addr       = 1;
        *(volatile uint8_t*)(uintptr_t)(g_ul_hwready_addr + 1) = 1;
        *(volatile uint8_t*)(uintptr_t)(g_ul_hwready_addr + 2) = 1;
    }

    /* The game stores its "screen size" in two floats (get_screen_width/height @
     * 0x09657950 / 0x09657954). The 2D menu UI is laid out relative to these, and
     * the 3D scene render-textures are sized from them. ULTRA_SCREEN_SCALE
     * multiplies both (aspect preserved) so the game re-lays-out + re-renders at a
     * corrected size. Logs the original once for diagnosis. */
    {
        static float s_ow = 0.0f, s_oh = 0.0f; static int s_got = 0, s_init = 0;
        static float s_scl = 1.0f, s_tw = 0.0f, s_th = 0.0f;
        if (!s_init) { s_init = 1;
            const char *e = getenv("ULTRA_SCREEN_SCALE"); if (e && *e) s_scl = (float)atof(e);
            const char *w = getenv("ULTRA_SCREEN_W");     if (w && *w) s_tw = (float)atof(w);
            const char *h = getenv("ULTRA_SCREEN_H");     if (h && *h) s_th = (float)atof(h);
        }
        volatile float *psw = (volatile float*)(uintptr_t)0x09657950u;
        volatile float *psh = (volatile float*)(uintptr_t)0x09657954u;
        if (!IsBadReadPtr((const void*)psw, 8)) {
            if (!s_got && *psw > 100.0f && *psw < 6000.0f && *psh > 100.0f && *psh < 6000.0f) {
                s_ow = *psw; s_oh = *psh; s_got = 1;
                loader_log(LOG_WARN, "[SCREEN] get_screen_size orig=%.1f x %.1f", s_ow, s_oh);
            }
            if (s_got && !IsBadWritePtr((void*)psw, 8)) {
                if (s_tw > 0.0f && s_th > 0.0f) { *psw = s_tw; *psh = s_th; }       /* explicit */
                else if (s_scl != 1.0f)         { *psw = s_ow * s_scl; *psh = s_oh * s_scl; }
            }
        }
    }

    /* GSEVO natural-START re-assert (sha-gated, inert otherwise). The guest's
     * make_switch_data (run in the vblank cb just above) REBUILDS swstate+8 every
     * frame, wiping any START bit we wrote at poll time. game_start_check (called
     * from the mode handler inside the display cb below) tests bit0 of swstate+8.
     * This is the only window - after make_switch_data, before game_start_check -
     * where writing swstate+8 bit0 survives to the gate, so the credited game
     * enters on the game's OWN path with no FSM override. See gsevo_post_vblank. */
    { extern void gsevo_post_vblank(void); gsevo_post_vblank(); }

    int snaptr = getenv("ULTRA_SNAP_TRACE") != 0;
    if (snaptr) loader_log(LOG_WARN, "[SNAPTRACE] f%d pre-displaycb", frame);
    if (g_ul_display_cb) {
        typedef void (*vfn)(void);
        ((vfn)(uintptr_t)g_ul_display_cb)();
    }
    if (snaptr) loader_log(LOG_WARN, "[SNAPTRACE] f%d post-displaycb / pre-present", frame);
    /* Up-scale the game's native content corner to fill the widescreen window
     * (the game composites into a 640x480 corner of the framebuffer). */
    { extern void glx_present_upscale(void); glx_present_upscale(); }
    if (snaptr) loader_log(LOG_WARN, "[SNAPTRACE] f%d post-present", frame);
    /* Flush the GL pipeline before presenting. The live present path otherwise
     * issues NO flush, so on a HARDWARE backend (zink/d3d12) the game's first
     * sync/readback can block on work that was recorded but never submitted to
     * the GPU - the "stalls at frame 2" symptom. glFinish on the first few frames
     * forces submission; glFlush after keeps steady-state cheap. Harmless on
     * llvmpipe (software). */
    {
        static void (__stdcall *p_finish)(void) = 0;
        static void (__stdcall *p_flush)(void)  = 0;
        static int resolved = 0;
        if (!resolved) {
            HMODULE gl = GetModuleHandleA("opengl32.dll");
            if (!gl) gl = LoadLibraryA("opengl32.dll");
            if (gl) {
                p_finish = (void(__stdcall*)(void))GetProcAddress(gl, "glFinish");
                p_flush  = (void(__stdcall*)(void))GetProcAddress(gl, "glFlush");
            }
            resolved = 1;
        }
        if (frame <= 4) { if (p_finish) p_finish(); }
        else            { if (p_flush)  p_flush();  }
    }
    /* Capture the freshly-rendered back buffer BEFORE the swap. ULTRA_DUMP_FRAME
     * accepts a comma-separated list (e.g. "90,130,170") to grab several frames. */
    { const char *df = getenv("ULTRA_DUMP_FRAME");
      if (df && *df) { char want[160], key[16];
        snprintf(want, sizeof want, ",%s,", df);
        snprintf(key,  sizeof key,  ",%d,", frame);
        if (strstr(want, key)) dump_frame_bmp(frame); } }
    /* Dev hook: clean self-terminate at a given frame (after the dump above ran),
     * so reconnaissance runs don't leave a window open. Inert without the env. */
    { const char *qf = getenv("ULTRA_QUIT_FRAME");
      if (qf && *qf) { int q = atoi(qf); if (q > 0 && frame >= q) {
          loader_log(LOG_WARN, "[SDL3] ULTRA_QUIT_FRAME=%d reached at frame %d, exiting", q, frame);
          fflush(stderr); ExitProcess(0); } } }
    if (frame >= 2) splash_content_check();   /* fade splash on first non-black frame */
    /* Ultra Omni-Track thin white aim border (camera light guns) - gated on
     * g_settings.lightgun_border; byte-identical when off. Drawn after the frame
     * is presented, before the buffer swap. */
    { extern void glx_draw_aim_border(void); glx_draw_aim_border(); }
    if (g_ul_swap_pending) {
        g_ul_swap_pending = 0;
        if (SDL_GL_SwapWindow_) SDL_GL_SwapWindow_(g_window);
    }
    return 1;
}
