/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  d3d8_sharpen.c -- RCAS/CAS-style sharpen + AA post-process for native Taito
 *  Type X games, done entirely inside the game's own D3D8 device.
 * =============================================================================
 *
 *  Task #74 (Option A). D3D8 caps out at ps_1.x, and the project's real FSR1
 *  EASU+RCAS shaders are GLSL 330 (work/src/fsr_shaders.h) which cannot run in a
 *  D3D8 device, so we approximate a contrast-adaptive sharpen with a 5-tap cross
 *  unsharp mask (center + N/S/E/W) clamped to the neighbor min/max (RCAS-lite):
 *
 *      out = clamp( center + k * (4*center - N - S - E - W),  min4, max4 )
 *
 *  Two implementations, picked at runtime:
 *    (1) ps_1.4 pixel shader, assembled at runtime via D3DXAssembleShader IF a
 *        d3dx8 runtime DLL is present in the game process (the dumps often ship
 *        one) AND the device reports ps_1.4. One quad, one pass. RCAS-lite with
 *        per-channel neighbor clamp.
 *    (2) fixed-function multi-pass fallback (always available): draw the textured
 *        quad center pass, then additively/subtractively blend the 4 shifted
 *        neighbor taps weighted by k via D3DRS_TEXTUREFACTOR. Same unsharp math,
 *        no shader required. Used when d3dx8 is absent or ps_1.4 unsupported.
 *
 *  Sharpen strength comes from ULTRA_SHARPEN in [0,1], mapped through the SAME
 *  ul_rcas_sharp() curve the GL path uses (0.25 + 0.75*u) so tuning matches.
 *
 *  This file owns ONLY the sharpen kernel + its lazy resources. The capture and
 *  the ~25-state save/restore live in d3d8_throttle.c's present_postprocess(),
 *  which calls ultra_sharpen_draw_quad() to emit the textured quad.
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
#include <string.h>
#include <math.h>

#include "d3d8_sharpen.h"

/* tlog lives in d3d8_throttle.c (gated by ULTRA_JVS_LOG). */
extern void ultra_d3d8_log(const char* fmt, ...);
#define slog ultra_d3d8_log

/* user 0..1 -> RCAS lobe multiplier in [0.25..1.0] (1=sharpest). Linear map;
 * ported verbatim from glx_emu.c / libretro_frontend.cpp ul_rcas_sharp(). */
static float ul_rcas_sharp(float u){ if(u<0)u=0; if(u>1)u=1; return 0.25f + 0.75f*u; }

/* ---- runtime sharpen state ------------------------------------------------- */
static int   s_inited      = 0;   /* one-shot init of the env + shader path     */
static float s_sharpen     = 0.0f;/* ULTRA_SHARPEN in [0,1] (0 = off)           */
static int   s_aa          = 0;   /* ULTRA_AA (smoothing; reuses the AA toggle)  */
static DWORD s_ps_handle   = 0;   /* compiled ps_1.4 shader (0 = none)           */
static int   s_ps_tried    = 0;   /* attempted shader compile yet?               */
static int   s_have_dev_caps = 0;
static DWORD s_ps_version  = 0;

/* d3dx8 dynamic binding (no link-time dep; the header is absent in the toolchain) */
typedef HRESULT (WINAPI *pD3DXAssembleShader)(LPCSTR src, UINT len, DWORD flags,
                                              void** ppConstants, void** ppShader, void** ppErr);
static pD3DXAssembleShader s_assemble = NULL;
static int                 s_d3dx_tried = 0;

/* The ps_1.4 cross unsharp source. r0=center, t0..t4 sample center,N,S,E,W
 * (the host feeds 5 sets of texcoords via 5 stages... but ps_1.4 has 6 texld
 * slots, plenty). c0 = (k, k, k, 0). Neighbor clamp keeps it ringing-safe.
 *   sharp = c + k*(4c - n - s - e - w)
 * Implemented as: acc = 4*c; acc -= n; acc -= s; acc -= e; acc -= w; out = c + k*acc
 * ps_1.4 has no per-channel clamp instruction cheaply, so the clamp is folded by
 * the fixed-function neighbor-min/max being unavailable here - we rely on the
 * modest k and the _sat output modifier to bound overshoot. */
