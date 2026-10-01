/* SPDX-License-Identifier: MIT
 * test_sharpen_offline.c -- headless offline verification of the D3D8 sharpen
 * kernel (task #74). Creates a hidden-window D3D8 device (HAL, falling back to
 * REF), uploads a synthetic checkerboard into an RT texture, runs the real
 * ultra_sharpen_draw_quad() over a second RT, and writes before/after 24bpp BMPs.
 *
 * This proves the pass EXECUTES and the orientation/UVs are correct without a
 * real Type X dump. It exercises the fixed-function fallback (no d3dx8 here);
 * the ps_1.4 path needs a d3dx8 runtime which a real game dump supplies.
 *
 * Build (32-bit, with a Windows TMP set):
 *   gcc -O2 -std=c11 -o test_sharpen_offline.exe test_sharpen_offline.c \
 *       d3d8_sharpen.c -ld3d8 -luser32 -lgdi32
 * (ultra_d3d8_log is stubbed locally so we don't pull in d3d8_throttle.c.)
 */
#include <windows.h>
#define DIRECT3D_VERSION 0x0800
#define COBJMACROS
#include <d3d8.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d8_sharpen.h"

/* the sharpen module logs through this extern (normally in d3d8_throttle.c) */
void ultra_d3d8_log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n');
}

#define W 256
#define H 192

static void dump_bmp(IDirect3DDevice8* dev, IDirect3DSurface8* surf, const char* path) {
    D3DSURFACE_DESC d; if (FAILED(IDirect3DSurface8_GetDesc(surf, &d))) return;
    IDirect3DSurface8* sys = NULL;
    if (FAILED(IDirect3DDevice8_CreateImageSurface(dev, d.Width, d.Height, d.Format, &sys))) return;
    if (FAILED(IDirect3DDevice8_CopyRects(dev, surf, NULL, 0, sys, NULL))) { IDirect3DSurface8_Release(sys); return; }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(IDirect3DSurface8_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        int w=(int)d.Width, h=(int)d.Height, rowsz=w*3, pad=(4-(rowsz&3))&3, stride=rowsz+pad, imgsz=stride*h;
        unsigned char fh[14]={0}, ih[40]={0}; unsigned filesz=14+40+imgsz;
        fh[0]='B'; fh[1]='M'; *(unsigned*)(fh+2)=filesz; *(unsigned*)(fh+10)=54;
        *(unsigned*)(ih+0)=40; *(int*)(ih+4)=w; *(int*)(ih+8)=h;
        *(unsigned short*)(ih+12)=1; *(unsigned short*)(ih+14)=24; *(unsigned*)(ih+20)=imgsz;
        FILE* f=fopen(path,"wb");
        if(f){ fwrite(fh,1,14,f); fwrite(ih,1,40,f); unsigned char* row=malloc(stride);
            for(int y=h-1;y>=0;--y){ unsigned char* src=(unsigned char*)lr.pBits+y*lr.Pitch; memset(row,0,stride);
                for(int x=0;x<w;++x){ row[x*3+0]=src[x*4+0]; row[x*3+1]=src[x*4+1]; row[x*3+2]=src[x*4+2]; }
                fwrite(row,1,stride,f);} free(row); fclose(f); printf("wrote %s (%dx%d)\n",path,w,h);}
        IDirect3DSurface8_UnlockRect(sys);
    }
    IDirect3DSurface8_Release(sys);
}

/* fill a lockable (MANAGED) texture with a checkerboard. Hard edges = ideal
 * sharpen test. Returns the texture (MANAGED so we can LockRect directly). */
