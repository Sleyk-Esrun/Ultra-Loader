/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  test_d3d11_capture.c -- synthetic D3D11 present app that proves the
 *  d3d11_throttle.c capture path works end-to-end WITHOUT a real arcade dump.
 * =============================================================================
 *
 *  We can't run a Nu/ALLS title (no dump, keychip-gated). But the capture path --
 *  swap-chain Present hook -> GetBuffer(0) -> STAGING CopyResource -> Map -> BMP --
 *  is API-generic, so a tiny synthetic title exercises every line of it:
 *
 *    1. Create a real D3D11 device + DXGI swap chain on a hidden HWND. We call the
 *       loader's OWN ultra_Hook_D3D11CreateDeviceAndSwapChain (the canonical real
 *       hook the IAT/GetProcAddress route to) so the returned swap chain's vtable
 *       is patched exactly as it would be in a hooked game.
 *    2. Clear the back buffer to a known color.
 *    3. Call swap_chain->Present(...). Because the vtable is hooked, this lands in
 *       Hook_Present, which (ULTRA_PRESENT_DUMP set, ULTRA_DUMP_EVERY=1) copies the
 *       back buffer into the staging texture and writes a BMP.
 *    4. Assert the BMP exists, is the right size, and its top-left pixel matches the
 *       clear color -- proving the GPU->staging->disk round-trip captured the frame.
 *
 *  Uses the WARP software rasterizer (D3D_DRIVER_TYPE_WARP) so it runs on a headless
 *  CI box with no GPU. Exit 0 = capture verified; non-zero = failure.
 * =============================================================================
 */
#include <windows.h>
#define COBJMACROS
#include <initguid.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DEFINE_GUID(TST_IID_ID3D11Texture2D, 0x6f15aaf2, 0xd208, 0x4e89,
            0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c);

/* the canonical real hook from d3d11_throttle.c (loose-typed extern matches the ABI) */
extern HRESULT __stdcall ultra_Hook_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
    ID3D11DeviceContext**);

/* the logger the hook links against (env-gated; print nothing unless ULTRA_JVS_LOG) */
void ultra_d3d8_log(const char* fmt, ...) {
    const char* p = getenv("ULTRA_JVS_LOG"); if (!p || !*p) return;
    FILE* f = fopen(p, "a"); if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}
/* the IAT patcher the hook's install path calls -- a no-op stub for the standalone
 * test (we drive the create hook directly, no game IAT to walk). */
void ultra_iat_patch_all(const char* a, const char* b, void* c) { (void)a;(void)b;(void)c; }

#define W 256
#define H 128

int main(void) {
    char tmp[MAX_PATH]; GetTempPathA(sizeof tmp, tmp);
    char dir[MAX_PATH + 32]; snprintf(dir, sizeof dir, "%sul_d3d11_cap", tmp);
    CreateDirectoryA(dir, NULL);
    /* arm the capture path: dump every present into dir */
    SetEnvironmentVariableA("ULTRA_PRESENT_DUMP", dir);
    SetEnvironmentVariableA("ULTRA_DUMP_EVERY", "1");
    _putenv_s("ULTRA_PRESENT_DUMP", dir);
    _putenv_s("ULTRA_DUMP_EVERY", "1");

    /* hidden HWND for the swap chain */
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "ul_d3d11_test";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("ul_d3d11_test", "ul", WS_OVERLAPPEDWINDOW,
                              0, 0, W, H, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("FAIL: CreateWindow\n"); return 2; }

    DXGI_SWAP_CHAIN_DESC scd; memset(&scd, 0, sizeof scd);
    scd.BufferCount = 1;
    scd.BufferDesc.Width = W; scd.BufferDesc.Height = H;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = NULL; ID3D11Device* dev = NULL; ID3D11DeviceContext* ctx = NULL;
    D3D_FEATURE_LEVEL got;
    /* drive the loader's OWN create hook so the returned swap chain is vtable-hooked */
    HRESULT hr = ultra_Hook_D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
        &scd, &sc, &dev, &got, &ctx);
    if (FAILED(hr) || !sc || !dev || !ctx) {
        printf("FAIL: create device+swapchain 0x%08lX\n", hr); return 3;
    }

    /* RTV on back buffer 0, clear to a known color (R=0.20 G=0.40 B=0.80) */
    ID3D11Texture2D* back = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(sc, 0, &TST_IID_ID3D11Texture2D, (void**)&back)) || !back) {
        printf("FAIL: GetBuffer\n"); return 4;
    }
    ID3D11RenderTargetView* rtv = NULL;
    if (FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)back, NULL, &rtv)) || !rtv) {
        printf("FAIL: CreateRTV\n"); return 5;
    }
    const float col[4] = { 0.20f, 0.40f, 0.80f, 1.0f };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, col);
    ID3D11DeviceContext_Flush(ctx);

    /* Present -> Hook_Present -> capture -> BMP (ULTRA_DUMP_EVERY=1 => dumps now) */
    hr = IDXGISwapChain_Present(sc, 0, 0);
    if (FAILED(hr)) { printf("FAIL: Present 0x%08lX\n", hr); return 6; }

    /* the hook names the file "<dir>/d3d11_0001.bmp" (present count == 1) */
    char bmp[MAX_PATH + 64]; snprintf(bmp, sizeof bmp, "%s/d3d11_0001.bmp", dir);
    FILE* f = fopen(bmp, "rb");
    if (!f) { printf("FAIL: no BMP at %s (capture did not fire)\n", bmp); return 7; }
    unsigned char fh[14], ih[40];
    if (fread(fh,1,14,f) != 14 || fread(ih,1,40,f) != 40) { printf("FAIL: BMP header short\n"); fclose(f); return 8; }
    int bw = *(int*)(ih+4), bh = *(int*)(ih+8);
    unsigned short bpp = *(unsigned short*)(ih+14);
    if (fh[0] != 'B' || fh[1] != 'M' || bw != W || bh != H || bpp != 24) {
        printf("FAIL: BMP geometry %dx%d bpp=%u (want %dx%d 24)\n", bw, bh, bpp, W, H); fclose(f); return 9;
    }
    /* bottom-up 24-bit: the LAST row in the file is image-top. Read top-left pixel
     * (= B8G8R8A8 clear) and check it's our color (B~204 G~102 R~51, +/- rounding). */
    int rowsz = W*3, pad = (4-(rowsz&3))&3, stride = rowsz+pad;
    long dataoff = 54 + (long)(H-1)*stride;
    fseek(f, dataoff, SEEK_SET);
    unsigned char px[3] = {0,0,0};
    if (fread(px,1,3,f) != 3) { printf("FAIL: BMP pixel read\n"); fclose(f); return 10; }
    fclose(f);
    int eb = (int)(0.80f*255+0.5f), eg = (int)(0.40f*255+0.5f), er = (int)(0.20f*255+0.5f);
    int db = abs((int)px[0]-eb), dg = abs((int)px[1]-eg), dr = abs((int)px[2]-er);
    /* WARP + sRGB-ish rounding tolerance */
    if (db > 12 || dg > 12 || dr > 12) {
        printf("FAIL: pixel BGR=(%u,%u,%u) want ~(%d,%d,%d)\n", px[0],px[1],px[2], eb,eg,er);
        return 11;
    }

    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(back);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    IDXGISwapChain_Release(sc);
    DestroyWindow(hwnd);

    printf("PASS: d3d11 capture %dx%d 24bpp, top-left BGR=(%u,%u,%u) == clear color\n",
           bw, bh, px[0], px[1], px[2]);
    return 0;
}
