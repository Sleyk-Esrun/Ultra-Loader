/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  gl_present.c -- frame limiter + present capture for the x86 arcade PC boards
 *                  that render with OpenGL (Win32 WGL). Sibling of d3d11_throttle.c
 *                  (DXGI), d3d9_throttle.c (D3D9) and d3d8_throttle.c (D3D8).
 * =============================================================================
 *
 *  A handful of PC arcade boards present through OpenGL instead of Direct3D --
 *  notably Namco System N2 titles (and any other GL board a future dump lands on).
 *  Unlike D3D8/9/11 (where Present/Present is a COM vtable method), a Win32 GL app
 *  swaps its back buffer through a FLAT C export:
 *
 *      opengl32.dll!wglSwapBuffers(HDC)     -- the GL buffer swap
 *      gdi32.dll!SwapBuffers(HDC)           -- the GDI buffer swap (wgl tail-calls it)
 *
 *  There is no vtable to patch -- the hook IS the exported shim. We acquire the
 *  entry points two ways, whichever fires first (mirroring the D3D11 dual path):
 *    1. IAT patch on opengl32.dll!wglSwapBuffers (+ gdi32.dll!SwapBuffers) -- both
 *       wired in jvs_serial_hook.c's install path.
 *    2. GetProcAddress interception -- Hook_GetProcAddress (jvs_serial_hook.c)
 *       already returns ultra_Hook_wglSwapBuffers for the name "wglSwapBuffers",
 *       covering a game that resolves the swap fn at runtime.
 *
 *  A GL title may swap via EITHER export (opengl32!wglSwapBuffers internally
 *  tail-calls gdi32!SwapBuffers), so we hook BOTH and a shared g_swapped guard
 *  makes the first-swap-of-a-frame win so a frame is never double-throttled.
 *
 *  Present-time behavior, identical contract to the D3D8/9/11 hooks:
 *    - throttle the swap to a target fps (ULTRA_FPS_CAP, or a cabinet 60Hz default
 *      when ULTRA_GL_BOARD/ULTRA_GL_PRESENT=1). QPC limiter, byte-for-byte the
 *      d3d11_throttle.c algorithm.
 *    - env-gated back-buffer BMP dump (ULTRA_PRESENT_DUMP / ULTRA_ROT_DUMP = dir,
 *      cadence via ULTRA_DUMP_EVERY) -- the GL analog of the D3D11 staging dumper.
 *      GL has no GetRenderTargetData/CopyResource; the canonical CPU read-back is
 *      glReadPixels(GL_BACK, GL_BGRA) against the back buffer, then write the SAME
 *      24-bit bottom-up BMP the other three dumpers write, so the same offline
 *      tooling reads all four. NB GL's origin is bottom-left and the BMP is already
 *      bottom-up, so GL rows map straight down with NO vertical flip (the one place
 *      the GL dumper is simpler than D3D, which is top-left written bottom-up).
 *    - force-windowed option (ULTRA_SEGA_WINDOWED / auto): a GL arcade title goes
 *      exclusive-fullscreen via ChangeDisplaySettings[Ex] + a borderless topmost
 *      window, not a swapchain flag. We intercept the mode switch and swallow it so
 *      a focus change can't black-stall the unattended loop -- the GL analog of the
 *      D3D11 SetFullscreenState(FALSE) / D3D9 exclusive-device-lost fix.
 *
 *  EVERY knob is DEFAULT-OFF. With nothing armed the only added work per swap is the
 *  limiter (a pure no-op when g_target_fps is 0) and a single forwarded call, so the
 *  native render path is byte-identical -- Type X / X2 (D3D8/D3D9) never touch this.
 *
 *  HONESTY: this is SCAFFOLDING. It compiles clean against the mingw GL/WGL/GDI
 *  headers, exports the symbol the install path expects (ultra_Hook_wglSwapBuffers),
 *  and is lifecycle-validated. The capture path is proven against a synthetic GL
 *  present app (test_gl_capture.c). It is NOT verified against a real N2 title -- we
 *  have no dump. The board is "wired and ready for a dump", NOT verified-playable.
 *
 *  Clean-room: Ultra Loader's own code written from the public WGL / GDI / OpenGL
 *  1.1 SDK semantics only. No third-party loader source consulted.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 *  ultra_raw_export -- resolve an export by WALKING THE MODULE'S EXPORT TABLE.
 *
 *  Why this exists (crash reproduced live on Mario Kart GP DX, 2026-09-09):
 *  this hook installs its own Hook_GetProcAddress, which returns OUR wrapper
 *  for exactly the names below (Direct3DCreate9, D3D11CreateDeviceAndSwapChain,
 *  CreateDXGIFactory[1], wglSwapBuffers, SwapBuffers) so that PACKED games --
 *  which have no static IAT slot and resolve the renderer at runtime -- still
 *  get hooked. But the wrappers themselves then did:
 *
 *      Real_X = GetProcAddress(module, "X");   // <-- our own hook answers
 *      ... Real_X(...)                          // <-- calls ITSELF, forever
 *
 *  so the wrapper resolved itself as "the real one" and recursed until the
 *  stack died: ACCESS_VIOLATION inside the hook, right after the
 *  "hook armed (packed path)" line appears TWICE in the log.
 *
 *  Reading the export directory directly cannot be intercepted, so it always
 *  yields the genuine implementation. Returns NULL for a forwarded export
 *  (RVA pointing back inside the export directory), which the callers already
 *  treat as "no real function".
 * ------------------------------------------------------------------------- */
