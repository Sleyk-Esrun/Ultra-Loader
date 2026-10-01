/* ------------------------------------------------------------------ *
 * eiptrace.c - single-step "recent EIP" tracer.
 *
 * When a crash leaves EIP=0 (a jmp/ret/call to a null/clobbered target) the
 * faulting instruction's address is lost. This tracer plants an INT3 at a
 * chosen function entry (env ULTRA_TRACE_AT=0xADDR), and from that point sets
 * the trap flag and records every ELF-range EIP into a ring. On the next crash
 * the ring is dumped - so you see the exact instructions executed right before
 * the transfer-to-zero (and can spot the overflow/clobber that caused it).
 *
 * Slow (one VEH exception per instruction) but it runs once and then crashes.
 * ------------------------------------------------------------------ */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include "loader.h"

#define RING 220
#define ELF_LO 0x0804f074u
#define ELF_HI 0x081f352cu

static volatile uint32_t g_ring[RING];
static volatile uint32_t g_ring_sp[RING];
static volatile int      g_pos = 0;
static volatile int      g_on  = 0;
static uint32_t          g_trace_at = 0;
static uint8_t           g_orig_byte = 0;
static uint32_t          g_watch = 0;       /* address to watch for writes */
static uint32_t          g_watch_last = 0;
static uint32_t          g_prev_eip = 0;
static int               g_watch_retslot = 0;

/* ---- milestone one-shot breakpoints (env ULTRA_BP_LIST=0xA,0xB,...) ---------
 * Plant a one-shot INT3 at each listed address. When hit, log "[BP] reached
 * 0xADDR" (unbuffered), restore the original byte, and continue. No
 * single-stepping -> no flood. Used to find the LAST init function reached
 * before a silent process death. Env-gated; inert for ABC and normal runs. */
#define BP_MAX 32
static struct { uint32_t addr; uint8_t orig; int armed; } g_bp[BP_MAX];
static int g_bp_count = 0;
static int g_bp_persist = 0;        /* ULTRA_BP_PERSIST=1: re-arm after each hit */
static int g_bp_replant_idx = -1;   /* index whose INT3 must be replanted after a single-step */

void eiptrace_arm_bplist(void)
{
    const char *e = getenv("ULTRA_BP_LIST");
    if (!e || !*e) return;
    if (getenv("ULTRA_BP_PERSIST")) g_bp_persist = 1;
    char buf[512]; strncpy(buf, e, sizeof(buf)-1); buf[sizeof(buf)-1]=0;
    char *p = buf;
    while (*p && g_bp_count < BP_MAX) {
        while (*p==',' || *p==' ') p++;
        if (!*p) break;
        uint32_t a = (uint32_t)strtoul(p, &p, 0);
        if (!a) continue;
        DWORD old;
        if (VirtualProtect((void*)(uintptr_t)a, 1, PAGE_EXECUTE_READWRITE, &old)) {
            g_bp[g_bp_count].addr = a;
            g_bp[g_bp_count].orig = *(volatile uint8_t*)(uintptr_t)a;
            *(volatile uint8_t*)(uintptr_t)a = 0xCC;
            VirtualProtect((void*)(uintptr_t)a, 1, old, &old);
            g_bp[g_bp_count].armed = 1;
            loader_log(LOG_INFO, "[BP] armed one-shot INT3 at 0x%08x", a);
            g_bp_count++;
        }
    }
}

