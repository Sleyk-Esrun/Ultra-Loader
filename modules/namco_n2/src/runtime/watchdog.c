/* ------------------------------------------------------------------ *
 * watchdog.c - hang detector / live stack dumper.
 *
 * The maintrace INT3 probes tell us the last main()/init_graphics call
 * that STARTED, but if the game then spins forever inside a callee (e.g.
 * a malformed std::list walk in init_sprite) the log just goes silent and
 * we learn nothing. This watchdog runs on its own thread, watches the
 * loader's log heartbeat, and when the game has emitted nothing for N
 * seconds it suspends the guest thread, snapshots its CPU + stack, and
 * dumps EIP / registers / the ELF return-address chain - the same data a
 * crash dump gives, but for a *hang*. Map the addresses with `nm`.
 *
 * Tunable:  set ULTRA_WATCHDOG=<seconds>   (0 disables; default 20)
 * ------------------------------------------------------------------ */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loader.h"

/* defined in elf_loader.c, bumped by every loader_log() call */
extern volatile uint32_t g_ul_heartbeat;

/* ELF .text window for After Burner Climax (abc). Stack words in this range
 * are return addresses into game code - they reconstruct the call path. */
#define ELF_TEXT_LO 0x0804f074u
#define ELF_TEXT_HI 0x081f352cu

static HANDLE        g_game_thread = NULL;
static volatile LONG g_wd_stop     = 0;

