/* SPDX-License-Identifier: MIT
 * NOTE: independent namco_es3 board copy -- adapted from native_x86/src/hook/jvs_core.c (reference only; no build-time or runtime dependency on native_x86 remains).
 * =============================================================================
 *  jvs_core_es3.c  --  Ultra Loader clean-room JVS I/O engine (implementation)
 * =============================================================================
 *  Clean-room from the published JAMMA Video Standard. No third-party loader
 *  code. Models one JVS I/O node answering a host arcade game's command stream.
 *
 *  Wire format (one packet):
 *      E0 | NODE | NUM | DATA... | SUM
 *  - E0 (SYNC) is literal and never escaped; it only ever marks a packet start.
 *  - Every other field byte equal to E0 or D0 is escaped on the wire as
 *    D0 followed by (byte-1). NUM counts the *logical* (unescaped) bytes that
 *    follow it, including SUM. SUM = (NODE + NUM + sum(DATA)) & 0xFF.
 *  - Host address is 0x00; nodes take 0x01..0x1F; 0xFF is broadcast.
 * =============================================================================
 */
#include "jvs_core.h"

#define ULTRA_JVS_BOARD_ID "namco_es3"
#include "jvs_ident_db.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <windows.h>

#define JVS_RX_CAP   4096u

struct ultra_jvs_node {
    /* config */
    char     id[128];
    uint8_t  players, switches_pp, coin_slots, analog_ch, analog_bits, gpo_slots;
    uint8_t  has_screen_pos;
    uint8_t  analog_out_ch;           /* JVS function 0x13: analog OUTPUT.
                                       * A force-feedback steering board is
                                       * exactly the node that has this, and it
                                       * is the only thing in the feature list
                                       * that distinguishes it from a plain I/O
                                       * board. 0 = do not advertise it. */
    /* runtime */
    uint8_t  address;                 /* 0 = unassigned */
    uint32_t coin[JVS_MAX_COINS];     /* 14-bit counters the game reads/debits */
    ultra_jvs_input in;
    /* receive accumulator (raw wire bytes) */
    uint8_t  rx[JVS_RX_CAP];
    size_t   rx_len;
};

/* -------------------------------------------------------------------------- */
/*  Lifecycle / config                                                         */
/* -------------------------------------------------------------------------- */

static void set_default_id(ultra_jvs_node* n) {
    /* Generic, widely-accepted-format I/O identity. Branded as ours but in the
     * conventional "maker;board;version;date" shape games expect. Overridable. */
    /* WHICH I/O BOARD WE CLAIM TO BE.
     *
     * This is not cosmetic. Titles that match the IOIDENT (0x10) reply against
     * their own accept-table refuse to boot on a board they do not recognize,
     * and the self-branded identity that used to live here was on no game's
     * list anywhere -- Mario Kart Arcade GP DX stopped dead on it with
     * "E03-02  I/O board type is abnormal".
     *
     * ultra_jvs_ident_autoselect() reads the GAME'S OWN executable for the
     * identities it accepts and answers with one of those, falling back to the
     * catalog. See jvs_ident_db.h. ULTRA_JVS_ID overrides everything. */
    {
        const char *sel = ultra_jvs_ident_autoselect(ULTRA_JVS_BOARD_ID);
        if (sel && *sel) {
            size_t k = strlen(sel);
            if (k >= sizeof n->id) k = sizeof n->id - 1;
            memcpy(n->id, sel, k);
            n->id[k] = '\0';
            return;
        }
    }
}

ultra_jvs_node* jvs_create(void) {
    ultra_jvs_node* n = (ultra_jvs_node*)calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->players      = 2;
    n->switches_pp  = 13;
    n->coin_slots   = 2;
    n->analog_ch    = 8;
    n->analog_bits  = 16;
    n->gpo_slots    = 2;
    n->has_screen_pos = 0;
    /* Start every axis CENTERED, not at zero.
     *
     * calloc leaves analog[] at 0x0000, and this header's own convention (see
     * jvs_core.h) is that 0x0000 is an EXTREME -- full lock -- while 0x8000 is
     * center. So between jvs_create() and the hook's first jvs_set_input() the
     * node was telling the game that every wheel, pedal and stick was jammed
     * hard against its stop. A driving game that runs a steering check during
     * boot reads that state before any input has been published. */
    {
        int a;
        for (a = 0; a < JVS_MAX_ANALOG; ++a) n->in.analog[a] = 0x8000;
        n->in.screen_x = n->in.screen_y = 0x8000;
    }
    set_default_id(n);
    return n;
}

void jvs_destroy(ultra_jvs_node* n) { free(n); }

void jvs_set_id(ultra_jvs_node* n, const char* id) {
    if (!n) return;
    if (!id || !*id) { set_default_id(n); return; }
    size_t k = strlen(id);
    if (k >= sizeof(n->id)) k = sizeof(n->id) - 1;
    memcpy(n->id, id, k);
    n->id[k] = '\0';
}

void jvs_set_features(ultra_jvs_node* n,
                      uint8_t players, uint8_t switches_per_player,
                      uint8_t coin_slots, uint8_t analog_channels,
                      uint8_t analog_bits, uint8_t gpo_slots,
                      uint8_t has_screen_pos) {
    if (!n) return;
    if (players)             n->players     = players > JVS_MAX_PLAYERS ? JVS_MAX_PLAYERS : players;
    if (switches_per_player) n->switches_pp = switches_per_player;
    if (coin_slots)          n->coin_slots  = coin_slots > JVS_MAX_COINS ? JVS_MAX_COINS : coin_slots;
    if (analog_channels)     n->analog_ch   = analog_channels > JVS_MAX_ANALOG ? JVS_MAX_ANALOG : analog_channels;
    if (analog_bits)         n->analog_bits = analog_bits;
    if (gpo_slots)           n->gpo_slots   = gpo_slots;
    n->has_screen_pos = has_screen_pos ? 1 : 0;
}

void jvs_set_input(ultra_jvs_node* n, const ultra_jvs_input* in) {
    if (n && in) n->in = *in;
}
void jvs_get_input(const ultra_jvs_node* n, ultra_jvs_input* out) {
    if (n && out) *out = n->in; else if (out) memset(out, 0, sizeof(*out));
}

void jvs_insert_coin(ultra_jvs_node* n, int slot, int count) {
    if (!n || slot < 0 || slot >= JVS_MAX_COINS || count <= 0) return;
    n->coin[slot] += (uint32_t)count;
    if (n->coin[slot] > 0x3FFF) n->coin[slot] = 0x3FFF;   /* 14-bit field */
}

uint8_t  jvs_address(const ultra_jvs_node* n)            { return n ? n->address : 0; }
uint32_t jvs_coin_count(const ultra_jvs_node* n, int s)  {
    return (n && s >= 0 && s < JVS_MAX_COINS) ? n->coin[s] : 0;
}

/* -------------------------------------------------------------------------- */
/*  Wire helpers                                                                */
/* -------------------------------------------------------------------------- */

/* Read one logical byte from wire[*wp..len), unescaping. Returns 1 on success
 * (advances *wp), 0 if not enough wire bytes are present yet. */
static int rd(const uint8_t* w, size_t len, size_t* wp, uint8_t* out) {
    if (*wp >= len) return 0;
    uint8_t b = w[*wp];
    if (b == JVS_ESCAPE) {
        if (*wp + 1 >= len) return 0;             /* escape seen, payload pending */
        *out = (uint8_t)(w[*wp + 1] + 1);
        *wp += 2;
    } else {
        *out = b;
        *wp += 1;
    }
    return 1;
}

/* Append one logical byte to out[*op..cap), escaping E0/D0. *op always advances
 * by the number of wire bytes the byte occupies; the store is skipped past cap. */
static void wr(uint8_t* out, size_t* op, size_t cap, uint8_t b) {
    uint8_t seq[2]; size_t k = 0;
    if (b == JVS_SYNC || b == JVS_ESCAPE) { seq[k++] = JVS_ESCAPE; seq[k++] = (uint8_t)(b - 1); }
    else                                  { seq[k++] = b; }
    for (size_t j = 0; j < k; ++j) { if (*op < cap) out[*op] = seq[j]; (*op)++; }
}

/* -------------------------------------------------------------------------- */
/*  Command dispatch -- build the logical response data (status + reports)     */
/* -------------------------------------------------------------------------- */

/* ===========================================================================
 *  STR PCB (Mario Kart Arcade GP DX steering board) -- stream emulation
 * ===========================================================================
 *
 *  The STR board is not a JVS node and not a second COM port. It is a BYTE
 *  STREAM tunnelled through the Namco manufacturer command 0x70 on node 1:
 *
 *      70 16 <a> <b>        one-shot init      reply: report only
 *      70 18 50 4C ...      HOST -> BOARD      bytes the game sends us
 *      70 15 <seq> FF       poll, form A       reply: report + ONE byte, == 0
 *      70 15 <seq> 20       poll, form B       reply: report + count + bytes
 *
 *  Form B is the read channel. Whatever we return lands in a 256-entry ring at
 *  node+0x484 (writer 0x839AD0), which the board driver at 0x5ECxxx pops with
 *  read(buf,n) at 0x839B90. That driver is a 5-state machine dispatched from
 *  [edi+4] through the table at 0x5ECEC8; each state reads a fixed-size reply
 *  and gates on resp[4] == ASCII '1':
 *
 *      state 0  read 7      resp[4]=='1'        -> flag +0x117
 *      state 1  read 7      resp[4]=='1'        -> flag +0x118
 *      state 2  read 0x16   resp[4]=='1'        -> copies dwords from resp+5
 *      state 3  read 0x0A   resp[4]=='1' && resp[6]!=0 -> byte +0x115
 *      state 4  read 7      resp[4]!=0 && resp[4]!='0' -> flag +0x11B
 *
 *  So the board's replies are fixed-size records whose 5th byte is the status
 *  character. We answer each poll with the record the CURRENT state is waiting
 *  for, advancing when the game acknowledges by moving on.
 *
 *  WHY NOT JUST PATCH THE GATES: that was tried first and is a dead end, kept
 *  documented in namco_dongle_es3.c. Forcing a handler to succeed sets its state
 *  to 6, and the dispatcher does `cmp eax,4 / ja skip`, so 6 is TERMINAL -- the
 *  whole driver parks before the later states ever run and their flags stay
 *  clear. The handshake has to actually happen.
 *
 *  HONEST STATUS: the record CONTENTS beyond the status byte are not known.
 *  Everything here is shaped to satisfy the checks the driver actually makes;
 *  padding is zero. If a later screen needs real steering feedback this will
 *  need real values, but the boot check only tests the status byte.
 *  ULTRA_NAMCO_STR=0 disables the whole thing.
 * =========================================================================== */

#define STR_RING_MAX 64

typedef struct {
    uint8_t  buf[STR_RING_MAX];   /* bytes queued for the next form-B poll */
    int      len;
    int      step;                /* which handshake record to hand out next */
    int      enabled;
    int      inited;
    int      served;         /* driver state we last queued a record for */
} ultra_str_state;

static ultra_str_state g_str;

/* The game's load delta, published by str_diag() once it resolves it. The
 * record dispatcher below needs it to read the driver's own state. */
static uintptr_t g_img_delta = 0;
static int       g_img_delta_ok = 0;
static int str_rd(uintptr_t a, void* out, size_t n);

/* The DRIVER decides which check is running, not us. 0x5E9BF0 is
 *     fastcall(ecx = wanted state, edx = driver):
 *         if (driver->state == 6) { driver->state = ecx;
 *                                   driver->done[0x117 + ecx] = 0; return 1; }
 * so the done flags are 0x117+state and the checks do NOT run 0,1,2,3,4 in
 * order - row 0x16C asks for check 0 and then jumps straight to check 3.
 * Keying our replies off a local counter therefore answers the wrong record;
 * read driver->state instead. */
static int reader_state(void) {
    uint32_t objp = 0, st = 0;
    if (!g_img_delta_ok) return -1;
    if (!str_rd(0x00E423D4 + g_img_delta, &objp, 4) || !objp) return -1;
    if (!str_rd(objp + 4, &st, 4)) return -1;
    return (st <= 4) ? (int)st : -1;
}

static void str_init(void) {
    const char *e;
    if (g_str.inited) return;
    g_str.inited = 1;
    e = getenv("ULTRA_NAMCO_STR");
    g_str.enabled = !(e && e[0] == '0');
    g_str.step = 0;
    g_str.len  = 0;
    g_str.served = -1;
}

/* ===========================================================================
 *  STR DRIVER LIVE PROBE  (read-only)
 * ===========================================================================
 * Four rounds of inferring the STR board's state from the wire alone produced
 * three wrong diagnoses. The driver's own state is READABLE, so read it.
 *
 * Mario Kart Arcade GP DX is not packed (plain .text/.rdata/.data at a
 * 0x400000 base), so these are fixed addresses, verified by disassembly:
 *
 *   0x00E423D4  void*  -> the STR driver object
 *                          +0x004  int   state    (0..4 = a check, 6 = idle/done)
 *                          +0x117  byte  check-0 done flag
 *                          +0x118  byte  check-1 done flag
 *                          +0x11C  byte  phase    (0 = build command, 1 = parse reply)
 *                          +0x120  int   tick     (acts once per 30 ticks)
 *                          +0x088  byte[] response buffer
 *   0x00E45280  byte      GATE. 0x5ECF01 / 0x5ECF43: when this is zero the
 *                          builder skips its send AND the parser skips its
 *                          read, so the parser then tests a memset-zeroed
 *                          buffer against 0x31 and fails forever - with
 *                          nothing whatsoever appearing on the wire.
 *   0x00E458E1  byte      node index; node = 0xE460F0 + index * 0x638
 *                          +0x220 byte  a write is in flight
 *                          +0x221 byte  running read offset (the byte echoed
 *                                       back as "70 15 <offset> 20")
 *
 * The state machine dispatches with "cmp eax,4 / ja skip", so state 6 is not a
 * crash - it is this check REPORTING SUCCESS and parking. The init at 0x5ECDA0
 * sets state = 6, so the driver starts idle and does nothing at all until the
 * supervisor at 0x5EA6xx hands it a check to run. */
static int str_rd(uintptr_t a, void* out, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)a, &mbi, sizeof mbi) != sizeof mbi) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    if ((uintptr_t)mbi.BaseAddress + mbi.RegionSize < a + n) return 0;
    memcpy(out, (const void*)a, n);
    return 1;
}

/* ---------------------------------------------------------------------------
 *  RETURN TO GAME MENU  (clSequenceManager)
 * ---------------------------------------------------------------------------
 * MKDX runs its screens through a scene manager whose name the developers left
 * in the binary: clSequenceManager, from .../source/game/sequence/SequenceManager.hpp
 *
 *   instance ptr : 0x00E2FDD8   (stored at 0x0078E113, NULL until boot ends)
 *   +0x38  clSequenceBase* current
 *   +0x3C  int currentId        which screen we are on now
 *   +0x40  int nextId           THE REQUEST FIELD; -1 = idle
 *   +0x44  int prevId
 *
 * clSequenceManager::Update (0x00705450, vtable 0x0096CF74 slot +8) runs once
 * per frame off task group 2 (TASK_GROUP_SEQUENCE). If nextId != -1 it calls
 * ChangeSequence (0x00705E30) - Finalize + dtor on the old screen, then
 * CreateSequence (0x00705510) through a 59-entry jump table at 0x00705D3C -
 * and writes -1 back. The GAME ITSELF sets nextId by an inlined store in 75
 * places, so this variable IS its scene API; there is no helper to call.
 *
 * That makes "Return to Game Menu" one aligned 32-bit store. It is instant,
 * touches nothing else, and cannot destabilise the process - unlike rolling
 * back the whole address space, which is what home_state.h attempts for boards
 * with no known trigger (and which is still disabled; see that header).
 *
 * WHICH ID:
 *   0x35 GameOver  DEFAULT. Ungated, and the faithful "this credit is over"
 *                  path - the game walks itself back to attract afterwards.
 *   5    Title     direct, but GATED at 0x00705486-0x007054A5: if nextId is
 *                  5, 9 or 0x0A while [0x00E423F4] != 0 and the helper at
 *                  0x005ED870 returns non-zero, Update returns early WITHOUT
 *                  clearing nextId AND without ticking the current screen -
 *                  the game stalls until the gate clears. 0x00E423F4 sits
 *                  beside the card-reader globals (0xE423D4 driver, 0xE423E0
 *                  serial slots) and 0x5ED870 is inside the reader pump, so
 *                  the gate is almost certainly "a card transaction is live".
 *
 * Because of that stall the watchdog below is armed whichever id is used.
 * ULTRA_ES3_HOME_SEQ=<n> overrides the id (decimal). */
/* This TU has no loader_log(); everything here reports through ULTRA_JVS_LOG
 * like CARDRW / CARDVENDOR / STEERROW do. */
static void es3_home_log(const char *fmt, ...) {
    const char *lp = getenv("ULTRA_JVS_LOG");
    FILE *f;
    va_list ap;
    if (!lp || !*lp) return;
    f = fopen(lp, "a");
    if (!f) return;
    fputs("HOME: ", f);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc(10, f);
    fclose(f);
}

#define ES3_SEQ_TITLE     5
#define ES3_SEQ_GAMEDEMO  4   /* attract demo; falls through to Title */
#define ES3_SEQ_GAMEOVER  0x35

static int  g_home_pending = -1;   /* id we requested, -1 = nothing outstanding */
static int  g_home_wait    = 0;    /* frames since the request */
static int  g_home_retried = 0;    /* already fell back to GameOver? */
static int  g_title_hold   = 0;    /* frames left holding the idle Title */

