/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  d3d8_throttle.c -- frame limiter for native Taito Type X games.
 * =============================================================================
 *
 *  These games pace their logic off the D3D8 Present (the original cabinet's
 *  60Hz vsync). On a PC -- especially WINDOWED, where D3D8 cannot vsync, and on
 *  high-refresh monitors -- Present returns immediately, so the whole game loop
 *  runs far over 60fps and everything looks fast-forwarded. We hook
 *  IDirect3D8::CreateDevice -> IDirect3DDevice8::Present and throttle Present to
 *  a target frame rate (default 60, override ULTRA_FPS_CAP). A QPC-based limiter
 *  is monitor/refresh/windowing independent, which vsync-forcing is not.
 *
 *  Clean-room: Ultra Loader's own code over the public COM/D3D8 ABI.
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

#include "d3d8_sharpen.h"

/* COM vtable indices (verified against the IDirect3D8 / IDirect3DDevice8 ABI). */
#define IDIRECT3D8_CREATEDEVICE    15
#define IDIRECT3DDEVICE8_RESET     14
#define IDIRECT3DDEVICE8_PRESENT   15

typedef HRESULT (__stdcall *pCreateDevice)(void* self, UINT adapter, int devtype, HWND hwnd,
                                           DWORD flags, void* pp, void** ppdev);
typedef HRESULT (__stdcall *pPresent)(void* self, const RECT* src, const RECT* dst,
                                      HWND hwndOverride, const void* dirty);
typedef HRESULT (__stdcall *pReset8)(void* self, void* pp);

static pCreateDevice Real_CreateDevice = NULL;
static pPresent      Real_Present      = NULL;
static pReset8       Real_Reset8       = NULL;
static int           g_present_hooked  = 0;
static void**        g_patched_vtbl    = NULL;   /* the device vtable we patched   */

/* ---- upscaler / rotation state -------------------------------------------- */
static HWND g_devwnd   = NULL;   /* the game's D3D8 device window               */
static int  g_up_w = 0, g_up_h = 0;   /* target client size (0 = no upscale)    */
static int  g_upscaled = 0;      /* window resized yet                          */
static int  g_rotate   = 0;      /* 0 / 90 / 180 / 270 (universal screen rotate)*/
static int  g_aspect_fill = 1;   /* 1 = stretch to window, 0 = keep aspect (pillarbox) */
static int  g_bb_w = 640, g_bb_h = 480;  /* game backbuffer size (capture src)  */
static float g_sharpen = 0.0f;   /* ULTRA_SHARPEN in [0,1] (0 = off; RCAS-style) */
static int   g_aa      = 0;      /* ULTRA_AA (edge smoothing; shared GL toggle)   */
/* rotation render resources (created lazily on the device) */
static IDirect3DTexture8*   g_tex     = NULL;   /* holds the captured game frame   */
static IDirect3DTexture8*   g_scratch = NULL;   /* tiny RT parked to free backbuffer */

static void read_target_res(void) {
    /* ULTRA_RES = "1080p" | "1440p" | "4k" | "WxH"; or ULTRA_RES_W / ULTRA_RES_H */
    const char* r = getenv("ULTRA_RES");
    if (r && *r) {
        if      (!strcmp(r, "1080p")) { g_up_w = 1920; g_up_h = 1080; }
        else if (!strcmp(r, "1440p")) { g_up_w = 2560; g_up_h = 1440; }
        else if (!strcmp(r, "4k") || !strcmp(r, "2160p")) { g_up_w = 3840; g_up_h = 2160; }
        else { int w=0,h=0; if (sscanf(r, "%dx%d", &w, &h) == 2 && w>0 && h>0) { g_up_w=w; g_up_h=h; } }
    }
    const char* w = getenv("ULTRA_RES_W"); const char* h = getenv("ULTRA_RES_H");
    if (w && *w && h && *h) { int wi=atoi(w), hi=atoi(h); if (wi>0&&hi>0){ g_up_w=wi; g_up_h=hi; } }
    /* universal screen rotation: ULTRA_ROTATE = 90 | 180 | 270 (0 = off) */
    const char* rot = getenv("ULTRA_ROTATE");
    if (rot && *rot) { int v = atoi(rot); if (v==90||v==180||v==270) g_rotate = v; }
    /* aspect: ULTRA_ASPECT = fill/stretch | keep/pillarbox.
     *   non-rotated default = fill (16:9 games already match the screen)
     *   ROTATED default     = keep/pillarbox - a portrait TATE image stretched to
     *     fill 16:9 is ~3x too wide (the "looks horrible" distortion); show it at
     *     correct aspect, centered, with side bars. Explicit fill/stretch overrides. */
    const char* asp = getenv("ULTRA_ASPECT");
    char ac = (asp && *asp) ? asp[0] : 0;
    if (ac=='k'||ac=='K'||ac=='p'||ac=='P') g_aspect_fill = 0;        /* keep/pillarbox */
    else if (ac=='f'||ac=='F'||ac=='s'||ac=='S') g_aspect_fill = 1;   /* fill/stretch  */
    else if (g_rotate) g_aspect_fill = 0;                            /* TATE default = pillarbox */
    /* rotation needs an output window size; default to 1080p if none given */
    if (g_rotate && g_up_w == 0) { g_up_w = 1920; g_up_h = 1080; }
    /* sharpen/AA post-process: ULTRA_SHARPEN in [0,1], ULTRA_AA 0/1. Both
     * !getenv-style (only set if the env exists) so an unset env = OFF and the
     * native present path stays byte-identical. The sharpen runs at backbuffer
     * resolution inside the game's device, BEFORE the windowed stretch. */
    const char* shp = getenv("ULTRA_SHARPEN");
    if (shp && *shp) { float v = (float)atof(shp); if (v<0) v=0; if (v>1) v=1; g_sharpen = v; }
    const char* aa = getenv("ULTRA_AA");
    if (aa && *aa) g_aa = atoi(aa);
}

