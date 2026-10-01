#include "compat32.h"
/*
 * main.c - Ultra Loader entry point
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>   /* SHGetFolderPathA / CSIDL_LOCAL_APPDATA */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "loader.h"
#include "patcher.h"
#include "iso_loader.h"
#include "lxdecrypt.h"
#include "glut_stub.h"
#include "segaapi_stub.h"
#include "auxv.h"
#include "glx_bridge.h"
#include "libgl_stub.h"
#include "threads.h"
#include "jvs_input.h"
#include "settings.h"
#include "shmem.h"
#include "lightgun_service.h"
#include "boot_console.h"    /* unified boot console (shared with module host + injector) */
#include "ultra_catalog.h"   /* unified system+title resolver */
#ifdef _WIN64
#include "uc_launcher.h"
#endif

extern PatchOptions g_patch_opts;

/* Fiber-based ELF launch: allows switching to a custom ABI stack with
 * Windows properly registering the new stack in the TEB (unlike direct
 * gs:[0x08]/[0x10] writes which are ignored on modern Windows). */
typedef struct { uintptr_t entry; void *stack; } FiberLaunchArgs;
static FiberLaunchArgs g_fiber_args;

/* Globals holding the ELF ABI args for _start64 to read directly.
 * This avoids needing to switch rsp to the ABI stack buffer. */
static uint64_t g_elf_argc = 0;
static uint64_t g_elf_argv_ptr = 0;  /* pointer to argv[0] */

/* NtWriteVirtualMemory - used to update TEB stack limits */
typedef LONG (__stdcall *NtWriteVirtualMemory_t)(HANDLE, PVOID, PVOID, SIZE_T, SIZE_T*);
static NtWriteVirtualMemory_t g_NtWVM = NULL;

static VOID CALLBACK elf_fiber_proc(LPVOID param)
{
    FiberLaunchArgs *a = (FiberLaunchArgs*)param;

    uintptr_t newrsp    = (uintptr_t)a->stack;
    uintptr_t new_limit = newrsp - (16 * 1024 * 1024) + 0x1000;
    uintptr_t new_base  = newrsp + 0x1000;

    /* Update TEB StackLimit/StackBase via WriteProcessMemory.
     * Direct gs:[0x08]/[0x10] writes are silently ignored on modern Windows.
     * WriteProcessMemory works because the TEB is ordinary mapped memory. */
    HANDLE self = GetCurrentProcess();
    /* NT_TIB layout: ExceptionList@0, StackBase@8, StackLimit@16 (in Win64) */
    /* Actually: StackBase = gs:[0x08], StackLimit = gs:[0x10] in NT_TIB */
    /* Get TEB via NtCurrentTeb() equivalent: gs:[0x30] = TEB self-pointer (Win64) */
    /* Use __readgsqword which is available in mingw */
    uintptr_t teb = (uintptr_t)NtCurrentTeb();
    if (teb) {
        SIZE_T written;
        WriteProcessMemory(self, (PVOID)(teb + 0x08), &new_limit, 8, &written);
        WriteProcessMemory(self, (PVOID)(teb + 0x10), &new_base,  8, &written);
    }

    /* Verify - use volatile load to bypass optimization */
    volatile uintptr_t *tib = (volatile uintptr_t*)teb;
    uintptr_t rl = tib[1];  /* offset 0x08 = index 1 of uintptr_t array */
    uintptr_t rb = tib[2];  /* offset 0x10 = index 2 */

    /* WriteFile-based log (safe even with wrong stack limits) */
    {
        char buf[128];
        int n = wsprintfA(buf, "[FIBER] TEB: StackLimit=0x%I64x (want 0x%I64x) Base=0x%I64x\r\n",
                          (unsigned long long)rl, (unsigned long long)new_limit,
                          (unsigned long long)rb);
        DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, n, &w, NULL);
    }

    /* Now switch stack and jump to ELF entry */
    {
        char buf[128];
        int n = wsprintfA(buf, "[FIBER] rsp->0x%I64x jmp->0x%I64x\r\n",
                          (unsigned long long)newrsp, (unsigned long long)a->entry);
        DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, n, &w, NULL);
    }

    uintptr_t entry = a->entry;
    __asm__ volatile (
        "mov rsp, %0\n\t"
        "xor rbp, rbp\n\t"
        "jmp %1\n\t"
        :: "r"(newrsp), "r"(entry)
        : "memory"
    );
    __builtin_unreachable();
}
static void print_banner(void)
{
    printf(
        "\n"
        "  ██╗   ██╗██╗  ████████╗██████╗  █████╗      ██╗      ██████╗  █████╗ ██████╗\n"
        "  ██║   ██║██║  ╚══██╔══╝██╔══██╗██╔══██╗     ██║     ██╔═══██╗██╔══██╗██╔══██╗\n"
        "  ██║   ██║██║     ██║   ██████╔╝███████║     ██║     ██║   ██║███████║██║  ██║\n"
        "  ██║   ██║██║     ██║   ██╔══██╗██╔══██║     ██║     ██║   ██║██╔══██║██║  ██║\n"
        "  ╚██████╔╝███████╗██║   ██║  ██║██║  ██║     ███████╗╚██████╔╝██║  ██║██████╔╝\n"
        "   ╚═════╝ ╚══════╝╚═╝   ╚═╝  ╚═╝╚═╝  ╚═╝     ╚══════╝ ╚═════╝ ╚═╝  ╚═╝╚═════╝\n"
        "  v" LOADER_VERSION ", Linux ELF Runner for Windows x64\n\n"
    );
}

static void print_usage(const char *prog)
{
    printf(
        "Usage: %s [options] <elf> [args...]\n\n"
        "Run options:\n"
        "  -v              Debug logging\n"
        "  -vv             Verbose (every syscall)\n"
        "  -list-syms      Dump symbol table and exit\n\n"
        "Patch options:\n"
        "  -no-backup      Don't create .bak of original\n"
        "  -no-patch       Skip patching (pre-patched ELFs)\n"
        "  -force-patch    Patch even if community-patched\n"
        "  -dry-run        Show patches, don't run\n"
        "  -restore        Restore original from .bak\n\n"
        "Controls:\n"
        "  P1: WASD=dirs  Z/X/C/V=btns  1=start  5=coin\n"
        "  P2: Arrows  Num1-4=btns  2=start  6=coin\n"
        "  F1=service  F2=test  ESC=exit\n\n",
        prog);
}

static void dump_symbols(LoaderCtx *ctx)
{
    printf("=== Symbol Table (%d entries) ===\n", ctx->symbol_count);
    for (int i = 0; i < ctx->symbol_count; i++)
        printf("  [%04d] 0x%016llx  %s\n", i,
               (unsigned long long)ctx->symbols[i].addr,
               ctx->symbols[i].name);
}


/* ------------------------------------------------------------------ */
/* Crash handler - catches access violations after ELF entry           */
/* ------------------------------------------------------------------ */
#include "thunk32.h"

static bool g_in_elf = false;

/* Override the CRT/winpthreads __stack_chk_fail so a tripped stack canary
 * (raised as __fastfail 0xC0000409, which bypasses our VEH and terminates
 * silently) logs WHERE it happened before dying. The canary-protected
 * functions in this loader are all winpthreads (pthread_cond_*timedwait,
 * mutex_timedlock, etc.), so this pinpoints a worker-thread pthread
 * stack-corruption. Diagnostic only - additive, ABC-safe (ABC never trips
 * a loader canary). Reads the immediate caller's return address off our
 * own frame and walks the EBP chain. */
__attribute__((used, noreturn))
void __stack_chk_fail(void)
{
    void *caller = __builtin_return_address(0);
    void *fp     = __builtin_frame_address(0);
    char b[256]; int n;
    n = wsprintfA(b, "[LBRGH/ERR] === STACK CANARY (__stack_chk_fail) === caller_ret=0x%08X fp=0x%08X tid=%lu\r\n",
                  (unsigned)(uintptr_t)caller, (unsigned)(uintptr_t)fp, GetCurrentThreadId());
    DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), b, n, &w, NULL);
    /* Walk EBP chain from our caller's frame for the loader-side call path. */
    uint32_t e = (uint32_t)(uintptr_t)fp;
    for (int d = 0; d < 10; d++) {
        if (e < 0x10000 || (e & 3) || IsBadReadPtr((void*)(uintptr_t)e, 8)) break;
        uint32_t *fr = (uint32_t*)(uintptr_t)e;
        n = wsprintfA(b, "  scf[%d] ebp=0x%08X ret=0x%08X\r\n", d, e, fr[1]);
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), b, n, &w, NULL);
        if (fr[0] <= e) break;
        e = fr[0];
    }
    FlushFileBuffers(GetStdHandle(STD_ERROR_HANDLE));
    TerminateProcess(GetCurrentProcess(), 0x409);
    for (;;) {}  /* noreturn */
}