/* ---------------------------------------------------------------------------
 *  CoCreateInstance DIAGNOSTIC  (why the game says OFFLINE)
 * ---------------------------------------------------------------------------
 * The game's AMCUS auth client constructor does:
 *
 *   0x004010BB  push CLSID{4603BB03-...} ... call ds:[0x008CB7E4]  ; CoCreateInstance
 *   0x004010C6  test eax,eax
 *   0x004010C8  jns  0x004010DD          ; ok -> created = 1
 *   0x004010CC  push 0x1A                ; else E05-11 and created = 0
 *
 * created==0 makes every later status call return daemon_error = -1, which
 * raises 0x1A again at 0x004597B4 and calls SetOfflineMode(TRUE,TRUE). So the
 * whole "offline" state hangs on this one HRESULT, and nothing in the game
 * logs it.
 *
 * Rather than patch code at 0x004010C6 (only 4 bytes before the next
 * instruction, so a 5-byte jmp would straddle it), we swap the IAT SLOT. The
 * import address is known from the disassembly - 0x008CB7E4 - so this is a
 * single pointer write, and every caller in the process is covered, not just
 * the AMCUS one.
 *
 * The HRESULT names the failure precisely:
 *   0x80004002 E_NOINTERFACE           proxy/stub not registered (IAuth cannot
 *                                      marshal across the process boundary)
 *   0x80040154 REGDB_E_CLASSNOTREG     CLSID/LocalServer32 missing
 *   0x80080005 CO_E_SERVER_EXEC_FAILURE AMAuthd launched and died
 *   0x800401F0 CO_E_NOTINITIALIZED     apartment not initialized
 *   0x00000000 S_OK                    online */
typedef HRESULT (WINAPI *cocreate_fn)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static cocreate_fn g_real_cocreate = NULL;
static int g_cocreate_logged = 0;

static const char *hr_name(HRESULT hr) {
    switch ((unsigned)hr) {
    case 0x00000000u: return "S_OK";
    case 0x00000001u: return "S_FALSE";
    case 0x80004002u: return "E_NOINTERFACE (proxy/stub NOT registered)";
    case 0x80040154u: return "REGDB_E_CLASSNOTREG (CLSID missing)";
    case 0x80080005u: return "CO_E_SERVER_EXEC_FAILURE (server died)";
    case 0x800401F0u: return "CO_E_NOTINITIALIZED (no CoInitialize)";
    case 0x80070005u: return "E_ACCESSDENIED";
    case 0x80040111u: return "CLASS_E_CLASSNOTAVAILABLE";
    default:          return "?";
    }
}

