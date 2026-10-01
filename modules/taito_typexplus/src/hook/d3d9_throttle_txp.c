/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  d3d9_throttle_txp.c -- frame limiter + present post-process for Taito Type X+
 *                         (Direct3D 9). Board's own independent copy.
 * =============================================================================
 *
 *  Taito Type X+ titles are PC-class Direct3D 9 games that pace their loop off
 *  Present (the cabinet's 60Hz vsync); windowed on a PC that runs unthrottled
 *  would fast-forward. We IAT-hook d3d9.dll!Direct3DCreate9 ->
 *  IDirect3D9::CreateDevice -> IDirect3DDevice9::Present and:
 *    - throttle Present to a target fps (default 60, ULTRA_FPS_CAP),
 *    - optionally rotate / upscale-to-window / sharpen (all DEFAULT-OFF, byte-
 *      identical native path when all off),
 *    - dump the back buffer to a 24-bit BMP (ULTRA_ROT_DUMP=dir) for offline
 *      verification when there is no real dump to eyeball on-screen yet.
 *
 *  Read modules/native_x86/src/hook/d3d9_throttle.c as a reference only -- this
 *  is Taito Type X+'s own compiled copy, with the Taito Type X2-specific gun
 *  bridge call removed (this board has no lightgun bridge dependency) and its
 *  own env-var names for the force-windowed knobs.
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

#include "hook_txp.h"

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

#define tlog ultra_txp_log

/* ---- upscaler / rotation / sharpen state ----------------------------------- */
static HWND g_devwnd   = NULL;
static int  g_up_w = 0, g_up_h = 0;     /* target client size (0 = no upscale)  */
static int  g_upscaled = 0;
static int  g_rotate   = 0;             /* 0 / 90 / 180 / 270                    */
static int  g_aspect_fill = 1;          /* 1 = stretch, 0 = keep aspect          */
static int  g_bb_w = 640, g_bb_h = 480; /* game backbuffer size (capture src)    */
static float g_sharpen = 0.0f;          /* ULTRA_SHARPEN [0,1] (0 = off)         */
static int   g_aa      = 0;             /* ULTRA_AA edge-smoothing toggle        */

static IDirect3DTexture9*   g_tex        = NULL;
static IDirect3DSurface9*   g_scratch    = NULL;  /* tiny RT to free the backbuffer */
static IDirect3DSurface9*   g_dumpsys    = NULL;  /* sysmem readback for BMP dump  */
static int                  g_dumpsys_w = 0, g_dumpsys_h = 0;
static D3DFORMAT            g_dumpsys_fmt = D3DFMT_UNKNOWN;

/* Current device/render window, or NULL before the game's first CreateDevice.
 * Ultra Omni-Bridge (hook_dllmain_txp.c) polls this as its window getter. */
HWND ultra_txp_d3d9_window(void) {
    return g_devwnd;
}

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
    /* Borderless window: no explicit target size -> auto-set to monitor
     * resolution so the hook upscales to fill the monitor with WS_POPUP. */
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
    const char* shp = getenv("ULTRA_SHARPEN");
    if (shp && *shp) { float v = (float)atof(shp); if (v<0) v=0; if (v>1) v=1; g_sharpen = v; }
    const char* aa = getenv("ULTRA_AA");
    if (aa && *aa) g_aa = atoi(aa);
}

/* Force-windowed control. A fullscreen-EXCLUSIVE D3D9 device is LOST whenever
 * it loses foreground focus; under an unattended auto-pilot that (re)forces
 * the game window, running windowed makes the device focus-independent.
 * Default: force windowed when ULTRA_TXP_AUTO=1 OR ULTRA_TXP_WINDOWED=1, or
 * the universal ULTRA_WINDOWED/ULTRA_BORDERLESS knobs; never when
 * ULTRA_TXP_WINDOWED=0. Off otherwise (native fullscreen path). */
