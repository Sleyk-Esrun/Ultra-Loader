/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  d3d9_throttle_rawthrills.c -- D3D9 present-hook for the Raw Thrills PC hook.
 * =============================================================================
 *
 *  Newer Raw Thrills cabs (believed Nitro / H2Overdrive era) are expected to
 *  move off D3D8 (see d3d8_throttle_rawthrills.c, the confirmed TFAFSB/RawApp
 *  API) onto D3D9 or D3D11. rawthrills_pc_main.c sets ULTRA_D3D11=auto on
 *  every launch specifically so ONE hook DLL serves both generations without
 *  per-title configuration -- this file and d3d11_throttle_rawthrills.c are
 *  both installed under that same flag; whichever API a given game's binary
 *  actually imports is the one that fires (first/only-wins), the other
 *  installer's IAT patch is a no-op against that process's import table.
 *
 *  WHAT THIS FILE DOES (same contract as the D3D8 hook):
 *    - throttles Present to a target fps (default 60, ULTRA_FPS_CAP),
 *    - optional rotate / upscale-to-window (ULTRA_RES / ULTRA_ROTATE /
 *      ULTRA_ASPECT), default OFF,
 *    - optional back-buffer BMP dump (ULTRA_ROT_DUMP=dir, or front-buffer via
 *      ULTRA_DUMP_FRONT=1) -- the offline verification path for this
 *      DUMP-GATED board,
 *    - force-windowed (ULTRA_SEGA_WINDOWED, or the universal ULTRA_WINDOWED /
 *      ULTRA_BORDERLESS) so a fullscreen-exclusive device isn't lost on a
 *      focus change under an unattended/injected launch.
 *
 *  Adapted from this project's own canonical D3D9 present-hook (read
 *  READ-ONLY for reference, not linked against): the Type X2 lightgun-bridge
 *  call (ultra_typex2_gun_set_window) is NOT ported here -- Raw Thrills'
 *  lightgun titles (Big Buck Hunter family) go through the universal lightgun
 *  service (ULTRA_LIGHTGUN, wired in rawthrills_pc_main.c) which is a
 *  separate, not-yet-connected integration point; wiring THIS present-hook's
 *  device window into that bridge is future work, called out here rather than
 *  silently dropped.
 *
 *  Clean-room: Ultra Loader's own code over the public COM/D3D9 ABI.
 *  Self-contained: no include of, or link against, the shared native_x86 hook
 *  tree or any other board's module folder.
 *
 *  Env gate: ULTRA_D3D11 set to ANY non-empty value (rawthrills_pc_main.c uses
 *  "auto"; "1" also works) -- one flag arms both this file and
 *  d3d11_throttle_rawthrills.c together.
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

#include "rawthrills_hook.h"

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

/* Local log shim -- see the identical comment in d3d8_throttle_rawthrills.c:
 * ul_rt_log() does not auto-newline, so wrap it once instead of editing every
 * ported tlog(...) call site. */
static void rt_d3d9_log(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    ul_rt_log("%s\n", buf);
}
#define tlog rt_d3d9_log

/* ---- upscaler / rotation / sharpen state (sharpen parsed for env parity
 * with the canonical hook but not applied -- no RCAS kernel is wired here,
 * matching the canonical D3D9 hook's own current state: the flag exists,
 * the pixel kernel does not). ------------------------------------------- */
static HWND g_devwnd   = NULL;
static int  g_up_w = 0, g_up_h = 0;
static int  g_upscaled = 0;
static int  g_rotate   = 0;
static int  g_aspect_fill = 1;
static int  g_bb_w = 640, g_bb_h = 480;

static IDirect3DTexture9*   g_tex        = NULL;
static IDirect3DSurface9*   g_scratch    = NULL;
static IDirect3DSurface9*   g_dumpsys    = NULL;
static int                  g_dumpsys_w = 0, g_dumpsys_h = 0;
static D3DFORMAT            g_dumpsys_fmt = D3DFMT_UNKNOWN;

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
 * sets to "1" on every launch; the universal ULTRA_WINDOWED / ULTRA_BORDERLESS
 * knobs (loader settings_apply()) are honored too. Same logic as the D3D8 and
 * D3D11 siblings in this board's hook, so all three present-hooks agree on
 * whether to force windowed. */
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