static LONG WINAPI elf_crash_handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT          *ctx = ep->ContextRecord;

    /* DIAGNOSTIC (env ULTRA_VEH_TRACE=1): prove the VEH is entered and dump the
     * raw faulting code/EIP/fault before ANY guard/filter could drop it. Used to
     * chase a silent termination where no crash report appeared. Env-gated, so
     * inert for ABC and every normal run. */
    if (getenv("ULTRA_VEH_TRACE")) {
        char b[256];
        int n = wsprintfA(b, "[VEH] code=0x%08X EIP=0x%08X fault=0x%08X np=%lu tid=%lu\r\n",
                          (unsigned)er->ExceptionCode, (unsigned)ctx->Rip,
                          (unsigned)(er->NumberParameters>=2 ? er->ExceptionInformation[1] : 0),
                          (unsigned long)er->NumberParameters, GetCurrentThreadId());
        DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), b, n, &w, NULL);
        FlushFileBuffers(GetStdHandle(STD_ERROR_HANDLE));
    }

    /* Reentrancy guard: if the handler itself faults (bad stack read, etc.),
     * Windows re-enters it and we can spin forever (an unkillable hang where
     * even Ctrl+C can't break in, because the process is stuck in kernel
     * exception dispatch). On ANY re-entry, terminate immediately. */
    static volatile LONG s_in_handler = 0;
    if (InterlockedExchange(&s_in_handler, 1) != 0) {
        /* Already handling a crash and we faulted again - bail hard. */
        TerminateProcess(GetCurrentProcess(), 4);
        return EXCEPTION_CONTINUE_SEARCH; /* not reached */
    }

    if (!g_in_elf) { s_in_handler = 0; return EXCEPTION_CONTINUE_SEARCH; }

    /* ===================================================================
     * LAYER 1 - MALLEABLE SUBSTRATE (demand-paging)
     * If the game reads/writes an address that isn't committed yet, but the
     * address is a plausible ELF address (anywhere in the low 32-bit user
     * range, and NOT the null page), commit a zeroed page there on the spot
     * and resume. The environment thus materializes memory wherever the game
     * reaches - constructors/main can scribble freely and it "just works".
     * Only genuinely bad accesses (null page, or kernel range) fall through
     * to the crash path below. This is reusable for any system, not just
     * Lindbergh. */
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        uintptr_t bad = (uintptr_t)er->ExceptionInformation[1];

        /* MEMSNAP first-touch trap: if this fault lands on a page armed by
         * memsnap_veh_capture_arm() (e.g. Cruis'n Blast's main page), take a
         * full-range snapshot BEFORE the substrate demand-page path can
         * clobber the plaintext with a zeroed VirtualAlloc. One-shot per arm. */
        {
            extern int memsnap_veh_check(uintptr_t fault_addr, uintptr_t eip);
            if (memsnap_veh_check(bad, (uintptr_t)ctx->Rip)) {
                s_in_handler = 0;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        /* Plausible ELF data/code range: above the null guard page, below the
         * 3GB user boundary. The ELF's vaddrs live ~0x08000000-0x0A100000 and
         * its stack/heap/thunks are also in low memory; we accept the whole
         * low range so any in-image touch is satisfied, but we refuse the null
         * page (real null derefs) and high/kernel addresses (real bugs). */
        if (bad >= 0x00010000 && bad < 0xC0000000) {
            uintptr_t page = bad & ~((uintptr_t)0xFFF);
            void *got = VirtualAlloc((void*)page, 0x1000,
                                     MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (got) {
                extern void autoprofile_note_page(uint32_t page_addr);
                autoprofile_note_page((uint32_t)page);
                extern void snap_note_substrate(uint32_t page_addr);  /* capture this guest scribble page in Snapshots */
                snap_note_substrate((uint32_t)page);
                static long s_dp_count = 0;
                long c = InterlockedIncrement(&s_dp_count);
                /* Log the first handful and then every 256th, so we see the
                 * pattern (Layer 2 observation) without flooding. */
                if (c <= 32 || (c & 0xFF) == 0)
                    loader_log(LOG_WARN, "[SUBSTRATE] demand-paged 0x%08x (touch #%ld) eip=0x%08x",
                               (unsigned)page, c, (unsigned)ctx->Rip);
                s_in_handler = 0;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            /* VirtualAlloc failed - fall through to report it. */
        }
    }
    /* =================================================================== */

    /* MAINTRACE: software breakpoints at main()'s call sites. Catch INT3 and
     * single-step traps here so we can log which init function main reaches. */
    if (er->ExceptionCode == EXCEPTION_BREAKPOINT) {
        extern int eiptrace_bp_hit(CONTEXT *ctx);
        if (eiptrace_bp_hit(ctx)) { s_in_handler = 0; return EXCEPTION_CONTINUE_EXECUTION; }
        extern int eiptrace_on_breakpoint(CONTEXT *ctx);
        if (eiptrace_on_breakpoint(ctx)) { s_in_handler = 0; return EXCEPTION_CONTINUE_EXECUTION; }
        extern int maintrace_on_breakpoint(CONTEXT *ctx);
        if (maintrace_on_breakpoint(ctx)) { s_in_handler = 0; return EXCEPTION_CONTINUE_EXECUTION; }
    }
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        extern int eiptrace_on_singlestep(CONTEXT *ctx);
        if (eiptrace_on_singlestep(ctx)) { s_in_handler = 0; return EXCEPTION_CONTINUE_EXECUTION; }
        extern int maintrace_on_singlestep(CONTEXT *ctx);
        if (maintrace_on_singlestep(ctx)) { s_in_handler = 0; return EXCEPTION_CONTINUE_EXECUTION; }
    }

    /* Loop guard: if the SAME instruction faults repeatedly, the game is stuck
     * re-executing a bad access (e.g. a stub returned a non-dereferenceable
     * handle). Resuming would spin forever. Dump once, then hard-exit on the
     * second identical fault so the log stays readable instead of a storm. */
    {
        static uint64_t s_last_fault_eip = (uint64_t)-1;
        static int      s_fault_repeat   = 0;
        if ((uint64_t)ctx->Rip == s_last_fault_eip) {
            if (++s_fault_repeat >= 1) {
                const char m[] = "[LBRGH/ERR] same fault repeating, hard exit to avoid storm\r\n";
                DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), m, sizeof(m)-1, &w, NULL);
                ExitProcess(2);
            }
        } else {
            s_last_fault_eip = (uint64_t)ctx->Rip;
            s_fault_repeat   = 0;
        }
    }

    /* Only handle first-chance fatal exceptions. Includes the __fastfail
     * codes (0xC0000409 STATUS_STACK_BUFFER_OVERRUN from a GS stack-cookie
     * check / __chk failure, and 0xC0000602 STATUS_FAIL_FAST_EXCEPTION) so a
     * guest .so compiled with -fstack-protector that trips its canary still
     * gets the direct register/stack/EBP dump instead of vanishing silently. */
    #ifndef STATUS_STACK_BUFFER_OVERRUN
    #define STATUS_STACK_BUFFER_OVERRUN 0xC0000409
    #endif
    #ifndef STATUS_FAIL_FAST_EXCEPTION
    #define STATUS_FAIL_FAST_EXCEPTION  0xC0000602
    #endif
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION &&
        er->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION &&
        er->ExceptionCode != EXCEPTION_STACK_OVERFLOW &&
        er->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        er->ExceptionCode != (DWORD)STATUS_STACK_BUFFER_OVERRUN &&
        er->ExceptionCode != (DWORD)STATUS_FAIL_FAST_EXCEPTION)
        { s_in_handler = 0; return EXCEPTION_CONTINUE_SEARCH; }

    const char *type = "UNKNOWN";
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)    type = "ACCESS_VIOLATION";
    if (er->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) type = "ILLEGAL_INSTRUCTION";
    if (er->ExceptionCode == EXCEPTION_STACK_OVERFLOW)      type = "STACK_OVERFLOW";
    if (er->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO)  type = "DIV_BY_ZERO";
    if (er->ExceptionCode == (DWORD)STATUS_STACK_BUFFER_OVERRUN) type = "STACK_BUFFER_OVERRUN(GS/chk)";
    if (er->ExceptionCode == (DWORD)STATUS_FAIL_FAST_EXCEPTION)  type = "FAIL_FAST";

    /* DIRECT, UNBUFFERED crash summary FIRST. The buffered loader_log report
     * below can be lost if another thread tears the process down mid-write
     * (worker-thread fault during multi-threaded init). This WriteFile block
     * guarantees the essential triage data (type/EIP/fault/regs + top stack +
     * EBP return chain) always lands in the log. Additive: no control change. */
    {
        char b[512]; int n;
        uint32_t *ssp = (uint32_t*)(uintptr_t)ctx->Rsp;
        n = wsprintfA(b,
            "[LBRGH/ERR] === ELF CRASH (direct): %s ===\r\n"
            "  EIP=0x%08X  fault=0x%08X  acc=%s  ESP=0x%08X  EBP=0x%08X\r\n"
            "  EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X ESI=0x%08X EDI=0x%08X\r\n",
            type, (unsigned)ctx->Rip,
            (unsigned)(er->NumberParameters>=2 ? er->ExceptionInformation[1] : 0),
            (er->NumberParameters>=1 && er->ExceptionInformation[0]) ? "W" : "R",
            (unsigned)ctx->Rsp, (unsigned)ctx->Rbp,
            (unsigned)ctx->Rax,(unsigned)ctx->Rbx,(unsigned)ctx->Rcx,
            (unsigned)ctx->Rdx,(unsigned)ctx->Rsi,(unsigned)ctx->Rdi);
        DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), b, n, &w, NULL);
        { extern DWORD g_ul_main_tid;
          n = wsprintfA(b, "  faulting TID=%lu (main TID=%lu)%s\r\n",
                        GetCurrentThreadId(), g_ul_main_tid,
                        GetCurrentThreadId()==g_ul_main_tid ? "" : "  <-- WORKER THREAD");
          WriteFile(GetStdHandle(STD_ERROR_HANDLE), b, n, &w, NULL); }
        /* top 8 stack words */
        if (ssp && !IsBadReadPtr(ssp, 8*4)) {
            n = wsprintfA(b, "  stk: %08X %08X %08X %08X %08X %08X %08X %08X\r\n",
                ssp[0],ssp[1],ssp[2],ssp[3],ssp[4],ssp[5],ssp[6],ssp[7]);
            WriteFile(GetStdHandle(STD_ERROR_HANDLE), b, n, &w, NULL);
        }
        /* EBP return chain (the call path) */
        uint32_t e = (uint32_t)ctx->Rbp;
        for (int d = 0; d < 8; d++) {
            if (e < 0x10000 || (e & 3) || IsBadReadPtr((void*)(uintptr_t)e, 8)) break;
            uint32_t *fr = (uint32_t*)(uintptr_t)e;
            n = wsprintfA(b, "  ebp[%d]=0x%08X ret=0x%08X\r\n", d, e, fr[1]);
            WriteFile(GetStdHandle(STD_ERROR_HANDLE), b, n, &w, NULL);
            if (fr[0] <= e) break;
            e = fr[0];
        }
        /* ESP scan for ELF .text return addresses (survives a clobbered EBP) */
        if (ssp && !IsBadReadPtr(ssp, 64*4)) {
            for (int i = 0; i < 64; i++) {
                uint32_t v = ssp[i];
                if (v >= 0x08048000 && v < 0x09000000) {
                    n = wsprintfA(b, "  txt[esp+%d]=0x%08X\r\n", i*4, v);
                    WriteFile(GetStdHandle(STD_ERROR_HANDLE), b, n, &w, NULL);
                }
            }
        }
    }

    loader_log(LOG_ERROR, "=== ELF CRASH: %s ===", type);
    {
        extern volatile int g_cur_ctor_idx;
        extern volatile uint32_t g_cur_ctor_addr;
        if (g_cur_ctor_idx >= 0)
            loader_log(LOG_ERROR, "  (occurred during ctor[%d] @0x%08x)",
                       g_cur_ctor_idx, (unsigned)g_cur_ctor_addr);
    }
    loader_log(LOG_ERROR, "  Fault address:  0x%08llx", (unsigned long long)er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        loader_log(LOG_ERROR, "  Access type:    %s", er->ExceptionInformation[0] ? "WRITE" : "READ");
        loader_log(LOG_ERROR, "  Bad address:    0x%08llx", (unsigned long long)er->ExceptionInformation[1]);
    }
    loader_log(LOG_ERROR, "  EIP/RIP:  0x%08llx", (unsigned long long)ctx->Rip);
    loader_log(LOG_ERROR, "  ESP/RSP:  0x%08llx", (unsigned long long)ctx->Rsp);
    loader_log(LOG_ERROR, "  EAX/RAX:  0x%08llx", (unsigned long long)ctx->Rax);
    loader_log(LOG_ERROR, "  EBX/RBX:  0x%08llx", (unsigned long long)ctx->Rbx);
    loader_log(LOG_ERROR, "  ECX/RCX:  0x%08llx", (unsigned long long)ctx->Rcx);
    loader_log(LOG_ERROR, "  EDX/RDX:  0x%08llx", (unsigned long long)ctx->Rdx);
    loader_log(LOG_ERROR, "  EBP:0x%08llx  ESI:0x%08llx  EDI:0x%08llx",
               (unsigned long long)ctx->Rbp, (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi);

    /* Print bytes at fault address (guarded against an unreadable RIP) */
    uint8_t *rip = (uint8_t*)(uintptr_t)ctx->Rip;
    if (rip && (uintptr_t)rip >= 0x1000 && !IsBadReadPtr(rip, 8)) {
        loader_log(LOG_ERROR, "  Bytes at RIP: %02x %02x %02x %02x %02x %02x %02x %02x",
                   rip[0],rip[1],rip[2],rip[3],rip[4],rip[5],rip[6],rip[7]);
    }

    /* DIAGNOSTIC (additive, env-gated): dump 0x40 bytes around each GP register
     * that points into a readable region. Used to inspect the GSEVO mh-heap block
     * headers at the fault (EBX=corrupt free block, EAX=heap workslot). Inert
     * unless ULTRA_CRASH_MEM=1, so ABC's gate run is byte-identical. */
    if (getenv("ULTRA_CRASH_MEM")) {
        struct { const char *nm; uint32_t v; } regs[] = {
            {"EAX",(uint32_t)ctx->Rax},{"EBX",(uint32_t)ctx->Rbx},
            {"ECX",(uint32_t)ctx->Rcx},{"EDX",(uint32_t)ctx->Rdx},
            {"ESI",(uint32_t)ctx->Rsi},{"EDI",(uint32_t)ctx->Rdi},
        };
        for (int r = 0; r < 6; r++) {
            uint32_t a = regs[r].v;
            if (a < 0x1000) continue;
            uint8_t *m = (uint8_t*)(uintptr_t)a;
            if (IsBadReadPtr(m, 0x60)) { loader_log(LOG_ERROR, "  MEM[%s=0x%08x]: <unreadable>", regs[r].nm, a); continue; }
            uint32_t *w = (uint32_t*)m;
            loader_log(LOG_ERROR, "  MEM[%s=0x%08x] +00: %08x %08x %08x %08x %08x %08x %08x %08x",
                       regs[r].nm, a, w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7]);
            loader_log(LOG_ERROR, "  MEM[%s=0x%08x] +20: %08x %08x %08x %08x %08x %08x %08x %08x",
                       regs[r].nm, a, w[8],w[9],w[10],w[11],w[12],w[13],w[14],w[15]);
            loader_log(LOG_ERROR, "  MEM[%s=0x%08x] +40: %08x %08x %08x %08x %08x %08x %08x %08x",
                       regs[r].nm, a, w[16],w[17],w[18],w[19],w[20],w[21],w[22],w[23]);
        }
        /* GSEVO event-list sublist walk: ebx is a ctrl-event node; its sublist
         * head is [ebx+0x40] and the chain advances by +0x48, with +0x3c the
         * field the faulting loop zeroes. Walk + print up to 12 nodes so we can
         * see where a NULL/garbage link enters the chain. */
        {
            uint32_t ebx = (uint32_t)ctx->Rbx;
            if (ebx >= 0x1000 && !IsBadReadPtr((void*)(uintptr_t)ebx, 0x4c)) {
                uint32_t head = *(uint32_t*)(uintptr_t)(ebx + 0x40);
                loader_log(LOG_ERROR, "  EVT ebx=0x%08x flags[+8]=%08x sublist-head[+0x40]=%08x [+0x44]=%08x [+0x3c]=%08x [+0x48]=%08x",
                           ebx, *(uint32_t*)(uintptr_t)(ebx+8), head,
                           *(uint32_t*)(uintptr_t)(ebx+0x44),
                           *(uint32_t*)(uintptr_t)(ebx+0x3c),
                           *(uint32_t*)(uintptr_t)(ebx+0x48));
                uint32_t n = head; int steps = 0;
                while (n && steps < 12) {
                    if (IsBadReadPtr((void*)(uintptr_t)n, 0x4c)) {
                        loader_log(LOG_ERROR, "    sub[%d] @0x%08x <unreadable>", steps, n); break;
                    }
                    uint32_t nx = *(uint32_t*)(uintptr_t)(n + 0x48);
                    loader_log(LOG_ERROR, "    sub[%d] @0x%08x flags[+8]=%08x [+0x3c]=%08x next[+0x48]=%08x",
                               steps, n, *(uint32_t*)(uintptr_t)(n+8),
                               *(uint32_t*)(uintptr_t)(n+0x3c), nx);
                    n = nx; steps++;
                }
            }
        }
        /* Dump LIVE code bytes around the faulting EIP region so we can confirm the
         * in-memory instructions match the ELF file (catches a relocation/patch that
         * altered the test/jne and explains an "impossible" branch). */
        {
            uint32_t code_lo = (uint32_t)ctx->Rip & ~0xfu;
            if (code_lo >= 0x10) code_lo -= 0x10;
            if (!IsBadReadPtr((void*)(uintptr_t)code_lo, 0x30)) {
                uint8_t *c = (uint8_t*)(uintptr_t)code_lo;
                char line[160]; int p = 0;
                p += wsprintfA(line+p, "  CODE@0x%08x:", code_lo);
                for (int k = 0; k < 0x30; k++) p += wsprintfA(line+p, " %02x", c[k]);
                loader_log(LOG_ERROR, "%s", line);
            }
        }
        /* heap-2 workslot is in guest .bss at 0x086602b8; dump it + arena root. */
        uint32_t ws = 0x086602b8;
        if (!IsBadReadPtr((void*)(uintptr_t)ws, 0x2c)) {
            uint32_t *w = (uint32_t*)(uintptr_t)ws;
            loader_log(LOG_ERROR, "  HEAP2 workslot@0x%08x: [0]=%08x [+4]=%08x [+8arena]=%08x [+0xc]=%08x [+0x10]=%08x [+0x24tag]=%08x",
                       ws, w[0],w[1],w[2],w[3],w[4],w[9]);
            uint32_t arena = w[2];   /* [workslot+8] = arena root block */
            if (arena >= 0x1000 && !IsBadReadPtr((void*)(uintptr_t)arena, 0x24)) {
                uint32_t *ar = (uint32_t*)(uintptr_t)arena;
                loader_log(LOG_ERROR, "  HEAP2 arena root@0x%08x: tag=%08x size=%08x flag=%08x prev=%08x next=%08x canary=%08x",
                           arena, ar[0],ar[1],ar[2],ar[3],ar[4],ar[7]);
            }
        }
    }

    /* Stack trace - print first 16 words (guarded against a bad ESP) */
    loader_log(LOG_ERROR, "  Stack (top 16 words):");
    uint32_t *sp = (uint32_t*)(uintptr_t)ctx->Rsp;
    if (sp && !IsBadReadPtr(sp, 16*4)) {
        for (int i = 0; i < 16; i++) {
            loader_log(LOG_ERROR, "    [esp+%02d] = 0x%08x", i*4, sp[i]);
        }
    }

    /* For a null-pointer call (EIP≈0), print the ELF return-chain NOW - the EBP
     * walk just below can itself fault on a clobbered frame, which would abort
     * the dump before the caller is ever reported. */
    if (ctx->Rip < 0x10000 && sp && !IsBadReadPtr(sp, 160*4)) {
        loader_log(LOG_ERROR, "  NULL-call caller chain (ESP scan):");
        int nf = 0;
        for (int i = 0; i < 160; i++) {
            uint32_t v = sp[i];
            if (v >= 0x0804f074 && v < 0x081f352c) {
                loader_log(LOG_ERROR, "    callsite[%d]: [esp+%-4d] = 0x%08x", nf, i*4, v);
                if (++nf >= 24) break;
            }
        }
        if (!nf) loader_log(LOG_ERROR, "    (no ELF .text return in 160 words)");
        extern void eiptrace_dump(void);
        eiptrace_dump();
        fflush(stdout);
    }

    /* Add EBP stack trace - guard every frame read so the handler can NEVER
     * fault on itself (a bad EBP at crash time was making this walk crash,
     * masking the real fault and tripping the loop guard). */
    loader_log(LOG_ERROR, "  Call stack (EBP chain):");
    uint32_t ebp = ctx->Rbp;
    for (int depth = 0; depth < 8; depth++) {
        /* frame must be aligned, in a plausible range, and actually readable */
        if (ebp < 0x10000 || ebp >= 0xfffff000 || (ebp & 3) != 0) break;
        if (IsBadReadPtr((void*)(uintptr_t)ebp, 8)) break;
        uint32_t *frame = (uint32_t*)ebp;
        uint32_t ret_addr = frame[1];
        uint32_t next_ebp = frame[0];
        loader_log(LOG_ERROR, "    [%2d] EBP=0x%08x ret=0x%08x", depth, ebp, ret_addr);
        if (next_ebp <= ebp) break;
        ebp = next_ebp;
    }

    /* DIAGNOSTIC (additive, inert unless ULTRA_TRACE_AT armed the ring): dump the
     * single-step EIP/ESP ring for EVERY crash type, not only the EIP=0 null-call
     * path below. Needed to debug in-range faults (e.g. the GSEVO mh-heap fault at
     * 0x080f3818). eiptrace_dump() is a no-op when tracing was never armed, so
     * non-traced runs (including ABC's gate) are byte-identical. */
    { extern void eiptrace_dump(void); eiptrace_dump(); fflush(stdout); }

    /* EIP=0 means code called through a null pointer.
     *
     * NOTE: the legitimate _start -> __libc_start_main bootstrap does NOT reach
     * here - the GOT slot at 0x0833d0ec is pre-patched to our stub before the
     * ELF runs, so _start calls the stub directly. Therefore ANY EIP=0 that
     * reaches this handler is a real null function-pointer call in game/ctor
     * code (an unresolved import or uninitialized callback). Report where it
     * came from and stop; do NOT redirect to __libc_start_main (that re-runs
     * ctors+main on a corrupted stack and cascades). */
    if (ctx->Rip < 0x10000 && er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uint32_t *sp = (uint32_t*)(uintptr_t)ctx->Rsp;
        loader_log(LOG_ERROR, "  EIP=0 NULL call/ret-to-zero");
        /* Print the caller chain BEFORE attempting repair - autoprofile may
         * resume (and re-fault), so we must capture the call path first. */
        if (sp && !IsBadReadPtr(sp, 160*4)) {
            int found0 = 0;
            for (int i = 0; i < 160; i++) {
                uint32_t v = sp[i];
                if (v >= 0x0804f074 && v < 0x081f352c) {
                    loader_log(LOG_ERROR, "    callchain[%d]: [esp+%-4d] = 0x%08x", found0, i*4, v);
                    if (++found0 >= 24) break;
                }
            }
            if (!found0) loader_log(LOG_ERROR, "    (no ELF .text return in 160 words)");
        }
        fflush(stdout);
        /* REAL-TIME REPAIR: try to resolve this null call from the ELF's own
         * GOT/symbol tables and patch it live. If derivable, this records the
         * fix into the profile and resumes into the real target. */
        extern int autoprofile_repair_null_call(CONTEXT *ctx);
        if (autoprofile_repair_null_call(ctx)) {
            s_in_handler = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        loader_log(LOG_ERROR, "  (not auto-repairable)");
        /* Walk the stack and report EVERY value that lands in the ELF .text
         * range (0x0804f074..0x081f352c) - the full surviving return-address
         * chain. The shallowest such address is the function that made the
         * null call (or whose return address was clobbered to 0); the rest
         * give the call path up to main. A gap of zeros before the first hit
         * indicates a clobbered return slot (stack corruption). */
        if (sp && !IsBadReadPtr(sp, 160*4)) {
            int found = 0, first_zero_run = -1;
            for (int i = 0; i < 160; i++) {
                uint32_t v = sp[i];
                if (v >= 0x0804f074 && v < 0x081f352c) {
                    loader_log(LOG_ERROR, "    chain[%d]: [esp+%-4d] = 0x%08x  <-- ELF .text return",
                               found, i*4, v);
                    if (++found >= 24) break;
                }
                if (v == 0 && first_zero_run < 0) first_zero_run = i;
            }
            if (!found)
                loader_log(LOG_ERROR, "    (no ELF .text return address found in 160 words, stack wiped)");
            else if (first_zero_run >= 0)
                loader_log(LOG_ERROR, "    first zero word at [esp+%d] (possible clobbered return slot)", first_zero_run*4);
            /* Raw window for context. */
            for (int i = 0; i < 16; i++)
                loader_log(LOG_ERROR, "    raw [esp+%02d] = 0x%08x", i*4, sp[i]);
        }
        fflush(stdout);
        /* If a debugger is attached, stop HERE with the faulting context still
         * live (ctx holds the faulting Eip/Esp; the faulting stack at ctx->Rsp
         * is intact and readable). Otherwise exit as before. */
        if (IsDebuggerPresent()) { DebugBreak(); }
        ExitProcess(3);
    }

    if (IsDebuggerPresent()) { DebugBreak(); }
    ExitProcess(1);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* Host log callback exposed to an optional extension module. */
void ultra_ext_log(const char *msg) { if (msg) loader_log(LOG_INFO, "[EXT] %s", msg); }

/* Set an env var through every channel Mesa might read: the Win32 environment,
 * our own CRT, AND ucrtbase.dll's CRT (Mesa/libgallium_wgl reads getenv against
 * ucrtbase's snapshot, which SetEnvironmentVariableA does NOT update). */
static void ul_set_env_all(const char *name, const char *val)
{
    char buf[192];
    snprintf(buf, sizeof buf, "%s=%s", name, val);
    SetEnvironmentVariableA(name, val);
    _putenv(buf);
    HMODULE uc = GetModuleHandleA("ucrtbase.dll");
    if (!uc) uc = LoadLibraryA("ucrtbase.dll");
    if (uc) {
        int (*uc_putenv)(const char*) = (int(*)(const char*))GetProcAddress(uc, "_putenv");
        if (uc_putenv) uc_putenv(buf);
        int (*uc_putenv_s)(const char*,const char*) = (int(*)(const char*,const char*))GetProcAddress(uc, "_putenv_s");
        if (uc_putenv_s) uc_putenv_s(name, val);
    }
}

/* ============================================================================
 *  P5 - RETRO ROUTING. Ultra Loader is 32-bit (it maps 32-bit Lindbergh ELFs);
 *  the retro emulator cores are 64-bit. So retro content is handed to the
 *  separate 64-bit ultra-module-host.exe (re-exec style), which loads the right
 *  module DLL + drives the core. Native ELFs fall through unchanged.
 * ========================================================================== */

/* Find a file next to the loader, or in the dev modtest\ staging dir. */
static int ul_find_near(const char *exe_dir, const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s\\%s", exe_dir, name);
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 1;
    snprintf(out, cap, "%s\\modtest\\%s", exe_dir, name);
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 1;

    /* Per-user persistent install: %LOCALAPPDATA%\Ultra Loader\<core>\<name>
     * Lets the user run the loader exe from anywhere (USB stick, custom dir)
     * while installed module cores follow the user across loader moves. */
    char appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appdata))) {
        char root[MAX_PATH];
        snprintf(root, sizeof root, "%s\\Ultra Loader", appdata);
        snprintf(out, cap, "%s\\%s", root, name);
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 1;
        char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*", root);
        WIN32_FIND_DATAA fd; HANDLE hf = FindFirstFileA(pat, &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.cFileName[0] == '.') continue;
                snprintf(out, cap, "%s\\%s\\%s", root, fd.cFileName, name);
                if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) { FindClose(hf); return 1; }
            } while (FindNextFileA(hf, &fd));
            FindClose(hf);
        }
    }
    return 0;
}

