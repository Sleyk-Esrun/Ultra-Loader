/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  d3d8_throttle_rawthrills.c -- D3D8 present-hook for the Raw Thrills PC hook.
 * =============================================================================
 *
 *  ENGINE FACT (from rawthrills_pc_main.c's static analysis of sdaemon.exe /
 *  the TFAFSB dump): RawApp titles (The Fast and the Furious Super Bikes, Big
 *  Buck Hunter Pro/HD/Safari, Target Terror, ...) render with D3D8
 *  (Direct3DCreate8) plus cg.dll/cgD3D9.dll for movies/UI. This is the ONE
 *  present API confirmed against a real dump; board_profile.c's generic
 *  "BP_GFX_MIXED / D3D9+D3D11" classification is a family-level guess that
 *  rawthrills_pc_main.c's own header comment explicitly calls stale for this
 *  reason. The spawner already sets ULTRA_D3D8=1 unconditionally on every
 *  launch -- this file is what makes that flag do something.
 *
 *  Newer Raw Thrills cabs (Nitro, H2Overdrive era) are believed to move to
 *  D3D9/D3D11 -- see d3d9_throttle_rawthrills.c / d3d11_throttle_rawthrills.c,
 *  armed together via ULTRA_D3D11=auto. All three present-hooks are installed
 *  unconditionally by rawthrills_hook_main.c; each is a no-op IAT patch on any
 *  process that doesn't actually import its target DLL/function, so only the
 *  API the game's binary really links against ever fires (first/only-wins).
 *
 *  WHAT THIS FILE DOES (mirrors d3d9/d3d11's contract):
 *    - throttles Present to a target fps (default 60, ULTRA_FPS_CAP),
 *    - optional rotate / upscale-to-window (ULTRA_RES / ULTRA_ROTATE /
 *      ULTRA_ASPECT, same env contract as the D3D9 hook), default OFF,
 *    - optional back-buffer BMP dump (ULTRA_ROT_DUMP=dir) -- the offline
 *      verification path for this DUMP-GATED board: with no real cabinet to
 *      screenshot, this is how boot progress on a user-supplied dump gets
 *      confirmed,
 *    - force-windowed (ULTRA_SEGA_WINDOWED, or the universal ULTRA_WINDOWED /
 *      ULTRA_BORDERLESS) so a fullscreen-exclusive device isn't lost on a
 *      focus change under an unattended/injected launch.
 *
 *  NOT ported: the RCAS-style sharpen pass (ultra_sharpen_draw_quad) that the
 *  canonical d3d8_throttle.c pairs with d3d8_sharpen.c/h. That is a separate
 *  dependency this board's hook does not need for its core job (frame pacing +
 *  verification dump); adding it would pull in an extra file this pass did
 *  not ask for. ULTRA_SHARPEN is therefore not read here -- omitted rather
 *  than silently accepted-and-ignored, so an operator setting it gets no
 *  surprise no-op.
 *
 *  Clean-room: Ultra Loader's own code over the public COM/D3D8 ABI, adapted
 *  from this project's own D3D9 hook (see d3d9_throttle_rawthrills.c) to the
 *  D3D8 vtable shape. Self-contained: no include of, or link against, the
 *  shared native_x86 hook tree or any other board's module folder.
 *
 *  Env gate: ULTRA_D3D8=1 (set unconditionally by rawthrills_pc_main.c).
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

#include "rawthrills_hook.h"

/* COM vtable indices (verified against the IDirect3D8 / IDirect3DDevice8 ABI,
 * same as the canonical d3d8_throttle.c). */
#define IDIRECT3D8_CREATEDEVICE    15
#define IDIRECT3DDEVICE8_RESET     14
#define IDIRECT3DDEVICE8_PRESENT   15

typedef HRESULT (__stdcall *pCreateDevice8)(void* self, UINT adapter, int devtype, HWND hwnd,
                                            DWORD flags, void* pp, void** ppdev);
