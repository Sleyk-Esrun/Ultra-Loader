/*
 * auxv.c
 * ELF Auxiliary Vector Semantic Layer
 *
 * Linux's _start (the real entry point before main()) expects to find
 * an auxiliary vector on the stack below envp. glibc's __libc_start_main
 * reads AT_PHDR, AT_PHNUM, AT_ENTRY, AT_BASE, AT_HWCAP, AT_PAGESZ,
 * AT_RANDOM, AT_PLATFORM, and AT_CLKTCK during initialization.
 *
 * Without a correct auxv:
 *   - glibc's HWCAP detection fails (no CPUID extension flags)
 *   - Stack canary initialization fails (AT_RANDOM needed)
 *   - VDSO setup crashes (AT_SYSINFO_EHDR)
 *   - Some games crash before main() is reached
 *
 * We build a synthetic auxv and set up the Linux ABI stack frame
 * that _start expects:
 *
 *   [rsp]         argc
 *   [rsp+8]       argv[0]
 *   ...
 *   [rsp+8*(n+1)] NULL        (end of argv)
 *   [rsp+...]     envp[0]
 *   ...           NULL        (end of envp)
 *   [rsp+...]     AT_PHDR     (auxv start)
 *   [rsp+...]     phdr_addr
 *   ...
 *   [rsp+...]     AT_NULL 0   (auxv end)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <intrin.h>
#include "loader.h"
#include "auxv.h"

/* Linux AT_ constants */
#define AT_NULL         0
#define AT_IGNORE       1
#define AT_EXECFD       2
#define AT_PHDR         3
#define AT_PHENT        4
#define AT_PHNUM        5
#define AT_PAGESZ       6
#define AT_BASE         7
#define AT_FLAGS        8
#define AT_ENTRY        9
#define AT_NOTELF       10
#define AT_UID          11
#define AT_EUID         12
#define AT_GID          13
#define AT_EGID         14
#define AT_PLATFORM     15
#define AT_HWCAP        16
#define AT_CLKTCK       17
#define AT_SECURE       23
#define AT_BASE_PLATFORM 24
#define AT_RANDOM       25
#define AT_HWCAP2       26
#define AT_EXECFN       31
#define AT_SYSINFO_EHDR 33

/* HWCAP flags (x86-64) */
#define HWCAP_FPU       (1 << 0)
#define HWCAP_VME       (1 << 1)
#define HWCAP_DE        (1 << 2)
#define HWCAP_PSE       (1 << 3)
#define HWCAP_TSC       (1 << 4)
#define HWCAP_MSR       (1 << 5)
#define HWCAP_PAE       (1 << 6)
#define HWCAP_MCE       (1 << 7)
#define HWCAP_CX8       (1 << 8)
#define HWCAP_APIC      (1 << 9)
#define HWCAP_SEP       (1 << 11)
#define HWCAP_MTRR      (1 << 12)
#define HWCAP_PGE       (1 << 13)
#define HWCAP_MCA       (1 << 14)
#define HWCAP_CMOV      (1 << 15)
#define HWCAP_PAT       (1 << 16)
#define HWCAP_PSE36     (1 << 17)
#define HWCAP_CLFLUSH   (1 << 19)
#define HWCAP_MMX       (1 << 23)
#define HWCAP_FXSR      (1 << 24)
#define HWCAP_SSE       (1 << 25)
#define HWCAP_SSE2      (1 << 26)

typedef struct { uint64_t a_type; uint64_t a_val; } Auxv64;

/* ------------------------------------------------------------------ */
/* Read actual CPUID to synthesize a real HWCAP                       */
/* ------------------------------------------------------------------ */
static uint64_t build_hwcap(void)
{
    uint64_t hwcap = HWCAP_FPU | HWCAP_TSC | HWCAP_CX8 |
                     HWCAP_CMOV | HWCAP_MMX | HWCAP_FXSR;

    int info[4];
    __cpuid(info, 1);

    int edx = info[3];


    if (edx & (1 << 25)) hwcap |= HWCAP_SSE;
    if (edx & (1 << 26)) hwcap |= HWCAP_SSE2;
    if (edx & (1 << 19)) hwcap |= HWCAP_CLFLUSH;
    if (edx & (1 << 5))  hwcap |= HWCAP_MSR;
    if (edx & (1 << 6))  hwcap |= HWCAP_PAE;
    if (edx & (1 << 14)) hwcap |= HWCAP_MCA;

    loader_log(LOG_DEBUG, "HWCAP synthesized: 0x%llx", (unsigned long long)hwcap);
    return hwcap;
}

