/*
 * stubs_glibc.c
 * Option C: glibc stub layer.
 *
 * Lindbergh games are dynamically linked against glibc 2.x.
 * Instead of loading the real Linux glibc (which doesn't exist on Windows),
 * we register Win32-backed implementations of every libc symbol the game
 * imports. The dynamic linker (dynlink.c) resolves imports against this
 * table before anything else.
 *
 * Coverage targets the Lindbergh glibc ABI circa 2.3-2.5:
 *   - stdio / file I/O
 *   - string functions
 *   - memory allocation
 *   - threading (pthreads -> Win32 threads)
 *   - math
 *   - time
 *   - process / environment
 *   - dl* (dynamic loading stubs)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <setjmp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utime.h>
#include <io.h>
#include "loader.h"
#include "threads.h"
#include "libgl_stub.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
#define STUB_LOG(name) loader_log(LOG_DEBUG, "stub: " #name)

/* ------------------------------------------------------------------ */
/* errno (Linux errno pointer - glibc exposes __errno_location)       */
/* ------------------------------------------------------------------ */
static __thread int g_linux_errno = 0;

static int *stub___errno_location(void)
{
    return &g_linux_errno;
}

/* Linux timespec layout (used by nanosleep / pthread_cond_timedwait). */
struct ul_timespec { long tv_sec; long tv_nsec; };

/* ------------------------------------------------------------------ */
/* Guest heap - the game's malloc/new/STL/strings live in a DEDICATED  */
/* Win32 heap, kept physically separate from the loader AND the GL     */
/* driver (which both use the shared process heap). This isolation is   */
/* what lets Snapshots capture/restore the game's memory safely without */
/* clobbering live driver state, and is the foundation XSTATE needs.    */
/* Loader-internal allocations (pthread objects, thread structs, etc.)  */
/* deliberately STAY on the host CRT heap so they persist across a      */
/* snapshot restore and the guest's pointers to them remain valid.      */
/* guest_free/realloc validate the block so a cross-heap pointer (e.g.  */
/* something a loader stub handed the guest) is freed on the right heap.*/
/* ------------------------------------------------------------------ */
/* OPT-IN (ULTRA_GUEST_HEAP=1). Default OFF -> pure host-CRT passthrough, so the
 * working game is byte-for-byte unchanged. The guest heap is only needed for the
 * Snapshots/XSTATE path, and on its own does NOT make restore safe (a Win32 heap
 * can't be reverted in place - see snapshot.c); a custom arena allocator does.
 * Kept wired + inert so enabling it is a one-flag change when that work lands. */
static int gh_enabled(void) {
    static int e = -1;
    /* Driven by the `snapshots` setting (g_ul_snapshots, set in settings_apply before
     * the guest's first allocation). Default OFF = host-CRT passthrough, working game
     * byte-for-byte unchanged. ULTRA_GUEST_HEAP env overrides. Cached on first guest
     * alloc, which happens after settings_apply has run. */
    if (e < 0) { const char *v = getenv("ULTRA_GUEST_HEAP");
                 extern int g_ul_snapshots; e = v ? atoi(v) : g_ul_snapshots; }
    return e;
}
/* Revertible arena allocator (guest_alloc.c) - its entire state lives in the
 * captured arena, so a Snapshot can roll it back exactly. guest_free/realloc fall
 * back to the host CRT for any pointer NOT from the arena (e.g. memory a loader
 * stub handed the guest before the arena existed / from a non-arena path). */
extern void  *ga_malloc(size_t n);
extern void   ga_free(void *p);
extern void  *ga_realloc(void *p, size_t n);
extern void  *ga_calloc(size_t n, size_t s);
extern int    ga_owns(const void *p);
/* ---- aligned-block tagging (memalign/posix_memalign), see ul_aligned_alloc ----
 * A memalign block carries this header in the word(s) immediately before the
 * returned (aligned) pointer so the ordinary free()/realloc() can recover the
 * real heap base. glibc's free() accepts memalign pointers transparently; this
 * reproduces that. The magic is checked (guarded) on EVERY guest_free/realloc,
 * so any free path (C free, C++ operator delete, __cxa_free_exception, string
 * frees) unwraps an aligned block correctly. Plain blocks never carry the magic,
 * so non-aligned frees (e.g. all of ABC's) are byte-identical. */
#define UL_ALIGN_MAGIC 0x416C4748u  /* 'ALGH' */
typedef struct { uint32_t magic; uint32_t size; void *base; } AlignHdr;

/* Set the first time ul_aligned_alloc() hands out an aligned block. Until then,
 * no aligned block can exist, so guest_free/realloc skip the header probe
 * entirely -> a title that never calls memalign (e.g. ABC) takes a free path
 * byte-identical to before this change (no IsBadReadPtr, no header read). */
static volatile LONG g_any_aligned = 0;

static void *ul_aligned_base(void *ptr)
{
    if (!ptr || !g_any_aligned) return NULL;
    if ((uintptr_t)ptr < sizeof(AlignHdr)) return NULL;
    AlignHdr *h = (AlignHdr*)((uint8_t*)ptr - sizeof(AlignHdr));
    if (IsBadReadPtr(h, sizeof(AlignHdr))) return NULL;
    if (h->magic != UL_ALIGN_MAGIC) return NULL;
    return h->base;
}

/* ---- large-allocation alignment, glibc-faithful (GSEVO mh-heap arena) ---------
 * On real Lindbergh, glibc serves any malloc >= M_MMAP_THRESHOLD (default 128 KiB)
 * via mmap(), so the returned block is PAGE-aligned (4096), hence trivially
 * >=32-byte aligned. Windows msvcrt malloc/calloc only guarantee 16-byte
 * alignment, so a large arena base can land 16-misaligned (e.g. GSEVO's 256 KiB
 * texture-table heap at 0x...1f0).
 *
 * GSEVO's OWN sub-allocator (mh_malloc_internal) requires its arena base to be
 * 32-byte aligned: its block header is 0x20 bytes and the first payload sits at
 * base+0x20, so a 16-misaligned base forces a 0x10 front-align pad whose new
 * 0x20-byte split header overlaps and zeroes the original block's next-pointer
 * -> NULL deref (the EIP=0x080f3818 `mov [edi+0xc],edx`, edi=0 crash). mhCalloc
 * always passes alignment=0x20, so a 32-aligned arena base makes every payload
 * naturally 32-aligned (pad=0) and the corrupting front-split never happens.
 *
 * FIX: mirror glibc - route large guest_malloc/guest_calloc requests through the
 * already-freeable aligned path (ul_aligned_alloc), page-aligned. The aligned
 * block carries the AlignHdr magic that guest_free/guest_realloc already unwrap,
 * so it is freeable by every path. Small requests keep the existing 8/16-byte
 * behavior byte-for-byte.
 *
 * ABC SAFETY: ABC makes NO single allocation this large (its largest heap blocks
 * are well under 128 KiB) - verified the threshold is never crossed on the ABC
 * boot path - so ABC takes the original (non-aligned) branch and stays
 * byte-identical. The aligned path's free-side unwrap only fires on the magic
 * tag, which ABC's plain blocks never carry. */
#define UL_LARGE_ALIGN_THRESHOLD ((size_t)128 * 1024)   /* glibc M_MMAP_THRESHOLD */
#define UL_LARGE_ALIGN_BYTES     ((size_t)4096)         /* page alignment (>= 32) */
extern void *ul_aligned_alloc(size_t align, size_t size);

/* Raw guest allocation WITHOUT the large-block re-alignment (used by guest_malloc's
 * small path and by ul_aligned_alloc's backing alloc - the latter MUST bypass the
 * align path or it would recurse into itself). Identical to the pre-fix
 * guest_malloc body, so the small/ABC path is byte-for-byte unchanged. */
static void *guest_malloc_raw(size_t n) {
    if (!gh_enabled()) return malloc(n ? n : 1);
    void *p = ga_malloc(n ? n : 1);
    return p ? p : malloc(n ? n : 1);          /* arena exhausted -> host fallback */
}

/* env-gated alloc tracer (ULTRA_ALLOC_TRACE=1): log size + base alignment for any
 * allocation >= the trace threshold. Inert/zero-cost unless the env is set, so ABC
 * stays byte-identical. Used to find mh-heap arenas that come back under-32-aligned. */
static int ul_alloc_trace(void) {
    static int e = -1;
    if (e < 0) { const char *v = getenv("ULTRA_ALLOC_TRACE"); e = v ? atoi(v) : 0; }
    return e;
}
static void ul_alloc_trace_log(const char *who, size_t sz, void *p) {
    if (sz < 0x400) return;
    loader_log(LOG_INFO, "[ALLOC] %s sz=0x%zx -> %p (align&0x1f=0x%x %s)",
               who, sz, p, (unsigned)((uintptr_t)p & 0x1f),
               ((uintptr_t)p & 0x1f) ? "UNDER-32" : "ok32");
}

void *guest_malloc(size_t n)  {
    size_t want = n ? n : 1;
    /* glibc-faithful page alignment for large blocks (e.g. GSEVO mh arena). */
    if (want >= UL_LARGE_ALIGN_THRESHOLD) {
        void *ap = ul_aligned_alloc(UL_LARGE_ALIGN_BYTES, want);
        if (ap) { if (ul_alloc_trace()) ul_alloc_trace_log("malloc", want, ap); return ap; }
    }
    void *p = guest_malloc_raw(want);
    if (ul_alloc_trace()) ul_alloc_trace_log("malloc", want, p);
    return p;
}
void *guest_calloc(size_t a, size_t b) {
    size_t na = a ? a : 1, nb = b ? b : 1;
    size_t tot = na * nb;
    /* Large calloc -> page-aligned + zeroed (overflow-safe: skip if mul wraps). */
    if (na && tot / na == nb && tot >= UL_LARGE_ALIGN_THRESHOLD) {
        void *ap = ul_aligned_alloc(UL_LARGE_ALIGN_BYTES, tot);
        if (ap) { memset(ap, 0, tot); if (ul_alloc_trace()) ul_alloc_trace_log("calloc", tot, ap); return ap; }
    }
    if (!gh_enabled()) { void *p = calloc(na, nb); if (ul_alloc_trace()) ul_alloc_trace_log("calloc", tot, p); return p; }
    void *p = ga_calloc(na, nb);
    if (!p) p = calloc(na, nb);
    if (ul_alloc_trace()) ul_alloc_trace_log("calloc", tot, p);
    return p;
}
void  guest_free(void *p) {
    if (!p) return;
    { void *b = ul_aligned_base(p); if (b) p = b; } /* unwrap a memalign block */
    if (!gh_enabled()) { free(p); return; }
    if (ga_owns(p)) ga_free(p);
    else free(p);                              /* cross-heap host block */
}
void *guest_realloc(void *p, size_t n) {
    /* An aligned (memalign) block: re-alloc as aligned to preserve both the
     * alignment guarantee and free()-ability, copying min(old,new). */
    if (p) {
        void *base = ul_aligned_base(p);
        if (base) {
            AlignHdr *h = (AlignHdr*)((uint8_t*)p - sizeof(AlignHdr));
            size_t oldsz = h->size;
            if (n == 0) { guest_free(base); return NULL; }
            extern void *ul_aligned_alloc(size_t, size_t);
            void *np = ul_aligned_alloc(16, n);
            if (np) memcpy(np, p, n < oldsz ? n : oldsz);
            guest_free(base);
            return np;
        }
    }
    if (!gh_enabled()) return realloc(p, n);
    if (!p) return guest_malloc(n);
    if (!ga_owns(p)) return realloc(p, n);     /* host block -> host realloc */
    return ga_realloc(p, n);
}

/* ------------------------------------------------------------------ */
/* Memory allocation - guest data goes to the dedicated guest heap.    */
/* ------------------------------------------------------------------ */
/* ---- aligned allocation, glibc-compatible (memalign/posix_memalign) ----------
 * glibc's free() transparently accepts a memalign() pointer, so the game frees
 * an aligned block with the ORDINARY free (LGJ: tea::Platform::raw_malloc ->
 * platform_memalign(0x10,size); tea::Platform::raw_free -> free). The previous
 * implementation used _aligned_malloc, whose blocks are NOT freeable by plain
 * free() -> the CRT heap free-list walk faulted on a poisoned pointer
 * (the LGJ first-alloc/-free crash at ntdll 0x771249b4). The fix: carve aligned
 * blocks out of the SAME heap that guest_free/guest_realloc use (guest_malloc),
 * over-allocating room for alignment + a back-pointer header (AlignHdr, defined
 * up top), and tag each one with a magic word. guest_free/guest_realloc already
 * recognize the magic and recover the real base, so the block is freeable by
 * EVERY free path (C free, C++ operator delete, __cxa_free_exception, string
 * frees). Works for both host-CRT-passthrough and the revertible guest arena.
 *
 * ABC imports NEITHER memalign nor posix_memalign (verified from its .dynsym),
 * so the aligned path is never taken for ABC; the unwrap only fires on the magic
 * tag, which ABC's plain blocks never carry -> byte-identical for ABC. */
void *ul_aligned_alloc(size_t align, size_t size)   /* non-static: guest_realloc refs it */
{
    if (align < sizeof(void*)) align = sizeof(void*);
    /* align must be a power of two; round up if a caller passes something else. */
    if (align & (align - 1)) {
        size_t a = sizeof(void*);
        while (a < align) a <<= 1;
        align = a;
    }
    /* Worst-case slack: (align-1) to round the payload up + the back-pointer
     * header that sits immediately before the returned pointer. */
    size_t slack = align - 1 + sizeof(AlignHdr);
    if (size + slack < size) return NULL;            /* overflow */
    /* Back the aligned block with the RAW (non-realigning) guest allocator: a
     * large `size` here would otherwise re-enter guest_malloc's large-align path
     * and recurse into ul_aligned_alloc forever. guest_malloc_raw is exactly the
     * pre-fix guest_malloc body. */
    uint8_t *raw = (uint8_t*)guest_malloc_raw(size + slack);
    if (!raw) return NULL;
    /* First address >= raw+sizeof(AlignHdr) that is align-aligned. */
    uintptr_t p = (uintptr_t)raw + sizeof(AlignHdr);
    p = (p + (align - 1)) & ~(uintptr_t)(align - 1);
    AlignHdr *h = (AlignHdr*)(p - sizeof(AlignHdr));
    h->magic = UL_ALIGN_MAGIC;
    h->size  = (uint32_t)size;
    h->base  = raw;
    InterlockedExchange(&g_any_aligned, 1);   /* arm the free-path unwrap */
    return (void*)p;
}

/* ---- VirtualAlloc fallback for allocations CRT malloc can't satisfy --------
 * When guest_malloc returns NULL (CRT heap exhausted / fragmented), fall back
 * to VirtualAlloc which draws from the virtual address space directly.
 * Blocks are tracked in a small table so stub_free / guest_free can call
 * VirtualFree instead of CRT free for these special blocks. */
#define UL_VA_TRACK_MAX 256
static struct { void *ptr; } g_va_track[UL_VA_TRACK_MAX];
static CRITICAL_SECTION g_va_cs;
static volatile LONG    g_va_cs_init = 0;

static void ul_va_cs_ensure(void) {
    if (InterlockedCompareExchange(&g_va_cs_init, 1, 0) == 0)
        InitializeCriticalSection(&g_va_cs);
}

static void *ul_va_alloc(size_t n) {
    if (!n) n = 1;
    SIZE_T sz = (SIZE_T)(n + 4095) & ~(SIZE_T)4095;
    void *p = VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) return NULL;
    ul_va_cs_ensure();
    EnterCriticalSection(&g_va_cs);
    for (int i = 0; i < UL_VA_TRACK_MAX; i++) {
        if (!g_va_track[i].ptr) { g_va_track[i].ptr = p; break; }
    }
    LeaveCriticalSection(&g_va_cs);
    return p;
}

/* Returns 1 and frees via VirtualFree if p is a tracked VA block; 0 otherwise. */
int ul_va_free(void *p) {
    if (!p || !g_va_cs_init) return 0;
    EnterCriticalSection(&g_va_cs);
    for (int i = 0; i < UL_VA_TRACK_MAX; i++) {
        if (g_va_track[i].ptr == p) {
            g_va_track[i].ptr = NULL;
            LeaveCriticalSection(&g_va_cs);
            VirtualFree(p, 0, MEM_RELEASE);
            return 1;
        }
    }
    LeaveCriticalSection(&g_va_cs);
    return 0;
}

/* Manual EBP chain walk - works in 32-bit for ELF code with frame pointers.
 * Returns up to `max_depth` return addresses into `out[]`. */
static int ul_ebp_walk(uintptr_t *out, int max_depth)
{
    uintptr_t *fp;
    /* If THIS function has no frame pointer, __builtin_frame_address(0) gives
     * the CALLER's EBP. Either way, walk from there. */
    __asm__ volatile ("mov %%ebp, %0" : "=r"(fp));
    int n = 0;
    while (fp && n < max_depth) {
        uintptr_t next_fp = fp[0];
        uintptr_t ret_addr = fp[1];
        if (!ret_addr) break;
        out[n++] = ret_addr;
        if (next_fp <= (uintptr_t)fp || next_fp < 0x10000 || next_fp > 0xFFFF0000u) break;
        fp = (uintptr_t*)next_fp;
    }
    return n;
}

static void *stub_malloc(size_t n) {
    void *p = guest_malloc(n);
    if (!p && n > 0) {
        /* Log the failure + call stack so we can diagnose size corruption.
         * Return NULL so the ELF's _Znwj throws std::bad_alloc, which the
         * DW2 unwinder (now statically linked from libgcc_eh.a) propagates
         * to the game's catch handler.  DO NOT fall back to VirtualAlloc -
         * a corrupted allocation size (e.g. 0x8413528 = 138 MB) would give
         * the game a valid-looking pointer to the wrong buffer, causing a
         * later ACCESS_VIOLATION instead of a clean caught exception. */
        uintptr_t chain[12] = {0};
        int nc = ul_ebp_walk(chain, 12);
        /* DIAG: check if __xstat GOT slot 0x09C2F56C still points to our stub */
        if (!IsBadReadPtr((void*)0x09C2F56Cu, 4)) {
            uint32_t got_val = *(volatile uint32_t*)0x09C2F56Cu;
            loader_log(LOG_ERROR, "[MALLOC] __xstat GOT@0x09C2F56C = 0x%08x", got_val);
            if (!IsBadReadPtr((void*)(uintptr_t)got_val, 8)) {
                /* Print first 8 bytes of the function the GOT points to */
                uint8_t *code = (uint8_t*)(uintptr_t)got_val;
                loader_log(LOG_ERROR, "[MALLOC] GOT target bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
                           code[0],code[1],code[2],code[3],code[4],code[5],code[6],code[7]);
            }
        }
        /* Also dump [EBP_smb-0x30] which smpBinGetFileSizePKc reads as st_size_lo.
         * If xstat was NOT called, this stack slot may contain garbage from prev use. */
        {
            /* frame[0]=0x080b8bf6 is from compileShader; walk EBP to get compileShader's EBP
             * then compute smpBinGetFileSizePKc's EBP frame. */
            loader_log(LOG_ERROR, "[MALLOC] n=0x%zx looks like ELF stack addr (buf of smpBinGetFileSizePKc)", n);
        }
        loader_log(LOG_ERROR, "[MALLOC] guest_malloc(0x%zx) NULL, EBP chain (%d frames):", n, nc);
        for (int i = 0; i < nc; i++)
            loader_log(LOG_ERROR, "[MALLOC]   [%2d] 0x%08x", i, (unsigned)chain[i]);
        loader_log(LOG_WARN, "[MALLOC] returning NULL sz=0x%zx -> _Znwj will throw bad_alloc (DW2)", n);
    }
    return p;
}
static void *stub_calloc(size_t n, size_t s){ return guest_calloc(n,s); }
static void *stub_realloc(void *p, size_t n) {
    /* If p is a VirtualAlloc fallback block, handle specially. */
    if (p && g_va_cs_init) {
        int is_va = 0;
        EnterCriticalSection(&g_va_cs);
        for (int i = 0; i < UL_VA_TRACK_MAX; i++) {
            if (g_va_track[i].ptr == p) { is_va = 1; break; }
        }
        LeaveCriticalSection(&g_va_cs);
        if (is_va) {
            if (!n) { ul_va_free(p); return NULL; }
            void *np = stub_malloc(n);
            if (np) { MEMORY_BASIC_INFORMATION mbi; VirtualQuery(p, &mbi, sizeof(mbi));
                      size_t oldsz = mbi.RegionSize; memcpy(np, p, n < oldsz ? n : oldsz); }
            ul_va_free(p);
            return np;
        }
    }
    return guest_realloc(p, n);
}
static void  stub_free(void *p)            { if (!ul_va_free(p)) guest_free(p); }
static void *stub_memalign(size_t align, size_t size) {
    return ul_aligned_alloc(align, size);
}
static int   stub_posix_memalign(void **p, size_t align, size_t size) {
    if (!p) return 22; /* EINVAL */
    /* POSIX: align must be a power of two multiple of sizeof(void*). */
    *p = ul_aligned_alloc(align, size);
    return *p ? 0 : 12; /* ENOMEM */
}

