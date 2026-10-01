/* glx_emu.c - GLX emulator with Mesa-compatible struct layouts */
#include <stdio.h>
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "loader.h"
#include "glx_emu.h"
#include "settings.h"

/* === Ultra Loader port compat (tonight's build) ===
 * SDL3 branch used dynlink_register_shim(name,fn) and loader_ctx_t.
 * Tonight uses sym_register(ctx,name,addr) and LoaderCtx. Bridge them. */
typedef LoaderCtx loader_ctx_t;
extern LoaderCtx *g_loader_ctx;
static void dynlink_register_shim(const char *name, void *fn) {
    if (g_loader_ctx) sym_register(g_loader_ctx, name, (uintptr_t)fn);
}
/* glXGetProcAddress lookup: resolve a shim/symbol by name. */
uint32_t dynlink_find_shim(const char *name) {
    if (!g_loader_ctx || !name) return 0;
    return (uint32_t)sym_resolve(g_loader_ctx, name);
}
/* Window dimensions used by the FBO blit (set by sdl_window.c). */
int g_width  = 1280;
int g_height = 720;

/* Replace a failed ARB program with a valid fallback so a REAL GPU driver
 * (native/zink/d3d12) never wedges on a malformed program. OFF for software
 * llvmpipe (it tolerates failed programs and renders better without the
 * substitution). Set per driver in patch_game_gl_got; ULTRA_SHADER_FALLBACK=0/1. */
int g_ul_shader_fallback = 0;

/* ── glTexImage2D compatibility shim ───────────────────────────────────
 * The Lindbergh game targets old Mesa 6.x, which accepted depth-stencil
 * textures with internalformat=GL_DEPTH_COMPONENT (0x1902) + format=
 * GL_DEPTH_STENCIL (0x84F9) + type=GL_UNSIGNED_INT_24_8 (0x84FA).
 * Modern GL (your Mesa/AMD GL 4.6) rejects that combo with GL_INVALID_VALUE
 * (FBuffer::create / fbuffer.cpp:207). Fix it on the fly: when the game asks
 * for a DEPTH_STENCIL texture, force a valid internalformat. */
typedef void (WINAPI *glTexImage2D_t)(unsigned target, int level, int internalformat,
                                      int width, int height, int border,
                                      unsigned format, unsigned type, const void *pixels);
static glTexImage2D_t s_real_glTexImage2D = NULL;

#define UL_GL_DEPTH_COMPONENT     0x1902
#define UL_GL_DEPTH_STENCIL       0x84F9
#define UL_GL_DEPTH24_STENCIL8    0x88F0
#define UL_GL_DEPTH_COMPONENT24   0x81A6
#define UL_GL_UNSIGNED_INT_24_8   0x84FA

/* NOTE: the GAME is a Linux i386 binary and calls GL entry points as __cdecl
 * (caller cleans the stack). This wrapper is installed directly into the game's
 * GOT, so it MUST be __cdecl too. It was previously WINAPI (__stdcall), which
 * made it pop its 9 args (0x24 bytes) on return that the cdecl caller also
 * accounted for - shifting ESP by 36 bytes and corrupting the caller's frame
 * (the FBuffer::create ret-to-zero crash). We still call the REAL Mesa function
 * via s_real_glTexImage2D, which is correctly typed WINAPI (opengl32 is stdcall). */
static void __cdecl ul_glTexImage2D(unsigned target, int level, int internalformat,
                                   int width, int height, int border,
                                   unsigned format, unsigned type, const void *pixels)
{
    /* Safety net for FBuffer::create (fbuffer.cpp:207). If the game ever mis-
     * indexes its resolution-config table (bad DIP/config), it hands us absurd
     * dimensions (e.g. 34037 x 157350044). A modern GL driver rejects those
     * with GL_INVALID_VALUE and the game's error path then crashes. Clamp any
     * implausible size to the configured render resolution so the texture/FBO
     * allocation always succeeds. The real fix is feeding the correct DIP
     * resolution (so the game computes valid dimensions itself); this guard
     * just guarantees we never die here. */
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        int cw = g_width  > 0 && g_width  <= 8192 ? g_width  : 1024;
        int ch = g_height > 0 && g_height <= 8192 ? g_height : 768;
        loader_log(LOG_WARN,
            "[GLCOMPAT] glTexImage2D implausible dims %dx%d -> clamped to %dx%d "
            "(resolution-table mis-index? check DIP/ULTRA_RES)",
            width, height, cw, ch);
        width = cw; height = ch;
    }

    /* TEMP: trace the game's render-target textures (GL_TEXTURE_RECTANGLE_ARB =
     * 0x84F5) - these are the FBuffer scene/3D targets whose size = the "480p"
     * gameplay softness. Log distinct sizes. */
    if (getenv("ULTRA_TEX_TRACE") && target == 0x84F5u) {
        static int sw[48],sh[48],n=0; int seen=0;
        for(int i=0;i<n;i++) if(sw[i]==width&&sh[i]==height){seen=1;break;}
        if(!seen && n<48){ sw[n]=width;sh[n]=height;n++;
            loader_log(LOG_WARN,"[TEXRECT] %dx%d ifmt=0x%x fmt=0x%x type=0x%x frame=%d",
                       width,height,internalformat,format,type,g_glx.frame_count); }
    }
    /* Briefing signature: the movie playback re-uploads a 256x1 RGBA8 color ramp
     * (its LUT) every frame; gameplay never does. Stamp the frame so the present
     * path can detect "a cutscene movie is live" and zoom it to fill. */
    if (target == 0x0DE1u /*GL_TEXTURE_2D*/ && width == 256 && height == 1 &&
        (unsigned)internalformat == 0x8058u /*RGBA8*/) {
        extern int g_ul_swap_frame; extern int g_ul_cut_ramp_f;
        g_ul_cut_ramp_f = g_ul_swap_frame;
    }
    /* Movie/cutscene hunt: log EVERY glTexImage2D during a frame window so we can
     * see which texture (the decoded movie) is (re)uploaded each briefing frame. */
    if (getenv("ULTRA_TEX_TRACE")) { extern int g_ul_swap_frame; int f=g_ul_swap_frame;
        const char *w0=getenv("ULTRA_TEX_FRAME"); int lo = w0?atoi(w0):850;
        if (f>=lo && f<lo+6 && pixels)
            loader_log(LOG_WARN,"[TEXUP] glTexImage2D tgt=0x%x %dx%d ifmt=0x%x fmt=0x%x type=0x%x frame=%d",
                       target,width,height,internalformat,format,type,f);
    }

    int orig_if = internalformat;
    if (format == UL_GL_DEPTH_STENCIL) {
        /* depth-stencil texture: internalformat must be a sized DS format */
        if (internalformat == UL_GL_DEPTH_COMPONENT || internalformat == UL_GL_DEPTH_STENCIL)
            internalformat = UL_GL_DEPTH24_STENCIL8;
    } else if (format == UL_GL_DEPTH_COMPONENT) {
        /* plain depth texture: give it a sized depth format */
        if (internalformat == UL_GL_DEPTH_COMPONENT)
            internalformat = UL_GL_DEPTH_COMPONENT24;
    }
    if (internalformat != orig_if)
        loader_log(LOG_WARN, "[GLCOMPAT] glTexImage2D depth fix: internalformat 0x%x -> 0x%x (fmt=0x%x type=0x%x %dx%d)",
                   orig_if, internalformat, format, type, width, height);
    if (s_real_glTexImage2D)
        s_real_glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}

/* ── Forced anisotropic filtering ──────────────────────────────────────────
 * After Burner (and every Lindbergh flight/driving title) draws huge oblique
 * surfaces - ocean, ground, runway stretching to the horizon. The game's default
 * texture filtering goes blurry at those grazing angles. We wrap glTexParameteri
 * so that whenever the game sets a 2D texture's min/mag filter we ALSO set
 * GL_TEXTURE_MAX_ANISOTROPY (0x84FE) - and upgrade a plain GL_LINEAR min filter to
 * trilinear (LINEAR_MIPMAP_LINEAR) when mipmaps exist - keeping distant textures
 * crisp. Pure quality win, no game-logic effect. g_ul_aniso (env ULTRA_ANISO,
 * settings 'anisotropy') = max anisotropy, 0 = off. */
typedef void (WINAPI *glTexParameteri_t)(unsigned,unsigned,int);
static glTexParameteri_t s_real_glTexParameteri = NULL;
int g_ul_aniso = 16;
static void __cdecl ul_glTexParameteri(unsigned target, unsigned pname, int param)
{
    if (s_real_glTexParameteri) s_real_glTexParameteri(target, pname, param);
    if (g_ul_aniso > 0 && target == 0x0DE1u /*GL_TEXTURE_2D*/ &&
        (pname == 0x2800u /*MAG_FILTER*/ || pname == 0x2801u /*MIN_FILTER*/)) {
        if (s_real_glTexParameteri)
            s_real_glTexParameteri(target, 0x84FEu /*GL_TEXTURE_MAX_ANISOTROPY_EXT*/, g_ul_aniso);
    }
}

/* glTexSubImage2D wrapper - used to HUNT the per-frame movie upload during the
 * cutscene (video decoders update a texture sub-region every frame). cdecl, like
 * the game expects; forwards to the real stdcall fn. Inert without ULTRA_TEX_TRACE. */
typedef void (WINAPI *glTexSubImage2D_t)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
static glTexSubImage2D_t s_real_glTexSubImage2D = NULL;
static void __cdecl ul_glTexSubImage2D(unsigned target,int level,int xo,int yo,
                                       int width,int height,unsigned format,unsigned type,const void*pixels)
{
    if (getenv("ULTRA_TEX_TRACE")) { extern int g_ul_swap_frame; int f=g_ul_swap_frame;
        const char *w0=getenv("ULTRA_TEX_FRAME"); int lo = w0?atoi(w0):850;
        if (f>=lo && f<lo+6)
            loader_log(LOG_WARN,"[TEXSUB] glTexSubImage2D tgt=0x%x off=%d,%d %dx%d fmt=0x%x type=0x%x frame=%d",
                       target,xo,yo,width,height,format,type,f);
    }
    if (s_real_glTexSubImage2D)
        s_real_glTexSubImage2D(target,level,xo,yo,width,height,format,type,pixels);
}

/* ---- cdecl <- stdcall bridge for ALL GL functions -------------------------
 * The game is a Linux ELF: it calls GL __cdecl (caller pops args). Mesa's
 * opengl32 exports GL __stdcall (callee pops args). Dropping a raw stdcall
 * pointer into a GOT slot the game calls cdecl makes esp creep up by the arg
 * size on every call; in a GL-burst function with an `add esp,N` epilogue
 * (e.g. FBuffer::create) the drift makes its `mov $0,[esp+N]` arg writes land
 * on saved return slots -> stack zeroed -> ret to 0. So we wrap every resolved
 * GL pointer in a tiny generated cdecl thunk that forwards to the real fn and
 * restores esp from ebp, fully isolating the game's stack from the callee's
 * arg-popping. Works for any arg count (<=16 dwords) and for stdcall OR cdecl
 * callees, since esp is always restored from the frame pointer. */
static unsigned char *g_glthunk_pool = 0;
static size_t         g_glthunk_used = 0;
#define GLTHUNK_POOL_SZ 262144u
#define GLTHUNK_SZ      36u   /* template is 33 bytes (incl. cld); padded to 36 */

void *gl_cdecl_thunk(void *real_fn)
{
    if (!real_fn) return 0;
    if (!g_glthunk_pool) {
        g_glthunk_pool = (unsigned char*)VirtualAlloc(0, GLTHUNK_POOL_SZ,
                            MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_glthunk_pool) return real_fn; /* no pool: fall back to raw */
    }
    if (g_glthunk_used + GLTHUNK_SZ > GLTHUNK_POOL_SZ) return real_fn;
    unsigned char *t = g_glthunk_pool + g_glthunk_used;
    g_glthunk_used += GLTHUNK_SZ;
    static const unsigned char tpl[GLTHUNK_SZ] = {
        0x55,                       /* push ebp              */
        0x89,0xE5,                  /* mov  ebp,esp          */
        0x56,                       /* push esi              */
        0x57,                       /* push edi              */
        0x83,0xEC,0x40,             /* sub  esp,64           */
        0x8D,0x75,0x08,             /* lea  esi,[ebp+8]      */
        0x89,0xE7,                  /* mov  edi,esp          */
        0xB9,0x10,0x00,0x00,0x00,   /* mov  ecx,16           */
        0xFC,                       /* cld (forward copy!)   */
        0xF3,0xA5,                  /* rep movsd             */
        0xE8,0x00,0x00,0x00,0x00,   /* call real_fn (rel32)  */
        0x8D,0x65,0xF8,             /* lea  esp,[ebp-8]      */
        0x5F,                       /* pop  edi              */
        0x5E,                       /* pop  esi              */
        0x5D,                       /* pop  ebp              */
        0xC3                        /* ret                   */
    };
    memcpy(t, tpl, GLTHUNK_SZ);
    /* After the added cld: E8 opcode at offset 21; rel32 at 22..25; next instr at t+26 */
    uint32_t rel = (uint32_t)((uintptr_t)real_fn - (uintptr_t)(t + 26));
    t[22] = (unsigned char)(rel        & 0xff);
    t[23] = (unsigned char)((rel >> 8) & 0xff);
    t[24] = (unsigned char)((rel >> 16)& 0xff);
    t[25] = (unsigned char)((rel >> 24)& 0xff);
    return t;
}

/* ------------------------------------------------------------------ *
 * ARB program rewriter - the game's fragment programs were authored for
 * the Lindbergh's NVIDIA GeForce 7 (NV_fragment_program2): they start with
 * `OPTION NV_fragment_program2;` and use half-precision opcodes (MOVH, MADH,
 * DP3H, RSQH, LRPH, ...). Mesa/libgallium rejects the NV option ("invalid ARB
 * fragment program option"), so every such shader fails to compile and nothing
 * renders. We intercept glProgramStringARB, strip the NV OPTION line, and
 * downgrade the *H opcodes to their standard ARB_fragment_program equivalents
 * (the only difference is full vs half precision - visually negligible). The
 * RECT texture targets they use are already accepted by Mesa (ARB_texture_rectangle).
 * ------------------------------------------------------------------ */
static int arb_tokchar(char c) {
    return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';
}
/* Translate the NVIDIA GeForce-7 program dialect (NV_vertex_program3 /
 * NV_fragment_program2) the game ships down to plain ARB_vertex/fragment_program
 * that Mesa accepts. Returns the (possibly grown) length.  Handles, in order:
 *   - NV `OPTION` directives           -> blanked (ARB_* options kept)
 *   - SHORT / LONG precision qualifiers -> blanked  (NV decl precision hints)
 *   - half-precision opcodes (*H)       -> full precision (drop trailing H)
 *   - condition-code opcodes (*C)       -> drop the CC update (drop trailing C)
 *   - ARL dst , src                     -> ARL dst.x , src  (ARB needs a 1-mask)
 * Not yet handled (those programs still fail, gracefully): (CC) conditional
 * write masks and BRA/branch control flow used by a few sprite/per-light shaders. */
static int rewrite_arb_program(char *s, int len, int cap) {
    static const char *halfops[] = {
        "MOVH","ADDH","SUBH","MULH","MADH","DP3H","DP4H","RSQH","RCPH","LRPH",
        "MINH","MAXH","DSTH","FRCH","SLTH","SGEH","FLRH","POWH","EXPH","LOGH",
        "SINH","COSH","TEXH","TXPH","TXBH", 0
    };
    static const char *ccops[] = {
        "MOVC","ADDC","SUBC","MULC","MADC","DP3C","DP4C","DPHC","MINC","MAXC",
        "RCPC","RSQC","EX2C","LG2C","FLRC","FRCC","SLTC","SGEC","POWC","LRPC",
        "DSTC","SINC","COSC","ABSC","DP2C", 0
    };
    /* (1) Blank every `OPTION ... ;` directive that references an NV_ extension.
     * Substring scan (NOT token-based): the header glues straight into the
     * option in many programs ("!!ARBfp1.0OPTION NV_fragment_program2;"), so a
     * token scan would read "0OPTION" and miss it. Keep newlines for line nums. */
    for (int i = 0; i + 6 <= len; ) {
        if (!(s[i]=='O'&&s[i+1]=='P'&&s[i+2]=='T'&&s[i+3]=='I'&&s[i+4]=='O'&&s[i+5]=='N')) { i++; continue; }
        int k = i + 6; while (k < len && s[k] != ';') k++;
        if (k >= len) break;
        int hasNV = 0;
        for (int q = i; q + 3 <= k; q++) if (s[q]=='N'&&s[q+1]=='V'&&s[q+2]=='_') { hasNV = 1; break; }
        if (hasNV) for (int q = i; q <= k; q++) { if (s[q] != '\n') s[q] = ' '; }
        i = k + 1;
    }
    /* (2) Token pass: blank SHORT/LONG qualifiers; drop the trailing H/C from
     * half-precision and condition-code opcodes. All whitespace/';'-delimited
     * tokens, so token detection is safe. */
    for (int i = 0; i < len; ) {
        int tokstart = (i == 0 || !arb_tokchar(s[i-1])) && arb_tokchar(s[i]);
        if (!tokstart) { i++; continue; }
        int j = i; while (j < len && arb_tokchar(s[j])) j++;
        int toklen = j - i;
        if ((toklen == 5 && strncmp(s+i,"SHORT",5)==0) ||
            (toklen == 4 && strncmp(s+i,"LONG",4)==0)) {
            for (int q = i; q < j; q++) s[q] = ' ';   /* blank the qualifier */
            i = j; continue;
        }
        /* THE OPCODE ENDS AT '_', NOT AT THE END OF THE TOKEN.
         *
         * NV_fragment_program2's grammar is  op [R|H|X] [C] [_SAT], and
         * arb_tokchar() accepts '_', so "MADH_SAT" scans as ONE 8-character
         * token. Matching the WHOLE token against these tables therefore missed
         * every saturating opcode: strlen("MADH") is 4, toklen was 8, no entry
         * ever matched, the half-precision H was never stripped, and the driver
         * rejected the mnemonic outright -- taking the ENTIRE program down to
         * the flat passthrough fallback below. Saturating arithmetic is
         * pervasive in GeForce-7-era lighting, so this was not an edge case: it
         * is why After Burner's ships, exhaust plumes and missile trails
         * rendered untextured and flat. (Owner-reported 2026-09-19; the comment
         * at the fallback already recorded that ABC materials were landing
         * there, without anyone knowing why.)
         *
         * So: measure the opcode up to '_' and blank the suffix letter just
         * before it. SHORT/LONG above still want the full token length and are
         * unaffected -- neither ever carries a '_'. */
        int opend = i; while (opend < j && s[opend] != '_') opend++;
        int oplen = opend - i;
        for (const char **op = halfops; *op; op++)
            if (oplen == (int)strlen(*op) && strncmp(s+i, *op, oplen) == 0) { s[opend-1] = ' '; break; }
        for (const char **op = ccops; *op; op++)
            if (oplen == (int)strlen(*op) && strncmp(s+i, *op, oplen) == 0) { s[opend-1] = ' '; break; }
        i = j;
    }
    /* (2b) Size implicitly-sized PARAM arrays. ARB_vertex_program rejects a
     * relatively-addressed array declared `PARAM mat_world[] = {...}` - a
     * relative-indexed array must have a statically-known size. Count elements:
     * a `program.env[LO .. HI]` (or matrix) range = HI-LO+1, else top-level
     * commas+1. (mat_world[]->[12], fresnel_factor[]->[15].) Sizing every
     * empty-bracket array is safe & semantics-neutral (declaration text only). */
    for (int i = 0; i + 5 < len; ) {
        if (!((i==0||!arb_tokchar(s[i-1])) && strncmp(s+i,"PARAM",5)==0 &&
              (i+5>=len || !arb_tokchar(s[i+5])))) { i++; continue; }
        int j = i + 5;
        while (j < len && (s[j]==' '||s[j]=='\t')) j++;
        while (j < len && arb_tokchar(s[j])) j++;              /* array name */
        while (j < len && (s[j]==' '||s[j]=='\t')) j++;
        if (j >= len || s[j] != '[') { i += 5; continue; }
        int lb = j++;
        while (j < len && (s[j]==' '||s[j]=='\t')) j++;
        if (j >= len || s[j] != ']') { i = lb + 1; continue; } /* already sized */
        int rb = j;
        int ob = rb; while (ob < len && s[ob] != '{' && s[ob] != ';') ob++;
        if (ob >= len || s[ob] != '{') { i = rb + 1; continue; }
        int cb = ob; while (cb < len && s[cb] != '}') cb++;
        if (cb >= len) { i = rb + 1; continue; }
        int count = -1;
        for (int q = ob; q + 1 < cb; q++) if (s[q]=='.' && s[q+1]=='.') {
            int a = q - 1; while (a > ob && (s[a]==' '||s[a]=='\t')) a--;
            while (a > ob && s[a-1]>='0' && s[a-1]<='9') a--;
            int lo = atoi(s + a);
            int b = q + 2; while (b < cb && (s[b]==' '||s[b]=='\t')) b++;
            int hi = atoi(s + b);
            count = hi - lo + 1; break;
        }
        if (count < 1) { int depth = 0; count = 1;
            for (int q = ob+1; q < cb; q++) { if (s[q]=='[') depth++;
                else if (s[q]==']') { if (depth>0) depth--; }
                else if (s[q]==',' && depth==0) count++; } }
        char num[8]; int nn = snprintf(num, sizeof num, "%d", count);
        if (count >= 1 && count <= 512 && len + nn < cap) {
            memmove(s+rb+nn, s+rb, len-rb);
            memcpy(s+rb, num, nn);
            len += nn; cb += nn;
        }
        i = cb + 1;
    }
    /* (3) ARL dst,src -> ARL dst.x, src.x : ARB requires a single-component write
     * mask on the address-register load AND a scalar source operand; the NV form
     * omits both. This INSERTS ".x" (buffer grows; caller has headroom). */
    for (int i = 0; i + 3 <= len; ) {
        int at = (i==0 || !arb_tokchar(s[i-1])) &&
                 s[i]=='A'&&s[i+1]=='R'&&s[i+2]=='L' &&
                 (i+3>=len || !arb_tokchar(s[i+3]));
        if (!at) { i++; continue; }
        int j = i + 3;
        while (j < len && (s[j]==' '||s[j]=='\t')) j++;     /* skip to dest */
        int ds = j;
        while (j < len && arb_tokchar(s[j])) j++;           /* dest token   */
        if (j > ds && (j >= len || s[j] != '.') && len + 2 < cap) {
            memmove(s+j+2, s+j, len-j);
            s[j] = '.'; s[j+1] = 'x';
            len += 2;
            j += 2;
        }
        if (j < len && s[j]=='.') { j++; while (j<len && arb_tokchar(s[j])) j++; } /* skip dest swizzle */
        while (j < len && (s[j]==' '||s[j]=='\t')) j++;
        if (j < len && s[j]==',') {                         /* -> the source */
            j++;
            while (j < len && (s[j]==' '||s[j]=='\t')) j++;
            if (j < len && s[j]=='-') j++;                  /* optional sign */
            int ss = j;
            while (j < len && arb_tokchar(s[j])) j++;       /* source token  */
            if (j > ss && (j >= len || s[j] != '.') && len + 2 < cap) {
                memmove(s+j+2, s+j, len-j);
                s[j] = '.'; s[j+1] = 'x';
                len += 2; j += 2;
            }
        }
        i = j;
    }
    /* (3b) Multi-component address registers (matrix-palette / bone skinning).
     * Plain ARB_vertex_program permits relative addressing ONLY through the .x
     * component of the address register; NV_vertex_program3 allowed .x/.y/.z/.w.
     * The game's 4-bone skinning loads all four (ARL _adr, a_mtxidx) then indexes
     * mtxpal[_adr.y+n], mtxpal[_adr.z+n], mtxpal[_adr.w+n]. Step (3) above reduced
     * the load to "_adr.x" only, so _adr.y/.z/.w reference an unloaded lane and the
     * driver rejects the program ("invalid address component") -> the whole shader
     * falls back to flat passthrough and every skinned model collapses.
     *   Fix: reuse the single legal .x lane. Before the first use of each higher
     * component, reload it from the matching SOURCE lane (ARL _adr.x, <src>.<c>),
     * then rewrite that component to .x. The per-bone blocks are emitted in
     * x,y,z,w order, so a reused .x register reproduces the original semantics. */
    {
        int ai = -1;
        for (int q = 0; q + 3 <= len; q++)
            if ((q==0||!arb_tokchar(s[q-1])) && s[q]=='A'&&s[q+1]=='R'&&s[q+2]=='L'
                && (q+3>=len||!arb_tokchar(s[q+3]))) { ai = q; break; }
        if (ai >= 0) {
            int j = ai + 3;
            while (j < len && (s[j]==' '||s[j]=='\t')) j++;
            int as = j; while (j < len && arb_tokchar(s[j])) j++;   /* address-reg name */
            int al = j - as;
            if (al > 0 && al < 24) {
                char areg[24]; memcpy(areg, s+as, al); areg[al] = 0;
                if (j < len && s[j]=='.') { j++; while (j<len && arb_tokchar(s[j])) j++; }  /* skip .x */
                while (j < len && (s[j]==' '||s[j]=='\t')) j++;
                if (j < len && s[j]==',') { j++;
                    while (j < len && (s[j]==' '||s[j]=='\t')) j++;
                    if (j < len && s[j]=='-') j++;
                    int ss = j; while (j < len && arb_tokchar(s[j])) j++; /* source base (no swizzle) */
                    int sl = j - ss;
                    if (sl > 0 && sl < 24) {
                        char src[24]; memcpy(src, s+ss, sl); src[sl] = 0;
                        /* RELOAD BEFORE *EVERY* USE, NOT JUST THE FIRST.
                         *
                         * The previous version inserted ONE reload ahead of the FIRST use of
                         * areg.<c> and then rewrote EVERY areg.<c> in the program to areg.x. Its
                         * own comment admitted the assumption: that the per-bone blocks appear in
                         * x,y,z,w order, each component used once and never revisited.
                         *
                         * FOUR-BONE SKINNING DOES NOT DO THAT. It interleaves -- position, then
                         * normal, then tangent, per bone -- so a component is revisited after a
                         * later one has already overwritten the single shared .x lane. Every use
                         * after the first then indexes the WRONG bone matrix, vertices blend
                         * against matrices belonging to another bone, and the mesh tears apart.
                         * On screen: jagged, spiky, collapsed geometry -- owner-reported on After
                         * Burner Climax as a sharp polygon mess (2026-09-19).
                         *
                         * ARB_vertex_program has exactly ONE legal address lane, so the only
                         * correct translation is to reload it immediately before each use. That
                         * costs one extra ARL per relative access; ARL is cheap, and wrong
                         * geometry is not a trade worth making. */
                        const char comps[3] = { 'y','z','w' };
                        for (int ci = 0; ci < 3; ci++) {
                            char c = comps[ci];
                            for (int guard = 0; guard < 64; guard++) {
                                int use = -1;
                                for (int q = 0; q + al + 2 <= len; q++)
                                    if ((q==0||!arb_tokchar(s[q-1])) && strncmp(s+q, areg, al)==0 &&
                                        s[q+al]=='.' && s[q+al+1]==c) { use = q; break; }
                                if (use < 0) break;                       /* none left */
                                int st = use;
                                while (st > 0 && s[st-1] != ';' && s[st-1] != 10) st--;
                                char ins[64];
                                int il = snprintf(ins, sizeof ins, "ARL %s.x, %s.%c; ", areg, src, c);
                                /* No room to do it correctly? Rewriting the use WITHOUT its reload
                                 * would silently use the wrong matrix. Leave it and let the driver
                                 * reject the program -- a visible failure beats silent bad geometry. */
                                if (il <= 0 || len + il >= cap) break;
                                memmove(s+st+il, s+st, len-st);
                                memcpy(s+st, ins, il);
                                len += il;
                                use += il;                                /* the use moved */
                                s[use+al+1] = 'x';                        /* THIS use only */
                            }
                        }
                    }
                }
            }
        }
    }
    /* (4) |x| absolute-value operand -> ABS hoist. ARB has no |...| operand
     * modifier. For each |operand|, emit "ABS __abN, operand;" just before the
     * statement and replace |operand| with __abN. Then declare the __ab temps. */
    {
        int nabs = 0;
        for (int guard = 0; guard < 32; guard++) {
            int bar = -1;
            for (int q = 0; q < len; q++) if (s[q]=='|') { bar = q; break; }
            if (bar < 0) break;
            int oc = bar + 1; while (oc < len && s[oc] != '|') oc++;
            if (oc >= len) { s[bar] = ' '; break; }               /* unmatched */
            int oplen = oc - (bar + 1);
            if (oplen <= 0 || oplen > 40 || len + 96 >= cap) { s[bar]=' '; s[oc]=' '; continue; }
            char op[48]; memcpy(op, s+bar+1, oplen); op[oplen] = 0;
            char tmp[12]; int tl = snprintf(tmp, sizeof tmp, "__ab%d", nabs);
            /* replace the |op| span [bar..oc] with tmp */
            memmove(s+bar+tl, s+oc+1, len-(oc+1));
            memcpy(s+bar, tmp, tl);
            len += tl - (oc - bar + 1);
            /* hoist ABS before the statement (prev ';' or '\n') */
            int st = bar; while (st > 0 && s[st-1] != ';' && s[st-1] != '\n') st--;
            char hoist[80]; int hl = snprintf(hoist, sizeof hoist, "ABS %s, %s; ", tmp, op);
            memmove(s+st+hl, s+st, len-st);
            memcpy(s+st, hoist, hl);
            len += hl;
            nabs++;
        }
        if (nabs > 0) {                                            /* declare the temps */
            char decl[256]; int dl = snprintf(decl, sizeof decl, "TEMP ");
            for (int k = 0; k < nabs; k++) dl += snprintf(decl+dl, sizeof decl-dl, "%s__ab%d", k?",":"", k);
            dl += snprintf(decl+dl, sizeof decl-dl, ";\n");
            int hn = 0; while (hn < len && s[hn] != '\n') hn++;    /* after header line */
            if (hn < len && len + dl < cap) { hn++;
                memmove(s+hn+dl, s+hn, len-hn); memcpy(s+hn, decl, dl); len += dl; }
        }
    }
    /* (5) BRA branch-flatten (per-light skips). The game gates each point light:
     *     MOV tmp.w, plitN_param.x;  BRA skip (EQ.w);   # skip if disabled (==0)
     *     ...attenuation -> tmp.w...; BRA skip (LT.w);  # skip if out-of-range (<0)
     *     ...accumulate light...    ; skip:
     * ARB has no control flow, so we predicate: a disabled light has zero params
     * (its contribution multiplies out to ~0) so we DROP the EQ branch; for the
     * LT branch we clamp the tested register with MAX reg,reg,0 (a negative
     * attenuation -> 0 contribution, same as skipping). Then we drop the labels. */
    for (int guard = 0; guard < 64; guard++) {
        int b = -1;
        for (int q = 0; q + 3 <= len; q++)
            if ((q==0||!arb_tokchar(s[q-1])) && s[q]=='B'&&s[q+1]=='R'&&s[q+2]=='A'
                && (q+3>=len||!arb_tokchar(s[q+3]))) { b = q; break; }
        if (b < 0) break;
        int se = b; while (se < len && s[se] != ';') se++;
        if (se >= len) { s[b]=s[b+1]=s[b+2]=' '; continue; }
        /* parse "(<CO>.<c>)" */
        int lp = b; while (lp < se && s[lp] != '(') lp++;
        char c0=0,c1=0;
        if (lp + 2 < se) { c0=s[lp+1]; c1=s[lp+2]; }
        char repl[80]; int rl = 0;
        if (c0=='L' && c1=='T') {           /* LT: clamp the preceding dest to >=0 */
            int q = b-1; while (q>=0 && (s[q]==' '||s[q]=='\t'||s[q]=='\n'||s[q]=='\r')) q--;
            if (q>=0 && s[q]==';') {
                int pstart = q-1; while (pstart>=0 && s[pstart]!=';' && s[pstart]!='\n') pstart--; pstart++;
                int t=pstart; while (t<q && (s[t]==' '||s[t]=='\t')) t++;
                while (t<q && arb_tokchar(s[t])) t++;            /* opcode */
                while (t<q && (s[t]==' '||s[t]=='\t')) t++;
                int dstart=t; while (t<q && (arb_tokchar(s[t])||s[t]=='.')) t++;
                int dlen=t-dstart;
                if (dlen>0 && dlen<24) { char dst[28]; memcpy(dst,s+dstart,dlen); dst[dlen]=0;
                    rl = snprintf(repl,sizeof repl,"MAX %s, %s, 0.0;",dst,dst); }
            }
        }
        int span = se - b + 1;
        if (rl > 0 && rl <= span) { memcpy(s+b,repl,rl); for(int q=b+rl;q<=se;q++) s[q]=' '; }
        else if (rl > 0 && len+(rl-span) < cap) { memmove(s+b+rl,s+se+1,len-(se+1)); memcpy(s+b,repl,rl); len += rl-span; }
        else { for(int q=b;q<=se;q++) s[q]=' '; }     /* EQ / unparsed: drop the branch */
    }
    /* (5b) Drop NV labels: any `identifier :` (ARB uses ':' nowhere else). */
    for (int i = 0; i < len; ) {
        int ts = (i==0||!arb_tokchar(s[i-1])) && arb_tokchar(s[i]);
        if (!ts) { i++; continue; }
        int j=i; while (j<len && arb_tokchar(s[j])) j++;
        int k=j; while (k<len && (s[k]==' '||s[k]=='\t')) k++;
        if (k<len && s[k]==':') { for(int q=i;q<=k;q++) if(s[q]!='\n') s[q]=' '; i=k+1; }
        else i=j;
    }
    /* (6) Condition-code WRITE-MASK (fragment): `OP dst(LT.s), args;` writes dst
     * only where the CC's .s lane < 0; the CC source is dst's current value. ARB
     * has no CC, so split into: compute the value into __ccN, then CMP-select on
     * dst (CMP dst,a,b,c => (a<0)?b:c). Only LT is used by ABC; others are dropped. */
    {
        int ncc = 0;
        for (int guard = 0; guard < 16; guard++) {
            int lp = -1;
            for (int q = 1; q + 5 < len; q++)
                if (s[q]=='(' && arb_tokchar(s[q-1]) && s[q+3]=='.' && s[q+5]==')'
                    && s[q+1]>='A'&&s[q+1]<='Z' && s[q+2]>='A'&&s[q+2]<='Z') { lp = q; break; }
            if (lp < 0) break;
            char cc0=s[lp+1], cc1=s[lp+2], comp=s[lp+4]; int rp=lp+5;
            int de=lp, ds=de; while (ds>0 && arb_tokchar(s[ds-1])) ds--;
            int dl=de-ds;
            if (cc0!='L'||cc1!='T'||dl<=0||dl>=24) { for(int q=lp;q<=rp;q++) s[q]=' '; continue; }
            char dst[24]; memcpy(dst,s+ds,dl); dst[dl]=0;
            int oe=ds; while (oe>0 && (s[oe-1]==' '||s[oe-1]=='\t')) oe--;
            int os=oe; while (os>0 && arb_tokchar(s[os-1])) os--;
            int ol=oe-os;
            if (ol<=0||ol>=8) { for(int q=lp;q<=rp;q++) s[q]=' '; continue; }
            char op[8]; memcpy(op,s+os,ol); op[ol]=0;
            int ae=rp+1; while (ae<len && s[ae]!=';') ae++;
            int as=rp+1; while (as<ae && (s[as]==' '||s[as]==','||s[as]=='\t')) as++;
            int al=ae-as;
            if (al<=0||al>=96||ae>=len) { for(int q=lp;q<=rp;q++) s[q]=' '; continue; }
            char args[100]; memcpy(args,s+as,al); args[al]=0;
            char repl[240];
            /* INDEX BY ncc, NOT guard. The TEMP declaration below emits
             * __cc0..__cc(ncc-1), and ncc counts only SUCCESSFUL rewrites --
             * but five `continue` paths above advance `guard` while leaving ncc
             * alone (non-LT condition code, dest/opcode/arg too long, buffer
             * cannot grow). The first such skip desynchronised the two forever,
             * so the emitted text referenced a temp that was never declared:
             * "invalid temporary register" at link time, the whole program
             * discarded, the material silently flat. One unusual instruction
             * anywhere in a program poisoned every later rewrite in it. */
            int rl = snprintf(repl,sizeof repl,"%s __cc%d.%c, %s; CMP %s.%c, %s.%c, __cc%d.%c, %s.%c;",
                              op,ncc,comp,args, dst,comp, dst,comp, ncc,comp, dst,comp);
            int span = ae-os+1;
            if (rl<=span) { memcpy(s+os,repl,rl); for(int q=os+rl;q<=ae;q++) s[q]=' '; }
            else if (len+(rl-span)<cap) { memmove(s+os+rl,s+ae+1,len-(ae+1)); memcpy(s+os,repl,rl); len+=rl-span; }
            else { for(int q=lp;q<=rp;q++) s[q]=' '; continue; }
            ncc++;
        }
        if (ncc > 0) {                                    /* declare __cc temps */
            char decl[200]; int dl = snprintf(decl,sizeof decl,"TEMP ");
            for (int k=0;k<ncc;k++) dl += snprintf(decl+dl,sizeof decl-dl,"%s__cc%d",k?",":"",k);
            dl += snprintf(decl+dl,sizeof decl-dl,";\n");
            int hn=0; while (hn<len && s[hn]!='\n') hn++;
            if (hn<len && len+dl<cap) { hn++; memmove(s+hn+dl,s+hn,len-hn); memcpy(s+hn,decl,dl); len+=dl; }
        }
    }
    return len;
}

