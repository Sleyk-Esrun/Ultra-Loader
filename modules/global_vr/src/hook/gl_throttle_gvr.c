/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  gl_throttle_gvr.c -- OpenGL frame limiter for Global VR (gdi32/opengl32 path)
 * =============================================================================
 *
 *  Global VR PC titles (Aliens Extermination, Big Buck Hunter Safari, etc.)
 *  render through OpenGL + GLUT rather than D3D9. GLUT calls gdi32!SwapBuffers
 *  (some titles call opengl32!wglSwapBuffers directly); this file hooks both so
 *  the frame limiter catches every title regardless of which call it uses.
 *
 *  Also tracks the render window from the HDC passed to SwapBuffers -- needed
 *  by the Omni-Bridge and the pause menu overlay, both of which need a HWND to
 *  target their SendInput/SendMessage calls.
 *
 *  INDEPENDENT COPY -- no code shared with any other board's hook tree.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "global_vr_hook.h"

#define glog(fmt, ...) ultra_gvr_log("[gvr-gl] " fmt, ##__VA_ARGS__)

/* ---- window tracking ------------------------------------------------------- */
static HWND g_gl_wnd = NULL;

static void track_window(HDC hdc) {
    if (g_gl_wnd || !hdc) return;
    HWND w = WindowFromDC(hdc);
    if (!w) return;
    g_gl_wnd = w;
    glog("render window = %p", (void*)w);
    /* Dismiss the Ultra Loader splash now that the game has a window up. */
    HWND sp = FindWindowA("UltraLoaderSplash", NULL);
    if (sp) { ShowWindow(sp, SW_HIDE); glog("dismissed loader splash"); }
    SetForegroundWindow(w);
}

HWND ultra_gvr_gl_window(void) { return g_gl_wnd; }

/* ---- QPC frame limiter ---------------------------------------------------- */
static double        g_fps   = 60.0;
static LARGE_INTEGER g_freq, g_last;
static int           g_tinit = 0;

static void gl_throttle(void) {
    if (!g_tinit) {
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_last);
        const char* e = getenv("ULTRA_FPS_CAP");
        if (e && *e) { double v = atof(e); if (v >= 1.0 && v <= 1000.0) g_fps = v; }
        timeBeginPeriod(1);
        g_tinit = 1;
        glog("frame limiter armed @ %.2f fps (opengl path)", g_fps);
        return;
    }
    const double target = (double)g_freq.QuadPart / g_fps;
    LARGE_INTEGER now;
    for (;;) {
        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - g_last.QuadPart);
        if (elapsed >= target) break;
        double remain_ms = (target - elapsed) * 1000.0 / (double)g_freq.QuadPart;
        if (remain_ms > 3.0) Sleep((DWORD)(remain_ms - 2.0));
        else YieldProcessor();
    }
    g_last.QuadPart += (LONGLONG)target;
    if ((double)(now.QuadPart - g_last.QuadPart) > target * 4.0) g_last = now;
}

/* ---- hook implementations ------------------------------------------------- */
typedef BOOL (WINAPI *pSwapBuffers_t)(HDC);
typedef BOOL (WINAPI *pWglSwapBuffers_t)(HDC);
static pSwapBuffers_t    Real_SwapBuffers    = NULL;
static pWglSwapBuffers_t Real_WglSwapBuffers = NULL;

static BOOL WINAPI Hook_SwapBuffers(HDC hdc) {
    track_window(hdc);
    gl_throttle();
    return Real_SwapBuffers ? Real_SwapBuffers(hdc) : FALSE;
}

static BOOL WINAPI Hook_WglSwapBuffers(HDC hdc) {
    track_window(hdc);
    gl_throttle();
    return Real_WglSwapBuffers ? Real_WglSwapBuffers(hdc) : FALSE;
}

/* ===========================================================================
 *  GLSL reserved-word compat shim (modern-driver taming)
 * ===========================================================================
 *  Old Global VR / Cg-derived shaders use 'half', 'half2..4', 'fixed',
 *  'fixed2..4' (and Cg matrix types) that modern GL drivers reject as reserved
 *  words. A failed compile can leave a null info-log the game then strlen()s at
 *  boot -> ACCESS_VIOLATION deep in glut32 (proven from ALIENS.exe.9736.dmp:
 *  a strlen reading a near-null pointer during shader init). We rewrite those
 *  whole-word tokens to their GLSL equivalents BEFORE the driver compiles them.
 *
 *  The game has no static import for GL2 entry points -- it resolves
 *  glShaderSource(ARB) through opengl32!wglGetProcAddress (ALIENS.exe imports
 *  that) -- so we IAT-hook wglGetProcAddress and hand back our wrapper for those
 *  two names, passing every other name straight through. Gated:
 *  ULTRA_GVR_GLSL_FIX=0 disables it. Regression-safe for the other six Global VR
 *  titles: a shader with no reserved word is copied verbatim, and 'float' is a
 *  superset of 'half' so any title whose driver DID accept 'half' is unaffected.
 * =========================================================================== */
