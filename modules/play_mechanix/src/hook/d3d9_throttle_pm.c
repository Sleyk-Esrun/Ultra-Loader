/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  d3d9_throttle_pm.c -- Direct3D9 present hook for the Play-Mechanix-only
 *                        hook DLL (this board's own copy)
 * =============================================================================
 *
 *  PROVENANCE: adapted from modules/native_x86/src/hook/d3d9_throttle.c (read
 *  only as reference) into Play Mechanix's own independent hook DLL, per the
 *  project owner's per-board-independence directive. Same proven present-hook
 *  shape every other board's D3D9 hook uses (frame limiter + force-windowed +
 *  optional upscale/rotate), PLUS this board's own lightgun device-window
 *  hand-off: pm_gun_bridge.c needs the game's real device window to map its
 *  aim source into client pixels, so CreateDevice hands that window to
 *  pm_gun_bridge_set_window() the moment it is known.
 *
 *  Play Mechanix's Chicago-PC catalog (Big Buck Hunter and kin) is assumed
 *  D3D9-era hardware -- no real dump on hand to confirm; this is the best-
 *  evidenced render API for a cabinet of this generation. Like every other
 *  native arcade board here, the game paces its loop off Present (a real
 *  cabinet's 60Hz vsync); windowed on a PC runs unthrottled without this hook.
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

#include "pm_hook.h"

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

#define tlog pm_log

/* ---- upscaler / rotation state --------------------------------------------- */
static HWND g_devwnd   = NULL;
static int  g_up_w = 0, g_up_h = 0;     /* target client size (0 = no upscale)  */
static int  g_upscaled = 0;
static int  g_rotate   = 0;             /* 0 / 90 / 180 / 270                    */
static int  g_aspect_fill = 1;          /* 1 = stretch, 0 = keep aspect          */
static int  g_bb_w = 640, g_bb_h = 480; /* game backbuffer size                  */

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
    if (g_up_w == 0) {
        const char* bl = getenv("ULTRA_BORDERLESS");
        if (bl && *bl == '1') {
            g_up_w = GetSystemMetrics(SM_CXSCREEN);
            g_up_h = GetSystemMetrics(SM_CYSCREEN);
        }
    }
    const char* rot = getenv("ULTRA_ROTATE");
    if (rot && *rot) { int v = atoi(rot); if (v==90||v==180||v==270) g_rotate = v; }
    const char* asp = getenv("ULTRA_ASPECT");
    char ac = (asp && *asp) ? asp[0] : 0;
    if (ac=='k'||ac=='K'||ac=='p'||ac=='P') g_aspect_fill = 0;
    else if (ac=='f'||ac=='F'||ac=='s'||ac=='S') g_aspect_fill = 1;
    else if (g_rotate) g_aspect_fill = 0;
    if (g_rotate && g_up_w == 0) { g_up_w = 1920; g_up_h = 1080; }
}

/* Force-windowed control. A fullscreen-EXCLUSIVE D3D9 device is LOST whenever
 * it loses foreground focus. Default: force windowed when ULTRA_PM_WINDOWED=1
 * (play_mechanix_main.c sets this) or the universal ULTRA_WINDOWED/
 * ULTRA_BORDERLESS/ULTRA_SEGA_WINDOWED knobs; never when ULTRA_PM_WINDOWED=0. */
static int force_windowed(void) {
    static int v = -1;
    if (v < 0) {
        const char* w = getenv("ULTRA_PM_WINDOWED");
        if (w && *w) { v = (w[0] == '1'); }
        else {
            const char* uw = getenv("ULTRA_WINDOWED");
            const char* bl = getenv("ULTRA_BORDERLESS");
            const char* sw = getenv("ULTRA_SEGA_WINDOWED");
            if ((uw && *uw == '1') || (bl && *bl == '1') || (sw && *sw == '1')) v = 1;
            else v = 0;
        }
    }
    return v;
}

/* ---- frame limiter (QPC, API-agnostic) ------------------------------------- */
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
        tlog("[play_mechanix] D3D9: frame limiter armed @ %.2f fps", g_target_fps);
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

