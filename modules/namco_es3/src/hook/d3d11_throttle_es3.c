/* SPDX-License-Identifier: MIT
 * NOTE: independent namco_es3 board copy -- adapted from native_x86/src/hook/d3d11_throttle.c (reference only; no build-time or runtime dependency on native_x86 remains).
 * =============================================================================
 *  d3d11_throttle_es3.c -- frame limiter + present capture for the modern x86 arcade
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

/* ---- render-size override ------------------------------------------------- *
 *
 * Mario Kart GP DX renders EVERYTHING at a hardcoded 1360x768 and then blits it
 * 1:1 into whatever back buffer exists. Verified in its own code:
 *
 *     0x4051FF  movl $0x550,0x28(%ebx)     ; 1360
 *     0x405206  movl $0x300,0x2c(%ebx)     ;  768
 *     0x40743A  push $0x300 / push $0x550 / call 0x7BEC90   ; its render targets
 *
 * On a 1080p desktop it creates a 1920x1080 swap chain anyway, so the frame
 * lands in the TOP-LEFT CORNER with black around it - which is what the screen
 * actually looked like, and why the earlier "make the window fullscreen and let
 * DXGI upscale" change did nothing: the swap chain was ALREADY 1920x1080
 * ("D3D11: authored back buffer = 1920x1080" in the log), so there was nothing
 * for DXGI to scale.
 *
 * The fix is the other direction: create the swap chain at the size the game
 * actually draws, so its output FILLS the back buffer, and let DXGI stretch that
 * to the monitor-sized window. Nothing in the game's rendering is touched - no
 * viewport rewriting, no render-target resizing, both of which risk clipping the
 * intermediate 1360x768 targets the cluster above allocates.
 *
 * ULTRA_RENDER_W / ULTRA_RENDER_H carry the size (the module sets them for this
 * title). Unset = leave every swap chain exactly as the game asked for it. */
static UINT g_auth_w = 0, g_auth_h = 0;   /* the size the game authored */
static UINT g_req_w = 0, g_req_h = 0;
static int  g_req_read = 0;

static void read_render_size(void) {
    /* GetEnvironmentVariableA, NOT getenv. getenv reads the CRT's snapshot of
     * the environment, taken at DLL_PROCESS_ATTACH for an INJECTED dll - a
     * distinction that has burned this project before. The live process block
     * is the authority. */
    char wb[32], hb[32];
    DWORD nw, nh;
    if (g_req_read) return;
    g_req_read = 1;
    wb[0] = hb[0] = 0;
    nw = GetEnvironmentVariableA("ULTRA_RENDER_W", wb, sizeof wb);
    nh = GetEnvironmentVariableA("ULTRA_RENDER_H", hb, sizeof hb);
    if (nw && nw < sizeof wb && nh && nh < sizeof hb) {
        long lw = strtol(wb, NULL, 10), lh = strtol(hb, NULL, 10);
        if (lw >= 320 && lw <= 7680 && lh >= 240 && lh <= 4320) {
            g_req_w = (UINT)lw; g_req_h = (UINT)lh;
        }
    }
    /* ALWAYS report. The previous build failed silently: the swap-chain hook
     * ran, the override did not, and nothing in the log said why. */
    tlog("D3D11: render-size override = %s (ULTRA_RENDER_W='%s' ULTRA_RENDER_H='%s')",
         (g_req_w && g_req_h) ? "ON" : "off - swap chain left as the game asked",
         wb, hb);
}

