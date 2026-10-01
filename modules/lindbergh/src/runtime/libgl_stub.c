/*
 * libgl_stub.c
 *
 * Intercepts dlopen("libGL.so*") / dlopen("libGLX.so*") and returns a
 * fake handle. dlsym() on that handle resolves:
 *
 *   1. GLX functions  -> our glx_bridge stubs (context/surface management)
 *   2. gl* functions  -> opengl32.dll via GetProcAddress / wglGetProcAddress
 *                        (these are identical on Linux and Windows x64)
 *
 * This is Option C applied to the graphics stack: instead of shipping
 * a real libGL.so (impossible on Windows), we register every gl* symbol
 * the game might call.
 *
 * GL function coverage strategy:
 *   - Core OpenGL 1.1: GetProcAddress(opengl32.dll) covers everything
 *   - GL extensions (ARB/EXT/NV): wglGetProcAddress covers everything
 *   - We register a catch-all resolver so unknown gl* symbols still work
 *
 * The fake dlopen handle encodes the library type so dlsym knows which
 * resolver to use.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loader.h"
#include "glx_bridge.h"
#include "libgl_stub.h"

/* ------------------------------------------------------------------ */
/* Fake handle table                                                    */
/* ------------------------------------------------------------------ */
#define HANDLE_LIBGL    ((void*)0x4C474C00)  /* "LGL\0" */
#define HANDLE_LIBGLX   ((void*)0x4C475800)  /* "LGX\0" */
#define HANDLE_LIBGLES  ((void*)0x4C474500)  /* "LGE\0" */

static HMODULE g_opengl32 = NULL;

static HMODULE get_opengl32(void)
{
    if (!g_opengl32) g_opengl32 = LoadLibraryA("opengl32.dll");
    return g_opengl32;
}

static void *make_cdecl_wrapper(void *stdcall_fn); /* defined below */

/* ------------------------------------------------------------------ */
/* GL function resolver                                                */
/* ------------------------------------------------------------------ */
/*
 * Resolution order:
 *   1. GLX bridge (context/swap/extension query functions)
 *   2. opengl32.dll core (OpenGL 1.1 functions)
 *   3. wglGetProcAddress (all extensions, OpenGL 1.2+)
 */
