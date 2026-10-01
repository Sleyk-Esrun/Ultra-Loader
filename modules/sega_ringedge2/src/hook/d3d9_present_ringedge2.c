/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  d3d9_present_ringedge2.c -- D3D9 frame limiter + present post-process for
 *                              the Sega RingEdge 2 board (this board's own
 *                              copy; earlier RingEdge 2 titles render D3D9).
 * =============================================================================
 *
 *  RingEdge 2 titles pace their game loop off Present (the cabinet's 60Hz
 *  vsync); windowed on a PC that runs unthrottled (fast-forwarded). We
 *  IAT-hook d3d9.dll!Direct3DCreate9 -> IDirect3D9::CreateDevice ->
 *  IDirect3DDevice9::Present and:
 *    - throttle Present to a target fps (default 60, ULTRA_FPS_CAP),
 *    - optionally rotate / upscale-to-window / sharpen (all DEFAULT-OFF, same
 *      env contract every present hook in this project uses),
 *    - dump the back buffer to a 24-bit BMP (ULTRA_ROT_DUMP=dir) for offline
 *      verification of a packed/headless game we cannot screenshot,
 *    - force-windowed option (ULTRA_SEGA_WINDOWED / ULTRA_WINDOWED /
 *      ULTRA_BORDERLESS): a fullscreen-EXCLUSIVE D3D9 device is LOST whenever
 *      it loses foreground focus; running windowed makes the device
 *      focus-independent.
 *
 *  This board is MIXED (BP_HOOK_D3D9_AND_D3D11): this hook and
 *  d3d11_present_ringedge2.c are BOTH always armed. Whichever graphics API the
 *  running title actually calls is the one that ever produces a live device --
 *  the other hook's IAT patch is simply never exercised. That is this board's
 *  "first create-fn wins" mixed-board detection.
 *
 *  Clean-room: Ultra Loader's own code over the public COM / D3D9 ABI. No
 *  dependency on modules/native_x86/src/hook/'s compiled output (read as
 *  reference only).
 *
 *  D3D9 ABI notes (verified against the IDirect3DDevice9 vtable):
 *    - IDirect3D9::CreateDevice          = slot 16
 *    - IDirect3DDevice9::Reset           = slot 16
 *    - IDirect3DDevice9::Present         = slot 17
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#define DIRECT3D_VERSION 0x0900
#define COBJMACROS
#include <d3d9.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ringedge2_hook.h"

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

/* COM vtable indices (IDirect3D9 / IDirect3DDevice9 ABI). */
#define IDIRECT3D9_CREATEDEVICE      16
#define IDIRECT3DDEVICE9_RESET       16
#define IDIRECT3DDEVICE9_PRESENT     17

typedef HRESULT (__stdcall *pCreateDevice9)(void* self, UINT adapter, D3DDEVTYPE devtype,
                                            HWND hwnd, DWORD flags,
                                            D3DPRESENT_PARAMETERS* pp, void** ppdev);
typedef HRESULT (__stdcall *pPresent9)(void* self, const RECT* src, const RECT* dst,
                                       HWND hwndOverride, const RGNDATA* dirty);
typedef HRESULT (__stdcall *pReset9)(void* self, D3DPRESENT_PARAMETERS* pp);

static pCreateDevice9 Real_CreateDevice9 = NULL;
static pPresent9      Real_Present9      = NULL;
static pReset9        Real_Reset9        = NULL;
static int            g_present_hooked   = 0;

/* ---- upscaler / rotation / sharpen state ----------------------------------- */
static HWND g_devwnd   = NULL;
static int  g_up_w = 0, g_up_h = 0;     /* target client size (0 = no upscale)  */
static int  g_upscaled = 0;
static int  g_rotate   = 0;             /* 0 / 90 / 180 / 270                    */
static int  g_aspect_fill = 1;          /* 1 = stretch, 0 = keep aspect          */
static int  g_bb_w = 640, g_bb_h = 480; /* game backbuffer size (capture src)    */

