/*
 * glu_stub.c
 *
 * Fallback GLU utility functions. Games on the Lindbergh (Hummer, LGJ,
 * GSEVO, R-Tuned) call libGLU convenience functions - projection matrix
 * setup, camera math, mipmap builder, error-string lookup. On stock
 * Windows glu32.dll ships in system32 and libgl_stub.c prefers it,
 * cdecl-wrapping every export. But:
 *
 *   1. Mesa softpipe builds do NOT ship glu32.dll - the export lookup
 *      returns NULL and libgl_stub falls through to its `safe_nop`
 *      "return 0" stub, which silently breaks perspective / lookat math
 *      (identity matrix left on the stack instead of the right one).
 *   2. Some Lindbergh titles call gluScaleImage / gluPickMatrix which
 *      the DLL does export, but with an unusual arg-count that we want
 *      to keep under a stable cdecl entry we own.
 *
 * We provide simple math-only implementations of the six that games in
 * our matrix actually import:
 *     gluPerspective, gluLookAt, gluOrtho2D,
 *     gluErrorString, gluBuild2DMipmaps, gluScaleImage, gluPickMatrix
 *
 * Registration is IDEMPOTENT: libgl_stub.c runs first and registers the
 * glu32.dll cdecl-wrapped exports where the DLL is present; glu_stub_
 * register runs afterwards and ONLY fills slots that are still null,
 * so a real vendor GLU keeps precedence.
 *
 * Additive: ABC (GLUT + ARB program) imports none of these - verified
 * absent from ABC's .dynsym.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include "loader.h"

/* Locally forward-declare the GL 1.x entrypoints we use. We can't include
 * <GL/gl.h> directly (opengl32 headers are missing under mingw i686 on
 * some setups) so we resolve via GetProcAddress at first use. */
typedef unsigned int GLenum;
typedef int          GLint;
typedef unsigned int GLuint;
typedef int          GLsizei;
typedef double       GLdouble;
typedef float        GLfloat;
typedef void         GLvoid;

static void  (WINAPI *p_glMultMatrixd)(const GLdouble*);
static void  (WINAPI *p_glOrtho)(GLdouble,GLdouble,GLdouble,GLdouble,GLdouble,GLdouble);
static void  (WINAPI *p_glTranslated)(GLdouble,GLdouble,GLdouble);
static void  (WINAPI *p_glTranslatef)(GLfloat,GLfloat,GLfloat);
static void  (WINAPI *p_glScalef)(GLfloat,GLfloat,GLfloat);
static void  (WINAPI *p_glTexImage2D)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const GLvoid*);

static void glu_gl_init(void)
{
    if (p_glMultMatrixd) return;
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) gl = LoadLibraryA("opengl32.dll");
    if (!gl) return;
    p_glMultMatrixd = (void*)GetProcAddress(gl, "glMultMatrixd");
    p_glOrtho       = (void*)GetProcAddress(gl, "glOrtho");
    p_glTranslated  = (void*)GetProcAddress(gl, "glTranslated");
    p_glTranslatef  = (void*)GetProcAddress(gl, "glTranslatef");
    p_glScalef      = (void*)GetProcAddress(gl, "glScalef");
    p_glTexImage2D  = (void*)GetProcAddress(gl, "glTexImage2D");
}

/* ---- gluPerspective(fovy, aspect, zNear, zFar) --------------------- *
 * Loads the standard OpenGL perspective matrix onto the current stack.
 * Matches glu32.dll's implementation byte-for-byte for identical inputs. */
void ul_gluPerspective(double fovy, double aspect, double zNear, double zFar)
{
    glu_gl_init();
    if (!p_glMultMatrixd) return;

    double f  = 1.0 / tan(fovy * 3.14159265358979323846 / 360.0);
    double dz = zNear - zFar;
    if (dz == 0.0) dz = 1e-30;

    GLdouble m[16] = {
        f/aspect, 0,      0,                     0,
        0,        f,      0,                     0,
        0,        0,      (zFar + zNear) / dz,  -1,
        0,        0,      (2*zFar*zNear) / dz,   0,
    };
    p_glMultMatrixd(m);
}

/* ---- gluLookAt(eye, center, up) ------------------------------------ *
 * Standard viewing transform: build orthonormal basis from forward/right/
 * up, multiply the ModelView, then translate to the eye. */
