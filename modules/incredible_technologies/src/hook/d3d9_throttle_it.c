/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  d3d9_throttle_it.c -- frame limiter + present hook for Incredible
 *                        Technologies board games (Direct3D 9)
 * =============================================================================
 *
 *  Golden Tee-family PC titles render with D3D9 and pace their game loop off
 *  Present (the cabinet's 60Hz vsync); windowed on a PC that runs unthrottled
 *  would fast-forward. We IAT-hook d3d9.dll!Direct3DCreate9 ->
 *  IDirect3D9::CreateDevice -> IDirect3DDevice9::Present and:
 *    - throttle Present to a target fps (default 60, ULTRA_FPS_CAP),
 *    - optionally force windowed mode (ULTRA_WINDOWED / ULTRA_BORDERLESS /
 *      ULTRA_IT_WINDOWED), which also makes the device focus-independent so
 *      an unattended auto-pilot delivering coin/start keystrokes can't lose
 *      an exclusive device mid-credit-loop,
 *    - dump the back buffer to a 24-bit BMP (ULTRA_IT_DUMP=dir) for offline
 *      verification of a game we cannot screenshot interactively.
 *
 *  Independence: this is this board's own copy of the D3D9 present-hook
 *  technique used elsewhere in this project (native_x86/src/hook/d3d9_throttle.c
 *  was read for research only). No source or link-time dependency on it, or
 *  on any other board's copy, remains. No rotation/upscale/sharpen -- out of
 *  scope for this board's I/O+render+audio task; the frame limiter and
 *  windowed-force are the two things that actually gate playability.
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

#include "it_hook.h"

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
static HWND           g_devwnd           = NULL;
static int            g_bb_w = 640, g_bb_h = 480;

/* -------------------------------------------------------------------------- */
/*  Windowed-force control                                                     */
/* -------------------------------------------------------------------------- */
static int force_windowed(void) {
    static int v = -1;
    if (v < 0) {
        const char* w = ul_it_env("ULTRA_IT_WINDOWED");
        if (w && *w) { v = (w[0] == '1'); }
        else {
            const char* uw = ul_it_env("ULTRA_WINDOWED");
            const char* bl = ul_it_env("ULTRA_BORDERLESS");
            v = ((uw && uw[0] == '1') || (bl && bl[0] == '1')) ? 1 : 0;
        }
    }
    return v;
}

/* -------------------------------------------------------------------------- */
/*  Frame limiter (QPC-based)                                                  */
/* -------------------------------------------------------------------------- */
static double        g_target_fps = 60.0;
static LARGE_INTEGER g_freq, g_last;
static int           g_timing_init = 0;

static void throttle(void) {
    if (!g_timing_init) {
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_last);
        const char* e = ul_it_env("ULTRA_FPS_CAP");
        if (e && *e) { double v = atof(e); if (v >= 1.0 && v <= 1000.0) g_target_fps = v; }
        timeBeginPeriod(1);
        g_timing_init = 1;
        ul_it_log("D3D9: frame limiter armed @ %.2f fps\n", g_target_fps);
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

/* -------------------------------------------------------------------------- */
/*  Backbuffer -> 24-bit BMP dump (verification path, ULTRA_IT_DUMP=dir)       */
/* -------------------------------------------------------------------------- */
static IDirect3DSurface9* g_dumpsys = NULL;
static int g_dumpsys_w = 0, g_dumpsys_h = 0;
static D3DFORMAT g_dumpsys_fmt = D3DFMT_UNKNOWN;

static void dump_backbuffer_bmp(IDirect3DDevice9* dev, const char* path) {
    IDirect3DSurface9* bb = NULL;
    HRESULT hgb = IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hgb) || !bb) { ul_it_log("D3D9 dump: GetBackBuffer 0x%08lX\n", hgb); return; }
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
    if (FAILED(hr)) { ul_it_log("D3D9 dump: GetRenderTargetData 0x%08lX\n", hr); return; }

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
        ul_it_log("D3D9: dumped %dx%d fmt=%u -> %s\n", w, h, d.Format, path);
    }
    IDirect3DSurface9_UnlockRect(g_dumpsys);
}

static const char* g_dump_dir = NULL;
static int         g_dump_dir_read = 0;
static int         g_present_count = 0;
static int dump_due(int n) {
    static int every = -1;
    if (every < 0) { const char* e = ul_it_env("ULTRA_IT_DUMP_EVERY"); every = (e && *e) ? atoi(e) : 0; }
    if (every > 0) return (n % every) == 0;
    return (n==120||n==300||n==600||n==900||n==1200);
}

/* -------------------------------------------------------------------------- */
/*  Present / Reset / CreateDevice hooks                                       */
/* -------------------------------------------------------------------------- */
static HRESULT __stdcall Hook_Present9(void* self, const RECT* src, const RECT* dst,
                                       HWND hwndOverride, const RGNDATA* dirty) {
    throttle();
    IDirect3DDevice9* dev = (IDirect3DDevice9*)self;

    if (!g_dump_dir_read) { g_dump_dir = ul_it_env("ULTRA_IT_DUMP"); g_dump_dir_read = 1; }
    const int want_dump = (g_dump_dir && *g_dump_dir);
    if (want_dump) {
        int dev_ok = (IDirect3DDevice9_TestCooperativeLevel(dev) == D3D_OK);
        ++g_present_count;
        if (dev_ok && dump_due(g_present_count)) {
            char p[512];
            snprintf(p, sizeof p, "%s/dump_raw_%04d.bmp", g_dump_dir, g_present_count);
            dump_backbuffer_bmp(dev, p);
        }
    }

    return Real_Present9(self, src, dst, hwndOverride, dirty);
}