/* ------------------------------------------------------------------ */
/* String functions (call CRT versions - same ABI on x64)             */
/* ------------------------------------------------------------------ */
static size_t stub_strlen(const char *s)                     { return strlen(s); }
static char  *stub_strcpy(char *d, const char *s)            { return strcpy(d,s); }
static char  *stub_strncpy(char *d, const char *s, size_t n) { return strncpy(d,s,n); }
static char  *stub_strcat(char *d, const char *s)            { return strcat(d,s); }
static char  *stub_strncat(char *d, const char *s, size_t n) { return strncat(d,s,n); }
static int    stub_strcmp(const char *a, const char *b)      { return strcmp(a,b); }
static int    stub_strncmp(const char *a,const char *b,size_t n){ return strncmp(a,b,n); }
static char  *stub_strchr(const char *s, int c)              { return strchr(s,c); }
static char  *stub_strrchr(const char *s, int c)             { return strrchr(s,c); }
static char  *stub_strstr(const char *h, const char *n)      { return strstr(h,n); }
static char  *stub_strdup(const char *s) {   /* guest owns + frees the copy -> guest heap */
    if (!s) return NULL;
    size_t l = strlen(s);
    char *p = (char*)guest_malloc(l + 1);
    if (p) memcpy(p, s, l + 1);
    return p;
}
static char  *stub_strndup(const char *s, size_t n) {
    size_t l = strnlen(s, n);
    char *p  = (char*)guest_malloc(l + 1);
    if (p) { memcpy(p, s, l); p[l] = 0; }
    return p;
}
static char  *stub_strerror(int e)                           { return strerror(e); }
static int    stub_strcasecmp(const char *a, const char *b)  { return _stricmp(a,b); }
static int    stub_strncasecmp(const char *a,const char *b,size_t n){ return _strnicmp(a,b,n); }
static char  *stub_index(const char *s, int c)               { return strchr(s,c); }
static char  *stub_rindex(const char *s, int c)              { return strrchr(s,c); }

/* ------------------------------------------------------------------ */
/* Memory functions                                                    */
/* ------------------------------------------------------------------ */
static void  *stub_memcpy(void *d, const void *s, size_t n)  { return memcpy(d,s,n); }
static void  *stub_memmove(void *d, const void *s, size_t n) { return memmove(d,s,n); }
static void  *stub_memset(void *d, int c, size_t n)          { return memset(d,c,n); }
static int    stub_memcmp(const void *a, const void *b, size_t n){ return memcmp(a,b,n); }
static void  *stub_memchr(const void *s, int c, size_t n)    { return memchr(s,c,n); }
static void  *stub_bzero_wrapper(void *s, size_t n)          { memset(s,0,n); return NULL; }

/* ------------------------------------------------------------------ */
/* stdio                                                                */
/* ------------------------------------------------------------------ */
static int    stub_printf(const char *fmt, ...)  {
    va_list ap; va_start(ap,fmt); int r=vprintf(fmt,ap); va_end(ap); return r;
}
static int    stub_fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); int r=vfprintf(f,fmt,ap); va_end(ap); return r;
}
static int    stub_sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); int r=vsprintf(buf,fmt,ap); va_end(ap); return r;
}
static int    stub_snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); int r=vsnprintf(buf,n,fmt,ap); va_end(ap); return r;
}
static int    stub_vprintf(const char *fmt, va_list ap)          { return vprintf(fmt,ap); }
static int    stub_vfprintf(FILE *f, const char *fmt, va_list ap){ return vfprintf(f,fmt,ap); }
static int    stub_vsprintf(char *b, const char *fmt, va_list ap){ return vsprintf(b,fmt,ap); }
static int    stub_vsnprintf(char *b,size_t n,const char *fmt,va_list ap){ return vsnprintf(b,n,fmt,ap); }
static int    stub_sscanf(const char *s, const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); int r=vsscanf(s,fmt,ap); va_end(ap); return r;
}
static FILE  *stub_fopen(const char *path, const char *mode)  {
    FILE *f = fopen(path,mode);
    if (getenv("ULTRA_FOPEN_TRACE") && path)
        loader_log(LOG_INFO, "[FOPEN] \"%s\" (%s) -> %s", path, mode?mode:"", f?"OK":"FAIL");
    return f;
}
static FILE  *stub_fopen64(const char *path, const char *mode){
    FILE *f = fopen(path,mode);
    if (getenv("ULTRA_FOPEN_TRACE") && path)
        loader_log(LOG_INFO, "[FOPEN64] \"%s\" (%s) -> %s", path, mode?mode:"", f?"OK":"FAIL");
    return f;
}
static int    stub_fclose(FILE *f)                            { return fclose(f); }
static int    stub_fflush(FILE *f)                            { return fflush(f); }
static size_t stub_fread(void *p, size_t sz, size_t n, FILE *f){ return fread(p,sz,n,f); }
static size_t stub_fwrite(void *p,size_t sz,size_t n, FILE *f) { return fwrite(p,sz,n,f); }
static int    stub_fseek(FILE *f, long off, int w)            { return fseek(f,off,w); }
static int    stub_fseeko(FILE *f, long off, int w)           { return fseek(f,(long)off,w); }
static long   stub_ftell(FILE *f)                             { return ftell(f); }
static int    stub_feof(FILE *f)                              { return feof(f); }
static int    stub_ferror(FILE *f)                            { return ferror(f); }
static char  *stub_fgets(char *s, int n, FILE *f)            { return fgets(s,n,f); }
static int    stub_fputs(const char *s, FILE *f)              { return fputs(s,f); }
static int    stub_fgetc(FILE *f)                             { return fgetc(f); }
static int    stub_fputc(int c, FILE *f)                      { return fputc(c,f); }
static int    stub_putchar(int c)                             { return putchar(c); }
static int    stub_puts(const char *s)                        { return puts(s); }
static int    stub_getchar(void)                              { return getchar(); }

/* stdin / stdout / stderr pointers */
static FILE *stub_stdin_ptr(void)  { return stdin; }
static FILE *stub_stdout_ptr(void) { return stdout; }
static FILE *stub_stderr_ptr(void) { return stderr; }

/* ------------------------------------------------------------------ */
/* Math                                                                 */
/* ------------------------------------------------------------------ */
static double stub_sin(double x)   { return sin(x); }
static double stub_cos(double x)   { return cos(x); }
static double stub_tan(double x)   { return tan(x); }
static double stub_asin(double x)  { return asin(x); }
static double stub_acos(double x)  { return acos(x); }
static double stub_atan(double x)  { return atan(x); }
static double stub_atan2(double y, double x){ return atan2(y,x); }
static double stub_sqrt(double x)  { return sqrt(x); }
static double stub_pow(double b, double e){ return pow(b,e); }
static double stub_exp(double x)   { return exp(x); }
static double stub_log(double x)   { return log(x); }
static double stub_log2(double x)  { return log2(x); }
static double stub_log10(double x) { return log10(x); }
static double stub_floor(double x) { return floor(x); }
static double stub_ceil(double x)  { return ceil(x); }
static double stub_fabs(double x)  { return fabs(x); }
static double stub_fmod(double x, double y){ return fmod(x,y); }
static float  stub_sinf(float x)   { return sinf(x); }
static float  stub_cosf(float x)   { return cosf(x); }
static float  stub_tanf(float x)   { return tanf(x); }
static float  stub_sqrtf(float x)  { return sqrtf(x); }
static float  stub_powf(float b, float e){ return powf(b,e); }
static float  stub_fabsf(float x)  { return fabsf(x); }
static float  stub_floorf(float x) { return floorf(x); }
static float  stub_ceilf(float x)  { return ceilf(x); }
/* Float math variants the game emits but glibc-double-only coverage missed.
 * A gcc-built guest that does float trig/mod (e.g. GSEVO's init_camera_param
 * computes a FOV via atanf) imports the -f symbols, NOT the double ones.
 * Purely additive: a guest that never imports them is unaffected. */
static float  stub_atanf(float x)            { return atanf(x); }
static float  stub_atan2f(float y, float x)  { return atan2f(y, x); }
static float  stub_fmodf(float x, float y)   { return fmodf(x, y); }
static float  stub_log10f(float x)           { return log10f(x); }
static float  stub_log2f(float x)            { return log2f(x); }
static float  stub_hypotf(float x, float y)  { return hypotf(x, y); }

/* ------------------------------------------------------------------ */
/* Time                                                                 */
/* ------------------------------------------------------------------ */
typedef struct { int32_t tv_sec; int32_t tv_nsec; } Linux_Timespec;
typedef struct { int32_t tv_sec; int32_t tv_usec; } Linux_Timeval;

static int stub_gettimeofday(Linux_Timeval *tv, void *tz)
{
    (void)tz;
    extern void vtime_wall(int32_t*, int32_t*);   /* virtual clock (rewindable by Snapshots) */
    int32_t sec = 0, usec = 0; vtime_wall(&sec, &usec);
    tv->tv_sec  = sec;
    tv->tv_usec = usec;
    return 0;
}

static time_t stub_time(time_t *t)
{
    extern void vtime_wall(int32_t*, int32_t*);
    int32_t sec = 0, usec = 0; vtime_wall(&sec, &usec);
    if (t) *t = (time_t)sec;
    return (time_t)sec;
}

/* ------------------------------------------------------------------ */
/* Environment                                                          */
/* ------------------------------------------------------------------ */
static char *stub_getenv(const char *name) { return getenv(name); }
static int   stub_setenv(const char *n, const char *v, int ow) {
    return _putenv_s(n, v);
}
static int   stub_unsetenv(const char *n) { return _putenv_s(n, ""); }

/* ------------------------------------------------------------------ */
/* Process                                                              */
/* ------------------------------------------------------------------ */
/* Write a symbolized exit backtrace to a dedicated file that bypasses any
 * stderr routing the launcher might drop. Cruis'n Blast's launcher currently
 * redirects only stdout, so loader_log() (stderr) never reaches disk - this
 * gives us a guaranteed diagnostic channel to identify WHICH caller VA fired
 * the exit path, so we can NOP the next packer self-check.
 *
 * For every frame we resolve GetModuleHandleEx(frame) + GetModuleFileNameA
 * so we can distinguish a caller inside the packer stub, the fake link_map
 * deref, a Windows API, or a runtime shim. */
static void write_exit_trace(const char *why, int code, void **frames, USHORT n)
{
    CreateDirectoryA("C:\\tmp", NULL);
    CreateDirectoryA("C:\\tmp\\cb_snaps", NULL);
    FILE *tf = fopen("C:\\tmp\\cb_snaps\\cb_exit_trace.txt", "a");
    if (!tf) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(tf, "\n=== [EXIT] %s(code=%d) pid=%lu tid=%lu %04u-%02u-%02u %02u:%02u:%02u.%03u ===\n",
            why, code, (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId(),
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    fprintf(tf, "Backtrace (%u frames):\n", n);
    for (USHORT i = 0; i < n; i++) {
        HMODULE hMod = NULL;
        char base[MAX_PATH] = "?";
        uintptr_t off = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)frames[i], &hMod) && hMod) {
            char path[MAX_PATH];
            if (GetModuleFileNameA(hMod, path, sizeof path)) {
                const char *bn = strrchr(path, '\\');
                bn = bn ? bn + 1 : path;
                strncpy(base, bn, sizeof(base) - 1);
                base[sizeof(base) - 1] = 0;
            }
            off = (uintptr_t)frames[i] - (uintptr_t)hMod;
        } else {
            /* No module - likely inside a mapped ELF segment (packer/guest) */
            uintptr_t va = (uintptr_t)frames[i];
            if (va >= 0x08000000 && va < 0x10000000) {
                snprintf(base, sizeof base, "<guest-elf>");
                off = va;
            } else {
                snprintf(base, sizeof base, "<unmapped>");
                off = va;
            }
        }
        fprintf(tf, "  [%2u] 0x%016llx  %s+0x%llx\n",
                i, (unsigned long long)(uintptr_t)frames[i],
                base, (unsigned long long)off);
    }
    fflush(tf); fclose(tf);
}

static void stub_exit(int code) {
    loader_log(LOG_ERROR, "=== ELF called exit(%d) ===", code);
    /* Print a Windows call stack so we know who called exit */
    void *frames[32]; USHORT n = CaptureStackBackTrace(0, 32, frames, NULL);
    loader_log(LOG_ERROR, "  Backtrace (%u frames):", n);
    for (USHORT i = 0; i < n; i++)
        loader_log(LOG_ERROR, "    [%2u] 0x%016llx", i, (unsigned long long)(uintptr_t)frames[i]);
    /* Dedicated stderr-independent trace so the diagnostic survives even if
     * the launcher drops stderr (as Cruis'n Blast's currently does). */
    write_exit_trace("exit", code, (void**)frames, n);
    fflush(stdout); fflush(stderr);
    /* Final memory snapshot: at this point the packer has fully unpacked
     * the guest ELF into memory. Static extraction saw only ciphertext;
     * this snap captures the plaintext code region. */
    { extern void memsnap_final(const char *reason); memsnap_final("exit"); }
    ExitProcess((UINT)code);
}
static void stub__exit(int code) { stub_exit(code); }
static void stub_abort(void) {
    loader_log(LOG_ERROR, "=== ELF called abort() ===");
    void *frames[32]; USHORT n = CaptureStackBackTrace(0, 32, frames, NULL);
    for (USHORT i = 0; i < n; i++)
        loader_log(LOG_ERROR, "    [%2u] 0x%016llx", i, (unsigned long long)(uintptr_t)frames[i]);
    write_exit_trace("abort", 134, (void**)frames, n);
    fflush(stdout); fflush(stderr);
    { extern void memsnap_final(const char *reason); memsnap_final("abort"); }
    TerminateProcess(GetCurrentProcess(), 134);
}
static int  stub_atexit(void(*fn)(void)) { return atexit(fn); }
static int  stub_atoi(const char *s)  { return atoi(s); }
static long stub_atol(const char *s)  { return atol(s); }
static double stub_atof(const char *s){ return atof(s); }
static long stub_strtol(const char *s, char **e, int b)  { return strtol(s,e,b); }
static unsigned long stub_strtoul(const char *s,char **e,int b){ return strtoul(s,e,b); }
static double stub_strtod(const char *s, char **e)       { return strtod(s,e); }

/* ------------------------------------------------------------------ */
/* Sorting / searching                                                  */
/* ------------------------------------------------------------------ */
static void stub_qsort(void *b, size_t n, size_t sz,
                        int(*cmp)(const void*,const void*)) { qsort(b,n,sz,cmp); }
static void *stub_bsearch(const void *k, const void *b, size_t n, size_t sz,
                           int(*cmp)(const void*,const void*)){ return bsearch(k,b,n,sz,cmp); }

/* ------------------------------------------------------------------ */
/* pthreads -> Win32 threads                                           */
/* ------------------------------------------------------------------ */
typedef void* pthread_t;
typedef void* pthread_mutex_t;
typedef void* pthread_cond_t;
typedef void* pthread_attr_t;
typedef void* pthread_mutexattr_t;
typedef void* pthread_condattr_t;
typedef void* pthread_key_t;

typedef struct {
    void *(*start)(void*);
    void *arg;
} ThreadStart;

static DWORD WINAPI thread_trampoline(LPVOID param)
{
    ThreadStart *ts = (ThreadStart*)param;
    void *(*fn)(void*) = ts->start;
    void *arg = ts->arg;
    free(ts);
    fn(arg);
    return 0;
}

static int stub_pthread_create(pthread_t *tid, const pthread_attr_t *attr,
                                void *(*start)(void*), void *arg)
{
    ThreadStart *ts = malloc(sizeof(ThreadStart));
    ts->start = start; ts->arg = arg;
    HANDLE h = CreateThread(NULL, 0, thread_trampoline, ts, 0, NULL);
    if (!h) { free(ts); return 11; } /* EAGAIN */
    *tid = h;
    return 0;
}

static int stub_pthread_join(pthread_t tid, void **retval)
{
    void *caller = __builtin_return_address(0);
    loader_log(LOG_WARN, "[BLOCK] pthread_join(tid=%p) from ret=%p, waiting (10s timeout)",
               (void*)tid, caller);
    fflush(stdout);
    DWORD r = WaitForSingleObject((HANDLE)tid, 10000);
    if (r == WAIT_TIMEOUT)
        loader_log(LOG_ERROR, "[BLOCK] pthread_join TIMED OUT, thread never finished (ret=%p)", caller);
    if (retval) *retval = NULL;
    CloseHandle((HANDLE)tid);
    return 0;
}

static int stub_pthread_detach(pthread_t tid)
{
    CloseHandle((HANDLE)tid);
    return 0;
}

static pthread_t stub_pthread_self(void)
{
    /* MUST be a UNIQUE, STABLE id per thread. GetCurrentThread() returns the
     * pseudo-handle (HANDLE)-2 which is IDENTICAL for every thread - that broke
     * candy::CriticalSection ownership tracking: candy stores pthread_self() as
     * the lock owner and gates Event::wait on isLockedByThisThread(). With one
     * shared pseudo-handle every worker thread believed it owned every lock, so
     * a worker entered candy::Event::wait and called pthread_cond_timedwait on a
     * CRITICAL_SECTION it did NOT actually hold at the Win32 level ->
     * SleepConditionVariableCS faulted in ntdll (the intermittent worker crash
     * EIP=0x7713FC93). GetCurrentThreadId() is a unique per-thread DWORD, stable
     * for the thread's lifetime - exactly the pthread_self() contract candy
     * needs. ABC is single-threaded through its lock paths, so ABC is unchanged
     * (it never compares two live thread ids). */
    return (pthread_t)(uintptr_t)GetCurrentThreadId();
}

/* ------------------------------------------------------------------ *
 * Atomic lazy init for statically-initialized mutexes/conds.
 * The guest commonly uses PTHREAD_MUTEX_INITIALIZER / PTHREAD_COND_INITIALIZER
 * (all-zero), so *m / *c is NULL until first use. Creating the Win32 object on
 * first touch WITHOUT a guard races: two threads each make a DIFFERENT
 * CRITICAL_SECTION and lose mutual exclusion -> data corruption -> the
 * non-deterministic crashes seen once init goes multi-threaded. One guard lock
 * (initialized single-threaded at stub-registration time) makes lazy creation
 * atomic, so all threads converge on the same object. */
static CRITICAL_SECTION g_lazy_init_lock;
static volatile LONG    g_lazy_init_ready = 0;

void glibc_threads_lazy_init(void)   /* called once from the stub registrar */
{
    if (InterlockedExchange(&g_lazy_init_ready, 1) == 0)
        InitializeCriticalSection(&g_lazy_init_lock);
}

/* ---- guest-address-keyed sync side table -----------------------------------
 * PREVIOUSLY the host CRITICAL_SECTION / CONDITION_VARIABLE pointer was stored
 * INSIDE the guest pthread_mutex_t/pthread_cond_t (in its first word). That
 * breaks when a title (LGJ candy::platform::LockObject) does any of: embeds the
 * mutex in a C++ object later COPIED/MOVED by value (the host pointer is
 * duplicated -> double-free / dangling), or whose recursive-mutex layout puts
 * non-zero __kind bytes where we expected the handle, or zero-inits the
 * containing struct after init. The worker-thread crash (write to ntdll
 * NULL+0x14 inside EnterCriticalSection, reached via candy::Event::wait ->
 * CriticalSection -> LockObject::lock -> pthread_mutex_lock) was exactly this.
 *
 * FIX: key the host object by the GUEST OBJECT ADDRESS in a side table (same
 * model the semaphore code already uses). The guest struct bytes are never
 * touched, so layout/copy/zero-init cannot corrupt the mapping. Equivalent
 * behavior for ABC (one CS per guest mutex address); robust for LGJ. */
#define SYNC_TABLE_SIZE 4096
typedef struct { uint32_t key; void *obj; int recursive; } SyncEnt;
static SyncEnt g_mtx_tab[SYNC_TABLE_SIZE];
static SyncEnt g_cnd_tab[SYNC_TABLE_SIZE];

static void *sync_lookup(SyncEnt *tab, uint32_t key)
{
    uint32_t h = (key >> 3) & (SYNC_TABLE_SIZE - 1);
    for (uint32_t i = 0; i < SYNC_TABLE_SIZE; i++) {
        uint32_t s = (h + i) & (SYNC_TABLE_SIZE - 1);
        if (tab[s].key == key && tab[s].obj) return tab[s].obj;
        if (tab[s].key == 0 && !tab[s].obj)  return NULL; /* empty slot: not present */
    }
    return NULL;
}
static void sync_insert(SyncEnt *tab, uint32_t key, void *obj)
{
    uint32_t h = (key >> 3) & (SYNC_TABLE_SIZE - 1);
    for (uint32_t i = 0; i < SYNC_TABLE_SIZE; i++) {
        uint32_t s = (h + i) & (SYNC_TABLE_SIZE - 1);
        if (tab[s].key == key) { tab[s].obj = obj; return; }  /* replace */
        if (!tab[s].obj) { tab[s].key = key; tab[s].obj = obj; return; }
    }
    /* table full (shouldn't happen): leak gracefully rather than corrupt */
}
static void sync_remove(SyncEnt *tab, uint32_t key)
{
    uint32_t h = (key >> 3) & (SYNC_TABLE_SIZE - 1);
    for (uint32_t i = 0; i < SYNC_TABLE_SIZE; i++) {
        uint32_t s = (h + i) & (SYNC_TABLE_SIZE - 1);
        if (tab[s].key == key && tab[s].obj) { tab[s].obj = NULL; return; }
        if (tab[s].key == 0 && !tab[s].obj) return;
    }
}

/* RACE FIX (candy worker crash, EIP inside RtlSleepConditionVariableCS):
 * the OLD code did a LOCKLESS sync_lookup() fast-path, then re-checked under
 * g_lazy_init_lock. But sync_insert() writes (key, obj) non-atomically AND
 * relocates entries along an open-addressing probe chain; a concurrent
 * lockless reader could (a) hit the `key==0 && !obj` empty-slot terminator
 * mid-relocation and FALSE-MISS an existing entry, then create a SECOND
 * CRITICAL_SECTION/CONDITION_VARIABLE for the same guest key. Two host objects
 * for one guest sync object => lost mutual exclusion => the worker pool walked
 * a half-built condvar wait list and faulted in ntdll. With many candy worker
 * threads spinning up cond/mutex objects at once this fires intermittently
 * (a race, NOT deterministic - it loses to the XF86VidMode main-thread path
 * when timing differs).
 *
 * FIX: serialize ALL table access under g_lazy_init_lock. ensure_mutex runs on
 * every pthread_mutex_lock, but an uncontended CRITICAL_SECTION enter/leave is
 * a couple of atomics (~tens of ns) - correctness over micro-perf. This makes
 * the converge-on-one-object guarantee hold for any number of threads. ABC is
 * single-init-thread for these paths, so behavior there is unchanged. */