static double        g_target_fps = 60.0;
static LARGE_INTEGER g_freq, g_last;
static int           g_timing_init = 0;
static int           g_timeperiod_set = 0;   /* timeBeginPeriod(1) raised? */

/* Shared logger (gated by ULTRA_JVS_LOG). Non-static so d3d8_sharpen.c can use
 * the same log file via the ultra_d3d8_log() extern. tlog() is the in-file name. */
void ultra_d3d8_log(const char* fmt, ...) {
    const char* p = getenv("ULTRA_JVS_LOG"); if (!p || !*p) return;
    FILE* f = fopen(p, "a"); if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}
static void tlog(const char* fmt, ...) {
    const char* p = getenv("ULTRA_JVS_LOG"); if (!p || !*p) return;
    FILE* f = fopen(p, "a"); if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

/* Pace to g_target_fps using QueryPerformanceCounter: coarse-Sleep most of the
 * wait (1ms timer granularity), spin the last ~1ms for accuracy. */
static void throttle(void) {
    if (!g_timing_init) {
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_last);
        const char* e = getenv("ULTRA_FPS_CAP");
        if (e && *e) { double v = atof(e); if (v >= 1.0 && v <= 1000.0) g_target_fps = v; }
        timeBeginPeriod(1); g_timeperiod_set = 1;
        g_timing_init = 1;
        tlog("D3D8: frame limiter armed @ %.2f fps", g_target_fps);
        return;                                   /* first Present: set baseline only */
    }
    const double target = (double)g_freq.QuadPart / g_target_fps;   /* ticks per frame */
    LARGE_INTEGER now;
    for (;;) {
        QueryPerformanceCounter(&now);
        const double elapsed = (double)(now.QuadPart - g_last.QuadPart);
        if (elapsed >= target) break;
        const double remain_ms = (target - elapsed) * 1000.0 / (double)g_freq.QuadPart;
        if (remain_ms > 3.0) Sleep((DWORD)(remain_ms - 2.0));       /* coarse (1ms timer) */
        else YieldProcessor();                                      /* spin the final ~3ms, accurate */
    }
    g_last.QuadPart += (LONGLONG)target;                            /* fixed cadence (no drift) */
    if ((double)(now.QuadPart - g_last.QuadPart) > target * 4.0) g_last = now;  /* resync after a stall */

    /* measured-FPS heartbeat (verify the cap is working) */
    static LARGE_INTEGER mark; static int frames = 0;
    if (frames == 0) mark = now;
    if (++frames >= 180) {
        double secs = (double)(now.QuadPart - mark.QuadPart) / (double)g_freq.QuadPart;
        if (secs > 0.0) tlog("D3D8: measured %.1f fps (cap %.0f)", frames / secs, g_target_fps);
        frames = 0;
    }
}

/* Vertex for a screen-space textured quad (pre-transformed). */
typedef struct { float x, y, z, rhw, u, v; } RVERT;

/* DIAG: copy a (non-current-RT) surface to a lockable sysmem surface and write a
 * 32bpp BMP. Lets us see the exact orientation of a surface, free of screenshot
 * timing/window flakiness. surf must NOT be the device's current render target. */
static void dump_surface_bmp(IDirect3DDevice8* dev, IDirect3DSurface8* surf, const char* path) {
    if (!surf) return;
    D3DSURFACE_DESC d; if (FAILED(IDirect3DSurface8_GetDesc(surf, &d))) { tlog("D3D8 dump: GetDesc fail"); return; }
    IDirect3DSurface8* sys = NULL;
    HRESULT hc = IDirect3DDevice8_CreateImageSurface(dev, d.Width, d.Height, d.Format, &sys);
    if (FAILED(hc)) { tlog("D3D8 dump: CreateImageSurface 0x%08lX", hc); return; }
    HRESULT hcr = IDirect3DDevice8_CopyRects(dev, surf, NULL, 0, sys, NULL);
    if (FAILED(hcr)) { tlog("D3D8 dump: CopyRects->sys 0x%08lX", hcr); IDirect3DSurface8_Release(sys); return; }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(IDirect3DSurface8_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        int w = (int)d.Width, h = (int)d.Height;
        int rowsz = w * 3, pad = (4 - (rowsz & 3)) & 3, stride = rowsz + pad;
        int imgsz = stride * h;
        unsigned char fh[14] = {0}, ih[40] = {0};
        unsigned filesz = 14 + 40 + imgsz;
        fh[0]='B'; fh[1]='M'; *(unsigned*)(fh+2)=filesz; *(unsigned*)(fh+10)=54;
        *(unsigned*)(ih+0)=40; *(int*)(ih+4)=w; *(int*)(ih+8)=h; /* +h = bottom-up */
        *(unsigned short*)(ih+12)=1; *(unsigned short*)(ih+14)=24; *(unsigned*)(ih+20)=imgsz;
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(fh,1,14,f); fwrite(ih,1,40,f);
            unsigned char* row = (unsigned char*)malloc(stride);
            for (int y = h-1; y >= 0; --y) {     /* BMP is bottom-up */
                unsigned char* src = (unsigned char*)lr.pBits + y * lr.Pitch;
                memset(row, 0, stride);
                for (int x = 0; x < w; ++x) {     /* BGRA -> BGR */
                    row[x*3+0] = src[x*4+0]; row[x*3+1] = src[x*4+1]; row[x*3+2] = src[x*4+2];
                }
                fwrite(row, 1, stride, f);
            }
            free(row); fclose(f);
        }
        IDirect3DSurface8_UnlockRect(sys);
        tlog("D3D8: dumped %dx%d -> %s", w, h, path);
    }
    IDirect3DSurface8_Release(sys);
}

