/*
 * dynlink.c
 * Symbol table management, dependency resolution, and relocation application.
 * This is Option C: instead of loading real libcs, we redirect imports to our
 * stub implementations that call Win32 underneath.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "loader.h"
#include "thunk32.h"

/* ---- GOT-slot <-> symbol map for real-time repair -------------------- */
typedef struct { uint32_t slot; uint32_t resolved; const char *name; } SlotMap;
#define SLOTMAP_MAX 4096
static SlotMap s_slotmap[SLOTMAP_MAX];
static int     s_slotmap_count = 0;

/* Persistent, own-module string arena for symbol names recorded from the
 * guest's dynstr. The original design stored the raw dynstr pointer, which
 * is only stable while the guest's mapped segment stays exactly where the
 * loader put it - a VA-reservation failure, a later remap, or any protection
 * flip on the segment can turn that pointer into garbage. When the NULL-GOT
 * trap fires and prints the name it reads back mojibake ("����j" and friends).
 *
 * Fix: at record time, copy the name into a bump-allocator arena we own
 * (VirtualAlloc'd, never freed, never remapped, never inside the guest ELF).
 * The trap thunk's PUSH imm32 therefore holds a pointer into our own memory,
 * and the dispatcher can print it directly. Also survives the case where the
 * guest overwrites/scribbles its own dynstr region. */
#define NAME_ARENA_CHUNK 0x10000   /* 64 KB per arena chunk */
static char    *g_name_arena       = NULL;
static uint32_t g_name_arena_off   = 0;
static uint32_t g_name_arena_size  = 0;

/* MinGW-safe page-level readability probe. MinGW GCC (i686-w64-mingw32) does
 * NOT ship the SEH __try/__except keywords, so we can't wrap a raw pointer
 * dereference in a language-level try. Instead, ask the kernel what state
 * the pages backing [p, p+n) are in: if any page is uncommitted or
 * PAGE_NOACCESS, the read would fault. */
static int ptr_is_readable(const void *p, size_t n)
{
    if (!p || !n) return 0;
    const uint8_t *cur = (const uint8_t*)p;
    const uint8_t *end = cur + n;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(cur, &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT) return 0;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        /* Any of the read-capable protections is fine. */
        DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & readable) == 0) return 0;
        const uint8_t *region_end = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        cur = (region_end > cur) ? region_end : cur + 1;
    }
    return 1;
}

static const char *arena_strdup(const char *src)
{
    if (!src) return NULL;
    /* Bound the copied length so a corrupt dynstr pointer doesn't walk off a
     * page. 512 is well over the longest real ELF symbol name (mangled C++
     * ctors on Lindbergh top out around 300). */
    /* Cheap first-byte probe: if the source page isn't readable at all,
     * bail before we scan for the NUL. */
    if (!ptr_is_readable(src, 1)) return NULL;
    size_t len = 0;
    while (len < 512) {
        /* Re-probe when crossing a page (only when advancing into a new page). */
        if (((uintptr_t)(src + len) & 0xFFF) == 0 && !ptr_is_readable(src + len, 1))
            break;
        if (src[len] == '\0') break;
        len++;
    }

    size_t need = len + 1;
    if (g_name_arena_off + need > g_name_arena_size) {
        size_t sz = (need > NAME_ARENA_CHUNK) ? need : NAME_ARENA_CHUNK;
        void *p = VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p) return NULL;
        g_name_arena      = (char*)p;
        g_name_arena_off  = 0;
        g_name_arena_size = (uint32_t)sz;
    }
    char *dst = g_name_arena + g_name_arena_off;
    memcpy(dst, src, len);
    dst[len] = '\0';
    g_name_arena_off += (uint32_t)need;
    return dst;
}

void dynlink_record_slot(uint32_t slot_addr, const char *name, uint32_t resolved)
{
    if (s_slotmap_count >= SLOTMAP_MAX) return;
    /* Copy the caller's dynstr pointer into our persistent arena. From here
     * on the slotmap and every downstream consumer (log lines, PUSH imm32 in
     * the NULL-GOT trap thunk, slotmap_enumerate for the GL binder) see a
     * pointer into memory WE own. Even if the guest ELF later unmaps its
     * dynstr segment, the strings survive. */
    const char *stable = arena_strdup(name);
    if (!stable) stable = name;   /* fall back to raw pointer if arena is exhausted */
    for (int i = 0; i < s_slotmap_count; i++)
        if (s_slotmap[i].slot == slot_addr) { s_slotmap[i].name = stable; s_slotmap[i].resolved = resolved; return; }
    s_slotmap[s_slotmap_count].slot     = slot_addr;
    s_slotmap[s_slotmap_count].name     = stable;
    s_slotmap[s_slotmap_count].resolved = resolved;
    s_slotmap_count++;
}