/* ------------------------------------------------------------------ */
/* Build the full Linux ABI initial stack                             */
/* ------------------------------------------------------------------ */
/*
 * Layout (grows down, rsp points to argc):
 *
 *   argc                 8 bytes
 *   argv[0]              8 bytes  -> pointer to string
 *   argv[1]              8 bytes
 *   ...
 *   argv[argc-1]         8 bytes
 *   NULL                 8 bytes  (end of argv)
 *   envp[0]              8 bytes  -> "HOME=/game"
 *   envp[1]              8 bytes  -> "PATH=/usr/bin:/bin"
 *   ...
 *   NULL                 8 bytes  (end of envp)
 *   auxv[0] type + val   16 bytes
 *   ...
 *   AT_NULL pair         16 bytes
 *   --- string data area ---
 *   argv string copies
 *   env string copies
 *   platform string "x86_64"
 *   16 random bytes      (AT_RANDOM)
 */

#define STACK_SIZE (1 * 1024 * 1024)  /* 1 MB initial stack */

static const char *g_default_env[] = {
    "HOME=/game",
    "PATH=/usr/local/bin:/usr/bin:/bin",
    "LD_LIBRARY_PATH=/game/lib:/usr/lib",
    "TERM=xterm",
    "USER=lindbergh",
    "LANG=en_US.UTF-8",
    NULL
};

