/*
 * snapshot.c - in-session save-states for the native in-process game ("Snapshots").
 *
 * Model (RE-verified):
 *   - The loader is strictly cooperative single-threaded: the game advances only
 *     when sdl_window_pump() calls its vblank/display callbacks. Capture/restore run
 *     at a clean frame boundary (overlay up, or the pump-top headless hook) with no
 *     guest code mid-flight, so a memory snapshot is self-consistent and no CPU
 *     context save is needed (the game re-derives registers from restored RAM).
 *   - We capture ONLY positively-identified guest memory: the revertible guest ARENA
 *     allocator (guest_alloc.c - its whole state lives in the arena, unlike an OS
 *     heap), the guest ELF segments, the guest brk heap, and tracked anon mmap
 *     arenas. NOT the process heap or the AMD driver's GPU buffers (restoring those
 *     crashes the driver), and NOT the ABI stack (the loader's pump runs on it).
 *   - ABC's heap footprint is large (~380 MB), too big for a 32-bit RAM blob, so
 *     snapshots STREAM straight to disk (.uls): region-by-region, no full-size RAM
 *     copy. Slots are files. Restore re-commits each region, writes it back in place
 *     (never relocate - audio Voice.pcm + GL names alias these addresses), and keeps
 *     the GL context alive (volatile VRAM self-heals next frame).
 *   - Peripherals: stop the watchdog; hold the audio lock across the writes so the
 *     mixer can't read torn PCM; reset voices so the game re-issues SEGAAPI playback.
 *
 * Requires the guest arena (ULTRA_GUEST_HEAP=1); otherwise the game's heap is in the
 * shared host CRT heap, which can't be captured/restored safely.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "loader.h"
#include "snapshot.h"

/* Demand-paged substrate registry - the loader's VEH pager (main.c) commits zeroed
 * RWX 4KB pages wherever the game touches unmapped low memory ("scribble anywhere").
 * That memory holds real game data (render/scratch buffers) but isn't an ELF seg,
 * the arena, brk, or a tracked mmap - so it was the missing capture region. The
 * pager calls snap_note_substrate() on every commit; we capture exactly those. */
#define SNAP_SUBSTRATE_MAX 262144
static uint32_t g_sub_pages[SNAP_SUBSTRATE_MAX];
static int      g_sub_n = 0;
void snap_note_substrate(uint32_t page){ if (g_sub_n < SNAP_SUBSTRATE_MAX) g_sub_pages[g_sub_n++] = page; }
static int sub_cmp(const void *a, const void *b){ uint32_t x=*(const uint32_t*)a, y=*(const uint32_t*)b; return x<y?-1:x>y?1:0; }

/* Drain the GPU before capturing/restoring guest memory (boundary discipline):
 * the present path only glFlush'es, so the GPU can still be DMA-reading guest
 * textures/vertex arrays when we overwrite them - a race that makes restore
 * intermittently corrupt/hang. glFinish blocks until the GPU is idle. Called on
 * the pump thread (GL context current). */
static void snap_gl_drain(void)
{
    static void (__stdcall *p_finish)(void) = 0; static int resolved = 0;
    if (!resolved) { resolved = 1; HMODULE g = GetModuleHandleA("opengl32.dll");
                     if (g) p_finish = (void(__stdcall*)(void))GetProcAddress(g, "glFinish"); }
    if (p_finish) p_finish();
}

#define GUEST_LO          ((uintptr_t)0x00010000u)
#define GUEST_HI          ((uintptr_t)0xC0000000u)
#define SNAP_MAX_REGIONS  65536         /* arena + ELF + brk + mmaps + sparse substrate pages */
#define ULS_MAGIC         0x53534C55u   /* 'ULSS' */
#define ULS_VER           4u

static int g_snap_debug = -1;
static int snap_dbg(void){ if (g_snap_debug < 0){ const char*e=getenv("ULTRA_SNAP_DEBUG"); g_snap_debug=e?atoi(e):0; } return g_snap_debug; }

/* Set from the `snapshots` setting (settings.c). When on, the guest runs on the
 * revertible arena and restore is enabled. Off by default = working game untouched. */
int g_ul_snapshots = 0;

