/* SPDX-License-Identifier: MIT
 * d3d8_sharpen.h -- RCAS/CAS-style sharpen post-process for native Type X games.
 * See d3d8_sharpen.c. Consumed by d3d8_throttle.c's present_postprocess().
 */
#ifndef ULTRA_D3D8_SHARPEN_H
#define ULTRA_D3D8_SHARPEN_H
#if defined(_WIN32)

#include <windows.h>
#define DIRECT3D_VERSION 0x0800
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read ULTRA_SHARPEN / ULTRA_AA once (idempotent). */
void  ultra_sharpen_read_env(void);
/* 1 if sharpen or AA is requested (sharpen>0 || aa). */
int   ultra_sharpen_enabled(void);
/* ULTRA_SHARPEN in [0,1] (0 = off). */
float ultra_sharpen_strength(void);
/* ULTRA_AA flag (edge smoothing toggle, shared with the GL path). */
int   ultra_sharpen_aa(void);

/* Emit the sharpening quad over [dx,dy,dw,dh] (backbuffer space) sampling `tex`
 * with rotation UVs uo[4]/vo[4] (corner order TL,TR,BR,BL). texel_u/texel_v are
 * one source texel in UV units (1/bb_w, 1/bb_h). The caller MUST have captured
 * the frame into `tex`, set the RT to the backbuffer, neutralized device state,
 * and be inside BeginScene/EndScene. Returns 1 if a sharpen pass was emitted, 0
 * if it declined (sharpen disabled) so the caller draws its plain quad instead. */
int   ultra_sharpen_draw_quad(IDirect3DDevice8* dev, IDirect3DTexture8* tex,
                              float dx, float dy, float dw, float dh,
                              const float* uo, const float* vo,
                              float texel_u, float texel_v);

/* Free the cached pixel shader (call on device teardown if ever needed). */
void  ultra_sharpen_release(IDirect3DDevice8* dev);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif /* ULTRA_D3D8_SHARPEN_H */