static int force_windowed(void) {
    static int v = -1;
    if (v < 0) {
        const char* w = getenv("ULTRA_TXP_WINDOWED");
        if (w && *w) { v = (w[0] == '1'); }
        else {
            const char* uw = getenv("ULTRA_WINDOWED");
            const char* bl = getenv("ULTRA_BORDERLESS");
            if ((uw && *uw == '1') || (bl && *bl == '1')) v = 1;
            else { const char* a = getenv("ULTRA_TXP_AUTO"); v = (a && a[0] == '1'); }
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
        tlog("D3D9: frame limiter armed @ %.2f fps", g_target_fps);
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
        if (secs > 0.0) tlog("D3D9: measured %.1f fps (cap %.0f)", frames / secs, g_target_fps);
        frames = 0;
    }
}

typedef struct { float x, y, z, rhw, u, v; } RVERT;

/* ---- BMP dump (the verification path) -------------------------------------- */
static void dump_backbuffer_bmp9(IDirect3DDevice9* dev, const char* path) {
    if (!dev) return;
    static int front = -1;
    if (front < 0) { const char* e = getenv("ULTRA_DUMP_FRONT"); front = (e && e[0]=='1'); }
    if (front) {
        D3DDISPLAYMODE dm; if (FAILED(IDirect3DDevice9_GetDisplayMode(dev, 0, &dm))) { tlog("D3D9 dump(front): GetDisplayMode fail"); return; }
        int fw = (int)dm.Width, fh = (int)dm.Height;
        if (g_dumpsys && (g_dumpsys_w != fw || g_dumpsys_h != fh || g_dumpsys_fmt != D3DFMT_A8R8G8B8)) {
            IDirect3DSurface9_Release(g_dumpsys); g_dumpsys = NULL;
        }
        if (!g_dumpsys) {
            HRESULT hc = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, fw, fh,
                            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_dumpsys, NULL);
            if (FAILED(hc) || !g_dumpsys) { tlog("D3D9 dump(front): CreateOffscreenPlainSurface 0x%08lX (%dx%d)", hc, fw, fh); return; }
            g_dumpsys_w = fw; g_dumpsys_h = fh; g_dumpsys_fmt = D3DFMT_A8R8G8B8;
        }
        HRESULT hr = IDirect3DDevice9_GetFrontBufferData(dev, 0, g_dumpsys);
        if (FAILED(hr)) { tlog("D3D9 dump(front): GetFrontBufferData 0x%08lX (%dx%d)", hr, fw, fh); return; }
        D3DLOCKED_RECT flr;
        if (FAILED(IDirect3DSurface9_LockRect(g_dumpsys, &flr, NULL, D3DLOCK_READONLY))) { tlog("D3D9 dump(front): LockRect fail"); return; }
        int rowsz = fw*3, pad = (4-(rowsz&3))&3, stride = rowsz+pad, imgsz = stride*fh;
        unsigned char fhh[14]={0}, ihh[40]={0}; unsigned filesz = 14+40+imgsz;
        fhh[0]='B'; fhh[1]='M'; *(unsigned*)(fhh+2)=filesz; *(unsigned*)(fhh+10)=54;
        *(unsigned*)(ihh+0)=40; *(int*)(ihh+4)=fw; *(int*)(ihh+8)=fh;
        *(unsigned short*)(ihh+12)=1; *(unsigned short*)(ihh+14)=24; *(unsigned*)(ihh+20)=imgsz;
        FILE* ff = fopen(path,"wb");
        if (ff) {
            fwrite(fhh,1,14,ff); fwrite(ihh,1,40,ff);
            unsigned char* row=(unsigned char*)malloc(stride);
            for (int y=fh-1;y>=0;--y){ unsigned char* src=(unsigned char*)flr.pBits + y*flr.Pitch;
                memset(row,0,stride);
                for (int x=0;x<fw;++x){ row[x*3+0]=src[x*4+0]; row[x*3+1]=src[x*4+1]; row[x*3+2]=src[x*4+2]; }
                fwrite(row,1,stride,ff); }
            free(row); fclose(ff);
            tlog("D3D9: dumped FRONT %dx%d -> %s", fw, fh, path);
        }
        IDirect3DSurface9_UnlockRect(g_dumpsys);
        return;
    }
    IDirect3DSurface9* bb = NULL;
    HRESULT hgb = IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hgb) || !bb) { tlog("D3D9 dump: GetBackBuffer 0x%08lX", hgb); return; }
    D3DSURFACE_DESC d;
    if (FAILED(IDirect3DSurface9_GetDesc(bb, &d))) { tlog("D3D9 dump: GetDesc fail"); IDirect3DSurface9_Release(bb); return; }
    if (g_dumpsys && (g_dumpsys_w != (int)d.Width || g_dumpsys_h != (int)d.Height || g_dumpsys_fmt != d.Format)) {
        IDirect3DSurface9_Release(g_dumpsys); g_dumpsys = NULL;
    }
    if (!g_dumpsys) {
        HRESULT hc = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height,
                        d.Format, D3DPOOL_SYSTEMMEM, &g_dumpsys, NULL);
        if (FAILED(hc) || !g_dumpsys) { tlog("D3D9 dump: CreateOffscreenPlainSurface 0x%08lX", hc); IDirect3DSurface9_Release(bb); return; }
        g_dumpsys_w = (int)d.Width; g_dumpsys_h = (int)d.Height; g_dumpsys_fmt = d.Format;
    }
    HRESULT hr = IDirect3DDevice9_GetRenderTargetData(dev, bb, g_dumpsys);
    IDirect3DSurface9_Release(bb);
    if (FAILED(hr)) { tlog("D3D9 dump: GetRenderTargetData 0x%08lX (fmt=%u %ux%u)", hr, d.Format, d.Width, d.Height); return; }

    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DSurface9_LockRect(g_dumpsys, &lr, NULL, D3DLOCK_READONLY))) { tlog("D3D9 dump: LockRect fail"); return; }
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
        tlog("D3D9: dumped %dx%d fmt=%u -> %s", w, h, d.Format, path);
    }
    IDirect3DSurface9_UnlockRect(g_dumpsys);
}