typedef void (__stdcall *pglProgramStringARB)(unsigned, unsigned, int, const void*);
static pglProgramStringARB g_real_glProgramStringARB = 0;

void __cdecl stub_glProgramStringARB(unsigned target, unsigned format, int len, const void *string)
{
    static char buf[1 << 17];   /* 128KB; programs are <2KB. Render thread only. */
    if (!g_real_glProgramStringARB || !string) return;
    int n = len; if (n < 0) n = 0; if (n > (int)sizeof(buf)-1) n = sizeof(buf)-1;
    memcpy(buf, string, n); buf[n] = 0;
    n = rewrite_arb_program(buf, n, (int)sizeof(buf) - 1);   /* may grow (ARL .x) */
    buf[n] = 0;

    static unsigned (__stdcall *p_err)(void) = 0;
    static const unsigned char* (__stdcall *p_str)(unsigned) = 0;
    static void (__stdcall *p_iv)(unsigned,int*) = 0;
    static int resolved = 0;
    if (!resolved) { HMODULE gl = GetModuleHandleA("opengl32.dll"); if(!gl) gl=LoadLibraryA("opengl32.dll");
        p_err=(void*)GetProcAddress(gl,"glGetError"); p_str=(void*)GetProcAddress(gl,"glGetString");
        p_iv=(void*)GetProcAddress(gl,"glGetIntegerv"); resolved=1; }

    /* Only touch glGetError when the fallback is active (hardware backends).
     * On llvmpipe we leave the error state exactly as the game expects it. */
    if (g_ul_shader_fallback && p_err) p_err();          /* clear prior error */
    g_real_glProgramStringARB(target, format, n, buf);
    unsigned e = (g_ul_shader_fallback && p_err) ? p_err() : 0;

    if (e) {
        /* COUNT AND LOG EVERY FAILURE. Two shader bugs lived here for months
         * precisely because this path was silent: the _SAT token bug and the
         * __cc index desync (both fixed 2026-09-19) each discarded whole
         * programs and left materials flat, with nothing in any log to say so.
         * One line per distinct failure, plus a running total, so the next
         * regression announces itself instead of being reported as "the game
         * looks wrong". Cheap: programs are compiled once, not per frame. */
        static int s_fail_count = 0;
        ++s_fail_count;
        if (s_fail_count <= 20) {
            const char *why = (target == 0x8620) ? "vertex" : "fragment";
            loader_log(LOG_WARN, "[SHADER] %s program REJECTED by the driver "
                       "(GL error 0x%04x) -- falling back to flat passthrough. failure #%d",
                       why, e, s_fail_count);
        } else if (s_fail_count == 21) {
            loader_log(LOG_WARN, "[SHADER] (further program failures suppressed)");
        }
        /* The (rewritten) program still failed to compile -- a handful of
         * sprite/per-light shaders use NV branches / condition-code write masks
         * we don't yet translate.  On a SOFTWARE driver (llvmpipe) a failed
         * program is harmless, but on a REAL GPU driver (AMD, Zink, d3d12) the
         * game deadlocks at the first draw that binds it.  Install a minimal
         * VALID fallback so the driver always has a well-formed program: the
         * affected object renders plainly instead of wedging the GPU. */
        if (getenv("ULTRA_ARB_DEBUG")) {
            int pos = -1; if (p_iv) p_iv(0x864Bu /*PROGRAM_ERROR_POSITION_ARB*/, &pos);
            const unsigned char *es = p_str ? p_str(0x8874u /*PROGRAM_ERROR_STRING_ARB*/) : 0;
            loader_log(LOG_ERROR, "[ARB] err=0x%x pos=%d target=0x%x: %s -> fallback installed",
                       e, pos, target, es?(const char*)es:"?");
            /* Dump the (rewritten) failing program to a file for offline analysis
             * of the BRA/CC constructs. One file per distinct error position. */
            static int seenpos[32]; static int dn = 0; int dup=0;
            for (int i=0;i<dn;i++) if (seenpos[i]==pos) { dup=1; break; }
            if (!dup && dn < 32) { seenpos[dn++] = pos;
                char path[300];
                /* Dump dir: ULTRA_DUMP_DIR if set, else cwd. The old hardcoded
                 * C:\Users\Sleyk\Desktop path silently failed once the build moved
                 * to I:\ (fopen NULL on a missing dir). Diagnostic-only. */
                const char *dd = getenv("ULTRA_DUMP_DIR");
                if (dd && *dd) snprintf(path,sizeof path,"%s\\ul_shaderfail_%x_pos%d.txt",dd,target,pos);
                else           snprintf(path,sizeof path,"ul_shaderfail_%x_pos%d.txt",target,pos);
                FILE *ff = fopen(path,"wb");
                if (ff) { fprintf(ff,"// err=0x%x pos=%d target=0x%x: %s\n",e,pos,target,es?(const char*)es:"?");
                          fwrite(buf,1,strlen(buf),ff); fclose(ff);
                          loader_log(LOG_WARN,"[ARB] dumped failing program -> %s",path); } }
        }
        /* Vertex fallback: transform the position, emit a vertex color, and pass
         * ALL FOUR texcoord sets through. The programs that reach this fallback are
         * the title's per-vertex hardware-lighting shaders (NV vertex_program3 with
         * CAL/RET subroutines + nested ARL array indexing + relatively-indexed
         * PARAM arrays - constructs ARB_vertex_program can't express).
         *
         * The color source is TITLE-GATED:
         *  - GSEVO (g_ul_gsevo): the OLD `MOV result.color, vertex.color` path
         *    passed GSEVO's green-dominant default vertex color straight through ->
         *    uniform green cast on every lit surface. For GSEVO we emit WHITE so
         *    texture*white = the texture's own albedo (flat unlit, but correctly
         *    colored).
         *  - EVERY OTHER TITLE (incl. ABC): pass vertex.color (and the secondary
         *    specular color) through, so the game's PRE-COMPUTED per-vertex
         *    lighting/material color drives the surface rather than a flat white.
         *    This restores diffuse/specular shading on ABC ship bodies when their
         *    NV programs hit the fallback (the old white-replacement was the cause
         *    of the "flat dog-shit" ship look).
         * Passing texcoord[0..3] (not just 0..1) keeps multi-texture + projected-
         * lightmap coords mapped. */
        extern int g_ul_gsevo;
        const char *fb_vp_white =
            "!!ARBvp1.0\n"
            "DP4 result.position.x, state.matrix.mvp.row[0], vertex.position;\n"
            "DP4 result.position.y, state.matrix.mvp.row[1], vertex.position;\n"
            "DP4 result.position.z, state.matrix.mvp.row[2], vertex.position;\n"
            "DP4 result.position.w, state.matrix.mvp.row[3], vertex.position;\n"
            "MOV result.color, {1,1,1,1};\n"
            "MOV result.color.secondary, {0,0,0,0};\n"
            "MOV result.texcoord[0], vertex.texcoord[0];\n"
            "MOV result.texcoord[1], vertex.texcoord[1];\n"
            "MOV result.texcoord[2], vertex.texcoord[2];\n"
            "MOV result.texcoord[3], vertex.texcoord[3];\n"
            "MOV result.fogcoord.x, vertex.position.z;\n"
            "END\n";
        const char *fb_vp_lit =
            "!!ARBvp1.0\n"
            "DP4 result.position.x, state.matrix.mvp.row[0], vertex.position;\n"
            "DP4 result.position.y, state.matrix.mvp.row[1], vertex.position;\n"
            "DP4 result.position.z, state.matrix.mvp.row[2], vertex.position;\n"
            "DP4 result.position.w, state.matrix.mvp.row[3], vertex.position;\n"
            "MOV result.color, vertex.color;\n"
            "MOV result.color.secondary, vertex.color.secondary;\n"
            "MOV result.texcoord[0], vertex.texcoord[0];\n"
            "MOV result.texcoord[1], vertex.texcoord[1];\n"
            "MOV result.texcoord[2], vertex.texcoord[2];\n"
            "MOV result.texcoord[3], vertex.texcoord[3];\n"
            "MOV result.fogcoord.x, vertex.position.z;\n"
            "END\n";
        const char *fb = (target == 0x8620u /* GL_VERTEX_PROGRAM_ARB */)
          ? (g_ul_gsevo ? fb_vp_white : fb_vp_lit)
          : "!!ARBfp1.0\n"
            "MOV result.color, fragment.color;\n"
            "END\n";
        g_real_glProgramStringARB(target, format, (int)strlen(fb), fb);
        if (p_err) p_err();                              /* swallow the error */
    }
}

/* Install the NV->ARB program rewriter for a title that binds glProgramStringARB
 * OUTSIDE the SDL/generic-binder path (e.g. GLUT-based GSEVO, whose GL imports
 * are resolved by libgl_resolve_extensions in libgl_stub.c). `realfn` is the RAW
 * stdcall vendor glProgramStringARB (from GetProcAddress/wglGetProcAddress). We
 * stash it as the rewriter's forward target and ALSO settle g_ul_shader_fallback
 * (via ul_decide_gl_modes) so the rewriter installs a valid fallback program on
 * hardware drivers for any shader it can't fully translate - exactly the ABC
 * path. Returns the cdecl rewriter to register under "glProgramStringARB".
 * ADDITIVE for ABC: ABC never reaches this (its glProgramStringARB is bound by
 * patch_game_gl_got/the generic binder, which set g_real_glProgramStringARB the
 * same way). */
void *ul_install_arb_rewriter(void *realfn)
{
    extern void ul_decide_gl_modes_pub(void);
    if (!realfn) return 0;
    g_real_glProgramStringARB = (pglProgramStringARB)realfn;
    ul_decide_gl_modes_pub();   /* set g_ul_shader_fallback for the active driver */
    return (void*)stub_glProgramStringARB;
}


#define GLX_POOL_SIZE (256 * 1024)  /* 256KB pool for GLX objects */
static unsigned char g_glx_pool[GLX_POOL_SIZE];
static size_t        g_glx_pool_off = 0;

static void *glx_pool_alloc(size_t sz) {
    size_t aligned = (sz + 15) & ~15;
    if (g_glx_pool_off + aligned > GLX_POOL_SIZE) {
        loader_log(LOG_WARN, "[GLX] glx_pool_alloc: pool exhausted! sz=%zu", sz);
        return calloc(1, sz);
    }
    void *p = (void*)&g_glx_pool[g_glx_pool_off];
    memset(p, 0, aligned);
    g_glx_pool_off += aligned;
    return p;
}


glx_state_t g_glx = {0};

/* ── GL extension constants ──────────────────────────────────────────── */
#define GL_FRAMEBUFFER        0x8D40
#define GL_READ_FRAMEBUFFER   0x8CA8
#define GL_DRAW_FRAMEBUFFER   0x8CA9
#define GL_RENDERBUFFER       0x8D41
#define GL_COLOR_ATTACHMENT0  0x8CE0
#define GL_DEPTH_ATTACHMENT   0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_DEPTH24_STENCIL8   0x88F0
#define GL_RGBA8              0x8058
#define GL_COLOR_BUFFER_BIT   0x4000
#define GL_LINEAR             0x2601

typedef void (WINAPI *glGenFramebuffers_t)(int,unsigned int*);
typedef void (WINAPI *glBindFramebuffer_t)(unsigned int,unsigned int);
typedef void (WINAPI *glDeleteFramebuffers_t)(int,const unsigned int*);
typedef void (WINAPI *glGenRenderbuffers_t)(int,unsigned int*);
typedef void (WINAPI *glBindRenderbuffer_t)(unsigned int,unsigned int);
typedef void (WINAPI *glRenderbufferStorage_t)(unsigned int,unsigned int,int,int);
typedef void (WINAPI *glFramebufferRenderbuffer_t)(unsigned int,unsigned int,unsigned int,unsigned int);
typedef void (WINAPI *glBlitFramebuffer_t)(int,int,int,int,int,int,int,int,unsigned int,unsigned int);
typedef void (WINAPI *glFlush_t)(void);
typedef void (WINAPI *glViewport_t)(int,int,int,int);

#define GFB  ((glGenFramebuffers_t)g_glx.glGenFramebuffers)
#define BFB  ((glBindFramebuffer_t)g_glx.glBindFramebuffer)
#define DFB  ((glDeleteFramebuffers_t)g_glx.glDeleteFramebuffers)
#define GRB  ((glGenRenderbuffers_t)g_glx.glGenRenderbuffers)
#define BRB  ((glBindRenderbuffer_t)g_glx.glBindRenderbuffer)
#define RBS  ((glRenderbufferStorage_t)g_glx.glRenderbufferStorage)
#define FBRB ((glFramebufferRenderbuffer_t)g_glx.glFramebufferRenderbuffer)
#define BFBB ((glBlitFramebuffer_t)g_glx.glBlitFramebuffer)

/* ── Object registry ─────────────────────────────────────────────────── */
static void reg_add(void *ptr) {
    if (g_glx.n_objects < GLX_MAX_OBJECTS)
        g_glx.objects[g_glx.n_objects++] = ptr;
}


/* ── Helper window ───────────────────────────────────────────────────── */
static bool create_helper_window(void) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "GLX_EMU_HELPER";
    RegisterClassA(&wc);
    g_glx.helper_hwnd = CreateWindowExA(0,"GLX_EMU_HELPER","",WS_POPUP,0,0,1,1,
                                         NULL,NULL,GetModuleHandleA(NULL),NULL);
    if (!g_glx.helper_hwnd) return false;
    g_glx.helper_hdc = GetDC(g_glx.helper_hwnd);
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd),1,
        PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER,
        PFD_TYPE_RGBA,32,0,0,0,0,0,0,0,0,0,0,0,0,0,24,8,0,PFD_MAIN_PLANE,0,0,0,0};
    int pf = ChoosePixelFormat(g_glx.helper_hdc, &pfd);
    if (!pf) pf = 1;
    SetPixelFormat(g_glx.helper_hdc, pf, &pfd);
    loader_log(LOG_INFO, "[GLX] Helper window: hwnd=0x%p hdc=0x%p pf=%d",
             g_glx.helper_hwnd, g_glx.helper_hdc, pf);
    return true;
}