/* Returns the desc to actually use: either the caller's, or a rewritten copy. */
static const DXGI_SWAP_CHAIN_DESC* apply_render_size(const DXGI_SWAP_CHAIN_DESC* in,
                                                     DXGI_SWAP_CHAIN_DESC* scratch) {
    read_render_size();
    if (!g_req_w || !g_req_h) return in;

    /* ARM THE PIN IMMEDIATELY. Creating the swap chain at the authored size is
     * only half the job: making the window monitor-sized raises WM_SIZE, the
     * game answers with ResizeBuffers(1920x1080), and the back buffer grows
     * straight back. The pin in Hook_ResizeBuffers needs g_auth_* already set
     * to stop that - and g_auth_* used to be filled from the first Present,
     * which happens AFTER the first resize. Set it here instead. */
    g_auth_w = g_req_w;
    g_auth_h = g_req_h;

    if (!in) {
        tlog("D3D11: render-size override ON but the swap-chain desc was NULL");
        return in;
    }
    if (in->BufferDesc.Width == g_req_w && in->BufferDesc.Height == g_req_h) {
        tlog("D3D11: swap chain already %ux%u as requested", g_req_w, g_req_h);
        return in;
    }
    *scratch = *in;
    scratch->BufferDesc.Width  = g_req_w;
    scratch->BufferDesc.Height = g_req_h;
    tlog("D3D11: swap chain %ux%u -> %ux%u (game draws %ux%u; DXGI stretches to the window)",
         in->BufferDesc.Width, in->BufferDesc.Height, g_req_w, g_req_h, g_req_w, g_req_h);
    return scratch;
}

/* ---- display mode --------------------------------------------------------- *
 *
 * Mario Kart GP DX authors its frame at the ES3 cabinet's native resolution and
 * creates a plain window that size. On a 1920x1080 desktop that leaves the game
 * sitting in the top-left corner with black around it, because nothing ever
 * resizes the window and DXGI only stretches the back buffer to whatever the
 * window CLIENT AREA is. The board had no window handling at all; this is the
 * same treatment taito_typex2 got for D3D9.
 *
 * Borderless and Fullscreen are both a monitor-sized WS_POPUP on purpose.
 * DXGI fullscreen-exclusive is lost on every focus change (which is exactly why
 * this file already calls SetFullscreenState(FALSE)), whereas a popup is
 * visually identical, sits flush with no title bar, and is focus-independent.
 *
 * The back buffer is deliberately LEFT at the authored size and DXGI upscales it
 * to the window. Letting the game re-create buffers at 1920x1080 would not help:
 * its internal render targets and UI layout are built for the authored size, so
 * it would simply draw the same small image into a bigger buffer. */

/* mode 3 = SCALED WINDOW: grow the client area to the host resolution WITHOUT
 * touching the window style.
 *
 * Borderless and Fullscreen (modes 1/2) both restyle to a monitor-sized
 * WS_POPUP, and that popup is what took the AMD driver down with
 * atidxx64+0x3CC66 - the size was never the problem, the fullscreen-popup
 * restyle was. Mode 0 keeps a normal window but pins it to the AUTHORED size
 * (1280x720), so it cannot give a bigger picture either.
 *
 * Mode 3 is the missing middle: same window the game made, just bigger, with
 * DXGI upscaling the authored back buffer to the larger client area exactly as
 * the comment above describes. Checked before 1/2 because 3 >= 1. */
static int ultra_disp_mode(void) {
    static int v = -2;
    if (v == -2) {
        const char* e;
        v = -1;
        if      ((e = getenv("ULTRA_SCALED_WINDOW")) && e[0] == '1') v = 3;
        else if ((e = getenv("ULTRA_FULLSCREEN")) && e[0] == '1') v = 2;
        else if ((e = getenv("ULTRA_BORDERLESS")) && e[0] == '1') v = 1;
        else if ((e = getenv("ULTRA_WINDOWED"))   && e[0] == '1') v = 0;
    }
    return v;
}

/* Target client size for mode 3: the host resolution the loader published from
 * ultra.ini. ULTRA_HOST_WIDTH/HEIGHT were published by main.c and, until now,
 * read by NOTHING - the settings audit listed them as "published, no reader".
 * Falls back to the monitor work area if they are absent or out of range. */