static int prot_writable(DWORD p)
{
    DWORD base = p & 0xFF;
    if (p & PAGE_GUARD) return 0;
    if (p & PAGE_WRITECOMBINE) return 0;   /* driver GPU memory - never ours */
    return base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
           base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

typedef struct { uint32_t base; uint32_t size; uint32_t protect; } RegionDesc;
static RegionDesc g_regs[SNAP_MAX_REGIONS];
static int        g_nregs;

/* ---- capture scope (positively-identified guest memory) --------------------- */
typedef struct { uintptr_t base, end; } ScopeRange;
#define SNAP_MAX_SCOPE 32768
static ScopeRange g_scope[SNAP_MAX_SCOPE];
static int        g_nscope;

static void scope_add(uintptr_t base, uintptr_t end)
{
    if (end <= base) return;
    if (g_nscope < SNAP_MAX_SCOPE) { g_scope[g_nscope].base = base; g_scope[g_nscope].end = end; g_nscope++; }
}
static int in_scope(uintptr_t base, size_t size)
{
    uintptr_t e = base + size;
    for (int i = 0; i < g_nscope; i++)
        if (base < g_scope[i].end && g_scope[i].base < e) return 1;
    return 0;
}

/* EXCLUDE list: loader/driver memory that shares the low address space with the
 * game but must NOT be rolled back - the host process heaps (the loader's CRT heap
 * AND any driver bookkeeping that lives in the process heap; reverting it was the
 * original driver crash). Everything else low + committed + writable + private is
 * treated as guest memory and captured (client vertex arrays, scratch buffers,
 * pools the game allocated outside the arena). */
#define SNAP_LOW_BOUND  0x10000000u     /* guest low memory lives below this; driver's big buffers are above */
static ScopeRange g_excl[SNAP_MAX_SCOPE];
static int        g_nexcl;
static void excl_add(uintptr_t base, uintptr_t end)
{
    if (end <= base) return;
    if (g_nexcl < SNAP_MAX_SCOPE) { g_excl[g_nexcl].base = base; g_excl[g_nexcl].end = end; g_nexcl++; }
}
static int in_excl(uintptr_t base, size_t size)
{
    uintptr_t e = base + size;
    for (int i = 0; i < g_nexcl; i++)
        if (base < g_excl[i].end && g_excl[i].base < e) return 1;
    return 0;
}
static void snap_build_scope(int dbg)
{
    g_nscope = 0; g_nexcl = 0;
    size_t heap_bytes = 0;
    /* 1) revertible guest arena (guest_alloc.c) - full heap state in [base,brk) */
    { extern void ga_bounds(uintptr_t*, uintptr_t*); uintptr_t ab = 0, ae = 0;
      ga_bounds(&ab, &ae);
      if (ae > ab) { scope_add(ab, ae); heap_bytes = (size_t)(ae - ab); } }
    /* 2) guest ELF image segments (.data/.bss/GOT) */
    extern LoaderCtx *g_loader_ctx;
    if (g_loader_ctx)
        for (int i = 0; i < g_loader_ctx->image_count; i++) {
            LoadedElf *im = g_loader_ctx->images[i]; if (!im) continue;
            for (int s = 0; s < im->seg_count; s++)
                scope_add(im->segs[s].mapped, im->segs[s].mapped + im->segs[s].memsz);
        }
    /* 3) guest brk heap */
    { uintptr_t bb = 0, bc = 0; extern void snap_get_guest_brk(uintptr_t*, uintptr_t*);
      snap_get_guest_brk(&bb, &bc); if (bc > bb) scope_add(bb, bc); }
    /* 4) guest anonymous mmap arenas */
    { uintptr_t b[512]; uint32_t z[512]; extern int snap_get_guest_mmaps(uintptr_t*, uint32_t*, int);
      int n = snap_get_guest_mmaps(b, z, 512); for (int i = 0; i < n; i++) scope_add(b[i], b[i] + z[i]); }
    /* 5) demand-paged substrate pages (game scribble memory) - sort + coalesce
     *    adjacent 4KB pages into ranges so the page walk captures them. */
    if (g_sub_n > 0) {
        qsort(g_sub_pages, g_sub_n, sizeof(uint32_t), sub_cmp);
        int i = 0;
        while (i < g_sub_n) {
            uintptr_t b = g_sub_pages[i], e = (uintptr_t)g_sub_pages[i] + 0x1000;
            int j = i + 1;
            while (j < g_sub_n && g_sub_pages[j] == (uint32_t)e) { e += 0x1000; j++; }
            scope_add(b, e); i = j;
        }
    }
    /* (the ABI stack is deliberately excluded - the loader pump runs on it) */
    if (dbg) loader_log(LOG_WARN, "[SNAP] scope: %d ranges (arena %.1f MB, %d substrate pages)",
                        g_nscope, heap_bytes / 1048576.0, g_sub_n);
}

/* Enumerate committed writable MEM_PRIVATE pages inside the scope into g_regs. */
static size_t snap_enumerate(int dbg)
{
    snap_build_scope(dbg);
    g_nregs = 0;
    size_t total = 0;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = GUEST_LO;
    while (addr < GUEST_HI) {
        if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof mbi)) break;
        uintptr_t rb = (uintptr_t)mbi.BaseAddress; size_t rs = mbi.RegionSize;
        if (rs == 0) break;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && prot_writable(mbi.Protect)) {
            if (in_scope(rb, rs)) {                                /* positively-identified guest memory only */
                if (g_nregs < SNAP_MAX_REGIONS) {
                    g_regs[g_nregs].base = (uint32_t)rb; g_regs[g_nregs].size = (uint32_t)rs;
                    g_regs[g_nregs].protect = mbi.Protect; g_nregs++; total += rs;
                }
            } else if (dbg && rs >= 0x1000) {                     /* log EVERY skipped committed-writable region */
                loader_log(LOG_WARN, "[SNAP]  SKIP 0x%08zx %7.3f MB prot=0x%lx", rb, rs/1048576.0, (unsigned long)mbi.Protect);
            }
        }
        (void)in_excl;
        addr = rb + rs;
    }
    if (dbg) loader_log(LOG_WARN, "[SNAP] enumerate: %d regions, %.1f MB", g_nregs, total / 1048576.0);
    return total;
}

