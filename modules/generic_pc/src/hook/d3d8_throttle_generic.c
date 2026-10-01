/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  d3d8_throttle_generic.c -- generic_pc module's OWN D3D8 present hook: frame
 *                             limiter + forced-windowed. One of FOUR sibling
 *                             present-hook files (d3d9/d3d11/gl) this catch-all
 *                             board arms simultaneously, since it does not know
 *                             its game's graphics API in advance.
 * =============================================================================
 *
 *  Some PC-based arcade titles (the oldest Taito-lineage boards especially)
 *  render with Direct3D 8. Like every arcade board they pace their game loop
 *  off Present (the cabinet's 60Hz vsync); a fullscreen-exclusive device on a
 *  PC can also stall under an injected/automated launch, so we IAT-hook
 *  d3d8.dll!Direct3DCreate8 -> IDirect3D8::CreateDevice ->
 *  IDirect3DDevice8::Present and:
 *    - throttle Present to a target fps (default 60, ULTRA_FPS_CAP),
 *    - force the device windowed (ULTRA_WINDOWED / ULTRA_BORDERLESS /
 *      ULTRA_SEGA_WINDOWED) so an injected/automated launch survives a focus
 *      change without the device going lost.
 *
 *  This is the generic_pc module's OWN independent copy of the D3D8
 *  present-hook pattern (native_x86/src/hook/d3d8_throttle.c was read only as
 *  reference). It intentionally does NOT port that file's rotate/upscale/
 *  sharpen/BMP-dump pipeline: D3D8's CopyRects refuses a source that is the
 *  device's current render target, which would need an extra scratch-texture
 *  park step purely for diagnostics; keeping this file to the present-hook +
 *  frame-limiter + forced-windowed core keeps it minimal, correct, and
 *  self-contained -- exactly what a fourth-of-four auto-detect probe needs.
 *
 *  Clean-room: Ultra Loader's own code over the public COM / D3D8 ABI.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#define DIRECT3D_VERSION 0x0800
#define COBJMACROS
#include <d3d8.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* COM vtable indices (verified against the IDirect3D8 / IDirect3DDevice8 ABI). */
#define IDIRECT3D8_CREATEDEVICE    15
#define IDIRECT3DDEVICE8_PRESENT   15

typedef HRESULT (__stdcall *pCreateDevice8)(void* self, UINT adapter, int devtype, HWND hwnd,
                                            DWORD flags, void* pp, void** ppdev);
typedef HRESULT (__stdcall *pPresent8)(void* self, const RECT* src, const RECT* dst,
                                       HWND hwndOverride, const void* dirty);

static pCreateDevice8 Real_CreateDevice8 = NULL;
static pPresent8      Real_Present8      = NULL;
static int            g_present_hooked   = 0;

/* shared logger (gated by ULTRA_JVS_LOG); defined in hook_dllmain_generic.c. */
void ultra_genpc_log(const char* fmt, ...);
#define tlog ultra_genpc_log

static HWND g_devwnd = NULL;

/* Force-windowed control. A fullscreen-EXCLUSIVE device is LOST whenever it
 * loses foreground focus; under an injected/suspended-launch flow that can
 * stall the game's loop. Running windowed makes the device focus-independent.
 * Any of ULTRA_WINDOWED / ULTRA_BORDERLESS / ULTRA_SEGA_WINDOWED = 1 arms it;
 * explicit "0" never does. */
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

/* ---- frame limiter (QPC, API-agnostic -- identical algorithm to the D3D9/
 * D3D11/GL siblings so measured pacing is consistent whichever API fires). --- */
static double        g_target_fps = 60.0;
static LARGE_INTEGER g_freq, g_last;
static int           g_timing_init = 0;
static int           g_timeperiod_set = 0;

static void throttle(void) {
    if (!g_timing_init) {
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_last);
        const char* e = getenv("ULTRA_FPS_CAP");
        if (e && *e) { double v = atof(e); if (v >= 1.0 && v <= 1000.0) g_target_fps = v; }
        timeBeginPeriod(1); g_timeperiod_set = 1;
        g_timing_init = 1;
        tlog("D3D8: frame limiter armed @ %.2f fps\n", g_target_fps);
        return;                                   /* first Present: baseline only */
    }
    const double target = (double)g_freq.QuadPart / g_target_fps;
    LARGE_INTEGER now;
    for (;;) {
        QueryPerformanceCounter(&now);
        const double elapsed = (double)(now.QuadPart - g_last.QuadPart);
        if (elapsed >= target) break;
        const double remain_ms = (target - elapsed) * 1000.0 / (double)g_freq.QuadPart;
        if (remain_ms > 3.0) Sleep((DWORD)(remain_ms - 2.0));       /* coarse (1ms timer) */
        else YieldProcessor();                                      /* spin the final ~3ms */
    }
    g_last.QuadPart += (LONGLONG)target;                             /* fixed cadence (no drift) */
    if ((double)(now.QuadPart - g_last.QuadPart) > target * 4.0) g_last = now;  /* resync after a stall */

    static LARGE_INTEGER mark; static int frames = 0;
    if (frames == 0) mark = now;
    if (++frames >= 180) {
        double secs = (double)(now.QuadPart - mark.QuadPart) / (double)g_freq.QuadPart;
        if (secs > 0.0) tlog("D3D8: measured %.1f fps (cap %.0f)\n", frames / secs, g_target_fps);
        frames = 0;
    }
}