static CRITICAL_SECTION *ensure_mutex(pthread_mutex_t *m)
{
    uint32_t key = (uint32_t)(uintptr_t)m;
    EnterCriticalSection(&g_lazy_init_lock);
    CRITICAL_SECTION *cs = (CRITICAL_SECTION*)sync_lookup(g_mtx_tab, key);
    if (!cs) {
        cs = (CRITICAL_SECTION*)malloc(sizeof(CRITICAL_SECTION));
        /* recursive by default - glibc default + candy uses RECURSIVE; a
         * Win32 CRITICAL_SECTION is recursive, which is a safe superset. */
        InitializeCriticalSection(cs);
        sync_insert(g_mtx_tab, key, cs);
    }
    LeaveCriticalSection(&g_lazy_init_lock);
    return cs;
}

static CONDITION_VARIABLE *ensure_cond(pthread_cond_t *c)
{
    uint32_t key = (uint32_t)(uintptr_t)c;
    EnterCriticalSection(&g_lazy_init_lock);
    CONDITION_VARIABLE *cv = (CONDITION_VARIABLE*)sync_lookup(g_cnd_tab, key);
    if (!cv) {
        cv = (CONDITION_VARIABLE*)malloc(sizeof(CONDITION_VARIABLE));
        InitializeConditionVariable(cv);
        sync_insert(g_cnd_tab, key, cv);
    }
    LeaveCriticalSection(&g_lazy_init_lock);
    return cv;
}

static int stub_pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    (void)a;
    /* Drop any stale mapping for this address (object reused), then lazily
     * (re)create on first lock. Zero the guest first word so a title that
     * checks "is initialized" sees a consistent state. */
    uint32_t key = (uint32_t)(uintptr_t)m;
    EnterCriticalSection(&g_lazy_init_lock);
    CRITICAL_SECTION *old = (CRITICAL_SECTION*)sync_lookup(g_mtx_tab, key);
    if (old) { DeleteCriticalSection(old); free(old); sync_remove(g_mtx_tab, key); }
    LeaveCriticalSection(&g_lazy_init_lock);
    ensure_mutex(m);              /* create now so a copy-before-lock still maps */
    return 0;
}

static int stub_pthread_mutex_destroy(pthread_mutex_t *m)
{
    uint32_t key = (uint32_t)(uintptr_t)m;
    EnterCriticalSection(&g_lazy_init_lock);
    CRITICAL_SECTION *cs = (CRITICAL_SECTION*)sync_lookup(g_mtx_tab, key);
    if (cs) { DeleteCriticalSection(cs); free(cs); sync_remove(g_mtx_tab, key); }
    LeaveCriticalSection(&g_lazy_init_lock);
    return 0;
}

static int stub_pthread_mutex_lock(pthread_mutex_t *m)
{
    EnterCriticalSection(ensure_mutex(m));
    return 0;
}

static int stub_pthread_mutex_unlock(pthread_mutex_t *m)
{
    CRITICAL_SECTION *cs = (CRITICAL_SECTION*)sync_lookup(g_mtx_tab, (uint32_t)(uintptr_t)m);
    if (cs) LeaveCriticalSection(cs);
    return 0;
}

static int stub_pthread_mutex_trylock(pthread_mutex_t *m)
{
    return TryEnterCriticalSection(ensure_mutex(m)) ? 0 : 16; /* EBUSY */
}

static int stub_pthread_mutexattr_init(pthread_mutexattr_t *a)    { *a=NULL; return 0; }
static int stub_pthread_mutexattr_destroy(pthread_mutexattr_t *a) { return 0; }
static int stub_pthread_mutexattr_settype(pthread_mutexattr_t *a, int t){ return 0; }

static int stub_pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
    (void)a;
    uint32_t key = (uint32_t)(uintptr_t)c;
    EnterCriticalSection(&g_lazy_init_lock);
    CONDITION_VARIABLE *old = (CONDITION_VARIABLE*)sync_lookup(g_cnd_tab, key);
    if (old) { free(old); sync_remove(g_cnd_tab, key); }  /* CVs need no Delete */
    LeaveCriticalSection(&g_lazy_init_lock);
    ensure_cond(c);
    return 0;
}

static int stub_pthread_cond_destroy(pthread_cond_t *c)
{
    uint32_t key = (uint32_t)(uintptr_t)c;
    EnterCriticalSection(&g_lazy_init_lock);
    CONDITION_VARIABLE *cv = (CONDITION_VARIABLE*)sync_lookup(g_cnd_tab, key);
    if (cv) { free(cv); sync_remove(g_cnd_tab, key); }
    LeaveCriticalSection(&g_lazy_init_lock);
    return 0;
}

/* RECURSIVE-MUTEX + OWNERSHIP-SAFE condvar wait (candy worker-thread crash class).
 *
 * SleepConditionVariableCS has a hard contract that the documented candy crash
 * (EIP in ntdll, inc/dec on a CS with LockCount=0xFFFFFFFA RecursionCount=
 * 0xFFFFFFFF - i.e. an over-released CS) is a symptom of:
 *   (1) the calling thread MUST currently own the CS, and
 *   (2) it releases the CS exactly ONCE (one LockCount decrement) on entry and
 *       re-acquires it ONCE on wake.
 * POSIX pthread_cond_wait on a RECURSIVE mutex (candy uses PTHREAD_MUTEX_
 * RECURSIVE - see candy::platform::LockObject ctor: mutexattr_settype(.,1))
 * must instead release ALL recursion levels and restore them on wake. If a
 * guest enters the recursive mutex N>1 deep then waits, a single-release
 * SleepConditionVariableCS would leave the CS still held (recur N-1) so no
 * other thread can take it to signal -> stall; and any guest-side imbalance in
 * that window can drive the CS LockCount negative -> the ntdll fault.
 *
 * FIX (defensive, semantics-correct): if we do not currently own the CS, do a
 * plain timed sleep (never call SleepConditionVariableCS unowned). If we do own
 * it, fully unwind the recursion to depth 1 so SleepConditionVariableCS's single
 * release truly unlocks it, then restore the saved depth after wake. For the
 * common depth-1 case (all observed candy + ABC waits: recur=1) this is
 * byte-for-byte the old behavior - the unwind/restore loops run zero times.
 *
 * Returns: 1 if it actually slept on the condvar (signaled or timed out via the
 * CV), 0 if it could not (didn't own the CS) and fell back to a plain sleep. */
static int cond_wait_cs(CONDITION_VARIABLE *cv, CRITICAL_SECTION *cs, DWORD ms,
                        int *timed_out)
{
    if (timed_out) *timed_out = 0;
    if (!cv || !cs) { if (ms) Sleep(ms > 50 ? 50 : ms); return 0; }

    /* Do we currently own this CS?  RtlIsCriticalSectionLockedByThread isn't in
     * the public headers, so read the documented fields directly: OwningThread
     * holds the owner's thread id (as a HANDLE-typed field) when held. */
    DWORD me = GetCurrentThreadId();
    int owned = (cs->LockCount != -1) &&
                ((DWORD)(uintptr_t)cs->OwningThread == me);
    if (!owned) {
        /* Caller doesn't own the CS - calling SleepConditionVariableCS here is
         * exactly the over-release that faults ntdll. Don't. The guest re-checks
         * its predicate on return, so a bounded plain sleep is safe. */
        loader_log(LOG_DEBUG, "[cond] wait on UNOWNED cs=%p (tid=%lu owner=%lu) "
                   "-> plain sleep", cs, (unsigned long)me,
                   (unsigned long)(uintptr_t)cs->OwningThread);
        Sleep(ms > 50 ? 50 : ms);
        return 0;
    }

    /* Save and unwind recursion to depth 1 (SleepConditionVariableCS releases
     * exactly one level).  RecursionCount is the # of times THIS thread holds
     * it; for a freshly-locked CriticalLock that is 1, so this loop is a no-op. */
    long saved_recur = cs->RecursionCount;
    while (cs->RecursionCount > 1) LeaveCriticalSection(cs);

    BOOL ok = SleepConditionVariableCS(cv, cs, ms);

    /* Restore the extra recursion levels we released (re-enter saved_recur-1
     * times). On wake SleepConditionVariableCS already re-took level 1. */
    for (long i = 1; i < saved_recur; i++) EnterCriticalSection(cs);

    if (!ok) { if (timed_out) *timed_out = 1; }
    return 1;
}

static int stub_pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    /* signal/wait now share the same Win32 object (ensure_cond), and the mutex
     * is consistent (ensure_mutex), so the predicate pattern is race-free. Keep
     * a bounded wait as a belt-and-suspenders net against a guest that signals
     * with no waiter; the caller re-checks its predicate on return. */
    CONDITION_VARIABLE *cv = ensure_cond(c);
    CRITICAL_SECTION   *cs = ensure_mutex(m);
    if (getenv("ULTRA_COND_DBG"))
        loader_log(LOG_INFO, "[cond] wait c=%p m=%p cv=%p cs=%p ownerTid=%lu thisTid=%lu lockCount=%ld recur=%ld",
                   c, m, cv, cs, cs?(unsigned long)(uintptr_t)cs->OwningThread:0,
                   GetCurrentThreadId(), cs?(long)cs->LockCount:0, cs?(long)cs->RecursionCount:0);
    int timed_out = 0;
    if (cond_wait_cs(cv, cs, 10000, &timed_out) && timed_out)
        loader_log(LOG_DEBUG, "[cond] wait woke on 10s timeout (ret=%p)",
                   __builtin_return_address(0));
    return 0;
}

/* pthread_cond_timedwait(c, m, abstime): abstime is an absolute CLOCK_REALTIME
 * timespec {tv_sec, tv_nsec}. Convert to a relative ms wait. Returns 0 on
 * signal, ETIMEDOUT(110) on timeout - the caller re-checks its predicate. */
static int stub_pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                                       const struct ul_timespec *abstime)
{
    DWORD ms = 10000;
    if (abstime) {
        /* now (realtime, seconds) from the host clock */
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime);
        /* FILETIME = 100ns since 1601; Unix epoch offset = 116444736000000000 */
        ULONGLONG now_ns = (t - 116444736000000000ULL) * 100ULL;
        long long want_ns = (long long)abstime->tv_sec * 1000000000LL + abstime->tv_nsec;
        long long rel_ns  = want_ns - (long long)now_ns;
        if (rel_ns <= 0) ms = 0;
        else { long long m2 = rel_ns / 1000000LL; ms = (m2 > 0x7fffffffLL) ? 0x7fffffff : (DWORD)m2; }
    }
    int timed_out = 0;
    cond_wait_cs(ensure_cond(c), ensure_mutex(m), ms, &timed_out);
    return timed_out ? 110 /* ETIMEDOUT */ : 0;
}

static int stub_pthread_cond_signal(pthread_cond_t *c)
{
    WakeConditionVariable(ensure_cond(c));
    return 0;
}

static int stub_pthread_cond_broadcast(pthread_cond_t *c)
{
    WakeAllConditionVariable(ensure_cond(c));
    return 0;
}

/* Thread-local storage keys */
static DWORD g_tls_keys[256];
static int   g_tls_key_count = 0;

static int stub_pthread_key_create(pthread_key_t *key, void(*dtor)(void*))
{
    if (g_tls_key_count >= 256) return 11;
    DWORD idx = TlsAlloc();
    if (idx == TLS_OUT_OF_INDEXES) return 11;
    g_tls_keys[g_tls_key_count] = idx;
    *key = (pthread_key_t)(uintptr_t)idx;
    return 0;
}

static int stub_pthread_key_delete(pthread_key_t key)
{
    TlsFree((DWORD)(uintptr_t)key);
    return 0;
}

static void *stub_pthread_getspecific(pthread_key_t key)
{
    return TlsGetValue((DWORD)(uintptr_t)key);
}

static int stub_pthread_setspecific(pthread_key_t key, const void *val)
{
    return TlsSetValue((DWORD)(uintptr_t)key, (LPVOID)val) ? 0 : 22;
}

static int stub_pthread_once(void *once, void(*init)(void))
{
    /* Simple spin-once using Interlocked */
    volatile LONG *flag = (volatile LONG*)once;
    if (InterlockedCompareExchange(flag, 1, 0) == 0)
        init();
    return 0;
}

static int stub_pthread_attr_init(pthread_attr_t *a)             { *a=NULL; return 0; }
static int stub_pthread_attr_destroy(pthread_attr_t *a)          { return 0; }
static int stub_pthread_attr_setstacksize(pthread_attr_t *a, size_t s){ return 0; }
static int stub_pthread_attr_setdetachstate(pthread_attr_t *a,int s){ return 0; }
/* Scheduling attrs are advisory on Win32; accept + ignore (the thread still
 * runs at the OS default priority). LGJ's candy::Thread::create configures
 * these before spawning worker threads. */
static int stub_pthread_attr_setschedpolicy(pthread_attr_t *a,int p){ (void)a;(void)p; return 0; }
static int stub_pthread_attr_setschedparam(pthread_attr_t *a, const void *p){ (void)a;(void)p; return 0; }
static int stub_pthread_attr_getschedparam(const pthread_attr_t *a, void *p){ (void)a; if(p) *(int*)p=0; return 0; }
static int stub_pthread_attr_setinheritsched(pthread_attr_t *a,int i){ (void)a;(void)i; return 0; }
static int stub_pthread_attr_setscope(pthread_attr_t *a,int s){ (void)a;(void)s; return 0; }
static int stub_pthread_setschedparam(pthread_t t,int pol,const void *p){ (void)t;(void)pol;(void)p; return 0; }
static int stub_pthread_equal(pthread_t a, pthread_t b){ return a==b; }
/* sched_get_priority_max/min: report a small valid range (Linux SCHED_OTHER is
 * 0/0; SCHED_FIFO/RR are 1..99). create() just clamps into [min,max], so any
 * sane pair works. NULL here was the crash (candy::Thread::create+0xcb). */
static int stub_sched_get_priority_max(int policy){ (void)policy; return 99; }
static int stub_sched_get_priority_min(int policy){ (void)policy; return 1;  }
static int stub_sched_yield(void){ Sleep(0); return 0; }

/* ------------------------------------------------------------------ */
/* semaphore                                                            */
/* ------------------------------------------------------------------ */
/* Linux sem_t lives IN the game's BSS as a raw struct. Rather than store a
 * Win32 HANDLE into that memory (fragile - statically-initialized or
 * pre-wired semaphores then hold garbage), map the guest sem_t ADDRESS to a
 * Win32 handle via a lookup table, honoring the init value. A sem the game
 * initializes to 1 then lets the first wait pass immediately - which is
 * exactly what unblocks the init_graphics frame-sync semaphore. Ported from
 * the SDL3 branch's working semaphore model. */
typedef void* sem_t;
#define SEM_TABLE_SIZE 64
static struct { uint32_t guest_addr; HANDLE handle; int initialized; } g_sem_table[SEM_TABLE_SIZE];
static CRITICAL_SECTION g_sem_table_lock;
static int g_sem_table_ready = 0;

static HANDLE sem_get_or_create(uint32_t guest_addr, unsigned init_val) {
    if (!g_sem_table_ready) { InitializeCriticalSection(&g_sem_table_lock); g_sem_table_ready = 1; }
    EnterCriticalSection(&g_sem_table_lock);
    for (int i = 0; i < SEM_TABLE_SIZE; i++)
        if (g_sem_table[i].initialized && g_sem_table[i].guest_addr == guest_addr) {
            HANDLE h = g_sem_table[i].handle; LeaveCriticalSection(&g_sem_table_lock); return h;
        }
    for (int i = 0; i < SEM_TABLE_SIZE; i++)
        if (!g_sem_table[i].initialized) {
            g_sem_table[i].guest_addr  = guest_addr;
            g_sem_table[i].handle      = CreateSemaphoreA(NULL, (LONG)init_val, 0x7FFFFFFF, NULL);
            g_sem_table[i].initialized = 1;
            HANDLE h = g_sem_table[i].handle;
            LeaveCriticalSection(&g_sem_table_lock);
            loader_log(LOG_WARN, "[SEM] new semaphore for guest 0x%08x init=%u", guest_addr, init_val);
            return h;
        }
    LeaveCriticalSection(&g_sem_table_lock);
    loader_log(LOG_ERROR, "[SEM] table full! addr=0x%08x", guest_addr);
    return NULL;
}

static int stub_sem_init(sem_t *s, int p, unsigned v) {
    (void)p;
    /* Cruis'n Blast (Raw Thrills packer) reads a low offset off the
     * "libc.so.6" dlopen handle and passes the result as the sem_t*.
     * With the zero-mapped handle page, that offset now reads 0, so
     * we see s == NULL. Any bogus low pointer (< 4KB) is equally a
     * poisoned computation from the packer's link_map dereference -
     * treat it as a no-op success instead of crashing sem_get_or_create. */
    if (!s || (uintptr_t)s < 0x1000u) {
        /* Cruis'n Blast packer computes bogus low pointers (e.g. 0xa3c) as
         * offset-off-fake-link_map and expects sem_wait/sem_post on that same
         * pointer to succeed. Old no-op path made wait/post fail -> exit(1).
         * Register a real semaphore keyed by the low address AND pre-signal
         * it (init as always-signaled: cap-1 with 1 available) so the
         * packer's wait immediately proceeds. */
        uint32_t key = (uint32_t)(uintptr_t)s;
        loader_log(LOG_WARN, "[SEM] init on NULL/low sem_t* %p init=%u -> pretend-success + always-signaled", (void*)s, v);
        sem_get_or_create(key, v ? v : 1);
        return 0;
    }
    sem_get_or_create((uint32_t)(uintptr_t)s, v); return 0;
}
static int stub_sem_destroy(sem_t *s) {
    if (!g_sem_table_ready) return 0;
    uint32_t ga = (uint32_t)(uintptr_t)s;
    EnterCriticalSection(&g_sem_table_lock);
    for (int i = 0; i < SEM_TABLE_SIZE; i++)
        if (g_sem_table[i].initialized && g_sem_table[i].guest_addr == ga) {
            CloseHandle(g_sem_table[i].handle); g_sem_table[i].initialized = 0; g_sem_table[i].guest_addr = 0; break;
        }
    LeaveCriticalSection(&g_sem_table_lock);
    return 0;
}
/* Throttle the [SEM] spam: a per-semaphore spin (wait/post in a tight poll loop)
 * floods the log and, worse, keeps the watchdog heartbeat alive so it can never
 * catch the spinning thread. Log only the first few ops per semaphore. */
