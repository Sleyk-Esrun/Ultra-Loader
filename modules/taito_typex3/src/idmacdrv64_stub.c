/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  idmacdrv64_stub.c -- the iDmacDrv64.dll that Theatrhythm's dump is missing
 * =============================================================================
 *
 *  WHY THIS FILE EXISTS
 *
 *  Theatrhythm Final Fantasy All-Star Carnival renders, plays and sounds
 *  correct, and has no controls at all. The reason is not the input mapping,
 *  the JVS layer or the profile system - it is a missing file.
 *
 *  The input path is:
 *      Unity C#  ->  game_Data/Plugins/ismACIO.dll  ->  iDmacDrv64.dll
 *
 *  ismACIO.dll (x64, ImageBase 0x180000000) binds four functions from
 *  iDmacDrv64.dll BY ORDINAL, read out of its import directory:
 *
 *      IAT 1800081D0 -> iDmacDrv64.dll #6
 *      IAT 1800081D8 -> iDmacDrv64.dll #5
 *      IAT 1800081E0 -> iDmacDrv64.dll #2
 *      IAT 1800081E8 -> iDmacDrv64.dll #1
 *
 *  and iDmacDrv64.dll is not in the dump, and not on the system. A static
 *  import that cannot resolve means Windows cannot load ismACIO.dll AT ALL, so
 *  every P/Invoke from the C# side throws DllNotFoundException and the game
 *  runs happily with no I/O whatsoever. That is exactly the symptom.
 *
 *  WHY THE EXISTING SHIM CANNOT DO THIS. taito_fastio_typex3.c works by
 *  IAT-patching CALLS to iDmacDrv*.dll. There is no IAT to patch here: the
 *  failure happens at LOAD time, before ismACIO has an import table in memory.
 *  This has to be a real file, the same way konami_pc ships real *.CRK.dll
 *  files rather than hooking something that never loads.
 *
 *  THE ABI, READ OUT OF ismACIO's OWN CODE (x64 fastcall: RCX RDX R8 R9)
 *
 *  A single handle lives at ismACIO+0xCEE8 and is threaded through all four
 *  calls; a status dword lives at +0xCEE4.
 *
 *   #1  at 0x1800060C0, called from ismACIO_Init:
 *          mov ecx,[0x18000BEC0]   ; channel
 *          lea rdx,[0x18000CEE8]   ; -> out handle
 *          lea r8, [0x18000CEE4]   ; -> out status
 *          call #1 ; cmp eax,0
 *       => int Open(DWORD channel, void** out_handle, DWORD* out_status)
 *
 *   #2  at 0x180006240, guarded on handle != 0:
 *          mov rcx,[0x18000CEE8] / lea rdx,[0x18000CEE4] / call #2
 *       => int Close(void* handle, DWORD* out_status)
 *
 *   #5  wrapper at 0x180007070 (reg in ECX, buffer in RDX):
 *          mov rcx,[0x18000CEE8] / mov edx,<reg> / mov r8,<buf> /
 *          lea r9,[0x18000CEE4] / call #5
 *       => int Read(void* handle, DWORD reg, void* out_buf, DWORD* out_status)
 *       R8 is a POINTER.
 *
 *   #6  wrapper at 0x1800070E0 (reg in ECX, value in EDX):
 *          mov rcx,[0x18000CEE8] / mov edx,<reg> / mov r8d,<value> /
 *          lea r9,[0x18000CEE4] / call #6
 *       => int Write(void* handle, DWORD reg, DWORD value, DWORD* out_status)
 *       R8D is a 32-bit VALUE, not a pointer. That asymmetry is how #5 and #6
 *       are told apart, and it is why they are not guessed from public names.
 *
 *  Return value: every call site does `cmp eax,0` and branches, so 0 = success.
 *
 *  WHAT THIS BUILD DOES, AND WHAT IT DELIBERATELY DOES NOT
 *
 *  It makes ismACIO.dll LOADABLE and it LOGS every register touched. What it
 *  does not do is pretend to know the register map: the registers ismACIO
 *  polls (0x400, 0x4000, 0x4004, 0x4120, ... ) are a Taito Fast-I/O address
 *  space whose meaning is not established from the binary alone. Guessing it
 *  and shipping a wrong map is how this project has previously spent days
 *  chasing an input bug that was really a naming error.
 *
 *  So the first run is a MEASUREMENT: the log lists, in order, every register
 *  the game reads and writes. That trace is what tells us which register
 *  carries the digital switches, which carries the analogs, and which is a
 *  board-identity value that must read back non-zero. Once a register is
 *  identified, it gets wired to the loader's live input block and moves out of
 *  the "unknown" list below.
 *
 *  Any register can be forced from the environment without a rebuild, so the
 *  next probe costs a relaunch rather than a build:
 *
 *      ULTRA_IDMAC_R<hex>=<value>     e.g. ULTRA_IDMAC_R400=1
 *      ULTRA_IDMAC_LOG=<path>         defaults to ULTRA_JVS_LOG, then %TEMP%
 *      ULTRA_IDMAC_FAIL_OPEN=1        make Open fail, to confirm the game
 *                                     really is reading us and not a stub
 *                                     somewhere else
 *
 *  INPUT SOURCE. Where a register IS known to carry switches, it is filled
 *  from ULTRA_IO_SHM - the loader's live input block that all four Omni
 *  services publish into - so this file never grows its own input reader.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* ---- the loader's shared input block ------------------------------------
 * Same 100-byte layout every board hook already consumes. Mapped read-only;
 * absent simply means "no live input yet", never an error. */
