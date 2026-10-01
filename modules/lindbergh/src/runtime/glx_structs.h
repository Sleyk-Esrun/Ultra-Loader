/* glx_structs.h - Mesa-compatible GLX struct layouts
 *
 * These match the actual Mesa 6.x / DRI1 era struct layouts that the
 * Lindbergh game binary expects. The game reads fields directly from
 * these structs - they must be ABI-compatible with libGL.so.1 on Linux.
 *
 * Sources: Mesa 6.5 src/glx/x11/glxclient.h, glx.h
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <windows.h>

/* ── Forward declarations ─────────────────────────────────────────────── */
typedef struct _GLXContextRec   *GLXContext;
typedef struct _GLXFBConfigRec  *GLXFBConfig;
typedef uint32_t                 GLXDrawable;   /* XID on Linux */
typedef uint32_t                 GLXPbuffer;    /* XID on Linux */
typedef uint32_t                 Display;       /* opaque display ptr */

/* ── GLXFBConfigRec - Mesa 6.x layout ───────────────────────────────────
 * Matches __GLXFBConfigRec in Mesa glxclient.h
 */
typedef struct _GLXFBConfigRec {
    /* Visual/fbconfig attributes */
    int     visualType;         /* 0x8000 = TrueColor */
    int     transparentType;    /* 0 = none */
    int     transparentRed;
    int     transparentGreen;
    int     transparentBlue;
    int     transparentAlpha;
    int     transparentIndex;
    int     visualCaveat;       /* 0x20 = GLX_NONE */

    int     associatedVisualId;
    int     screen;

    int     drawableType;       /* 0x7 = window|pixmap|pbuffer */
    int     renderType;         /* 1 = RGBA */
    int     maxPbufferWidth;
    int     maxPbufferHeight;
    int     maxPbufferPixels;
    int     optimalPbufferWidth;
    int     optimalPbufferHeight;

    int     visualSelectGroup;

    /* Bitfield attributes */
    unsigned int id;            /* fbconfigID / XID */
    unsigned int glxVendorPrivate;

    int     rgbMode;            /* 1 = RGB */
    int     colorIndexMode;     /* 0 */
    int     doubleBufferMode;   /* 1 */
    int     stereoMode;         /* 0 */
    int     haveAccumBuffer;    /* 0 */
    int     haveDepthBuffer;    /* 1 */
    int     haveStencilBuffer;  /* 1 */

    int     accumRedBits;
    int     accumGreenBits;
    int     accumBlueBits;
    int     accumAlphaBits;
    int     depthBits;          /* 24 */
    int     stencilBits;        /* 8 */
    int     indexBits;

    int     redBits;            /* 8 */
    int     greenBits;          /* 8 */
    int     blueBits;           /* 8 */
    int     alphaBits;          /* 8 */
    unsigned int redMask;
    unsigned int greenMask;
    unsigned int blueMask;
    unsigned int alphaMask;

    unsigned int multiSampleSize;
    unsigned int nMultiSampleBuffers;
    int     maxAuxBuffers;
    int     level;
    int     extendedRange;

    double  minRed, maxRed;
    double  minGreen, maxGreen;
    double  minBlue, maxBlue;
    double  minAlpha, maxAlpha;

    /* Our extension: WGL backend */
    int     pf_index;           /* Windows pixel format index */
} GLXFBConfigRec;

/* ── GLXContextRec - Mesa 6.x __GLXcontextRec layout ────────────────────
 * The game may read vtable ptr, currentDpy, currentDrawable etc.
 */
typedef struct _GLXContextRec {
    /* vtable - first field, game may read this */
    void       *vtable;         /* points to our context vtable */

    /* Mesa __GLXcontextRec fields (Mesa 6.5 glxclient.h) */
    Display    *currentDpy;     /* current display */
    GLXDrawable currentDrawable;/* current draw drawable */
    GLXDrawable currentReadable;/* current read drawable */
    uint32_t    xid;            /* context XID */
    uint32_t    glxVendorPrivate;

    /* Context state */
    int         renderType;     /* 1 = RGBA */
    int         screen;
    int         isDirect;       /* 1 = direct rendering */

    /* Thread state */
    uint32_t    thread_id;

    /* Our extension: WGL backend */
    HGLRC       hglrc;
    HDC         hdc;
    bool        is_current;
    bool        valid;

    /* Padding to 128 bytes - game may access beyond known fields */
    uint8_t     _pad[128 - 52];
} GLXContextRec;

/* ── GLXPbufferRec - pbuffer state ───────────────────────────────────── */
typedef struct _GLXPbufferRec {
    uint32_t    xid;            /* drawable XID */
    uint32_t    type;           /* GLX_PBUFFER_BIT = 4 */
    int         width;
    int         height;
    unsigned int fbo;           /* OpenGL FBO */
    unsigned int rb_color;
    unsigned int rb_depth;
    bool        valid;
    uint8_t     _pad[64 - 33];
} GLXPbufferRec;

/* ── Fake Display ────────────────────────────────────────────────────── */
typedef struct {
    int         fd;             /* connection fd - game may read this */
    int         lock;
    void       *heads;          /* screen list */
    int         nscreens;
    int         default_screen;
    uint32_t    resource_base;
    uint32_t    resource_mask;
} FakeDisplay;