void *auxv_build_stack(LoadedElf *exe, int argc, char **argv,
                        size_t *out_stack_size)
{
    uint8_t *stack = NULL;

    if (exe && exe->is_elf32) {
        /* ELF32 runs in 64-bit mode but its code truncates pointers to 32 bits.
         * The stack AND all string data must be below 4GB so argv/envp pointers
         * remain valid when the ELF reads them with 32-bit loads. */
        static const uintptr_t s_candidates[] = {
            0x70000000, 0x60000000, 0x50000000, 0x40000000,
            0x30000000, 0x20000000, 0
        };
        for (int i = 0; s_candidates[i]; i++) {
            stack = VirtualAlloc((LPVOID)s_candidates[i], STACK_SIZE,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (stack) break;
        }
        if (!stack) {
            loader_log(LOG_ERROR, "auxv_build_stack: cannot allocate ELF32 stack below 4GB");
            return NULL;
        }
        if ((uintptr_t)stack > 0xFFFFFFFFULL) {
            loader_log(LOG_ERROR, "auxv_build_stack: ELF32 stack at 0x%llx > 4GB",
                       (unsigned long long)(uintptr_t)stack);
            VirtualFree(stack, 0, MEM_RELEASE);
            return NULL;
        }
        loader_log(LOG_INFO, "ELF32 ABI stack in low memory: 0x%08x", (uint32_t)(uintptr_t)stack);
    } else {
        stack = VirtualAlloc(NULL, STACK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }

    if (!stack) {
        loader_log(LOG_ERROR, "Failed to allocate ABI stack");
        return NULL;
    }

    /*
     * We fill the stack from near the top (high address) downward.
     * Then rsp will point to the argc word.
     *
     * Use a staging buffer:
     */
    uint8_t *stage    = malloc(STACK_SIZE);
    if (!stage) {
        loader_log(LOG_ERROR, "auxv_build_stack: malloc(%d) failed", STACK_SIZE);
        VirtualFree(stack, 0, MEM_RELEASE);
        return NULL;
    }
    uint8_t *str_area = stage + STACK_SIZE - 4096; /* string data near top */
    uint8_t *str_ptr  = str_area;
    size_t   str_used = 0;

    /* Helper: copy a string into the staging area, return address */
    #define PUSH_STRING(s) ({ \
        size_t _l = strlen(s) + 1; \
        memcpy(str_ptr, s, _l); \
        uintptr_t _addr = (uintptr_t)(stack + STACK_SIZE - 4096) + str_used; \
        str_ptr += _l; str_used += _l; \
        _addr; \
    })

    /* Platform string */
    uintptr_t platform_addr = PUSH_STRING("x86_64");

    /* 16 random bytes for AT_RANDOM */
    uintptr_t random_addr = (uintptr_t)(stack + STACK_SIZE - 4096) + str_used;
    typedef BOOLEAN (WINAPI *RtlGenRandom_t)(PVOID, ULONG);
    static RtlGenRandom_t RtlGenRandom = NULL;
    if (!RtlGenRandom) {
        HMODULE adv = LoadLibraryA("advapi32.dll");
        RtlGenRandom = (RtlGenRandom_t)GetProcAddress(adv, "SystemFunction036");
    }
    if (RtlGenRandom) RtlGenRandom(str_ptr, 16);
    str_ptr += 16; str_used += 16;

    /* Executable filename */
    uintptr_t execfn_addr = PUSH_STRING(exe->path);

    /* Argv strings */
    uintptr_t argv_addrs[256] = {0};
    for (int i = 0; i < argc && i < 255; i++)
        argv_addrs[i] = PUSH_STRING(argv[i]);

    /* Env strings */
    uintptr_t envp_addrs[64] = {0};
    int nenv = 0;
    for (int i = 0; g_default_env[i] && nenv < 63; i++)
        envp_addrs[nenv++] = PUSH_STRING(g_default_env[i]);

    /* Now build the word area at the bottom of stage */
    uint64_t *wp = (uint64_t*)stage;
    size_t   wi  = 0;

    /* argc */
    wp[wi++] = (uint64_t)argc;

    /* argv pointers */
    for (int i = 0; i < argc; i++) wp[wi++] = argv_addrs[i];
    wp[wi++] = 0; /* NULL */

    /* envp pointers */
    for (int i = 0; i < nenv; i++) wp[wi++] = envp_addrs[i];
    wp[wi++] = 0; /* NULL */

    /* auxv - use ELF32 fields when loading a 32-bit binary */
    #define AUX(t, v) do { wp[wi++] = (t); wp[wi++] = (uint64_t)(v); } while(0)

    if (exe->is_elf32 && exe->ehdr32) {
        AUX(AT_PHDR,   exe->base + exe->ehdr32->e_phoff);
        AUX(AT_PHENT,  sizeof(Elf32_Phdr));
        AUX(AT_PHNUM,  exe->ehdr32->e_phnum);
    } else if (exe->ehdr) {
        AUX(AT_PHDR,   exe->base + exe->ehdr->e_phoff);
        AUX(AT_PHENT,  sizeof(Elf64_Phdr));
        AUX(AT_PHNUM,  exe->ehdr->e_phnum);
    }
    AUX(AT_PAGESZ,   4096);
    AUX(AT_BASE,     exe->load_bias);
    AUX(AT_FLAGS,    0);
    AUX(AT_ENTRY,    exe->entry);
    AUX(AT_UID,      1000);
    AUX(AT_EUID,     1000);
    AUX(AT_GID,      1000);
    AUX(AT_EGID,     1000);
    AUX(AT_CLKTCK,   100);
    AUX(AT_PLATFORM, platform_addr);
    AUX(AT_HWCAP,    build_hwcap());
    AUX(AT_RANDOM,   random_addr);
    AUX(AT_EXECFN,   execfn_addr);
    AUX(AT_SECURE,   0);
    AUX(AT_NULL,     0);

    #undef AUX

    /* Copy word area into bottom of stack buffer */
    size_t word_bytes = wi * sizeof(uint64_t);
    memcpy(stack, stage, word_bytes);

    /* Copy string area into top of stack buffer */
    memcpy(stack + STACK_SIZE - 4096, str_area, str_used);

    free(stage);

    if (out_stack_size) *out_stack_size = STACK_SIZE;

    loader_log(LOG_INFO, "ABI stack built at 0x%p (%zu word entries, %zu str bytes)",
               stack, wi, str_used);

    return stack;

    #undef PUSH_STRING
}

/* ------------------------------------------------------------------ */
/* 32-bit ABI stack builder for ELF32 (IA-32 compat mode)            */
/* ------------------------------------------------------------------ */
/*
 * ELF32 _start (i386 glibc) expects:
 *   [esp+0]           argc        (4 bytes, uint32_t)
 *   [esp+4]           argv[0]     (4 bytes, 32-bit pointer)
 *   ...
 *   [esp+4*(argc+1)]  NULL        (4 bytes, end of argv)
 *   [esp+4*(argc+2)]  envp[0]     (4 bytes, 32-bit pointer)
 *   ...
 *   NULL                          (4 bytes, end of envp)
 *   AT_type           (4 bytes)
 *   AT_val            (4 bytes)
 *   ...
 *   AT_NULL  0        (8 bytes, end of auxv)
 *
 * All pointers MUST be 32-bit (< 4GB). The stack buffer itself must
 * also be at a 32-bit-accessible address.
 */