static void read_target_res(void) {
    const char* r = ul_re2_env("ULTRA_RES");
    if (r && *r) {
        if      (!strcmp(r, "1080p")) { g_up_w = 1920; g_up_h = 1080; }
        else if (!strcmp(r, "1440p")) { g_up_w = 2560; g_up_h = 1440; }
        else if (!strcmp(r, "4k") || !strcmp(r, "2160p")) { g_up_w = 3840; g_up_h = 2160; }
        else { int w=0,h=0; if (sscanf(r, "%dx%d", &w, &h) == 2 && w>0 && h>0) { g_up_w=w; g_up_h=h; } }
    }
    const char* w = ul_re2_env("ULTRA_RES_W"); const char* h = ul_re2_env("ULTRA_RES_H");
    if (w && *w && h && *h) { int wi=atoi(w), hi=atoi(h); if (wi>0&&hi>0){ g_up_w=wi; g_up_h=hi; } }
    if (g_up_w == 0 && ul_re2_env_on("ULTRA_BORDERLESS")) {
        g_up_w = GetSystemMetrics(SM_CXSCREEN);
        g_up_h = GetSystemMetrics(SM_CYSCREEN);
    }
    const char* rot = ul_re2_env("ULTRA_ROTATE");
    if (rot && *rot) { int v = atoi(rot); if (v==90||v==180||v==270) g_rotate = v; }
    const char* asp = ul_re2_env("ULTRA_ASPECT");
    char ac = (asp && *asp) ? asp[0] : 0;
    if (ac=='k'||ac=='K'||ac=='p'||ac=='P') g_aspect_fill = 0;
    else if (ac=='f'||ac=='F'||ac=='s'||ac=='S') g_aspect_fill = 1;
    else if (g_rotate) g_aspect_fill = 0;
    if (g_rotate && g_up_w == 0) { g_up_w = 1920; g_up_h = 1080; }
}

/* Force-windowed control: default on for this board (sega_ringedge2_main.c
 * sets ULTRA_SEGA_WINDOWED=1 unless the user already set it). */
static int force_windowed(void) {
    static int v = -1;
    if (v < 0) {
        const char* sw = ul_re2_env("ULTRA_SEGA_WINDOWED");
        const char* uw = ul_re2_env("ULTRA_WINDOWED");
        const char* bl = ul_re2_env("ULTRA_BORDERLESS");
        if (sw && *sw) v = (sw[0] == '1');
        else v = ((uw && uw[0]=='1') || (bl && bl[0]=='1')) ? 1 : 0;
    }
    return v;
}

/* ---- frame limiter (QPC) --------------------------------------------------- */
static double        g_target_fps = 60.0;
static LARGE_INTEGER g_freq, g_last;
static int           g_timing_init = 0;

static void throttle(void) {
    if (!g_timing_init) {
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_last);
        const char* e = ul_re2_env("ULTRA_FPS_CAP");
        if (e && *e) { double v = atof(e); if (v >= 1.0 && v <= 1000.0) g_target_fps = v; }
        timeBeginPeriod(1);
        g_timing_init = 1;
        ul_re2_log("D3D9: frame limiter armed @ %.2f fps", g_target_fps);
        return;
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
}

/* ---- BMP dump (verification path) -----------------------------------------
 * GetRenderTargetData into a SYSTEMMEM surface, write a 24-bit bottom-up BMP
 * (same on-disk format the D3D11 dumper emits). */
static IDirect3DSurface9* g_dumpsys = NULL;
static int                g_dumpsys_w = 0, g_dumpsys_h = 0;
static D3DFORMAT          g_dumpsys_fmt = D3DFMT_UNKNOWN;