/* ---- rotation / sharpen post-process resources ----------------------------- */
static int ensure_rot_resources9(IDirect3DDevice9* dev) {
    if (g_tex && g_scratch) return 1;
    if (!g_tex) {
        HRESULT hr = IDirect3DDevice9_CreateTexture(dev, g_bb_w, g_bb_h, 1, D3DUSAGE_RENDERTARGET,
                                                    D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_tex, NULL);
        if (FAILED(hr)) { tlog("D3D9: CreateTexture(RT) failed 0x%08lX", hr); return 0; }
        IDirect3DSurface9* ts = NULL;
        if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(g_tex, 0, &ts)) && ts) {
            IDirect3DSurface9* prevRT = NULL;
            IDirect3DDevice9_GetRenderTarget(dev, 0, &prevRT);
            if (SUCCEEDED(IDirect3DDevice9_SetRenderTarget(dev, 0, ts)))
                IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
            if (prevRT) { IDirect3DDevice9_SetRenderTarget(dev, 0, prevRT); IDirect3DSurface9_Release(prevRT); }
            IDirect3DSurface9_Release(ts);
        }
    }
    if (!g_scratch) {
        HRESULT hr = IDirect3DDevice9_CreateRenderTarget(dev, 64, 64, D3DFMT_X8R8G8B8,
                        D3DMULTISAMPLE_NONE, 0, FALSE, &g_scratch, NULL);
        if (FAILED(hr)) { tlog("D3D9: CreateRenderTarget(scratch) failed 0x%08lX", hr); return 0; }
    }
    tlog("D3D9: rotation resources ready (rotate=%d out=%dx%d src=%dx%d)", g_rotate, g_up_w, g_up_h, g_bb_w, g_bb_h);
    return 1;
}

static void release_default_resources9(void) {
    if (g_tex)     { IDirect3DTexture9_Release(g_tex);     g_tex = NULL; }
    if (g_scratch) { IDirect3DSurface9_Release(g_scratch); g_scratch = NULL; }
}

