/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  d3d11_throttle_alls.c -- D3D11/DXGI present hook for Sega ALLS (HX/UX/UX2/
 *                           MX2). Board-owned, standalone copy.
 * =============================================================================
 *
 *  Sega ALLS (Win10 IoT) renders through Direct3D 11. Unlike D3D8/D3D9 (where
 *  Present is a method on the device vtable), a D3D11 title presents through
 *  the DXGI swap chain, NOT the device, so this hook wraps the swap-chain
 *  vtable:
 *      IDXGISwapChain::Present        vtable slot 8
 *      IDXGISwapChain::ResizeBuffers  vtable slot 13
 *  (slots derived from the documented COM layout: IUnknown[0..2], IDXGIObject
 *  [3..6], IDXGIDeviceSubObject[7], then IDXGISwapChain methods begin at 8.)
 *
 *  Two acquisition paths, whichever the title uses:
 *    1. d3d11.dll!D3D11CreateDeviceAndSwapChain -- device+swapchain in one call.
 *    2. dxgi.dll!CreateDXGIFactory[1] -> IDXGIFactory::CreateSwapChain -- a bare
 *       device + factory-created swap chain (the Sega Nu/ALLS DXGI-factory
 *       render path this board is documented to use).
 *
 *  Present-time behavior:
 *    - throttle Present to a target fps (default 60, ULTRA_FPS_CAP),
 *    - env-gated back-buffer BMP dump (ULTRA_PRESENT_DUMP / ULTRA_ROT_DUMP=dir)
 *      for headless verification,
 *    - force-windowed (ULTRA_SEGA_WINDOWED): a DXGI fullscreen-exclusive swap
 *      chain stalls the loop on a focus change under an unattended run;
 *      SetFullscreenState(FALSE) keeps it focus-independent.
 *
 *  EVERY knob is DEFAULT-OFF; this file's install is itself only called when
 *  ULTRA_D3D11 / ULTRA_SEGA_D3D11 is armed by sega_alls_main.c.
 *
 *  HONESTY: this is SCAFFOLDING, structurally proven against the mingw d3d11.h/
 *  dxgi.h headers. It is NOT verified against a real ALLS title -- no dump on
 *  hand, and the container is keychip/TPM-gated (see sega_keychip_alls.c).
 *
 *  Clean-room: Ultra Loader's own code written from the public COM/DXGI/D3D11
 *  SDK semantics only.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#define COBJMACROS
#include <initguid.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "sega_alls_hook.h"

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

/* Local IID definitions (some mingw toolchains omit these from libdxguid.a in
 * the 64-bit lane) -- with <initguid.h> above, DEFINE_GUID instantiates the
 * symbols in THIS translation unit, so no -ldxguid dependency is required. */
DEFINE_GUID(ULTRA_ALLS_IID_ID3D11Texture2D, 0x6f15aaf2, 0xd208, 0x4e89,
            0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c);
DEFINE_GUID(ULTRA_ALLS_IID_ID3D11Device,    0xdb6f6ddb, 0xac77, 0x4e88,
            0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40);
DEFINE_GUID(ULTRA_ALLS_IID_IDXGIFactory,    0x7b7166ec, 0x21c7, 0x44ae,
            0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69);

#define IDXGISWAPCHAIN_PRESENT        8
#define IDXGISWAPCHAIN_RESIZEBUFFERS  13
#define IDXGIFACTORY_CREATESWAPCHAIN  10