static FARPROC ultra_raw_export(HMODULE mod, const char* want) {
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos; IMAGE_NT_HEADERS* nt; IMAGE_DATA_DIRECTORY d;
    IMAGE_EXPORT_DIRECTORY* ed; DWORD *names, *funcs; WORD* ords; DWORD i;
    if (!base || !want) return NULL;
    dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!d.VirtualAddress || !d.Size) return NULL;
    ed    = (IMAGE_EXPORT_DIRECTORY*)(base + d.VirtualAddress);
    names = (DWORD*)(base + ed->AddressOfNames);
    ords  = (WORD*) (base + ed->AddressOfNameOrdinals);
    funcs = (DWORD*)(base + ed->AddressOfFunctions);
    for (i = 0; i < ed->NumberOfNames; ++i) {
        const char* nm = (const char*)(base + names[i]);
        if (strcmp(nm, want) == 0) {
            DWORD rva = funcs[ords[i]];
            if (rva >= d.VirtualAddress && rva < d.VirtualAddress + d.Size)
                return NULL;                     /* forwarded export */
            return (FARPROC)(void*)(base + rva);
        }
    }
    return NULL;
}

/* GL constants we need -- declared locally so we never depend on a GL header being
 * present at the call site (mingw ships <GL/gl.h>, but resolving the few enums we
 * use by value keeps this unit header-light and matches the dynamic-resolve style). */
#ifndef GL_BACK
#define GL_BACK                  0x0405
#endif
#ifndef GL_RGBA
#define GL_RGBA                  0x1908
#endif
#ifndef GL_BGRA
#define GL_BGRA                  0x80E1   /* GL_BGRA_EXT, core since GL 1.2 */
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE         0x1401
#endif
#ifndef GL_PACK_ALIGNMENT
#define GL_PACK_ALIGNMENT        0x0D05
#endif
#ifndef GL_VIEWPORT
#define GL_VIEWPORT              0x0BA2
#endif
#ifndef GL_READ_BUFFER
#define GL_READ_BUFFER           0x0C02
#endif

/* shared logger (gated by ULTRA_JVS_LOG); same file the JVS/D3D8/D3D9/D3D11 hooks
 * use. ultra_d3d8_log appends its own '\n', so format strings carry no newline. */
extern void ultra_d3d8_log(const char* fmt, ...);
#define glog ultra_d3d8_log

extern void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* ---- frame limiter (QPC, identical algorithm to d3d11) -------------------- */
static double        g_target_fps  = 0.0;   /* 0 = no cap until armed            */
static LARGE_INTEGER g_freq, g_last;
static int           g_timing_init = 0;
static int           g_raised_timer = 0;