typedef HRESULT (__stdcall *pPresent8)(void* self, const RECT* src, const RECT* dst,
                                       HWND hwndOverride, const void* dirty);
typedef HRESULT (__stdcall *pReset8)(void* self, void* pp);

static pCreateDevice8 Real_CreateDevice8 = NULL;
static pPresent8      Real_Present8      = NULL;
static pReset8        Real_Reset8        = NULL;
static void**         g_patched_vtbl     = NULL;

/* Local log shim: rawthrills_hook.h's ul_rt_log() does NOT auto-append '\n'
 * (every other stub in this hook passes its own trailing \n); the ported
 * present-hook body below calls tlog(...) the same way the reference file
 * does (no trailing \n per call), so wrap it once here instead of editing
 * every call site. */
static void rt_d3d8_log(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    ul_rt_log("%s\n", buf);
}
#define tlog rt_d3d8_log

/* ---- upscaler / rotation state (mirrors d3d9_throttle_rawthrills.c) ------- */
static HWND g_devwnd     = NULL;
static int  g_up_w = 0, g_up_h = 0;
static int  g_upscaled   = 0;
static int  g_rotate     = 0;
static int  g_aspect_fill = 1;
static int  g_bb_w = 640, g_bb_h = 480;
static IDirect3DTexture8* g_tex     = NULL;
static IDirect3DTexture8* g_scratch = NULL;

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

/* force-windowed: ULTRA_SEGA_WINDOWED is the flag rawthrills_pc_main.c already
 * sets to "1" on every launch (see its header comment); the universal
 * ULTRA_WINDOWED / ULTRA_BORDERLESS knobs (loader settings_apply()) are
 * honored too so a user's global window-mode choice still applies. */
static int force_windowed(void) {
    const char* sw = getenv("ULTRA_SEGA_WINDOWED");
    if (sw && *sw) return (sw[0] == '1');
    const char* uw = getenv("ULTRA_WINDOWED");
    const char* bl = getenv("ULTRA_BORDERLESS");
    return (uw && *uw == '1') || (bl && *bl == '1');
}

/* ---- frame limiter (QPC) --------------------------------------------------- */
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
        tlog("D3D8(rt): frame limiter armed @ %.2f fps", g_target_fps);
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
        if (secs > 0.0) tlog("D3D8(rt): measured %.1f fps (cap %.0f)", frames / secs, g_target_fps);
        frames = 0;
    }
}

typedef struct { float x, y, z, rhw, u, v; } RVERT8;

/* BMP dump of a non-current-RT surface -- the offline verification path. */
static void dump_surface_bmp8(IDirect3DDevice8* dev, IDirect3DSurface8* surf, const char* path) {
    if (!surf) return;
    D3DSURFACE_DESC d; if (FAILED(IDirect3DSurface8_GetDesc(surf, &d))) { tlog("D3D8(rt) dump: GetDesc fail"); return; }
    IDirect3DSurface8* sys = NULL;
    HRESULT hc = IDirect3DDevice8_CreateImageSurface(dev, d.Width, d.Height, d.Format, &sys);
    if (FAILED(hc)) { tlog("D3D8(rt) dump: CreateImageSurface 0x%08lX", hc); return; }
    HRESULT hcr = IDirect3DDevice8_CopyRects(dev, surf, NULL, 0, sys, NULL);
    if (FAILED(hcr)) { tlog("D3D8(rt) dump: CopyRects->sys 0x%08lX", hcr); IDirect3DSurface8_Release(sys); return; }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(IDirect3DSurface8_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        int w = (int)d.Width, h = (int)d.Height;
        int rowsz = w * 3, pad = (4 - (rowsz & 3)) & 3, stride = rowsz + pad;
        int imgsz = stride * h;
        unsigned char fh[14] = {0}, ih[40] = {0};
        unsigned filesz = 14 + 40 + imgsz;
        fh[0]='B'; fh[1]='M'; *(unsigned*)(fh+2)=filesz; *(unsigned*)(fh+10)=54;
        *(unsigned*)(ih+0)=40; *(int*)(ih+4)=w; *(int*)(ih+8)=h;
        *(unsigned short*)(ih+12)=1; *(unsigned short*)(ih+14)=24; *(unsigned*)(ih+20)=imgsz;
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(fh,1,14,f); fwrite(ih,1,40,f);
            unsigned char* row = (unsigned char*)malloc(stride);
            for (int y = h-1; y >= 0; --y) {
                unsigned char* src = (unsigned char*)lr.pBits + y * lr.Pitch;
                memset(row, 0, stride);
                for (int x = 0; x < w; ++x) {
                    row[x*3+0] = src[x*4+0]; row[x*3+1] = src[x*4+1]; row[x*3+2] = src[x*4+2];
                }
                fwrite(row, 1, stride, f);
            }
            free(row); fclose(f);
        }
        IDirect3DSurface8_UnlockRect(sys);
        tlog("D3D8(rt): dumped %dx%d -> %s", w, h, path);
    }
    IDirect3DSurface8_Release(sys);
}

