/*
 * guest_alloc.c - a small, fully self-contained heap allocator used as the GUEST
 * heap (enabled by ULTRA_GUEST_HEAP=1; routed from stubs_glibc.c / stubs_cxx.c).
 *
 * WHY: a Win32/OS heap cannot be reverted in place - its internal metadata
 * advances as it grows, so writing an older Snapshot back over it corrupts the
 * allocator. This allocator keeps ALL of its state (free list + every block tag +
 * the carve high-water) INSIDE the arena, so a Snapshot that reverts the arena's
 * bytes reverts the allocator EXACTLY to the saved state. That is what makes
 * in-session save-states safe, and it is the XSTATE foundation.
 *
 * Design (Knuth/CSAPP): one contiguous arena VirtualAlloc-RESERVED up front and
 * committed lazily; implicit boundary-tagged blocks ([HDR][payload][FTR], size in
 * both tags, low bit = allocated) with a prologue+epilogue so coalescing needs no
 * edge cases; an explicit LIFO doubly-linked free list (links live in free-block
 * payloads); first-fit; immediate coalescing on free; 8-byte payload alignment.
 *
 * Pointers stored in the arena are absolute but the arena sits at a FIXED address
 * for the life of the process, so they stay valid across an in-session restore.
 * (Cross-session/portable saves are XSTATE's job, not this baseline.)
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>

typedef unsigned int u32;
typedef unsigned char u8;

#define A_RESERVE  ((size_t)512 << 20)   /* address-space reserve (not committed) */
#define A_CHUNK    ((size_t)8   << 20)   /* commit granularity */
#define WSIZE 4u
#define DSIZE 8u
#define MINBLK 16u                       /* HDR+FTR(8) + fd,bk(8) */
#define A_ALIGN(s) (((s) + (DSIZE-1)) & ~(size_t)(DSIZE-1))

#define PACK(sz,al)  ((u32)((sz) | (al)))
#define GET(p)       (*(u32*)(p))
#define PUT(p,v)     (*(u32*)(p) = (u32)(v))
#define GET_SIZE(p)  (GET(p) & ~0x7u)
#define GET_ALLOC(p) (GET(p) & 0x1u)
#define HDRP(bp)     ((u8*)(bp) - WSIZE)
#define FTRP(bp)     ((u8*)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)
#define NEXT_BLKP(bp)((u8*)(bp) + GET_SIZE((u8*)(bp) - WSIZE))
#define PREV_BLKP(bp)((u8*)(bp) - GET_SIZE((u8*)(bp) - DSIZE))
#define FD(bp)       (*(void**)(bp))             /* next free block (payload+0) */
#define BK(bp)       (*(void**)((u8*)(bp)+WSIZE))/* prev free block (payload+4) */

/* Persistent allocator state lives in the arena header (captured by Snapshots). */
typedef struct { u32 magic; void *free_head; u8 *brk; u32 pad; } AHdr;
#define A_MAGIC 0x47414C4Cu  /* 'GALL' */

static u8     *g_base = NULL;     /* reserve base - fixed for the process lifetime */
static size_t  g_committed = 0;   /* physical committed bytes (monotonic; not part of revertible state) */
static u8     *g_heap_listp = 0;  /* prologue payload - constant once set */
static CRITICAL_SECTION g_alk;    /* serialize (loader-side mutex; not allocator state) */
static int     g_alk_init = 0;

static AHdr *H(void) { return (AHdr*)g_base; }

/* Commit physical pages up to (but capture only uses [base,brk)). Returns old brk. */
static void *mem_sbrk(ptrdiff_t incr)
{
    u8 *old = H()->brk;
    u8 *nb  = old + incr;
    if (incr < 0 || nb > g_base + A_RESERVE) return (void*)-1;
    size_t need = (size_t)(nb - g_base);
    while (g_committed < need) {
        size_t want = need - g_committed;
        size_t chunk = want > A_CHUNK ? want : A_CHUNK;
        if (g_committed + chunk > A_RESERVE) chunk = A_RESERVE - g_committed;
        if (!VirtualAlloc(g_base + g_committed, chunk, MEM_COMMIT, PAGE_READWRITE)) return (void*)-1;
        g_committed += chunk;
    }
    H()->brk = nb;
    return old;
}

static void fl_insert(void *bp)
{
    FD(bp) = H()->free_head;
    BK(bp) = NULL;
    if (H()->free_head) BK(H()->free_head) = bp;
    H()->free_head = bp;
}
static void fl_remove(void *bp)
{
    void *fd = FD(bp), *bk = BK(bp);
    if (bk) FD(bk) = fd; else H()->free_head = fd;
    if (fd) BK(fd) = bk;
}

static void *coalesce(void *bp)
{
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = GET_SIZE(HDRP(bp));
    if (prev_alloc && next_alloc) {
        /* isolated */
    } else if (prev_alloc && !next_alloc) {
        void *n = NEXT_BLKP(bp); fl_remove(n);
        size += GET_SIZE(HDRP(n));
        PUT(HDRP(bp), PACK(size,0)); PUT(FTRP(bp), PACK(size,0));
    } else if (!prev_alloc && next_alloc) {
        void *p = PREV_BLKP(bp); fl_remove(p);
        size += GET_SIZE(HDRP(p));
        PUT(FTRP(bp), PACK(size,0)); PUT(HDRP(p), PACK(size,0));
        bp = p;
    } else {
        void *p = PREV_BLKP(bp), *n = NEXT_BLKP(bp); fl_remove(p); fl_remove(n);
        size += GET_SIZE(HDRP(p)) + GET_SIZE(HDRP(n));
        PUT(HDRP(p), PACK(size,0)); PUT(FTRP(n), PACK(size,0));
        bp = p;
    }
    fl_insert(bp);
    return bp;
}

