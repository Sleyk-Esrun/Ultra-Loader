/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  d3d11_throttle.c -- frame limiter + present capture for the modern x86 arcade
 *                      PC boards that render with Direct3D 11 / DXGI.
 *                      Sibling of d3d9_throttle.c (D3D9) and d3d8_throttle.c.
 * =============================================================================
 *
 *  The post-2010 PC arcade boards -- Taito Type X3 / X4 / X-Zero, Sega RingEdge 2,
 *  Sega Nu / Nu 2 (Nu SX), Sega ALLS (HX / UX / UX2 / MX2), Namco System ES3 /
 *  ES3X, exA-Arcadia, Konami Bemani PC, UNICO PC boards, NESiCAxLive titles --
 *  almost all present through Direct3D 11. Unlike D3D8/D3D9 (where Present is a
 *  method on the device vtable), a D3D11 title presents through the DXGI swap
 *  chain, NOT the device. So this hook wraps the swap-chain vtable:
 *
 *      IDXGISwapChain::Present        vtable slot 8
 *      IDXGISwapChain::ResizeBuffers  vtable slot 13
 *
 *  (slots derived from the documented COM layout: IUnknown[0..2], IDXGIObject
 *  [3..6], IDXGIDeviceSubObject[7], then IDXGISwapChain methods begin at 8;
 *  GetBuffer=9 ... ResizeBuffers=13 ... Present=8.)
 *
 *  We acquire those real function pointers two ways, whichever fires first:
 *    1. IAT / GetProcAddress on d3d11.dll!D3D11CreateDeviceAndSwapChain -- when the
 *       title creates its device + swap chain in one call (the common case). Our
 *       Hook_CreateDevSC forwards to the real export and then patches the returned
 *       swap chain's vtable.
 *    2. IAT / GetProcAddress on dxgi.dll!CreateDXGIFactory[1] -- when the title
 *       makes a bare device then asks an IDXGIFactory to CreateSwapChain /
 *       CreateSwapChainForHwnd. We patch the factory's vtable so any swap chain it
 *       produces is hooked. (Covers the Sega Nu/ALLS DXGI-factory render path.)
 *
 *  Present-time behavior, identical contract to the D3D8/D3D9 hooks:
 *    - throttle Present to a target fps (default 60, ULTRA_FPS_CAP),
 *    - env-gated back-buffer BMP dump (ULTRA_PRESENT_DUMP / ULTRA_ROT_DUMP = dir,
 *      cadence via ULTRA_DUMP_EVERY) -- the D3D11 analog of the D3D9 dumper, the
 *      verification path for a headless/packed board we cannot screenshot. Capture
 *      copies the back buffer (ID3D11Texture2D from GetBuffer(0)) into a CPU-
 *      readable STAGING texture (D3D11_USAGE_STAGING) via CopyResource, then Map()s
 *      it and writes a 24-bit bottom-up BMP (same on-disk format as the D3D9/D3D8
 *      dumpers, so the same offline tooling reads all three).
 *    - force-windowed option (ULTRA_SEGA_WINDOWED / auto): a DXGI fullscreen-
 *      exclusive swap chain stalls the game loop when it loses focus under the
 *      unattended auto-pilot, exactly like the D3D9 exclusive-device-lost problem;
 *      SetFullscreenState(FALSE) keeps the swap chain focus-independent.
 *
 *  EVERY knob is DEFAULT-OFF. With nothing set the only added work per Present is
 *  the limiter (itself a no-op when ULTRA_FPS_CAP is unset and no cabinet default
 *  is forced -- see g_target_fps gating) and a single forwarded call, so the
 *  native render path is byte-identical.
 *
 *  HONESTY: this is SCAFFOLDING. It compiles clean against the mingw d3d11.h /
 *  dxgi.h, exports the symbols the install path expects, and is lifecycle-validated
 *  (install/teardown idempotent, graceful with no device). The capture path is
 *  proven against a synthetic D3D11 present app (test_d3d11_capture.c). It is NOT
 *  verified against a real Nu/ALLS/Type X3 title -- we have no dump, and those
 *  containers are keychip/DRM-encrypted (see sega_keychip.c / sega_board.c). Each
 *  board is "wired and ready for a dump", NOT verified-playable.
 *
 *  Clean-room: Ultra Loader's own code written from the public COM / DXGI / D3D11
 *  SDK semantics only.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#define COBJMACROS