typedef unsigned int GL_uint;
typedef int          GL_sizei;
typedef int          GL_int;
typedef char         GL_char;
typedef void (__stdcall *pGlShaderSource)(GL_uint, GL_sizei, const GL_char* const*, const GL_int*);

static pGlShaderSource Real_glShaderSource    = NULL;   /* core (GL2) */
static pGlShaderSource Real_glShaderSourceARB = NULL;   /* ARB_shader_objects */

static int glsl_fix_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("ULTRA_GVR_GLSL_FIX"); v = !(e && e[0] == '0'); }
    return v;
}
static int gl_is_ident_char(char c) {
    return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';
}
/* malloc'd null-terminated rewrite of [src, src+len); caller frees. len<0 => strlen. */
static char* glsl_rewrite(const char* src, int len) {
    static const struct { const char* from; const char* to; } M[] = {
        {"half4x4","mat4"},{"half3x3","mat3"},{"half2x2","mat2"},
        {"fixed4x4","mat4"},{"fixed3x3","mat3"},{"fixed2x2","mat2"},
        {"half4","vec4"},{"half3","vec3"},{"half2","vec2"},
        {"fixed4","vec4"},{"fixed3","vec3"},{"fixed2","vec2"},
        {"half","float"},{"fixed","float"},
    };
    if (!src) return NULL;
    if (len < 0) len = (int)strlen(src);
    char* out = (char*)malloc((size_t)len * 2 + 16);
    if (!out) return NULL;
    int i = 0, o = 0;
    while (i < len) {
        char c = src[i];
        if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_') {
            int j = i + 1;
            while (j < len && gl_is_ident_char(src[j])) j++;
            int tlen = j - i, m = -1;
            for (unsigned k = 0; k < sizeof(M)/sizeof(M[0]); ++k) {
                if ((int)strlen(M[k].from) == tlen && memcmp(src+i, M[k].from, (size_t)tlen) == 0) { m=(int)k; break; }
            }
            if (m >= 0) { const char* r = M[m].to; while (*r) out[o++] = *r++; }
            else        { for (int p = i; p < j; ++p) out[o++] = src[p]; }
            i = j;
        } else { out[o++] = c; i++; }
    }
    out[o] = 0;
    return out;
}
static void gl_shader_source_common(pGlShaderSource real, const char* tag,
        GL_uint sh, GL_sizei count, const GL_char* const* str, const GL_int* len) {
    if (!real) return;
    if (count <= 0 || !str) { real(sh, count, str, len); return; }
    char** nb = (char**)malloc(sizeof(char*) * (size_t)count);
    if (!nb) { real(sh, count, str, len); return; }
    int ok = 1;
    for (int i = 0; i < count; ++i) {
        int l = (len && len[i] >= 0) ? len[i] : -1;
        nb[i] = str[i] ? glsl_rewrite(str[i], l) : NULL;
        if (str[i] && !nb[i]) ok = 0;
    }
    if (ok) { real(sh, count, (const GL_char* const*)nb, NULL);   /* our copies are NUL-terminated */
              glog("glsl shim: rewrote %d %s shader string(s) (half/fixed -> float/vec/mat)", count, tag); }
    else      real(sh, count, str, len);
    for (int i = 0; i < count; ++i) free(nb[i]);
    free(nb);
}
static void __stdcall Wrap_glShaderSource(GL_uint sh, GL_sizei count, const GL_char* const* str, const GL_int* len) {
    gl_shader_source_common(Real_glShaderSource, "core", sh, count, str, len);
}
static void __stdcall Wrap_glShaderSourceARB(GL_uint sh, GL_sizei count, const GL_char* const* str, const GL_int* len) {
    gl_shader_source_common(Real_glShaderSourceARB, "ARB", sh, count, str, len);
}

typedef PROC (WINAPI *pWglGetProcAddress)(LPCSTR);
static pWglGetProcAddress Real_wglGetProcAddress = NULL;
static PROC WINAPI Hook_wglGetProcAddress(LPCSTR name) {
    PROC real = Real_wglGetProcAddress ? Real_wglGetProcAddress(name) : NULL;
    if (!real || !name || !glsl_fix_enabled()) return real;
    if (strcmp(name, "glShaderSource") == 0) {
        if (!Real_glShaderSource) Real_glShaderSource = (pGlShaderSource)(void*)real;
        return (PROC)Wrap_glShaderSource;
    }
    if (strcmp(name, "glShaderSourceARB") == 0) {
        if (!Real_glShaderSourceARB) Real_glShaderSourceARB = (pGlShaderSource)(void*)real;
        return (PROC)Wrap_glShaderSourceARB;
    }
    return real;
}