static void dump_backbuffer_bmp9(IDirect3DDevice9* dev, const char* path) {
    if (!dev) return;
    IDirect3DSurface9* bb = NULL;
    HRESULT hgb = IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hgb) || !bb) { ul_re2_log("D3D9 dump: GetBackBuffer 0x%08lX", hgb); return; }
    D3DSURFACE_DESC d;
    if (FAILED(IDirect3DSurface9_GetDesc(bb, &d))) { IDirect3DSurface9_Release(bb); return; }
    if (g_dumpsys && (g_dumpsys_w != (int)d.Width || g_dumpsys_h != (int)d.Height || g_dumpsys_fmt != d.Format)) {
        IDirect3DSurface9_Release(g_dumpsys); g_dumpsys = NULL;
    }
    if (!g_dumpsys) {
        HRESULT hc = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height,
                        d.Format, D3DPOOL_SYSTEMMEM, &g_dumpsys, NULL);
        if (FAILED(hc) || !g_dumpsys) { IDirect3DSurface9_Release(bb); return; }
        g_dumpsys_w = (int)d.Width; g_dumpsys_h = (int)d.Height; g_dumpsys_fmt = d.Format;
    }
    HRESULT hr = IDirect3DDevice9_GetRenderTargetData(dev, bb, g_dumpsys);
    IDirect3DSurface9_Release(bb);
    if (FAILED(hr)) { ul_re2_log("D3D9 dump: GetRenderTargetData 0x%08lX", hr); return; }

    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DSurface9_LockRect(g_dumpsys, &lr, NULL, D3DLOCK_READONLY))) return;
    int w = (int)d.Width, h = (int)d.Height;
    int rowsz = w * 3, pad = (4 - (rowsz & 3)) & 3, stride = rowsz + pad;
    int imgsz = stride * h;
    unsigned char fh[14] = {0}, ih[40] = {0};
    unsigned filesz = 14 + 40 + imgsz;
    fh[0]='B'; fh[1]='M'; *(unsigned*)(fh+2)=filesz; *(unsigned*)(fh+10)=54;
    *(unsigned*)(ih+0)=40; *(int*)(ih+4)=w; *(int*)(ih+8)=h;
    *(unsigned short*)(ih+12)=1; *(unsigned short*)(ih+14)=24; *(unsigned*)(ih+20)=imgsz;
    int bytespp = (d.Format == D3DFMT_R5G6B5 || d.Format == D3DFMT_X1R5G5B5 ||
                   d.Format == D3DFMT_A1R5G5B5) ? 2 : 4;
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(fh,1,14,f); fwrite(ih,1,40,f);
        unsigned char* row = (unsigned char*)malloc(stride);
        for (int y = h-1; y >= 0; --y) {
            unsigned char* src = (unsigned char*)lr.pBits + y * lr.Pitch;
            memset(row, 0, stride);
            if (bytespp == 4) {
                for (int x = 0; x < w; ++x) {
                    row[x*3+0] = src[x*4+0]; row[x*3+1] = src[x*4+1]; row[x*3+2] = src[x*4+2];
                }
            } else {
                for (int x = 0; x < w; ++x) {
                    unsigned short px = ((unsigned short*)src)[x];
                    unsigned r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
                    row[x*3+0] = (unsigned char)(b << 3);
                    row[x*3+1] = (unsigned char)(g << 2);
                    row[x*3+2] = (unsigned char)(r << 3);
                }
            }
            fwrite(row, 1, stride, f);
        }
        free(row); fclose(f);
        ul_re2_log("D3D9: dumped %dx%d fmt=%u -> %s", w, h, d.Format, path);
    }
    IDirect3DSurface9_UnlockRect(g_dumpsys);
}

