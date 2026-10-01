/* ---------------------------------------------------------------------------
 *  amcredit_ringedge2.c -- break the amCredit "drain forever" deadlock
 *
 *  WHAT THIS FIXES, and how it was found (Caladrius AC, 2026-09-18).
 *
 *  Caladrius presented exactly ONE frame and then never presented another --
 *  806 seconds of watchdog samples with present9=1 (+0), peekmsg=1 (+0),
 *  sleep=0, wait=0. Not blocked: SPINNING, parked at SILVER_AMP_R_ROM.exe
 *  +0x1435F on every single sample.
 *
 *  That address is inside the game's per-frame credit poll at VA 0x004141B0
 *  (called from 0x0041E2CB, which matches the watchdog's stack frame
 *  0x41E2D0). Its tail drains the amCredit event queue:
 *
 *      00414338  call 0x540e30          ; amCreditPopEvent()
 *      0041433d  cmp  eax, ebx          ; ebx = 0, set by 'xor ebx,ebx' at 0x4141C6
 *      0041433f  je   0x414363          ; 0 -> queue empty -> DONE, return
 *      00414341  ...                    ; 1 = coin, 2 = service, else ignore
 *      0041435a  call 0x540e30
 *      0041435f  cmp  eax, ebx
 *      00414361  jne  0x414341          ; anything but 0 -> keep draining
 *
 *  and amCreditPopEvent itself is:
 *
 *      00540e30  xor  eax, eax
 *      00540e32  cmp  dword ptr [0x6d7860], eax     ; amCredit initialized?
 *      00540e38  jne  0x540e40
 *      00540e3a  mov  eax, 0xfffffffd               ; -3 = NOT INITIALIZED
 *      00540e3f  ret
 *      00540e40  ...                                ; real 16-entry ring pop
 *
 *  So when amCredit is NOT initialized the accessor returns -3, the loop's
 *  only exit test is "== 0", and -3 is not 0. The loop can never terminate.
 *  THE GAME HANGS FOREVER, on its own main thread, on frame 1.
 *
 *  This is a latent bug in the game, not in us: on a real cabinet amCredit is
 *  always initialized before the first frame, so the -3 branch is unreachable
 *  and the loop always exits on the first pop. We reach it because the
 *  game's I/O bring-up bails out earlier (amCreditInit at 0x5417B0, called
 *  from 0x413FF8, never runs -- its jvs_coin.dat is never opened), which
 *  leaves [0x6d7860] at zero while the per-frame poll runs regardless.
 *
 *  THE FIX: make the "not initialized" answer 0 ("nothing to drain") instead
 *  of -3. That is exactly what the caller means by 0, and it is TRUE: an
 *  uninitialised credit queue has no pending events. The loop then exits on
 *  its first pop, the frame completes, and the game keeps running.
 *
 *  WHY THIS IS SAFE, measured rather than assumed:
 *    - amCreditPopEvent (0x540e30) has EXACTLY TWO callers in the whole
 *      image, 0x414338 and 0x41435A -- both of them the two arms of the loop
 *      above. Nothing else in the game reads this return value, so nothing
 *      else can tell -3 from 0.
 *    - Every OTHER amCredit entry point tests [0x6d7860] itself and returns
 *      its own -3; this patch does not touch them, so "amCredit is not ready"
 *      is still reported truthfully everywhere it is actually consulted.
 *    - It does not fabricate credits. The queue stays empty; no coin event is
 *      invented. It only stops the game spinning on an empty queue.
 *
 *  WHY A SIGNATURE AND NOT AN ADDRESS: the bare 'mov eax,-3 / ret' byte
 *  sequence occurs 44 TIMES in this one executable, so patching by opcode
 *  alone would be a shotgun. The full amCreditPopEvent idiom --
 *
 *      33 C0  39 05 <flag32>  75 06  B8 FD FF FF FF  C3
 *
 *  -- occurs exactly ONCE, and the flag32 it carries is the amCredit init
 *  flag, which this file reads back afterwards to report the real state. A
 *  title that does not contain the idiom is not patched at all, so this TU is
 *  inert on every other RingEdge 2 game (transformersha included) unless it
 *  has the identical amCredit build.
 *
 *  ULTRA_RE2_AMCREDIT_FIX=0 disables it. Default ON: the signature is its own
 *  guard, and with it off this title cannot reach frame 2.
 * ------------------------------------------------------------------------- */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ringedge2_hook.h"