static void *extend_heap(size_t bytes)
{
    bytes = A_ALIGN(bytes);
    if (bytes < MINBLK) bytes = MINBLK;
    u8 *bp = (u8*)mem_sbrk((ptrdiff_t)bytes);
    if (bp == (u8*)-1) return NULL;
    PUT(HDRP(bp), PACK(bytes,0));            /* old epilogue becomes free block hdr */
    PUT(FTRP(bp), PACK(bytes,0));
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0,1));     /* new epilogue */
    return coalesce(bp);
}

static int ga_init(void)
{
    if (g_base) return 1;
    if (!g_alk_init) { InitializeCriticalSection(&g_alk); g_alk_init = 1; }
    g_base = (u8*)VirtualAlloc(NULL, A_RESERVE, MEM_RESERVE, PAGE_READWRITE);
    if (!g_base) return 0;
    if (!VirtualAlloc(g_base, A_CHUNK, MEM_COMMIT, PAGE_READWRITE)) { g_base = NULL; return 0; }
    g_committed = A_CHUNK;
    /* arena header, then the CSAPP prologue/epilogue starting 8-aligned after it */
    AHdr *h = (AHdr*)g_base;
    h->magic = A_MAGIC; h->free_head = NULL;
    u8 *start = (u8*)A_ALIGN((uintptr_t)(g_base + sizeof(AHdr)));
    h->brk = start;                          /* sbrk frontier begins here */
    u8 *p = (u8*)mem_sbrk(4 * WSIZE);
    if (p == (u8*)-1) return 0;
    PUT(p + 0,        0);                     /* alignment padding */
    PUT(p + WSIZE,    PACK(DSIZE,1));         /* prologue header */
    PUT(p + 2*WSIZE,  PACK(DSIZE,1));         /* prologue footer */
    PUT(p + 3*WSIZE,  PACK(0,1));             /* epilogue header */
    g_heap_listp = p + 2*WSIZE;
    if (!extend_heap(A_CHUNK)) return 0;
    return 1;
}

static void *find_fit(size_t asize)
{
    for (void *bp = H()->free_head; bp; bp = FD(bp))
        if (GET_SIZE(HDRP(bp)) >= asize) return bp;
    return NULL;
}
static void place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp));
    fl_remove(bp);
    if (csize - asize >= MINBLK) {
        PUT(HDRP(bp), PACK(asize,1)); PUT(FTRP(bp), PACK(asize,1));
        void *nb = NEXT_BLKP(bp);
        PUT(HDRP(nb), PACK(csize - asize,0)); PUT(FTRP(nb), PACK(csize - asize,0));
        fl_insert(nb);
    } else {
        PUT(HDRP(bp), PACK(csize,1)); PUT(FTRP(bp), PACK(csize,1));
    }
}

/* ---- public API ------------------------------------------------------------- */
int ga_ready(void) { return g_base != NULL; }

void *ga_malloc(size_t size)
{
    if (!g_base && !ga_init()) return NULL;
    if (size == 0) return NULL;
    size_t asize = (size <= DSIZE) ? 2*DSIZE : A_ALIGN(size + DSIZE);
    EnterCriticalSection(&g_alk);
    void *bp = find_fit(asize);
    if (!bp) {
        size_t ext = asize > A_CHUNK ? asize : A_CHUNK;
        if (extend_heap(ext)) bp = find_fit(asize);
    }
    if (bp) place(bp, asize);
    LeaveCriticalSection(&g_alk);
    return bp;
}

void ga_free(void *bp)
{
    if (!bp || !g_base) return;
    EnterCriticalSection(&g_alk);
    size_t size = GET_SIZE(HDRP(bp));
    PUT(HDRP(bp), PACK(size,0));
    PUT(FTRP(bp), PACK(size,0));
    coalesce(bp);
    LeaveCriticalSection(&g_alk);
}

size_t ga_usable(void *bp)   /* payload bytes available in an allocated block */
{
    if (!bp || !g_base) return 0;
    return GET_SIZE(HDRP(bp)) - DSIZE;
}

void *ga_realloc(void *bp, size_t size)
{
    if (!bp) return ga_malloc(size);
    if (size == 0) { ga_free(bp); return NULL; }
    size_t old = ga_usable(bp);
    if (old >= size) return bp;              /* shrink/keep in place */
    void *np = ga_malloc(size);
    if (!np) return NULL;
    memcpy(np, bp, old);
    ga_free(bp);
    return np;
}

void *ga_calloc(size_t n, size_t s)
{
    size_t tot = n * s;
    if (n && tot / n != s) return NULL;      /* overflow */
    void *p = ga_malloc(tot);
    if (p) memset(p, 0, tot);
    return p;
}

/* p is an allocation handed out by this arena? (cross-heap free disambiguation) */
int ga_owns(const void *p)
{
    if (!g_base || !p) return 0;
    return (const u8*)p >= g_base && (const u8*)p < g_base + g_committed;
}

/* Capture bounds for Snapshots: the carved extent [base, brk) holds the header,
 * all block tags and the free list - i.e. the allocator's entire revertible state. */
void ga_bounds(uintptr_t *base, uintptr_t *end)
{
    if (base) *base = (uintptr_t)g_base;
    if (end)  *end  = g_base ? (uintptr_t)H()->brk : 0;
}