void *auxv_build_stack32(LoadedElf *exe, int argc, char **argv,
                          size_t *out_stack_size)
{
    /* STACK32_SIZE: the guest's main-thread stack. Originally 512KB, anchored at
     * base 0x70000000 (top 0x70080000). That is too small for titles that build
     * a very large SINGLE on-stack frame: Ghost Squad Evolution's
     * create_vertex_buffer does `sub esp, 0xFA08C` (~1MB) - esp then drops BELOW
     * 0x70000000 into unmapped memory, and the first write faults in a context
     * where esp is already off the mapped stack, so the VEH can't even run
     * (silent 0xC0000005, no crash report). Grow the reservation to 8MB.
     *
     * CRITICAL - keep the TOP byte-identical so ABC is unaffected: all ABI math
     * (string/word area, g_abi_stack_hi, the esp _start sees) is anchored to
     * `stack + STACK32_SIZE` (the TOP of the buffer). We therefore allocate the
     * larger block so its TOP stays at the original 0x70080000 by choosing a base
     * = top - STACK32_SIZE. ABC's argc/argv/auxv/abi_esp addresses are then
     * exactly as before; only previously-unmapped memory BELOW the old base is
     * now committed (purely additive). The candidate fallbacks preserve the same
     * top-anchored relationship. */
#define STACK32_SIZE     (8 * 1024 * 1024)   /* 8MB reserve (was 512KB) */
#define STACK32_TOP      0x70080000u          /* fixed top (== old 0x70000000+512KB) */

    /* Allocate so the TOP lands at a fixed low address. Primary: top=0x70080000
     * (base 0x6F880000). Fallbacks keep a fixed top at descending anchors. */
    uint8_t *stack = NULL;
    static const uintptr_t s_tops[] = {
        STACK32_TOP, 0x60000000, 0x50000000, 0x40000000,
        0x30000000, 0x20000000, 0x18000000, 0x14000000, 0
    };
    for (int i = 0; s_tops[i]; i++) {
        uintptr_t base = s_tops[i] - STACK32_SIZE;
        stack = VirtualAlloc((LPVOID)base, STACK32_SIZE,
                              MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
        if (stack) break;
    }
    if (!stack) {
        stack = VirtualAlloc(NULL, STACK32_SIZE,
                              MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    }
    if (!stack) {
        loader_log(LOG_ERROR, "auxv_build_stack32: allocation failed: %lu", GetLastError());
        return NULL;
    }
    if ((uintptr_t)stack > 0xFFFFFFFFULL) {
        loader_log(LOG_ERROR, "auxv_build_stack32: stack at 0x%llx > 4GB, cannot use in 32-bit mode",
                   (unsigned long long)(uintptr_t)stack);
        VirtualFree(stack, 0, MEM_RELEASE);
        return NULL;
    }

    memset(stack, 0, STACK32_SIZE);

    /* String area: top 4KB of the stack buffer */
    uint8_t *str_area = stack + STACK32_SIZE - 4096;
    uint8_t *str_ptr  = str_area;
    size_t   str_used = 0;

    #define PUSH_STR32(s) ({ \
        size_t _l = strlen(s) + 1; \
        memcpy(str_ptr, s, _l); \
        uint32_t _a = (uint32_t)((uintptr_t)(stack + STACK32_SIZE - 4096) + str_used); \
        str_ptr += _l; str_used += _l; \
        _a; \
    })

    uint32_t platform_addr = PUSH_STR32("i686");

    /* 16 random bytes for AT_RANDOM */
    uint32_t random_addr = (uint32_t)((uintptr_t)(stack + STACK32_SIZE - 4096) + str_used);
    typedef BOOLEAN (WINAPI *RtlGenRandom_t)(PVOID, ULONG);
    static RtlGenRandom_t RtlGenRandom32 = NULL;
    if (!RtlGenRandom32) {
        HMODULE adv = LoadLibraryA("advapi32.dll");
        RtlGenRandom32 = (RtlGenRandom_t)GetProcAddress(adv, "SystemFunction036");
    }
    if (RtlGenRandom32) RtlGenRandom32(str_ptr, 16);
    str_ptr += 16; str_used += 16;

    uint32_t execfn_addr = PUSH_STR32(exe->path);

    /* Argv strings */
    uint32_t argv_addrs[256] = {0};
    for (int i = 0; i < argc && i < 255; i++)
        argv_addrs[i] = PUSH_STR32(argv[i]);

    /* Env strings */
    uint32_t envp_addrs[64] = {0};
    int nenv = 0;
    for (int i = 0; g_default_env[i] && nenv < 63; i++)
        envp_addrs[nenv++] = PUSH_STR32(g_default_env[i]);

    /* Build 32-bit word area.
     * CRITICAL: The word area (argc/argv/envp/auxv) must be placed near the
     * TOP of the stack buffer, not the bottom. When _start runs, it:
     *   pop  esi          (reads argc from [esp], advances esp by 4)
     *   push ...many...   (pushes args for __libc_start_main BELOW esp)
     * If esp is at the bottom of our buffer, those pushes land in unmapped
     * memory -> access violation before _start makes a single function call.
     *
     * Layout: [GROW_ROOM ... WORD_AREA ... STRING_AREA]
     *         ^stack base                              ^stack+STACK32_SIZE
     * esp points into WORD_AREA (at argc).
     * _start pushes go into GROW_ROOM (below argc).
     * We reserve 64KB for _start's pushes and the initial call frames.
     */
    #define STACK32_GROW_ROOM (64 * 1024)

    /* String area: top 4KB of the buffer (already set up) */
    /* Word area: placed just below string area */
    /* argc goes at: stack + STACK32_SIZE - 4096 - (word_bytes) - GROW_ROOM... */
    /* Simpler: just count words first, then place at STACK32_SIZE - 4096 - word_estimate */

    /* Pre-count words needed */
    size_t word_count = 1                    /* argc */
                      + (size_t)argc + 1     /* argv + NULL */
                      + (size_t)nenv + 1     /* envp + NULL */
                      + 2 * 18;             /* auxv (18 pairs) */
    size_t word_bytes = word_count * sizeof(uint32_t);

    /* Place word area just below string area, with grow room below that */
    uint8_t *word_base = stack + STACK32_SIZE - 4096 - (word_bytes + 64);
    /* Ensure grow room between stack base and word_base */
    if (word_base < stack + STACK32_GROW_ROOM)
        word_base = stack + STACK32_GROW_ROOM;

    uint32_t *wp = (uint32_t*)word_base;
    size_t wi = 0;

    /* argc */
    wp[wi++] = (uint32_t)argc;

    /* argv pointers */
    for (int i = 0; i < argc; i++) wp[wi++] = argv_addrs[i];
    wp[wi++] = 0;

    /* envp pointers */
    for (int i = 0; i < nenv; i++) wp[wi++] = envp_addrs[i];
    wp[wi++] = 0;

    /* auxv (pairs of uint32_t: type, value) */
    #define AUX32(t,v) do { wp[wi++] = (uint32_t)(t); wp[wi++] = (uint32_t)(v); } while(0)

    /* AT_PHDR: use ELF32-specific phdr location */
    AUX32(AT_PHDR,    (uint32_t)(exe->base + ((Elf32_Ehdr*)exe->base)->e_phoff));
    AUX32(AT_PHENT,   sizeof(Elf32_Phdr));
    AUX32(AT_PHNUM,   ((Elf32_Ehdr*)exe->base)->e_phnum);
    AUX32(AT_PAGESZ,  4096);
    AUX32(AT_BASE,    (uint32_t)(uint64_t)exe->load_bias);
    AUX32(AT_FLAGS,   0);
    AUX32(AT_ENTRY,   (uint32_t)exe->entry);
    AUX32(AT_UID,     1000);
    AUX32(AT_EUID,    1000);
    AUX32(AT_GID,     1000);
    AUX32(AT_EGID,    1000);
    AUX32(AT_CLKTCK,  100);
    AUX32(AT_PLATFORM, platform_addr);
    AUX32(AT_HWCAP,   0x0183f9ff); /* typical i686 HWCAP */
    AUX32(AT_RANDOM,  random_addr);
    AUX32(AT_EXECFN,  execfn_addr);
    AUX32(AT_SECURE,  0);
    AUX32(AT_NULL,    0);

    #undef AUX32
    #undef PUSH_STR32

    if (out_stack_size) *out_stack_size = STACK32_SIZE;

    loader_log(LOG_INFO, "ELF32 ABI stack at 0x%08x (argc at 0x%08x, grow_room=%dKB)",
               (uint32_t)(uintptr_t)stack,
               (uint32_t)(uintptr_t)word_base,
               (int)((word_base - stack) / 1024));
    loader_log(LOG_INFO, "  argc=%d  argv[0]=0x%08x",
               argc, argc > 0 ? argv_addrs[0] : 0);

    /* Return pointer to argc - this becomes esp on _start entry */
    /* Also publish the real argc/argv so __libc_start_main stub can use the
     * correct values regardless of how _start's call frame is reconstructed. */
    extern volatile uint32_t g_abi_argc;
    extern volatile uint32_t g_abi_argv;   /* address of argv[0] array in the word area */
    extern volatile uint32_t g_abi_stack_hi; /* a high, aligned spot in the ABI stack */
    g_abi_argc = (uint32_t)argc;
    g_abi_argv = (uint32_t)(uintptr_t)&wp[1]; /* wp[0]=argc, wp[1..]=argv[] */
    /* Give the stub a valid esp/ebp for calling main(): use a 16-byte-aligned
     * address a bit BELOW the word area (into the grow-room), so main()'s frame
     * and its [ebp-N] writes land on real, mapped ABI-stack memory rather than
     * on the loader's native stack/ebp (which is garbage after the fault
     * redirect). 8KB of headroom below argc is plenty for main's frame. */
    g_abi_stack_hi = ((uint32_t)(uintptr_t)word_base - 0x2000) & ~0xFu;
    return word_base;

#undef STACK32_GROW_ROOM
#undef STACK32_SIZE
}