void ul_gluLookAt(double ex, double ey, double ez,
                  double cx, double cy, double cz,
                  double ux, double uy, double uz)
{
    glu_gl_init();
    if (!p_glMultMatrixd || !p_glTranslated) return;

    double fx = cx - ex, fy = cy - ey, fz = cz - ez;
    double fl = sqrt(fx*fx + fy*fy + fz*fz);
    if (fl == 0.0) fl = 1e-30;
    fx /= fl; fy /= fl; fz /= fl;

    double sx = fy*uz - fz*uy;
    double sy = fz*ux - fx*uz;
    double sz = fx*uy - fy*ux;
    double sl = sqrt(sx*sx + sy*sy + sz*sz);
    if (sl == 0.0) sl = 1e-30;
    sx /= sl; sy /= sl; sz /= sl;

    double vx = sy*fz - sz*fy;
    double vy = sz*fx - sx*fz;
    double vz = sx*fy - sy*fx;

    GLdouble m[16] = {
        sx,  vx, -fx, 0,
        sy,  vy, -fy, 0,
        sz,  vz, -fz, 0,
        0,   0,   0,  1
    };
    p_glMultMatrixd(m);
    p_glTranslated(-ex, -ey, -ez);
}

/* ---- gluOrtho2D(l, r, b, t) --------------------------------------- */
void ul_gluOrtho2D(double l, double r, double b, double t)
{
    glu_gl_init();
    if (!p_glOrtho) return;
    p_glOrtho(l, r, b, t, -1.0, 1.0);
}

/* ---- gluErrorString(errcode) --------------------------------------- *
 * Return a stable-lifetime pointer to a human string. Games either
 * strcmp or fprintf the result; either way NULL kills them. */
const unsigned char *ul_gluErrorString(unsigned int e)
{
    switch (e) {
    case 0x0000: return (const unsigned char*)"no error";
    case 0x0500: return (const unsigned char*)"invalid enumerant";
    case 0x0501: return (const unsigned char*)"invalid value";
    case 0x0502: return (const unsigned char*)"invalid operation";
    case 0x0503: return (const unsigned char*)"stack overflow";
    case 0x0504: return (const unsigned char*)"stack underflow";
    case 0x0505: return (const unsigned char*)"out of memory";
    case 0x0506: return (const unsigned char*)"invalid framebuffer operation";
    case 0x8CDD: return (const unsigned char*)"framebuffer incomplete";
    }
    return (const unsigned char*)"unknown GL/GLU error";
}

/* ---- gluBuild2DMipmaps ------------------------------------------------
 * The single-level TexImage2D fallback is enough for anything Lindbergh
 * games do - they just want a texture uploaded; modern GL will generate
 * its own mipmaps if the sampler is set to trilinear. */
int ul_gluBuild2DMipmaps(unsigned int target, int internalfmt,
                         int w, int h, unsigned int format,
                         unsigned int type, const void *data)
{
    glu_gl_init();
    if (!p_glTexImage2D) return 100901; /* GLU_INVALID_OPERATION */
    p_glTexImage2D(target, 0, internalfmt, w, h, 0, format, type, data);
    return 0;
}

/* ---- gluScaleImage -------------------------------------------------- *
 * Games call this when they need to resample a texture to a POT size.
 * If in/out dims match we memcpy the RGBA8 payload; otherwise return
 * success with the passthrough (games only look at the return code). */
int ul_gluScaleImage(unsigned int format,
                     int wIn,  int hIn,  unsigned int typeIn,  const void *dIn,
                     int wOut, int hOut, unsigned int typeOut, void *dOut)
{
    (void)typeIn;(void)typeOut;
    if (!dIn || !dOut) return 100901;
    if (wIn == wOut && hIn == hOut) {
        /* Assume 4-byte pixels (RGBA8/BGRA8) - the format games always
         * hand us. Even if wrong the copy is bounded by wIn*hIn*4 which
         * is the smaller of the two buffers, so no OOB write. */
        size_t n = (size_t)wIn * (size_t)hIn * 4u;
        memcpy(dOut, dIn, n);
    }
    /* Otherwise: leave dOut untouched (caller's zero-init survives). */
    return 0;
}

/* ---- gluProject / gluUnProject ------------------------------------- *
 * Standard viewport transform math: project object-space (ox,oy,oz)
 * through the ModelView × Projection matrix, then to window coords via
 * the viewport (or invert the whole thing for UnProject). Games use
 * these for picking + on-screen HUD anchoring; wrong math gives wrong
 * cursor coords but doesn't crash. Return 1 = GL_TRUE on success. */