static void throttle(void) {
    if (!g_timing_init) {
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_last);
        const char* e = getenv("ULTRA_FPS_CAP");
        if (e && *e) { double v = atof(e); if (v >= 1.0 && v <= 1000.0) g_target_fps = v; }
        timeBeginPeriod(1);
        g_timing_init = 1;
        tlog("D3D9(rt): frame limiter armed @ %.2f fps", g_target_fps);
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
        if (secs > 0.0) tlog("D3D9(rt): measured %.1f fps (cap %.0f)", frames / secs, g_target_fps);
        frames = 0;
    }
}

typedef struct { float x, y, z, rhw, u, v; } RVERT9;

static void dump_backbuffer_bmp9(IDirect3DDevice9* dev, const char* path) {
    if (!dev) return;
    static int front = -1;
    if (front < 0) { const char* e = getenv("ULTRA_DUMP_FRONT"); front = (e && e[0]=='1'); }
    if (front) {
        D3DDISPLAYMODE dm; if (FAILED(IDirect3DDevice9_GetDisplayMode(dev, 0, &dm))) { tlog("D3D9(rt) dump(front): GetDisplayMode fail"); return; }
        int fw = (int)dm.Width, fh = (int)dm.Height;
        if (g_dumpsys && (g_dumpsys_w != fw || g_dumpsys_h != fh || g_dumpsys_fmt != D3DFMT_A8R8G8B8)) {
            IDirect3DSurface9_Release(g_dumpsys); g_dumpsys = NULL;
        }
        if (!g_dumpsys) {
            HRESULT hc = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, fw, fh,
                            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_dumpsys, NULL);
            if (FAILED(hc) || !g_dumpsys) { tlog("D3D9(rt) dump(front): CreateOffscreenPlainSurface 0x%08lX (%dx%d)", hc, fw, fh); return; }
            g_dumpsys_w = fw; g_dumpsys_h = fh; g_dumpsys_fmt = D3DFMT_A8R8G8B8;
        }
        HRESULT hr = IDirect3DDevice9_GetFrontBufferData(dev, 0, g_dumpsys);
        if (FAILED(hr)) { tlog("D3D9(rt) dump(front): GetFrontBufferData 0x%08lX (%dx%d)", hr, fw, fh); return; }
        D3DLOCKED_RECT flr;
        if (FAILED(IDirect3DSurface9_LockRect(g_dumpsys, &flr, NULL, D3DLOCK_READONLY))) { tlog("D3D9(rt) dump(front): LockRect fail"); return; }
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
            tlog("D3D9(rt): dumped FRONT %dx%d -> %s", fw, fh, path);
        }
        IDirect3DSurface9_UnlockRect(g_dumpsys);
        return;
    }
    IDirect3DSurface9* bb = NULL;
    HRESULT hgb = IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hgb) || !bb) { tlog("D3D9(rt) dump: GetBackBuffer 0x%08lX", hgb); return; }
    D3DSURFACE_DESC d;
    if (FAILED(IDirect3DSurface9_GetDesc(bb, &d))) { tlog("D3D9(rt) dump: GetDesc fail"); IDirect3DSurface9_Release(bb); return; }
    if (g_dumpsys && (g_dumpsys_w != (int)d.Width || g_dumpsys_h != (int)d.Height || g_dumpsys_fmt != d.Format)) {
        IDirect3DSurface9_Release(g_dumpsys); g_dumpsys = NULL;
    }
    if (!g_dumpsys) {
        HRESULT hc = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height,
                        d.Format, D3DPOOL_SYSTEMMEM, &g_dumpsys, NULL);
        if (FAILED(hc) || !g_dumpsys) { tlog("D3D9(rt) dump: CreateOffscreenPlainSurface 0x%08lX", hc); IDirect3DSurface9_Release(bb); return; }
        g_dumpsys_w = (int)d.Width; g_dumpsys_h = (int)d.Height; g_dumpsys_fmt = d.Format;
    }
    HRESULT hr = IDirect3DDevice9_GetRenderTargetData(dev, bb, g_dumpsys);
    IDirect3DSurface9_Release(bb);
    if (FAILED(hr)) { tlog("D3D9(rt) dump: GetRenderTargetData 0x%08lX (fmt=%u %ux%u)", hr, d.Format, d.Width, d.Height); return; }

    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DSurface9_LockRect(g_dumpsys, &lr, NULL, D3DLOCK_READONLY))) { tlog("D3D9(rt) dump: LockRect fail"); return; }
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
        tlog("D3D9(rt): dumped %dx%d fmt=%u -> %s", w, h, d.Format, path);
    }
    IDirect3DSurface9_UnlockRect(g_dumpsys);
}