static int ensure_rot_resources(IDirect3DDevice8* dev) {
    if (g_tex && g_scratch) return 1;
    if (!g_tex) {
        HRESULT hr = IDirect3DDevice8_CreateTexture(dev, g_bb_w, g_bb_h, 1, D3DUSAGE_RENDERTARGET,
                                                    D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_tex);
        if (FAILED(hr)) { tlog("D3D8: CreateTexture(RT) failed 0x%08lX", hr); return 0; }
        /* clear it to opaque black once: a render-target texture's initial contents
         * are driver-defined (white on some), so a frame where the capture fails
         * would otherwise sample white. */
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
        /* a small render target we can bind so the backbuffer is no longer the
         * device's current RT -- CopyRects refuses a source that is the live RT. */
        HRESULT hr = IDirect3DDevice8_CreateTexture(dev, 64, 64, 1, D3DUSAGE_RENDERTARGET,
                                                    D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_scratch);
        if (FAILED(hr)) { tlog("D3D8: CreateTexture(scratch) failed 0x%08lX", hr); return 0; }
    }
    tlog("D3D8: rotation resources ready (rotate=%d out=%dx%d src=%dx%d)", g_rotate, g_up_w, g_up_h, g_bb_w, g_bb_h);
    return 1;
}

/* Post-process the finished game frame IN PLACE inside its own backbuffer, then
 * let the game's own windowed Present stretch that backbuffer to the window. We
 * render back into the implicit backbuffer (not an additional swapchain) because
 * in windowed D3D8 only the implicit swapchain reaches the window.
 *
 *   mode bit PP_ROTATE  -> rotate the frame (rotation UVs, unchanged behavior)
 *   mode bit PP_SHARPEN -> apply the RCAS-style sharpen kernel to the quad
 * The two fuse: a sharpen+rotate run samples the sharpen kernel inside the rotated
 * quad (one capture, one draw -- never double-captures). mode==PP_ROTATE alone is
 * byte-identical to the previous present_rotated().
 *
 * Returns 1 if the backbuffer now holds the processed frame (caller Real_Presents). */