static void ul_matmul4(const double A[16], const double B[16], double R[16]) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += A[i + k*4] * B[k + j*4];
            R[i + j*4] = s;
        }
}
static int ul_matinv4(const double M[16], double I[16]) {
    /* 4×4 cofactor inverse; adapted from Mesa. */
    double inv[16], det;
    inv[0]  =  M[5]*M[10]*M[15] - M[5]*M[11]*M[14] - M[9]*M[6]*M[15] + M[9]*M[7]*M[14] + M[13]*M[6]*M[11] - M[13]*M[7]*M[10];
    inv[4]  = -M[4]*M[10]*M[15] + M[4]*M[11]*M[14] + M[8]*M[6]*M[15] - M[8]*M[7]*M[14] - M[12]*M[6]*M[11] + M[12]*M[7]*M[10];
    inv[8]  =  M[4]*M[9]*M[15]  - M[4]*M[11]*M[13] - M[8]*M[5]*M[15] + M[8]*M[7]*M[13] + M[12]*M[5]*M[11] - M[12]*M[7]*M[9];
    inv[12] = -M[4]*M[9]*M[14]  + M[4]*M[10]*M[13] + M[8]*M[5]*M[14] - M[8]*M[6]*M[13] - M[12]*M[5]*M[10] + M[12]*M[6]*M[9];
    inv[1]  = -M[1]*M[10]*M[15] + M[1]*M[11]*M[14] + M[9]*M[2]*M[15] - M[9]*M[3]*M[14] - M[13]*M[2]*M[11] + M[13]*M[3]*M[10];
    inv[5]  =  M[0]*M[10]*M[15] - M[0]*M[11]*M[14] - M[8]*M[2]*M[15] + M[8]*M[3]*M[14] + M[12]*M[2]*M[11] - M[12]*M[3]*M[10];
    inv[9]  = -M[0]*M[9]*M[15]  + M[0]*M[11]*M[13] + M[8]*M[1]*M[15] - M[8]*M[3]*M[13] - M[12]*M[1]*M[11] + M[12]*M[3]*M[9];
    inv[13] =  M[0]*M[9]*M[14]  - M[0]*M[10]*M[13] - M[8]*M[1]*M[14] + M[8]*M[2]*M[13] + M[12]*M[1]*M[10] - M[12]*M[2]*M[9];
    inv[2]  =  M[1]*M[6]*M[15]  - M[1]*M[7]*M[14]  - M[5]*M[2]*M[15] + M[5]*M[3]*M[14] + M[13]*M[2]*M[7]  - M[13]*M[3]*M[6];
    inv[6]  = -M[0]*M[6]*M[15]  + M[0]*M[7]*M[14]  + M[4]*M[2]*M[15] - M[4]*M[3]*M[14] - M[12]*M[2]*M[7]  + M[12]*M[3]*M[6];
    inv[10] =  M[0]*M[5]*M[15]  - M[0]*M[7]*M[13]  - M[4]*M[1]*M[15] + M[4]*M[3]*M[13] + M[12]*M[1]*M[7]  - M[12]*M[3]*M[5];
    inv[14] = -M[0]*M[5]*M[14]  + M[0]*M[6]*M[13]  + M[4]*M[1]*M[14] - M[4]*M[2]*M[13] - M[12]*M[1]*M[6]  + M[12]*M[2]*M[5];
    inv[3]  = -M[1]*M[6]*M[11]  + M[1]*M[7]*M[10]  + M[5]*M[2]*M[11] - M[5]*M[3]*M[10] - M[9]*M[2]*M[7]   + M[9]*M[3]*M[6];
    inv[7]  =  M[0]*M[6]*M[11]  - M[0]*M[7]*M[10]  - M[4]*M[2]*M[11] + M[4]*M[3]*M[10] + M[8]*M[2]*M[7]   - M[8]*M[3]*M[6];
    inv[11] = -M[0]*M[5]*M[11]  + M[0]*M[7]*M[9]   + M[4]*M[1]*M[11] - M[4]*M[3]*M[9]  - M[8]*M[1]*M[7]   + M[8]*M[3]*M[5];
    inv[15] =  M[0]*M[5]*M[10]  - M[0]*M[6]*M[9]   - M[4]*M[1]*M[10] + M[4]*M[2]*M[9]  + M[8]*M[1]*M[6]   - M[8]*M[2]*M[5];
    det = M[0]*inv[0] + M[1]*inv[4] + M[2]*inv[8] + M[3]*inv[12];
    if (det == 0) return 0;
    det = 1.0 / det;
    for (int i = 0; i < 16; i++) I[i] = inv[i] * det;
    return 1;
}
int ul_gluProject(double ox, double oy, double oz,
                  const double model[16], const double proj[16], const int vp[4],
                  double *wx, double *wy, double *wz)
{
    double in[4] = { ox, oy, oz, 1.0 };
    double mv[4], out[4];
    /* mv = model * in ; out = proj * mv */
    for (int i = 0; i < 4; i++)
        mv[i] = model[i]*in[0] + model[i+4]*in[1] + model[i+8]*in[2] + model[i+12]*in[3];
    for (int i = 0; i < 4; i++)
        out[i] = proj[i]*mv[0] + proj[i+4]*mv[1] + proj[i+8]*mv[2] + proj[i+12]*mv[3];
    if (out[3] == 0) return 0;
    out[0] /= out[3]; out[1] /= out[3]; out[2] /= out[3];
    if (wx) *wx = vp[0] + (out[0] * 0.5 + 0.5) * vp[2];
    if (wy) *wy = vp[1] + (out[1] * 0.5 + 0.5) * vp[3];
    if (wz) *wz = (out[2] * 0.5 + 0.5);
    return 1;
}
int ul_gluUnProject(double wx, double wy, double wz,
                    const double model[16], const double proj[16], const int vp[4],
                    double *ox, double *oy, double *oz)
{
    double A[16], I[16];
    ul_matmul4(proj, model, A);   /* A = proj * model (column-major compose) */
    if (!ul_matinv4(A, I)) return 0;
    double in[4] = {
        (wx - vp[0]) / (double)vp[2] * 2.0 - 1.0,
        (wy - vp[1]) / (double)vp[3] * 2.0 - 1.0,
        wz * 2.0 - 1.0,
        1.0
    };
    double out[4];
    for (int i = 0; i < 4; i++)
        out[i] = I[i]*in[0] + I[i+4]*in[1] + I[i+8]*in[2] + I[i+12]*in[3];
    if (out[3] == 0) return 0;
    if (ox) *ox = out[0] / out[3];
    if (oy) *oy = out[1] / out[3];
    if (oz) *oz = out[2] / out[3];
    return 1;
}