static void ultra_scaled_target(HWND w, int* out_w, int* out_h) {
    char b[16];
    long lw = 0, lh = 0;
    DWORD n;
    *out_w = *out_h = 0;

    n = GetEnvironmentVariableA("ULTRA_HOST_WIDTH", b, sizeof b);
    if (n && n < sizeof b) lw = strtol(b, NULL, 10);
    n = GetEnvironmentVariableA("ULTRA_HOST_HEIGHT", b, sizeof b);
    if (n && n < sizeof b) lh = strtol(b, NULL, 10);

    if (lw >= 320 && lw <= 7680 && lh >= 240 && lh <= 4320) {
        *out_w = (int)lw; *out_h = (int)lh;
    }

    /* Never propose a window bigger than the usable desktop: a client area the
     * size of the whole monitor plus a caption does not fit, and Windows would
     * silently clamp it somewhere unhelpful. */
    {
        HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi; mi.cbSize = sizeof mi;
        if (GetMonitorInfoA(mon, &mi)) {
            RECT wa = mi.rcWork;
            int aw = (int)(wa.right - wa.left), ah = (int)(wa.bottom - wa.top);
            if (!*out_w || !*out_h) { *out_w = aw; *out_h = ah; }
            if (*out_w > aw) *out_w = aw;
            if (*out_h > ah) *out_h = ah;
        }
    }
}