static void guid_str(const GUID *g, char *out, size_t cap) {
    if (!g) { _snprintf(out, cap, "(null)"); return; }
    _snprintf(out, cap,
              "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
              (unsigned long)g->Data1, g->Data2, g->Data3,
              g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
              g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static HRESULT WINAPI ultra_cocreate(REFCLSID rclsid, LPUNKNOWN outer,
                                     DWORD ctx, REFIID riid, LPVOID *ppv)
{
    HRESULT hr;
    if (!g_real_cocreate) return REGDB_E_CLASSNOTREG;
    hr = g_real_cocreate(rclsid, outer, ctx, riid, ppv);

    /* Only the first few, and only failures after the first success, so this
     * cannot flood a log the game writes thousands of lines to. */
    if (g_cocreate_logged < 8) {
        const char *lp = getenv("ULTRA_JVS_LOG");
        FILE *f = (lp && *lp) ? fopen(lp, "a") : NULL;
        if (f) {
            char c[64], i[64];
            guid_str((const GUID*)rclsid, c, sizeof c);
            guid_str((const GUID*)riid,   i, sizeof i);
            fprintf(f, "COCREATE: clsid=%s iid=%s ctx=%lu -> hr=%08lX %s\n",
                    c, i, (unsigned long)ctx, (unsigned long)hr, hr_name(hr));
            fclose(f);
            g_cocreate_logged++;
        }
    }
    return hr;
}

/* Swap the IAT slot the game calls through. 0x008CB7E4 is the CoCreateInstance
 * import, read straight off the call at 0x004010C0. */
static void cocreate_diag_install(uintptr_t delta) {
    uintptr_t slot = 0x008CB7E4 + delta;
    DWORD old;
    uint32_t cur = 0;

    if (!str_rd(slot, &cur, 4) || !cur) return;
    if ((cocreate_fn)(uintptr_t)cur == ultra_cocreate) return;   /* already ours */

    g_real_cocreate = (cocreate_fn)(uintptr_t)cur;
    if (!VirtualProtect((void*)slot, 4, PAGE_READWRITE, &old)) return;
    *(volatile uint32_t*)slot = (uint32_t)(uintptr_t)ultra_cocreate;
    VirtualProtect((void*)slot, 4, old, &old);
    es3_home_log("CoCreateInstance IAT hooked at %08X (real=%08X)",
                 (unsigned)slot, (unsigned)cur);
}

/* Current screen, or -1 if the sequence manager is not up yet. */
static int es3_sequence_id(void) {
    uint32_t mgr = 0, cur = 0;
    if (!g_img_delta_ok) return -1;
    if (!str_rd(0x00E2FDD8 + g_img_delta, &mgr, 4) || !mgr) return -1;
    if (!str_rd(mgr + 0x3C, &cur, 4)) return -1;
    return (int)cur;
}

/* Is the game sitting in its attract / title loop? Sequence ids 2..6 are the
 * attract set (Notice, Logo, GameDemo, Title, PlayDemo) - 0x0070E000 uses the
 * same range test. This is how the pause menu knows the game is already at its
 * own menu, with no user setup and nothing to capture. */
int ultra_es3_at_game_menu(void) {
    int id = es3_sequence_id();
    return (id >= 2 && id <= 6) || id == 0x36;
}

/* Is the game actually driving right now? Sequence 38 is Race.
 * Used to keep menu-only input mappings away from the pedals. */
int ultra_es3_in_race(void) {
    return es3_sequence_id() == 38;
}

/* ---------------------------------------------------------------------------
 *  RETURN TO TITLE on Pokken Tournament (ferrum_app.exe, 64-bit)
 * ---------------------------------------------------------------------------
 * The MKDX path above is MKDX's: its addresses are that binary's, and it is
 * gated on g_img_delta_ok, which only ever gets set by MKDX's own STR steering
 * diagnostic. On every other ES3 title it returns -1 immediately, which is why
 * "Return to Game Title" silently did nothing on anything but Mario Kart.
 *
 * Pokken has the same shape of mechanism, found by disassembly rather than
 * guessed. Its scene manager dispatches through a 19-entry jump table at
 * 0x140585C6C, and create_scene(mgr, id) at 0x140585520 takes the id in edx:
 *
 *   0x00 initialize  0x04 title            0x0B arcade      0x11 fatalerror
 *   0x01 notice      0x05 attract tutorial 0x0C testmode    0x12 systemcheck
 *   0x02 logo        0x06 demo             0x0D tutorial
 *   0x03 opening     0x0A modeselect       0x0F vs_lan
 *
 * And the manager already contains a trigger we can drive, in the fatal-error
 * path at 0x1405865BD:
 *
 *   cmp byte ptr [0x140E78218],0   ; the fatal latch
 *   je   skip                      ; clear -> carry on normally
 *   ...                            ; wait for the running scene to finish
 *   mov  edx,11h                   ; <- the scene it switches to
 *   mov  rcx,rbx                   ; the manager
 *   call create_scene
 *   mov  [rbx+0A8h],rax            ; becomes the current scene
 *
 * So one byte asks the manager to change scene, and the id it changes TO is a
 * hardcoded immediate we can repoint. Setting the latch after repointing it to
 * 0x04 gets us the title screen - done by the GAME, on the GAME's own thread,
 * through its own code path. That is the same bargain as MKDX's nextId store:
 * no calling into the engine from a foreign thread, nothing to destabilise.
 *
 * This machinery is free precisely because we disabled it: every +0x25==0
 * record is now cosmetic, so nothing else ever arms this latch.
 *
 *   ULTRA_ES3_PKN_SCENE=<n>   go to a different scene id (4 = title)
 */
#if defined(_WIN64)
#define PKN_SIG_RVA    0x5865FEu     /* "mov edx,11h"  = BA 11 00 00 00  */
#define PKN_IMM_RVA    0x5865FFu     /* its imm32                         */
#define PKN_LATCH_RVA  0xE78218u     /* errmgr+0x38                       */
#define PKN_SCENE_TITLE 0x04

static unsigned char *pkn_image(size_t *size)
{
    /* Read SizeOfImage out of the PE header rather than calling psapi - this TU
     * does not link it, and the header is right there in the mapped image. */
    unsigned char *b = (unsigned char *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    if (!b) return NULL;
    dos = (IMAGE_DOS_HEADER *)b;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    nt = (IMAGE_NT_HEADERS *)(b + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    if (size) *size = (size_t)nt->OptionalHeader.SizeOfImage;
    return b;
}

/* Is this the Pokken build we mapped? Checked by CONTENT, so a different build
 * is skipped rather than patched at a stale offset. */
static unsigned char *pkn_site(void)
{
    size_t sz = 0;
    unsigned char *b = pkn_image(&sz);
    if (!b || sz < PKN_SIG_RVA + 8) return NULL;
    /* accept the original 11h, or 04h if we already repointed it */
    if (b[PKN_SIG_RVA] != 0xBA) return NULL;
    if (b[PKN_SIG_RVA + 1] != 0x11 && b[PKN_SIG_RVA + 1] != PKN_SCENE_TITLE) return NULL;
    if (b[PKN_SIG_RVA + 2] || b[PKN_SIG_RVA + 3] || b[PKN_SIG_RVA + 4]) return NULL;
    return b;
}

int ultra_es3_pkn_available(void) { return pkn_site() != NULL; }

int ultra_es3_pkn_return_to_title(void)
{
    unsigned char *b = pkn_site();
    unsigned want = PKN_SCENE_TITLE;
    const char *e;
    DWORD old;

    if (!b) return -1;
    e = getenv("ULTRA_ES3_PKN_SCENE");
    if (e && e[0]) want = (unsigned)strtoul(e, NULL, 0);
    if (want > 0x12) want = PKN_SCENE_TITLE;

    if (b[PKN_IMM_RVA] != (unsigned char)want) {
        if (!VirtualProtect(b + PKN_IMM_RVA, 1, PAGE_EXECUTE_READWRITE, &old)) {
            es3_home_log("scene: VirtualProtect on the scene id failed (%lu)", GetLastError());
            return -1;
        }
        b[PKN_IMM_RVA] = (unsigned char)want;
        VirtualProtect(b + PKN_IMM_RVA, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), b + PKN_IMM_RVA, 1);
        es3_home_log("scene: scene-change target repointed to id %u", want);
    }
    /* Arm the latch. The manager picks it up on its next tick, waits for the
     * running scene to finish, and switches. */
    b[PKN_LATCH_RVA] = 1;
    es3_home_log("scene: requested scene %u (title) via the manager latch", want);
    return 0;
}

/* Withdraw the request once the switch has happened, so a scene that later
 * finishes on its own is not bounced straight back to the title. Called from
 * the frame tick; cheap enough to run every frame. */
void ultra_es3_pkn_tick(void)
{
    static int armed_frames;
    unsigned char *b = pkn_site();
    if (!b) return;
    if (b[PKN_LATCH_RVA]) {
        if (++armed_frames > 240) {          /* ~4s at 60fps: long enough for
                                              * the running scene to finish */
            b[PKN_LATCH_RVA] = 0;
            armed_frames = 0;
            es3_home_log("scene: scene latch withdrawn");
        }
    } else armed_frames = 0;
}
#else
int ultra_es3_pkn_available(void) { return 0; }
int ultra_es3_pkn_return_to_title(void) { return -1; }
void ultra_es3_pkn_tick(void) { }
#endif

int ultra_es3_home_available(void) {
    if (ultra_es3_pkn_available()) return 1;
    return es3_sequence_id() >= 0;   /* manager exists => we can drive it */
}

/* Ask the game to go back to its own menu. Returns 0 if the request was made. */
int ultra_es3_return_to_game_menu(void) {
    uint32_t mgr = 0;
    const char *e;
    int want, cur;

    /* Pokken first - its own manager, its own mechanism. The MKDX path below
     * is gated on g_img_delta_ok, which only MKDX's STR diagnostic ever sets,
     * so without this every non-MKDX title fell straight out at -1 and the
     * menu item did nothing. */
    if (ultra_es3_pkn_available()) return ultra_es3_pkn_return_to_title();

    if (!g_img_delta_ok) return -1;
    if (!str_rd(0x00E2FDD8 + g_img_delta, &mgr, 4) || !mgr) {
        es3_home_log("sequence manager not up yet -- ignoring");
        return -1;
    }
    cur = es3_sequence_id();
    if (ultra_es3_at_game_menu()) {
        es3_home_log("already at the game menu (sequence %d)", cur);
        return 0;
    }

    e = getenv("ULTRA_ES3_HOME_SEQ");
    /* TITLE by default. GameOver was the first choice because id 5 is gated
     * at 0x00705486 and a gated request stalls the sequence manager - but
     * the watchdog below now withdraws an unconsumed request and retries
     * with GameOver, so we can aim at the screen the operator actually
     * wants and fall back only if the gate is genuinely closed. */
    /* Enter the ATTRACT LOOP rather than jumping straight at Title.
     *
     * Sending 5 (clSequenceTitle) is accepted, but Title::Update
     * short-circuits at 0x007173EA: with credits or [this+0x46] already set it
     * takes the forced-next-id branch at 0x00717400 and leaves immediately -
     * landing on 45 (clSequenceTitleSelect), which IS a real menu. That is the
     * 'it goes to a game menu, not the title screen' the operator reported.
     *
     * The idle loop the game runs on its own is 5 -> 54 (Tips) -> 6 (PlayDemo)
     * -> 5, entered via 2 (Notice) -> 4 (GameDemo) -> 5. Entering at GameDemo
     * lets it fall through to a genuinely idle Title instead of bouncing.
     *
     * ULTRA_ES3_HOME_SEQ overrides: 5 Title, 2 Notice, 6 PlayDemo, 0x35
     * GameOver. */
    want = (e && e[0]) ? atoi(e) : ES3_SEQ_TITLE;

    *(volatile uint32_t*)(uintptr_t)(mgr + 0x40) = (uint32_t)want;
    {   const char *th = getenv("ULTRA_ES3_TITLE_HOLD");
        int frames = (th && th[0] && th[0] != '0') ? atoi(th) : 240;  /* ~4 s */
        g_title_hold = (want == ES3_SEQ_TITLE) ? frames : 0;
    }
    g_home_pending = want;
    g_home_wait    = 0;
    g_home_retried = 0;
    es3_home_log("sequence %d -> %d requested (mgr=%08X)", cur, want, mgr);
    return 0;
}

/* Watchdog. Update() clears nextId to -1 as soon as it acts on the request. If
 * our value is still sitting there it means Update took the gated early-return
 * path, which ALSO skips ticking the current screen - the game would look
 * frozen. Put nextId back to -1 so the game carries on, and say so. */
/* Log every scene transition.
 *
 * Return to Game Menu sends sequence 5 (clSequenceTitle) and the manager
 * accepts it, but that is not the screen the operator means by 'the title
 * screen'. Rather than guess a fourth id, record what the game does on its
 * own: walk to the screen you want and the log names its id.
 *
 * One line per CHANGE, so it costs nothing while a screen is up. */
/* Keep the idle Title screen ON SCREEN after Return to Game Menu.
 *
 * clSequenceTitle::Update (0x007173B0) bounces straight back off the title if
 * a transition is already pending on it:
 *
 *     0x007173EA  cmp BYTE [this+0x46], 0   ; clear -> normal update, stays
 *     0x00717400  mov eax, [this+0x48]      ; pending forced sequence id
 *     0x00717403  cmp eax, -1               ; -1 = nothing pending -> stays
 *     0x00717412  mov [mgr+0x40], eax       ; <<< jumps away, usually to 45
 *                                           ;     clSequenceTitleSelect - a MENU
 *     0x00717415  mov [this+0x48], -1       ; the game's OWN 'consumed' write
 *
 * That is why asking for Title landed on a game menu: a credit had already
 * scheduled the jump, and Title honors it on its first frame. Aiming at
 * GameDemo instead does not help - it falls through to Title and hits the same
 * check.
 *
 * So clear the pending id for a short while after the operator asks to go
 * home. -1 is the game's own sentinel for 'nothing pending', written by the
 * very next instruction at 0x00717415, so this is the game's mechanism rather
 * than a patch. Credits are NOT touched - the player keeps them.
 *
 * Time-boxed: only for ~4 s after the request, and only while the current
 * sequence IS Title. Outside that window the game schedules its own
 * transitions normally, so the natural attract rotation (5 -> 54 Tips -> 6
 * PlayDemo -> 5) is untouched and a credit works again immediately after.
 *
 * ULTRA_ES3_TITLE_HOLD=0 disables; =<frames> changes the window. */
static void es3_title_hold(void) {
    uint32_t mgr = 0, cur = 0, curid = 0, pend = 0;
    const char *e;

    if (g_title_hold <= 0) return;
    g_title_hold--;

    e = getenv("ULTRA_ES3_TITLE_HOLD");
    if (e && e[0] == '0') { g_title_hold = 0; return; }

    if (!str_rd(0x00E2FDD8 + g_img_delta, &mgr, 4) || !mgr) return;
    if (!str_rd(mgr + 0x3C, &curid, 4)) return;
    if ((int)curid != ES3_SEQ_TITLE) return;      /* only while Title is up */
    if (!str_rd(mgr + 0x38, &cur, 4) || !cur) return;
    if (!str_rd(cur + 0x48, &pend, 4)) return;
    if ((int)pend == -1) return;                  /* already nothing pending */

    *(volatile uint32_t*)(uintptr_t)(cur + 0x48) = 0xFFFFFFFFu;
    es3_home_log("held Title: cleared pending jump to sequence %d", (int)pend);
}

static void es3_scene_watch(void) {
    static int last = -999;
    int now = es3_sequence_id();
    if (now == last || now < 0) return;
    es3_home_log("SCENE CHANGE %d -> %d", last == -999 ? -1 : last, now);
    last = now;
}

static void es3_home_watchdog(void) {
    uint32_t mgr = 0, next = 0;

    if (g_home_pending < 0) return;
    if (!str_rd(0x00E2FDD8 + g_img_delta, &mgr, 4) || !mgr) { g_home_pending = -1; return; }
    if (!str_rd(mgr + 0x40, &next, 4)) { g_home_pending = -1; return; }

    if ((int)next != (int)(uint32_t)g_home_pending) {   /* consumed */
        g_home_pending = -1;
        return;
    }
    if (++g_home_wait < 120) return;     /* ~2 s at 60 Hz */

    /* Withdraw first so the manager is never left stalled. */
    *(volatile uint32_t*)(uintptr_t)(mgr + 0x40) = 0xFFFFFFFFu;

    if (g_home_pending == ES3_SEQ_TITLE && !g_home_retried) {
        /* Title was gated. GameOver is ungated and the game walks itself
         * back to attract from there, so it is the honest second choice. */
        g_home_retried = 1;
        g_home_wait    = 0;
        g_home_pending = ES3_SEQ_GAMEOVER;
        *(volatile uint32_t*)(uintptr_t)(mgr + 0x40) = (uint32_t)ES3_SEQ_GAMEOVER;
        es3_home_log("Title was gated at 0x705486 -- falling back to GameOver");
        return;
    }
    es3_home_log("sequence %d was not accepted in %d frames"
                 " -- request withdrawn so the game does not stall",
                 g_home_pending, g_home_wait);
    g_home_pending = -1;
    g_home_retried = 0;
}

/* ---------------------------------------------------------------------------
 *  CARD VENDOR SERIAL PORT  (the actual cause of error 0x46)
 * ---------------------------------------------------------------------------
 * Row 0x16C starts an ASYNCHRONOUS open of the card-vendor port (COM2, serial
 * slot 1) and marks it "in flight":
 *
 *     0x5EB373   mov dword [slot1.status], 0xFFFFFFFF
 *     0x5EB379   mov dword [slot1.state],  2
 *
 * Row 0x186 then polls 0x5E95B0 every frame waiting for the open to COMPLETE:
 *
 *     [[0xE423E0]+0x08] == 0    slot1.status  -> success
 *     [[0xE423E0]+0x0C] == 1    slot1.state   -> open
 *     0x83C7D0(1)       == 0    port 1 request queue drained
 *
 * On a real cabinet the serial driver finishes the open and writes those. We
 * claim COM2 in the hook but never drive this object, so the poll stays false,
 * row 0x186 counts to 0x384 (900) frames at 0x5EB5A2 and raises error 0x46.
 *
 * [0xE423E0] is a 2-entry array of {status, state} DWORD pairs, 8 bytes each:
 *   slot 0 = +0x00/+0x04  "COM4" (drive board), opened by row 0x138
 *   slot 1 = +0x08/+0x0C  "COM2" (card vendor), opened by row 0x16C
 * NOTE: this is NOT the steering object an older comment in this file called
 * it, and the 0xFC/0x100 offsets steer_trace_dump prints are slot 31 - i.e.
 * meaningless. Same mislabelling family as the 0xE423D4 "STR" name.
 *
 * We only ever act on the exact in-flight pattern, so a port that is healthy,
 * closed, or owned by real hardware is never touched. */
static void vendor_port_ready(uintptr_t delta) {
    static int fixes = 0;
    uint32_t arr = 0, status = 0, state = 0;

    if (!str_rd(0x00E423E0 + delta, &arr, 4) || !arr) return;

    /* [0x00E423E0] is a 2-entry array of {status, state} DWORD pairs:
     *   slot 0 = +0x00/+0x04  COM4, opened by row 0x138 (steering). errors 0x43/0x44
     *   slot 1 = +0x08/+0x0C  COM2, opened by row 0x16C, polled by row 0x186
     *                         (card vendor / unit 2). errors 0x45/0x46
     *
     * ONLY SLOT 1. Slot 0 belongs to the steering row, which already passes;
     * writing to it risks the E22-12 fix for no gain, since unit 1's OFF/OK
     * text is decided by +0xAD4 (which must stay 0), not by this slot.
     *
     * A row marks its open in flight as {status=0xFFFFFFFF, state=2} and waits
     * for the serial layer to report {status=0, state=1}. There is no real COM2
     * behind it, so we report the completion.
     *
     * RE-ASSERT EVERY TICK. Writing once was not enough - the game's own
     * failing open put the slot back, which the log caught red-handed:
     *   CARDVENDOR[2]: slot1 held open (was status=FFFFFED3 state=1)
     * Holding it is what actually cleared E07-22. */
    if (!str_rd(arr + 0x08, &status, 4)) return;
    if (!str_rd(arr + 0x0C, &state, 4))  return;
    if (status == 0 && state == 1) return;      /* already open */
    if (status == 0 && state == 0) return;      /* never opened - leave it */

    *(volatile uint32_t*)(uintptr_t)(arr + 0x08) = 0;   /* status = success */
    *(volatile uint32_t*)(uintptr_t)(arr + 0x0C) = 1;   /* state  = open    */

    if (fixes < 12) {
        const char *lp = getenv("ULTRA_JVS_LOG");
        FILE *f = (lp && *lp) ? fopen(lp, "a") : NULL;
        if (f) {
            fprintf(f, "CARDVENDOR[%d]: slot1 held open (was status=%08X state=%u)"
                       " arr=%08X\n", fixes, status, state, arr);
            fclose(f);
        }
        fixes++;
    }
}




/* ---------------------------------------------------------------------------
 *  STEERING ROW TRACE  (vtable trampoline)
 * ---------------------------------------------------------------------------
 * The boot-check rows are dispatched through a table of thiscall handlers in
 * WRITABLE .data at 0xA0D880:
 *
 *     [3] 0xA0D88C -> 0x5EAC90   row y=0x11E  camera
 *     [4] 0xA0D890 -> 0x5EAE00   row y=0x138  STEERING      <-- this one
 *     [5] 0xA0D894 -> 0x5EB0A0   row y=0x152  IC card R/W
 *
 * (The row ids are Y coordinates, 26px apart, which is how the handlers map
 * onto the on-screen list.)
 *
 * 0x5EAE00 keeps everything that matters in its "this":
 *     +0x4C  sub-state   2 = done, 3 = NG
 *     +0x70  timer       error once it passes 0x12C0 (4800 frames, 80s)
 *     +0x78  inner step  0 -> 1 -> ... ; step 0 and 1 both need the sound
 *                        object at [0xE416A0]+0x180 to be non-NULL, and BAIL
 *                        to the timer check when it is not - which would mean
 *                        the check body at 0x5EAFE4 is never reached at all,
 *                        and NOPing the verdict at 0x5EB014 can do nothing.
 *     +0xAD4/+0xAD8/+0xADC  the three config-derived gates
 *
 * "this" is only ever passed in ECX by the dispatcher, so capture it: point
 * the vtable slot at a 10-byte stub that stores ECX and jumps to the real
 * handler. Ten bytes, one write, no detour of the function itself. */
static volatile uint32_t g_h4_this = 0;
/* Row 0x16C (IC card vendor, handler 0x5EB260, vtable slot [6] at 0xA0D898).
 * Its step 0 should fire the moment the reader's done[0x117] is set, but the
 * step never advances even though every input it reads looks correct. So count
 * the calls: if this stays 0 the row is not being dispatched at all, and the
 * problem is upstream of the row rather than inside it. */
static volatile uint32_t g_h6_this  = 0;
static volatile uint32_t g_h6_calls = 0;

/* Per-call trace of row 0x16C. The counting stub proved the row IS dispatched
 * but only FIVE times, then never again while its timer keeps climbing - which
 * is the real anomaly, not step 0's arithmetic. Log what it sees on entry. */
static void __cdecl ultra_row6_log(uint32_t self) {
    static unsigned n = 0;
    uint32_t s54 = 0, s78 = 0, s70 = 0, drv = 0, dst = 0, ad8 = 0;
    uint8_t dn[5];
    const char* lp;
    FILE* f;
    if (n >= 24) return;
    ++n;
    memset(dn, 0xEE, sizeof dn);
    str_rd(self + 0x54, &s54, 4);
    str_rd(self + 0x78, &s78, 4);
    str_rd(self + 0x70, &s70, 4);
    str_rd(self + 0xAD8, &ad8, 4);
    if (g_img_delta_ok && str_rd(0x00E423D4 + g_img_delta, &drv, 4) && drv) {
        str_rd(drv + 0x004, &dst, 4);
        str_rd(drv + 0x117, dn, 5);
    }
    lp = getenv("ULTRA_JVS_LOG");
    if (!lp || !*lp) return;
    f = fopen(lp, "a");
    if (!f) return;
    fprintf(f, "ROW6[%u]: this=%08X [54]=%d step=%d timer=%d ad8=%d | "
               "drv state=%d done=%02X %02X %02X %02X %02X\n",
            n, self, (int)s54, (int)s78, (int)s70, (int)ad8,
            (int)dst, dn[0], dn[1], dn[2], dn[3], dn[4]);
    fclose(f);
}

static void steer_trace_install(uintptr_t delta) {
    static int done = 0;
    uint8_t* stub;
    uintptr_t slot, real;
    uint32_t rel, cur = 0;
    DWORD old = 0;
    const char* lp;
    FILE* f;
    const char* what;

    if (done) return;
    done = 1;

    /* slot [6] first: 16-byte stub = mov [g_h6_this],ecx ; inc [g_h6_calls] ; jmp */
    {
        uintptr_t s6 = 0x00A0D898 + delta, r6 = 0x005EB260 + delta;
        uint32_t c6 = 0; DWORD o6 = 0;
        if (str_rd(s6, &c6, 4) && c6 == (uint32_t)r6) {
            uint8_t* st6 = (uint8_t*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                                                  PAGE_EXECUTE_READWRITE);
            if (st6) {
                uint32_t a1 = (uint32_t)(uintptr_t)&g_h6_this;
                uint32_t a2 = (uint32_t)(uintptr_t)&g_h6_calls;
                uint32_t rl, rc;
                /* mov [g_h6_this],ecx ; inc [g_h6_calls] ; push ecx ; push ecx
                 * ; call ultra_row6_log ; add esp,4 ; pop ecx ; jmp real
                 * ECX is the thiscall "this" and MUST survive, hence the
                 * push/pop around the cdecl logger. */
                st6[0] = 0x89; st6[1] = 0x0D; memcpy(st6 + 2, &a1, 4);
                st6[6] = 0xFF; st6[7] = 0x05; memcpy(st6 + 8, &a2, 4);
                st6[12] = 0x51;                       /* push ecx (save) */
                st6[13] = 0x51;                       /* push ecx (arg)  */
                st6[14] = 0xE8;
                rc = (uint32_t)((uintptr_t)&ultra_row6_log - ((uintptr_t)st6 + 19));
                memcpy(st6 + 15, &rc, 4);
                st6[19] = 0x83; st6[20] = 0xC4; st6[21] = 0x04;  /* add esp,4 */
                st6[22] = 0x59;                       /* pop ecx */
                st6[23] = 0xE9;
                rl = (uint32_t)(r6 - ((uintptr_t)st6 + 28));
                memcpy(st6 + 24, &rl, 4);
                FlushInstructionCache(GetCurrentProcess(), st6, 28);
                if (VirtualProtect((LPVOID)s6, 4, PAGE_READWRITE, &o6)) {
                    *(uint32_t*)s6 = (uint32_t)(uintptr_t)st6;
                    VirtualProtect((LPVOID)s6, 4, o6, &o6);
                }
            }
        }
    }

    slot = 0x00A0D890 + delta;
    real = 0x005EAE00 + delta;
    if (!str_rd(slot, &cur, 4) || cur != (uint32_t)real) {
        what = "vtable slot did not hold the expected handler";
        stub = NULL;
    } else {
        stub = (uint8_t*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
        if (!stub) {
            what = "VirtualAlloc failed";
        } else {
            /* mov [&g_h4_this], ecx  ;  jmp real */
            stub[0] = 0x89; stub[1] = 0x0D;
            memcpy(stub + 2, &(uint32_t){0}, 4);
            {   uint32_t addr = (uint32_t)(uintptr_t)&g_h4_this;
                memcpy(stub + 2, &addr, 4); }
            stub[6] = 0xE9;
            rel = (uint32_t)(real - ((uintptr_t)stub + 11));
            memcpy(stub + 7, &rel, 4);
            FlushInstructionCache(GetCurrentProcess(), stub, 11);
            if (!VirtualProtect((LPVOID)slot, 4, PAGE_READWRITE, &old)) {
                what = "VirtualProtect on the vtable failed";
            } else {
                *(uint32_t*)slot = (uint32_t)(uintptr_t)stub;
                VirtualProtect((LPVOID)slot, 4, old, &old);
                what = "installed";
            }
        }
    }
    lp = getenv("ULTRA_JVS_LOG");
    if (!lp || !*lp) return;
    f = fopen(lp, "a");
    if (!f) return;
    fprintf(f, "STEERTRACE: slot=%08X had=%08X want=%08X stub=%08X : %s\n",
            (unsigned)slot, cur, (unsigned)real,
            (unsigned)(uintptr_t)stub, what);
    fclose(f);
}

/* ---------------------------------------------------------------------------
 *  DRIVE-BOARD "E00" FORCE
 * ---------------------------------------------------------------------------
 * MEASURED, not guessed. The live trace says the steering row sits at inner
 * step 0 forever with its timer climbing toward the 0x12C0 timeout:
 *
 *   STEERROW: substate[4C]=1 step[78]=0 timer[70]=6..704 ad4=1 ad8=1 adc=1
 *             sound[180]=013773A8
 *
 * Step 0 (0x5EAF48) advances only through this sequence:
 *
 *     mov  0x70(%edi),%eax          ; edi = [0xE416A0]+0x180
 *     call *WaitForSingleObject     ; take the object's mutex
 *     mov  0x4a(%edi),%cl           ; read the guarded flag
 *     call *ReleaseMutex
 *     cmpb $0x0,<flag>
 *     je   <bail - step NOT incremented>
 *     incl 0x78(%ebx)               ; step++
 *
 * (The two calls are WaitForSingleObject/ReleaseMutex, resolved from the IAT
 * at 0x8CB164 / 0x8CB19C - NOT sound calls, which is what they looked like
 * before the imports were checked.)
 *
 * That +0x4A flag is set in exactly one place, the drive-board line parser at
 * 0x5DB9F0:
 *
 *     cmpb $0x45,(%ebx)     ; 'E'
 *     cmp  $0x30,%al        ; '0'
 *     cmp  $0x30,%bl        ; '0'
 *     movb $0x1,0x4a(%edi)  ; the board answered "E00" = no fault
 *
 * with 'E02'..'E08' mapping to error codes 0x56..0x5B and "C01"/"C06" driving
 * +0x49. That is the Namco drive-board ASCII protocol, spoken over a serial
 * link this machine does not have. Rather than emulate the whole board just to
 * clear a boot gate, assert the one byte it would have produced.
 *
 * The object is heap-allocated and the write is a single aligned byte, so it
 * needs no VirtualProtect and cannot tear against the mutex holder. It is
 * re-asserted every poll because the owning subsystem can clear it.
 *
 * ULTRA_NAMCO_STEER_BYPASS=0 disables this (and the 0x5EB014 patch). */
static void steer_force_ready(uintptr_t delta) {
    static int logged = 0;
    uint32_t g = 0, obj = 0;
    uint8_t v = 0, v49 = 0;
    const char* e;
    const char* lp;
    FILE* f;

    e = getenv("ULTRA_NAMCO_STEER_BYPASS");
    if (e && e[0] == '0') return;

    if (!str_rd(0x00E416A0 + delta, &g, 4) || !g)  return;
    if (!str_rd(g + 0x180, &obj, 4) || !obj)       return;
    if (!str_rd(obj + 0x49, &v49, 1))              return;
    if (!str_rd(obj + 0x4A, &v, 1))                return;
    if (v49 && v) return;                           /* already C01 + E00 */

    /* BOTH flags, not just E00. Forcing only +0x4A moved the row from step 0
     * to step 2 and then stalled again, because the path at 0x5EAFB9 is:
     *
     *     cmpb $0x0,0x49(%eax)   ; the C01 status flag
     *     je   0x5EB069          ; zero -> fail, every frame
     *     cmpl $0x2,0x4c(%ebx)
     *     cmpl $0x0,0xad4(%ebx)
     *     jle  0x5EB019
     *     ...                    ; then the helper call, whose verdict at
     *                            ; 0x5EB014 we have already NOPed
     *
     * +0x49 comes from the same parser: "C01" sets it, "C06" clears it. So a
     * healthy board reports C01 (status) AND E00 (no fault); only the pair
     * gets through. Setting +0x49 additionally silences the timeout at
     * 0x5EAF98, which is the branch that raises 0x61 -> E22-12. */
    *(volatile uint8_t*)(uintptr_t)(obj + 0x49) = 1;   /* C01 */
    *(volatile uint8_t*)(uintptr_t)(obj + 0x4A) = 1;   /* E00 */

    if (logged) return;
    logged = 1;
    lp = getenv("ULTRA_JVS_LOG");
    if (!lp || !*lp) return;
    f = fopen(lp, "a");
    if (!f) return;
    fprintf(f, "STEERE00: drive-board object %08X +0x49=1 (C01) +0x4A=1 (E00)\n", obj);
    fclose(f);
}

/* ---------------------------------------------------------------------------
 *  IC CARD READER: DECLARE NONE FITTED
 * ---------------------------------------------------------------------------
 * With the steering row cleared the boot list advances to
 * "ICカードR/Wチェック" and stops on
 *     E07-11  ICカードR/W 未接続(ユニット1)
 * "IC card R/W (unit 1) is not connected".
 *
 * Its row handler is vtable slot [6], 0x5EB260, and its very first test is an
 * early-out that reports success with no hardware at all:
 *
 *     xor  %edi,%edi
 *     cmp  %edi,0xad8(%ebx)     ; the configured reader COUNT
 *     jne  <go look for one on COM2>
 *     mov  %eax,0x54(%ebx)      ; eax = 2 -> sub-state DONE
 *     movl $0x1,0x58(%ebx)
 *     mov  $0x1,%al ; ret       ; PASS
 *
 * The "go look" branch is 0x5EB355:
 *     push "COM2" / push $1 / call 0x83CE10   ; serial_open(slot 1, "COM2")
 *     cmp  $0x1,%eax / jne -> E07-11
 *
 * so a non-zero count means it demands a real card reader on COM2. The count
 * is not board-reported: 0x5EA0F1 loads it from config byte 0x1A of the blob
 * at [[0xE416A0]+0x188]+0x34, the same blob that supplies +0xAD4 (0x19) and
 * +0xADC (0x1B). Zero it at the source AND in the live object, because the
 * object field is refreshed from the blob.
 *
 * ALL THREE card rows are covered by TWO config bytes, from the full handler
 * map (vtable 0xA0D880, row id = Y coordinate, 26px apart):
 *
 *   [5] 0x5EB0A0  row 0x152  IC card R/W          gate +0xAD4 (cfg 0x19)
 *   [6] 0x5EB260  row 0x16C  IC card vendor       gate +0xAD8 (cfg 0x1A)
 *   [7] 0x5EB490  row 0x186  vendor IC card R/W   gate +0xAD8 (cfg 0x1A)
 *
 * +0xAD4 additionally short-circuits the STEERING row: 0x5EAFDB does
 * "cmpl $0x0,0xad4(%ebx) / jle 0x5EB019", and 0x5EB019 is inside the success
 * path, so a zero there reaches success without even calling the helper.
 *
 * +0xADC (cfg 0x1B) is deliberately LEFT ALONE: it is what sends the camera
 * row (0x5EACF3) and the steering row (0x5EAE67) down their working paths.
 * Zeroing it would turn the camera OFF, and that row already reports OK.
 *
 * The online rows need nothing - auth (0x5EB890), update (0x5EB9A0) and game
 * server (0x5EC380) each carry their own "オフライン" state.
 *
 * This is the honest configuration for this cabinet - there IS no card reader
 * attached - rather than faking reader responses. Card play is unavailable;
 * guest play is what the machine is.
 *
 * ULTRA_NAMCO_NO_CARDRW=0 keeps the reader check enabled. */
/* ---------------------------------------------------------------------------
 *  BANAPASSPORT CARD READER: ENABLE IT PROPERLY
 * ---------------------------------------------------------------------------
 * An earlier version of this declared NO reader fitted to get past
 * "E07-11 ICカードR/W 未接続(ユニット1)". That clears the boot screen but
 * throws away card play - Banapassport carries the player license, character
 * and kart unlocks, VS rating and ghost data. (Credits are unaffected; they
 * come over JVS.) So configure a reader that IS there instead.
 *
 * The boot rows are a 20-byte table at 0x9209xx, {label, index, x, y, -1},
 * paired with the handler vtable at 0xA0D880. The y field IS the row id:
 *
 *     0x104 I/O      0x11E camera    0x138 steering
 *     0x152 IC card R/W   0x16C IC card vendor   0x186 vendor IC card R/W
 *
 * ★ Row 0x152 (handler 0x5EB0A0) is what arms the driver at 0xE423D4. That
 *   makes 0xE423D4 the CARD READER, not the steering board - this file called
 *   it "STR" throughout on an early wrong guess, and the 5-state protocol it
 *   speaks (02 30 00 00 30 03 / 02 41 00 00 41 03 out, 7/7/22/10/7 back with
 *   resp[4]=='1') is the READER protocol, tunnelled over JVS 0x70, not a
 *   steering link. The emulation for it was already correct and the game was
 *   already consuming it; it simply never ran because nothing armed it.
 *
 * Arming needs BOTH, at 0x5EB0FB and 0x5EB119:
 *     cmp %edx,0xad4(%ebx) / jne <NG>    ->  +0xAD4 must be 0
 *     cmp %edx,0xad8(%ebx) / jle <skip>  ->  +0xAD8 must be > 0
 * and 0x5EB10F sets the row sub-state to DONE unconditionally, so the row
 * passes on its own once +0xAD4 is 0.
 *
 * Those come from config bytes at [[0xE416A0]+0x188]+0x34 (0x5EA0F1):
 *     0x19 -> +0xAD4    0x1A -> +0xAD8    0x1B -> +0xADC    0x1C -> +0xAE0
 * shipped as 1,1,1,1. +0xADC and +0xAE0 drive the steering and camera rows and
 * are LEFT ALONE - both already report OK.
 *
 * Caveat, honestly stated: the card VENDOR rows (0x16C, 0x186) share +0xAD8
 * with the reader and have no count of their own, so a non-zero value also
 * makes them look for a card dispenser on COM2. That is separate optional
 * hardware. ULTRA_NAMCO_CARDRW=0 reverts to "no reader fitted". */
/* TEMPORARY unconditional diagnostic (bring-up only): the real logging below
 * only ever fires once real values are in hand, so if this function is
 * bailing on one of its own early-return pointer-chain reads (0x00E416A0 ->
 * +0x188 -> +0x34 -> +0x19/+0x1A), we would otherwise get ZERO log output
 * ever, indistinguishable from "ran fine, nothing to change." This logs
 * every step, every call, no gating, so the next run says exactly where it
 * stops. Remove once IC card R/W is confirmed working. */
static void cre_trace(const char* fmt, ...) {
    /* GetEnvironmentVariableA, not getenv -- this can now run as early as
     * DllMain's very first action (called from ultra_es3_early_patches),
     * where getenv()'s CRT table may not be populated yet at all. */
    char lp[MAX_PATH];
    DWORD lpn = GetEnvironmentVariableA("ULTRA_JVS_LOG", lp, sizeof lp);
    if (!(lpn > 0 && lpn < sizeof lp && lp[0])) return;
    FILE* f = fopen(lp, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void card_reader_enable(uintptr_t delta) {
    static int logged = 0;
    static int calls = 0;
    uint32_t a1 = 0, a2 = 0, cfg = 0, self;
    uint8_t  c19 = 0xFF, c1a = 0xFF;
    uint32_t o_ad4 = 0xFFFFFFFFu, o_ad8 = 0xFFFFFFFFu;
    uint32_t want_ad4 = 0, want_ad8 = 0;
    uint32_t sub50 = 0;
    uint8_t  rp19a = 0;
    int changed = 0;
    const char* e;
    const char* lp;
    FILE* f;

    if (calls < 5) { calls++; cre_trace("CRE#%d: enter delta=%08X", calls, (unsigned)delta); }

    /* ULTRA_NAMCO_CARDRW:
     *   unset / "1"  ad4=0, ad8=1  A CARD READER IS FITTED. Runs the reader's
     *                              boot self-test so the driver at 0xE423D4
     *                              actually identifies itself - the thing
     *                              Banapassport needs. Also makes rows 0x16C /
     *                              0x186 look for a card DISPENSER on COM2,
     *                              which the hook now claims.
     *   "none"        ad4=0, ad8=0  reader present but self-test skipped and the
     *                              vendor rows early-out. The known-good boot,
     *                              kept as the fallback.
     *   "0"           touch nothing.
     *
     * ad8 is a BOOT-SCREEN value: on this object it is read only at 0x5EB268
     * and 0x5EB498 (rows 0x16C / 0x186) plus the row 0x152 arming test. It does
     * NOT disable the reader - 0x5EB103 sets the "reader present" flag at
     * [0xE416A0]+0x19A and 0x5EB10F marks the row DONE regardless of it. Only
     * the self-test is skipped. */
    e = getenv("ULTRA_NAMCO_CARDRW");
    if (e && e[0] == '0') return;
    /* DEFAULT IS ad8=0 -- "no card dispenser fitted". See ERROR 0x46 below.
     *
     * ── 2026-09-13: what ad8 ACTUALLY gates (two independent disassemblies) ──
     *
     * RETRACTED: an earlier revision of this comment claimed the boot runs a
     * FIVE-step reader self-test and that we only answered checks 0 and 3.
     * That was wrong, and it was wrong in a way worth recording, because the
     * log line that produced it LOOKS like proof:
     *
     *     done=01 00 00 01 00      <- checks 0 and 3 done, 1/2/4 blank
     *
     * Checks 1, 2 and 4 are blank because THE BOOT PATH NEVER ASKS FOR THEM.
     * done[] lives at [[0xE423D4]+0x117..0x11B] and the whole boot-test module
     * reads exactly two of those bytes: +0x117 at 0x5EB3F6 and +0x11A at
     * 0x5EB3BD, both inside row 0x16C. done[1]/[2]/[4] are consumed only by the
     * service/test menu (0x7B2102, 0x7B21D4, 0x7B22BE...). No call site in the
     * binary arms check 2 at all. Serving 0 and 3 IS the complete boot contract,
     * and str_queue_next() already does it correctly.
     *
     * ── The real meaning of ad8 ──
     * [self+0xAD8] is read by THREE rows, and it means something different to
     * each. That overload is the entire bug:
     *   row 0x152 (0x5EB0A0) -> "run the card R/W self-test"   (we satisfy this)
     *   row 0x16C (0x5EB260) -> "open the card VENDOR on COM2"
     *   row 0x186 (0x5EB490) -> "a card DISPENSER is fitted on COM2"
     *
     * With ad8=1 all three arm. Rows 0x152 and 0x16C pass ([+0x50]=2, [+0x54]=2).
     * Row 0x186 then polls the COM2 open that row 0x16C kicked off at 0x5EB380:
     *
     *     0x5EB4F4  cmp [ebx+0xAD8], 0   ; ad8==0 -> 0x5EB4FD instant PASS
     *     0x5E95B0  gate: [[0xE423E0]+0x08]==0 && [[0xE423E0]+0x0C]==1
     *     0x5EB5A2  cmp [ebx+0x70], 0x384 ; 900 frames
     *     0x5EB5D4  push 0x46
     *
     * Row 0x16C sets slot1.status=0xFFFFFFFF (0x5EB373) and slot1.state=2
     * (0x5EB379) to mean "open in flight". Nothing on our side ever completes
     * it, so the gate is false for 900 frames and 0x46 is raised. It is a SERIAL
     * outcome, not a done[] outcome -- the reader records were never the issue.
     *
     * vendor_port_ready() below completes that open, which is what lets ad8=1
     * work. Until that is proven on a real boot, ad8=0 stays the default: it
     * short-circuits row 0x186 at 0x5EB4FD and is the boot the game is PLAYABLE
     * on. Arming ad8 does not disable the reader either way -- 0x5EB103 still
     * sets the "reader present" flag at [0xE416A0]+0x19A and 0x5EB10F still
     * marks the row done.
     *
     * ULTRA_NAMCO_CARDRW=1 opts INTO the armed path (self-test + dispenser).
     * ULTRA_NAMCO_CARDRW=0 leaves the config untouched entirely. */
    /* MODES (ULTRA_NAMCO_CARDRW):
     *   unset / "0"  ad8=0  no reader fitted. Both IC rows short-circuit to
     *                       pass; the boot screen reads I/C R/W OFF. The
     *                       known-good, always-playable boot.
     *   "1"          ad8=1  reader armed. VERIFIED 2026-09-13: row 0x152 arms
     *                       the driver, row 0x16C passes, row 0x186 passes once
     *                       vendor_port_ready() holds COM2 open, E07-22 gone.
     *
     * +0xAD4 uses a two-phase update (see card_reader_enable() self block):
     *   Phase 1: stay 0 so row 0x152 arms the reader (0x5EB0FB requires ad4==0).
     *   Phase 2: once [self+0x50]==2 and reader_present confirmed, flip to 1 so
     *            the display branch at 0x5EB0B6 shows "OK" instead of "OFF".
     * The steering short-circuit at 0x5EAFDB is safe: rows run in y-order so
     * [+0x4C] is 2 before [+0x50] becomes 2, and 0x5EAFD1 bails before 0x5EAFDB. */
    want_ad8 = (e && e[0] == '0') ? 0u : 1u;   /* ARMED BY DEFAULT: E07-22 is fixed */

    if (!str_rd(0x00E416A0 + delta, &a1, 4) || !a1) {
        if (calls <= 5) cre_trace("CRE#%d: STOP at a1 (0x00E416A0+delta) -- read failed or NULL", calls);
        return;
    }
    if (!str_rd(a1 + 0x188, &a2, 4) || !a2) {
        if (calls <= 5) cre_trace("CRE#%d: STOP at a2 (a1+0x188=%08X) -- read failed or NULL", calls, a1);
        return;
    }
    if (!str_rd(a2 + 0x34, &cfg, 4) || !cfg) {
        if (calls <= 5) cre_trace("CRE#%d: STOP at cfg (a2+0x34=%08X) -- read failed or NULL", calls, a2);
        return;
    }
    if (!str_rd(cfg + 0x19, &c19, 1)) {
        if (calls <= 5) cre_trace("CRE#%d: STOP reading cfg+0x19 (cfg=%08X)", calls, cfg);
        return;
    }
    if (!str_rd(cfg + 0x1A, &c1a, 1)) {
        if (calls <= 5) cre_trace("CRE#%d: STOP reading cfg+0x1A (cfg=%08X)", calls, cfg);
        return;
    }
    if (calls <= 5) cre_trace("CRE#%d: got cfg=%08X c19=%u c1a=%u want_ad8=%u g_h4_this=%08X",
                               calls, cfg, c19, c1a, want_ad8, (unsigned)g_h4_this);

    if (c19 != 0) {
        *(volatile uint8_t*)(uintptr_t)(cfg + 0x19) = 0; changed = 1;
    }
    if (c1a != (uint8_t)want_ad8) {
        *(volatile uint8_t*)(uintptr_t)(cfg + 0x1A) = (uint8_t)want_ad8; changed = 1;
    }

    self = g_h4_this;
    if (self) {
        /* Two-phase ad4 update.
         *
         * ad4 must be 0 for row 0x152's init path (0x5EB0FB: cmp [ebx+0xad4],edx /
         * jne NG) to arm the reader and set [0xE416A0+delta+0x19A]=1. Once the
         * row is done ([self+0x50]==2) AND reader_present is confirmed (a1+0x19A==1),
         * we flip ad4 to 1. The display branch at 0x5EB0B6 then takes the "OK" path.
         *
         * The steering row (0x5EAE00) checks ad4 at 0x5EAFDB, but only when its own
         * sub-state [+0x4C] is NOT yet 2. Because rows run in y-order (0x138 before
         * 0x152), the steering row's [+0x4C] is already 2 by the time IC R/W
         * sets [+0x50]=2. Any subsequent steering dispatch sees [+0x4C]==2 at
         * 0x5EAFD1 and returns done before ever reaching 0x5EAFDB. Safe. */
        if (str_rd(self + 0x50, &sub50, 4) && sub50 == 2 &&
            a1 && str_rd(a1 + 0x19A, &rp19a, 1) && rp19a == 1) {
            want_ad4 = 1;
        }
        if (str_rd(self + 0xAD4, &o_ad4, 4) && o_ad4 != want_ad4) {
            *(volatile uint32_t*)(uintptr_t)(self + 0xAD4) = want_ad4; changed = 1;
        }
        /* The object copies the config ONCE at init (0x5EA0F1), so writing the
         * config bytes afterwards never reaches it - the live field has to be
         * set too. This is why an earlier ad8=0 attempt had no effect: it only
         * wrote config, and g_h4_this was still NULL at the time. */
        if (str_rd(self + 0xAD8, &o_ad8, 4) && o_ad8 != want_ad8) {
            *(volatile uint32_t*)(uintptr_t)(self + 0xAD8) = want_ad8; changed = 1;
        }
    }

    if (logged || !changed) return;
    logged = 1;
    lp = getenv("ULTRA_JVS_LOG");
    if (!lp || !*lp) return;
    f = fopen(lp, "a");
    if (!f) return;
    fprintf(f, "CARDRW: mode=%s cfg=%08X [0x19]=%u->0 [0x1A]=%u->%u ; obj=%08X "
               "[+0xAD4]=%u->%u [+0xAD8]=%u->%u\n",
            want_ad8 ? "arm-selftest" : "reader-present-no-selftest",
            cfg, c19, c1a, want_ad8, self, o_ad4, want_ad4, o_ad8, want_ad8);
    fclose(f);
}

/* ---------------------------------------------------------------------------
 *  CONTENT ROUTER: REPORT FOUND
 * ---------------------------------------------------------------------------
 * E05-01 "コンテンツルーター未接続" - the content router is the Namco ALL.Net
 * box this cabinet would talk to over the LAN. There isn't one, and there is
 * nothing to emulate on a machine that is not on that network.
 *
 * The check is a single byte. At 0x6E0F9D:
 *
 *     mov  0xA2BE04,%ecx
 *     cmpb $0x0,0x141(%ecx)    ; router found?
 *     sete %al / test %al,%al
 *     je   0x6E10D7            ; non-zero -> SUCCESS
 *     mov  0xE2FEB0,%eax / inc %eax / mov %eax,0xE2FEB0
 *     cmp  $0xA,%eax
 *     jge  0x6E10AD            ; ten tries -> push $0x14 -> E05-01
 *
 * and the success path at 0x6E10D7 just parks the retry counter at -180 and
 * re-arms, so a standing "found" is exactly what a present router looks like.
 *
 * The flag is only ever written to ZERO in the binary (0x6E3C66, 0x6E3CA4) -
 * it is set by a network reply that cannot arrive here - so assert it. Same
 * shape as the drive board's C01/E00 bytes: heap object, single aligned byte,
 * re-asserted each poll because the discovery code clears it.
 *
 * ULTRA_NAMCO_ROUTER=0 disables. */
/* Retarget the two E05-01 raises to the success path. TIMING-INDEPENDENT, and
 * that is the whole point: forcing the +0x141 flag is not enough because the
 * object at [0xA2BE04] does not exist until the network subsystem starts. In
 * the 2026-09-13 capture the force landed at log line 12064 of 24246 - roughly
 * 7,700 lines after the first JVS poll - by which time the check had already
 * spent its ten retries and REGISTERED the error. 0x5F1130 latches the id into
 * a list, so clearing the flag afterwards cannot un-raise it.
 *
 *   0x6E0F61  0f 84 50 01 00 00  je  0x6E10B7   ; [0xE423E4] NULL  -> raise
 *   0x6E0FC3  0f 8d e4 00 00 00  jge 0x6E10AD   ; 10 retries       -> raise
 *
 * Both keep their condition but now land on 0x6E10D7, the success path, which
 * parks the retry counter at -180 and re-arms - exactly what a present router
 * produces. Only the rel32 changes, so the instruction length is unchanged and
 * nothing downstream shifts.
 *
 * ULTRA_NAMCO_ROUTER=0 disables. */
static void router_patch(uintptr_t delta) {
    static int done = 0;
    static const struct { uint32_t va; uint8_t op[2]; uint32_t old_rel, new_rel; const char* what; }
    P[2] = {
        { 0x006E0F61, { 0x0F, 0x84 }, 0x00000150, 0x00000170, "je  null-object -> success" },
        { 0x006E0FC3, { 0x0F, 0x8D }, 0x000000E4, 0x0000010E, "jge retries-out -> success" },
    };
    int i;
    const char* e;
    const char* lp;
    FILE* f;

    if (done) return;
    done = 1;
    e = getenv("ULTRA_NAMCO_ROUTER");
    if (e && e[0] == '0') return;

    lp = getenv("ULTRA_JVS_LOG");
    f = (lp && *lp) ? fopen(lp, "a") : NULL;

    for (i = 0; i < 2; ++i) {
        uintptr_t at = P[i].va + delta;
        uint8_t cur[6];
        DWORD old = 0;
        const char* r;
        if (!str_rd(at, cur, 6)) r = "unreadable";
        else if (cur[0] != P[i].op[0] || cur[1] != P[i].op[1] ||
                 memcmp(cur + 2, &P[i].old_rel, 4) != 0) r = "bytes did not match, left alone";
        else if (!VirtualProtect((LPVOID)at, 6, PAGE_EXECUTE_READWRITE, &old))
            r = "VirtualProtect failed";
        else {
            memcpy((void*)(at + 2), &P[i].new_rel, 4);
            VirtualProtect((LPVOID)at, 6, old, &old);
            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)at, 6);
            r = "PATCHED";
        }
        if (f) fprintf(f, "ROUTERPATCH: %08X %s : %s\n", (unsigned)at, P[i].what, r);
    }
    if (f) fclose(f);
}

/* ===========================================================================
 *  ERROR TRACE  --  who raises what, exactly
 * ===========================================================================
 * Every on-screen "係員を呼んでください" error funnels through the registrar at
 * 0x5F1130, a stdcall taking ONE argument: the error id (0x14 = E05_01,
 * 0x34 = E07_11, 0x61..0x63 = the steering/card row timeouts, and so on - the
 * id indexes the 16-byte table at 0x915D60+, {id, name, msgA, msgB}).
 *
 * Static analysis says only 0x6E10C1 can push 0x14, and both branches into it
 * are patched - yet E05-01 still appears. Rather than guess a fourth time,
 * INSTRUMENT it: a 6-byte inline hook on the registrar's prologue that records
 * the id AND the return address of every single call. The return address,
 * rebased by the load delta, names the exact instruction that raised it.
 *
 * The prologue is relocated verbatim into the stub, so the function still does
 * exactly what it did:
 *     55        push ebp
 *     8b ec     mov  ebp,esp
 *     83 e4 f8  and  esp,-8      <- 6 bytes, no relative operands, safe to move
 *
 * pushad/pushfd around the logger keeps every register and flag intact; the
 * logger is cdecl and its two arguments are cleaned up by the stub. At entry
 * [esp]=return address and [esp+4]=the id, which after pushad(32)+pushfd(4)
 * sit at [esp+0x24] and [esp+0x28].
 *
 * ULTRA_NAMCO_ERRTRACE=0 disables. */
/* Dump the registrar's error LIST. 0x5F1130 dedups against five dwords at
 * [*(void**)0xA2BC2C] + 0x3C:
 *     xor %eax,%eax / lea 0x3c(%edi),%ecx
 *     cmp %ebx,(%ecx) / je <already present>
 *     inc %eax / add $4,%ecx / cmp $5,%eax / jl <loop>
 * The trace proves E07-11 is never RAISED through the registrar, so either the
 * list holds an id nothing logged - meaning a second writer - or the red box is
 * driven by something other than this list entirely. Print it and find out. */
static void err_list_dump(uintptr_t delta) {
    static uint32_t last[5] = { 0xEEEEEEEEu, 0, 0, 0, 0 };
    uint32_t head = 0, e[5];
    int i, changed = 0;
    const char* lp;
    FILE* f;
    if (!str_rd(0x00A2BC2C + delta, &head, 4) || !head) return;
    if (!str_rd(head, &head, 4) || !head) return;        /* one deref */
    for (i = 0; i < 5; ++i) e[i] = 0;
    if (!str_rd(head + 0x3C, e, 20)) return;
    for (i = 0; i < 5; ++i) if (e[i] != last[i]) changed = 1;
    if (!changed) return;
    for (i = 0; i < 5; ++i) last[i] = e[i];
    lp = getenv("ULTRA_JVS_LOG");
    if (!lp || !*lp) return;
    f = fopen(lp, "a");
    if (!f) return;
    fprintf(f, "ERRLIST: obj=%08X [%08X %08X %08X %08X %08X]\n",
            head, e[0], e[1], e[2], e[3], e[4]);
    fclose(f);
}

static volatile uint8_t g_err_sup = 0;   /* stub<->logger handoff */

/* Returns NON-ZERO to make the registrar a no-op for this raise.
 *
 * NEUTRALIZING THE NETWORK SERVICES WHOLESALE. Patching raise sites one at a
 * time worked but cost a run each: 0x1A (AMCUS COM), then 0x43, then 0x10 from
 * two more sites - and the content-router service has more of them. Since we
 * already own the registrar, filter there instead: nothing in the offline
 * network/auth stack can raise a boot-blocking error again, including sites
 * never seen.
 *
 * Filtered by CALLER RANGE, not by id, because ids are reused across
 * subsystems and the on-screen code is not the struct name (id 0x1A displayed
 * as E07-11, id 0x10 as E05-01). The range is the address the raise RETURNS to,
 * rebased to the static image, so it is stable under ASLR:
 *
 *     0x006E0000..0x006E2000   content-router / ALL.Net service
 *     0x00400000..0x00402000   AMCUS COM activation (0x4010CC)
 *     0x005E9000..0x005E9A00   the null-interface raise at 0x5E96D9
 *
 * Everything outside those ranges still raises normally - a genuine I/O or
 * hardware fault must still stop the machine, and we still want to SEE it.
 *
 * ULTRA_NAMCO_ERRFILTER=0 keeps the trace but stops the filtering. */
static int __cdecl ultra_err_log(uint32_t id, uint32_t ret) {
    static unsigned n = 0;
    static int filt = -1;
    uint32_t sret = ret - (uint32_t)g_img_delta;
    int sup = 0;
    const char* lp;
    FILE* f;

    if (filt < 0) {
        const char* e = getenv("ULTRA_NAMCO_ERRFILTER");
        filt = !(e && e[0] == '0');
    }
    if (filt) {
        if (sret >= 0x006E0000 && sret < 0x006E2000) sup = 1;   /* router   */
        else if (sret >= 0x00400000 && sret < 0x00402000) sup = 1; /* AMCUS  */
        else if (sret >= 0x005E9000 && sret < 0x005E9A00) sup = 1; /* iface  */
    }

    if (n < 64) {
        ++n;
        lp = getenv("ULTRA_JVS_LOG");
        if (lp && *lp && (f = fopen(lp, "a")) != NULL) {
            fprintf(f, "ERRRAISE[%u]: id=0x%02X from %08X (static %08X) : %s\n",
                    n, id, ret, sret, sup ? "SUPPRESSED (offline service)" : "raised");
            fclose(f);
        }
    }
    return sup;
}

static void err_trace_install(uintptr_t delta) {
    static int done = 0;
    static const uint8_t PRO[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 };
    uintptr_t at = 0x005F1130 + delta;
    uint8_t cur[6], *stub;
    DWORD old = 0;
    uint32_t rel;
    const char* e;
    const char* lp;
    FILE* f;
    const char* what;

    if (done) return;
    done = 1;
    e = getenv("ULTRA_NAMCO_ERRTRACE");
    if (e && e[0] == '0') return;

    if (!str_rd(at, cur, 6))                    what = "unreadable";
    else if (memcmp(cur, PRO, 6) != 0)          what = "prologue did not match, left alone";
    else {
        stub = (uint8_t*)VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
        if (!stub) what = "VirtualAlloc failed";
        else {
            int k = 0;
            stub[k++] = 0x60;                                     /* pushad        */
            stub[k++] = 0x9C;                                     /* pushfd        */
            stub[k++] = 0x8B; stub[k++] = 0x44; stub[k++] = 0x24; stub[k++] = 0x24; /* mov eax,[esp+24h] ret */
            stub[k++] = 0x8B; stub[k++] = 0x4C; stub[k++] = 0x24; stub[k++] = 0x28; /* mov ecx,[esp+28h] id  */
            stub[k++] = 0x50;                                     /* push eax  (ret)*/
            stub[k++] = 0x51;                                     /* push ecx  (id) */
            stub[k++] = 0xE8;                                     /* call logger    */
            rel = (uint32_t)((uintptr_t)&ultra_err_log - ((uintptr_t)stub + k + 4));
            memcpy(stub + k, &rel, 4); k += 4;
            stub[k++] = 0x83; stub[k++] = 0xC4; stub[k++] = 0x08; /* add esp,8      */
            {   /* stash AL where popad cannot clobber it */
                uint32_t sa = (uint32_t)(uintptr_t)&g_err_sup;
                stub[k++] = 0xA2; memcpy(stub + k, &sa, 4); k += 4;   /* mov [g],al */
                stub[k++] = 0x9D;                                 /* popfd          */
                stub[k++] = 0x61;                                 /* popad          */
                stub[k++] = 0x80; stub[k++] = 0x3D;               /* cmp byte [g],0 */
                memcpy(stub + k, &sa, 4); k += 4;
                stub[k++] = 0x00;
            }
            stub[k++] = 0x74; stub[k++] = 0x05;                   /* je  +5 -> run  */
            stub[k++] = 0x33; stub[k++] = 0xC0;                   /* xor eax,eax    */
            stub[k++] = 0xC2; stub[k++] = 0x04; stub[k++] = 0x00; /* ret 4 (stdcall)*/
            memcpy(stub + k, PRO, 6); k += 6;                     /* relocated      */
            stub[k++] = 0xE9;                                     /* jmp back       */
            rel = (uint32_t)((at + 6) - ((uintptr_t)stub + k + 4));
            memcpy(stub + k, &rel, 4); k += 4;
            FlushInstructionCache(GetCurrentProcess(), stub, k);

            if (!VirtualProtect((LPVOID)at, 6, PAGE_EXECUTE_READWRITE, &old))
                what = "VirtualProtect failed";
            else {
                uint32_t jrel = (uint32_t)((uintptr_t)stub - (at + 5));
                ((uint8_t*)at)[0] = 0xE9;
                memcpy((void*)(at + 1), &jrel, 4);
                ((uint8_t*)at)[5] = 0x90;                          /* pad to 6      */
                VirtualProtect((LPVOID)at, 6, old, &old);
                FlushInstructionCache(GetCurrentProcess(), (LPCVOID)at, 6);
                what = "INSTALLED";
            }
        }
    }
    lp = getenv("ULTRA_JVS_LOG");
    if (!lp || !*lp) return;
    f = fopen(lp, "a");
    if (!f) return;
    fprintf(f, "ERRTRACE: registrar %08X : %s\n", (unsigned)at, what);
    fclose(f);
}

/* ---------------------------------------------------------------------------
 *  SUPPRESS THE NETWORK / AUTH ERROR RAISES
 * ---------------------------------------------------------------------------
 * Found by tracing the registrar rather than by reading the disassembly - the
 * static guesses were wrong three times. The live trace showed the error list
 * holds exactly two ids and both come through 0x5F1130:
 *
 *     ERRLIST: [0x1A, 0x43, 0x6F, 0x6F, 0x6F]     (0x6F = empty slot)
 *     ERRRAISE: id=0x1A from 004010D3
 *     ERRRAISE: id=0x43 from 005E96E0
 *
 * id 0x1A, at 0x4010A2, is a COM activation:
 *     CoInitialize(NULL)
 *     CoCreateInstance({4603BB03-058D-43D9-B96F-639BE908C1ED}, NULL,
 *                      CLSCTX_LOCAL_SERVER,
 *                      {045A5150-D2B3-4590-A38B-C1158678E1AC}, &p)
 *     jns success ; push $0x1A ; call 0x5F1130
 * That CLSID belongs to AMCUS\AMAuthd.exe - Namco's authentication daemon,
 * an OUT-OF-PROCESS COM server. It is not registered on this machine, so the
 * activation fails. Nothing about the arcade I/O could ever have fixed this.
 *
 * id 0x43, at 0x5E96CA, is the same shape: an interface pointer at
 * [esi+edx*8+4] is NULL, so it raises.
 *
 * SUPPRESS THE RAISE, DO NOT FAKE SUCCESS. The success path at 0x4010DD sets
 * the "available" byte at (%esi) to 1; forcing that while the interface
 * pointer is NULL invites a crash the first time the game calls through it.
 * NOPing just the two "push id / call registrar" pairs leaves the state
 * honestly reporting "not available" - which is what an offline cabinet is -
 * while keeping the red box off the screen.
 *
 * ULTRA_NAMCO_NOAUTHERR=0 disables. */
static void err_suppress(uintptr_t delta) {
    static int done = 0;
    static const struct { uint32_t va; uint8_t b[7]; const char* what; } P[2] = {
        { 0x004010CC, { 0x6A, 0x1A, 0xE8, 0x5D, 0x00, 0x1F, 0x00 }, "id 0x1A  AMCUS CoCreateInstance failed" },
        { 0x005E96D9, { 0x6A, 0x43, 0xE8, 0x50, 0x7A, 0x00, 0x00 }, "id 0x43  null interface pointer" },
    };
    /* id 0x10 - the router "link up" gate, found by the registrar trace once
     * 0x1A and 0x43 were out of the way. Two sites, identical shape:
     *     cmpl $0x0,0xe2fdec        ; must be non-zero
     *     mov  0xe2fdd8,%eax        ; must be non-zero
     *     cmpl $0x0,0x3c(%eax)      ; ZERO here falls through to the raise
     *     push $0x10 / call 0x5F1130
     * Make the last branch unconditional so it always skips. Branches rather
     * than the flag itself, because [0xE2FDD8] is built late and a flag force
     * loses the race - exactly how the earlier +0x141 attempt failed. Only the
     * condition changes; the target and the instruction length do not. */
    static const struct { uint32_t va; int n; uint8_t old_b[6]; uint8_t new_b[6]; const char* what; } B[2] = {
        { 0x006E1200, 2, { 0x75, 0x0F }, { 0xEB, 0x0F },
          "id 0x10 site A: jne -> jmp (always skip)" },
        { 0x006E1861, 6, { 0x0F, 0x85, 0x16, 0x01, 0x00, 0x00 },
                         { 0xE9, 0x17, 0x01, 0x00, 0x00, 0x90 },
          "id 0x10 site B: jne -> jmp (always skip)" },
    };
    int i;
    const char* e;
    const char* lp;
    FILE* f;

    if (done) return;
    done = 1;
    e = getenv("ULTRA_NAMCO_NOAUTHERR");
    if (e && e[0] == '0') return;

    lp = getenv("ULTRA_JVS_LOG");
    f = (lp && *lp) ? fopen(lp, "a") : NULL;

    for (i = 0; i < 2; ++i) {
        uintptr_t at = P[i].va + delta;
        uint8_t cur[7];
        DWORD old = 0;
        const char* r;
        if (!str_rd(at, cur, 7))                     r = "unreadable";
        else if (memcmp(cur, P[i].b, 7) != 0)        r = "bytes did not match, left alone";
        else if (!VirtualProtect((LPVOID)at, 7, PAGE_EXECUTE_READWRITE, &old))
                                                     r = "VirtualProtect failed";
        else {
            memset((void*)at, 0x90, 7);
            VirtualProtect((LPVOID)at, 7, old, &old);
            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)at, 7);
            r = "SUPPRESSED";
        }
        if (f) fprintf(f, "ERRSUPPRESS: %08X %s : %s\n", (unsigned)at, P[i].what, r);
    }
    for (i = 0; i < 2; ++i) {
        uintptr_t at = B[i].va + delta;
        uint8_t cur[6];
        DWORD old = 0;
        const char* r;
        if (!str_rd(at, cur, (size_t)B[i].n))            r = "unreadable";
        else if (memcmp(cur, B[i].old_b, (size_t)B[i].n) != 0)
                                                         r = "bytes did not match, left alone";
        else if (!VirtualProtect((LPVOID)at, (SIZE_T)B[i].n, PAGE_EXECUTE_READWRITE, &old))
                                                         r = "VirtualProtect failed";
        else {
            memcpy((void*)at, B[i].new_b, (size_t)B[i].n);
            VirtualProtect((LPVOID)at, (SIZE_T)B[i].n, old, &old);
            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)at, (SIZE_T)B[i].n);
            r = "PATCHED";
        }
        if (f) fprintf(f, "ERRSUPPRESS: %08X %s : %s\n", (unsigned)at, B[i].what, r);
    }
    if (f) fclose(f);
}