#include <initguid.h>     /* so DEFINE_GUID instantiates the IIDs locally */
#include <d3d11.h>
#include <dxgi.h>
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

/* Some mingw toolchains don't emit these IIDs in libdxguid.a for the 64-bit lane,
 * so define them locally from the documented interface GUIDs. With <initguid.h>
 * above, DEFINE_GUID instantiates the symbols in THIS translation unit -- no
 * -ldxguid link dependency. */
DEFINE_GUID(ULTRA_IID_ID3D11Texture2D, 0x6f15aaf2, 0xd208, 0x4e89,
            0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c);
DEFINE_GUID(ULTRA_IID_ID3D11Device,    0xdb6f6ddb, 0xac77, 0x4e88,
            0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40);
DEFINE_GUID(ULTRA_IID_IDXGIFactory,    0x7b7166ec, 0x21c7, 0x44ae,
            0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69);

/* DXGI / swap-chain vtable indices. */
#define IDXGISWAPCHAIN_PRESENT        8
#define IDXGISWAPCHAIN_RESIZEBUFFERS  13
/* IDXGIFactory vtable: IUnknown[0..2], IDXGIObject[3..6], then IDXGIFactory:
 * EnumAdapters=7, MakeWindowAssociation=8, GetWindowAssociation=9,
 * CreateSwapChain=10, CreateSoftwareAdapter=11. */
#define IDXGIFACTORY_CREATESWAPCHAIN  10

typedef HRESULT (__stdcall *pPresent)(IDXGISwapChain* self, UINT sync, UINT flags);
typedef HRESULT (__stdcall *pResizeBuffers)(IDXGISwapChain* self, UINT count,
                                            UINT w, UINT h, DXGI_FORMAT fmt, UINT flags);
typedef HRESULT (__stdcall *pCreateSwapChain)(IDXGIFactory* self, IUnknown* dev,
                                              DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** ppsc);

static pPresent        Real_Present       = NULL;
static pResizeBuffers  Real_ResizeBuffers = NULL;
static pCreateSwapChain Real_CreateSwapChain = NULL;
static int             g_present_hooked   = 0;
static int             g_factory_hooked   = 0;
static volatile LONG   g_installed        = 0;

/* shared logger (gated by ULTRA_JVS_LOG); same file the JVS/D3D8/D3D9 hooks use.
 * ultra_d3d8_log appends its own '\n', so format strings carry no trailing newline. */
extern void ultra_d3d8_log(const char* fmt, ...);
#define tlog ultra_d3d8_log

/* ---- frame limiter (QPC, identical algorithm to d3d9) --------------------- */
static double        g_target_fps  = 0.0;   /* 0 = no cap until armed            */
static LARGE_INTEGER g_freq, g_last;
static int           g_timing_init = 0;
static int           g_raised_timer = 0;

/* Whether the limiter should run at all. Armed only if ULTRA_FPS_CAP is set OR a
 * Sega-board cabinet 60Hz default is explicitly requested (ULTRA_SEGA_D3D11=1 path
 * sets a 60 default). When neither is set g_target_fps stays 0 and throttle() is a
 * pure no-op -> the native path is byte-identical. */
static void throttle(void) {
    if (g_target_fps <= 0.0) return;            /* limiter disarmed: byte-identical */
    if (!g_timing_init) {
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_last);
        timeBeginPeriod(1); g_raised_timer = 1;
        g_timing_init = 1;
        tlog("D3D11: frame limiter armed @ %.2f fps", g_target_fps);
        return;                                 /* first Present: baseline only      */
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
        if (secs > 0.0) tlog("D3D11: measured %.1f fps (cap %.0f)", frames / secs, g_target_fps);
        frames = 0;
    }
}