/* Decide whether `path` is retro content and which module DLL handles it.
 * Returns 1 + fills mod_out (absolute DLL path) for retro; 0 for native ELF.
 * ULTRA_MODULE=flycast|fbneo|mame|triforce|chihiro forces a module. */
static int ul_route_retro(const char *path, const char *exe_dir,
                          char *mod_out, size_t mod_cap)
{
    const char *modname = NULL;
    const char *forced = getenv("ULTRA_MODULE");
    char forced_dll[64];
    if (forced && *forced) {
        snprintf(forced_dll, sizeof forced_dll, "ultra_%s.dll", forced);
        modname = forced_dll;
    } else {
        size_t n = strlen(path);
        #define EXT(s) (n >= sizeof(s)-1 && _stricmp(path + n - (sizeof(s)-1), s) == 0)
        if      (EXT(".gdi")||EXT(".chd")||EXT(".cdi")||EXT(".cue")) modname = "ultra_flycast.dll";
        else if (EXT(".gcm")||EXT(".rvz")||EXT(".gcz")||EXT(".tgc")) modname = "ultra_triforce.dll";
        else if (EXT(".xbe"))                                        modname = "ultra_chihiro.dll";
        else if (EXT(".zip")||EXT(".7z")||EXT(".fba")||EXT(".lst"))  modname = "ultra_fbneo.dll";
        /* Native PC arcade (Taito Type X/X2/X3, NESiCA, RingEdge PC, etc.): a Windows
         * PE .exe. The native_x86 module launches it suspended and injects the
         * clean-room I/O + D3D8 display hook (which reads ULTRA_RES/ROTATE/FPS/ASPECT
         * set by settings_apply). Native Lindbergh/ABC games are ELF (no .exe), so
         * this doesn't catch them. */
        else if (EXT(".exe"))                                        modname = "ultra_native_x86.dll";
        #undef EXT
    }
    if (!modname) return 0;                       /* native ELF */
    if (!ul_find_near(exe_dir, modname, mod_out, mod_cap)) {
        loader_log(LOG_WARN, "[ROUTE] retro content but %s not next to loader; trying native", modname);
        return 0;
    }
    return 1;
}