static IDirect3DTexture8* make_checker(IDirect3DDevice8* dev) {
    IDirect3DTexture8* tex=NULL;
    HRESULT hr = IDirect3DDevice8_CreateTexture(dev,W,H,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&tex);
    if (FAILED(hr) || !tex) { printf("CreateTexture(managed checker) 0x%08lX\n",hr); return NULL; }
    IDirect3DSurface8* ts=NULL;
    if (FAILED(IDirect3DTexture8_GetSurfaceLevel(tex,0,&ts))) { return tex; }
    D3DLOCKED_RECT lr;
    HRESULT hl = IDirect3DSurface8_LockRect(ts, &lr, NULL, 0);
    if (SUCCEEDED(hl)) {
        for (int y=0;y<H;y++){ unsigned* p=(unsigned*)((char*)lr.pBits+y*lr.Pitch);
            for (int x=0;x<W;x++){ int c=((x>>4)+(y>>4))&1; p[x]= c?0xFFFFFFFFu:0xFF202020u; } }
        IDirect3DSurface8_UnlockRect(ts);
    } else printf("checker LockRect 0x%08lX\n",hl);
    IDirect3DSurface8_Release(ts);
    return tex;
}

int main(void) {
    /* use _putenv (CRT env) not SetEnvironmentVariableA: mingw getenv() snapshots
     * the CRT env and does NOT see a Win32 SetEnvironmentVariableA done in-process.
     * In the real pipeline the loader sets it BEFORE CreateProcess, so the game's
     * getenv inherits it correctly -- this just mirrors that for the harness. */
    _putenv("ULTRA_SHARPEN=0.6");
    IDirect3D8* d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d) { printf("Direct3DCreate8 failed\n"); return 2; }

    WNDCLASSA wc={0}; wc.lpfnWndProc=DefWindowProcA; wc.hInstance=GetModuleHandleA(NULL);
    wc.lpszClassName="ULSharpTest"; RegisterClassA(&wc);
    HWND hwnd=CreateWindowA("ULSharpTest","t",WS_OVERLAPPEDWINDOW,0,0,W,H,NULL,NULL,wc.hInstance,NULL);

    D3DPRESENT_PARAMETERS pp; memset(&pp,0,sizeof pp);
    pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat=D3DFMT_X8R8G8B8; pp.BackBufferWidth=W; pp.BackBufferHeight=H;
    pp.hDeviceWindow=hwnd;

    IDirect3DDevice8* dev=NULL;
    HRESULT hr = IDirect3D8_CreateDevice(d3d,D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,hwnd,
                    D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&dev);
    if (FAILED(hr)) {
        printf("HAL CreateDevice 0x%08lX, trying REF...\n",hr);
        hr = IDirect3D8_CreateDevice(d3d,D3DADAPTER_DEFAULT,D3DDEVTYPE_REF,hwnd,
                    D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&dev);
    }
    if (FAILED(hr)) { printf("CreateDevice failed 0x%08lX (no headless D3D8 device available)\n",hr); return 3; }
    printf("D3D8 device created\n");

    /* source checkerboard (MANAGED, lockable) + two output RTs (the 'backbuffer') */
    IDirect3DTexture8* src = make_checker(dev);
    IDirect3DTexture8 *outA=NULL, *outB=NULL, *scratch=NULL;
    IDirect3DDevice8_CreateTexture(dev,W,H,1,D3DUSAGE_RENDERTARGET,D3DFMT_X8R8G8B8,D3DPOOL_DEFAULT,&outA);
    IDirect3DDevice8_CreateTexture(dev,W,H,1,D3DUSAGE_RENDERTARGET,D3DFMT_X8R8G8B8,D3DPOOL_DEFAULT,&outB);
    IDirect3DDevice8_CreateTexture(dev,64,64,1,D3DUSAGE_RENDERTARGET,D3DFMT_X8R8G8B8,D3DPOOL_DEFAULT,&scratch);
    if(!src||!outA||!outB||!scratch){ printf("CreateTexture(RT) failed\n"); return 4; }

    IDirect3DSurface8 *outASurf=NULL,*outBSurf=NULL,*scratchSurf=NULL;
    IDirect3DTexture8_GetSurfaceLevel(outA,0,&outASurf);
    IDirect3DTexture8_GetSurfaceLevel(outB,0,&outBSurf);
    IDirect3DTexture8_GetSurfaceLevel(scratch,0,&scratchSurf);

    typedef struct { float x,y,z,rhw,u,v; } RV;
    const float o=-0.5f;
    float u[4]={0,1,1,0}, v[4]={0,0,1,1};  /* identity UVs (no rotation), TL,TR,BR,BL */

    /* helper macro: bind the plain-copy state for a quad sampling `src` */
    #define PLAIN_STATE() do { \
        IDirect3DDevice8_SetVertexShader(dev, D3DFVF_XYZRHW|D3DFVF_TEX1); \
        IDirect3DDevice8_SetPixelShader(dev, 0); \
        IDirect3DDevice8_SetRenderState(dev,D3DRS_LIGHTING,FALSE); \
        IDirect3DDevice8_SetRenderState(dev,D3DRS_ZENABLE,FALSE); \
        IDirect3DDevice8_SetRenderState(dev,D3DRS_CULLMODE,D3DCULL_NONE); \
        IDirect3DDevice8_SetRenderState(dev,D3DRS_ALPHABLENDENABLE,FALSE); \
        IDirect3DDevice8_SetTexture(dev,0,(IDirect3DBaseTexture8*)src); \
        IDirect3DDevice8_SetTextureStageState(dev,0,D3DTSS_COLOROP,D3DTOP_SELECTARG1); \
        IDirect3DDevice8_SetTextureStageState(dev,0,D3DTSS_COLORARG1,D3DTA_TEXTURE); \
        IDirect3DDevice8_SetTextureStageState(dev,0,D3DTSS_ALPHAOP,D3DTOP_DISABLE); \
        IDirect3DDevice8_SetTextureStageState(dev,0,D3DTSS_MAGFILTER,D3DTEXF_POINT); \
        IDirect3DDevice8_SetTextureStageState(dev,0,D3DTSS_MINFILTER,D3DTEXF_POINT); \
        IDirect3DDevice8_SetTextureStageState(dev,0,D3DTSS_ADDRESSU,D3DTADDRESS_CLAMP); \
        IDirect3DDevice8_SetTextureStageState(dev,0,D3DTSS_ADDRESSV,D3DTADDRESS_CLAMP); \
        IDirect3DDevice8_SetTextureStageState(dev,0,D3DTSS_TEXCOORDINDEX,0); \
    } while(0)

    RV q[4]={ {0+o,0+o,0,1,0,0},{W+o,0+o,0,1,1,0},{W+o,H+o,0,1,1,1},{0+o,H+o,0,1,0,1} };

    /* ---- BEFORE: plain copy of the checker into outA ---- */
    IDirect3DDevice8_SetRenderTarget(dev, outASurf, NULL);
    IDirect3DDevice8_Clear(dev,0,NULL,D3DCLEAR_TARGET,0xFF000000,1.0f,0);
    if (SUCCEEDED(IDirect3DDevice8_BeginScene(dev))) {
        PLAIN_STATE();
        IDirect3DDevice8_DrawPrimitiveUP(dev,D3DPT_TRIANGLEFAN,2,q,sizeof(RV));
        IDirect3DDevice8_EndScene(dev);
    }
    IDirect3DDevice8_SetRenderTarget(dev, scratchSurf, NULL);   /* free outA to read */
    dump_bmp(dev, outASurf, "sharp_before.bmp");

    /* ---- AFTER: plain copy + the real sharpen pass into outB ---- */
    IDirect3DDevice8_SetRenderTarget(dev, outBSurf, NULL);
    IDirect3DDevice8_Clear(dev,0,NULL,D3DCLEAR_TARGET,0xFF000000,1.0f,0);
    if (SUCCEEDED(IDirect3DDevice8_BeginScene(dev))) {
        PLAIN_STATE();
        IDirect3DDevice8_DrawPrimitiveUP(dev,D3DPT_TRIANGLEFAN,2,q,sizeof(RV));  /* base */
        int did = ultra_sharpen_draw_quad(dev, src, 0,0,(float)W,(float)H, u,v, 1.0f/W, 1.0f/H);
        printf("ultra_sharpen_draw_quad returned %d (1=pass emitted)\n", did);
        IDirect3DDevice8_EndScene(dev);
    }
    IDirect3DDevice8_SetRenderTarget(dev, scratchSurf, NULL);
    dump_bmp(dev, outBSurf, "sharp_after.bmp");

    printf("DONE. Compare sharp_before.bmp vs sharp_after.bmp.\n");
    return 0;
}