/* ---- output-window resize for upscale/force-windowed ----------------------- */
static WNDPROC g_orig_wndproc = NULL;
static LRESULT CALLBACK re2_output_wndproc9(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if (g_orig_wndproc) return CallWindowProcA(g_orig_wndproc, h, msg, wp, lp);
    return DefWindowProcA(h, msg, wp, lp);
}
static void resize_output_window9(HWND w) {
    if (!w || g_upscaled) return;
    SetClassLongPtrA(w, GCLP_HBRBACKGROUND, (LONG_PTR)GetStockObject(BLACK_BRUSH));
    if ((WNDPROC)(LONG_PTR)GetWindowLongPtrA(w, GWLP_WNDPROC) != re2_output_wndproc9)
        g_orig_wndproc = (WNDPROC)(LONG_PTR)SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)re2_output_wndproc9);
    RECT wr = { 0, 0, g_up_w, g_up_h };
    SetWindowLongA(w, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    AdjustWindowRect(&wr, WS_POPUP, FALSE);
    SetWindowPos(w, HWND_TOPMOST, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(w);
    InvalidateRect(w, NULL, TRUE);
    g_upscaled = 1;
    ul_re2_log("D3D9: output window -> %dx%d%s", g_up_w, g_up_h, g_rotate ? " (rotated)" : "");
}

/* ---- dump scheduling -------------------------------------------------------- */
static const char* g_dump_dir = NULL;
static int         g_dump_dir_read = 0;
static char        g_dump_dir_copy[512];
static int         g_present_count = 0;
static int dump_due(int n) {
    static int every = -1;
    if (every < 0) { const char* e = ul_re2_env("ULTRA_DUMP_EVERY"); every = (e && *e) ? atoi(e) : 0; }
    if (every > 0) return (n % every) == 0;
    return (n==120||n==300||n==600||n==900||n==1200||n==1500||n==1800||n==2100);
}

/* ---- frame heartbeat --------------------------------------------------------
 *
 *  THE ONE THING THE LOG COULD NOT SAY. throttle() logs exactly once, on its
 *  first call, so "frame limiter armed @ 60.00 fps" proves frame 1 happened and
 *  says nothing whatsoever about frame 2. "Stopped dead after one Present" and
 *  "rendering happily into a window nobody can see" produced the identical
 *  log, and they have completely different fixes.
 *
 *  So: log the first few frames individually (a game that stops at 1, 2 or 3 is
 *  a different animal from one that stops at 600), then one line every
 *  ULTRA_RE2_FRAMELOG frames -- default 300, i.e. one line per five seconds at
 *  60 fps, which is cheap enough to leave on permanently.
 * --------------------------------------------------------------------------- */
static void present_heartbeat9(int n) {
    static int          every = -1;
    static LARGE_INTEGER hb_freq, hb_t0, hb_prev;
    static int          hb_init = 0;
    LARGE_INTEGER now;
    int due;

    if (every < 0) {
        const char* e = ul_re2_env("ULTRA_RE2_FRAMELOG");
        every = (e && *e) ? atoi(e) : 300;
        if (every < 0) every = 0;
    }
    if (!every) return;

    if (!hb_init) {
        QueryPerformanceFrequency(&hb_freq);
        QueryPerformanceCounter(&hb_t0);
        hb_prev = hb_t0;
        hb_init = 1;
    }
    due = (n <= 5) || (n == 10) || (n == 30) || (n == 60) || ((n % every) == 0);
    if (!due) return;

    QueryPerformanceCounter(&now);
    {
        double total = (double)(now.QuadPart - hb_t0.QuadPart) / (double)hb_freq.QuadPart;
        double since = (double)(now.QuadPart - hb_prev.QuadPart) / (double)hb_freq.QuadPart;
        HWND w = g_devwnd;
        RECT r; memset(&r, 0, sizeof r);
        if (w && IsWindow(w)) GetWindowRect(w, &r);
        hb_prev = now;
        ul_re2_log("D3D9: PRESENT #%d  t=%.2fs  dt=%.3fs  hwnd=%p "
                   "visible=%d iconic=%d foreground=%d rect=(%ld,%ld)-(%ld,%ld) bb=%dx%d",
                   n, total, since, (void*)w,
                   (w && IsWindowVisible(w)) ? 1 : 0,
                   (w && IsIconic(w)) ? 1 : 0,
                   (w && GetForegroundWindow() == w) ? 1 : 0,
                   r.left, r.top, r.right, r.bottom, g_bb_w, g_bb_h);
    }
}

static HRESULT __stdcall Hook_Present9(void* self, const RECT* src, const RECT* dst,
                                       HWND hwndOverride, const RGNDATA* dirty) {
    throttle();
    IDirect3DDevice9* dev = (IDirect3DDevice9*)self;
    /* COPY the env value, do not cache the pointer: ul_re2_env hands back one
     * of 16 ROTATING static slots (ringedge2_hook_main.c:60-66), so a cached
     * pointer is overwritten by the 16th unrelated env read and the dump path
     * silently becomes someone else's string. */
    if (!g_dump_dir_read) {
        const char* e = ul_re2_env("ULTRA_ROT_DUMP");
        g_dump_dir_copy[0] = 0;
        if (e && *e) {
            strncpy(g_dump_dir_copy, e, sizeof g_dump_dir_copy - 1);
            g_dump_dir_copy[sizeof g_dump_dir_copy - 1] = 0;
        }
        g_dump_dir = g_dump_dir_copy;
        g_dump_dir_read = 1;
    }
    const int want_dump = (g_dump_dir && *g_dump_dir);

    if ((g_up_w > 0) && !g_upscaled)
        resize_output_window9(g_devwnd ? g_devwnd : hwndOverride);

    ++g_present_count;
    present_heartbeat9(g_present_count);
    if (want_dump && dump_due(g_present_count)) {
        char p[512];
        snprintf(p, sizeof p, "%s/dump_raw_%04d.bmp", g_dump_dir, g_present_count);
        dump_backbuffer_bmp9(dev, p);
    }

    if (g_up_w > 0)                    /* stretch backbuffer -> enlarged window */
        return Real_Present9(self, NULL, NULL, hwndOverride, NULL);
    return Real_Present9(self, src, dst, hwndOverride, dirty);
}

static HRESULT __stdcall Hook_Reset9(void* self, D3DPRESENT_PARAMETERS* pp) {
    if (pp) {
        if (pp->BackBufferWidth > 0 && pp->BackBufferHeight > 0) {
            g_bb_w = (int)pp->BackBufferWidth; g_bb_h = (int)pp->BackBufferHeight;
        }
        if ((g_up_w > 0 || force_windowed()) && !pp->Windowed) {
            pp->Windowed = TRUE;
            pp->BackBufferFormat = D3DFMT_X8R8G8B8;
            pp->FullScreen_RefreshRateInHz = 0;
            pp->PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
        }
    }
    HRESULT hr = Real_Reset9(self, pp);
    if (SUCCEEDED(hr) && (g_up_w > 0 || g_rotate)) g_upscaled = 0;
    ul_re2_log("D3D9: Reset -> 0x%08lX (bb=%dx%d)", hr, g_bb_w, g_bb_h);
    return hr;
}

static void patch_vtable_entry(void* obj, int index, void* hook, void** out_real) {
    if (!obj) return;
    void** vtbl = *(void***)obj;
    DWORD old;
    if (VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        if (out_real && !*out_real) *out_real = (void*)vtbl[index];
        vtbl[index] = hook;
        VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
    }
}

static HRESULT __stdcall Hook_CreateDevice9(void* self, UINT adapter, D3DDEVTYPE devtype,
                                            HWND hwnd, DWORD flags,
                                            D3DPRESENT_PARAMETERS* pp, void** ppdev) {
    read_target_res();
    if (pp) {
        g_devwnd = pp->hDeviceWindow ? pp->hDeviceWindow : hwnd;
        if (pp->BackBufferWidth > 0 && pp->BackBufferHeight > 0) {
            g_bb_w = (int)pp->BackBufferWidth; g_bb_h = (int)pp->BackBufferHeight;
        }
        ul_re2_log("D3D9: CreateDevice bb=%ux%u fmt=%u hwnd=%08X windowed=%u",
             pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat,
             (unsigned)(uintptr_t)pp->hDeviceWindow, pp->Windowed);
        if ((g_up_w > 0 || force_windowed()) && !pp->Windowed) {
            pp->Windowed = TRUE;
            pp->BackBufferFormat = D3DFMT_X8R8G8B8;
            pp->FullScreen_RefreshRateInHz = 0;
            pp->PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
            ul_re2_log("D3D9: forced WINDOWED (backbuffer stays %ux%u)",
                 pp->BackBufferWidth, pp->BackBufferHeight);
        }
    } else if (!g_devwnd) {
        g_devwnd = hwnd;
    }
    HRESULT hr = Real_CreateDevice9(self, adapter, devtype, hwnd, flags, pp, ppdev);
    if (hr >= 0 && ppdev && *ppdev && !g_present_hooked) {
        patch_vtable_entry(*ppdev, IDIRECT3DDEVICE9_PRESENT, (void*)Hook_Present9, (void**)&Real_Present9);
        patch_vtable_entry(*ppdev, IDIRECT3DDEVICE9_RESET,   (void*)Hook_Reset9,   (void**)&Real_Reset9);
        g_present_hooked = 1;
        ul_re2_log("D3D9: device created -> Present+Reset hooked");
        { HWND sp = FindWindowA("UltraLoaderSplash", NULL);
          if (sp) { ShowWindow(sp, SW_HIDE); ul_re2_log("D3D9: dismissed loader splash"); } }
        if (g_devwnd) {
            if (force_windowed() && g_up_w == 0) {
                int dw = GetSystemMetrics(SM_CXSCREEN);
                int dh = GetSystemMetrics(SM_CYSCREEN);
                SetWindowLongA(g_devwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                SetWindowPos(g_devwnd, HWND_TOPMOST, 0, 0, dw, dh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                SetWindowPos(g_devwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                ul_re2_log("D3D9: force-windowed borderless %dx%d", dw, dh);
            }
            SetForegroundWindow(g_devwnd);
        }
    }
    return hr;
}

/* Patch IDirect3D9::CreateDevice (slot 16) on the returned IDirect3D9. */
static void ul_re2_d3d9_install_throttle(void* d3d9) {
    if (!d3d9) return;
    patch_vtable_entry(d3d9, IDIRECT3D9_CREATEDEVICE, (void*)Hook_CreateDevice9, (void**)&Real_CreateDevice9);
}

/* Real Direct3DCreate9 hook. Called via the IAT patch / GetProcAddress
 * interception in jvs_serial_ringedge2.c. Resolves the real export, calls it,
 * then arms the throttle/dump on the returned IDirect3D9. */
typedef IDirect3D9* (__stdcall *pDirect3DCreate9)(UINT);
static pDirect3DCreate9 Real_Direct3DCreate9 = NULL;
void* __stdcall ul_re2_Hook_Direct3DCreate9(UINT ver) {
    if (!Real_Direct3DCreate9) {
        HMODULE m = GetModuleHandleA("d3d9.dll");
        if (!m) m = LoadLibraryA("d3d9.dll");
        if (m) Real_Direct3DCreate9 = (pDirect3DCreate9)(void*)ultra_raw_export(m, "Direct3DCreate9");
    }
    IDirect3D9* r = Real_Direct3DCreate9 ? Real_Direct3DCreate9(ver) : NULL;
    ul_re2_log("D3D9: Direct3DCreate9(0x%X) -> %p %s", ver, (void*)r, r ? "OK" : "NULL(D3D9 init failed)");
    if (r) ul_re2_d3d9_install_throttle(r);
    return r;
}

/* Exported window getter -- the current D3D9 device window (NULL until the
 * game's first CreateDevice), read by ultra_omni_bridge_install() via
 * ringedge2_hook_main.c's combined getter (this board is mixed D3D9/D3D11,
 * see d3d11_present_ringedge2.c's ul_re2_d3d11_window() for the other half). */
HWND ul_re2_d3d9_window(void) {
    return g_devwnd;
}

/* Running Present count, read by the watchdog heartbeat
 * (watchdog_ringedge2.c). Zero forever on a D3D11 title. */
unsigned long ul_re2_d3d9_present_count(void) {
    return (unsigned long)g_present_count;
}

/* install entry (always armed -- this DLL is dedicated to RingEdge 2, a mixed
 * D3D9/D3D11 board, so BOTH present hooks are always compiled in and armed). */
void ul_re2_d3d9_install(void) {
    /* d3d9.dll is a static import for a non-packed D3D9 title; if the process
     * already has it loaded, patch its Direct3DCreate9 export directly too
     * (belt-and-suspenders alongside the IAT patch in jvs_serial_ringedge2.c). */
    ul_re2_iat_patch_all("d3d9.dll", "Direct3DCreate9", (void*)ul_re2_Hook_Direct3DCreate9);
}

#endif /* _WIN32 */
