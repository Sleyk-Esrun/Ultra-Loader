/* SPDX-License-Identifier: MIT
 * NOTE: independent namco_es3 board copy -- adapted from native_x86/src/hook/gamestate_probe.c (reference only; no build-time or runtime dependency on native_x86 remains).
 * =============================================================================
 *  gamestate_probe_es3.c -- in-process probe/poke of Chaos Breaker game.exe globals
 * =============================================================================
 *
 *  Per the game.exe disassembly (re_typex/), exit code 1 happens iff the game
 *  state byte at VA 0x4f2116 reaches 0x0F (frame loop `return 1`). Three gates
 *  write it: the JVS I/O watchdog (state 0x0E via g_ioStatus<0 or 60 bad frames),
 *  an operator/"config-dirty reboot" (0x4f2140 bit 8 clear), and the backup
 *  checksum (0x45f305). Our hook runs INSIDE the game, so we read/poke those
 *  globals directly (RVA-relative to the real module base => ASLR-safe).
 *
 *  ULTRA_TYPEX_POKE=1 enables the poke (keep I/O happy + neutralize the gates).
 *  ULTRA_JVS_LOG receives a trace of state transitions so we can see which gate
 *  would have fired. This is the freeplay-style loader poke, applied to the exit.
 * =============================================================================
 */
#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* game.exe image base is 0x400000; these are the VAs from the disasm. */
#define VA_GAMESTATE 0x4f2116u   /* byte: 0x0E=JVS-error, 0x0F=shutdown->exit(1) */
#define VA_IOSTATUS  0x45dc08u   /* dword: 1=frame-ready(good); <0 => watchdog   */
#define VA_IOWATCH   0x4f3268u   /* dword: consecutive bad frames; >=60 => error */
#define VA_IORESULT  0x4f3264u   /* dword: last io_poll() result; <0 => error    */
#define VA_CTRL      0x4f2140u   /* dword: bit8(0x100) set = keep-running         */
#define VA_BACKUPBAD 0x45f305u   /* byte: 1 = BACKUP DATA FAILED                  */
#define IMAGE_BASE   0x400000u
/* These VAs are a per-GAME profile (Chaos Breaker). Applying them to any other
 * executable pokes unrelated memory and crashes it (observed: GigaWing -> access
 * violation). Gate the poke on the host image's PE entry point so the profile
 * only ever fires on the exact game it was built for. */
#define AOE_CHAOS_BREAKER 0x00047091u   /* OptionalHeader.AddressOfEntryPoint */

static unsigned image_entry_rva(BYTE* base) {
    if (!base) return 0;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return (unsigned)nt->OptionalHeader.AddressOfEntryPoint;
}