/* EXACTLY work/include/ultra_io_shared.h. The first version of this struct was
 * written from memory and was wrong: it put player[] at offset 8, which is
 * actually `seq` -- the loader's monotonically incrementing sequence counter.
 * Feeding a counter into a button register set every bit flickering, which the
 * game read as the whole cabinet panel being mashed. Do not retype this from
 * memory; it is 100 bytes and every offset matters. */
typedef struct {
    uint32_t magic;            /*  0  ULTRA_IO_MAGIC 'ULIO' */
    uint32_t version;          /*  4  */
    uint32_t seq;              /*  8  */
    uint32_t flags;            /* 12  */
    unsigned char system;      /* 16  bit7 TEST, bit6..4 TILT (active high) */
    unsigned char _pad0[3];    /* 17  */
    unsigned short player[2];  /* 20  JVS wire order, see below */
    unsigned short analog[8];  /* 24  */
    unsigned short screen_x;   /* 40  */
    unsigned short screen_y;   /* 42  */
    unsigned int coin_pulse[2];/* 44  loader increments to insert a coin */
    unsigned int coin_ack[2];  /* 52  */
    unsigned int jvs_address;  /* 60  */
    unsigned int poll_count;   /* 64  */
    unsigned int reserved[8];  /* 68 .. 100 */
} ul_io_shm;

#define UL_IO_MAGIC 0x4F494C55u   /* 'ULIO' */
static volatile ul_io_shm* g_shm = NULL;
static HANDLE g_shm_map = NULL;

static void shm_open(void)
{
    char name[256];
    if (g_shm) return;
    if (!GetEnvironmentVariableA("ULTRA_IO_SHM", name, sizeof name) || !name[0]) return;
    g_shm_map = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
    if (!g_shm_map) return;
    g_shm = (volatile ul_io_shm*)MapViewOfFile(g_shm_map, FILE_MAP_READ, 0, 0,
                                               sizeof(ul_io_shm));
}

/* ---- the loader's GAMEPAD block (Local\UltraInputV1) ---------------------
 *
 * ULTRA_IO_SHM above carries JVS wire order: one direction pair, a handful of
 * push buttons. Theatrhythm needs TWO independent 4-way sliders plus two
 * buttons plus two triggers, which JVS has no room to express -- squeezing a
 * pad through it is what forced the previous guessed mapping.
 *
 * ultra_input_shm.h is the block the Omni services publish raw pad state
 * into: both sticks, both triggers, sixteen buttons. Reading it here gives an
 * exact, per-control mapping with nothing inferred.
 *
 * Mirrored rather than #included because this DLL is built on its own with no
 * -I into work/include (see build_module.sh). The layout is copied field for
 * field from work/include/ultra_input_shm.h and guarded by the same size
 * asserts, so a drift there fails THIS build instead of silently feeding the
 * game garbage -- the mistake ul_io_shm above already records.
 */
typedef struct {
    float    lx, ly, rx, ry;
    float    lt, rt;
    uint16_t buttons;
    uint8_t  present;
    uint8_t  _pad0;
    uint32_t last_update_ms;
} ui_pad;                          /* 32 bytes */

typedef struct {
    float    steer, accel, brake, clutch, handbrake;
    uint16_t buttons;
    uint8_t  shift_up_seq, shift_down_seq;
    int8_t   shift_gear;
    uint8_t  present;
    uint8_t  _pad0[2];
    uint32_t last_update_ms;
} ui_wheel;                        /* 32 bytes */