static volatile long g_sem_log_budget = 6;
static int stub_sem_wait(sem_t *s) {
    /* Lazily create the sem if a wait arrives before init (packer sometimes
     * does this on its low-pointer sems). Init as 1 so first wait passes. */
    HANDLE h = sem_get_or_create((uint32_t)(uintptr_t)s, 1);
    if (!h) return 0;  /* pretend success - never let packer see -1 */
    int log = g_sem_log_budget > 0;
    if (log) { g_sem_log_budget--; loader_log(LOG_WARN, "[SEM] wait 0x%08x ... caller=0x%08x", (uint32_t)(uintptr_t)s, (uint32_t)(uintptr_t)__builtin_return_address(0)); fflush(stdout); }
    DWORD r = WaitForSingleObject(h, 5000); /* 5s timeout - never deadlock */
    if (log) {
        if (r == WAIT_TIMEOUT) loader_log(LOG_WARN, "[SEM] wait TIMEOUT 0x%08x, continuing", (uint32_t)(uintptr_t)s);
        else                   loader_log(LOG_WARN, "[SEM] wait done 0x%08x", (uint32_t)(uintptr_t)s);
    }
    return 0;
}
static int stub_sem_post(sem_t *s) {
    HANDLE h = sem_get_or_create((uint32_t)(uintptr_t)s, 0);
    if (h) {
        ReleaseSemaphore(h, 1, NULL);
        if (g_sem_log_budget > 0) { g_sem_log_budget--; loader_log(LOG_WARN, "[SEM] post 0x%08x", (uint32_t)(uintptr_t)s); }
    }
    return 0;
}
static int stub_sem_trywait(sem_t *s) {
    HANDLE h = sem_get_or_create((uint32_t)(uintptr_t)s, 0);
    return (h && WaitForSingleObject(h, 0) == WAIT_OBJECT_0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* dl* stubs (no actual dynamic loading - everything is pre-resolved)  */
/* ------------------------------------------------------------------ */
static void *stub_dlopen(const char *filename, int flags)
{
    loader_log(LOG_WARN, "dlopen(%s) stubbed", filename ? filename : "(null)");
    /* Return a fake non-NULL handle */
    return (void*)0xDEAD1000;
}

static void *stub_dlsym(void *handle, const char *symbol)
{
    loader_log(LOG_DEBUG, "dlsym(%s)", symbol);
    /* Check our own stub table */
    extern LoaderCtx *g_loader_ctx;
    void *r = NULL;
    if (g_loader_ctx)
        r = (void*)sym_resolve(g_loader_ctx, symbol);
    if (!r) loader_log(LOG_WARN, "dlsym(%s) -> NULL (game may call NULL)",
                      symbol ? symbol : "(null)");
    return r;
}

static int stub_dlclose(void *handle) { return 0; }

static char *stub_dlerror(void) { return "dlopen/dlsym not fully supported"; }

/* Global loader context pointer for dlsym */
LoaderCtx *g_loader_ctx = NULL;

/* ------------------------------------------------------------------ */
/* Misc libc                                                            */
/* ------------------------------------------------------------------ */
static int stub_rand(void)           { return rand(); }
static void stub_srand(unsigned s)   { srand(s); }
static double stub_drand48(void)     { return (double)rand() / RAND_MAX; }
static void stub_srand48(long seed)  { srand((unsigned)seed); }

static void *stub_mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    /* Forward to syscall dispatch */
    extern uintptr_t syscall_dispatch(uint64_t, uint64_t, uint64_t, uint64_t,
                                       uint64_t, uint64_t, uint64_t);
    return (void*)syscall_dispatch(9,
        (uint64_t)addr, (uint64_t)len, (uint64_t)prot,
        (uint64_t)flags, (uint64_t)fd, (uint64_t)off);
}

static int stub_munmap(void *addr, size_t len)
{
    extern uintptr_t syscall_dispatch(uint64_t, uint64_t, uint64_t, uint64_t,
                                       uint64_t, uint64_t, uint64_t);
    return (int)syscall_dispatch(11, (uint64_t)addr, (uint64_t)len, 0,0,0,0);
}

static int stub_mprotect(void *addr, size_t len, int prot)
{
    extern uintptr_t syscall_dispatch(uint64_t, uint64_t, uint64_t, uint64_t,
                                       uint64_t, uint64_t, uint64_t);
    return (int)syscall_dispatch(10, (uint64_t)addr, (uint64_t)len, (uint64_t)prot,0,0,0);
}

static int stub_usleep(unsigned usec) { Sleep(usec / 1000); return 0; }
static unsigned stub_sleep(unsigned s){ Sleep(s * 1000); return 0; }

static int stub_open(const char *path, int flags, ...)
{
    extern uintptr_t syscall_dispatch(uint64_t, uint64_t, uint64_t, uint64_t,
                                       uint64_t, uint64_t, uint64_t);
    return (int)syscall_dispatch(2, (uint64_t)path, (uint64_t)flags, 0,0,0,0);
}

static int stub_close(int fd)
{
    extern uintptr_t syscall_dispatch(uint64_t, uint64_t, uint64_t, uint64_t,
                                       uint64_t, uint64_t, uint64_t);
    return (int)syscall_dispatch(3, (uint64_t)fd, 0,0,0,0,0);
}

static long stub_read(int fd, void *buf, size_t n)
{
    extern uintptr_t syscall_dispatch(uint64_t, uint64_t, uint64_t, uint64_t,
                                       uint64_t, uint64_t, uint64_t);
    return syscall_dispatch(0, (uint64_t)fd, (uint64_t)buf, (uint64_t)n, 0,0,0);
}

static long stub_write(int fd, const void *buf, size_t n)
{
    extern uintptr_t syscall_dispatch(uint64_t, uint64_t, uint64_t, uint64_t,
                                       uint64_t, uint64_t, uint64_t);
    return syscall_dispatch(1, (uint64_t)fd, (uint64_t)buf, (uint64_t)n, 0,0,0);
}

/* ------------------------------------------------------------------ */
/* libc completeness for non-ABC titles (LGJ etc.)                     */
/* ALL additive: ABC's .dynsym imports none of these (verified), so    */
/* registering them cannot change ABC - its GOT slots are unaffected.  */
/* These plug the basic-libc symbols LGJ's main()/init path calls that  */
/* the loader hadn't been providing (getcwd was the first: its unbound  */
/* GOT slot = NULL -> main called 0 -> EIP=0 crash).                    */
/* ------------------------------------------------------------------ */

/* getcwd: glibc returns buf (or NULL on error); buf==NULL allocates a
 * buffer of `size` (or a suitable one). Backed by the Win32 cwd, which is the
 * game's disk0 working dir - the same dir the VFS/open path resolves against. */
static char *stub_getcwd(char *buf, size_t size)
{
    char tmp[1024];
    DWORD n = GetCurrentDirectoryA((DWORD)sizeof(tmp), tmp);
    if (!n || n >= sizeof(tmp)) { g_linux_errno = 34 /*ERANGE*/; return NULL; }
    if (!buf) {                         /* glibc extension: allocate */
        size_t need = (size && size > n + 1) ? size : (size_t)n + 1;
        buf = (char*)guest_malloc(need);
        if (!buf) { g_linux_errno = 12; return NULL; }
        memcpy(buf, tmp, n + 1);
        return buf;
    }
    if (size < (size_t)n + 1) { g_linux_errno = 34; return NULL; }
    memcpy(buf, tmp, n + 1);
    return buf;
}
static char *stub_get_current_dir_name(void) { return stub_getcwd(NULL, 0); }
static int   stub_chdir(const char *path) { return SetCurrentDirectoryA(path) ? 0 : -1; }
static int   stub_getpid(void)  { return (int)GetCurrentProcessId(); }
static int   stub_getppid(void) { return 1; }
static unsigned stub_getuid(void) { return 0; }
static unsigned stub_geteuid(void){ return 0; }
static unsigned stub_getgid(void) { return 0; }
static int   stub_dup(int fd)         { return _dup(fd); }
static int   stub_dup2(int a, int b)  { return _dup2(a, b); }
static FILE *stub_fdopen(int fd, const char *mode) { return _fdopen(fd, mode); }
static int   stub_fileno(FILE *f)     { return _fileno(f); }
static int   stub_raise(int sig)      { (void)sig; return 0; }   /* no-op (we don't deliver signals) */
static int   stub_getpagesize(void)   { return 4096; }

/* __assert_fail(expr, file, line, func): log + abort, glibc semantics. */
static void stub___assert_fail(const char *expr, const char *file,
                               unsigned line, const char *func)
{
    loader_log(LOG_ERROR, "[ASSERT] %s:%u: %s: Assertion `%s' failed.",
               file ? file : "?", line, func ? func : "?", expr ? expr : "?");
    /* Diagnostic: dump the caller chain so the assert site can be addr2line'd.
     * Env-gated (ULTRA_ASSERT_BT) so ABC's run is byte-identical without it. */
    if (getenv("ULTRA_ASSERT_BT")) {
        uintptr_t *bp;
        __asm__ __volatile__("mov %%ebp, %0" : "=r"(bp));
        for (int i = 0; i < 12 && bp; i++) {
            uintptr_t ret = bp[1];
            loader_log(LOG_ERROR, "[ASSERT-BT] [%2d] ebp=0x%08x ret=0x%08x", i, (unsigned)(uintptr_t)bp, (unsigned)ret);
            uintptr_t *nbp = (uintptr_t*)bp[0];
            if (nbp <= bp) break;        /* stop on non-ascending frame */
            bp = nbp;
        }
    }
    fflush(stdout);
    abort();
}

/* nanosleep(req, rem): sleep req->tv_sec/tv_nsec. */
static int stub_nanosleep(const struct ul_timespec *req, struct ul_timespec *rem)
{
    if (req) {
        long s  = req->tv_sec;
        long ns = req->tv_nsec;
        /* Negative tv_nsec means the frame loop's budget underflowed
         * (elapsed > budget).  On real Linux nanosleep returns EINVAL;
         * we return immediately so the game catches up rather than hanging. */
        if (s >= 0 && ns >= 0) {
            /* Cap at 33 ms - a 60 Hz loop sleeps at most ~16 ms per frame;
             * any larger value is a wrapped budget-underflow, not a real sleep. */
            DWORD ms = (DWORD)(s * 1000 + ns / 1000000);
            if (ms > 33) ms = 0;
            if (ms > 0) Sleep(ms);
        }
    }
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

/* realpath(path, resolved): canonicalize. Win32 GetFullPathName is a reasonable
 * analog; the VFS/open path accepts the resulting Windows path. */
static char *stub_realpath(const char *path, char *resolved)
{
    char tmp[1024];
    DWORD n = GetFullPathNameA(path ? path : ".", (DWORD)sizeof(tmp), tmp, NULL);
    if (!n || n >= sizeof(tmp)) return NULL;
    if (!resolved) { resolved = (char*)guest_malloc(n + 1); if (!resolved) return NULL; }
    memcpy(resolved, tmp, n + 1);
    return resolved;
}

/* setlocale / localeconv: minimal C-locale. */
static char *stub_setlocale(int cat, const char *loc) { (void)cat; (void)loc; return (char*)"C"; }
static void *stub_localeconv(void) { return localeconv(); }

/* opendir / readdir / closedir backed by Win32 FindFirstFile. glibc struct
 * dirent: d_ino(8) d_off(8) d_reclen(2) d_type(1) d_name[256]. We fill d_name
 * (the only field games read) and a synthetic d_type. */
typedef struct { HANDLE h; WIN32_FIND_DATAA fd; int first; int valid; } UlDir;
struct ul_dirent64 { uint64_t d_ino; int64_t d_off; unsigned short d_reclen; unsigned char d_type; char d_name[256]; };
static void *stub_opendir(const char *name)
{
    if (!name) return NULL;
    char pat[1024];
    _snprintf(pat, sizeof(pat), "%s\\*", name);
    UlDir *d = (UlDir*)guest_malloc(sizeof(UlDir) + sizeof(struct ul_dirent64));
    if (!d) return NULL;
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { guest_free(d); return NULL; }
    d->first = 1; d->valid = 1;
    return d;
}
static void *stub_readdir(void *dirp)
{
    UlDir *d = (UlDir*)dirp;
    if (!d || !d->valid) return NULL;
    if (!d->first) {
        if (!FindNextFileA(d->h, &d->fd)) { d->valid = 0; return NULL; }
    }
    d->first = 0;
    struct ul_dirent64 *de = (struct ul_dirent64*)((uint8_t*)d + sizeof(UlDir));
    memset(de, 0, sizeof(*de));
    strncpy(de->d_name, d->fd.cFileName, sizeof(de->d_name) - 1);
    de->d_type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 4 /*DT_DIR*/ : 8 /*DT_REG*/;
    de->d_reclen = sizeof(*de);
    return de;
}
static int stub_closedir(void *dirp)
{
    UlDir *d = (UlDir*)dirp;
    if (!d) return -1;
    if (d->h && d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    guest_free(d);
    return 0;
}

/* ---- ctype: glibc __ctype_b_loc / tolower_loc / toupper_loc -----------------
 * glibc's ctype macros index a table at [-128..255] via a TLS-ish pointer-to-
 * pointer. We build static tables covering that full signed range and return a
 * pointer to a pointer into the middle (index 0). The classification bitmask is
 * glibc's _ISbit layout. C++ <locale>/<ctype> and many parsers depend on these. */
enum { UL_ISupper=0x0100, UL_ISlower=0x0200, UL_ISalpha=0x0400, UL_ISdigit=0x0800,
       UL_ISxdigit=0x1000, UL_ISspace=0x2000, UL_ISprint=0x4000, UL_ISgraph=0x8000,
       UL_ISblank=0x0001, UL_IScntrl=0x0002, UL_ISpunct=0x0004, UL_ISalnum=0x0008 };
static unsigned short g_ctype_b[384];        /* [-128..255], index 128 = 0 */
static int            g_ctype_tolower[384];
static int            g_ctype_toupper[384];
/* The glibc-2.3+ FUNCTION form (__ctype_*_loc) returns &<these ptr vars>.
 * The glibc-2.0/2.2 DATA form (__ctype_b / __ctype_tolower / __ctype_toupper)
 * is the SAME pointer variable, exported directly as a data symbol. libCg.so
 * (built vs old glibc) imports the data form via R_386_GLOB_DAT: its GOT slot
 * is filled with the ADDRESS of the variable, then it does
 *   mov eax,[GOT slot]   ; eax = &__ctype_tolower (the variable's address)
 *   mov eax,[eax]        ; eax = the table base (= &g_ctype_*[128])
 *   mov eax,[eax+c*4]    ; eax = table[c], c in [-128..255]
 * (verified by disasm of libCg cgiGetTypeSizes loop @ off 0xb51d4..0xb51fc:
 *  GOT slot 0x20d35c == __ctype_tolower; the +128 pre-offset + int32 stride
 *  match exactly). So we register &g_ctype_*_ptr as the DATA symbol, and
 *  g_ctype_*_ptr must hold &g_ctype_*[128] BEFORE the game dereferences it.
 *  We statically init them to the table base AND call ul_ctype_init() at
 *  stub-registration so the table CONTENTS are populated before guest code. */
static const unsigned short *g_ctype_b_ptr       = &g_ctype_b[128];
static const int            *g_ctype_tolower_ptr = &g_ctype_tolower[128];
static const int            *g_ctype_toupper_ptr = &g_ctype_toupper[128];
static int g_ctype_ready = 0;
static void ul_ctype_init(void)
{
    if (g_ctype_ready) return;
    for (int i = 0; i < 384; i++) {
        int c = i - 128;
        unsigned short f = 0;
        int lo = c, up = c;
        if (c >= 0 && c < 256) {
            if (isupper(c)) { f |= UL_ISupper|UL_ISalpha|UL_ISalnum|UL_ISprint|UL_ISgraph; lo = tolower(c); }
            if (islower(c)) { f |= UL_ISlower|UL_ISalpha|UL_ISalnum|UL_ISprint|UL_ISgraph; up = toupper(c); }
            if (isdigit(c)) f |= UL_ISdigit|UL_ISalnum|UL_ISprint|UL_ISgraph|UL_ISxdigit;
            else if (isxdigit(c)) f |= UL_ISxdigit;
            if (isspace(c))  f |= UL_ISspace;
            if (c == ' ' || c == '\t') f |= UL_ISblank;
            if (iscntrl(c))  f |= UL_IScntrl;
            if (ispunct(c))  f |= UL_ISpunct|UL_ISprint|UL_ISgraph;
            if (isprint(c))  f |= UL_ISprint;
            if (isgraph(c))  f |= UL_ISgraph;
        }
        g_ctype_b[i] = f;
        g_ctype_tolower[i] = lo;
        g_ctype_toupper[i] = up;
    }
    g_ctype_b_ptr       = &g_ctype_b[128];
    g_ctype_tolower_ptr = &g_ctype_tolower[128];
    g_ctype_toupper_ptr = &g_ctype_toupper[128];
    g_ctype_ready = 1;
}
static const unsigned short **stub___ctype_b_loc(void)       { ul_ctype_init(); return &g_ctype_b_ptr; }
static const int            **stub___ctype_tolower_loc(void) { ul_ctype_init(); return &g_ctype_tolower_ptr; }
static const int            **stub___ctype_toupper_loc(void) { ul_ctype_init(); return &g_ctype_toupper_ptr; }
static int stub___ctype_get_mb_cur_max(void) { return 1; }

/* _IO_getc / _IO_putc: glibc's getc/putc out-of-line bodies. */
static int stub__IO_getc(FILE *f) { return fgetc(f); }
static int stub__IO_putc(int c, FILE *f) { return fputc(c, f); }
static int stub__IO_fgetc(FILE *f) { return fgetc(f); }
static int stub__IO_fputc(int c, FILE *f) { return fputc(c, f); }

/* getc / putc / getchar / putchar aliases - glibc frequently exports these
 * as real symbols (not just macros), so Hummer's GOT has slots for them. */
static int stub_getc_alias(FILE *f)       { return fgetc(f); }
static int stub_putc_alias(int c, FILE *f){ return fputc(c, f); }
static int stub_getchar_alias(void)       { return fgetc(stdin); }
static int stub_putchar_alias(int c)      { return fputc(c, stdout); }

/* ---- xstat family --------------------------------------------------- *
 * glibc 2.x exports stat/lstat/fstat as thin inline wrappers that call
 * the versioned symbols __xstat / __lxstat / __fxstat (or the *64 forms).
 * Lindbergh binaries link against these versioned symbols directly, so
 * the runtime must provide them.
 *
 * We synthesize a Linux i386 stat64 structure and fill it from the
 * matching Win32 attributes / MSVCRT fstat call. Only the fields games
 * actually touch (mode, size, mtime, blksize) are meaningful; the rest
 * are zero.
 *
 * Additive: ABC never calls stat*() on its game files. */
/* linux_stat64: byte-for-byte match of Linux i386 SysV ABI struct stat64.
 * Layout (96 bytes):
 *   0x00: st_dev      (8) = uint32_t[2]
 *   0x08: __pad0      (4)
 *   0x0C: __st_ino    (4)
 *   0x10: st_mode     (4)
 *   0x14: st_nlink    (4)
 *   0x18: st_uid      (4)
 *   0x1C: st_gid      (4)
 *   0x20: st_rdev     (8) = uint32_t[2]
 *   0x28: __pad3      (4)
 *   0x2C: st_size     (8) = uint32_t lo + uint32_t hi ← ELF reads lo at 0x2C
 *   0x34: st_blksize  (4)
 *   0x38: st_blocks   (8) = uint32_t[2]
 *   0x40..0x57: timestamps (6×4 = 24)
 *   0x58: st_ino      (8) = uint32_t[2]
 * Total = 96 bytes
 *
 * We avoid int64_t/uint64_t members because MinGW32 gives them 8-byte alignment
 * (Windows ABI), inserting 4 bytes of padding before st_size and inflating
 * sizeof to 104.  Splitting every 64-bit field into two uint32_t fields avoids
 * any alignment padding and keeps sizeof exactly 96, matching the Linux ABI. */
struct linux_stat64 {
    /* 0x00 */ uint32_t st_dev_lo,  st_dev_hi;
    /* 0x08 */ uint8_t  __pad0[4];
    /* 0x0C */ uint32_t __st_ino;
    /* 0x10 */ uint32_t st_mode;
    /* 0x14 */ uint32_t st_nlink;
    /* 0x18 */ uint32_t st_uid;
    /* 0x1C */ uint32_t st_gid;
    /* 0x20 */ uint32_t st_rdev_lo, st_rdev_hi;
    /* 0x28 */ uint8_t  __pad3[4];
    /* 0x2C */ uint32_t st_size_lo, st_size_hi; /* ← ELF reads st_size_lo from [buf+0x2C] */
    /* 0x34 */ uint32_t st_blksize;
    /* 0x38 */ uint32_t st_blocks_lo, st_blocks_hi;
    /* 0x40 */ uint32_t st_atime_,    st_atime_nsec;
    /* 0x48 */ uint32_t st_mtime_,    st_mtime_nsec;
    /* 0x50 */ uint32_t st_ctime_,    st_ctime_nsec;
    /* 0x58 */ uint32_t st_ino_lo,    st_ino_hi;
    /* 0x60: sizeof = 96 */
};

static uint32_t ft_to_unix(const FILETIME *ft)
{
    uint64_t t = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (t < 116444736000000000ULL) return 0;
    return (uint32_t)((t - 116444736000000000ULL) / 10000000ULL);
}

static int stub___xstat64(int ver, const char *path, struct linux_stat64 *buf)
{
    (void)ver;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!path || !buf) { g_linux_errno = 22 /*EINVAL*/; return -1; }
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) {
        g_linux_errno = 2 /*ENOENT*/;
        loader_log(LOG_INFO, "[XSTAT] FAIL path=\"%.120s\"", path ? path : "(null)");
        return -1;
    }
    /* FIX: memset ONLY up to st_ino_hi (offset 0x5C = 92 bytes).
     * smpBinGetFileSizePKc places its statbuf at [EBP-0x5C]; a full 96-byte
     * memset reaches [EBP+3] and zeroes the saved caller EBP, so compileShader
     * runs with EBP=0 after return → [ebp-0x20] reads 0xFFFFFFE0 (kernel space).
     * Stopping at 92 bytes leaves [EBP+0..3] (st_ino_hi slot) untouched. */
    memset(buf, 0, offsetof(struct linux_stat64, st_ino_hi));
    int isdir = !!(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    buf->st_mode  = isdir ? (0040000u | 0755u) : (0100000u | 0644u);
    buf->st_nlink = 1;
    buf->st_size_lo = fa.nFileSizeLow;
    buf->st_size_hi = fa.nFileSizeHigh;
    buf->st_blksize = 4096;
    { uint64_t sz = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
      buf->st_blocks_lo = (uint32_t)((sz + 511) / 512);
      buf->st_blocks_hi = 0; }
    buf->st_atime_  = ft_to_unix(&fa.ftLastAccessTime);
    buf->st_mtime_  = ft_to_unix(&fa.ftLastWriteTime);
    buf->st_ctime_  = ft_to_unix(&fa.ftCreationTime);
    /* Do NOT set st_ino_lo = 1: st_ino_lo is at struct offset 0x58 which maps
     * to [EBP_smb-4] = the saved EBX slot in smpBinGetFileSizePKc's stack frame.
     * Writing 1 there corrupts the caller's EBX after pop %ebx in the epilogue.
     * st_ino_lo is already 0 from the memset above; __st_ino=0 is fine too. */
    buf->__st_ino = 0;
    loader_log(LOG_INFO, "[XSTAT] OK path=\"%.120s\" st_size=0x%08x",
               path ? path : "(null)", fa.nFileSizeLow);
    return 0;
}

static int stub___lxstat64(int ver, const char *path, struct linux_stat64 *buf)
{
    return stub___xstat64(ver, path, buf);   /* NTFS: no symlinks in our model */
}

static int stub___fxstat64(int ver, int fd, struct linux_stat64 *buf)
{
    (void)ver;
    if (!buf) { g_linux_errno = 22 /*EINVAL*/; return -1; }
    struct _stat64 s;
    if (_fstat64(fd, &s) != 0) return -1;
    memset(buf, 0, offsetof(struct linux_stat64, st_ino_hi));   /* same EBP-save fix */
    buf->st_mode    = s.st_mode;
    buf->st_nlink   = s.st_nlink;
    buf->st_size_lo = (uint32_t)(s.st_size & 0xFFFFFFFFu);
    buf->st_size_hi = (uint32_t)((uint64_t)s.st_size >> 32);
    buf->st_blksize = 4096;
    { uint64_t sz = (uint64_t)s.st_size;
      buf->st_blocks_lo = (uint32_t)((sz + 511) / 512);
      buf->st_blocks_hi = 0; }
    buf->st_atime_  = (uint32_t)s.st_atime;
    buf->st_mtime_  = (uint32_t)s.st_mtime;
    buf->st_ctime_  = (uint32_t)s.st_ctime;
    buf->st_ino_lo = buf->__st_ino = (uint32_t)s.st_ino;
    return 0;
}

/* 32-bit non-*64 forms are the same shape here (games treat st_size as
 * int64 via off_t typedef, and none of them differ within this ABI). */
static int stub___xstat(int ver, const char *path, struct linux_stat64 *buf)  { return stub___xstat64(ver, path, buf); }
static int stub___lxstat(int ver, const char *path, struct linux_stat64 *buf) { return stub___lxstat64(ver, path, buf); }
static int stub___fxstat(int ver, int fd, struct linux_stat64 *buf)           { return stub___fxstat64(ver, fd, buf); }

/* ---- termios --------------------------------------------------------
 * Serial-line ioctl surface. Games probe it on stdin/stdout/serial FDs
 * (Cg's shader compiler runtime dumps banners via isatty->tcgetattr).
 * Our world has no TTY; report success with zeroed fields so the game's
 * "not a tty" branches fall through gracefully.
 *
 * Additive: ABC imports none of these. */
struct ul_termios {
    unsigned int c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_cc[32];
    unsigned int c_ispeed, c_ospeed;
};

static int  stub_tcgetattr(int fd, struct ul_termios *t)
{
    (void)fd; if (t) memset(t, 0, sizeof(*t)); return 0;
}
static int  stub_tcsetattr(int fd, int act, const struct ul_termios *t)
{
    (void)fd;(void)act;(void)t; return 0;
}
static void stub_cfmakeraw(struct ul_termios *t)
{
    if (t) memset(t, 0, sizeof(*t));
}
static int  stub_cfsetispeed(struct ul_termios *t, unsigned int s)
{
    if (t) t->c_ispeed = s; return 0;
}
static int  stub_cfsetospeed(struct ul_termios *t, unsigned int s)
{
    if (t) t->c_ospeed = s; return 0;
}
static unsigned int stub_cfgetispeed(const struct ul_termios *t)
{
    return t ? t->c_ispeed : 0;
}
static unsigned int stub_cfgetospeed(const struct ul_termios *t)
{
    return t ? t->c_ospeed : 0;
}
static int  stub_tcflush(int fd, int q) { (void)fd;(void)q; return 0; }
static int  stub_tcdrain(int fd)        { (void)fd; return 0; }
static int  stub_tcflow (int fd, int a) { (void)fd;(void)a; return 0; }
static int  stub_tcsendbreak(int fd, int d) { (void)fd;(void)d; return 0; }

/* ---- wchar / locale (Hummer + LGJ) -----------------------------------------
 * Guest is 32-bit ELF glibc → wchar_t is int32 (NOT the Windows u16 wchar_t
 * we get if we use the host <wchar.h>). We MUST use uint32_t explicitly.
 *
 * Semantics: C locale, ASCII-fastpath. Any code point <= 0x7F behaves per the
 * host <ctype.h>; codepoints > 0x7F pass through unchanged (no classification,
 * no case-folding). That's exactly what the C locale prescribes, and it's what
 * libstdc++'s std::ctype<wchar_t> was built against on stock Debian etch.
 *
 * ABI note: glibc's mbstate_t is 8 bytes on i386. We never inspect it (we're
 * C-locale byte-per-char), so we treat the pointer as opaque.
 *
 * Additive/byte-safe: ABC imports none of these - verified by absence from
 * ABC's .dynsym in the working ABC build. Registering them cannot regress ABC.
 */
typedef uint32_t ul_wc_t;   /* glibc wchar_t = int32 (NOT Windows u16) */

/* memory-block wchar helpers (u32 stride) */
static size_t   stub_wcslen(const ul_wc_t *s) { size_t n=0; while(s[n]) n++; return n; }
static ul_wc_t *stub_wmemcpy(ul_wc_t *d, const ul_wc_t *s, size_t n) {
    for (size_t i=0;i<n;i++) d[i]=s[i]; return d;
}
static ul_wc_t *stub_wmemmove(ul_wc_t *d, const ul_wc_t *s, size_t n) {
    if (d < s) { for (size_t i=0;i<n;i++) d[i]=s[i]; }
    else       { for (size_t i=n;i-- > 0;) d[i]=s[i]; }
    return d;
}
static ul_wc_t *stub_wmemset(ul_wc_t *d, ul_wc_t c, size_t n) {
    for (size_t i=0;i<n;i++) d[i]=c; return d;
}
static ul_wc_t *stub_wmemchr(const ul_wc_t *s, ul_wc_t c, size_t n) {
    for (size_t i=0;i<n;i++) if (s[i]==c) return (ul_wc_t*)&s[i];
    return NULL;
}
static int      stub_wmemcmp(const ul_wc_t *a, const ul_wc_t *b, size_t n) {
    for (size_t i=0;i<n;i++) if (a[i]!=b[i]) return a[i]<b[i] ? -1 : 1;
    return 0;
}

/* byte <-> wchar (C-locale, byte-per-char) */
static ul_wc_t stub_btowc(int c) {
    return (c==EOF || (unsigned)c > 0x7f) ? (ul_wc_t)-1 : (ul_wc_t)(unsigned char)c;
}
static int     stub_wctob(ul_wc_t wc) { return (wc <= 0x7f) ? (int)wc : EOF; }

/* wide case-folding (ASCII-fastpath) */
static ul_wc_t stub_towlower(ul_wc_t wc) {
    return (wc <= 0x7f) ? (ul_wc_t)tolower((int)wc) : wc;
}
static ul_wc_t stub_towupper(ul_wc_t wc) {
    return (wc <= 0x7f) ? (ul_wc_t)toupper((int)wc) : wc;
}

/* multibyte <-> wchar - C locale = 1 byte per char (guest's mbstate is opaque) */
static size_t stub_mbrtowc(ul_wc_t *pwc, const char *s, size_t n, void *ps) {
    (void)ps;
    if (!s || !*s) { if (pwc) *pwc = 0; return 0; }
    if (n == 0) return (size_t)-2;   /* incomplete, per POSIX */
    if (pwc) *pwc = (ul_wc_t)(unsigned char)*s;
    return 1;
}
static size_t stub_wcrtomb(char *s, ul_wc_t wc, void *ps) {
    (void)ps;
    if (!s) return 1;                 /* per POSIX: state-reset query */
    if (wc > 0xff) return (size_t)-1; /* not representable in C locale */
    s[0] = (char)wc;
    return 1;
}

/* locale-aware queries - return "no class matches" / empty output.
 * Only affects code paths that actually classify wide chars; the game's normal
 * fast-path branches (isalpha/isdigit on narrow chars) don't touch these. */
static int    stub_iswctype(ul_wc_t wc, unsigned desc) { (void)wc;(void)desc; return 0; }
static unsigned stub_wctype(const char *prop)          { (void)prop; return 0; }
static size_t stub_wcsftime(ul_wc_t *buf, size_t max, const ul_wc_t *fmt, const void *tm) {
    (void)fmt;(void)tm;
    if (buf && max) buf[0] = 0;
    return 0;
}

/* collation (C locale = compare as codepoints) */
static int stub_wcscoll(const ul_wc_t *a, const ul_wc_t *b) {
    for (size_t i=0;; i++) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        if (a[i] == 0) return 0;
    }
}
/* identity xfrm: strxfrm(dst,src,n) writes at most n chars, returns wcslen(src) */
static size_t stub_wcsxfrm(ul_wc_t *dst, const ul_wc_t *src, size_t n) {
    size_t sl = stub_wcslen(src);
    if (n) {
        size_t copy = sl < n ? sl : n - 1;
        for (size_t i=0;i<copy;i++) dst[i] = src[i];
        if (n > 0) dst[copy] = 0;
    }
    return sl;
}

/* stdio wide getc/putc/ungetc - narrow-byte delegate */
static ul_wc_t stub_getwc(FILE *f) {
    int c = fgetc(f); return c==EOF ? (ul_wc_t)-1 : (ul_wc_t)(unsigned char)c;
}
static ul_wc_t stub_putwc(ul_wc_t wc, FILE *f) {
    return fputc((int)wc & 0xff, f) == EOF ? (ul_wc_t)-1 : wc;
}
static ul_wc_t stub_ungetwc(ul_wc_t wc, FILE *f) {
    return ungetc((int)wc & 0xff, f) == EOF ? (ul_wc_t)-1 : wc;
}

/* math helpers in LGJ's unresolved set */
static int   stub_finite(double x)  { return _finite(x); }
static int   stub_finitef(float x)  { return _finite((double)x); }
static int   stub_isinf(double x)   { return (!_finite(x) && !_isnan(x)) ? (x < 0 ? -1 : 1) : 0; }
static int   stub_isnan(double x)   { return _isnan(x); }
static double stub_sinh(double x)   { return sinh(x); }
static double stub_cosh(double x)   { return cosh(x); }
static double stub_tanh(double x)   { return tanh(x); }
static float  stub_expf(float x)    { return expf(x); }
static double stub_modf(double x, double *ip) { return modf(x, ip); }
static double stub_frexp(double x, int *e)    { return frexp(x, e); }

/* Real ABI argc/argv, published by auxv_build_stack32 before launch.
 * The stub uses these instead of trusting the reconstructed _start call frame,
 * which is shifted by one slot when reached via the null-PLT fault redirect. */
volatile uint32_t g_abi_argc = 0;
volatile uint32_t g_abi_argv = 0;
volatile uint32_t g_abi_stack_hi = 0;  /* aligned esp/ebp for running main() on the ELF stack */
volatile uint32_t g_elf_load_bias = 0; /* slide applied to ELF vaddrs at map time */
volatile int      g_cur_ctor_idx  = -1;   /* index of ctor currently executing (-1 = none) */
volatile uint32_t g_cur_ctor_addr = 0;    /* address of ctor currently executing */

/* Resilient-ctor support: when g_ul_ctor_jmp_set is non-zero, the VEH handler
 * (defined just below) longjmps back through *g_ul_ctor_jmp_ptr on any AV. */
volatile uint32_t g_ul_ctor_jmp_set = 0;
jmp_buf          *g_ul_ctor_jmp_ptr = NULL;

LONG NTAPI ul_ctor_veh_handler(EXCEPTION_POINTERS *ep)
{
    /* Only catch AV (and ILL/STK/DIV) when a ctor is guarded - otherwise leave
     * other handlers (the syscall trap, the crash logger) alone. */
    if (!g_ul_ctor_jmp_set || !g_ul_ctor_jmp_ptr || !ep || !ep->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c != EXCEPTION_ACCESS_VIOLATION &&
        c != EXCEPTION_ILLEGAL_INSTRUCTION &&
        c != EXCEPTION_STACK_OVERFLOW &&
        c != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        c != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    /* Clear the guard flag so a second fault during longjmp doesn't recurse,
     * then longjmp back to the ctor loop. */
    jmp_buf *jb = g_ul_ctor_jmp_ptr;
    g_ul_ctor_jmp_set = 0;
    g_ul_ctor_jmp_ptr = NULL;
    longjmp(*jb, 1);
    return EXCEPTION_CONTINUE_SEARCH; /* unreachable */
}

/* Watchdog: detects a constructor that hangs (never returns). Polls the
 * current-ctor index; if it stays on the same ctor too long, the ctor is
 * stuck - report it and terminate so the process doesn't freeze unkillably. */
DWORD WINAPI ctor_watchdog(LPVOID unused)
{
    (void)unused;
    int last = -2, stable_ms = 0;
    for (;;) {
        Sleep(500);
        int cur = g_cur_ctor_idx;
        if (cur < 0) { last = -2; stable_ms = 0; continue; } /* done / not in ctors */
        if (cur == last) {
            stable_ms += 500;
            if (stable_ms >= 8000) {
                loader_log(LOG_ERROR, "[WATCHDOG] ctor[%d] @0x%08x HUNG (no return in %dms), terminating",
                           cur, (unsigned)g_cur_ctor_addr, stable_ms);
                fflush(stdout);
                TerminateProcess(GetCurrentProcess(), 5);
            }
        } else {
            last = cur; stable_ms = 0;
        }
    }
    return 0;
}

/* Naked trampoline: run main() on the ELF ABI stack with a valid esp AND ebp.
 * cdecl args (read explicitly off our own frame, no compiler register guessing):
 *   [esp+4]=mainfn  [esp+8]=argc  [esp+12]=argv  [esp+16]=abi_esp
 * Returns main()'s eax. Saves/restores all callee-saved regs and the native esp. */
__attribute__((naked, used))
int run_main_on_abi_stack(uint32_t mainfn, uint32_t argc,
                          uint32_t argv, uint32_t abi_esp)
{
    __asm__ __volatile__(
        "push ebp                \n\t"
        "push ebx                \n\t"
        "push esi                \n\t"
        "push edi                \n\t"
        /* read our cdecl args BEFORE switching stacks.
         * after 4 pushes, our return addr is at [esp+16], so:
         *   mainfn = [esp+20], argc = [esp+24], argv = [esp+28], abi_esp = [esp+32] */
        "mov  esi, [esp+20]      \n\t" /* esi = mainfn (callee-saved, survives the call) */
        "mov  ecx, [esp+24]      \n\t" /* ecx = argc */
        "mov  edx, [esp+28]      \n\t" /* edx = argv */
        "mov  eax, [esp+32]      \n\t" /* eax = abi_esp */
        "mov  edi, esp           \n\t" /* edi = saved native esp (callee-saved, survives) */
        "and  eax, -16           \n\t" /* align target esp */
        "mov  esp, eax           \n\t" /* switch to ABI stack */
        "mov  ebp, eax           \n\t" /* give main a valid frame base */
        "push 0                  \n\t" /* envp = NULL */
        "push edx                \n\t" /* argv */
        "push ecx                \n\t" /* argc */
        "call esi                \n\t" /* eax = main(argc, argv, envp) */
        "mov  esp, edi           \n\t" /* restore native esp (eax = main's return preserved) */
        "pop  edi                \n\t"
        "pop  esi                \n\t"
        "pop  ebx                \n\t"
        "pop  ebp                \n\t"
        "ret                     \n\t"
    );
}

/* ------------------------------------------------------------------ */
/* setjmp / longjmp (x86-32 cdecl)                                     */
/* ------------------------------------------------------------------ */
/* libCg.so's shader compiler uses _setjmp/longjmp for error handling
 * (its yacc/lex-style parser longjmps out on a syntax/profile error).
 * Both endpoints go through OUR implementations, so we use a private,
 * UNMANGLED jmp_buf layout (glibc pointer-mangles esp/eip with the TLS
 * stack-guard; we don't, and don't need to, since libCg never inspects
 * the buffer). Layout: env[0..5] = {ebx, esi, edi, ebp, caller_esp, eip}.
 * The buffer libCg passes is >= sizeof(glibc jmp_buf) (156 bytes), so 6
 * dwords fit with room to spare. ABC does not import _setjmp/longjmp
 * (verified: not in ABC .dynsym) - additive/dead for ABC. */
__attribute__((naked, used))
int stub__setjmp(void *env)
{
    __asm__ __volatile__(
        "mov  edx, [esp+4]       \n\t" /* edx = env (jmp_buf) */
        "mov  [edx+0],  ebx      \n\t"
        "mov  [edx+4],  esi      \n\t"
        "mov  [edx+8],  edi      \n\t"
        "mov  [edx+12], ebp      \n\t"
        "lea  ecx, [esp+4]       \n\t" /* caller's esp = our esp + 4 (pop return addr) */
        "mov  [edx+16], ecx      \n\t"
        "mov  ecx, [esp]         \n\t" /* return address = saved eip */
        "mov  [edx+20], ecx      \n\t"
        "xor  eax, eax           \n\t" /* setjmp returns 0 */
        "ret                     \n\t"
    );
}

__attribute__((naked, used))
void stub_longjmp(void *env, int val)
{
    __asm__ __volatile__(
        "mov  edx, [esp+4]       \n\t" /* edx = env */
        "mov  eax, [esp+8]       \n\t" /* eax = val */
        "test eax, eax           \n\t"
        "jnz  1f                 \n\t"
        "mov  eax, 1             \n\t" /* longjmp(env,0) must return 1 */
        "1:                      \n\t"
        "mov  ebx, [edx+0]       \n\t"
        "mov  esi, [edx+4]       \n\t"
        "mov  edi, [edx+8]       \n\t"
        "mov  ebp, [edx+12]      \n\t"
        "mov  esp, [edx+16]      \n\t" /* restore caller's esp (return addr already popped) */
        "mov  ecx, [edx+20]      \n\t" /* saved eip */
        "jmp  ecx                \n\t" /* resume at setjmp's caller; eax = val */
    );
}

/* __register_frame_info / __deregister_frame_info / __cxa_finalize:
 * libCg imports these (DWARF EH frame registration + atexit-style
 * finalizer). We don't run libCg's C++ EH unwinder or destructors in the
 * loader, so safe no-ops. __cxa_finalize(void*) returns void. */
static void stub___register_frame_info(void *begin, void *ob)   { (void)begin; (void)ob; }
static void *stub___deregister_frame_info(void *begin)          { (void)begin; return NULL; }
static void stub___register_frame_info_bases(void *a, void *b, void *c, void *d) { (void)a;(void)b;(void)c;(void)d; }
static void *stub___deregister_frame_info_bases(void *begin)    { (void)begin; return NULL; }
static void stub___cxa_finalize(void *d)                        { (void)d; }

/* ------------------------------------------------------------------ */
/* Generic C++ global-constructor table discovery                      */
/* ------------------------------------------------------------------ */
/* Locate the CURRENTLY-LOADED main ELF's constructor tables from its
 * own ELF metadata (section headers for legacy .ctors, dynamic section
 * for modern .init_array) instead of a hardcoded per-title address.
 *
 * Results are RUNTIME addresses (file vaddr + load_bias) so they can be
 * dereferenced and called directly. The walk policy differs per table:
 *   - .ctors      : array bounded by leading 0xffffffff + trailing 0,
 *                   CALLED IN REVERSE (highest slot first). Returns the
 *                   biased [lo,hi] *data* range (excluding sentinels need
 *                   not be precise; the caller skips 0/-1 per entry).
 *   - .init_array : DT_INIT_ARRAY/DT_INIT_ARRAYSZ, CALLED IN FORWARD order.
 *
 * Returns true if at least one table was found. Any field left 0 means
 * "that table is absent for this title". */
typedef struct {
    uint32_t ctors_lo;     /* biased addr of FIRST word of .ctors (the 0xffffffff sentinel) */
    uint32_t ctors_hi;     /* biased addr of LAST  word of .ctors (the trailing 0 terminator) */
    uint32_t initarr_va;   /* biased addr of .init_array[0], 0 if none */
    uint32_t initarr_cnt;  /* number of .init_array entries */
} CtorTables;

static int find_ctor_tables(CtorTables *out)
{
    memset(out, 0, sizeof(*out));
    extern LoaderCtx *g_loader_ctx;
    if (!g_loader_ctx || g_loader_ctx->image_count < 1) return 0;

    LoadedElf *img = g_loader_ctx->images[0];   /* images[0] = main exe */
    if (!img || !img->is_elf32 || !img->ehdr32 || !img->file_data) return 0;
    uint32_t bias = (uint32_t)img->load_bias;

    /* (A) Legacy .ctors via the section header table (parsed on demand from
     *     file_data, which is retained for the lifetime of the process). */
    Elf32_Ehdr *eh = img->ehdr32;
    if (eh->e_shoff && eh->e_shnum && eh->e_shentsize >= sizeof(Elf32_Shdr) &&
        eh->e_shstrndx < eh->e_shnum) {
        /* Bounds-check the section header table against the file size. */
        uint64_t shtab_end = (uint64_t)eh->e_shoff +
                             (uint64_t)eh->e_shnum * eh->e_shentsize;
        if (shtab_end <= img->file_size) {
            Elf32_Shdr *shstr = (Elf32_Shdr*)(img->file_data + eh->e_shoff +
                                   (uint32_t)eh->e_shstrndx * eh->e_shentsize);
            const char *strtab = (const char*)(img->file_data + shstr->sh_offset);
            uint32_t strtab_sz = shstr->sh_size;
            for (uint32_t i = 0; i < eh->e_shnum; i++) {
                Elf32_Shdr *sh = (Elf32_Shdr*)(img->file_data + eh->e_shoff +
                                     i * eh->e_shentsize);
                if (sh->sh_name >= strtab_sz) continue;
                const char *nm = strtab + sh->sh_name;
                if (strcmp(nm, ".ctors") == 0 && sh->sh_size >= 8 && sh->sh_addr) {
                    out->ctors_lo = (uint32_t)sh->sh_addr + bias;
                    out->ctors_hi = (uint32_t)sh->sh_addr + sh->sh_size - 4 + bias;
                    break;
                }
            }
        }
    }

    /* (B) Modern .init_array via the dynamic section (honored if present). */
    if (img->dynamic) {
        uint32_t arr_va = 0, arr_sz = 0;
        /* dynamic stored as Elf64_Dyn* but for ELF32 the entries are Elf32_Dyn. */
        for (Elf32_Dyn *d = (Elf32_Dyn*)img->dynamic; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_INIT_ARRAY)   arr_va = (uint32_t)d->d_un.d_ptr + bias;
            if (d->d_tag == DT_INIT_ARRAYSZ) arr_sz = (uint32_t)d->d_un.d_val;
        }
        if (arr_va && arr_sz) {
            out->initarr_va  = arr_va;
            out->initarr_cnt = arr_sz / 4;
        }
    }

    return (out->ctors_lo || out->initarr_va) ? 1 : 0;
}

/* Validate that a biased address is committed + readable AND looks like a
 * code pointer into the main image's executable mapping (not GOT/data). A
 * bogus ctor entry (e.g. GOT data misread as a function pointer, which is
 * exactly what crashed LGJ on the hardcoded-ABC path) is rejected so it is
 * SKIPPED rather than called into oblivion. */
static int ctor_target_is_sane(uint32_t target)
{
    if (!target || target == 0xffffffff) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)(uintptr_t)target, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    DWORD p = mbi.Protect & 0xff;
    int readable  = (p == PAGE_READONLY || p == PAGE_READWRITE ||
                     p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READ ||
                     p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY ||
                     p == PAGE_EXECUTE);
    if (!readable) return 0;
    if (mbi.Protect & PAGE_GUARD) return 0;
    /* Must point into the main image's mapped span (vaddr range + bias), not
     * heap/stack/other. This rejects GOT-data-as-pointer like ABC's
     * 0x08344ce8 region misread for LGJ. */
    extern LoaderCtx *g_loader_ctx;
    if (g_loader_ctx && g_loader_ctx->image_count >= 1) {
        LoadedElf *img = g_loader_ctx->images[0];
        if (img) {
            uint32_t bias = (uint32_t)img->load_bias;
            for (int s = 0; s < img->seg_count; s++) {
                LoadedSegment *sg = &img->segs[s];
                uint32_t lo = (uint32_t)sg->vaddr + bias;
                uint32_t hi = lo + (uint32_t)sg->memsz;
                if (target >= lo && target < hi) {
                    /* Constructors live in an executable segment. */
                    if (sg->flags & PF_X) return 1;
                    return 0;
                }
            }
            return 0; /* in-image check available and target not in any seg */
        }
    }
    /* No image info: fall back to the readability check already passed. */
    return 1;
}