static void steer_bypass(uintptr_t delta);   /* defined below */

/* Applied from DllMain, BEFORE the game's first thread runs.
 *
 * Timing is the whole problem with this one. The content-router check is driven
 * from the frame loop and its NULL-object branch fires within the first frames,
 * long before our first JVS poll (log line ~4310 in the 2026-09-13 captures).
 * 0x5F1130 LATCHES the error id into a list, so anything applied later - the
 * +0x141 flag force, or the branch patch when it lived on the poll path - is
 * already too late to stop the red box. The hook is injected into a SUSPENDED
 * process and DllMain runs before the entry point, so patch here. */
void ultra_es3_early_patches(void) {
    HMODULE h = GetModuleHandleA(NULL);
    uintptr_t delta;

    /* ULTRA_ES3_ERRPATCH arms the legacy error/router/steering patch set.
     *
     * This used to test the running executable's FILE NAME against one title.
     * A board hook has no business knowing which games run on it - the game
     * that needs these patches says so in its own container instead, which also
     * means every container on this board embeds an identical module. */
    {
        /* This function is called as the VERY FIRST action inside
         * DLL_PROCESS_ATTACH -- earlier than anything else in this DLL's
         * lifecycle, including the pausemenu/JVS setup below. Confirmed by
         * direct evidence, not theory: with ULTRA_ES3_ERRPATCH=1 and
         * ULTRA_JVS_LOG correctly published (both visible in the loader's
         * own log), literally nothing downstream of this gate ever produced
         * a single trace line, including unconditional ones added purely to
         * prove whether this function was even entered. That is a stricter
         * variant of the documented getenv()-CRT-snapshot trap already fixed
         * elsewhere in this file: this early in DLL_PROCESS_ATTACH, the
         * CRT's own getenv() table may not be populated at all yet, not just
         * stale. GetEnvironmentVariableA reads the real PEB environment
         * block directly, with no CRT initialization dependency. */
        char eb[8];
        DWORD en = GetEnvironmentVariableA("ULTRA_ES3_ERRPATCH", eb, sizeof eb);
        if (!(en > 0 && en < sizeof eb && eb[0] && eb[0] != '0')) {
            cre_trace("EARLY: gate FAILED (en=%lu eb0=%d) -- returning now", (unsigned long)en, eb[0]);
            return;
        }
    }
    cre_trace("EARLY: gate passed, h=%08X", (unsigned)(uintptr_t)h);
    if (!h) { cre_trace("EARLY: h is NULL -- returning"); return; }

    delta = (uintptr_t)h - (uintptr_t)0x00400000;
    g_img_delta = delta;
    g_img_delta_ok = 1;
    cre_trace("EARLY: delta=%08X, calling cocreate_diag_install", (unsigned)delta);
    cocreate_diag_install(delta);
    cre_trace("EARLY: calling err_trace_install");
    err_trace_install(delta);   /* FIRST: catch even the earliest raise */
    cre_trace("EARLY: calling err_suppress");
    err_suppress(delta);
    cre_trace("EARLY: calling router_patch");
    router_patch(delta);
    cre_trace("EARLY: calling steer_bypass");
    steer_bypass(delta);
    cre_trace("EARLY: steer_bypass returned, about to call card_reader_enable");

    /* card_reader_enable() (IC card R/W row) used to run ONLY from inside
     * str_diag(), which itself only fires on a specific JVS byte pattern
     * (form==0x20, a one-time board-open handshake) observed deep in JVS
     * write processing. That is a fragile, easy-to-miss trigger -- if that
     * exact handshake byte doesn't occur (or occurs differently) on a given
     * boot, str_diag() never runs AT ALL, and neither does anything inside
     * it, silently. steer_bypass/router_patch above never had this problem
     * because they are called directly and unconditionally right here, from
     * DllMain. Call card_reader_enable() the same reliable way -- it no
     * longer depends on that JVS trigger firing at all. (The str_diag() call
     * site is left in place as a harmless re-assertion if it does fire.) */
    card_reader_enable(delta);
    /* Same reasoning: vendor_port_ready() completes the COM2 card-vendor
     * open the row 0x16C/0x186 checks are waiting on, and it too was ONLY
     * ever called from inside the same fragile str_diag() trigger. */
    vendor_port_ready(delta);
}