const char *dynlink_symbol_for_got_slot(uint32_t slot_addr)
{
    for (int i = 0; i < s_slotmap_count; i++)
        if (s_slotmap[i].slot == slot_addr) return s_slotmap[i].name;
    return NULL;
}

/* Iterate the recorded JUMP_SLOT GOT slots (slot address + symbol name).
 * Used by the generic GL GOT binder to discover every gl / glX / glut import
 * in the loaded ELF without any hardcoded per-game address table.
 * Returns the number of entries; fills the caller arrays up to 'max'. */
int dynlink_slotmap_enumerate(uint32_t *out_slots, const char **out_names, int max)
{
    int n = 0;
    for (int i = 0; i < s_slotmap_count && n < max; i++) {
        if (out_slots) out_slots[n] = s_slotmap[i].slot;
        if (out_names) out_names[n] = s_slotmap[i].name;
        n++;
    }
    return n;
}

uint32_t dynlink_resolve_name(const char *name)
{
    extern LoaderCtx *g_loader_ctx;
    if (!g_loader_ctx || !name) return 0;
    uintptr_t v = sym_resolve(g_loader_ctx, name);
    if (v > 0xFFFFFFFFULL) {                 /* 64-bit host addr -> thunk        */
        uint32_t t = thunk32_lookup(v);
        if (!t) t = thunk32_register(v, name);
        v = t;
    }
    return (uint32_t)v;
}

/* ------------------------------------------------------------------ */
/* NULL-GOT trap: per-symbol trampoline for unresolved PLT entries.    */
/*                                                                      */
/* After elf_relocate_all(), any PLT slot whose symbol didn't resolve is */
/* still zero. If the guest calls it, EIP=0 and the crash log shows a    */
/* useless "access violation at 0x00000000" with no clue what symbol was */
/* missing. To make these crashes self-diagnosing:                      */
/*                                                                      */
/*   1. Walk the slotmap (populated during R_386_JMP_SLOT relocation).  */
/*   2. For each slot with resolved==0, allocate a 12-byte thunk in an  */
/*      RX page. The thunk pushes the symbol name pointer and jumps to  */
/*      the common dispatcher below.                                    */
/*   3. Patch the GOT slot to point at its per-slot thunk.              */
/*                                                                      */
/* When the guest hits an unresolved import, we log the EXACT symbol    */
/* name and ExitProcess(1) instead of crashing at EIP=0.                */
/* ------------------------------------------------------------------ */

/* Common dispatcher - takes the symbol name pushed by the per-slot thunk.
 * cdecl / stdcall don't matter here: we never return.
 *
 * Robustness: even though dynlink_record_slot copies the name into our own
 * arena, defend against the case where the arena was exhausted at record
 * time and we fell back to the raw guest dynstr pointer (whose backing
 * segment may have been remapped/unmapped by now). If the pushed pointer
 * isn't readable, or reads back non-ASCII bytes at offset 0, print a hex
 * dump of the pointer value itself so the log still identifies which slot
 * fired rather than dumping mojibake. */