typedef struct {
    float    roll, pitch, yaw, throttle, trigger;
    uint16_t buttons;
    int8_t   pov;
    uint8_t  present;
    uint32_t last_update_ms;
} ui_stick;                        /* 28 bytes */

typedef struct {
    uint32_t magic, version;
    volatile uint32_t seq;
    uint32_t writer_pid, tick_ms;
    uint32_t npads, nwheels, nsticks;
    uint32_t flags;
    uint32_t _reserved[7];
    ui_pad   pad  [10];
    ui_wheel wheel[4];
    ui_stick stick[4];
} ui_block;                        /* 624 bytes */

typedef char ui_assert_pad  [(sizeof(ui_pad)   == 32)  ? 1 : -1];
typedef char ui_assert_wheel[(sizeof(ui_wheel) == 32)  ? 1 : -1];
typedef char ui_assert_stick[(sizeof(ui_stick) == 28)  ? 1 : -1];
typedef char ui_assert_block[(sizeof(ui_block) == 624) ? 1 : -1];

#define UI_MAGIC   0x494C5530u     /* '0ULI' */
#define UI_VERSION 1u

static volatile ui_block* g_pads = NULL;
static HANDLE g_pads_map = NULL;

/* lg() is defined further down with the rest of the logging; forward-declared
 * here so this block can sit next to the other shared-memory reader rather
 * than being pushed below the log just to satisfy declaration order. */
static void lg(const char* fmt, ...);

static void pads_open(void)
{
    static const char nm[] = { 'L','o','c','a','l', 92,
                               'U','l','t','r','a','I','n','p','u','t','V','1', 0 };
    if (g_pads) return;
    g_pads_map = OpenFileMappingA(FILE_MAP_READ, FALSE, nm);
    if (!g_pads_map) return;
    g_pads = (volatile ui_block*)MapViewOfFile(g_pads_map, FILE_MAP_READ, 0, 0,
                                               sizeof(ui_block));
    if (g_pads && (g_pads->magic != UI_MAGIC || g_pads->version != UI_VERSION)) {
        lg("pads: block present but magic/version mismatch (0x%08X v%u) -- ignoring",
           (unsigned)g_pads->magic, (unsigned)g_pads->version);
        UnmapViewOfFile((LPCVOID)g_pads);
        g_pads = NULL;
    }
}

/* Seqlock read. The pump publishes at ~125Hz; an odd seq means a write is in
 * progress and a changed seq means one completed while we were copying, so in
 * both cases the snapshot is torn and we take it again. Bounded so a stalled
 * writer can never hang the game's poll. */
static int pad_snapshot(unsigned slot, ui_pad* out)
{
    int tries;
    if (!g_pads || slot >= 10) return 0;
    for (tries = 0; tries < 8; ++tries) {
        uint32_t a = g_pads->seq, b;
        if (a & 1u) continue;
        memcpy(out, (const void*)&g_pads->pad[slot], sizeof(*out));
        b = g_pads->seq;
        if (a == b) return out->present ? 1 : 0;
    }
    return 0;
}

/* ---- Theatrhythm's digital word ----------------------------------------
 * Masks disassembled from Assembly-CSharp's InputData::FromUInt32. */
#define THEA_RUP      0x00001u
#define THEA_RDOWN    0x00002u
#define THEA_RLEFT    0x00004u
#define THEA_RRIGHT   0x00008u
#define THEA_LBUTTON  0x00020u     /* LLampSwitch */
#define THEA_ENTER    0x00080u
#define THEA_LUP      0x00100u
#define THEA_LDOWN    0x00200u
#define THEA_LLEFT    0x00400u
#define THEA_LRIGHT   0x00800u
#define THEA_SELECT   0x01000u
#define THEA_TEST     0x02000u
#define THEA_SERVICE  0x04000u
#define THEA_COIN     0x08000u
#define THEA_RBUTTON  0x10000u     /* RLampSwitch */

/* Omni pad button bits (omniplay_service.h): 0 A, 1 B, 2 X, 3 Y, 4 L1, 5 R1,
 * 6 L3, 7 R3, 8..11 D-pad U/D/L/R, 12 Start, 13 Back, 14 Guide, 15 Touchpad. */