static int ensure_rot_resources8(IDirect3DDevice8* dev) {
    if (g_tex && g_scratch) return 1;
    if (!g_tex) {
        HRESULT hr = IDirect3DDevice8_CreateTexture(dev, g_bb_w, g_bb_h, 1, D3DUSAGE_RENDERTARGET,
                                                    D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_tex);
        if (FAILED(hr)) { tlog("D3D8(rt): CreateTexture(RT) failed 0x%08lX", hr); return 0; }
        IDirect3DSurface8* ts = NULL;
        if (SUCCEEDED(IDirect3DTexture8_GetSurfaceLevel(g_tex, 0, &ts)) && ts) {
            IDirect3DSurface8* prevRT = NULL; IDirect3DSurface8* prevDS = NULL;
            IDirect3DDevice8_GetRenderTarget(dev, &prevRT);
            IDirect3DDevice8_GetDepthStencilSurface(dev, &prevDS);
            if (SUCCEEDED(IDirect3DDevice8_SetRenderTarget(dev, ts, NULL)))
                IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
            if (prevRT) { IDirect3DDevice8_SetRenderTarget(dev, prevRT, prevDS); IDirect3DSurface8_Release(prevRT); }
            if (prevDS) IDirect3DSurface8_Release(prevDS);
            IDirect3DSurface8_Release(ts);
        }
    }
    if (!g_scratch) {
        HRESULT hr = IDirect3DDevice8_CreateTexture(dev, 64, 64, 1, D3DUSAGE_RENDERTARGET,
                                                    D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_scratch);
        if (FAILED(hr)) { tlog("D3D8(rt): CreateTexture(scratch) failed 0x%08lX", hr); return 0; }
    }
    tlog("D3D8(rt): rotation resources ready (rotate=%d out=%dx%d src=%dx%d)", g_rotate, g_up_w, g_up_h, g_bb_w, g_bb_h);
    return 1;
}

/* Rotate the finished frame in place inside the backbuffer (no sharpen pass --
 * see file header). State is saved/restored per-call rather than via a state
 * block (a state block per Present is a known driver-stall risk on D3D8). */