#define PP_ROTATE   1
#define PP_SHARPEN  2
static IDirect3DStateBlock9* g_sb = NULL;
static int present_postprocess9(IDirect3DDevice9* dev, int mode) {
    if (!(mode & PP_ROTATE)) return 0;
    if (!ensure_rot_resources9(dev)) return 0;

    IDirect3DSurface9 *gameBB = NULL, *texSurf = NULL, *oldRT = NULL, *oldDS = NULL;
    HRESULT hrGBB = IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &gameBB);
    HRESULT hrGSL = IDirect3DTexture9_GetSurfaceLevel(g_tex, 0, &texSurf);
    IDirect3DDevice9_GetRenderTarget(dev, 0, &oldRT);
    IDirect3DDevice9_GetDepthStencilSurface(dev, &oldDS);

    HRESULT hrSR = E_FAIL;
    if (gameBB && texSurf && g_scratch) {
        IDirect3DDevice9_SetRenderTarget(dev, 0, g_scratch);
        IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);
        hrSR = IDirect3DDevice9_StretchRect(dev, gameBB, NULL, texSurf, NULL, D3DTEXF_LINEAR);
    }
    if (FAILED(hrSR)) {
        if (oldRT) IDirect3DDevice9_SetRenderTarget(dev, 0, oldRT);
        IDirect3DDevice9_SetDepthStencilSurface(dev, oldDS);
        if (oldDS)   IDirect3DSurface9_Release(oldDS);
        if (oldRT)   IDirect3DSurface9_Release(oldRT);
        if (texSurf) IDirect3DSurface9_Release(texSurf);
        if (gameBB)  IDirect3DSurface9_Release(gameBB);
        static int warned = 0;
        if (!warned) { warned = 1; tlog("D3D9 ROT: StretchRect capture failed 0x%08lX -- leaving frame untouched", hrSR); }
        return 0;
    }

    HRESULT hrSRT = E_FAIL, hrBS = E_FAIL, hrDP = E_FAIL;
    if (gameBB && SUCCEEDED(hrSRT = IDirect3DDevice9_SetRenderTarget(dev, 0, gameBB))) {
        if (!g_sb) IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &g_sb);
        if (!g_sb) {
            if (oldRT) IDirect3DDevice9_SetRenderTarget(dev, 0, oldRT);
            IDirect3DDevice9_SetDepthStencilSurface(dev, oldDS);
            if (oldDS)   IDirect3DSurface9_Release(oldDS);
            if (oldRT)   IDirect3DSurface9_Release(oldRT);
            if (texSurf) IDirect3DSurface9_Release(texSurf);
            if (gameBB)  IDirect3DSurface9_Release(gameBB);
            static int warned = 0;
            if (!warned) { warned = 1; tlog("D3D9 ROT: CreateStateBlock failed -- skipping post-process (no clobber)"); }
            return 0;
        }
        IDirect3DStateBlock9_Capture(g_sb);

        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        if (SUCCEEDED(hrBS = IDirect3DDevice9_BeginScene(dev))) {
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
            const float o = -0.5f;   /* texel-center alignment */
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
            hrDP = IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 2, q, sizeof(RVERT));
            IDirect3DDevice9_EndScene(dev);
        }
        if (g_sb) IDirect3DStateBlock9_Apply(g_sb);
        if (oldRT) IDirect3DDevice9_SetRenderTarget(dev, 0, oldRT);
    }
    IDirect3DDevice9_SetDepthStencilSurface(dev, oldDS);
    if (oldDS)   IDirect3DSurface9_Release(oldDS);
    if (oldRT)   IDirect3DSurface9_Release(oldRT);
    if (texSurf) IDirect3DSurface9_Release(texSurf);
    if (gameBB)  IDirect3DSurface9_Release(gameBB);

    static int diag = 0;
    if (!diag) { diag = 1;
        tlog("D3D9 ROT DIAG: GetBB=0x%08lX GetSurf=0x%08lX StretchRect=0x%08lX SetRT=0x%08lX BeginScene=0x%08lX DrawPrim=0x%08lX",
             hrGBB, hrGSL, hrSR, hrSRT, hrBS, hrDP);
    }
    return (hrSRT == D3D_OK);
}

/* ---- output-window resize for upscale -------------------------------------- */
static WNDPROC g_orig_wndproc = NULL;