static void router_force_found(uintptr_t delta) {
    static int logged = 0;
    uint32_t obj = 0;
    uint8_t v = 0;
    const char* e;
    const char* lp;
    FILE* f;

    e = getenv("ULTRA_NAMCO_ROUTER");
    if (e && e[0] == '0') return;

    if (!str_rd(0x00A2BE04 + delta, &obj, 4) || !obj) return;
    if (!str_rd(obj + 0x141, &v, 1)) return;
    if (v) return;                                  /* already reported found */

    *(volatile uint8_t*)(uintptr_t)(obj + 0x141) = 1;

    if (logged) return;
    logged = 1;
    lp = getenv("ULTRA_JVS_LOG");
    if (!lp || !*lp) return;
    f = fopen(lp, "a");
    if (!f) return;
    fprintf(f, "ROUTER: content-router object %08X +0x141 forced to 1 (found)\n", obj);
    fclose(f);
}

static void steer_trace_dump(uintptr_t delta) {
    uint32_t self = g_h4_this;
    uint32_t g = 0, snd = 0;
    uint32_t s4c = 0, s70 = 0, s78 = 0, ad4 = 0, ad8 = 0, adc = 0, s48 = 0, s50 = 0;
    const char* lp;
    FILE* f;
    if (!self) return;
    str_rd(self + 0x04C, &s4c, 4);
    str_rd(self + 0x070, &s70, 4);
    str_rd(self + 0x078, &s78, 4);
    str_rd(self + 0x048, &s48, 4);
    str_rd(self + 0x050, &s50, 4);
    str_rd(self + 0xAD4, &ad4, 4);
    str_rd(self + 0xAD8, &ad8, 4);
    str_rd(self + 0xADC, &adc, 4);
    if (str_rd(0x00E416A0 + delta, &g, 4) && g) str_rd(g + 0x180, &snd, 4);
    lp = getenv("ULTRA_JVS_LOG");
    if (!lp || !*lp) return;
    f = fopen(lp, "a");
    if (!f) return;
    {
        /* Row 0x16C's step 0 is:
         *     mov  0xE423D4,%edx
         *     cmpb $0x0,0x117(%edx)   ; reader check 0 done?
         *     je   <wait>
         *     mov  $0x3,%ecx / call 0x5E9BF0   ; arm check 3
         *     test %al,%al / je <wait>
         *     incl 0x78(%ebx)
         * and its timeout at 0x5EB413 (0x384 frames) raises 0x63 -> E07-11.
         * So log the driver state, ALL FIVE done flags (0x117+state) and this
         * row's own sub-state, which is the only way to tell whether 0x117 is
         * really set when the row looks at it. */
        uint32_t s54 = 0, s58 = 0, drv = 0, dst = 0;
        uint8_t  dn[5];
        memset(dn, 0xEE, sizeof dn);
        str_rd(self + 0x054, &s54, 4);
        str_rd(self + 0x058, &s58, 4);
        if (str_rd(0x00E423D4 + delta, &drv, 4) && drv) {
            str_rd(drv + 0x004, &dst, 4);
            str_rd(drv + 0x117, dn, 5);
        }
        fprintf(f, "STEERROW: this=%08X [4C]=%d [50]=%d [54]=%d [58]=%d "
                   "step[78]=%d timer[70]=%d ad4=%d ad8=%d adc=%d | "
                   "drv=%08X state=%d done=%02X %02X %02X %02X %02X | row6calls=%u this6=%08X\n",
                self, (int)s4c, (int)s50, (int)s54, (int)s58,
                (int)s78, (int)s70, (int)ad4, (int)ad8, (int)adc,
                drv, (int)dst, dn[0], dn[1], dn[2], dn[3], dn[4], g_h6_calls, g_h6_this);
    }
    (void)s48; (void)g; (void)snd;
    fclose(f);
}