static int present_rotate8(IDirect3DDevice8* dev) {
    if (!ensure_rot_resources8(dev)) return 0;

    IDirect3DSurface8 *gameBB = NULL, *texSurf = NULL, *scratchSurf = NULL, *oldRT = NULL, *oldDS = NULL;
    HRESULT hrGBB = IDirect3DDevice8_GetBackBuffer(dev, 0, D3DBACKBUFFER_TYPE_MONO, &gameBB);
    HRESULT hrGSL = IDirect3DTexture8_GetSurfaceLevel(g_tex, 0, &texSurf);
    IDirect3DTexture8_GetSurfaceLevel(g_scratch, 0, &scratchSurf);
    HRESULT hrCR = E_FAIL, hrSRT = E_FAIL, hrBS = E_FAIL, hrDP = E_FAIL;

    IDirect3DDevice8_GetRenderTarget(dev, &oldRT);
    IDirect3DDevice8_GetDepthStencilSurface(dev, &oldDS);

    if (gameBB && texSurf && scratchSurf) {
        IDirect3DDevice8_SetRenderTarget(dev, scratchSurf, NULL);
        hrCR = IDirect3DDevice8_CopyRects(dev, gameBB, NULL, 0, texSurf, NULL);
    }

    if (gameBB && SUCCEEDED(hrSRT = IDirect3DDevice8_SetRenderTarget(dev, gameBB, NULL))) {
        DWORD svVS = 0, svPS = 0;
        IDirect3DBaseTexture8 *svTex0 = NULL, *svTex1 = NULL;
        DWORD rsLight=0, rsZ=0, rsZW=0, rsCull=0, rsAB=0, rsAT=0, rsFog=0, rsSten=0, rsCW=0;
        DWORD t0_cop=0,t0_ca1=0,t0_aop=0,t0_mag=0,t0_min=0,t0_au=0,t0_av=0,t0_tci=0,t0_ttf=0;
        DWORD t1_cop=0,t1_aop=0;
        IDirect3DDevice8_GetVertexShader(dev, &svVS);
        IDirect3DDevice8_GetPixelShader(dev, &svPS);
        IDirect3DDevice8_GetTexture(dev, 0, &svTex0);
        IDirect3DDevice8_GetTexture(dev, 1, &svTex1);
        IDirect3DDevice8_GetRenderState(dev, D3DRS_LIGHTING, &rsLight);
        IDirect3DDevice8_GetRenderState(dev, D3DRS_ZENABLE, &rsZ);
        IDirect3DDevice8_GetRenderState(dev, D3DRS_ZWRITEENABLE, &rsZW);
        IDirect3DDevice8_GetRenderState(dev, D3DRS_CULLMODE, &rsCull);
        IDirect3DDevice8_GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &rsAB);
        IDirect3DDevice8_GetRenderState(dev, D3DRS_ALPHATESTENABLE, &rsAT);
        IDirect3DDevice8_GetRenderState(dev, D3DRS_FOGENABLE, &rsFog);
        IDirect3DDevice8_GetRenderState(dev, D3DRS_STENCILENABLE, &rsSten);
        IDirect3DDevice8_GetRenderState(dev, D3DRS_COLORWRITEENABLE, &rsCW);
        IDirect3DDevice8_GetTextureStageState(dev, 0, D3DTSS_COLOROP, &t0_cop);
        IDirect3DDevice8_GetTextureStageState(dev, 0, D3DTSS_COLORARG1, &t0_ca1);
        IDirect3DDevice8_GetTextureStageState(dev, 0, D3DTSS_ALPHAOP, &t0_aop);
        IDirect3DDevice8_GetTextureStageState(dev, 0, D3DTSS_MAGFILTER, &t0_mag);
        IDirect3DDevice8_GetTextureStageState(dev, 0, D3DTSS_MINFILTER, &t0_min);
        IDirect3DDevice8_GetTextureStageState(dev, 0, D3DTSS_ADDRESSU, &t0_au);
        IDirect3DDevice8_GetTextureStageState(dev, 0, D3DTSS_ADDRESSV, &t0_av);
        IDirect3DDevice8_GetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, &t0_tci);
        IDirect3DDevice8_GetTextureStageState(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, &t0_ttf);
        IDirect3DDevice8_GetTextureStageState(dev, 1, D3DTSS_COLOROP, &t1_cop);
        IDirect3DDevice8_GetTextureStageState(dev, 1, D3DTSS_ALPHAOP, &t1_aop);

        if (SUCCEEDED(hrBS = IDirect3DDevice8_BeginScene(dev))) {
            IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
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
            RVERT8 q[4] = {
                { dx+o,    dy+o,    0, 1, u[0], v[0] },
                { dx+dw+o, dy+o,    0, 1, u[1], v[1] },
                { dx+dw+o, dy+dh+o, 0, 1, u[2], v[2] },
                { dx+o,    dy+dh+o, 0, 1, u[3], v[3] },
            };
            IDirect3DDevice8_SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
            IDirect3DDevice8_SetPixelShader(dev, 0);
            IDirect3DDevice8_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
            IDirect3DDevice8_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
            IDirect3DDevice8_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
            IDirect3DDevice8_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
            IDirect3DDevice8_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
            IDirect3DDevice8_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
            IDirect3DDevice8_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
            IDirect3DDevice8_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
            IDirect3DDevice8_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0x0000000F);
            IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8*)g_tex);
            IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
            IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE);
            IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
            IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
            IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ADDRESSU,  D3DTADDRESS_CLAMP);
            IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ADDRESSV,  D3DTADDRESS_CLAMP);
            IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, 0);
            IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            IDirect3DDevice8_SetTexture(dev, 1, NULL);
            IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            hrDP = IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 2, q, sizeof(RVERT8));
            IDirect3DDevice8_EndScene(dev);
        }
        IDirect3DDevice8_SetVertexShader(dev, svVS);
        IDirect3DDevice8_SetPixelShader(dev, svPS);
        IDirect3DDevice8_SetTexture(dev, 0, svTex0); if (svTex0) IDirect3DBaseTexture8_Release(svTex0);
        IDirect3DDevice8_SetTexture(dev, 1, svTex1); if (svTex1) IDirect3DBaseTexture8_Release(svTex1);
        IDirect3DDevice8_SetRenderState(dev, D3DRS_LIGHTING, rsLight);
        IDirect3DDevice8_SetRenderState(dev, D3DRS_ZENABLE, rsZ);
        IDirect3DDevice8_SetRenderState(dev, D3DRS_ZWRITEENABLE, rsZW);
        IDirect3DDevice8_SetRenderState(dev, D3DRS_CULLMODE, rsCull);
        IDirect3DDevice8_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, rsAB);
        IDirect3DDevice8_SetRenderState(dev, D3DRS_ALPHATESTENABLE, rsAT);
        IDirect3DDevice8_SetRenderState(dev, D3DRS_FOGENABLE, rsFog);
        IDirect3DDevice8_SetRenderState(dev, D3DRS_STENCILENABLE, rsSten);
        IDirect3DDevice8_SetRenderState(dev, D3DRS_COLORWRITEENABLE, rsCW);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLOROP, t0_cop);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, t0_ca1);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, t0_aop);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MAGFILTER, t0_mag);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MINFILTER, t0_min);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ADDRESSU, t0_au);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ADDRESSV, t0_av);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, t0_tci);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, t0_ttf);
        IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_COLOROP, t1_cop);
        IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, t1_aop);
        IDirect3DDevice8_SetRenderTarget(dev, oldRT, oldDS);
    } else if (gameBB) {
        IDirect3DDevice8_SetRenderTarget(dev, oldRT, oldDS);
    }
    if (oldRT)       IDirect3DSurface8_Release(oldRT);
    if (oldDS)       IDirect3DSurface8_Release(oldDS);
    if (texSurf)     IDirect3DSurface8_Release(texSurf);
    if (scratchSurf) IDirect3DSurface8_Release(scratchSurf);
    if (gameBB)      IDirect3DSurface8_Release(gameBB);

    static int diag = 0;
    if (!diag) { diag = 1;
        tlog("D3D8(rt) ROT DIAG: GetBB=0x%08lX GetSurf=0x%08lX CopyRects=0x%08lX SetRT=0x%08lX BeginScene=0x%08lX DrawPrim=0x%08lX",
             hrGBB, hrGSL, hrCR, hrSRT, hrBS, hrDP);
    }
    return (hrSRT == D3D_OK);
}

