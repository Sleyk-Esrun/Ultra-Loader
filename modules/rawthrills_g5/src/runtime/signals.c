#include "compat32.h"
/*
 * signals.c
 * Linux Signal Semantic Layer
 *
 * Linux signals are fundamentally different from Win32 structured exceptions:
 *   - Signals have per-thread masks (sigprocmask)
 *   - Signals can be queued (real-time signals)
 *   - SA_RESTART causes certain syscalls to restart after signal delivery
 *   - SA_SIGINFO passes siginfo_t with fault address etc.
 *   - sigaltstack() allows a separate signal stack
 *   - SIGALRM is driven by setitimer() / alarm()
 *   - SIGSEGV can be caught and recovered from
 *
 * Our strategy:
 *   1. Maintain per-signal handler table (registered via rt_sigaction)
 *   2. Map SIGSEGV -> Win32 EXCEPTION_ACCESS_VIOLATION (via VEH)
 *   3. SIGALRM -> Win32 CreateWaitableTimer
 *   4. SIGUSR1/SIGUSR2 -> named Win32 events (inter-thread)
 *   5. SIGTERM/SIGINT -> SetConsoleCtrlHandler
 *   6. Other signals: best-effort delivery via a dedicated signal thread
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "loader.h"
#include "signals.h"

/* ------------------------------------------------------------------ */
/* Linux signal numbers                                                */
/* ------------------------------------------------------------------ */
#define LINUX_SIGHUP     1
#define LINUX_SIGINT     2
#define LINUX_SIGQUIT    3
#define LINUX_SIGILL     4
#define LINUX_SIGTRAP    5
#define LINUX_SIGABRT    6
#define LINUX_SIGBUS     7
#define LINUX_SIGFPE     8
#define LINUX_SIGKILL    9
#define LINUX_SIGUSR1   10
#define LINUX_SIGSEGV   11
#define LINUX_SIGUSR2   12
#define LINUX_SIGPIPE   13
#define LINUX_SIGALRM   14
#define LINUX_SIGTERM   15
#define LINUX_SIGCHLD   17
#define LINUX_SIGCONT   18
#define LINUX_SIGSTOP   19
#define LINUX_SIGWINCH  28
#define LINUX_NSIG      65

/* SA flags */
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

/* Special handler values */
#define SIG_DFL ((void*)0)
#define SIG_IGN ((void*)1)

/* ucontext_t stub (used by SA_SIGINFO handlers) */
typedef struct {
    uint64_t uc_flags;
    void    *uc_link;
    struct { void *ss_sp; int ss_flags; size_t ss_size; } uc_stack;
    /* gregs omitted - fill on demand */
    uint64_t _pad[128];
} Linux_Ucontext;

/* ------------------------------------------------------------------ */
/* Signal handler table                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    void    *handler;     /* SIG_DFL, SIG_IGN, or function pointer */
    uint64_t mask[16];
    int      flags;
    bool     registered;
} SigEntry;

static SigEntry g_sighandlers[LINUX_NSIG];
static HANDLE   g_alrm_timer = NULL;
static HANDLE   g_alrm_thread = NULL;

/* ------------------------------------------------------------------ */
/* Deliver a signal to the current thread                              */
/* ------------------------------------------------------------------ */
static void deliver_signal(int signo, Linux_Siginfo *info, Linux_Ucontext *uc)
{
    if (signo <= 0 || signo >= LINUX_NSIG) return;
    SigEntry *e = &g_sighandlers[signo];

    if (!e->registered || e->handler == SIG_DFL) {
        /* Default action */
        switch (signo) {
        case LINUX_SIGKILL:
        case LINUX_SIGTERM:
            loader_log(LOG_INFO, "Default action for signal %d: terminate", signo);
            ExitProcess(signo + 128);
            break;
        case LINUX_SIGPIPE:
        case LINUX_SIGHUP:
            loader_log(LOG_DEBUG, "Ignoring signal %d (default)", signo);
            break;
        default:
            loader_log(LOG_DEBUG, "Unhandled signal %d (default action: ignore)", signo);
            break;
        }
        return;
    }

    if (e->handler == SIG_IGN) return;

    /* Call the registered handler */
    if (e->flags & SA_SIGINFO) {
        Linux_Siginfo  local_info = {0};
        Linux_Ucontext local_uc   = {0};
        if (!info) info = &local_info;
        if (!uc)   uc   = &local_uc;
        local_info.si_signo = signo;
        void (*sa)(int, Linux_Siginfo*, void*) =
            (void(*)(int, Linux_Siginfo*, void*))e->handler;
        sa(signo, info, uc);
    } else {
        void (*h)(int) = (void(*)(int))e->handler;
        h(signo);
    }

    /* SA_RESETHAND: restore default after delivery */
    if (e->flags & SA_RESETHAND) {
        e->handler = SIG_DFL;
        e->flags   = 0;
    }
}