/* The idiom, with the 4 flag bytes wild. 16 bytes total. */
#define SIG_LEN 16
static const unsigned char SIG[SIG_LEN] = {
    0x33, 0xC0,                   /* xor  eax, eax                 */
    0x39, 0x05, 0,0,0,0,          /* cmp  dword ptr [flag32], eax  */
    0x75, 0x06,                   /* jne  +6                       */
    0xB8, 0xFD, 0xFF, 0xFF, 0xFF, /* mov  eax, 0xfffffffd   <-- us */
    0xC3                          /* ret                           */
};
/* Which of those bytes must match (0 = wildcard). */
static const unsigned char MASK[SIG_LEN] = {
    1,1, 1,1, 0,0,0,0, 1,1, 1,1,1,1,1, 1
};
#define PATCH_OFF 10              /* offset of the 'mov eax,-3' inside SIG */

static volatile LONG s_done;      /* 1 once patched (or given up) */
static unsigned char* s_flag;     /* amCredit init flag, from the signature   */
static unsigned char* s_head;     /* ring head pointer, likewise              */

/* THE CODE SECTIONS of the main module -- NOT the whole image.
 *
 * The first version of this walked OptionalHeader.SizeOfImage. For Caladrius
 * that is 0x10D12000 = 269 MiB, of which .data alone is 266.9 MiB of demand-zero
 * BSS with only 0xBA400 bytes actually on disk. Byte-scanning all of it,
 * synchronously inside DllMain under the PE loader lock, cost roughly 0.7 s of
 * startup and assumed every page in a 269 MiB reservation is committed and
 * readable -- true for this exe, NOT guaranteed for the next title, and an
 * uncommitted page there would be an access violation of precisely the kind
 * that killed the 17:26 run from a different code path.
 *
 * We are looking for an instruction sequence, so only sections marked executable
 * can contain it. For this title that is .text at 0x1B436F = 1.70 MiB: 158x less
 * memory touched, the same single hit, and no dependence on BSS being committed.
 * The section walk mirrors the one the watchdog already uses to report its
 * code range (watchdog_ringedge2.c:186) -- the log line
 *   "[wd] watchdog armed: ... code=0x401000-0x5B536F"
 * is that same computation, so this range is already proven on this game. */
#define MAX_CODE_SECTIONS 8
typedef struct { unsigned char* p; size_t n; } code_range;

static int main_code_ranges(unsigned char** image_base, code_range* out, int max)
{
    HMODULE h = GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER* dos;
    IMAGE_NT_HEADERS* nt;
    IMAGE_SECTION_HEADER* sec;
    int i, n = 0;

    if (!h) return 0;
    dos = (IMAGE_DOS_HEADER*)h;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS*)((unsigned char*)h + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    *image_base = (unsigned char*)h;
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < (int)nt->FileHeader.NumberOfSections && n < max; ++i, ++sec) {
        DWORD vsz;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        vsz = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        if (!vsz) continue;
        out[n].p = (unsigned char*)h + sec->VirtualAddress;
        out[n].n = (size_t)vsz;
        ++n;
    }
    return n;
}

/* A pointer lifted out of the instruction stream is only trustworthy if it lands
 * inside this module's own image. s_head in particular is just the dword that
 * followed an `8b 0d` we matched on; if a future build shifts that by a byte we
 * would hand the watchdog a wild pointer, and the watchdog is the one thread
 * that must never be the one that dies. */
static int in_image(const unsigned char* p, size_t n)
{
    HMODULE h = GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER* dos;
    IMAGE_NT_HEADERS* nt;
    unsigned char* b;
    size_t isize;
    if (!p || !h) return 0;
    dos = (IMAGE_DOS_HEADER*)h;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS*)((unsigned char*)h + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    b = (unsigned char*)h;
    isize = (size_t)nt->OptionalHeader.SizeOfImage;
    return p >= b && (size_t)(p - b) + n <= isize;
}

static int matches(const unsigned char* p)
{
    int i;
    for (i = 0; i < SIG_LEN; ++i)
        if (MASK[i] && p[i] != SIG[i]) return 0;
    return 1;
}

