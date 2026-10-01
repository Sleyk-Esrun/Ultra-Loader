/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  d3d9_throttle_pcplus.c -- andamiro_pcplus module's OWN D3D9 present hook:
 *                            frame limiter + forced-windowed + upscale + dump.
 * =============================================================================
 *
 *  Pump It Up on Andamiro PC+ hardware renders with Direct3D 9. Like every
 *  arcade board it paces its game loop off Present (the cabinet's 60Hz vsync);
 *  a fullscreen-exclusive device on a PC can also go LOST the instant an
 *  injected/automated launch changes focus, so we IAT-hook
 *  d3d9.dll!Direct3DCreate9 -> IDirect3D9::CreateDevice ->
 *  IDirect3DDevice9::Present and:
 *    - throttle Present to a target fps (default 60, ULTRA_FPS_CAP),
 *    - force the device windowed (ULTRA_SEGA_WINDOWED / ULTRA_WINDOWED /
 *      ULTRA_BORDERLESS -- andamiro_pcplus_main.c sets ULTRA_SEGA_WINDOWED=1
 *      by default) so an injected/automated launch survives a focus change
 *      without the device going D3DERR_DEVICELOST,
 *    - optionally stretch the backbuffer to a bigger output window
 *      (ULTRA_RES / ULTRA_RES_W / ULTRA_RES_H, or auto-fill the monitor when
 *      ULTRA_BORDERLESS=1) by simply resizing the window and letting Present's
 *      own NULL-rect stretch-blit do the scaling -- no extra render target /
 *      shader resources needed since this board has no rotate requirement,
 *    - optionally dump the back buffer to a 24-bit BMP (ULTRA_ROT_DUMP=dir)
 *      for offline verification of a boot frame this board cannot be
 *      screenshotted for yet (no real game dump -- see UNTESTED.txt).
 *
 *  This is the andamiro_pcplus module's OWN independent copy of the D3D9
 *  present-hook pattern (native_x86/src/hook/d3d9_throttle.c was read only as
 *  reference). It intentionally does NOT port that file's rotate/sharpen
 *  post-process pipeline -- a Pump It Up cabinet monitor is native landscape
 *  and nothing in this board's brief calls for rotation, so keeping this file
 *  to present-hook + frame-limiter + forced-windowed + upscale-by-resize +
 *  diagnostic-dump keeps it minimal, correct, and self-contained.
 *
 *  Clean-room: Ultra Loader's own code over the public COM / D3D9 ABI.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#define DIRECT3D_VERSION 0x0900
#define COBJMACROS
#include <d3d9.h>
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

/* shared logger (gated by ULTRA_JVS_LOG); defined in hook_dllmain_pcplus.c. */
void ultra_pcplus_log(const char* fmt, ...);
#define tlog ultra_pcplus_log

static HWND g_devwnd    = NULL;
static int  g_bb_w = 640, g_bb_h = 480;  /* game backbuffer size (diagnostic only) */
static int  g_up_w = 0,  g_up_h = 0;     /* target output window size (0 = none)  */
static int  g_upscaled   = 0;

static void read_target_res(void) {
    const char* r = getenv("ULTRA_RES");
    if (r && *r) {
        if      (!strcmp(r, "1080p")) { g_up_w = 1920; g_up_h = 1080; }
        else if (!strcmp(r, "1440p")) { g_up_w = 2560; g_up_h = 1440; }
        else if (!strcmp(r, "4k") || !strcmp(r, "2160p")) { g_up_w = 3840; g_up_h = 2160; }
        else { int w=0,h=0; if (sscanf(r, "%dx%d", &w, &h) == 2 && w>0 && h>0) { g_up_w=w; g_up_h=h; } }
    }
    const char* w = getenv("ULTRA_RES_W"); const char* h = getenv("ULTRA_RES_H");
    if (w && *w && h && *h) { int wi=atoi(w), hi=atoi(h); if (wi>0&&hi>0){ g_up_w=wi; g_up_h=hi; } }
    /* Borderless window: no explicit target size -> auto-set to monitor resolution
     * so the hook upscales to fill the monitor with WS_POPUP (no chrome). */
    if (g_up_w == 0) {
        const char* bl = getenv("ULTRA_BORDERLESS");
        if (bl && *bl == '1') {
            g_up_w = GetSystemMetrics(SM_CXSCREEN);
            g_up_h = GetSystemMetrics(SM_CYSCREEN);
        }
    }
}

/* Force-windowed control. A fullscreen-EXCLUSIVE D3D9 device is LOST whenever it
 * loses foreground focus; under an injected/suspended-launch flow that triggers
 * a device-lost the game's loop may stall on. Running windowed makes the device
 * focus-independent. Default: force windowed when andamiro_pcplus_main.c's
 * default ULTRA_SEGA_WINDOWED=1 is set, or when the universal ULTRA_WINDOWED /
 * ULTRA_BORDERLESS knobs are on; never when explicitly turned off. */
static int force_windowed(void) {
    static int v = -1;
    if (v < 0) {
        const char* sw = getenv("ULTRA_SEGA_WINDOWED");
        const char* uw = getenv("ULTRA_WINDOWED");
        const char* bl = getenv("ULTRA_BORDERLESS");
        if (sw && *sw) v = (sw[0] == '1');
        else if ((uw && uw[0] == '1') || (bl && bl[0] == '1')) v = 1;
        else v = 0;
    }
    return v;
}

/* ---- frame limiter (QPC, API-agnostic) ------------------------------------ */
static double        g_target_fps = 60.0;
static LARGE_INTEGER g_freq, g_last;
static int           g_timing_init = 0;
static int           g_time_period_set = 0;

static void throttle(void) {
    if (!g_timing_init) {
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_last);
        const char* e = getenv("ULTRA_FPS_CAP");
        if (e && *e) { double v = atof(e); if (v >= 1.0 && v <= 1000.0) g_target_fps = v; }
        timeBeginPeriod(1);
        g_time_period_set = 1;
        g_timing_init = 1;
        tlog("D3D9: frame limiter armed @ %.2f fps\n", g_target_fps);
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

    static LARGE_INTEGER mark; static int frames = 0;
    if (frames == 0) mark = now;
    if (++frames >= 180) {
        double secs = (double)(now.QuadPart - mark.QuadPart) / (double)g_freq.QuadPart;
        if (secs > 0.0) tlog("D3D9: measured %.1f fps (cap %.0f)\n", frames / secs, g_target_fps);
        frames = 0;
    }
}

void ultra_pcplus_d3d9_teardown(void) {
    if (g_time_period_set) { timeEndPeriod(1); g_time_period_set = 0; }
}

/* Current D3D9 device window (NULL until CreateDevice has run). Used as the
 * ultra_bridge_window_fn accessor for Ultra Omni-Bridge (omni_bridge.c) --
 * see hook_dllmain_pcplus.c's ultra_omni_bridge_install() call. */
HWND ultra_pcplus_device_window(void) {
    return g_devwnd;
}

/* ---- output-window resize for upscale --------------------------------------
 * Present with NULL src/dst rects stretches the backbuffer to the current
 * client rect automatically, so upscaling only needs the window resized once
 * (no extra textures / state blocks / shader passes -- this board has no
 * rotate requirement, unlike the reference file's fuller pipeline). */
static WNDPROC g_orig_wndproc = NULL;
static LRESULT CALLBACK ultra_pcplus_output_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if (g_orig_wndproc) return CallWindowProcA(g_orig_wndproc, h, msg, wp, lp);
    return DefWindowProcA(h, msg, wp, lp);
}
static void resize_output_window(HWND w) {
    if (!w || g_upscaled) return;
    SetClassLongPtrA(w, GCLP_HBRBACKGROUND, (LONG_PTR)GetStockObject(BLACK_BRUSH));
    if ((WNDPROC)(LONG_PTR)GetWindowLongPtrA(w, GWLP_WNDPROC) != ultra_pcplus_output_wndproc)
        g_orig_wndproc = (WNDPROC)(LONG_PTR)SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)ultra_pcplus_output_wndproc);
    RECT wr = { 0, 0, g_up_w, g_up_h };
    SetWindowLongA(w, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    AdjustWindowRect(&wr, WS_POPUP, FALSE);
    SetWindowPos(w, HWND_TOPMOST, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(w);
    InvalidateRect(w, NULL, TRUE);
    g_upscaled = 1;
    tlog("D3D9: output window -> %dx%d (black bg, erase swallowed, foreground)\n", g_up_w, g_up_h);
}

/* ---- BMP dump (the verification path) --------------------------------------
 * Read the live back buffer into a SYSTEMMEM surface via GetRenderTargetData and
 * write a 24-bit bottom-up BMP. Works in windowed AND fullscreen-exclusive. The
 * sysmem surface is cached and reused across frames to avoid per-dump alloc. */
static IDirect3DSurface9* g_dumpsys = NULL;
static int                g_dumpsys_w = 0, g_dumpsys_h = 0;
static D3DFORMAT          g_dumpsys_fmt = D3DFMT_UNKNOWN;

static void dump_backbuffer_bmp9(IDirect3DDevice9* dev, const char* path) {
    if (!dev) return;
    IDirect3DSurface9* bb = NULL;
    HRESULT hgb = IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hgb) || !bb) { tlog("D3D9 dump: GetBackBuffer 0x%08lX\n", hgb); return; }
    D3DSURFACE_DESC d;
    if (FAILED(IDirect3DSurface9_GetDesc(bb, &d))) { tlog("D3D9 dump: GetDesc fail\n"); IDirect3DSurface9_Release(bb); return; }
    if (g_dumpsys && (g_dumpsys_w != (int)d.Width || g_dumpsys_h != (int)d.Height || g_dumpsys_fmt != d.Format)) {
        IDirect3DSurface9_Release(g_dumpsys); g_dumpsys = NULL;
    }
    if (!g_dumpsys) {
        HRESULT hc = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height,
                        d.Format, D3DPOOL_SYSTEMMEM, &g_dumpsys, NULL);
        if (FAILED(hc) || !g_dumpsys) { tlog("D3D9 dump: CreateOffscreenPlainSurface 0x%08lX\n", hc); IDirect3DSurface9_Release(bb); return; }
        g_dumpsys_w = (int)d.Width; g_dumpsys_h = (int)d.Height; g_dumpsys_fmt = d.Format;
    }
    HRESULT hr = IDirect3DDevice9_GetRenderTargetData(dev, bb, g_dumpsys);
    IDirect3DSurface9_Release(bb);
    if (FAILED(hr)) { tlog("D3D9 dump: GetRenderTargetData 0x%08lX\n", hr); return; }

    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DSurface9_LockRect(g_dumpsys, &lr, NULL, D3DLOCK_READONLY))) { tlog("D3D9 dump: LockRect fail\n"); return; }
    int w = (int)d.Width, h = (int)d.Height;
    int rowsz = w * 3, pad = (4 - (rowsz & 3)) & 3, stride = rowsz + pad;
    int imgsz = stride * h;
    unsigned char fh[14] = {0}, ih[40] = {0};
    unsigned filesz = 14 + 40 + imgsz;
    fh[0]='B'; fh[1]='M'; *(unsigned*)(fh+2)=filesz; *(unsigned*)(fh+10)=54;
    *(unsigned*)(ih+0)=40; *(int*)(ih+4)=w; *(int*)(ih+8)=h;   /* +h = bottom-up */
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
                for (int x = 0; x < w; ++x) {        /* BGRA -> BGR */
                    row[x*3+0] = src[x*4+0]; row[x*3+1] = src[x*4+1]; row[x*3+2] = src[x*4+2];
                }
            } else {                                  /* R5G6B5 -> BGR */
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
        tlog("D3D9: dumped %dx%d fmt=%u -> %s\n", w, h, d.Format, path);
    }
    IDirect3DSurface9_UnlockRect(g_dumpsys);
}