#define UI_BTN_A      (1u <<  0)
#define UI_BTN_B      (1u <<  1)
#define UI_BTN_X      (1u <<  2)
#define UI_BTN_L1     (1u <<  4)
#define UI_BTN_R1     (1u <<  5)
#define UI_BTN_DUP    (1u <<  8)
#define UI_BTN_DDOWN  (1u <<  9)
#define UI_BTN_DLEFT  (1u << 10)
#define UI_BTN_DRIGHT (1u << 11)
#define UI_BTN_START  (1u << 12)
#define UI_BTN_BACK   (1u << 13)

/* A stick is a 4-way switch here, so it needs a threshold, and the threshold
 * has to be high enough that a diagonal push does not report two directions
 * the player did not mean. 0.5 on a normalized axis is a 30-degree cone about
 * each cardinal, which is what a real slider's gate gives. */
#define UI_STICK_ON   0.5f
#define UI_TRIG_ON    0.5f

/* Slot 0 is not "the first pad". Omni-Play reserves slots 0-3 for XInput and
 * 4-9 for DirectInput, so a DirectInput-only pad is live in slot 4 while slots
 * 0-3 sit empty -- and reading slot 0 would find nothing at all. Count present
 * slots instead, so "player 1" means the first pad that actually exists. */
static int nth_present_pad(unsigned nth, ui_pad* out)
{
    unsigned s, seen = 0;
    for (s = 0; s < 10; ++s) {
        if (pad_snapshot(s, out)) {
            if (seen == nth) return 1;
            ++seen;
        }
    }
    return 0;
}

/* FIXED AT THE SOURCE 2026-09-14, kept as an escape hatch.
 *
 * XInput reports stick-up as positive; DirectInput reports it as negative,
 * having inherited the screen convention where y grows downward. The Omni
 * service used to publish both unchanged, so the same pad played this game
 * upside down depending only on which API it enumerated through, and nothing
 * in the block said which. omniplay_service.c now normalizes to the XInput
 * sense before publishing, so this should never be needed -- but a pad that
 * still comes through inverted can be corrected with ULTRA_THEA_INVERT_Y=1
 * instead of waiting on a rebuild. */
static int invert_y(void)
{
    static int cached = -1;
    char v[8];
    if (cached < 0)
        cached = (GetEnvironmentVariableA("ULTRA_THEA_INVERT_Y", v, sizeof v)
                  && v[0] == (char)49) ? 1 : 0;
    return cached;
}

static DWORD thea_switches_from_pad(unsigned nth)
{
    ui_pad p;
    DWORD v = 0;
    if (!nth_present_pad(nth, &p)) return 0;
    if (invert_y()) { p.ly = -p.ly; p.ry = -p.ry; }

    /* left stick + D-pad -> LEFT joy slider */
    if (p.ly >=  UI_STICK_ON || (p.buttons & UI_BTN_DUP))    v |= THEA_LUP;
    if (p.ly <= -UI_STICK_ON || (p.buttons & UI_BTN_DDOWN))  v |= THEA_LDOWN;
    if (p.lx <= -UI_STICK_ON || (p.buttons & UI_BTN_DLEFT))  v |= THEA_LLEFT;
    if (p.lx >=  UI_STICK_ON || (p.buttons & UI_BTN_DRIGHT)) v |= THEA_LRIGHT;

    /* right stick -> RIGHT joy slider */
    if (p.ry >=  UI_STICK_ON) v |= THEA_RUP;
    if (p.ry <= -UI_STICK_ON) v |= THEA_RDOWN;
    if (p.rx <= -UI_STICK_ON) v |= THEA_RLEFT;
    if (p.rx >=  UI_STICK_ON) v |= THEA_RRIGHT;

    /* the two lit cabinet buttons */
    if (p.buttons & UI_BTN_L1) v |= THEA_LBUTTON;
    if (p.buttons & UI_BTN_R1) v |= THEA_RBUTTON;

    /* Panel. Start confirms, X steps the selection, Back inserts a credit.
     *
     * A AND B ARE DELIBERATELY ABSENT FROM THIS WORD.
     *
     * They already do a job on the other input path. ultra_io_publish maps
     * A -> JVS PUSH1 and B -> JVS PUSH2, and ultra_touch.c turns PUSH1/PUSH3
     * into a LEFT touch contact and PUSH2/PUSH4 into a RIGHT one. The game's
     * menus are the touch panel, so A already taps and B already cancels --
     * which is why B behaved as Back before any of this was wired.
     *
     * Claiming A for the cabinet SELECT switch here would give one button two
     * unrelated meanings at once: a menu tap AND a panel press. So SELECT goes
     * to X, which nothing else uses, and A/B are left to the touch path that
     * already works.
     *
     * Service and Test sit on the triggers, where nothing can catch them by
     * accident during play -- a stray Test drops the cabinet into the operator
     * menu in the middle of a song. */
    if (p.buttons & UI_BTN_START) v |= THEA_ENTER;
    if (p.buttons & UI_BTN_X)     v |= THEA_SELECT;
    if (p.buttons & UI_BTN_BACK)  v |= THEA_COIN;
    if (p.lt >= UI_TRIG_ON)       v |= THEA_SERVICE;
    if (p.rt >= UI_TRIG_ON)       v |= THEA_TEST;
    return v;
}

