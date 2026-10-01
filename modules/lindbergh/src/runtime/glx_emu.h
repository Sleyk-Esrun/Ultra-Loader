/* glx_emu.h - GLX emulator using Mesa-compatible struct layouts
 *
 * All GLX handles returned to the game are REAL HEAP POINTERS to
 * Mesa-ABI-compatible structs. The game can read any field and get
 * a meaningful value.
 *
 * Lifecycle: emulator owns all objects. free() is intercepted.
 */
#pragma once
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include "glx_structs.h"

#define GLX_EMU_MAGIC  0x474C5845u  /* "GLXE" */
#define GLX_MAX_OBJECTS 64

/* Object type tag (overlaid at end of each struct) */
typedef enum {
    GLX_OBJ_FBCONFIG  = 1,
    GLX_OBJ_CONTEXT   = 2,
    GLX_OBJ_PBUFFER   = 3,
    GLX_OBJ_DISPLAY   = 4,
} glx_obj_type_t;

/* Tag appended after each object so we can identify it in free() */
typedef struct {
    uint32_t       magic;
    glx_obj_type_t type;
    bool           alive;
} glx_tag_t;

/* Tagged wrappers - the game sees a pointer to the mesa struct,
 * we append our tag after it for lifecycle management */
typedef struct {
    GLXFBConfigRec mesa;   /* MUST be first - game sees this */
    glx_tag_t      tag;
    /* GLX ABI: glXChooseFBConfig returns pointer to array of GLXFBConfig.
     * We embed the array here so it's always valid. */
    GLXFBConfig    array[2];
} glx_fbconfig_obj_t;

typedef struct {
    GLXContextRec  mesa;   /* MUST be first - game sees this */
    glx_tag_t      tag;
} glx_context_obj_t;

typedef struct {
    GLXPbufferRec  mesa;   /* MUST be first - game sees this */
    glx_tag_t      tag;
} glx_pbuffer_obj_t;

typedef struct {
    FakeDisplay    mesa;   /* MUST be first */
    glx_tag_t      tag;
} glx_display_obj_t;

/* Global emulator state */
typedef struct {
    bool    initialized;
    HWND    main_hwnd;
    HDC     main_hdc;
    HWND    helper_hwnd;
    HDC     helper_hdc;

    /* WGL function pointers */
    HGLRC (WINAPI *wglCreateContext)(HDC);
    BOOL  (WINAPI *wglDeleteContext)(HGLRC);
    BOOL  (WINAPI *wglMakeCurrent)(HDC, HGLRC);
    HGLRC (WINAPI *wglGetCurrentContext)(void);
    HDC   (WINAPI *wglGetCurrentDC)(void);
    PROC  (WINAPI *wglGetProcAddress)(LPCSTR);
    BOOL  (WINAPI *wglShareLists)(HGLRC, HGLRC);

    /* Current context */
    glx_context_obj_t *current_ctx;
    glx_pbuffer_obj_t *current_draw;  /* set by glXMakeCurrent */
    uint32_t           frame_count;

    /* GL extensions */
    bool  ext_loaded;
    void *glGenFramebuffers, *glBindFramebuffer, *glDeleteFramebuffers;
    void *glGenRenderbuffers, *glBindRenderbuffer, *glRenderbufferStorage;
    void *glFramebufferRenderbuffer, *glBlitFramebuffer;
    void *glFlush, *glViewport;

    /* Object registry for free() interception */
    void *objects[GLX_MAX_OBJECTS];
    int   n_objects;

    /* Singleton display */
    glx_display_obj_t *display;
} glx_state_t;

extern glx_state_t g_glx;

/* Init */
bool glx_emu_init(HWND hwnd, HDC hdc);
void glx_load_extensions(void);

/* Object creation - returns Mesa-compatible heap pointers */
glx_display_obj_t  *glx_get_display(void);
glx_fbconfig_obj_t *glx_choose_fbconfig(const int *attribs, int *n_out);
glx_context_obj_t  *glx_create_context(glx_fbconfig_obj_t *cfg,
                                        glx_context_obj_t *share);
glx_pbuffer_obj_t  *glx_create_pbuffer(glx_fbconfig_obj_t *cfg, int w, int h);

/* Operations */
bool glx_make_current(glx_pbuffer_obj_t *draw, glx_context_obj_t *ctx);
void glx_swap_buffers(glx_pbuffer_obj_t *drawable);
void glx_destroy_context(glx_context_obj_t *ctx);
void glx_destroy_pbuffer(glx_pbuffer_obj_t *pb);

/* free() interception - returns true if we owned the pointer */
bool glx_intercept_free(void *ptr);