/* ------------------------------------------------------------------ */
/* Jump to ELF _start with proper ABI stack                           */
/* ------------------------------------------------------------------ */
/*
 * This is the clean entry trampoline. We set rsp to our prepared stack,
 * zero rdx (ABI requirement: rtld_fini = NULL), then jmp to _start.
 *
 * Must be naked / no prologue so we don't corrupt our own rsp before
 * the jump. Implemented in inline asm.
 */
__attribute__((noreturn))
void auxv_jump_to_entry(uintptr_t entry, void *stack_top)
{
#ifdef _WIN64
    /* 64-bit: entry=rcx, stack_top=rdx (Windows x64 ABI) */
    __asm__ __volatile__ (
        "mov rsp, rdx\n\t"
        "xor rbp, rbp\n\t"
        "xor rdx, rdx\n\t"
        "jmp rcx\n\t"
        :: "c"(entry), "d"(stack_top)
    );
#else
    /* 32-bit: not used - ELF32 uses elf32_launch() instead */
    ExitProcess(1);
#endif
    __builtin_unreachable();
}

/* ELF32 entry trampoline - 64-bit host calling 32-bit ELF _start.
 * We can't actually switch to 32-bit mode from 64-bit Windows without
 * a mode switch (not possible in user mode).
 * Instead we run the 32-bit code in 64-bit long mode - works because:
 *   - x86-64 can execute all x86-32 instructions
 *   - Addressing is 32-bit (top 32 bits of pointers will be 0)
 *   - The ELF is loaded in low 32-bit address space
 * We just call the entry point directly as a function pointer.
 */