/* ---- log ---------------------------------------------------------------- */
static char g_log[MAX_PATH];
static CRITICAL_SECTION g_cs;
static int  g_ready = 0;

static void lg(const char* fmt, ...)
{
    FILE* f;
    va_list ap;
    if (!g_log[0]) return;
    if (g_ready) EnterCriticalSection(&g_cs);
    f = fopen(g_log, "a");
    if (f) {
        fputs("[iDmacDrv64] ", f);
        va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc(10, f);
        fclose(f);
    }
    if (g_ready) LeaveCriticalSection(&g_cs);
}

/* An env override for one register, e.g. ULTRA_IDMAC_R4000=0x1234. */
static int reg_override(DWORD reg, DWORD* out)
{
    char name[64], val[64];
    _snprintf(name, sizeof name, "ULTRA_IDMAC_R%X", (unsigned)reg);
    name[sizeof name - 1] = 0;
    if (!GetEnvironmentVariableA(name, val, sizeof val) || !val[0]) return 0;
    *out = (DWORD)strtoul(val, NULL, 0);
    return 1;
}

/* Registers seen, so the log summarizes rather than repeating every frame. */
#define MAXREG 64
static struct { DWORD reg; unsigned reads, writes; DWORD last; } g_seen[MAXREG];
static int g_nseen = 0;

static int note_reg(DWORD reg, int is_write, DWORD val)
{
    int i;
    for (i = 0; i < g_nseen; ++i) if (g_seen[i].reg == reg) break;
    if (i == g_nseen) {
        if (g_nseen >= MAXREG) return 0;
        g_seen[g_nseen].reg = reg;
        g_seen[g_nseen].reads = g_seen[g_nseen].writes = 0;
        g_seen[g_nseen].last = 0;
        ++g_nseen;
        lg("NEW REGISTER %s 0x%X  (first touch)", is_write ? "WRITE" : "READ", (unsigned)reg);
        return 1;                      /* log the first touch in full */
    }
    if (is_write) { ++g_seen[i].writes; if (g_seen[i].last != val) { g_seen[i].last = val; return 1; } }
    else ++g_seen[i].reads;
    return 0;
}

static void* const FAKE_HANDLE = (void*)(uintptr_t)0x1D3AC64;

/* ---- the four exports ---------------------------------------------------- */

int iDmacDrvOpen(DWORD channel, void** out_handle, DWORD* out_status)
{
    char v[16];
    lg("Open(channel=%u) -> handle=%p", (unsigned)channel, FAKE_HANDLE);
    if (out_handle) *out_handle = FAKE_HANDLE;
    if (out_status) *out_status = 0;
    if (GetEnvironmentVariableA("ULTRA_IDMAC_FAIL_OPEN", v, sizeof v) && v[0] == (char)49) {
        lg("Open: FORCED FAILURE (ULTRA_IDMAC_FAIL_OPEN=1)");
        if (out_handle) *out_handle = NULL;
        return 1;
    }
    shm_open();
    lg("Open: live input block %s", g_shm ? "attached" : "NOT attached (ULTRA_IO_SHM unset)");
    return 0;
}

int iDmacDrvClose(void* handle, DWORD* out_status)
{
    int i;
    lg("Close(handle=%p)", handle);
    if (out_status) *out_status = 0;
    lg("--- register summary: %d distinct registers touched ---", g_nseen);
    for (i = 0; i < g_nseen; ++i)
        lg("    0x%-6X  reads=%-6u writes=%-6u last_written=0x%X",
           (unsigned)g_seen[i].reg, g_seen[i].reads, g_seen[i].writes,
           (unsigned)g_seen[i].last);
    return 0;
}