/* ---- optional: keep GLUT off exclusive fullscreen (gated, DEFAULT OFF) ------
 * Some Global VR titles glutEnterGameMode -> ChangeDisplaySettings(CDS_FULLSCREEN)
 * to an exclusive mode; on a desktop that can strand focus/resolution. When
 * ULTRA_GVR_GL_FORCEWINDOW=1 we answer the exclusive switch with
 * DISP_CHANGE_SUCCESSFUL WITHOUT changing the real mode, so the title renders on
 * the desktop resolution in a normal window. Default OFF, and the IAT hook is
 * only installed when the flag is set, so behaviour is byte-identical for every
 * title that does not opt in. */
typedef LONG (WINAPI *pCDSExA)(LPCSTR, DEVMODEA*, HWND, DWORD, LPVOID);
typedef LONG (WINAPI *pCDSA)(DEVMODEA*, DWORD);
static pCDSExA Real_CDSExA = NULL;
static pCDSA   Real_CDSA   = NULL;
static int gl_forcewindow(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("ULTRA_GVR_GL_FORCEWINDOW"); v = (e && e[0]=='1'); }
    return v;
}
static LONG WINAPI Hook_CDSExA(LPCSTR dev, DEVMODEA* dm, HWND w, DWORD fl, LPVOID p) {
    if (fl & CDS_FULLSCREEN) { glog("gl: swallowed CDS_FULLSCREEN (force-window)"); return DISP_CHANGE_SUCCESSFUL; }
    return Real_CDSExA ? Real_CDSExA(dev, dm, w, fl, p) : DISP_CHANGE_FAILED;
}
static LONG WINAPI Hook_CDSA(DEVMODEA* dm, DWORD fl) {
    if (fl & CDS_FULLSCREEN) { glog("gl: swallowed CDS_FULLSCREEN (force-window)"); return DISP_CHANGE_SUCCESSFUL; }
    return Real_CDSA ? Real_CDSA(dm, fl) : DISP_CHANGE_FAILED;
}

/* ---- install -------------------------------------------------------------- */
void ultra_gvr_gl_install(void) {
    if (!Real_SwapBuffers) {
        HMODULE gdi = GetModuleHandleA("gdi32.dll");
        if (!gdi) gdi = LoadLibraryA("gdi32.dll");
        if (gdi) Real_SwapBuffers = (pSwapBuffers_t)(void*)ultra_gvr_raw_export(gdi, "SwapBuffers");
    }
    if (!Real_WglSwapBuffers) {
        HMODULE ogl = GetModuleHandleA("opengl32.dll");
        if (!ogl) ogl = LoadLibraryA("opengl32.dll");
        if (ogl) Real_WglSwapBuffers = (pWglSwapBuffers_t)(void*)ultra_gvr_raw_export(ogl, "wglSwapBuffers");
    }
    /* Patch both names in every loaded module's IAT. Idempotent. */
    ultra_gvr_iat_patch_all("gdi32.dll",    "SwapBuffers",    (void*)Hook_SwapBuffers);
    ultra_gvr_iat_patch_all("opengl32.dll", "wglSwapBuffers", (void*)Hook_WglSwapBuffers);

    /* GLSL reserved-word shim: intercept wglGetProcAddress so glShaderSource(ARB)
     * comes back as our rewriter. Default ON; the hook only special-cases those
     * two names and passes everything else through unchanged. */
    if (glsl_fix_enabled()) {
        if (!Real_wglGetProcAddress) {
            HMODULE ogl = GetModuleHandleA("opengl32.dll");
            if (!ogl) ogl = LoadLibraryA("opengl32.dll");
            if (ogl) Real_wglGetProcAddress = (pWglGetProcAddress)(void*)ultra_gvr_raw_export(ogl, "wglGetProcAddress");
        }
        ultra_gvr_iat_patch_all("opengl32.dll", "wglGetProcAddress", (void*)Hook_wglGetProcAddress);
    }

    /* Optional force-window: only install when explicitly opted in. */
    if (gl_forcewindow()) {
        HMODULE u = GetModuleHandleA("user32.dll");
        if (u) {
            if (!Real_CDSExA) Real_CDSExA = (pCDSExA)(void*)ultra_gvr_raw_export(u, "ChangeDisplaySettingsExA");
            if (!Real_CDSA)   Real_CDSA   = (pCDSA)  (void*)ultra_gvr_raw_export(u, "ChangeDisplaySettingsA");
        }
        ultra_gvr_iat_patch_all("user32.dll", "ChangeDisplaySettingsExA", (void*)Hook_CDSExA);
        ultra_gvr_iat_patch_all("user32.dll", "ChangeDisplaySettingsA",   (void*)Hook_CDSA);
    }
}

void ultra_gvr_gl_teardown(void) {
    if (g_tinit) { timeEndPeriod(1); g_tinit = 0; }
}

#endif /* _WIN32 */