static void throttle(void) {
    if (g_target_fps <= 0.0) return;            /* limiter disarmed: byte-identical */
    if (!g_timing_init) {
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_last);
        timeBeginPeriod(1); g_raised_timer = 1;
        g_timing_init = 1;
        glog("GL: frame limiter armed @ %.2f fps", g_target_fps);
        return;                                 /* first swap: baseline only         */
    }
    const double target = (double)g_freq.QuadPart / g_target_fps;
    LARGE_INTEGER now;
    for (;;) {
        QueryPerformanceCounter(&now);
        const double elapsed = (double)(now.QuadPart - g_last.QuadPart);
        if (elapsed >= target) break;
        const double remain_ms = (target - elapsed) * 1000.0 / (double)g_freq.QuadPart;
        if (remain_ms > 3.0) Sleep((DWORD)(remain_ms - 2.0));
        else YieldProcessor();
    }
    g_last.QuadPart += (LONGLONG)target;
    if ((double)(now.QuadPart - g_last.QuadPart) > target * 4.0) g_last = now;

    static LARGE_INTEGER mark; static int frames = 0;
    if (frames == 0) mark = now;
    if (++frames >= 180) {
        double secs = (double)(now.QuadPart - mark.QuadPart) / (double)g_freq.QuadPart;
        if (secs > 0.0) glog("GL: measured %.1f fps (cap %.0f)", frames / secs, g_target_fps);
        frames = 0;
    }
}

/* ---- env-gated knobs (read once, lazily, on first swap) ------------------- */
static const char* g_dump_dir   = NULL;     /* ULTRA_PRESENT_DUMP / ULTRA_ROT_DUMP   */
static int         g_dump_every = 0;        /* ULTRA_DUMP_EVERY=N (0 = sample cadence)*/
static int         g_force_windowed = 0;    /* ULTRA_SEGA_WINDOWED / auto            */
static unsigned    g_present_count = 0;

static int force_windowed_env(void) {
    const char* w = getenv("ULTRA_SEGA_WINDOWED");
    if (w && *w) return (w[0] == '1');
    const char* a = getenv("ULTRA_TYPEX2_AUTO");        /* shared auto-pilot flag     */
    return (a && a[0] == '1');
}

static void read_knobs(void) {
    const char* cap = getenv("ULTRA_FPS_CAP");
    if (cap && *cap) { double v = atof(cap); if (v >= 1.0 && v <= 1000.0) g_target_fps = v; }
    /* a GL cabinet defaults to 60Hz when the board profile arms the GL hook. */
    if (g_target_fps <= 0.0) {
        const char* b = getenv("ULTRA_GL_BOARD");
        const char* p = getenv("ULTRA_GL_PRESENT");
        if ((b && b[0]=='1') || (p && p[0]=='1')) g_target_fps = 60.0;
    }
    g_dump_dir = getenv("ULTRA_PRESENT_DUMP");
    if (!g_dump_dir || !*g_dump_dir) g_dump_dir = getenv("ULTRA_ROT_DUMP");
    const char* e = getenv("ULTRA_DUMP_EVERY");
    g_dump_every = (e && *e) ? atoi(e) : 0;
    g_force_windowed = force_windowed_env();
}

/* sample several swaps so a content frame is reliably caught (attract loops through
 * black transitions); same cadence as the D3D8/D3D9/D3D11 dumpers. ULTRA_DUMP_EVERY
 * overrides with a periodic dump every N swaps. */
static int dump_due(unsigned n) {
    if (g_dump_every > 0) return (n % (unsigned)g_dump_every) == 0;
    return (n==120||n==300||n==600||n==900||n==1200||n==1500||n==1800||n==2100);
}

/* ---- dynamically-resolved GL 1.1 entry points ----------------------------- *
 * We never static-link these: mingw's opengl32 import lib only guarantees GL 1.1,
 * and resolving against the LIVE opengl32.dll is the same robustness stance the
 * D3D11 hook uses for its real export. All of these are core GL 1.0/1.1 (BGRA is
 * the GL 1.2 GL_BGRA_EXT token, present on every desktop driver) so they live in
 * opengl32 directly -- NOT via wglGetProcAddress (which returns NULL for core 1.1). */
typedef void    (__stdcall *PFN_glReadBuffer)(unsigned int mode);
typedef void    (__stdcall *PFN_glReadPixels)(int x, int y, int w, int h,
                                              unsigned int fmt, unsigned int type, void* px);
typedef void    (__stdcall *PFN_glPixelStorei)(unsigned int pname, int param);
typedef void    (__stdcall *PFN_glGetIntegerv)(unsigned int pname, int* params);
typedef unsigned int (__stdcall *PFN_glGetError)(void);
typedef HGLRC   (WINAPI *PFN_wglGetCurrentContext)(void);
typedef HDC     (WINAPI *PFN_wglGetCurrentDC)(void);