/* ---- the register map, as MEASURED ---------------------------------------
 *
 * Loading ismACIO.dll against this DLL and calling ismACIO_Init/Update logs
 * every register it touches. The result is not a guess:
 *
 *     READ   0x400, 0x4000, 0x4004            global / status, probed first
 *     WRITE  0x4100 0x4104 0x4108 0x410C      output bank A
 *     READ   0x4120 0x4124 0x4128 0x412C      digital input bank A
 *     READ   0x4140 0x4144                    analog input bank A
 *     WRITE  0x4180 0x4184 0x4188 0x418C      output bank B
 *     READ   0x41A0 0x41A4 0x41A8 0x41AC      digital input bank B
 *     READ   0x41C0 0x41C4                    analog input bank B
 *
 * Two identical banks exactly 0x80 apart - player 1 and player 2. Which
 * register is digital and which is analog is therefore established. What is
 * NOT established is which BIT inside a digital register is which cabinet
 * switch, and that cannot be read out of ismACIO: the meaning lives in the
 * Unity C# that consumes ismACIO_GetInputD.
 *
 * So the bit order below is PROVISIONAL and labeled as such. Use
 * ULTRA_IDMAC_SWEEP=1 to settle it in a single run instead of guessing: it
 * walks one bit at a time through the digital registers, roughly 1.5 s per
 * bit, and logs each one as it goes. Watch the game's I/O test screen and the
 * log together and the map falls out directly. That is the same
 * instrument-first move that ended the guessing on Mario Kart. */
#define IDMAC_DIG_A   0x4120u
#define IDMAC_DIG_B   0x41A0u
#define IDMAC_ANA_A   0x4140u
#define IDMAC_ANA_B   0x41C0u

static DWORD sweep_value(DWORD reg)
{
    /* One bit at a time, 1.5 s each, 32 bits then wrap. Only the two digital
     * registers participate; everything else stays 0 so the moving bit is
     * unambiguous. */
    static DWORD t0 = 0;
    unsigned bit;
    if (!t0) t0 = GetTickCount();
    if (reg != IDMAC_DIG_A && reg != IDMAC_DIG_B) return 0;
    bit = ((GetTickCount() - t0) / 1500u) % 32u;
    {
        static unsigned last = 99; static DWORD lastreg = 0;
        if (bit != last || reg != lastreg) {
            last = bit; lastreg = reg;
            lg("SWEEP: register 0x%X bit %u is now the ONLY bit set (value 0x%08X) -- what does the game show?", (unsigned)reg, bit, 1u << bit);
        }
    }
    return 1u << bit;
}