void ultra_genpc_d3d8_teardown(void) {
    if (g_timeperiod_set) { timeEndPeriod(1); g_timeperiod_set = 0; }
}

/* Device window getter for the Omni-Bridge (ultra_omni_bridge_install's
 * get_window callback) -- see hook_dllmain_generic.c. NULL until CreateDevice
 * has actually fired (the bridge stays inert until then). */
HWND ultra_genpc_d3d8_window(void) { return g_devwnd; }

static HRESULT __stdcall Hook_Present8(void* self, const RECT* src, const RECT* dst,
                                       HWND hwndOverride, const void* dirty) {
    throttle();
    return Real_Present8(self, src, dst, hwndOverride, dirty);
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

static HRESULT __stdcall Hook_CreateDevice8(void* self, UINT adapter, int devtype, HWND hwnd,
                                            DWORD flags, void* pp, void** ppdev) {
    /* D3DPRESENT_PARAMETERS (x86, 4-byte fields): [0]bbW [1]bbH [2]bbFmt [3]bbCount
     * [4]MultiSample [5]SwapEffect [6]hDeviceWindow [7]Windowed [8]AutoDepth
     * [9]AutoDepthFmt [10]Flags [11]RefreshHz [12]PresentInterval */
    if (pp) {
        unsigned* p = (unsigned*)pp;
        g_devwnd = (HWND)(uintptr_t)p[6];
        if (!g_devwnd) g_devwnd = hwnd;
        tlog("D3D8: CreateDevice bb=%ux%u fmt=%u hwnd=%08X windowed=%u\n",
             p[0], p[1], p[2], p[6], p[7]);
        if (force_windowed() && !p[7]) {
            p[7]  = 1;    /* Windowed = TRUE */
            p[2]  = 22;   /* BackBufferFormat = D3DFMT_X8R8G8B8 (desktop-safe) */
            p[11] = 0;    /* FullScreen_RefreshRateInHz must be 0 when windowed */
            p[12] = 0;    /* PresentationInterval = DEFAULT (windowed) */
            tlog("D3D8: forced WINDOWED (backbuffer stays %ux%u)\n", p[0], p[1]);
        }
    } else if (!g_devwnd) {
        g_devwnd = hwnd;
    }
    HRESULT hr = Real_CreateDevice8(self, adapter, devtype, hwnd, flags, pp, ppdev);
    if (hr >= 0 && ppdev && *ppdev && !g_present_hooked) {
        patch_vtable_entry(*ppdev, IDIRECT3DDEVICE8_PRESENT, (void*)Hook_Present8, (void**)&Real_Present8);
        g_present_hooked = 1;
        tlog("D3D8: device created -> Present hooked (limiter armed)\n");
        { HWND sp = FindWindowA("UltraLoaderSplash", NULL);
          if (sp) { ShowWindow(sp, SW_HIDE); tlog("D3D8: dismissed loader splash\n"); } }
        if (g_devwnd) SetForegroundWindow(g_devwnd);
    }
    return hr;
}

/* Patch IDirect3D8::CreateDevice (slot 15) on the returned IDirect3D8. */
void ultra_genpc_d3d8_install_throttle(void* d3d8) {
    if (!d3d8) return;
    patch_vtable_entry(d3d8, IDIRECT3D8_CREATEDEVICE, (void*)Hook_CreateDevice8, (void**)&Real_CreateDevice8);
}

/* Real Direct3DCreate8 hook. Called via the IAT patch AND the GetProcAddress
 * interception in jvs_serial_hook_generic.c. Resolves the real export, calls
 * it, then arms the throttle on the returned IDirect3D8. */
typedef void* (__stdcall *pDirect3DCreate8)(UINT);
static pDirect3DCreate8 Real_Direct3DCreate8 = NULL;
void* __stdcall ultra_genpc_hook_direct3dcreate8(UINT ver) {
    if (!Real_Direct3DCreate8) {
        HMODULE m = GetModuleHandleA("d3d8.dll");
        if (!m) m = LoadLibraryA("d3d8.dll");
        if (m) Real_Direct3DCreate8 = (pDirect3DCreate8)(void*)GetProcAddress(m, "Direct3DCreate8");
    }
    void* r = Real_Direct3DCreate8 ? Real_Direct3DCreate8(ver) : NULL;
    tlog("D3D8: Direct3DCreate8(0x%X) -> %p %s\n", ver, r, r ? "OK" : "NULL(D3D8 init failed)");
    if (r) ultra_genpc_d3d8_install_throttle(r);
    return r;
}

#endif /* _WIN32 */