typedef struct { float x, y, z, rhw, u, v; } RVERT;

/* ---- rotation post-process resources (only touched when ULTRA_ROTATE set) -- */
static IDirect3DTexture9*   g_tex     = NULL;
static IDirect3DSurface9*   g_scratch = NULL;
static IDirect3DStateBlock9* g_sb     = NULL;

static int ensure_rot_resources9(IDirect3DDevice9* dev) {
    if (g_tex && g_scratch) return 1;
    if (!g_tex) {
        HRESULT hr = IDirect3DDevice9_CreateTexture(dev, g_bb_w, g_bb_h, 1, D3DUSAGE_RENDERTARGET,
                                                    D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_tex, NULL);
        if (FAILED(hr)) { tlog("[play_mechanix] D3D9: CreateTexture(RT) failed 0x%08lX", hr); return 0; }
    }
    if (!g_scratch) {
        HRESULT hr = IDirect3DDevice9_CreateRenderTarget(dev, 64, 64, D3DFMT_X8R8G8B8,
                        D3DMULTISAMPLE_NONE, 0, FALSE, &g_scratch, NULL);
        if (FAILED(hr)) { tlog("[play_mechanix] D3D9: CreateRenderTarget(scratch) failed 0x%08lX", hr); return 0; }
    }
    return 1;
}
static void release_default_resources9(void) {
    if (g_tex)     { IDirect3DTexture9_Release(g_tex);     g_tex = NULL; }
    if (g_scratch) { IDirect3DSurface9_Release(g_scratch); g_scratch = NULL; }
}

static int present_rotate9(IDirect3DDevice9* dev) {
    if (!g_rotate) return 0;
    if (!ensure_rot_resources9(dev)) return 0;

    IDirect3DSurface9 *gameBB = NULL, *texSurf = NULL, *oldRT = NULL, *oldDS = NULL;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &gameBB))) return 0;
    if (FAILED(IDirect3DTexture9_GetSurfaceLevel(g_tex, 0, &texSurf))) { IDirect3DSurface9_Release(gameBB); return 0; }
    IDirect3DDevice9_GetRenderTarget(dev, 0, &oldRT);
    IDirect3DDevice9_GetDepthStencilSurface(dev, &oldDS);

    HRESULT hrSR = E_FAIL;
    IDirect3DDevice9_SetRenderTarget(dev, 0, g_scratch);
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);
    hrSR = IDirect3DDevice9_StretchRect(dev, gameBB, NULL, texSurf, NULL, D3DTEXF_LINEAR);

    int ok = 0;
    if (SUCCEEDED(hrSR) && SUCCEEDED(IDirect3DDevice9_SetRenderTarget(dev, 0, gameBB))) {
        if (!g_sb) IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &g_sb);
        if (g_sb) {
            IDirect3DStateBlock9_Capture(g_sb);
            IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
            if (SUCCEEDED(IDirect3DDevice9_BeginScene(dev))) {
                float W = (float)g_bb_w, H = (float)g_bb_h, dx = 0, dy = 0, dw = W, dh = H;
                if (!g_aspect_fill) {
                    float WW = (float)g_up_w, WH = (float)g_up_h;
                    float aw = (g_rotate == 90 || g_rotate == 270) ? (float)g_bb_h : (float)g_bb_w;
                    float ah = (g_rotate == 90 || g_rotate == 270) ? (float)g_bb_w : (float)g_bb_h;
                    float s = (WW/aw < WH/ah) ? WW/aw : WH/ah;
                    float fw = aw * s, fh = ah * s;
                    float fx = (WW - fw) * 0.5f, fy = (WH - fh) * 0.5f;
                    dx = fx / WW * W; dy = fy / WH * H;
                    dw = fw / WW * W; dh = fh / WH * H;
                }
                float u[4], v[4];
                switch (g_rotate) {
                    case 90:  u[0]=0;v[0]=1; u[1]=0;v[1]=0; u[2]=1;v[2]=0; u[3]=1;v[3]=1; break;
                    case 180: u[0]=1;v[0]=1; u[1]=0;v[1]=1; u[2]=0;v[2]=0; u[3]=1;v[3]=0; break;
                    case 270: u[0]=1;v[0]=0; u[1]=1;v[1]=1; u[2]=0;v[2]=1; u[3]=0;v[3]=0; break;
                    default:  u[0]=0;v[0]=0; u[1]=1;v[1]=0; u[2]=1;v[2]=1; u[3]=0;v[3]=1; break;
                }
                const float o = -0.5f;
                RVERT q[4] = {
                    { dx+o,    dy+o,    0, 1, u[0], v[0] },
                    { dx+dw+o, dy+o,    0, 1, u[1], v[1] },
                    { dx+dw+o, dy+dh+o, 0, 1, u[2], v[2] },
                    { dx+o,    dy+dh+o, 0, 1, u[3], v[3] },
                };
                IDirect3DDevice9_SetVertexShader(dev, NULL);
                IDirect3DDevice9_SetPixelShader(dev, NULL);
                IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
                IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
                IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
                IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
                IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
                IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
                IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
                IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
                IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
                IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0x0000000F);
                IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)g_tex);
                IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
                IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE);
                IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP,   D3DTOP_DISABLE);
                IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE);
                IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
                IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
                IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 2, q, sizeof(RVERT));
                IDirect3DDevice9_EndScene(dev);
                ok = 1;
            }
            IDirect3DStateBlock9_Apply(g_sb);
        }
        IDirect3DDevice9_SetRenderTarget(dev, 0, oldRT);
    }
    IDirect3DDevice9_SetDepthStencilSurface(dev, oldDS);
    if (oldDS)   IDirect3DSurface9_Release(oldDS);
    if (oldRT)   IDirect3DSurface9_Release(oldRT);
    if (texSurf) IDirect3DSurface9_Release(texSurf);
    if (gameBB)  IDirect3DSurface9_Release(gameBB);
    return ok;
}