/* ---------------------------------------------------------------------------
 *  STEERING-CHECK BYPASS
 * ---------------------------------------------------------------------------
 * The drive-cabinet boot screen stops on "ステアリングチェック チェック中"
 * because the steering check needs a SERIAL port the machine does not have.
 * 0x5EAFE4 calls 0x5E9500, which calls the opener at 0x83CE10 with the ASCII
 * name "COM4" (0x937AA4). That opener ends in
 *
 *      call 0x840A30            ; bring the port up
 *      test %eax,%eax
 *      je   <ok>                ; 0 = up    -> returns 1
 *      movl $0xFFFFFFFF,(%ebp)  ; mark slot dead
 *      mov  $0xFFFFFFFE,%eax    ; -2
 *
 * and the whole verdict then funnels through two bytes:
 *
 *      5EB012:  84 C0   test %al,%al
 *      5EB014:  74 53   je 0x5EB069     <- the "still checking" branch
 *      5EB016:  success: [esi+0x18e]=1, [ebx+0x4c]=2, [ebx+0x50]=1, ret 1
 *
 * NOPing that JE makes the check report done. The success path was read
 * instruction by instruction first: it touches frame locals and null-guarded
 * pointers only, so it cannot fault the way the earlier STR-gate patch did.
 *
 * The bytes are verified before writing, the page protection is restored
 * immediately, and it happens EXACTLY ONCE - repeated VirtualProtect churn
 * over live code is what stripped EXECUTE and caused random crashes on 33
 * boards once before.
 *
 * ULTRA_NAMCO_STEER_BYPASS=0 disables it. */
static void steer_bypass(uintptr_t delta) {
    static int done = 0;
    const char* e;
    const char* lp;
    FILE* f;
    uint8_t cur[2];
    uintptr_t at;
    DWORD old = 0;
    const char* what;

    if (done) return;
    done = 1;
    e = getenv("ULTRA_NAMCO_STEER_BYPASS");
    if (e && e[0] == '0') return;

    at = 0x005EB014 + delta;
    if (!str_rd(at, cur, 2))                      what = "unreadable";
    else if (cur[0] != 0x74 || cur[1] != 0x53)    what = "bytes did not match, left alone";
    else if (!VirtualProtect((LPVOID)at, 2, PAGE_EXECUTE_READWRITE, &old))
                                                  what = "VirtualProtect failed";
    else {
        ((uint8_t*)at)[0] = 0x90;
        ((uint8_t*)at)[1] = 0x90;
        VirtualProtect((LPVOID)at, 2, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)at, 2);
        what = "PATCHED je -> nop nop";
    }

    lp = getenv("ULTRA_JVS_LOG");
    if (!lp || !*lp) return;
    f = fopen(lp, "a");
    if (!f) return;
    fprintf(f, "STEERBYPASS: at=%08X was=%02X %02X : %s\n",
            (unsigned)at, cur[0], cur[1], what);
    fclose(f);
}

static void str_diag(void) {
    /* MK_AGP3_FINAL sets DYNAMICBASE, so Windows RELOCATES it and the
     * disassembly's 0x400000-based addresses are never where they say they
     * are at runtime. The first version of this probe compared the module
     * handle against 0x400000 and silently logged nothing for a whole run.
     * Rebase everything by the real load delta instead. */
    static int on = -1;
    static uintptr_t delta = 0;
    static unsigned n = 0;
    uint32_t objp = 0, state = 0, tick = 0;
    uint8_t gate = 0, nidx = 0, phase = 0, f117 = 0, f118 = 0;
    uint8_t resp[8], nhdr[4], inflight = 0, roff = 0;
    uintptr_t node;
    const char* lp;
    FILE* f;

    if (on < 0) {
        HMODULE h = GetModuleHandleA(NULL);
        char nm[MAX_PATH];
        /* Same flag as the patch set above: the title that wants those patches
         * is the title that wants this diagnostic, and it asks for both in its
         * container rather than being recognized by filename here. The exe name
         * is still READ - the diagnostic prints it - but nothing is decided
         * from it. */
        nm[0] = 0;
        if (!h || !GetModuleFileNameA(h, nm, sizeof nm)) nm[0] = 0;
        {
            /* GetEnvironmentVariableA, not getenv: this DLL is loaded into an
             * already-running game process, and the loader publishes container
             * [env] keys (like this one, from golden.ini) via SetEnvironmentVariableA
             * AFTER this DLL's CRT has already taken its getenv() snapshot. getenv()
             * here always read stale/empty and silently disabled every patch this
             * gate covers (steer_bypass, steer_force_ready, router_patch,
             * card_reader_enable, vendor_port_ready, ...) even though golden.ini
             * correctly set ULTRA_ES3_ERRPATCH=1. Same trap already documented and
             * fixed this way elsewhere in this tree (e.g. allnet_server.c). */
            char eb[8];
            DWORD en = GetEnvironmentVariableA("ULTRA_ES3_ERRPATCH", eb, sizeof eb);
            on = (en > 0 && en < sizeof eb && eb[0] && eb[0] != '0') ? 1 : 0;
        }
        delta = (uintptr_t)h - (uintptr_t)0x00400000;
        g_img_delta = delta;
        g_img_delta_ok = 1;
        {
            const char* lp0 = getenv("ULTRA_JVS_LOG");
            if (lp0 && *lp0) {
                FILE* f0 = fopen(lp0, "a");
                if (f0) {
                    fprintf(f0, "STRDIAG: base=%08X delta=%08X exe=%s on=%d\n",
                            (unsigned)(uintptr_t)h, (unsigned)delta, nm, on);
                    fclose(f0);
                }
            }
        }
    }
    if (!on) return;
    steer_bypass(delta);
    router_patch(delta);
    steer_trace_install(delta);
    steer_force_ready(delta);
    card_reader_enable(delta);
    vendor_port_ready(delta);
    es3_home_watchdog();
    es3_scene_watch();
    es3_title_hold();
    router_force_found(delta);
    err_list_dump(delta);
    if ((n++ % 60) != 0) return;
    steer_trace_dump(delta);

    if (!str_rd(0x00E45280 + delta, &gate, 1))  return;
    if (!str_rd(0x00E458E1 + delta, &nidx, 1))  return;
    if (!str_rd(0x00E423D4 + delta, &objp, 4))  return;

    memset(resp, 0, sizeof resp);
    memset(nhdr, 0, sizeof nhdr);
    if (objp) {
        str_rd(objp + 0x004, &state, 4);
        str_rd(objp + 0x117, &f117, 1);
        str_rd(objp + 0x118, &f118, 1);
        str_rd(objp + 0x11C, &phase, 1);
        str_rd(objp + 0x120, &tick, 4);
        str_rd(objp + 0x088, resp, 8);
    }
    node = 0x00E460F0 + delta + (uintptr_t)nidx * 0x638;
    str_rd(node + 0x000, nhdr, 4);
    str_rd(node + 0x220, &inflight, 1);
    str_rd(node + 0x221, &roff, 1);

    /* THE STEERING BOARD IS NOT ON THE JVS BUS AT ALL.
     *
     * 0x5EAFE4 is the steering check. It calls 0x5E9500, which opens a SERIAL
     * PORT by name - and the name pushed at 0x5E953F is the ASCII string
     * "COM4" at 0x937AA4 (0x5E952C pushes "COM2" for the other device). The
     * game log confirms it: the only ports it ever opened are COM3, its JVS
     * I/O board, and one \.\COM1. COM4 is never opened.
     *
     * The reason is the first instruction of the opener at 0x83CE10:
     *
     *     cmpb $0x0,0xA20560
     *     jne  <proceed>
     *     mov  $0xFFFFFF9B,%eax     ; -101
     *     ret
     *
     * That flag starts at 0 in the image and is set to 1 in exactly one place,
     * 0x83C733, inside the serial subsystem init at 0x83C6F0 - which is
     * unconditional once entered (memset 8 slots of 0x590 at 0xA20568, three
     * sub-inits, set flag), and is reached from exactly one call site,
     * 0x71F25D, guarded by "is 0xE423E0 still NULL".
     *
     * So read the three things that decide it:
     *   0xA20560  serial subsystem ready
     *   0xE423E0  the steering-check object (NULL = its ctor never ran)
     *   0xA20568  port slot 0: dword 0 = port index (-1 = never opened),
     *             +0x1B = the 15-char name the opener copied in
     * Slot stride is 0x590, 8 slots. */
    {
        uint8_t  sflag = 0;
        uint32_t sobj = 0, sfc = 0, s100 = 0;
        int      sl;
        const char* lp3 = getenv("ULTRA_JVS_LOG");
        FILE* f3 = (lp3 && *lp3) ? fopen(lp3, "a") : NULL;
        if (f3) {
            str_rd(0x00A20560 + delta, &sflag, 1);
            str_rd(0x00E423E0 + delta, &sobj, 4);
            if (sobj) {
                str_rd(sobj + 0x0FC, &sfc, 4);
                str_rd(sobj + 0x100, &s100, 4);
            }
            fprintf(f3, "SERIAL: ready=%u steerobj=%08X [+fc]=%u [+100]=%u",
                    sflag, sobj, sfc, s100);
            for (sl = 0; sl < 3; ++sl) {
                uintptr_t sb = 0x00A20568 + delta + (uintptr_t)sl * 0x590;
                uint32_t idx = 0xEEEEEEEEu;
                char nm2[16];
                memset(nm2, 0, sizeof nm2);
                str_rd(sb, &idx, 4);
                str_rd(sb + 0x1B, nm2, 15);
                nm2[15] = 0;
                fprintf(f3, " | slot%d idx=%d name=%s", sl, (int)idx, nm2);
            }
            fprintf(f3, "\n");
            fclose(f3);
        }
    }

    /* WHY THE CHECK NEVER STARTS.
     *
     * 0x5EB0F9 is the only site that hands the STR driver a check:
     *
     *     cmp  %edx,0xad4(%ebx)      ; edx = 0, so [+0xad4] must be 0
     *     jne  <NG>
     *     cmp  %edx,0xad8(%ebx)      ; [+0xad8] must be > 0
     *     jle  <skip>                ; <-- we take this every frame
     *     mov  0xE423D4,%eax
     *     cmpl $0x6,0x4(%eax)        ; driver idle?
     *     mov  %edx,0x4(%eax)        ; state = 0  -> RUN CHECK 0
     *
     * and 0x5EA0F1 fills both from a CONFIG blob, not from anything the I/O
     * board reports:
     *
     *     mov    0x188(%esi),%eax    ; esi = [0xE416A0]
     *     mov    0x34(%eax),%edx
     *     movzbl 0x19(%edx),%ecx  -> [+0xad4]
     *     movzbl 0x1a(%edx),%ecx  -> [+0xad8]
     *
     * So if cfg[0x1a] is 0 the steering check can never begin no matter what
     * the board answers - which is exactly the observed state=6 forever. */
    {
        uint32_t a1 = 0, a2 = 0, cfg = 0;
        uint8_t blob[48];
        memset(blob, 0, sizeof blob);
        if (str_rd(0x00E416A0 + delta, &a1, 4) && a1 &&
            str_rd(a1 + 0x188, &a2, 4) && a2 &&
            str_rd(a2 + 0x34, &cfg, 4) && cfg &&
            str_rd(cfg, blob, sizeof blob)) {
            const char* lp2 = getenv("ULTRA_JVS_LOG");
            FILE* f2 = (lp2 && *lp2) ? fopen(lp2, "a") : NULL;
            if (f2) {
                int q;
                fprintf(f2, "STRCFG: cfg=%08X [0x19]=%u [0x1a]=%u [0x1b]=%u dump=",
                        cfg, blob[0x19], blob[0x1a], blob[0x1b]);
                for (q = 0; q < 48; ++q) fprintf(f2, "%02X ", blob[q]);
                fprintf(f2, "\n");
                fclose(f2);
            }
        }
    }

    lp = getenv("ULTRA_JVS_LOG");
    if (!lp || !*lp) return;
    f = fopen(lp, "a");
    if (!f) return;
    fprintf(f,
        "STRDIAG: gate=%u nidx=%u obj=%08X state=%d phase=%u tick=%u "
        "done(117/118)=%u/%u resp=%02X %02X %02X %02X %02X %02X %02X %02X | "
        "node=%08X hdr=%02X %02X %02X %02X inflight=%u roff=%02X\n",
        gate, nidx, objp, (int)state, phase, tick, f117, f118,
        resp[0], resp[1], resp[2], resp[3], resp[4], resp[5], resp[6], resp[7],
        (unsigned)node, nhdr[0], nhdr[1], nhdr[2], nhdr[3], inflight, roff);
    fclose(f);
}