/* __libc_start_main: glibc calls this to bootstrap main() */
int stub___libc_start_main(
    int(*main)(int,char**,char**),
    int argc, char **argv,
    void(*init)(void), void(*fini)(void),
    void(*rtld_fini)(void), void *stack_end)
{
    (void)fini; (void)rtld_fini; (void)stack_end; (void)argc; (void)argv;
    {
        HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        const char msg[] = "[LBRGH] __libc_start_main STUB HIT\r\n";
        DWORD w; WriteFile(h, msg, sizeof(msg)-1, &w, NULL);
    }

    /* Use the loader's known-correct argc/argv, NOT the passed-in args:
     * when this stub is entered via the null-PLT fault redirect, the cdecl
     * frame is shifted and `argv` would be garbage (causing main() to
     * dereference a bad pointer on its first instructions). */
    int    real_argc = (int)g_abi_argc;
    char **real_argv = (char**)(uintptr_t)g_abi_argv;
    if (real_argc <= 0) { real_argc = argc; real_argv = argv; } /* fallback */

    loader_log(LOG_INFO, "[LBRGH] __libc_start_main: main=%p argc=%d argv=%p (init=%p)",
               (void*)main, real_argc, (void*)real_argv, (void*)init);
    fflush(stdout);

#ifdef _WIN64
    /* 64-bit: need uc_call32 to re-enter the Unicorn emulator */
    extern uint32_t uc_call32(uint32_t fn_va, int nargs, uint32_t *args);
    if (init) {
        loader_log(LOG_INFO, "__libc_start_main: calling init()");
        uc_call32((uint32_t)(uintptr_t)(void*)init, 0, NULL);
    }
    {
        uint32_t args[3] = { (uint32_t)real_argc, (uint32_t)(uintptr_t)real_argv, 0 };
        uint32_t ret = uc_call32((uint32_t)(uintptr_t)(void*)main, 3, args);
        loader_log(LOG_INFO, "__libc_start_main: main() returned %d", (int)ret);
        return (int)ret;
    }
#else
    /* 32-bit WOW64: init() is __libc_csu_init -> _init -> __do_global_ctors_aux,
     * which walks the .ctors array and runs every C++ global constructor.
     * main() needs these (it dereferences globals a constructor sets up), but
     * calling csu_init as a black box crashes inside one constructor with no
     * way to see which. So we walk .ctors OURSELVES here, logging each
     * constructor address right before calling it through the trampoline (valid
     * ABI-stack frame). The last address logged before a crash = the culprit.
     *
     * .ctors layout (confirmed from the ELF):
     *   0x08344c00 = 0xffffffff terminator
     *   0x08344c04 .. 0x08344cc0 = 48 constructor pointers (low->high)
     *   __do_global_ctors_aux starts at the HIGH end (0x08344cc0) and walks
     *   DOWN, calling each, until it hits the 0xffffffff terminator. */
    extern int run_main_on_abi_stack(uint32_t mainfn, uint32_t argc,
                                     uint32_t argv, uint32_t abi_esp);

    /* ENVIRONMENT-FIRST BOOT MODEL:
     * Skip the entire .ctors walk. The constructors keep dying because they
     * touch subsystems (effects/task/GL) in an order our environment doesn't
     * satisfy. Instead we rely on the fake environment being fully stood up
     * BEFORE the ELF runs (boards, X11, GL/WGL, pthread/TLS, pre-init - all
     * installed in the launch path prior to __libc_start_main), then jump
     * straight to main(). The game's own init_*() sequence (init_glut,
     * init_graphics, init_task, ...) re-initializes what it actually needs at
     * runtime, so skipping static ctors lets us see how far main itself gets
     * on a ready environment. Set SKIP_ALL_CTORS=0 to restore the walk. */
    #define SKIP_ALL_CTORS 0
    if (SKIP_ALL_CTORS) {
        loader_log(LOG_WARN, "__libc_start_main: SKIPPING ALL global ctors (environment-first boot)");
        loader_log(LOG_INFO, "  (fake environment already initialized: boards/X11/GL/pthread/pre-init)");
        fflush(stdout);
    } else {
        uint32_t bias = g_elf_load_bias;

        /* Is the active title ABC? Key the ABC-specific special-casing off the
         * sha anchor, exactly as patcher.c does elsewhere. ABC currently applies
         * NO ctor skip (the old 0x080e2586 skip is disabled), only the watchdog
         * - which is title-agnostic - so for ABC the generic walk must reproduce
         * the identical call sequence. */
        extern char g_elf_sha_hex_buf[65];
        int is_abc = (_strnicmp(g_elf_sha_hex_buf, "0acdfb8fd65a71bd", 16) == 0 ||
                      _strnicmp(g_elf_sha_hex_buf, "39bb5668b0366099", 16) == 0);

        /* GENERIC: discover THIS title's own ctor tables from its ELF metadata
         * (.ctors section + DT_INIT_ARRAY) instead of a literal address. */
        CtorTables ct;
        int have_tables = find_ctor_tables(&ct);

        /* Bias the discovered .ctors range (find_ctor_tables already added the
         * load bias). If the parse failed, fall back to ABC's hardcoded range
         * (correct only for ABC; harmless for others since the per-entry sanity
         * gate rejects garbage). */
        uint32_t CTORS_HI, CTORS_LO; /* HI = last data word, LO = leading sentinel word */
        if (have_tables && ct.ctors_lo) {
            CTORS_HI = ct.ctors_hi;
            CTORS_LO = ct.ctors_lo;
            loader_log(LOG_INFO, "__libc_start_main: .ctors via ELF parse: lo@0x%08x hi@0x%08x%s",
                       CTORS_LO, CTORS_HI, is_abc ? " (ABC)" : "");
        } else {
            CTORS_HI = 0x08344cc0 + bias; /* ABC fallback: highest ctor slot */
            CTORS_LO = 0x08344c00 + bias; /* ABC fallback: 0xffffffff terminator */
            loader_log(LOG_WARN, "__libc_start_main: .ctors ELF parse empty; using ABC fallback range");
        }
        loader_log(LOG_INFO, "__libc_start_main: running global ctors (.ctors walk, bias=0x%08x)", bias);

        /* Watchdog: if any single ctor runs longer than ~8s, it's hung (e.g.
         * waiting on something the game loop would provide). Report which ctor
         * and exit, instead of an unkillable freeze. (Title-agnostic.) */
        {
            extern DWORD WINAPI ctor_watchdog(LPVOID);
            HANDLE wd = CreateThread(NULL, 0, ctor_watchdog, NULL, 0, NULL);
            if (wd) CloseHandle(wd);
        }
        loader_log(LOG_INFO, "  .ctors hi@0x%08x lo@0x%08x", CTORS_HI, CTORS_LO);
        /* Diagnostic: dump the first few raw words at the head so we can see
         * what's actually stored (and whether the pointers are biased). */
        for (int d = 0; d < 4; d++) {
            uint32_t a = CTORS_HI - d*4;
            MEMORY_BASIC_INFORMATION dbi;
            if (VirtualQuery((void*)(uintptr_t)a, &dbi, sizeof(dbi)) && dbi.State == MEM_COMMIT)
                loader_log(LOG_INFO, "    [dump] 0x%08x = 0x%08x", a, *(uint32_t*)(uintptr_t)a);
        }
        fflush(stdout);
        extern volatile int  g_cur_ctor_idx;
        extern volatile uint32_t g_cur_ctor_addr;
        int n = 0, skipped = 0, ctor_faulted = 0;

        /* RESILIENT CTOR mode (ULTRA_LBRGH_RESILIENT_CTORS=1): wrap each ctor
         * in setjmp + VEH so an access-violation in one ctor doesn't kill the
         * boot. Logs the fault, clears the guard, continues with the next.
         * Default OFF -- ABC/GSEVO ctors run fine, this only helps titles like
         * Hummer Extreme that crash in static-init due to a relocation gap. */
        static jmp_buf s_ctor_jmp;
        static volatile int s_ctor_guarding = 0;
        int  resilient = 0;
        { const char *e = getenv("ULTRA_LBRGH_RESILIENT_CTORS");
          resilient = (e && atoi(e)) ? 1 : 0; }
        PVOID veh = NULL;
        if (resilient) {
            /* Install a high-priority VEH that catches AV during a guarded ctor
             * and longjmps back to the loop. */
            extern LONG NTAPI ul_ctor_veh_handler(EXCEPTION_POINTERS *ep);
            veh = AddVectoredExceptionHandler(1, ul_ctor_veh_handler);
            loader_log(LOG_INFO, "  RESILIENT CTOR mode ON (faults will be logged + skipped)");
            fflush(stdout);
        }

        /* .ctors: CALLED IN REVERSE - highest slot first, down to the leading
         * 0xffffffff sentinel. Each entry is sanity-gated so a GOT/data word
         * misread as a pointer (the exact thing that crashed LGJ on the
         * hardcoded-ABC path) is SKIPPED, not called. */
        for (uint32_t slot = CTORS_HI; slot >= CTORS_LO; slot -= 4) {
            uint32_t fn = *(uint32_t*)(uintptr_t)slot;
            if (fn != 0xffffffff && fn != 0) {
                uint32_t target = fn;
                if (!ctor_target_is_sane(target)) {
                    loader_log(LOG_WARN, "  ctor[%d] @slot 0x%08x -> 0x%08x SKIPPED (not a code ptr into image)",
                               n, slot, target);
                    fflush(stdout);
                    skipped++;
                } else {
                    g_cur_ctor_idx  = n;
                    g_cur_ctor_addr = target;
                    loader_log(LOG_INFO, "  ctor[%d] @slot 0x%08x -> 0x%08x  (calling, normal stack)", n, slot, target);
                    fflush(stdout);
                    void (*ctor_fn)(void) = (void(*)(void))(uintptr_t)target;
                    if (resilient) {
                        extern volatile uint32_t g_ul_ctor_jmp_set;
                        extern jmp_buf *g_ul_ctor_jmp_ptr;
                        g_ul_ctor_jmp_ptr = &s_ctor_jmp;
                        if (setjmp(s_ctor_jmp) == 0) {
                            s_ctor_guarding = 1;
                            g_ul_ctor_jmp_set = 1;
                            ctor_fn();
                            s_ctor_guarding = 0;
                            g_ul_ctor_jmp_set = 0;
                            loader_log(LOG_INFO, "  ctor[%d] @0x%08x returned OK", n, target);
                        } else {
                            s_ctor_guarding = 0;
                            g_ul_ctor_jmp_set = 0;
                            ctor_faulted++;
                            loader_log(LOG_WARN, "  ctor[%d] @0x%08x FAULTED, skipped (resilient mode)", n, target);
                        }
                    } else {
                        ctor_fn();
                        loader_log(LOG_INFO, "  ctor[%d] @0x%08x returned OK", n, target);
                    }
                    fflush(stdout);
                    n++;
                }
            }
            if (slot < CTORS_LO + 4) break; /* underflow-safe: stop after the lo word */
        }
        g_cur_ctor_idx = -1;
        if (veh) RemoveVectoredExceptionHandler(veh);
        if (ctor_faulted) {
            loader_log(LOG_WARN, "  RESILIENT CTOR: %d ctor(s) faulted and were skipped, game may be unstable",
                       ctor_faulted);
            fflush(stdout);
        }

        /* .init_array: CALLED IN FORWARD order, if the title provides one.
         * (Neither ABC nor LGJ does - both use legacy .ctors - so this is a
         * no-op for them, but honored for any title that ships one.) */
        if (have_tables && ct.initarr_va && ct.initarr_cnt) {
            loader_log(LOG_INFO, "__libc_start_main: .init_array @0x%08x (%u entries, forward)",
                       ct.initarr_va, ct.initarr_cnt);
            uint32_t *arr = (uint32_t*)(uintptr_t)ct.initarr_va;
            for (uint32_t i = 0; i < ct.initarr_cnt; i++) {
                uint32_t target = arr[i];
                if (!ctor_target_is_sane(target)) {
                    if (target && target != 0xffffffff)
                        loader_log(LOG_WARN, "  init[%u] -> 0x%08x SKIPPED (not a code ptr)", i, target);
                    continue;
                }
                g_cur_ctor_idx  = n;
                g_cur_ctor_addr = target;
                loader_log(LOG_INFO, "  init[%u] -> 0x%08x  (calling)", i, target);
                fflush(stdout);
                ((void(*)(void))(uintptr_t)target)();
                loader_log(LOG_INFO, "  init[%u] @0x%08x returned OK", i, target);
                fflush(stdout);
                n++;
            }
            g_cur_ctor_idx = -1;
        }

        loader_log(LOG_WARN, "__libc_start_main: all %d ctors done (%d skipped, %d faulted)",
                   n, skipped, ctor_faulted);
        /* DIAG: for Hummer, peek at the bin-path buffer set up by ctors.
         * smpBinGetFileSizePKc uses the string at 0x0A566180 as a prefix. */
        extern char g_elf_sha_hex_buf[65];
        if (_strnicmp(g_elf_sha_hex_buf, "3a68e45a2e905e26", 16) == 0) {
            const char *binpath = (const char*)(uintptr_t)0x0A566180u;
            if (!IsBadReadPtr(binpath, 4)) {
                loader_log(LOG_WARN, "[HUMMER] 0x0A566180 = \"%.*s\" (bin path prefix after ctors)",
                           64, binpath);
            } else {
                loader_log(LOG_WARN, "[HUMMER] 0x0A566180 not readable (BSS not mapped?)");
            }
        }
        fflush(stdout);
    }
    loader_log(LOG_INFO, "__libc_start_main: calling main() at %p", (void*)main);
    { extern void maintrace_arm(void); maintrace_arm(); }
    fflush(stdout);

    /* Call main() on the ELF ABI stack with a valid esp AND ebp via a naked
     * trampoline (see run_main_on_abi_stack below). A plain C call can't be
     * used: this stub is entered through the null-PLT fault redirect, so our
     * inherited ebp is a loader address, and this main() begins with
     * `mov [ebp-0x58],eax` - it writes relative to ebp before establishing its
     * own frame. We must give it a real ABI-stack ebp/esp. */
    /* PROBE: dump the exact values we're about to launch main with, and the
     * first bytes at mainfn, via direct WriteFile so it can't be lost. This
     * pins down whether mainfn/abi_esp are sane at the moment of transfer. */
    {
        extern int run_main_on_abi_stack(uint32_t,uint32_t,uint32_t,uint32_t);
        uint32_t mf = (uint32_t)(uintptr_t)main;
        uint8_t *mb = (uint8_t*)(uintptr_t)mf;
        char buf[160];
        int n = 0;
        if (!IsBadReadPtr(mb, 8)) {
            n = wsprintfA(buf, "[LAUNCH] mainfn=0x%08X bytes=%02X %02X %02X %02X %02X %02X  abi_esp=0x%08X argc=%d\r\n",
                          mf, mb[0],mb[1],mb[2],mb[3],mb[4],mb[5], (unsigned)g_abi_stack_hi, real_argc);
        } else {
            n = wsprintfA(buf, "[LAUNCH] mainfn=0x%08X UNREADABLE  abi_esp=0x%08X\r\n", mf, (unsigned)g_abi_stack_hi);
        }
        DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, n, &w, NULL);
    }
    int ret = run_main_on_abi_stack((uint32_t)(uintptr_t)main,
                                    (uint32_t)real_argc,
                                    (uint32_t)(uintptr_t)real_argv,
                                    g_abi_stack_hi);
    loader_log(LOG_INFO, "__libc_start_main: main() returned %d", ret);
    return ret;