#define PP_ROTATE   1
#define PP_SHARPEN  2
static int present_postprocess(IDirect3DDevice8* dev, int mode) {
    if (!ensure_rot_resources(dev)) return 0;
    const int do_sharpen = (mode & PP_SHARPEN) != 0;

    static int diag = 0;   /* one-shot per-call HRESULT dump */
    /* DIAG dump frames (ULTRA_ROT_DUMP=dir): sample several frames so we reliably
     * catch a content frame (attract loops through black transitions). */
    static int s_pf = 0; const int s_dumpN = ++s_pf;
    static const char* s_dd = NULL; static int s_dd_read = 0;
    if (!s_dd_read) { s_dd = getenv("ULTRA_ROT_DUMP"); s_dd_read = 1; }   /* read once */
    const int s_do_dump = (s_dd && *s_dd) &&
        (s_dumpN==700||s_dumpN==1100||s_dumpN==1500||s_dumpN==1900||s_dumpN==2300||s_dumpN==2700);
    IDirect3DSurface8 *gameBB = NULL, *texSurf = NULL, *scratchSurf = NULL, *oldRT = NULL, *oldDS = NULL;
    HRESULT hrGBB = IDirect3DDevice8_GetBackBuffer(dev, 0, D3DBACKBUFFER_TYPE_MONO, &gameBB);
    HRESULT hrGSL = IDirect3DTexture8_GetSurfaceLevel(g_tex, 0, &texSurf);
    IDirect3DTexture8_GetSurfaceLevel(g_scratch, 0, &scratchSurf);
    HRESULT hrCR = E_FAIL, hrSRT = E_FAIL, hrBS = E_FAIL, hrDP = E_FAIL;

    IDirect3DDevice8_GetRenderTarget(dev, &oldRT);
    IDirect3DDevice8_GetDepthStencilSurface(dev, &oldDS);

    /* 1) snapshot the game's finished frame into our texture. CopyRects rejects a
     * source that is the device's current render target, and the backbuffer IS
     * the live RT at Present time -- so park the RT on the scratch surface first. */
    if (gameBB && texSurf && scratchSurf) {
        IDirect3DDevice8_SetRenderTarget(dev, scratchSurf, NULL);   /* free the backbuffer */
        hrCR = IDirect3DDevice8_CopyRects(dev, gameBB, NULL, 0, texSurf, NULL);
        /* DIAG: dump the RAW captured frame (sideways, pre-rotate = native render). */
        if (s_do_dump) {
            char p[512];
            snprintf(p, sizeof p, "%s/dump_raw_%04d.bmp", s_dd, s_dumpN);
            dump_surface_bmp(dev, texSurf, p);
        }
    }

    /* 2) draw the rotated quad back over the same backbuffer */
    if (gameBB && SUCCEEDED(hrSRT = IDirect3DDevice8_SetRenderTarget(dev, gameBB, NULL))) {
        /* Save the exact device state we are about to clobber so the game's NEXT
         * frame inherits what it left (D3D8 state is sticky; a 3D title would
         * otherwise corrupt - depth off, our quad texture still bound, etc.). We
         * save/restore individual states rather than a D3DSBT_ALL state block:
         * per-frame CreateStateBlock leaks/stalls the driver and hung the game. */
        DWORD svVS = 0, svPS = 0;
        IDirect3DBaseTexture8 *svTex0 = NULL, *svTex1 = NULL;
        DWORD rsLight=0, rsZ=0, rsZW=0, rsCull=0, rsAB=0, rsAT=0, rsFog=0, rsSten=0, rsCW=0;
        DWORD t0_cop=0,t0_ca1=0,t0_aop=0,t0_mag=0,t0_min=0,t0_au=0,t0_av=0,t0_tci=0,t0_ttf=0;
        DWORD t1_cop=0,t1_aop=0;
        /* extra states the sharpen path may touch (so a 3D title's next frame is
         * not corrupted): blend factors, texture-factor, stage-0 col/alpha arg2/1.
         * Always saved/restored even when sharpen is off -- cheap, keeps it simple. */
        DWORD rsSrcB=0, rsDstB=0, rsTF=0, t0_ca2=0, t0_aa1=0;
        IDirect3DBaseTexture8 *svTex2=NULL,*svTex3=NULL,*svTex4=NULL;
        DWORD t2_cop=0,t3_cop=0,t4_cop=0;
        /* sampler states the ps_1.4 cross overwrites on stages 1..4 (the st=0..4
         * loop in ultra_sharpen_draw_quad sets TEXCOORDINDEX + MAG/MINFILTER +
         * ADDRESSU/V on EVERY stage). Stage 0's copies are saved above (t0_*);
         * stages 1..4 must be saved here too or they leak into the game's frame. */
        DWORD sh_tci[5]={0}, sh_mag[5]={0}, sh_min[5]={0}, sh_au[5]={0}, sh_av[5]={0};
        float sh_psc0[4]={0,0,0,0}; int sh_psc0_ok=0;  /* ps const c0 (sharpen gain) */
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
        /* extra saves for the sharpen path (harmless when sharpen is off) */
        IDirect3DDevice8_GetRenderState(dev, D3DRS_SRCBLEND, &rsSrcB);
        IDirect3DDevice8_GetRenderState(dev, D3DRS_DESTBLEND, &rsDstB);
        IDirect3DDevice8_GetRenderState(dev, D3DRS_TEXTUREFACTOR, &rsTF);
        IDirect3DDevice8_GetTextureStageState(dev, 0, D3DTSS_COLORARG2, &t0_ca2);
        IDirect3DDevice8_GetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, &t0_aa1);
        if (do_sharpen) {   /* the ps_1.4 cross binds g_tex on stages 1..4 too */
            IDirect3DDevice8_GetTexture(dev, 2, &svTex2);
            IDirect3DDevice8_GetTexture(dev, 3, &svTex3);
            IDirect3DDevice8_GetTexture(dev, 4, &svTex4);
            IDirect3DDevice8_GetTextureStageState(dev, 2, D3DTSS_COLOROP, &t2_cop);
            IDirect3DDevice8_GetTextureStageState(dev, 3, D3DTSS_COLOROP, &t3_cop);
            IDirect3DDevice8_GetTextureStageState(dev, 4, D3DTSS_COLOROP, &t4_cop);
            for (DWORD st = 1; st < 5; ++st) {   /* stages 1..4 sampler states */
                IDirect3DDevice8_GetTextureStageState(dev, st, D3DTSS_TEXCOORDINDEX, &sh_tci[st]);
                IDirect3DDevice8_GetTextureStageState(dev, st, D3DTSS_MAGFILTER, &sh_mag[st]);
                IDirect3DDevice8_GetTextureStageState(dev, st, D3DTSS_MINFILTER, &sh_min[st]);
                IDirect3DDevice8_GetTextureStageState(dev, st, D3DTSS_ADDRESSU, &sh_au[st]);
                IDirect3DDevice8_GetTextureStageState(dev, st, D3DTSS_ADDRESSV, &sh_av[st]);
            }
            /* ps_1.4 path overwrites pixel-shader constant register c0 with the gain */
            sh_psc0_ok = SUCCEEDED(IDirect3DDevice8_GetPixelShaderConstant(dev, 0, sh_psc0, 1));
        }
        /* Clear to black only AFTER BeginScene succeeds: if BeginScene transiently
         * fails (device not ready / already in a scene), clearing first would leave
         * an all-black backbuffer that Present stretches as a black flash. Keeping
         * the Clear inside the success block leaves the game's own rendered frame
         * intact, so a failed post-process frame shows the real image, not black. */
        if (SUCCEEDED(hrBS = IDirect3DDevice8_BeginScene(dev))) {
            IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
            /* dest rect in BACKBUFFER space (g_bb_w x g_bb_h); the windowed Present
             * then stretches the whole backbuffer to the g_up_w x g_up_h window. */
            float W = (float)g_bb_w, H = (float)g_bb_h, dx = 0, dy = 0, dw = W, dh = H;
            if (!g_aspect_fill) {
                /* keep aspect against the FINAL window: portrait fit, pillarboxed.
                 * Work in window space, then convert back to backbuffer space. */
                float WW = (float)g_up_w, WH = (float)g_up_h;
                float aw = (g_rotate == 90 || g_rotate == 270) ? (float)g_bb_h : (float)g_bb_w;
                float ah = (g_rotate == 90 || g_rotate == 270) ? (float)g_bb_w : (float)g_bb_h;
                float s = (WW/aw < WH/ah) ? WW/aw : WH/ah;
                float fw = aw * s, fh = ah * s;             /* fitted size in window px */
                float fx = (WW - fw) * 0.5f, fy = (WH - fh) * 0.5f;
                dx = fx / WW * W; dy = fy / WH * H;          /* back to backbuffer px */
                dw = fw / WW * W; dh = fh / WH * H;
            }
            /* UVs per rotation, corner order TL,TR,BR,BL */
            float u[4], v[4];
            switch (g_rotate) {
                case 90:  u[0]=0;v[0]=1; u[1]=0;v[1]=0; u[2]=1;v[2]=0; u[3]=1;v[3]=1; break;
                case 180: u[0]=1;v[0]=1; u[1]=0;v[1]=1; u[2]=0;v[2]=0; u[3]=1;v[3]=0; break;
                case 270: u[0]=1;v[0]=0; u[1]=1;v[1]=1; u[2]=0;v[2]=1; u[3]=0;v[3]=0; break;
                default:  u[0]=0;v[0]=0; u[1]=1;v[1]=0; u[2]=1;v[2]=1; u[3]=0;v[3]=1; break;
            }
            const float o = -0.5f;  /* texel-center alignment */
            RVERT q[4] = {
                { dx+o,      dy+o,      0, 1, u[0], v[0] },   /* TL */
                { dx+dw+o,   dy+o,      0, 1, u[1], v[1] },   /* TR */
                { dx+dw+o,   dy+dh+o,   0, 1, u[2], v[2] },   /* BR */
                { dx+o,      dy+dh+o,   0, 1, u[3], v[3] },   /* BL */
            };
            /* Neutralize whatever pipeline state the game left bound, or our
             * fixed-function UVs get ignored (a lingering pixel shader or texture
             * transform would just resample the frame unrotated). */
            IDirect3DDevice8_SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
            IDirect3DDevice8_SetPixelShader(dev, 0);            /* drop game's pixel shader */
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
            /* kill any second texture stage the game had enabled */
            IDirect3DDevice8_SetTexture(dev, 1, NULL);
            IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            /* PASS 1 (always): lay down the resampled (rotated/identity) frame.
             * This is the base 'center' image. The fixed-function sharpen fallback
             * blends a contrast-lift onto it; the ps_1.4 path overwrites it. */
            hrDP = IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 2, q, sizeof(RVERT));
            /* PASS 2 (optional): RCAS-style sharpen, sampling g_tex with the SAME
             * rotation UVs so sharpen fuses with rotation in one capture. texel
             * size is one source texel in UV units (1/bb_w, 1/bb_h). */
            if (do_sharpen) {
                ultra_sharpen_draw_quad(dev, g_tex, dx, dy, dw, dh, u, v,
                                        1.0f / (float)g_bb_w, 1.0f / (float)g_bb_h);
                /* the sharpen path may have touched stage 0 col/alpha ops, texcoord
                 * index and the vertex shader / blend; the outer save/restore below
                 * restores all of those to the game's values, so no cleanup here. */
            }
            IDirect3DDevice8_EndScene(dev);
        }
        /* DIAG: dump the FINAL processed backbuffer (what Present stretches to the
         * screen) -- includes rotation AND the post-sharpen pass, so this is the
         * offline proof of the sharpen kernel + orientation. Park RT on scratch to
         * free gameBB, dump, then restore. Filename notes which passes ran. */
        if (s_do_dump && scratchSurf) {
            IDirect3DDevice8_SetRenderTarget(dev, scratchSurf, NULL);
            char p[512];
            snprintf(p, sizeof p, "%s/dump_out_%s%s_%04d.bmp", s_dd,
                     (mode & PP_ROTATE) ? "rot" : "id",
                     do_sharpen ? "_sharp" : "", s_dumpN);
            dump_surface_bmp(dev, gameBB, p);
        }
        /* restore the game's exact render/stage/shader/texture state, then its RT.
         * GetTexture AddRef'd svTex0/1; SetTexture AddRefs again, so Release ours. */
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
        /* restore the extra states the sharpen path may have changed */
        IDirect3DDevice8_SetRenderState(dev, D3DRS_SRCBLEND, rsSrcB);
        IDirect3DDevice8_SetRenderState(dev, D3DRS_DESTBLEND, rsDstB);
        IDirect3DDevice8_SetRenderState(dev, D3DRS_TEXTUREFACTOR, rsTF);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, t0_ca2);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, t0_aa1);
        if (do_sharpen) {   /* unbind + restore stages 1..4 used by the ps_1.4 cross */
            IDirect3DDevice8_SetTexture(dev, 2, svTex2); if (svTex2) IDirect3DBaseTexture8_Release(svTex2);
            IDirect3DDevice8_SetTexture(dev, 3, svTex3); if (svTex3) IDirect3DBaseTexture8_Release(svTex3);
            IDirect3DDevice8_SetTexture(dev, 4, svTex4); if (svTex4) IDirect3DBaseTexture8_Release(svTex4);
            IDirect3DDevice8_SetTextureStageState(dev, 2, D3DTSS_COLOROP, t2_cop);
            IDirect3DDevice8_SetTextureStageState(dev, 3, D3DTSS_COLOROP, t3_cop);
            IDirect3DDevice8_SetTextureStageState(dev, 4, D3DTSS_COLOROP, t4_cop);
            for (DWORD st = 1; st < 5; ++st) {   /* restore stages 1..4 sampler states */
                IDirect3DDevice8_SetTextureStageState(dev, st, D3DTSS_TEXCOORDINDEX, sh_tci[st]);
                IDirect3DDevice8_SetTextureStageState(dev, st, D3DTSS_MAGFILTER, sh_mag[st]);
                IDirect3DDevice8_SetTextureStageState(dev, st, D3DTSS_MINFILTER, sh_min[st]);
                IDirect3DDevice8_SetTextureStageState(dev, st, D3DTSS_ADDRESSU, sh_au[st]);
                IDirect3DDevice8_SetTextureStageState(dev, st, D3DTSS_ADDRESSV, sh_av[st]);
            }
            if (sh_psc0_ok) IDirect3DDevice8_SetPixelShaderConstant(dev, 0, sh_psc0, 1);
        }
        IDirect3DDevice8_SetRenderTarget(dev, oldRT, oldDS);   /* restore game's targets */
    } else if (gameBB) {
        /* SetRenderTarget(gameBB) failed (or gameBB null): the RT was already parked
         * on the 64x64 g_scratch (with DS=NULL) at the capture step to free the
         * backbuffer, and the restore inside the block above did NOT run. Re-bind the
         * game's original RT/DS here so the device doesn't keep rendering every
         * subsequent frame into the tiny scratch surface (which would destroy the
         * game's output -- not byte-safe). */
        IDirect3DDevice8_SetRenderTarget(dev, oldRT, oldDS);
    }
    if (oldRT)      IDirect3DSurface8_Release(oldRT);
    if (oldDS)      IDirect3DSurface8_Release(oldDS);
    if (texSurf)    IDirect3DSurface8_Release(texSurf);
    if (scratchSurf) IDirect3DSurface8_Release(scratchSurf);
    if (gameBB)     IDirect3DSurface8_Release(gameBB);
    if (!diag) {
        diag = 1;
        tlog("D3D8 ROT DIAG: GetBB(game)=0x%08lX gameBB=%p GetSurf=0x%08lX texSurf=%p CopyRects=0x%08lX",
             hrGBB, (void*)gameBB, hrGSL, (void*)texSurf, hrCR);
        tlog("D3D8 ROT DIAG: SetRT=0x%08lX BeginScene=0x%08lX DrawPrim=0x%08lX (in-backbuffer rotate)",
             hrSRT, hrBS, hrDP);
    }
    return (hrSRT == D3D_OK);
}