/* ---- env-gated knobs (read once at install) ------------------------------- */
static const char* g_dump_dir   = NULL;     /* ULTRA_PRESENT_DUMP / ULTRA_ROT_DUMP   */
static int         g_dump_every = 0;        /* ULTRA_DUMP_EVERY=N (0 = sample cadence)*/
static int         g_force_windowed = 0;    /* ULTRA_SEGA_WINDOWED / auto            */
static unsigned    g_present_count = 0;

static int force_windowed(void) {
    const char* w = getenv("ULTRA_SEGA_WINDOWED");
    if (w && *w) return (w[0] == '1');
    const char* a = getenv("ULTRA_TYPEX2_AUTO");        /* shared auto-pilot flag     */
    return (a && a[0] == '1');
}

static void read_knobs(void) {
    const char* cap = getenv("ULTRA_FPS_CAP");
    if (cap && *cap) { double v = atof(cap); if (v >= 1.0 && v <= 1000.0) g_target_fps = v; }
    g_dump_dir = getenv("ULTRA_PRESENT_DUMP");
    if (!g_dump_dir || !*g_dump_dir) g_dump_dir = getenv("ULTRA_ROT_DUMP");
    const char* e = getenv("ULTRA_DUMP_EVERY");
    g_dump_every = (e && *e) ? atoi(e) : 0;
    g_force_windowed = force_windowed();
}

/* sample several presents so a content frame is reliably caught (attract loops
 * through black transitions); same cadence as the D3D8/D3D9 dumpers. ULTRA_DUMP_EVERY
 * overrides with a periodic dump every N presents. */
static int dump_due(unsigned n) {
    if (g_dump_every > 0) return (n % (unsigned)g_dump_every) == 0;
    return (n==120||n==300||n==600||n==900||n==1200||n==1500||n==1800||n==2100);
}

/* ---- back-buffer capture -> 24-bit bottom-up BMP -------------------------- *
 * The D3D11 analog of the D3D9 GetRenderTargetData dumper:
 *   GetBuffer(0) -> back-buffer ID3D11Texture2D
 *   create (cache) a matching D3D11_USAGE_STAGING / CPU_READ texture
 *   CopyResource(staging, backbuffer)   (full-surface GPU->staging copy)
 *   Map(staging, READ) -> walk rows -> write BGR 24-bit BMP, bottom-up.
 * The staging texture is cached and only recreated when the back-buffer size or
 * format changes, mirroring g_dumpsys reuse in the D3D9 path. */
static ID3D11Texture2D* g_staging = NULL;
static UINT             g_stg_w = 0, g_stg_h = 0;
static DXGI_FORMAT      g_stg_fmt = DXGI_FORMAT_UNKNOWN;

static void release_staging(void) {
    if (g_staging) { ID3D11Texture2D_Release(g_staging); g_staging = NULL; }
    g_stg_w = g_stg_h = 0; g_stg_fmt = DXGI_FORMAT_UNKNOWN;
}

/* Write one mapped staging surface as a 24-bit BMP. fmt drives channel order:
 * B8G8R8A8 (the DXGI swap-chain norm) is BGRA in memory -> direct BGR copy;
 * R8G8B8A8 is RGBA -> swap R/B; R10G10B10A2 is packed 32-bit -> approximate to 8-bit.
 * Anything else still writes (channels approximated) rather than fail. */