typedef HRESULT (__stdcall *pPresent)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (__stdcall *pResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT (__stdcall *pCreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);

static pPresent         Real_Present         = NULL;
static pResizeBuffers   Real_ResizeBuffers   = NULL;
static pCreateSwapChain Real_CreateSwapChain = NULL;
static int              g_present_hooked     = 0;
static int              g_factory_hooked     = 0;
static volatile LONG    g_installed          = 0;

/* Current render/device window (DXGI_SWAP_CHAIN_DESC::OutputWindow, captured
 * the moment the swap chain is hooked below). Ultra Omni-Bridge's window
 * getter (hook_dllmain_alls.c) reads this via ul_alls_device_window(); NULL
 * until a swap chain has actually been hooked. */
static HWND g_device_window = NULL;

/* ---- frame limiter (QPC) ---------------------------------------------- */
static double        g_target_fps  = 0.0;
static LARGE_INTEGER g_freq, g_last;
static int           g_timing_init  = 0;
static int           g_raised_timer = 0;

static void throttle(void) {
    if (g_target_fps <= 0.0) return;
    if (!g_timing_init) {
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_last);
        timeBeginPeriod(1); g_raised_timer = 1;
        g_timing_init = 1;
        ul_alls_log("D3D11: frame limiter armed @ %.2f fps\n", g_target_fps);
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

/* ---- env-gated knobs ---------------------------------------------------- */
static const char* g_dump_dir   = NULL;
static int         g_dump_every = 0;
static int         g_force_windowed = 0;
static unsigned    g_present_count  = 0;

static int force_windowed(void) {
    const char* w = ul_alls_env("ULTRA_SEGA_WINDOWED");
    return w && *w && w[0] == '1';
}
static void read_knobs(void) {
    const char* cap = ul_alls_env("ULTRA_FPS_CAP");
    if (cap && *cap) { double v = atof(cap); if (v >= 1.0 && v <= 1000.0) g_target_fps = v; }
    g_dump_dir = ul_alls_env("ULTRA_PRESENT_DUMP");
    if (!g_dump_dir || !*g_dump_dir) g_dump_dir = ul_alls_env("ULTRA_ROT_DUMP");
    const char* e = ul_alls_env("ULTRA_DUMP_EVERY");
    g_dump_every = (e && *e) ? atoi(e) : 0;
    g_force_windowed = force_windowed();
}
static int dump_due(unsigned n) {
    if (g_dump_every > 0) return (n % (unsigned)g_dump_every) == 0;
    return (n==120||n==300||n==600||n==900||n==1200||n==1500||n==1800||n==2100);
}

/* ---- back-buffer capture -> 24-bit bottom-up BMP ------------------------ */
static ID3D11Texture2D* g_staging = NULL;
static UINT             g_stg_w = 0, g_stg_h = 0;
static DXGI_FORMAT      g_stg_fmt = DXGI_FORMAT_UNKNOWN;

static void release_staging(void) {
    if (g_staging) { ID3D11Texture2D_Release(g_staging); g_staging = NULL; }
    g_stg_w = g_stg_h = 0; g_stg_fmt = DXGI_FORMAT_UNKNOWN;
}

static void write_bmp_from_mapped(const char* path, const unsigned char* base, int pitch,
                                  int w, int h, DXGI_FORMAT fmt) {
    int rowsz = w * 3, pad = (4 - (rowsz & 3)) & 3, stride = rowsz + pad;
    int imgsz = stride * h;
    unsigned char fhh[14] = {0}, ihh[40] = {0};
    unsigned filesz = 14 + 40 + (unsigned)imgsz;
    fhh[0]='B'; fhh[1]='M'; *(unsigned*)(fhh+2)=filesz; *(unsigned*)(fhh+10)=54;
    *(unsigned*)(ihh+0)=40; *(int*)(ihh+4)=w; *(int*)(ihh+8)=h;
    *(unsigned short*)(ihh+12)=1; *(unsigned short*)(ihh+14)=24; *(unsigned*)(ihh+20)=(unsigned)imgsz;
    FILE* f = fopen(path, "wb");
    if (!f) { ul_alls_log("D3D11 dump: fopen('%s') failed\n", path); return; }
    fwrite(fhh,1,14,f); fwrite(ihh,1,40,f);
    unsigned char* row = (unsigned char*)malloc(stride);
    if (!row) { fclose(f); return; }
    for (int y = h-1; y >= 0; --y) {
        const unsigned char* src = base + (size_t)y * pitch;
        memset(row, 0, stride);
        if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
            for (int x = 0; x < w; ++x) {
                row[x*3+0] = src[x*4+2]; row[x*3+1] = src[x*4+1]; row[x*3+2] = src[x*4+0];
            }
        } else if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM) {
            for (int x = 0; x < w; ++x) {
                unsigned px = ((const unsigned*)src)[x];
                unsigned r = (px >>  0) & 0x3FF, g = (px >> 10) & 0x3FF, b = (px >> 20) & 0x3FF;
                row[x*3+0] = (unsigned char)(b >> 2);
                row[x*3+1] = (unsigned char)(g >> 2);
                row[x*3+2] = (unsigned char)(r >> 2);
            }
        } else {
            for (int x = 0; x < w; ++x) {
                row[x*3+0] = src[x*4+0]; row[x*3+1] = src[x*4+1]; row[x*3+2] = src[x*4+2];
            }
        }
        fwrite(row, 1, stride, f);
    }
    free(row); fclose(f);
    ul_alls_log("D3D11: dumped %dx%d fmt=%d -> %s\n", w, h, (int)fmt, path);
}