/* One sweep over the module's executable sections.
 *   1 = patched, -1 = refused (signature not unique), 0 = not found (retry).
 * The tri-state matters: the old version returned 1 for "refused" too, so a
 * refusal was indistinguishable from a success in the caller. */
#define AMC_PATCHED  1
#define AMC_REFUSED (-1)
#define AMC_NOTFOUND 0

/* The signature is 16 bytes, but we also read 6 bytes PAST it to recover the
 * ring-head pointer from the following 'mov ecx,[head32]'. The scan bound must
 * account for that tail or the last candidate in a section reads up to 6 bytes
 * past the section end. */
#define SIG_TAIL 6

static int try_patch(void)
{
    unsigned char* base = NULL;
    unsigned char* hit  = NULL;
    code_range     code[MAX_CODE_SECTIONS];
    int  nsec, sx, found = 0;
    size_t i;
    DWORD old;

    nsec = main_code_ranges(&base, code, MAX_CODE_SECTIONS);
    if (nsec <= 0) return AMC_NOTFOUND;

    /* Count every occurrence before touching anything: if the idiom is NOT
     * unique the assumption behind this patch does not hold for this image
     * and we must not guess which one is amCredit's. Two is enough to know
     * that, so stop there rather than finish the sweep. */
    for (sx = 0; sx < nsec && found < 2; ++sx) {
        unsigned char* p = code[sx].p;
        size_t n = code[sx].n;
        if (n < (size_t)(SIG_LEN + SIG_TAIL)) continue;
        for (i = 0; i + SIG_LEN + SIG_TAIL <= n; ++i) {
            if (p[i] != 0x33) continue;          /* cheap first-byte reject */
            if (!matches(p + i)) continue;
            ++found;
            if (!hit) hit = p + i;
            if (found >= 2) break;
        }
    }
    if (!found) return AMC_NOTFOUND;
    if (found > 1) {
        ul_re2_log("[amcredit] signature is NOT unique in this image -- refusing "
                   "to patch, because we cannot tell which match is amCredit's");
        InterlockedExchange(&s_done, 1);
        return AMC_REFUSED;
    }

    /* Both pointers come out of the instruction stream, so neither is trusted
     * until it is shown to land inside this module. A rejected pointer only
     * costs us the watchdog read-back; the patch itself still goes in. */
    {
        unsigned char* f = (unsigned char*)(uintptr_t)(*(DWORD*)(hit + 4));
        unsigned char* h = NULL;
        /* 'mov ecx,[head32]' is the next instruction after the ret: 8B 0D <head32> */
        if (hit[SIG_LEN] == 0x8B && hit[SIG_LEN + 1] == 0x0D)
            h = (unsigned char*)(uintptr_t)(*(DWORD*)(hit + SIG_LEN + 2));
        s_flag = in_image(f, sizeof(DWORD)) ? f : NULL;
        s_head = in_image(h, sizeof(DWORD)) ? h : NULL;
    }

    if (!VirtualProtect(hit + PATCH_OFF, 5, PAGE_EXECUTE_READWRITE, &old)) {
        ul_re2_log("[amcredit] VirtualProtect failed at %p (err %lu)",
                   (void*)(hit + PATCH_OFF), (unsigned long)GetLastError());
        return AMC_NOTFOUND;
    }
    /* mov eax,0xfffffffd  ->  xor eax,eax ; nop ; nop ; nop
     * Same 5 bytes, so the following 'ret' and every branch target around it
     * stay exactly where they were.
     *
     * WRITE THE FIRST BYTE LAST. Another thread could in principle be executing
     * these bytes while we rewrite them. Filling 1..4 first leaves the sequence
     * decoding as 'mov eax,0x909090C0' throughout -- still a single harmless
     * 5-byte instruction -- and only the final store flips it to 'xor eax,eax'.
     * Writing byte 0 first would expose a transient 'xor eax,eax; std; <junk>'.
     * Costs nothing, so there is no reason to leave the window open. */
    hit[PATCH_OFF + 1] = 0xC0;
    hit[PATCH_OFF + 2] = 0x90;
    hit[PATCH_OFF + 3] = 0x90;
    hit[PATCH_OFF + 4] = 0x90;
    MemoryBarrier();
    hit[PATCH_OFF + 0] = 0x33;
    VirtualProtect(hit + PATCH_OFF, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), hit + PATCH_OFF, 5);

    ul_re2_log("[amcredit] PATCHED amCreditPopEvent at %p "
               "(image+0x%lX): returns 0 (empty) instead of -3 (uninitialised). "
               "init flag at %p, ring head at %p",
               (void*)hit, (unsigned long)(size_t)(hit - base),
               (void*)s_flag, (void*)s_head);
    InterlockedExchange(&s_done, 1);
    return AMC_PATCHED;
}