static void write_bmp_from_mapped(const char* path, const unsigned char* base, int pitch,
                                  int w, int h, DXGI_FORMAT fmt) {
    int rowsz = w * 3, pad = (4 - (rowsz & 3)) & 3, stride = rowsz + pad;
    int imgsz = stride * h;
    unsigned char fhh[14] = {0}, ihh[40] = {0};
    unsigned filesz = 14 + 40 + (unsigned)imgsz;
    fhh[0]='B'; fhh[1]='M'; *(unsigned*)(fhh+2)=filesz; *(unsigned*)(fhh+10)=54;
    *(unsigned*)(ihh+0)=40; *(int*)(ihh+4)=w; *(int*)(ihh+8)=h;   /* +h = bottom-up */
    *(unsigned short*)(ihh+12)=1; *(unsigned short*)(ihh+14)=24; *(unsigned*)(ihh+20)=(unsigned)imgsz;
    FILE* f = fopen(path, "wb");
    if (!f) { tlog("D3D11 dump: fopen('%s') failed", path); return; }
    fwrite(fhh,1,14,f); fwrite(ihh,1,40,f);
    unsigned char* row = (unsigned char*)malloc(stride);
    if (!row) { fclose(f); return; }
    for (int y = h-1; y >= 0; --y) {
        const unsigned char* src = base + (size_t)y * pitch;
        memset(row, 0, stride);
        if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM) {            /* RGBA -> BGR */
            for (int x = 0; x < w; ++x) {
                row[x*3+0] = src[x*4+2]; row[x*3+1] = src[x*4+1]; row[x*3+2] = src[x*4+0];
            }
        } else if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM) {  /* packed 2-10-10-10 -> 8b BGR */
            for (int x = 0; x < w; ++x) {
                unsigned px = ((const unsigned*)src)[x];
                unsigned r = (px >>  0) & 0x3FF, g = (px >> 10) & 0x3FF, b = (px >> 20) & 0x3FF;
                row[x*3+0] = (unsigned char)(b >> 2);
                row[x*3+1] = (unsigned char)(g >> 2);
                row[x*3+2] = (unsigned char)(r >> 2);
            }
        } else {                                            /* B8G8R8A8 (default) -> BGR */
            for (int x = 0; x < w; ++x) {
                row[x*3+0] = src[x*4+0]; row[x*3+1] = src[x*4+1]; row[x*3+2] = src[x*4+2];
            }
        }
        fwrite(row, 1, stride, f);
    }
    free(row); fclose(f);
    tlog("D3D11: dumped %dx%d fmt=%d -> %s", w, h, (int)fmt, path);
}

static void dump_backbuffer_bmp11(IDXGISwapChain* sc, const char* path) {
    if (!sc) return;
    ID3D11Texture2D* back = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(sc, 0, &ULTRA_IID_ID3D11Texture2D, (void**)&back)) || !back) {
        tlog("D3D11 dump: GetBuffer(0) failed"); return;
    }
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    if (FAILED(IDXGISwapChain_GetDevice(sc, &ULTRA_IID_ID3D11Device, (void**)&dev)) || !dev) {
        tlog("D3D11 dump: GetDevice failed"); ID3D11Texture2D_Release(back); return;
    }
    ID3D11Device_GetImmediateContext(dev, &ctx);
    if (!ctx) { tlog("D3D11 dump: GetImmediateContext null"); ID3D11Device_Release(dev); ID3D11Texture2D_Release(back); return; }

    D3D11_TEXTURE2D_DESC bd;
    ID3D11Texture2D_GetDesc(back, &bd);

    /* (re)create the cached staging texture when geometry/format changes */
    if (g_staging && (g_stg_w != bd.Width || g_stg_h != bd.Height || g_stg_fmt != bd.Format))
        release_staging();
    if (!g_staging) {
        D3D11_TEXTURE2D_DESC sd; memset(&sd, 0, sizeof sd);
        sd.Width = bd.Width; sd.Height = bd.Height; sd.MipLevels = 1; sd.ArraySize = 1;
        sd.Format = bd.Format; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        HRESULT hc = ID3D11Device_CreateTexture2D(dev, &sd, NULL, &g_staging);
        if (FAILED(hc) || !g_staging) {
            tlog("D3D11 dump: CreateTexture2D(STAGING) 0x%08lX (%ux%u fmt=%d)", hc, bd.Width, bd.Height, (int)bd.Format);
            ID3D11DeviceContext_Release(ctx); ID3D11Device_Release(dev); ID3D11Texture2D_Release(back); return;
        }
        g_stg_w = bd.Width; g_stg_h = bd.Height; g_stg_fmt = bd.Format;
    }

    /* If the swap chain is MSAA (Count>1) CopyResource into a non-MSAA staging is
     * invalid; a real MSAA board needs ResolveSubresource first. Back-buffers are
     * almost always single-sampled, so we guard + skip rather than mis-copy. */
    if (bd.SampleDesc.Count > 1) {
        static int warned = 0;
        if (!warned) { warned = 1; tlog("D3D11 dump: MSAA back buffer (count=%u) -- resolve path not yet wired, skipping", bd.SampleDesc.Count); }
        ID3D11DeviceContext_Release(ctx); ID3D11Device_Release(dev); ID3D11Texture2D_Release(back); return;
    }

    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)g_staging, (ID3D11Resource*)back);

    D3D11_MAPPED_SUBRESOURCE m; memset(&m, 0, sizeof m);
    HRESULT hm = ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)g_staging, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hm) || !m.pData) {
        tlog("D3D11 dump: Map(staging) 0x%08lX", hm);
        ID3D11DeviceContext_Release(ctx); ID3D11Device_Release(dev); ID3D11Texture2D_Release(back); return;
    }
    write_bmp_from_mapped(path, (const unsigned char*)m.pData, (int)m.RowPitch,
                          (int)bd.Width, (int)bd.Height, bd.Format);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)g_staging, 0);

    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    ID3D11Texture2D_Release(back);
}