#endif
}

/* No-op stub for null .ctors entries and unresolved PLT calls */
void stub_noop(void) {}

uint32_t stub_unresolved_plt(void)
{
    uint32_t caller = 0;
    __asm__ volatile ("mov %0, [esp]" : "=r"(caller));
    loader_log(LOG_ERROR, "[STUB] Unresolved PLT call! caller=0x%08x", caller);
    fflush(stdout);
    return 0;
}

/* ------------------------------------------------------------------ */
/* libgcc soft-arithmetic helpers (64-bit on a 32-bit target).         */
/*                                                                      */
/* A 32-bit ELF compiled by gcc does NOT inline 64-bit divide/modulo/   */
/* shift; instead it emits calls to the compiler runtime (libgcc):      */
/* __divdi3, __udivdi3, __moddi3, ... These are PLT imports just like   */
/* any libc symbol, and if unresolved the dynamic linker leaves the GOT */
/* slot 0 -> the game calls through NULL -> EIP=0 crash. GSEVO hits this */
/* in timeGetTime() (a 64-bit microsecond value / 1000), which runs on  */
/* the JVS/scheduler thread every frame.                                */
/*                                                                      */
/* These helpers are a GENERAL loader fix: any gcc-built guest that does */
/* 64-bit math now resolves them. They are purely additive -- a guest    */
/* that never imports them (e.g. ABC) is byte-identical.                 */
/*                                                                      */
/* ABI: cdecl, 64-bit args passed on the stack, 64-bit result returned  */
/* in edx:eax. A C function taking/returning `long long` under the      */
/* default mingw cdecl convention matches this exactly. The `/` `%` etc. */
/* below are themselves lowered by the HOST compiler to the HOST libgcc  */
/* (statically linked into ultra-loader.exe) -- not a recursive call     */
/* into these guest-facing stubs, because the host symbols have the      */
/* mangled libgcc names and these wrappers are named guest_*.            */
/* ------------------------------------------------------------------ */
static long long          guest___divdi3 (long long a, long long b)            { return a / b; }
static unsigned long long guest___udivdi3(unsigned long long a, unsigned long long b) { return a / b; }
static long long          guest___moddi3 (long long a, long long b)            { return a % b; }
static unsigned long long guest___umoddi3(unsigned long long a, unsigned long long b) { return a % b; }
static long long          guest___muldi3 (long long a, long long b)            { return a * b; }
static long long          guest___negdi2 (long long a)                         { return -a; }
static long long          guest___ashldi3(long long a, int b)                  { return a << b; }
static long long          guest___ashrdi3(long long a, int b)                  { return a >> b; }
static unsigned long long guest___lshrdi3(unsigned long long a, int b)         { return a >> b; }
static int                guest___cmpdi2 (long long a, long long b)            { return a < b ? 0 : (a == b ? 1 : 2); }
static int                guest___ucmpdi2(unsigned long long a, unsigned long long b) { return a < b ? 0 : (a == b ? 1 : 2); }
/* __divmoddi4 / __udivmoddi4: quotient is the return value; remainder is
 * written through the trailing pointer arg (newer gcc emits these). */
static long long          guest___divmoddi4 (long long a, long long b, long long *rem)
                                                { if (rem) *rem = a % b; return a / b; }
static unsigned long long guest___udivmoddi4(unsigned long long a, unsigned long long b, unsigned long long *rem)
                                                { if (rem) *rem = a % b; return a / b; }

/* ------------------------------------------------------------------ */
/* Round 3 stubs (Hummer + LGJ)                                        */
/* ------------------------------------------------------------------ */

/* ctype aliases via __ctype_b_loc. glibc exports isalnum/isalpha/iscntrl
 * as real symbols in addition to the macros; Hummer's GOT holds slots
 * for them. Consult our populated g_ctype_b table (same one __ctype_b_loc
 * hands back to guest inline macros), so results match the loc form. */
static int stub_isalnum(int c) { ul_ctype_init(); return (c >= -128 && c < 256) ? (g_ctype_b[c + 128] & UL_ISalnum) != 0 : 0; }
static int stub_isalpha(int c) { ul_ctype_init(); return (c >= -128 && c < 256) ? (g_ctype_b[c + 128] & UL_ISalpha) != 0 : 0; }
static int stub_iscntrl(int c) { ul_ctype_init(); return (c >= -128 && c < 256) ? (g_ctype_b[c + 128] & UL_IScntrl) != 0 : 0; }