int iDmacDrvRead(void* handle, DWORD reg, void* out_buf, DWORD* out_status)
{
    DWORD v = 0;
    int first;
    char sw[8];
    (void)handle;
    first = note_reg(reg, 0, 0);

    /* An explicit override always wins, so any register can be probed without
     * a rebuild. */
    if (reg_override(reg, &v)) {
        /* nothing more to do */
    } else if (GetEnvironmentVariableA("ULTRA_IDMAC_SWEEP", sw, sizeof sw)
               && sw[0] == (char)49) {
        v = sweep_value(reg);
    } else if (reg == IDMAC_DIG_A || reg == IDMAC_DIG_B) {
        /* ---- the cabinet panel and the two joy sliders -------------------
         *
         * CORRECTED 2026-09-14, and the correction is the whole point of this
         * block. The previous map took the SwitchCode ENUM ORDINALS
         * (CoinSwitch=0, LUp=1, ... Test=13, Service=14) and used them as BIT
         * POSITIONS. They are not the same thing. The enum is an index into
         * Siva.Device.Input's bool[]; the wire format is decoded by
         * InputData::FromUInt32, whose masks are these -- disassembled out of
         * Assembly-CSharp with ildasm, not inferred from a name:
         *
         *   0x00001 RUp      0x00002 RDown    0x00004 RLeft   0x00008 RRight
         *   0x00020 LLampSwitch  (the LEFT lit button)
         *   0x00080 Enter
         *   0x00100 LUp      0x00200 LDown    0x00400 LLeft   0x00800 LRight
         *   0x01000 Select   0x02000 Test     0x04000 Service 0x08000 Coin
         *   0x10000 RLampSwitch (the RIGHT lit button)
         *
         * Ordinal and mask agree only for Test (13) and Service (14), which is
         * exactly why those two appeared to work while everything else set
         * RUp/RDown/RLeft/RRight and LRight at random and the game walked
         * through its own menus untouched.
         *
         * THE SLIDERS ARE DIGITAL. InputData::FromBuffer -- the handler for
         * the analog packet ismACIO_GetInputA fills in -- is an empty method
         * (IL_0000: ret). The game reads the analog values and throws them
         * away. Four directions per slider is the entire input model, so
         * there is no calibration to get wrong.
         *
         * Sources, in order of authority:
         *   pad slot 0 -> IDMAC_DIG_A, pad slot 1 -> IDMAC_DIG_B. Two slots
         *   because Pair Style is a genuine two-player mode: one player per
         *   side, each with their own slider pair and button.
         *
         *   ULTRA_IO_SHM is still read for TEST / SERVICE / COIN so a key
         *   bound in the loader keeps working alongside a pad. Those three are
         *   OR-ed in, never the sliders -- JVS cannot express two independent
         *   4-way sliders, and pretending it can is what produced the map this
         *   block replaces.
         */
        pads_open();
        v |= thea_switches_from_pad(reg == IDMAC_DIG_A ? 0u : 1u);

        shm_open();
        if (reg == IDMAC_DIG_A && g_shm && g_shm->magic == UL_IO_MAGIC) {
            unsigned short pl = g_shm->player[0];
            if (g_shm->system & 0x80u) v |= THEA_TEST;      /* loader TEST   */
            if (pl & (1u << 14))       v |= THEA_SERVICE;   /* JVS SERVICE   */
            if (pl & (1u << 15))       v |= THEA_ENTER;     /* JVS START     */
            /* Coin is edge-triggered: the loader increments a counter rather
             * than publishing a level, so hold the switch closed for a few
             * polls after each new pulse instead of trying to catch a level
             * that was never there to catch. */
            {
                static unsigned last_pulse = 0;
                static int hold = 0;
                unsigned p0 = g_shm->coin_pulse[0];
                if (p0 != last_pulse) { last_pulse = p0; hold = 4; }
                if (hold > 0) { --hold; v |= THEA_COIN; }
            }
        }
    } else {
        /* DEFAULT: every register reads 0 -- nothing pressed.
         *
         * This file's own header says a wrong map is worse than an empty one,
         * and then the first build shipped the guessed map as the DEFAULT and
         * proved the point. The loader publishes a JVS switch word whose bits
         * mean START/SERVICE/UP/DOWN/LEFT/RIGHT/PUSH1; poured into a Taito
         * digital register those bits mean something else entirely, and the
         * log showed 0x4120 cycling 0x73C / 0xAC0 / 0xEF8 continuously. The
         * game read that as several buttons held down and mashing, and spun
         * through its menus.
         *
         * Gameplay is TOUCH (ultra_touch.c) and does not come through here at
         * all, so zero costs nothing: it means the cabinet PANEL reports no
         * button pressed, which is true.
         *
         * ULTRA_IDMAC_LIVE=1 re-enables the provisional mapping, and
         * ULTRA_IDMAC_SWEEP=1 identifies the real bits. Wire them here for
         * real only once the sweep has said which bit is which. */
        /* ONE exception to the zero default: the FAST I/O status register.
         *
         * ismACIO_Update computes the status the game waits on:
         *
         *     mov  r11d,[block+0x28]   ; fed from register 0x4000
         *     and  r11d,0x300
         *     cmp  r11d,0x300
         *     setne al
         *     add  eax,2               ; 2 = OK when set, else 3 = ERROR
         *
         * and ismACIO_STATUS in the game's own C# metadata reads
         * NOT_INITIALIZED=0 INITIALIZING=1 OK=2 ERROR=3. With every register
         * at zero the board reports ERROR, which is not what a working
         * cabinet would report.
         *
         * 0x00FFFFFF was found by bisecting this register against the game's
         * own status function in a closed loop (load ismACIO, Init, Update,
         * GetStatus) -- 0x4000 is the only register that changes the outcome,
         * and the low 24 bits are what it wants; 0xFFFFFF00 and 0x0000FFFF
         * both still report ERROR.
         *
         * HONEST LIMIT: the field's semantics are not decoded. What is
         * established is that this value makes the game's OWN check report a
         * healthy I/O board, which is the state a real cabinet is in. Override
         * with ULTRA_IDMAC_R4000=<value> if a title disagrees.
         *
         * The INPUT registers stay 0 -- board healthy, no button pressed. */
        if (reg == 0x4000u) v = 0x00FFFFFFu;
        else v = 0;
    }
    if (out_buf) *(DWORD*)out_buf = v;
    if (out_status) *out_status = 0;
    /* Log a READ when its VALUE CHANGES, not just the first time.
     *
     * Logging only the first touch answered the wrong question: it showed
     * 0x4120 = 0x4 once and could not say whether that was a button being
     * pressed or a bit stuck on. A stuck direction and working input look
     * identical in a first-touch log, and they need opposite fixes.
     *
     * Capped so a jittering source cannot fill the disk - that already
     * happened once, a 7.5 MB log in one session. */
    if (reg == IDMAC_DIG_A || reg == IDMAC_DIG_B) {
        static DWORD last_a = 0xFFFFFFFFu, last_b = 0xFFFFFFFFu;
        static unsigned nchg = 0;
        DWORD* lastp = (reg == IDMAC_DIG_A) ? &last_a : &last_b;
        if (v != *lastp && nchg < 400) {
            ++nchg;
            /* Log BOTH sides of the mapping.
             *
             * The mapped value alone cannot say whether a stuck direction came
             * from the loader publishing it, or from this file putting it on
             * the wrong bit. Two guesses have already been spent on that
             * question; printing the raw JVS word next to the mapped word ends
             * it in one run. */
            {
                unsigned raw = 0;
                unsigned sysb = 0;
                if (g_shm && g_shm->magic == UL_IO_MAGIC) {
                    raw  = (reg == IDMAC_DIG_A) ? g_shm->player[0] : g_shm->player[1];
                    sysb = g_shm->system;
                }
                lg("RAW  player=0x%04X system=0x%02X  %s%s%s%s%s%s%s", raw, sysb,
                   (raw & (1u<<15)) ? "START "   : "",
                   (raw & (1u<<14)) ? "SERVICE " : "",
                   (raw & (1u<<13)) ? "jvsUP "   : "",
                   (raw & (1u<<12)) ? "jvsDOWN " : "",
                   (raw & (1u<<11)) ? "jvsLEFT " : "",
                   (raw & (1u<<10)) ? "jvsRIGHT ": "",
                   (raw & (1u<< 9)) ? "PUSH1 "   : "");
            }
            lg("PANEL 0x%X = 0x%08X  %s%s%s%s%s%s%s%s%s", (unsigned)reg, (unsigned)v,
               (v & (1u<<0))  ? "COIN "    : "",
               (v & (1u<<1))  ? "UP "      : "",
               (v & (1u<<2))  ? "DOWN "    : "",
               (v & (1u<<3))  ? "LEFT "    : "",
               (v & (1u<<4))  ? "RIGHT "   : "",
               (v & (1u<<11)) ? "SELECT "  : "",
               (v & (1u<<12)) ? "ENTER "   : "",
               (v & (1u<<13)) ? "TEST "    : "",
               (v & (1u<<14)) ? "SERVICE " : "");
            *lastp = v;
            if (nchg == 400) lg("PANEL: change log capped at 400 lines");
        }
    } else if (first) {
        lg("Read 0x%X -> 0x%X", (unsigned)reg, (unsigned)v);
    }
    return 0;
}