/* ---- hooked entrypoints --------------------------------------------------- */
static HRESULT __stdcall Hook_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
    throttle();
    ++g_present_count;
    /* env-gated BMP dump of the finished back buffer. Runs BEFORE Real_Present so
     * the just-rendered frame is still in back-buffer 0 (a FLIP-model swap chain
     * rotates buffers on Present); the front buffer is what reaches the monitor but
     * GetBuffer(0) before Present holds the frame the game just drew. */
    if (g_dump_dir && *g_dump_dir && dump_due(g_present_count)) {
        char p[512];
        snprintf(p, sizeof p, "%s/d3d11_%04u.bmp", g_dump_dir, g_present_count);
        dump_backbuffer_bmp11(self, p);
    }
    return Real_Present(self, sync, flags);
}

static HRESULT __stdcall Hook_ResizeBuffers(IDXGISwapChain* self, UINT count,
                                            UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    /* the cached staging texture is sized to the old back buffer; drop it so the
     * next dump recreates it against the new geometry. */
    release_staging();
    return Real_ResizeBuffers(self, count, w, h, fmt, flags);
}

/* Patch the live swap-chain vtable in place (slots 8 + 13). Idempotent. */
static void hook_swapchain_vtable(IDXGISwapChain* sc) {
    if (g_present_hooked || !sc) return;
    void** vt = *(void***)sc;
    DWORD old;
    /* protect the whole span [Present .. ResizeBuffers] so both writes land. */
    SIZE_T span = sizeof(void*) * (IDXGISWAPCHAIN_RESIZEBUFFERS - IDXGISWAPCHAIN_PRESENT + 1);
    if (!VirtualProtect(&vt[IDXGISWAPCHAIN_PRESENT], span, PAGE_EXECUTE_READWRITE, &old))
        return;
    if (!Real_Present)       Real_Present       = (pPresent)vt[IDXGISWAPCHAIN_PRESENT];
    if (!Real_ResizeBuffers) Real_ResizeBuffers = (pResizeBuffers)vt[IDXGISWAPCHAIN_RESIZEBUFFERS];
    vt[IDXGISWAPCHAIN_PRESENT]       = (void*)Hook_Present;
    vt[IDXGISWAPCHAIN_RESIZEBUFFERS] = (void*)Hook_ResizeBuffers;
    VirtualProtect(&vt[IDXGISWAPCHAIN_PRESENT], span, old, &old);
    g_present_hooked = 1;
    { HWND sp = FindWindowA("UltraLoaderSplash", NULL);
      if (sp) { ShowWindow(sp, SW_HIDE); tlog("D3D11: dismissed loader splash"); } }

    /* force-windowed: drop fullscreen-exclusive so a focus change can't stall the
     * loop (DXGI fullscreen is lost on focus loss, like the D3D9 exclusive device). */
    if (g_force_windowed) {
        HRESULT hr = IDXGISwapChain_SetFullscreenState(sc, FALSE, NULL);
        tlog("D3D11: force-windowed SetFullscreenState(FALSE) -> 0x%08lX", hr);
    }
    tlog("D3D11: swap-chain vtable hooked (Present slot %d, ResizeBuffers slot %d)",
         IDXGISWAPCHAIN_PRESENT, IDXGISWAPCHAIN_RESIZEBUFFERS);
}