__attribute__((noreturn))
static void null_got_trap_dispatch(const char *sym_name)
{
    char safe[520];
    const char *out = "<unknown>";

    if (sym_name) {
        /* Probe readability at page granularity via VirtualQuery (MinGW GCC
         * doesn't ship MS SEH, so we can't use __try/__except here). */
        int readable = ptr_is_readable(sym_name, 1);
        int printable_head = 0;
        if (readable) {
            /* Real ELF symbol names start with printable ASCII. Anything
             * outside 0x20..0x7E on the first byte means we're staring at
             * unmapped/scrambled memory. */
            unsigned char c0 = (unsigned char)sym_name[0];
            printable_head = (c0 >= 0x20 && c0 <= 0x7E);
        }

        if (readable && printable_head) {
            size_t n = 0;
            while (n < sizeof(safe) - 1) {
                if (((uintptr_t)(sym_name + n) & 0xFFF) == 0 &&
                    !ptr_is_readable(sym_name + n, 1))
                    break;
                unsigned char c = (unsigned char)sym_name[n];
                if (c == 0) break;
                /* Stop if the string turns to garbage mid-way; anything
                 * outside printable ASCII in an ELF symbol name is a strong
                 * signal we've walked off the valid region. */
                if (c < 0x20 || c > 0x7E) break;
                safe[n] = (char)c;
                n++;
            }
            safe[n] = '\0';
            if (n > 0) out = safe;
        }

        if (out != safe) {
            /* Bad pointer / mojibake. Report the pointer value itself so the
             * user can grep the slotmap dump for which slot fired. */
            _snprintf(safe, sizeof(safe),
                      "<name-ptr-unreadable @0x%08x>", (unsigned)(uintptr_t)sym_name);
            safe[sizeof(safe) - 1] = '\0';
            out = safe;
        }
    }

    loader_log(LOG_ERROR, "NULL-GOT-CALL: %s (see prior 'Unresolved' WARNs)", out);
    fflush(stdout);
    fflush(stderr);
    ExitProcess(1);
}

/* Trampoline template (12 bytes):
 *   68 XX XX XX XX     push imm32          ; symbol name pointer
 *   B8 YY YY YY YY     mov  eax, imm32     ; &null_got_trap_dispatch
 *   FF E0              jmp  eax
 * The dispatcher never returns, so we don't clean up the pushed arg. */
static uint32_t g_trap_pool_base = 0;
static uint32_t g_trap_pool_off  = 0;
static uint32_t g_trap_pool_size = 0;

static uint32_t alloc_trap_thunk(const char *name)
{
    #define TRAP_STUB_SZ 12
    if (g_trap_pool_off + TRAP_STUB_SZ > g_trap_pool_size) {
        /* (Re)allocate a fresh RX pool. 64 KB fits ~5400 thunks. */
        SIZE_T sz = 0x10000;
        void *p = VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
        if (!p) return 0;
        g_trap_pool_base = (uint32_t)(uintptr_t)p;
        g_trap_pool_off  = 0;
        g_trap_pool_size = (uint32_t)sz;
    }
    uint8_t *stub = (uint8_t*)(uintptr_t)(g_trap_pool_base + g_trap_pool_off);
    uint32_t disp = (uint32_t)(uintptr_t)&null_got_trap_dispatch;
    uint32_t nm   = (uint32_t)(uintptr_t)name;

    stub[0]  = 0x68;                      /* push imm32 */
    *(uint32_t*)(stub + 1)  = nm;
    stub[5]  = 0xB8;                      /* mov eax, imm32 */
    *(uint32_t*)(stub + 6)  = disp;
    stub[10] = 0xFF;                      /* jmp eax */
    stub[11] = 0xE0;

    uint32_t addr = g_trap_pool_base + g_trap_pool_off;
    g_trap_pool_off += TRAP_STUB_SZ;
    return addr;
}

/* Public entry - call AFTER elf_relocate_all(). Walks the slotmap, patches
 * every null JMP_SLOT to a per-symbol trap thunk. Returns count patched. */
int dynlink_install_null_got_traps(void)
{
    int patched = 0;
    for (int i = 0; i < s_slotmap_count; i++) {
        if (s_slotmap[i].resolved != 0) continue;   /* already bound */
        uint32_t *slot = (uint32_t*)(uintptr_t)s_slotmap[i].slot;
        /* Skip if the slot was independently populated after the reloc pass
         * (e.g. by a later binder). We only replace true nulls. */
        DWORD old;
        if (!VirtualProtect(slot, sizeof(uint32_t), PAGE_READWRITE, &old))
            continue;
        if (*slot == 0) {
            uint32_t thunk = alloc_trap_thunk(s_slotmap[i].name);
            if (thunk) {
                *slot = thunk;
                s_slotmap[i].resolved = thunk;
                patched++;
            }
        }
        VirtualProtect(slot, sizeof(uint32_t), old, &old);
    }
    loader_log(LOG_INFO, "NULL-GOT trap: %d unresolved PLT slots armed with per-symbol thunks",
               patched);
    return patched;
}