/* Log every record we hand over, so the next capture says WHICH step stalled
 * instead of leaving it to be inferred from record sizes. */
static void str_log(int step, int n) {
    const char* lp = getenv("ULTRA_JVS_LOG");
    FILE* f;
    if (!lp || !*lp) return;
    f = fopen(lp, "a");
    if (!f) return;
    fprintf(f, "STR: emit step=%d len=%d\n", step, n);
    fclose(f);
}

/* Queue the record the driver's CURRENT state is waiting for. Sizes come from
 * the read(buf,n) calls in each handler; byte 4 is the status character the
 * handler tests. */
static void str_queue_next(void) {
    static const struct { int n; } REC[5] = { {7}, {7}, {0x16}, {0x0A}, {7} };
    int n, k;
    if (g_str.len > 0) return;                 /* previous record not taken yet */
    if (g_str.step < 0 || g_str.step > 4) return;
    n = REC[g_str.step].n;
    if (n > STR_RING_MAX) n = STR_RING_MAX;
    for (k = 0; k < n; ++k) g_str.buf[k] = 0x00;
    g_str.buf[0] = 0x02;                       /* STX, matching the game's own
                                                * command framing at 0x5ECFA0 */
    g_str.buf[1] = 0x41;                       /* 'A' */
    g_str.buf[4] = 0x31;                       /* '1' = the status every state
                                                * checks (state 4 only needs
                                                * != 0 and != '0', so '1' works
                                                * for it too) */
    if (g_str.step == 3) {
        /* Check 3 feeds the card VENDOR row (0x16C). Its parser at 0x5ED1A0
         * keeps two bytes:
         *     [+0x115] = resp[5]      [+0x116] = resp[6]
         * and 0x5EB3C6 then requires  [+0x116] == 0x31  ('1'), not merely
         * non-zero - an earlier 0x01 here satisfied the parser and still
         * failed the row. [+0x115] becomes the "cards sold out" flag via
         *     (~(resp[5] >> 3)) & 1   -> stored at [esi+0x1A0]
         * and 0x5EB28B shows "カード売り切れ中" when that is non-zero, so
         * bit 3 of resp[5] must be SET to report cards available. */
        g_str.buf[5] = 0x38;                   /* bit 3 set: not sold out */
        g_str.buf[6] = 0x31;                   /* '1' */
    }
    if (n > 1) g_str.buf[n - 1] = 0x03;        /* ETX */
    g_str.len = n;
}