/* PAINT THE LETTERBOX BLACK. DO NOT JUST REFUSE TO PAINT IT.
 *
 * This used to answer WM_ERASEBKGND with 1 and nothing else, which tells
 * Windows "the background is already erased" -- so the BLACK_BRUSH set on the
 * class below was never actually used, and the part of the client area that
 * Present does not cover kept whatever the window surface started life as.
 * That is white. With aspect=fill the picture covers every pixel so nobody
 * noticed; the first title to ask for a letterbox got two white bars beside
 * the game.
 *
 * Filling it here costs one FillRect per WM_ERASEBKGND, and a D3D9 Present
 * generates no paint messages at all, so this runs when the window is resized
 * or uncovered and essentially never otherwise. */
static void paint_black9(HWND h, HDC dc) {
    RECT rc;
    if (!dc || !GetClientRect(h, &rc)) return;
    FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
}
static LRESULT CALLBACK ultra_output_wndproc9(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) { paint_black9(h, (HDC)wp); return 1; }
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
    SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(w);
    /* Black it out NOW rather than waiting for a paint message that may never
     * come -- a D3D9 game that presents every frame can go its whole life
     * without a WM_PAINT, and until one arrives the bars stay white. */
    { HDC dc = GetDC(w); if (dc) { paint_black9(w, dc); ReleaseDC(w, dc); } }
    InvalidateRect(w, NULL, TRUE);
    g_upscaled = 1;
    tlog("D3D9: output window -> %dx%d%s (black bg, erase swallowed, foreground)", g_up_w, g_up_h, g_rotate ? " (rotated)" : "");
}