/* Returns 1 if it handled a milestone breakpoint. */
int eiptrace_bp_hit(CONTEXT *ctx)
{
    uint32_t eip = (uint32_t)ctx->Eip;
    for (int i = 0; i < g_bp_count; i++) {
        if (g_bp[i].armed && (eip == g_bp[i].addr || eip == g_bp[i].addr + 1)) {
            DWORD old;
            VirtualProtect((void*)(uintptr_t)g_bp[i].addr, 1, PAGE_EXECUTE_READWRITE, &old);
            *(volatile uint8_t*)(uintptr_t)g_bp[i].addr = g_bp[i].orig;
            VirtualProtect((void*)(uintptr_t)g_bp[i].addr, 1, old, &old);
            g_bp[i].armed = 0;
            ctx->Eip = g_bp[i].addr;     /* resume at the real instruction */
            /* dump child-list fields off EBX (the event node) so we can watch the
             * deletion loop walk: +0x40 head, +0x44 tail, +0x3c parent. */
            uint32_t ebx = (uint32_t)ctx->Ebx;
            uint32_t h40=0,t44=0,p3c=0,nx48=0;
            if (ebx >= 0x1000 && !IsBadReadPtr((void*)(uintptr_t)ebx, 0x50)) {
                h40 = *(uint32_t*)(uintptr_t)(ebx+0x40);
                t44 = *(uint32_t*)(uintptr_t)(ebx+0x44);
                p3c = *(uint32_t*)(uintptr_t)(ebx+0x3c);
                nx48 = *(uint32_t*)(uintptr_t)(ebx+0x48);
            }
            char b[320];
            int n = wsprintfA(b, "[BP] reached 0x%08x  EAX=%08x EBX=%08x ESI=%08x EDI=%08x | [ebx+40h]=%08x [+44h]=%08x [+3ch]=%08x [+48h]=%08x\r\n",
                              g_bp[i].addr,
                              (unsigned)ctx->Eax, (unsigned)ctx->Ebx,
                              (unsigned)ctx->Esi, (unsigned)ctx->Edi,
                              h40, t44, p3c, nx48);
            DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), b, n, &w, NULL);
            FlushFileBuffers(GetStdHandle(STD_ERROR_HANDLE));
            if (g_bp_persist) {
                /* single-step over the real instruction, then replant the INT3 */
                g_bp_replant_idx = i;
                ctx->EFlags |= 0x100;      /* TF */
            }
            return 1;
        }
    }
    return 0;
}

void eiptrace_arm(void)
{
    eiptrace_arm_bplist();
    const char *e = getenv("ULTRA_TRACE_AT");
    if (!e || !*e) return;
    g_trace_at = (uint32_t)strtoul(e, NULL, 0);
    if (!g_trace_at) return;
    const char *we = getenv("ULTRA_WATCH_ADDR");
    if (we && *we) g_watch = (uint32_t)strtoul(we, NULL, 0);
    if (getenv("ULTRA_WATCH_RETSLOT")) g_watch_retslot = 1;
    DWORD old;
    if (VirtualProtect((void*)(uintptr_t)g_trace_at, 1, PAGE_EXECUTE_READWRITE, &old)) {
        g_orig_byte = *(volatile uint8_t*)(uintptr_t)g_trace_at;
        *(volatile uint8_t*)(uintptr_t)g_trace_at = 0xCC;     /* INT3 */
        VirtualProtect((void*)(uintptr_t)g_trace_at, 1, old, &old);
        loader_log(LOG_INFO, "[EIPTRACE] armed INT3 at 0x%08x (single-step on hit)", g_trace_at);
    }
}

/* Returns 1 if it handled the breakpoint. */
int eiptrace_on_breakpoint(CONTEXT *ctx)
{
    uint32_t eip = (uint32_t)ctx->Eip;
    if (!g_trace_at) return 0;
    if (eip == g_trace_at || eip == g_trace_at + 1) {
        DWORD old;
        VirtualProtect((void*)(uintptr_t)g_trace_at, 1, PAGE_EXECUTE_READWRITE, &old);
        *(volatile uint8_t*)(uintptr_t)g_trace_at = g_orig_byte;   /* one-shot: restore */
        VirtualProtect((void*)(uintptr_t)g_trace_at, 1, old, &old);
        ctx->Eip = g_trace_at;        /* resume at the real instruction */
        g_on = 1;
        if (g_watch_retslot) g_watch = (uint32_t)ctx->Esp;   /* [esp] = return slot at entry */
        if (g_watch) {
            /* HARDWARE data breakpoint (DR0) on the watched word: 4-byte WRITE.
             * Traps the EXACT writing instruction (no single-step off-by-one,
             * catches writes from Mesa/wrapper code too). */
            ctx->Dr0 = g_watch;
            ctx->Dr7 = 0x000D0001u;   /* L0=1, RW0=01(write), LEN0=11(4 bytes) */
            ctx->Dr6 = 0;
            ctx->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
            loader_log(LOG_ERROR, "[EIPTRACE] DR0 write-watch armed on 0x%08x (return slot)", g_watch);
        } else {
            ctx->EFlags |= 0x100;     /* no watch -> plain single-step trace */
        }
        loader_log(LOG_INFO, "[EIPTRACE] entry 0x%08x reached; tracing", g_trace_at);
        return 1;
    }
    return 0;
}