/* When we enlarge the game window for upscaling, the OS paints the client with
 * the window-class background brush on every WM_ERASEBKGND (resize, uncover,
 * invalidation) BEFORE our Present blits. If that brush is white/COLOR_WINDOW it
 * flashes a white frame/box around the stretched game image (the "white box
 * surrounding everything" that only appears once upscaling resizes the window).
 * We subclass the window to swallow WM_ERASEBKGND (our Present owns every client
 * pixel) and force a BLACK class brush as a belt-and-suspenders. */
static WNDPROC g_orig_wndproc = NULL;
static LRESULT CALLBACK ultra_output_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;   /* claim it handled; never erase to white */
    if (g_orig_wndproc) return CallWindowProcA(g_orig_wndproc, h, msg, wp, lp);
    return DefWindowProcA(h, msg, wp, lp);
}

static void resize_output_window(HWND w) {
    if (!w || g_upscaled) return;
    /* kill the white background BEFORE the resize so the first erase is black */
    SetClassLongPtrA(w, GCLP_HBRBACKGROUND, (LONG_PTR)GetStockObject(BLACK_BRUSH));
    if (!g_orig_wndproc)
        g_orig_wndproc = (WNDPROC)(LONG_PTR)SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)ultra_output_wndproc);
    RECT wr = { 0, 0, g_up_w, g_up_h };
    SetWindowLongA(w, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    AdjustWindowRect(&wr, WS_POPUP, FALSE);
    SetWindowPos(w, HWND_TOP, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                 SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    InvalidateRect(w, NULL, TRUE);   /* force one black erase of the new client */
    g_upscaled = 1;
    tlog("D3D8: output window -> %dx%d%s (black bg, erase swallowed)", g_up_w, g_up_h, g_rotate ? " (rotated)" : "");
}

static HRESULT __stdcall Hook_Present(void* self, const RECT* src, const RECT* dst,
                                      HWND hwndOverride, const void* dirty) {
    throttle();
    IDirect3DDevice8* dev = (IDirect3DDevice8*)self;
    const int do_sharpen = (g_sharpen > 0.0f);   /* ULTRA_SHARPEN>0 -> RCAS-style pass */
    if ((g_up_w > 0 || g_rotate) && !g_upscaled)
        resize_output_window(g_devwnd ? g_devwnd : hwndOverride);
    /* Run the in-backbuffer post-process whenever rotation OR sharpen is on. The
     * rotate-only path is byte-identical to before (PP_ROTATE, no sharpen). */
    if (g_rotate || do_sharpen) {
        int mode = (g_rotate ? PP_ROTATE : 0) | (do_sharpen ? PP_SHARPEN : 0);
        present_postprocess(dev, mode);
        /* if the window was enlarged (upscale/rotate) stretch bb->window; else the
         * sharpen-only case keeps native res, so present normally. */
        if (g_up_w > 0 || g_rotate)
            return Real_Present(self, NULL, NULL, hwndOverride, NULL);
        return Real_Present(self, src, dst, hwndOverride, dirty);
    }
    if (g_up_w > 0)                     /* plain upscale: stretch bb -> enlarged window */
        return Real_Present(self, NULL, NULL, hwndOverride, NULL);
    return Real_Present(self, src, dst, hwndOverride, dirty);   /* native path */
}

/* 1 when a post-process feature that creates POOL_DEFAULT resources / subclasses
 * the window is active. Used to gate the Reset hook so the native path (no env set)
 * never installs it and stays byte-identical. */
static int postprocess_active(void) {
    return (g_up_w > 0) || g_rotate || (g_sharpen > 0.0f) || g_aa;
}

/* IDirect3DDevice8::Reset hook (only installed when a post-process feature is on).
 * POOL_DEFAULT resources (g_tex / g_scratch) and the cached pixel shader MUST be
 * released before Reset or it returns D3DERR_INVALIDCALL and the device can't be
 * recovered (hang/black-screen on device-lost / mode change). Release them, let
 * the game Reset, and recreate lazily on the next post-process frame. */
static HRESULT __stdcall Hook_Reset(void* self, void* pp) {
    IDirect3DDevice8* dev = (IDirect3DDevice8*)self;
    if (g_tex)     { IDirect3DTexture8_Release(g_tex);     g_tex = NULL; }
    if (g_scratch) { IDirect3DTexture8_Release(g_scratch); g_scratch = NULL; }
    ultra_sharpen_release(dev);
    HRESULT hr = Real_Reset8 ? Real_Reset8(self, pp) : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr) && postprocess_active()) g_upscaled = 0;   /* re-resize next Present */
    tlog("D3D8: Reset -> 0x%08lX", hr);
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