/* ------------------------------------------------------------------ */
/* Symbol table                                                         */
/* ------------------------------------------------------------------ */
bool sym_register(LoaderCtx *ctx, const char *name, uintptr_t addr)
{
    if (ctx->symbol_count >= MAX_SYMBOLS) {
        loader_log(LOG_WARN, "Symbol table full, cannot register: %s", name);
        return false;
    }
    /* Check for duplicate */
    for (int i = 0; i < ctx->symbol_count; i++) {
        if (strcmp(ctx->symbols[i].name, name) == 0) {
            loader_log(LOG_DEBUG, "Overriding symbol: %s", name);
            ctx->symbols[i].addr = addr;
            return true;
        }
    }
    ctx->symbols[ctx->symbol_count].name = name;
    ctx->symbols[ctx->symbol_count].addr = addr;
    ctx->symbol_count++;
    return true;
}

/* Match an exported symbol name against the requested name, IGNORING any GNU
 * version suffix on the export. ELF symbol-versioning encodes the version as
 * "name@VERSION" (default binding "name@@VERSION") in dynstr. libCg/libCgGL
 * export e.g. "cgCreateContext@@VERSION" while the game imports the bare
 * "cgCreateContext"; a plain strcmp never matches. Compare up to the first '@'
 * on the export side. */
static int sym_name_matches(const char *exported, const char *wanted)
{
    if (!exported || !wanted) return 0;
    size_t i = 0;
    for (; wanted[i]; i++) {
        if (exported[i] != wanted[i]) return 0;
    }
    /* wanted fully consumed: export must end here, OR continue only with the
     * version separator '@' (so "foo" matches "foo" and "foo@@V" but not
     * "foobar"). */
    return exported[i] == '\0' || exported[i] == '@';
}

/* Internal: like sym_resolve but skips a given image when searching ELF
 * exports. The relocation paths pass the importing image as 'exclude' so a
 * symbol is NEVER satisfied by the SAME object that is importing it. This is
 * the ABI rule for R_386_COPY (the importer's own dynsym entry is the BSS
 * COPY DESTINATION, st_value points into the importer's BSS, not a real
 * definition) and also prevents a self-referential resolve for any image.
 *
 * Why this matters: ABC is loaded ALONE (no .so deps). Before the ELF32
 * sym_resolve fix, the buggy Elf64 cast never matched ABC's own C++ runtime
 * COPY symbols (_ZSt4cerr, vtables, ...), so they stayed 0 and the COPY was
 * skipped - ABC's libc-supplied iostream state stayed whatever the stub/BSS
 * default was, and ABC ran to frame 1. With the ELF32 fix, sym_resolve started
 * MATCHING those symbols in ABC's OWN dynsym and the COPY clobbered ABC's
 * iostream vtables with its own BSS slot => crash in msvcrt memcpy. Excluding
 * the importing image restores the exact prior (skip) behavior for ABC while
 * still letting the GAME resolve cg* from the SEPARATE libCg/libCgGL images. */
static uintptr_t sym_resolve_ex(LoaderCtx *ctx, const char *name, LoadedElf *exclude);

uintptr_t sym_resolve(LoaderCtx *ctx, const char *name)
{
    return sym_resolve_ex(ctx, name, NULL);
}