void *libgl_resolve(const char *name)
{
    if (!name) return NULL;

    /* 1. GLX / X11 bridge functions */
    if (strncmp(name, "glX", 3) == 0 || strncmp(name, "X", 1) == 0) {
        return stub_glXGetProcAddressARB((const uint8_t*)name);
    }

    /* 2. Core OpenGL 1.1 from opengl32.dll - stdcall on Windows, must wrap */
    HMODULE gl = get_opengl32();
    if (gl) {
        void *p = (void*)GetProcAddress(gl, name);
        if (p) return make_cdecl_wrapper(p);
    }

    /* 3. Extensions and OpenGL 1.2+ via wglGetProcAddress - stdcall, must wrap */
    void *p = stub_glXGetProcAddressARB((const uint8_t*)name);
    if (p) return make_cdecl_wrapper(p);

    /* gl* entry points unresolved on this backend: return a no-op rather than
     * NULL, so a pointer the game caches and later calls (without a NULL check)
     * doesn't jump to 0. Non-GL symbols still return NULL. */
    if (strncmp(name, "gl", 2) == 0) {
        extern uint32_t gl_unresolved_noop(void);
        loader_log(LOG_WARN, "[libGL] unresolved %s -> no-op stub", name);
        return (void*)(uintptr_t)&gl_unresolved_noop;
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* dlopen interceptor                                                   */
/* ------------------------------------------------------------------ */
bool libgl_is_gl_library(const char *filename)
{
    if (!filename) return false;
    return (strstr(filename, "libGL.so")   != NULL ||
            strstr(filename, "libGLX.so")  != NULL ||
            strstr(filename, "libGLES")    != NULL ||
            strstr(filename, "libEGL.so")  != NULL ||
            strstr(filename, "libGLU.so")  != NULL ||
            strstr(filename, "nvidia")     != NULL ||
            strstr(filename, "libglx")     != NULL);
}

void *libgl_dlopen(const char *filename)
{
    loader_log(LOG_INFO, "libGL: intercepted dlopen(\"%s\")", filename);

    if (strstr(filename, "libGLX") || strstr(filename, "libGL"))
        return HANDLE_LIBGL;
    if (strstr(filename, "libGLES"))
        return HANDLE_LIBGLES;

    return HANDLE_LIBGL; /* default for any GL-related lib */
    return NULL;
}

void *libgl_dlsym(void *handle, const char *symbol)
{
    if (handle == HANDLE_LIBGL   ||
        handle == HANDLE_LIBGLX  ||
        handle == HANDLE_LIBGLES) {
        return libgl_resolve(symbol);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Register all known GL core symbols directly in the loader symbol   */
/* table so ELF import resolution (non-dlopen path) also works        */
/* ------------------------------------------------------------------ */

/* List of OpenGL 1.1 core functions in opengl32.dll */
static const char *gl_core_functions[] = {
    "glAccum", "glAlphaFunc", "glAreTexturesResident", "glArrayElement",
    "glBegin", "glBindTexture", "glBitmap", "glBlendFunc",
    "glCallList", "glCallLists", "glClear", "glClearAccum",
    "glClearColor", "glClearDepth", "glClearIndex", "glClearStencil",
    "glClipPlane", "glColor3b", "glColor3bv", "glColor3d", "glColor3dv",
    "glColor3f", "glColor3fv", "glColor3i", "glColor3iv", "glColor3s",
    "glColor3sv", "glColor3ub", "glColor3ubv", "glColor3ui", "glColor3uiv",
    "glColor3us", "glColor3usv", "glColor4b", "glColor4bv", "glColor4d",
    "glColor4dv", "glColor4f", "glColor4fv", "glColor4i", "glColor4iv",
    "glColor4s", "glColor4sv", "glColor4ub", "glColor4ubv", "glColor4ui",
    "glColor4uiv", "glColor4us", "glColor4usv", "glColorMask",
    "glColorMaterial", "glColorPointer", "glCopyPixels", "glCopyTexImage1D",
    "glCopyTexImage2D", "glCopyTexSubImage1D", "glCopyTexSubImage2D",
    "glCullFace", "glDeleteLists", "glDeleteTextures", "glDepthFunc",
    "glDepthMask", "glDepthRange", "glDisable", "glDisableClientState",
    "glDrawArrays", "glDrawBuffer", "glDrawElements", "glDrawPixels",
    "glEdgeFlag", "glEdgeFlagPointer", "glEdgeFlagv", "glEnable",
    "glEnableClientState", "glEnd", "glEndList", "glEvalCoord1d",
    "glEvalCoord1dv", "glEvalCoord1f", "glEvalCoord1fv", "glEvalCoord2d",
    "glEvalCoord2dv", "glEvalCoord2f", "glEvalCoord2fv", "glEvalMesh1",
    "glEvalMesh2", "glEvalPoint1", "glEvalPoint2", "glFeedbackBuffer",
    "glFinish", "glFlush", "glFogf", "glFogfv", "glFogi", "glFogiv",
    "glFrontFace", "glFrustum", "glGenLists", "glGenTextures",
    "glGetBooleanv", "glGetClipPlane", "glGetDoublev", "glGetError",
    "glGetFloatv", "glGetIntegerv", "glGetLightfv", "glGetLightiv",
    "glGetMapdv", "glGetMapfv", "glGetMapiv", "glGetMaterialfv",
    "glGetMaterialiv", "glGetPixelMapfv", "glGetPixelMapuiv",
    "glGetPixelMapusv", "glGetPointerv", "glGetPolygonStipple",
    "glGetString", "glGetTexEnvfv", "glGetTexEnviv", "glGetTexGendv",
    "glGetTexGenfv", "glGetTexGeniv", "glGetTexImage", "glGetTexLevelParameterfv",
    "glGetTexLevelParameteriv", "glGetTexParameterfv", "glGetTexParameteriv",
    "glHint", "glIndexMask", "glIndexPointer", "glIndexd", "glIndexdv",
    "glIndexf", "glIndexfv", "glIndexi", "glIndexiv", "glIndexs",
    "glIndexsv", "glIndexub", "glIndexubv", "glInitNames", "glInterleavedArrays",
    "glIsEnabled", "glIsList", "glIsTexture", "glLightModelf",
    "glLightModelfv", "glLightModeli", "glLightModeliv", "glLightf",
    "glLightfv", "glLighti", "glLightiv", "glLineStipple", "glLineWidth",
    "glListBase", "glLoadIdentity", "glLoadMatrixd", "glLoadMatrixf",
    "glLoadName", "glLogicOp", "glMap1d", "glMap1f", "glMap2d", "glMap2f",
    "glMapGrid1d", "glMapGrid1f", "glMapGrid2d", "glMapGrid2f",
    "glMaterialf", "glMaterialfv", "glMateriali", "glMaterialiv",
    "glMatrixMode", "glMultMatrixd", "glMultMatrixf", "glNewList",
    "glNormal3b", "glNormal3bv", "glNormal3d", "glNormal3dv",
    "glNormal3f", "glNormal3fv", "glNormal3i", "glNormal3iv",
    "glNormal3s", "glNormal3sv", "glNormalPointer", "glOrtho",
    "glPassThrough", "glPixelMapfv", "glPixelMapuiv", "glPixelMapusv",
    "glPixelStoref", "glPixelStorei", "glPixelTransferf", "glPixelTransferi",
    "glPixelZoom", "glPointSize", "glPolygonMode", "glPolygonOffset",
    "glPolygonStipple", "glPopAttrib", "glPopClientAttrib", "glPopMatrix",
    "glPopName", "glPrioritizeTextures", "glPushAttrib", "glPushClientAttrib",
    "glPushMatrix", "glPushName", "glRasterPos2d", "glRasterPos2dv",
    "glRasterPos2f", "glRasterPos2fv", "glRasterPos2i", "glRasterPos2iv",
    "glRasterPos2s", "glRasterPos2sv", "glRasterPos3d", "glRasterPos3dv",
    "glRasterPos3f", "glRasterPos3fv", "glRasterPos3i", "glRasterPos3iv",
    "glRasterPos3s", "glRasterPos3sv", "glRasterPos4d", "glRasterPos4dv",
    "glRasterPos4f", "glRasterPos4fv", "glRasterPos4i", "glRasterPos4iv",
    "glRasterPos4s", "glRasterPos4sv", "glReadBuffer", "glReadPixels",
    "glRectd", "glRectdv", "glRectf", "glRectfv", "glRecti", "glRectiv",
    "glRects", "glRectsv", "glRenderMode", "glRotated", "glRotatef",
    "glScaled", "glScalef", "glScissor", "glSelectBuffer", "glShadeModel",
    "glStencilFunc", "glStencilMask", "glStencilOp", "glTexCoord1d",
    "glTexCoord1dv", "glTexCoord1f", "glTexCoord1fv", "glTexCoord1i",
    "glTexCoord1iv", "glTexCoord1s", "glTexCoord1sv", "glTexCoord2d",
    "glTexCoord2dv", "glTexCoord2f", "glTexCoord2fv", "glTexCoord2i",
    "glTexCoord2iv", "glTexCoord2s", "glTexCoord2sv", "glTexCoord3d",
    "glTexCoord3dv", "glTexCoord3f", "glTexCoord3fv", "glTexCoord3i",
    "glTexCoord3iv", "glTexCoord3s", "glTexCoord3sv", "glTexCoord4d",
    "glTexCoord4dv", "glTexCoord4f", "glTexCoord4fv", "glTexCoord4i",
    "glTexCoord4iv", "glTexCoord4s", "glTexCoord4sv", "glTexCoordPointer",
    "glTexEnvf", "glTexEnvfv", "glTexEnvi", "glTexEnviv", "glTexGend",
    "glTexGendv", "glTexGenf", "glTexGenfv", "glTexGeni", "glTexGeniv",
    "glTexImage1D", "glTexImage2D", "glTexParameterf", "glTexParameterfv",
    "glTexParameteri", "glTexParameteriv", "glTexSubImage1D", "glTexSubImage2D",
    "glTranslated", "glTranslatef", "glVertex2d", "glVertex2dv",
    "glVertex2f", "glVertex2fv", "glVertex2i", "glVertex2iv",
    "glVertex2s", "glVertex2sv", "glVertex3d", "glVertex3dv",
    "glVertex3f", "glVertex3fv", "glVertex3i", "glVertex3iv",
    "glVertex3s", "glVertex3sv", "glVertex4d", "glVertex4dv",
    "glVertex4f", "glVertex4fv", "glVertex4i", "glVertex4iv",
    "glVertex4s", "glVertex4sv", "glVertexPointer", "glViewport",
    NULL
};

/* ------------------------------------------------------------------ *
 * stdcall -> cdecl adapter.
 * opengl32.dll / Mesa GL functions are __stdcall (WINAPI): the callee pops
 * its argument bytes (ret N). The Linux game calls them __cdecl (caller owns
 * the args). Calling a raw stdcall GL function with args from cdecl code
 * therefore corrupts ESP by N bytes - e.g. glFrustum (6 GLdouble = 48 bytes)
 * smashed calc_camera_matrix's saved EBP/return address -> ret-to-0 crash.
 * glx_emu hand-wraps the few functions it overrides; this wraps ALL the rest.
 * The adapter copies up to 128 arg-bytes to a scratch frame, calls the
 * stdcall function, then restores ESP via `lea esp,[ebp-8]` regardless of how
 * many bytes the callee popped, and returns cdecl-style (pops only the return
 * address). EAX/ST(0) return values pass through untouched.
 * ------------------------------------------------------------------ */
static void *make_cdecl_wrapper(void *stdcall_fn)
{
    static const uint8_t tmpl[] = {
        0x55,                               /* push ebp                 */
        0x89,0xE5,                          /* mov  ebp,esp             */
        0x57,                               /* push edi                 */
        0x56,                               /* push esi                 */
        0x81,0xEC,0x80,0x00,0x00,0x00,      /* sub  esp,0x80            */
        0x8D,0x75,0x08,                     /* lea  esi,[ebp+8] (args)  */
        0x89,0xE7,                          /* mov  edi,esp     (dst)   */
        0xB9,0x20,0x00,0x00,0x00,           /* mov  ecx,0x20 (32 dwords)*/
        0xFC,                               /* cld                      */
        0xF3,0xA5,                          /* rep movsd                */
        0xB8,0,0,0,0,                       /* mov  eax,<stdcall_fn>  @25*/
        0xFF,0xD0,                          /* call eax                 */
        0x8D,0x65,0xF8,                     /* lea  esp,[ebp-8]         */
        0x5E,                               /* pop  esi                 */
        0x5F,                               /* pop  edi                 */
        0x5D,                               /* pop  ebp                 */
        0xC3                                /* ret  (cdecl)             */
    };
    uint8_t *w = (uint8_t*)VirtualAlloc(NULL, sizeof(tmpl),
                                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!w) return stdcall_fn;                 /* fail open: raw (may corrupt) */
    memcpy(w, tmpl, sizeof(tmpl));
    *(void**)(w + 25) = stdcall_fn;
    return w;
}

static bool already_registered(LoaderCtx *ctx, const char *name)
{
    for (int j = 0; j < ctx->symbol_count; j++)
        if (ctx->symbols[j].addr && strcmp(ctx->symbols[j].name, name) == 0)
            return true;
    return false;
}

void libgl_register_stubs(LoaderCtx *ctx)
{
    loader_log(LOG_INFO, "Registering libGL stub layer");

    HMODULE gl = get_opengl32();
    int registered = 0, wrapped = 0;

    for (int i = 0; gl_core_functions[i]; i++) {
        const char *name = gl_core_functions[i];
        /* Don't clobber glx_emu's hand-written cdecl overrides (depth-fix etc.)
         * which were registered earlier. */
        if (already_registered(ctx, name)) continue;
        void *p = gl ? (void*)GetProcAddress(gl, name) : NULL;
        if (p) {
            sym_register(ctx, name, (uintptr_t)make_cdecl_wrapper(p));
            registered++; wrapped++;
        }
    }

    loader_log(LOG_INFO, "  Registered %d core GL symbols from opengl32.dll (%d cdecl-wrapped)",
               registered, wrapped);

    /* Register a generic resolver for unknown gl* symbols via a thunk.
     * This handles OpenGL 1.2+ and all extensions. The thunk calls
     * libgl_resolve() at runtime so we don't need to enumerate every
     * extension function upfront. */
    loader_log(LOG_INFO, "  libGL dynamic resolver installed");
}

/* ==================================================================== *
 * GLSL/ARB shader-object DIAGNOSTIC + COMPAT wrappers (GSEVO ShaderDiag)
 *
 * GSEVO (Ghost Squad Evolution) builds its shaders through the
 * GL_ARB_shader_objects path: glCreateShaderObjectARB -> glShaderSourceARB
 * -> glCompileShaderARB, then GLCreateVertexShader() (gl_util.cpp:2053)
 * reads glGetObjectParameterivARB(GL_OBJECT_COMPILE_STATUS_ARB). On the RX
 * 580 vendor GL that status comes back FALSE and the game asserts. ABC, by
 * contrast, uses ARB-ASSEMBLY programs via glProgramStringARB and never
 * touches these object entry points, so wrapping them here is purely
 * ADDITIVE for ABC (the symbols aren't even in ABC's import table).
 *
 * These cdecl wrappers sit in front of the real (stdcall) vendor functions.
 * They (1) dump the exact source the game uploads and the vendor compiler's
 * info-log + status when ULTRA_GLSL_DUMP is set, and (2) optionally patch
 * the uploaded source for modern-GLSL compatibility (ULTRA_GLSL_FIX, on by
 * default; disable with ULTRA_GLSL_FIX=0). The real fn is reached through a
 * make_cdecl_wrapper-wrapped pointer so the calling convention matches.
 * ==================================================================== */

/* GL enums we read */
#define UL_GL_OBJECT_COMPILE_STATUS_ARB   0x8B81
#define UL_GL_OBJECT_LINK_STATUS_ARB      0x8B82
#define UL_GL_OBJECT_INFO_LOG_LENGTH_ARB  0x8B84
#define UL_GL_VERTEX_SHADER_ARB           0x8B31
#define UL_GL_FRAGMENT_SHADER_ARB         0x8B30

typedef unsigned int GLhandleARB_;

/* Real vendor functions, reached cdecl-style (make_cdecl_wrapper output). */
static GLhandleARB_ (*s_real_CreateShaderObjectARB)(unsigned);
static void         (*s_real_ShaderSourceARB)(GLhandleARB_, int, const char**, const int*);
static void         (*s_real_CompileShaderARB)(GLhandleARB_);
static void         (*s_real_GetObjectParameterivARB)(GLhandleARB_, unsigned, int*);
static void         (*s_real_GetInfoLogARB)(GLhandleARB_, int, int*, char*);

/* Track each shader object's declared type so the dump can label it. Small
 * linear table - GSEVO creates only a handful of shader objects. */
#define UL_SH_MAX 256
static struct { GLhandleARB_ h; unsigned type; } s_sh_tab[UL_SH_MAX];
static int s_sh_n = 0;
static void ul_sh_settype(GLhandleARB_ h, unsigned type){
    for(int i=0;i<s_sh_n;i++) if(s_sh_tab[i].h==h){ s_sh_tab[i].type=type; return; }
    if(s_sh_n<UL_SH_MAX){ s_sh_tab[s_sh_n].h=h; s_sh_tab[s_sh_n].type=type; s_sh_n++; }
}
static unsigned ul_sh_gettype(GLhandleARB_ h){
    for(int i=0;i<s_sh_n;i++) if(s_sh_tab[i].h==h) return s_sh_tab[i].type;
    return 0;
}

static int ul_glsl_dump(void){ const char*e=getenv("ULTRA_GLSL_DUMP"); return e&&*e&&atoi(e)!=0; }
static int ul_glsl_fix(void){  const char*e=getenv("ULTRA_GLSL_FIX");  return !e||!*e||atoi(e)!=0; } /* default ON */

/* Concatenate the game's (count,string,length) array into one NUL-terminated
 * buffer. length may be NULL (each string NUL-terminated) or per-string len. */
static char *ul_glsl_join(int count, const char**strings, const int*lengths, int *out_len){
    size_t total=0;
    for(int i=0;i<count;i++){
        if(!strings||!strings[i]) continue;
        total += (lengths && lengths[i]>=0) ? (size_t)lengths[i] : strlen(strings[i]);
    }
    char *buf=(char*)malloc(total+1); if(!buf){ *out_len=0; return NULL; }
    size_t off=0;
    for(int i=0;i<count;i++){
        if(!strings||!strings[i]) continue;
        size_t n=(lengths && lengths[i]>=0)?(size_t)lengths[i]:strlen(strings[i]);
        memcpy(buf+off,strings[i],n); off+=n;
    }
    buf[off]=0; *out_len=(int)off; return buf;
}

/* Compatibility rewrite of the uploaded GLSL source for the modern driver.
 * Handles 2008-era Sega/Namco shaders that prepend engine #define blocks
 * BEFORE the shader's own #version directive.  Strict drivers (Intel/AMD/Mesa)
 * reject #version that is not the very first token.
 *
 * Strategy:
 *   1. If #version is already first → no change.
 *   2. If #version is buried after #defines → extract it, write it first,
 *      blank the original line with spaces (preserves line numbers in errors).
 *   3. If no #version at all → inject "#version 110" (desktop default).
 * Returns a newly malloc'd buffer (caller frees) or NULL if no change needed. */
static char *ul_glsl_compat(const char *src, int len){
    if(!src||len<=0) return NULL;
    int is_arbasm = (len>=5) && src[0]=='!' && src[1]=='!' &&
                    (strncmp(src,"!!ARB",5)==0);
    if(is_arbasm) return NULL;

    /* Check if #version is the first non-whitespace token. */
    const char*p=src; while(p<src+len && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) p++;
    if((p+8<=src+len) && strncmp(p,"#version",8)==0) return NULL; /* already first */

    /* Search the entire source for a #version line. */
    const char *ver_line = NULL; int ver_line_len = 0;
    for(const char *s=src; s<src+len; s++){
        if(*s=='#' && (s==src || *(s-1)=='\n')){
            if((src+len-s)>=8 && strncmp(s,"#version",8)==0){
                const char *eol=s; while(eol<src+len && *eol!='\n') eol++;
                if(eol<src+len) eol++; /* include the newline */
                ver_line=s; ver_line_len=(int)(eol-s);
                break;
            }
        }
    }

    if(ver_line){
        /* Hoist: version line first, then source with that line blanked. */
        char *out=(char*)malloc(ver_line_len+len+1); if(!out) return NULL;
        memcpy(out, ver_line, ver_line_len);
        memcpy(out+ver_line_len, src, len);
        /* Blank the original #version in the copy (now offset by ver_line_len). */
        char *dup_ver = out+ver_line_len+(ver_line-src);
        for(int i=0; i<ver_line_len && dup_ver[i]!='\n'; i++) dup_ver[i]=' ';
        out[ver_line_len+len]=0;
        return out;
    }

    /* No #version at all - inject 110. */
    static const char hdr[] = "#version 110\n";
    char *out=(char*)malloc(len+sizeof(hdr)+1); if(!out) return NULL;
    memcpy(out,hdr,sizeof(hdr)-1);
    memcpy(out+sizeof(hdr)-1,src,len);
    out[sizeof(hdr)-1+len]=0;
    return out;
}

static GLhandleARB_ ul_CreateShaderObjectARB(unsigned type){
    GLhandleARB_ h = s_real_CreateShaderObjectARB ? s_real_CreateShaderObjectARB(type) : 0;
    ul_sh_settype(h,type);
    if(ul_glsl_dump())
        loader_log(LOG_WARN,"[GLSL] glCreateShaderObjectARB(type=0x%x %s) -> handle %u",
                   type, type==UL_GL_VERTEX_SHADER_ARB?"VERTEX":type==UL_GL_FRAGMENT_SHADER_ARB?"FRAGMENT":"?", h);
    return h;
}

static void ul_ShaderSourceARB(GLhandleARB_ obj, int count, const char**strings, const int*lengths){
    int joined_len=0; char *joined = ul_glsl_join(count,strings,lengths,&joined_len);
    unsigned ty = ul_sh_gettype(obj);
    if(ul_glsl_dump()){
        loader_log(LOG_WARN,"[GLSL] glShaderSourceARB(handle=%u %s, count=%d, len=%d) SOURCE BEGIN >>>",
                   obj, ty==UL_GL_VERTEX_SHADER_ARB?"VERTEX":ty==UL_GL_FRAGMENT_SHADER_ARB?"FRAGMENT":"?",
                   count, joined_len);
        /* Emit the source line-by-line so it survives the log formatter. */
        if(joined){ char*s=joined; int ln=1;
            while(*s){ char*nl=strchr(s,'\n'); if(nl)*nl=0;
                loader_log(LOG_WARN,"[GLSL]  %3d| %s",ln++,s);
                if(!nl) break; *nl='\n'; s=nl+1; }
        }
        loader_log(LOG_WARN,"[GLSL] <<< glShaderSourceARB SOURCE END (handle=%u)",obj);
    }
    /* Compatibility patch (default on). */
    char *patched = ul_glsl_fix() && joined ? ul_glsl_compat(joined,joined_len) : NULL;
    if(s_real_ShaderSourceARB){
        if(patched){
            const char* one[1]={patched}; int onel[1]={(int)strlen(patched)};
            if(ul_glsl_dump()) loader_log(LOG_WARN,"[GLSL] -> uploading PATCHED source (handle=%u, +#version)",obj);
            s_real_ShaderSourceARB(obj,1,one,onel);
        } else {
            s_real_ShaderSourceARB(obj,count,strings,lengths);
        }
    }
    if(patched) free(patched);
    if(joined) free(joined);
}

static void ul_CompileShaderARB(GLhandleARB_ obj){
    if(s_real_CompileShaderARB) s_real_CompileShaderARB(obj);
    if(ul_glsl_dump()){
        int status=-1, loglen=0; char info[4096]={0};
        if(s_real_GetObjectParameterivARB){
            s_real_GetObjectParameterivARB(obj,UL_GL_OBJECT_COMPILE_STATUS_ARB,&status);
            s_real_GetObjectParameterivARB(obj,UL_GL_OBJECT_INFO_LOG_LENGTH_ARB,&loglen);
        }
        if(s_real_GetInfoLogARB) s_real_GetInfoLogARB(obj,(int)sizeof(info)-1,0,info);
        unsigned ty = ul_sh_gettype(obj);
        loader_log(LOG_WARN,"[GLSL] glCompileShaderARB(handle=%u %s): COMPILE_STATUS=%d INFO_LOG_LEN=%d",
                   obj, ty==UL_GL_VERTEX_SHADER_ARB?"VERTEX":ty==UL_GL_FRAGMENT_SHADER_ARB?"FRAGMENT":"?",
                   status, loglen);
        if(info[0]){
            char*s=info; while(*s){ char*nl=strchr(s,'\n'); if(nl)*nl=0;
                loader_log(LOG_WARN,"[GLSL]  COMPILER: %s",s);
                if(!nl) break; s=nl+1; }
        } else {
            loader_log(LOG_WARN,"[GLSL]  COMPILER: <empty info log>");
        }
    }
}

static void ul_GetObjectParameterivARB(GLhandleARB_ obj, unsigned pname, int *params){
    if(s_real_GetObjectParameterivARB) s_real_GetObjectParameterivARB(obj,pname,params);
    if(ul_glsl_dump() && params &&
       (pname==UL_GL_OBJECT_COMPILE_STATUS_ARB || pname==UL_GL_OBJECT_LINK_STATUS_ARB))
        loader_log(LOG_WARN,"[GLSL] glGetObjectParameterivARB(handle=%u, pname=0x%x) -> %d (game reads %s)",
                   obj, pname, *params,
                   pname==UL_GL_OBJECT_COMPILE_STATUS_ARB?"COMPILE_STATUS":"LINK_STATUS");
}

static void ul_GetInfoLogARB(GLhandleARB_ obj, int maxlen, int *length, char *infolog){
    if(s_real_GetInfoLogARB) s_real_GetInfoLogARB(obj,maxlen,length,infolog);
    if(ul_glsl_dump() && infolog && infolog[0])
        loader_log(LOG_WARN,"[GLSL] glGetInfoLogARB(handle=%u): %s",obj,infolog);
}

/* ------------------------------------------------------------------ */
/* Resolve remaining GL extension symbols via wglGetProcAddress */
/* Called after primary symbol registration to fill in extensions     */
/* ------------------------------------------------------------------ */
void libgl_resolve_extensions(LoaderCtx *ctx)
{
    /* List of every unresolved GL extension from the ABC import table */
    static const char *ext_symbols[] = {
        /* GL 1.3+ multitexture */
        "glActiveTexture", "glClientActiveTexture",
        "glMultiTexCoord2f", "glMultiTexCoord2fv",
        /* GL 1.4 */
        "glBlendColor", "glBlendEquation", "glBlendFuncSeparate",
        "glLoadTransposeMatrixf", "glMultTransposeMatrixf",
        /* GL 1.5 VBO */
        "glBindBuffer", "glDeleteBuffers", "glGenBuffers", "glBufferData",
        "glMapBuffer", "glUnmapBuffer",
        /* GL 2.0 */
        "glVertexAttrib1f", "glVertexAttribPointerARB",
        "glEnableVertexAttribArrayARB", "glDisableVertexAttribArrayARB",
        /* ARB programs */
        "glGenProgramsARB", "glDeleteProgramsARB", "glBindProgramARB",
        "glProgramStringARB",
        "glProgramLocalParameter4fARB", "glProgramLocalParameter4fvARB",
        "glProgramEnvParameter4fARB",  "glProgramEnvParameter4fvARB",
        /* ARB VBO */
        "glBindBufferARB", "glDeleteBuffersARB", "glBufferDataARB",
        "glIsBufferARB", "glGetBufferParameterivARB",
        /* EXT framebuffer */
        "glGenFramebuffersEXT", "glBindFramebufferEXT",
        "glDeleteFramebuffersEXT", "glCheckFramebufferStatusEXT",
        "glFramebufferTexture2DEXT",
        "glDeleteRenderbuffersEXT",
        /* NV fences */
        "glGenFencesNV", "glDeleteFencesNV", "glSetFenceNV",
        "glFinishFenceNV", "glIsFenceNV",
        /* Compressed textures */
        "glCompressedTexImage2D",
        /* ARB clamp */
        "glClampColorARB",
        /* GLU */
        "gluErrorString", "gluOrtho2D", "gluLookAt",
        /* GLX extras */
        "glXGetCurrentDisplay", "glXGetCurrentContext",
        "glXGetCurrentDrawable", "glXCreatePbuffer", "glXDestroyPbuffer",
        "glXGetRefreshRateSGI", "glXGetVideoSyncSGI",
        /* XFree */
        "XFree",
        /* pthread scheduling */
        "pthread_attr_setschedparam", "pthread_attr_setschedpolicy",
        "pthread_getschedparam", "pthread_setschedparam",

        /* --- LGJ (Let's Go Jungle) GL-extension imports -----------------
         * LGJ binds Cg via the bundled libCg/libCgGL (now resolved by the
         * ELF32 sym_resolve fix in dynlink.c), but it ALSO imports these 31
         * GLSL-ARB / VBO-ARB / query-ARB / blend-EXT / FBO-EXT / NV / GLX
         * functions directly. The RX 580 vendor OpenGL (ULTRA_GL_DRIVER=
         * native) exports all of them via wglGetProcAddress; they resolve
         * through the same cdecl-wrapped path as the GL 1.5 names above. The
         * glX one routes to its own stub. Additive - ABC imports none of
         * these (ABC uses the GLSL 2.0 core names without the ARB suffix). */
        /* ARB shader objects (GL_ARB_shader_objects / vertex/fragment_shader) */
        "glCreateShaderObjectARB", "glShaderSourceARB", "glCompileShaderARB",
        "glLinkProgramARB", "glUseProgramObjectARB",
        "glGetObjectParameterivARB", "glGetInfoLogARB",
        "glDeleteObjectARB", "glIsProgramARB",
        /* ARB multitexture (explicit ARB suffix) */
        "glActiveTextureARB", "glMultiTexCoord2fARB", "glMultiTexCoord2fvARB",
        /* ARB VBO map (explicit ARB suffix) */
        "glMapBufferARB", "glUnmapBufferARB", "glGenBuffersARB",
        /* ARB occlusion queries */
        "glBeginQueryARB", "glEndQueryARB", "glGenQueriesARB",
        "glDeleteQueriesARB", "glGetQueryObjectuivARB",
        /* ARB compressed textures (explicit ARB suffix) */
        "glCompressedTexImage2DARB", "glCompressedTexSubImage2DARB",
        /* EXT blend */
        "glBlendColorEXT", "glBlendEquationEXT",
        "glBlendEquationSeparateEXT", "glBlendFuncSeparateEXT",
        /* EXT framebuffer object */
        "glFramebufferRenderbufferEXT",
        /* core 1.2 */
        "glDrawRangeElements",
        /* NV primitive restart / program params */
        "glPrimitiveRestartIndexNV", "glProgramParameters4fvNV",
        /* GLX server string (routes to the glX stub branch below) */
        "glXQueryServerString",

        /* --- GSEVO (Ghost Squad Evolution) GL-extension imports ----------
         * GSEVO's InitOpenGL()/opengl_init() bind the GL 1.5/2.0 *core* names
         * (no ARB suffix) plus a few ARB GLSL/query/point/secondary-color
         * entry points and gluPerspective. ABC used the ARB-suffixed variants,
         * so these core names were never in the table -> their GOT slots bound
         * to 0 -> `call glGenQueries` (the first one InitOpenGL reaches) jumped
         * to NULL. All exist in the RX 580 vendor opengl32.dll / glu32.dll and
         * resolve through the same wglGetProcAddress + cdecl-wrapper path.
         * Additive: ABC imports none of these (it uses the ARB names above). */
        "glGenQueries", "glDeleteQueries", "glBeginQuery", "glEndQuery",
        "glGetQueryObjectuiv", "glGetQueryObjectiv", "glIsQuery",
        "glAttachObjectARB", "glCreateProgramObjectARB",
        "glCreateShaderObjectARB",
        "glGetAttribLocationARB", "glGetProgramivARB",
        "glGetUniformLocationARB",
        "glUniform1iARB", "glUniform1fARB", "glUniform1fvARB", "glUniform1ivARB",
        "glUniform2fvARB", "glUniform3fvARB", "glUniform4fvARB",
        "glUniformMatrix3fvARB", "glUniformMatrix4fvARB",
        "glPointParameterfARB", "glPointParameterfvARB",
        "glSecondaryColorPointer", "glSecondaryColor3fv",
        "gluPerspective",

        /* --- Hummer Extreme Edition GL-extension imports -----------------
         * Hummer uses FBO-EXT fully (create/bind/attach/check/generate mipmap),
         * GL imaging subset (glColorTable* / ARB_imaging), DepthBoundsEXT,
         * 64-bit timer queries, and several NV/ATI/APPLE extensions.
         * The EXT FBO set is a superset of ABC's; only the ones missing from
         * the table above are listed here. */
        /* EXT FBO completions */
        "glBindRenderbufferEXT", "glGenRenderbuffersEXT",
        "glRenderbufferStorageEXT", "glGetRenderbufferParameterivEXT",
        "glIsRenderbufferEXT", "glIsFramebufferEXT",
        "glFramebufferTexture1DEXT", "glFramebufferTexture3DEXT",
        "glGetFramebufferAttachmentParameterivEXT",
        "glGenerateMipmapEXT",
        /* EXT renderbuffer multisample */
        "glRenderbufferStorageMultisampleEXT",
        "glBlitFramebufferEXT",
        /* ARB_imaging / GL imaging subset */
        "glColorTable", "glColorTableParameterfv", "glColorTableParameteriv",
        "glColorSubTable", "glCopyColorTable", "glGetColorTable",
        "glGetColorTableParameterfv", "glGetColorTableParameteriv",
        "glConvolutionFilter2D", "glGetConvolutionFilter",
        /* EXT depth bounds test */
        "glDepthBoundsEXT",
        /* Timer / 64-bit queries */
        "glGetQueryObjecti64vEXT", "glGetQueryObjectui64vEXT",
        "glGetQueryObjecti64v", "glGetQueryObjectui64v",
        /* NV fence / register combiners */
        "glCombinerParameterfvNV", "glCombinerParameterivNV",
        "glCombinerParameterfNV",  "glCombinerParameteriNV",
        "glCombinerInputNV", "glCombinerOutputNV",
        "glFinalCombinerInputNV",
        "glGetCombinerInputParameterfvNV", "glGetCombinerOutputParameterfvNV",
        "glGetFinalCombinerInputParameterfvNV",
        /* NV vertex program extended params */
        "glProgramNamedParameter4fNV", "glProgramNamedParameter4fvNV",
        "glGetProgramNamedParameterfvNV",
        /* ATI / AMD specific */
        "glStencilOpSeparateATI", "glStencilFuncSeparateATI",
        /* APPLE element array */
        "glElementPointerAPPLE", "glDrawElementArrayAPPLE",
        "glDrawRangeElementArrayAPPLE",
        /* EXT coordinate frame / stencil two-side */
        "glActiveStencilFaceEXT",
        /* EXT secondary color (explicit ext suffix variant) */
        "glSecondaryColor3fvEXT", "glSecondaryColorPointerEXT",
        /* WGL-based swap control (via GLX shim) */
        "wglSwapIntervalEXT", "wglGetSwapIntervalEXT",
        NULL
    };

    HMODULE gl  = get_opengl32();
    HMODULE glu = LoadLibraryA("glu32.dll");

    int resolved = 0;
    for (int i = 0; ext_symbols[i]; i++) {
        const char *name = ext_symbols[i];

        /* Already registered? Skip. */
        bool found = false;
        for (int j = 0; j < ctx->symbol_count; j++) {
            if (strcmp(ctx->symbols[j].name, name) == 0 &&
                ctx->symbols[j].addr != 0) {
                found = true; break;
            }
        }
        if (found) continue;

        void *p = NULL;
        /* Functions sourced from opengl32/glu32/wgl are __stdcall (WINAPI): the
         * callee pops its args (ret N). The Linux game calls them __cdecl, so
         * they MUST go through the cdecl adapter or ESP leaks by the arg size.
         * (gluLookAt = 9 doubles = 72 bytes: a raw stdcall gluLookAt did `ret
         * 0x48` and smashed calc_camera_matrix's stack on type-5 ortho cameras
         * -> ret-to-0.) The loader's OWN stubs below (free, pthread no-op, glX)
         * are already cdecl and are left raw. */
        bool from_dll = false;

        /* Try wglGetProcAddress first (extensions). Resolve it dynamically from
         * whichever opengl32.dll is active (Mesa or the vendor ICD) - we no
         * longer static-link opengl32 so the driver can be chosen at runtime. */
        if (!p && gl) {
            static void* (WINAPI *p_wglGPA)(const char*) = 0;
            static HMODULE s_gl = 0;
            if (s_gl != gl) { s_gl = gl; p_wglGPA = (void*)GetProcAddress(gl, "wglGetProcAddress"); }
            if (p_wglGPA) { p = (void*)p_wglGPA(name); if (p) from_dll = true; }
        }
        /* Then opengl32.dll */
        if (!p && gl) {
            p = (void*)GetProcAddress(gl, name);
            if (p) from_dll = true;
        }
        /* Then glu32.dll */
        if (!p && glu) {
            p = (void*)GetProcAddress(glu, name);
            if (p) from_dll = true;
        }
        /* XFree -> HeapFree equivalent stub */
        if (!p && strcmp(name, "XFree") == 0)
            p = (void*)free;
        /* pthread scheduling stubs (setschedparam, etc.) - return 0 = success.
         * The stub bytes MUST live in executable memory: a static .data array
         * is non-executable, so CALLing it faults with DEP (seen as init_sound
         * -> init_adx -> cridlg_SetSchedParam -> pthread_setschedparam crash).
         * Allocate one executable `xor eax,eax; ret` and reuse it. */
        if (!p && strncmp(name, "pthread_", 8) == 0) {
            static uint8_t *pthread_nop = NULL;
            if (!pthread_nop) {
                pthread_nop = (uint8_t*)VirtualAlloc(NULL, 16, MEM_COMMIT|MEM_RESERVE,
                                                     PAGE_EXECUTE_READWRITE);
                if (pthread_nop) { pthread_nop[0]=0x31; pthread_nop[1]=0xC0; pthread_nop[2]=0xC3; }
            }
            p = pthread_nop;
        }
        /* GLX stubs that return fake values */
        if (!p && name[0]=='g' && name[1]=='l' && name[2]=='X') {
            p = stub_glXGetProcAddressARB((const uint8_t*)name);
        }

        /* --- GLSL/ARB shader-object DIAGNOSTIC + COMPAT interception ------
         * Replace the plain cdecl-wrapped vendor fn with our own cdecl wrapper
         * for the five shader-object entry points the GLSL path uses. Purely
         * additive: ABC never imports these (it uses glProgramStringARB). The
         * wrappers forward to the real vendor fn (stored cdecl-callable) and add
         * source/compiler dumps (ULTRA_GLSL_DUMP) + a compat source patch
         * (ULTRA_GLSL_FIX, default on). */
        if (p && from_dll) {
            int handled = 0;
            /* glProgramStringARB: route through the ABC-proven NV->ARB program
             * rewriter (glx_emu.c). GSEVO's "GLCreateVertexShader" (gl_util.cpp
             * :2053) uploads !!ARBvp1.0 + "OPTION NV_vertex_program2;" via this
             * entry point then asserts on glGetIntegerv(PROGRAM_ERROR_POSITION_
             * ARB)!=-1. Without the rewriter the modern AMD GL rejects the NV
             * OPTION line -> errpos!=-1 -> assert. The rewriter strips the NV
             * dialect (same fix ABC needed). ABC reaches this fn via the SDL/
             * generic-binder path, not here, so this is additive for ABC. */
            if (!strcmp(name,"glProgramStringARB")) {
                extern void *ul_install_arb_rewriter(void *realfn);
                void *rw = ul_install_arb_rewriter(p);   /* p = RAW stdcall vendor fn */
                if (rw) {
                    sym_register(ctx, name, (uintptr_t)rw);
                    loader_log(LOG_INFO,"[GLSL] NV->ARB program rewriter installed for %s (real=%p)",name,p);
                    handled = 1;
                }
            } else
            if (!strcmp(name,"glCreateShaderObjectARB")) {
                s_real_CreateShaderObjectARB = (GLhandleARB_(*)(unsigned))make_cdecl_wrapper(p);
                sym_register(ctx, name, (uintptr_t)ul_CreateShaderObjectARB);
                loader_log(LOG_INFO,"[GLSL] diag wrapper installed for %s (real=%p)",name,p);
                handled = 1;
            } else if (!strcmp(name,"glShaderSourceARB")) {
                s_real_ShaderSourceARB = (void(*)(GLhandleARB_,int,const char**,const int*))make_cdecl_wrapper(p);
                sym_register(ctx, name, (uintptr_t)ul_ShaderSourceARB);
                loader_log(LOG_INFO,"[GLSL] diag wrapper installed for %s (real=%p)",name,p);
                handled = 1;
            } else if (!strcmp(name,"glCompileShaderARB")) {
                s_real_CompileShaderARB = (void(*)(GLhandleARB_))make_cdecl_wrapper(p);
                sym_register(ctx, name, (uintptr_t)ul_CompileShaderARB);
                loader_log(LOG_INFO,"[GLSL] diag wrapper installed for %s (real=%p)",name,p);
                handled = 1;
            } else if (!strcmp(name,"glGetObjectParameterivARB")) {
                s_real_GetObjectParameterivARB = (void(*)(GLhandleARB_,unsigned,int*))make_cdecl_wrapper(p);
                sym_register(ctx, name, (uintptr_t)ul_GetObjectParameterivARB);
                loader_log(LOG_INFO,"[GLSL] diag wrapper installed for %s (real=%p)",name,p);
                handled = 1;
            } else if (!strcmp(name,"glGetInfoLogARB")) {
                s_real_GetInfoLogARB = (void(*)(GLhandleARB_,int,int*,char*))make_cdecl_wrapper(p);
                sym_register(ctx, name, (uintptr_t)ul_GetInfoLogARB);
                loader_log(LOG_INFO,"[GLSL] diag wrapper installed for %s (real=%p)",name,p);
                handled = 1;
            }
            if (handled) { resolved++; continue; }
        }

        if (p) {
            /* Wrap stdcall DLL exports to cdecl; leave loader cdecl stubs raw. */
            void *reg = from_dll ? make_cdecl_wrapper(p) : p;
            sym_register(ctx, name, (uintptr_t)reg);
            resolved++;
            loader_log(LOG_DEBUG, "GL ext resolved: %s -> 0x%p%s", name, reg,
                       from_dll ? " (cdecl-wrapped)" : "");
        } else {
            loader_log(LOG_DEBUG, "GL ext unresolved: %s (will stub as no-op)", name);
            /* Install a safe no-op that returns 0 */
            static const uint8_t safe_nop[] = {
                0x55,             /* push ebp */
                0x89,0xE5,        /* mov ebp,esp */
                0x31,0xC0,        /* xor eax,eax */
                0x5D,             /* pop ebp */
                0xC3              /* ret */
            };
            /* Allocate per-symbol to avoid aliasing */
            uint8_t *stub = (uint8_t*)VirtualAlloc(NULL, 16,
                MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (stub) {
                memcpy(stub, safe_nop, sizeof(safe_nop));
                sym_register(ctx, name, (uintptr_t)stub);
                resolved++;
            }
        }
    }
    loader_log(LOG_INFO, "GL extension resolver: %d symbols resolved", resolved);
}