/* Returns 1 if it handled the single-step (keeps stepping). */
int eiptrace_on_singlestep(CONTEXT *ctx)
{
    /* persist-BP replant: we single-stepped over a restored BP instruction;
     * put the INT3 back so it fires again next time. */
    if (g_bp_replant_idx >= 0) {
        int i = g_bp_replant_idx;
        g_bp_replant_idx = -1;
        DWORD old;
        VirtualProtect((void*)(uintptr_t)g_bp[i].addr, 1, PAGE_EXECUTE_READWRITE, &old);
        *(volatile uint8_t*)(uintptr_t)g_bp[i].addr = 0xCC;
        VirtualProtect((void*)(uintptr_t)g_bp[i].addr, 1, old, &old);
        g_bp[i].armed = 1;
        ctx->EFlags &= ~0x100u;   /* clear TF: this was a one-shot step, stop stepping */
        return 1;     /* handled */
    }
    if (!g_on) return 0;
    uint32_t eip = (uint32_t)ctx->Eip;
    /* DR0 hardware write-watch trap: DR6 bit 0 set means the watched word was
     * just written. EIP points at the instruction AFTER the writer (data
     * breakpoints are trap-class), so the writer is the instruction just
     * before EIP - but EIP itself is the precise location to disassemble. */
    if (g_watch && (ctx->Dr6 & 0x1)) {
        uint32_t v = *(volatile uint32_t*)(uintptr_t)g_watch;
        loader_log(LOG_ERROR, "[EIPTRACE] *** DR0 WRITE: 0x%08x now 0x%08x, by/just-before EIP=0x%08x  EAX=0x%08x ECX=0x%08x EDX=0x%08x",
                   g_watch, v, eip, (unsigned)ctx->Eax, (unsigned)ctx->Ecx, (unsigned)ctx->Edx);
        ctx->Dr6 = 0;
        ctx->ContextFlags |= CONTEXT_DEBUG_REGISTERS;  /* keep DR0 armed */
        return 1;
    }
    if (g_watch) { return 0; }     /* DR mode: ignore plain single-steps */
    if (eip >= ELF_LO && eip < ELF_HI) {
        g_ring[g_pos] = eip;
        g_ring_sp[g_pos] = (uint32_t)ctx->Esp;
        g_pos = (g_pos + 1) % RING;
    }
    /* Watchpoint: the instruction at g_prev_eip just executed; if it changed
     * the watched word, it's the clobber we're hunting. */
    if (g_watch) {
        uint32_t cur = *(volatile uint32_t*)(uintptr_t)g_watch;
        if (cur != g_watch_last) {
            loader_log(LOG_ERROR, "[EIPTRACE] *** WATCH 0x%08x: 0x%08x -> 0x%08x  WRITTEN BY EIP=0x%08x",
                       g_watch, g_watch_last, cur, g_prev_eip);
            g_watch_last = cur;
        }
    }
    g_prev_eip = eip;
    ctx->EFlags |= 0x100;             /* keep TF set */
    return 1;
}

void eiptrace_dump(void)
{
    if (!g_on) return;
    loader_log(LOG_ERROR, "[EIPTRACE] last ELF EIP/ESP before crash (oldest -> newest):");
    uint32_t prev_sp = 0;
    for (int i = 0; i < RING; i++) {
        int k = (g_pos + i) % RING;
        uint32_t v = g_ring[k], sp = g_ring_sp[k];
        if (!v) continue;
        int32_t d = prev_sp ? (int32_t)(sp - prev_sp) : 0;
        loader_log(LOG_ERROR, "    eip=0x%08x esp=0x%08x  dsp=%+d", v, sp, d);
        prev_sp = sp;
    }
}