static uintptr_t sym_resolve_ex(LoaderCtx *ctx, const char *name, LoadedElf *exclude)
{
    /* 1. Check stub table first (Option C: stubs win over ELF exports) */
    for (int i = 0; i < ctx->symbol_count; i++) {
        if (strcmp(ctx->symbols[i].name, name) == 0)
            return ctx->symbols[i].addr;
    }

    /* 2. Search all loaded ELF images for an export.
     * MUST dispatch on the image's ELF class: an Elf32_Sym is 16 bytes with a
     * DIFFERENT field order (st_value at +4, st_info at +12) than the 24-byte
     * Elf64_Sym (st_info at +4, st_value at +8). The old code indexed every
     * image as Elf64_Sym[] + used ELF64_ST_BIND, so for the 32-bit
     * libCg/libCgGL it walked with the wrong stride and read garbage st_info/
     * st_value -> never matched -> all 97 game cg / cgi imports bound to 0.
     * Now ELF32 images are walked as Elf32_Sym[] with ELF32_ST_* and the
     * version suffix is stripped when name-matching. */
    for (int img_i = 0; img_i < ctx->image_count; img_i++) {
        LoadedElf *img = ctx->images[img_i];
        if (!img->dynsym || !img->dynstr) continue;
        if (img == exclude) continue;   /* never resolve a symbol from the importing image */

        if (img->is_elf32) {
            Elf32_Sym *syms = (Elf32_Sym*)img->dynsym;
            for (size_t s = 1; s < img->dynsym_count; s++) {
                Elf32_Sym *sym = &syms[s];
                if (sym->st_shndx == SHN_UNDEF) continue;
                if (sym->st_value == 0) continue;

                uint8_t bind = ELF32_ST_BIND(sym->st_info);
                uint8_t type = ELF32_ST_TYPE(sym->st_info);
                if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
                if (type != STT_FUNC && type != STT_OBJECT) continue;

                const char *sname = img->dynstr + sym->st_name;
                if (sym_name_matches(sname, name))
                    return (uint32_t)sym->st_value + (uint32_t)img->load_bias;
            }
        } else {
            for (size_t s = 1; s < img->dynsym_count; s++) {
                Elf64_Sym *sym = &img->dynsym[s];
                if (sym->st_shndx == SHN_UNDEF) continue;
                if (sym->st_value == 0) continue;

                uint8_t bind = ELF64_ST_BIND(sym->st_info);
                uint8_t type = ELF64_ST_TYPE(sym->st_info);
                if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
                if (type != STT_FUNC && type != STT_OBJECT) continue;

                const char *sname = img->dynstr + sym->st_name;
                if (sym_name_matches(sname, name))
                    return sym->st_value + img->load_bias;
            }
        }
    }

    /* Fallback for gl* symbols: try the GL resolver (wglGetProcAddress /
     * opengl32.dll / no-op stub).  Hummer, LGJ and HOTD4 use GLX-direct and
     * import many GL extension entry-points (glGenRenderbuffersEXT etc.) via
     * their PLT rather than via dlsym, so sym_resolve must handle them. */
    if (name && name[0] == 'g' && name[1] == 'l') {
        extern void *libgl_resolve(const char *name);
        void *gl_fn = libgl_resolve(name);
        if (gl_fn) return (uintptr_t)(intptr_t)gl_fn;
    }

    loader_log(LOG_WARN, "Unresolved symbol: %s", name);
    return 0;
}

static bool relocate_image32(LoaderCtx *ctx, LoadedElf *img); /* forward */

/* ------------------------------------------------------------------ */
/* Apply a single RELA relocation                                       */
/* ------------------------------------------------------------------ */
static bool apply_rela(LoaderCtx *ctx, LoadedElf *img, Elf64_Rela *rela)
{
    uint32_t sym_idx = (uint32_t)ELF64_R_SYM(rela->r_info);
    uint32_t rtype   = (uint32_t)ELF64_R_TYPE(rela->r_info);

    uintptr_t *target = (uintptr_t*)(rela->r_offset + img->load_bias);

    uintptr_t sym_val = 0;
    const char *sym_name = NULL;

    if (sym_idx != 0 && img->dynsym) {
        Elf64_Sym *sym = &img->dynsym[sym_idx];
        if (img->dynstr) sym_name = img->dynstr + sym->st_name;

        if (sym->st_shndx != SHN_UNDEF && sym->st_value != 0) {
            sym_val = sym->st_value + img->load_bias;
        } else if (sym_name) {
            sym_val = sym_resolve(ctx, sym_name);
            if (!sym_val) {
                loader_log(LOG_WARN, "  Reloc: unresolved %s (rtype=%u)", sym_name, rtype);
                /* For JUMP_SLOT, install a crash stub so we get a useful error */
            }
        }
    }

    DWORD old;
    VirtualProtect(target, sizeof(uintptr_t), PAGE_READWRITE, &old);

    switch (rtype) {
    case R_X86_64_64:
        /* S + A */
        *target = sym_val + (uintptr_t)rela->r_addend;
        break;

    case R_X86_64_GLOB_DAT:
    case R_X86_64_JUMP_SLOT:
        /* S */
        *target = sym_val;
        break;

    case R_X86_64_RELATIVE:
        /* B + A */
        *target = img->load_bias + (uintptr_t)rela->r_addend;
        break;

    default:
        loader_log(LOG_WARN, "  Unhandled reloc type %u at 0x%zx", rtype, (uintptr_t)target);
        break;
    }

    VirtualProtect(target, sizeof(uintptr_t), old, &old);
    return true;
}

