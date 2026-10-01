/*
 * lindbergh_boards.c - Sega Lindbergh hardware board emulation for Ultra Loader
 *
 * Ported from lindbergh-loader (CC BY-NC-SA): securityBoard.c, baseBoard.c, eeprom.c.
 * Adapted to Ultra Loader's 32-bit Windows VFS + VEH model.
 *
 * THREE devices, reached three ways:
 *   1. Security board - port I/O (IN/OUT instructions) -> handled via VEH here.
 *   2. Baseboard      - ioctl on /dev/lbb   -> lbb_ioctl() (called from vfs_ioctl).
 *   3. EEPROM         - ioctl on /dev/i2c/0 -> eeprom_ioctl() (called from vfs_ioctl).
 *
 * The game reads its DIP/hardware state by POLLING port 0x38 with IN, and reads the
 * EEPROM/baseboard via ioctl. It does NOT expect BSS pre-filled - it populates its own
 * memory from these responses. That is why the old BSS-poke approach failed.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "loader.h"
#include "jvs_input.h"
#include "lindbergh_eeprom_data.h"

/* ================================================================== */
/* Resolution / DIP configuration (640x480 default, no buttons)        */
/* ================================================================== */
/* DIP 4,5,6 encode resolution. 640x480 => all 0 => IN returns 0xFFFFFFFF. */
static int g_dip[9]      = {0};   /* index 1..8 (index 0 unused, per reference) */
static int g_serviceSw   = 0;
static int g_testSw      = 0;

/* Single source of truth for the render resolution. Default is the canonical
 * Lindbergh mode (1024x768), which is a valid DIP-encodable mode for every
 * game. The patcher overrides this per-game (e.g. After Burner Climax is a
 * widescreen title -> 1360x768) and the ULTRA_RES env var overrides everything.
 */
int g_ul_render_width  = 1024;
int g_ul_render_height = 768;

void lindbergh_set_default_resolution(int w, int h)
{
    if (w > 0 && h > 0) { g_ul_render_width = w; g_ul_render_height = h; }
}

/* The 8 DIP-encodable Lindbergh resolutions, in (d4,d5,d6) order. */
static const struct { int w, h, d4, d5, d6; } DIP_MODES[] = {
    { 640,  480,  0,0,0 },
    { 800,  600,  0,0,1 },
    { 1024, 768,  0,1,0 },
    { 1280, 1024, 0,1,1 },
    { 800,  480,  1,0,0 },
    { 1024, 600,  1,0,1 },
    { 1280, 768,  1,1,0 },
    { 1360, 768,  1,1,1 },
};

void lindbergh_boards_set_resolution(int w, int h)
{
    /* Exact match against a real DIP mode. */
    for (unsigned i = 0; i < sizeof(DIP_MODES)/sizeof(DIP_MODES[0]); i++) {
        if (DIP_MODES[i].w == w && DIP_MODES[i].h == h) {
            g_dip[4] = DIP_MODES[i].d4;
            g_dip[5] = DIP_MODES[i].d5;
            g_dip[6] = DIP_MODES[i].d6;
            return;
        }
    }
    /* No exact DIP mode for this resolution. The game decodes these 3 bits to
     * index its internal resolution table, so an unencodable resolution would
     * leave the DIP at 640x480 (mode 0) and re-introduce the mis-index crash.
     * Snap to the nearest encodable mode by area and warn loudly. */
    long target = (long)w * (long)h;
    int best = 2; /* default to 1024x768 if nothing else fits */
    long bestdiff = -1;
    for (unsigned i = 0; i < sizeof(DIP_MODES)/sizeof(DIP_MODES[0]); i++) {
        long diff = (long)DIP_MODES[i].w * DIP_MODES[i].h - target;
        if (diff < 0) diff = -diff;
        if (bestdiff < 0 || diff < bestdiff) { bestdiff = diff; best = (int)i; }
    }
    g_dip[4] = DIP_MODES[best].d4;
    g_dip[5] = DIP_MODES[best].d5;
    g_dip[6] = DIP_MODES[best].d6;
    loader_log(LOG_WARN,
        "[BOARDS] %dx%d is not a DIP-encodable Lindbergh mode; snapping DIP to nearest %dx%d. "
        "Set ULTRA_RES to one of: 640x480 800x600 1024x768 1280x1024 800x480 1024x600 1280x768 1360x768",
        w, h, DIP_MODES[best].w, DIP_MODES[best].h);
}

