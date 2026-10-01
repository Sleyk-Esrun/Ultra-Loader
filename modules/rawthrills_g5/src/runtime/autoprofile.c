/*
 * autoprofile.c - Ultra Loader self-learning boot system.
 *
 * Three cooperating mechanisms, as one system:
 *
 *  1. REAL-TIME REPAIR: when the game faults on something *derivable* - a null
 *     call through a GOT/relocation slot the ELF names, an unmapped page, an
 *     unresolved symbol - the repair engine fixes it in place from the ELF's
 *     own tables and resumes. The binary guides the fix.
 *
 *  2. RECORDING: every successful repair (slot -> resolved target, page commit,
 *     etc.) is appended to an in-memory profile keyed by the ELF's SHA-256.
 *
 *  3. AUTO-PROFILE REPLAY: on the next boot of the same ELF, the recorded
 *     profile is pre-applied up front, so the slow trial-by-fault discovery
 *     happens once. First run learns (slower); later runs replay (fast,
 *     deterministic). Profiles are the loader's own artifact per game.
 *
 * HONEST SCOPE: this repairs/records the *derivable* class - values, addresses,
 * symbols, pages - which is the bulk of what a loader does. It does NOT author
 * correct *implementations*: a buggy stub (e.g. a wrong __cxa_atexit) is a code
 * fix, not something a fault signal can search for. Those are fixed in the
 * runtime once and benefit every game. This module handles everything else.
 */

#include "compat32.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "loader.h"

/* ---- profile entry types --------------------------------------------- */
typedef enum {
    PROF_SYMBIND = 1,   /* a GOT/data slot was bound to a resolved address    */
    PROF_PAGE    = 2,   /* a page address was demand-committed                */
} ProfKind;

typedef struct {
    uint32_t kind;
    uint32_t addr;      /* slot address (SYMBIND) or page address (PAGE)      */
    uint32_t value;     /* resolved target (SYMBIND); unused for PAGE         */
} ProfEntry;

#define PROF_MAX 8192
static ProfEntry  s_prof[PROF_MAX];
static int        s_prof_count = 0;
static char       s_prof_path[MAX_PATH] = {0};
static int        s_replaying = 0;   /* 1 while pre-applying a loaded profile */

/* ELF hash, published by the patcher; keys the profile file. */
char        g_elf_sha_hex_buf[65] = {0};
const char *g_elf_sha_hex = (const char*)0;

/* The loader's symbol resolver (implemented in dynlink.c). Given a symbol
 * name it returns the stub/host address we use for it, or 0. We also expose a
 * reverse lookup: given a GOT slot address, what symbol name does the ELF's
 * relocation table assign to it? */
extern uint32_t dynlink_resolve_name(const char *name);            /* 0 if unknown */
extern const char *dynlink_symbol_for_got_slot(uint32_t slot_addr);/* NULL if none */

/* ---- profile recording ----------------------------------------------- */
static void prof_record(uint32_t kind, uint32_t addr, uint32_t value)
{
    if (s_replaying) return;            /* don't re-record while replaying    */
    if (s_prof_count >= PROF_MAX) return;
    /* de-dup */
    for (int i = 0; i < s_prof_count; i++)
        if (s_prof[i].kind == kind && s_prof[i].addr == addr) return;
    s_prof[s_prof_count].kind  = kind;
    s_prof[s_prof_count].addr  = addr;
    s_prof[s_prof_count].value = value;
    s_prof_count++;
}

/* Public: substrate calls this when it demand-commits a page, so page commits
 * become part of the profile and can be pre-committed next boot. */
void autoprofile_note_page(uint32_t page_addr)
{
    prof_record(PROF_PAGE, page_addr, 0);
}

/* ---- profile file I/O ------------------------------------------------- */
void autoprofile_init(const char *elf_sha256_hex)
{
    /* Profile path: <exe-dir>\profiles\<sha>.ulp  (Ultra Loader Profile) */
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, sizeof(dir));
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    char pdir[MAX_PATH];
    snprintf(pdir, sizeof(pdir), "%s\\profiles", dir);
    CreateDirectoryA(pdir, NULL);
    snprintf(s_prof_path, sizeof(s_prof_path), "%s\\%.*s.ulp",
             pdir, 32, elf_sha256_hex ? elf_sha256_hex : "unknown");
    loader_log(LOG_INFO, "[PROFILE] path: %s", s_prof_path);
}

/* Load an existing profile and PRE-APPLY it (replay): pre-commit recorded
 * pages and pre-bind recorded slots, so first-fault discovery is skipped.
 * Returns the number of entries applied (0 = no profile / first run). */