/* Returns logical response length in `rsp`, or 0 for "no response" (RESET). */
static size_t handle_packet(ultra_jvs_node* n, uint8_t dest,
                            const uint8_t* d, size_t dlen,
                            uint8_t* rsp, size_t rsp_cap) {
    /* Special case: Taito games (Chaos Breaker, GigaWing Generations) poll command
     * 0x04 addressed to the HOST address (0x00) every session right after switching to
     * the run state, and BLOCK on the reply -- no answer -> they retransmit (0x2F) then
     * flag I/O failed (ioResult=-3 -> watchdog -> shutdown). It is not a standard node
     * command; we ACK it so the I/O stays healthy. (RE-derived: this is THE thing that
     * made input dead / the games unplayable.) All other host-bound (0x00) traffic is
     * genuinely a node->host response and not for us. */
    const int host_cmd04 = (dest == JVS_HOST_ADDR && dlen >= 1 && d[0] == 0x04);
    if (dest == JVS_HOST_ADDR && !host_cmd04) return 0;
    /* Addressing-aware filter (skipped for the host_cmd04 special case):
     *  - unaddressed (addr 0): accept everything (setup phase: RESET / SET_ADDR).
     *  - addressed: answer only packets to OUR address; for broadcast accept ONLY
     *    RESET. We must stay SILENT to broadcast node-probes (e.g. 0x2F) once
     *    addressed -- that silence is how the host learns the chain is fully
     *    addressed and stops walking SET_ADDRESS. (Replying made it loop forever.) */
    if (!host_cmd04 && n->address != 0) {
        if (dest == JVS_BROADCAST) {
            if (!(dlen >= 1 && d[0] == JVS_CMD_RESET)) return 0;
        } else if (dest != n->address) {
            return 0;
        }
    }

    size_t r = 0;
    uint8_t status = JVS_STATUS_NORMAL;
    int no_response = 0, overflow = 0;

    /* A JVS packet's NUM is one byte, so the logical response (status + reports)
     * is at most 254 bytes (NUM = data + 1 <= 255). Bound to that AND the
     * caller's buffer. PUT stores only within bound and otherwise flags overflow
     * and stops growing -- this is what stops a large READ_SWITCH/ANALOG count
     * (np*nb up to 65025) from running r past rsp[] (the critical OOB read). */
    const size_t cap = (rsp_cap < 254) ? rsp_cap : 254;
    /* reserve rsp[0] for status; fill reports after it */
    #define PUT(b) do { if (1 + r < cap) { rsp[1 + r] = (uint8_t)(b); r++; } else { overflow = 1; } } while (0)

    size_t i = 0;
    while (i < dlen) {
        uint8_t cmd = d[i++];
        switch (cmd) {

        case JVS_CMD_RESET:                       /* F0 D9 -- no response */
            if (i < dlen) i++;                     /* consume 0xD9 */
            n->address = 0;
            no_response = 1;
            i = dlen;                              /* RESET is terminal */
            break;

        case JVS_CMD_SET_ADDR:                     /* F1 addr */
            if (i >= dlen) { status = JVS_STATUS_UNKNOWN; i = dlen; break; }
            /* Only an UNaddressed node accepts an address (JVS daisy-chain). Once
             * addressed we ignore further broadcast SET_ADDRESS with NO response,
             * so the host sees no node beyond us and stops walking the chain. */
            if (n->address == 0) {
                n->address = d[i++];
                PUT(JVS_REPORT_NORMAL);
            } else {
                i++;                 /* consume addr byte; stay silent */
                no_response = 1;
                i = dlen;
            }
            break;

        case JVS_CMD_SET_COMMS:                    /* F2 method */
            if (i < dlen) i++;
            PUT(JVS_REPORT_NORMAL);
            break;

        case 0x04:                                 /* Taito host-addr poll: ACK so I/O stays healthy */
            PUT(JVS_REPORT_NORMAL);
            break;

        case JVS_CMD_IOIDENT: {                    /* 10 -> report + id\0 */
            PUT(JVS_REPORT_NORMAL);
            const char* s = n->id;
            while (*s) PUT((uint8_t)*s++);
            PUT(0x00);
            break;
        }
        case JVS_CMD_CMD_REV:  PUT(JVS_REPORT_NORMAL); PUT(0x13); break; /* 1.3 */
        case JVS_CMD_JVS_REV:  PUT(JVS_REPORT_NORMAL); PUT(0x30); break; /* 3.0 */
        case JVS_CMD_COMM_VER: PUT(JVS_REPORT_NORMAL); PUT(0x10); break; /* 1.0 */

        case JVS_CMD_FEATCHK: {                     /* 14 -> report + features + 0x00 */
            PUT(JVS_REPORT_NORMAL);
            /* switch input: players, switches/player */
            PUT(0x01); PUT(n->players); PUT(n->switches_pp); PUT(0x00);
            /* coin input: slots */
            PUT(0x02); PUT(n->coin_slots); PUT(0x00); PUT(0x00);
            /* analog input: channels, bits */
            PUT(0x03); PUT(n->analog_ch); PUT(n->analog_bits); PUT(0x00);
            if (n->has_screen_pos) {                /* screen position: Xbits,Ybits,#ch */
                PUT(0x06); PUT(0x10); PUT(0x10); PUT(0x01);
            }
            if (n->analog_out_ch) {                 /* analog output: channels */
                PUT(0x13); PUT(n->analog_out_ch); PUT(0x00); PUT(0x00);
            }
            /* general purpose output: slots(bits) */
            PUT(0x12); PUT(n->gpo_slots); PUT(0x00); PUT(0x00);
            PUT(0x00);                              /* end of features */
            break;
        }

        case JVS_CMD_MAIN_ID:                       /* 15 + string\0 */
            while (i < dlen && d[i] != 0x00) i++;
            if (i < dlen) i++;                      /* consume terminator */
            PUT(JVS_REPORT_NORMAL);
            break;

        case JVS_CMD_READ_SWITCH: {                 /* 20 nplayers nbytes */
            if (i + 1 >= dlen) { status = JVS_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t np = d[i++], nb = d[i++];
            PUT(JVS_REPORT_NORMAL);
            PUT(n->in.system);                      /* system (TEST/TILT) */
            for (uint8_t p = 0; p < np; ++p) {
                uint16_t pw = (p < JVS_MAX_PLAYERS) ? n->in.player[p] : 0;
                for (uint8_t b = 0; b < nb; ++b) {
                    uint8_t v = (b == 0) ? (uint8_t)(pw >> 8)
                              : (b == 1) ? (uint8_t)(pw & 0xff)
                              : 0;
                    PUT(v);
                }
            }
            break;
        }

        case JVS_CMD_READ_COIN: {                   /* 21 nslots -> 2 bytes/slot */
            if (i >= dlen) { status = JVS_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t ns = d[i++];
            PUT(JVS_REPORT_NORMAL);
            for (uint8_t s = 0; s < ns; ++s) {
                uint32_t c = (s < JVS_MAX_COINS) ? n->coin[s] : 0;
                if (c > 0x3FFF) c = 0x3FFF;
                PUT((uint8_t)((c >> 8) & 0x3F));    /* cond=00 (normal) | count hi */
                PUT((uint8_t)(c & 0xFF));           /* count lo */
            }
            break;
        }

        case JVS_CMD_READ_ANALOG: {                 /* 22 nch -> 2 bytes/ch (BE) */
            if (i >= dlen) { status = JVS_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nc = d[i++];
            PUT(JVS_REPORT_NORMAL);
            for (uint8_t c = 0; c < nc; ++c) {
                /* Center (0x8000) for any channel we have no data for: a 0 here
                 * reads as full-negative deflection, which on a driving wheel /
                 * pedal axis would jam steering hard-left or the brake fully on. */
                uint16_t v = (c < JVS_MAX_ANALOG) ? n->in.analog[c] : 0x8000;
                PUT((uint8_t)(v >> 8)); PUT((uint8_t)(v & 0xff));
            }
            break;
        }

        case JVS_CMD_READ_ROTARY: {                 /* 23 nch -> 2 bytes/ch (0) */
            if (i >= dlen) { status = JVS_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nc = d[i++];
            PUT(JVS_REPORT_NORMAL);
            for (uint8_t c = 0; c < nc; ++c) { PUT(0); PUT(0); }
            break;
        }

        case JVS_CMD_READ_SCRPOS: {                 /* 25 which -> X hi/lo, Y hi/lo */
            if (i >= dlen) { status = JVS_STATUS_UNKNOWN; i = dlen; break; }
            i++;                                     /* channel index (single gun) */
            PUT(JVS_REPORT_NORMAL);
            PUT((uint8_t)(n->in.screen_x >> 8)); PUT((uint8_t)(n->in.screen_x & 0xff));
            PUT((uint8_t)(n->in.screen_y >> 8)); PUT((uint8_t)(n->in.screen_y & 0xff));
            break;
        }

        case JVS_CMD_READ_MISC: {                   /* 26 nbytes -> zeros */
            if (i >= dlen) { status = JVS_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nbytes = d[i++];
            PUT(JVS_REPORT_NORMAL);
            for (uint8_t b = 0; b < nbytes; ++b) PUT(0);
            break;
        }

        case JVS_CMD_COIN_DEC:                       /* 30 slot hi lo */
        case JVS_CMD_COIN_INC: {                     /* 35 slot hi lo */
            if (i + 2 >= dlen) { status = JVS_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t slot = d[i++];
            uint16_t amt = (uint16_t)((d[i] << 8) | d[i + 1]); i += 2;
            /* JVS COIN_DECREASE/INCREASE number coin slots 1-based (slot 1 = the
             * first chute), whereas READ_COIN reports them in array order from the
             * first slot. Map the 1-based wire slot to our 0-based array so a debit
             * hits the SAME counter the game just read. (RE-derived from Wacky Race:
             * it READ_COIN slot0 = 2 credits but COIN_DECREASE slot=1 -- treating
             * that 1 as a 0-based index debited the wrong (empty) counter, so the
             * credit never dropped and the game never committed to the paid race.)
             * A game that genuinely uses 0-based slot 0 still maps to coin[0] via
             * the >=1 guard below (slot 0 -> index 0). */
            uint8_t idx = (slot >= 1) ? (uint8_t)(slot - 1) : 0;
            if (idx < JVS_MAX_COINS) {
                if (cmd == JVS_CMD_COIN_DEC) {
                    n->coin[idx] = (n->coin[idx] >= amt) ? n->coin[idx] - amt : 0;
                } else {
                    n->coin[idx] += amt;
                    if (n->coin[idx] > 0x3FFF) n->coin[idx] = 0x3FFF;
                }
            }
            PUT(JVS_REPORT_NORMAL);
            break;
        }

        case JVS_CMD_GPO: {                          /* 32 nbytes data... */
            if (i >= dlen) { status = JVS_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nbytes = d[i++];
            /* A declared output length that runs past the packet end is a malformed
             * frame: flag param error rather than silently truncating + ACKing it
             * NORMAL (which would drop the bundled READ_COIN/READ_ANALOG after it). */
            if (nbytes > (uint8_t)(dlen - i)) { status = JVS_STATUS_UNKNOWN; i = dlen; break; }
            i += nbytes;                                            /* consume outputs */
            PUT(JVS_REPORT_NORMAL);
            break;
        }

        case JVS_CMD_ANALOG_OUT: {                    /* 33 nch + 2 bytes/ch (force-fb) */
            /* Driving games (e.g. Wacky Race) drive the wheel force-feedback motor
             * via this command EVERY frame, bundled in the SAME packet as
             * READ_SWITCH/READ_COIN/READ_ANALOG. The byte after the command is the
             * channel count; each channel is a 16-bit DAC value (2 bytes). We have
             * no physical motor, so we consume + ACK and KEEP PARSING -- the prior
             * `default:` fall-through here aborted the packet, so READ_COIN and
             * READ_ANALOG (which follow it) were never answered and the game saw a
             * dead coin slot / dead wheel. (RE-derived from the live JVS trace.) */
            if (i >= dlen) { status = JVS_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nch = d[i++];
            /* nch channels * 2 bytes/channel must fit in the remaining packet; a
             * short/corrupt frame is a param error, not a silently-truncated ACK
             * (which would drop the READ_COIN/READ_ANALOG that follow it). */
            if ((uint16_t)nch * 2u > (uint16_t)(dlen - i)) { status = JVS_STATUS_UNKNOWN; i = dlen; break; }
            i += (uint16_t)nch * 2u;                                /* consume DAC words */
            PUT(JVS_REPORT_NORMAL);
            break;
        }

        case JVS_CMD_CHAR_OUT: {                      /* 34 nbytes + display chars */
            /* Character/segment display output (operator panel). Length-prefixed;
             * consume + ACK so a bundled poll keeps parsing. */
            if (i >= dlen) { status = JVS_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nbytes = d[i++];
            /* same under-run guard as GPO/ANALOG_OUT: a length that runs past the
             * packet end is malformed, flag param error instead of a short ACK. */
            if (nbytes > (uint8_t)(dlen - i)) { status = JVS_STATUS_UNKNOWN; i = dlen; break; }
            i += nbytes;
            PUT(JVS_REPORT_NORMAL);
            break;
        }

        case JVS_CMD_GPO_BYTE:                        /* 38 index value */
            if (i + 1 >= dlen) { status = JVS_STATUS_UNKNOWN; i = dlen; break; }
            i += 2;
            PUT(JVS_REPORT_NORMAL);
            break;

        case JVS_CMD_RETRANSMIT:                      /* 2F -- ack only */
            PUT(JVS_REPORT_NORMAL);
            break;

        case JVS_CMD_READ_KEYCODE:                    /* 24 -- no keypad: ack, no keys */
            PUT(JVS_REPORT_NORMAL);
            break;

        case 0x70: {
            const uint8_t sub = (i < dlen) ? d[i] : 0x00;
            /* MANUFACTURER-SPECIFIC COMMAND (Namco).
             *
             * Mario Kart Arcade GP DX drives its steering/force-feedback board
             * through this opcode, and answering JVS_STATUS_UNKNOWN to it -- the
             * spec-correct thing to do for a command whose length you cannot
             * know -- flags the WHOLE packet, so the game also loses the
             * READ_SWITCH / READ_COIN / READ_ANALOG reports that shared it. Its
             * boot-check then sits on "ステアリングチェック チェック中" forever.
             *
             * Observed on that title (2026-09-10), after JVS un-escaping, the
             * 0x70 commands are ALWAYS last in the packet and take these shapes:
             *
             *     70 18 50 4C 02 02 58        70 18 50 4C 80 08
             *     70 15 00 FF                 70 16 20 06
             *     70 18 50 4C 14 D0 3B 57 69 6E 41 72
             *
             * Sub-opcode 0x18 carries a variable payload, so there is no fixed
             * length to skip. What IS reliable across every captured packet is
             * that 0x70 never occurs INSIDE a payload -- so we delimit on the
             * opcode byte itself: consume up to the next 0x70 or end of packet,
             * and emit exactly one report per command. That keeps the report
             * COUNT right without pretending to know the payload format.
             *
             * The report is a bare NORMAL with no data, which is the minimal
             * valid answer. If the board is expected to return state (a wheel
             * position echo, an FFB ack) this will not be enough and the check
             * will still stall -- but it stalls WITHOUT poisoning the rest of
             * the packet, which is the necessary first step either way. */
            PUT(JVS_REPORT_NORMAL);
            /* HOW MANY DATA BYTES FOLLOW THE REPORT.
             *
             * This is not a guess any more. Mario Kart Arcade GP DX's own
             * parser says it, per sub-opcode (addresses are its MK_AGP3_FINAL
             * .exe at a 0x400000 image base):
             *
             *   sub 0x16  ->  report ONLY.
             *        0x839E9B: on report==1 it does a single `inc edi`, i.e.
             *        consumes one byte, then sets its "init done" flag.
             *
             *   sub 0x15  ->  report + EXACTLY ONE data byte, which MUST be 0.
             *        0x839F27: `mov dl,[edi+1]` / `add edi,2` / `test dl,dl` /
             *        `jne` -> 0x839EDA, which returns -29. So a non-zero byte
             *        is a hard error. The other branch of the same command
             *        (0x839F53) reads that byte as a COUNT of bytes to follow,
             *        where 0 likewise means "nothing more" -- so a single 0x00
             *        is the correct answer to BOTH forms.
             *
             *   sub 0x18  ->  report + EXACTLY ONE data byte, per group.
             *        0x837EB2: `mov dl,[ecx+1]` / `add ecx,2`, repeated while
             *        its pending-count at node+0x36C is non-zero.
             *
             * Why this matters: we used to answer every 0x70 with a bare report
             * and no data. For sub 0x15 -- which that game sends on EVERY poll
             * -- the parser then read the frame's CHECKSUM as the data byte.
             * A checksum is essentially never 0, so every poll returned -29,
             * and because the error path bails before clearing the request flag
             * at obj+0x220 the game re-sent the identical frame forever. That is
             * exactly what the capture shows: `70 15 00 FF` repeated 7476 times
             * with no progress. */
            {
                static int pad = -2, fill = -1;
                int ndata, k;
                if (pad == -2) {
                    const char *e = getenv("ULTRA_JVS_V70_PAD");
                    const char *f = getenv("ULTRA_JVS_V70_FILL");
                    pad  = (e && *e) ? atoi(e) : -1;      /* -1 = use the table */
                    if (pad > 32) pad = 32;
                    fill = (f && *f) ? (int)strtol(f, NULL, 16) : 0;
                    if (fill < 0 || fill > 0xFF) fill = 0;
                }
                if (pad >= 0) {
                    ndata = pad;                           /* explicit override */
                    for (k = 0; k < ndata; ++k) PUT((uint8_t)fill);
                } else if (sub == 0x15) {
                    /* Which of the two poll forms is this? The 4th byte says:
                     *   70 15 <seq> FF  -> form A, reply is report + ONE zero
                     *   70 15 <seq> 20  -> form B, reply is report + count + data
                     * Form B is the STR board's read channel. */
                    uint8_t form = (i + 2 < dlen) ? d[i + 2] : 0xFF;
                    if (form == 0x20) {
                        /* THE BOARD SELF-CLOCKS ITS BOOT SEQUENCE, AND IT IS
                         * FINITE. Measured from the 2026-09-13 post-reset
                         * capture. (The earlier run in the same file misled two
                         * diagnoses: this hook APPENDS, so that file held two
                         * runs; the real one starts at the JVS RESET.)
                         *
                         *   70 18 ... x9   <- all BEFORE init, host presets
                         *   70 16 20 06    <- board OPEN
                         *   70 15 00 FF    <- form A probe
                         *   70 15 00 20    <- read channel, ~11000 times
                         *
                         * The game never writes again after 70 16. So gating
                         * records on writes - the previous attempt - guarantees
                         * silence forever: that run returned 0 records and hung
                         * in the same place. The board has to volunteer its five
                         * boot records once it is opened.
                         *
                         * It must also STOP. The first attempt queued one per
                         * poll with the step capped at 4, so state 4's 7-byte
                         * record kept being pushed after the driver had stopped
                         * reading: 5282 records into a 256-byte ring, which
                         * wraps and destroys record alignment.
                         *
                         * So: one record per poll for steps 0..4, then go
                         * permanently quiet. Total 7+7+22+10+7 = 53 bytes, which
                         * cannot overflow the ring even if the driver never
                         * reads a single one. str_queue_next() is the step
                         * guard - it queues nothing once step leaves 0..4. */
                        str_init();
                        str_diag();
                        /* SERVE WHATEVER CHECK THE DRIVER IS ACTUALLY ON.
                         *
                         * The driver's command never reaches the wire. Its
                         * builder does queue the 6 bytes - 0x5ECF25 calls the
                         * ring writer at 0x839A40 on node+4, and the gate at
                         * 0xE45280 is 1 so the send is not skipped - but the
                         * JVS layer never drains that ring: across a whole run
                         * the only 70 18 frames are the nine "PL" ones from a
                         * different device, and node+0x220 (write-in-flight)
                         * stays 0 throughout. So there is no command edge to
                         * key replies off.
                         *
                         * The driver's state IS readable, so use it directly.
                         * 0x5E9BF0 sets state and clears done flag 0x117+state,
                         * and the rows drive it out of order (row 0x16C asks for
                         * check 0, then check 3), which is why a local 0..4
                         * counter answered with the wrong record and left
                         * check 3 unanswered forever.
                         *
                         * Queue one record per state transition; go idle again
                         * when the driver parks at 6 so the next check is
                         * served fresh. */
                        if (g_str.enabled) {
                            int rs = reader_state();
                            if (rs < 0) {
                                g_str.served = -1;     /* idle: re-arm */
                            } else if (rs != g_str.served && g_str.len == 0) {
                                g_str.step = rs;
                                str_queue_next();
                                g_str.served = rs;
                            }
                        }
                        if (g_str.len > 0) {
                            int n = g_str.len;
                            if (n > 0x20) n = 0x20;        /* the game asked for <=32 */
                            PUT((uint8_t)n);
                            for (k = 0; k < n; ++k) PUT(g_str.buf[k]);
                            g_str.len = 0;
                            str_log(g_str.step, n);
                        } else {
                            PUT(0x00);                     /* boot sequence done */
                        }
                    } else {
                        PUT(0x00);                         /* form A: must be zero */
                    }
                } else if (sub == 0x18) {
                    /* HOST -> BOARD write.
                     *
                     * THIS NODE IS THE BANAPASSPORT CARD READER, not the
                     * steering board. Row 0x152 ("ICカードR/Wチェック",
                     * handler 0x5EB0A0) is what arms the driver at 0xE423D4:
                     *
                     *     cmp  %edx,0xad4(%ebx)   ; must be 0
                     *     jne  <NG>
                     *     mov  %ecx,0x50(%ebx)    ; row DONE (unconditional)
                     *     cmp  %edx,0xad8(%ebx)   ; must be > 0
                     *     jle  <skip>
                     *     mov  0xE423D4,%eax
                     *     cmpl $0x6,0x4(%eax)     ; idle?
                     *     mov  %edx,0x4(%eax)     ; state = 0 -> RUN
                     *
                     * Once armed, its builders at 0x5ECEF0 / 0x5ECFA0 emit
                     * 02 30 00 00 30 03 and 02 41 00 00 41 03 through
                     * 0x839A40 on node+4, which reaches the wire as 70 18.
                     * An STX payload is therefore a genuine reader command and
                     * the board owes exactly one reply.
                     *
                     * The nine writes seen before any of this ("PL" + data)
                     * belong to a different device and must NOT clock us. */
                    PUT(0x00);
                } else if (sub == 0x16) {
                    str_init();
                    g_str.step = 0;                        /* board OPEN: arm the
                                                            * 5-record sequence */
                    g_str.len  = 0;
                    /* report only */
                } else {
                    ndata = 0;
                    for (k = 0; k < ndata; ++k) PUT(0x00);
                }
            }
            while (i < dlen && d[i] != 0x70) i++;
            break;
        }

        default:
            /* Unknown command: per spec, flag the whole packet and stop, since
             * we cannot know this command's parameter length to skip it. Reports
             * for valid commands earlier in the packet remain in rsp[]. */
            status = JVS_STATUS_UNKNOWN;
            i = dlen;
            break;
        }
    }
    #undef PUT

    if (no_response) return 0;
    /* If we hit the response-size ceiling, report ACK overflow rather than a
     * length-wrapped frame; r is already bounded to <= cap-1 (<= 253). */
    rsp[0] = overflow ? JVS_STATUS_OVERFLOW : status;
    return r + 1;     /* +1 for the status byte (<= 254, so NUM <= 255) */
}

/* -------------------------------------------------------------------------- */

/*  Transport entry: feed wire bytes, get framed responses                     */
/* -------------------------------------------------------------------------- */


/* ===========================================================================
 *  SECOND JVS NODE  --  the drive cabinet's STR PCB
 * ===========================================================================
 *
 * Mario Kart Arcade GP DX expects TWO nodes on its JVS bus: the I/O board at
 * address 1 and the steering / force-feedback board ("STR PCB") at address 2.
 * With only one node present its boot check sits on
 * "ステアリングチェック チェック中" forever and then reports E22-12.
 *
 * MEASURED, 2026-09-10 -- this is not a guess:
 *   - Holding the SENSE line at "chain not complete" (ULTRA_JVS_SENSE1=0) makes
 *     the game immediately send SETADDR for a SECOND node:
 *         TX E0 FF 03 F1 01 F4      (address 1)
 *         TX E0 FF 03 F1 02 F5      (address 2)   <- it wants node 2
 *   - Its driver keeps three node records (stride 0x638) and its state machine
 *     idles in state 0 for exactly as long as EVERY known node is still of the
 *     "awaiting init" kind -- which one node can never stop being.
 *   - Forcing that state machine past state 0 by hand did advance the check
 *     momentarily, confirming the gate, before it fell back for want of a real
 *     board to talk to.
 *
 * Enabled with ULTRA_JVS_NODES=2 (default 1, so every other board is
 * byte-identical to before). The second node's identity defaults to the drive
 * board from the catalog and is overridable with ULTRA_JVS_ID2.
 * ========================================================================= */
static ultra_jvs_node *g_jvs_node2;

static int jvs_nodes_wanted(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("ULTRA_JVS_NODES");
        cached = (e && *e) ? atoi(e) : 1;
        if (cached < 1) cached = 1;
        if (cached > 2) cached = 2;      /* only the pair is implemented */
    }
    return cached;
}

/* Lazily build node 2 as a copy of node 1's feature set, with its own identity
 * and its own address. Input state is mirrored from node 1 on every packet so
 * whichever node the game polls sees the same live controls. */
static ultra_jvs_node *jvs_second_node(ultra_jvs_node *n1) {
    if (jvs_nodes_wanted() < 2 || !n1) return NULL;
    if (!g_jvs_node2) {
        g_jvs_node2 = jvs_create();
        if (!g_jvs_node2) return NULL;
        g_jvs_node2->players     = n1->players;
        g_jvs_node2->switches_pp = n1->switches_pp;
        g_jvs_node2->coin_slots  = n1->coin_slots;
        g_jvs_node2->analog_ch   = n1->analog_ch;
        g_jvs_node2->analog_bits = n1->analog_bits;
        g_jvs_node2->gpo_slots   = n1->gpo_slots;
        /* The STR PCB drives the wheel, so it is the node with ANALOG OUTPUT.
         * Overridable/disable-able with ULTRA_JVS_AOUT2=<channels>. */
        {
            const char *e = getenv("ULTRA_JVS_AOUT2");
            int ao = (e && *e) ? atoi(e) : 2;
            if (ao < 0)  ao = 0;
            if (ao > 16) ao = 16;
            g_jvs_node2->analog_out_ch = (uint8_t)ao;
        }
        {
            const char *e = getenv("ULTRA_JVS_ID2");
            if (e && *e) jvs_set_id(g_jvs_node2, e);
            else         jvs_set_id(g_jvs_node2, "namco ltd.;P-DRIVE PCB;Ver1.00;JPN");
        }
    }
    return g_jvs_node2;
}

/* Every enabled node has taken an address -> the host may stop walking
 * SET_ADDRESS. The serial hook drives the SENSE line off this. */
int jvs_chain_addressed(const ultra_jvs_node *n) {
    if (!n || n->address == 0) return 0;
    if (jvs_nodes_wanted() < 2) return 1;
    return g_jvs_node2 && g_jvs_node2->address != 0;
}

size_t jvs_host_bytes(ultra_jvs_node* n,
                      const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t out_cap) {
    if (!n) return 0;

    /* accumulate */
    if (in && in_len) {
        if (n->rx_len + in_len > JVS_RX_CAP) n->rx_len = 0;   /* overflow: resync */
        if (in_len <= JVS_RX_CAP) {
            memcpy(n->rx + n->rx_len, in, in_len);
            n->rx_len += in_len;
        }
    }

    size_t op = 0;

    for (;;) {
        /* find SYNC at the head */
        size_t start = 0;
        while (start < n->rx_len && n->rx[start] != JVS_SYNC) start++;
        if (start) {                       /* drop noise before SYNC */
            memmove(n->rx, n->rx + start, n->rx_len - start);
            n->rx_len -= start;
        }
        if (n->rx_len < 2) break;          /* need at least SYNC + something */

        size_t wp = 1;                     /* past SYNC */
        uint8_t node, num;
        if (!rd(n->rx, n->rx_len, &wp, &node)) break;   /* need more */
        if (!rd(n->rx, n->rx_len, &wp, &num))  break;
        if (num == 0) {                    /* impossible (needs >=1 for sum): resync */
            memmove(n->rx, n->rx + 1, n->rx_len - 1); n->rx_len -= 1; continue;
        }

        /* read num logical bytes: (num-1) data + 1 checksum. num is one byte
         * (<=255), so a 256-byte buffer always holds a whole packet's data. */
        uint8_t data[256];
        size_t got = 0; int incomplete = 0;
        for (; got < num; ++got) {
            if (!rd(n->rx, n->rx_len, &wp, &data[got])) { incomplete = 1; break; }
        }
        if (incomplete) break;             /* whole packet not on the wire yet */

        uint8_t sum_rx = data[num - 1];
        size_t  dlen   = (size_t)num - 1;  /* data excluding checksum */
        uint8_t sum = (uint8_t)(node + num);
        for (size_t k = 0; k < dlen; ++k) sum = (uint8_t)(sum + data[k]);

        /* consume this packet's wire bytes regardless of checksum outcome */
        size_t consumed = wp;

        if (sum != sum_rx) {
            /* checksum mismatch: drop the SYNC and resync (do not consume rest) */
            memmove(n->rx, n->rx + 1, n->rx_len - 1); n->rx_len -= 1; continue;
        }

        /* dispatch -- pick which node this packet belongs to */
        uint8_t rsp[640];
        ultra_jvs_node *tgt = n;
        {
            ultra_jvs_node *n2 = jvs_second_node(n);
            if (n2) {
                n2->in = n->in;                 /* mirror live controls */
                if (node == JVS_BROADCAST) {
                    if (dlen >= 1 && data[0] == JVS_CMD_RESET) {
                        n2->address = 0;        /* RESET clears the whole chain */
                    } else if (dlen >= 1 && data[0] == JVS_CMD_SET_ADDR
                               && n->address != 0 && n2->address == 0) {
                        tgt = n2;               /* node 1 is taken -> node 2 answers */
                    }
                } else if (node != 0 && node == n2->address && node != n->address) {
                    tgt = n2;
                }
            }
        }
        size_t rlen = handle_packet(tgt, node, data, dlen, rsp, sizeof(rsp));
        if (rlen > sizeof(rsp)) rlen = sizeof(rsp);   /* defensive: never read past rsp */

        if (rlen) {
            /* frame: SYNC, NODE(=host 0x00), NUM(=rlen+1), data..., SUM */
            uint8_t nodb = JVS_HOST_ADDR;
            uint8_t numb = (uint8_t)(rlen + 1);
            uint8_t csum = (uint8_t)(nodb + numb);
            for (size_t k = 0; k < rlen; ++k) csum = (uint8_t)(csum + rsp[k]);

            if (op < out_cap) out[op] = JVS_SYNC;   /* SYNC: literal, never escaped */
            op++;
            wr(out, &op, out_cap, nodb);
            wr(out, &op, out_cap, numb);
            for (size_t k = 0; k < rlen; ++k) wr(out, &op, out_cap, rsp[k]);
            wr(out, &op, out_cap, csum);
        }

        /* remove consumed bytes */
        memmove(n->rx, n->rx + consumed, n->rx_len - consumed);
        n->rx_len -= consumed;
        if (n->rx_len == 0) break;
    }

    return op;
}