static HRESULT __stdcall Hook_Reset9(void* self, D3DPRESENT_PARAMETERS* pp) {
    if (g_dumpsys) { IDirect3DSurface9_Release(g_dumpsys); g_dumpsys = NULL; }
    if (pp) {
        if (pp->BackBufferWidth > 0 && pp->BackBufferHeight > 0) {
            g_bb_w = (int)pp->BackBufferWidth; g_bb_h = (int)pp->BackBufferHeight;
        }
        if (force_windowed() && !pp->Windowed) {
            pp->Windowed = TRUE;
            pp->BackBufferFormat = D3DFMT_X8R8G8B8;
            pp->FullScreen_RefreshRateInHz = 0;
            pp->PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
        }
    }
    HRESULT hr = Real_Reset9(self, pp);
    ul_it_log("D3D9: Reset -> 0x%08lX (bb=%dx%d)\n", hr, g_bb_w, g_bb_h);
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
    if (pp) {
        g_devwnd = pp->hDeviceWindow ? pp->hDeviceWindow : hwnd;
        if (pp->BackBufferWidth > 0 && pp->BackBufferHeight > 0) {
            g_bb_w = (int)pp->BackBufferWidth; g_bb_h = (int)pp->BackBufferHeight;
        }
        ul_it_log("D3D9: CreateDevice bb=%ux%u fmt=%u hwnd=%08X windowed=%u\n",
             pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat,
             (unsigned)(uintptr_t)pp->hDeviceWindow, pp->Windowed);
        if (force_windowed() && !pp->Windowed) {
            pp->Windowed = TRUE;
            pp->BackBufferFormat = D3DFMT_X8R8G8B8;
            pp->FullScreen_RefreshRateInHz = 0;
            pp->PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
            ul_it_log("D3D9: forced WINDOWED (backbuffer stays %ux%u)\n",
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
        ul_it_log("D3D9: device created -> Present+Reset hooked\n");
        { HWND sp = FindWindowA("UltraLoaderSplash", NULL);
          if (sp) { ShowWindow(sp, SW_HIDE); ul_it_log("D3D9: dismissed loader splash\n"); } }
        if (g_devwnd) SetForegroundWindow(g_devwnd);
    }
    return hr;
}

/* Patch IDirect3D9::CreateDevice (slot 16) on the returned IDirect3D9. */
void ultra_it_d3d9_install_throttle(void* d3d9) {
    if (!d3d9) return;
    patch_vtable_entry(d3d9, IDIRECT3D9_CREATEDEVICE, (void*)Hook_CreateDevice9, (void**)&Real_CreateDevice9);
}

/* Real Direct3DCreate9 hook: resolves the real export, calls it, then arms
 * the throttle on the returned IDirect3D9. Called both from the d3d9.dll IAT
 * patch and from jvs_serial_hook_it.c's GetProcAddress interception (packed
 * game path). */
typedef IDirect3D9* (__stdcall *pDirect3DCreate9)(UINT);
static pDirect3DCreate9 Real_Direct3DCreate9 = NULL;
void* __stdcall ul_it_hook_direct3dcreate9(UINT ver) {
    if (!Real_Direct3DCreate9) {
        HMODULE m = GetModuleHandleA("d3d9.dll");
        if (!m) m = LoadLibraryA("d3d9.dll");
        if (m) Real_Direct3DCreate9 = (pDirect3DCreate9)(void*)ultra_raw_export(m, "Direct3DCreate9");
    }
    IDirect3D9* r = Real_Direct3DCreate9 ? Real_Direct3DCreate9(ver) : NULL;
    ul_it_log("D3D9: Direct3DCreate9(0x%X) -> %p %s\n", ver, (void*)r, r ? "OK" : "NULL(D3D9 init failed)");
    if (r) ultra_it_d3d9_install_throttle(r);
    return r;
}

void ul_it_install_d3d9(void) {
    /* Static-import path: d3d9.dll!Direct3DCreate9 already loaded by the game.
     * jvs_serial_hook_it.c's ul_it_jvs_serial_install() also IAT-patches this
     * same (dll,fn) pair -- both calls are idempotent no-ops on a process that
     * doesn't import it, so calling from both places is safe and covers
     * whichever installs first. */
    ul_it_iat_patch_all("d3d9.dll", "Direct3DCreate9", (void*)ul_it_hook_direct3dcreate9);
}

/* Current D3D9 device window, or NULL if CreateDevice hasn't fired yet
 * (g_devwnd is set in Hook_CreateDevice9 above). Board-neutral accessor used
 * by omni_bridge.c's ultra_bridge_window_fn callback. */
HWND ul_it_get_render_window(void) { return g_devwnd; }

#endif /* _WIN32 */