/* ---- dump scheduling ------------------------------------------------------ */
static const char* g_dump_dir = NULL;
static int         g_dump_dir_read = 0;
static int         g_present_count = 0;
static int dump_due(int n) {
    static int every = -1;
    if (every < 0) { const char* e = getenv("ULTRA_DUMP_EVERY"); every = (e && *e) ? atoi(e) : 0; }
    if (every > 0) return (n % every) == 0;
    return (n==120||n==300||n==600||n==900||n==1200||n==1500||n==1800||n==2100);
}

static HRESULT __stdcall Hook_Present9(void* self, const RECT* src, const RECT* dst,
                                       HWND hwndOverride, const RGNDATA* dirty) {
    throttle();
    IDirect3DDevice9* dev = (IDirect3DDevice9*)self;
    if (!g_dump_dir_read) { g_dump_dir = getenv("ULTRA_ROT_DUMP"); g_dump_dir_read = 1; }
    const int want_dump = (g_dump_dir && *g_dump_dir);

    /* If the device is LOST our dump call would return D3DERR_DEVICELOST; skip
     * it and just throttle + Real_Present, which surfaces the lost device to
     * the game so it can Reset. Only checked when the dump feature is active,
     * so the plain native path stays byte-identical. */
    int dev_ok = 1;
    if (want_dump) dev_ok = (IDirect3DDevice9_TestCooperativeLevel(dev) == D3D_OK);

    if (g_up_w > 0 && !g_upscaled)
        resize_output_window(g_devwnd ? g_devwnd : hwndOverride);

    ++g_present_count;
    if (want_dump && dev_ok && dump_due(g_present_count)) {
        char p[512];
        snprintf(p, sizeof p, "%s/dump_raw_%04d.bmp", g_dump_dir, g_present_count);
        dump_backbuffer_bmp9(dev, p);
    }

    if (g_up_w > 0)          /* stretch backbuffer -> enlarged window */
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
    /* A Reset can restore the window to its original size/style, so re-apply
     * the enlarged-window setup: clear g_upscaled so the next Present re-runs
     * resize_output_window (the re-subclass there is guarded, so g_orig_wndproc
     * is never double-wrapped). */
    if (SUCCEEDED(hr) && g_up_w > 0) g_upscaled = 0;
    tlog("D3D9: Reset -> 0x%08lX (bb=%dx%d)\n", hr, g_bb_w, g_bb_h);
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
        tlog("D3D9: CreateDevice bb=%ux%u fmt=%u swapeffect=%u hwnd=%08X windowed=%u\n",
             pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->SwapEffect,
             (unsigned)(uintptr_t)pp->hDeviceWindow, pp->Windowed);
        /* upscale needs windowed so Present can stretch the backbuffer to the
         * enlarged window; the injected/auto-pilot launch needs windowed so a
         * focus change can't lose the exclusive device. Force windowed +
         * desktop-safe 32-bit format in either case. */
        if ((g_up_w > 0 || force_windowed()) && !pp->Windowed) {
            pp->Windowed = TRUE;
            pp->BackBufferFormat = D3DFMT_X8R8G8B8;
            pp->FullScreen_RefreshRateInHz = 0;
            pp->PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
            tlog("D3D9: forced WINDOWED (%s) (backbuffer stays %ux%u)\n",
                 g_up_w > 0 ? "upscale" : "auto-pilot", pp->BackBufferWidth, pp->BackBufferHeight);
        }
    } else if (!g_devwnd) {
        g_devwnd = hwnd;
    }
    HRESULT hr = Real_CreateDevice9(self, adapter, devtype, hwnd, flags, pp, ppdev);
    if (hr >= 0 && ppdev && *ppdev && !g_present_hooked) {
        patch_vtable_entry(*ppdev, IDIRECT3DDEVICE9_PRESENT, (void*)Hook_Present9, (void**)&Real_Present9);
        patch_vtable_entry(*ppdev, IDIRECT3DDEVICE9_RESET,   (void*)Hook_Reset9,   (void**)&Real_Reset9);
        g_present_hooked = 1;
        tlog("D3D9: device created -> Present+Reset hooked (limiter%s)\n", (g_up_w>0)?" + upscale":"");
        { HWND sp = FindWindowA("UltraLoaderSplash", NULL);
          if (sp) { ShowWindow(sp, SW_HIDE); tlog("D3D9: dismissed loader splash\n"); } }
        if (g_devwnd) {
            if (force_windowed() && g_up_w == 0) {
                int dw = GetSystemMetrics(SM_CXSCREEN);
                int dh = GetSystemMetrics(SM_CYSCREEN);
                SetWindowLongA(g_devwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                SetWindowPos(g_devwnd, HWND_TOPMOST, 0, 0, dw, dh,
                             SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                SetWindowPos(g_devwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE);
                tlog("D3D9: force-windowed borderless %dx%d (no upscale)\n", dw, dh);
            }
            SetForegroundWindow(g_devwnd);
        }
    }
    return hr;
}

/* Patch IDirect3D9::CreateDevice (slot 16) on the returned IDirect3D9. */
void ultra_pcplus_d3d9_install_throttle(void* d3d9) {
    if (!d3d9) return;
    patch_vtable_entry(d3d9, IDIRECT3D9_CREATEDEVICE, (void*)Hook_CreateDevice9, (void**)&Real_CreateDevice9);
}

/* Real Direct3DCreate9 hook. Called via the IAT patch AND the GetProcAddress
 * interception in jvs_serial_hook_pcplus.c. Resolves the real export, calls
 * it, then arms the throttle/dump on the returned IDirect3D9. */
typedef IDirect3D9* (__stdcall *pDirect3DCreate9)(UINT);
static pDirect3DCreate9 Real_Direct3DCreate9 = NULL;
void* __stdcall ultra_pcplus_hook_direct3dcreate9(UINT ver) {
    if (!Real_Direct3DCreate9) {
        HMODULE m = GetModuleHandleA("d3d9.dll");
        if (!m) m = LoadLibraryA("d3d9.dll");
        if (m) Real_Direct3DCreate9 = (pDirect3DCreate9)(void*)ultra_raw_export(m, "Direct3DCreate9");
    }
    IDirect3D9* r = Real_Direct3DCreate9 ? Real_Direct3DCreate9(ver) : NULL;
    tlog("D3D9: Direct3DCreate9(0x%X) -> %p %s\n", ver, (void*)r, r ? "OK" : "NULL(D3D9 init failed)");
    if (r) ultra_pcplus_d3d9_install_throttle(r);
    return r;
}

#endif /* _WIN32 */