static int ensure_rot_resources9(IDirect3DDevice9* dev) {
    if (g_tex && g_scratch) return 1;
    if (!g_tex) {
        HRESULT hr = IDirect3DDevice9_CreateTexture(dev, g_bb_w, g_bb_h, 1, D3DUSAGE_RENDERTARGET,
                                                    D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_tex, NULL);
        if (FAILED(hr)) { tlog("D3D9(rt): CreateTexture(RT) failed 0x%08lX", hr); return 0; }
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
        if (FAILED(hr)) { tlog("D3D9(rt): CreateRenderTarget(scratch) failed 0x%08lX", hr); return 0; }
    }
    tlog("D3D9(rt): rotation resources ready (rotate=%d out=%dx%d src=%dx%d)", g_rotate, g_up_w, g_up_h, g_bb_w, g_bb_h);
    return 1;
}

static void release_default_resources9(void) {
    if (g_tex)     { IDirect3DTexture9_Release(g_tex);     g_tex = NULL; }
    if (g_scratch) { IDirect3DSurface9_Release(g_scratch); g_scratch = NULL; }
}

static IDirect3DStateBlock9* g_sb = NULL;
static int present_rotate9(IDirect3DDevice9* dev) {
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
        if (!warned) { warned = 1; tlog("D3D9(rt) ROT: StretchRect capture failed 0x%08lX -- leaving frame untouched", hrSR); }
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
            if (!warned) { warned = 1; tlog("D3D9(rt) ROT: CreateStateBlock failed -- skipping post-process (no clobber)"); }
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
            const float o = -0.5f;
            RVERT9 q[4] = {
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
            hrDP = IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 2, q, sizeof(RVERT9));
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
        tlog("D3D9(rt) ROT DIAG: GetBB=0x%08lX GetSurf=0x%08lX StretchRect=0x%08lX SetRT=0x%08lX BeginScene=0x%08lX DrawPrim=0x%08lX",
             hrGBB, hrGSL, hrSR, hrSRT, hrBS, hrDP);
    }
    return (hrSRT == D3D_OK);
}

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
    SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(w);
    InvalidateRect(w, NULL, TRUE);
    g_upscaled = 1;
    tlog("D3D9(rt): output window -> %dx%d%s (black bg, erase swallowed, foreground)", g_up_w, g_up_h, g_rotate ? " (rotated)" : "");
}

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
    const int do_rotate  = (g_rotate != 0);
    if (!g_dump_dir_read) { g_dump_dir = getenv("ULTRA_ROT_DUMP"); g_dump_dir_read = 1; }
    const int want_dump  = (g_dump_dir && *g_dump_dir);
    int dev_ok = 1;
    if (do_rotate || want_dump)
        dev_ok = (IDirect3DDevice9_TestCooperativeLevel(dev) == D3D_OK);

    if ((g_up_w > 0 || do_rotate) && !g_upscaled)
        resize_output_window9(g_devwnd ? g_devwnd : hwndOverride);

    if (do_rotate && dev_ok)
        present_rotate9(dev);

    ++g_present_count;
    if (want_dump && dev_ok && dump_due(g_present_count)) {
        char p[512];
        snprintf(p, sizeof p, "%s/d3d9_%04d.bmp", g_dump_dir, g_present_count);
        dump_backbuffer_bmp9(dev, p);
    }

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
    tlog("D3D9(rt): Reset -> 0x%08lX (bb=%dx%d)", hr, g_bb_w, g_bb_h);
    return hr;
}