/* ---- disk streaming --------------------------------------------------------- */
/* vsync = the loader-side glXGetVideoSyncSGI counter; audio_sz = bytes of loader
 * audio-engine voice state appended after the regions. Both are loader-side state
 * the game reads back to pace itself, so they must roll back with the snapshot
 * (else vsync spins / the audio sample count goes garbage and the converter sleds). */
struct ULSHdr { uint32_t magic, ver, nregs, frame, w, h, vsync, audio_sz; };

int snapshot_save_file(const char *path)
{
    int dbg = snap_dbg();
    snap_gl_drain();                                     /* GPU idle before reading guest memory */
    size_t total = snap_enumerate(dbg);
    FILE *f = fopen(path, "wb");
    if (!f) { loader_log(LOG_ERROR, "[SNAP] cannot write %s", path); return -1; }
    extern int g_ul_swap_frame, g_force_w, g_force_h;
    extern uint32_t glx_vsync_get(void);
    extern int audio_state_size(void); extern void audio_state_save(void*);
    int audio_sz = audio_state_size();
    struct ULSHdr h = { ULS_MAGIC, ULS_VER, (uint32_t)g_nregs,
                        (uint32_t)g_ul_swap_frame, (uint32_t)g_force_w, (uint32_t)g_force_h,
                        glx_vsync_get(), (uint32_t)audio_sz };
    fwrite(&h, sizeof h, 1, f);
    for (int i = 0; i < g_nregs; i++) {                 /* stream: desc + raw bytes, region by region */
        fwrite(&g_regs[i], sizeof(RegionDesc), 1, f);
        fwrite((void*)(uintptr_t)g_regs[i].base, 1, g_regs[i].size, f);
    }
    if (audio_sz > 0) {                                 /* loader audio-engine voice state */
        static unsigned char abuf[1 << 16];
        if (audio_sz <= (int)sizeof abuf) { audio_state_save(abuf); fwrite(abuf, 1, audio_sz, f); }
    }
    fclose(f);
    loader_log(LOG_WARN, "[SNAP] saved %s: %d regions, %.1f MB (frame %d)",
               path, g_nregs, total / 1048576.0, h.frame);
    return 0;
}

/* Restore requires the guest arena (the `snapshots` setting / ULTRA_GUEST_HEAP);
 * without it the game ran on the host CRT heap and there's nothing revertible to
 * restore. With it, restore is reliable (GPU drained + audio/vsync/clock rolled
 * back). Dev override: ULTRA_SNAP_UNSAFE=1. */