/* ---- D3D11CreateDeviceAndSwapChain hook (primary acquisition path) --------- *
 * Forwards to the real export, then hooks the returned swap chain's vtable. This
 * is the canonical real hook the IAT patch + GetProcAddress interception (both in
 * jvs_serial_hook.c) route to -- it SUPERSEDES the logging passthrough that used
 * to live in d3d8_throttle.c. */
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

/* Exported (no name-mangle, __stdcall) -- referenced by jvs_serial_hook.c. The
 * forward type there is the loose 12-void* form; this concrete signature is ABI-
 * compatible (same parameter slots). read_knobs runs lazily on first call so the
 * hook is correct whether armed via ultra_d3d11_install() or only via the IAT/
 * GetProcAddress route (when ULTRA_SEGA_D3D11 was not set). */
HRESULT __stdcall ultra_Hook_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* ad, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags,
    const D3D_FEATURE_LEVEL* fl, UINT nfl, UINT sdk,
    const DXGI_SWAP_CHAIN_DESC* scd, IDXGISwapChain** ppsc,
    ID3D11Device** ppdev, D3D_FEATURE_LEVEL* pfl, ID3D11DeviceContext** ppctx) {
    static int knobs = 0;
    if (!knobs) { knobs = 1; read_knobs(); }
    resolve_real_create();
    if (!Real_CreateDevSC) {
        tlog("D3D11: real D3D11CreateDeviceAndSwapChain not resolved -> E_FAIL");
        return E_FAIL;
    }
    HRESULT hr = Real_CreateDevSC(ad, dt, sw, flags, fl, nfl, sdk, scd, ppsc, ppdev, pfl, ppctx);
    if (SUCCEEDED(hr) && ppsc && *ppsc) {
        tlog("D3D11: D3D11CreateDeviceAndSwapChain -> hooking returned swap chain");
        hook_swapchain_vtable(*ppsc);
    }
    return hr;
}

/* ---- IDXGIFactory::CreateSwapChain hook (secondary acquisition path) -------- *
 * Titles that make a bare device then ask a factory for the swap chain go through
 * here. We patch the factory vtable; any swap chain it returns gets vtable-hooked. */
static HRESULT __stdcall Hook_CreateSwapChain(IDXGIFactory* self, IUnknown* dev,
                                              DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** ppsc) {
    HRESULT hr = Real_CreateSwapChain(self, dev, desc, ppsc);
    if (SUCCEEDED(hr) && ppsc && *ppsc) {
        tlog("D3D11: IDXGIFactory::CreateSwapChain -> hooking returned swap chain");
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
    tlog("D3D11: IDXGIFactory vtable hooked (CreateSwapChain slot %d)", IDXGIFACTORY_CREATESWAPCHAIN);
}

/* CreateDXGIFactory[1] hook: forward, then patch the returned factory's vtable. */
typedef HRESULT (__stdcall *pCreateDXGIFactory)(REFIID riid, void** ppFactory);
static pCreateDXGIFactory Real_CreateDXGIFactory = NULL, Real_CreateDXGIFactory1 = NULL;

static HRESULT __stdcall Hook_CreateDXGIFactory(REFIID riid, void** ppFactory) {
    HRESULT hr = Real_CreateDXGIFactory ? Real_CreateDXGIFactory(riid, ppFactory) : E_FAIL;
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) hook_factory_vtable((IDXGIFactory*)*ppFactory);
    return hr;
}
static HRESULT __stdcall Hook_CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    HRESULT hr = Real_CreateDXGIFactory1 ? Real_CreateDXGIFactory1(riid, ppFactory) : E_FAIL;
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) hook_factory_vtable((IDXGIFactory*)*ppFactory);
    return hr;
}