static PFN_glReadBuffer         p_glReadBuffer    = NULL;
static PFN_glReadPixels         p_glReadPixels    = NULL;
static PFN_glPixelStorei        p_glPixelStorei   = NULL;
static PFN_glGetIntegerv        p_glGetIntegerv   = NULL;
static PFN_glGetError           p_glGetError      = NULL;
static PFN_wglGetCurrentContext p_wglGetCurContext= NULL;
static PFN_wglGetCurrentDC      p_wglGetCurDC     = NULL;
static int                      g_gl_resolved     = 0;

static void resolve_gl(void) {
    if (g_gl_resolved) return;
    HMODULE m = GetModuleHandleA("opengl32.dll");
    if (!m) m = LoadLibraryA("opengl32.dll");
    if (!m) return;
    p_glReadBuffer     = (PFN_glReadBuffer)    (void*)GetProcAddress(m, "glReadBuffer");
    p_glReadPixels     = (PFN_glReadPixels)    (void*)GetProcAddress(m, "glReadPixels");
    p_glPixelStorei    = (PFN_glPixelStorei)   (void*)GetProcAddress(m, "glPixelStorei");
    p_glGetIntegerv    = (PFN_glGetIntegerv)   (void*)GetProcAddress(m, "glGetIntegerv");
    p_glGetError       = (PFN_glGetError)      (void*)GetProcAddress(m, "glGetError");
    p_wglGetCurContext = (PFN_wglGetCurrentContext)(void*)GetProcAddress(m, "wglGetCurrentContext");
    p_wglGetCurDC      = (PFN_wglGetCurrentDC) (void*)GetProcAddress(m, "wglGetCurrentDC");
    g_gl_resolved = 1;
}

/* ---- back-buffer capture -> 24-bit bottom-up BMP -------------------------- *
 * The GL analog of the D3D11 staging dumper. fmt path: we ask glReadPixels for
 * GL_BGRA so the bytes already match the BMP's BGR order (no per-pixel R/B swap --
 * the same default branch the D3D11 B8G8R8A8 writer takes). A strict driver that
 * rejects BGRA falls back to GL_RGBA + R/B swap. Orientation: GL origin is
 * bottom-left and the BMP is bottom-up, so rows map straight (no vertical flip). */
static void write_bmp_bgra(const char* path, const unsigned char* base,
                           int w, int h, int rgba_swapped) {
    int rowsz = w * 3, pad = (4 - (rowsz & 3)) & 3, stride = rowsz + pad;
    int imgsz = stride * h;
    unsigned char fhh[14] = {0}, ihh[40] = {0};
    unsigned filesz = 14 + 40 + (unsigned)imgsz;
    fhh[0]='B'; fhh[1]='M'; *(unsigned*)(fhh+2)=filesz; *(unsigned*)(fhh+10)=54;
    *(unsigned*)(ihh+0)=40; *(int*)(ihh+4)=w; *(int*)(ihh+8)=h;   /* +h = bottom-up */
    *(unsigned short*)(ihh+12)=1; *(unsigned short*)(ihh+14)=24; *(unsigned*)(ihh+20)=(unsigned)imgsz;
    FILE* f = fopen(path, "wb");
    if (!f) { glog("GL dump: fopen('%s') failed", path); return; }
    fwrite(fhh,1,14,f); fwrite(ihh,1,40,f);
    unsigned char* row = (unsigned char*)malloc(stride);
    if (!row) { fclose(f); return; }
    /* GL row 0 is image-BOTTOM; BMP is written bottom-up (bottom row first), so we
     * emit GL rows in their natural order 0..h-1 -- identity, no flip. */
    for (int y = 0; y < h; ++y) {
        const unsigned char* src = base + (size_t)y * (size_t)w * 4;
        memset(row, 0, stride);
        if (rgba_swapped) {                         /* RGBA in memory -> BGR */
            for (int x = 0; x < w; ++x) {
                row[x*3+0] = src[x*4+2]; row[x*3+1] = src[x*4+1]; row[x*3+2] = src[x*4+0];
            }
        } else {                                    /* BGRA in memory -> direct BGR */
            for (int x = 0; x < w; ++x) {
                row[x*3+0] = src[x*4+0]; row[x*3+1] = src[x*4+1]; row[x*3+2] = src[x*4+2];
            }
        }
        fwrite(row, 1, stride, f);
    }
    free(row); fclose(f);
    glog("GL: dumped %dx%d %s -> %s", w, h, rgba_swapped ? "RGBA" : "BGRA", path);
}