int snapshot_load_file(const char *path)
{
    if (!g_ul_snapshots && !getenv("ULTRA_SNAP_UNSAFE")) {
        loader_log(LOG_WARN, "[SNAP] restore unavailable: enable Snapshots in settings (needs relaunch).");
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { loader_log(LOG_ERROR, "[SNAP] cannot read %s", path); return -1; }
    struct ULSHdr h;
    if (fread(&h, sizeof h, 1, f) != 1 || h.magic != ULS_MAGIC || h.ver != ULS_VER) {
        loader_log(LOG_ERROR, "[SNAP] %s: bad header", path); fclose(f); return -1;
    }
    extern void watchdog_stop(void), watchdog_start(void);
    extern void audio_lock(void), audio_unlock(void), audio_reset_all_voices(void);
    watchdog_stop();
    snap_gl_drain();                                    /* GPU idle before overwriting guest memory it may be DMA-reading */
    audio_reset_all_voices();                           /* stop the mixer from reading guest PCM so the long disk
                                                         * write needs NO held lock (avoids a multi-second lock-hold
                                                         * starving the audio thread - a source of flaky restores) */
    int wrote = 0; size_t bytes = 0;
    for (uint32_t i = 0; i < h.nregs; i++) {
        RegionDesc r;
        if (fread(&r, sizeof r, 1, f) != 1) break;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)(uintptr_t)r.base, &mbi, sizeof mbi) && mbi.State != MEM_COMMIT)
            VirtualAlloc((LPVOID)(uintptr_t)r.base, r.size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        DWORD oldp = 0;
        VirtualProtect((LPVOID)(uintptr_t)r.base, r.size, PAGE_EXECUTE_READWRITE, &oldp);
        size_t got = fread((void*)(uintptr_t)r.base, 1, r.size, f);  /* stream straight into memory (mixer silent) */
        VirtualProtect((LPVOID)(uintptr_t)r.base, r.size, r.protect, &oldp);
        wrote++; bytes += got;
        if (got != r.size) { loader_log(LOG_ERROR, "[SNAP] %s: short read region %u", path, i); break; }
    }
    /* Roll the loader audio engine back in lockstep (playback positions etc.) so
     * the game's free-buffer math stays sane; falls back to a reset if absent.
     * audio_state_restore takes the lock itself (brief). */
    if (h.audio_sz > 0) {
        static unsigned char abuf[1 << 16];
        if (h.audio_sz <= (uint32_t)sizeof abuf && fread(abuf, 1, h.audio_sz, f) == h.audio_sz) {
            extern void audio_state_restore(const void*); audio_state_restore(abuf);
        }
    }
    { extern void glx_vsync_set(uint32_t); glx_vsync_set(h.vsync); }  /* resync vsync pacing */
    { extern void ul_snap_fence_safe(int); ul_snap_fence_safe(64); }  /* hard-sync NV fences past the desync */
    watchdog_start();
    fclose(f);
    loader_log(LOG_WARN, "[SNAP] loaded %s: %d regions, %.1f MB (frame %d)",
               path, wrote, bytes / 1048576.0, h.frame);
    return 0;
}

/* ---- disk-backed slots ------------------------------------------------------ */
static void slot_path(int slot, char *buf, int cap)
{
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, sizeof exe);
    char *s = strrchr(exe, '\\'); if (s) *s = 0; else exe[0] = 0;
    snprintf(buf, cap, "%s\\ul_snapshot_%d.uls", exe, slot);
}

int snapshot_save(int slot)
{
    if (slot < 0 || slot >= SNAP_NUM_SLOTS) return -1;
    char p[MAX_PATH]; slot_path(slot, p, sizeof p);
    return snapshot_save_file(p);
}
int snapshot_load(int slot)
{
    if (slot < 0 || slot >= SNAP_NUM_SLOTS) return -1;
    char p[MAX_PATH]; slot_path(slot, p, sizeof p);
    return snapshot_load_file(p);
}
int snapshot_slot_used(int slot)
{
    if (slot < 0 || slot >= SNAP_NUM_SLOTS) return 0;
    char p[MAX_PATH]; slot_path(slot, p, sizeof p);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}
void snapshot_slot_info(int slot, int *frame, int *bytes_mb)
{
    if (frame) *frame = 0; if (bytes_mb) *bytes_mb = 0;
    if (slot < 0 || slot >= SNAP_NUM_SLOTS) return;
    char p[MAX_PATH]; slot_path(slot, p, sizeof p);
    FILE *f = fopen(p, "rb"); if (!f) return;
    struct ULSHdr h;
    if (fread(&h, sizeof h, 1, f) == 1 && h.magic == ULS_MAGIC && frame) *frame = (int)h.frame;
    fclose(f);
    if (bytes_mb) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA(p, GetFileExInfoStandard, &fa))
            *bytes_mb = (int)((((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) / 1048576);
    }
}