static WNDPROC g_orig_wndproc8 = NULL;
static LRESULT CALLBACK ultra_output_wndproc8(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if (g_orig_wndproc8) return CallWindowProcA(g_orig_wndproc8, h, msg, wp, lp);
    return DefWindowProcA(h, msg, wp, lp);
}
static void resize_output_window8(HWND w) {
    if (!w || g_upscaled) return;
    SetClassLongPtrA(w, GCLP_HBRBACKGROUND, (LONG_PTR)GetStockObject(BLACK_BRUSH));
    if ((WNDPROC)(LONG_PTR)GetWindowLongPtrA(w, GWLP_WNDPROC) != ultra_output_wndproc8)
        g_orig_wndproc8 = (WNDPROC)(LONG_PTR)SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)ultra_output_wndproc8);
    RECT wr = { 0, 0, g_up_w, g_up_h };
    SetWindowLongA(w, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    AdjustWindowRect(&wr, WS_POPUP, FALSE);
    SetWindowPos(w, HWND_TOP, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                 SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    InvalidateRect(w, NULL, TRUE);
    g_upscaled = 1;
    tlog("D3D8(rt): output window -> %dx%d%s (black bg, erase swallowed)", g_up_w, g_up_h, g_rotate ? " (rotated)" : "");
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

static HRESULT __stdcall Hook_Present8(void* self, const RECT* src, const RECT* dst,
                                       HWND hwndOverride, const void* dirty) {
    throttle();
    IDirect3DDevice8* dev = (IDirect3DDevice8*)self;
    if ((g_up_w > 0 || g_rotate) && !g_upscaled)
        resize_output_window8(g_devwnd ? g_devwnd : hwndOverride);

    if (g_rotate) present_rotate8(dev);

    if (!g_dump_dir_read) { g_dump_dir = getenv("ULTRA_ROT_DUMP"); g_dump_dir_read = 1; }
    if (g_dump_dir && *g_dump_dir) {
        ++g_present_count;
        if (dump_due(g_present_count)) {
            IDirect3DSurface8* bb = NULL;
            if (SUCCEEDED(IDirect3DDevice8_GetBackBuffer(dev, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
                char p[512];
                snprintf(p, sizeof p, "%s/d3d8_%04d.bmp", g_dump_dir, g_present_count);
                dump_surface_bmp8(dev, bb, p);
                IDirect3DSurface8_Release(bb);
            }
        }
    }

    if (g_up_w > 0 || g_rotate)
        return Real_Present8(self, NULL, NULL, hwndOverride, NULL);
    return Real_Present8(self, src, dst, hwndOverride, dirty);
}

static int postprocess_active8(void) {
    return (g_up_w > 0) || g_rotate;
}

static HRESULT __stdcall Hook_Reset8(void* self, void* pp) {
    if (g_tex)     { IDirect3DTexture8_Release(g_tex);     g_tex = NULL; }
    if (g_scratch) { IDirect3DTexture8_Release(g_scratch); g_scratch = NULL; }
    HRESULT hr = Real_Reset8 ? Real_Reset8(self, pp) : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr) && postprocess_active8()) g_upscaled = 0;
    tlog("D3D8(rt): Reset -> 0x%08lX", hr);
    return hr;
}

static void patch_vtable_entry8(void* obj, int index, void* hook, void** out_real) {
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
    read_target_res();
    if (pp) {
        unsigned* p = (unsigned*)pp;
        g_devwnd = (HWND)(uintptr_t)p[6];
        if (!g_devwnd) g_devwnd = hwnd;
        if (p[0] > 0 && p[1] > 0) { g_bb_w = (int)p[0]; g_bb_h = (int)p[1]; }
        tlog("D3D8(rt): CreateDevice bb=%ux%u fmt=%u swapeffect=%u hwnd=%08X windowed=%u ms=%u flags=%08X",
             p[0], p[1], p[2], p[5], p[6], p[7], p[4], p[10]);
        int want_windowed = (g_up_w > 0) || force_windowed();
        if (want_windowed && !p[7]) {
            p[7]  = 1;    /* Windowed = TRUE */
            p[2]  = 22;   /* BackBufferFormat = D3DFMT_X8R8G8B8 */
            p[11] = 0;    /* FullScreen_RefreshRateInHz = 0 (must be, when windowed) */
            p[12] = 0;    /* PresentationInterval = DEFAULT */
            tlog("D3D8(rt): forced WINDOWED (%s, backbuffer stays %ux%u)",
                 g_up_w > 0 ? "upscale" : "auto/user", p[0], p[1]);
        }
    } else if (!g_devwnd) {
        g_devwnd = hwnd;
    }
    HRESULT hr = Real_CreateDevice8(self, adapter, devtype, hwnd, flags, pp, ppdev);
    if (hr >= 0 && ppdev && *ppdev) {
        void** vtbl = *(void***)*ppdev;
        if (vtbl != g_patched_vtbl) {
            patch_vtable_entry8(*ppdev, IDIRECT3DDEVICE8_PRESENT, (void*)Hook_Present8, (void**)&Real_Present8);
            if (postprocess_active8())
                patch_vtable_entry8(*ppdev, IDIRECT3DDEVICE8_RESET, (void*)Hook_Reset8, (void**)&Real_Reset8);
            g_patched_vtbl = vtbl;
            tlog("D3D8(rt): device created -> Present%s hooked (limiter%s)",
                 postprocess_active8() ? "+Reset" : "", (g_up_w>0)?" + upscale":"");
            HWND sp = FindWindowA("UltraLoaderSplash", NULL);
            if (sp) { ShowWindow(sp, SW_HIDE); tlog("D3D8(rt): dismissed loader splash"); }
        }
    }
    return hr;
}

/* Direct3DCreate8 IAT hook: forward to the real export, then patch its
 * CreateDevice so we can reach the device's Present. */
typedef IDirect3D8* (__stdcall *pDirect3DCreate8)(UINT);
static pDirect3DCreate8 Real_Direct3DCreate8 = NULL;
static void* __stdcall Hook_Direct3DCreate8(UINT ver) {
    if (!Real_Direct3DCreate8) {
        HMODULE m = GetModuleHandleA("d3d8.dll");
        if (!m) m = LoadLibraryA("d3d8.dll");
        if (m) Real_Direct3DCreate8 = (pDirect3DCreate8)(void*)GetProcAddress(m, "Direct3DCreate8");
    }
    IDirect3D8* r = Real_Direct3DCreate8 ? Real_Direct3DCreate8(ver) : NULL;
    tlog("D3D8(rt): Direct3DCreate8(0x%X) -> %p %s", ver, (void*)r, r ? "OK" : "NULL(D3D8 init failed)");
    if (r) patch_vtable_entry8(r, IDIRECT3D8_CREATEDEVICE, (void*)Hook_CreateDevice8, (void**)&Real_CreateDevice8);
    return r;
}

void ul_rt_d3d8_install(void) {
    if (!ul_rt_env_on("ULTRA_D3D8")) {
        ul_rt_log("d3d8: skipped (ULTRA_D3D8!=1)\n");
        return;
    }
    ul_rt_iat_patch_all("d3d8.dll", "Direct3DCreate8", (void*)Hook_Direct3DCreate8);
    ul_rt_log("d3d8: armed (present-hook: limiter + rotate/upscale + BMP dump + force-windowed)\n");
}

/* Current device/render window, or NULL if CreateDevice hasn't fired yet.
 * Added for Ultra Omni-Bridge (omni_bridge.c) -- its window-getter adapter in
 * rawthrills_hook_main.c falls back across the D3D8/D3D9/D3D11 siblings since
 * only whichever API the title actually links against ever sets its window. */
HWND ul_rt_d3d8_window(void) { return g_devwnd; }

#endif /* _WIN32 */
