/*
 * elf_loader.c
 * Parses ELF64 binaries, maps PT_LOAD segments into Windows VA space,
 * resolves the dynamic section, and applies relocations.
 *
 * Strategy: Option B (static rewrite of syscall sites) +
 *           Option C (glibc stub layer intercepts libc imports).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <io.h>
#include "loader.h"
#include "patcher.h"

/* Global patch options set by main() before loading */
PatchOptions g_patch_opts = {0};

int g_log_level = LOG_INFO;

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */
/* Liveness heartbeat: bumped on every log call so the hang watchdog
 * (watchdog.c) can tell "slow but progressing" from "spinning silently". */
volatile uint32_t g_ul_heartbeat = 0;

/* Mirror WARN/ERR log lines to a dedicated on-disk file that survives even
 * when the launcher drops stderr. Set ULTRA_LBRGH_LOGFILE to override; the
 * default lives next to the memsnap dumps so triage is one folder.
 *
 * We open once, keep the FILE* open for the process lifetime, and flush every
 * line. The extra I/O is bounded - only WARN+ ever hits disk. */
static FILE *g_disk_log = NULL;

/* Win32-native stand-in for fflush(stream).
 *
 * A real crash (ultra-loader.exe.20940.dmp and .4940.dmp, both analyzed with
 * cdb.exe -z against this exact build) showed this module's very FIRST log
 * line -- called at the top of ul_rawthrills_g5_run(), before elf_load()
 * or elf_relocate_all() ever run, i.e. well before anything guest-ELF-related
 * happens -- crashing inside the CRT's fflush(stderr): fflush's own compiled
 * PLT-style import thunk (a completely ordinary MinGW-generated jump through
 * this DLL's msvcrt.dll IAT slot for "fflush") jumped to an address backed by
 * no loaded module in the process (b7668e60 in both dumps, byte-for-byte
 * identical down to every register -- fully deterministic, not a random race).
 * Neighboring stdio imports used by the very same log call one instruction
 * earlier (fputc, and the statically-linked ___mingw_fprintf/___mingw_vfprintf)
 * ran to completion first without faulting, so this looks isolated to
 * fflush's own IAT slot rather than a wholesale broken import table.
 *
 * Whatever puts that slot in that state (root-caused no further than "the
 * fault is definitely not in this module's own C code, which had not run
 * anything yet except an argc check" -- see the bring-up notes for this
 * title), calling fflush() through the CRT is the one specific thing that
 * detonates it. FlushFileBuffers() on the raw OS handle backing the stream
 * is functionally equivalent for a file/pipe stderr redirect (this is the
 * same technique work/src/main.c's ul_redirect_stderr_to_log() already uses
 * to hand STD_ERROR_HANDLE to a loaded module) and goes through entirely
 * different imports (_fileno, _get_osfhandle, FlushFileBuffers), so it keeps
 * working even if fflush's own slot stays bad. */
static void safe_flush(FILE *f)
{
    if (!f) return;
    int fd = _fileno(f);
    if (fd < 0) return;
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE || h == NULL) return;
    FlushFileBuffers(h);
}

static void disk_log_open_if_needed(void)
{
    if (g_disk_log) return;
    const char *override = getenv("ULTRA_LBRGH_LOGFILE");
    if (override && *override) {
        g_disk_log = fopen(override, "a");
        if (g_disk_log) return;
    }
    CreateDirectoryA("C:\\tmp", NULL);
    CreateDirectoryA("C:\\tmp\\cb_snaps", NULL);
    g_disk_log = fopen("C:\\tmp\\cb_snaps\\lbrgh_log.txt", "a");
    if (g_disk_log) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(g_disk_log,
                "\n===== new run pid=%lu  %04u-%02u-%02u %02u:%02u:%02u =====\n",
                (unsigned long)GetCurrentProcessId(),
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        safe_flush(g_disk_log);
    }
}