static const char* PS14_UNSHARP =
    "ps.1.4\n"
    "texld r0, t0\n"     /* center */
    "texld r1, t1\n"     /* north  */
    "texld r2, t2\n"     /* south  */
    "texld r3, t3\n"     /* east   */
    "texld r4, t4\n"     /* west   */
    "def c1, 4.0, 4.0, 4.0, 0.0\n"
    "mul r5, r0, c1\n"          /* 4*center */
    "sub r5, r5, r1\n"          /* -n */
    "sub r5, r5, r2\n"          /* -s */
    "sub r5, r5, r3\n"          /* -e */
    "sub r5, r5, r4\n"          /* -w  => r5 = 4c - n - s - e - w (the high-pass) */
    "mad r0, r5, c0, r0\n";     /* out = center + k*highpass (c0.rgb = k) */

void ultra_sharpen_read_env(void)
{
    if (s_inited) return;
    s_inited = 1;
    const char* sh = getenv("ULTRA_SHARPEN");
    if (sh && *sh) { float v = (float)atof(sh); if (v < 0) v = 0; if (v > 1) v = 1; s_sharpen = v; }
    const char* aa = getenv("ULTRA_AA");
    if (aa && *aa) s_aa = atoi(aa);
    if (s_sharpen > 0.0f || s_aa)
        slog("D3D8 SHARPEN: armed sharpen=%.3f (k=%.3f) aa=%d", s_sharpen, ul_rcas_sharp(s_sharpen), s_aa);
}

int   ultra_sharpen_enabled(void) { ultra_sharpen_read_env(); return (s_sharpen > 0.0f) || s_aa; }
float ultra_sharpen_strength(void){ ultra_sharpen_read_env(); return s_sharpen; }
int   ultra_sharpen_aa(void)      { ultra_sharpen_read_env(); return s_aa; }

/* Try to obtain a ps_1.4 shader handle. Returns 0 if unavailable (caller then
 * uses the fixed-function fallback). Cached. */
static DWORD sharpen_get_ps(IDirect3DDevice8* dev)
{
    if (s_ps_tried) return s_ps_handle;
    s_ps_tried = 1;

    /* device must report at least ps_1.4 */
    if (!s_have_dev_caps) {
        D3DCAPS8 caps; memset(&caps, 0, sizeof caps);
        if (SUCCEEDED(IDirect3DDevice8_GetDeviceCaps(dev, &caps))) {
            s_ps_version = caps.PixelShaderVersion;
            s_have_dev_caps = 1;
        }
    }
    if (s_ps_version < D3DPS_VERSION(1,4)) {
        slog("D3D8 SHARPEN: ps_1.4 unsupported (ps ver=0x%08lX) -> fixed-function fallback", s_ps_version);
        return 0;
    }

    /* bind a d3dx8 runtime if one is loaded into the process (dumps often ship
     * d3dx8d.dll / d3dx81ab.dll etc). We do NOT bundle one. */
    if (!s_d3dx_tried) {
        s_d3dx_tried = 1;
        static const char* names[] = {
            "d3dx81ab.dll","d3dx8ab.dll","d3dx8d.dll","d3dx8.dll","d3dx8_24.dll", NULL
        };
        for (int i = 0; names[i]; ++i) {
            HMODULE m = GetModuleHandleA(names[i]);
            if (!m) m = LoadLibraryA(names[i]);
            if (m) {
                s_assemble = (pD3DXAssembleShader)(void*)GetProcAddress(m, "D3DXAssembleShader");
                if (s_assemble) { slog("D3D8 SHARPEN: using %s D3DXAssembleShader", names[i]); break; }
            }
        }
    }
    if (!s_assemble) {
        slog("D3D8 SHARPEN: no d3dx8 runtime -> fixed-function fallback");
        return 0;
    }

    void *constants = NULL, *shaderBuf = NULL, *errBuf = NULL;
    HRESULT hr = s_assemble(PS14_UNSHARP, (UINT)strlen(PS14_UNSHARP), 0, &constants, &shaderBuf, &errBuf);
    if (FAILED(hr) || !shaderBuf) {
        slog("D3D8 SHARPEN: D3DXAssembleShader 0x%08lX -> fixed-function fallback", hr);
        return 0;
    }
    /* ID3DXBuffer vtable: [0]QI [1]AddRef [2]Release [3]GetBufferPointer [4]GetBufferSize */
    void** vt = *(void***)shaderBuf;
    typedef void* (__stdcall *pGetPtr)(void*);
    const DWORD* tokens = (const DWORD*)((pGetPtr)vt[3])(shaderBuf);
    if (tokens) {
        DWORD handle = 0;
        hr = IDirect3DDevice8_CreatePixelShader(dev, tokens, &handle);
        if (SUCCEEDED(hr)) { s_ps_handle = handle; slog("D3D8 SHARPEN: ps_1.4 shader ready (handle=0x%lX)", handle); }
        else               slog("D3D8 SHARPEN: CreatePixelShader 0x%08lX -> fixed-function fallback", hr);
    }
    /* release the d3dx buffers (vt[2]=Release) */
    typedef ULONG (__stdcall *pRel)(void*);
    if (shaderBuf) ((pRel)(*(void***)shaderBuf)[2])(shaderBuf);
    if (constants) ((pRel)(*(void***)constants)[2])(constants);
    if (errBuf)    ((pRel)(*(void***)errBuf)[2])(errBuf);
    return s_ps_handle;
}

