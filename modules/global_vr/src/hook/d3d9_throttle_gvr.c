/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  d3d9_throttle_gvr.c -- frame limiter + force-windowed present hook for the
 *                         Global VR board (Direct3D 9)
 * =============================================================================
 *
 *  Global VR PC-arcade titles render with D3D8 or D3D9 depending on the title
 *  (per the board's public documentation); this covers the D3D9 path. Like
 *  every cabinet in this project they pace their game loop off Present (the
 *  cabinet's vsync); windowed on a PC that runs unthrottled would fast-forward
 *  the game. We IAT-hook d3d9.dll!Direct3DCreate9 -> IDirect3D9::CreateDevice ->
 *  IDirect3DDevice9::Present and:
 *    - throttle Present to a target fps (default 60, ULTRA_FPS_CAP),
 *    - force the device windowed when requested (needed for the injected/
 *      automated launch path, and a prerequisite for any future upscale),
 *    - optionally stretch the backbuffer to a larger window (ULTRA_RES /
 *      ULTRA_BORDERLESS), all DEFAULT-OFF except force-windowed, which
 *      global_vr_main.c already arms by default via ULTRA_SEGA_WINDOWED=1.
 *
 *  Rotation / sharpen / frame-dump post-processing (present in the Type X2
 *  reference file) is intentionally NOT ported here -- no Global VR title's
 *  screen orientation or verification need is known yet (framework-only, no
 *  dump), so that machinery would be speculative complexity with nothing to
 *  validate it against. Add it if/when a real dump shows a need.
 *
 *  INDEPENDENT COPY: the present-hook shape here was read FOR REFERENCE ONLY
 *  from native_x86/src/hook/d3d9_throttle.c and reimplemented from scratch
 *  against this board's own global_vr_hook.h -- no object code from that file
 *  (or any other board's) is linked here.
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
#include <string.h>

#include "global_vr_hook.h"

/* ultra_raw_export: now lives in iat_patch_gvr.c as ultra_gvr_raw_export().
 * Thin alias kept so the rest of this file compiles unchanged. */
#define ultra_raw_export ultra_gvr_raw_export

#define tlog ultra_gvr_log

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

/* ---- upscale state ---------------------------------------------------------
 * Simple "stretch the whole backbuffer to a bigger window" path -- no rotate,
 * no sharpen, no capture. */
static HWND g_devwnd   = NULL;
static int  g_up_w = 0, g_up_h = 0;     /* target client size (0 = no upscale)  */
static int  g_upscaled = 0;

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
    /* Borderless window with no explicit target size -> fill the monitor. */
    if (g_up_w == 0) {
        const char* bl = getenv("ULTRA_BORDERLESS");
        if (bl && *bl == '1') {
            g_up_w = GetSystemMetrics(SM_CXSCREEN);
            g_up_h = GetSystemMetrics(SM_CYSCREEN);
        }
    }
}

/* Force-windowed control. A fullscreen-EXCLUSIVE D3D9 device is LOST whenever
 * it loses foreground focus; under an injected/automated launch (where the
 * hook itself may (re)foreground the game) that can stall the game's loop on
 * a device-lost it never recovers from. Running windowed sidesteps that.
 * global_vr_main.c already defaults ULTRA_SEGA_WINDOWED=1 for this board, so
 * this is ON by default; ULTRA_GVR_WINDOWED is a board-specific override, and
 * ULTRA_WINDOWED/ULTRA_BORDERLESS are the universal loader-wide knobs. */
static int force_windowed(void) {
    static int v = -1;
    if (v < 0) {
        const char* gw = getenv("ULTRA_GVR_WINDOWED");
        if (gw && *gw) { v = (gw[0] == '1'); }
        else {
            const char* uw = getenv("ULTRA_WINDOWED");
            const char* bl = getenv("ULTRA_BORDERLESS");
            const char* sw = getenv("ULTRA_SEGA_WINDOWED");   /* shared frontend knob */
            v = ((uw && uw[0]=='1') || (bl && bl[0]=='1') || (sw && sw[0]=='1'));
        }
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
        const char* e = getenv("ULTRA_FPS_CAP");
        if (e && *e) { double v = atof(e); if (v >= 1.0 && v <= 1000.0) g_target_fps = v; }
        timeBeginPeriod(1);
        g_timing_init = 1;
        tlog("[gvr-d3d9] frame limiter armed @ %.2f fps", g_target_fps);
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

/* ---- output-window resize for upscale -------------------------------------- */
static WNDPROC g_orig_wndproc = NULL;
static LRESULT CALLBACK ultra_output_wndproc9(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if (g_orig_wndproc) return CallWindowProcA(g_orig_wndproc, h, msg, wp, lp);
    return DefWindowProcA(h, msg, wp, lp);
}
static void resize_output_window9(HWND w) {
    if (!w || g_upscaled) return;
    SetClassLongPtrA(w, GCLP_HBRBACKGROUND, (LONG_PTR)GetStockObject(BLACK_BRUSH));
    if ((WNDPROC)(LONG_PTR)GetWindowLongPtrA(w, GWLP_WNDPROC) != ultra_output_wndproc9)
        g_orig_wndproc = (WNDPROC)(LONG_PTR)SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)ultra_output_wndproc9);
    RECT wr = { 0, 0, g_up_w, g_up_h };
    SetWindowLongA(w, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    AdjustWindowRect(&wr, WS_POPUP, FALSE);
    SetWindowPos(w, HWND_TOPMOST, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(w);
    InvalidateRect(w, NULL, TRUE);
    g_upscaled = 1;
    tlog("[gvr-d3d9] output window -> %dx%d (black bg, erase swallowed, foreground)", g_up_w, g_up_h);
}

static HRESULT __stdcall Hook_Present9(void* self, const RECT* src, const RECT* dst,
                                       HWND hwndOverride, const RGNDATA* dirty) {
    throttle();
    if (g_up_w > 0 && !g_upscaled)
        resize_output_window9(g_devwnd ? g_devwnd : hwndOverride);
    if (g_up_w > 0)        /* stretch backbuffer -> enlarged window */
        return Real_Present9(self, NULL, NULL, hwndOverride, NULL);
    return Real_Present9(self, src, dst, hwndOverride, dirty);
}

static HRESULT __stdcall Hook_Reset9(void* self, D3DPRESENT_PARAMETERS* pp) {
    if (pp) {
        if ((g_up_w > 0 || force_windowed()) && !pp->Windowed) {
            pp->Windowed = TRUE;
            pp->BackBufferFormat = D3DFMT_X8R8G8B8;
            pp->FullScreen_RefreshRateInHz = 0;
            pp->PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
        }
    }
    HRESULT hr = Real_Reset9(self, pp);
    /* A Reset can restore the window's original (fullscreen/native) size/style,
     * so the enlarged-window setup must be re-applied on the next Present. */
    if (SUCCEEDED(hr) && g_up_w > 0) g_upscaled = 0;
    tlog("[gvr-d3d9] Reset -> 0x%08lX", hr);
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
        tlog("[gvr-d3d9] CreateDevice bb=%ux%u fmt=%u hwnd=%08X windowed=%u",
             pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat,
             (unsigned)(uintptr_t)pp->hDeviceWindow, pp->Windowed);
        if ((g_up_w > 0 || force_windowed()) && !pp->Windowed) {
            pp->Windowed = TRUE;
            pp->BackBufferFormat = D3DFMT_X8R8G8B8;
            pp->FullScreen_RefreshRateInHz = 0;
            pp->PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
            tlog("[gvr-d3d9] forced WINDOWED (bb stays %ux%u)", pp->BackBufferWidth, pp->BackBufferHeight);
        }
    } else if (!g_devwnd) {
        g_devwnd = hwnd;
    }
    HRESULT hr = Real_CreateDevice9(self, adapter, devtype, hwnd, flags, pp, ppdev);
    if (hr >= 0 && ppdev && *ppdev && !g_present_hooked) {
        patch_vtable_entry(*ppdev, IDIRECT3DDEVICE9_PRESENT, (void*)Hook_Present9, (void**)&Real_Present9);
        patch_vtable_entry(*ppdev, IDIRECT3DDEVICE9_RESET,   (void*)Hook_Reset9,   (void**)&Real_Reset9);
        g_present_hooked = 1;
        tlog("[gvr-d3d9] device created -> Present+Reset hooked%s", (g_up_w>0)?" (+upscale)":"");
        /* Ultra Loader's splash is always first on screen; dismiss it now that
         * the game has its own device up (per the project's splash-architecture
         * rule -- never hack the game itself, just stop occluding its window). */
        HWND sp = FindWindowA("UltraLoaderSplash", NULL);
        if (sp) { ShowWindow(sp, SW_HIDE); tlog("[gvr-d3d9] dismissed loader splash"); }
        if (g_devwnd) SetForegroundWindow(g_devwnd);
    }
    return hr;
}

/* Patch IDirect3D9::CreateDevice (slot 16) on the returned IDirect3D9. */
static void install_throttle(void* d3d9) {
    if (!d3d9) return;
    patch_vtable_entry(d3d9, IDIRECT3D9_CREATEDEVICE, (void*)Hook_CreateDevice9, (void**)&Real_CreateDevice9);
}

/* Direct3DCreate9 hook: resolves the real export, calls it, then arms the
 * throttle on the returned IDirect3D9. */
typedef IDirect3D9* (__stdcall *pDirect3DCreate9)(UINT);
static pDirect3DCreate9 Real_Direct3DCreate9 = NULL;
static void* __stdcall Hook_Direct3DCreate9(UINT ver) {
    if (!Real_Direct3DCreate9) {
        HMODULE m = GetModuleHandleA("d3d9.dll");
        if (!m) m = LoadLibraryA("d3d9.dll");
        if (m) Real_Direct3DCreate9 = (pDirect3DCreate9)(void*)ultra_raw_export(m, "Direct3DCreate9");
    }
    IDirect3D9* r = Real_Direct3DCreate9 ? Real_Direct3DCreate9(ver) : NULL;
    tlog("[gvr-d3d9] Direct3DCreate9(0x%X) -> %p %s", ver, (void*)r, r ? "OK" : "NULL(D3D9 init failed)");
    if (r) install_throttle(r);
    return r;
}

void ultra_gvr_d3d9_install(void) {
    ultra_gvr_iat_patch_all("d3d9.dll", "Direct3DCreate9", (void*)Hook_Direct3DCreate9);
}

void ultra_gvr_d3d9_teardown(void) {
    if (g_timing_init) { timeEndPeriod(1); g_timing_init = 0; }
}

/* Current device/render window (set from CreateDevice's pp->hDeviceWindow /
 * hwnd), or NULL before the game has created its D3D9 device yet. Used by
 * omni_bridge_gvr.c so the Omni-Bridge cursor/click synthesis targets the
 * right window without reaching into this file's statics directly. */
HWND ultra_gvr_d3d9_window(void) { return g_devwnd; }

#endif /* _WIN32 */