/* pthread_cancel / pthread_exit - the Hummer worker-teardown path. We
 * have no async-cancel machinery, so cancel returns 0 (success, no-op).
 * pthread_exit terminates the current thread with ExitThread - the exit
 * code arg (a void*) is truncated to DWORD, which matches Linux behavior
 * closely enough (games only check "did it terminate", not the code). */
static int  stub_pthread_cancel(void *thread)          { (void)thread; return 0; }
static void stub_pthread_exit(void *retval)            { ExitThread((DWORD)(uintptr_t)retval); }

/* poll - Linux pollfd -> WSAPoll for socket FDs; for plain file/pipe FDs
 * we can't do a real poll on Win32 so we return "ready" (events = revents)
 * with no timeout wait. Games use this for non-blocking check loops; a
 * "ready" answer just makes them do an immediate read/write. */
struct ul_pollfd { int fd; short events; short revents; };
static int stub_poll(struct ul_pollfd *fds, unsigned long nfds, int timeout) {
    (void)timeout;
    if (!fds) return 0;
    for (unsigned long i = 0; i < nfds; i++) fds[i].revents = fds[i].events;
    return (int)nfds;
}

/* sysconf - canonical POSIX runtime constants. Defaults chosen to match
 * glibc/Linux for _SC_CLK_TCK (100), _SC_PAGESIZE (4096), _SC_NPROCESSORS
 * (host CPU count), _SC_OPEN_MAX (1024). Unknown names return -1. */
enum { UL_SC_CLK_TCK = 2, UL_SC_PAGESIZE = 30, UL_SC_PAGE_SIZE = 30,
       UL_SC_NPROCESSORS_ONLN = 84, UL_SC_NPROCESSORS_CONF = 83,
       UL_SC_OPEN_MAX = 4, UL_SC_ARG_MAX = 0 };
static long stub_sysconf(int name) {
    switch (name) {
    case UL_SC_CLK_TCK:          return 100;
    case UL_SC_PAGESIZE:         return 4096;
    case UL_SC_NPROCESSORS_ONLN:
    case UL_SC_NPROCESSORS_CONF: {
        SYSTEM_INFO si; GetSystemInfo(&si);
        return (long)si.dwNumberOfProcessors;
    }
    case UL_SC_OPEN_MAX:         return 1024;
    case UL_SC_ARG_MAX:          return 131072;
    }
    return -1;
}

/* random / srandom - libc BSD-style RNG, backed by rand()/srand(). */
static long stub_random(void)               { return (long)rand(); }
static void stub_srandom(unsigned int seed) { srand(seed); }

/* localtime_r - reentrant form; localtime() on Windows returns a pointer
 * to a static, so copy into caller's struct. */
static struct tm *stub_localtime_r(const time_t *t, struct tm *out) {
    if (!t || !out) return NULL;
    struct tm *r = localtime(t);
    if (!r) return NULL;
    *out = *r;
    return out;
}

/* waitpid - we have no forked children on Windows; return -1 (no child). */
static int stub_waitpid(int pid, int *status, int options) {
    (void)pid; (void)options;
    if (status) *status = 0;
    return -1;
}

/* writev - gather write; fall back to sequential write() per iovec. */
struct ul_iovec { void *iov_base; unsigned iov_len; };
static long stub_writev(int fd, const struct ul_iovec *iov, int iovcnt) {
    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        int n = _write(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return total > 0 ? total : -1;
        total += n;
        if ((unsigned)n < iov[i].iov_len) break;
    }
    return total;
}

/* setvbuf / ungetc / remove / rename - thin CRT forwards. */
static int stub_setvbuf(FILE *f, char *buf, int mode, size_t sz)  { return setvbuf(f, buf, mode, sz); }
static int stub_ungetc(int c, FILE *f)                            { return ungetc(c, f); }
static int stub_remove_file(const char *path)                     { return remove(path); }
static int stub_rename_file(const char *from, const char *to)     { return rename(from, to); }

/* strings - strcoll = strcmp under the C locale (what we serve).
 * strxfrm(dst, src, n) is defined so that strcmp(strxfrm(a), strxfrm(b))
 * == strcoll(a, b); the trivial impl copies src into dst (bounded) and
 * returns strlen(src). */
static int    stub_strcoll(const char *a, const char *b)          { return strcmp(a, b); }
static size_t stub_strxfrm(char *dst, const char *src, size_t n)  {
    size_t sl = strlen(src);
    if (dst && n > 0) { size_t copy = sl < n - 1 ? sl : n - 1; memcpy(dst, src, copy); dst[copy] = 0; }
    return sl;
}

/* fs - most of these are hard-linkable-only-on-Linux. Return -1 and set
 * errno = EPERM/ENOSYS is standard; but games only check the return sign
 * so returning -1 is enough. link/symlink -> unsupported; fseeko64/
 * ftello64/lseek64 -> _fseeki64/_ftelli64/_lseeki64. */
static int      stub_link(const char *from, const char *to)         { (void)from; (void)to; g_linux_errno = 1; return -1; }
static int      stub_symlink(const char *from, const char *to)      { (void)from; (void)to; g_linux_errno = 1; return -1; }
static long long stub_fseeko64(FILE *f, long long off, int whence)  { return _fseeki64(f, off, whence); }
static long long stub_ftello64(FILE *f)                             { return _ftelli64(f); }
static long long stub_lseek64(int fd, long long off, int whence)    { return _lseeki64(fd, off, whence); }
static int      stub_statvfs64(const char *path, void *buf)         { (void)path; if (buf) memset(buf, 0, 96); return 0; }
static int      stub_readdir64_r(void *dir, void *entry, void **result) { (void)dir;(void)entry; if (result) *result = NULL; return 0; }
static int      stub___xmknod(int ver, const char *path, unsigned mode, void *dev) { (void)ver;(void)path;(void)mode;(void)dev; g_linux_errno = 1; return -1; }
static long     stub_pathconf(const char *path, int name)           { (void)path; return stub_sysconf(name); }
static int      stub_utime(const char *path, const void *times)     {
    /* Windows _utime takes struct _utimbuf (2×time_t). Guest struct utimbuf
     * has the same 2×time_t layout on 32-bit - safe forward. */
    return _utime(path, (struct _utimbuf *)times);
}

/* misc - __fixunsdfdi = double -> uint64 (libgcc soft-float helper).
 * daemon() = "fork + setsid"; we have no fork, return 0 (pretend success).
 * kswap_collect: Sega's Lindbergh frame-swap sync collect (libkswapapi.so).
 * sched_get/setaffinity: CPU affinity is honor-only on Windows for these
 * arcade titles; 0 = success. */
static unsigned long long stub___fixunsdfdi(double x) {
    if (x <= 0.0)                    return 0;
    if (x >= 18446744073709551616.0) return (unsigned long long)-1;
    return (unsigned long long)x;
}
static int stub_daemon(int nochdir, int noclose)                   { (void)nochdir; (void)noclose; return 0; }
static int stub_kswap_collect(void *arg)                           { (void)arg; return 0; }
static int stub_sched_getaffinity(int pid, size_t sz, void *mask)  { (void)pid;(void)sz;(void)mask; return 0; }
static int stub_sched_setaffinity(int pid, size_t sz, const void *mask) { (void)pid;(void)sz;(void)mask; return 0; }

/* ------------------------------------------------------------------ */
/* Register all stubs with the loader context                          */
/* ------------------------------------------------------------------ */
#define REG(name) sym_register(ctx, #name, (uintptr_t)stub_##name)
#define REG2(exported, fn) sym_register(ctx, exported, (uintptr_t)fn)
/* register a guest_NAME wrapper under the libgcc symbol "NAME" */
#define REGGCC(name) sym_register(ctx, #name, (uintptr_t)guest_##name)

void stubs_register_all(LoaderCtx *ctx)
{
    g_loader_ctx = ctx;
    loader_log(LOG_INFO, "=== Registering glibc stubs (Option C) ===");

    /* errno */
    REG(__errno_location);

    /* Memory */
    REG(malloc); REG(calloc); REG(realloc); REG(free);
    REG(memalign); REG(posix_memalign);
    REG2("__libc_malloc", stub_malloc);
    REG2("__libc_free", stub_free);

    /* String */
    REG(strlen); REG(strcpy); REG(strncpy); REG(strcat); REG(strncat);
    REG(strcmp); REG(strncmp); REG(strchr); REG(strrchr); REG(strstr);
    REG(strdup); REG(strndup); REG(strerror);
    REG(strcasecmp); REG(strncasecmp);
    REG(index); REG(rindex);

    /* Memory ops */
    REG(memcpy); REG(memmove); REG(memset); REG(memcmp); REG(memchr);
    REG2("bzero", stub_bzero_wrapper);
    REG2("__memcpy_chk", stub_memcpy);
    REG2("__memset_chk", stub_memset);

    /* stdio */
    REG(printf); REG(fprintf); REG(sprintf); REG(snprintf);
    REG(vprintf); REG(vfprintf); REG(vsprintf); REG(vsnprintf);
    REG(sscanf);
    REG(fopen); REG(fopen64); REG(fclose); REG(fflush);
    REG(fread); REG(fwrite); REG(fseek); REG(fseeko); REG(ftell);
    REG(feof); REG(ferror); REG(fgets); REG(fputs); REG(fgetc); REG(fputc);
    REG(putchar); REG(puts); REG(getchar);
    /* stdin/stdout/stderr are data symbols (R_386_COPY relocations). The game
     * reads e.g. its `stderr` variable and passes it to fprintf. We must give
     * those slots REAL msvcrt FILE* values, or fprintf(stderr,...) derefs null
     * and crashes (seen in prj_tracef during FBuffer::create error logging).
     * Register the symbol VALUE as the FILE* pointer so R_386_COPY copies it. */
    sym_register(ctx, "stdout", (uintptr_t)stdout);
    sym_register(ctx, "stderr", (uintptr_t)stderr);
    sym_register(ctx, "stdin",  (uintptr_t)stdin);
    /* glibc also exposes these as __sF-style; cover common aliases. */
    sym_register(ctx, "__stdoutp", (uintptr_t)stdout);
    sym_register(ctx, "__stderrp", (uintptr_t)stderr);
    sym_register(ctx, "__stdinp",  (uintptr_t)stdin);

    /* Math */
    REG(sin); REG(cos); REG(tan); REG(asin); REG(acos); REG(atan); REG(atan2);
    REG(sqrt); REG(pow); REG(exp); REG(log); REG(log2); REG(log10);
    REG(floor); REG(ceil); REG(fabs); REG(fmod);
    REG(sinf); REG(cosf); REG(tanf); REG(sqrtf); REG(powf);
    REG(fabsf); REG(floorf); REG(ceilf);
    /* Missing float-math variants (NULL before -> NULL call crash). GSEVO's
     * init_camera_param() calls atanf for the camera FOV. Additive. */
    REG(atanf); REG(atan2f); REG(fmodf);
    REG(log10f); REG(log2f); REG(hypotf);

    /* libgcc 64-bit soft-arithmetic helpers (a gcc-built 32-bit guest
     * emits these for any 64-bit divide/modulo/shift/compare). Additive:
     * a guest that doesn't import them is unaffected. GSEVO needs
     * __divdi3 for its per-frame timeGetTime() microsecond->ms divide. */
    REGGCC(__divdi3);  REGGCC(__udivdi3);
    REGGCC(__moddi3);  REGGCC(__umoddi3);
    REGGCC(__muldi3);  REGGCC(__negdi2);
    REGGCC(__ashldi3); REGGCC(__ashrdi3); REGGCC(__lshrdi3);
    REGGCC(__cmpdi2);  REGGCC(__ucmpdi2);
    REGGCC(__divmoddi4); REGGCC(__udivmoddi4);

    /* Time */
    REG(gettimeofday); REG(time);

    /* Environment */
    REG(getenv); REG(setenv); REG(unsetenv);

    /* Process */
    REG(exit); REG2("_exit", stub__exit); REG(abort); REG(atexit);
    REG(atoi); REG(atol); REG(atof);
    REG(strtol); REG(strtoul); REG(strtod);

    /* Sort */
    REG(qsort); REG(bsearch);

    /* pthreads */
    glibc_threads_lazy_init();   /* establish the lazy-init guard lock (single-threaded here) */
    REG(pthread_create); REG(pthread_join); REG(pthread_detach); REG(pthread_self);
    REG(pthread_mutex_init); REG(pthread_mutex_destroy);
    REG(pthread_mutex_lock); REG(pthread_mutex_unlock); REG(pthread_mutex_trylock);
    REG(pthread_mutexattr_init); REG(pthread_mutexattr_destroy);
    REG(pthread_mutexattr_settype);
    REG(pthread_cond_init); REG(pthread_cond_destroy);
    REG(pthread_cond_wait); REG(pthread_cond_signal); REG(pthread_cond_broadcast);
    REG(pthread_key_create); REG(pthread_key_delete);
    REG(pthread_getspecific); REG(pthread_setspecific);
    REG(pthread_once);
    REG(pthread_attr_init); REG(pthread_attr_destroy);
    REG(pthread_attr_setstacksize); REG(pthread_attr_setdetachstate);
    REG(pthread_attr_setschedpolicy); REG(pthread_attr_setschedparam);
    REG(pthread_attr_getschedparam); REG(pthread_attr_setinheritsched);
    REG(pthread_attr_setscope); REG(pthread_setschedparam);
    REG(pthread_equal); REG(pthread_cond_timedwait);
    REG(sched_get_priority_max); REG(sched_get_priority_min); REG(sched_yield);

    /* semaphore */
    REG(sem_init); REG(sem_destroy); REG(sem_wait); REG(sem_post); REG(sem_trywait);

    /* dl* */
    REG(dlopen); REG(dlsym); REG(dlclose); REG(dlerror);

    /* Misc */
    REG(rand); REG(srand); REG(drand48); REG(srand48);
    REG(mmap); REG2("mmap64", stub_mmap); REG(munmap); REG(mprotect);
    REG(usleep); REG(sleep);
    REG(open); REG(close); REG(read); REG(write);
    REG2("open64", stub_open);
    REG2("__libc_start_main", stub___libc_start_main);

    /* ---- libc completeness for non-ABC titles (LGJ etc.) - all additive ---- */
    /* process / fs / path */
    REG(getcwd); REG(get_current_dir_name); REG(chdir);
    REG(getpid); REG(getppid); REG(getuid); REG(geteuid); REG(getgid);
    REG(dup); REG(dup2); REG(fdopen); REG(fileno);
    REG(realpath); REG(getpagesize);
    REG2("__write", stub_write);
    /* dirent */
    REG(opendir); REG(readdir); REG2("readdir64", stub_readdir); REG(closedir);
    /* signals / sleep / assert */
    REG(raise); REG(nanosleep); REG(__assert_fail);
    /* locale / ctype */
    REG(setlocale); REG(localeconv);
    REG(__ctype_b_loc); REG(__ctype_tolower_loc); REG(__ctype_toupper_loc);
    REG(__ctype_get_mb_cur_max);
    /* Legacy glibc-2.0/2.2 ctype DATA symbols (libCg.so imports these as
     * R_386_GLOB_DAT OBJECTs, size 4). Each is a POINTER VARIABLE holding the
     * table base (&g_ctype_*[128]); we export the ADDRESS of that variable so
     * libCg's GOT slot binds to it and its `mov eax,[slot]; mov eax,[eax]`
     * double-deref lands on the populated table. Populate the table contents
     * NOW (before any guest deref). ABC imports only the _loc function form,
     * so these data exports are additive/dead for ABC. */
    ul_ctype_init();
    REG2("__ctype_b",       (uintptr_t)&g_ctype_b_ptr);
    REG2("__ctype_tolower", (uintptr_t)&g_ctype_tolower_ptr);
    REG2("__ctype_toupper", (uintptr_t)&g_ctype_toupper_ptr);
    /* setjmp/longjmp + EH-frame/finalizer no-ops - needed by libCg's shader
     * compiler error path (libCg longjmps on a parse error). Additive: ABC
     * imports none of these. */
    REG2("_setjmp",  (uintptr_t)stub__setjmp);
    REG2("setjmp",   (uintptr_t)stub__setjmp);
    REG2("__sigsetjmp", (uintptr_t)stub__setjmp);  /* libCg may use the sig variant */
    REG2("longjmp",  (uintptr_t)stub_longjmp);
    REG2("siglongjmp", (uintptr_t)stub_longjmp);
    REG2("__register_frame_info",         (uintptr_t)stub___register_frame_info);
    REG2("__deregister_frame_info",       (uintptr_t)stub___deregister_frame_info);
    REG2("__register_frame_info_bases",   (uintptr_t)stub___register_frame_info_bases);
    REG2("__deregister_frame_info_bases", (uintptr_t)stub___deregister_frame_info_bases);
    REG2("__cxa_finalize",                (uintptr_t)stub___cxa_finalize);
    /* stdio out-of-line */
    REG(_IO_getc); REG(_IO_putc);
    REG(_IO_fgetc); REG(_IO_fputc);
    /* getc/putc/getchar/putchar as real symbols (glibc exports them
     * out-of-line in addition to the macros). Alias-registered so games
     * whose GOT holds a real slot don't fail to resolve.  ABC has no
     * dependency on these (already registered fgetc/fputc/putchar). */
    REG2("getc",     (uintptr_t)stub_getc_alias);
    REG2("putc",     (uintptr_t)stub_putc_alias);
    REG2("getchar",  (uintptr_t)stub_getchar_alias);
    REG2("putchar",  (uintptr_t)stub_putchar_alias);

    /* xstat family - Hummer + generic libc file-probe path. Additive. */
    REG2("__xstat",    (uintptr_t)stub___xstat);
    REG2("__xstat64",  (uintptr_t)stub___xstat64);
    REG2("__lxstat",   (uintptr_t)stub___lxstat);
    REG2("__lxstat64", (uintptr_t)stub___lxstat64);
    REG2("__fxstat",   (uintptr_t)stub___fxstat);
    REG2("__fxstat64", (uintptr_t)stub___fxstat64);

    /* termios - no-op TTY surface. Additive. */
    REG2("tcgetattr",   (uintptr_t)stub_tcgetattr);
    REG2("tcsetattr",   (uintptr_t)stub_tcsetattr);
    REG2("cfmakeraw",   (uintptr_t)stub_cfmakeraw);
    REG2("cfsetispeed", (uintptr_t)stub_cfsetispeed);
    REG2("cfsetospeed", (uintptr_t)stub_cfsetospeed);
    REG2("cfgetispeed", (uintptr_t)stub_cfgetispeed);
    REG2("cfgetospeed", (uintptr_t)stub_cfgetospeed);
    REG2("tcflush",     (uintptr_t)stub_tcflush);
    REG2("tcdrain",     (uintptr_t)stub_tcdrain);
    REG2("tcflow",      (uintptr_t)stub_tcflow);
    REG2("tcsendbreak", (uintptr_t)stub_tcsendbreak);
    /* math */
    REG(finite); REG(finitef); REG(isinf); REG(isnan);
    REG(sinh); REG(cosh); REG(tanh); REG(expf); REG(modf); REG(frexp);

    /* wchar / locale (Hummer + LGJ) - C-locale, ASCII-fastpath. Additive:
     * ABC imports none of these, so registration cannot regress ABC. Guest
     * wchar_t is int32 (32-bit ELF glibc), not the host u16. */
    REG(wcslen); REG(wmemcpy); REG(wmemmove); REG(wmemset); REG(wmemchr); REG(wmemcmp);
    REG(btowc);  REG(wctob);   REG(towlower); REG(towupper);
    REG(mbrtowc); REG(wcrtomb);
    REG(iswctype); REG(wctype); REG(wcsftime);
    REG(wcscoll); REG(wcsxfrm);
    REG(getwc);  REG(putwc);   REG(ungetwc);

    /* zlib inflate (GSEVO gzip-compresses some assets; ABC imports none of
     * these so the registration is additive/no-op for it). */
    { extern void zlib_shim_register(LoaderCtx *ctx); zlib_shim_register(ctx); }

    /* FTDI FT4232H + libudev shim (Cruis'n Blast Raw Thrills G5). Registers
     * FT_* + udev_* into the loader symbol table so the ELF's PLT can bind
     * them directly without going through dlopen/dlsym. Also spawns the
     * synthetic RIO2/UHB producer threads lazily on the first open(). */
    { extern void ftdi_shim_register(void *ctx); ftdi_shim_register(ctx); }

    /* ---- Round 3 stubs (Hummer + LGJ) - all additive, ABC-safe ---- */
    /* ctype real-symbol aliases (glibc exports these in addition to the
     * inline macros). Use our populated g_ctype_b table. */
    REG(isalnum); REG(isalpha); REG(iscntrl);
    /* pthread completeness */
    REG(pthread_cancel); REG(pthread_exit);
    /* libc: net/proc/time/io */
    REG(poll); REG(sysconf); REG(random); REG2("srandom", (uintptr_t)stub_srandom);
    REG(localtime_r); REG(waitpid); REG(writev); REG(setvbuf); REG(ungetc);
    REG2("remove", (uintptr_t)stub_remove_file);
    REG2("rename", (uintptr_t)stub_rename_file);
    /* strings */
    REG(strcoll); REG(strxfrm);
    /* fs - link/symlink -1, 64-bit seek forwards, misc -1/0 */
    REG(link); REG(symlink); REG(fseeko64); REG(ftello64); REG(lseek64);
    REG(statvfs64); REG(readdir64_r); REG(__xmknod); REG(pathconf); REG(utime);
    /* misc - libgcc double->uint64, daemon, sched-affinity, kswap collect */
    REG(__fixunsdfdi); REG(daemon); REG(kswap_collect);
    REG(sched_getaffinity); REG(sched_setaffinity);

    /* Cruis'n Blast Unresolved-symbol pass - additive libc/libstdc++/pthread
     * completeness (gethostname, clock, gmtime, statfs, getpwuid, dlvsym,
     * shmget/rmdir/lseek/chmod/umask, pthread_rwlock_*, _S_empty_rep_storage,
     * std::exception vtables + typeinfos, __*_chk variants, epoll_create1,
     * pipe2, uname, sysinfo, timerfd/signalfd, stat64/lstat64/fstat64). */
    { extern void cruisn_stubs_register(LoaderCtx *);
      extern void cruisn_stubs_register_late(LoaderCtx *);
      cruisn_stubs_register(ctx);
      cruisn_stubs_register_late(ctx); }

    loader_log(LOG_INFO, "Registered %d stubs", ctx->symbol_count);
}