int autoprofile_load_and_apply(void)
{
    if (!s_prof_path[0]) return 0;
    FILE *f = fopen(s_prof_path, "rb");
    if (!f) {
        loader_log(LOG_INFO, "[PROFILE] none yet, this is a LEARNING run (first boot)");
        return 0;
    }
    /* header: magic 'ULP1' + count */
    uint32_t magic = 0, count = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != 0x31504C55u) { fclose(f); return 0; }
    if (fread(&count, 4, 1, f) != 1 || count > PROF_MAX)     { fclose(f); return 0; }

    s_replaying = 1;
    int applied = 0;
    for (uint32_t i = 0; i < count; i++) {
        ProfEntry e;
        if (fread(&e, sizeof(e), 1, f) != 1) break;
        s_prof[s_prof_count++] = e;     /* keep it so we re-save supersets     */
        if (e.kind == PROF_PAGE) {
            VirtualAlloc((void*)(uintptr_t)e.addr, 0x1000,
                         MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            applied++;
        } else if (e.kind == PROF_SYMBIND) {
            /* pre-bind the slot to the recorded target (guard unmapped slots) */
            if (!IsBadWritePtr((void*)(uintptr_t)e.addr, 4)) {
                *(uint32_t*)(uintptr_t)e.addr = e.value; applied++;
            }
        }
    }
    fclose(f);
    s_replaying = 0;
    loader_log(LOG_INFO, "[PROFILE] REPLAY run, pre-applied %d learned entries", applied);
    return applied;
}

void autoprofile_save(void)
{
    if (!s_prof_path[0] || s_prof_count == 0) return;
    FILE *f = fopen(s_prof_path, "wb");
    if (!f) { loader_log(LOG_WARN, "[PROFILE] could not write %s", s_prof_path); return; }
    uint32_t magic = 0x31504C55u, count = (uint32_t)s_prof_count;
    fwrite(&magic, 4, 1, f);
    fwrite(&count, 4, 1, f);
    fwrite(s_prof, sizeof(ProfEntry), s_prof_count, f);
    fclose(f);
    loader_log(LOG_INFO, "[PROFILE] saved %d entries to %s", s_prof_count, s_prof_path);
}

/* ----------------------------------------------------------------------
 * REAL-TIME REPAIR of a null function-pointer call.
 *
 * On EIP=0, the instruction right before [esp] (the pushed return address) is
 * the faulting `call`. We decode it to find whether it was `call *[mem32]`
 * (FF 15 disp32) or `call *reg`/`call *[reg+disp]` (FF /2). For the common
 * `call *[GOT_slot]` form we extract the slot address, ask the ELF which
 * symbol that slot belongs to, resolve it to our stub, patch the slot, point
 * EIP at the target, record it, and resume.
 *
 * Returns 1 if it repaired & set ctx->Rip to resume; 0 if not derivable.
 * -------------------------------------------------------------------- */
int autoprofile_repair_null_call(CONTEXT *ctx)
{
    uint32_t *sp = (uint32_t*)(uintptr_t)ctx->Rsp;
    if (!sp || IsBadReadPtr(sp, 4)) return 0;
    uint32_t ret = sp[0];                 /* addr after the bad call          */
    if (!ret) return 0;

    /* The call instruction ends at `ret`. Scan back up to 7 bytes for an
     * FF-prefixed indirect CALL and decode its operand. */
    const uint8_t *p = (const uint8_t*)(uintptr_t)ret;
    uint32_t slot = 0;       /* memory slot the call dereferenced, if any     */
    for (int back = 2; back <= 7; back++) {
        const uint8_t *c = p - back;
        if (IsBadReadPtr((void*)c, back)) continue;
        if (c[0] != 0xFF) continue;
        uint8_t modrm = c[1];
        uint8_t reg   = (modrm >> 3) & 7;     /* must be /2 for CALL r/m       */
        if (reg != 2) continue;
        uint8_t mod = (modrm >> 6) & 3;
        uint8_t rm  = modrm & 7;
        /* call *disp32  (FF 15 dd dd dd dd) : 6 bytes, mod=00 rm=101 */
        if (mod == 0 && rm == 5 && back == 6) {
            slot = *(const uint32_t*)(c + 2);
            break;
        }
        /* call *[reg]  (FF 10..) mod=00 rm!=5,4 : 2 bytes - slot is in *reg,
         * which we can't recover post-fault reliably; skip (non-derivable). */
    }

    if (!slot) return 0;                  /* not a memory-slot call form       */

    /* Ask the ELF: what symbol does this GOT slot hold? */
    const char *name = dynlink_symbol_for_got_slot(slot);
    if (!name) {
        loader_log(LOG_WARN, "[REPAIR] null call via slot 0x%08x, ELF has no symbol for it (non-derivable)", slot);
        return 0;
    }
    uint32_t target = dynlink_resolve_name(name);
    if (!target) {
        loader_log(LOG_WARN, "[REPAIR] slot 0x%08x is '%s' but we have no implementation (non-derivable)", slot, name);
        return 0;
    }

    /* Derivable! Patch the slot, record it, resume into the real target. */
    if (IsBadWritePtr((void*)(uintptr_t)slot, 4)) return 0;
    *(uint32_t*)(uintptr_t)slot = target;
    prof_record(PROF_SYMBIND, slot, target);
    loader_log(LOG_INFO, "[REPAIR] bound slot 0x%08x -> %s @0x%08x (recorded); resuming",
               slot, name, target);
    ctx->Rip = target;
    return 1;
}