/* ---- dump scheduling -------------------------------------------------------- */
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

    /* ONE-SHOT GEOMETRY DUMP.
     *
     * The window is resized to 1920x1080 and the backbuffer is 640x480, and
     * the picture still lands small in a corner -- so one of the assumptions
     * in between is wrong, and reasoning about it from the outside has not
     * settled which. Print the actual numbers once: what the game asks Present
     * to do, which window it presents to, how big that window's client area
     * really is, and what the runtime says the backbuffer and viewport are.
     * Whichever of those disagrees with the others is the answer. */
    {
        static int told = 0;
        if (!told) {
            HWND pw = hwndOverride ? hwndOverride : g_devwnd;
            RECT cr = {0,0,0,0}, wr = {0,0,0,0};
            D3DVIEWPORT9 vp;
            IDirect3DSurface9* bb = NULL;
            D3DSURFACE_DESC sd;
            memset(&vp, 0, sizeof vp); memset(&sd, 0, sizeof sd);
            told = 1;
            if (pw) { GetClientRect(pw, &cr); GetWindowRect(pw, &wr); }
            IDirect3DDevice9_GetViewport(dev, &vp);
            if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0,
                          D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
                IDirect3DSurface9_GetDesc(bb, &sd);
                IDirect3DSurface9_Release(bb);
            }
            tlog("D3D9 GEOM: srcRect=%s dstRect=%s hwndOverride=%08X devwnd=%08X",
                 src ? "SET" : "NULL", dst ? "SET" : "NULL",
                 (unsigned)(uintptr_t)hwndOverride, (unsigned)(uintptr_t)g_devwnd);
            if (src) tlog("D3D9 GEOM:   src = %ld,%ld .. %ld,%ld",
                          src->left, src->top, src->right, src->bottom);
            if (dst) tlog("D3D9 GEOM:   dst = %ld,%ld .. %ld,%ld",
                          dst->left, dst->top, dst->right, dst->bottom);
            tlog("D3D9 GEOM: present-window client=%ldx%ld  window=%ld,%ld %ldx%ld",
                 cr.right - cr.left, cr.bottom - cr.top, wr.left, wr.top,
                 wr.right - wr.left, wr.bottom - wr.top);
            tlog("D3D9 GEOM: backbuffer surface = %ux%u   viewport = %lu,%lu %lux%lu",
                 sd.Width, sd.Height, vp.X, vp.Y, vp.Width, vp.Height);
            tlog("D3D9 GEOM: upscale target g_up = %dx%d  (g_upscaled=%d)",
                 g_up_w, g_up_h, g_upscaled);
        }
    }

    const int do_sharpen = (g_sharpen > 0.0f);
    const int do_rotate  = (g_rotate != 0);
    if (!g_dump_dir_read) { g_dump_dir = getenv("ULTRA_ROT_DUMP"); g_dump_dir_read = 1; }
    const int want_dump  = (g_dump_dir && *g_dump_dir);
    int dev_ok = 1;
    if (do_rotate || do_sharpen || want_dump)
        dev_ok = (IDirect3DDevice9_TestCooperativeLevel(dev) == D3D_OK);

    /* RE-ASSERT THE SIZE, DO NOT LATCH IT.
     *
     * resize_output_window9 sets g_upscaled and then refuses to run again, so
     * the window is sized exactly once per device Reset. That is enough only
     * if nothing else ever touches the window -- and here something does: the
     * log shows the resize running three times and the picture still arriving
     * at 640x480, so the size is being put back after we set it.
     *
     * Rather than fight over who ran last, check the client area every frame
     * and correct it whenever it is wrong. It costs one GetClientRect per
     * present when the size is already right, which is nothing. */
    if ((g_up_w > 0 || do_rotate)) {
        HWND ow = g_devwnd ? g_devwnd : hwndOverride;
        if (ow) {
            RECT cr;
            if (GetClientRect(ow, &cr)) {
                int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
                if (cw != g_up_w || ch != g_up_h) {
                    g_upscaled = 0;               /* clear the latch */
                    resize_output_window9(ow);
                }
            }
        }
    }

    /* Periodic geometry trace, so the STEADY state is visible and not just the
     * first frame. One line every ~5s at 60fps. */
    {
        static unsigned tick = 0;
        if ((tick++ % 300u) == 0u) {
            HWND ow = g_devwnd ? g_devwnd : hwndOverride;
            RECT cr = {0,0,0,0}, wr = {0,0,0,0};
            HWND par = ow ? GetParent(ow) : NULL;
            RECT pr = {0,0,0,0};
            if (ow) { GetClientRect(ow, &cr); GetWindowRect(ow, &wr); }
            if (par) GetWindowRect(par, &pr);
            tlog("D3D9 GEOM#%u: devwnd=%08X client=%ldx%ld win=(%ld,%ld %ldx%ld) "
                 "parent=%08X pwin=(%ld,%ld %ldx%ld) upscaled=%d",
                 tick - 1, (unsigned)(uintptr_t)ow,
                 cr.right - cr.left, cr.bottom - cr.top,
                 wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
                 (unsigned)(uintptr_t)par,
                 pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top,
                 g_upscaled);

            /* THE ACTUAL BACK BUFFER, not the size the game asked for.
             *
             * "picture sits in a corner of the screen" has exactly two causes
             * and they need opposite fixes, so guessing is worthless:
             *
             *   bb == 640x480  in a 1920x1080 client -> D3D9 is SUPPOSED to
             *       stretch on Present and is not. The window/present path is
             *       at fault.
             *   bb == 1920x1080 -> the Reset picked up our resized window, the
             *       game kept drawing its 640x480 image into the top-left of a
             *       much larger buffer, and nothing is stretching because
             *       nothing needs to. The fix is then to STOP the buffer
             *       growing, or to scale the game's own viewport.
             *
             * GetBackBuffer reports what exists right now; g_bb_* only records
             * what the caller requested, which is why the log so far cannot
             * tell these two apart. */
            {
                IDirect3DDevice9* dev9 = (IDirect3DDevice9*)self;  /* self is void* here */
                IDirect3DSurface9* bbs = NULL;
                if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(
                        dev9, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bbs)) && bbs) {
                    D3DSURFACE_DESC sd; memset(&sd, 0, sizeof sd);
                    if (SUCCEEDED(IDirect3DSurface9_GetDesc(bbs, &sd))) {
                        D3DVIEWPORT9 vp; memset(&vp, 0, sizeof vp);
                        int havevp = SUCCEEDED(IDirect3DDevice9_GetViewport(dev9, &vp));
                        tlog("D3D9 GEOM#%u: ACTUAL backbuffer=%ux%u requested=%dx%d "
                             "viewport=%s%ld,%ld %lux%lu",
                             tick - 1, sd.Width, sd.Height, g_bb_w, g_bb_h,
                             havevp ? "" : "(unavailable) ",
                             (long)vp.X, (long)vp.Y,
                             (unsigned long)vp.Width, (unsigned long)vp.Height);
                    }
                    IDirect3DSurface9_Release(bbs);
                }
            }
        }
    }

    if (do_rotate && dev_ok)
        present_postprocess9(dev, PP_ROTATE | (do_sharpen ? PP_SHARPEN : 0));

    ++g_present_count;
    if (want_dump && dev_ok && dump_due(g_present_count)) {
        char p[512];
        snprintf(p, sizeof p, "%s/dump_raw_%04d.bmp", g_dump_dir, g_present_count);
        dump_backbuffer_bmp9(dev, p);
    }

    if (g_up_w > 0 || do_rotate) {
        /* ASPECT, ON THE PATH THAT ACTUALLY RUNS.
         *
         * g_aspect_fill was only ever consulted inside present_postprocess9(),
         * and that returns immediately unless PP_ROTATE is set - so for a plain
         * non-rotated game the aspect setting was UNREACHABLE and every title
         * got a full stretch. A 640x480 game blown to 1920x1080 is 4:3 forced
         * into 16:9: everything the wrong shape, and HUD art authored for the
         * 4:3 frame ends up distorted off the sides.
         *
         * Passing an explicit destination rect is all D3D9 needs: it blits the
         * back buffer into that rect and leaves the rest of the client area
         * alone - which is already painted black (BLACK_BRUSH class background
         * + WM_ERASEBKGND swallowed in ultra_output_wndproc9), so the letterbox
         * bars come for free. NULL keeps the old full-stretch behavior when
         * fill is what was asked for. */
        RECT keep;
        const RECT* pdst = NULL;
        if (!g_aspect_fill && g_up_w > 0 && g_up_h > 0 && g_bb_w > 0 && g_bb_h > 0) {
            double sx = (double)g_up_w / (double)g_bb_w;
            double sy = (double)g_up_h / (double)g_bb_h;
            double s  = (sx < sy) ? sx : sy;
            int fw = (int)((double)g_bb_w * s + 0.5);
            int fh = (int)((double)g_bb_h * s + 0.5);
            if (fw > 0 && fh > 0) {
                keep.left   = (g_up_w - fw) / 2;
                keep.top    = (g_up_h - fh) / 2;
                keep.right  = keep.left + fw;
                keep.bottom = keep.top  + fh;
                pdst = &keep;
                {   static int said = 0;
                    if (!said) { said = 1;
                        tlog("D3D9: aspect=keep -> %dx%d source letterboxed into "
                             "%ldx%ld at (%ld,%ld) of a %dx%d window",
                             g_bb_w, g_bb_h, keep.right - keep.left,
                             keep.bottom - keep.top, keep.left, keep.top,
                             g_up_w, g_up_h); }
                }
            }
        }
        return Real_Present9(self, NULL, pdst, hwndOverride, NULL);
    }
    return Real_Present9(self, src, dst, hwndOverride, dirty);
}

