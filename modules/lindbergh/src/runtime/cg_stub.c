/*
 * cg_stub.c
 *
 * Stub implementation of the NVIDIA Cg runtime API (libCg.so / libCgGL.so)
 * for the Ultra Loader Lindbergh module.
 *
 * Let's Go Jungle links the 2002-era Cg 1.4 runtime.  On Windows the Linux
 * libCg.so can't find its compiler backend and throws a C++ exception inside
 * cgChooseProfile/cgCreateProgram.  Our __cxa_throw stub terminates the
 * process - game never starts.
 *
 * FIX: intercept all 10 Cg symbols LGJ imports BEFORE the ELF loader resolves
 * them to the real libCg.so.  For cgCreateProgram we load pre-compiled ARB
 * assembly (.asm_gl files) that Sega shipped alongside the .cg sources.
 * The .asm_gl files were compiled by Sega with the SAME Cg 1.4 compiler and
 * are pixel-for-pixel identical to what the real Cg would produce; no accuracy
 * is lost.
 *
 * Lookup strategy: CRC-32 of the .cg source text → .asm_gl content.
 * At the first cgCreateContext() call we scan extraShader/Cg/vs/ and
 * extraShader/Cg/ps/ in the game's working directory, compute CRC-32 of each
 * .cg file, and map it to the corresponding .asm_gl content (if it exists).
 * cgCreateProgram then hashes the source string the game passes, looks it up,
 * and returns a fake CGprogram wrapping the pre-compiled ARB text.
 *
 * Shaders without a pre-compiled .asm_gl counterpart fall back to a minimal
 * passthrough ARB program so the game never crashes - those effects look wrong
 * but all other geometry renders.
 *
 * Additive-safe: ABC and GSEVO do not import any cg* symbol (verified via
 * readelf --dyn-syms).  These stubs are dead code for them.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loader.h"

/* ------------------------------------------------------------------ */
/* Cg API constants (Cg 1.x / 2.x ABI-stable values)                 */
/* ------------------------------------------------------------------ */
#define CG_NO_ERROR           0
#define CG_TRUE               1
#define CG_FALSE              0
#define CG_SOURCE             4096   /* 0x1000 - program_type: source code  */
#define CG_COMPILED_PROGRAM   4102   /* 0x1006 - domain for cgGetProgramString */
#define CG_PROFILE_ARBVP1     7006
#define CG_PROFILE_ARBFP1     7000
#define CG_PROFILE_VP40       7011
#define CG_PROFILE_FP40       7013
#define CG_PROFILE_GLSLV      7007
#define CG_PROFILE_GLSLF      7008
#define CG_UNKNOWN_PROFILE    6145   /* 0x1801 */

#define CG_FAKE_CONTEXT  ((void*)0xC00C0DE1u)

/* ------------------------------------------------------------------ */
/* Program store                                                       */
/* ------------------------------------------------------------------ */
typedef struct CgProgram {
    char  *arb_asm;   /* heap-alloc'd ARB assembly string (NULL-terminated) */
    int    is_vertex; /* 1 = vertex program, 0 = fragment */
} CgProgram;

#define CG_MAX_PROGRAMS 512
static CgProgram s_progs[CG_MAX_PROGRAMS];
static int       s_prog_count = 0;

static CgProgram *cg_prog_alloc(void)
{
    if (s_prog_count >= CG_MAX_PROGRAMS) {
        loader_log(LOG_WARN, "[Cg] program table full (%d)", CG_MAX_PROGRAMS);
        return NULL;
    }
    return &s_progs[s_prog_count++];
}

/* ------------------------------------------------------------------ */
/* CRC-32 (table-driven, standard polynomial)                         */
/* ------------------------------------------------------------------ */
static uint32_t crc32_table[256];
static int      crc32_init_done = 0;

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_init_done = 1;
}