/* ------------------------------------------------------------------ */
/* Updated dlopen - intercepts libGL.so                               */
/* ------------------------------------------------------------------ */
/* Note: the original stub_dlopen is still registered; this override
 * is patched in via sym_register after stubs_register_all() runs.   */

/* Shared "libc-family handle" DATA page. Two-way defense:
 *
 * (A) Cruis'n Blast's Raw Thrills packed _start disassembly (see docs) shows
 *     the packer treats the dlopen handle as an OPAQUE token - it NULL-checks
 *     it and passes it straight to dlsym(handle, name). No l_addr/l_name/l_ld
 *     read, no chained deref, no field probe. So a raw sentinel WOULD work
 *     for that path, provided dlsym recognizes it.
 *
 * (B) But other titles (and glibc-rtld-style debug helpers, GNU IFUNC probes,
 *     _dl_debug_state walkers, some LD_DEBUG tracers) do walk the returned
 *     handle as a `struct link_map*`. Field offsets (i386):
 *        +0x00  l_addr    ElfW(Addr)       - load bias (0 for main program)
 *        +0x04  l_name    char*            - DT_SONAME / filename string
 *        +0x08  l_ld      ElfW(Dyn)*       - pointer to _DYNAMIC array
 *        +0x0C  l_next    struct link_map* - next in loaded-DSO chain
 *        +0x10  l_prev    struct link_map* - previous in loaded-DSO chain
 *        (+0x14..+0x??  private rtld state - never touched by well-behaved
 *         callers, and even glibc's <link.h> only exposes the five above)
 *
 * We build a REAL fake link_map at page[0] backed by strings + a DT_NULL-only
 * Elf32_Dyn array inside the SAME page, so any deref through l_ld returns a
 * legal end-of-list dyn entry (d_tag=DT_NULL, d_un=0). l_next/l_prev self-loop
 * so walkers that iterate the DSO chain terminate at us. The remainder of the
 * 64KB page stays zero: any field-offset probe (e.g. `handle->l_addr + 0xa6c`
 * a la the old sem_init theory) still lands in RW zeros.
 *
 * Reused as the dlsym libc-handle sentinel too. */

/* Elf32 minimal typedefs (avoid pulling in <elf.h> - MinGW headers vary). */
typedef struct { int32_t d_tag; uint32_t d_val; } Ul_Elf32_Dyn;
#define UL_DT_NULL   0
#define UL_DT_STRTAB 5
#define UL_DT_SONAME 14

/* On-page layout (offsets INSIDE the 64KB page):
 *   0x0000  fake_link_map (20 bytes)
 *   0x0020  l_name string "libc.so.6\0"      (10 bytes, padded)
 *   0x0040  strtab region (32 bytes zeros + soname copy)
 *   0x0080  Elf32_Dyn table (3 entries * 8B = 24 bytes)
 *   0x0100..0xFFFF  zero playground for any offset-based deref
 */
struct ul_fake_link_map {
    uint32_t l_addr;     /* 0x00 */
    uint32_t l_name;     /* 0x04 - char*        */
    uint32_t l_ld;       /* 0x08 - Elf32_Dyn*   */
    uint32_t l_next;     /* 0x0C */
    uint32_t l_prev;     /* 0x10 */
};

static void *g_libc_handle_page = NULL;
static void *get_libc_handle_page(void) {
    if (!g_libc_handle_page) {
        /* 64KB RW zero page: reads return 0, writes to any offset OK. */
        g_libc_handle_page = VirtualAlloc(NULL, 0x10000,
                                          MEM_COMMIT|MEM_RESERVE,
                                          PAGE_READWRITE);
        /* Fallback if VirtualAlloc failed - keep old sentinel behavior. */
        if (!g_libc_handle_page) {
            g_libc_handle_page = (void*)0xDEAD2000u;
            return g_libc_handle_page;
        }

        /* Populate a REAL fake struct link_map at page[0]. */
        uint8_t *p = (uint8_t*)g_libc_handle_page;

        /* l_name string at page+0x20 */
        char *name_str = (char*)(p + 0x20);
        memcpy(name_str, "libc.so.6", 10);

        /* strtab at page+0x40 (index 0 = empty string, then soname). */
        char *strtab = (char*)(p + 0x40);
        strtab[0] = '\0';
        memcpy(strtab + 1, "libc.so.6", 10);

        /* Elf32_Dyn table at page+0x80: DT_SONAME -> strtab offset 1,
         * DT_STRTAB -> strtab pointer, DT_NULL terminator. */
        Ul_Elf32_Dyn *dyn = (Ul_Elf32_Dyn*)(p + 0x80);
        dyn[0].d_tag = UL_DT_SONAME; dyn[0].d_val = 1;
        dyn[1].d_tag = UL_DT_STRTAB; dyn[1].d_val = (uint32_t)(uintptr_t)strtab;
        dyn[2].d_tag = UL_DT_NULL;   dyn[2].d_val = 0;

        /* Fill link_map at page[0].
         * l_addr = base of our 64KB page (NOT 0). Real glibc uses the DSO
         * load bias; the Raw Thrills packer computes sem_t (and mutex_t)
         * addresses as `handle->l_addr + fixed_offset` (offset ~0xa38 for
         * the first sem_t). With l_addr=0 that lands at low 0xa38 (unmapped
         * -> packer exits(1) when it reads back the sem_t magic). With
         * l_addr=page_base the same computation lands inside the 64KB RW
         * zero playground (0x100..0xFFFF), so any subsequent load/store on
         * the derived pointer just reads/writes zeros and passes. */
        struct ul_fake_link_map *lm = (struct ul_fake_link_map*)p;
        lm->l_addr = (uint32_t)(uintptr_t)p;   /* fake load bias = page base */
        lm->l_name = (uint32_t)(uintptr_t)name_str;
        lm->l_ld   = (uint32_t)(uintptr_t)dyn;
        /* Self-loop so walkers that do `while (m) m = m->l_next` terminate
         * on next iteration (they'll see the same link_map twice max -
         * cheaper than another allocation and safe for a stub). Callers that
         * check "is this the head?" typically stop when l_prev == NULL, so we
         * set l_prev = NULL and l_next = self.
         * FIX 2026-07-02: previous NULLs caused the Cruis'n Blast packer to
         * compute `handle->l_next + 0xa6c` = 0xa6c and pass that as sem_t*,
         * which our sem_init treated as a low-poison and returned success but
         * with a Win32 handle keyed to 0xa6c. The packer then did further
         * arithmetic off l_next/l_prev that landed in unmapped memory ->
         * exit(1). Setting BOTH l_next and l_prev to the page base so any
         * `(l_next|l_prev) + K` lands in the 64KB RW zero playground. */
        lm->l_next = (uint32_t)(uintptr_t)p;   /* self-ref -> RW zero page */
        lm->l_prev = (uint32_t)(uintptr_t)p;   /* self-ref -> RW zero page */

        loader_log(LOG_INFO,
                   "[stub] libc_handle_page: fake link_map @ %p (l_name=\"libc.so.6\", l_ld=%p, 64KB RW)",
                   (void*)lm, (void*)dyn);
    }
    return g_libc_handle_page;
}

static void *stub_dlopen_v2(const char *filename, int flags)
{
    /* dlopen(NULL, ...) is glibc-ese for "the main program itself". A game
     * that then does dlsym(handle, "__environ") or dlsym(handle, "somesym")
     * expects the returned handle to be usable against the running program
     * symbol table. We use a distinct sentinel (0xDEAD2000) so stub_dlsym_v2
     * routes NULL/main-program lookups into the loader symbol table before
     * falling through to the well-known data-symbol table and code stub.
     * Purely additive: no known title relied on this returning NULL. */
    if (!filename) {
        void *h = get_libc_handle_page();
        loader_log(LOG_INFO, "[stub] dlopen(NULL) -> main-program handle %p (zero-mapped)", h);
        return h;
    }
    /* Cruis'n Blast (Raw Thrills packed _start) explicitly dlopens
     * "libc.so.6" / "libpthread.so.0" / "libdl.so.2" / "libm.so.6" and then
     * dlsym's names like `getauxval`, `sem_init`, `pthread_create` off the
     * returned handle to bypass the PLT (avoids WEAK-bind-to-0 traps). Route
     * these to the SAME main-program sentinel 0xDEAD2000 so stub_dlsym_v2 hits
     * the loader symbol table (where cru_getauxval, stub_sem_init, and every
     * pthread helper live) instead of falling through to the code no-op stub -
     * which is what the diagnosis said turned dlsym(unknown_data_sym) into a
     * bogus base pointer and produced `sem_init(0x00000a64, 1)`. Purely
     * additive: any title that never dlopens libc directly is unaffected. */
    if (filename) {
        const char *slash = strrchr(filename, '/');
        const char *bs    = strrchr(filename, '\\');
        const char *base  = slash ? slash + 1 : (bs ? bs + 1 : filename);
        if (!strncmp(base, "libc.so",       7) ||
            !strncmp(base, "libpthread.so", 13) ||
            !strncmp(base, "libdl.so",      8) ||
            !strncmp(base, "libm.so",       7) ||
            !strncmp(base, "librt.so",      8) ||
            !strncmp(base, "libnsl.so",     9) ||
            !strncmp(base, "libresolv.so", 12) ||
            !strncmp(base, "libcrypt.so",  11) ||
            !strncmp(base, "libgcc_s.so",  11)) {
            void *h = get_libc_handle_page();
            loader_log(LOG_INFO, "[stub] dlopen(%s) -> libc-family handle %p (64KB zero page)", filename, h);
            return h;
        }
    }
    if (filename && libgl_is_gl_library(filename))
        return libgl_dlopen(filename);
    /* Cruis'n Blast dlopens libhasp_linux_x86_*.so at boot. Route it to our
     * HASP shim so hasp_shim_lookup() can serve the 30 SDK entry points.  */
    {
        extern int   hasp_shim_matches(const char *);
        extern void *hasp_shim_dlopen(const char *);
        if (filename && hasp_shim_matches(filename))
            return hasp_shim_dlopen(filename);
    }
    /* Cruis'n Blast (Raw Thrills G5) also dlopens libudev.so + libftd2xx.so
     * to enumerate its FT4232H arcade I/O (steering wheel + coin/start board).
     * Route to the FTDI shim so subsequent dlsym() finds the synthetic udev
     * enumeration + D2XX handlers. Purely additive: ABC/GSEVO/LGJ never
     * dlopen these libraries. */
    {
        extern int   ftdi_shim_is_ftdi_library(const char *);
        extern int   ftdi_shim_is_udev_library(const char *);
        extern void *ftdi_shim_dlopen(const char *, int);
        if (filename &&
            (ftdi_shim_is_ftdi_library(filename) ||
             ftdi_shim_is_udev_library(filename)))
            return ftdi_shim_dlopen(filename, flags);
    }
    /* Cruis'n Blast - PhysX 2.8.1 (libPhysXLoader.so.1) + FMOD (libfmod.so.10)
     * + libtheora video codec. All three are stubbed to no-ops so the game
     * boots to attract; PhysX collision returns "no hit", FMOD is silent
     * (SOUND_STATUS=silent per the Cruis'n Blast game-complete gap), theora
     * plays no frames. Additive: any title that never dlopens these names
     * sees zero change. */
    if (filename) {
        extern bool  physx_loader_is_lib(const char*);
        extern void *physx_loader_dlopen(const char*);
        extern bool  fmod_wire_is_lib(const char*);
        extern void *fmod_wire_dlopen(const char*);
        if (physx_loader_is_lib(filename)) return physx_loader_dlopen(filename);
        if (fmod_wire_is_lib(filename))    return fmod_wire_dlopen(filename);
        if (strstr(filename, "libtheora") || strstr(filename, "libogg") ||
            strstr(filename, "libvorbis")) {
            loader_log(LOG_INFO, "[stub] dlopen(%s) -> silent codec handle",
                       filename);
            return (void*)0xDEADC0DEu;
        }
    }
    loader_log(LOG_WARN, "dlopen(%s) stubbed", filename ? filename : "(null)");
    extern LoaderCtx *g_loader_ctx;
    /* Return a fake non-NULL handle */
    return (void*)0xDEAD1000;
}

static void *stub_dlsym_v2(void *handle, const char *symbol)
{
    /* Try libGL resolution first */
    void *p = libgl_dlsym(handle, symbol);
    if (p) return p;
    /* Fall back to loader symbol table */
    extern LoaderCtx *g_loader_ctx;
    if (g_loader_ctx) { uintptr_t r = sym_resolve(g_loader_ctx, symbol); if (r) return (void*)r; }

    /* Sentinel HL / Aladdin HASP SDK - full 30-entry surface + SysGetDongleHID.
     * The Raw Thrills Dongle wrapper calls hasp_encrypt() then hasp_decrypt()
     * and compares to the original, so we implement true AES-128-CBC
     * round-trip in hasp_shim.c. Purely additive for ABC/GSEVO/LGJ, which
     * never dlsym any of these names. */
    if (symbol) {
        extern void *hasp_shim_lookup(const char *symbol);
        void *hasp = hasp_shim_lookup(symbol);
        if (hasp) return hasp;
    }

    /* FTDI FT4232H / libudev shim (Cruis'n Blast RIO2 + UHB). Serves udev
     * enumeration + D2XX + list-entry walk + sysattr lookups. Purely additive
     * (other titles never dlsym FT_* or udev_* names). */
    if (symbol) {
        extern void *ftdi_shim_lookup(const char *symbol);
        void *ftdi = ftdi_shim_lookup(symbol);
        if (ftdi) return ftdi;
    }

    /* PhysX 2.8.1 loader + FMOD (Cruis'n Blast). Handle-keyed so we ONLY
     * intercept when the game holds the shim's marker handle - this keeps
     * an unrelated title whose GAME code happens to name a symbol NxCreate*
     * or FMOD_System_* out of our way (they'd never dlopen libPhysXLoader
     * or libfmod.so, so their handle differs).                            */
    if (symbol && handle == (void*)0xDEADFACE) {
        extern void *physx_loader_dlsym(void*, const char*);
        void *px = physx_loader_dlsym(handle, symbol);
        if (px) return px;
    }
    if (symbol && handle == (void*)0xDEADF00D) {
        extern void *fmod_wire_dlsym(void*, const char*);
        void *fm = fmod_wire_dlsym(handle, symbol);
        if (fm) return fm;
    }

    /* GSEVO loads several Sega/CRI middleware shared objects (snd_util.so =
     * CRI ADX/ADXM/Sofdec audio + mwPly movie; libkswapapi.so = frame-swap sync;
     * sndoutput* = sound output) and resolves their entry points by dlsym(),
     * CACHING the pointers and CALLING them unconditionally (e.g. adx_init ->
     * call [so_ADXM_SetupFramework]; a NULL there is a call-to-0 access
     * violation). These .so's are stubbed, so the lookups return NULL.
     *
     * NAME-KEYED DISPATCH (cri_shim.c): for a KNOWN middleware symbol, hand back
     * a dedicated per-function clean-room shim that honors the public CRI
     * ADX/ADXT/ADXM/Sofdec + sndoutput contract (Create/SetupFramework return a
     * non-NULL dummy handle; GetStat returns STOP; everything else is a success
     * no-op). This is what lets the game deref a returned handle safely and lets
     * its "wait until stopped" loops complete. Keyed on symbols ABC never
     * dlsym's, so it is purely additive for ABC. */
    if (symbol) {
        extern void *cri_shim_lookup(const char *symbol);
        void *cri = cri_shim_lookup(symbol);
        if (cri) return cri;
    }

    /* Well-known glibc DATA symbols. A game (Cruis'n Blast custom bootstrap
     * observed) may dlsym these and treat the return value as a struct-base
     * pointer - e.g. `sem_init(&((struct linkmap*)dlsym(h,"__environ"))->field,
     * ...)`. If we hand back NULL (or worse, the safe no-op CODE stub below),
     * that becomes `sem_init(NULL+0xa70, ...)` and fires exit(1). Return real
     * addresses of loader-side data cells that already exist for the ABI. All
     * purely additive: any title that doesn't dlsym these names sees no
     * change. */
    if (symbol) {
        static char *s_progname = "cruisn.elf";
        static char *s_environ_null = NULL;
        extern volatile uint32_t g_abi_stack_hi;
        extern volatile uint32_t g_abi_argc;
        extern volatile uint32_t g_abi_argv;
        if (!strcmp(symbol, "__libc_stack_end"))               return (void*)&g_abi_stack_hi;
        if (!strcmp(symbol, "__environ"))                      return (void*)&s_environ_null;
        if (!strcmp(symbol, "environ"))                        return (void*)&s_environ_null;
        if (!strcmp(symbol, "_environ"))                       return (void*)&s_environ_null;
        if (!strcmp(symbol, "__progname"))                     return (void*)&s_progname;
        if (!strcmp(symbol, "__progname_full"))                return (void*)&s_progname;
        if (!strcmp(symbol, "program_invocation_name"))        return (void*)&s_progname;
        if (!strcmp(symbol, "program_invocation_short_name"))  return (void*)&s_progname;
        if (!strcmp(symbol, "__libc_argc"))                    return (void*)&g_abi_argc;
        if (!strcmp(symbol, "__libc_argv"))                    return (void*)&g_abi_argv;
        /* Additional glibc-internal cells Raw Thrills packed _start touches.
         * Give a real zero-cell address (from the same zero page used above)
         * so callers that deref this pointer get zero, not a crash. */
        {
            static uint32_t s_zero_cell = 0;
            static uint32_t s_one_cell  = 1;   /* _dl_starting_up = 1 until init done */
            if (!strcmp(symbol, "_dl_starting_up"))            return (void*)&s_one_cell;
            if (!strcmp(symbol, "__dso_handle"))               return (void*)&s_zero_cell;
            if (!strcmp(symbol, "_dl_argv"))                   return (void*)&g_abi_argv;
            if (!strcmp(symbol, "_dl_argc"))                   return (void*)&g_abi_argc;
            if (!strcmp(symbol, "__pointer_chk_guard_local"))  return (void*)&s_zero_cell;
            if (!strcmp(symbol, "__pointer_chk_guard"))        return (void*)&s_zero_cell;
        }
    }

    /* Unknown symbol against a LIBC-family handle: the caller could be treating
     * this as a DATA pointer (e.g. `sem_init(&((struct*)dlsym(libc,X))->f, 1)`).
     * If we hand back the executable no-op code stub, `&code_stub->f_at_0xa64`
     * lands in a valid RX page and dereferences garbage - exactly the observed
     * `sem_init(guest 0x00000a64, 1)` crash. Return a fresh 64KB zero-mapped
     * read/write DATA page instead: offsets up to 64KB are legal, reads yield
     * zero (matches an uninitialized libc data cell), writes silently persist.
     * Purely additive: the LIBC-family handle sentinel is only produced by the
     * new dlopen route above, so no other title ever reaches this branch. */
    if (symbol && (handle == (void*)0xDEAD2000u || handle == g_libc_handle_page)) {
        /* Reuse the SAME zero page we handed out from dlopen("libc.so.6")
         * so a caller that dlsym's an unknown data symbol reads zeroes
         * that match the reads it would make directly off the handle. */
        void *page = get_libc_handle_page();
        loader_log(LOG_WARN, "dlsym(libc_handle, %s) unresolved -> zero-mapped data page %p (libc-safe)",
                   symbol, page);
        return page;
    }

    /* Unknown symbol: hand back a shared, safe `xor eax,eax; ret` executable
     * stub (this branch only runs after libGL + the loader symbol table + the
     * CRI dispatch all miss), so an unexpected call no-ops instead of faulting.
     * cdecl-safe: the stub pops nothing, leaving the caller's stack intact.
     * Purely additive for ABC, which uses SEGAAPI and never reaches an
     * unresolved dlsym on this path. */
    if (symbol) {
        static uint8_t *dl_noop = NULL;
        if (!dl_noop) {
            dl_noop = (uint8_t*)VirtualAlloc(NULL, 16, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (dl_noop) { dl_noop[0]=0x31; dl_noop[1]=0xC0; dl_noop[2]=0xC3; } /* xor eax,eax; ret */
        }
        /* Log the miss so the next smoke run tells us the exact symbol the
         * Cruis'n Blast bootstrap wanted (helps refine this table). */
        loader_log(LOG_WARN, "dlsym(handle=%p, %s) unresolved -> safe no-op code stub (middleware)", handle, symbol);
        return dl_noop;
    }
    return NULL;
}

void stubs_upgrade_dl(LoaderCtx *ctx)
{
    sym_register(ctx, "dlopen", (uintptr_t)stub_dlopen_v2);
    sym_register(ctx, "dlsym",  (uintptr_t)stub_dlsym_v2);
    /* Also register __errno_location as thread-aware version */
    sym_register(ctx, "__errno_location", (uintptr_t)threads_errno_location);
}