/* ------------------------------------------------------------------ */
/* Relocate one image                                                   */
/* ------------------------------------------------------------------ */
static bool relocate_image(LoaderCtx *ctx, LoadedElf *img)
{
    loader_log(LOG_INFO, "Relocating: %s", img->path);

    int count = 0;

    if (img->rela_dyn) {
        for (size_t i = 0; i < img->rela_dyn_count; i++) {
            apply_rela(ctx, img, &img->rela_dyn[i]);
            count++;
        }
    }

    if (img->rela_plt) {
        for (size_t i = 0; i < img->rela_plt_count; i++) {
            apply_rela(ctx, img, &img->rela_plt[i]);
            count++;
        }
    }

    loader_log(LOG_INFO, "  Applied %d relocations", count);
    return true;
}

/* ------------------------------------------------------------------ */
/* Relocate all loaded images                                           */
/* ------------------------------------------------------------------ */
bool elf_relocate_all(LoaderCtx *ctx)
{
    loader_log(LOG_INFO, "=== Relocation pass ===");
    for (int i = 0; i < ctx->image_count; i++) {
        LoadedElf *img = ctx->images[i];
        bool ok = img->is_elf32 ? relocate_image32(ctx, img)
                                : relocate_image(ctx, img);
        if (!ok) return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Run .init_array functions                                            */
/* ------------------------------------------------------------------ */
static void run_init_array(LoadedElf *img)
{
    if (!img->dynamic) return;
    uintptr_t arr_va = 0;
    size_t    arr_sz = 0;

    for (Elf64_Dyn *d = img->dynamic; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_INIT_ARRAY)    arr_va = d->d_un.d_ptr + img->load_bias;
        if (d->d_tag == DT_INIT_ARRAYSZ)  arr_sz = d->d_un.d_val;
    }

    if (!arr_va || !arr_sz) return;

    uintptr_t *arr = (uintptr_t*)arr_va;
    size_t count   = arr_sz / sizeof(uintptr_t);
    loader_log(LOG_INFO, "Running %zu .init_array entries", count);

    for (size_t i = 0; i < count; i++) {
        if (arr[i] && arr[i] != (uintptr_t)-1) {
            loader_log(LOG_DEBUG, "  init[%zu] = 0x%zx", i, arr[i]);
            ((void(*)(void))arr[i])();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry point transfer                                                 */
/* ------------------------------------------------------------------ */
void elf_execute(LoaderCtx *ctx, LoadedElf *exe, int argc, char **argv)
{
    /* Run init constructors for all images */
    for (int i = 0; i < ctx->image_count; i++)
        run_init_array(ctx->images[i]);

    loader_log(LOG_INFO, "=== Transferring control to ELF entry 0x%zx ===", exe->entry);

    /*
     * Linux x86-64 process entry (as per ABI):
     *   rsp -> argc
     *          argv[0]..argv[argc-1], NULL
     *          envp[0]..., NULL
     *          auxv
     *
     * We build a minimal stack layout and jump.
     * (envp and auxv are sparse; real games may query auxv for AT_HWCAP etc.)
     */
    typedef void (*EntryFn)(void);
    EntryFn entry = (EntryFn)exe->entry;

    /* Minimal: just call it. A proper implementation would set up
     * the full ABI stack frame in assembly. For now this is enough
     * to reach _start -> __libc_start_main -> main via our stubs. */
    entry();
}

/* ------------------------------------------------------------------ */
/* ELF32 relocation (Rel, not Rela - no addend)                       */
/* ------------------------------------------------------------------ */
static bool relocate_image32(LoaderCtx *ctx, LoadedElf *img)
{
    loader_log(LOG_INFO, "Relocating ELF32: %s", img->path);
    int count = 0;

    /* Persistent slot->name map for real-time repair: if a GOT slot is later
     * called through while null, the repair engine looks the slot up here to
     * learn which symbol it should hold, then re-resolves and patches it. */
    extern void dynlink_record_slot(uint32_t slot_addr, const char *name, uint32_t resolved);

    /* rela_plt and rela_dyn fields hold Elf32_Rel pointers */
    Elf32_Rel *rel_plt = (Elf32_Rel*)img->rela_plt;
    Elf32_Rel *rel_dyn = (Elf32_Rel*)img->rela_dyn;
    size_t plt_count   = img->rela_plt_count;
    size_t dyn_count   = img->rela_dyn_count;

    for (int pass = 0; pass < 2; pass++) {
        Elf32_Rel *rels = (pass == 0) ? rel_dyn : rel_plt;
        size_t     n    = (pass == 0) ? dyn_count : plt_count;
        if (!rels) continue;

        for (size_t i = 0; i < n; i++) {
            uint32_t sym_idx = ELF32_R_SYM(rels[i].r_info);
            uint32_t rtype   = ELF32_R_TYPE(rels[i].r_info);

            uint32_t *target = (uint32_t*)(uintptr_t)
                               ((uint32_t)rels[i].r_offset + (uint32_t)img->load_bias);

            uintptr_t sym_val = 0;
            const char *sym_name = NULL;
            /* R_386_COPY semantics: copy the VALUE of the symbol from the
             * defining library into the game's BSS. The game's own dynsym
             * st_value points at the BSS slot itself, so we must resolve the
             * name against OUR registered symbols (msvcrt stderr/stdout/etc.),
             * never use st_value. Force name-resolution for COPY relocs. */
            if (sym_idx != 0 && img->dynsym) {
                Elf32_Sym *sym = &((Elf32_Sym*)img->dynsym)[sym_idx];
                if (rtype == 5 /* R_386_COPY */ && img->dynstr) {
                    const char *name = img->dynstr + sym->st_name;
                    sym_name = name;
                    /* exclude 'img': a COPY's own dynsym entry is the BSS
                     * destination, never the source. */
                    sym_val = sym_resolve_ex(ctx, name, img);
                    if (!sym_val) loader_log(LOG_WARN, "  ELF32 COPY unresolved: %s", name);
                } else if (sym->st_shndx != SHN_UNDEF && sym->st_value != 0) {
                    sym_val = (uint32_t)sym->st_value + (uint32_t)img->load_bias;
                } else if (img->dynstr) {
                    const char *name = img->dynstr + sym->st_name;
                    sym_name = name;
                    /* exclude 'img' so an UNDEF import is never satisfied by a
                     * stray same-image alias - matches pre-fix ABC behavior. */
                    sym_val = sym_resolve_ex(ctx, name, img);
                    if (!sym_val)
                        loader_log(LOG_WARN, "  ELF32 unresolved: %s", name);
                    /* If sym_val is a 64-bit address (> 4GB), route through thunk */
                    else if (sym_val > 0xFFFFFFFFULL) {
                        uint32_t thunk = thunk32_lookup(sym_val);
                        if (!thunk) thunk = thunk32_register(sym_val, name);
                        if (thunk) {
                            sym_val = thunk;
                        } else {
                            loader_log(LOG_WARN, "  ELF32: cannot thunk %s (addr=0x%llx)", name, (unsigned long long)sym_val);
                            sym_val = 0;
                        }
                    }
                }
            }

            DWORD old;
            VirtualProtect(target, sizeof(uint32_t), PAGE_READWRITE, &old);

            switch (rtype) {
            case R_386_32:       *target = (uint32_t)(sym_val); break;
            case R_386_GLOB_DAT: *target = (uint32_t)(sym_val); break;
            case R_386_JMP_SLOT:
                *target = (uint32_t)(sym_val);
                loader_log(LOG_DEBUG, "  JMP_SLOT @0x%08x -> 0x%08x (%s)",
                           (uint32_t)(uintptr_t)target, (uint32_t)sym_val,
                           sym_name ? sym_name : "?");
                /* Record for real-time repair: even if sym_val is 0 now, the
                 * name lets the repair engine resolve it later on a null call. */
                if (sym_name)
                    dynlink_record_slot((uint32_t)(uintptr_t)target, sym_name, (uint32_t)sym_val);
                break;
            case R_386_RELATIVE: *target = (uint32_t)(img->load_bias + *target); break;
            case R_386_PC32:     *target = (uint32_t)(sym_val - (uintptr_t)target - 4); break;
            case 5 /* R_386_COPY */:
                /* Copy data from definition to BSS. Skip if sym_val is 0. */
                if (sym_val) { *target = (uint32_t)sym_val; } /* pointer copy */ 
                break;
            default:
                loader_log(LOG_WARN, "  ELF32 unhandled reloc type %u", rtype);
                break;
            }

            VirtualProtect(target, sizeof(uint32_t), old, &old);
            count++;
        }
    }

    loader_log(LOG_INFO, "  ELF32: applied %d relocations", count);
    return true;
}