/* Read the current GL back buffer of HDC into a 24-bit BMP. Guards: a context must
 * be current (the swap fn runs on the render thread so it is) and the drawable size
 * must be sane. */
static void dump_backbuffer_gl(HDC hdc, const char* path) {
    resolve_gl();
    if (!p_glReadPixels || !p_glReadBuffer || !p_glPixelStorei || !p_glGetIntegerv) {
        glog("GL dump: glReadPixels family unresolved"); return;
    }
    /* a context must be current on this thread, and (if available) match hdc */
    if (p_wglGetCurContext && !p_wglGetCurContext()) {
        glog("GL dump: no current GL context -- skipping"); return;
    }

    /* drawable size from the window the HDC belongs to; fall back to GL_VIEWPORT. */
    int w = 0, h = 0;
    HWND hwnd = hdc ? WindowFromDC(hdc) : NULL;
    if (hwnd) {
        RECT rc; if (GetClientRect(hwnd, &rc)) { w = rc.right - rc.left; h = rc.bottom - rc.top; }
    }
    if (w <= 0 || h <= 0) {
        int vp[4] = {0,0,0,0};
        p_glGetIntegerv(GL_VIEWPORT, vp);
        w = vp[2]; h = vp[3];
    }
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        glog("GL dump: implausible drawable %dx%d -- skipping", w, h); return;
    }

    size_t bytes = (size_t)w * (size_t)h * 4;
    unsigned char* buf = (unsigned char*)malloc(bytes);
    if (!buf) { glog("GL dump: malloc(%zu) failed", bytes); return; }

    /* save + set pack alignment to 1 (tight rows), read GL_BACK, restore. */
    int prev_align = 4, prev_readbuf = GL_BACK;
    p_glGetIntegerv(GL_PACK_ALIGNMENT, &prev_align);
    p_glGetIntegerv(GL_READ_BUFFER, &prev_readbuf);
    p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
    p_glReadBuffer(GL_BACK);

    int rgba_swapped = 0;
    if (p_glGetError) while (p_glGetError() != 0) { /* drain pre-existing errors */ }
    p_glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, buf);
    if (p_glGetError && p_glGetError() != 0) {
        /* strict GLES-style driver rejected BGRA: fall back to RGBA + swap */
        p_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        rgba_swapped = 1;
    }

    /* restore prior pixel-store / read-buffer state so we don't perturb the game. */
    p_glPixelStorei(GL_PACK_ALIGNMENT, prev_align);
    p_glReadBuffer((unsigned int)prev_readbuf);

    write_bmp_bgra(path, buf, w, h, rgba_swapped);
    free(buf);
}

/* ---- force-windowed: swallow ChangeDisplaySettings[Ex] mode switches ------- *
 * A GL arcade title switches to an exclusive fullscreen mode via
 * ChangeDisplaySettings[Ex] (+ a WS_POPUP/WS_EX_TOPMOST borderless window). Under
 * the unattended auto-pilot, a focus loss on that exclusive mode can black-stall
 * the loop (the GL analog of the D3D9 exclusive-device-lost / DXGI fullscreen
 * problem). When force-windowed is armed we return DISP_CHANGE_SUCCESSFUL WITHOUT
 * switching, so the game keeps a focus-independent windowed drawable. */
typedef LONG (WINAPI *PFN_CDS_A)(DEVMODEA*, DWORD);
typedef LONG (WINAPI *PFN_CDS_W)(DEVMODEW*, DWORD);
typedef LONG (WINAPI *PFN_CDSEx_A)(LPCSTR, DEVMODEA*, HWND, DWORD, LPVOID);
typedef LONG (WINAPI *PFN_CDSEx_W)(LPCWSTR, DEVMODEW*, HWND, DWORD, LPVOID);
static PFN_CDS_A   Real_CDS_A   = NULL;
static PFN_CDS_W   Real_CDS_W   = NULL;
static PFN_CDSEx_A Real_CDSEx_A = NULL;
static PFN_CDSEx_W Real_CDSEx_W = NULL;

/* swallow only a genuine mode SET (lpDevMode != NULL and not a CDS_TEST probe);
 * a NULL devmode means "reset to registry default" which we let through. */