static void glog(const char* fmt, ...) {
    const char* p = getenv("ULTRA_JVS_LOG"); if (!p || !*p) return;
    FILE* f = fopen(p, "a"); if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

static DWORD WINAPI probe_thread(LPVOID arg) {
    (void)arg;
    const char* pe = getenv("ULTRA_TYPEX_POKE");
    int poke = pe && pe[0] == '1';

    BYTE* base = (BYTE*)GetModuleHandleA(NULL);     /* game.exe actual base */
    if (!base) return 0;

    /* Refuse to poke unless the host image is the exact game this profile targets
     * (Chaos Breaker). On any other binary the VAs are meaningless -> corruption. */
    unsigned aoe = image_entry_rva(base);
    if (poke && aoe != AOE_CHAOS_BREAKER) {
        glog("PROBE: poke DISABLED -- image entry RVA %08X != Chaos Breaker (%08X); "
             "this profile only applies to Chaos Breaker. Observing only.",
             aoe, AOE_CHAOS_BREAKER);
        poke = 0;
    }
    #define P(va, T) ((volatile T*)(base + ((va) - IMAGE_BASE)))
    volatile uint8_t*  gameState = P(VA_GAMESTATE, uint8_t);
    volatile int32_t*  ioStatus  = P(VA_IOSTATUS,  int32_t);
    volatile uint32_t* ioWatch   = P(VA_IOWATCH,   uint32_t);
    volatile int32_t*  ioResult  = P(VA_IORESULT,  int32_t);
    volatile uint32_t* ctrl      = P(VA_CTRL,      uint32_t);
    volatile uint8_t*  backupBad = P(VA_BACKUPBAD, uint8_t);
    #undef P

    /* DYNAMIC MENU INSTRUMENTATION (RE confirmation): the on-screen menu's state lives
     * in the object at *[0x4f95e0]; cursor=[obj+0xc], item-count=[obj+0x10], active
     * handler=[obj+0x9c]. Internal input word @0x473726. Watch whether injected input
     * actually changes the input word and the cursor - to learn what the menu reads. */
    volatile uint32_t* menuObjPtr = (volatile uint32_t*)(base + (0x4f95e0u - IMAGE_BASE));
    volatile uint16_t* inWordP1   = (volatile uint16_t*)(base + (0x473726u - IMAGE_BASE));
    volatile uint16_t* inWordP2   = (volatile uint16_t*)(base + (0x473728u - IMAGE_BASE));
    (void)inWordP2;   /* documented P2 input-word address; not read in this diag loop */
    /* JVS node-1 buffer (does jvs.lib store OUR switch bytes here?) + node status word. */
    volatile uint8_t*  ndSys      = (volatile uint8_t*)(base + (0x5bbb87u - IMAGE_BASE));
    volatile uint8_t*  ndP1b1     = (volatile uint8_t*)(base + (0x5bbb88u - IMAGE_BASE));
    volatile uint8_t*  ndP1b2     = (volatile uint8_t*)(base + (0x5bbb89u - IMAGE_BASE));
    volatile int32_t*  ndStatus1  = (volatile int32_t*)(base + (0x474878u - IMAGE_BASE));

    Sleep(1500);   /* let the game finish mapping/initializing its .data first */
    glog("PROBE: started (base=%p poke=%d)", (void*)base, poke);
    uint8_t last_gs = 0xFF;
    unsigned itc = 0;
    uint16_t last_in = 0xFFFF, last_cur = 0xFFFF;

    for (;;) {
        uint8_t gs = *gameState;
        if (gs != last_gs) {
            glog("PROBE: gameState %02X -> %02X  (ioStatus=%d ioWatch=%u ioResult=%d ctrl=%08X backupBad=%u)",
                 last_gs, gs, (int)*ioStatus, *ioWatch, (int)*ioResult, *ctrl, *backupBad);
            last_gs = gs;
        }
        /* menu/input dump: on input-word change (so we see if injection lands) + ~1s heartbeat */
        {
            uint16_t inw = *inWordP1;
            uint32_t obj = *menuObjPtr;
            uint16_t cur = 0xFFFF, cnt = 0xFFFF; uint32_t hnd = 0;
            if (obj > 0x10000u && obj < 0x80000000u) {
                /* `obj` is a 32-bit guest pointer; widen via uintptr_t for the cast */
                uintptr_t op = (uintptr_t)obj;
                cur = *(volatile uint16_t*)(op + 0x0c);
                cnt = *(volatile uint16_t*)(op + 0x10);
                hnd = *(volatile uint32_t*)(op + 0x9c);
            }
            if (inw != last_in || cur != last_cur || (itc % 250u) == 0u) {
                glog("PROBE: gs=%02X ioSt=%d ndStat=%d | node1[sys=%02X b1=%02X b2=%02X] -> inP1=%04X | menuObj=%08X cur=%04X cnt=%04X hnd=%08X",
                     gs, (int)*ioStatus, (int)*ndStatus1, *ndSys, *ndP1b1, *ndP1b2, inw, obj, cur, cnt, hnd);
                last_in = inw; last_cur = cur;
            }
        }
        itc++;
        if (poke) {
            if (*ioStatus < 0) *ioStatus = 1;       /* keep frame-ready (Rank#1) */
            if (*ioResult < 0) *ioResult = 1;
            if (*ioWatch != 0) *ioWatch = 0;        /* reset watchdog            */
            /* DO NOT set 0x4f2140 bit 0x100: jvs.lib RE proved the input edge-dispatch
             * @0x40e310 does `test [0x4f2140],0x104; jne skip` -> bit 0x100 (or 0x04)
             * SUPPRESSES all button-press events. Our old `*ctrl |= 0x100` keep-alive
             * poke was silencing ALL input (0x473726 stayed 0). The game manages 0x100
             * itself; we must leave it (and 0x04) CLEAR so input edges fire. Opt-in only:*/
            { const char* kc = getenv("ULTRA_POKE_KEEPRUN"); if (kc && kc[0]=='1') *ctrl |= 0x100u; }
            if (*backupBad) *backupBad = 0;          /* clear backup-fail (Rank#3)*/
            if (gs == 0x0E || gs == 0x0F) {          /* veto error/shutdown state */
                *gameState = (last_gs < 0x0E) ? last_gs : 0x00;
                glog("PROBE: VETOED gameState %02X", gs);
            }
        }
        Sleep(2);
    }
}

void ultra_gamestate_probe_start(void) {
    /* Only meaningful for this game family; gated by env so it never interferes
     * with anything else. */
    if (!getenv("ULTRA_TYPEX_POKE") && !getenv("ULTRA_PROBE")) return;
    CreateThread(NULL, 0, probe_thread, NULL, 0, NULL);
}
#endif /* _WIN32 */