static void dump_backbuffer_bmp11(IDXGISwapChain* sc, const char* path) {
    if (!sc) return;
    ID3D11Texture2D* back = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(sc, 0, &ULTRA_ALLS_IID_ID3D11Texture2D, (void**)&back)) || !back) {
        ul_alls_log("D3D11 dump: GetBuffer(0) failed\n"); return;
    }
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    if (FAILED(IDXGISwapChain_GetDevice(sc, &ULTRA_ALLS_IID_ID3D11Device, (void**)&dev)) || !dev) {
        ul_alls_log("D3D11 dump: GetDevice failed\n"); ID3D11Texture2D_Release(back); return;
    }
    ID3D11Device_GetImmediateContext(dev, &ctx);
    if (!ctx) { ul_alls_log("D3D11 dump: GetImmediateContext null\n"); ID3D11Device_Release(dev); ID3D11Texture2D_Release(back); return; }

    D3D11_TEXTURE2D_DESC bd;
    ID3D11Texture2D_GetDesc(back, &bd);

    if (g_staging && (g_stg_w != bd.Width || g_stg_h != bd.Height || g_stg_fmt != bd.Format))
        release_staging();
    if (!g_staging) {
        D3D11_TEXTURE2D_DESC sd; memset(&sd, 0, sizeof sd);
        sd.Width = bd.Width; sd.Height = bd.Height; sd.MipLevels = 1; sd.ArraySize = 1;
        sd.Format = bd.Format; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HRESULT hc = ID3D11Device_CreateTexture2D(dev, &sd, NULL, &g_staging);
        if (FAILED(hc) || !g_staging) {
            ul_alls_log("D3D11 dump: CreateTexture2D(STAGING) 0x%08lX (%ux%u fmt=%d)\n",
                       hc, bd.Width, bd.Height, (int)bd.Format);
            ID3D11DeviceContext_Release(ctx); ID3D11Device_Release(dev); ID3D11Texture2D_Release(back); return;
        }
        g_stg_w = bd.Width; g_stg_h = bd.Height; g_stg_fmt = bd.Format;
    }

    if (bd.SampleDesc.Count > 1) {
        static int warned = 0;
        if (!warned) { warned = 1; ul_alls_log("D3D11 dump: MSAA back buffer (count=%u) -- skipping\n", bd.SampleDesc.Count); }
        ID3D11DeviceContext_Release(ctx); ID3D11Device_Release(dev); ID3D11Texture2D_Release(back); return;
    }

    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)g_staging, (ID3D11Resource*)back);

    D3D11_MAPPED_SUBRESOURCE m; memset(&m, 0, sizeof m);
    HRESULT hm = ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)g_staging, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hm) || !m.pData) {
        ul_alls_log("D3D11 dump: Map(staging) 0x%08lX\n", hm);
        ID3D11DeviceContext_Release(ctx); ID3D11Device_Release(dev); ID3D11Texture2D_Release(back); return;
    }
    write_bmp_from_mapped(path, (const unsigned char*)m.pData, (int)m.RowPitch,
                          (int)bd.Width, (int)bd.Height, bd.Format);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)g_staging, 0);

    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    ID3D11Texture2D_Release(back);
}

/* ---- hooked entrypoints -------------------------------------------------- */
static HRESULT __stdcall Hook_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
    throttle();
    ++g_present_count;
    if (g_dump_dir && *g_dump_dir && dump_due(g_present_count)) {
        char p[512];
        snprintf(p, sizeof p, "%s/d3d11_alls_%04u.bmp", g_dump_dir, g_present_count);
        dump_backbuffer_bmp11(self, p);
    }
    return Real_Present(self, sync, flags);
}
static HRESULT __stdcall Hook_ResizeBuffers(IDXGISwapChain* self, UINT count,
                                            UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    release_staging();
    return Real_ResizeBuffers(self, count, w, h, fmt, flags);
}