void lindbergh_boards_set_switch(int test, int service)
{
    g_testSw = test; g_serviceSw = service;
}

/* ================================================================== */
/* SECURITY BOARD - port I/O                                           */
/* ================================================================== */
#define SECURITY_BOARD_FRONT_PANEL          0x38
#define SECURITY_BOARD_FRONT_PANEL_NON_ROOT 0x1038

static uint16_t g_basePortAddress = 0xFFFF;

static void securityBoardIn(uint16_t port, uint32_t *data)
{
    switch (port) {
    case SECURITY_BOARD_FRONT_PANEL_NON_ROOT:
    case SECURITY_BOARD_FRONT_PANEL: {
        uint32_t result = 0xFFFFFFFF;
        if (g_dip[6])    result &= ~0x800;
        if (g_dip[5])    result &= ~0x400;
        if (g_dip[4])    result &= ~0x200;
        if (g_dip[3])    result &= ~0x100;
        if (g_dip[2])    result &= ~0x080;
        if (g_dip[1])    result &= ~0x040;
        if (g_dip[8])    result &= ~0x020;
        if (g_dip[7])    result &= ~0x010;
        if (g_serviceSw) result &= ~0x008;
        if (g_testSw)    result &= ~0x004;
        *data = result;
        break;
    }
    default:
        /* Unknown port: leave EAX as-is (reference returns 0/no-op). */
        break;
    }
}

/* ----- Port-I/O VEH ------------------------------------------------ */
/* Catches IN/OUT privileged-instruction faults in the 32-bit ELF and
 * services them. Must run BEFORE the crash handlers (priority 1, installed
 * after them so it is first in the VEH chain - see lindbergh_boards_install). */
static PVOID g_portio_veh = NULL;