/*
 * Switch CPU from 64-bit (CS=0x33) to 32-bit compat mode (CS=0x23) and
 * jump to the ELF32 entry point. Windows maintains CS=0x23 for WoW64.
 *
 * A far return (rex64 retf) pops RIP then CS, performing the mode switch.
 * Stack layout before retf:
 *   [rsp+8]  0x23   <- 32-bit compat CS selector
 *   [rsp+0]  entry  <- 32-bit entry point
 *
 * On entry to 32-bit _start:
 *   esp = stack_top (32-bit ABI stack with argc/argv/envp/auxv)
 *   ebp = 0, edx = 0 (no rtld_fini)
 */
void auxv_jump_to_entry32(uintptr_t entry, void *stack_top)
{
#ifdef _WIN64
    /* Windows x64: entry in rcx, stack_top in rdx */
    __asm__ volatile (
        "mov rax, rcx\n\t"
        "mov rsp, rdx\n\t"
        "xor rbp, rbp\n\t"
        "xor rdx, rdx\n\t"
        "xor rbx, rbx\n\t"
        "xor rsi, rsi\n\t"
        "xor rdi, rdi\n\t"
        "sub rsp, 16\n\t"
        "mov qword ptr [rsp+8], 0x23\n\t"
        "mov qword ptr [rsp],   rax\n\t"
        "rex64 retf\n\t"
        ::: "memory", "rax", "rbx", "rdx", "rsi", "rdi", "rbp"
    );
#else
    (void)entry; (void)stack_top;
    ExitProcess(1);
#endif
    __builtin_unreachable();
}