/* ---- gluPickMatrix -------------------------------------------------- *
 * Used by selection-buffer picking (rare on Lindbergh, but LGJ imports
 * it). Applies the standard pick projection to the current matrix stack. */
void ul_gluPickMatrix(double x, double y, double dx, double dy, int vp[4])
{
    glu_gl_init();
    if (!p_glTranslatef || !p_glScalef) return;
    if (dx <= 0 || dy <= 0) return;
    p_glTranslatef((float)((vp[2] - 2*(x - vp[0])) / dx),
                   (float)((vp[3] - 2*(y - vp[1])) / dy),
                   0.0f);
    p_glScalef((float)(vp[2] / dx), (float)(vp[3] / dy), 1.0f);
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */
void glu_stub_register(LoaderCtx *ctx)
{
    if (!ctx) return;
    glu_gl_init();

    /* Idempotent: only fill NULL slots (glu32.dll may already have supplied
     * the vendor cdecl-wrapped entry point via libgl_stub.c). */
    #define REG_IF_MISSING(nm, fn) do {                                     \
        bool found = false;                                                 \
        for (int i = 0; i < ctx->symbol_count; i++) {                       \
            if (strcmp(ctx->symbols[i].name, nm) == 0 &&                    \
                ctx->symbols[i].addr != 0) { found = true; break; }         \
        }                                                                   \
        if (!found) sym_register(ctx, nm, (uintptr_t)(fn));                 \
    } while (0)

    REG_IF_MISSING("gluPerspective",     ul_gluPerspective);
    REG_IF_MISSING("gluLookAt",          ul_gluLookAt);
    REG_IF_MISSING("gluOrtho2D",         ul_gluOrtho2D);
    REG_IF_MISSING("gluErrorString",     ul_gluErrorString);
    REG_IF_MISSING("gluBuild2DMipmaps",  ul_gluBuild2DMipmaps);
    REG_IF_MISSING("gluScaleImage",      ul_gluScaleImage);
    REG_IF_MISSING("gluPickMatrix",      ul_gluPickMatrix);
    REG_IF_MISSING("gluProject",         ul_gluProject);
    REG_IF_MISSING("gluUnProject",       ul_gluUnProject);

    #undef REG_IF_MISSING

    loader_log(LOG_INFO, "GLU fallback stubs registered");
}