static int swallow_mode_change(const void* devmode, DWORD flags) {
    if (!g_force_windowed) return 0;
    if (!devmode) return 0;                 /* reset-to-default: pass through */
    if (flags & CDS_TEST) return 0;         /* a capability probe: let it answer truthfully */
    return 1;
}

static LONG WINAPI Hook_CDS_A(DEVMODEA* dm, DWORD flags) {
    if (swallow_mode_change(dm, flags)) {
        glog("GL: force-windowed -> swallowed ChangeDisplaySettingsA mode switch");
        return DISP_CHANGE_SUCCESSFUL;
    }
    return Real_CDS_A ? Real_CDS_A(dm, flags) : DISP_CHANGE_FAILED;
}
static LONG WINAPI Hook_CDS_W(DEVMODEW* dm, DWORD flags) {
    if (swallow_mode_change(dm, flags)) {
        glog("GL: force-windowed -> swallowed ChangeDisplaySettingsW mode switch");
        return DISP_CHANGE_SUCCESSFUL;
    }
    return Real_CDS_W ? Real_CDS_W(dm, flags) : DISP_CHANGE_FAILED;
}
static LONG WINAPI Hook_CDSEx_A(LPCSTR dev, DEVMODEA* dm, HWND hwnd, DWORD flags, LPVOID p) {
    if (swallow_mode_change(dm, flags)) {
        glog("GL: force-windowed -> swallowed ChangeDisplaySettingsExA mode switch");
        return DISP_CHANGE_SUCCESSFUL;
    }
    return Real_CDSEx_A ? Real_CDSEx_A(dev, dm, hwnd, flags, p) : DISP_CHANGE_FAILED;
}
static LONG WINAPI Hook_CDSEx_W(LPCWSTR dev, DEVMODEW* dm, HWND hwnd, DWORD flags, LPVOID p) {
    if (swallow_mode_change(dm, flags)) {
        glog("GL: force-windowed -> swallowed ChangeDisplaySettingsExW mode switch");
        return DISP_CHANGE_SUCCESSFUL;
    }
    return Real_CDSEx_W ? Real_CDSEx_W(dev, dm, hwnd, flags, p) : DISP_CHANGE_FAILED;
}

static void install_force_windowed(void) {
    HMODULE u = GetModuleHandleA("user32.dll");
    if (!u) u = LoadLibraryA("user32.dll");
    if (u) {
        if (!Real_CDS_A)   Real_CDS_A   = (PFN_CDS_A)  (void*)GetProcAddress(u, "ChangeDisplaySettingsA");
        if (!Real_CDS_W)   Real_CDS_W   = (PFN_CDS_W)  (void*)GetProcAddress(u, "ChangeDisplaySettingsW");
        if (!Real_CDSEx_A) Real_CDSEx_A = (PFN_CDSEx_A)(void*)GetProcAddress(u, "ChangeDisplaySettingsExA");
        if (!Real_CDSEx_W) Real_CDSEx_W = (PFN_CDSEx_W)(void*)GetProcAddress(u, "ChangeDisplaySettingsExW");
    }
    ultra_iat_patch_all("user32.dll", "ChangeDisplaySettingsA",   (void*)Hook_CDS_A);
    ultra_iat_patch_all("user32.dll", "ChangeDisplaySettingsW",   (void*)Hook_CDS_W);
    ultra_iat_patch_all("user32.dll", "ChangeDisplaySettingsExA", (void*)Hook_CDSEx_A);
    ultra_iat_patch_all("user32.dll", "ChangeDisplaySettingsExW", (void*)Hook_CDSEx_W);
    glog("GL: force-windowed mode-switch interception armed");
}

/* ---- the shared swap shim ------------------------------------------------- *
 * Both ultra_Hook_wglSwapBuffers (opengl32) and ultra_Hook_SwapBuffers (gdi32) land
 * here. A per-frame guard makes the first swap of a frame do the throttle/dump and
 * a back-to-back second swap (when a game double-calls, or when wgl tail-calls gdi)
 * is forwarded untouched, so a frame is never double-limited. */
static volatile LONG g_in_swap = 0;
static int           g_knobs_read = 0;