/* Exported factory-create hooks -- IAT-patched by jvs_serial_hook.c onto dxgi.dll.
 * (Forward-declared loosely there as 2-arg HRESULT __stdcall.) */
HRESULT __stdcall ultra_Hook_CreateDXGIFactory(REFIID riid, void** ppFactory) {
    static int knobs = 0; if (!knobs) { knobs = 1; read_knobs(); }
    if (!Real_CreateDXGIFactory) {
        HMODULE m = GetModuleHandleA("dxgi.dll"); if (!m) m = LoadLibraryA("dxgi.dll");
        if (m) Real_CreateDXGIFactory = (pCreateDXGIFactory)(void*)ultra_raw_export(m, "CreateDXGIFactory");
    }
    return Hook_CreateDXGIFactory(riid, ppFactory);
}
HRESULT __stdcall ultra_Hook_CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    static int knobs = 0; if (!knobs) { knobs = 1; read_knobs(); }
    if (!Real_CreateDXGIFactory1) {
        HMODULE m = GetModuleHandleA("dxgi.dll"); if (!m) m = LoadLibraryA("dxgi.dll");
        if (m) Real_CreateDXGIFactory1 = (pCreateDXGIFactory)(void*)ultra_raw_export(m, "CreateDXGIFactory1");
    }
    return Hook_CreateDXGIFactory1(riid, ppFactory);
}

extern void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* ---- install / teardown (hook_dllmain.c calls these under ULTRA_SEGA_D3D11) - */
void ultra_d3d11_install(void) {
    if (InterlockedExchange(&g_installed, 1)) return;   /* idempotent */
    read_knobs();
    /* armed via the Sega-board path: default the cabinet to 60Hz if no explicit cap */
    if (g_target_fps <= 0.0) g_target_fps = 60.0;
    resolve_real_create();
    /* IAT-patch both acquisition paths so the title's swap chain is hooked whichever
     * create route it takes. (jvs_serial_hook.c also patches the device+swapchain
     * creator + GetProcAddress; this adds the bare-factory route + re-arm for a
     * late-loaded d3d11/dxgi.) */
    ultra_iat_patch_all("d3d11.dll", "D3D11CreateDeviceAndSwapChain", (void*)ultra_Hook_D3D11CreateDeviceAndSwapChain);
    ultra_iat_patch_all("dxgi.dll",  "CreateDXGIFactory",  (void*)ultra_Hook_CreateDXGIFactory);
    ultra_iat_patch_all("dxgi.dll",  "CreateDXGIFactory1", (void*)ultra_Hook_CreateDXGIFactory1);
    tlog("D3D11: install fps_cap=%.1f dump=%s every=%d windowed=%d",
         g_target_fps, g_dump_dir ? g_dump_dir : "(off)", g_dump_every, g_force_windowed);
}

void ultra_d3d11_teardown(void) {
    if (!InterlockedExchange(&g_installed, 0)) return;
    if (g_raised_timer) { timeEndPeriod(1); g_raised_timer = 0; }
    release_staging();
    /* the vtable patch is left in place: process teardown frees the address space
     * wholesale; an orderly unload would need the cached Real_* pointers restored. */
}

#endif /* _WIN32 */