/* Launch the 64-bit module host for retro content, wait, return its exit code. */
static int ul_launch_module_host(const char *exe_dir, const char *mod_dll, const char *content)
{
    char host[MAX_PATH];
    if (!ul_find_near(exe_dir, "ultra-module-host.exe", host, sizeof host)) {
        loader_log(LOG_ERROR, "[ROUTE] ultra-module-host.exe not found (required for retro content)");
        return 1;
    }
    char cmd[2300];
    snprintf(cmd, sizeof cmd, "\"%s\" \"%s\" \"%s\"", host, mod_dll, content);
    loader_log(LOG_INFO, "[ROUTE] retro content -> %s", cmd);

    /* Forward the user's lightgun calibration to the 64-bit host. The host runs
     * its own Ultra Lightgun Service (it owns the game window where raw-input
     * actually lands), so it just needs the calibration knobs from g_settings. */
    {
        char buf[32];
        snprintf(buf, sizeof buf, "%d", g_settings.lightgun_enable);   SetEnvironmentVariableA("ULTRA_LIGHTGUN_ENABLE", buf);
        snprintf(buf, sizeof buf, "%d", g_settings.lightgun_x_sens);   SetEnvironmentVariableA("ULTRA_LIGHTGUN_XSENS",  buf);
        snprintf(buf, sizeof buf, "%d", g_settings.lightgun_y_sens);   SetEnvironmentVariableA("ULTRA_LIGHTGUN_YSENS",  buf);
        snprintf(buf, sizeof buf, "%d", g_settings.lightgun_x_offset); SetEnvironmentVariableA("ULTRA_LIGHTGUN_XOFF",   buf);
        snprintf(buf, sizeof buf, "%d", g_settings.lightgun_y_offset); SetEnvironmentVariableA("ULTRA_LIGHTGUN_YOFF",   buf);
        snprintf(buf, sizeof buf, "%d", g_settings.lightgun_invert_x); SetEnvironmentVariableA("ULTRA_LIGHTGUN_INVX",   buf);
        snprintf(buf, sizeof buf, "%d", g_settings.lightgun_invert_y); SetEnvironmentVariableA("ULTRA_LIGHTGUN_INVY",   buf);
    }

    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si; memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, exe_dir, &si, &pi)) {
        loader_log(LOG_ERROR, "[ROUTE] CreateProcess(module host) failed (%lu)", GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return (int)code;
}

DWORD g_ul_main_tid = 0;   /* main (guest entry) thread id - crash dump tags worker faults */

/* Exported entry point. The loader's dispatcher (work/src/main.c) calls this
 * after detecting an ELF game. The function takes the SAME argv the loader
 * received so all CLI flags (-v, -no-patch, etc.) still work. */
__declspec(dllexport)
int ul_lindbergh_run(int argc, char **argv)
{
    g_ul_main_tid = GetCurrentThreadId();
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    char exe_dir[MAX_PATH];
    GetModuleFileNameA(NULL, exe_dir, sizeof exe_dir);
    { char *s = strrchr(exe_dir, '\\'); if (s) *s = 0; }

    /* ---- Ultra Loader settings: load ultra.ini, optionally open the menu ---- */
    settings_load(exe_dir);
    /* Bring up the universal lightgun engine right after settings load. The
     * backend creates its own message-only window and self-pumps, so this is
     * safe before any HWND exists. Calibration is seeded from g_settings. */
    lightgun_init(NULL);
    atexit(lightgun_shutdown);   /* clean raw-input registration + HWND teardown */
    {
        int want_menu = 0;
        for (int i = 1; i < argc; i++)
            if (!strcmp(argv[i], "-settings") || !strcmp(argv[i], "-config") || !strcmp(argv[i], "-menu"))
                want_menu = 1;
        /* No game on the command line and no remembered game -> open the menu too. */
        if (want_menu) {
            int launch = settings_show_menu(exe_dir);
            if (!launch) return 0;   /* saved/canceled without launching */
        }
    }

    /* Console window: the exe is built as a GUI-subsystem app (-mwindows) so NO
     * console is ever auto-shown - no jarring flash on launch. A console is
     * created on demand only when the user asks for one (-console / -v), and the
     * standard streams are reattached to it so logs are visible. When stdio is
     * already redirected (e.g. the test harness piping to a file), the inherited
     * handles are kept as-is and we do NOT steal them with a new console. */
    {
        int want_console = 0;
        for (int i = 1; i < argc; i++)
            if (!strcmp(argv[i], "-console") || !strcmp(argv[i], "-v") || !strcmp(argv[i], "-verbose"))
                { want_console = 1; break; }
        int stdout_redirected = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_UNKNOWN;
        if (want_console && GetConsoleWindow() == NULL && !stdout_redirected) {
            if (AllocConsole()) {
                freopen("CONOUT$", "w", stdout);
                freopen("CONOUT$", "w", stderr);
                freopen("CONIN$",  "r", stdin);
                setvbuf(stderr, NULL, _IONBF, 0);
                setvbuf(stdout, NULL, _IONBF, 0);
                SetConsoleTitleA("Ultra Loader, console");
            }
        }
    }

    /* Select the GL driver BEFORE anything loads opengl32.dll / libgallium_wgl.dll
     * (libgl_register_stubs, ~line 599). Modes via ULTRA_GL_DRIVER:
     *   native (=gpu/amd/nvidia/intel/system) - the GPU vendor's REAL OpenGL
     *       driver (hardware, fast). PROVEN best: the bundled Mesa is shadowed by
     *       pre-loading the system opengl32 ICD. Works on AMD/Nvidia/Intel (all
     *       expose a compatibility profile + ARB assembly programs).
     *   llvmpipe (default) - Mesa software rasterizer, always works, no GPU.
     *   zink - Mesa GL-on-Vulkan; d3d12 - Mesa DirectX12 (both still wedge on
     *       this game's render, kept for experimentation).
     * An explicit GALLIUM_DRIVER env still selects a Mesa backend. */
    {
        char cur[64];
        const char *drv;
        if (GetEnvironmentVariableA("GALLIUM_DRIVER", cur, sizeof cur) > 0) {
            drv = cur;                              /* explicit Mesa override wins */
        } else {
            drv = getenv("ULTRA_GL_DRIVER");
            /* Default to the real vendor GPU driver (native): the user has a
             * capable GPU (AMD RX 580) and the software path (llvmpipe) throws
             * render_pass GL errors in gameplay. ULTRA_GL_DRIVER=llvmpipe forces
             * software if native ever misbehaves. */
            if (!drv || !*drv) drv = "native";
        }
        /* Export the RESOLVED driver so every downstream consumer (notably the GL
         * shim's offscreen/fill gate in glx_emu.c) sees the SAME decision. On a
         * plain double-click the process inherits no ULTRA_GL_DRIVER, so without
         * this the shim's getenv() returns NULL, offscreen turns OFF, and the game
         * renders straight to a 640x480 window with no fill. This is the single
         * source of truth for which driver we picked. */
        ul_set_env_all("ULTRA_GL_DRIVER", drv);
        int native = (!_stricmp(drv,"native") || !_stricmp(drv,"gpu") || !_stricmp(drv,"system") ||
                      !_stricmp(drv,"amd") || !_stricmp(drv,"nvidia") || !_stricmp(drv,"intel"));
        if (native) {
            /* Pre-load the system (SysWOW64 on 64-bit Windows) opengl32.dll so
             * every later "opengl32.dll" load - ours AND SDL's - resolves to the
             * vendor ICD instead of the bundled Mesa shim (module names are keyed
             * by base name, so loading it first by full path wins). */
            char sysdir[MAX_PATH], sysgl[MAX_PATH];
            GetSystemDirectoryA(sysdir, sizeof sysdir);
            snprintf(sysgl, sizeof sysgl, "%s\\opengl32.dll", sysdir);
            HMODULE h = LoadLibraryA(sysgl);
            loader_log(LOG_INFO, "[GL] driver = native vendor GL (%s = %p)", sysgl, h);
        } else {
            /* Mesa path: pre-load the BUNDLED Mesa opengl32.dll by full path so it
             * claims the "opengl32.dll" module name (we no longer static-link it). */
            char mesagl[MAX_PATH];
            snprintf(mesagl, sizeof mesagl, "%s\\opengl32.dll", exe_dir);
            HMODULE hm = LoadLibraryA(mesagl);
            if (!hm) hm = LoadLibraryA("opengl32.dll");   /* fallback to search */
            char tmp[64];
            if (GetEnvironmentVariableA("GALLIUM_DRIVER", tmp, sizeof tmp) == 0)
                ul_set_env_all("GALLIUM_DRIVER", drv);
            int is_hw = (!_stricmp(drv, "zink") || !_stricmp(drv, "d3d12"));
            if (is_hw) {
                ul_set_env_all("mesa_glthread", "false");
                ul_set_env_all("MESA_GLTHREAD",  "false");
                if (!_stricmp(drv, "d3d12")) ul_set_env_all("GALLIUM_THREAD", "0");
            }
            loader_log(LOG_INFO, "[GL] driver = %s%s (Mesa %s=%p)", drv,
                       is_hw ? " (hardware; glthread off)" : "", mesagl, hm);
        }
    }
    print_banner();

    /* NOTE: no early exit on argc<2. Double-clicking the exe (GUI subsystem, no
     * args) must fall through to the menu fallback below (arg_start>=argc), which
     * opens the settings menu or launches the remembered game. -h prints usage. */

    int  arg_start      = 1;
    bool list_syms      = false;
    bool do_restore     = false;
    bool do_dry_run     = false;
    bool skip_patch     = false;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-v"))           { g_log_level = LOG_DEBUG; arg_start = i+1; }
        else if (!strcmp(argv[i], "-vv"))          { g_log_level = LOG_DEBUG; arg_start = i+1; }
        else if (!strcmp(argv[i], "-list-syms"))   { list_syms  = true;       arg_start = i+1; }
        else if (!strcmp(argv[i], "-no-backup"))   { g_patch_opts.skip_backup = true; arg_start = i+1; }
        else if (!strcmp(argv[i], "-no-patch"))    { skip_patch = true;       arg_start = i+1; }
        else if (!strcmp(argv[i], "-force-patch")) { g_patch_opts.force = true; arg_start = i+1; }
        else if (!strcmp(argv[i], "-dry-run"))     { do_dry_run = true;       arg_start = i+1; }
        else if (!strcmp(argv[i], "-restore"))     { do_restore = true;       arg_start = i+1; }
        else if (!strcmp(argv[i], "-console"))     {                          arg_start = i+1; }
        else if (!strcmp(argv[i], "-settings") || !strcmp(argv[i], "-config") || !strcmp(argv[i], "-menu"))
            { arg_start = i+1; }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
            { print_usage(argv[0]); return 0; }
        else if (argv[i][0] == '-' && argv[i][1] == 'l' && argv[i][2] == 'p' && argv[i][3] == 't') {
            /* Raw Thrills convention: -lpt<hex-base> selects the game's LPT
             * base register (0x378 default). Configures the shim here so the
             * portio_veh handler routes the right byte range. */
            uint16_t base = 0;
            extern bool lpt_shim_parse_argv(const char*, uint16_t*);
            extern uint16_t lpt_shim_configure(uint16_t);
            if (lpt_shim_parse_argv(argv[i], &base)) lpt_shim_configure(base);
            else                                     lpt_shim_configure(0x378);
            arg_start = i+1;
        }
        else { arg_start = i; break; }
    }

    static char *menu_argv[2];
    int have_game = (arg_start < argc && argv[arg_start] && argv[arg_start][0]);
    /* A remembered game counts only if the FILE still exists - a stale/garbage ini
     * value (e.g. a stray newline) then falls back to the menu rather than failing. */
    int saved_ok  = (g_settings.game_path[0] &&
                     GetFileAttributesA(g_settings.game_path) != INVALID_FILE_ATTRIBUTES);
    loader_log(LOG_INFO, "[ARGS] argc=%d game=[%s] saved=[%s] saved_ok=%d boot_to_menu=%d",
               argc, have_game ? argv[arg_start] : "(none)", g_settings.game_path, saved_ok, g_settings.boot_to_menu);

    /* ── No module core installed ───────────────────────────────────────────────
     * If no game is on the command line AND no saved game path is valid, the shell
     * has nothing to load - this is the "waiting for module core" state. We print
     * a clear message before falling into the settings menu. */
    if (!have_game && !saved_ok) {
        loader_log(LOG_INFO, "");
        loader_log(LOG_INFO, "  +------------------------------------------+");
        loader_log(LOG_INFO, "  |   ULTRA LOADER, No module core found.   |");
        loader_log(LOG_INFO, "  |   Add a core to core/ to begin playing.  |");
        loader_log(LOG_INFO, "  +------------------------------------------+");
        loader_log(LOG_INFO, "");
    }

    /* ── LAUNCHER pass (no game on the command line, i.e. a double-click) ───────
     * Play the boot splash + init console to COMPLETION, then (unless the user
     * unchecked "show menu on launch" and has a valid saved game) show the settings
     * menu. When a game is chosen, RE-EXEC the loader with that game path as a
     * fresh process - that 2nd process is the RUNNER pass (it shows its OWN splash +
     * init console, then loads the cabinet). The settings UI and the running game
     * never share a process, exactly as requested. */
    if (!have_game) {
        if (g_settings.boot_to_menu || !saved_ok) {
            /* Splash fully finishes (blocking, self-cleaning) BEFORE the menu. */
            { extern volatile int g_splash_intro; extern void show_splash(void);
              g_splash_intro = 1; show_splash(); g_splash_intro = 0; }
            if (!settings_show_menu(exe_dir) || g_settings.game_path[0] == 0) return 0;
        }
        /* Close this launcher and re-open as the runner for the chosen game. */
        char exepath[MAX_PATH]; GetModuleFileNameA(NULL, exepath, sizeof exepath);
        char cmd[2048];
        snprintf(cmd, sizeof cmd, "\"%s\" \"%s\"", exepath, g_settings.game_path);
        STARTUPINFOA si; PROCESS_INFORMATION pi; memset(&si,0,sizeof si); si.cb=sizeof si; memset(&pi,0,sizeof pi);
        loader_log(LOG_INFO, "[LAUNCH] re-exec runner: %s", cmd);
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, exe_dir, &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            return 0;                       /* launcher exits; runner takes over */
        }
        loader_log(LOG_WARN, "[LAUNCH] re-exec failed (%lu), running in-process", GetLastError());
        menu_argv[0] = g_settings.game_path; argv = menu_argv; arg_start = 0; argc = 1;  /* fallback */
    }

    const char *elf_path = argv[arg_start];
    int         elf_argc = argc - arg_start;
    char      **elf_argv = argv + arg_start;

    /* Apply settings for this title: overlay the easy-config preset (matched by
     * path), then push everything into the runtime (output resolution, display
     * mode, skip-flag env the patcher reads). ULTRA_RES env still overrides res. */
    settings_apply_easy_config(elf_path);
    settings_apply();
    { const char *r = getenv("ULTRA_RES"); int rw, rh;
      if (r && sscanf(r, "%dx%d", &rw, &rh) == 2 && rw >= 320 && rh >= 240 && rw <= 7680 && rh <= 4320) {
          extern int g_force_w, g_force_h; g_force_w = rw; g_force_h = rh;
          /* ULTRA_RES is applied AFTER settings_apply, so recompute the 3D scene
           * scale from the final height (unless ULTRA_SCENE_SCALE is explicit). */
          if (!getenv("ULTRA_SCENE_SCALE")) { int s=(rh+479)/480; if(s<1)s=1; if(s>4)s=4;
              extern int g_ul_scene_scale; g_ul_scene_scale = s; } } }

    /* Optional extension module: if ultra_ext.dll sits next to the loader, load it
     * and hand it a small host API so external/companion modules can integrate.
     * Inert when absent. */
    {
        char extpath[MAX_PATH];
        snprintf(extpath, sizeof extpath, "%s\\ultra_ext.dll", exe_dir);
        if (GetFileAttributesA(extpath) != INVALID_FILE_ATTRIBUTES) {
            HMODULE em = LoadLibraryA(extpath);
            if (em) {
                typedef struct { int abi_version; const char *loader_version;
                                 const char *game_path; void *settings;
                                 void (*log)(const char*); } UltraHostApi;
                typedef int (*ext_init_fn)(const UltraHostApi*);
                ext_init_fn ei = (ext_init_fn)(void*)GetProcAddress(em, "ultra_ext_init");
                if (ei) {
                    extern void ultra_ext_log(const char*);
                    UltraHostApi host = { 1, "0.1.0", elf_path, &g_settings, ultra_ext_log };
                    ei(&host);
                }
            }
        }
    }

    /* Resolve the ELF to an ABSOLUTE path up front. Just below we
     * SetCurrentDirectory() into the game folder; after that, a relative ELF
     * path would re-resolve under the new cwd (e.g. ...\disk0\<rel>\abc) and
     * fail with ERROR_PATH_NOT_FOUND. GetFullPathName resolves against the
     * current (launch) directory, so capture the absolute path before any
     * chdir and use it everywhere after. */
    char elf_path_abs[MAX_PATH];
    {
        DWORD n = GetFullPathNameA(argv[arg_start], MAX_PATH, elf_path_abs, NULL);
        if (n > 0 && n < MAX_PATH) {
            elf_path    = elf_path_abs;
            elf_argv[0] = elf_path_abs;  /* guest argv[0] gets the real path too */
        }
    }

    /* ELF-patcher maintenance modes operate on the native ELF and are meaningless
     * for routed content (a PE .exe / disc image) - resolve them BEFORE routing so
     * `ultra-loader.exe -restore game.exe` doesn't launch the game instead. */
    if (do_restore) return patcher_restore(elf_path) ? 0 : 1;
    if (do_dry_run) { patcher_dry_run(elf_path); return 0; }

    /* P5: retro content (Dreamcast/NAOMI/arcade/GameCube/Xbox images) is handed
     * to the 64-bit module host; native Lindbergh/Type X ELFs fall through. The
     * easy-config preset + GL driver env are already applied above, and the
     * provisioner's ULTRA_*_CORE env (if a core is installed) is inherited. */
    {
        char modpath[MAX_PATH];
        if (ul_route_retro(elf_path, exe_dir, modpath, sizeof modpath)) {
            /* Splash + boot console already ran in the shell (work/src/main.c)
             * before this DLL was loaded. Just spawn the module host. The retro
             * branch here is only reached when the DLL is loaded directly (e.g.
             * legacy callers); the normal flow routes retro content in the shell. */
            int rc = ul_launch_module_host(exe_dir, modpath, elf_path);
            if (rc == 42) {   /* in-game "Load Different Game" -> reopen the menu */
                char exepath[MAX_PATH]; GetModuleFileNameA(NULL, exepath, sizeof exepath);
                char rcmd[MAX_PATH + 4]; snprintf(rcmd, sizeof rcmd, "\"%s\"", exepath);
                STARTUPINFOA si; PROCESS_INFORMATION pi;
                memset(&si, 0, sizeof si); si.cb = sizeof si; memset(&pi, 0, sizeof pi);
                if (CreateProcessA(NULL, rcmd, NULL, NULL, FALSE, 0, NULL, exe_dir, &si, &pi)) {
                    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                }
                return 0;
            }
            return rc;
        }
    }

    /* Lindbergh dispatch: if the game file is an ELF, hand off to the
     * standalone Lindbergh runtime module (ultra-lindbergh.exe) sitting in
     * modules_pkg/.../lindbergh/. This makes Lindbergh a true separate module
     * - the loader is just the dispatcher. Falls back to the in-process path
     * if the module binary isn't found (Phase-1 transition compatibility). */
    {
        FILE *fp = fopen(elf_path, "rb");
        unsigned char hdr[4] = {0};
        if (fp) { fread(hdr, 1, 4, fp); fclose(fp); }
        int is_elf = (hdr[0] == 0x7F && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F');
        if (is_elf) {
            char lind_exe[MAX_PATH];
            /* Try a few likely locations (next to loader exe first, then the
             * module's source-tree home for dev/CI runs). */
            const char *candidates[] = {
                "ultra-lindbergh.exe",
                "modules_pkg\\ultra-loader-modules\\modules\\lindbergh\\ultra-lindbergh.exe",
                "..\\modules_pkg\\ultra-loader-modules\\modules\\lindbergh\\ultra-lindbergh.exe",
            };
            int found = 0;
            for (int i = 0; i < (int)(sizeof candidates / sizeof candidates[0]); i++) {
                snprintf(lind_exe, sizeof lind_exe, "%s\\%s", exe_dir, candidates[i]);
                if (GetFileAttributesA(lind_exe) != INVALID_FILE_ATTRIBUTES) { found = 1; break; }
            }
            if (found) {
                loader_log(LOG_INFO, "[DISPATCH] ELF -> Lindbergh module: %s", lind_exe);
                char cmd[MAX_PATH * 3];
                snprintf(cmd, sizeof cmd, "\"%s\" \"%s\"", lind_exe, elf_path);
                STARTUPINFOA si; PROCESS_INFORMATION pi;
                memset(&si, 0, sizeof si); si.cb = sizeof si;
                memset(&pi, 0, sizeof pi);
                if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                    WaitForSingleObject(pi.hProcess, INFINITE);
                    DWORD rc = 0; GetExitCodeProcess(pi.hProcess, &rc);
                    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                    return (int)rc;
                }
                loader_log(LOG_WARN, "[DISPATCH] CreateProcess failed (%lu), falling back to in-process",
                           GetLastError());
            }
            /* fall through to in-process Lindbergh path */
        }
    }

    /* Disable patcher if -no-patch */
    if (skip_patch) g_patch_opts.dry_run = true;

    loader_log(LOG_INFO, "Ultra Loader v" LOADER_VERSION);
    loader_log(LOG_INFO, "Target: %s", elf_path);

    /* Set working directory to game folder so relative paths work */
    char game_dir[MAX_PATH];
    strncpy(game_dir, elf_path, MAX_PATH-1);
    char *last_sep = strrchr(game_dir, '\\');
    if (!last_sep) last_sep = strrchr(game_dir, '/');
    if (last_sep) {
        *last_sep = '\0';
        SetCurrentDirectoryA(game_dir);
        loader_log(LOG_INFO, "Working dir: %s", game_dir);
    }

    /* Verify file is accessible before doing anything else */
    {
        HANDLE ftest = CreateFileA(elf_path, GENERIC_READ, FILE_SHARE_READ,
                                   NULL, OPEN_EXISTING, 0, NULL);
        if (ftest == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            loader_log(LOG_ERROR, "Cannot open ELF: error %lu", err);
            if (err == ERROR_ACCESS_DENIED)
                loader_log(LOG_ERROR, "Access denied, try running as Administrator");
            if (err == ERROR_FILE_NOT_FOUND)
                loader_log(LOG_ERROR, "File not found: %s", elf_path);
            return 1;
        }
        LARGE_INTEGER fsz;
        GetFileSizeEx(ftest, &fsz);
        loader_log(LOG_INFO, "File accessible: %lld bytes", (long long)fsz.QuadPart);
        CloseHandle(ftest);
    }

    /* ---- Build loader context ---- */
    LoaderCtx *ctx = loader_ctx_create();
    if (!ctx) { fprintf(stderr, "Failed to create loader context\n"); return 1; }

    /* ---- ISO detection and mounting ---- */
    char actual_elf_path[MAX_PATH];
    strncpy(actual_elf_path, elf_path, MAX_PATH-1);
    IsoMount *iso = NULL;
    uint8_t  *iso_elf_buf  = NULL;
    size_t    iso_elf_size = 0;
    char      iso_elf_name[256] = {0};
    bool      from_iso = false;

    {
        size_t plen = strlen(elf_path);
        const char *ext = (plen > 4) ? elf_path + plen - 4 : "";
        bool is_disc_image = _stricmp(ext, ".iso")  == 0 ||
                             _stricmp(ext, ".img")  == 0 ||
                             _stricmp(ext, ".bin")  == 0 ||
                             _stricmp(ext, "dump")  == 0 ||  /* .dump */
                             (plen > 5 && _stricmp(elf_path + plen - 5, ".dump") == 0);

        if (is_disc_image) {
            /* First try as a Lindbergh raw dump via lxdecrypt pipeline */
            loader_log(LOG_INFO, "Disc image detected, trying Lindbergh raw dump...");
            char dump_dir[MAX_PATH];
            strncpy(dump_dir, elf_path, MAX_PATH-1);
            char *last_sep = strrchr(dump_dir, '\\');
            if (last_sep) *last_sep = '\0';
            else strncpy(dump_dir, ".", MAX_PATH-1);

            size_t lx_elf_sz = 0;
            uint8_t *lx_elf = lx_extract_elf(elf_path, dump_dir, &lx_elf_sz);
            if (lx_elf) {
                /* Write to temp and proceed */
                char temp[MAX_PATH];
                GetTempPathA(MAX_PATH, temp);
                snprintf(actual_elf_path, MAX_PATH, "%s\\ultraloader_game.elf", temp);
                FILE *tf = fopen(actual_elf_path, "wb");
                if (tf) {
                    fwrite(lx_elf, 1, lx_elf_sz, tf);
                    fclose(tf);
                    loader_log(LOG_INFO, "Lindbergh ELF ready: %zu bytes", lx_elf_sz);
                }
                free(lx_elf);
                goto skip_iso_mount;
            }

            /* Fall back to standard ISO 9660 mount */
            loader_log(LOG_INFO, "Not a raw Lindbergh dump, trying standard ISO 9660...");
            iso = iso_mount(elf_path);
            if (!iso) {
                loader_log(LOG_ERROR, "Failed to mount ISO");
                return 1;
            }

            const char *iso_elf_path = iso_find_elf(iso);
            if (!iso_elf_path) {
                loader_log(LOG_ERROR, "No game ELF found in ISO");
                iso_unmount(iso);
                return 1;
            }

            strncpy(iso_elf_name, iso_elf_path, 255);
            iso_elf_buf = iso_extract_elf(iso, iso_elf_path, &iso_elf_size);
            if (!iso_elf_buf) {
                iso_unmount(iso);
                return 1;
            }

            from_iso = true;
            loader_log(LOG_INFO, "ELF extracted from ISO: %s (%zu bytes)",
                       iso_elf_name, iso_elf_size);
        }
        skip_iso_mount:;
    }

    /* Install crash handler for ELF execution */
    /* Guarantee a stack reserve so the handler can still run (and dump) even if
     * a fault exhausts or corrupts the active stack - otherwise a bad fault in
     * a constructor tears the process down with no recoverable handler, which
     * is exactly the silent mid-write death we've been unable to diagnose. */
    {
        ULONG guarantee = 64 * 1024; /* 64KB reserved for exception handling */
        SetThreadStackGuarantee(&guarantee);
    }
    AddVectoredExceptionHandler(1, elf_crash_handler);

    /* ---- Init 32-bit thunk pool (Heaven's Gate trampolines) ---- */
    extern bool thunk32_init(void);
    thunk32_init();

    /* ---- Register all stub layers ---- */
    stubs_register_all(ctx);
    glx_register_stubs(ctx);
    /* Override the GLX rendering functions with the ported FBO-aware versions
     * (glx_emu.c). Done here, before ELF relocation, so the game's GOT binds to
     * the FBO-capable glXCreatePbuffer/MakeCurrent/SwapBuffers/etc. */
    { extern void glx_register_sgi_shims(void); glx_register_sgi_shims(); }
    libgl_register_stubs(ctx);
    /* GLU fallback stubs - fills gluPerspective/LookAt/etc. slots that
     * libgl_register_stubs couldn't resolve from glu32.dll. Idempotent:
     * skips slots already bound by the DLL path. Additive for ABC. */
    { extern void glu_stub_register(LoaderCtx*); glu_stub_register(ctx); }
    stubs_upgrade_dl(ctx);
    sym_register(ctx, "shm_open",   (uintptr_t)shmem_shm_open);
    sym_register(ctx, "shm_unlink", (uintptr_t)shmem_shm_unlink);
    glut_register_stubs(ctx);
    /* Additive: Cruis'n Blast (Raw Thrills G5) needs a handful of GLUT-3 APIs
     * the base pass doesn't cover (glutTimerFunc, glutWarpPointer,
     * glutFullScreen, glut{Hide,Show,Iconify}Window, glutForceJoystickPoll,
     * glut{Get,Set}Window). glut_shim.c registers only those - every other
     * title is untouched. */
    { extern void glut_shim_register_extras(LoaderCtx*);
      glut_shim_register_extras(ctx); }
    segaapi_register_stubs(ctx);
    extern void cxx_register_stubs(LoaderCtx*);
    cxx_register_stubs(ctx);
    /* C++ EH ABI (_Unwind_* + __gxx_personality_v0) - Hummer + LGJ */
    extern void unwind_stub_register(LoaderCtx*);
    unwind_stub_register(ctx);
    /* Cg runtime bypass - LGJ: intercept libCg.so/libCgGL.so symbols so
     * cgCreateProgram returns pre-compiled .asm_gl ARB rather than invoking
     * the Linux Cg compiler backend (which throws on Windows). */
    extern void cg_stub_register(LoaderCtx*);
    cg_stub_register(ctx);
    /* OpenSSL EVP/RSA/ERR stubs (Hummer dongle-signature verify path) */
    extern void ossl_stub_register(LoaderCtx*);
    ossl_stub_register(ctx);

    /* ---- Init subsystems ---- */
    jvs_init();

    /* ---- Install VEH syscall trap ---- */
    if (!syscall_trap_install(ctx))
        loader_log(LOG_WARN, "VEH install failed, relying on Option B only");

    /* ---- Load the ELF ---- */

    if (from_iso && iso_elf_buf) {
        /* Write to temp file */
        char temp[MAX_PATH];
        GetTempPathA(MAX_PATH, temp);
        snprintf(actual_elf_path, MAX_PATH, "%s\\ultraloader_game.elf", temp);

        FILE *tf = fopen(actual_elf_path, "wb");
        if (tf) {
            fwrite(iso_elf_buf, 1, iso_elf_size, tf);
            fclose(tf);
            loader_log(LOG_INFO, "ISO ELF staged to: %s", actual_elf_path);
        } else {
            loader_log(LOG_ERROR, "Cannot write temp ELF");
            return 1;
        }
        free(iso_elf_buf);
    }

    LoadedElf *exe = elf_load(ctx, actual_elf_path);
    if (!exe) {
        loader_log(LOG_ERROR, "Failed to load ELF: %s", elf_path);
        loader_ctx_destroy(ctx);
        return 1;
    }

    /* SPAWN THE BOOT CONSOLE NOW - the ELF is mapped, so the worker thread's
     * GDI/DIB allocations can't conflict with the guest segments. Earlier
     * than this risks an address-space race; this is the earliest safe spot.
     * The console fades out only when the game has drawn real colored
     * content (ul_lindbergh_content_seen via g_splash_dismiss). */
    {
        const char *no_splash = getenv("ULTRA_NO_SPLASH");
        if (!no_splash || !atoi(no_splash)) {
            static char sysb[80], titb[80], audb[48], iob[48], gpub[48];
            const char *s = getenv("ULTRA_BOOT_SYSTEM");
            const char *t = getenv("ULTRA_BOOT_TITLE");
            const char *a = getenv("ULTRA_BOOT_AUDIO");
            const char *i = getenv("ULTRA_BOOT_IO");
            const char *g = getenv("ULTRA_BOOT_GPU");
            if (s) strncpy(sysb, s, sizeof sysb - 1);
            if (t) strncpy(titb, t, sizeof titb - 1);
            if (a) strncpy(audb, a, sizeof audb - 1);
            if (i) strncpy(iob,  i, sizeof iob  - 1);
            if (g) strncpy(gpub, g, sizeof gpub - 1);
            if (!sysb[0]) strcpy(sysb, "Sega Lindbergh");
            if (!titb[0]) strcpy(titb, "(title)");
            if (!audb[0]) strcpy(audb, "SEGA / CRI");
            if (!iob[0])  strcpy(iob,  "JVS / I/O Board");
            if (!gpub[0]) strcpy(gpub, "OpenGL (native)");
            UltraBootIdentity bid = { sysb, titb, audb, iob, gpub,
                                       g_settings.width, g_settings.height };
            ultra_boot_console_run_async(&bid, exe_dir, NULL);
            loader_log(LOG_INFO, "[BOOT-CONSOLE] hold-over started (post-ELF, overlays game's loading until content)");
        }
    }

    /* ---- Patch FS-relative accesses and stack canaries ---- */
    threads_patch_fs_accesses(exe);

    /* ---- Load game-specific .so dependencies ---- */
    char dir[MAX_PATH];
    strncpy(dir, elf_path, MAX_PATH-1);
    char *slash = strrchr(dir, '\\');
    if (!slash) slash = strrchr(dir, '/');
    if (slash) *(slash+1) = '\0';
    else { dir[0]='.'; dir[1]='\\'; dir[2]='\0'; }

    for (int i = 0; i < exe->needed_count; i++) {
        const char *soname = exe->needed[i];
        if (strstr(soname,"libsegaapi") || strstr(soname,"libglut") ||
            strstr(soname,"libstdc++") || strstr(soname,"libGLU") ||
            strstr(soname,"libc.so") || strstr(soname,"libm.so") ||
            strstr(soname,"libpthread") || strstr(soname,"libdl.so") ||
            strstr(soname,"libGL") || strstr(soname,"libX") ||
            strstr(soname,"linux-vdso") || strstr(soname,"ld-linux")) {
            loader_log(LOG_INFO, "Skipping (stub-covered): %s", soname);
            continue;
        }
        char sopath[MAX_PATH];
        snprintf(sopath, MAX_PATH, "%s%s", dir, soname);
        loader_log(LOG_INFO, "Loading dependency: %s", sopath);
        if (!elf_load(ctx, sopath))
            loader_log(LOG_WARN, "  Could not load %s (may be stub-covered)", soname);
    }

    /* ---- Install crash handler to log ELF crashes ---- */
    signals_install_crash_handler();

    /* ---- Apply relocations ---- */
    /* Pre-init WGL context so wglGetProcAddress works, then resolve extensions */
    glx_preinit();
    libgl_resolve_extensions(ctx);
    if (!elf_relocate_all(ctx)) {
        loader_log(LOG_ERROR, "Relocation failed");
        loader_ctx_destroy(ctx);
        return 1;
    }

    if (list_syms) { dump_symbols(ctx); loader_ctx_destroy(ctx); return 0; }

    /* ---- Patch GOT to prevent null-pointer crashes from unresolved PLT ----
     * After eager relocation, GOT[2] (_dl_runtime_resolve) is 0. Any unresolved
     * PLT entry that calls through PLT[0] -> GOT[2] would crash at EIP=0 with
     * no symbol name in the crash log.
     *
     * TWO-LAYER TRAP:
     *  (1) Per-slot trap thunks - dynlink_install_null_got_traps() walks the
     *      slotmap (populated during relocation) and patches every null
     *      JMP_SLOT to a small trampoline that logs the EXACT symbol name
     *      before ExitProcess(1). This makes null-GOT crashes self-diagnosing
     *      across ALL loaded images (game exe, libCg, libCgGL, other .so).
     *  (2) GOT[2] fallback - for the game exe, also point _dl_runtime_resolve
     *      at the legacy anonymous trap so any slot missed by the slotmap
     *      (edge case) still doesn't EIP=0. Address range is ABC-specific
     *      but harmless on other titles (the write is a no-op if the range
     *      isn't the game's GOT). */
    {
        extern int dynlink_install_null_got_traps(void);
        extern uint32_t stub_unresolved_plt(void);
        int per_slot = dynlink_install_null_got_traps();

        /* Legacy GOT[2] anon-trap fallback (ABC layout). */
        uint32_t trap_addr = (uint32_t)(uintptr_t)stub_unresolved_plt;
        uint32_t *got = (uint32_t*)0x0833ccd4;
        DWORD old;
        if (VirtualProtect(got, 0x1000, PAGE_READWRITE, &old)) {
            got[2] = trap_addr;  /* _dl_runtime_resolve → anon trap */
            /* Also sweep any remaining nulls in the ABC PLT range (defense
             * in depth for slots that somehow escaped the slotmap). */
            uint32_t *plt_got = (uint32_t*)0x0833cce0;
            uint32_t *plt_end = (uint32_t*)0x0833d330;
            int patched = 0;
            for (uint32_t *p = plt_got; p < plt_end; p++) {
                if (*p == 0) { *p = trap_addr; patched++; }
            }
            VirtualProtect(got, 0x1000, old, &old);
            loader_log(LOG_INFO,
                       "GOT[2] patched; %d per-symbol thunks + %d anon fallback slots trapped",
                       per_slot, patched);
        }
    }

    /* ---- X11 fake server + Lindbergh pre-init ---- */
    {
        extern bool x11_server_start(void);
        extern void lindbergh_preinit(void);

        if (x11_server_start()) {
            /* Tell libX11 to connect to our local fake server via TCP */
            SetEnvironmentVariableA("DISPLAY", "127.0.0.1:0");
            loader_log(LOG_INFO, "X11 fake server started, DISPLAY=127.0.0.1:0");
        } else {
            loader_log(LOG_WARN, "X11 fake server failed to start, libX11 may stall");
        }

        lindbergh_preinit();
    }

    /* ---- Ultra Loader self-learning profile: init, replay, arm save ---- */
    {
        extern void autoprofile_init(const char *sha_hex);
        extern int  autoprofile_load_and_apply(void);
        extern void autoprofile_save(void);
        /* g_elf_sha_hex is set by the patcher when it hashes the image. */
        extern const char *g_elf_sha_hex;
        autoprofile_init(g_elf_sha_hex);
        autoprofile_load_and_apply();   /* 0 entries = first/learning run */
        atexit(autoprofile_save);       /* persist learned profile on exit */
    }

    /* ---- Build Linux ABI stack and launch ---- */
    loader_log(LOG_INFO, "=== Launching: %s ===", elf_path);
    size_t stack_sz;
    void *abi_stack;
    if (exe->is_elf32) {
        /* Use the 32-bit ABI stack builder: all pointers stored as uint32_t */
        abi_stack = auxv_build_stack32(exe, elf_argc, elf_argv, &stack_sz);
    } else {
        abi_stack = auxv_build_stack(exe, elf_argc, elf_argv, &stack_sz);
    }
    if (!abi_stack) {
        loader_log(LOG_ERROR, "Failed to build ABI stack");
        loader_ctx_destroy(ctx);
        return 1;
    }

    /* Run .init_array constructors */
    for (int i = 0; i < ctx->image_count; i++) { /* handled inside elf_execute */ }

    if (exe->is_elf32) {
        /* ---- ELF32 native launch under WOW64 ----
         *
         * We are a 32-bit PE process under WOW64. The ELF32 code runs
         * directly in our 32-bit address space - no emulator needed.
         * All stub function pointers are 32-bit. [ebp-N] addressing
         * works correctly. Native speed.
         */
        loader_log(LOG_INFO, "ELF32 native launch (WOW64 32-bit mode)");
        loader_log(LOG_INFO, "  entry=0x%08x stack=0x%08x",
                   (uint32_t)exe->entry, (uint32_t)(uintptr_t)abi_stack);
        {
            uint32_t got_val = *(uint32_t*)0x0833d0ec;
            uint8_t *ep = (uint8_t*)exe->entry;
            loader_log(LOG_INFO, "  GOT[__libc_start_main]@0x0833d0ec = 0x%08x", got_val);
            loader_log(LOG_INFO, "  _start[0..7]: %02x %02x %02x %02x %02x %02x %02x %02x",
                       ep[0],ep[1],ep[2],ep[3],ep[4],ep[5],ep[6],ep[7]);
        }
        fflush(stdout); fflush(stderr);
        g_in_elf = true;

        /* ---- DATA/CODE LOCATION PROBE ----
         * Resolve where code and data actually landed vs the loader's bias.
         * 'app' is a .data pointer with baked-in value 0x083480e0 at file
         * vaddr 0x082eeb30. main reads it on its first instruction. We probe
         * both the unbiased (0x082eeb30) and biased (0x082e6b30) addresses,
         * and the same for main's code bytes (55 fc 31 c0 ... at 0x08061b58),
         * each guarded so the probe can't fault. */
        {
            uint32_t bias = (uint32_t)exe->load_bias;
            uint32_t app_unb = 0x082eeb30;
            uint32_t app_bia = 0x082eeb30 + bias;
            uint32_t main_unb = 0x08061b58;
            uint32_t main_bia = 0x08061b58 + bias;
            loader_log(LOG_INFO, "  [PROBE] bias=0x%08x  (app expects 0x083480e0)", bias);
            if (!IsBadReadPtr((void*)(uintptr_t)app_unb, 4))
                loader_log(LOG_INFO, "  [PROBE] app @unbiased 0x%08x = 0x%08x", app_unb, *(uint32_t*)(uintptr_t)app_unb);
            else
                loader_log(LOG_INFO, "  [PROBE] app @unbiased 0x%08x = <unmapped>", app_unb);
            if (!IsBadReadPtr((void*)(uintptr_t)app_bia, 4))
                loader_log(LOG_INFO, "  [PROBE] app @biased   0x%08x = 0x%08x", app_bia, *(uint32_t*)(uintptr_t)app_bia);
            else
                loader_log(LOG_INFO, "  [PROBE] app @biased   0x%08x = <unmapped>", app_bia);
            if (!IsBadReadPtr((void*)(uintptr_t)main_unb, 4))
                loader_log(LOG_INFO, "  [PROBE] main @unbiased 0x%08x bytes = %02x %02x %02x %02x", main_unb,
                           *(uint8_t*)(uintptr_t)main_unb, *(uint8_t*)(uintptr_t)(main_unb+1),
                           *(uint8_t*)(uintptr_t)(main_unb+2), *(uint8_t*)(uintptr_t)(main_unb+3));
            else
                loader_log(LOG_INFO, "  [PROBE] main @unbiased 0x%08x = <unmapped>", main_unb);
            if (!IsBadReadPtr((void*)(uintptr_t)main_bia, 4))
                loader_log(LOG_INFO, "  [PROBE] main @biased   0x%08x bytes = %02x %02x %02x %02x", main_bia,
                           *(uint8_t*)(uintptr_t)main_bia, *(uint8_t*)(uintptr_t)(main_bia+1),
                           *(uint8_t*)(uintptr_t)(main_bia+2), *(uint8_t*)(uintptr_t)(main_bia+3));
            else
                loader_log(LOG_INFO, "  [PROBE] main @biased   0x%08x = <unmapped>", main_bia);
            fflush(stdout);
        }

        /* Publish load bias so the __libc_start_main stub can locate .ctors
         * (and any other vaddr-based structures) at their real mapped address. */
        extern volatile uint32_t g_elf_load_bias;
        g_elf_load_bias = (uint32_t)exe->load_bias;
        loader_log(LOG_INFO, "  publishing load_bias=0x%08x for ctor walk", (uint32_t)exe->load_bias);

        /* Boot console already spawned at ul_lindbergh_run entry. */

        /* Arm the hang watchdog on this (the game) thread: if the guest spins
         * silently, it suspends us and dumps EIP + the ELF return chain. */
        extern void watchdog_start(void);
        watchdog_start();
        extern void eiptrace_arm(void);
        eiptrace_arm();

        /* Periodic memory snapshotter - captures unpacked payload for
         * self-decrypting titles (Cruis'n Blast). Opt-in via ULTRA_MEMSNAP=1,
         * auto-armed when ULTRA_GAME contains "cruis"/"blast". */
        extern void memsnap_start(void);
        memsnap_start();

        extern void elf32_launch(uint32_t entry, uint32_t *abi_stack);
        elf32_launch((uint32_t)exe->entry, (uint32_t*)abi_stack);
        /* Never returns */
    } else {
        auxv_jump_to_entry(exe->entry, abi_stack);
    }

    /* Unreachable */
    if (iso) iso_unmount(iso);
    loader_ctx_destroy(ctx);
    return 0;
}