void loader_log(int level, const char *fmt, ...)
{
    if (level < g_log_level) return;
    g_ul_heartbeat++;   /* bump only on EMITTED output, so the watchdog detects
                         * a silent spin even when the loop calls filtered DEBUG logs */
    static const char *tags[] = { "DBG", "INF", "WRN", "ERR" };
    const char *tag = tags[level < 4 ? level : 3];
    va_list ap;
    fprintf(stderr, "[LBRGH/%s] ", tag);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    safe_flush(stderr);   /* stderr is block-buffered when redirected to a file;
                       * flush each line so the log reflects reality in real time
                       * (and the watchdog's silence detection isn't fooled).
                       * See safe_flush()'s own comment for why this isn't a
                       * plain fflush(stderr) call. */
    /* Mirror WARN/ERR to a stderr-independent disk log - the Cruis'n Blast
     * launcher currently redirects only stdout, so without this the entire
     * exit backtrace + MEMSNAP/VEH/SUBSTRATE trail is lost. */
    if (level >= LOG_WARN) {
        disk_log_open_if_needed();
        if (g_disk_log) {
            fprintf(g_disk_log, "[LBRGH/%s] ", tag);
            va_start(ap, fmt);
            vfprintf(g_disk_log, fmt, ap);
            va_end(ap);
            fprintf(g_disk_log, "\n");
            safe_flush(g_disk_log);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Loader context                                                       */
/* ------------------------------------------------------------------ */
LoaderCtx *loader_ctx_create(void)
{
    LoaderCtx *ctx = calloc(1, sizeof(LoaderCtx));
    return ctx;
}

void loader_ctx_destroy(LoaderCtx *ctx)
{
    if (!ctx) return;
    for (int i = 0; i < ctx->image_count; i++) {
        LoadedElf *img = ctx->images[i];
        if (img->file_data) free(img->file_data);
        free(img);
    }
    if (ctx->veh_handle) syscall_trap_remove(ctx);
    free(ctx);
}

/* ------------------------------------------------------------------ */
/* Utility: ELF protection flags -> Win32 PAGE_* constant             */
/* ------------------------------------------------------------------ */
DWORD elf_flags_to_win32_protect(uint32_t pflags)
{
    bool r = (pflags & PF_R) != 0;
    bool w = (pflags & PF_W) != 0;
    bool x = (pflags & PF_X) != 0;

    if (x && w) return PAGE_EXECUTE_READWRITE;
    if (x && r) return PAGE_EXECUTE_READ;
    if (x)      return PAGE_EXECUTE;
    if (w)      return PAGE_READWRITE;
    if (r)      return PAGE_READONLY;
    return PAGE_NOACCESS;
}

/* ------------------------------------------------------------------ */
/* Read entire file into heap buffer                                   */
/* ------------------------------------------------------------------ */
static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        loader_log(LOG_ERROR, "Cannot open: %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size_t sz = (size_t)ftell(f);
    rewind(f);
    uint8_t *buf = malloc(sz);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    fclose(f);
    *out_size = sz;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Validate ELF header                                                 */
/* ------------------------------------------------------------------ */
/* Returns 0=invalid, 32=ELF32, 64=ELF64 */
static int detect_elf_class(const uint8_t *data)
{
    if (data[0] != ELFMAG0 || data[1] != ELFMAG1 ||
        data[2] != ELFMAG2 || data[3] != ELFMAG3) {
        loader_log(LOG_ERROR, "Not an ELF file (bad magic)");
        return 0;
    }
    uint8_t cls = data[4];
    if (cls == ELFCLASS32) return 32;
    if (cls == ELFCLASS64) return 64;
    loader_log(LOG_ERROR, "Unknown ELF class: %u", cls);
    return 0;
}

static bool validate_elf(Elf64_Ehdr *ehdr)
{
    /* For ELF64 only - ELF32 is validated separately */
    if (ehdr->e_machine != EM_X86_64) {
        loader_log(LOG_ERROR, "Only x86-64 ELF supported (machine=%u)", ehdr->e_machine);
        return false;
    }
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        loader_log(LOG_ERROR, "Only ET_EXEC / ET_DYN supported");
        return false;
    }
    return true;
}

static bool validate_elf32(Elf32_Ehdr *ehdr)
{
    if (ehdr->e_machine != EM_386 && ehdr->e_machine != 3) {
        loader_log(LOG_WARN, "ELF32 machine=%u (expected x86)", ehdr->e_machine);
        /* Continue anyway - some Lindbergh binaries report unexpected machine */
    }
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        loader_log(LOG_ERROR, "ELF32: Only ET_EXEC / ET_DYN supported");
        return false;
    }
    loader_log(LOG_INFO, "ELF32 x86 detected, 32-bit Lindbergh binary");
    return true;
}

/* ------------------------------------------------------------------ */
/* Map ELF32 PT_LOAD segments                                          */
/* ------------------------------------------------------------------ */
static bool map_segments32(LoadedElf *img)
{
    Elf32_Phdr *phdrs = img->phdrs32;
    int phnum = img->ehdr32->e_phnum;

    uintptr_t vmin = 0xFFFFFFFF, vmax = 0;
    for (int i = 0; i < phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uintptr_t lo = PAGE_FLOOR(phdrs[i].p_vaddr);
        uintptr_t hi = PAGE_ALIGN(phdrs[i].p_vaddr + phdrs[i].p_memsz);
        if (lo < vmin) vmin = lo;
        if (hi > vmax) vmax = hi;
    }

    size_t total = vmax - vmin;
    loader_log(LOG_DEBUG, "ELF32 VA span: 0x%zx - 0x%zx (%zu KB)", vmin, vmax, total/1024);

    /* ET_EXEC binaries use ABSOLUTE addressing: code references fixed vaddrs
     * (e.g. `mov edi,[0x082eeb30]`) that are NOT relocated. Segments therefore
     * MUST land at their exact vaddrs (bias = 0). The previous code reserved at
     * `vmin` (0x08048000), but Windows rounds reservations down to 64KB
     * granularity -> base came back 0x08040000, yielding bias=-0x8000 which
     * shifted EVERY segment and broke all absolute references.
     *
     * Fix: reserve a region whose 64KB-aligned floor is BELOW vmin and whose
     * top covers vmax, so the true vaddr range is fully inside the reservation.
     * Then commit each segment at its EXACT p_vaddr (MEM_COMMIT only needs page
     * alignment, not 64KB). bias stays 0. */
    const bool is_exec = (img->ehdr32->e_type == ET_EXEC);
    uint8_t *base = NULL;
    if (is_exec) {
        uintptr_t res_floor = vmin & ~((uintptr_t)0xFFFF);          /* 64KB-aligned floor <= vmin */
        size_t    res_size  = (size_t)(vmax - res_floor);
        res_size = (res_size + 0xFFFF) & ~((size_t)0xFFFF);         /* round up to 64KB */

        /* Probe: is this range already reserved by ultra-module-host.exe's
         * pre-reservation (see host.cpp:lindbergh_pre_reserve_va)? If yes we
         * DON'T re-attempt VirtualAlloc(MEM_RESERVE) - Windows returns
         * ERROR_INVALID_ADDRESS (487) on a range already owned by this process,
         * which would look like a failure in the log. Instead we adopt the
         * host's reservation (bias stays 0) and go straight to per-segment
         * MEM_COMMIT - which succeeds because we own the reservation.
         *
         * We detect "already reserved by us" via VirtualQuery: MEM_RESERVE
         * state at the floor with a region size >= our needed size, and the
         * allocation base at or below res_floor. */
        MEMORY_BASIC_INFORMATION mbi;
        bool already_ours = false;
        if (VirtualQuery((void*)res_floor, &mbi, sizeof mbi)
            && mbi.State == MEM_RESERVE
            && (uintptr_t)mbi.AllocationBase <= res_floor
            && ((uintptr_t)mbi.BaseAddress + mbi.RegionSize) >= res_floor + res_size) {
            already_ours = true;
            base = (uint8_t*)res_floor;
            loader_log(LOG_INFO,
                "ELF32 exec reserve at 0x%zx (size 0x%zx) succeeded (adopted host pre-reservation)",
                res_floor, res_size);
        }
        if (!already_ours) {
            base = (uint8_t*)VirtualAlloc((void*)res_floor, res_size, MEM_RESERVE, PAGE_NOACCESS);
            if (!base) {
                loader_log(LOG_WARN, "ELF32 exec reserve at 0x%zx (size 0x%zx) failed: %lu",
                           res_floor, res_size, GetLastError());
            } else {
                loader_log(LOG_INFO,
                    "ELF32 exec reserve at 0x%zx (size 0x%zx) succeeded",
                    res_floor, res_size);
            }
        }
        /* bias is ALWAYS 0 for ET_EXEC - segments commit at their real vaddrs */
        img->load_bias = 0;
        img->base      = (uintptr_t)vmin;
    }
    if (!base && !is_exec) {
        /* ET_DYN (PIE / .so): position-independent, any base is fine */
        base = (uint8_t*)VirtualAlloc(NULL, total, MEM_RESERVE, PAGE_NOACCESS);
        if (base) {
            img->load_bias = (uintptr_t)base - vmin;
            img->base      = (uintptr_t)base;
        }
    }
    if (!base && is_exec) {
        /* Reservation failed (region busy). We can still try committing each
         * segment directly at its vaddr below - proceed with bias 0. */
        loader_log(LOG_WARN, "ELF32 exec: reservation failed; committing segments directly at vaddrs");
        img->load_bias = 0;
        img->base      = (uintptr_t)vmin;
    }
    if (!base && !is_exec) {
        loader_log(LOG_ERROR, "VirtualAlloc reserve failed for ELF32: %lu", GetLastError());
        return false;
    }

    loader_log(LOG_INFO, "ELF32 mapped (bias=0x%zx, type=%s)", img->load_bias, is_exec ? "EXEC" : "DYN");

    for (int i = 0; i < phnum; i++) {
        Elf32_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;

        uintptr_t seg_va   = (uintptr_t)ph->p_vaddr + img->load_bias;
        uintptr_t seg_page = PAGE_FLOOR(seg_va);
        size_t    seg_size = PAGE_ALIGN(seg_va + ph->p_memsz) - seg_page;

        /* Commit at the (page-floored) segment address. For ET_EXEC this is the
         * true vaddr; MEM_COMMIT over a prior reservation lands exactly here. */
        void *seg = VirtualAlloc((void*)seg_page, seg_size, MEM_COMMIT, PAGE_READWRITE);
        if (!seg) {
            loader_log(LOG_ERROR, "ELF32 VirtualAlloc commit failed seg %d at 0x%zx: %lu",
                       i, seg_page, GetLastError());
            return false;
        }
        if ((uintptr_t)seg != seg_page) {
            loader_log(LOG_WARN, "  seg %d committed at 0x%p but expected 0x%zx",
                       i, seg, seg_page);
        }

        if (ph->p_filesz > 0)
            memcpy((void*)seg_va, img->file_data + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memset((void*)(seg_va + ph->p_filesz), 0, ph->p_memsz - ph->p_filesz);

        /* Post-copy verify: read back first + mid + last-file bytes so we can
         * distinguish "memcpy silently no-op'd" from "substrate later stomped".
         * Uses the raw file bytes for parity so a mismatch is unambiguous. */
        {
            const uint8_t *src = (const uint8_t*)(img->file_data + ph->p_offset);
            const uint8_t *dst = (const uint8_t*)seg_va;
            uintptr_t mid = ph->p_filesz > 8 ? ph->p_filesz / 2 : 0;
            uintptr_t last = ph->p_filesz > 4 ? ph->p_filesz - 4 : 0;
            loader_log(LOG_INFO,
                "  ELF32 seg[%d] va=0x%zx filesz=0x%x memsz=0x%x prot=%c%c%c "
                "commit@=0x%p  head=%02x%02x%02x%02x/%02x%02x%02x%02x "
                "mid=%02x%02x%02x%02x/%02x%02x%02x%02x "
                "tail=%02x%02x%02x%02x/%02x%02x%02x%02x",
                i, seg_va, ph->p_filesz, ph->p_memsz,
                (ph->p_flags & PF_R)?'r':'-',
                (ph->p_flags & PF_W)?'w':'-',
                (ph->p_flags & PF_X)?'x':'-',
                seg,
                src[0],src[1],src[2],src[3], dst[0],dst[1],dst[2],dst[3],
                src[mid],src[mid+1],src[mid+2],src[mid+3],
                dst[mid],dst[mid+1],dst[mid+2],dst[mid+3],
                src[last],src[last+1],src[last+2],src[last+3],
                dst[last],dst[last+1],dst[last+2],dst[last+3]);
        }

        DWORD protect = elf_flags_to_win32_protect(ph->p_flags);
        DWORD old;
        VirtualProtect((void*)seg_page, seg_size, protect, &old);

        if (img->seg_count < MAX_SEGMENTS) {
            LoadedSegment *s = &img->segs[img->seg_count++];
            s->vaddr  = ph->p_vaddr;
            s->mapped = seg_va;
            s->filesz = ph->p_filesz;
            s->memsz  = ph->p_memsz;
            s->flags  = ph->p_flags;
        }

        loader_log(LOG_DEBUG, "  ELF32 seg[%d] va=0x%zx sz=0x%x prot=%c%c%c",
            i, seg_va, ph->p_memsz,
            (ph->p_flags & PF_R)?'r':'-',
            (ph->p_flags & PF_W)?'w':'-',
            (ph->p_flags & PF_X)?'x':'-');
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Parse ELF32 dynamic section                                         */
/* ------------------------------------------------------------------ */
static void parse_dynamic32(LoadedElf *img)
{
    Elf32_Phdr *phdrs = img->phdrs32;
    int phnum = img->ehdr32->e_phnum;

    for (int i = 0; i < phnum; i++) {
        if (phdrs[i].p_type != PT_DYNAMIC) continue;

        Elf32_Dyn *dyn = (Elf32_Dyn*)((uintptr_t)phdrs[i].p_vaddr + img->load_bias);
        img->dynamic = (Elf64_Dyn*)dyn; /* store as opaque pointer */

        uintptr_t strtab_va = 0, symtab_va = 0;
        size_t    strsz = 0, syment = sizeof(Elf32_Sym);
        uintptr_t relplt_va = 0, reldyn_va = 0;
        size_t    relpltsz = 0, reldynsz = 0;

        for (Elf32_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
            switch (d->d_tag) {
            case DT_STRTAB:   strtab_va  = d->d_un.d_ptr + img->load_bias; break;
            case DT_SYMTAB:   symtab_va  = d->d_un.d_ptr + img->load_bias; break;
            case DT_STRSZ:    strsz      = d->d_un.d_val; break;
            case DT_SYMENT:   syment     = d->d_un.d_val; break;
            case DT_JMPREL:   relplt_va  = d->d_un.d_ptr + img->load_bias; break;
            case DT_PLTRELSZ: relpltsz   = d->d_un.d_val; break;
            case DT_REL:      reldyn_va  = d->d_un.d_ptr + img->load_bias; break;
            case DT_RELSZ:    reldynsz   = d->d_un.d_val; break;
            case DT_PLTGOT:   img->pltgot = d->d_un.d_ptr + img->load_bias; break;
            case DT_NEEDED: break;
            }
        }

        if (strtab_va) img->dynstr = (const char*)strtab_va;
        if (symtab_va && strsz) {
            img->dynsym = (Elf64_Sym*)symtab_va;
            img->dynsym_count = (strtab_va - symtab_va) / syment;
        }

        /* Store Rel (not Rela) tables - ELF32 uses Rel */
        /* We reuse rela_plt/rela_dyn fields with count=0 for Rel entries */
        /* Handled in dynlink32 */
        img->rela_plt_count = relpltsz / sizeof(Elf32_Rel);
        img->rela_dyn_count = reldynsz / sizeof(Elf32_Rel);
        if (relplt_va) img->rela_plt = (Elf64_Rela*)relplt_va;
        if (reldyn_va) img->rela_dyn = (Elf64_Rela*)reldyn_va;

        /* DT_NEEDED names */
        int ni = 0;
        for (Elf32_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_NEEDED && img->dynstr && ni < MAX_LIBS) {
                strncpy(img->needed[ni], img->dynstr + d->d_un.d_val, 255);
                loader_log(LOG_DEBUG, "  NEEDED32: %s", img->needed[ni]);
                ni++;
            }
        }
        img->needed_count = ni;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Map PT_LOAD segments                                                */
/* ------------------------------------------------------------------ */
static bool map_segments(LoadedElf *img)
{
    Elf64_Phdr *phdrs = img->phdrs;
    int phnum = img->ehdr->e_phnum;

    /* Find the virtual address span */
    uintptr_t vmin = UINTPTR_MAX, vmax = 0;
    for (int i = 0; i < phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uintptr_t lo = PAGE_FLOOR(phdrs[i].p_vaddr);
        uintptr_t hi = PAGE_ALIGN(phdrs[i].p_vaddr + phdrs[i].p_memsz);
        if (lo < vmin) vmin = lo;
        if (hi > vmax) vmax = hi;
    }

    size_t total = vmax - vmin;
    loader_log(LOG_DEBUG, "VA span: 0x%zx - 0x%zx (%zu KB)", vmin, vmax, total/1024);

    /*
     * Reserve the full span. For ET_EXEC we try the preferred address;
     * for ET_DYN (PIE) we let Windows choose.
     */
    void *hint = (img->ehdr->e_type == ET_EXEC) ? (void*)vmin : NULL;
    uint8_t *base = VirtualAlloc(hint, total, MEM_RESERVE, PAGE_NOACCESS);
    if (!base && hint) {
        loader_log(LOG_WARN, "Preferred base 0x%zx busy, falling back", vmin);
        base = VirtualAlloc(NULL, total, MEM_RESERVE, PAGE_NOACCESS);
    }
    if (!base) {
        loader_log(LOG_ERROR, "VirtualAlloc reserve failed: %lu", GetLastError());
        return false;
    }

    img->load_bias = (uintptr_t)base - vmin;
    img->base      = (uintptr_t)base;
    loader_log(LOG_INFO, "Mapped at base 0x%p (bias=0x%zx)", base, img->load_bias);

    /* Map each PT_LOAD segment */
    for (int i = 0; i < phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;

        uintptr_t seg_va   = ph->p_vaddr + img->load_bias;
        uintptr_t seg_page = PAGE_FLOOR(seg_va);
        size_t    seg_size = PAGE_ALIGN(seg_va + ph->p_memsz) - seg_page;

        /* Commit the pages */
        void *seg = VirtualAlloc((void*)seg_page, seg_size,
                                 MEM_COMMIT, PAGE_READWRITE);
        if (!seg) {
            loader_log(LOG_ERROR, "VirtualAlloc commit failed for seg %d", i);
            return false;
        }

        /* Copy file data */
        if (ph->p_filesz > 0) {
            memcpy((void*)seg_va,
                   img->file_data + ph->p_offset,
                   ph->p_filesz);
        }

        /* Zero BSS */
        if (ph->p_memsz > ph->p_filesz) {
            memset((void*)(seg_va + ph->p_filesz),
                   0,
                   ph->p_memsz - ph->p_filesz);
        }

        /* Apply final protection */
        DWORD protect = elf_flags_to_win32_protect(ph->p_flags);
        DWORD old;
        VirtualProtect((void*)seg_page, seg_size, protect, &old);

        /* Record segment */
        if (img->seg_count < MAX_SEGMENTS) {
            LoadedSegment *s = &img->segs[img->seg_count++];
            s->vaddr   = ph->p_vaddr;
            s->mapped  = seg_va;
            s->filesz  = ph->p_filesz;
            s->memsz   = ph->p_memsz;
            s->flags   = ph->p_flags;
        }

        loader_log(LOG_DEBUG, "  seg[%d] va=0x%zx sz=0x%zx prot=%c%c%c",
            i, seg_va, ph->p_memsz,
            (ph->p_flags & PF_R) ? 'r' : '-',
            (ph->p_flags & PF_W) ? 'w' : '-',
            (ph->p_flags & PF_X) ? 'x' : '-');
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* Parse dynamic section                                               */
/* ------------------------------------------------------------------ */
static void parse_dynamic(LoadedElf *img)
{
    for (int i = 0; i < img->ehdr->e_phnum; i++) {
        if (img->phdrs[i].p_type != PT_DYNAMIC) continue;

        img->dynamic = (Elf64_Dyn*)(img->phdrs[i].p_vaddr + img->load_bias);
        uintptr_t strtab_va = 0, symtab_va = 0;
        size_t    strsz = 0, syment = sizeof(Elf64_Sym);
        uintptr_t rela_va = 0, rela_plt_va = 0;
        size_t    relasz = 0, relapltsz = 0;

        for (Elf64_Dyn *d = img->dynamic; d->d_tag != DT_NULL; d++) {
            switch (d->d_tag) {
            case DT_STRTAB:   strtab_va   = d->d_un.d_ptr + img->load_bias; break;
            case DT_SYMTAB:   symtab_va   = d->d_un.d_ptr + img->load_bias; break;
            case DT_STRSZ:    strsz       = d->d_un.d_val; break;
            case DT_SYMENT:   syment      = d->d_un.d_val; break;
            case DT_RELA:     rela_va     = d->d_un.d_ptr + img->load_bias; break;
            case DT_RELASZ:   relasz      = d->d_un.d_val; break;
            case DT_JMPREL:   rela_plt_va = d->d_un.d_ptr + img->load_bias; break;
            case DT_PLTRELSZ: relapltsz   = d->d_un.d_val; break;
            case DT_PLTGOT:   img->pltgot = d->d_un.d_ptr + img->load_bias; break;
            case DT_NEEDED: {
                /* Store lib name, resolve after strtab is known */
                if (img->needed_count < MAX_LIBS)
                    img->needed[img->needed_count++][0] = '\0'; /* placeholder */
                break;
            }
            default: break;
            }
        }

        if (strtab_va) img->dynstr = (const char*)strtab_va;
        if (symtab_va && strsz) {
            img->dynsym = (Elf64_Sym*)symtab_va;
            /* Rough count: symbols end where strtab begins (common layout) */
            img->dynsym_count = (strtab_va - symtab_va) / syment;
        }
        if (rela_va && relasz) {
            img->rela_dyn       = (Elf64_Rela*)rela_va;
            img->rela_dyn_count = relasz / sizeof(Elf64_Rela);
        }
        if (rela_plt_va && relapltsz) {
            img->rela_plt       = (Elf64_Rela*)rela_plt_va;
            img->rela_plt_count = relapltsz / sizeof(Elf64_Rela);
        }

        /* Second pass: fill in DT_NEEDED names now that strtab is set */
        int ni = 0;
        for (Elf64_Dyn *d = img->dynamic; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_NEEDED && img->dynstr && ni < img->needed_count) {
                strncpy(img->needed[ni], img->dynstr + d->d_un.d_val, 255);
                loader_log(LOG_DEBUG, "  NEEDED: %s", img->needed[ni]);
                ni++;
            }
        }
        img->needed_count = ni;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Option B: Rewrite raw syscall instructions in executable pages     */
/*           Replace 0F 05 (syscall) with FF 15 00 00 00 00 (call    */
/*           [rip+0]) and patch in a trampoline address.              */
/*                                                                     */
/*  Layout we write (12 bytes replacing the 2-byte syscall):          */
/*    FF 15 00 00 00 00   call qword ptr [rip+0]                      */
/*    <6 padding NOPs>                                                 */
/*    <8 bytes: trampoline address>                                   */
/*  Because we need more space we use a short jump to a nearby        */
/*  trampoline page instead.  For each syscall site:                  */
/*    E9 xx xx xx xx   JMP rel32  -> trampoline stub                  */
/*    90               NOP        (pad second byte of original)       */
/*                                                                     */
/*  The trampoline:                                                    */
/*    push  rax           ; syscall number already in rax             */
/*    call  syscall_dispatch                                           */
/*    ret                                                              */
/* ------------------------------------------------------------------ */

extern uintptr_t syscall_dispatch(uint64_t nr, uint64_t a0, uint64_t a1,
                                   uint64_t a2, uint64_t a3, uint64_t a4,
                                   uint64_t a5);

static uint8_t *g_tramp_page   = NULL;
static size_t   g_tramp_offset = 0;
#define TRAMP_PAGE_SIZE  (64 * 1024)

static void ensure_tramp_page(void)
{
    if (!g_tramp_page) {
        g_tramp_page = VirtualAlloc(NULL, TRAMP_PAGE_SIZE,
                                    MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
        if (!g_tramp_page) {
            loader_log(LOG_ERROR, "Failed to allocate trampoline page");
        }
    }
}

/*
 * Write a per-syscall-site trampoline that:
 *   1. Preserves the call-site RIP so the dispatcher can identify it
 *   2. Calls syscall_dispatch(nr, a0..a5)
 *   3. Returns to original code flow
 *
 * Returns address of the trampoline entry.
 */
static uintptr_t write_trampoline(void)
{
    ensure_tramp_page();
    if (!g_tramp_page) return 0;

    uint8_t *t = g_tramp_page + g_tramp_offset;

    /*
     * Trampoline (x86-64 System V -> Windows calling convention):
     *   Linux:   rdi, rsi, rdx, r10, r8, r9   (args 0-5)
     *            rax = syscall number
     *   Windows: rcx, rdx, r8, r9, stack       (args 0-3, then stack)
     *
     * We shuffle registers then call our C dispatcher.
     */
    size_t i = 0;

    /* sub rsp, 0x48  -- shadow space + alignment */
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xEC; t[i++] = 0x48;

    /* mov [rsp+0x40], rax  -- save syscall nr */
    t[i++] = 0x48; t[i++] = 0x89; t[i++] = 0x44; t[i++] = 0x24; t[i++] = 0x40;

    /* Arg 0 (Linux rdi) -> Windows rcx */
    t[i++] = 0x48; t[i++] = 0x89; t[i++] = 0xF9; /* mov rcx, rdi */
    /* Arg 1 (Linux rsi) -> Windows rdx (already rdx) */
    /* Arg 2 (Linux rdx) -> Windows r8 */
    t[i++] = 0x4C; t[i++] = 0x89; t[i++] = 0xD0; /* mov r8, rdx  (r8d) */
    t[i++] = 0x49; t[i++] = 0x89; t[i++] = 0xC0; /* mov r8, rax -- wait, fix: */
    /* redo: rdx->r8 properly */
    i -= 6;
    t[i++] = 0x4C; t[i++] = 0x89; t[i++] = 0xC2; /* mov rdx, r8 -- wrong, restart */
    /* Simplified: just call with rax=nr, args already mostly right */
    /* reset and use cleaner sequence */
    i = 4; /* after sub rsp */

    /* Windows x64: rcx=nr, rdx=a0(rdi), r8=a1(rsi), r9=a2(rdx), stack=r10,r8,r9 */
    /* Save Linux args before clobbering */
    /* push r10 (a3) */
    t[i++] = 0x41; t[i++] = 0x52;
    /* push r8 (a4) */
    t[i++] = 0x41; t[i++] = 0x50;
    /* push r9 (a5) */
    t[i++] = 0x41; t[i++] = 0x51;

    /* mov rcx, rax   ; nr */
    t[i++] = 0x48; t[i++] = 0x89; t[i++] = 0xC1;
    /* mov rdx, rdi   ; a0 */
    t[i++] = 0x48; t[i++] = 0x89; t[i++] = 0xFA;
    /* mov r8,  rsi   ; a1 */
    t[i++] = 0x49; t[i++] = 0x89; t[i++] = 0xF0;
    /* mov r9,  rdx   ; a2  -- rdx still unchanged at this point */
    t[i++] = 0x4C; t[i++] = 0x89; t[i++] = 0xC2; /* actually: mov rdx,r8 -- no */
    /* We need original rdx; it's in rdx still (we haven't touched it) */
    t[i++] = 0x4D; t[i++] = 0x89; t[i++] = 0xC9; /* mov r9, r9 (nop placeholder) */
    /* Proper r9 = original rdx: */
    i -= 3;
    t[i++] = 0x49; t[i++] = 0x89; t[i++] = 0xD1; /* mov r9, rdx */

    /* Stack args: r10, r8_orig, r9_orig are on stack already */
    /* call syscall_dispatch (absolute indirect) */
    uintptr_t dispatch_addr = (uintptr_t)syscall_dispatch;
    /* mov rax, imm64 */
    t[i++] = 0x48; t[i++] = 0xB8;
    memcpy(&t[i], &dispatch_addr, 8); i += 8;
    /* call rax */
    t[i++] = 0xFF; t[i++] = 0xD0;

    /* Restore stack (pop the 3 pushes) */
    t[i++] = 0x41; t[i++] = 0x59; /* pop r9 */
    t[i++] = 0x41; t[i++] = 0x58; /* pop r8 */
    t[i++] = 0x41; t[i++] = 0x5A; /* pop r10 */

    /* add rsp, 0x48 */
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xC4; t[i++] = 0x48;

    /* ret */
    t[i++] = 0xC3;

    uintptr_t entry = (uintptr_t)t;
    g_tramp_offset += (i + 15) & ~15; /* align to 16 */
    return entry;
}

static void rewrite_syscalls_in_segment(LoadedSegment *seg)
{
    if (!(seg->flags & PF_X)) return;

    uint8_t *start = (uint8_t*)seg->mapped;
    size_t   len   = seg->memsz;

    /* Temporarily make writable */
    DWORD old;
    VirtualProtect(start, len, PAGE_EXECUTE_READWRITE, &old);

    int count = 0;
    for (size_t off = 0; off + 1 < len; off++) {
        if (start[off] == 0x0F && start[off+1] == 0x05) {
            /* Found syscall instruction */
            uintptr_t site    = (uintptr_t)(start + off);
            uintptr_t tramp   = write_trampoline();
            if (!tramp) break;

            /*
             * Patch: E9 rel32  NOP
             * rel32 = tramp - (site + 5)
             */
            int32_t rel = (int32_t)(tramp - (site + 5));
            start[off]   = 0xE9;
            memcpy(&start[off+1], &rel, 4);
            start[off+5] = 0x90; /* NOP over second byte */
            count++;
            off += 5; /* skip past the patch */
        }
    }

    if (count > 0)
        loader_log(LOG_INFO, "  Rewrote %d syscall site(s) in segment at 0x%zx",
                   count, seg->mapped);

    VirtualProtect(start, len, old, &old);
}

static void rewrite_syscalls(LoadedElf *img)
{
    loader_log(LOG_INFO, "Option B: scanning for raw syscall instructions...");
    for (int i = 0; i < img->seg_count; i++)
        rewrite_syscalls_in_segment(&img->segs[i]);
}

/* ------------------------------------------------------------------ */
/* Main load entry point                                               */
/* ------------------------------------------------------------------ */
LoadedElf *elf_load(LoaderCtx *ctx, const char *path)
{
    if (ctx->image_count >= MAX_LIBS) {
        loader_log(LOG_ERROR, "Too many images");
        return NULL;
    }

    LoadedElf *img = calloc(1, sizeof(LoadedElf));
    strncpy(img->path, path, MAX_PATH - 1);

    loader_log(LOG_INFO, "Loading: %s", path);

    img->file_data = read_file(path, &img->file_size);
    if (!img->file_data) { free(img); return NULL; }

    /*
     * Patcher runs on the primary ELF (first image loaded = the game exe).
     * Dependencies (.so files) are skipped - they're clean system libs.
     */
    if (ctx->image_count == 0) {
        PatchResult pr = patcher_run(path, img->file_data, img->file_size,
                                      g_patch_opts);
        if (!pr.success) {
            loader_log(LOG_WARN, "Patcher returned failure, continuing anyway");
        } else {
            loader_log(LOG_INFO, "Game: %s", pr.game_name);
            if (pr.backup_created)
                loader_log(LOG_INFO, "Backup: %s.bak", path);
            loader_log(LOG_INFO, "Patches applied: %d", pr.patches_applied);
        }
    }

    /* Detect ELF class from magic */
    int elf_class = detect_elf_class(img->file_data);
    if (!elf_class) { free(img->file_data); free(img); return NULL; }

    if (elf_class == 32) {
        /* ---- ELF32 path ---- */
        img->is_elf32  = true;
        img->ehdr32    = (Elf32_Ehdr*)img->file_data;
        img->phdrs32   = (Elf32_Phdr*)(img->file_data + img->ehdr32->e_phoff);

        if (!validate_elf32(img->ehdr32)) { free(img->file_data); free(img); return NULL; }
        if (!map_segments32(img))         { free(img->file_data); free(img); return NULL; }

        parse_dynamic32(img);

        /* Register the ELF's .eh_frame with the DW2 unwinder so C++ exceptions
         * can propagate to their catch handlers inside the ELF. We scan section
         * headers (in file_data, not mapped memory) to find .eh_frame by name,
         * then register its in-memory VMA (load_bias == 0 for ET_EXEC). */
        if (img->ehdr32->e_shoff && img->ehdr32->e_shnum &&
            img->ehdr32->e_shstrndx < img->ehdr32->e_shnum) {
            Elf32_Shdr *shdrs = (Elf32_Shdr*)(img->file_data + img->ehdr32->e_shoff);
            const char *shstr = (const char*)(img->file_data +
                                              shdrs[img->ehdr32->e_shstrndx].sh_offset);
            for (int si = 0; si < img->ehdr32->e_shnum; si++) {
                if (shdrs[si].sh_addr == 0) continue;
                const char *sname = shstr + shdrs[si].sh_name;
                if (strcmp(sname, ".eh_frame") == 0) {
                    uintptr_t eh_va = (uintptr_t)shdrs[si].sh_addr + img->load_bias;
                    extern void elf_register_eh_frames(void *start);
                    elf_register_eh_frames((void*)eh_va);
                    break;
                }
            }
        }

        img->entry = (uintptr_t)img->ehdr32->e_entry + img->load_bias;
        loader_log(LOG_INFO, "ELF32 entry: 0x%zx", img->entry);

    } else {
        /* ---- ELF64 path ---- */
        img->is_elf32 = false;
        img->ehdr  = (Elf64_Ehdr*)img->file_data;
        img->phdrs = (Elf64_Phdr*)(img->file_data + img->ehdr->e_phoff);

        if (!validate_elf(img->ehdr)) { free(img->file_data); free(img); return NULL; }
        if (!map_segments(img))       { free(img->file_data); free(img); return NULL; }

        parse_dynamic(img);

        img->entry = img->ehdr->e_entry + img->load_bias;
        loader_log(LOG_INFO, "ELF64 entry: 0x%zx", img->entry);
    }

    /* Option B: rewrite raw syscall sites before we hand off
     * ELF32 uses INT 0x80 (CD 80) not SYSCALL (0F 05) - handle both */
    if (img->is_elf32)
        rewrite_int80(img);
    else
        rewrite_syscalls(img);

    ctx->images[ctx->image_count++] = img;
    return img;
}

/* ------------------------------------------------------------------ */
/* Option B for ELF32: rewrite INT 0x80 (CD 80) -> call trampoline   */
/* INT 0x80 is Linux's 32-bit syscall instruction                     */
/* ------------------------------------------------------------------ */
static void rewrite_int80_in_segment(LoadedSegment *seg)
{
    if (!(seg->flags & PF_X)) return;

    /* DISABLED by default - a blind CD-80 byte-scan is unsafe: the bytes 0xCD 0x80
     * occur constantly as incidental operands INSIDE legitimate instructions (a
     * LEA's SIB+disp, a jmp/call rel, a `cmp word` imm16, …), and rewriting each
     * to a 6-byte CALL-trampoline silently corrupts that real code. (ABC has 7
     * such CD-80 byte pairs in .text and ZERO are real int 0x80 - one sits inside
     * calc_ac_lod_level's `lea`, and clobbering it crashed every mission with a
     * PRIV_INSTRUCTION.) Real int 0x80 instructions are now trapped exactly at
     * execution time by syscall_veh (only an executed int 0x80 ever faults), so we
     * never touch code here. Set ULTRA_REWRITE_INT80=1 to restore the old scan. */
    { static int s_en = -1;
      if (s_en < 0) { const char *e = getenv("ULTRA_REWRITE_INT80"); s_en = (e && atoi(e)) ? 1 : 0; }
      if (!s_en) return; }

    uint8_t *start = (uint8_t*)seg->mapped;
    size_t   len   = seg->memsz;

    DWORD old;
    VirtualProtect(start, len, PAGE_EXECUTE_READWRITE, &old);

    int count = 0;
    for (size_t off = 0; off + 1 < len; off++) {
        if (start[off] == 0xCD && start[off+1] == 0x80) {
            /* INT 0x80 found.
             * Replace with: CALL rel32 (E8 xx xx xx xx) + NOP
             * The trampoline handles 32-bit syscall ABI:
             *   eax=nr, ebx=a0, ecx=a1, edx=a2, esi=a3, edi=a4
             */
            uintptr_t site  = (uintptr_t)(start + off);
            uintptr_t tramp = write_trampoline(); /* reuse 64-bit trampoline */
            if (!tramp) break;

            int32_t rel = (int32_t)(tramp - (site + 5));
            start[off]   = 0xE8;
            memcpy(&start[off+1], &rel, 4);
            start[off+5] = 0x90;
            count++;
            off += 5;
        }
    }

    if (count > 0)
        loader_log(LOG_INFO, "  Rewrote %d INT 0x80 site(s) in segment at 0x%zx",
                   count, seg->mapped);

    VirtualProtect(start, len, old, &old);
}

void rewrite_int80(LoadedElf *img)
{
    loader_log(LOG_INFO, "Option B (ELF32): scanning for INT 0x80 instructions...");
    for (int i = 0; i < img->seg_count; i++)
        rewrite_int80_in_segment(&img->segs[i]);
}