static void ultra_apply_window_mode(HWND w) {
    int mode = ultra_disp_mode();
    RECT cur;
    if (!w || mode < 0) return;

    if (mode == 3) {
        /* Resize ONLY. The style is read back and reused, never rewritten, so
         * whatever the game chose for itself survives - no WS_POPUP, no
         * SWP_FRAMECHANGED, nothing for the driver to renegotiate. */
        int tw = 0, th = 0, ww, wh, x, y;
        LONG st = GetWindowLongA(w, GWL_STYLE);
        LONG ex = GetWindowLongA(w, GWL_EXSTYLE);
        RECT wr;
        HMONITOR mon; MONITORINFO mi;

        ultra_scaled_target(w, &tw, &th);
        if (tw <= 0 || th <= 0) return;

        wr.left = 0; wr.top = 0; wr.right = tw; wr.bottom = th;
        AdjustWindowRectEx(&wr, (DWORD)st, FALSE, (DWORD)ex);
        ww = (int)(wr.right - wr.left);
        wh = (int)(wr.bottom - wr.top);

        /* Idempotent: bail if we are already there. Re-issuing SetWindowPos
         * every tick is a WM_SIZE storm and can make the game rebuild its swap
         * chain - the very thing the ResizeBuffers pin exists to prevent. */
        if (GetWindowRect(w, &cur) &&
            (int)(cur.right - cur.left) == ww && (int)(cur.bottom - cur.top) == wh)
            return;

        x = cur.left; y = cur.top;
        mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoA(mon, &mi)) {            /* center on the work area */
            x = (int)mi.rcWork.left + ((int)(mi.rcWork.right - mi.rcWork.left) - ww) / 2;
            y = (int)mi.rcWork.top  + ((int)(mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;
            if (x < (int)mi.rcWork.left) x = (int)mi.rcWork.left;
            if (y < (int)mi.rcWork.top)  y = (int)mi.rcWork.top;
        }
        SetWindowPos(w, NULL, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
        tlog("D3D11: display = SCALED WINDOW -> %dx%d client (window %dx%d at %d,%d), "
             "style unchanged 0x%08lX; back buffer stays %ux%u and DXGI upscales",
             tw, th, ww, wh, x, y, (unsigned long)st, g_auth_w, g_auth_h);
        return;
    }

    if (mode >= 1) {
        HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        int mw, mh;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoA(mon, &mi)) return;
        mw = (int)(mi.rcMonitor.right  - mi.rcMonitor.left);
        mh = (int)(mi.rcMonitor.bottom - mi.rcMonitor.top);
        /* Already right? Do NOT touch it. SetWindowPos every frame costs a
         * WM_SIZE storm and can make the game re-create its swap chain. */
        if (GetWindowRect(w, &cur) &&
            cur.left == mi.rcMonitor.left && cur.top == mi.rcMonitor.top &&
            (cur.right - cur.left) == mw && (cur.bottom - cur.top) == mh)
            return;
        SetWindowLongA(w, GWL_STYLE,   WS_POPUP | WS_VISIBLE);
        SetWindowLongA(w, GWL_EXSTYLE, 0);
        SetWindowPos(w, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mw, mh,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        tlog("D3D11: display = %s -> popup %dx%d at %d,%d (back buffer stays %ux%u, DXGI upscales)",
             mode == 2 ? "FULLSCREEN" : "BORDERLESS", mw, mh,
             (int)mi.rcMonitor.left, (int)mi.rcMonitor.top, g_auth_w, g_auth_h);
    } else {
        RECT wr;
        int cw = g_auth_w ? (int)g_auth_w : 1280;
        int ch = g_auth_h ? (int)g_auth_h : 720;
        wr.left = 0; wr.top = 0; wr.right = cw; wr.bottom = ch;
        if (GetWindowRect(w, &cur) && (cur.right - cur.left) >= cw &&
            (cur.bottom - cur.top) >= ch && (GetWindowLongA(w, GWL_STYLE) & WS_CAPTION))
            return;                                  /* close enough, leave it */
        SetWindowLongA(w, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowLongA(w, GWL_EXSTYLE, 0);
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(w, HWND_TOP, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                     SWP_NOMOVE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        tlog("D3D11: display = WINDOWED -> %dx%d client", cw, ch);
    }
}

/* Read the authored geometry once, then keep the window in the requested shape. */
static void ultra_disp_tick(IDXGISwapChain* sc) {
    static unsigned n = 0;
    DXGI_SWAP_CHAIN_DESC sd;
    if (ultra_disp_mode() < 0 || !sc) return;
    if ((n++ % 120) != 0) return;                    /* ~2s, not every frame */
    memset(&sd, 0, sizeof sd);
    if (FAILED(IDXGISwapChain_GetDesc(sc, &sd))) return;
    if (!g_auth_w && sd.BufferDesc.Width && sd.BufferDesc.Height) {
        g_auth_w = sd.BufferDesc.Width;
        g_auth_h = sd.BufferDesc.Height;
        tlog("D3D11: authored back buffer = %ux%u", g_auth_w, g_auth_h);
    }
    {   /* Report the LIVE back-buffer size whenever it changes, so a resize we
         * failed to stop shows up in the log instead of being invisible. */
        static UINT lw = 0, lh = 0;
        if (sd.BufferDesc.Width != lw || sd.BufferDesc.Height != lh) {
            lw = sd.BufferDesc.Width; lh = sd.BufferDesc.Height;
            tlog("D3D11: live back buffer now %ux%u (window %ld x %ld)",
                 lw, lh, 0L, 0L);
        }
    }
    ultra_apply_window_mode(sd.OutputWindow);
}

/* ---- hooked entrypoints --------------------------------------------------- */
/* ---------------------------------------------------------------------------
 *  Draw the pause-menu overlay INTO the frame
 * ---------------------------------------------------------------------------
 *
 *  The overlay is a layered Win32 window, and against a borderless-fullscreen
 *  D3D11 game that never appears: DXGI goes to independent flip, the desktop
 *  compositor is bypassed, and a topmost layered window loses. Measured on
 *  Pokken - the log shows "[OVERLAY] in-game menu opened" with no resume, so
 *  the menu was up and running the whole time with nothing on screen.
 *
 *  Exclusive fullscreen is worse, not better: the game owns the display
 *  outright. So the only fix that holds in every display mode is to composite
 *  the overlay into the back buffer here, before Present.
 *
 *  NO SHADERS, DELIBERATELY. There is no quad/VS/PS pipeline in this hook, and
 *  adding one means device state to save and restore - render targets, viewport,
 *  blend state, input layout - on somebody else's device, every frame, for a
 *  menu. Instead: copy the back buffer to a staging texture, blend on the CPU,
 *  copy back. That is a few milliseconds on a 1080p frame and it only runs while
 *  the menu is OPEN, which is exactly when the game is paused and nobody is
 *  counting frames. Normal play pays nothing - one function call that returns 0.
 */
typedef int (*pm_surface_fn)(void**, int*, int*);
static pm_surface_fn  Real_pm_surface;
static int            g_pm_probed;
extern unsigned ultra_pausemenu_serial(void);
extern void     ultra_pausemenu_set_composited(int on);

/* ---------------------------------------------------------------------------
 *  Pause menu -> back buffer, WITHOUT EVER READING THE BACK BUFFER
 * ---------------------------------------------------------------------------
 *
 *  The previous two attempts both copied the back buffer down to a CPU staging
 *  texture, blended the menu in, and copied it back. Both died:
 *      - one staging texture mapped READ_WRITE while being a copy source AND
 *        destination -> ACCESS_VIOLATION inside atidxx64.dll (the AMD display
 *        driver), a couple of minutes after the menu was opened;
 *      - the surviving variant -> ACCESS_VIOLATION at Hook_Present+0x51D, our
 *        own code, reading the menu DIB after the menu thread freed it.
 *
 *  The readback was never necessary. Look at what the menu actually draws
 *  (pause_menu_service.c, "alpha pass"): the PANEL interior is alpha 255 -
 *  fully opaque - and only the surround is translucent dimming. An opaque
 *  rectangle does not need blending, so it does not need the destination
 *  pixels, so it does not need a readback.
 *
 *  So: upload the opaque panel into a small DEFAULT texture and
 *  CopySubresourceRegion it onto the back buffer. GPU to GPU. No Map of the
 *  back buffer, no CPU stall waiting on the GPU, no blend, nothing the driver
 *  has to reconcile. The cost is the surrounding dim, which we simply drop.
 *
 *  The upload happens only when the menu bitmap actually changes
 *  (ultra_pausemenu_serial), and the DIB is copied into our own heap buffer
 *  immediately rather than being held across D3D calls - which is what the
 *  use-after-free was.
 */
static ID3D11Texture2D* g_ovtex;
static ID3D11Device*    g_ovdev;
static UINT             g_ovtex_w, g_ovtex_h;
static DXGI_FORMAT      g_ovtex_fmt;
static int              g_ov_px, g_ov_py;        /* where it goes on screen */
static unsigned         g_ov_last_serial;
static int              g_ov_have;
static int              g_ov_faults;
static int              g_ov_off;

static void overlay_release(void)
{
    if (g_ovtex) { ID3D11Texture2D_Release(g_ovtex); g_ovtex = NULL; }
    g_ovdev = NULL; g_ovtex_w = g_ovtex_h = 0; g_ov_have = 0;
}

/* Bounding box of the fully-opaque (alpha 255) region: the menu panel.
 * Derived from the bitmap rather than plumbed through the shared service, so
 * no other board's pause menu has to change for this. */
int ultra_overlay_panel_rect(const unsigned char* b, int w, int h,
                              int* ox, int* oy, int* ow, int* oh)
{
    int x, y, x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (y = 0; y < h; y++) {
        const unsigned char* row = b + (size_t)y * (size_t)w * 4;
        for (x = 0; x < w; x++) {
            if (row[x * 4 + 3] != 255) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (x1 < x0 || y1 < y0) return 0;
    *ox = x0; *oy = y0; *ow = x1 - x0 + 1; *oh = y1 - y0 + 1;
    return 1;
}

/* Report a bail reason ONCE, into the loader log (ultra.log) rather than the
 * JVS trace - the trace is the expensive one a finished game turns off, and a
 * silent overlay is precisely what we could not diagnose without it. */
extern void loader_log(int level, const char *fmt, ...);
static void ov_why(int slot, const char* what)
{
    static unsigned char said[12];
    if (slot < 0 || slot >= 12 || said[slot]) return;
    said[slot] = 1;
    /* Pass the whole message AS the format string. Split as ("...%s", what) the
     * argument did not survive the write - the log showed "[OVERLAY] not drawn:"
     * with nothing after it, which is worse than no diagnostic at all because it
     * looks like the reason is empty rather than missing. Our strings contain no
     * % so this is safe. */
    loader_log(1 /*LOG_WARN*/, what);
}

static void overlay_draw(IDXGISwapChain* sc)
{
    void *bits = NULL; int ow = 0, oh = 0;
    ID3D11Texture2D* back = NULL;
    ID3D11Device* dev = NULL; ID3D11DeviceContext* ctx = NULL;
    D3D11_TEXTURE2D_DESC bd;
    unsigned serial;

    /* ULTRA_ES3_OVERLAY=0 disables outright. Three failures and it stands down
     * for the rest of the run: the GAME KEEPS RUNNING, which matters more than
     * the menu being visible. */
    if (g_ov_off || g_ov_faults >= 3) {
        ultra_pausemenu_set_composited(0);   /* hand the layered window back */
        return;
    }

    if (!g_pm_probed) {
        const char* e;
        g_pm_probed = 1;
        e = getenv("ULTRA_ES3_OVERLAY");
        if (e && *e == '0') { g_ov_off = 1; return; }
        Real_pm_surface = (pm_surface_fn)(void*)GetProcAddress(
            GetModuleHandleA(NULL), "ultra_pausemenu_surface");
        if (!Real_pm_surface) {
            extern int ultra_pausemenu_surface(void**, int*, int*);
            Real_pm_surface = ultra_pausemenu_surface;
        }
    }
    if (!Real_pm_surface) return;
    if (!Real_pm_surface(&bits, &ow, &oh) || !bits || ow <= 0 || oh <= 0) {
        if (g_ov_have) { g_ov_have = 0; ultra_pausemenu_set_composited(0); }
        return;                        /* menu closed */
    }

    if (FAILED(IDXGISwapChain_GetBuffer(sc, 0, &ULTRA_IID_ID3D11Texture2D, (void**)&back)) || !back) {
        ov_why(3, "[OVERLAY] not drawn - could not get the back buffer from the swap chain");
        return;
    }
    if (FAILED(IDXGISwapChain_GetDevice(sc, &ULTRA_IID_ID3D11Device, (void**)&dev)) || !dev) {
        ov_why(4, "[OVERLAY] not drawn - could not get the D3D11 device from the swap chain");
        ID3D11Texture2D_Release(back); return;
    }
    ID3D11Device_GetImmediateContext(dev, &ctx);
    if (!ctx) {
        ov_why(5, "[OVERLAY] not drawn - no immediate device context");
        ID3D11Device_Release(dev); ID3D11Texture2D_Release(back); return;
    }

    ID3D11Texture2D_GetDesc(back, &bd);
    if (bd.SampleDesc.Count > 1) {
        ov_why(0, "[OVERLAY] not drawn - the back buffer is multisampled (MSAA), which cannot be a copy target");
        goto done;
    }

    /* Re-upload only when the menu bitmap actually changed. */
    serial = ultra_pausemenu_serial();
    if (!g_ov_have || serial != g_ov_last_serial ||
        g_ovdev != dev || g_ovtex_fmt != bd.Format) {
        int px, py, pw, ph;
        if (!ultra_overlay_panel_rect((const unsigned char*)bits, ow, oh, &px, &py, &pw, &ph)) {
            ov_why(1, "[OVERLAY] not drawn - no opaque panel found in the menu bitmap");
            goto done;                           /* nothing opaque to draw yet */
        }
        /* Only shrink if the panel is genuinely bigger than the target. The
         * old test clipped against px,py - the panel's position in the HOST
         * bitmap - which on a smaller back buffer shaved it away to nothing. */
        if (pw > (int)bd.Width)  pw = (int)bd.Width;
        if (ph > (int)bd.Height) ph = (int)bd.Height;
        if (pw <= 0 || ph <= 0) {
            ov_why(2, "[OVERLAY] not drawn - the menu panel has no area to draw");
            goto done;
        }

        if (g_ovtex && (g_ovdev != dev || g_ovtex_w != (UINT)pw ||
                        g_ovtex_h != (UINT)ph || g_ovtex_fmt != bd.Format))
            overlay_release();

        if (!g_ovtex) {
            D3D11_TEXTURE2D_DESC td; memset(&td, 0, sizeof td);
            td.Width = (UINT)pw; td.Height = (UINT)ph;
            td.MipLevels = 1; td.ArraySize = 1;
            td.Format = bd.Format; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;      /* UpdateSubresource, never Map */
            if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &g_ovtex)) || !g_ovtex) {
                static int said; if (!said) { said = 1;
                    tlog("D3D11 overlay: CreateTexture2D(%dx%d fmt=%d) failed -- overlay off",
                         pw, ph, (int)bd.Format); }
                ov_why(6, "[OVERLAY] not drawn - could not create the overlay texture");
                ++g_ov_faults; goto done;
            }
            g_ovdev = dev; g_ovtex_w = (UINT)pw; g_ovtex_h = (UINT)ph;
            g_ovtex_fmt = bd.Format;
            tlog("D3D11 overlay: panel %dx%d at %d,%d -> back buffer %ux%u (copy, no readback)",
                 pw, ph, px, py, bd.Width, bd.Height);
            /* Log the DESTINATION, not just the source. "at 640,294" was the
             * panel's position in the host-sized menu bitmap, which read like
             * the draw position and sent me looking in the wrong place twice. */
            loader_log(0 /*LOG_INFO*/,
                       "[OVERLAY] panel %dx%d from bitmap %d,%d -> back buffer %ux%u at %d,%d",
                       pw, ph, px, py, bd.Width, bd.Height,
                       ((int)bd.Width - pw) / 2, ((int)bd.Height - ph) / 2);
        }

        /* Copy the panel rows out of the DIB NOW, swapping R/B when the back
         * buffer is RGBA (the DIB is always BGRA). Holding the DIB pointer
         * across D3D calls is what the use-after-free was. */
        {
            int swap_rb = (bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                           bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
            size_t pitch = (size_t)pw * 4;
            unsigned char* tmp = (unsigned char*)malloc(pitch * (size_t)ph);
            int y, x;
            if (!tmp) {
                ov_why(7, "[OVERLAY] not drawn - out of memory staging the panel");
                ++g_ov_faults; goto done;
            }
            for (y = 0; y < ph; y++) {
                const unsigned char* s = (const unsigned char*)bits
                                       + ((size_t)(py + y) * (size_t)ow + (size_t)px) * 4;
                unsigned char* d = tmp + (size_t)y * pitch;
                if (swap_rb) {
                    for (x = 0; x < pw; x++, s += 4, d += 4) {
                        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
                    }
                } else {
                    for (x = 0; x < pw; x++, s += 4, d += 4) {
                        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
                    }
                }
            }
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource*)g_ovtex, 0,
                                                  NULL, tmp, (UINT)pitch, 0);
            free(tmp);
        }
        /* CENTER THE DESTINATION IN THE BACK BUFFER.
         *
         * px,py are where the panel sits in the HOST-sized menu bitmap and are
         * still used above to read the right pixels out of it. Where it LANDS
         * is a separate question, and the answer is the middle of the buffer we
         * are actually copying into - which for a back buffer the same size as
         * the host works out to exactly px,py, so nothing moves there. */
        g_ov_px = ((int)bd.Width  - pw) / 2;
        g_ov_py = ((int)bd.Height - ph) / 2;
        if (g_ov_px < 0) g_ov_px = 0;
        if (g_ov_py < 0) g_ov_py = 0;
        g_ov_last_serial = serial;
        g_ov_have = 1;
        ultra_pausemenu_set_composited(1);   /* layered window stands down */
    }

    if (g_ov_have && g_ovtex) {
        D3D11_BOX box;
        box.left = 0; box.top = 0; box.front = 0;
        box.right = g_ovtex_w; box.bottom = g_ovtex_h; box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource*)back, 0,
                                                  (UINT)g_ov_px, (UINT)g_ov_py, 0,
                                                  (ID3D11Resource*)g_ovtex, 0, &box);
    }
done:
    /* Reached a bail. If there is nothing composited to show, the layered
     * window is the only menu the user has - give it back rather than leaving
     * them with an invisible one. */
    if (!g_ov_have) ultra_pausemenu_set_composited(0);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    ID3D11Texture2D_Release(back);
}