/* ---- output-window resize for upscale -------------------------------------- */
static WNDPROC g_orig_wndproc = NULL;
static LRESULT CALLBACK pm_output_wndproc9(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if (g_orig_wndproc) return CallWindowProcA(g_orig_wndproc, h, msg, wp, lp);
    return DefWindowProcA(h, msg, wp, lp);
}
static void resize_output_window9(HWND w) {
    if (!w || g_upscaled) return;
    SetClassLongPtrA(w, GCLP_HBRBACKGROUND, (LONG_PTR)GetStockObject(BLACK_BRUSH));
    if ((WNDPROC)(LONG_PTR)GetWindowLongPtrA(w, GWLP_WNDPROC) != pm_output_wndproc9)
        g_orig_wndproc = (WNDPROC)(LONG_PTR)SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)pm_output_wndproc9);
    RECT wr = { 0, 0, g_up_w, g_up_h };
    SetWindowLongA(w, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    AdjustWindowRect(&wr, WS_POPUP, FALSE);
    SetWindowPos(w, HWND_TOPMOST, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(w);
    InvalidateRect(w, NULL, TRUE);
    g_upscaled = 1;
    tlog("[play_mechanix] D3D9: output window -> %dx%d%s", g_up_w, g_up_h, g_rotate ? " (rotated)" : "");
}

static HRESULT __stdcall Hook_Present9(void* self, const RECT* src, const RECT* dst,
                                       HWND hwndOverride, const RGNDATA* dirty) {
    throttle();
    IDirect3DDevice9* dev = (IDirect3DDevice9*)self;
    const int do_rotate = (g_rotate != 0);
    int dev_ok = 1;
    if (do_rotate) dev_ok = (IDirect3DDevice9_TestCooperativeLevel(dev) == D3D_OK);

    if ((g_up_w > 0 || do_rotate) && !g_upscaled)
        resize_output_window9(g_devwnd ? g_devwnd : hwndOverride);

    if (do_rotate && dev_ok) present_rotate9(dev);

    if (g_up_w > 0 || do_rotate)
        return Real_Present9(self, NULL, NULL, hwndOverride, NULL);
    return Real_Present9(self, src, dst, hwndOverride, dirty);
}

static HRESULT __stdcall Hook_Reset9(void* self, D3DPRESENT_PARAMETERS* pp) {
    release_default_resources9();
    if (g_sb) { IDirect3DStateBlock9_Release(g_sb); g_sb = NULL; }
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
    tlog("[play_mechanix] D3D9: Reset -> 0x%08lX (bb=%dx%d)", hr, g_bb_w, g_bb_h);
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
        tlog("[play_mechanix] D3D9: CreateDevice bb=%ux%u fmt=%u hwnd=%08X windowed=%u",
             pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat,
             (unsigned)(uintptr_t)pp->hDeviceWindow, pp->Windowed);
        if ((g_up_w > 0 || force_windowed()) && !pp->Windowed) {
            pp->Windowed = TRUE;
            pp->BackBufferFormat = D3DFMT_X8R8G8B8;
            pp->FullScreen_RefreshRateInHz = 0;
            pp->PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
            tlog("[play_mechanix] D3D9: forced WINDOWED (backbuffer stays %ux%u)",
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
        tlog("[play_mechanix] D3D9: device created -> Present+Reset hooked (limiter%s)",
             g_rotate?" + rotate":"");
        /* Hand the real device window to the lightgun bridge so its mouse-
         * fallback aim maps to the right client rect. */
        if (g_devwnd) pm_gun_bridge_set_window(g_devwnd);
        { HWND sp = FindWindowA("UltraLoaderSplash", NULL);
          if (sp) { ShowWindow(sp, SW_HIDE); tlog("[play_mechanix] D3D9: dismissed loader splash"); } }
        if (g_devwnd) {
            if (force_windowed() && g_up_w == 0) {
                int dw = GetSystemMetrics(SM_CXSCREEN);
                int dh = GetSystemMetrics(SM_CYSCREEN);
                SetWindowLongA(g_devwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                SetWindowPos(g_devwnd, HWND_TOPMOST, 0, 0, dw, dh,
                             SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                SetWindowPos(g_devwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE);
                tlog("[play_mechanix] D3D9: force-windowed borderless %dx%d (no upscale)", dw, dh);
            }
            SetForegroundWindow(g_devwnd);
        }
    }
    return hr;
}

static void pm_d3d9_install_throttle(void* d3d9) {
    if (!d3d9) return;
    patch_vtable_entry(d3d9, IDIRECT3D9_CREATEDEVICE, (void*)Hook_CreateDevice9, (void**)&Real_CreateDevice9);
}

typedef IDirect3D9* (__stdcall *pDirect3DCreate9)(UINT);
static pDirect3DCreate9 Real_Direct3DCreate9 = NULL;
void* __stdcall PM_Hook_Direct3DCreate9(UINT ver) {
    if (!Real_Direct3DCreate9) {
        HMODULE m = GetModuleHandleA("d3d9.dll");
        if (!m) m = LoadLibraryA("d3d9.dll");
        if (m) Real_Direct3DCreate9 = (pDirect3DCreate9)(void*)ultra_raw_export(m, "Direct3DCreate9");
    }
    IDirect3D9* r = Real_Direct3DCreate9 ? Real_Direct3DCreate9(ver) : NULL;
    tlog("[play_mechanix] D3D9: Direct3DCreate9(0x%X) -> %p %s", ver, (void*)r, r ? "OK" : "NULL(D3D9 init failed)");
    if (r) pm_d3d9_install_throttle(r);
    return r;
}

void pm_d3d9_install(void) {
    pm_iat_patch_all("d3d9.dll", "Direct3DCreate9", (void*)PM_Hook_Direct3DCreate9);
}

/* Current device/render window, or NULL until CreateDevice fires. Used by
 * pm_gun_bridge_set_window() (already, via the CreateDevice hook above) and by
 * the Omni-Bridge window-getter callback wired in hook_dllmain_pm.c. */
HWND pm_d3d9_get_window(void) { return g_devwnd; }

void pm_d3d9_teardown(void) {
    if (g_timing_init) { timeEndPeriod(1); g_timing_init = 0; }
}

#endif /* _WIN32 */