static void hook_swapchain_vtable(IDXGISwapChain* sc) {
    if (g_present_hooked || !sc) return;
    void** vt = *(void***)sc;
    DWORD old;
    SIZE_T span = sizeof(void*) * (IDXGISWAPCHAIN_RESIZEBUFFERS - IDXGISWAPCHAIN_PRESENT + 1);
    if (!VirtualProtect(&vt[IDXGISWAPCHAIN_PRESENT], span, PAGE_EXECUTE_READWRITE, &old))
        return;
    if (!Real_Present)       Real_Present       = (pPresent)vt[IDXGISWAPCHAIN_PRESENT];
    if (!Real_ResizeBuffers) Real_ResizeBuffers = (pResizeBuffers)vt[IDXGISWAPCHAIN_RESIZEBUFFERS];
    vt[IDXGISWAPCHAIN_PRESENT]       = (void*)Hook_Present;
    vt[IDXGISWAPCHAIN_RESIZEBUFFERS] = (void*)Hook_ResizeBuffers;
    VirtualProtect(&vt[IDXGISWAPCHAIN_PRESENT], span, old, &old);
    g_present_hooked = 1;
    { DXGI_SWAP_CHAIN_DESC desc;
      if (SUCCEEDED(IDXGISwapChain_GetDesc(sc, &desc)) && desc.OutputWindow) {
          g_device_window = desc.OutputWindow;
          ul_alls_log("D3D11: device window captured hwnd=%p\n", (void*)g_device_window);
      } }
    { HWND sp = FindWindowA("UltraLoaderSplash", NULL);
      if (sp) { ShowWindow(sp, SW_HIDE); ul_alls_log("D3D11: dismissed loader splash\n"); } }
    if (g_force_windowed) {
        HRESULT hr = IDXGISwapChain_SetFullscreenState(sc, FALSE, NULL);
        ul_alls_log("D3D11: force-windowed SetFullscreenState(FALSE) -> 0x%08lX\n", hr);
    }
    ul_alls_log("D3D11: swap-chain vtable hooked (Present slot %d, ResizeBuffers slot %d)\n",
               IDXGISWAPCHAIN_PRESENT, IDXGISWAPCHAIN_RESIZEBUFFERS);
}

typedef HRESULT (__stdcall *pD3D11CreateDeviceAndSwapChain)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
    ID3D11DeviceContext**);
static pD3D11CreateDeviceAndSwapChain Real_CreateDevSC = NULL;

static void resolve_real_create(void) {
    if (Real_CreateDevSC) return;
    HMODULE m = GetModuleHandleA("d3d11.dll");
    if (!m) m = LoadLibraryA("d3d11.dll");
    if (m) Real_CreateDevSC = (pD3D11CreateDeviceAndSwapChain)
                              (void*)ultra_raw_export(m, "D3D11CreateDeviceAndSwapChain");
}

static HRESULT __stdcall Hook_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* ad, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags,
    const D3D_FEATURE_LEVEL* fl, UINT nfl, UINT sdk,
    const DXGI_SWAP_CHAIN_DESC* scd, IDXGISwapChain** ppsc,
    ID3D11Device** ppdev, D3D_FEATURE_LEVEL* pfl, ID3D11DeviceContext** ppctx) {
    static int knobs = 0;
    if (!knobs) { knobs = 1; read_knobs(); }
    resolve_real_create();
    if (!Real_CreateDevSC) {
        ul_alls_log("D3D11: real D3D11CreateDeviceAndSwapChain not resolved -> E_FAIL\n");
        return E_FAIL;
    }
    HRESULT hr = Real_CreateDevSC(ad, dt, sw, flags, fl, nfl, sdk, scd, ppsc, ppdev, pfl, ppctx);
    if (SUCCEEDED(hr) && ppsc && *ppsc) {
        ul_alls_log("D3D11: D3D11CreateDeviceAndSwapChain -> hooking returned swap chain\n");
        hook_swapchain_vtable(*ppsc);
    }
    return hr;
}