static LONG WINAPI portio_veh(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    /* Port I/O from ring-3 raises a privileged-instruction fault. Some
     * configs surface it as ACCESS_VIOLATION; accept both, then verify the
     * opcode so we never swallow an unrelated fault. */
    if (code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT *ctx = ep->ContextRecord;
    uint8_t *eip = (uint8_t *)(uintptr_t)ctx->Eip;
    if (!eip || (uintptr_t)eip < 0x1000) return EXCEPTION_CONTINUE_SEARCH;

    uint8_t op = eip[0];
    /* Skip a 0x66 operand-size prefix if present (16-bit port ops). */
    int len = 1;
    if (op == 0x66) { op = eip[1]; len = 2; }

    /* Cruis'n Blast (Raw Thrills) - LPT-based coin/start/service via UHB. See
     * lpt_shim.c. `lpt_shim_is_lpt_port` is CHEAP (single-range compare); it
     * returns false for everything the Sega security board owns, so the
     * existing Sega code path is untouched for GSEVO/ABC/HOTD4/etc. */
    extern bool    lpt_shim_is_lpt_port(uint16_t);
    extern uint8_t lpt_shim_read(uint16_t);
    extern void    lpt_shim_write(uint16_t, uint8_t);

    switch (op) {
    case 0xEC:   /* IN AL, DX            */
    case 0xED: { /* IN EAX, DX           */
        uint16_t port_full = (uint16_t)(ctx->Edx & 0xFFFF);
        if (lpt_shim_is_lpt_port(port_full)) {
            uint8_t v = lpt_shim_read(port_full);
            ctx->Eax = (ctx->Eax & 0xFFFFFF00u) | v;
            ctx->Eip += len;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        uint16_t port = port_full;
        if (g_basePortAddress == 0xFFFF) g_basePortAddress = port;
        if (port > 0x38) port = (uint16_t)(port - g_basePortAddress);
        uint32_t val = (uint32_t)ctx->Eax;
        securityBoardIn(port, &val);
        ctx->Eax = val;
        ctx->Eip += len;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    case 0xE4:   /* IN AL, imm8          */
    case 0xE5: { /* IN EAX, imm8         */
        uint16_t port_full = eip[len];
        if (lpt_shim_is_lpt_port(port_full)) {
            uint8_t v = lpt_shim_read(port_full);
            ctx->Eax = (ctx->Eax & 0xFFFFFF00u) | v;
            ctx->Eip += len + 1;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        uint16_t port = port_full;
        if (g_basePortAddress == 0xFFFF) g_basePortAddress = port;
        if (port > 0x38) port = (uint16_t)(port - g_basePortAddress);
        uint32_t val = (uint32_t)ctx->Eax;
        securityBoardIn(port, &val);
        ctx->Eax = val;
        ctx->Eip += len + 1; /* opcode + imm8 */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    case 0xEE:   /* OUT DX, AL           */
    case 0xEF: { /* OUT DX, EAX          */
        uint16_t port_full = (uint16_t)(ctx->Edx & 0xFFFF);
        if (lpt_shim_is_lpt_port(port_full))
            lpt_shim_write(port_full, (uint8_t)(ctx->Eax & 0xFF));
        ctx->Eip += len;       /* writes are no-ops otherwise */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    case 0xE6:   /* OUT imm8, AL         */
    case 0xE7: { /* OUT imm8, EAX        */
        uint16_t port_full = eip[len];
        if (lpt_shim_is_lpt_port(port_full))
            lpt_shim_write(port_full, (uint8_t)(ctx->Eax & 0xFF));
        ctx->Eip += len + 1;   /* opcode + imm8 */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    default:
        return EXCEPTION_CONTINUE_SEARCH; /* not a port instruction */
    }
}

/* ================================================================== */
/* EEPROM - /dev/i2c/0 ioctl (flat image over fake SMBus)              */
/* ================================================================== */
#define I2C_GET_FUNCTIONS   0x705
#define I2C_SMBUS_TRANSFER  0x720
#define I2C_SET_SLAVE_MODE  0x703
#define I2C_BUFFER_CLEAR    0x1261
#define I2C_READ            1
#define I2C_SEEK            2
#define I2C_WRITE           3
#define I2C_SMBUS_BLOCK_MAX 32

union i2c_smbus_data {
    uint8_t  byte;
    uint16_t word;
    uint8_t  block[I2C_SMBUS_BLOCK_MAX + 2];
};
struct i2c_smbus_ioctl_data {
    uint8_t  read_write;
    uint8_t  command;
    uint32_t size;
    union i2c_smbus_data *data;
};

/* Writable RAM copy of the verified EEPROM image, with a file mirror so
 * settings/credits persist across runs (like the reference's fopen-backed file). */
static uint8_t g_eeprom[8192];
static uint32_t g_eeprom_size = 0;
static uint32_t g_eeprom_pos  = 0;
static char     g_eeprom_path[MAX_PATH] = {0};

static void eeprom_persist(void)
{
    if (!g_eeprom_path[0]) return;
    FILE *f = fopen(g_eeprom_path, "wb");
    if (f) { fwrite(g_eeprom, 1, g_eeprom_size, f); fclose(f); }
}

void lindbergh_eeprom_init(void)
{
    /* Load embedded hardware-dumped image as the default. */
    g_eeprom_size = LINDBERGH_EEPROM_SIZE;
    if (g_eeprom_size > sizeof(g_eeprom)) g_eeprom_size = sizeof(g_eeprom);
    memcpy(g_eeprom, LINDBERGH_EEPROM_DATA, g_eeprom_size);

    /* If a persisted eeprom.bin exists next to the game, prefer it. */
    char cwd[MAX_PATH];
    if (GetCurrentDirectoryA(sizeof(cwd), cwd)) {
        snprintf(g_eeprom_path, sizeof(g_eeprom_path), "%s\\eeprom.bin", cwd);
        FILE *f = fopen(g_eeprom_path, "rb");
        if (f) {
            uint32_t n = (uint32_t)fread(g_eeprom, 1, sizeof(g_eeprom), f);
            fclose(f);
            if (n >= 512) { g_eeprom_size = n;
                loader_log(LOG_INFO, "[EEPROM] loaded persisted image (%u bytes)", n); }
            else { /* too small/corrupt - restore embedded */
                g_eeprom_size = LINDBERGH_EEPROM_SIZE;
                memcpy(g_eeprom, LINDBERGH_EEPROM_DATA, g_eeprom_size);
            }
        } else {
            eeprom_persist(); /* write the embedded image out for next time */
            loader_log(LOG_INFO, "[EEPROM] wrote default image to %s", g_eeprom_path);
        }
    }
    loader_log(LOG_INFO, "[EEPROM] ready (%u bytes, board ID at 0x0F)", g_eeprom_size);
}

/* ------------------------------------------------------------------ *
 * Guest-pointer safety.
 * The game hands us raw 32-bit pointers inside its ioctl arguments. A
 * mis-decoded request used to make us dereference garbage and take the
 * whole process down (the eeprom_ioctl 0x720 crash). VirtualQuery lets us
 * prove a range is committed and accessible BEFORE we touch it, so a bad
 * pointer becomes a logged no-op instead of an access violation.
 * ------------------------------------------------------------------ */
static int ul_ptr_accessible(const void *ptr, size_t size, int need_write)
{
    if (!ptr || size == 0) return 0;
    uintptr_t a = (uintptr_t)ptr;
    if (a + size < a) return 0;                          /* wrap/overflow */
    uintptr_t end = a + size;
    const DWORD READABLE = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY;
    const DWORD WRITABLE = PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) == 0) return 0;
        if (mbi.State != MEM_COMMIT) return 0;
        DWORD p = mbi.Protect;
        if (p & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        if (!(p & READABLE)) return 0;
        if (need_write && !(p & WRITABLE)) return 0;
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;  /* next region */
    }
    return 1;
}
static inline int ul_ptr_readable(const void *p, size_t n) { return ul_ptr_accessible(p, n, 0); }
static inline int ul_ptr_writable(void *p, size_t n)       { return ul_ptr_accessible(p, n, 1); }

/* SMBus transaction types (the i2c_smbus_ioctl_data.size field) and direction. */
#define UL_SMBUS_WRITE      0
#define UL_SMBUS_READ       1
#define UL_SMBUS_BYTE       1
#define UL_SMBUS_BYTE_DATA  2
#define UL_SMBUS_WORD_DATA  3
#define UL_EEPROM_SLAVE     0x57   /* ABC selects this EEPROM via I2C_SLAVE (0x703) */

static uint32_t g_i2c_slave = UL_EEPROM_SLAVE;   /* last I2C_SLAVE target */

static inline uint8_t eeprom_read_at(uint32_t pos)
{
    if (g_eeprom_size == 0) return 0xFF;
    return g_eeprom[pos % g_eeprom_size];
}
static inline uint32_t eeprom_advance(uint32_t pos)
{
    uint32_t sz = g_eeprom_size ? g_eeprom_size : 1;
    return (pos + 1) % sz;
}

int64_t eeprom_ioctl(uint64_t request, void *argp)
{
    switch ((unsigned int)request) {

    case I2C_GET_FUNCTIONS: {
        if (!ul_ptr_writable(argp, sizeof(uint32_t))) return 0;
        uint32_t *functions = (uint32_t *)argp;
        functions[0] = 0x20000 | 0x40000 | 0x80000 | 0x100000 |
                       0x200000 | 0x400000 | 0x1000000 | 0x2000000 | 0x8000000;
        return 0;
    }

    /* I2C_SLAVE / I2C_SLAVE_FORCE: the 7-bit address is passed BY VALUE in
     * argp (not via a pointer). Remember it so we know which device the
     * following SMBus transfers target. */
    case I2C_SET_SLAVE_MODE:
        g_i2c_slave = (uint32_t)(uintptr_t)argp;
        loader_log(LOG_DEBUG, "[EEPROM] I2C_SLAVE -> 0x%02x", g_i2c_slave & 0xff);
        return 0;

    /* I2C_SMBUS (0x720). After Burner Climax (and the other Lindbergh
     * titles) drive an M24Cxx serial EEPROM at slave 0x57 over SMBus. argp
     * is a `struct i2c_smbus_ioctl_data` (read_write@0, command@1, size@4,
     * data@8) - NOT i2c_rdwr_ioctl_data, which was the old crash. The game's
     * access helpers map cleanly onto M24Cxx semantics:
     *   WRITE BYTE_DATA(cmd=addr_hi, data.byte=addr_lo) -> set 16-bit address
     *   READ  BYTE                                      -> read byte at addr, auto-increment
     * We serve the verified hardware EEPROM image so the game's backup-record
     * checksums validate and init_arcade proceeds (amEepromRead chain). */
    case I2C_SMBUS_TRANSFER: {
        if (!ul_ptr_readable(argp, sizeof(struct i2c_smbus_ioctl_data))) {
            loader_log(LOG_DEBUG, "[EEPROM] SMBUS: argp %p not readable", argp);
            return 0;
        }
        struct i2c_smbus_ioctl_data *s = (struct i2c_smbus_ioctl_data *)argp;
        uint8_t  rw   = s->read_write;
        uint8_t  cmd  = s->command;
        uint32_t size = s->size;
        union i2c_smbus_data *data = s->data;
        if (data && !ul_ptr_writable(data, sizeof(union i2c_smbus_data)))
            data = NULL;                       /* unusable union -> no data I/O */

        switch (size) {
        case UL_SMBUS_BYTE:                    /* current-address read / single write */
            if (rw == UL_SMBUS_READ) {
                if (data) data->byte = eeprom_read_at(g_eeprom_pos);
                g_eeprom_pos = eeprom_advance(g_eeprom_pos);
            } else if (data && g_eeprom_size) {
                g_eeprom[g_eeprom_pos % g_eeprom_size] = data->byte;
                g_eeprom_pos = eeprom_advance(g_eeprom_pos);
                eeprom_persist();
            }
            return 0;

        case UL_SMBUS_BYTE_DATA:
            if (rw == UL_SMBUS_WRITE) {
                /* set 16-bit address pointer: hi = command, lo = data.byte */
                uint32_t lo = data ? data->byte : 0;
                g_eeprom_pos = ((uint32_t)cmd << 8) | lo;
                if (g_eeprom_size) g_eeprom_pos %= g_eeprom_size;
            } else if (data) {                 /* random read at 8-bit offset cmd */
                g_eeprom_pos = cmd;
                data->byte = eeprom_read_at(g_eeprom_pos);
                g_eeprom_pos = eeprom_advance(g_eeprom_pos);
            }
            return 0;

        case UL_SMBUS_WORD_DATA:
            if (rw == UL_SMBUS_READ && data) {
                uint8_t lo = eeprom_read_at(cmd);
                uint8_t hi = eeprom_read_at(cmd + 1);
                data->word = (uint16_t)lo | ((uint16_t)hi << 8);
                g_eeprom_pos = eeprom_advance((uint32_t)cmd + 1);
            }
            return 0;

        default:
            loader_log(LOG_DEBUG, "[EEPROM] SMBUS size=%u rw=%u cmd=0x%02x slave=0x%02x (accepted)",
                       size, rw, cmd, g_i2c_slave & 0xff);
            return 0;
        }
    }

    case I2C_BUFFER_CLEAR:
        return 0;

    default:
        loader_log(LOG_DEBUG, "[EEPROM] unknown ioctl 0x%x", (unsigned)request);
        return 0;
    }
}

/* ================================================================== */
/* BASEBOARD - /dev/lbb ioctl + read/write                             */
/* ================================================================== */
#define SERIAL_STRING            "FE11-X018012022X"

#define BASEBOARD_INIT           0x300
#define BASEBOARD_GET_VERSION    0x8004BC02
#define BASEBOARD_SEEK_SHM       0x400
#define BASEBOARD_READ_SRAM      0x601
#define BASEBOARD_WRITE_SRAM     0x600
#define BASEBOARD_REQUEST        0xC020BC06
#define BASEBOARD_RECEIVE        0xC020BC07
#define BASEBOARD_GET_SERIAL     0x120
#define BASEBOARD_WRITE_FLASH    0x180
#define BASEBOARD_GET_SENSE_LINE 0x210
#define BASEBOARD_PROCESS_JVS    0x220
#define BASEBOARD_READY          0x201

typedef struct { uint32_t srcAddress, srcSize, destAddress, destSize; } BBCommand;
typedef struct { uint32_t *data; uint32_t offset; uint32_t size; } readData_t;
typedef struct { uint32_t offset; uint32_t *data; uint32_t size; } writeData_t;

static BBCommand g_serialCommand = {0};
static BBCommand g_jvsCommand    = {0};   /* src/dest from the last PROCESS_JVS REQUEST */
static uint8_t   g_jvsResp[256]  = {0};   /* JVS response frame, built on REQUEST */
static int       g_jvsRespLen    = 0;
static int       g_jvs_log_budget = 24;   /* throttle bc/JVS request dumps */
static uint8_t   g_sharedMemory[1024 * 32] = {0};
static uint32_t  g_shmIndex   = 0;
static int       g_selectReply = -1;
static uint8_t   g_sram[256 * 1024] = {0};
static char      g_serialString[32]  = SERIAL_STRING;

int lindbergh_baseboard_select_reply(void) { return g_selectReply; }

void lindbergh_baseboard_init(void)
{
    /* Optional SRAM persistence next to the game. */
    char cwd[MAX_PATH], path[MAX_PATH];
    if (GetCurrentDirectoryA(sizeof(cwd), cwd)) {
        snprintf(path, sizeof(path), "%s\\sram.bin", cwd);
        FILE *f = fopen(path, "rb");
        if (f) { fread(g_sram, 1, sizeof(g_sram), f); fclose(f); }
    }
    strcpy(g_serialString, SERIAL_STRING);
    loader_log(LOG_INFO, "[BASEBOARD] ready (serial %s)", g_serialString);
}

static int g_lbb_rw_log = 30;
int64_t lbb_read(void *buf, size_t count)
{
    if (g_lbb_rw_log > 0) { loader_log(LOG_INFO, "[LBB] read idx=0x%x count=%u", g_shmIndex, (unsigned)count); g_lbb_rw_log--; }
    if (g_shmIndex + count > sizeof(g_sharedMemory)) count = sizeof(g_sharedMemory) - g_shmIndex;
    memcpy(buf, &g_sharedMemory[g_shmIndex], count);
    return (int64_t)count;
}

int64_t lbb_write(const void *buf, size_t count)
{
    if (g_shmIndex + count > sizeof(g_sharedMemory)) count = sizeof(g_sharedMemory) - g_shmIndex;
    memcpy(&g_sharedMemory[g_shmIndex], buf, count);
    return (int64_t)count;
}

int64_t lbb_ioctl(uint64_t request, void *argp)
{
    switch ((unsigned int)request) {

    case BASEBOARD_GET_VERSION: {
        uint8_t versionData[4] = {0x00, 0x19, 0x20, 0x07};
        if (argp) memcpy(argp, versionData, 4);
        return 0;
    }

    case BASEBOARD_INIT:
        return 0;

    case BASEBOARD_READY:
        g_selectReply = 0;
        return 0;

    case BASEBOARD_SEEK_SHM:
        g_shmIndex = (uint32_t)(uintptr_t)argp;
        return 0;

    case BASEBOARD_READ_SRAM: {
        readData_t *d = (readData_t *)argp;
        if (d && d->data && d->offset + d->size <= sizeof(g_sram))
            memcpy(d->data, &g_sram[d->offset], d->size);
        return 0;
    }

    case BASEBOARD_WRITE_SRAM: {
        writeData_t *d = (writeData_t *)argp;
        if (d && d->data && d->offset + d->size <= sizeof(g_sram))
            memcpy(&g_sram[d->offset], d->data, d->size);
        return 0;
    }

    case BASEBOARD_REQUEST: {
        uint32_t *d = (uint32_t *)argp;
        if (!d) return 0;
        switch (d[0]) {
        case BASEBOARD_GET_SERIAL:
            g_serialCommand.destAddress = d[1];
            g_serialCommand.destSize    = d[2];
            break;
        case BASEBOARD_WRITE_FLASH:
            loader_log(LOG_WARN, "[BASEBOARD] game tried to write baseboard flash (ignored)");
            break;
        case BASEBOARD_PROCESS_JVS: {
            /* The game wrote a full JVS request frame into baseboard shared memory.
             *   d[1]=srcAddr d[2]=srcSize d[3]=destAddr d[4]=destSize
             * Read it, run it through the JVS emulator now, and stash the response
             * for the matching RECEIVE to copy back (mirrors lindbergh-loader). */
            g_jvsCommand.srcAddress  = d[1];
            g_jvsCommand.srcSize     = d[2];
            g_jvsCommand.destAddress = d[3];
            g_jvsCommand.destSize    = d[4];
            uint32_t src = g_jvsCommand.srcAddress, ssz = g_jvsCommand.srcSize;
            if (src < sizeof(g_sharedMemory) && ssz > 0 && src + ssz <= sizeof(g_sharedMemory)) {
                const uint8_t *req = &g_sharedMemory[src];
                if (g_jvs_log_budget > 0) {
                    char hx[3*40+1]; int n = ssz < 40 ? (int)ssz : 40, o = 0;
                    for (int k = 0; k < n; k++) o += snprintf(hx+o, sizeof(hx)-o, "%02x ", req[k]);
                    loader_log(LOG_INFO, "[JVS/bc] REQUEST src=0x%x sz=%u dst=0x%x: %s",
                               src, ssz, g_jvsCommand.destAddress, hx);
                    g_jvs_log_budget--;
                }
                g_jvsRespLen = jvs_process_frame(req, (int)ssz, g_jvsResp, sizeof(g_jvsResp));
            } else {
                g_jvsRespLen = 0;
            }
            break;
        }
        case BASEBOARD_GET_SENSE_LINE:
            break;
        default:
            loader_log(LOG_DEBUG, "[BASEBOARD] unknown REQUEST cmd 0x%x", d[0]);
        }
        d[0] |= 0xF0000000;   /* ACK - game polls for this */
        return 0;
    }

    case BASEBOARD_RECEIVE: {
        uint32_t *d = (uint32_t *)argp;
        if (!d) return 0;
        switch (d[0] & 0xFFF) {
        case BASEBOARD_GET_SERIAL:
            if (g_serialCommand.destAddress + 96 + (uint32_t)strlen(g_serialString)
                <= sizeof(g_sharedMemory)) {
                memcpy(&g_sharedMemory[g_serialCommand.destAddress + 96],
                       g_serialString, strlen(g_serialString));
            }
            d[1] = 1;
            break;
        case BASEBOARD_GET_SENSE_LINE:
            d[2] = (uint32_t)jvs_sense_line(); /* 3=unaddressed, 1=addressed/done */
            d[1] = 1;
            break;
        case BASEBOARD_PROCESS_JVS: {
            /* Copy the response frame built during REQUEST back into shared memory
             * at the dest the game gave us, and report its length. */
            uint32_t dst = g_jvsCommand.destAddress;
            int len = g_jvsRespLen;
            if (len > 0 && dst < sizeof(g_sharedMemory) &&
                dst + (uint32_t)len <= sizeof(g_sharedMemory)) {
                memcpy(&g_sharedMemory[dst], g_jvsResp, (size_t)len);
                d[2] = dst;
                d[3] = (uint32_t)len;
                d[1] = 1;          /* frame ready */
            } else {
                d[1] = 0;          /* no frame (e.g. broadcast reset) */
            }
            break;
        }
        default:
            loader_log(LOG_DEBUG, "[BASEBOARD] unknown RECEIVE cmd 0x%x", d[0] & 0xFFF);
        }
        d[0] |= 0xF0000000;   /* ACK */
        return 0;
    }

    default:
        loader_log(LOG_DEBUG, "[BASEBOARD] unknown ioctl 0x%x", (unsigned)request);
        return 0;
    }
}

/* ================================================================== */
/* Public install - call once during Lindbergh pre-init                */
/* ================================================================== */
void lindbergh_boards_install(void)
{
    lindbergh_eeprom_init();
    lindbergh_baseboard_init();

    /* ---- Resolve the render resolution -------------------------------------
     * Start from the per-game default (set by the patcher; falls back to the
     * 1024x768 global default), then let ULTRA_RES / ULTRA_WIDTH / ULTRA_HEIGHT
     * override it at runtime so a resolution can be dialled in without a
     * rebuild. This drives the DIP switches the game reads, so it determines
     * the index into the game's internal resolution table. */
    /* The game's INTERNAL render resolution = the per-game DIP native (set by
     * the patcher; ABC = 1360x768). This is what the game actually renders, and
     * it's capped to a real DIP mode (max ~1360x768) - so it MUST stay decoupled
     * from the OUTPUT resolution. ULTRA_RES selects the OUTPUT window (main.c);
     * the loader renders the game at its native res into an offscreen FBO and
     * scales that to the output (any of 480p..4K). ULTRA_RENDER_RES overrides the
     * game's internal res for advanced use (it still snaps to a DIP mode). */
    int rw = g_ul_render_width, rh = g_ul_render_height;
    {
        char buf[64];
        /* ONLY ULTRA_RENDER_RES overrides the game's internal render res - ULTRA_RES
         * selects the OUTPUT window (main.c) and is intentionally NOT read here, so
         * the game ALWAYS renders at its native DIP res (ABC = 1360x768) into the
         * offscreen FBO regardless of the output resolution. The game reads back
         * that fixed viewport as its "screen size" (glx_emu GL_VIEWPORT remap), lays
         * its UI out for it and fills it; we scale-blit the offscreen to the output
         * window at present, so it fills at EVERY output size (720p..4K). */
        DWORD n = GetEnvironmentVariableA("ULTRA_RENDER_RES", buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            int a = 0, b = 0;
            if ((sscanf(buf, "%dx%d", &a, &b) == 2 ||
                 sscanf(buf, "%dX%d", &a, &b) == 2 ||
                 sscanf(buf, "%d %d", &a, &b) == 2) && a > 0 && b > 0) {
                rw = a; rh = b;
            }
        }
    }
    g_ul_render_width = rw; g_ul_render_height = rh;
    lindbergh_boards_set_resolution(rw, rh);
    loader_log(LOG_INFO, "[BOARDS] game render resolution %dx%d", rw, rh);

    loader_log(LOG_INFO,
        "[BOARDS] render resolution = %dx%d (DIP 4,5,6 = %d,%d,%d)",
        rw, rh, g_dip[4], g_dip[5], g_dip[6]);

    /* Install the port-I/O VEH as FIRST handler (priority 1). It is added
     * after the crash handlers, so it sits ahead of them in the chain and
     * services IN/OUT before the crash handler's ExitProcess can fire. */
    g_portio_veh = AddVectoredExceptionHandler(1, portio_veh);
    loader_log(LOG_INFO, "[BOARDS] security/baseboard/eeprom installed; port-I/O VEH @ %p",
               g_portio_veh);
}
