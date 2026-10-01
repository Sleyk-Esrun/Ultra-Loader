/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  game_chaosbreaker.c -- Taito Type X "Chaos Breaker" per-game board_io_ops
 * =============================================================================
 *
 *  EXTRACTION NOTE: this file consolidates two things that used to live INLINE
 *  in the shared hook source tree (native_x86/src/hook/), where a Chaos-Breaker
 *  hack could -- and did -- regress other, unrelated boards built from the same
 *  files (Cave PC, Konami PC, ...). See board_io_ops.h for the full rationale.
 *  Both pieces moved here verbatim (logic unchanged, only re-homed):
 *
 *   1. gamestate_probe.c's ENTIRE probe_thread() -- an in-process background
 *      thread that pokes game.exe's exit-gate globals so the JVS I/O watchdog
 *      never trips and the backup-checksum / config-dirty reboots never fire.
 *      Was started unconditionally from hook_dllmain.c's DllMain via the public
 *      ultra_gamestate_probe_start(); now started from this file's on_install
 *      callback (chaosbreaker_on_install). board_io_ops.h documents on_install as
 *      being invoked from ultra_jvs_serial_install() -- that call site does not
 *      exist yet (see the "KNOWN GAP" note further down), so
 *      ultra_board_ops_autoregister() below also calls chaosbreaker_on_install()
 *      directly (idempotent) as a working path today, matching the same gap
 *      game_wackyrace.c's own header independently documents and works around.
 *
 *   2. hook_dllmain.c's xinput_thread ULTRA_DEMO_INPUT `if (demo) { ... }`
 *      branch -- a scripted coin-insert + START + attract/character-select/
 *      fight button sequence. Was inline in the shared generic gamepad-polling
 *      thread; now this file's autopilot_tick callback, called every tick by
 *      that same generic thread (see board_io_ops.h's autopilot_tick contract)
 *      once this file's ops are registered -- but ONLY registered with a non-
 *      NULL autopilot_tick when ULTRA_DEMO_INPUT=1, else a real player's
 *      gamepad/loader input keeps driving the game exactly as before (see the
 *      CRITICAL CORRECTNESS NOTE on ultra_board_ops_autoregister() below).
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
 *
 *  Env vars this file reads (all default-off; a plain loader run is unchanged):
 *    ULTRA_TYPEX_POKE / ULTRA_PROBE  -- start the probe thread (unchanged from
 *                                       gamestate_probe.c; gates the THREAD).
 *    ULTRA_POKE_KEEPRUN              -- opt-in extra poke bit (unchanged).
 *    ULTRA_JVS_LOG                   -- trace file path (unchanged).
 *    ULTRA_FORCE_CHAOSBREAKER        -- NEW (see ultra_board_ops_autoregister
 *                                       below): forces IDENTITY registration
 *                                       even if the AddressOfEntryPoint check
 *                                       fails. Deliberately a different flag
 *                                       from ULTRA_TYPEX_POKE/ULTRA_PROBE --
 *                                       see the comment on autoregister for why.
 * =============================================================================
 */
#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "board_io_ops.h"
#include "jvs_core.h"

/* ============================================================================
 *  PART 1 -- gamestate_probe.c, moved verbatim (probe/poke thread)
 * ============================================================================
 *  game.exe image base is 0x400000; these are the VAs from the disasm.
 */
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
 * only ever fires on the exact game it was built for. THIS IS THE LOAD-BEARING
 * SAFETY CHECK -- preserved byte-for-byte from gamestate_probe.c; do not weaken
 * it. It now ALSO gates ultra_board_ops_autoregister()'s registration itself
 * (see below), which is strictly TIGHTER than the original: previously only the
 * poke variable was gated by it and the thread would still start and read (not
 * write) Chaos-Breaker VAs against whatever exe had ULTRA_TYPEX_POKE/ULTRA_PROBE
 * set; now a non-Chaos-Breaker process can never get this ops struct registered
 * at all (short of the explicit ULTRA_FORCE_CHAOSBREAKER override below, which
 * still leaves this exact poke-gate in place as a second line of defense). */
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

/* ---- board_io_ops::on_install -------------------------------------------
 * Old ultra_gamestate_probe_start() renamed/re-homed as this file's on_install
 * callback. Preserves the EXACT original opt-in gate: without ULTRA_TYPEX_POKE
 * or ULTRA_PROBE set, this is a no-op, same as before the migration (a plain
 * loader run against the real Chaos Breaker binary is unchanged).
 *
 * KNOWN GAP, WORKED AROUND (see ultra_board_ops_autoregister() below): board_io_ops.h
 * documents on_install as being called "once from ultra_jvs_serial_install(),
 * AFTER the generic Real_* kernel32/COM pointers are resolved" -- but as of this
 * writing jvs_serial_hook.c's ultra_jvs_serial_install() does not actually call
 * ops->on_install() anywhere (grepped; no call site exists yet -- game_wackyrace.c's
 * header comment independently notes the exact same gap for its own on_install).
 * Left un-worked-around, this thread would NEVER start even with ULTRA_TYPEX_POKE=1,
 * because hook_dllmain.c's DllMain now skips the old ultra_gamestate_probe_start()
 * call whenever a board_io_ops is registered (`if (!ultra_board_ops_get())
 * ultra_gamestate_probe_start();`), and Chaos Breaker's ops register unconditionally
 * on AOE match -- that would be a real regression (ULTRA_TYPEX_POKE going from
 * "keeps the exit gates happy" to "silently does nothing"), not just a missing nice-
 * to-have, so ultra_board_ops_autoregister() below calls this function directly
 * once, right after registering, instead of waiting on the unwired seam. The
 * `.on_install` field stays set too, for contract-compliance and so this becomes a
 * harmless no-op duplicate call if/when jvs_serial_hook.c's wiring is added later
 * (the s_started guard makes repeat calls free). Proper fix is still that one-line
 * `if (ops && ops->on_install) ops->on_install();` call site inside
 * ultra_jvs_serial_install() -- a prerequisite before gamestate_probe.c can be
 * deleted outright; see the report for detail. */
static void chaosbreaker_on_install(void) {
    if (!getenv("ULTRA_TYPEX_POKE") && !getenv("ULTRA_PROBE")) return;
    /* Idempotent guard: board_io_ops.h's on_install contract does not promise
     * single-call (ultra_jvs_serial_install() is re-invoked every ~1s by
     * hook_dllmain.c's periodic re-patch worker, and we also self-call this once
     * from autoregister -- see above), so make sure we only ever spawn one probe
     * thread regardless of how many times we're called. */
    static LONG s_started = 0;
    if (InterlockedCompareExchange(&s_started, 1, 0) != 0) return;
    CreateThread(NULL, 0, probe_thread, NULL, 0, NULL);
}

/* ============================================================================
 *  PART 2 -- hook_dllmain.c's xinput_thread ULTRA_DEMO_INPUT branch, moved
 * ============================================================================
 *  Moved verbatim from the `if (demo) { ... }` block in xinput_thread. Timing
 *  (tick thresholds/phases) is bit-for-bit unchanged; only the OUTPUT calls
 *  changed, from writing hook_dllmain.c's g_xinput_in/g_local_coin statics
 *  directly to going through the generic ultra_board_ops_set_input() /
 *  ultra_board_ops_insert_coin() seam (board_io_ops.h), which hook_dllmain.c
 *  forwards to those same statics under the hood -- so the end effect on the
 *  JVS node is identical.
 *
 *  hook_dllmain.c's generic xinput_thread calls this once per ~8ms loop tick
 *  (after Sleep(8)) whenever a board_io_ops with a non-NULL autopilot_tick is
 *  registered, in place of running its own wr_auto/demo/gamepad branches for
 *  this process. Since autopilot_tick() takes no tick/frame argument, this
 *  file keeps its OWN static tick counter (s_tick) to reproduce the identical
 *  cadence the original inline code got from xinput_thread's shared `tick`.
 */
/* STALE-COMMENT FLAG (carried over verbatim from hook_dllmain.c's xinput_thread,
 * found there unchanged during this extraction -- NOT invented or dropped by
 * this move, per instructions): the paragraph immediately below describes a
 * DOWN x11 -> SELECT debug-menu-navigation script gated by an env var
 * ULTRA_DEMO_SELECT. NEITHER that navigation script NOR any
 * getenv("ULTRA_DEMO_SELECT") call exists anywhere in the code that follows,
 * in hook_dllmain.c's history as read for this extraction, or in this file.
 * The code actually present (the second and third comment paragraphs below,
 * and the real logic under them) is a DIFFERENT, later script: coin-insert +
 * START + attract -> character-select -> fight button sequence. This looks
 * like documentation left behind from an earlier version of the demo script
 * that was since replaced without updating/removing the first paragraph. Left
 * in place, flagged, rather than silently deleted or silently "fixed" by
 * inventing the missing env-var check. */
/* Chaos Breaker DEBUG MENU navigation via JVS Player-1 BYTE2 (RE-confirmed:
 * the menu reads the game's internal word @0x473726, fed from JVS node+0x185
 * = P1 byte2, where DOWN=bit4 0x10, UP=bit5 0x20, SELECT=bit2 0x04). byte2 is
 * the LOW byte of ultra_jvs_input.player[0]. Auto-repeat is bypassed in the
 * boot state, so drive DISCRETE one-frame edges. Sequence: 11x DOWN to reach
 * DEMO TEST, then SELECT, then idle (let the demo render) -- then repeat.
 * ULTRA_DEMO_SELECT=1 enables the SELECT press (off = just scroll, proves nav). */
/* Drive clean menu DOWN: byte1 bit4 (0x10) -> internal 0x01 = cursor DOWN.
 * One-frame edges (~32ms press, ~0.35s gap) so each press = one move. The
 * probe logs cursor=[menuObj+0xc] -> watch it increment. byte1 = HIGH byte. */
/* Auto-play: insert a coin, press START, then mash PUSH1 + move so the
 * game goes attract -> credit -> character select -> fight (gameplay).
 * Standard JVS byte1: START 0x80, UP 0x20, DOWN 0x10, LEFT 0x08, RIGHT 0x04,
 * PUSH1 0x02, PUSH2 0x01. Coin via ultra_board_ops_insert_coin(0) (was a
 * direct InterlockedIncrement(&g_local_coin) in hook_dllmain.c; now routed
 * through the generic forwarder, same net effect on the coin counter). */
static void chaosbreaker_autopilot_tick(void) {
    static unsigned s_tick = 0;
    unsigned t = s_tick;

    uint8_t b1 = 0;
    if (t == 240u || t == 245u) ultra_board_ops_insert_coin(0);   /* 2 coins ~2s */
    if (t >= 360u && t < 368u) b1 = 0x80;                               /* START ~3s */
    else if (t > 420u) {                                               /* then play */
        unsigned ph = (t / 45u) % 6u;
        if ((t % 45u) < 6u) {
            if      (ph == 0u) b1 = 0x02;   /* PUSH1 (confirm/attack) */
            else if (ph == 1u) b1 = 0x04;   /* RIGHT */
            else if (ph == 2u) b1 = 0x02;   /* PUSH1 */
            else if (ph == 3u) b1 = 0x08;   /* LEFT  */
            else if (ph == 4u) b1 = 0x01;   /* PUSH2 */
            else               b1 = 0x80;   /* START (advance) */
        }
    }
    ultra_jvs_input in; memset(&in, 0, sizeof in);
    in.player[0] = (uint16_t)(b1 << 8);
    for (int a = 0; a < JVS_MAX_ANALOG; ++a) in.analog[a] = 0x8000;
    in.screen_x = in.screen_y = 0x8000;
    ultra_board_ops_set_input(&in);

    s_tick++;
    /* NOTE: the generic xinput_thread owns Sleep(8) and g_stop; this callback
     * must not sleep or loop itself (board_io_ops.h's autopilot_tick contract). */
}

/* ============================================================================
 *  PART 3 -- auto-detection / registration
 * ============================================================================
 *  Overrides the weak no-op ultra_board_ops_autoregister() in board_io_ops.c.
 *  Called exactly once from hook_dllmain.c's DllMain, before
 *  ultra_jvs_serial_install(). Decides FOR ITSELF whether the injected process
 *  is Chaos Breaker:
 *
 *   1. PRIMARY: image_entry_rva(base) == AOE_CHAOS_BREAKER -- the exact same
 *      AddressOfEntryPoint check gamestate_probe.c's probe_thread() used to
 *      gate its poke (see the AOE_CHAOS_BREAKER comment above). Reused here to
 *      gate REGISTRATION ITSELF, which is strictly tighter than the pre-move
 *      behavior (see that comment for why).
 *
 *   2. ESCAPE HATCH: ULTRA_FORCE_CHAOSBREAKER=1 -- a NEW env var introduced by
 *      this file, deliberately NOT reusing ULTRA_TYPEX_POKE / ULTRA_PROBE.
 *      Those two remain, unchanged, the opt-in gates for whether the probe
 *      thread actually starts/pokes once this ops IS registered (see
 *      chaosbreaker_on_install()/probe_thread() above) -- they say nothing
 *      about WHICH game the process is. Testers have historically set
 *      ULTRA_PROBE=1 while running arbitrary Type X titles just to get a log
 *      trace; if autoregister treated that as proof-of-identity, this ops
 *      struct (and its Chaos-Breaker-specific autopilot_tick button script)
 *      could get registered against an unrelated game the first time someone
 *      set ULTRA_PROBE=1 on it -- reopening exactly the cross-contamination
 *      hole board_io_ops.h exists to close. ULTRA_FORCE_CHAOSBREAKER is kept
 *      separate and named for exactly what it does (force this identity),
 *      so a tester has to say so explicitly and unambiguously. Even when
 *      forced, probe_thread()'s own AOE check still gates the actual POKE
 *      (writes), so a wrong force can produce misleading log output but
 *      cannot corrupt an unrelated game's memory.
 *
 *  CRITICAL CORRECTNESS NOTE -- autopilot_tick is registered CONDITIONALLY,
 *  gated on ULTRA_DEMO_INPUT, exactly like the original inline branch was:
 *  the AOE identity check above is unconditionally true for every real launch
 *  of the real Chaos Breaker binary (identity, not opt-in), but the demo
 *  button-mashing script must NOT run for a normal session with a real
 *  player's gamepad plugged in. board_io_ops.h's contract is that once ANY
 *  autopilot_tick is registered, the generic xinput_thread calls it EVERY
 *  TICK "instead of its own gamepad-polling body" -- i.e. unconditionally,
 *  with no fallback to real-controller input. Registering
 *  chaosbreaker_autopilot_tick unconditionally (the naive reading of "gate
 *  identity on AOE, full stop") would therefore hijack a real player's input
 *  with the scripted demo sequence on EVERY real Chaos Breaker launch, which
 *  is a severe regression, not an equivalent extraction. So: this ops struct's
 *  autopilot_tick is chaosbreaker_autopilot_tick only when getenv("ULTRA_DEMO_INPUT")
 *  == "1" (the SAME env check hook_dllmain.c's xinput_thread used to decide
 *  `demo`); otherwise it is NULL, so xinput_thread falls through to its normal
 *  wr_auto/(demo=false)/real-gamepad body for this process, unaffected -- a
 *  real player's controller drives the game exactly as before. */
#define ULTRA_ENV_FORCE_CHAOSBREAKER "ULTRA_FORCE_CHAOSBREAKER"

/* Uniquely-named + constructor-attributed: runs automatically at DLL load,
 * before the CRT calls the exported DllMain, with no explicit call site
 * anywhere in the shared generic files. MUST be uniquely named (not the
 * generic "ultra_board_ops_autoregister" this file used before) because a
 * board's build can link MORE THAN ONE game_*.c file into the same binary
 * (the shared legacy hook DLL serves both this file's board, Taito Type X,
 * AND game_wackyrace.c's board, Type X2) -- a shared well-known symbol name
 * is a link-time collision the moment two game modules share a binary. See
 * board_io_ops.h's registration section. */
static void __attribute__((constructor)) game_chaosbreaker_autoregister(void) {
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    unsigned aoe = image_entry_rva(base);
    int match = (aoe == AOE_CHAOS_BREAKER);
    if (!match) {
        const char* f = getenv(ULTRA_ENV_FORCE_CHAOSBREAKER);
        if (f && f[0] == '1') {
            glog("CHAOSBREAKER: forced registration via %s (image entry RVA %08X != "
                 "expected %08X -- OVERRIDDEN; verify this really is Chaos Breaker, "
                 "the poke itself stays gated on the real AOE match regardless)",
                 ULTRA_ENV_FORCE_CHAOSBREAKER, aoe, AOE_CHAOS_BREAKER);
            match = 1;
        }
    }
    if (!match) return;   /* not Chaos Breaker: stay a no-op, same as the weak default */

    /* Two variants of the SAME ops identity, differing only in whether the
     * demo autopilot script is wired -- see the CRITICAL CORRECTNESS NOTE
     * above for why this cannot be a single unconditionally-populated struct. */
    static const board_io_ops s_ops_demo = {
        .name                       = "Chaos Breaker (Taito Type X)",
        .on_install                 = chaosbreaker_on_install,
        .on_create_file             = NULL,
        .on_read_file               = NULL,
        .on_write_file              = NULL,
        .on_close_handle            = NULL,
        .on_wait_comm_event_pending = NULL,
        .on_load_library_redirect   = NULL,
        .autopilot_tick             = chaosbreaker_autopilot_tick,
    };
    static const board_io_ops s_ops_no_demo = {
        .name                       = "Chaos Breaker (Taito Type X)",
        .on_install                 = chaosbreaker_on_install,
        .on_create_file             = NULL,
        .on_read_file               = NULL,
        .on_write_file              = NULL,
        .on_close_handle            = NULL,
        .on_wait_comm_event_pending = NULL,
        .on_load_library_redirect   = NULL,
        .autopilot_tick             = NULL,   /* real gamepad / loader input drives the game */
    };
    const char* dm = getenv("ULTRA_DEMO_INPUT");
    int demo = dm && dm[0] == '1';
    ultra_board_ops_register(demo ? &s_ops_demo : &s_ops_no_demo);

    /* Self-trigger on_install (see the KNOWN GAP note on chaosbreaker_on_install()
     * above): jvs_serial_hook.c does not yet call ops->on_install() anywhere, so
     * without this direct call the probe/poke thread would never start even with
     * ULTRA_TYPEX_POKE=1 set, for EVERY real Chaos Breaker session (a regression,
     * since gamestate_probe.c's unconditional DllMain call is now skipped whenever
     * this ops is registered). chaosbreaker_on_install() is idempotent (its own
     * env-var gate + an InterlockedCompareExchange spawn guard), so calling it here
     * is safe and harmless to also call again later from the documented seam, if/
     * when jvs_serial_hook.c is fixed to do so. Timing note: this runs earlier than
     * the original ultra_gamestate_probe_start() call did (that ran after
     * open_shared()/configure_node()/ultra_jvs_serial_install()/ultra_dongle_install()/
     * ultra_dinput8_install(); this runs at the very top of DllMain, before all of
     * those) -- harmless because probe_thread() itself Sleep(1500)s before touching
     * anything and never reads the JVS/COM engine those calls set up, only game.exe's
     * own globals via its own GetModuleHandleA(NULL) base. */
    chaosbreaker_on_install();
}

#endif /* _WIN32 */