static HRESULT __stdcall Hook_Reset9(void* self, D3DPRESENT_PARAMETERS* pp) {
    release_default_resources9();
    if (g_sb) { IDirect3DStateBlock9_Release(g_sb); g_sb = NULL; }
    if (pp) {
        if (pp->BackBufferWidth > 0 && pp->BackBufferHeight > 0) {
            g_bb_w = (int)pp->BackBufferWidth; g_bb_h = (int)pp->BackBufferHeight;
        }
        /* PIN THE BACK BUFFER TO THE AUTHORED SIZE.
         *
         * 0x0 means "match the window client area". Correct until we upscale:
         * once the output window is 1920x1080 the next Reset silently hands the
         * game a 1920x1080 back buffer, it carries on drawing its 640x480 image
         * into one corner, and NOTHING STRETCHES - as far as D3D9 is concerned
         * the buffer already fits the window.
         *
         * Measured on Gundam Spirits of Zeon, 2026-09-15:
         *     ACTUAL backbuffer=1920x1080 requested=640x480 viewport=0,600 640x480
         * 600 + 480 = 1080: the frame pinned to the BOTTOM of the screen, which
         * is exactly the reported symptom.
         *
         * Holding the buffer at the authored size restores the intended path -
         * the game renders 640x480 and windowed Present stretches it to fill the
         * client area. Only while upscaling, so the plain path is untouched. */
        if (g_up_w > 0 && pp->BackBufferWidth == 0 && pp->BackBufferHeight == 0 &&
            g_bb_w > 0 && g_bb_h > 0) {
            pp->BackBufferWidth  = (UINT)g_bb_w;
            pp->BackBufferHeight = (UINT)g_bb_h;
            tlog("D3D9: Reset asked for auto-size -> pinned back buffer to %dx%d "
                 "(window %dx%d; Present stretches)", g_bb_w, g_bb_h, g_up_w, g_up_h);
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
    tlog("D3D9: Reset -> 0x%08lX (bb=%dx%d)", hr, g_bb_w, g_bb_h);
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
        tlog("D3D9: CreateDevice bb=%ux%u fmt=%u swapeffect=%u hwnd=%08X windowed=%u ms=%u/%lu flags=%08lX",
             pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->SwapEffect,
             (unsigned)(uintptr_t)pp->hDeviceWindow, pp->Windowed, pp->MultiSampleType,
             (unsigned long)pp->MultiSampleQuality, (unsigned long)pp->Flags);
        if ((g_up_w > 0 || force_windowed()) && !pp->Windowed) {
            pp->Windowed = TRUE;
            pp->BackBufferFormat = D3DFMT_X8R8G8B8;
            pp->FullScreen_RefreshRateInHz = 0;
            pp->PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
            tlog("D3D9: forced WINDOWED (%s) (backbuffer stays %ux%u)",
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
        tlog("D3D9: device created -> Present+Reset hooked (limiter%s%s%s)",
             (g_up_w>0)?" + upscale":"", g_rotate?" + rotate":"", (g_sharpen>0)?" + sharpen":"");
        { HWND sp = FindWindowA("UltraLoaderSplash", NULL);
          if (sp) { ShowWindow(sp, SW_HIDE); tlog("D3D9: dismissed loader splash"); } }
        if (g_devwnd) {
            if (force_windowed() && g_up_w == 0) {
                int dw = GetSystemMetrics(SM_CXSCREEN);
                int dh = GetSystemMetrics(SM_CYSCREEN);
                SetWindowLongA(g_devwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                SetWindowPos(g_devwnd, HWND_TOPMOST, 0, 0, dw, dh,
                             SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                SetWindowPos(g_devwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE);
                tlog("D3D9: force-windowed borderless %dx%d (no upscale)", dw, dh);
            }
            SetForegroundWindow(g_devwnd);
        }
    }
    return hr;
}

/* Patch IDirect3D9::CreateDevice (slot 16) on the returned IDirect3D9. */
void ultra_d3d9_install_throttle_txp(void* d3d9) {
    if (!d3d9) return;
    patch_vtable_entry(d3d9, IDIRECT3D9_CREATEDEVICE, (void*)Hook_CreateDevice9, (void**)&Real_CreateDevice9);
}

/* Real Direct3DCreate9 hook. Called via the IAT patch / GetProcAddress
 * interception in jvs_serial_hook_txp.c. Resolves the real export, calls it,
 * then arms the throttle/dump on the returned IDirect3D9. */
typedef IDirect3D9* (__stdcall *pDirect3DCreate9)(UINT);
static pDirect3DCreate9 Real_Direct3DCreate9 = NULL;
void* __stdcall ultra_Hook_Direct3DCreate9_txp(UINT ver) {
    if (!Real_Direct3DCreate9) {
        HMODULE m = GetModuleHandleA("d3d9.dll");
        if (!m) m = LoadLibraryA("d3d9.dll");
        if (m) Real_Direct3DCreate9 = (pDirect3DCreate9)(void*)ultra_raw_export(m, "Direct3DCreate9");
    }
    IDirect3D9* r = Real_Direct3DCreate9 ? Real_Direct3DCreate9(ver) : NULL;
    tlog("D3D9: Direct3DCreate9(0x%X) -> %p %s", ver, (void*)r, r ? "OK" : "NULL(D3D9 init failed)");
    if (r) ultra_d3d9_install_throttle_txp(r);
    return r;
}

#endif /* _WIN32 */