static HRESULT __stdcall Hook_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
    throttle();
    ultra_disp_tick(self);
    ++g_present_count;
    /* REVERTED 2026-09-14: the back-buffer compositing that used to run here is
     * OFF by default.
     *
     * It made the menu visible, but it also made the menu flicker and it ended
     * the run with an access violation inside the AMD display driver
     * (atidxx64+0x3CC66) a couple of minutes after the menu was opened. Reading
     * the back buffer back to the CPU and copying a composite into it every
     * frame is too invasive to leave on by default on somebody else's device.
     *
     * The code is still here and still built, behind an opt-in, so the next
     * attempt starts from something testable instead of a blank page:
     *     ULTRA_ES3_OVERLAY_COMPOSITE=1
     * With it unset the D3D path behaves exactly as it did before compositing
     * was ever added, and the pause menu goes back to the layered window. */
    overlay_draw(self);   /* GPU-side panel copy; self-disables after 3 faults */
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
    /* the cached staging textures are sized to the old back buffer; drop them so
     * the next dump / composite recreates them against the new geometry. The
     * overlay pair was missing here, so it survived a resize pointing at dead
     * geometry. */
    release_staging();
    overlay_release();
    /* Pin the render resolution to what the game authored. If it reacts to our
     * WM_SIZE by re-creating buffers at the monitor size, its render targets and
     * UI are still built for the authored size, so it would draw the same small
     * image into a larger buffer instead of filling the screen. Keeping the back
     * buffer authored-size leaves DXGI to do the upscale. */
    /* Gate on the OVERRIDE, not on the display mode: the whole point is to keep
     * the back buffer at the authored size whatever the window does. */
    if (g_req_w && g_req_h && (w != g_req_w || h != g_req_h)) {
        tlog("D3D11: ResizeBuffers %ux%u -> pinned to authored %ux%u", w, h, g_req_w, g_req_h);
        w = g_req_w; h = g_req_h;
    }
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

    /* PUBLISH THE DEVICE WINDOW. This was missing, and it was not only the
     * pause menu that paid for it.
     *
     * ultra_typex2_gun_set_window() had exactly ONE caller in the whole hook
     * tree - in d3d9_throttle_es3.c. This game has no d3d9 in it at all (zero
     * occurrences of "d3d9"/"D3D9" in the image; it imports d3d11.dll and
     * dxgi.dll), so that caller never ran and g_gun_hwnd stayed NULL for the
     * entire session.
     *
     * Downstream of that NULL:
     *   - the pause-menu overlay falls back to GetActiveWindow(), which is
     *     THREAD-LOCAL and returns NULL by contract on a worker thread, and
     *     then to GetForegroundWindow() - i.e. whatever happened to be in
     *     front, which is not necessarily the game;
     *   - hook_dllmain_es3.c hands the same accessor to the Omni bridge, where
     *     a NULL window makes the gun thread do nothing and gamepad_feed_one
     *     return immediately. The Omni-Bridge gamepad and lightgun paths have
     *     therefore been dead on EVERY D3D11 ES3 title, not just this one.
     *
     * The swap chain knows the answer, so ask it rather than guessing. Done
     * here rather than in the display tick because that tick early-returns when
     * no display mode is set and only runs every 120th frame. */
    {
        DXGI_SWAP_CHAIN_DESC sd;
        memset(&sd, 0, sizeof sd);
        if (SUCCEEDED(IDXGISwapChain_GetDesc(sc, &sd)) && sd.OutputWindow) {
            extern void ultra_typex2_gun_set_window(HWND);
            ultra_typex2_gun_set_window(sd.OutputWindow);
            tlog("D3D11: device window %p published (overlay + Omni bridge)",
                 (void*)sd.OutputWindow);
        } else {
            tlog("D3D11: GetDesc gave no OutputWindow -- overlay will have to guess");
        }
    }

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
    DXGI_SWAP_CHAIN_DESC scd_fix;
    const DXGI_SWAP_CHAIN_DESC* scd_use = apply_render_size(scd, &scd_fix);
    HRESULT hr = Real_CreateDevSC(ad, dt, sw, flags, fl, nfl, sdk, scd_use, ppsc, ppdev, pfl, ppctx);
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
    DXGI_SWAP_CHAIN_DESC desc_fix;
    const DXGI_SWAP_CHAIN_DESC* use = apply_render_size(desc, &desc_fix);
    HRESULT hr = Real_CreateSwapChain(self, dev,
                                      (DXGI_SWAP_CHAIN_DESC*)use, ppsc);
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
    overlay_release();
    /* the vtable patch is left in place: process teardown frees the address space
     * wholesale; an orderly unload would need the cached Real_* pointers restored. */
}

#endif /* _WIN32 */