static HRESULT __stdcall Hook_CreateSwapChain(IDXGIFactory* self, IUnknown* dev,
                                              DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** ppsc) {
    HRESULT hr = Real_CreateSwapChain(self, dev, desc, ppsc);
    if (SUCCEEDED(hr) && ppsc && *ppsc) {
        ul_alls_log("D3D11: IDXGIFactory::CreateSwapChain -> hooking returned swap chain\n");
        hook_swapchain_vtable(*ppsc);
    }
    return hr;
}
static void hook_factory_vtable(IDXGIFactory* fac) {
    if (g_factory_hooked || !fac) return;
    void** vt = *(void***)fac;
    DWORD old;
    if (!VirtualProtect(&vt[IDXGIFACTORY_CREATESWAPCHAIN], sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
        return;
    if (!Real_CreateSwapChain) Real_CreateSwapChain = (pCreateSwapChain)vt[IDXGIFACTORY_CREATESWAPCHAIN];
    vt[IDXGIFACTORY_CREATESWAPCHAIN] = (void*)Hook_CreateSwapChain;
    VirtualProtect(&vt[IDXGIFACTORY_CREATESWAPCHAIN], sizeof(void*), old, &old);
    g_factory_hooked = 1;
    ul_alls_log("D3D11: IDXGIFactory vtable hooked (CreateSwapChain slot %d)\n", IDXGIFACTORY_CREATESWAPCHAIN);
}

typedef HRESULT (__stdcall *pCreateDXGIFactory)(REFIID, void**);
static pCreateDXGIFactory Real_CreateDXGIFactory = NULL, Real_CreateDXGIFactory1 = NULL;

static HRESULT __stdcall Hook_CreateDXGIFactory_(REFIID riid, void** ppFactory) {
    HRESULT hr = Real_CreateDXGIFactory ? Real_CreateDXGIFactory(riid, ppFactory) : E_FAIL;
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) hook_factory_vtable((IDXGIFactory*)*ppFactory);
    return hr;
}
static HRESULT __stdcall Hook_CreateDXGIFactory1_(REFIID riid, void** ppFactory) {
    HRESULT hr = Real_CreateDXGIFactory1 ? Real_CreateDXGIFactory1(riid, ppFactory) : E_FAIL;
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) hook_factory_vtable((IDXGIFactory*)*ppFactory);
    return hr;
}
HRESULT __stdcall ultra_alls_Hook_CreateDXGIFactory(REFIID riid, void** ppFactory) {
    static int knobs = 0; if (!knobs) { knobs = 1; read_knobs(); }
    if (!Real_CreateDXGIFactory) {
        HMODULE m = GetModuleHandleA("dxgi.dll"); if (!m) m = LoadLibraryA("dxgi.dll");
        if (m) Real_CreateDXGIFactory = (pCreateDXGIFactory)(void*)ultra_raw_export(m, "CreateDXGIFactory");
    }
    return Hook_CreateDXGIFactory_(riid, ppFactory);
}
HRESULT __stdcall ultra_alls_Hook_CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    static int knobs = 0; if (!knobs) { knobs = 1; read_knobs(); }
    if (!Real_CreateDXGIFactory1) {
        HMODULE m = GetModuleHandleA("dxgi.dll"); if (!m) m = LoadLibraryA("dxgi.dll");
        if (m) Real_CreateDXGIFactory1 = (pCreateDXGIFactory)(void*)ultra_raw_export(m, "CreateDXGIFactory1");
    }
    return Hook_CreateDXGIFactory1_(riid, ppFactory);
}

/* ---- install / teardown -------------------------------------------------- */
void ul_alls_d3d11_install(void) {
    if (InterlockedExchange(&g_installed, 1)) return;
    read_knobs();
    if (g_target_fps <= 0.0) g_target_fps = 60.0;
    resolve_real_create();
    ul_alls_iat_patch_all("d3d11.dll", "D3D11CreateDeviceAndSwapChain", (void*)Hook_D3D11CreateDeviceAndSwapChain);
    ul_alls_iat_patch_all("dxgi.dll",  "CreateDXGIFactory",  (void*)ultra_alls_Hook_CreateDXGIFactory);
    ul_alls_iat_patch_all("dxgi.dll",  "CreateDXGIFactory1", (void*)ultra_alls_Hook_CreateDXGIFactory1);
    ul_alls_log("D3D11: install fps_cap=%.1f dump=%s every=%d windowed=%d\n",
               g_target_fps, g_dump_dir ? g_dump_dir : "(off)", g_dump_every, g_force_windowed);
}
void ul_alls_d3d11_teardown(void) {
    if (!InterlockedExchange(&g_installed, 0)) return;
    if (g_raised_timer) { timeEndPeriod(1); g_raised_timer = 0; }
    release_staging();
}

/* Board-owned window getter for Ultra Omni-Bridge (ultra_bridge_window_fn
 * shape: HWND (*)(void)). Returns NULL until the swap chain has been hooked
 * and its OutputWindow captured above -- the bridge stays fully inert until
 * then (see omni_bridge.c's own NULL-window guards). */
HWND ul_alls_device_window(void) {
    return g_device_window;
}

#endif /* _WIN32 */