/* ------------------------------------------------------------------ */
/* rt_sigaction implementation                                         */
/* ------------------------------------------------------------------ */
int signal_rt_sigaction(int signo, const Linux_Sigaction *act,
                         Linux_Sigaction *oldact, size_t sigsetsize)
{
    if (signo <= 0 || signo >= LINUX_NSIG) return -22; /* EINVAL */
    if (signo == LINUX_SIGKILL || signo == LINUX_SIGSTOP) return -22;

    SigEntry *e = &g_sighandlers[signo];

    if (oldact) {
        oldact->sa_handler  = e->registered ? e->handler : SIG_DFL;
        oldact->sa_flags    = e->flags;
        memcpy(oldact->sa_mask, e->mask, sizeof(e->mask));
    }

    if (act) {
        e->handler    = act->sa_handler;
        e->flags      = act->sa_flags;
        e->registered = true;
        memcpy(e->mask, act->sa_mask, sizeof(e->mask));

        loader_log(LOG_DEBUG, "rt_sigaction: sig=%d handler=0x%p flags=0x%x",
                   signo, e->handler, e->flags);

        /* Special: SIGSEGV with SA_SIGINFO -> we hook via VEH */
        /* (Already done in syscall VEH; signal VEH is separate) */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* rt_sigprocmask                                                      */
/* ------------------------------------------------------------------ */
int signal_rt_sigprocmask(int how, const uint64_t *set,
                           uint64_t *oldset, size_t sigsetsize)
{
    /* We maintain a per-thread signal mask conceptually but don't
     * enforce it (no kernel to preempt us). Just acknowledge. */
    static __thread uint64_t g_sigmask[16] = {0};
    if (oldset) memcpy(oldset, g_sigmask, sigsetsize);
    if (set) {
        switch (how) {
        case 0: /* SIG_BLOCK */
            for (size_t i = 0; i < sigsetsize/8; i++) g_sigmask[i] |= set[i];
            break;
        case 1: /* SIG_UNBLOCK */
            for (size_t i = 0; i < sigsetsize/8; i++) g_sigmask[i] &= ~set[i];
            break;
        case 2: /* SIG_SETMASK */
            memcpy(g_sigmask, set, sigsetsize);
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* SIGALRM via Win32 waitable timer                                   */
/* ------------------------------------------------------------------ */

static DWORD WINAPI alrm_timer_thread(LPVOID param)
{
    (void)param;
    while (g_alrm_timer) {
        DWORD r = WaitForSingleObject(g_alrm_timer, INFINITE);
        if (r == WAIT_OBJECT_0) {
            loader_log(LOG_DEBUG, "SIGALRM fired");
            deliver_signal(LINUX_SIGALRM, NULL, NULL);
        }
    }
    return 0;
}

int signal_setitimer(int which, const Linux_Itimerval *new_val,
                      Linux_Itimerval *old_val)
{
    if (which != 0) return -22; /* only ITIMER_REAL */

    if (old_val) memset(old_val, 0, sizeof(*old_val));

    if (!g_alrm_timer) {
        g_alrm_timer  = CreateWaitableTimerA(NULL, FALSE, NULL);
        g_alrm_thread = CreateThread(NULL, 0, alrm_timer_thread, NULL, 0, NULL);
    }

    if (!new_val || (new_val->it_value.tv_sec == 0 &&
                     new_val->it_value.tv_usec == 0)) {
        CancelWaitableTimer(g_alrm_timer);
        return 0;
    }

    /* Convert to 100ns units (negative = relative) */
    int64_t us   = new_val->it_value.tv_sec * 1000000LL +
                   new_val->it_value.tv_usec;
    int64_t due  = -(us * 10); /* 100ns units, negative = relative */
    int64_t period_ms = (new_val->it_interval.tv_sec * 1000LL +
                         new_val->it_interval.tv_usec / 1000LL);

    LARGE_INTEGER li; li.QuadPart = due;
    SetWaitableTimer(g_alrm_timer, &li, (LONG)period_ms, NULL, NULL, FALSE);
    return 0;
}

/* alarm() convenience */
unsigned signal_alarm(unsigned seconds)
{
    Linux_Itimerval val = {0};
    val.it_value.tv_sec = seconds;
    signal_setitimer(0, &val, NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* SIGSEGV -> VEH bridge                                               */
/* ------------------------------------------------------------------ */
static PVOID g_sig_veh = NULL;

static LONG WINAPI signal_veh(EXCEPTION_POINTERS *ep)
{
    int signo = 0;
    Linux_Siginfo info = {0};

    /* MAINTRACE uses INT3 breakpoints + single-step traps to log main()'s init
     * progress. Those are NOT crashes - handle them here (or pass through) so
     * they reach the maintrace logic instead of being reported as a crash. */
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
        extern int maintrace_on_breakpoint(CONTEXT *ctx);
        if (maintrace_on_breakpoint(ep->ContextRecord)) return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        /* eiptrace single-steps from a chosen entry to capture the EIPs right
         * before a transfer-to-zero. It must run here (this VEH is first and
         * would otherwise swallow the trap without re-arming TF). */
        extern int eiptrace_on_singlestep(CONTEXT *ctx);
        if (eiptrace_on_singlestep(ep->ContextRecord)) return EXCEPTION_CONTINUE_EXECUTION;
        extern int maintrace_on_singlestep(CONTEXT *ctx);
        if (maintrace_on_singlestep(ep->ContextRecord)) return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_EXECUTION; /* stray single-step: ignore */
    }

    switch (ep->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        signo = LINUX_SIGSEGV;
        info.si_code = 1; /* SEGV_MAPERR */
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        signo = LINUX_SIGILL;
        break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        signo = LINUX_SIGFPE;
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }

    SigEntry *e = &g_sighandlers[signo];
    if (!e->registered || e->handler == SIG_DFL || e->handler == SIG_IGN)
        return EXCEPTION_CONTINUE_SEARCH; /* let Windows handle it */

    info.si_signo = signo;
    deliver_signal(signo, &info, NULL);

    /* If the handler returned, try to continue */
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* ------------------------------------------------------------------ */
/* Console ctrl handler (SIGINT / SIGTERM)                             */
/* ------------------------------------------------------------------ */
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    int signo = (type == CTRL_C_EVENT) ? LINUX_SIGINT : LINUX_SIGTERM;
    deliver_signal(signo, NULL, NULL);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* kill() / tgkill() - send signal to self or thread                  */
/* ------------------------------------------------------------------ */
int signal_kill(int pid, int signo)
{
    if (pid == (int)GetCurrentProcessId() || pid == 0 || pid == -1) {
        deliver_signal(signo, NULL, NULL);
        return 0;
    }
    loader_log(LOG_WARN, "kill(%d, %d): cross-process signals not supported", pid, signo);
    return -1;
}

int signal_tgkill(int tgid, int tid, int signo)
{
    return signal_kill(tgid, signo);
}

/* ------------------------------------------------------------------ */
/* sigaltstack (stub - we don't switch stacks)                         */
/* ------------------------------------------------------------------ */
int signal_sigaltstack(const Linux_Stack *ss, Linux_Stack *oss)
{
    /* Acknowledge but don't actually switch; most games don't rely on it */
    if (oss) { oss->ss_sp = NULL; oss->ss_flags = 2; oss->ss_size = 0; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public init                                                          */
/* ------------------------------------------------------------------ */
void signals_init(void)
{
    memset(g_sighandlers, 0, sizeof(g_sighandlers));

    /* Set up SIGPIPE as IGN by default (common Linux default for servers/games) */
    g_sighandlers[LINUX_SIGPIPE].handler    = SIG_IGN;
    g_sighandlers[LINUX_SIGPIPE].registered = true;

    g_sig_veh = AddVectoredExceptionHandler(1, signal_veh);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    loader_log(LOG_INFO, "Signal semantic layer initialized");
}

void signals_shutdown(void)
{
    if (g_sig_veh)  { RemoveVectoredExceptionHandler(g_sig_veh); g_sig_veh = NULL; }
    if (g_alrm_timer) { CloseHandle(g_alrm_timer); g_alrm_timer = NULL; }
}

/* ------------------------------------------------------------------ */
/* Crash reporter - catches access violations during ELF execution     */
/* ------------------------------------------------------------------ */
static LONG WINAPI elf_crash_handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT          *ctx = ep->ContextRecord;

    /* MAINTRACE breakpoints/single-steps are not crashes - let them through. */
    if (er->ExceptionCode == EXCEPTION_BREAKPOINT) {
        extern int maintrace_on_breakpoint(CONTEXT *c);
        if (maintrace_on_breakpoint(ctx)) return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        extern int maintrace_on_singlestep(CONTEXT *c);
        if (maintrace_on_singlestep(ctx)) return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    /* CPU hardware faults are the ONLY thing this handler may treat as a crash.
     * Software-raised exceptions - above all the MSVC C++ exception
     * (0xE06D7363, "msc"), which Mesa/the GL stack throws AND catches
     * internally during WGL context creation - are normal control flow and
     * must be left to their own __try/__except. Swallowing 0xE06D7363 here was
     * aborting the process during WGL pre-init, before the ELF even ran.
     * (The old rip-range check below treated DLL addresses like 0x7594xxxx as
     * "in_elf", so a thrown-and-handled C++ exception looked like an ELF crash.) */
    switch (er->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            break;                                /* genuine CPU fault - handle it */
        default:
            return EXCEPTION_CONTINUE_SEARCH;     /* C++ throw, debug events, … */
    }

    /* Only handle crashes in the ELF region or pool region */
    uintptr_t rip = (uintptr_t)ctx->Rip;
    uintptr_t fault_addr = (uintptr_t)er->ExceptionInformation[1];

    /* Log every exception that touches our regions for diagnostics.
     * ELF image really lives in 0x08048000..0x0A0CB000; keep the upper bound
     * just above that so Mesa (0x64../0x67..), the ABI stack (0x70..) and
     * Windows DLLs (0x75..) are NOT misclassified as ELF code. */
    bool in_elf  = (rip >= 0x08000000 && rip <  0x0A200000);
    bool in_pool = (rip >= 0x01000000 && rip <= 0x02000000);
    bool in_stack = (rip >= 0x6f000000 && rip <= 0x71000000);
    if (in_elf || in_pool || in_stack || er->ExceptionCode == STATUS_STACK_OVERFLOW) {
        char buf[128];
        int n = wsprintfA(buf, "[VEH] code=0x%08X rip=0x%08X rsp=0x%08X\r\n",
                          (unsigned)er->ExceptionCode, (unsigned)rip, (unsigned)ctx->Rsp);
        DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, n, &w, NULL);
    }
    if (!in_elf && !in_pool && !in_stack) {
        /* But still log it if it looks ELF-related */
        if (fault_addr >= 0x08000000 && fault_addr < 0xB0000000) {
            loader_log(LOG_ERROR, "CRASH: loader code at RIP=0x%zx accessing ELF addr 0x%zx",
                       rip, fault_addr);
            fflush(stdout);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    /* Handle pool/thunk crashes too */
    if (in_pool && !in_elf) {
        char buf[256];
        int n = wsprintfA(buf, "[LBRGH/ERR] POOL CRASH: code=0x%08X RIP=0x%08X RSP=0x%08X\r\n",
                          (unsigned)er->ExceptionCode, (unsigned)rip, (unsigned)ctx->Rsp);
        DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, n, &w, NULL);
        loader_log(LOG_ERROR, "=== POOL/THUNK CRASH ===");
        loader_log(LOG_ERROR, "Exception: 0x%08X at RIP=0x%016zx (pool offset 0x%zx)",
                   er->ExceptionCode, rip, rip - 0x01000000ULL);
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
            loader_log(LOG_ERROR, "  Fault addr: 0x%016zx (%s)",
                       fault_addr, er->ExceptionInformation[0]==0?"read":"write");
        loader_log(LOG_ERROR, "  RSP=0x%016llx RCX=0x%016llx RDX=0x%016llx",
                   (unsigned long long)ctx->Rsp, (unsigned long long)ctx->Rcx,
                   (unsigned long long)ctx->Rdx);
        fflush(stdout);
        ExitProcess(1);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    const char *excname = "UNKNOWN";
    switch (er->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        excname = er->ExceptionInformation[0] == 0 ? "READ_AV" :
                  er->ExceptionInformation[0] == 1 ? "WRITE_AV" : "EXEC_AV";
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: excname = "ILLEGAL_INSTRUCTION"; break;
    case EXCEPTION_STACK_OVERFLOW:      excname = "STACK_OVERFLOW";       break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:  excname = "DIV_BY_ZERO";         break;
    case EXCEPTION_PRIV_INSTRUCTION:    excname = "PRIV_INSTRUCTION";     break;
    case EXCEPTION_BREAKPOINT:          excname = "BREAKPOINT";           break;
    }

    loader_log(LOG_ERROR, "=== ELF CRASH ===");
    loader_log(LOG_ERROR, "Exception: %s (code=0x%08X)", excname, er->ExceptionCode);
    {
        char buf[256];
        int n = wsprintfA(buf, "[LBRGH/ERR] === ELF CRASH: %s ===\r\n  Fault address:  0x%08X\r\n  Access type:    %s\r\n  Bad address:    0x%08X\r\n  EIP/RIP:  0x%08X\r\n  ESP/RSP:  0x%08X\r\n",
            excname, (unsigned)er->ExceptionCode,
            er->ExceptionInformation[0]==0?"READ":"WRITE",
            (unsigned)fault_addr, (unsigned)rip, (unsigned)ctx->Rsp);
        DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, n, &w, NULL);
    }
    loader_log(LOG_ERROR, "  RIP=0x%016zx  (ELF offset 0x%zx)",
               rip, rip - 0x08040000ULL);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        loader_log(LOG_ERROR, "  Fault addr: 0x%016zx (%s)",
                   fault_addr, er->ExceptionInformation[0]==0?"read":"write");
    loader_log(LOG_ERROR, "  EAX=0x%08x EBX=0x%08x ECX=0x%08x EDX=0x%08x",
               (uint32_t)ctx->Rax, (uint32_t)ctx->Rbx,
               (uint32_t)ctx->Rcx, (uint32_t)ctx->Rdx);
    loader_log(LOG_ERROR, "  ESI=0x%08x EDI=0x%08x ESP=0x%08x EBP=0x%08x",
               (uint32_t)ctx->Rsi, (uint32_t)ctx->Rdi,
               (uint32_t)ctx->Rsp, (uint32_t)ctx->Rbp);

    /* Print nearby stack */
    uint32_t *sp = (uint32_t*)(uintptr_t)(uint32_t)ctx->Rsp;
    loader_log(LOG_ERROR, "  Stack dump (esp=0x%08x):", (uint32_t)ctx->Rsp);
    for (int i = 0; i < 8; i++) {
            loader_log(LOG_ERROR, "    [esp+%02d] 0x%08x", i*4, sp[i]);
        
    }

    /* Try to show what code is at RIP */
    uint8_t *code = (uint8_t*)rip;
    loader_log(LOG_ERROR, "  Code at RIP: %02x %02x %02x %02x %02x %02x %02x %02x",
               code[0], code[1], code[2], code[3],
               code[4], code[5], code[6], code[7]);

    loader_log(LOG_ERROR, "=================");
    ExitProcess(1);
    return EXCEPTION_EXECUTE_HANDLER;
}

void signals_install_crash_handler(void)
{
    AddVectoredExceptionHandler(0, elf_crash_handler);

    /* Also install as unhandled exception filter - catches cases that bypass VEH */
    SetUnhandledExceptionFilter((LPTOP_LEVEL_EXCEPTION_FILTER)elf_crash_handler);

    loader_log(LOG_DEBUG, "ELF crash handler installed");
}