static int wd_readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = (uintptr_t)p, e = a + n;
    if (e < a) return 0;
    while (a < e) {
        if (!VirtualQuery((LPCVOID)a, &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT) return 0;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}

/* Snapshot taken while the guest thread is suspended, then logged after it
 * resumes (never log while it is suspended - it may hold the stdio lock). */
typedef struct {
    int      ok;
    CONTEXT  c;
    uint8_t  code[16];   int code_ok;
    uint32_t stack[128]; int nstack;   /* raw words from ESP up */
} WdSnap;

static void wd_capture(WdSnap *s)
{
    memset(s, 0, sizeof *s);
    if (SuspendThread(g_game_thread) == (DWORD)-1) return;

    s->c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS;
    if (GetThreadContext(g_game_thread, &s->c)) {
        s->ok = 1;
        uint8_t *eip = (uint8_t *)(uintptr_t)s->c.Eip;
        if (wd_readable(eip, sizeof s->code)) { memcpy(s->code, eip, sizeof s->code); s->code_ok = 1; }
        uint32_t *sp = (uint32_t *)(uintptr_t)s->c.Esp;
        for (int i = 0; i < 128; i++) {
            if (!wd_readable(sp + i, 4)) break;
            s->stack[i] = sp[i];
            s->nstack = i + 1;
        }
    }
    ResumeThread(g_game_thread);
}

static void wd_dump(const char *tag, WdSnap *s)
{
    if (!s->ok) { loader_log(LOG_ERROR, "[WATCHDOG] %s: GetThreadContext failed (%lu)", tag, GetLastError()); return; }
    CONTEXT *c = &s->c;
    loader_log(LOG_ERROR, "=== HANG WATCHDOG: %s ===", tag);
    loader_log(LOG_ERROR, "  EIP=0x%08x  ESP=0x%08x  EBP=0x%08x",
               (unsigned)c->Eip, (unsigned)c->Esp, (unsigned)c->Ebp);
    loader_log(LOG_ERROR, "  EAX=0x%08x EBX=0x%08x ECX=0x%08x EDX=0x%08x ESI=0x%08x EDI=0x%08x",
               (unsigned)c->Eax, (unsigned)c->Ebx, (unsigned)c->Ecx,
               (unsigned)c->Edx, (unsigned)c->Esi, (unsigned)c->Edi);
    if (s->code_ok)
        loader_log(LOG_ERROR, "  bytes@EIP: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                   s->code[0],s->code[1],s->code[2],s->code[3],s->code[4],
                   s->code[5],s->code[6],s->code[7],s->code[8],s->code[9]);

    /* Return-address chain: every stack word that lands in ELF .text. The
     * shallowest is the function currently spinning (or its caller). */
    loader_log(LOG_ERROR, "  ELF .text return chain (ESP scan):");
    int found = 0;
    for (int i = 0; i < s->nstack; i++) {
        uint32_t v = s->stack[i];
        if (v >= ELF_TEXT_LO && v < ELF_TEXT_HI) {
            loader_log(LOG_ERROR, "    chain[%d]: [esp+%-4d] = 0x%08x", found, i*4, v);
            if (++found >= 20) break;
        }
    }
    if (!found) loader_log(LOG_ERROR, "    (no ELF .text return address in %d stack words)", s->nstack);

    /* EBP frame chain (works when frames are intact). */
    loader_log(LOG_ERROR, "  EBP frame chain:");
    uint32_t ebp = c->Ebp;
    for (int d = 0; d < 12; d++) {
        if (ebp < 0x10000 || ebp >= 0xfffff000 || (ebp & 3)) break;
        if (!wd_readable((void *)(uintptr_t)ebp, 8)) break;
        uint32_t *f = (uint32_t *)(uintptr_t)ebp;
        loader_log(LOG_ERROR, "    [%2d] EBP=0x%08x ret=0x%08x", d, ebp, f[1]);
        if (f[0] <= ebp) break;
        ebp = f[0];
    }
}

/* Dump `words` 32-bit words at `addr`, and if the first word looks like a
 * pointer, follow it one level (handy for inspecting std::list sentinels). */
static void wd_dump_mem(uint32_t addr, int words)
{
    if (!wd_readable((void *)(uintptr_t)addr, (size_t)words * 4)) {
        loader_log(LOG_ERROR, "  [mem 0x%08x] <unreadable>", addr);
        return;
    }
    uint32_t *p = (uint32_t *)(uintptr_t)addr;
    for (int i = 0; i < words; i += 4) {
        loader_log(LOG_ERROR, "  [0x%08x] %08x %08x %08x %08x",
                   addr + i*4, p[i],
                   (i+1<words)?p[i+1]:0, (i+2<words)?p[i+2]:0, (i+3<words)?p[i+3]:0);
    }
    uint32_t head = p[0];
    if (head >= 0x10000 && head < 0xc0000000 && head != addr &&
        wd_readable((void *)(uintptr_t)head, 16)) {
        uint32_t *n = (uint32_t *)(uintptr_t)head;
        loader_log(LOG_ERROR, "    -> [0x%08x] (head._M_next target) %08x %08x %08x %08x",
                   head, n[0], n[1], n[2], n[3]);
    }
}

/* Walk an old-SGI std::list: the object at `listaddr` holds a single pointer
 * to the heap sentinel node; node._M_next is at offset 0. Follow _M_next and
 * report length / cycle-back / corruption - tells finite-huge from infinite. */
static void wd_walk_chain(uint32_t listaddr)
{
    if (!wd_readable((void *)(uintptr_t)listaddr, 4)) {
        loader_log(LOG_ERROR, "  [chain 0x%08x] list obj unreadable", listaddr);
        return;
    }
    uint32_t sentinel = *(uint32_t *)(uintptr_t)listaddr;
    loader_log(LOG_ERROR, "  [chain] list@0x%08x -> sentinel 0x%08x", listaddr, sentinel);
    uint32_t cur = sentinel, steps = 0;
    const uint32_t MAX = 4096;
    for (; steps < MAX; steps++) {
        if (!wd_readable((void *)(uintptr_t)cur, 8)) {
            loader_log(LOG_ERROR, "    step %u: node 0x%08x UNREADABLE (broken chain)", steps, cur);
            return;
        }
        uint32_t next = *(uint32_t *)(uintptr_t)cur;
        if (steps < 8 || steps % 512 == 0)
            loader_log(LOG_ERROR, "    step %u: node 0x%08x -> next 0x%08x", steps, cur, next);
        if (next == sentinel) {
            loader_log(LOG_ERROR, "    -> cycles back to sentinel after %u nodes (VALID, finite)", steps + 1);
            return;
        }
        cur = next;
    }
    loader_log(LOG_ERROR, "    -> did NOT cycle back within %u steps (infinite/huge or corrupt)", MAX);
}

static void wd_dump_watchmem(void)
{
    const char *chain = getenv("ULTRA_WD_CHAIN");
    /* Default to the init_sprite tag_SpriteBuffer list array when unset, so a
     * watchdog fire always shows the chain even if env vars don't propagate. */
    if (!chain || !*chain) chain = "0x956bc30,0x956bc34";
    {
        char cb[128]; strncpy(cb, chain, sizeof cb - 1); cb[sizeof cb - 1] = 0;
        char *sv = NULL;
        for (char *t = strtok_r(cb, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) {
            uint32_t a = (uint32_t)strtoul(t, NULL, 0);
            if (a) wd_walk_chain(a);
        }
    }
    const char *m = getenv("ULTRA_WD_MEM");
    if (!m || !*m) return;
    char buf[256]; strncpy(buf, m, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    loader_log(LOG_ERROR, "  --- ULTRA_WD_MEM regions ---");
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        uint32_t a = (uint32_t)strtoul(tok, NULL, 0);
        if (a) wd_dump_mem(a, 16);
    }
}

static DWORD WINAPI wd_thread(LPVOID arg)
{
    int secs = (int)(intptr_t)arg;
    uint32_t last = g_ul_heartbeat;
    int stall = 0, fires = 0;

    for (;;) {
        Sleep(1000);
        if (InterlockedCompareExchange(&g_wd_stop, 0, 0)) return 0;

        uint32_t hb = g_ul_heartbeat;
        if (hb != last) { last = hb; stall = 0; continue; }   /* progressing */
        if (++stall < secs) continue;                          /* not silent long enough */

        /* Silent for `secs` seconds - take two snapshots ~700ms apart so we
         * can see whether EIP is moving (spin loop) or wedged on a wait. */
        WdSnap a, b;
        wd_capture(&a);
        Sleep(700);
        wd_capture(&b);
        char tag[80];
        snprintf(tag, sizeof tag, "no output for %ds (fire #%d), snapshot A", secs, fires + 1);
        wd_dump(tag, &a);
        snprintf(tag, sizeof tag, "snapshot B (+700ms)  EIP moved: %s",
                 (a.ok && b.ok && a.c.Eip != b.c.Eip) ? "YES (spinning)" : "no (blocked)");
        wd_dump(tag, &b);
        wd_dump_watchmem();
        loader_log(LOG_ERROR, "[WATCHDOG] guest produced no output for %ds; see dumps above. "
                              "Map EIP/chain with nm. (ULTRA_WATCHDOG=0 to disable)", secs);

        fires++;
        last = g_ul_heartbeat;
        stall = 0;
        if (fires >= 3) {            /* stop spamming once it is clearly wedged */
            loader_log(LOG_ERROR, "[WATCHDOG] 3 dumps taken; standing down.");
            return 0;
        }
    }
}

/* Called from main() on the GAME thread, just before it enters the ELF. */
void watchdog_start(void)
{
    const char *e = getenv("ULTRA_WATCHDOG");
    int secs = e ? atoi(e) : 20;
    if (secs <= 0) { loader_log(LOG_INFO, "[WATCHDOG] disabled"); return; }

    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &g_game_thread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                         FALSE, 0)) {
        loader_log(LOG_ERROR, "[WATCHDOG] DuplicateHandle failed (%lu)", GetLastError());
        return;
    }
    HANDLE h = CreateThread(NULL, 0, wd_thread, (LPVOID)(intptr_t)secs, 0, NULL);
    if (h) CloseHandle(h);
    loader_log(LOG_INFO, "[WATCHDOG] armed: dump guest stack after %ds of silence "
                         "(set ULTRA_WATCHDOG to tune, 0=off)", secs);
}

void watchdog_stop(void) { InterlockedExchange(&g_wd_stop, 1); }