/* The image may not be readable in its final form the instant we attach (a
 * packed title unpacks itself after DllMain returns), so retry for a while
 * rather than decide on one look. Cheap now that the scan is section-scoped:
 * ~1.7 MB for Caladrius, once a second, for at most a minute, and it stops the
 * moment it succeeds or refuses.
 *
 * NOTE this path has never actually run on Caladrius -- try_patch() succeeds
 * synchronously at install -- so it is untested code kept for packed titles.
 * The Sleep is sliced so DLL_PROCESS_DETACH is observed promptly instead of up
 * to a second later. */
static volatile LONG s_stop;

static DWORD WINAPI retry_thread(LPVOID unused)
{
    int tries, k;
    (void)unused;
    for (tries = 0; tries < 60; ++tries) {
        for (k = 0; k < 10; ++k) {
            if (InterlockedCompareExchange(&s_stop, 0, 0)) return 0;
            if (InterlockedCompareExchange(&s_done, 0, 0)) return 0;
            Sleep(100);
        }
        if (try_patch() != AMC_NOTFOUND) return 0;
    }
    if (!InterlockedCompareExchange(&s_done, 0, 0))
        ul_re2_log("[amcredit] signature never appeared in this image -- no "
                   "patch applied (normal for a title that does not use this "
                   "amCredit build)");
    return 0;
}

void ul_re2_amcredit_install(void)
{
    /* ul_re2_env(), not getenv(): a value published with SetEnvironmentVariable
     * inside this process is invisible to the CRT copy, so getenv() would make
     * this documented kill switch silently do nothing. */
    const char* e = ul_re2_env("ULTRA_RE2_AMCREDIT_FIX");
    if (e && e[0] == '0') {
        ul_re2_log("[amcredit] disabled (ULTRA_RE2_AMCREDIT_FIX=0)");
        return;
    }
    if (try_patch() != AMC_NOTFOUND) return;
    CreateThread(NULL, 0, retry_thread, NULL, 0, NULL);
}

void ul_re2_amcredit_shutdown(void)
{
    InterlockedExchange(&s_stop, 1);
}

/* Read back for the watchdog: the two numbers that say whether the game's own
 * I/O bring-up ever ran. -1 means "we never located them", which is also what
 * a pointer rejected by in_image() reports -- better a missing number than the
 * watchdog thread taking a fault. */
void ul_re2_amcredit_state(long* init_flag, long* ring_head)
{
    unsigned char* f = s_flag;
    unsigned char* h = s_head;
    if (init_flag) *init_flag = f ? (long)*(DWORD*)f : -1;
    if (ring_head) *ring_head = h ? (long)*(DWORD*)h : -1;
}