static HRESULT __stdcall Hook_CreateDevice(void* self, UINT adapter, int devtype, HWND hwnd,
                                           DWORD flags, void* pp, void** ppdev) {
    /* D3DPRESENT_PARAMETERS (x86, 4-byte fields): [0]bbW [1]bbH [2]bbFmt [3]bbCount
     * [4]MultiSample [5]SwapEffect [6]hDeviceWindow [7]Windowed [8]AutoDepth
     * [9]AutoDepthFmt [10]Flags [11]RefreshHz [12]PresentInterval */
    read_target_res();
    if (pp) {
        unsigned* p = (unsigned*)pp;
        g_devwnd = (HWND)(uintptr_t)p[6];
        if (!g_devwnd) g_devwnd = hwnd;
        if (p[0] > 0 && p[1] > 0) { g_bb_w = (int)p[0]; g_bb_h = (int)p[1]; }  /* capture src size */
        tlog("D3D8: CreateDevice bb=%ux%u fmt=%u swapeffect=%u hwnd=%08X windowed=%u ms=%u flags=%08X",
             p[0], p[1], p[2], p[5], p[6], p[7], p[4], p[10]);
        /* For upscaling we must run WINDOWED so Present stretches the 640x480
         * backbuffer to our enlarged window. Fullscreen-exclusive can't do that
         * (backbuffer == display mode). Keep backbuffer at the game's res; force
         * windowed + a desktop-compatible 32-bit format + windowed-legal interval. */
        /* A FULLSCREEN-EXCLUSIVE 16-BIT REQUEST CANNOT SUCCEED ON WINDOWS 10.
         *
         * Chaos Breaker asks for exactly that:
         *     CreateDevice bb=640x480 fmt=23 swapeffect=3 windowed=0
         * fmt 23 is D3DFMT_R5G6B5. WDDM stopped exposing 16-bit exclusive
         * display modes, so CreateDevice answers D3DERR_INVALIDCALL (0x8876086C)
         * and the game prints "CreateDevice error 8876086c / Error:DrawBase
         * Initalize" and quits. It does this with NO loader and NO hook present
         * too, so it is the display stack, not anything of ours -- which also
         * means the ONLY place it can be fixed is right here, in the one hook
         * that owns the parameters.
         *
         * The fixup is the same one upscaling already needed, so this only
         * widens WHEN it runs: any fullscreen request in a 16-bit format gets
         * turned into a windowed one in a desktop-compatible 32-bit format. The
         * backbuffer size is untouched, so the game still renders 640x480 and
         * Present stretches it.
         *
         * ULTRA_D3D8_FORCE_WINDOWED=1 forces this for ANY fullscreen request,
         * =0 disables the widening entirely and restores upscale-only behavior. */
        {
            /* 16-bit backbuffer formats: R5G6B5(23), X1R5G5B5(24), A1R5G5B5(25) */
            int fmt16 = (p[2] == 23 || p[2] == 24 || p[2] == 25);
            int force = 0, off = 0;
            const char *e = getenv("ULTRA_D3D8_FORCE_WINDOWED");
            if (e && e[0] == '1') force = 1;
            if (e && e[0] == '0') off   = 1;

            if (!p[7] && !off && (g_up_w > 0 || fmt16 || force)) {
                const char *why = (g_up_w > 0) ? "upscale"
                                : fmt16        ? "16-bit fullscreen is unavailable on this OS"
                                :                "ULTRA_D3D8_FORCE_WINDOWED=1";
                p[7]  = 1;    /* Windowed = TRUE */
                p[2]  = 22;   /* BackBufferFormat = D3DFMT_X8R8G8B8 (desktop-safe) */
                p[11] = 0;    /* FullScreen_RefreshRateInHz must be 0 when windowed */
                p[12] = 0;    /* PresentationInterval = DEFAULT (windowed) */
                tlog("D3D8: forced WINDOWED (%s); backbuffer stays %ux%u, fmt -> X8R8G8B8",
                     why, p[0], p[1]);
            }
        }
    } else if (!g_devwnd) {
        g_devwnd = hwnd;
    }
    HRESULT hr = Real_CreateDevice(self, adapter, devtype, hwnd, flags, pp, ppdev);
    if (hr >= 0 && ppdev && *ppdev) {
        /* Patch Present per-device: if the game recreates its device (Reset-by-
         * recreate, or a second device) with a DIFFERENT vtable, the new vtable's
         * Present is unpatched and the limiter/post-process would silently drop off.
         * Re-patch whenever we see a vtable we haven't patched yet; the patch is
         * idempotent on the same vtable, so we skip it when it matches. */
        void** vtbl = *(void***)*ppdev;
        if (vtbl != g_patched_vtbl) {
            patch_vtable_entry(*ppdev, IDIRECT3DDEVICE8_PRESENT, (void*)Hook_Present, (void**)&Real_Present);
            /* Reset hook only when a post-process feature is on, so the plain
             * native (limiter-only) path never alters the Reset slot. */
            if (postprocess_active())
                patch_vtable_entry(*ppdev, IDIRECT3DDEVICE8_RESET, (void*)Hook_Reset, (void**)&Real_Reset8);
            g_patched_vtbl = vtbl;
            g_present_hooked = 1;
            tlog("D3D8: device created -> Present%s hooked (limiter%s)",
                 postprocess_active() ? "+Reset" : "", (g_up_w>0)?" + upscale":"");
        }
    }
    return hr;
}