static void patch_vtable_entry9(void* obj, int index, void* hook, void** out_real) {
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
        /* Future integration point: wire g_devwnd into the universal lightgun
         * bridge (ULTRA_LIGHTGUN, see rawthrills_pc_main.c) for gun titles in
         * this family (Big Buck Hunter). Not implemented in this pass -- see
         * file header. */
        if (pp->BackBufferWidth > 0 && pp->BackBufferHeight > 0) {
            g_bb_w = (int)pp->BackBufferWidth; g_bb_h = (int)pp->BackBufferHeight;
        }
        tlog("D3D9(rt): CreateDevice bb=%ux%u fmt=%u swapeffect=%u hwnd=%08X windowed=%u ms=%u/%lu flags=%08lX",
             pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->SwapEffect,
             (unsigned)(uintptr_t)pp->hDeviceWindow, pp->Windowed, pp->MultiSampleType,
             (unsigned long)pp->MultiSampleQuality, (unsigned long)pp->Flags);
        if ((g_up_w > 0 || force_windowed()) && !pp->Windowed) {
            pp->Windowed = TRUE;
            pp->BackBufferFormat = D3DFMT_X8R8G8B8;
            pp->FullScreen_RefreshRateInHz = 0;
            pp->PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
            tlog("D3D9(rt): forced WINDOWED (%s) (backbuffer stays %ux%u)",
                 g_up_w > 0 ? "upscale" : "auto/user", pp->BackBufferWidth, pp->BackBufferHeight);
        }
    } else if (!g_devwnd) {
        g_devwnd = hwnd;
    }
    HRESULT hr = Real_CreateDevice9(self, adapter, devtype, hwnd, flags, pp, ppdev);
    if (hr >= 0 && ppdev && *ppdev && !g_present_hooked) {
        patch_vtable_entry9(*ppdev, IDIRECT3DDEVICE9_PRESENT, (void*)Hook_Present9, (void**)&Real_Present9);
        patch_vtable_entry9(*ppdev, IDIRECT3DDEVICE9_RESET,   (void*)Hook_Reset9,   (void**)&Real_Reset9);
        g_present_hooked = 1;
        tlog("D3D9(rt): device created -> Present+Reset hooked (limiter%s%s)",
             (g_up_w>0)?" + upscale":"", g_rotate?" + rotate":"");
        HWND sp = FindWindowA("UltraLoaderSplash", NULL);
        if (sp) { ShowWindow(sp, SW_HIDE); tlog("D3D9(rt): dismissed loader splash"); }
        if (g_devwnd) {
            if (force_windowed() && g_up_w == 0) {
                int dw = GetSystemMetrics(SM_CXSCREEN);
                int dh = GetSystemMetrics(SM_CYSCREEN);
                SetWindowLongA(g_devwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                SetWindowPos(g_devwnd, HWND_TOPMOST, 0, 0, dw, dh,
                             SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                SetWindowPos(g_devwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE);
                tlog("D3D9(rt): force-windowed borderless %dx%d (no upscale)", dw, dh);
            }
            SetForegroundWindow(g_devwnd);
        }
    }
    return hr;
}

/* Direct3DCreate9 IAT hook: forward to the real export, then arm the
 * throttle/dump on the returned IDirect3D9. */
typedef IDirect3D9* (__stdcall *pDirect3DCreate9)(UINT);
static pDirect3DCreate9 Real_Direct3DCreate9 = NULL;
static void* __stdcall Hook_Direct3DCreate9(UINT ver) {
    if (!Real_Direct3DCreate9) {
        HMODULE m = GetModuleHandleA("d3d9.dll");
        if (!m) m = LoadLibraryA("d3d9.dll");
        if (m) Real_Direct3DCreate9 = (pDirect3DCreate9)(void*)ultra_raw_export(m, "Direct3DCreate9");
    }
    IDirect3D9* r = Real_Direct3DCreate9 ? Real_Direct3DCreate9(ver) : NULL;
    tlog("D3D9(rt): Direct3DCreate9(0x%X) -> %p %s", ver, (void*)r, r ? "OK" : "NULL(D3D9 init failed)");
    if (r) patch_vtable_entry9(r, IDIRECT3D9_CREATEDEVICE, (void*)Hook_CreateDevice9, (void**)&Real_CreateDevice9);
    return r;
}

void ul_rt_d3d9_install(void) {
    const char* v = ul_rt_env("ULTRA_D3D11");   /* one flag arms D3D9 + D3D11 together */
    if (!v || !v[0]) {
        ul_rt_log("d3d9: skipped (ULTRA_D3D11 unset)\n");
        return;
    }
    ul_rt_iat_patch_all("d3d9.dll", "Direct3DCreate9", (void*)Hook_Direct3DCreate9);
    ul_rt_log("d3d9: armed (present-hook: limiter + rotate/upscale + BMP dump + force-windowed)\n");
}

/* Current device/render window, or NULL if CreateDevice hasn't fired yet.
 * Added for Ultra Omni-Bridge (omni_bridge.c) -- see the identical comment in
 * d3d8_throttle_rawthrills.c. */
HWND ul_rt_d3d9_window(void) { return g_devwnd; }

#endif /* _WIN32 */