/* ===========================================================================
 *  VIRTUAL COIN -- a credit button for a game that has no coin key.
 *
 *  WHY THIS EXISTS. Caladrius cannot be given a coin button by mapping one,
 *  and that is a property of the game. Its keyboard table binds exactly three
 *  system controls -- 0 = SERVICE ('1'), 1 = TEST ('2'), 3 = exit (ESC). The
 *  slot a coin would occupy, control 2, has NO key bound AND is read by nothing
 *  anywhere in the 3 MB image. Coin reaches this title only over JVS, through
 *  amCreditUpdateJVS() / amJvspAckCoinInput(), i.e. through amCredit -- and
 *  amCredit never initializes here.
 *
 *  But the thing the game actually consumes is one step further in: an EVENT
 *  RING that its per-frame credit poll drains. We already know that ring
 *  exactly, because the same signature that finds amCreditPopEvent hands us its
 *  head pointer. The game's own producer is at 0x00540E70:
 *
 *      00540e70  mov eax,[0x6d79bc]      ; head
 *      00540e75  mov ecx,[0x6d79c0]      ; tail
 *      00540e7b  lea edx,[eax-1] / cmp ecx,edx / je  full
 *      00540e82  cmp ecx,0xf / jne + / test eax,eax / je full
 *      00540e8b  mov edx,[esp+4]         ; the event value
 *      00540e8f  mov [ecx*4+0x6d79c4],edx
 *      00540e96  inc tail, wrap at 0x10
 *
 *  and the consumer at 0x00414341 treats value 1 and value 2 as the coin /
 *  service events. So we push a 1 and the game sees a coin.
 *
 *  Two things have to be true for the game to read it, and we do both:
 *    - the ring must be reachable: amCreditPopEvent returns early unless the
 *      init flag is non-zero, so we set it. With head == tail the very same
 *      function then reports EMPTY by its own logic, which is exactly what our
 *      patch above was faking -- so setting the flag makes that patch moot
 *      rather than fighting it.
 *    - the push must respect the game's own full/wrap rules, which the code
 *      below mirrors instruction for instruction.
 *
 *  HONEST LIMIT: this delivers a COIN EVENT. Whether the credit COUNTER then
 *  increments is amCredit's business, and amCredit's own state was never
 *  initialized, so it may take the event and still show zero credits. FREE PLAY
 *  (free_play = 1 in the game's SILVER.ini) remains the reliable route to
 *  actually starting a game. This is the credit button; free play is the
 *  guarantee.
 *
 *  OPT-IN. ULTRA_RE2_COIN_KEY names a virtual-key code; unset, none of this
 *  runs and nothing touches the init flag. The container binds a pad control to
 *  a key the game does not use (F12) and we watch for that key, so the bridge
 *  needs no knowledge of this at all -- it just sends a keystroke as usual.
 * ======================================================================== */

static volatile LONG s_coin_pushed;

/* Mirror of the game's producer at 0x540E70. Returns 1 if the event went in. */
static int amc_push_event(DWORD value)
{
    DWORD *head, *tail, *ring;
    DWORD h, t;
    if (!s_head || !in_image(s_head, 8 + 16 * sizeof(DWORD))) return 0;
    head = (DWORD*)s_head;
    tail = (DWORD*)(s_head + 4);
    ring = (DWORD*)(s_head + 8);
    h = *head; t = *tail;
    if (h > 0x10 || t > 0x10) return 0;          /* state we do not recognize */
    if (t == h - 1) return 0;                    /* full, per the game's test */
    if (t == 0xF && h == 0) return 0;            /* full at the wrap          */
    ring[t] = value;
    t = (t + 1 >= 0x10) ? 0 : t + 1;
    *tail = t;
    return 1;
}

static DWORD WINAPI coin_thread(LPVOID arg)
{
    int vk = (int)(INT_PTR)arg;
    int prev = 0;
    for (;;) {
        int now;
        if (InterlockedCompareExchange(&s_stop, 0, 0)) return 0;
        now = (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
        if (now && !prev) {
            if (s_flag && !*(DWORD*)s_flag) {
                *(DWORD*)s_flag = 1;             /* make the ring reachable */
                ul_re2_log("[amcredit] coin: marking amCredit reachable so the "
                           "game's credit poll will drain its event ring");
            }
            if (amc_push_event(1))
                ul_re2_log("[amcredit] COIN pushed (event 1) -- ring now head=%lu tail=%lu",
                           (unsigned long)*(DWORD*)s_head,
                           (unsigned long)*(DWORD*)(s_head + 4));
            else
                ul_re2_log("[amcredit] coin press ignored: ring full or not located");
            InterlockedIncrement(&s_coin_pushed);
        }
        prev = now;
        Sleep(16);                               /* ~60 Hz, same as the game */
    }
}

void ul_re2_amcredit_coin_install(void)
{
    const char* e = ul_re2_env("ULTRA_RE2_COIN_KEY");
    int vk;
    if (!e || !*e) return;                       /* opt-in; off by default */
    vk = (int)strtol(e, NULL, 0);
    if (vk <= 0 || vk > 255) {
        ul_re2_log("[amcredit] ULTRA_RE2_COIN_KEY='%s' is not a VK code 1..255", e);
        return;
    }
    ul_re2_log("[amcredit] virtual coin armed on VK 0x%02X -- a press pushes a "
               "coin event into the game's own credit ring", vk);
    CreateThread(NULL, 0, coin_thread, (LPVOID)(INT_PTR)vk, 0, NULL);
}