/* Teardown: drop the 1ms timer-resolution we raised in throttle() so we're a clean
 * citizen on process/DLL detach (paired timeBeginPeriod/timeEndPeriod). No
 * behavioral change to the games -- they only rely on the cap while running. */
void ultra_d3d8_teardown(void) {
    if (g_timeperiod_set) { timeEndPeriod(1); g_timeperiod_set = 0; }
}

/* Called by the Direct3DCreate8 hook with the returned IDirect3D8 pointer:
 * patch its CreateDevice so we can reach the device's Present. */
void ultra_d3d8_install_throttle(void* d3d8) {
    if (!d3d8) return;
    patch_vtable_entry(d3d8, IDIRECT3D8_CREATEDEVICE, (void*)Hook_CreateDevice, (void**)&Real_CreateDevice);
}

/* ---------------------------------------------------------------------------
 * The sibling non-D3D8 renderer hooks (D3D9 / D3D11 / OpenGL) all live in their
 * own translation units now; d3d8_throttle.c is D3D8-only. The notes below record
 * where each moved so the create-fn / swap-fn acquisition (wired in
 * jvs_serial_hook.c) is easy to trace.
 *
 * NOTE: ultra_Hook_Direct3DCreate9 now lives in d3d9_throttle.c as a REAL hook
 * (frame limiter + back-buffer BMP dump + rotation/upscale), not a passthrough.
 * NOTE: ultra_Hook_D3D11CreateDeviceAndSwapChain now lives in d3d11_throttle.c as
 * a REAL hook (DXGI swap-chain Present limiter + staging-texture BMP capture +
 * force-windowed), not a passthrough.
 * NOTE: ultra_Hook_wglSwapBuffers now lives in gl_present.c as a REAL hook
 * (frame limiter + glReadPixels back-buffer BMP capture + force-windowed mode-
 * switch interception), not a passthrough. The logging-passthrough GL stub that
 * used to live here has been superseded; d3d8_throttle.c no longer defines it. */

#endif /* _WIN32 */