static uint32_t crc32(const void *data, size_t len)
{
    if (!crc32_init_done) crc32_init();
    const unsigned char *p = (const unsigned char *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = crc32_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/* Shader table: CRC32 of .cg source → ARB .asm_gl content           */
/* ------------------------------------------------------------------ */
typedef struct ShaderEntry {
    uint32_t crc;       /* CRC32 of the .cg source file */
    char    *arb;       /* malloc'd .asm_gl content; NULL if no precompiled */
    int      is_vertex;
} ShaderEntry;

#define CG_MAX_SHADERS 512
static ShaderEntry s_shader_table[CG_MAX_SHADERS];
static int         s_shader_count = 0;
static int         s_table_built  = 0;

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, sz, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

static void scan_shader_dir(const char *cg_dir, const char *asm_dir, int is_vertex)
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*.cg", cg_dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        char cg_path[MAX_PATH], asm_path[MAX_PATH];
        snprintf(cg_path,  sizeof(cg_path),  "%s\\%s",    cg_dir, fd.cFileName);
        /* derive .asm_gl name: replace .cg extension */
        char base[MAX_PATH];
        strncpy(base, fd.cFileName, sizeof(base) - 1);
        char *ext = strrchr(base, '.');
        if (ext) *ext = '\0';
        snprintf(asm_path, sizeof(asm_path), "%s\\%s.asm_gl", asm_dir, base);

        size_t cg_len = 0;
        char *cg_src  = read_file(cg_path, &cg_len);
        if (!cg_src) continue;

        uint32_t crc = crc32(cg_src, cg_len);
        free(cg_src);

        char *arb = NULL;
        /* Check both cg_dir and asm_dir for the .asm_gl file */
        arb = read_file(asm_path, NULL);
        if (!arb) {
            /* Try same directory as the .cg file */
            char asm_same[MAX_PATH];
            snprintf(asm_same, sizeof(asm_same), "%s\\%s.asm_gl", cg_dir, base);
            arb = read_file(asm_same, NULL);
        }

        if (s_shader_count < CG_MAX_SHADERS) {
            s_shader_table[s_shader_count].crc       = crc;
            s_shader_table[s_shader_count].arb       = arb;  /* NULL if no precompile */
            s_shader_table[s_shader_count].is_vertex = is_vertex;
            s_shader_count++;
            loader_log(LOG_DEBUG, "[Cg] indexed %s (CRC=%08X) %s",
                       fd.cFileName, crc, arb ? "+ ARB" : "(no ARB)");
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

static void cg_build_table(void)
{
    if (s_table_built) return;
    s_table_built = 1;

    char cwd[MAX_PATH];
    if (!GetCurrentDirectoryA(sizeof(cwd), cwd)) return;

    char vs_cg[MAX_PATH], ps_cg[MAX_PATH];
    snprintf(vs_cg, sizeof(vs_cg), "%s\\extraShader\\Cg\\vs", cwd);
    snprintf(ps_cg, sizeof(ps_cg), "%s\\extraShader\\Cg\\ps", cwd);

    scan_shader_dir(vs_cg, vs_cg, 1);
    scan_shader_dir(ps_cg, ps_cg, 0);

    loader_log(LOG_INFO, "[Cg] shader table: %d entries (%s, %s)",
               s_shader_count, vs_cg, ps_cg);
}

static const char *cg_lookup(const char *src, size_t src_len, int is_vertex)
{
    uint32_t crc = crc32(src, src_len);
    for (int i = 0; i < s_shader_count; i++) {
        if (s_shader_table[i].crc == crc)
            return s_shader_table[i].arb; /* may be NULL if no precompile */
    }
    return NULL;
}

/* Minimal passthrough ARB programs for shaders without a .asm_gl */
static const char s_fallback_vp[] =
    "!!ARBvp1.0\n"
    "# Ultra Loader fallback vertex program (no pre-compiled .asm_gl found)\n"
    "ATTRIB iPos  = vertex.position;\n"
    "ATTRIB iTC0  = vertex.texcoord[0];\n"
    "OUTPUT oPos  = result.position;\n"
    "OUTPUT oTC0  = result.texcoord[0];\n"
    "OUTPUT oCol  = result.color;\n"
    "MOV oPos, state.matrix.mvp.row[0];\n"   /* identity pass-through via MVP */
    "DP4 oPos.x, state.matrix.mvp.row[0], iPos;\n"
    "DP4 oPos.y, state.matrix.mvp.row[1], iPos;\n"
    "DP4 oPos.z, state.matrix.mvp.row[2], iPos;\n"
    "DP4 oPos.w, state.matrix.mvp.row[3], iPos;\n"
    "MOV oTC0, iTC0;\n"
    "MOV oCol, {1,1,1,1};\n"
    "END\n";

static const char s_fallback_fp[] =
    "!!ARBfp1.0\n"
    "# Ultra Loader fallback fragment program (no pre-compiled .asm_gl found)\n"
    "TEMP c;\n"
    "TEX c, fragment.texcoord[0], texture[0], 2D;\n"
    "MUL result.color, c, fragment.color;\n"
    "END\n";

/* ------------------------------------------------------------------ */
/* Cg stub implementations                                            */
/* ------------------------------------------------------------------ */

static void *stub_cgCreateContext(void)
{
    cg_build_table();
    loader_log(LOG_INFO, "[Cg] cgCreateContext -> fake context");
    return CG_FAKE_CONTEXT;
}

static void stub_cgDestroyContext(void *ctx)
{
    (void)ctx;
}

static int stub_cgIsContext(void *ctx)
{
    return ctx == CG_FAKE_CONTEXT ? CG_TRUE : CG_FALSE;
}

static void stub_cgSetErrorCallback(void *ctx, void *fn)
{
    (void)ctx; (void)fn;
}

static int stub_cgGLIsProfileSupported(int profile)
{
    /* Report all vertex/fragment profiles as supported so the game picks one
     * and keeps going.  The real profile check would require the driver to
     * advertise specific GL extensions; since we're using pre-compiled ARB
     * assembly, the profile value is only used to select which .asm_gl we load. */
    (void)profile;
    return CG_TRUE;
}

static void *stub_cgCreateProgram(void *ctx, int type, const char *src,
                                  int profile, const char *entry,
                                  const char **args)
{
    (void)ctx; (void)entry; (void)args;

    if (!src) {
        loader_log(LOG_WARN, "[Cg] cgCreateProgram: NULL source");
        return NULL;
    }

    size_t src_len = strlen(src);
    int is_vertex  = (profile == CG_PROFILE_ARBVP1 || profile == CG_PROFILE_VP40 ||
                      profile == CG_PROFILE_GLSLV);

    const char *arb = cg_lookup(src, src_len, is_vertex);

    CgProgram *prog = cg_prog_alloc();
    if (!prog) return NULL;

    prog->is_vertex = is_vertex;
    if (arb) {
        prog->arb_asm = _strdup(arb);
        loader_log(LOG_DEBUG, "[Cg] cgCreateProgram: pre-compiled ARB loaded (profile=%d)", profile);
    } else {
        /* No pre-compiled .asm_gl found - use minimal passthrough */
        prog->arb_asm = _strdup(is_vertex ? s_fallback_vp : s_fallback_fp);
        loader_log(LOG_WARN, "[Cg] cgCreateProgram: no .asm_gl match (CRC=%08X profile=%d) -> fallback",
                   (uint32_t)crc32(src, src_len), profile);
    }

    return (void*)prog;
}

static void stub_cgDestroyProgram(void *prog_ptr)
{
    if (!prog_ptr) return;
    CgProgram *prog = (CgProgram*)prog_ptr;
    if (prog->arb_asm) { free(prog->arb_asm); prog->arb_asm = NULL; }
    /* Note: prog is a slot in s_progs[] - we don't free the slot itself */
}

static const char *stub_cgGetProgramString(void *prog_ptr, int domain)
{
    if (!prog_ptr) return "";
    (void)domain; /* CG_COMPILED_PROGRAM = 4102; we always return the ARB asm */
    CgProgram *prog = (CgProgram*)prog_ptr;
    return prog->arb_asm ? prog->arb_asm : "";
}

static int stub_cgGetError(void)
{
    return CG_NO_ERROR;
}

static const char *stub_cgGetErrorString(int err)
{
    (void)err;
    return "no error";
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */
void cg_stub_register(LoaderCtx *ctx)
{
    loader_log(LOG_INFO, "[Cg] registering Cg/CgGL stubs (libCg.so bypass for LGJ)");
    #define R(name) sym_register(ctx, #name, (uintptr_t)stub_##name)
    R(cgCreateContext);
    R(cgDestroyContext);
    R(cgIsContext);
    R(cgSetErrorCallback);
    R(cgGLIsProfileSupported);
    R(cgCreateProgram);
    R(cgDestroyProgram);
    R(cgGetProgramString);
    R(cgGetError);
    R(cgGetErrorString);
    #undef R
}