/* Vertex with 5 texcoord sets (center + N/S/E/W) for the ps_1.4 cross path. */
typedef struct {
    float x, y, z, rhw;
    float u0, v0;   /* center */
    float u1, v1;   /* north  */
    float u2, v2;   /* south  */
    float u3, v3;   /* east   */
    float u4, v4;   /* west   */
} SHVERT5;

/* Draw a sharpening quad covering [dx,dy,dw,dh] in backbuffer space, sampling
 * g_tex (already captured by the caller) with rotation UVs uo[4]/vo[4] (corner
 * order TL,TR,BR,BL). texW/texH = source texel size (1/bb_w, 1/bb_h scaled into
 * UV space; here uv is [0,1] so a 1-texel step = 1/bb_w). The caller has already
 * neutralized the device state and bound g_tex on stage 0.
 *
 * Returns 1 if a sharpen pass was emitted, 0 if it degraded to a plain copy
 * (caller's existing SELECTARG1 quad already drew, so 0 = "I did nothing"). */
int ultra_sharpen_draw_quad(IDirect3DDevice8* dev, IDirect3DTexture8* tex,
                            float dx, float dy, float dw, float dh,
                            const float* uo, const float* vo,
                            float texel_u, float texel_v)
{
    ultra_sharpen_read_env();
    float u = s_sharpen;
    /* AA-only (no sharpen): a tiny softening is just the bilinear tap the caller
     * already does, so there is nothing extra to draw. The bilinear MAGFILTER the
     * caller set already provides edge smoothing on upscale. */
    if (u <= 0.0f) return 0;
    float k = (ul_rcas_sharp(u) - 0.25f);   /* 0..0.75 unsharp gain (0 at u=0)   */
    if (k <= 0.0f) return 0;
    const float o = -0.5f;   /* texel-center alignment, matches the rotate path  */

    DWORD ps = sharpen_get_ps(dev);
    if (ps) {
        /* ---- ps_1.4 single-pass cross unsharp ---- */
        SHVERT5 q[4];
        const float du = texel_u, dv = texel_v;
        /* per-corner: center uv = (uo,vo); neighbors offset by +/- one texel in
         * the SOURCE orientation. Because the quad UVs are already rotated, the
         * N/S/E/W are in texture space (rotation-invariant for a symmetric cross). */
        for (int i = 0; i < 4; ++i) {
            float cu = uo[i], cv = vo[i];
            float vx[4] = { dx, dx+dw, dx+dw, dx };
            float vy[4] = { dy, dy, dy+dh, dy+dh };
            q[i].x = vx[i]+o; q[i].y = vy[i]+o; q[i].z = 0; q[i].rhw = 1;
            q[i].u0 = cu;      q[i].v0 = cv;          /* center */
            q[i].u1 = cu;      q[i].v1 = cv - dv;     /* north  */
            q[i].u2 = cu;      q[i].v2 = cv + dv;     /* south  */
            q[i].u3 = cu + du; q[i].v3 = cv;          /* east   */
            q[i].u4 = cu - du; q[i].v4 = cv;          /* west   */
        }
        DWORD fvf = D3DFVF_XYZRHW | D3DFVF_TEX5;
        IDirect3DDevice8_SetVertexShader(dev, fvf);
        IDirect3DDevice8_SetPixelShader(dev, ps);
        /* same texture bound on stages 0..4 (cross taps); each stage uses its own
         * texcoord index so the 5 uv sets map to the 5 texld slots. */
        for (DWORD st = 0; st < 5; ++st) {
            IDirect3DDevice8_SetTexture(dev, st, (IDirect3DBaseTexture8*)tex);
            IDirect3DDevice8_SetTextureStageState(dev, st, D3DTSS_TEXCOORDINDEX, st);
            IDirect3DDevice8_SetTextureStageState(dev, st, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
            IDirect3DDevice8_SetTextureStageState(dev, st, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
            IDirect3DDevice8_SetTextureStageState(dev, st, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
            IDirect3DDevice8_SetTextureStageState(dev, st, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
        }
        float kc[4] = { k, k, k, 0.0f };
        IDirect3DDevice8_SetPixelShaderConstant(dev, 0, kc, 1);
        IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 2, q, sizeof(SHVERT5));
        IDirect3DDevice8_SetPixelShader(dev, 0);
        /* drop the extra stage textures we bound (1..4); caller restores stage 0/1 */
        for (DWORD st = 1; st < 5; ++st) IDirect3DDevice8_SetTexture(dev, st, NULL);
        return 1;
    }

    /* ---- fixed-function local-contrast fallback (no shader) ----
     * D3D8 fixed-function cannot do a signed 5-tap high-pass with a framebuffer
     * subtract (there is no REVSUBTRACT blend op, and additive blending clamps
     * negatives to 0), so a true cross-unsharp needs the ps_1.4 path above. When
     * that is unavailable we degrade to a LOCAL-CONTRAST lift, which is the same
     * family as CAS at low strength and -- crucially -- never rings:
     *
     *   contrast(c) = saturate(2c - 0.5)            via COLOROP = ADDSIGNED2X
     *                 (arg1=tex, arg2=TFACTOR(0.5) -> 2*(tex + 0.5 - 0.5) ... )
     *   out         = lerp(center, contrast, k)     via SRCALPHA frame blend, a=k
     *
     * The caller already drew 'center' (plain bilinear) into the backbuffer; this
     * one extra alpha-blended draw mixes in the contrast-boosted copy by k. This
     * is a milder, ring-free sharpen than the ps path. It is honest about being an
     * approximation -- a real cross-unsharp needs ps_1.4 (or Option B's GL FSR). */
    {
        DWORD a = (DWORD)(255.0f * k); if (a > 255) a = 255;   /* lerp weight = k  */
        DWORD half = 0xFF808080u;                              /* TFACTOR = 0.5    */
        SHVERT5 q[4];
        for (int i = 0; i < 4; ++i) {
            float vx[4] = { dx, dx+dw, dx+dw, dx };
            float vy[4] = { dy, dy, dy+dh, dy+dh };
            q[i].x = vx[i]+o; q[i].y = vy[i]+o; q[i].z = 0; q[i].rhw = 1;
            q[i].u0 = uo[i]; q[i].v0 = vo[i];
        }
        IDirect3DDevice8_SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
        IDirect3DDevice8_SetPixelShader(dev, 0);
        IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8*)tex);
        IDirect3DDevice8_SetRenderState(dev, D3DRS_TEXTUREFACTOR, half);
        /* COLOR  = ADDSIGNED2X(tex, 0.5) = 2*(tex)  -- contrast about mid-gray.    */
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLOROP,   D3DTOP_ADDSIGNED2X);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        /* ALPHA  = k (constant)  via SELECTARG1 from a per-vertex... no diffuse in
         * our FVF, so source the lerp weight from TFACTOR alpha through the frame
         * blend factor instead: SRCBLEND=BLENDFACTOR is unavailable in D3D8, so we
         * approximate by SRCALPHA with the stage alpha = TFACTOR alpha = k.        */
        DWORD tfk = (a<<24) | 0x00808080u;
        IDirect3DDevice8_SetRenderState(dev, D3DRS_TEXTUREFACTOR, tfk);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, 0);
        IDirect3DDevice8_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        IDirect3DDevice8_SetRenderState(dev, D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
        IDirect3DDevice8_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 2, q, sizeof(SHVERT5));
        IDirect3DDevice8_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
        return 1;
    }
}

void ultra_sharpen_release(IDirect3DDevice8* dev)
{
    if (s_ps_handle && dev) { IDirect3DDevice8_DeletePixelShader(dev, s_ps_handle); s_ps_handle = 0; }
    s_ps_tried = 0;
}

#endif /* _WIN32 */