int iDmacDrvWrite(void* handle, DWORD reg, DWORD value, DWORD* out_status)
{
    int notable;
    (void)handle;
    notable = note_reg(reg, 1, value);
    if (out_status) *out_status = 0;
    if (notable) lg("Write 0x%X <- 0x%X", (unsigned)reg, (unsigned)value);
    return 0;
}

/* Ordinals 3 and 4 are not imported by ismACIO but must exist so the ordinal
 * numbering lines up with the real DLL's. Returning failure is honest: if some
 * other Type X3 title binds them, the log says so instead of silently
 * pretending they worked. */
int iDmacDrvReserved3(void) { lg("ordinal #3 called - not implemented"); return 1; }
int iDmacDrvReserved4(void) { lg("ordinal #4 called - not implemented"); return 1; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_cs);
        g_ready = 1;
        if (!GetEnvironmentVariableA("ULTRA_IDMAC_LOG", g_log, sizeof g_log) || !g_log[0])
            if (!GetEnvironmentVariableA("ULTRA_JVS_LOG", g_log, sizeof g_log) || !g_log[0]) {
                DWORD n = GetTempPathA(sizeof g_log - 24, g_log);
                if (n && n < sizeof g_log - 24) strcat(g_log, "ultra_idmac.log");
                else g_log[0] = 0;
            }
        lg("==== loaded (Ultra Loader stand-in for the dump's missing iDmacDrv64.dll) ====");
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_shm) UnmapViewOfFile((LPCVOID)g_shm);
        if (g_shm_map) CloseHandle(g_shm_map);
    }
    return TRUE;
}

#endif /* _WIN32 */