/* ── Extensions ──────────────────────────────────────────────────────── */
void glx_load_extensions(void) {
    if (g_glx.ext_loaded) return;
    HMODULE gl32 = GetModuleHandleA("opengl32.dll");
    if (!gl32) gl32 = LoadLibraryA("opengl32.dll");
    /* GLX-direct path (Hummer/LGJ/HOTD4) skips shim_glutInit so glx_emu_init()
     * is never called and g_glx.wglGetProcAddress is still NULL.  Bootstrap it
     * here before the EXT() macro tries to call through it. */
    if (!g_glx.wglGetProcAddress && gl32)
        g_glx.wglGetProcAddress = (PROC(WINAPI*)(LPCSTR))GetProcAddress(gl32, "wglGetProcAddress");
#define EXT(n) do { \
    if (g_glx.wglGetProcAddress) g_glx.n=(void*)g_glx.wglGetProcAddress(#n); \
    if(!g_glx.n&&gl32) g_glx.n=(void*)GetProcAddress(gl32,#n); } while(0)
    EXT(glGenFramebuffers); EXT(glBindFramebuffer); EXT(glDeleteFramebuffers);
    EXT(glGenRenderbuffers); EXT(glBindRenderbuffer); EXT(glRenderbufferStorage);
    EXT(glFramebufferRenderbuffer); EXT(glBlitFramebuffer);
#undef EXT
    g_glx.glFlush    = (void*)GetProcAddress(GetModuleHandleA("opengl32.dll"), "glFlush");
    g_glx.glViewport = (void*)GetProcAddress(GetModuleHandleA("opengl32.dll"), "glViewport");
    g_glx.ext_loaded = true;
    loader_log(LOG_INFO, "[GLX] Extensions: FBO=%s Blit=%s",
             g_glx.glGenFramebuffers?"YES":"NO",
             g_glx.glBlitFramebuffer?"YES":"NO");
}

/* ── Init ────────────────────────────────────────────────────────────── */
/* From shim_glut.c - Mesa handle and window handles */
extern HMODULE glut_get_mesa_handle(void);
extern HWND    glut_get_hwnd(void);
extern HDC     glut_get_hdc(void);
extern HGLRC   glut_get_hglrc(void);

/* sdl_window.c - splash dismiss + SDL-path guard.  Read/set from the GLX
 * swap path for non-GLUT games (Hummer) that never call glutMainLoop. */
extern volatile int g_splash_dismiss;
extern int          g_sdl_active;

bool glx_emu_init(HWND hwnd, HDC hdc) {
    if (g_glx.initialized) return true;

    /* With SDL3, the GL context already exists. Use it directly. */
    HMODULE gl32 = glut_get_mesa_handle();
    if (!gl32) gl32 = GetModuleHandleA("opengl32.dll");
    if (!gl32) gl32 = LoadLibraryA("opengl32.dll");
    if (!gl32) { loader_log(LOG_WARN, "[GLX] opengl32.dll not found"); return false; }

    loader_log(LOG_WARN, "[GLX] glx_emu_init: Mesa handle=0x%p", gl32);

#define LOAD(fn) g_glx.fn=(void*)GetProcAddress(gl32,#fn);
    LOAD(wglCreateContext) LOAD(wglDeleteContext)
    LOAD(wglMakeCurrent)   LOAD(wglGetCurrentContext)
    LOAD(wglGetCurrentDC)  LOAD(wglGetProcAddress)
#undef LOAD
    g_glx.wglShareLists = (void*)GetProcAddress(gl32,"wglShareLists");

    /* SDL3 already owns the context - get its HDC/HWND directly */
    if (hdc) {
        g_glx.main_hdc  = hdc;
        g_glx.main_hwnd = hwnd;
        loader_log(LOG_WARN, "[GLX] Using SDL3 HDC=0x%p HWND=0x%p", hdc, hwnd);
    } else if (g_glx.wglGetCurrentDC) {
        g_glx.main_hdc = g_glx.wglGetCurrentDC();
        loader_log(LOG_WARN, "[GLX] Using wglGetCurrentDC -> HDC=0x%p", g_glx.main_hdc);
    }

    /* Skip create_helper_window - SDL3 context already has pixel format set.
     * Use the SDL3 HDC as the helper HDC too. */
    g_glx.helper_hdc  = g_glx.main_hdc;
    g_glx.helper_hwnd = g_glx.main_hwnd;

    /* Load extension function pointers using the active context */
    glx_load_extensions();

    g_glx.initialized = true;
    loader_log(LOG_WARN, "[GLX] GLX emulation ready (SDL3 mode)");
    return true;
}

/* ── Display ─────────────────────────────────────────────────────────── */
glx_display_obj_t *glx_get_display(void) {
    if (g_glx.display) return g_glx.display;
    glx_display_obj_t *d = glx_pool_alloc(sizeof(*d));
    d->mesa.fd             = 3;      /* fake socket fd */
    d->mesa.nscreens       = 1;
    d->mesa.default_screen = 0;
    d->mesa.resource_base  = 0x04000000;
    d->mesa.resource_mask  = 0x001FFFFF;
    d->tag.magic = GLX_EMU_MAGIC;
    d->tag.type  = GLX_OBJ_DISPLAY;
    d->tag.alive = true;
    g_glx.display = d;
    reg_add(d);
    return d;
}

/* ── FBConfig ────────────────────────────────────────────────────────── */
glx_fbconfig_obj_t *glx_choose_fbconfig(const int *attribs, int *n_out) {
    glx_fbconfig_obj_t *obj = glx_pool_alloc(sizeof(*obj));
    GLXFBConfigRec *m = &obj->mesa;

    /* Fill Mesa-compatible fields */
    m->visualType        = 0x8000;  /* TrueColor */
    m->screen            = 0;
    m->drawableType      = 0x7;     /* window | pixmap | pbuffer */
    m->renderType        = 1;       /* RGBA */
    m->rgbMode           = 1;
    m->doubleBufferMode  = 1;
    m->haveDepthBuffer   = 1;
    m->haveStencilBuffer = 1;
    m->depthBits         = 24;
    m->stencilBits       = 8;
    m->redBits   = m->greenBits = m->blueBits = m->alphaBits = 8;
    m->redMask   = 0x00FF0000;
    m->greenMask = 0x0000FF00;
    m->blueMask  = 0x000000FF;
    m->alphaMask = 0xFF000000;
    m->maxPbufferWidth  = 4096;
    m->maxPbufferHeight = 4096;
    m->id        = 1;
    m->pf_index  = GetPixelFormat(g_glx.helper_hdc);
    if (!m->pf_index) m->pf_index = 1;

    obj->tag.magic = GLX_EMU_MAGIC;
    obj->tag.type  = GLX_OBJ_FBCONFIG;
    obj->tag.alive = true;

    /* GLX ABI: glXChooseFBConfig returns pointer to array of GLXFBConfig*
     * The array is embedded in the object. array[0] = pointer to mesa struct. */
    obj->array[0] = (GLXFBConfig)&obj->mesa;
    obj->array[1] = NULL;

    reg_add(obj);
    if (n_out) *n_out = 1;
    loader_log(LOG_INFO, "[GLX] FBConfig created: ptr=0x%p array@0x%p", &obj->mesa, obj->array);
    return obj;
}

/* ── Context ─────────────────────────────────────────────────────────── */
/* Install a real GL dispatch table at the Mesa _glapi_Dispatch address.
 * The game binary has 0x62254e00 and 0x62254d80 hardcoded as the Mesa
 * _glapi_Dispatch pointers (compiled against Lindbergh's Mesa libGL.so).
 * We allocate memory AT those exact addresses and fill with real function ptrs.
 * Then: game reads EAX = fixed_addr, calls [EAX + slot*4] -> real GL function. */
/* ── Viewport up-scaling ──────────────────────────────────────────────
 * The Lindbergh game draws the displayed frame with SEVERAL different native
 * viewport sizes on the window framebuffer (FBO 0) -- e.g. a large 3D pass and
 * a separate 640x480 HUD pass -- each intended to cover the whole screen in its
 * own coordinate space.  Left alone they only cover a corner of our widescreen
 * window.  We install a wrapper that maps every "fullscreen" pass onto the full
 * window, so the 3D scene and the HUD both fill it and line up.
 *
 * A pass counts as fullscreen when it is bound to FBO 0, anchored at the origin,
 * landscape, and at least screen-width.  The game's many square / portrait /
 * tiny passes (mipmap chains, glow/heat copy-to-texture buffers) are left
 * untouched so render-to-texture keeps working.  Set ULTRA_VP_SCALE=0 to off. */
typedef void (WINAPI *glScissor_t)(int,int,int,int);
typedef void (WINAPI *glGetIntegerv_t)(unsigned int, int*);
static glViewport_t    s_real_glViewport    = NULL;
static glScissor_t     s_real_glScissor     = NULL;
static glGetIntegerv_t s_real_glGetIntegerv = NULL;
static glBindFramebuffer_t s_real_glBindFB  = NULL;   /* real glBindFramebufferEXT */
int g_ul_vp_scale = 0;       /* viewport remap: 1=fill window, 0=off (default) */

/* ── Present-time up-scale ────────────────────────────────────────────────
 * The game composites its frame into the bottom-left content_w x content_h
 * corner of the window framebuffer (its UI is authored for a 640x480 screen,
 * the 3D scene arrives as low-res textures), leaving the rest of the widescreen
 * window blank.  At swap we copy that corner into a temp FBO and blit it back,
 * scaled, to fill the whole window.  Set ULTRA_PRESENT_UPSCALE=0 to disable. */
int g_ul_present_upscale = 0;   /* OFF: the game already fills the framebuffer
                                 * natively (its render texture composites to the
                                 * whole window). The old 640x480-corner crop+scale
                                 * was a software-era hack; ULTRA_PRESENT_UPSCALE=1
                                 * re-enables it. */
int g_ul_content_w = 640;
int g_ul_content_h = 480;

/* ── Cutscene / pre-mission briefing fill ──────────────────────────────────
 * The briefing ("PRESS START TO SKIP") composites a real-time deck scene into a
 * fixed lower-left sub-rectangle of the frame, with a large 3D jet model + text
 * overlaid on the rest. The deck scene is heavily desaturated, so a content
 * detector can't tell it apart from the dark overlay - but the briefing has a
 * RELIABLE signature: it (re)uploads a 256x1 RGBA8 color-ramp (the movie LUT)
 * every frame, which gameplay never does. When that signature is live AND the
 * per-title profile enables it, the present overrides the auto-fit box with a
 * fixed, calibrated rect so the deck scene zooms up to fill the screen. This is
 * present-time ONLY (the game's own draw/memory is never touched). */
int   g_ul_cut_fill   = 0;        /* per-title profile: enable briefing zoom (settings.c sets it) */
int   g_ul_cut_ramp_f = -1000;    /* last swap frame the movie LUT was uploaded */
/* Calibrated rect in offscreen (bottom-origin) 0..1 fractions. Defaults zoom the
 * bottom-left deck region; both spans equal so a 16:9 offscreen scales uniformly.
 * Env ULTRA_CUT_XLO/XHI/YLO/YHI override for tuning. */
float g_ul_cut_xlo = 0.00f,   g_ul_cut_xhi = 0.375f;
float g_ul_cut_ylo = 0.020f,  g_ul_cut_yhi = 0.66f;

int ul_cutscene_active(void){                 /* movie LUT seen within the last ~8 frames */
    extern int g_ul_swap_frame;
    return g_ul_cut_fill && (g_ul_swap_frame - g_ul_cut_ramp_f) <= 8;
}

/* ── Offscreen render redirect (hardware-GPU deadlock fix) ─────────────────
 * On a HARDWARE Mesa backend (zink/d3d12) the game's display callback wedges
 * INSIDE Mesa in a kernel wait: it renders to the default framebuffer (the
 * Vulkan/DXGI swapchain) and does mid-frame readback/sync, but the swapchain
 * can't advance until the callback returns -> deadlock (proven via a thread
 * dump: main thread stuck in libgallium_wgl -> KernelBase wait).  Fix: keep the
 * game OFF the swapchain entirely -- redirect every bind of framebuffer 0 to a
 * private offscreen FBO, let the game render/readback there freely, then blit
 * that FBO to the real swapchain ONCE at present.  Auto-enabled for hardware
 * backends; ULTRA_OFFSCREEN=0/1 overrides. */
int g_ul_offscreen = 0;
static unsigned int s_off_fbo = 0, s_off_col = 0, s_off_ds = 0;
static int s_off_w = 0, s_off_h = 0;

/* Display-fit at present time (offscreen -> output window). Always centered.
 * 0=Stretch (fill, default), 1=Pillarbox (fit aspect, bars), 2=Zoom (fill+crop).
 * g_ul_game_aspect = the game's intended display aspect (w/h); ABC = 4:3.
 * Settable from settings.c and ULTRA_DISP_MODE / ULTRA_GAME_ASPECT envs. */
/* The rectangle the game's picture is presented into, in client coords.
 * Published every present; consumed by the lightgun aim mapper in
 * jvs_input.c. Valid only once a frame has actually been presented. */
int g_ul_present_dx0 = 0, g_ul_present_dy0 = 0;
int g_ul_present_dx1 = 0, g_ul_present_dy1 = 0;
int g_ul_present_valid = 0;
int   g_ul_disp_mode   = 3;   /* default = Fit to Screen (full fill + top-overscan reveal) */
float g_ul_game_aspect = 4.0f/3.0f;

#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif

static int ul_off_render_w(void);     /* forward (defined below) */
static int ul_off_render_h(void);

/* A fullscreen-intent pass on "the screen": origin, landscape, screen-ish width,
 * drawn to the buffer the game presents - framebuffer 0 directly, OR (when the
 * offscreen redirect is active) our private offscreen FBO, which IS the screen as
 * far as the game is concerned. The game draws some screens (the attract / INSERT
 * COIN page, and gameplay) at a hardcoded 640x480, parked in the corner; remapping
 * those to fill is what gives us a full-stretch frame. The game's own non-zero
 * FBOs (render-to-texture passes) are left untouched. */
static int ul_vp_is_fullscreen(int x,int y,int w,int h) {
    if (x != 0 || y != 0) return 0;
    if (w <= h) return 0;                 /* square/portrait -> texture pass */
    if (w < 600) return 0;                /* small -> mipmap/copy pass       */
    if (s_real_glGetIntegerv) {
        int fb = 0;
        s_real_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
        if (fb != 0 && (!g_ul_offscreen || (unsigned)fb != s_off_fbo))
            return 0;                     /* a real game FBO -> leave alone   */
    }
    return 1;
}

static void WINAPI ul_glViewport(int x,int y,int w,int h) {
    if (getenv("ULTRA_VP_TRACE")) {   /* TEMP: log EVERY distinct viewport (x,y,w,h) + FBO/scissor */
        static int sx[64],sy[64],sw[64],sh[64],n=0; int found=0;
        for(int i=0;i<n;i++) if(sx[i]==x&&sy[i]==y&&sw[i]==w&&sh[i]==h){found=1;break;}
        if(!found && n<64){ sx[n]=x;sy[n]=y;sw[n]=w;sh[n]=h;n++;
            int fb=-1, sc=-1, sb[4]={0,0,0,0};
            if(s_real_glGetIntegerv){ s_real_glGetIntegerv(0x8CA6/*FB_BINDING*/,&fb);
                s_real_glGetIntegerv(0x0C10/*SCISSOR_BOX*/,sb); }
            int fs = ul_vp_is_fullscreen(x,y,w,h);
            loader_log(LOG_WARN,"[VPTRACE] vp(%d,%d,%d,%d) fbo=%d off=%u fs=%d scissor=[%d,%d,%d,%d] frame=%d",
                       x,y,w,h, fb, s_off_fbo, fs, sb[0],sb[1],sb[2],sb[3], g_glx.frame_count); }
    }
    if (g_ul_vp_scale && ul_vp_is_fullscreen(x,y,w,h)) {
        /* Fill the surface we actually render into: the offscreen FBO (then we
         * scale-blit it to the window at present), or the window directly. */
        int fw = g_ul_offscreen ? ul_off_render_w() : g_width;
        int fh = g_ul_offscreen ? ul_off_render_h() : g_height;
        if (s_real_glViewport) s_real_glViewport(0, 0, fw, fh);
        return;
    }
    if (s_real_glViewport) s_real_glViewport(x,y,w,h);
}

/* Optional ortho trace (ULTRA_PROJ_TRACE) - the game uses a custom UI matrix,
 * not glOrtho, so this is usually silent; kept for other titles. */
typedef void (WINAPI *glOrtho_t)(double,double,double,double,double,double);
static glOrtho_t s_real_glOrtho = NULL;
static void WINAPI ul_glOrtho(double l,double r,double b,double t,double n,double f){
    if (getenv("ULTRA_PROJ_TRACE")) { static int c=0; if(c<48){c++;
        loader_log(LOG_WARN,"[ORTHO] L%.0f R%.0f B%.0f T%.0f frame=%d",l,r,b,t,g_glx.frame_count); } }
    if (s_real_glOrtho) s_real_glOrtho(l,r,b,t,n,f);
}

/* Track the current matrix mode + trace the PROJECTION matrices the game loads,
 * so we can see the 2D-UI ortho range (to fix the top-of-menu overscan clip). */
static unsigned g_cur_mtx_mode = 0;
typedef void (WINAPI *glMatrixMode_t)(unsigned);
static glMatrixMode_t s_real_glMatrixMode = NULL;
static void WINAPI ul_glMatrixMode(unsigned m){ g_cur_mtx_mode = m; if (s_real_glMatrixMode) s_real_glMatrixMode(m); }

typedef void (WINAPI *glLoadMatrixf_t)(const float*);
static glLoadMatrixf_t s_real_glLoadMatrixf = NULL;
/* Tunable vertical fit applied to the game's PROJECTION matrix to reveal the
 * 2D-UI banner the game parks in the top overscan. m[5]=Y-scale, m[13]=Y-offset
 * (clip space). YSCALE<1 widens the vertical view (banner comes in); YOFF shifts
 * content down so the bottom UI bar stays anchored. Empirically tuned. */
/* Default 2D-UI vertical fit for After Burner Climax: the game parks UI in the
 * top AND bottom overscan (menu title banner clips at top, "Licensed by Boeing"
 * clips at bottom), so compress the UI projection SYMMETRICALLY (offset 0) to
 * reveal both edges - the 3D scene behind fills the revealed strips. 0.86 leaves
 * a little margin on each side. ULTRA_NO_UI_FIT disables; ULTRA_PROJ_YSCALE/
 * ULTRA_PROJ_YOFF tune (per-game values can override this later). */
/* No 2D-UI vertical squeeze by default - the game already fills the offscreen via
 * the viewport remap, and present scales the WHOLE offscreen to the window, so the
 * frame fills at any resolution. (The old squeeze revealed the menu's top overscan
 * but compressed gameplay into the lower part of the screen, leaving a white band
 * up top - exactly what we do NOT want.) Re-enable per-title via ULTRA_PROJ_YSCALE
 * / ULTRA_PROJ_YOFF if a specific menu ever needs it. */
static float g_proj_ys = 1.0f, g_proj_yo = 0.0f; static int g_proj_env = 0;
static int   g_proj_user = 0;   /* user explicitly pinned the fit via env -> don't auto-pick */
static void ul_proj_env_init(void){
    if (g_proj_env) return; g_proj_env = 1;
    if (getenv("ULTRA_NO_UI_FIT")) { g_proj_ys = 1.0f; g_proj_yo = 0.0f; g_proj_user = 1; }
    const char *a = getenv("ULTRA_PROJ_YSCALE"); if (a && *a) { g_proj_ys = (float)atof(a); g_proj_user = 1; }
    const char *b = getenv("ULTRA_PROJ_YOFF");   if (b && *b) { g_proj_yo = (float)atof(b); g_proj_user = 1; }
}
/* Frame index of the last PROJECTION matrix loaded via glLoadMatrixf (= the 3D
 * perspective for this game). The 2D-UI fit is applied ONLY when a 3D scene is
 * present behind the UI (so the revealed overscan strip is filled by the scene,
 * not black). Pure-2D screens (INSERT COIN / gray loading) never load a
 * perspective, so they get NO fit and stay edge-to-edge with no black bar. */
static int g_persp_frame = -1000;
static void WINAPI ul_glLoadMatrixf(const float* m){
    ul_proj_env_init();
    /* NOTE: the vertical fit is applied ONLY to the 2D-UI identity projection
     * (ul_glLoadIdentity), NOT to this perspective matrix - the 3D scene/gameplay
     * framing must stay exactly as the game intends. Here we only mark/trace. */
    if (g_cur_mtx_mode==0x1701 /*GL_PROJECTION*/) g_persp_frame = (int)g_glx.frame_count;
    if (getenv("ULTRA_PROJ_TRACE") && g_cur_mtx_mode==0x1701 /*GL_PROJECTION*/ && m){
        /* log each DISTINCT projection matrix (dedup by m0/m5/m11/m13) */
        static float lm0[16],lm5[16],lm11[16],lm13[16]; static int n=0,seen; seen=0;
        for(int i=0;i<n;i++) if(lm0[i]==m[0]&&lm5[i]==m[5]&&lm11[i]==m[11]&&lm13[i]==m[13]){seen=1;break;}
        if(!seen && n<16){ lm0[n]=m[0];lm5[n]=m[5];lm11[n]=m[11];lm13[n]=m[13];n++;
            loader_log(LOG_WARN,"[PROJ] %s m0=%.4f m5=%.4f m10=%.4f m11=%.1f m12=%.3f m13=%.3f m14=%.3f m15=%.1f f=%d",
                       (m[11]==0.0f? "ORTHO":"PERSP"), m[0],m[5],m[10],m[11],m[12],m[13],m[14],m[15],g_glx.frame_count); }
    }
    if (getenv("ULTRA_MV_TRACE") && g_cur_mtx_mode==0x1700 /*GL_MODELVIEW*/ && m){
        /* log each DISTINCT modelview matrix (dedup by m0/m5/m12/m13) - reveals
         * the UI coordinate->NDC scale/translate the 2D pass uses */
        static float lm0[24],lm5[24],lm12[24],lm13[24]; static int n=0,seen; seen=0;
        for(int i=0;i<n;i++) if(lm0[i]==m[0]&&lm5[i]==m[5]&&lm12[i]==m[12]&&lm13[i]==m[13]){seen=1;break;}
        if(!seen && n<24){ lm0[n]=m[0];lm5[n]=m[5];lm12[n]=m[12];lm13[n]=m[13];n++;
            loader_log(LOG_WARN,"[MV] m0=%.5f m5=%.5f m10=%.5f m12=%.4f m13=%.4f m14=%.4f m15=%.3f f=%d",
                       m[0],m[5],m[10],m[12],m[13],m[14],m[15],g_glx.frame_count); }
    }
    if (s_real_glLoadMatrixf) s_real_glLoadMatrixf(m);
}

/* The 2D UI is likely drawn under an IDENTITY projection (raw NDC). Intercept
 * glLoadIdentity in GL_PROJECTION mode: when the vertical fit is active, load a
 * Y-scaled/offset matrix instead of identity so the overscan banner is revealed. */
typedef void (WINAPI *glLoadIdentity_t)(void);
static glLoadIdentity_t s_real_glLoadIdentity = NULL;
static void ul_proj_env_init(void);
static void WINAPI ul_glLoadIdentity(void){
    if (g_cur_mtx_mode==0x1701 /*GL_PROJECTION*/) {
        ul_proj_env_init();
        if (getenv("ULTRA_PROJ_TRACE")) { static int c=0; if(c<4){c++;
            loader_log(LOG_WARN,"[PROJ] LoadIdentity in PROJECTION mode f=%d",g_glx.frame_count); } }
        /* Only fit when a 3D scene is present this/last frame (scene fills the
         * revealed strip); pure-2D screens stay unscaled -> no black bar. */
        int has3d = ((int)g_glx.frame_count - g_persp_frame) <= 1;
        /* Vertical overscan reveal. The game renders its UI into the top AND bottom
         * overscan and CLIPS it at render time (the menu title banner up top, the
         * "Licensed by Boeing" / OFFICIALLY LICENSED notice at the bottom) - only a
         * render-time projection change can bring it back. We apply a SYMMETRIC
         * squeeze (ys<1, yo=0) so both overscan strips come into view. This used to
         * leave a white band / shrink gameplay, but the present stage now detects
         * the real content bounding box and stretches it to fill, which (a) reveals
         * the banner+notice on menus and (b) transparently re-expands gameplay - so
         * the squeeze is safe to apply whenever a 3D scene is present. ULTRA_PROJ_*
         * env overrides still win (g_proj_user). */
        /* No squeeze by default: it left white strips top/bottom (the present can't
         * always crop them) and didn't reliably reveal the clipped Boeing notice.
         * The content bounding-box fill already covers the screen. A per-title
         * overscan reveal can still be forced with ULTRA_PROJ_YSCALE/YOFF. */
        float ys = g_proj_ys, yo = g_proj_yo;   /* default 1.0/0.0 = no squeeze */
        if (has3d && g_proj_env && (ys!=1.0f || yo!=0.0f) && s_real_glLoadMatrixf) {
            float mm[16] = {1,0,0,0, 0,ys,0,0, 0,0,1,0, 0,yo,0,1};
            s_real_glLoadMatrixf(mm);
            return;
        }
    }
    if (s_real_glLoadIdentity) s_real_glLoadIdentity();
}

/* The game derives its on-screen layout from glGetIntegerv(GL_VIEWPORT): at init
 * it reads back the default (full-window) viewport and treats that as the screen
 * size. If the output window is larger than the game's native render res, the
 * game lays its fixed-size UI out in a corner. THE FIX: when the game reads back
 * the full-window viewport, hand it our fixed render res instead. It then lays
 * out for the offscreen FBO we render into (which IS that size), filling it; we
 * scale-blit that offscreen to the real window at present. Sub-viewport readbacks
 * (the 640x480 / 1024x1024 internal passes) never equal the window, so they pass
 * through untouched. */
extern int g_ul_render_width, g_ul_render_height;
static void WINAPI ul_glGetIntegerv_diag(unsigned int pname, int *params){
    if (s_real_glGetIntegerv) s_real_glGetIntegerv(pname, params);
    if (pname==0x0BA2 && params){ /* GL_VIEWPORT */
        if (getenv("ULTRA_PROJ_TRACE")){ static int c=0; if(c<24){c++;
            loader_log(LOG_WARN,"[GIV] GL_VIEWPORT -> %d,%d,%d,%d frame=%d",
                       params[0],params[1],params[2],params[3],g_glx.frame_count); } }
        if (g_ul_offscreen && params[0]==0 && params[1]==0 &&
            params[2]==g_width && params[3]==g_height &&
            g_ul_render_width>=320 && g_ul_render_height>=240) {
            params[2] = g_ul_render_width;
            params[3] = g_ul_render_height;
        }
    }
}

static void WINAPI ul_glScissor(int x,int y,int w,int h) {
    /* Scale a fullscreen scissor box to the window too (the game leaves scissor
     * test off, but stay consistent if a title turns it on). */
    if (g_ul_vp_scale && ul_vp_is_fullscreen(x,y,w,h)) {
        if (s_real_glScissor) s_real_glScissor(0, 0, g_width, g_height);
        return;
    }
    if (s_real_glScissor) s_real_glScissor(x,y,w,h);
}

/* The game's NATIVE render resolution (DIP-encoded; e.g. 1360x768 for ABC). The
 * offscreen FBO is sized to THIS, decoupling what the game renders from the
 * output window size, so we can present at any resolution (480p..4K) by scaling. */
extern int g_ul_render_width, g_ul_render_height;
static int ul_off_render_w(void){ int w=g_ul_render_width; return (w>=320&&w<=8192)?w:g_width; }
static int ul_off_render_h(void){ int h=g_ul_render_height;return (h>=240&&h<=8192)?h:g_height; }

/* Lazily create the private offscreen FBO (color + packed depth/stencil) at the
 * game's native render res, and leave it bound.  Uses the REAL bind (BFB macro)
 * to avoid recursing through the bind wrapper. Returns 0 if FBO support is missing.
 *
 * SIZE-LOCK: once the FBO exists, we DO NOT tear it down for a size change.
 * ABC's per-hit damage-flash + screen-shake post-process briefly alters the
 * render target size mid-frame; the old recreate-on-mismatch path destroyed the
 * FBO and ALL its attachments, which surfaced to SDL/the native AMD GL driver
 * as a swapchain rebuild - visible as a window "respawn" / flash every time the
 * player ship took a hit. Fix: size the FBO once at first call (the native
 * render res is established by then) and KEEP it. A genuine permanent res
 * change (almost never happens in Lindbergh) is unsupported here by design;
 * if needed, expose an explicit reset path that runs between frames. */
static unsigned int ul_offscreen_fbo(void) {
    if (!g_glx.glGenFramebuffers || !g_glx.glGenRenderbuffers) return 0;
    if (s_off_fbo) return s_off_fbo;                 /* size-locked: never recreate */
    int rw = ul_off_render_w(), rh = ul_off_render_h();
    GFB(1,&s_off_fbo);
    BFB(GL_FRAMEBUFFER, s_off_fbo);
    GRB(1,&s_off_col); BRB(GL_RENDERBUFFER,s_off_col);
    RBS(GL_RENDERBUFFER, GL_RGBA8, rw, rh);
    FBRB(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_off_col);
    GRB(1,&s_off_ds); BRB(GL_RENDERBUFFER,s_off_ds);
    RBS(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, rw, rh);
    FBRB(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,   GL_RENDERBUFFER, s_off_ds);
    FBRB(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_off_ds);
    s_off_w = rw; s_off_h = rh;
    loader_log(LOG_WARN, "[GLX] offscreen render FBO=%u native %dx%d -> output %dx%d (size-locked)",
               s_off_fbo, rw, rh, g_width, g_height);
    return s_off_fbo;
}

/* Redirect any bind of framebuffer 0 (the swapchain) to our offscreen FBO. The
 * game's OWN framebuffers (non-zero) pass through untouched. */
static void WINAPI ul_glBindFramebufferEXT(unsigned target, unsigned fb) {
    if (g_ul_offscreen && fb == 0) { unsigned o = ul_offscreen_fbo(); if (o) fb = o; }
    if (s_real_glBindFB) s_real_glBindFB(target, fb);
}

static void glx_patch_dispatch_table(void) {
    HMODULE gl32 = GetModuleHandleA("opengl32.dll");
    if (!gl32) { loader_log(LOG_WARN, "[GLX] dispatch: opengl32 not found"); return; }

    /* One-time: resolve the real GL entry points our wrappers forward to,
     * and honor the ULTRA_VP_SCALE override. */
    if (!s_real_glGetIntegerv) {
        s_real_glGetIntegerv = (glGetIntegerv_t)GetProcAddress(gl32, "glGetIntegerv");
        s_real_glViewport    = (glViewport_t)   GetProcAddress(gl32, "glViewport");
        s_real_glScissor     = (glScissor_t)    GetProcAddress(gl32, "glScissor");
        /* real glBindFramebufferEXT for the offscreen-redirect wrapper (an
         * extension entry point - resolve via wglGetProcAddress first) */
        { typedef void* (WINAPI *wglGPA_t)(const char*);
          wglGPA_t wgpa = (wglGPA_t)GetProcAddress(gl32, "wglGetProcAddress");
          void *p = wgpa ? wgpa("glBindFramebufferEXT") : NULL;
          if (!p) p = (void*)GetProcAddress(gl32, "glBindFramebufferEXT");
          if (!p && wgpa) p = wgpa("glBindFramebuffer");
          s_real_glBindFB = (glBindFramebuffer_t)p; }
        const char *e = getenv("ULTRA_VP_SCALE");
        if (e && *e) g_ul_vp_scale = atoi(e);
        /* Offscreen redirect: explicit ULTRA_OFFSCREEN wins; otherwise auto-on
         * for hardware backends (zink/d3d12) where the swapchain deadlock lives,
         * off for llvmpipe (software is synchronous, no deadlock, no need). */
        const char *off = getenv("ULTRA_OFFSCREEN");
        if (off && *off) g_ul_offscreen = atoi(off);
        else { const char *drv = getenv("ULTRA_GL_DRIVER");
               /* No driver env at all = a plain double-click, where the loader
                * defaults to native vendor GL -> default offscreen ON so the fill
                * pipeline is live. Only software (llvmpipe/softpipe/swrast), which
                * is synchronous and renders directly, opts OUT. Everything else
                * (native/gpu/amd/nvidia/intel/zink/d3d12) gets offscreen+fill. */
               g_ul_offscreen = (!drv || !*drv) ? 1 :
                                !(!_stricmp(drv,"llvmpipe") || !_stricmp(drv,"softpipe") ||
                                  !_stricmp(drv,"swrast")   || !_stricmp(drv,"soft")); }
        loader_log(LOG_INFO, "[GLX] viewport scaling=%d offscreen=%d (window %dx%d)",
                   g_ul_vp_scale, g_ul_offscreen, g_width, g_height);
    }

    typedef void* (WINAPI *wglGPA_t)(const char*);
    wglGPA_t p_wglGPA = (wglGPA_t)GetProcAddress(gl32, "wglGetProcAddress");

    /* Mesa __glapi_table dispatch order (slots 0-249+) */
    static const char *gl_names[] = {
        "glNewList","glEndList","glCallList","glCallLists","glDeleteLists",
        "glGenLists","glListBase","glBegin","glBitmap","glColor3b",
        "glColor3bv","glColor3d","glColor3dv","glColor3f","glColor3fv",
        "glColor3i","glColor3iv","glColor3s","glColor3sv","glColor3ub",
        "glColor3ubv","glColor3ui","glColor3uiv","glColor3us","glColor3usv",
        "glColor4b","glColor4bv","glColor4d","glColor4dv","glColor4f",
        "glColor4fv","glColor4i","glColor4iv","glColor4s","glColor4sv",
        "glColor4ub","glColor4ubv","glColor4ui","glColor4uiv","glColor4us",
        "glColor4usv","glEdgeFlag","glEdgeFlagv","glEnd","glIndexd",
        "glIndexdv","glIndexf","glIndexfv","glIndexi","glIndexiv",
        "glIndexs","glIndexsv","glNormal3b","glNormal3bv","glNormal3d",
        "glNormal3dv","glNormal3f","glNormal3fv","glNormal3i","glNormal3iv",
        "glNormal3s","glNormal3sv","glRasterPos2d","glRasterPos2dv",
        "glRasterPos2f","glRasterPos2fv","glRasterPos2i","glRasterPos2iv",
        "glRasterPos2s","glRasterPos2sv","glRasterPos3d","glRasterPos3dv",
        "glRasterPos3f","glRasterPos3fv","glRasterPos3i","glRasterPos3iv",
        "glRasterPos3s","glRasterPos3sv","glRasterPos4d","glRasterPos4dv",
        "glRasterPos4f","glRasterPos4fv","glRasterPos4i","glRasterPos4iv",
        "glRasterPos4s","glRasterPos4sv","glRectd","glRectdv","glRectf",
        "glRectfv","glRecti","glRectiv","glRects","glRectsv",
        "glTexCoord1d","glTexCoord1dv","glTexCoord1f","glTexCoord1fv",
        "glTexCoord1i","glTexCoord1iv","glTexCoord1s","glTexCoord1sv",
        "glTexCoord2d","glTexCoord2dv","glTexCoord2f","glTexCoord2fv",
        "glTexCoord2i","glTexCoord2iv","glTexCoord2s","glTexCoord2sv",
        "glTexCoord3d","glTexCoord3dv","glTexCoord3f","glTexCoord3fv",
        "glTexCoord3i","glTexCoord3iv","glTexCoord3s","glTexCoord3sv",
        "glTexCoord4d","glTexCoord4dv","glTexCoord4f","glTexCoord4fv",
        "glTexCoord4i","glTexCoord4iv","glTexCoord4s","glTexCoord4sv",
        "glVertex2d","glVertex2dv","glVertex2f","glVertex2fv",
        "glVertex2i","glVertex2iv","glVertex2s","glVertex2sv",
        "glVertex3d","glVertex3dv","glVertex3f","glVertex3fv",
        "glVertex3i","glVertex3iv","glVertex3s","glVertex3sv",
        "glVertex4d","glVertex4dv","glVertex4f","glVertex4fv",
        "glVertex4i","glVertex4iv","glVertex4s","glVertex4sv",
        "glClipPlane","glColorMaterial","glCullFace","glFogf","glFogfv",
        "glFogi","glFogiv","glFrontFace","glHint","glLightf",
        "glLightfv","glLighti","glLightiv","glLightModelf","glLightModelfv",
        "glLightModeli","glLightModeliv","glLineStipple","glLineWidth",
        "glMaterialf","glMaterialfv","glMateriali","glMaterialiv",
        "glPointSize","glPolygonMode","glPolygonStipple","glScissor",
        "glShadeModel","glTexParameterf","glTexParameterfv","glTexParameteri",
        "glTexParameteriv","glTexImage1D","glTexImage2D","glTexEnvf",
        "glTexEnvfv","glTexEnvi","glTexEnviv","glTexGend","glTexGendv",
        "glTexGenf","glTexGenfv","glTexGeni","glTexGeniv",
        "glFeedbackBuffer","glSelectBuffer","glRenderMode",
        "glInitNames","glLoadName","glPassThrough","glPopName","glPushName",
        "glDrawBuffer","glClear","glClearAccum","glClearIndex",
        "glClearColor","glClearStencil","glClearDepth","glStencilMask",
        "glColorMask","glDepthMask","glIndexMask","glAccum",
        "glDisable","glEnable","glFinish","glFlush","glPopAttrib",
        "glPushAttrib","glMap1d","glMap1f","glMap2d","glMap2f",
        "glMapGrid1d","glMapGrid1f","glMapGrid2d","glMapGrid2f",
        "glEvalCoord1d","glEvalCoord1dv","glEvalCoord1f","glEvalCoord1fv",
        "glEvalCoord2d","glEvalCoord2dv","glEvalCoord2f","glEvalCoord2fv",
        "glEvalMesh1","glEvalPoint1","glEvalMesh2","glEvalPoint2",
        "glAlphaFunc","glBlendFunc","glLogicOp","glStencilFunc",
        "glStencilOp","glDepthFunc","glPixelZoom","glPixelTransferf",
        "glPixelTransferi","glPixelStoref","glPixelStorei","glPixelMapfv",
        "glPixelMapuiv","glPixelMapusv","glReadBuffer","glCopyPixels",
        "glReadPixels","glDrawPixels","glGetBooleanv","glGetClipPlane",
        "glGetDoublev","glGetError","glGetFloatv","glGetIntegerv",
        "glGetLightfv","glGetLightiv","glGetMapdv","glGetMapfv",
        "glGetMapiv","glGetMaterialfv","glGetMaterialiv","glGetPixelMapfv",
        "glGetPixelMapuiv","glGetPixelMapusv","glGetPolygonStipple",
        "glGetString","glGetTexEnvfv","glGetTexEnviv","glGetTexGendv",
        "glGetTexGenfv","glGetTexGeniv","glGetTexImage","glGetTexParameterfv",
        "glGetTexParameteriv","glGetTexLevelParameterfv","glGetTexLevelParameteriv",
        "glIsEnabled","glIsList","glDepthRange","glFrustum",
        "glLoadIdentity","glLoadMatrixf","glLoadMatrixd","glMatrixMode",
        "glMultMatrixf","glMultMatrixd","glOrtho","glPopMatrix",
        "glPushMatrix","glRotated","glRotatef","glScaled",
        "glScalef","glTranslated","glTranslatef","glViewport",
        "glArrayElement","glBindTexture","glColorPointer","glDisableClientState",
        "glDrawArrays","glDrawElements","glEdgeFlagPointer","glEnableClientState",
        "glGenTextures","glDeleteTextures","glGetPointerv","glIndexPointer",
        "glIndexub","glIndexubv","glInterleavedArrays","glIsTexture",
        "glNormalPointer","glPolygonOffset","glTexCoordPointer","glVertexPointer",
        "glAreTexturesResident","glCopyTexImage1D","glCopyTexImage2D",
        "glCopyTexSubImage1D","glCopyTexSubImage2D","glPrioritizeTextures",
        "glTexSubImage1D","glTexSubImage2D","glPopClientAttrib","glPushClientAttrib",
        /* OpenGL 1.2+ */
        "glBlendColor","glBlendEquation","glDrawRangeElements",
        "glColorTable","glColorTableParameterfv","glColorTableParameteriv",
        "glCopyColorTable","glGetColorTable","glGetColorTableParameterfv",
        "glGetColorTableParameteriv","glColorSubTable","glCopyColorSubTable",
        "glConvolutionFilter1D","glConvolutionFilter2D","glConvolutionParameterf",
        "glConvolutionParameterfv","glConvolutionParameteri","glConvolutionParameteriv",
        "glCopyConvolutionFilter1D","glCopyConvolutionFilter2D",
        "glGetConvolutionFilter","glGetConvolutionParameterfv",
        "glGetConvolutionParameteriv","glGetSeparableFilter","glSeparableFilter2D",
        "glGetHistogram","glGetHistogramParameterfv","glGetHistogramParameteriv",
        "glGetMinmax","glGetMinmaxParameterfv","glGetMinmaxParameteriv",
        "glHistogram","glMinmax","glResetHistogram","glResetMinmax",
        "glTexImage3D","glTexSubImage3D","glCopyTexSubImage3D",
        NULL
    };

    /* Count entries */
    int n_entries = 0;
    while (gl_names[n_entries]) n_entries++;
    uint32_t tbl_size = (n_entries + 64) * 4;  /* extra slack */

    /* Addresses the game expects Mesa dispatch tables at */
    uint32_t tbl_addrs[] = {0x62254e00, 0x62254d80, 0};  /* both game dispatch tables */

    for (int t = 0; tbl_addrs[t]; t++) {
        uint32_t addr = tbl_addrs[t];
        /* Check if address is already mapped */
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery((void*)(uintptr_t)addr, &mbi, sizeof(mbi));

        uint32_t *tbl;
        if (mbi.State == MEM_FREE) {
            /* Allocate OUR dispatch table at exactly this address */
            tbl = (uint32_t*)VirtualAlloc((LPVOID)(uintptr_t)addr, tbl_size,
                MEM_RESERVE|MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (!tbl) {
                /* Try nearest available address */
                tbl = (uint32_t*)VirtualAlloc(NULL, tbl_size,
                    MEM_RESERVE|MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                loader_log(LOG_WARN, "[GLX] Could not alloc at 0x%08x, got 0x%p", addr, tbl);
            } else {
                loader_log(LOG_INFO, "[GLX] Allocated dispatch table at 0x%08x (our own)", addr);
            }
        } else {
            /* Already mapped - make writable and patch in place */
            tbl = (uint32_t*)(uintptr_t)addr;
            DWORD old_prot;
            VirtualProtect(tbl, tbl_size, PAGE_EXECUTE_READWRITE, &old_prot);
            loader_log(LOG_INFO, "[GLX] Patching existing mapping at 0x%08x (state=%lu)", addr, mbi.State);
        }

        if (!tbl) continue;

        /* Fill entries that are uninitialized (AMD string data) or zero.
         * Leave already-initialized entries (valid code pointers) untouched.
         * Uninitialized AMD entries are in 0x62000000-0x64000000 string range. */
        int filled = 0, skipped = 0;
        for (int i = 0; gl_names[i] != NULL; i++) {
            uint32_t cur = tbl[i];
            /* Force-install our up-scaling wrappers for glViewport/glScissor,
             * overriding whatever pointer is there (init or not) so the game's
             * frame fills the whole window instead of a 640x480 corner. */
            if (g_ul_vp_scale) {
                if (s_real_glViewport && _stricmp(gl_names[i], "glViewport") == 0) {
                    tbl[i] = (uint32_t)(uintptr_t)&ul_glViewport; filled++; continue;
                }
                if (s_real_glScissor && _stricmp(gl_names[i], "glScissor") == 0) {
                    tbl[i] = (uint32_t)(uintptr_t)&ul_glScissor; filled++; continue;
                }
            }
            /* Check if entry is uninitialized / pointing to string data.
             * Uninitialized entries: zero, AMD string range, OR any address where
             * the first 2 bytes are 0x67 0x6c ("gl" string prefix = privileged instr). */
            int is_uninit = (cur == 0) ||
                            (cur >= 0x62000000 && cur < 0x64000000);
            if (!is_uninit && cur > 0x1000) {
                /* Check if target looks like string data (starts with "gl") */
                MEMORY_BASIC_INFORMATION _chk;
                if (VirtualQuery((void*)(uintptr_t)cur, &_chk, sizeof(_chk)) &&
                    _chk.State == MEM_COMMIT) {
                    uint8_t *p = (uint8_t*)(uintptr_t)cur;
                    if (p[0] == 0x67 && p[1] == 0x6c) is_uninit = 1; /* "gl" prefix */
                    if (p[0] == 0x6c && p[1] == 0x67) is_uninit = 1; /* "lg" */
                }
            }
            if (!is_uninit) { skipped++; continue; }
            void *real = p_wglGPA ? p_wglGPA(gl_names[i]) : NULL;
            if (!real) real = (void*)GetProcAddress(gl32, gl_names[i]);
            if (real) { tbl[i] = (uint32_t)(uintptr_t)real; filled++; }
        }
        loader_log(LOG_INFO, "[GLX] Mesa dispatch @ 0x%08x: filled %d, skipped %d (already init)", addr, filled, skipped);
    }
}


static uint32_t glx_ctx_vtable_noop(void) { return 1; }
static void *s_glx_ctx_vtable[16];
static bool  s_glx_ctx_vtable_init = false;
static void *glx_get_ctx_vtable(void) {
    if (!s_glx_ctx_vtable_init) {
        for (int i = 0; i < 16; i++)
            s_glx_ctx_vtable[i] = (void*)glx_ctx_vtable_noop;
        s_glx_ctx_vtable_init = true;
    }
    return s_glx_ctx_vtable;
}


glx_context_obj_t *glx_create_context(glx_fbconfig_obj_t *cfg,
                                        glx_context_obj_t *share) {
    /* Use SDL's context on the SDL window directly.
     * The game's GOT is patched to call real GL functions, bypassing
     * the opengl32 dispatch chain that was causing crashes.
     * Game renders into the SDL context -> SDL_GL_SwapWindow shows it. */
    /* Use Mesa context created by shim_glut.c */
    HGLRC hglrc  = glut_get_hglrc();
    HDC   sdl_hdc = glut_get_hdc();
    if (!hglrc) hglrc = g_glx.wglGetCurrentContext();
    if (!sdl_hdc) sdl_hdc = g_glx.main_hdc ? g_glx.main_hdc : g_glx.helper_hdc;
    loader_log(LOG_INFO, "[GLX] Using Mesa context 0x%p on Win32 HDC 0x%p", hglrc, sdl_hdc);
    if (!hglrc) { loader_log(LOG_ERROR, "[GLX] No GL context available"); return NULL; }

    if (share && share->mesa.hglrc && g_glx.wglShareLists)
        g_glx.wglShareLists(share->mesa.hglrc, hglrc);

    /* Context is already current via SDL3 - no need to re-bind */
    loader_log(LOG_WARN, "[GLX] glx_create_context: reusing SDL3 GL context hglrc=%p hdc=%p", hglrc, sdl_hdc);
    if (!hglrc) {
        loader_log(LOG_WARN, "[GLX] WARNING: No GL context yet, SDL3 window may not be created");
        /* Try to get it now */
        typedef HGLRC (WINAPI *wglGetCC_t)(void);
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (gl) {
            wglGetCC_t fn = (wglGetCC_t)GetProcAddress(gl, "wglGetCurrentContext");
            if (fn) hglrc = fn();
            loader_log(LOG_WARN, "[GLX] wglGetCurrentContext -> %p", hglrc);
        }
    }

    /* Get GL version */
    typedef const unsigned char* (WINAPI *glGetString_t)(unsigned int);
    HMODULE gl32 = GetModuleHandleA("opengl32.dll");
    glGetString_t _glGetString = gl32 ?
        (glGetString_t)GetProcAddress(gl32, "glGetString") : NULL;
    const unsigned char *ver = _glGetString ? _glGetString(0x1F02) : NULL;
    loader_log(LOG_INFO, "[GLX] GL version: %s", ver ? (const char*)ver : "NULL");

    if (ver && !g_glx.ext_loaded) glx_load_extensions();

    glx_context_obj_t *obj = glx_pool_alloc(sizeof(*obj));
    GLXContextRec *m = &obj->mesa;

    /* Fill Mesa-compatible fields */
    m->vtable = glx_get_ctx_vtable();
    m->currentDpy      = (Display*)glx_get_display();
    m->currentDrawable = 0;
    m->currentReadable = 0;
    m->xid             = 1;
    m->renderType      = 1;     /* RGBA */
    m->screen          = 0;
    m->isDirect        = 1;     /* direct rendering */
    m->thread_id       = GetCurrentThreadId();
    m->hglrc           = hglrc;
    m->hdc             = sdl_hdc;
    m->is_current      = true;
    m->valid           = true;

    obj->tag.magic = GLX_EMU_MAGIC;
    obj->tag.type  = GLX_OBJ_CONTEXT;
    obj->tag.alive = true;

    g_glx.current_ctx = obj;
    loader_log(LOG_WARN, "[GLX] Context created: ptr=0x%p hglrc=0x%p", obj, obj->mesa.hglrc);
    reg_add(obj);

    loader_log(LOG_INFO, "[GLX] Context READY: ptr=0x%p hglrc=0x%p", &obj->mesa, hglrc);
    return obj;
}

/* ── Pbuffer ─────────────────────────────────────────────────────────── */
glx_pbuffer_obj_t *glx_create_pbuffer(glx_fbconfig_obj_t *cfg, int w, int h) {
    if (w <= 0) w = 1024; if (h <= 0) h = 1024;
    glx_pbuffer_obj_t *obj = glx_pool_alloc(sizeof(*obj));
    GLXPbufferRec *m = &obj->mesa;

    m->xid    = 2;
    m->type   = 4;  /* GLX_PBUFFER_BIT */
    m->width  = w;
    m->height = h;
    m->valid  = true;

    obj->tag.magic = GLX_EMU_MAGIC;
    obj->tag.type  = GLX_OBJ_PBUFFER;
    obj->tag.alive = true;

    reg_add(obj);
    loader_log(LOG_INFO, "[GLX] Pbuffer READY: ptr=0x%p %dx%d", &obj->mesa, w, h);
    return obj;
}

static void pbuffer_ensure_fbo(glx_pbuffer_obj_t *obj) {
    GLXPbufferRec *d = &obj->mesa;
    if (d->fbo || !g_glx.glGenFramebuffers) return;
    unsigned int fbo=0, rbc=0, rbds=0;
    GFB(1,&fbo); BFB(GL_FRAMEBUFFER,fbo);
    GRB(1,&rbc); BRB(GL_RENDERBUFFER,rbc);
    RBS(GL_RENDERBUFFER,GL_RGBA8,d->width,d->height);
    FBRB(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,rbc);
    GRB(1,&rbds); BRB(GL_RENDERBUFFER,rbds);
    RBS(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,d->width,d->height);
    FBRB(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,rbds);
    FBRB(GL_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_RENDERBUFFER,rbds);
    BFB(GL_FRAMEBUFFER,0);
    d->fbo=fbo; d->rb_color=rbc; d->rb_depth=rbds;
    loader_log(LOG_INFO, "[GLX] Pbuffer FBO=%u (%dx%d)", fbo, d->width, d->height);
}

/* ── MakeCurrent ─────────────────────────────────────────────────────── */
bool glx_make_current(glx_pbuffer_obj_t *draw, glx_context_obj_t *ctx) {
    if (g_glx.current_ctx) g_glx.current_ctx->mesa.is_current = false;
    if (!ctx) {
        g_glx.wglMakeCurrent(NULL,NULL);
        g_glx.current_ctx = NULL;
        return true;
    }
    HDC hdc = ctx->mesa.hdc ? ctx->mesa.hdc : g_glx.main_hdc;
    BOOL ok = g_glx.wglMakeCurrent(hdc, ctx->mesa.hglrc);
    if (!ok) ok = g_glx.wglMakeCurrent(g_glx.helper_hdc, ctx->mesa.hglrc);
    if (!ok) { loader_log(LOG_WARN, "[GLX] wglMakeCurrent failed: %lu", GetLastError()); return false; }
    if (!g_glx.ext_loaded) glx_load_extensions();
    if (draw && draw->mesa.width > 0) {
        pbuffer_ensure_fbo(draw);
        if (g_glx.glBindFramebuffer && draw->mesa.fbo) {
            BFB(GL_FRAMEBUFFER, draw->mesa.fbo);
            if (g_glx.glViewport)
                ((glViewport_t)g_glx.glViewport)(0,0,draw->mesa.width,draw->mesa.height);
        }
    } else {
        /* In offscreen mode the "default" target is our private FBO, so the game
         * never renders to the swapchain (avoids the hardware-backend deadlock). */
        if (g_glx.glBindFramebuffer) {
            unsigned o = g_ul_offscreen ? ul_offscreen_fbo() : 0;
            BFB(GL_FRAMEBUFFER, o);
            /* Seed the GL viewport to the render res so the game caches THAT as
             * its screen size (it reads the default viewport at init), not the
             * larger output window - otherwise it lays its frame out for the
             * output size and overflows our render-res FBO. */
            if (o) {
                void *vp = g_glx.glViewport ? g_glx.glViewport
                         : (void*)GetProcAddress(GetModuleHandleA("opengl32.dll"), "glViewport");
                if (vp) ((glViewport_t)vp)(0, 0, ul_off_render_w(), ul_off_render_h());
            }
        }
    }
    ctx->mesa.is_current = true;
    g_glx.current_ctx = ctx;
    g_glx.current_draw = draw;
    return true;
}

/* ── SwapBuffers ─────────────────────────────────────────────────────── */
/* Called by kswap_collect -- swap the current drawable or main window */
void glx_swap_buffers_default(void) {
    /* If no context is tracked, ensure the main context is current */
    if (!g_glx.current_ctx && g_glx.main_hdc) {
        HGLRC cur = g_glx.wglGetCurrentContext ? g_glx.wglGetCurrentContext() : NULL;
        if (cur) {
            /* wglGetCurrentContext returned something -- find our context obj */
            for (int i = 0; i < g_glx.n_objects; i++) {
                glx_context_obj_t *c = (glx_context_obj_t*)g_glx.objects[i];
                if (c && c->tag.magic == GLX_EMU_MAGIC && c->tag.type == GLX_OBJ_CONTEXT
                    && c->mesa.hglrc == cur) {
                    g_glx.current_ctx = c;
                    break;
                }
            }
        }
    }
    glx_swap_buffers(g_glx.current_draw);
}

extern int g_width, g_height;
/* Copy the game's native content corner out of the window framebuffer and blit
 * it back, scaled, to fill the whole window. */
/* ── CAS contrast-adaptive sharpening present pass (ULTRA_PRESENT_FILTER) ──
 * Replaces the bilinear present blit: resolve the offscreen into a sampleable
 * texture, then draw a fullscreen triangle running an AMD-CAS-style sharpen +
 * edge-aware upscale to the output. Falls back to the linear blit if the GLSL
 * program can't be built (s_cas_state stays 0). Gate: ULTRA_PRESENT_FILTER
 * (1=on, default; 0=legacy linear), strength ULTRA_CAS_SHARP (0..1). */
typedef unsigned GLu_; typedef int GLi_;
static int   s_cas_state = -1, s_cas_enabled = -1;
static float s_cas_sharp = 0.4f;
static unsigned s_cas_prog=0, s_cas_vao=0, s_cas_fbo=0, s_cas_tex=0;
static int   s_cas_uInv=-1, s_cas_uSharp=-1, s_cas_uTex=-1, s_cas_tw=0, s_cas_th=0;
static int   s_cas_uXLo=-1, s_cas_uXHi=-1;
static GLu_(WINAPI *cCreateShader)(GLu_);
static void(WINAPI *cShaderSource)(GLu_,GLi_,const char*const*,const GLi_*);
static void(WINAPI *cCompileShader)(GLu_);
static void(WINAPI *cGetShaderiv)(GLu_,GLu_,GLi_*);
static GLu_(WINAPI *cCreateProgram)(void);
static void(WINAPI *cAttachShader)(GLu_,GLu_);
static void(WINAPI *cLinkProgram)(GLu_);
static void(WINAPI *cGetProgramiv)(GLu_,GLu_,GLi_*);
static void(WINAPI *cUseProgram)(GLu_);
static GLi_(WINAPI *cGetUniformLocation)(GLu_,const char*);
static void(WINAPI *cUniform1i)(GLi_,GLi_);
static void(WINAPI *cUniform1f)(GLi_,float);
static void(WINAPI *cUniform2f)(GLi_,float,float);
static void(WINAPI *cGenVertexArrays)(GLi_,GLu_*);
static void(WINAPI *cBindVertexArray)(GLu_);
static void(WINAPI *cActiveTexture)(GLu_);
static void(WINAPI *cGenTextures)(GLi_,GLu_*);
static void(WINAPI *cBindTexture)(GLu_,GLu_);
static void(WINAPI *cTexParameteri)(GLu_,GLu_,GLi_);
static void(WINAPI *cTexImage2D)(GLu_,GLi_,GLi_,GLi_,GLi_,GLi_,GLu_,GLu_,const void*);
static void(WINAPI *cFramebufferTexture2D)(GLu_,GLu_,GLu_,GLu_,GLi_);
static void(WINAPI *cDrawArrays)(GLu_,GLi_,GLi_);
static void(WINAPI *cViewport)(GLi_,GLi_,GLi_,GLi_);
static void(WINAPI *cClear)(GLu_);
static void(WINAPI *cClearColor)(float,float,float,float);
static void(WINAPI *cDisable)(GLu_);
static void(WINAPI *cColorMask)(unsigned char,unsigned char,unsigned char,unsigned char);
static void(WINAPI *cUniform4fv)(GLi_,GLi_,const float*);   /* FSR constants (con0..con3) */
static void(WINAPI *cGetShaderInfoLog)(GLu_,GLi_,GLi_*,char*);

static void* ul_glr(HMODULE gl, void* wgpa, const char* n){
    void* p=(void*)GetProcAddress(gl,n);
    if(!p && wgpa){ typedef void*(WINAPI*G)(const char*); p=((G)wgpa)(n); }
    return p;
}
static GLu_ ul_cas_compile(GLu_ type, const char* src){
    GLu_ sh=cCreateShader(type); cShaderSource(sh,1,&src,0); cCompileShader(sh);
    GLi_ ok=0; cGetShaderiv(sh,0x8B81/*COMPILE_STATUS*/,&ok);
    if(!ok){ char log[2048]={0}; if(cGetShaderInfoLog) cGetShaderInfoLog(sh,(GLi_)sizeof(log)-1,0,log);
        loader_log(LOG_WARN,"[GLSL] compile failed (type=0x%x): %s",type,log); return 0; }
    return sh;
}
/* Present filter selector: 0=Bilinear, 1=CAS, 2=FSR (default). */
static int s_filter = -1;
static int ul_filter(void){
    if(s_filter<0){ const char*e=getenv("ULTRA_PRESENT_FILTER"); s_filter=(e&&*e)?atoi(e):2;
        if(s_filter<0)s_filter=0; if(s_filter>2)s_filter=2; }
    return s_filter;
}
/* Resolve the shared GL3 shader/FBO entrypoints + create the fullscreen-triangle
 * VAO. Used by both the CAS and FSR present passes. Returns 1 once ready. */
static int s_gl3_state = -1;
static int ul_gl3_ready(void){
    if(s_gl3_state>=0) return s_gl3_state;
    s_gl3_state=0;
    if(!g_glx.glBlitFramebuffer || !g_glx.glBindFramebuffer || !g_glx.glGenFramebuffers) return 0;
    HMODULE gl=GetModuleHandleA("opengl32.dll"); if(!gl) gl=LoadLibraryA("opengl32.dll"); if(!gl) return 0;
    void* wgpa=(void*)GetProcAddress(gl,"wglGetProcAddress");
    #define R(v,n) do{ v=(void*)ul_glr(gl,wgpa,n); if(!v){ loader_log(LOG_WARN,"[GL3] missing %s",n); return 0; } }while(0)
    R(cCreateShader,"glCreateShader"); R(cShaderSource,"glShaderSource"); R(cCompileShader,"glCompileShader");
    R(cGetShaderiv,"glGetShaderiv"); R(cCreateProgram,"glCreateProgram"); R(cAttachShader,"glAttachShader");
    R(cLinkProgram,"glLinkProgram"); R(cGetProgramiv,"glGetProgramiv"); R(cUseProgram,"glUseProgram");
    R(cGetUniformLocation,"glGetUniformLocation"); R(cUniform1i,"glUniform1i"); R(cUniform1f,"glUniform1f");
    R(cUniform2f,"glUniform2f"); R(cGenVertexArrays,"glGenVertexArrays"); R(cBindVertexArray,"glBindVertexArray");
    R(cActiveTexture,"glActiveTexture"); R(cGenTextures,"glGenTextures"); R(cBindTexture,"glBindTexture");
    R(cTexParameteri,"glTexParameteri"); R(cTexImage2D,"glTexImage2D"); R(cFramebufferTexture2D,"glFramebufferTexture2D");
    R(cDrawArrays,"glDrawArrays"); R(cViewport,"glViewport"); R(cClear,"glClear"); R(cClearColor,"glClearColor");
    R(cDisable,"glDisable"); R(cColorMask,"glColorMask");
    #undef R
    cUniform4fv=(void*)ul_glr(gl,wgpa,"glUniform4fv");   /* non-fatal: only FSR needs it */
    cGetShaderInfoLog=(void*)ul_glr(gl,wgpa,"glGetShaderInfoLog");
    cGenVertexArrays(1,&s_cas_vao);
    s_gl3_state=1;
    return 1;
}
static int ul_cas_ready(void){
    if(ul_filter()<1) return 0;                    /* 0 = bilinear, no shader pass */
    if(s_cas_state>=0) return s_cas_state;
    s_cas_state=0;
    { const char*sh=getenv("ULTRA_CAS_SHARP"); if(sh&&*sh) s_cas_sharp=(float)atof(sh); }
    if(!ul_gl3_ready()) return 0;
    static const char* VS=
      "#version 330\nout vec2 vUV;\nvoid main(){vec2 p=vec2((gl_VertexID==2)?3.0:-1.0,(gl_VertexID==1)?3.0:-1.0);"
      "gl_Position=vec4(p,0.0,1.0);vUV=p*0.5+0.5;}\n";
    static const char* FS=
      "#version 330\nuniform sampler2D uTex;uniform vec2 uInv;uniform float uSharp;uniform float uXLo;uniform float uXHi;in vec2 vUV;out vec4 oC;\n"
      "void main(){vec2 sUV=vec2(mix(uXLo,uXHi,vUV.x),vUV.y);\n"
      "vec3 b=texture(uTex,sUV+vec2(0.0,-uInv.y)).rgb;vec3 d=texture(uTex,sUV+vec2(-uInv.x,0.0)).rgb;"
      "vec3 e=texture(uTex,sUV).rgb;vec3 f=texture(uTex,sUV+vec2(uInv.x,0.0)).rgb;vec3 h=texture(uTex,sUV+vec2(0.0,uInv.y)).rgb;"
      "vec3 mn=min(min(min(b,d),min(f,h)),e);vec3 mx=max(max(max(b,d),max(f,h)),e);"
      "vec3 amp=sqrt(clamp(min(mn,1.0-mx)/max(mx,1e-4),0.0,1.0));vec3 w=-amp*mix(0.125,0.2,uSharp);"
      "vec3 c=(w*(b+d+f+h)+e)/(1.0+4.0*w);oC=vec4(clamp(c,0.0,1.0),1.0);}\n";
    GLu_ vs=ul_cas_compile(0x8B31/*VERTEX_SHADER*/,VS), fs=ul_cas_compile(0x8B30/*FRAGMENT_SHADER*/,FS);
    if(!vs||!fs) return 0;
    s_cas_prog=cCreateProgram(); cAttachShader(s_cas_prog,vs); cAttachShader(s_cas_prog,fs); cLinkProgram(s_cas_prog);
    GLi_ ok=0; cGetProgramiv(s_cas_prog,0x8B82/*LINK_STATUS*/,&ok); if(!ok){ loader_log(LOG_WARN,"[CAS] link failed"); return 0; }
    s_cas_uTex=cGetUniformLocation(s_cas_prog,"uTex");
    s_cas_uInv=cGetUniformLocation(s_cas_prog,"uInv");
    s_cas_uSharp=cGetUniformLocation(s_cas_prog,"uSharp");
    s_cas_uXLo=cGetUniformLocation(s_cas_prog,"uXLo");
    s_cas_uXHi=cGetUniformLocation(s_cas_prog,"uXHi");
    s_cas_state=1;
    loader_log(LOG_WARN,"[CAS] contrast-adaptive sharpen present ready (sharp=%.2f)",s_cas_sharp);
    return 1;
}
/* ── FSR (EASU+RCAS) + FXAA present passes ────────────────────────────────
 * Verified AMD FSR1 EASU/RCAS + FXAA 3.11 GLSL port (see fsr_shaders.h). The
 * present source is resolved (auto-fit cropped) into a sampleable texture,
 * optionally FXAA'd, then either EASU->RCAS (FSR) or the CAS sharpen is run to
 * the window. All three share the fullscreen-triangle VAO (s_cas_vao). FSR
 * needs glUniform4fv; if absent it falls back to CAS, then the linear blit. */
#include "fsr_shaders.h"
static const char* UL_PRESENT_VS =
  "#version 330\nout vec2 vUV;\nvoid main(){vec2 p=vec2((gl_VertexID==2)?3.0:-1.0,(gl_VertexID==1)?3.0:-1.0);"
  "gl_Position=vec4(p,0.0,1.0);vUV=p*0.5+0.5;}\n";
/* present-source (resolve+FXAA) scratch targets */
static unsigned s_src_tex=0,  s_src_fbo=0;  static int s_src_w=0,  s_src_h=0;
static unsigned s_fxaa_tex=0, s_fxaa_fbo=0; static int s_fxaa_w=0, s_fxaa_h=0;
static int s_fxaa_state=-1, s_aa_enabled=-1;
static unsigned s_fxaa_prog=0; static int s_fxaa_uTex=-1, s_fxaa_uRcp=-1;
/* FSR programs + the EASU output-res intermediate */
static int s_fsr_state=-1; static float s_fsr_sharp=0.5f;
static unsigned s_fsr_easu=0, s_fsr_rcas=0;
static int s_fe_uTex=-1, s_fe_c0=-1, s_fe_c1=-1, s_fe_c2=-1, s_fe_c3=-1;
static int s_fr_uTex=-1, s_fr_uSharp=-1;
static unsigned s_fsr_mid_tex=0, s_fsr_mid_fbo=0; static int s_fsr_mid_w=0, s_fsr_mid_h=0;
/* Auto-fit content extents (computed by ul_content_xrange, consumed by the
 * present): horizontal crop [xlo..xhi] (4:3 menu -> 16:9 fill) and a top white-
 * band fraction (ytop) cropped off so the menu sky fills to the top edge. */
static float s_fit_xlo = 0.0f, s_fit_xhi = 1.0f, s_fit_ytop = 0.0f;
static float s_fit_ylo = 0.0f, s_fit_yhi = 1.0f;   /* content Y extent (GL bottom-origin, 0..1) */

static GLu_ ul_build_prog(GLu_ vs, const char* fs_src){
    GLu_ fs=ul_cas_compile(0x8B30/*FRAGMENT_SHADER*/,fs_src); if(!fs) return 0;
    GLu_ p=cCreateProgram(); cAttachShader(p,vs); cAttachShader(p,fs); cLinkProgram(p);
    GLi_ ok=0; cGetProgramiv(p,0x8B82/*LINK_STATUS*/,&ok);
    if(!ok){ loader_log(LOG_WARN,"[PRESENT] program link failed"); return 0; }
    return p;
}
static void ul_make_tex(unsigned* tex, unsigned* fbo, int* cw, int* ch, int w, int h){
    if(*cw!=w || *ch!=h || !*tex){
        if(!*tex) cGenTextures(1,tex);
        cBindTexture(0x0DE1,*tex);
        cTexImage2D(0x0DE1,0,0x8058,w,h,0,0x1908,0x1401,0);
        cTexParameteri(0x0DE1,0x2801,0x2601); cTexParameteri(0x0DE1,0x2800,0x2601); /* LINEAR */
        cTexParameteri(0x0DE1,0x2802,0x812F); cTexParameteri(0x0DE1,0x2803,0x812F); /* CLAMP  */
        if(!*fbo) GFB(1,fbo);
        BFB(0x8D40,*fbo); cFramebufferTexture2D(0x8D40,0x8CE0,0x0DE1,*tex,0);
        *cw=w; *ch=h;
    }
}
static void ul_fs_state_reset(void){
    cDisable(0x0C11/*SCISSOR*/); cDisable(0x0B71/*DEPTH*/); cDisable(0x0BE2/*BLEND*/);
    cDisable(0x0B44/*CULL_FACE*/); cDisable(0x0BC0/*ALPHA_TEST*/); cColorMask(1,1,1,1);
}
static int ul_aa_on(void){
    if(s_aa_enabled<0){ const char*e=getenv("ULTRA_AA"); s_aa_enabled=(e&&*e)?atoi(e):1; }
    return s_aa_enabled;
}
static int ul_fxaa_ready(void){
    if(!ul_aa_on()) return 0;
    if(s_fxaa_state>=0) return s_fxaa_state;
    s_fxaa_state=0;
    if(!ul_gl3_ready()) return 0;
    GLu_ vs=ul_cas_compile(0x8B31/*VERTEX_SHADER*/,UL_PRESENT_VS); if(!vs) return 0;
    s_fxaa_prog=ul_build_prog(vs,UL_FXAA_FS); if(!s_fxaa_prog) return 0;
    s_fxaa_uTex=cGetUniformLocation(s_fxaa_prog,"uTex");
    s_fxaa_uRcp=cGetUniformLocation(s_fxaa_prog,"uRcpFrame");
    s_fxaa_state=1; loader_log(LOG_WARN,"[FXAA] antialiasing present pass ready");
    return 1;
}
/* Resolve the offscreen's auto-fit crop [xlo..xhi] x full-height into a
 * sampleable texture; optionally FXAA it. Returns the texture; *pw,*ph = size. */
static unsigned ul_present_source(float xlo, float xhi, float ylo, float yhi, int* pw, int* ph){
    int sx0=(int)(s_off_w*xlo+0.5f), sx1=(int)(s_off_w*xhi+0.5f);
    if(sx1<=sx0 || sx1>s_off_w || sx0<0){ sx0=0; sx1=s_off_w; }
    int sy0=(int)(s_off_h*ylo+0.5f), sy1=(int)(s_off_h*yhi+0.5f);
    if(sy1<=sy0 || sy1>s_off_h || sy0<0){ sy0=0; sy1=s_off_h; }
    int cw=sx1-sx0, ch=sy1-sy0;
    ul_make_tex(&s_src_tex,&s_src_fbo,&s_src_w,&s_src_h, cw,ch);
    BFB(0x8CA8/*READ*/,s_off_fbo); BFB(0x8CA9/*DRAW*/,s_src_fbo);
    BFBB(sx0,sy0,sx1,sy1, 0,0,cw,ch, 0x4000, 0x2600/*NEAREST, 1:1*/);
    unsigned out=s_src_tex;
    if(ul_fxaa_ready()){
        ul_make_tex(&s_fxaa_tex,&s_fxaa_fbo,&s_fxaa_w,&s_fxaa_h, cw,ch);
        BFB(0x8D40,s_fxaa_fbo); ul_fs_state_reset(); cViewport(0,0,cw,ch);
        cActiveTexture(0x84C0); cBindTexture(0x0DE1,s_src_tex);
        cUseProgram(s_fxaa_prog);
        if(s_fxaa_uTex>=0) cUniform1i(s_fxaa_uTex,0);
        if(s_fxaa_uRcp>=0) cUniform2f(s_fxaa_uRcp, 1.0f/(float)cw, 1.0f/(float)ch);
        cBindVertexArray(s_cas_vao); cDrawArrays(0x0004,0,3);
        out=s_fxaa_tex;
    }
    *pw=cw; *ph=ch;
    return out;
}
static void ul_easu_con(float inW,float inH,float outW,float outH,
                        float c0[4],float c1[4],float c2[4],float c3[4]){
    c0[0]=inW/outW; c0[1]=inH/outH; c0[2]=0.5f*(inW/outW)-0.5f; c0[3]=0.5f*(inH/outH)-0.5f;
    c1[0]=1.0f/inW; c1[1]=1.0f/inH; c1[2]=1.0f/inW; c1[3]=-1.0f/inH;
    c2[0]=-1.0f/inW; c2[1]=2.0f/inH; c2[2]=1.0f/inW; c2[3]=2.0f/inH;
    c3[0]=0.0f; c3[1]=4.0f/inH; c3[2]=0.0f; c3[3]=0.0f;
}
/* user 0..1 -> RCAS lobe multiplier in [0.25..1.0] (1=sharpest). Linear map
 * (matches exp2(-2*(1-u)) endpoints) - avoids a libm dependency. */
static float ul_rcas_sharp(float u){ if(u<0)u=0; if(u>1)u=1; return 0.25f+0.75f*u; }
static int ul_fsr_ready(void){
    if(ul_filter()<2) return 0;
    if(s_fsr_state>=0) return s_fsr_state;
    s_fsr_state=0;
    { const char*sh=getenv("ULTRA_FSR_SHARP"); if(sh&&*sh) s_fsr_sharp=(float)atof(sh); }
    if(!ul_gl3_ready()) return 0;
    if(!cUniform4fv){ loader_log(LOG_WARN,"[FSR] glUniform4fv missing -> fallback to CAS"); return 0; }
    GLu_ vs=ul_cas_compile(0x8B31/*VERTEX_SHADER*/,UL_PRESENT_VS); if(!vs) return 0;
    s_fsr_easu=ul_build_prog(vs,UL_EASU_FS); if(!s_fsr_easu){ loader_log(LOG_WARN,"[FSR] EASU build failed"); return 0; }
    s_fsr_rcas=ul_build_prog(vs,UL_RCAS_FS); if(!s_fsr_rcas){ loader_log(LOG_WARN,"[FSR] RCAS build failed"); return 0; }
    s_fe_uTex=cGetUniformLocation(s_fsr_easu,"uTex");
    s_fe_c0=cGetUniformLocation(s_fsr_easu,"con0"); s_fe_c1=cGetUniformLocation(s_fsr_easu,"con1");
    s_fe_c2=cGetUniformLocation(s_fsr_easu,"con2"); s_fe_c3=cGetUniformLocation(s_fsr_easu,"con3");
    s_fr_uTex=cGetUniformLocation(s_fsr_rcas,"uTex"); s_fr_uSharp=cGetUniformLocation(s_fsr_rcas,"uSharp");
    s_fsr_state=1; loader_log(LOG_WARN,"[FSR] EASU+RCAS upscaler ready (sharp=%.2f)",s_fsr_sharp);
    return 1;
}
static void ul_fsr_do(int dx0,int dy0,int dx1,int dy1,int clearbars,float xlo,float xhi,float ylo,float yhi){
    int inW,inH; unsigned src=ul_present_source(xlo,xhi,ylo,yhi,&inW,&inH);
    int outW=dx1-dx0, outH=dy1-dy0; if(outW<1)outW=1; if(outH<1)outH=1;
    /* EASU: src(inW x inH) -> mid(outW x outH) */
    ul_make_tex(&s_fsr_mid_tex,&s_fsr_mid_fbo,&s_fsr_mid_w,&s_fsr_mid_h, outW,outH);
    BFB(0x8D40,s_fsr_mid_fbo); ul_fs_state_reset(); cViewport(0,0,outW,outH);
    cActiveTexture(0x84C0); cBindTexture(0x0DE1,src);
    cUseProgram(s_fsr_easu);
    if(s_fe_uTex>=0) cUniform1i(s_fe_uTex,0);
    { float c0[4],c1[4],c2[4],c3[4];
      ul_easu_con((float)inW,(float)inH,(float)outW,(float)outH,c0,c1,c2,c3);
      if(s_fe_c0>=0) cUniform4fv(s_fe_c0,1,c0); if(s_fe_c1>=0) cUniform4fv(s_fe_c1,1,c1);
      if(s_fe_c2>=0) cUniform4fv(s_fe_c2,1,c2); if(s_fe_c3>=0) cUniform4fv(s_fe_c3,1,c3); }
    cBindVertexArray(s_cas_vao); cDrawArrays(0x0004,0,3);
    /* RCAS: mid -> window dst rect */
    BFB(0x8D40,0); ul_fs_state_reset();
    if(clearbars){ cClearColor(0,0,0,1); cClear(0x4000); }
    cViewport(dx0,dy0,outW,outH);
    cActiveTexture(0x84C0); cBindTexture(0x0DE1,s_fsr_mid_tex);
    cUseProgram(s_fsr_rcas);
    if(s_fr_uTex>=0)   cUniform1i(s_fr_uTex,0);
    if(s_fr_uSharp>=0) cUniform1f(s_fr_uSharp, ul_rcas_sharp(s_fsr_sharp));
    cBindVertexArray(s_cas_vao); cDrawArrays(0x0004,0,3);
    cUseProgram(0); cBindVertexArray(0);
    BFB(0x8D40,s_off_fbo);
}
static void ul_cas_do(int dx0,int dy0,int dx1,int dy1,int clearbars,float xlo,float xhi,float ylo,float yhi){
    int inW,inH; unsigned src=ul_present_source(xlo,xhi,ylo,yhi,&inW,&inH);  /* cropped + optional FXAA */
    BFB(0x8D40,0); ul_fs_state_reset();
    if(clearbars){ cClearColor(0,0,0,1); cClear(0x4000); }
    cViewport(dx0,dy0,dx1-dx0,dy1-dy0);
    cActiveTexture(0x84C0/*TEXTURE0*/); cBindTexture(0x0DE1,src);
    cUseProgram(s_cas_prog);
    if(s_cas_uTex>=0)   cUniform1i(s_cas_uTex,0);
    if(s_cas_uInv>=0)   cUniform2f(s_cas_uInv, 1.0f/(float)inW, 1.0f/(float)inH);
    if(s_cas_uSharp>=0) cUniform1f(s_cas_uSharp, s_cas_sharp);
    if(s_cas_uXLo>=0)   cUniform1f(s_cas_uXLo, 0.0f);  /* already cropped in the source */
    if(s_cas_uXHi>=0)   cUniform1f(s_cas_uXHi, 1.0f);
    cBindVertexArray(s_cas_vao); cDrawArrays(0x0004/*TRIANGLES*/,0,3);
    cUseProgram(0); cBindVertexArray(0);
    BFB(0x8D40,s_off_fbo);                                               /* restore for next frame */
}

/* --- Real-time content auto-fit -------------------------------------------
 * The game's 3D menu screens render at 4:3 inside our 16:9 offscreen, leaving a
 * pure-white margin on the right (~25%); the 2D screens (gray boot, coin) fill
 * the full 16:9. We detect the actual non-white horizontal extent of the frame
 * each few frames (cheap downscaled readback) and report it as [xlo..xhi] in
 * 0..1 texture space. The present path then stretches THAT range to fill the
 * window width, so a 4:3 menu fills 16:9 with no white. Vertical is left fully
 * intact (0..1), so the top/bottom layout/overscan is untouched. Disable with
 * ULTRA_AUTOFIT=0; the readback runs only on the offscreen, never the desktop. */
static int   s_fit_enabled = -1;
/* Detect the actual CONTENT BOUNDING BOX inside the offscreen FBO and report it as
 * [xlo..xhi] x [ylo..yhi] in 0..1 GL (bottom-origin) coords. The game parks its
 * frame in a sub-rectangle of the offscreen (a 4:3 menu leaves white on the right;
 * gameplay composites into the bottom strip / a corner) with the rest left at the
 * white clear color. The present path stretches THIS box to fill the window, so
 * the menu AND gameplay fill the screen at any resolution regardless of where the
 * game placed them. Robust to internal bright pixels (we take the outer extent of
 * non-white cells, requiring >=2 per row/col to ignore specks) and to all-white
 * frames (we hold the previous box). Disable with ULTRA_AUTOFIT=0. The readback is
 * on the offscreen only - never the desktop. */
static void ul_content_xrange(void){
    if(s_fit_enabled<0){ const char*e=getenv("ULTRA_AUTOFIT"); s_fit_enabled=(e&&*e)?atoi(e):1; }
    if(!s_fit_enabled){ s_fit_xlo=0.0f; s_fit_xhi=1.0f; s_fit_ylo=0.0f; s_fit_yhi=1.0f; return; }
    /* Cutscene / briefing zoom: when the movie LUT signature is live, snap the
     * present box to the calibrated deck rect so it fills the screen. Skips the
     * content auto-detect entirely (the deck is too desaturated to detect). */
    { static int ce=-1;
      if(ce<0){ const char*e; ce=0;
          if((e=getenv("ULTRA_CUT_XLO"))&&*e) g_ul_cut_xlo=(float)atof(e);
          if((e=getenv("ULTRA_CUT_XHI"))&&*e) g_ul_cut_xhi=(float)atof(e);
          if((e=getenv("ULTRA_CUT_YLO"))&&*e) g_ul_cut_ylo=(float)atof(e);
          if((e=getenv("ULTRA_CUT_YHI"))&&*e) g_ul_cut_yhi=(float)atof(e);
          if((e=getenv("ULTRA_CUT_FILL"))&&*e) g_ul_cut_fill=atoi(e); }
      if(ul_cutscene_active()){
          s_fit_xlo=g_ul_cut_xlo; s_fit_xhi=g_ul_cut_xhi;
          s_fit_ylo=g_ul_cut_ylo; s_fit_yhi=g_ul_cut_yhi; return; }
    }
    /* ── GSEVO bottom-left composite fill ──────────────────────────────────
     * GSEVO (GLUT game-mode) reports a 640x480 logical screen to the game and
     * composites EVERY screen (attract, mission-select, gameplay) into a fixed
     * 640x480 sub-rectangle of the 1360x768 native offscreen FBO, parked in the
     * bottom-LEFT corner (verified: content x[0..640] y[0..480] over 1360x768,
     * the rest left at the game's clear color - gray on menus, black in 3D).
     * The white-only auto-detector below can't recover this (the empty region is
     * gray/black, not white), so the corner would present letterboxed-left.
     * Instead snap the present source box to that exact corner so it stretches to
     * fill the window - the same mechanism ABC's native 1360x768 frame uses, just
     * with a per-title fixed box. Source is GL bottom-origin, so y starts at 0.
     * Gated to GSEVO (g_ul_gsevo): ABC and every other title are byte-untouched
     * and keep the auto-fit path. ULTRA_GS_FILL=0 disables; ULTRA_GS_FILL_W/H
     * override the logical size if a build ever reports a different one. */
    { extern int g_ul_gsevo;
      static int ge=-1; static float gxh=0.0f, gyh=0.0f;
      if(ge<0){ const char*e=getenv("ULTRA_GS_FILL"); ge=(e&&*e)?atoi(e):1;
          int lw=0,lh=0; extern void glut_get_logical_size(int*,int*); glut_get_logical_size(&lw,&lh);
          if((e=getenv("ULTRA_GS_FILL_W"))&&*e) lw=atoi(e);
          if((e=getenv("ULTRA_GS_FILL_H"))&&*e) lh=atoi(e);
          int rw=ul_off_render_w(), rh=ul_off_render_h();
          if(lw>0&&lh>0&&rw>0&&rh>0){ gxh=(float)lw/(float)rw; gyh=(float)lh/(float)rh;
              if(gxh>1.0f)gxh=1.0f; if(gyh>1.0f)gyh=1.0f; } }
      if(g_ul_gsevo && ge && gxh>0.05f && gyh>0.05f){
          s_fit_xlo=0.0f; s_fit_xhi=gxh; s_fit_ylo=0.0f; s_fit_yhi=gyh; return; }
    }
    static int s_lb=-1; if(s_lb<0){ const char*e=getenv("ULTRA_CROP_LETTERBOX"); s_lb=(e&&*e)?atoi(e):1; } /* per-title: crop cinematic black bars */
    if(!g_glx.glBlitFramebuffer || !g_glx.glGenFramebuffers || !g_glx.glGenRenderbuffers || !s_off_fbo) return;
    static int s_call=0;                                     /* g_glx.frame_count is 0 in this path; self-count */
    if((s_call++ % 5) != 0) return;                          /* throttle: re-detect every 5th present */
    enum{ DW=160, DH=96 };
    static unsigned det_fbo=0, det_rb=0;
    if(!det_fbo){
        GFB(1,&det_fbo); GRB(1,&det_rb);
        BFB(GL_FRAMEBUFFER,det_fbo); BRB(GL_RENDERBUFFER,det_rb);
        RBS(GL_RENDERBUFFER,GL_RGBA8,DW,DH);
        FBRB(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,det_rb);
    }
    BFB(GL_READ_FRAMEBUFFER,s_off_fbo); BFB(GL_DRAW_FRAMEBUFFER,det_fbo);
    BFBB(0,0,s_off_w,s_off_h, 0,0,DW,DH, GL_COLOR_BUFFER_BIT, GL_LINEAR); /* average -> ignores stray specks */
    BFB(GL_READ_FRAMEBUFFER,det_fbo);
    static void(__stdcall*p_rp)(int,int,int,int,unsigned,unsigned,void*)=0;
    if(!p_rp){ HMODULE gl=GetModuleHandleA("opengl32.dll"); if(gl) p_rp=(void*)GetProcAddress(gl,"glReadPixels"); }
    if(p_rp){
        static unsigned char px[DW*DH*4];
        p_rp(0,0,DW,DH,0x1908/*RGBA*/,0x1401/*UBYTE*/,px);
        int colcnt[DW], rowcnt[DH], total=0;
        for(int x=0;x<DW;x++) colcnt[x]=0;
        for(int y=0;y<DH;y++) rowcnt[y]=0;
        for(int y=0;y<DH;y++) for(int x=0;x<DW;x++){
            unsigned char*p=px+(y*DW+x)*4;
            if(!(p[0]>236 && p[1]>236 && p[2]>236)){ colcnt[x]++; rowcnt[y]++; total++; }
        }
        if(total >= 8){                                      /* skip near-empty frames: hold last box */
            int xlo=-1,xhi=-1,ylo=-1,yhi=-1;
            for(int x=0;x<DW;x++) if(colcnt[x]>=2){ if(xlo<0)xlo=x; xhi=x; }
            for(int y=0;y<DH;y++) if(rowcnt[y]>=2){ if(ylo<0)ylo=y; yhi=y; }
            /* LETTERBOX CROP: trim contiguous near-black rows from the top/bottom
             * of the content box (cinematic black bands) so they fill the screen.
             * Only solid near-black EDGE runs are removed within the x-extent, so a
             * varied gameplay frame (no solid black bar) is left untouched. */
            if(s_lb && xlo>=0 && xhi>xlo && ylo>=0 && yhi>ylo){
                /* a row counts as a letterbox bar if >=88% of it is near-black, so a
                 * cinematic black band with sparse caption text still gets trimmed. */
                int bneed=((xhi-xlo+1)*88)/100; if(bneed<1)bneed=1;
                for(;;){ if(ylo>=yhi) break; int bc=0;
                    for(int xx=xlo;xx<=xhi;xx++){ unsigned char*pp=px+(ylo*DW+xx)*4; if(pp[0]<=32&&pp[1]<=32&&pp[2]<=32) bc++; }
                    if(bc<bneed) break; ylo++; }
                for(;;){ if(yhi<=ylo) break; int bc=0;
                    for(int xx=xlo;xx<=xhi;xx++){ unsigned char*pp=px+(yhi*DW+xx)*4; if(pp[0]<=32&&pp[1]<=32&&pp[2]<=32) bc++; }
                    if(bc<bneed) break; yhi--; }
                /* and trim near-WHITE edge COLUMNS (e.g. a menu's solid white side
                 * border) - a column qualifies only if MOST of its height is near-
                 * white, so bright sky over darker ocean/HUD in gameplay never does. */
                int need=((yhi-ylo+1)*85)/100; if(need<1)need=1;
                for(;;){ if(xlo>=xhi) break; int wc=0;
                    for(int yy=ylo;yy<=yhi;yy++){ unsigned char*pp=px+(yy*DW+xlo)*4; if(pp[0]>238&&pp[1]>238&&pp[2]>238) wc++; }
                    if(wc<need) break; xlo++; }
                for(;;){ if(xhi<=xlo) break; int wc=0;
                    for(int yy=ylo;yy<=yhi;yy++){ unsigned char*pp=px+(yy*DW+xhi)*4; if(pp[0]>238&&pp[1]>238&&pp[2]>238) wc++; }
                    if(wc<need) break; xhi--; }
            }
            if(xlo>=0 && xhi>xlo && ylo>=0 && yhi>ylo){
                float txlo=(float)xlo/(float)DW, txhi=(float)(xhi+1)/(float)DW;
                float tylo=(float)ylo/(float)DH, tyhi=(float)(yhi+1)/(float)DH; /* row 0 = GL bottom */
                /* A tiny content box = a transient text/loading screen (e.g. INSERT
                 * COIN: mostly white with a small caption). Don't magnify it; show
                 * the full frame. Only a substantial parked frame gets stretched. */
                if((txhi-txlo) < 0.25f || (tyhi-tylo) < 0.25f){ txlo=0.0f; txhi=1.0f; tylo=0.0f; tyhi=1.0f; }
                s_fit_xlo += (txlo - s_fit_xlo)*0.5f;  s_fit_xhi += (txhi - s_fit_xhi)*0.5f;
                s_fit_ylo += (tylo - s_fit_ylo)*0.5f;  s_fit_yhi += (tyhi - s_fit_yhi)*0.5f;
            }
        }
    }
    BFB(GL_FRAMEBUFFER,s_off_fbo);                           /* restore for the main present  */
}

static void ul_present_upscale(void) {
    static unsigned int s_fbo = 0, s_rb = 0;
    static int s_w = 0, s_h = 0;
    static int s_off = 0;          /* once a env override resolves to 0, give up */
    static int s_env = 0;
    if (!s_env) {                  /* honor env overrides once */
        s_env = 1;
        const char *e;
        if ((e = getenv("ULTRA_PRESENT_UPSCALE")) && *e) g_ul_present_upscale = atoi(e);
        if ((e = getenv("ULTRA_CONTENT_W")) && *e) g_ul_content_w = atoi(e);
        if ((e = getenv("ULTRA_CONTENT_H")) && *e) g_ul_content_h = atoi(e);
        if ((e = getenv("ULTRA_DISP_MODE")) && *e) g_ul_disp_mode = atoi(e);
        if ((e = getenv("ULTRA_GAME_ASPECT")) && *e) { float a=(float)atof(e); if (a>0.1f) g_ul_game_aspect = a; }
        loader_log(LOG_WARN, "[GLX] present disp_mode=%d game_aspect=%.3f upscale=%d window=%dx%d",
                   g_ul_disp_mode, g_ul_game_aspect, g_ul_present_upscale, g_width, g_height);
    }

    /* PRIMARY PATH: the game rendered to our offscreen FBO at its NATIVE res.
     * Scale-blit the whole thing to the output window -- this is how we present
     * at ANY resolution (480p..4K) crisply, decoupled from the game's fixed
     * internal res. Runs whenever offscreen is active, regardless of the legacy
     * corner-upscale flag below. */
    if (g_ul_offscreen && s_off_fbo && g_glx.glBlitFramebuffer && g_glx.glBindFramebuffer) {
        if (g_glx.frame_count < 4 || (getenv("ULTRA_VP_TRACE") && g_glx.frame_count%60==0))
            loader_log(LOG_WARN,"[PRESENT] blit off=%ux%u(fbo %u) -> win %dx%d frame=%d",
                       s_off_w,s_off_h,s_off_fbo,g_width,g_height,g_glx.frame_count);
        /* Real-time auto-fit: measure the non-white content extent so a 4:3 menu
         * gets stretched horizontally to fill the 16:9 window (vertical intact). */
        ul_content_xrange();
        if (getenv("ULTRA_VP_TRACE") && g_glx.frame_count%60==0)
            loader_log(LOG_WARN,"[AUTOFIT] x=[%.3f..%.3f] frame=%d",s_fit_xlo,s_fit_xhi,g_glx.frame_count);
        /* glBlitFramebuffer's write to the window is subject to the SCISSOR test
         * and color mask. The game leaves a scissor box (~1360x768) set, which
         * clips our up-scaled blit to that corner - fine at <=720p (box covers
         * the window) but corners at 1080p/4K. Force the full destination state. */
        static void (__stdcall *p_disable)(unsigned) = 0;
        static void (__stdcall *p_colormask)(unsigned char,unsigned char,unsigned char,unsigned char) = 0;
        static void (__stdcall *p_clear)(unsigned) = 0;
        static void (__stdcall *p_clearcol)(float,float,float,float) = 0;
        static int s_res = 0;
        if (!s_res) { HMODULE gl = GetModuleHandleA("opengl32.dll");
            if (gl) { p_disable=(void*)GetProcAddress(gl,"glDisable");
                      p_colormask=(void*)GetProcAddress(gl,"glColorMask");
                      p_clear=(void*)GetProcAddress(gl,"glClear");
                      p_clearcol=(void*)GetProcAddress(gl,"glClearColor"); } s_res=1; }
        if (p_disable) p_disable(0x0C11 /*GL_SCISSOR_TEST*/);
        if (p_colormask) p_colormask(1,1,1,1);
        BFB(GL_READ_FRAMEBUFFER, s_off_fbo);
        BFB(GL_DRAW_FRAMEBUFFER, 0);
        /* Display-mode aware destination rect, ALWAYS CENTERED. The offscreen holds
         * the game; g_ul_game_aspect is the game's intended display aspect (w/h).
         *   0 = Stretch     : fill the whole window (ignore aspect)
         *   1 = Pillarbox   : fit the game aspect inside the window (black bars)
         *   2 = Zoom        : fill the window at the game aspect, crop the overflow
         *   3 = Fit to Screen: full-window fill + stronger vertical squeeze (top
         *                      overscan revealed via the proj fit above)   [default] */
        {
            int W=g_width, H=g_height, dx0=0,dy0=0,dx1=W,dy1=H;
            float ga = g_ul_game_aspect;
            if ((g_ul_disp_mode == 1 || g_ul_disp_mode == 2) && ga > 0.1f) {
                if (g_ul_disp_mode == 1) {                 /* fit (letter/pillar bars) */
                    int pbw = (int)(H*ga + 0.5f);
                    if (pbw <= W) { dx0=(W-pbw)/2; dx1=dx0+pbw; }
                    else { int pbh=(int)(W/ga + 0.5f); dy0=(H-pbh)/2; dy1=dy0+pbh; }
                    if (p_clearcol) p_clearcol(0,0,0,1);   /* black bars */
                    if (p_clear)    p_clear(0x00004000u);
                } else {                                   /* zoom = fill + crop, centered */
                    int zw = (int)(H*ga + 0.5f);
                    if (zw >= W) { dx0=(W-zw)/2; dx1=dx0+zw; }
                    else { int zh=(int)(W/ga + 0.5f); dy0=(H-zh)/2; dy1=dy0+zh; }
                }
            }
            /* Source = the detected content bounding box (X and Y), shrunk by a small
             * OVERSCAN so the very-edge pixels (a sub-pixel white seam the box can
             * leave) are pushed off and the window is covered border-to-border. The
             * inset crops ~1.5% of the frame edges - invisible, guarantees no white.
             * Tunable via ULTRA_OVERSCAN (fraction, default 0.018; 0 = exact box). */
            static float s_ov = -1.0f;
            if (s_ov < 0.0f) { const char *e = getenv("ULTRA_OVERSCAN"); s_ov = (e && *e) ? (float)atof(e) : 0.006f;
                               if (s_ov < 0.0f) s_ov = 0.0f; if (s_ov > 0.2f) s_ov = 0.2f; }   /* small: clears edge seams w/o clipping corner HUD */
            float fxlo = s_fit_xlo, fxhi = s_fit_xhi, fylo = s_fit_ylo, fyhi = s_fit_yhi;
            { float ix = (fxhi - fxlo) * s_ov, iy = (fyhi - fylo) * s_ov;
              fxlo += ix; fxhi -= ix; fylo += iy; fyhi -= iy; }
            /* PUBLISH THE PRESENT GEOMETRY so the aim mapper can use the rectangle the
             * game is ACTUALLY drawn into.
             *
             * A lightgun title aims in the space of its own picture, but that picture is
             * not the window: in Fit mode (g_ul_disp_mode == 1, which is what ultra.ini
             * selects today) it sits inside letterbox/pillarbox bars, and the source is
             * further inset by the overscan. The aim code used to map the cursor across
             * the whole CLIENT RECT, so every pixel of black bar shifted the crosshair
             * away from the pointer -- owner-reported on Ghost Squad Evolution as the
             * crosshair being 'so far from my mouse' (2026-09-19).
             *
             * Because the present blits src[fx,fy] -> dst[dx,dy] one-to-one, a cursor at
             * fraction f across the DESTINATION rect is at fraction f across the visible
             * picture -- which is exactly the game's own aim space. So the mapper needs
             * the destination rect, and nothing more. */
            g_ul_present_dx0 = dx0; g_ul_present_dy0 = dy0;
            g_ul_present_dx1 = dx1; g_ul_present_dy1 = dy1;
            g_ul_present_valid = 1;
            int sx0 = (int)(s_off_w * fxlo + 0.5f);
            int sx1 = (int)(s_off_w * fxhi + 0.5f);
            int sy0 = (int)(s_off_h * fylo + 0.5f);
            int sy1 = (int)(s_off_h * fyhi + 0.5f);
            if (sx1 <= sx0 || sx0 < 0 || sx1 > s_off_w) { sx0 = 0; sx1 = s_off_w; }
            if (sy1 <= sy0 || sy0 < 0 || sy1 > s_off_h) { sy0 = 0; sy1 = s_off_h; }
            /* ── GSEVO present must use the PLAIN scale-blit, NOT FSR/CAS ───────
             * GSEVO composites its scene into a fixed bottom-left 640x480 corner of
             * the 1360x768 offscreen, so the GSEVO fill-crop hands the present a
             * TINY source sub-rect (~632x474) that it stretches ~3x to 1920x1080.
             * The FSR EASU/RCAS (and CAS) present passes resolve that sub-rect into
             * an intermediate texture and shader-upscale it - and for this small,
             * heavily-magnified crop the shader chain resolves to a BLACK window
             * (verified: path=FSR -> 0% non-black; path=BLIT -> 94% non-black, the
             * MISSION SELECT screen fills correctly with true colors). The plain
             * glBlitFramebuffer LINEAR path handles an arbitrary source rect with no
             * such failure. Gate the bypass on g_ul_gsevo so ABC (full-frame source,
             * FSR works perfectly) and every other title keep the FSR/CAS upscaler.
             * Tunable: ULTRA_GS_PLAINBLIT=0 re-enables FSR for GSEVO (debug). */
            extern int g_ul_gsevo;
            static int gs_plain=-1;
            if(gs_plain<0){ const char*e=getenv("ULTRA_GS_PLAINBLIT"); gs_plain=(e&&*e)?atoi(e):1; }
            int use_plain = (g_ul_gsevo && gs_plain);
            if (getenv("ULTRA_BLIT_TRACE")) { static int bt=0; if(bt<6){ bt++;
                loader_log(LOG_WARN,"[BLITBOX] fit[%.3f..%.3f,%.3f..%.3f] off=%dx%d src[%d,%d..%d,%d] dst[%d,%d..%d,%d] path=%s",
                    s_fit_xlo,s_fit_xhi,s_fit_ylo,s_fit_yhi,s_off_w,s_off_h,sx0,sy0,sx1,sy1,dx0,dy0,dx1,dy1,
                    use_plain?"BLIT(gs)":(ul_fsr_ready()?"FSR":(ul_cas_ready()?"CAS":"BLIT"))); } }
            if (!use_plain && ul_fsr_ready()) {
                ul_fsr_do(dx0, dy0, dx1, dy1, g_ul_disp_mode == 1, fxlo, fxhi, fylo, fyhi); /* FSR EASU+RCAS (+FXAA) - overscan-inset so no edge seam */
            } else if (!use_plain && ul_cas_ready()) {
                ul_cas_do(dx0, dy0, dx1, dy1, g_ul_disp_mode == 1, fxlo, fxhi, fylo, fyhi); /* CAS sharpen (+FXAA) - overscan-inset */
            } else {
                /* Plain scale-blit: full destination state (scissor off, full color
                 * mask) was already forced above; clear bars in pillarbox mode. */
                if (g_ul_disp_mode == 1) { if (p_clearcol) p_clearcol(0,0,0,1); if (p_clear) p_clear(0x00004000u); }
                BFBB(sx0,sy0,sx1,sy1,  dx0,dy0,dx1,dy1,  GL_COLOR_BUFFER_BIT, GL_LINEAR);
            }
        }
        BFB(GL_FRAMEBUFFER, s_off_fbo);          /* rebind for next frame */
        return;
    }
    if (g_ul_offscreen && g_glx.frame_count < 4)
        loader_log(LOG_WARN,"[PRESENT] offscreen blit SKIPPED s_off_fbo=%u blit=%p bind=%p",
                   s_off_fbo, (void*)g_glx.glBlitFramebuffer, (void*)g_glx.glBindFramebuffer);

    if (!g_ul_present_upscale) return;
    int sw = g_ul_content_w, sh = g_ul_content_h;
    if (sw <= 0 || sh <= 0) { s_off = 1; return; }
    if (sw >= g_width && sh >= g_height) return;   /* already fills, nothing to do */
    if (!g_glx.glBlitFramebuffer || !g_glx.glGenFramebuffers ||
        !g_glx.glGenRenderbuffers) return;

    /* The game renders at its native size with glViewport(0,0,sw,sh), i.e. into
     * the GL bottom-left corner (GL origin is bottom-left).  ULTRA_CONTENT_TOP=1
     * reads the top-left instead (for titles whose viewport sits at the top). */
    static int s_top = -1;
    if (s_top < 0) { const char *e = getenv("ULTRA_CONTENT_TOP"); s_top = (e && atoi(e)) ? 1 : 0; }
    int sy0 = s_top ? (g_height - sh) : 0;
    int sy1 = s_top ? g_height : sh;
    if (sy0 < 0) sy0 = 0;

    /* Software path: the game rendered to the window's back buffer.  Can't blit
     * default->default, so copy the corner into a temp FBO then scale it back. */
    if (!s_fbo || s_w != sw || s_h != sh) {
        if (s_fbo) DFB(1, &s_fbo);
        GFB(1, &s_fbo);
        GRB(1, &s_rb);
        BFB(GL_FRAMEBUFFER, s_fbo);
        BRB(GL_RENDERBUFFER, s_rb);
        RBS(GL_RENDERBUFFER, GL_RGBA8, sw, sh);
        FBRB(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_rb);
        BFB(GL_FRAMEBUFFER, 0);
        s_w = sw; s_h = sh;
    }
    BFB(GL_READ_FRAMEBUFFER, 0);
    BFB(GL_DRAW_FRAMEBUFFER, s_fbo);
    BFBB(0,sy0,sw,sy1,  0,0,sw,sh,  GL_COLOR_BUFFER_BIT, GL_LINEAR);
    BFB(GL_READ_FRAMEBUFFER, s_fbo);
    BFB(GL_DRAW_FRAMEBUFFER, 0);
    BFBB(0,0,sw,sh,  0,0,g_width,g_height,  GL_COLOR_BUFFER_BIT, GL_LINEAR);
    BFB(GL_FRAMEBUFFER, 0);
}

/* Public entry point for the SDL/GLUT present path (sdl_window_pump). */
void glx_present_upscale(void) { ul_present_upscale(); }

/* Bind the real window framebuffer (0) for reading, using the SAME resolved
 * pointer the present blit uses (glBindFramebuffer is a GL3 entry point that
 * GetProcAddress can't see - it must come via wglGetProcAddress). The frame-dump
 * path calls this so glReadPixels captures the actual scaled window, not whatever
 * offscreen FBO the present path left bound. */
void glx_bind_read_window(void) {
    if (g_glx.glBindFramebuffer) BFB(GL_READ_FRAMEBUFFER, 0);
}

/* Calibration aid: bind the RAW offscreen FBO for readback (the game's composite
 * BEFORE the present transform) and report its size, so a frame dump shows the
 * untransformed layout we must calibrate the cutscene crop rect against. */
int glx_bind_read_offscreen(int *w, int *h) {
    if (!g_ul_offscreen || !s_off_fbo || !g_glx.glBindFramebuffer) return 0;
    BFB(GL_READ_FRAMEBUFFER, s_off_fbo);
    if (w) *w = s_off_w; if (h) *h = s_off_h;
    return 1;
}

/* ── Ultra Omni-Track thin white aim border ───────────────────────────────
 * Present-time overlay for camera (IR-camera / webcam) light guns: a crisp
 * thin WHITE frame around the OUTER edge of the window. This is our slim
 * replacement for Sinden's thick border (the camera tracks the screen edge);
 * harmless decoration for IR/Wii-style guns.
 *
 * Drawn AFTER the game frame is fully presented into framebuffer 0 but BEFORE
 * SwapBuffers, using glScissor + glClear(white) for the 4 edge rectangles -
 * the most compat-safe method (no shaders/VAOs; works on the fixed-function +
 * ARB-asm GL the Lindbergh titles use). Sized to the CURRENT drawable, so it
 * auto-fits any output resolution (720p..4K). Fully gated on
 * g_settings.lightgun_border: when off, NOTHING here runs and the present path
 * is byte-identical. All touched GL state is saved and restored. */
typedef void (WINAPI *glEnable_t)(unsigned);
typedef void (WINAPI *glDisable_t)(unsigned);
typedef unsigned char (WINAPI *glIsEnabled_t)(unsigned);
typedef void (WINAPI *glClearColor_t)(float,float,float,float);
typedef void (WINAPI *glClear_t)(unsigned);
typedef void (WINAPI *glGetFloatv_t)(unsigned, float*);

void glx_draw_aim_border(void) {
    /* Draw when the user enabled the border OR a Sinden gun is auto-detected
     * (camera guns need it - true even in a mixed RS3+Sinden setup; harmless to
     * IR guns). Off + no Sinden -> no-op, so the present path stays byte-identical. */
    extern int lightgun_border_active(void);
    if (!lightgun_border_active()) return;                 /* gate: off -> no-op */

    int win_w = g_width  > 0 ? g_width  : 1280;
    int win_h = g_height > 0 ? g_height : 720;

    /* thickness in px, auto-fitted; sane clamp so a stray value can't paint the
     * whole screen white. Default to ~0.4% of the short edge when unset. */
    int t = g_settings.lightgun_border_thickness;
    if (t <= 0) { t = (win_h < win_w ? win_h : win_w) / 240; if (t < 2) t = 2; }
    int maxt = (win_h < win_w ? win_h : win_w) / 2;
    if (t > maxt) t = maxt;
    if (t < 1) return;

    static glEnable_t      p_enable     = NULL;
    static glDisable_t     p_disable    = NULL;
    static glIsEnabled_t   p_isenabled  = NULL;
    static glClearColor_t  p_clearcolor = NULL;
    static glClear_t       p_clear      = NULL;
    static glGetIntegerv_t p_getiv      = NULL;
    static glGetFloatv_t   p_getfv      = NULL;
    static int resolved = 0;
    if (!resolved) {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (!gl) gl = LoadLibraryA("opengl32.dll");
        if (gl) {
            p_enable     = (glEnable_t)     GetProcAddress(gl, "glEnable");
            p_disable    = (glDisable_t)    GetProcAddress(gl, "glDisable");
            p_isenabled  = (glIsEnabled_t)  GetProcAddress(gl, "glIsEnabled");
            p_clearcolor = (glClearColor_t) GetProcAddress(gl, "glClearColor");
            p_clear      = (glClear_t)      GetProcAddress(gl, "glClear");
            p_getiv      = (glGetIntegerv_t)GetProcAddress(gl, "glGetIntegerv");
            p_getfv      = (glGetFloatv_t)  GetProcAddress(gl, "glGetFloatv");
        }
        resolved = 1;
    }
    if (!p_enable || !p_disable || !p_clearcolor || !p_clear ||
        !s_real_glScissor) return;

    /* Make sure we draw to the real window (framebuffer 0), not a leftover
     * offscreen FBO the present blit may have left bound for READ. */
    if (g_glx.glBindFramebuffer) BFB(GL_DRAW_FRAMEBUFFER, 0);

    #ifndef GL_SCISSOR_TEST
    #define GL_SCISSOR_TEST 0x0C11
    #endif
    #ifndef GL_SCISSOR_BOX
    #define GL_SCISSOR_BOX 0x0C10
    #endif
    #ifndef GL_COLOR_CLEAR_VALUE
    #define GL_COLOR_CLEAR_VALUE 0x0C22
    #endif

    /* save state we touch */
    int   saved_scissor[4] = {0,0,0,0};
    float saved_clear[4]   = {0,0,0,0};
    unsigned char scissor_was_on = 0;
    if (p_isenabled) scissor_was_on = p_isenabled(GL_SCISSOR_TEST);
    if (p_getiv) p_getiv(GL_SCISSOR_BOX, saved_scissor);
    if (p_getfv) p_getfv(GL_COLOR_CLEAR_VALUE, saved_clear);

    p_enable(GL_SCISSOR_TEST);
    p_clearcolor(1.0f, 1.0f, 1.0f, 1.0f);

    /* bottom, top, left, right (GL scissor origin is bottom-left) */
    s_real_glScissor(0, 0, win_w, t);                 p_clear(GL_COLOR_BUFFER_BIT);
    s_real_glScissor(0, win_h - t, win_w, t);         p_clear(GL_COLOR_BUFFER_BIT);
    s_real_glScissor(0, 0, t, win_h);                 p_clear(GL_COLOR_BUFFER_BIT);
    s_real_glScissor(win_w - t, 0, t, win_h);         p_clear(GL_COLOR_BUFFER_BIT);

    /* restore */
    s_real_glScissor(saved_scissor[0], saved_scissor[1],
                     saved_scissor[2], saved_scissor[3]);
    p_clearcolor(saved_clear[0], saved_clear[1], saved_clear[2], saved_clear[3]);
    if (!scissor_was_on) p_disable(GL_SCISSOR_TEST);
}

void glx_swap_buffers(glx_pbuffer_obj_t *drawable) {
    g_glx.frame_count++;
    if (g_glx.frame_count <= 3) loader_log(LOG_WARN, "[RENDER] swap frame=%d drawable=%p fbo=%d ctx=%p current_draw=%p",
        g_glx.frame_count, drawable,
        (drawable && drawable->mesa.fbo) ? (int)drawable->mesa.fbo : -1,
        g_glx.current_ctx, g_glx.current_draw);
    if (g_glx.glFlush) ((glFlush_t)g_glx.glFlush)();
    glx_context_obj_t *ctx = g_glx.current_ctx;
    int did_blit = 0;
    if (ctx && drawable && drawable->mesa.fbo
        && g_glx.glBlitFramebuffer && g_glx.glBindFramebuffer) {
        BFB(GL_READ_FRAMEBUFFER, drawable->mesa.fbo);
        BFB(GL_DRAW_FRAMEBUFFER, 0);
        int blit_w = drawable->mesa.width  > 0 ? drawable->mesa.width  : g_width;
        int blit_h = drawable->mesa.height > 0 ? drawable->mesa.height : g_height;
        /* Scale the offscreen drawable up to fill the whole window. */
        BFBB(0,0,blit_w,blit_h,  0,0,g_width,g_height,
             GL_COLOR_BUFFER_BIT, GL_LINEAR);
        BFB(GL_FRAMEBUFFER, 0);
        did_blit = 1;
    }
    /* Game rendered straight to the window (no pbuffer): up-scale its content
     * corner to fill the widescreen window. */
    if (!did_blit && g_ul_present_upscale) ul_present_upscale();
    glx_draw_aim_border();   /* gated on g_settings.lightgun_border */

    /* HDC resolution order:
     * 1. wglGetCurrentDC() - live, authoritative when glXMakeCurrent bound the
     *    context on this thread (GLX-direct path after shim_glXMakeCurrent fix).
     * 2. glx_get_hdc() from the bridge - the HDC of the real game window created
     *    by stub_XCreateWindow.  Valid even when no context is current.
     * 3. g_glx.main_hdc - cached at glx_emu_init() time; may be the stale 1x1
     *    pre-init HDC if the bridge tore that window down.  Last resort. */
    HDC swap_hdc = g_glx.wglGetCurrentDC ? g_glx.wglGetCurrentDC() : NULL;
    if (!swap_hdc) {
        extern int  g_sdl_active;
        extern HDC  glx_get_hdc(void);
        if (!g_sdl_active) swap_hdc = glx_get_hdc();
    }
    if (!swap_hdc) swap_hdc = g_glx.main_hdc;
    if (swap_hdc && swap_hdc != g_glx.main_hdc) {
        /* Bridge's game-window HDC - update the cache and reload extensions
         * now that a real render context is active. */
        g_glx.main_hdc   = swap_hdc;
        g_glx.helper_hdc = swap_hdc;
        g_glx.ext_loaded = false;
        glx_load_extensions();
        loader_log(LOG_WARN, "[GLX] late-bound swap HDC=0x%p via wglGetCurrentDC (frame %d)", swap_hdc, g_glx.frame_count);
    }

    /* Non-GLUT games (Hummer) never call glutMainLoop so sdl_window_pump() and
     * splash_content_check() never fire.  Signal content-seen after the first
     * swap so the boot console closes on schedule. */
    if (!g_sdl_active && g_glx.frame_count >= 1 && !g_splash_dismiss) {
        g_splash_dismiss = 1;
        loader_log(LOG_INFO, "[GLX] GLX-direct path: content signaled at frame %d", g_glx.frame_count);
    }

    if (swap_hdc) SwapBuffers(swap_hdc);
    else loader_log(LOG_WARN, "[GLX] glx_swap_buffers: no HDC at frame %d, dropped", g_glx.frame_count);

    /* GLX-direct games (Hummer/LGJ/HOTD4) never call XNextEvent or
     * glutMainLoop, so Win32 messages are never pumped from the game loop.
     * Drain the queue here every frame to keep the window responsive. */
    {
        MSG _msg;
        while (PeekMessageA(&_msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&_msg);
            DispatchMessageA(&_msg);
            if (_msg.message == WM_QUIT) ExitProcess(0);
        }
    }
}

/* ── Destroy ─────────────────────────────────────────────────────────── */
void glx_destroy_context(glx_context_obj_t *ctx) {
    if (!ctx || !ctx->tag.alive) return;
    if (ctx->mesa.is_current) { g_glx.wglMakeCurrent(NULL,NULL); g_glx.current_ctx=NULL; }
    /* Don't delete SDL's context */
    ctx->tag.alive = false;
    loader_log(LOG_INFO, "[GLX] Context destroyed: ptr=0x%p", ctx);
}

void glx_destroy_pbuffer(glx_pbuffer_obj_t *pb) {
    if (!pb || !pb->tag.alive) return;
    if (pb->mesa.fbo && DFB) DFB(1,&pb->mesa.fbo);
    pb->tag.alive = false;
}
/* ── NV_fence GPU-sync wrapper (Snapshot restore) ────────────────────────────
 * ABC paces the GPU with NV_fence: glSetFenceNV(id) inserts a fence,
 * glFinishFenceNV(id) BLOCKS until the GPU passes it. After a Snapshot restore
 * the driver-side fence state no longer matches the rolled-back game, so the
 * blocking wait never completes and the game's display callback wedges in the
 * driver. For a short window after a restore we substitute a full glFinish
 * (which drains the GPU and ALWAYS completes - a strict superset of the fence
 * wait) so the game gets past the desync; once frames advance again the real
 * fence is re-armed each frame and we hand control back to it. */
static void *s_real_finishfence = 0;
static void *s_real_glfinish    = 0;
int  g_snap_fence_until = 0;     /* swap-frame until which fence waits are hard-synced */
void ul_snap_fence_safe(int frames){ extern int g_ul_swap_frame; g_snap_fence_until = g_ul_swap_frame + frames; }
static void ul_glfinish_now(void){
    if (!s_real_glfinish){ HMODULE g=GetModuleHandleA("opengl32.dll"); if(g) s_real_glfinish=(void*)GetProcAddress(g,"glFinish"); }
    if (s_real_glfinish) ((void(WINAPI*)(void))s_real_glfinish)();
}
static void __cdecl ul_glFinishFenceNV(unsigned id){
    extern int g_ul_swap_frame;
    if (g_ul_swap_frame < g_snap_fence_until){      /* post-restore: fence desynced -> hard sync */
        if (getenv("ULTRA_SNAP_TRACE")) loader_log(LOG_WARN, "[SNAP] glFinishFenceNV(%u) -> glFinish (post-restore)", id);
        ul_glfinish_now(); return;
    }
    if (s_real_finishfence) ((void(WINAPI*)(unsigned))s_real_finishfence)(id);
    else ul_glfinish_now();
}

/* Decide offscreen-redirect / viewport-scale / shader-fallback modes from the
 * driver + env. Idempotent (env-driven, no accumulation) so it is safe to call
 * before BOTH the generic binder and the hardcoded ABC table. Extracted from
 * patch_game_gl_got so the generic binder sees the same g_ul_offscreen /
 * g_ul_vp_scale that the glBindFramebufferEXT redirect + glViewport upscale
 * wrappers key off. */
static void ul_decide_gl_modes(void)
{
    const char *drv = getenv("ULTRA_GL_DRIVER");
    int native = drv && (!_stricmp(drv,"native")||!_stricmp(drv,"gpu")||!_stricmp(drv,"system")||
                         !_stricmp(drv,"amd")||!_stricmp(drv,"nvidia")||!_stricmp(drv,"intel"));
    int mesa_hw = drv && (!_stricmp(drv,"zink") || !_stricmp(drv,"d3d12"));
    const char *off = getenv("ULTRA_OFFSCREEN");
    if (off && *off) g_ul_offscreen = atoi(off);
    else g_ul_offscreen = (native || mesa_hw) ? 1 : 0;
    const char *fb = getenv("ULTRA_SHADER_FALLBACK");
    g_ul_shader_fallback = (fb && *fb) ? atoi(fb) : ((native || mesa_hw) ? 1 : 0);
    const char *vps = getenv("ULTRA_VP_SCALE");
    if (vps && *vps) g_ul_vp_scale = atoi(vps);
    else if (g_ul_offscreen) g_ul_vp_scale = 1;
}

/* Public shim so non-glx_emu translation units (libgl_stub.c) can settle the
 * GL mode flags before installing the ARB rewriter on the GLUT path. */
void ul_decide_gl_modes_pub(void) { ul_decide_gl_modes(); }

/* ------------------------------------------------------------------ *
 * glGetString interceptor - legacy-Cg stack-overflow guard.
 *
 * Let's Go Jungle links the 2002-era NVIDIA Cg runtime (libCg.so / libCgGL.so).
 * During _sShaderManager::initialize(), libCgGL's GL-profile detector calls
 * glGetString(GL_EXTENSIONS) / glGetString(GL_VERSION) and copies the result
 * into FIXED, small stack buffers sized for an early-2000s driver. A modern
 * RX 580 OpenGL ICD returns a multi-kilobyte GL_EXTENSIONS string and a long
 * GL_VERSION ("4.6.0 Compatibility Profile Context 25.x.x ..."), which overruns
 * those buffers and trips the compiler-inserted GS stack cookie ->
 * __fastfail(STATUS_STACK_BUFFER_OVERRUN, 0xC0000409). That failfast is raised
 * by KERNELBASE and bypasses our VEH + __stack_chk_fail override entirely,
 * which is exactly the blocker observed.
 *
 * FIX: when a Cg runtime is present, return CURATED, NUL-terminated, length-
 * capped strings for GL_EXTENSIONS / GL_VERSION / GL_RENDERER / GL_VENDOR. The
 * extension list is a SUPERSET of what both Cg/libCgGL and the Lindbergh ARB/Cg
 * shader path need, but short enough (<1KB) to never overflow a legacy buffer.
 * Everything else (and the no-Cg case, e.g. ABC) passes straight through to the
 * real driver string, so ABC is byte-identical.
 *
 * glGetString is SHARED with ABC. ABC does NOT load libCg/libCgGL (verified:
 * Cg is bundled only in the LGJ disk image; ABC's .dynsym has no cg* imports),
 * so the Cg-presence gate leaves ABC on the real driver strings unchanged. */
#define UL_GL_VENDOR     0x1F00
#define UL_GL_RENDERER   0x1F01
#define UL_GL_VERSION    0x1F02
#define UL_GL_EXTENSIONS 0x1F03

/* One-time scan of the loaded ELF images for a bundled Cg runtime. */
static int ul_cg_runtime_present(void)
{
    static int cached = -1;
    if (cached >= 0) return cached;
    cached = 0;
    if (g_loader_ctx) {
        for (int i = 0; i < g_loader_ctx->image_count; i++) {
            LoadedElf *img = g_loader_ctx->images[i];
            if (!img) continue;
            const char *p = img->path;
            /* match libCgGL.so or libCg.so anywhere in the path (case-insensitive
             * on the basename is unnecessary - Linux sonames are exact). */
            if (strstr(p, "libCgGL") || strstr(p, "libCg.")) { cached = 1; break; }
        }
    }
    return cached;
}

/* Curated, capped GL_EXTENSIONS - a superset of what libCgGL's GL profile and
 * the Lindbergh ARB/Cg shaders need (vertex/fragment programs, multitexture,
 * VBO, FBO, shader objects, GLSL 1.00, texture compression/rectangle/cube,
 * occlusion query, point parameters, common EXT/NV the engine probes). Kept
 * well under 1KB so legacy fixed buffers never overflow. */
static const char UL_CG_EXT_STRING[] =
    "GL_ARB_multitexture GL_ARB_texture_env_add GL_ARB_texture_env_combine "
    "GL_ARB_texture_env_dot3 GL_ARB_texture_cube_map GL_ARB_texture_compression "
    "GL_ARB_texture_rectangle GL_ARB_texture_non_power_of_two "
    "GL_ARB_vertex_program GL_ARB_fragment_program GL_ARB_vertex_buffer_object "
    "GL_ARB_pixel_buffer_object GL_ARB_vertex_shader GL_ARB_fragment_shader "
    "GL_ARB_shader_objects GL_ARB_shading_language_100 GL_ARB_occlusion_query "
    "GL_ARB_point_parameters GL_ARB_point_sprite GL_ARB_depth_texture "
    "GL_ARB_shadow GL_ARB_multisample GL_ARB_draw_buffers GL_ARB_half_float_pixel "
    "GL_EXT_framebuffer_object GL_EXT_framebuffer_blit GL_EXT_packed_depth_stencil "
    "GL_EXT_texture_filter_anisotropic GL_EXT_blend_func_separate "
    "GL_EXT_blend_equation_separate GL_EXT_stencil_two_side GL_EXT_secondary_color "
    "GL_EXT_bgra GL_EXT_abgr GL_EXT_texture3D GL_EXT_texture_lod_bias "
    "GL_NV_fence GL_NV_occlusion_query GL_NV_register_combiners "
    "GL_SGIS_generate_mipmap";

const unsigned char * __cdecl ul_glGetString(unsigned name)
{
    /* resolve the real driver glGetString once */
    static const unsigned char* (__stdcall *p_real)(unsigned) = 0;
    if (!p_real) {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (!gl) gl = LoadLibraryA("opengl32.dll");
        if (gl) p_real = (void*)GetProcAddress(gl, "glGetString");
    }
    const unsigned char *real = p_real ? p_real(name) : NULL;

    /* DIAGNOSTIC: log every glGetString with returned length + first chars so
     * the faulting call is identifiable in the log right before any failfast. */
    if (getenv("ULTRA_GLSTR_TRACE")) {
        size_t L = real ? strlen((const char*)real) : 0;
        char head[80]; size_t hn = L < 64 ? L : 64;
        if (real) { memcpy(head, real, hn); head[hn] = 0; } else head[0] = 0;
        loader_log(LOG_WARN, "[GLSTR] glGetString(0x%x) len=%zu head=\"%s\"",
                   name, L, head);
    }

    /* No Cg runtime (e.g. ABC): behave EXACTLY like the real driver. */
    if (!ul_cg_runtime_present()) return real;

    /* Cg present (LGJ): substitute curated, length-capped strings on the
     * profile-detection queries that legacy Cg copies into fixed buffers. */
    switch (name) {
    case UL_GL_EXTENSIONS:
        return (const unsigned char*)UL_CG_EXT_STRING;
    case UL_GL_VERSION:
        /* legacy-friendly 2.1 string so libCgGL's version parser stays within
         * its fixed buffer and selects the ARB shader profiles. */
        return (const unsigned char*)"2.1.0 Ultra Loader";
    case UL_GL_RENDERER:
        return (const unsigned char*)"Ultra Loader GL";
    case UL_GL_VENDOR:
        return (const unsigned char*)"Ultra Loader";
    default:
        return real;   /* GL_SHADING_LANGUAGE_VERSION etc. -> real driver */
    }
}

/* ------------------------------------------------------------------ *
 * Shared GL-name -> install-pointer resolver.
 *
 * Given a GL function NAME, resolve it against Mesa/native GL and return the
 * exact pointer the GOT slot should hold - including the SAME special-case
 * wrappers the hardcoded ABC table installs (glViewport upscale, glTexImage2D
 * depth-format fix, glProgramStringARB NV->ARB rewriter, anisotropy on
 * glTexParameteri, the offscreen glBindFramebufferEXT redirect, the matrix /
 * projection wrappers, the snapshot fence wrapper, ...). This is the single
 * source of truth used by the NAME-DRIVEN generic binder below.
 *
 * Returns NULL when the name is not a Mesa/native GL export (e.g. glX*, glut*,
 * glu*, which are bound separately to the loader's own shims at relocation
 * time) - the generic binder then leaves that slot untouched, preserving the
 * shim. Also sets up the matching s_real_* trampoline target as a side effect,
 * exactly like the hardcoded table. */
static void *ul_resolve_gl_install(const char *name, void *(WINAPI *gpa)(const char*), HMODULE gl32)
{
    if (!name) return NULL;
    /* Never touch the windowing / GLX / GLU slots - those are the loader's own
     * shims (glXMakeCurrent, glutMainLoop, gluLookAt, ...). Mesa doesn't export
     * them anyway, but skip defensively so a stray Mesa glu* can't clobber. */
    if (!strncmp(name, "glX", 3) || !strncmp(name, "glut", 4)) return NULL;
    if (!strncmp(name, "glu", 3) && strncmp(name, "glut", 4)) return NULL; /* gluXxx but not glut */

    void *fn = gpa ? gpa(name) : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, name);
    if (!fn) return NULL;   /* not a real GL export Mesa/native knows */

    /* --- special-case wrappers (must mirror patch_game_gl_got exactly) --- */
    if (!strcmp(name, "glTexImage2D")) {
        s_real_glTexImage2D = (glTexImage2D_t)fn;
        return (void*)ul_glTexImage2D;                 /* raw cdecl wrapper */
    }
    if (!strcmp(name, "glTexSubImage2D")) {
        s_real_glTexSubImage2D = (glTexSubImage2D_t)fn;
        return (void*)ul_glTexSubImage2D;              /* raw cdecl wrapper */
    }
    if (!strcmp(name, "glTexParameteri")) {
        s_real_glTexParameteri = (glTexParameteri_t)fn;
        { const char *e=getenv("ULTRA_ANISO"); if(e&&*e) g_ul_aniso=atoi(e); }
        return (void*)ul_glTexParameteri;              /* raw cdecl wrapper */
    }
    if (!strcmp(name, "glProgramStringARB")) {
        extern void __cdecl stub_glProgramStringARB(unsigned,unsigned,int,const void*);
        g_real_glProgramStringARB = (pglProgramStringARB)fn;
        return (void*)stub_glProgramStringARB;         /* raw cdecl rewriter */
    }
    if (!strcmp(name, "glFinishFenceNV")) {
        s_real_finishfence = fn;
        return (void*)ul_glFinishFenceNV;              /* raw cdecl wrapper */
    }
    if (!strcmp(name, "glGetString")) {
        /* legacy-Cg stack-overflow guard: ul_glGetString is itself __cdecl and
         * forwards to the real stdcall glGetString internally, so it goes into
         * the GOT directly (NOT through the cdecl thunk). For non-Cg titles it
         * returns the real driver string verbatim (ABC byte-identical). */
        return (void*)ul_glGetString;
    }
    /* The following install a WINAPI wrapper THROUGH the cdecl thunk (the game
     * calls cdecl; wrapper is stdcall -> needs the thunk like a raw GL fn). */
    if (!strcmp(name, "glMatrixMode")) {
        s_real_glMatrixMode = (glMatrixMode_t)fn;
        return gl_cdecl_thunk((void*)&ul_glMatrixMode);
    }
    if (!strcmp(name, "glViewport")) {
        s_real_glViewport = (glViewport_t)fn;
        if (!s_real_glGetIntegerv)
            s_real_glGetIntegerv = (glGetIntegerv_t)GetProcAddress(gl32, "glGetIntegerv");
        { const char *e = getenv("ULTRA_VP_SCALE"); if (e && *e) g_ul_vp_scale = atoi(e); }
        return gl_cdecl_thunk((void*)&ul_glViewport);
    }
    if (!strcmp(name, "glLoadMatrixf")) {
        s_real_glLoadMatrixf = (glLoadMatrixf_t)fn;
        return gl_cdecl_thunk((void*)&ul_glLoadMatrixf);
    }
    if (!strcmp(name, "glGetIntegerv")) {
        s_real_glGetIntegerv = (glGetIntegerv_t)fn;
        return gl_cdecl_thunk((void*)&ul_glGetIntegerv_diag);
    }
    if (!strcmp(name, "glLoadIdentity")) {
        s_real_glLoadIdentity = (glLoadIdentity_t)fn;
        return gl_cdecl_thunk((void*)&ul_glLoadIdentity);
    }
    if (!strcmp(name, "glOrtho")) {
        s_real_glOrtho = (glOrtho_t)fn;
        return gl_cdecl_thunk((void*)&ul_glOrtho);
    }
    if (!strcmp(name, "glBindFramebufferEXT")) {
        s_real_glBindFB = (glBindFramebuffer_t)fn;
        /* offscreen redirect only when offscreen is on (matches hardcoded) */
        return gl_cdecl_thunk(g_ul_offscreen ? (void*)&ul_glBindFramebufferEXT : fn);
    }

    /* --- default: real GL fn (stdcall) routed through the cdecl thunk --- */
    return gl_cdecl_thunk(fn);
}

/* NAME-DRIVEN GENERIC GL GOT BINDER.
 *
 * Walks the loaded ELF's recorded R_386_JUMP_SLOT relocations (the slot->name
 * map dynlink.c built during relocation) and, for every slot whose symbol is a
 * core/ARB/EXT GL function, writes the resolved pointer (via the shared
 * resolver above) straight into the slot. This makes GL bind AUTOMATICALLY for
 * ANY Lindbergh ELF, with no hardcoded per-game address table.
 *
 * SAFETY: every write is guarded by a VirtualQuery so an unmapped/foreign slot
 * is skipped (never corrupts another title's address space). g_ul_offscreen /
 * g_ul_vp_scale must already be decided (caller sets them, same as the
 * hardcoded path). Returns the number of slots bound. */
int bind_gl_got_generic(void)
{
    HMODULE gl32 = glut_get_mesa_handle();
    if (!gl32) gl32 = GetModuleHandleA("opengl32.dll");
    if (!gl32) gl32 = LoadLibraryA("opengl32.dll");
    if (!gl32) { loader_log(LOG_WARN, "[GOT-GEN] no GL handle, skipping generic bind"); return 0; }
    typedef void*(WINAPI*GPA_t)(const char*);
    GPA_t gpa = (GPA_t)GetProcAddress(gl32, "wglGetProcAddress");

    ul_decide_gl_modes();   /* set g_ul_offscreen / g_ul_vp_scale before binding */

    extern int dynlink_slotmap_enumerate(uint32_t*, const char**, int);
    enum { MAXSLOT = 4096 };
    static uint32_t slots[MAXSLOT];
    static const char *names[MAXSLOT];
    int n = dynlink_slotmap_enumerate(slots, names, MAXSLOT);

    int bound = 0, skipped_unmapped = 0, considered = 0;
    for (int i = 0; i < n; i++) {
        const char *nm = names[i];
        if (!nm || strncmp(nm, "gl", 2)) continue;     /* gl / glX / glut / glu */
        considered++;
        void *install = ul_resolve_gl_install(nm, gpa, gl32);
        if (!install) continue;                        /* glX/glut/glu or unknown - leave shim */
        uint32_t slot = slots[i];
        /* Guard: only write into committed, writable guest memory. */
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)(uintptr_t)slot, &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT) { skipped_unmapped++; continue; }
        DWORD old;
        if (!VirtualProtect((void*)(uintptr_t)slot, 4, PAGE_READWRITE, &old)) { skipped_unmapped++; continue; }
        *(uint32_t*)(uintptr_t)slot = (uint32_t)(uintptr_t)install;
        VirtualProtect((void*)(uintptr_t)slot, 4, old, &old);
        bound++;
    }
    loader_log(LOG_WARN, "[GOT-GEN] name-driven GL bind: %d slots bound (%d gl* considered, %d skipped unmapped, of %d JUMP_SLOTs)",
               bound, considered, skipped_unmapped, n);
    return bound;
}

/* GOT patch: write real GL function pointers directly into game's GOT.
 * This bypasses the opengl32->atioglxx ICD dispatch chain entirely.
 * The game's PLT stubs: CALL -> GOT -> we overwrite GOT with real ptrs.
 * Called once after wglMakeCurrent so wglGetProcAddress works correctly. */
void patch_game_gl_got(void) {
    HMODULE gl32 = glut_get_mesa_handle();
    if (!gl32) gl32 = GetModuleHandleA("opengl32.dll");
    if (!gl32) gl32 = LoadLibraryA("opengl32.dll");
    loader_log(LOG_WARN, "[GOT] patch_game_gl_got: gl32=%p", gl32);
    if (!gl32) { loader_log(LOG_WARN, "[GOT] No GL handle, cannot patch"); return; }

    /* ADDITIVE-FALLBACK GUARD: this hardcoded table is ABC-specific (literal ABC
     * GOT addresses). The generic name-driven binder (bind_gl_got_generic) has
     * already bound GL for whatever title loaded. Only run the hardcoded table
     * when the loaded ELF is actually ABC - verified by checking ABC's anchor
     * slot 0x08344ce8 is a recorded JUMP_SLOT named "glDrawElements". For any
     * other title we skip it entirely so we never write ABC's literal addresses
     * into a foreign address space. */
    {
        extern const char *dynlink_symbol_for_got_slot(uint32_t);
        const char *anchor = dynlink_symbol_for_got_slot(0x08344ce8u);
        if (!anchor || strcmp(anchor, "glDrawElements")) {
            loader_log(LOG_WARN, "[GOT] hardcoded ABC table SKIPPED (not ABC: anchor 0x08344ce8=%s), generic binder is authoritative",
                       anchor ? anchor : "<none>");
            return;
        }
        loader_log(LOG_WARN, "[GOT] ABC detected (anchor=glDrawElements), applying hardcoded table as additive fallback");
    }

    loader_log(LOG_WARN, "[GOT] Patching game GOT with Mesa function pointers");
    typedef void*(WINAPI*GPA_t)(const char*);
    GPA_t gpa = (GPA_t)GetProcAddress(gl32, "wglGetProcAddress");

    /* Decide offscreen-redirect / viewport-scale / shader-fallback modes BEFORE
     * the GOT loop (so the glBindFramebufferEXT patch installs the redirect
     * wrapper). Shared with the generic binder so both see the same decision. */
    ul_decide_gl_modes();
    loader_log(LOG_WARN, "[GOT] offscreen=%d vp_scale=%d shader_fallback=%d",
               g_ul_offscreen, g_ul_vp_scale, g_ul_shader_fallback);

    uint32_t *got;
    void *fn;
    int patched = 0;

    got = (uint32_t*)(uintptr_t)0x08344ce8u;
    fn = gpa ? gpa("glDrawElements") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glDrawElements");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344cecu;
    fn = gpa ? gpa("glSetFenceNV") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glSetFenceNV");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344cf8u;
    fn = gpa ? gpa("glTexCoordPointer") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glTexCoordPointer");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344d08u;
    fn = gpa ? gpa("glIsTexture") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glIsTexture");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344d10u;
    fn = gpa ? gpa("glMultTransposeMatrixf") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glMultTransposeMatrixf");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344d24u;
    fn = gpa ? gpa("glColor4ubv") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glColor4ubv");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344d28u;
    fn = gpa ? gpa("glCompressedTexImage2D") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glCompressedTexImage2D");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344d34u;
    fn = gpa ? gpa("glPixelStorei") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glPixelStorei");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344d40u;
    fn = gpa ? gpa("glFogf") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glFogf");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344d48u;
    fn = gpa ? gpa("glMapBuffer") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glMapBuffer");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344d54u;
    fn = gpa ? gpa("glPopAttrib") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glPopAttrib");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344d5cu;
    fn = gpa ? gpa("glTexImage2D") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glTexImage2D");
    if (fn) {
        /* Install our depth-format compatibility wrapper instead of raw Mesa,
         * forwarding to the real function. Fixes fbuffer.cpp:207 GL_INVALID_VALUE. */
        s_real_glTexImage2D = (glTexImage2D_t)fn;
        *got = (uint32_t)(uintptr_t)ul_glTexImage2D;
        patched++;
        loader_log(LOG_WARN, "[GLCOMPAT] glTexImage2D wrapped (real=0x%p, wrapper=0x%p)", fn, ul_glTexImage2D);
    }

    got = (uint32_t*)(uintptr_t)0x08344d6cu;
    fn = gpa ? gpa("glReadPixels") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glReadPixels");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344d74u;
    fn = gpa ? gpa("glGenTextures") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glGenTextures");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344d7cu;
    fn = gpa ? gpa("glFinishFenceNV") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glFinishFenceNV");
    if (fn) { s_real_finishfence = fn;                 /* wrap: hard-sync past fence desync after a Snapshot restore */
              *got = (uint32_t)(uintptr_t)ul_glFinishFenceNV; patched++; }

    got = (uint32_t*)(uintptr_t)0x08344d94u;
    fn = gpa ? gpa("glColor3f") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glColor3f");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344d98u;
    fn = gpa ? gpa("glVertexAttrib1f") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glVertexAttrib1f");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344d9cu;
    fn = gpa ? gpa("glAlphaFunc") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glAlphaFunc");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344da0u;
    fn = gpa ? gpa("glGenFramebuffersEXT") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glGenFramebuffersEXT");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344da4u;
    fn = gpa ? gpa("glColor4ub") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glColor4ub");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344db0u;
    fn = gpa ? gpa("glBlendColor") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glBlendColor");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344db8u;
    fn = gpa ? gpa("glRotatef") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glRotatef");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344dbcu;
    fn = gpa ? gpa("glClampColorARB") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glClampColorARB");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344de0u;
    fn = gpa ? gpa("glFlush") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glFlush");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344df8u;
    fn = gpa ? gpa("glGetBufferParameterivARB") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glGetBufferParameterivARB");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344e08u;
    fn = gpa ? gpa("glFinish") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glFinish");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344e2cu;
    fn = gpa ? gpa("glEnableClientState") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glEnableClientState");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344e38u;
    fn = gpa ? gpa("glBlendFuncSeparate") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glBlendFuncSeparate");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344e40u;
    fn = gpa ? gpa("glVertex3f") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glVertex3f");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344e70u;
    fn = gpa ? gpa("glMultiTexCoord2fv") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glMultiTexCoord2fv");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344e84u;
    fn = gpa ? gpa("glDrawArrays") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glDrawArrays");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344e88u;
    fn = gpa ? gpa("glTexCoord2fv") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glTexCoord2fv");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344e8cu;
    fn = gpa ? gpa("glMultMatrixf") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glMultMatrixf");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344e90u;
    fn = gpa ? gpa("glClearStencil") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glClearStencil");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344ea4u;
    fn = gpa ? gpa("glDeleteProgramsARB") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glDeleteProgramsARB");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344ea8u;
    fn = gpa ? gpa("glGetTexLevelParameteriv") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glGetTexLevelParameteriv");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344eacu;
    fn = gpa ? gpa("glRasterPos2f") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glRasterPos2f");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344eb0u;
    fn = gpa ? gpa("glTexParameterf") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glTexParameterf");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344eb4u;
    fn = gpa ? gpa("glDisable") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glDisable");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344ec0u;
    fn = gpa ? gpa("glDepthMask") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glDepthMask");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344eccu;
    fn = gpa ? gpa("glMatrixMode") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glMatrixMode");
    if (fn) { s_real_glMatrixMode = (glMatrixMode_t)fn;
              *got = (uint32_t)(uintptr_t)gl_cdecl_thunk((void*)&ul_glMatrixMode); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344ed4u;
    fn = gpa ? gpa("glGenProgramsARB") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glGenProgramsARB");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344ed8u;
    fn = gpa ? gpa("glBufferData") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glBufferData");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344eecu;
    fn = gpa ? gpa("glColorMask") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glColorMask");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344ef4u;
    fn = gpa ? gpa("glBindFramebufferEXT") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glBindFramebufferEXT");
    if (fn) {
        /* Offscreen redirect: route the game's framebuffer-0 binds to our private
         * FBO so it never touches (and never deadlocks on) the swapchain. */
        s_real_glBindFB = (glBindFramebuffer_t)fn;
        *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(
                   g_ul_offscreen ? (void*)&ul_glBindFramebufferEXT : fn);
        patched++;
    }

    got = (uint32_t*)(uintptr_t)0x08344f0cu;
    fn = gpa ? gpa("glNewList") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glNewList");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344f18u;
    fn = gpa ? gpa("glViewport") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glViewport");
    if (fn) {
        /* Up-scale the game's 640x480 viewport to fill the window.  This is the
         * path the game actually calls (its GOT), so the wrapper goes here. */
        s_real_glViewport = (glViewport_t)fn;
        if (!s_real_glGetIntegerv)
            s_real_glGetIntegerv = (glGetIntegerv_t)GetProcAddress(gl32, "glGetIntegerv");
        { const char *e = getenv("ULTRA_VP_SCALE"); if (e && *e) g_ul_vp_scale = atoi(e); }
        /* Always install the wrapper (it passes through when vp_scale=0) so the
         * trace + future remap see every call. */
        *got = (uint32_t)(uintptr_t)gl_cdecl_thunk((void*)&ul_glViewport);
        patched++;
        loader_log(LOG_WARN, "[GLX] glViewport GOT wrapped (vp_scale=%d win=%dx%d)",
                   g_ul_vp_scale, g_width, g_height);
    }

    got = (uint32_t*)(uintptr_t)0x08344f28u;
    fn = gpa ? gpa("glBegin") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glBegin");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344f54u;
    fn = gpa ? gpa("glDeleteRenderbuffersEXT") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glDeleteRenderbuffersEXT");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344f5cu;
    fn = gpa ? gpa("glLineWidth") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glLineWidth");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344f68u;
    fn = gpa ? gpa("glDeleteBuffersARB") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glDeleteBuffersARB");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344f80u;
    fn = gpa ? gpa("glProgramStringARB") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glProgramStringARB");
    if (fn) {
        /* Route through our rewriter (strips OPTION NV_fragment_program2 + the
         * half-precision opcodes Mesa rejects). stub is cdecl, calls real stdcall. */
        extern void __cdecl stub_glProgramStringARB(unsigned,unsigned,int,const void*);
        g_real_glProgramStringARB = (pglProgramStringARB)fn;
        *got = (uint32_t)(uintptr_t)stub_glProgramStringARB;
        patched++;
        loader_log(LOG_WARN, "[GL] glProgramStringARB -> NV->ARB rewriter installed");
    }

    got = (uint32_t*)(uintptr_t)0x08344f8cu;
    fn = gpa ? gpa("glPolygonOffset") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glPolygonOffset");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344fa8u;
    fn = gpa ? gpa("glGetFloatv") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glGetFloatv");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344fbcu;
    fn = gpa ? gpa("glCopyTexSubImage2D") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glCopyTexSubImage2D");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344fc0u;
    fn = gpa ? gpa("glBufferDataARB") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glBufferDataARB");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344fccu;
    fn = gpa ? gpa("glMultiTexCoord2f") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glMultiTexCoord2f");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344fd4u;
    fn = gpa ? gpa("glVertexAttribPointerARB") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glVertexAttribPointerARB");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344fd8u;
    fn = gpa ? gpa("glTexParameteri") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glTexParameteri");
    if (fn) { s_real_glTexParameteri = (glTexParameteri_t)fn;
              { const char *e=getenv("ULTRA_ANISO"); if(e&&*e) g_ul_aniso=atoi(e); }
              *got = (uint32_t)(uintptr_t)ul_glTexParameteri; patched++; }

    got = (uint32_t*)(uintptr_t)0x08344ff4u;
    fn = gpa ? gpa("glCheckFramebufferStatusEXT") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glCheckFramebufferStatusEXT");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08344ff8u;
    fn = gpa ? gpa("glBindBufferARB") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glBindBufferARB");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345008u;
    fn = gpa ? gpa("glTexCoord2f") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glTexCoord2f");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x0834500cu;
    fn = gpa ? gpa("glGenLists") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glGenLists");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345014u;
    fn = gpa ? gpa("glLoadMatrixf") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glLoadMatrixf");
    if (fn) { s_real_glLoadMatrixf = (glLoadMatrixf_t)fn;
              *got = (uint32_t)(uintptr_t)gl_cdecl_thunk((void*)&ul_glLoadMatrixf); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345024u;
    fn = gpa ? gpa("glDeleteFramebuffersEXT") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glDeleteFramebuffersEXT");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345028u;
    fn = gpa ? gpa("glProgramLocalParameter4fARB") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glProgramLocalParameter4fARB");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x0834503cu;
    fn = gpa ? gpa("glProgramLocalParameter4fvARB") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glProgramLocalParameter4fvARB");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345044u;
    fn = gpa ? gpa("glGenFencesNV") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glGenFencesNV");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345054u;
    fn = gpa ? gpa("glCallList") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glCallList");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345058u;
    fn = gpa ? gpa("glLightfv") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glLightfv");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x0834505cu;
    fn = gpa ? gpa("glGetError") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glGetError");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345064u;
    fn = gpa ? gpa("glDeleteFencesNV") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glDeleteFencesNV");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345068u;
    fn = gpa ? gpa("glGetIntegerv") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glGetIntegerv");
    if (fn) {
        s_real_glGetIntegerv = (glGetIntegerv_t)fn;
        *got = (uint32_t)(uintptr_t)gl_cdecl_thunk((void*)&ul_glGetIntegerv_diag);
        patched++;
    }

    got = (uint32_t*)(uintptr_t)0x08345074u;
    fn = gpa ? gpa("glPushAttrib") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glPushAttrib");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x0834508cu;
    fn = gpa ? gpa("glLoadIdentity") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glLoadIdentity");
    if (fn) { s_real_glLoadIdentity = (glLoadIdentity_t)fn;
              *got = (uint32_t)(uintptr_t)gl_cdecl_thunk((void*)&ul_glLoadIdentity); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345098u;
    fn = gpa ? gpa("glIsBufferARB") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glIsBufferARB");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083450a8u;
    fn = gpa ? gpa("glPopMatrix") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glPopMatrix");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083450acu;
    fn = gpa ? gpa("glVertexPointer") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glVertexPointer");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083450c8u;
    fn = gpa ? gpa("glGetBooleanv") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glGetBooleanv");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083450e0u;
    fn = gpa ? gpa("glOrtho") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glOrtho");
    if (fn) {
        s_real_glOrtho = (glOrtho_t)fn;
        *got = (uint32_t)(uintptr_t)gl_cdecl_thunk((void*)&ul_glOrtho);
        patched++;
    }

    got = (uint32_t*)(uintptr_t)0x083450e4u;
    fn = gpa ? gpa("glFogi") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glFogi");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083450f0u;
    fn = gpa ? gpa("glClearColor") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glClearColor");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345108u;
    fn = gpa ? gpa("glTexSubImage2D") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glTexSubImage2D");
    if (fn) { s_real_glTexSubImage2D = (glTexSubImage2D_t)fn;
              *got = (uint32_t)(uintptr_t)ul_glTexSubImage2D; patched++; }

    got = (uint32_t*)(uintptr_t)0x0834510cu;
    fn = gpa ? gpa("glFrontFace") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glFrontFace");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345134u;
    fn = gpa ? gpa("glDepthFunc") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glDepthFunc");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x0834513cu;
    fn = gpa ? gpa("glGetString") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glGetString");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345154u;
    fn = gpa ? gpa("glEndList") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glEndList");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345158u;
    fn = gpa ? gpa("glMaterialf") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glMaterialf");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345168u;
    fn = gpa ? gpa("glTexParameterfv") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glTexParameterfv");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345174u;
    fn = gpa ? gpa("glTexEnvf") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glTexEnvf");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x0834517cu;
    fn = gpa ? gpa("glPushMatrix") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glPushMatrix");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345180u;
    fn = gpa ? gpa("glEnableVertexAttribArrayARB") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glEnableVertexAttribArrayARB");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x0834518cu;
    fn = gpa ? gpa("glLoadTransposeMatrixf") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glLoadTransposeMatrixf");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345194u;
    fn = gpa ? gpa("glGetMaterialfv") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glGetMaterialfv");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345198u;
    fn = gpa ? gpa("glBindTexture") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glBindTexture");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083451a0u;
    fn = gpa ? gpa("glEnable") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glEnable");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083451a4u;
    fn = gpa ? gpa("glNormal3f") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glNormal3f");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083451b0u;
    fn = gpa ? gpa("glCullFace") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glCullFace");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083451b4u;
    fn = gpa ? gpa("glDeleteBuffers") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glDeleteBuffers");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083451b8u;
    fn = gpa ? gpa("glBindProgramARB") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glBindProgramARB");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083451c0u;
    fn = gpa ? gpa("glUnmapBuffer") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glUnmapBuffer");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083451d4u;
    fn = gpa ? gpa("glFrustum") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glFrustum");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083451e0u;
    fn = gpa ? gpa("glClear") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glClear");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083451e4u;
    fn = gpa ? gpa("glClientActiveTexture") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glClientActiveTexture");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083451fcu;
    fn = gpa ? gpa("glBindBuffer") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glBindBuffer");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345210u;
    fn = gpa ? gpa("glDisableClientState") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glDisableClientState");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345214u;
    fn = gpa ? gpa("glPolygonMode") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glPolygonMode");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x0834521cu;
    fn = gpa ? gpa("glProgramEnvParameter4fARB") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glProgramEnvParameter4fARB");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x0834522cu;
    fn = gpa ? gpa("glGenBuffers") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glGenBuffers");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345238u;
    fn = gpa ? gpa("glMaterialfv") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glMaterialfv");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345244u;
    fn = gpa ? gpa("glFramebufferTexture2DEXT") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glFramebufferTexture2DEXT");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345248u;
    fn = gpa ? gpa("glColorPointer") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glColorPointer");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345254u;
    fn = gpa ? gpa("glShadeModel") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glShadeModel");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x0834525cu;
    fn = gpa ? gpa("glScalef") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glScalef");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345260u;
    fn = gpa ? gpa("glNormalPointer") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glNormalPointer");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345264u;
    fn = gpa ? gpa("glReadBuffer") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glReadBuffer");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345268u;
    fn = gpa ? gpa("glClearDepth") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glClearDepth");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345284u;
    fn = gpa ? gpa("glDeleteTextures") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glDeleteTextures");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345290u;
    fn = gpa ? gpa("glIsEnabled") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glIsEnabled");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x0834529cu;
    fn = gpa ? gpa("glTranslatef") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glTranslatef");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083452c0u;
    fn = gpa ? gpa("glBlendEquation") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glBlendEquation");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083452c8u;
    fn = gpa ? gpa("glDisableVertexAttribArrayARB") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glDisableVertexAttribArrayARB");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083452e0u;
    fn = gpa ? gpa("glBlendFunc") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glBlendFunc");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083452ecu;
    fn = gpa ? gpa("glActiveTexture") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glActiveTexture");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x083452f4u;
    fn = gpa ? gpa("glEnd") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glEnd");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345304u;
    fn = gpa ? gpa("glDeleteLists") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glDeleteLists");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x0834530cu;
    fn = gpa ? gpa("glIsFenceNV") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glIsFenceNV");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    got = (uint32_t*)(uintptr_t)0x08345314u;
    fn = gpa ? gpa("glProgramEnvParameter4fvARB") : NULL;
    if (!fn) fn = (void*)GetProcAddress(gl32, "glProgramEnvParameter4fvARB");
    if (fn) { *got = (uint32_t)(uintptr_t)gl_cdecl_thunk(fn); patched++; }

    int failed = 0; (void)failed;
    loader_log(LOG_WARN, "[GOT] GOT patched: %d GL function pointers installed", patched);
    /* Verify first entry written correctly */
    { uint32_t *v=(uint32_t*)(uintptr_t)0x08344ce8u; loader_log(LOG_WARN, "[GOT] GOT[0] = 0x%08x (glDrawElements)", *v); }
    /* Check key drawing functions */
    #define CHECK_GOT(addr, name) { uint32_t *v=(uint32_t*)(uintptr_t)(addr); loader_log(LOG_WARN, "[GOT] " name " = 0x%08x", *v); }
    CHECK_GOT(0x8344f28, "glBegin")
    CHECK_GOT(0x8344e40, "glVertex3f")
    CHECK_GOT(0x83451e0, "glClear")
    CHECK_GOT(0x83452f4, "glEnd")
    /* calc_camera_matrix's GL calls - verify they route through the cdecl thunk
     * pool (else a raw stdcall slips through and smashes ESP -> camera ret-to-0). */
    loader_log(LOG_WARN, "[GOT] thunk pool: 0x%08x .. 0x%08x",
               (uint32_t)(uintptr_t)g_glthunk_pool,
               (uint32_t)(uintptr_t)(g_glthunk_pool + g_glthunk_used));
    CHECK_GOT(0x8344ecc, "glMatrixMode")
    CHECK_GOT(0x834517c, "glPushMatrix")
    CHECK_GOT(0x83450a8, "glPopMatrix")
    CHECK_GOT(0x834508c, "glLoadIdentity")
    CHECK_GOT(0x83451d4, "glFrustum")
    CHECK_GOT(0x8344fa8, "glGetFloatv")
    #undef CHECK_GOT
}

#ifndef P32
#define P32(p) ((uint32_t)(uintptr_t)(p))
#endif

/* Low-memory allocator: ensures all GLX objects are in the low 4GB
 * so 32-bit guest code can dereference the pointers safely.
 * Uses a static pool in the launcher's data segment (always <4GB). */


bool glx_intercept_free(void *ptr) {
    if (!ptr) return false;
    for (int i = 0; i < g_glx.n_objects; i++) {
        if (g_glx.objects[i] == ptr) {
            /* Find the tag after the mesa struct */
            glx_tag_t *tag = NULL;
            glx_fbconfig_obj_t *fc = (glx_fbconfig_obj_t*)ptr;
            glx_context_obj_t  *cx = (glx_context_obj_t*)ptr;
            glx_pbuffer_obj_t  *pb = (glx_pbuffer_obj_t*)ptr;
            /* Check which type by trying tag at known offset */
            if      (fc->tag.magic == GLX_EMU_MAGIC) tag = &fc->tag;
            else if (cx->tag.magic == GLX_EMU_MAGIC) tag = &cx->tag;
            else if (pb->tag.magic == GLX_EMU_MAGIC) tag = &pb->tag;

            if (tag) {
                loader_log(LOG_INFO, "[GLX] Intercepted free(%p) type=%d, swallowed", ptr, tag->type);
                return true;
            }
        }
    }
    /* Also intercept by address range of our display object */
    if (g_glx.display && ptr == g_glx.display) {
        loader_log(LOG_INFO, "[GLX] Intercepted free(display), swallowed");
        return true;
    }
    return false;
}


/* Low-memory allocator: ensures all GLX objects are in the low 4GB
 * so 32-bit guest code can dereference the pointers safely.
 * Uses a static pool in the launcher's data segment (always <4GB). */



/* ── shim_glX* wrappers registered with dynlink ──────────────────────────── */

#define GLX_ENSURE() do { \
    if (!g_glx.initialized) \
        glx_emu_init(glut_get_hwnd(), glut_get_hdc()); \
} while(0)

uint32_t shim_glXChooseFBConfig(uint32_t dpy, uint32_t screen, uint32_t attrib, uint32_t n) {
    GLX_ENSURE();
    int count = 0;
    glx_fbconfig_obj_t *obj = glx_choose_fbconfig((const int*)(uintptr_t)attrib, &count);
    if (!obj) return 0;
    loader_log(LOG_INFO, "[GLX] glXChooseFBConfig -> array=0x%p [0]=0x%p", obj->array, obj->array[0]);
    if (n) *(uint32_t*)(uintptr_t)n = (uint32_t)count;
    return P32(obj->array);
}

uint32_t shim_glXCreatePbuffer(uint32_t dpy, uint32_t cfg, uint32_t attrib) {
    GLX_ENSURE();
    glx_fbconfig_obj_t *fbcfg = NULL;
    if (cfg) {
        /* cfg is pointer to array, [0] is the fbconfig. Guard against
         * bridge-lane sentinels crossing here (e.g. Hummer's fake
         * FBConfig list) - a bare deref would AV. */
        void *cfg_p = (void*)(uintptr_t)cfg;
        if (!IsBadReadPtr(cfg_p, sizeof(uint32_t))) {
            uint32_t *arr = (uint32_t*)cfg_p;
            void *e0 = (void*)(uintptr_t)arr[0];
            if (e0 && !IsBadReadPtr(e0, sizeof(glx_fbconfig_obj_t))) {
                glx_fbconfig_obj_t *f = (glx_fbconfig_obj_t*)e0;
                if (f->tag.magic == GLX_EMU_MAGIC) fbcfg = f;
            }
        }
    }
    glx_pbuffer_obj_t *pb = glx_create_pbuffer(fbcfg, 1024, 1024);
    if (!pb) return 0;
    loader_log(LOG_WARN, "[GLX] glXCreatePbuffer 1024x1024 -> 0x%p", pb);
    return P32(pb);
}

/* Forward declarations for SGIX helpers */
uint32_t shim_glXChooseFBConfig(uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t shim_glXGetFBConfigAttrib(uint32_t, uint32_t, int, uint32_t);
uint32_t shim_glXCreateNewContext(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

/* SGIX pbuffer variants - same as regular but with SGIX suffix */
uint32_t shim_glXCreateGLXPbufferSGIX(uint32_t dpy, uint32_t cfg, uint32_t w, uint32_t h, uint32_t attrib) {
    loader_log(LOG_WARN, "[GLX] glXCreateGLXPbufferSGIX %dx%d", w, h);
    GLX_ENSURE();
    glx_fbconfig_obj_t *fbcfg = NULL;
    if (cfg) {
        void *p = (void*)(uintptr_t)cfg;
        if (!IsBadReadPtr(p, sizeof(glx_fbconfig_obj_t))) {
            glx_fbconfig_obj_t *f = (glx_fbconfig_obj_t*)p;
            if (f->tag.magic == GLX_EMU_MAGIC) fbcfg = f;
        }
    }
    glx_pbuffer_obj_t *pb = glx_create_pbuffer(fbcfg, (int)w, (int)h);
    loader_log(LOG_WARN, "[GLX] glXCreateGLXPbufferSGIX -> 0x%p", pb);
    return pb ? P32(pb) : 0;
}
uint32_t shim_glXDestroyGLXPbufferSGIX(uint32_t dpy, uint32_t pbuf) {
    (void)dpy;
    if (!pbuf) return 0;
    void *p = (void*)(uintptr_t)pbuf;
    if (IsBadReadPtr(p, sizeof(glx_pbuffer_obj_t))) return 0;
    glx_pbuffer_obj_t *pb = (glx_pbuffer_obj_t*)p;
    if (pb->tag.magic == GLX_EMU_MAGIC) glx_destroy_pbuffer(pb);
    return 0;
}
uint32_t shim_glXChooseFBConfigSGIX(uint32_t dpy, uint32_t screen, uint32_t attrib, uint32_t n) {
    loader_log(LOG_WARN, "[GLX] glXChooseFBConfigSGIX");
    return shim_glXChooseFBConfig(dpy, screen, attrib, n);
}
uint32_t shim_glXGetFBConfigAttribSGIX(uint32_t dpy, uint32_t cfg, uint32_t attr, uint32_t val) {
    return shim_glXGetFBConfigAttrib(dpy, cfg, attr, val);
}
uint32_t shim_glXCreateContextWithConfigSGIX(uint32_t dpy, uint32_t cfg, uint32_t type, uint32_t share, uint32_t direct) {
    loader_log(LOG_WARN, "[GLX] glXCreateContextWithConfigSGIX");
    return shim_glXCreateNewContext(dpy, cfg, type, share, direct);
}
uint32_t shim_glXMakeContextCurrentSGI(uint32_t dpy, uint32_t draw, uint32_t read, uint32_t ctx) {
    loader_log(LOG_WARN, "[GLX] glXMakeContextCurrentSGI: draw=0x%x read=0x%x ctx=0x%x", draw, read, ctx);
    return 1;
}

uint32_t shim_glXCreateNewContext(
    uint32_t dpy, uint32_t cfg_ptr, uint32_t type,
    uint32_t share_ptr, uint32_t direct)
{
    loader_log(LOG_WARN, "[GLX] glXCreateNewContext: cfg=0x%x type=%d share=0x%x direct=%d", cfg_ptr, type, share_ptr, direct);
    GLX_ENSURE();
    (void)dpy; (void)type; (void)direct;
    glx_fbconfig_obj_t *cfg = NULL;
    if (cfg_ptr) {
        void *p = (void*)(uintptr_t)cfg_ptr;
        if (!IsBadReadPtr(p, sizeof(glx_fbconfig_obj_t))) {
            glx_fbconfig_obj_t *f = (glx_fbconfig_obj_t*)p;
            if (f->tag.magic == GLX_EMU_MAGIC) cfg = f;
        }
    }
    glx_context_obj_t *share = NULL;
    if (share_ptr) {
        void *p = (void*)(uintptr_t)share_ptr;
        if (!IsBadReadPtr(p, sizeof(glx_context_obj_t))) {
            glx_context_obj_t *s = (glx_context_obj_t*)p;
            if (s->tag.magic == GLX_EMU_MAGIC) share = s;
        }
    }
    glx_context_obj_t *ctx = glx_create_context(cfg, share);
    loader_log(LOG_WARN, "[GLX] glXCreateNewContext -> ctx=%p", ctx);
    return ctx ? P32(ctx) : 0;
}

uint32_t shim_glXDestroyContext(uint32_t dpy, uint32_t ctx_ptr) {
    (void)dpy;
    if (!ctx_ptr) return 0;
    void *p = (void*)(uintptr_t)ctx_ptr;
    if (IsBadReadPtr(p, sizeof(glx_context_obj_t))) return 0;
    glx_context_obj_t *c = (glx_context_obj_t*)p;
    if (c->tag.magic == GLX_EMU_MAGIC) glx_destroy_context(c);
    return 0;
}

uint32_t shim_glXDestroyPbuffer(uint32_t dpy, uint32_t pbuf) {
    (void)dpy;
    if (!pbuf) return 0;
    void *p = (void*)(uintptr_t)pbuf;
    if (IsBadReadPtr(p, sizeof(glx_pbuffer_obj_t))) return 0;
    glx_pbuffer_obj_t *pb = (glx_pbuffer_obj_t*)p;
    if (pb->tag.magic == GLX_EMU_MAGIC) glx_destroy_pbuffer(pb);
    return 0;
}

uint32_t shim_glXGetCurrentContext(void) {
    glx_context_obj_t *ctx = g_glx.current_ctx;
    return ctx ? P32(ctx) : 0;
}
uint32_t shim_glXGetCurrentDisplay(void) {
    return g_glx.display ? P32(g_glx.display) : 1;
}
uint32_t shim_glXGetCurrentDrawable(void) {
    return g_glx.current_draw ? P32(g_glx.current_draw) : 0;
}



uint32_t shim_glXGetFBConfigAttrib(uint32_t dpy, uint32_t cfg_ptr, int attrib, uint32_t val_out) {
    (void)dpy;
    if (!val_out) return 1;
    int *out=(int*)(uintptr_t)val_out;
    glx_fbconfig_obj_t *cfg = NULL;
    if (cfg_ptr) {
        void *p = (void*)(uintptr_t)cfg_ptr;
        if (!IsBadReadPtr(p, sizeof(glx_fbconfig_obj_t))) {
            glx_fbconfig_obj_t *f = (glx_fbconfig_obj_t*)p;
            if (f->tag.magic == GLX_EMU_MAGIC) cfg = f;
        }
    }
    (void)cfg; (void)attrib;
    *out=0; return 0;
}

uint32_t shim_glXGetRefreshRateSGI(uint32_t r) { if(r) *(uint32_t*)(uintptr_t)r=60; return 0; }
/* The video-sync counter the game paces itself on. Exposed so Snapshots can
 * save/restore it: it's loader-side state, and if it keeps advancing while the
 * game's restored memory expects an older value, the game's vsync wait spins. */
uint32_t g_glx_vsync = 0;
uint32_t glx_vsync_get(void)        { return g_glx_vsync; }
void     glx_vsync_set(uint32_t v)  { g_glx_vsync = v; }
uint32_t shim_glXGetVideoSyncSGI(uint32_t c) {
    uint32_t val = (g_glx_vsync++) / 2;
    if(c) *(uint32_t*)(uintptr_t)c = val;
    loader_log(LOG_WARN, "[GLX] glXGetVideoSyncSGI -> %u (call %u)", val, g_glx_vsync);
    return 0;
}

/* Generic no-op for GL/GLX entry points this backend doesn't export. cdecl, so
 * the caller cleans the stack - safe for any GL signature returning void/int/
 * pointer (essentially all of them). Returns 0 (GL_FALSE / 0 / NULL). */
uint32_t gl_unresolved_noop(void) { return 0; }

uint32_t shim_glXGetProcAddress(uint32_t name_ptr) {
    if (!name_ptr) return 0;
    const char *name=(const char*)(uintptr_t)name_ptr;

    /* DIAGNOSTIC: log the CALLER's return address (who drives the proc-enum
     * storm) so we can addr2line the call site that crashes right after the
     * last lookup. Gated by ULTRA_GPA_TRACE. */
    if (getenv("ULTRA_GPA_TRACE")) {
        void *ra = __builtin_return_address(0);
        loader_log(LOG_WARN, "[GPA] glXGetProcAddress(\"%s\") caller_ret=%p esp~%p",
                   name, ra, (void*)&name_ptr);
        /* On the LAST proc-enum entry, walk the guest EBP chain so we can see
         * who calls the enum function (the frame that crashes with /GS). */
        if (!strcmp(name, "glActiveStencilFaceEXT")) {
            uint32_t *bp = (uint32_t*)__builtin_frame_address(0);
            loader_log(LOG_WARN, "[GPA] === EBP chain at last enum (glActiveStencilFaceEXT) ===");
            for (int d = 0; d < 12 && bp; d++) {
                /* validate bp is a readable stack ptr */
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery(bp, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
                uint32_t saved_bp = bp[0];
                uint32_t ret      = bp[1];
                loader_log(LOG_WARN, "[GPA]   frame[%d] ebp=%p saved_ebp=0x%08x ret=0x%08x",
                           d, (void*)bp, saved_bp, ret);
                if (saved_bp <= (uint32_t)(uintptr_t)bp) break;  /* must climb */
                bp = (uint32_t*)(uintptr_t)saved_bp;
            }
        }
    }

    /* Legacy-Cg stack-overflow guard (also covers the case where libCgGL resolves
     * the GL getters through glXGetProcAddress rather than its ELF GOT): hand back
     * our curated __cdecl glGetString so a multi-KB GL_EXTENSIONS / long GL_VERSION
     * string can't overrun Cg's fixed stack buffers. ABC-safe (real string when no
     * Cg runtime is loaded). */
    if (!strcmp(name, "glGetString"))
        return (uint32_t)(uintptr_t)&ul_glGetString;

    extern uint32_t dynlink_find_shim(const char*);
    uint32_t a=dynlink_find_shim(name);
    if (a) return a;
    if (g_glx.wglGetProcAddress) {
        PROC p=g_glx.wglGetProcAddress(name);
        if (p) return (uint32_t)(uintptr_t)gl_cdecl_thunk((void*)p);
    }
    /* Unresolved on this GL backend. Return a no-op stub instead of NULL:
     *
     * Empirically (see LGJ bring-up log 2026-07-01), returning NULL here
     * breaks games that CACHE the returned pointer without a null check -
     * they store it in a struct at +0x0C and later dereference. LGJ hits
     * this at guest EIP 0x083b491f with EAX=0 → immediate ACCESS_VIOLATION.
     *
     * Under noop, LGJ successfully progressed through 677 GL extension
     * loads to cgCreateContext (per ultra-loader-lgj-render.md memory).
     * The Cg profile-picker deals with the fact that "everything is present"
     * by throwing a C++ exception inside chooseProfile - which is now caught
     * by the _Unwind_* stubs added this session. So noop is the working
     * config for LGJ + any Sega Lindbergh title that caches ext ptrs.
     *
     * Trade-off: spec-strict callers (that check the return) will get a
     * benign 0-returning stub instead of NULL. Almost no arcade title
     * does this correctly, so noop is the pragmatic default. */
    loader_log(LOG_DEBUG, "[GLX] glXGetProcAddress(%s) unresolved -> noop (works around game caching NULL)", name);
    return (uint32_t)(uintptr_t)gl_cdecl_thunk((void*)gl_unresolved_noop);
}

static uint32_t shim_glXSwapIntervalSGI(uint32_t i) { (void)i; return 0; }
static uint32_t shim_glXSwapIntervalMESA(uint32_t i){ (void)i; return 0; }



uint32_t shim_glXMakeCurrent(uint32_t dpy, uint32_t draw_ptr, uint32_t ctx_ptr) {
    loader_log(LOG_WARN, "[GLX] glXMakeCurrent: draw=0x%x ctx=0x%x", draw_ptr, ctx_ptr);
    (void)dpy;

    /* GLX-direct path (Hummer, LGJ, HOTD4 - games that call XOpenDisplay and
     * manage their own GL context via glXCreate/MakeCurrent rather than using
     * GLUT).  For these games we need wglMakeCurrent to actually bind the
     * bridge's WGL context on the calling thread.  Without this, the game's
     * render thread never has a current GL context: wglGetCurrentDC() returns
     * NULL in glx_swap_buffers and the frame is dropped.
     *
     * SDL/GLUT path (ABC, GSEVO): g_sdl_active=1, SDL3 already owns the
     * context - keep as no-op to avoid fighting SDL's context management. */
    extern int g_sdl_active;
    if (!g_sdl_active) {
        extern HDC   glx_get_hdc(void);
        extern HGLRC glx_get_hglrc(void);
        HDC   hdc   = glx_get_hdc();
        HGLRC hglrc = glx_get_hglrc();
        if (ctx_ptr == 0) {
            /* glXMakeCurrent(dpy, None, NULL) - release context from thread */
            if (g_glx.wglMakeCurrent) g_glx.wglMakeCurrent(NULL, NULL);
        } else if (hdc && hglrc) {
            typedef BOOL (WINAPI *wglMakeCurrent_t)(HDC, HGLRC);
            HMODULE gl32 = GetModuleHandleA("opengl32.dll");
            wglMakeCurrent_t mc = g_glx.wglMakeCurrent;
            if (!mc && gl32)
                mc = (wglMakeCurrent_t)GetProcAddress(gl32, "wglMakeCurrent");
            if (mc) {
                if (!mc(hdc, hglrc))
                    loader_log(LOG_WARN, "[GLX] glXMakeCurrent: wglMakeCurrent(0x%p,0x%p) failed: %lu",
                               hdc, hglrc, GetLastError());
                else
                    loader_log(LOG_INFO, "[GLX] glXMakeCurrent: bound WGL context on thread (hdc=0x%p)", hdc);
            }
        }
    }
    return 1;
}

uint32_t shim_glXSwapBuffers(uint32_t dpy, uint32_t drawable_ptr) {
    (void)dpy;
    loader_log(LOG_WARN, "[RENDER] glXSwapBuffers called");
    /* Guard against fake XIDs from the bridge lane (e.g. Hummer's fake root
     * window 0xD4000001 planted by stub_XOpenDisplay in g_fake_xscreen.root).
     * A bare deref of ->tag.magic AVs at drawable+0x3C. If the pointer is
     * unmapped OR our magic tag doesn't match, treat it as "swap the main
     * SDL window" - glx_swap_buffers(NULL) handles that path, same policy
     * shim_glXMakeCurrent uses ("SDL3 keep current always"). */
    glx_pbuffer_obj_t *d = NULL;
    if (drawable_ptr) {
        void *p = (void*)(uintptr_t)drawable_ptr;
        if (!IsBadReadPtr(p, sizeof(glx_pbuffer_obj_t))) {
            glx_pbuffer_obj_t *cand = (glx_pbuffer_obj_t*)p;
            if (cand->tag.magic == GLX_EMU_MAGIC) d = cand;
        }
    }
    glx_swap_buffers(d);
    return 0;
}

uint32_t shim_glXChooseVisual(uint32_t dpy, uint32_t screen, uint32_t attr) {
    (void)dpy; (void)screen; (void)attr;
    GLX_ENSURE();
    /* Return a fake Visual. Sized so the GLX_EMU_MAGIC tag sits at +0x3C
     * (matches glx_pbuffer_obj_t layout / GLXPbufferRec 60B + tag). If any
     * driver-side code probes [visual+0x3C] for "GLXE" it will match and
     * treat this as a driver-owned handle to route on, rather than AVing.
     * The extra buffer past +0x40 gives ~192 bytes of readable zero-fill so
     * a naive struct-field walk past [0] doesn't fault. */
    static uint32_t fake_visual[64] = {0};
    fake_visual[0]  = 0x12345678;     /* legacy [0] cookie kept for compat */
    fake_visual[15] = GLX_EMU_MAGIC;  /* +0x3C: driver-side "GLXE" tag */
    return P32(fake_visual);
}

uint32_t shim_glXQueryExtensionsString(uint32_t dpy, uint32_t screen) {
    (void)dpy; (void)screen;
    return P32("GLX_ARB_multisample GLX_SGI_swap_control GLX_MESA_swap_control "
               "GLX_SGI_video_sync GLX_ARB_create_context GLX_EXT_swap_control");
}

extern uint32_t shim_glXMakeCurrent(uint32_t, uint32_t, uint32_t);
extern uint32_t shim_glXSwapBuffers(uint32_t, uint32_t);
void glx_register_sgi_shims(void) {
    extern void dynlink_register_shim(const char*, void*);
    dynlink_register_shim("glXGetRefreshRateSGI",  shim_glXGetRefreshRateSGI);
    dynlink_register_shim("glXGetVideoSyncSGI",    shim_glXGetVideoSyncSGI);
    dynlink_register_shim("glXSwapIntervalSGI",    shim_glXSwapIntervalSGI);
    dynlink_register_shim("glXSwapIntervalMESA",   shim_glXSwapIntervalMESA);
    dynlink_register_shim("glXCreateNewContext",   shim_glXCreateNewContext);
    dynlink_register_shim("glXDestroyContext",     shim_glXDestroyContext);
    dynlink_register_shim("glXCreatePbuffer",      shim_glXCreatePbuffer);
    dynlink_register_shim("glXDestroyPbuffer",     shim_glXDestroyPbuffer);
    dynlink_register_shim("glXMakeCurrent",        shim_glXMakeCurrent);
    dynlink_register_shim("glXSwapBuffers",        shim_glXSwapBuffers);
    dynlink_register_shim("glXGetCurrentContext",  shim_glXGetCurrentContext);
    dynlink_register_shim("glXGetCurrentDisplay",  shim_glXGetCurrentDisplay);
    dynlink_register_shim("glXGetCurrentDrawable", shim_glXGetCurrentDrawable);
    dynlink_register_shim("glXChooseFBConfig",     shim_glXChooseFBConfig);
    dynlink_register_shim("glXCreateGLXPbufferSGIX",        shim_glXCreateGLXPbufferSGIX);
    dynlink_register_shim("glXDestroyGLXPbufferSGIX",       shim_glXDestroyGLXPbufferSGIX);
    dynlink_register_shim("glXChooseFBConfigSGIX",          shim_glXChooseFBConfigSGIX);
    dynlink_register_shim("glXGetFBConfigAttribSGIX",       shim_glXGetFBConfigAttribSGIX);
    dynlink_register_shim("glXCreateContextWithConfigSGIX", shim_glXCreateContextWithConfigSGIX);
    dynlink_register_shim("glXMakeContextCurrentSGI",       shim_glXMakeContextCurrentSGI);
    dynlink_register_shim("glXGetFBConfigAttrib",  shim_glXGetFBConfigAttrib);
    dynlink_register_shim("glXGetProcAddress",     shim_glXGetProcAddress);
    dynlink_register_shim("glXGetProcAddressARB",  shim_glXGetProcAddress);
}