static void swap_common(HDC hdc) {
    if (!g_knobs_read) { g_knobs_read = 1; read_knobs();
        if (g_force_windowed) install_force_windowed(); }
    throttle();
    ++g_present_count;
    if (g_dump_dir && *g_dump_dir && dump_due(g_present_count)) {
        char p[512];
        snprintf(p, sizeof p, "%s/gl_%04u.bmp", g_dump_dir, g_present_count);
        /* dump BEFORE the real swap: the just-rendered frame is still in GL_BACK
         * (the swap is what rotates it to the front). */
        dump_backbuffer_gl(hdc, p);
    }
}

/* the real exports, resolved once. */
typedef BOOL (__stdcall *PFN_wglSwapBuffers)(HDC);
typedef BOOL (WINAPI    *PFN_SwapBuffers)(HDC);
static PFN_wglSwapBuffers Real_wglSwapBuffers = NULL;
static PFN_SwapBuffers    Real_SwapBuffers    = NULL;

static void resolve_real_swaps(void) {
    if (!Real_wglSwapBuffers) {
        HMODULE m = GetModuleHandleA("opengl32.dll"); if (!m) m = LoadLibraryA("opengl32.dll");
        if (m) Real_wglSwapBuffers = (PFN_wglSwapBuffers)(void*)ultra_raw_export(m, "wglSwapBuffers");
    }
    if (!Real_SwapBuffers) {
        HMODULE g = GetModuleHandleA("gdi32.dll"); if (!g) g = LoadLibraryA("gdi32.dll");
        if (g) Real_SwapBuffers = (PFN_SwapBuffers)(void*)ultra_raw_export(g, "SwapBuffers");
    }
}

/* Exported (no name-mangle, __stdcall) -- referenced by jvs_serial_hook.c's IAT
 * patch + GetProcAddress interception. REPLACES the logging passthrough that used
 * to live in d3d8_throttle.c. */
BOOL __stdcall ultra_Hook_wglSwapBuffers(HDC hdc) {
    resolve_real_swaps();
    /* if a re-entrant gdi SwapBuffers fires inside this (wgl tail-calls gdi), the
     * guard makes the inner one a plain passthrough so the frame isn't double-hit. */
    if (InterlockedExchange(&g_in_swap, 1) == 0) {
        swap_common(hdc);
        InterlockedExchange(&g_in_swap, 0);
    }
    return Real_wglSwapBuffers ? Real_wglSwapBuffers(hdc) : TRUE;
}

/* gdi32 sibling -- patched for GL titles that swap via gdi32!SwapBuffers directly.
 * Exported so jvs_serial_hook.c can IAT-patch / GetProcAddress-intercept it too. */
BOOL WINAPI ultra_Hook_SwapBuffers(HDC hdc) {
    resolve_real_swaps();
    if (InterlockedExchange(&g_in_swap, 1) == 0) {
        swap_common(hdc);
        InterlockedExchange(&g_in_swap, 0);
    }
    return Real_SwapBuffers ? Real_SwapBuffers(hdc) : TRUE;
}

/* ---- install (hook_dllmain.c calls this under ULTRA_GL_PRESENT/ULTRA_GL_BOARD) - */
static volatile LONG g_installed = 0;

void ultra_gl_install(void) {
    if (InterlockedExchange(&g_installed, 1)) return;   /* idempotent */
    read_knobs(); g_knobs_read = 1;
    resolve_real_swaps();
    /* Patch BOTH swap exports: opengl32!wglSwapBuffers AND gdi32!SwapBuffers, since
     * a GL title may call either (wgl internally tail-calls gdi). The opengl32 IAT
     * patch + GetProcAddress interception also live in jvs_serial_hook.c; this adds
     * the gdi32 sibling + re-arms for a late-loaded opengl32. */
    ultra_iat_patch_all("opengl32.dll", "wglSwapBuffers", (void*)ultra_Hook_wglSwapBuffers);
    ultra_iat_patch_all("gdi32.dll",    "SwapBuffers",    (void*)ultra_Hook_SwapBuffers);
    if (g_force_windowed) install_force_windowed();
    glog("GL: install fps_cap=%.1f dump=%s every=%d windowed=%d",
         g_target_fps, g_dump_dir ? g_dump_dir : "(off)", g_dump_every, g_force_windowed);
}

void ultra_gl_teardown(void) {
    if (!InterlockedExchange(&g_installed, 0)) return;
    if (g_raised_timer) { timeEndPeriod(1); g_raised_timer = 0; }
    /* the IAT patches are left in place: process teardown frees the address space
     * wholesale; an orderly unload would restore the cached Real_* pointers. */
}

#endif /* _WIN32 */
