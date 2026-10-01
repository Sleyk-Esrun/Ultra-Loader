/*
 * ftdi_shim.c
 * Ultra Loader - FTDI FT4232H virtual-serial shim for Cruis'n Blast.
 * See ftdi_shim.h for architecture.
 *
 * Implementation notes
 * --------------------
 * We fake THREE things Cruis'n Blast's bootstrap actually touches:
 *
 *   A) libudev.so.{0,1} - the game uses it (not raw ttyUSB enumeration) to
 *      discover which /dev/ttyUSB* is which arcade board. We satisfy the
 *      full enumerate flow it uses:
 *          udev_new
 *          udev_enumerate_new
 *          udev_enumerate_add_match_subsystem(subsys="tty")
 *          udev_enumerate_scan_devices
 *          udev_enumerate_get_list_entry -> list_entry_get_next / get_name
 *          udev_device_new_from_syspath
 *          udev_device_get_devnode              -> "/dev/rio2.<S>", "/dev/uhb.<S>"
 *          udev_device_get_sysattr_value(attr)   -> matches "idVendor"=0403,
 *                                                   "idProduct"=6015,
 *                                                   "product"="RIO2 IO Board" /
 *                                                   "UHB IO Board",
 *                                                   "serial"="RIO2ULTRA0001" / etc.
 *          udev_device_unref, udev_enumerate_unref, udev_unref
 *
 *      Everything else in the libudev API returns a benign no-op so the
 *      cabinet-image hotplug thread (if any) can spin without crashing.
 *
 *   B) libftd2xx.so - an alternate path: some Raw Thrills builds bypass the
 *      /dev/tty node entirely and talk to the FT4232H channels directly via
 *      the D2XX API. Cruis'n Blast doesn't do this by default but it's cheap
 *      to answer, and if the enterprise-image build uses it we're covered.
 *      We satisfy FT_ListDevices, FT_Open, FT_OpenEx, FT_Read, FT_Write,
 *      FT_Close, FT_GetStatus, FT_SetBaudRate, FT_SetBitMode, FT_Purge,
 *      FT_SetTimeouts, FT_SetLatencyTimer.
 *
 *   C) /dev/rio2.<S> and /dev/uhb.<S> open() paths. These are the real
 *      productive path in Cruis'n Blast: the game opens the udev-reported
 *      devnode with open(), then read()s a stream of framed protocol bytes.
 *      We back each open with the RIO2 or UHB producer thread's output pipe.
 *
 * Thread model:
 *   - One producer thread per active FD (RIO2, UHB).
 *   - Producers wake at ~10 ms (100 Hz) and stuff protocol frames into the
 *     "device" side of a Win32 anonymous-pipe pair.
 *   - Game read() drains the "host" side of the pair.
 *   - No mutex needed for the pipe (Win32 pipes are read/write-safe across
 *     threads). Producer/consumer state (last-poll time) is atomic.
 *
 * Frame format (clean-room minimal RIO2 v1, sufficient to keep the game's
 * framer happy while it reads its "input state" packet):
 *   byte 0     : 0xAA sync byte
 *   byte 1     : channel opcode
 *                  0x01 = wheel/pedal/gear (RIO2)
 *                  0x02 = coin/start/service (UHB)
 *                  0x0F = heartbeat
 *   byte 2     : payload length (excluding header + CRC)
 *   byte 3..N  : payload (little-endian)
 *   byte N+1   : 8-bit XOR checksum of bytes 1..N (matches typical RT boards)
 *
 * The payload the game reads for RIO2 opcode 0x01:
 *   +0..1   int16 steering   [-32768..+32767]
 *   +2      uint8 accel      [0..255]
 *   +3      uint8 brake      [0..255]
 *   +4      int8  gear       [-1=reverse, 0=neutral, 1..7]
 *   +5      uint8 buttons    (bit 0=view/horn, 1=start, 2=coin, 3=service)
 *
 * The payload the game reads for UHB opcode 0x02:
 *   +0      uint8 status_flags   (bit 0=coin OK, 1=start OK, 2=service OK,
 *                                 3=ticket dispenser present)
 *   +1      uint8 coin_pulses    (edge count since last read; the game's
 *                                 credit accumulator adds this)
 *   +2      uint8 service_pulses (edge count)
 *   +3      uint8 spare
 *
 * If OmniWheel is not enabled the RIO2 stream is centered (steering=0,
 * accel=0, brake=0, gear=0, buttons=0). Cruis'n Blast will boot straight
 * through attract mode on that stream - the game doesn't need real input
 * to reach the title screen.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include "loader.h"
#include "ftdi_shim.h"
#include "omniwheel_service.h"

/* Optional: consumer of the shared env-var Omni protocol. Modules built with
 * this header get zero-alloc access to the loader's OmniWheel calibration. */
#include "omni_client.h"

/* ==================================================================== */
/* Constants                                                             */
/* ==================================================================== */

#define FTDI_CH_RIO2   0
#define FTDI_CH_UHB    1
#define FTDI_CH_COUNT  2

#define FTDI_FD_BASE   0x3000   /* keep clear of vfs.c's 0x2000..0x2040 pool */
#define FTDI_FD_MAX    8        /* rio2 + uhb + spare (game can reopen)     */

/* Frame timing - the producer sleeps this many ms between frames. 100 Hz is
 * what the real cabinet ships and the CB framer expects steady 10ms cadence. */
#define FTDI_TICK_MS   10

/* Pipe buffer size - one screen of frames worth. Frames are ~10 bytes; a
 * ~2 KB buffer gives ~200 backlogged frames if the game stalls. */
#define FTDI_PIPE_BUF  2048

/* ==================================================================== */
/* Channel descriptor                                                    */
/* ==================================================================== */

typedef struct {
    int          channel;        /* FTDI_CH_RIO2 / FTDI_CH_UHB */
    const char  *devnode;        /* "/dev/rio2.RIO2ULTRA0001" */
    const char  *serial;         /* "RIO2ULTRA0001" */
    const char  *product;        /* "RIO2 IO Board" */
    const char  *syspath;        /* "/sys/class/tty/rio2_..." */
    uint16_t     vid;
    uint16_t     pid;
    HANDLE       read_side;      /* host side (game reads from here) */
    HANDLE       write_side;     /* device side (producer writes here) */
    HANDLE       thread;         /* producer thread */
    volatile LONG stop;          /* set to 1 to stop producer */
    int          refcount;       /* how many FDs point at this channel */
} FtdiChannel;

static FtdiChannel g_channels[FTDI_CH_COUNT];

typedef struct {
    int          in_use;
    int          channel_idx;    /* index into g_channels */
    int          flags;          /* O_NONBLOCK etc - remembered for read() */
} FtdiFd;

static FtdiFd g_fds[FTDI_FD_MAX];

static CRITICAL_SECTION g_lock;
static volatile LONG    g_initialised = 0;

/* ==================================================================== */
/* Small helpers                                                         */
/* ==================================================================== */

static int _strcaseeq(const char *a, const char *b) {
    if (!a || !b) return 0;
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static int _strhas(const char *hay, const char *needle) {
    return hay && needle && strstr(hay, needle) != NULL;
}

/* Windows CreatePipe returns two HANDLEs. We treat "read_side" as the game's
 * end and "write_side" as the producer's end. */
static int _make_pipe(HANDLE *r, HANDLE *w)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    return CreatePipe(r, w, &sa, FTDI_PIPE_BUF) ? 0 : -1;
}

/* ==================================================================== */
/* Protocol producers                                                    */
/* ==================================================================== */

/* Compute the XOR checksum over bytes[start..end-1]. */
static uint8_t _xor_cs(const uint8_t *p, size_t n) {
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= p[i];
    return x;
}

/* Read one snapshot of wheel/pedal state (from OmniWheel or nulls). */
static void _fill_rio2_payload(uint8_t *out)
{
    /* Defaults if no OmniWheel: centered, no pedals, neutral, no buttons. */
    int16_t steer = 0;
    uint8_t accel = 0, brake = 0;
    int8_t  gear  = 0;
    uint8_t btn   = 0;

    if (omni_client_wheel_enabled()) {
        /* Player 0 is the primary wheel - Cruis'n Blast is single-seat. */
        omniwheel_state_t st;
        if (omniwheel_get_state(0, &st) && st.present) {
            /* Normalized [-1,+1] -> int16. clamp to safe range. */
            float s = st.steer;
            if (s < -1.f) s = -1.f; else if (s > 1.f) s = 1.f;
            steer = (int16_t)(s * 32000.f);

            float a = st.accel; if (a < 0.f) a = 0.f; if (a > 1.f) a = 1.f;
            float b = st.brake; if (b < 0.f) b = 0.f; if (b > 1.f) b = 1.f;
            accel = (uint8_t)(a * 255.f);
            brake = (uint8_t)(b * 255.f);

            /* Gear: use absolute if H-shifter present, else derive from
             * shift_up/shift_down paddles. Cruis'n Blast uses paddles. */
            if (st.shift_gear != 0)      gear = st.shift_gear;
            else if (st.shift_up)        gear = 2;   /* upshift edge */
            else if (st.shift_down)      gear = 1;   /* downshift edge */

            /* Buttons: pack OmniWheel's cabinet bits into our 4-bit map. */
            if (st.buttons & 0x0001) btn |= 0x01;   /* view/horn */
            if (st.buttons & 0x0002) btn |= 0x02;   /* start */
            if (st.buttons & 0x0004) btn |= 0x04;   /* coin */
            if (st.buttons & 0x0008) btn |= 0x08;   /* service */
        }
    }

    out[0] = (uint8_t)(steer & 0xff);
    out[1] = (uint8_t)((steer >> 8) & 0xff);
    out[2] = accel;
    out[3] = brake;
    out[4] = (uint8_t)gear;
    out[5] = btn;
}

/* RIO2 producer - writes wheel/pedal frames + heartbeats at 100 Hz. */
static DWORD WINAPI _rio2_producer(LPVOID arg)
{
    FtdiChannel *c = (FtdiChannel*)arg;
    while (!InterlockedCompareExchange(&c->stop, 0, 0)) {
        uint8_t frame[16];
        frame[0] = 0xAA;                       /* sync */
        frame[1] = 0x01;                       /* opcode: wheel/pedal/gear */
        frame[2] = 6;                          /* payload length */
        _fill_rio2_payload(frame + 3);
        frame[9] = _xor_cs(frame + 1, 8);      /* CS over opcode..payload */

        DWORD n = 0;
        WriteFile(c->write_side, frame, 10, &n, NULL);

        Sleep(FTDI_TICK_MS);
    }
    return 0;
}

/* UHB producer - writes "coin OK / service OK / status" heartbeats. Coin
 * drop pulses can be injected via the loader's overlay (FREEPLAY toggles a
 * one-shot pulse; not implemented here yet, defaults to 0). */
static DWORD WINAPI _uhb_producer(LPVOID arg)
{
    FtdiChannel *c = (FtdiChannel*)arg;
    while (!InterlockedCompareExchange(&c->stop, 0, 0)) {
        uint8_t frame[16];
        frame[0] = 0xAA;                       /* sync */
        frame[1] = 0x02;                       /* opcode: coin/start/service */
        frame[2] = 4;                          /* payload length */
        /* status flags: coin OK | start OK | service OK | ticket dispenser */
        frame[3] = 0x0F;
        frame[4] = 0;                          /* coin pulses (edge count) */
        frame[5] = 0;                          /* service pulses */
        frame[6] = 0;                          /* spare */
        frame[7] = _xor_cs(frame + 1, 6);

        DWORD n = 0;
        WriteFile(c->write_side, frame, 8, &n, NULL);

        Sleep(FTDI_TICK_MS);
    }
    return 0;
}

/* ==================================================================== */
/* Channel bring-up                                                       */
/* ==================================================================== */

static void _start_channel(int idx)
{
    FtdiChannel *c = &g_channels[idx];
    if (c->thread) return;                     /* already started */
    if (_make_pipe(&c->read_side, &c->write_side) != 0) {
        loader_log(LOG_WARN, "ftdi_shim: pipe creation failed for ch=%d", idx);
        return;
    }
    c->stop = 0;
    LPTHREAD_START_ROUTINE fn = (idx == FTDI_CH_RIO2)
                              ? _rio2_producer
                              : _uhb_producer;
    DWORD tid = 0;
    c->thread = CreateThread(NULL, 0, fn, c, 0, &tid);
    loader_log(LOG_INFO, "ftdi_shim: started producer '%s' (serial=%s, tid=%lu)",
               c->product, c->serial, (unsigned long)tid);
}

static void _stop_channel(int idx)
{
    FtdiChannel *c = &g_channels[idx];
    if (!c->thread) return;
    InterlockedExchange(&c->stop, 1);
    /* Producer sleeps in 10 ms ticks - waiting 100 ms is enough to catch it. */
    WaitForSingleObject(c->thread, 100);
    CloseHandle(c->thread);   c->thread = NULL;
    CloseHandle(c->read_side);  c->read_side  = NULL;
    CloseHandle(c->write_side); c->write_side = NULL;
}

/* ==================================================================== */
/* Public lifecycle                                                       */
/* ==================================================================== */

void ftdi_shim_init(void)
{
    if (InterlockedCompareExchange(&g_initialised, 1, 0) != 0) return;
    InitializeCriticalSection(&g_lock);

    static char rio2_devnode[64], uhb_devnode[64];
    static char rio2_syspath[128], uhb_syspath[128];
    _snprintf(rio2_devnode, sizeof(rio2_devnode), "/dev/rio2.%s",
              FTDI_SHIM_SERIAL_RIO2);
    _snprintf(uhb_devnode,  sizeof(uhb_devnode),  "/dev/uhb.%s",
              FTDI_SHIM_SERIAL_UHB);
    _snprintf(rio2_syspath, sizeof(rio2_syspath), "/sys/class/tty/rio2_%s",
              FTDI_SHIM_SERIAL_RIO2);
    _snprintf(uhb_syspath,  sizeof(uhb_syspath),  "/sys/class/tty/uhb_%s",
              FTDI_SHIM_SERIAL_UHB);

    g_channels[FTDI_CH_RIO2].channel = FTDI_CH_RIO2;
    g_channels[FTDI_CH_RIO2].devnode = rio2_devnode;
    g_channels[FTDI_CH_RIO2].serial  = FTDI_SHIM_SERIAL_RIO2;
    g_channels[FTDI_CH_RIO2].product = "RIO2 IO Board";
    g_channels[FTDI_CH_RIO2].syspath = rio2_syspath;
    g_channels[FTDI_CH_RIO2].vid     = FTDI_SHIM_VID;
    g_channels[FTDI_CH_RIO2].pid     = FTDI_SHIM_PID_RT;

    g_channels[FTDI_CH_UHB].channel  = FTDI_CH_UHB;
    g_channels[FTDI_CH_UHB].devnode  = uhb_devnode;
    g_channels[FTDI_CH_UHB].serial   = FTDI_SHIM_SERIAL_UHB;
    g_channels[FTDI_CH_UHB].product  = "UHB IO Board";
    g_channels[FTDI_CH_UHB].syspath  = uhb_syspath;
    g_channels[FTDI_CH_UHB].vid      = FTDI_SHIM_VID;
    g_channels[FTDI_CH_UHB].pid      = FTDI_SHIM_PID_RT;

    /* Producers start lazily on first open() so a game that never opens the
     * devices doesn't spin two idle threads. udev enumeration alone doesn't
     * need the pipes; only open() consumes them. */

    loader_log(LOG_INFO, "ftdi_shim: initialized (RIO2=%s, UHB=%s)",
               rio2_devnode, uhb_devnode);
}

void ftdi_shim_shutdown(void)
{
    /* Only the thread that observes g_initialised transitioning 1->0
     * actually tears down. Others return immediately. */
    if (InterlockedCompareExchange(&g_initialised, 0, 1) != 1) return;

    /* Drain producers + close pipes + invalidate FD slots and channel
     * bookkeeping under the lock, so any concurrent read/write/close/ioctl
     * still-in-flight either sees a valid state OR bails out with -EBADF.
     * Only after every producer thread is joined and every FD slot is
     * cleared do we release the critical section - and only then delete
     * it. This closes the F5 race where DeleteCriticalSection could fire
     * while game threads were still calling ftdi_shim_read/close(). */
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < FTDI_CH_COUNT; i++) {
        _stop_channel(i);
        g_channels[i].refcount = 0;
    }
    memset(g_fds,      0, sizeof(g_fds));
    memset(g_channels, 0, sizeof(g_channels));
    LeaveCriticalSection(&g_lock);

    DeleteCriticalSection(&g_lock);
}

/* ==================================================================== */
/* Introspection                                                         */
/* ==================================================================== */

int ftdi_shim_channel_count(void) { return FTDI_CH_COUNT; }

const char *ftdi_shim_channel_devnode(int i) {
    if (i < 0 || i >= FTDI_CH_COUNT) return NULL;
    return g_channels[i].devnode;
}
const char *ftdi_shim_channel_serial(int i) {
    if (i < 0 || i >= FTDI_CH_COUNT) return NULL;
    return g_channels[i].serial;
}
const char *ftdi_shim_channel_product(int i) {
    if (i < 0 || i >= FTDI_CH_COUNT) return NULL;
    return g_channels[i].product;
}

/* ==================================================================== */
/* /dev/ path routing                                                    */
/* ==================================================================== */

int ftdi_shim_owns_path(const char *path)
{
    if (!path) return 0;
    if (strncmp(path, "/dev/rio2", 9) == 0) return 1;
    if (strncmp(path, "/dev/uhb",  8) == 0) return 1;
    return 0;
}

int ftdi_shim_open(const char *path, int flags)
{
    if (!ftdi_shim_owns_path(path)) return -1;
    if (!InterlockedCompareExchange(&g_initialised, 0, 0)) ftdi_shim_init();

    int ch_idx = (strncmp(path, "/dev/rio2", 9) == 0) ? FTDI_CH_RIO2 : FTDI_CH_UHB;

    EnterCriticalSection(&g_lock);
    _start_channel(ch_idx);
    g_channels[ch_idx].refcount++;

    int slot = -1;
    for (int i = 0; i < FTDI_FD_MAX; i++) {
        if (!g_fds[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        LeaveCriticalSection(&g_lock);
        loader_log(LOG_WARN, "ftdi_shim: FD pool exhausted (path=%s)", path);
        return -1;
    }
    g_fds[slot].in_use      = 1;
    g_fds[slot].channel_idx = ch_idx;
    g_fds[slot].flags       = flags;
    LeaveCriticalSection(&g_lock);

    int fd = FTDI_FD_BASE + slot;
    loader_log(LOG_INFO, "ftdi_shim: open(%s) = fd %d (channel=%d)",
               path, fd, ch_idx);
    return fd;
}

int ftdi_shim_is_fd(int fd) {
    return fd >= FTDI_FD_BASE && fd < FTDI_FD_BASE + FTDI_FD_MAX;
}

static FtdiFd *_fd_get(int fd) {
    if (!ftdi_shim_is_fd(fd)) return NULL;
    int i = fd - FTDI_FD_BASE;
    if (!g_fds[i].in_use) return NULL;
    return &g_fds[i];
}

long ftdi_shim_read(int fd, void *buf, size_t count)
{
    FtdiFd *f = _fd_get(fd);
    if (!f) return -9;                          /* -EBADF */
    FtdiChannel *c = &g_channels[f->channel_idx];
    if (!c->read_side) return 0;

    /* O_NONBLOCK: if nothing is pending, return 0 without blocking. Windows
     * anonymous pipes don't natively support PeekNamedPipe reliably in all
     * cases, but they do for CreatePipe outputs. */
    if (f->flags & 0x800 /* O_NONBLOCK */) {
        DWORD avail = 0;
        if (!PeekNamedPipe(c->read_side, NULL, 0, NULL, &avail, NULL)) return 0;
        if (avail == 0) { /* would-block */
            /* return -EAGAIN via the guest errno layer if the caller cares */
            return -11;
        }
        if (avail < count) count = avail;
    }

    DWORD got = 0;
    if (!ReadFile(c->read_side, buf, (DWORD)count, &got, NULL)) return -5; /* -EIO */
    return (long)got;
}

long ftdi_shim_write(int fd, const void *buf, size_t count)
{
    FtdiFd *f = _fd_get(fd);
    if (!f) return -9;
    /* Game writes are typically FFB commands + LED updates. We accept but
     * discard them - the loader's FFB path is served by omniwheel_ffb_*.
     * Log a small sample so protocol reverse-engineering is possible. */
    static int s_writelog = 8;
    if (s_writelog > 0) {
        s_writelog--;
        const uint8_t *p = (const uint8_t*)buf;
        char hex[3*32+1]; hex[0]=0;
        int n = (int)(count < 16 ? count : 16);
        for (int i = 0; i < n; i++) {
            char tmp[4]; _snprintf(tmp, sizeof(tmp), "%02x ", p[i]);
            strncat(hex, tmp, sizeof(hex)-strlen(hex)-1);
        }
        loader_log(LOG_DEBUG, "ftdi_shim: write ch=%d %d bytes: %s%s",
                   f->channel_idx, (int)count, hex, count > 16 ? "..." : "");
    }
    return (long)count;
}

int ftdi_shim_close(int fd)
{
    FtdiFd *f = _fd_get(fd);
    if (!f) return -9;
    int ch = f->channel_idx;
    EnterCriticalSection(&g_lock);
    memset(f, 0, sizeof(*f));
    if (--g_channels[ch].refcount <= 0) {
        _stop_channel(ch);
        g_channels[ch].refcount = 0;
    }
    LeaveCriticalSection(&g_lock);
    return 0;
}

/* Linux termios ioctl request codes - see vfs.c for the same constants. */
#define _TCGETS     0x5401
#define _TCSETS     0x5402
#define _TCSETSW    0x5403
#define _TCSETSF    0x5404
#define _TIOCMGET   0x5415
#define _TIOCMSET   0x5418
#define _TIOCEXCL   0x540C
#define _TIOCNXCL   0x540D
#define _FIONREAD   0x541B

long ftdi_shim_ioctl(int fd, unsigned long req, void *arg)
{
    FtdiFd *f = _fd_get(fd);
    if (!f) return -9;
    switch (req & 0xFFFF) {
    case (_TCGETS   & 0xFFFF):
    case (_TCSETS   & 0xFFFF):
    case (_TCSETSW  & 0xFFFF):
    case (_TCSETSF  & 0xFFFF):
        /* Just zero the termios struct - the game re-sets it, we ignore. */
        if (arg) memset(arg, 0, 60 /* max sizeof struct termios */);
        return 0;
    case (_TIOCMGET & 0xFFFF):
        /* Report DSR + CTS + DCD asserted (line "connected"). */
        if (arg) *(int*)arg = 0x1E0;
        return 0;
    case (_TIOCMSET & 0xFFFF):
    case (_TIOCEXCL & 0xFFFF):
    case (_TIOCNXCL & 0xFFFF):
        return 0;
    case (_FIONREAD & 0xFFFF): {
        FtdiChannel *c = &g_channels[f->channel_idx];
        DWORD avail = 0;
        if (c->read_side)
            PeekNamedPipe(c->read_side, NULL, 0, NULL, &avail, NULL);
        if (arg) *(int*)arg = (int)avail;
        return 0;
    }
    default:
        return 0;   /* accept unknowns silently - real driver does too */
    }
}

/* ==================================================================== */
/* libudev + libftd2xx dlopen / dlsym dispatch                            */
/* ==================================================================== */

int ftdi_shim_is_ftdi_library(const char *name) {
    if (!name) return 0;
    return _strhas(name, "libftd2xx") || _strhas(name, "libftdi");
}
int ftdi_shim_is_udev_library(const char *name) {
    if (!name) return 0;
    return _strhas(name, "libudev");
}

/* Fake dlopen handles - non-NULL sentinels the game caches + hands back. */
#define FTDI_HANDLE_D2XX  ((void*)0xF7D10001)
#define FTDI_HANDLE_UDEV  ((void*)0xF7D10002)

void *ftdi_shim_dlopen(const char *name, int flags)
{
    (void)flags;
    if (!name) return NULL;
    if (!InterlockedCompareExchange(&g_initialised, 0, 0)) ftdi_shim_init();
    if (ftdi_shim_is_udev_library(name)) {
        loader_log(LOG_INFO, "ftdi_shim: dlopen libudev -> synthetic handle");
        return FTDI_HANDLE_UDEV;
    }
    if (ftdi_shim_is_ftdi_library(name)) {
        loader_log(LOG_INFO, "ftdi_shim: dlopen libftd2xx -> synthetic handle");
        return FTDI_HANDLE_D2XX;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*   libudev synthesis                                                */
/* ------------------------------------------------------------------ */
/* We hand back opaque sentinels that carry enough type info to route  */
/* the next dispatch call correctly. Games only compare against NULL. */
typedef struct { int tag; } udev_h;
typedef struct { int tag; int channel_idx; udev_h *first; } udev_enum_h;
typedef struct udev_list_entry_h_s {
    int tag;
    int channel_idx;
    struct udev_list_entry_h_s *next;
} udev_list_entry_h;
typedef struct { int tag; int channel_idx; } udev_device_h;

/* Static singletons - udev's caller unrefs but we're idempotent so the
 * teardown just decrements refcount = no-op on our end. */
static udev_h            g_udev            = { 0xDE01 };
static udev_enum_h       g_udev_enum       = { 0xDE02, -1, NULL };
static udev_list_entry_h g_list_entries[FTDI_CH_COUNT];
static udev_device_h     g_devices     [FTDI_CH_COUNT];

static void *udev_new_stub(void) { return &g_udev; }
static void  udev_unref_stub(void *u) { (void)u; }

static void *udev_enumerate_new_stub(void *udev) {
    (void)udev;
    /* Rewire the linked list each enumerate_new so scan_devices is a full reset */
    for (int i = 0; i < FTDI_CH_COUNT; i++) {
        g_list_entries[i].tag = 0xDE03;
        g_list_entries[i].channel_idx = i;
        g_list_entries[i].next = (i + 1 < FTDI_CH_COUNT) ? &g_list_entries[i+1] : NULL;
        g_devices[i].tag = 0xDE04;
        g_devices[i].channel_idx = i;
    }
    g_udev_enum.first = NULL;
    return &g_udev_enum;
}
static void  udev_enumerate_unref_stub(void *e) { (void)e; }

static int udev_enumerate_add_match_subsystem_stub(void *e, const char *subs) {
    (void)e;
    /* Only "tty" enrols our channels. Any other subsystem: return 0 but skip. */
    if (subs && strcmp(subs, "tty") == 0) {
        g_udev_enum.channel_idx = 0;
    }
    return 0;
}
static int udev_enumerate_add_match_property_stub(void *e, const char *k, const char *v) {
    (void)e; (void)k; (void)v;  return 0;
}
static int udev_enumerate_add_match_sysattr_stub(void *e, const char *k, const char *v) {
    (void)e; (void)k; (void)v;  return 0;
}

static int udev_enumerate_scan_devices_stub(void *e) {
    udev_enum_h *ue = (udev_enum_h*)e;
    if (!ue) return -1;
    if (ue->channel_idx == 0)          /* "tty" was matched */
        ue->first = (udev_h*)&g_list_entries[0];   /* cast-only, opaque */
    return 0;
}

static void *udev_enumerate_get_list_entry_stub(void *e) {
    udev_enum_h *ue = (udev_enum_h*)e;
    return ue ? ue->first : NULL;
}

static void *udev_list_entry_get_next_stub(void *le) {
    udev_list_entry_h *l = (udev_list_entry_h*)le;
    return (l && l->tag == 0xDE03) ? l->next : NULL;
}

static const char *udev_list_entry_get_name_stub(void *le) {
    udev_list_entry_h *l = (udev_list_entry_h*)le;
    if (!l || l->tag != 0xDE03) return NULL;
    return g_channels[l->channel_idx].syspath;   /* what NEW_FROM_SYSPATH expects */
}

static void *udev_device_new_from_syspath_stub(void *u, const char *syspath) {
    (void)u;
    if (!syspath) return NULL;
    for (int i = 0; i < FTDI_CH_COUNT; i++)
        if (strcmp(syspath, g_channels[i].syspath) == 0) return &g_devices[i];
    return NULL;
}
static void  udev_device_unref_stub(void *d) { (void)d; }

static const char *udev_device_get_devnode_stub(void *d) {
    udev_device_h *dev = (udev_device_h*)d;
    if (!dev || dev->tag != 0xDE04) return NULL;
    return g_channels[dev->channel_idx].devnode;
}

static const char *udev_device_get_subsystem_stub(void *d) { (void)d; return "tty"; }

static const char *udev_device_get_syspath_stub(void *d) {
    udev_device_h *dev = (udev_device_h*)d;
    if (!dev || dev->tag != 0xDE04) return NULL;
    return g_channels[dev->channel_idx].syspath;
}

static const char *udev_device_get_sysname_stub(void *d) {
    udev_device_h *dev = (udev_device_h*)d;
    if (!dev || dev->tag != 0xDE04) return NULL;
    return (dev->channel_idx == FTDI_CH_RIO2) ? "rio2_ttyUSB0" : "uhb_ttyUSB1";
}

static const char *udev_device_get_sysattr_value_stub(void *d, const char *attr) {
    udev_device_h *dev = (udev_device_h*)d;
    if (!dev || dev->tag != 0xDE04 || !attr) return NULL;
    static char vidbuf[8], pidbuf[8];
    if (strcmp(attr, "idVendor") == 0) {
        _snprintf(vidbuf, sizeof(vidbuf), "%04x", g_channels[dev->channel_idx].vid);
        return vidbuf;
    }
    if (strcmp(attr, "idProduct") == 0) {
        _snprintf(pidbuf, sizeof(pidbuf), "%04x", g_channels[dev->channel_idx].pid);
        return pidbuf;
    }
    if (strcmp(attr, "product") == 0) return g_channels[dev->channel_idx].product;
    if (strcmp(attr, "serial")  == 0) return g_channels[dev->channel_idx].serial;
    if (strcmp(attr, "manufacturer") == 0) return "Raw Thrills";
    return NULL;
}

static const char *udev_device_get_property_value_stub(void *d, const char *k) {
    /* Alias of sysattr for the property-based lookup path. */
    return udev_device_get_sysattr_value_stub(d, k);
}

static void *udev_device_get_parent_stub(void *d) { return d; }
static void *udev_device_get_parent_with_subsystem_devtype_stub(void *d, const char *s, const char *t) {
    (void)s; (void)t; return d;
}

/* Monitor API - Cruis'n Blast doesn't hotplug at runtime, we no-op it. */
static void *udev_monitor_new_from_netlink_stub(void *u, const char *name) { (void)u; (void)name; return (void*)0xDEAD11; }
static int   udev_monitor_filter_add_match_subsystem_devtype_stub(void *m, const char *a, const char *b) { (void)m; (void)a; (void)b; return 0; }
static int   udev_monitor_enable_receiving_stub(void *m) { (void)m; return 0; }
static int   udev_monitor_get_fd_stub(void *m) { (void)m; return -1; }
static void *udev_monitor_receive_device_stub(void *m) { (void)m; return NULL; }
static void  udev_monitor_unref_stub(void *m) { (void)m; }

/* ------------------------------------------------------------------ */
/*   libftd2xx synthesis                                              */
/* ------------------------------------------------------------------ */

/* FT_STATUS constants - match FTDI headers. 0 = OK. */
#define FT_OK                  0
#define FT_INVALID_HANDLE      1
#define FT_DEVICE_NOT_FOUND    2
#define FT_DEVICE_NOT_OPENED   3

/* Fake FT_HANDLE - small tagged ints (index+1) so 0 stays invalid. */
typedef void* FT_HANDLE;

static uint32_t ft_ListDevices_stub(void *arg1, void *arg2, uint32_t flags) {
    /* Flag NUMBER_ONLY (0x80000000) reports device count. */
    if (flags & 0x80000000UL) {
        if (arg1) *(uint32_t*)arg1 = FTDI_CH_COUNT;
        return FT_OK;
    }
    /* Otherwise fill an array of serials (as strings) at arg1. */
    if (arg1) {
        char **arr = (char**)arg1;
        for (int i = 0; i < FTDI_CH_COUNT; i++) arr[i] = (char*)g_channels[i].serial;
    }
    return FT_OK;
}

static uint32_t ft_Open_stub(int devidx, FT_HANDLE *pHandle) {
    if (devidx < 0 || devidx >= FTDI_CH_COUNT) return FT_DEVICE_NOT_FOUND;
    /* Refcount the channel so a matching FT_Close actually tears it down
     * and drops the producer thread. Without this every FT_Open leaked a
     * hold on the channel: FT_Close was a no-op, so a game that opened +
     * closed the D2XX handle repeatedly kept the producer running forever
     * and eventually _stop_channel could never fire. */
    EnterCriticalSection(&g_lock);
    _start_channel(devidx);
    g_channels[devidx].refcount++;
    LeaveCriticalSection(&g_lock);
    if (pHandle) *pHandle = (FT_HANDLE)(uintptr_t)(devidx + 1);
    return FT_OK;
}

static uint32_t ft_OpenEx_stub(void *arg, uint32_t flags, FT_HANDLE *pHandle) {
    /* Flags: 1=serial, 2=descr, 4=location. Match by string against serial. */
    (void)flags;
    if (!arg) return FT_DEVICE_NOT_FOUND;
    for (int i = 0; i < FTDI_CH_COUNT; i++) {
        if (strcmp((const char*)arg, g_channels[i].serial) == 0 ||
            strcmp((const char*)arg, g_channels[i].product) == 0) {
            EnterCriticalSection(&g_lock);
            _start_channel(i);
            g_channels[i].refcount++;
            LeaveCriticalSection(&g_lock);
            if (pHandle) *pHandle = (FT_HANDLE)(uintptr_t)(i + 1);
            return FT_OK;
        }
    }
    return FT_DEVICE_NOT_FOUND;
}

static uint32_t ft_Close_stub(FT_HANDLE h) {
    int i = (int)((uintptr_t)h) - 1;
    if (i < 0 || i >= FTDI_CH_COUNT) return FT_INVALID_HANDLE;
    /* Mirror of ft_Open/ft_OpenEx: drop the ref we took on open and stop
     * the producer when the last user goes away. */
    EnterCriticalSection(&g_lock);
    if (--g_channels[i].refcount <= 0) {
        _stop_channel(i);
        g_channels[i].refcount = 0;
    }
    LeaveCriticalSection(&g_lock);
    return FT_OK;
}

static uint32_t ft_Read_stub(FT_HANDLE h, void *buf, uint32_t n, uint32_t *pReturned) {
    int i = (int)((uintptr_t)h) - 1;
    if (i < 0 || i >= FTDI_CH_COUNT) return FT_INVALID_HANDLE;
    FtdiChannel *c = &g_channels[i];
    DWORD avail = 0;
    if (c->read_side) PeekNamedPipe(c->read_side, NULL, 0, NULL, &avail, NULL);
    if (avail == 0) { if (pReturned) *pReturned = 0; return FT_OK; }
    if (avail < n) n = avail;
    DWORD got = 0;
    ReadFile(c->read_side, buf, n, &got, NULL);
    if (pReturned) *pReturned = (uint32_t)got;
    return FT_OK;
}

static uint32_t ft_Write_stub(FT_HANDLE h, const void *buf, uint32_t n, uint32_t *pWritten) {
    (void)buf;
    int i = (int)((uintptr_t)h) - 1;
    if (i < 0 || i >= FTDI_CH_COUNT) return FT_INVALID_HANDLE;
    if (pWritten) *pWritten = n;
    return FT_OK;
}

static uint32_t ft_GetStatus_stub(FT_HANDLE h, uint32_t *pRxAvail, uint32_t *pTxAvail, uint32_t *pEvt) {
    int i = (int)((uintptr_t)h) - 1;
    if (i < 0 || i >= FTDI_CH_COUNT) return FT_INVALID_HANDLE;
    DWORD avail = 0;
    if (g_channels[i].read_side)
        PeekNamedPipe(g_channels[i].read_side, NULL, 0, NULL, &avail, NULL);
    if (pRxAvail) *pRxAvail = (uint32_t)avail;
    if (pTxAvail) *pTxAvail = 0;
    if (pEvt)     *pEvt     = 0;
    return FT_OK;
}

static uint32_t ft_zero_ok_stub(void) { return FT_OK; }

/* ------------------------------------------------------------------ */
/*   Dispatch table                                                   */
/* ------------------------------------------------------------------ */

typedef struct { const char *name; void *fn; } SymEntry;

static SymEntry g_udev_syms[] = {
    /* Core */
    { "udev_new",                        (void*)udev_new_stub },
    { "udev_ref",                        (void*)udev_new_stub },
    { "udev_unref",                      (void*)udev_unref_stub },
    /* Enumeration */
    { "udev_enumerate_new",              (void*)udev_enumerate_new_stub },
    { "udev_enumerate_ref",              (void*)udev_enumerate_new_stub },
    { "udev_enumerate_unref",            (void*)udev_enumerate_unref_stub },
    { "udev_enumerate_add_match_subsystem",       (void*)udev_enumerate_add_match_subsystem_stub },
    { "udev_enumerate_add_nomatch_subsystem",     (void*)udev_enumerate_add_match_subsystem_stub },
    { "udev_enumerate_add_match_sysname",         (void*)udev_enumerate_add_match_property_stub },
    { "udev_enumerate_add_match_property",        (void*)udev_enumerate_add_match_property_stub },
    { "udev_enumerate_add_match_sysattr",         (void*)udev_enumerate_add_match_sysattr_stub },
    { "udev_enumerate_add_nomatch_sysattr",       (void*)udev_enumerate_add_match_sysattr_stub },
    { "udev_enumerate_add_match_tag",             (void*)udev_enumerate_add_match_property_stub },
    { "udev_enumerate_add_match_parent",          (void*)udev_enumerate_add_match_property_stub },
    { "udev_enumerate_add_match_is_initialized",  (void*)udev_enumerate_add_match_property_stub },
    { "udev_enumerate_scan_devices",     (void*)udev_enumerate_scan_devices_stub },
    { "udev_enumerate_scan_subsystems",  (void*)udev_enumerate_scan_devices_stub },
    { "udev_enumerate_get_list_entry",   (void*)udev_enumerate_get_list_entry_stub },
    /* List entry */
    { "udev_list_entry_get_next",        (void*)udev_list_entry_get_next_stub },
    { "udev_list_entry_get_name",        (void*)udev_list_entry_get_name_stub },
    { "udev_list_entry_get_value",       (void*)udev_list_entry_get_name_stub },
    { "udev_list_entry_get_by_name",     (void*)udev_list_entry_get_name_stub },
    /* Device */
    { "udev_device_new_from_syspath",    (void*)udev_device_new_from_syspath_stub },
    { "udev_device_new_from_devnum",     (void*)udev_device_new_from_syspath_stub },
    { "udev_device_new_from_subsystem_sysname", (void*)udev_device_new_from_syspath_stub },
    { "udev_device_ref",                 (void*)udev_device_new_from_syspath_stub },
    { "udev_device_unref",               (void*)udev_device_unref_stub },
    { "udev_device_get_devnode",         (void*)udev_device_get_devnode_stub },
    { "udev_device_get_subsystem",       (void*)udev_device_get_subsystem_stub },
    { "udev_device_get_devtype",         (void*)udev_device_get_subsystem_stub },
    { "udev_device_get_syspath",         (void*)udev_device_get_syspath_stub },
    { "udev_device_get_sysname",         (void*)udev_device_get_sysname_stub },
    { "udev_device_get_sysattr_value",   (void*)udev_device_get_sysattr_value_stub },
    { "udev_device_get_property_value",  (void*)udev_device_get_property_value_stub },
    { "udev_device_get_parent",          (void*)udev_device_get_parent_stub },
    { "udev_device_get_parent_with_subsystem_devtype",
      (void*)udev_device_get_parent_with_subsystem_devtype_stub },
    { "udev_device_get_action",          (void*)udev_device_get_subsystem_stub },
    /* Monitor (no-op) */
    { "udev_monitor_new_from_netlink",   (void*)udev_monitor_new_from_netlink_stub },
    { "udev_monitor_filter_add_match_subsystem_devtype",
      (void*)udev_monitor_filter_add_match_subsystem_devtype_stub },
    { "udev_monitor_enable_receiving",   (void*)udev_monitor_enable_receiving_stub },
    { "udev_monitor_get_fd",             (void*)udev_monitor_get_fd_stub },
    { "udev_monitor_receive_device",     (void*)udev_monitor_receive_device_stub },
    { "udev_monitor_unref",              (void*)udev_monitor_unref_stub },
    { NULL, NULL },
};

static SymEntry g_ftd2xx_syms[] = {
    { "FT_ListDevices",       (void*)ft_ListDevices_stub },
    { "FT_Open",              (void*)ft_Open_stub },
    { "FT_OpenEx",            (void*)ft_OpenEx_stub },
    { "FT_Close",             (void*)ft_Close_stub },
    { "FT_Read",              (void*)ft_Read_stub },
    { "FT_Write",             (void*)ft_Write_stub },
    { "FT_GetStatus",         (void*)ft_GetStatus_stub },
    { "FT_SetBaudRate",       (void*)ft_zero_ok_stub },
    { "FT_SetDataCharacteristics", (void*)ft_zero_ok_stub },
    { "FT_SetFlowControl",    (void*)ft_zero_ok_stub },
    { "FT_SetBitMode",        (void*)ft_zero_ok_stub },
    { "FT_Purge",             (void*)ft_zero_ok_stub },
    { "FT_ResetDevice",       (void*)ft_zero_ok_stub },
    { "FT_SetTimeouts",       (void*)ft_zero_ok_stub },
    { "FT_SetLatencyTimer",   (void*)ft_zero_ok_stub },
    { "FT_GetLatencyTimer",   (void*)ft_zero_ok_stub },
    { "FT_SetUSBParameters",  (void*)ft_zero_ok_stub },
    { "FT_SetChars",          (void*)ft_zero_ok_stub },
    { "FT_SetDtr",            (void*)ft_zero_ok_stub },
    { "FT_ClrDtr",            (void*)ft_zero_ok_stub },
    { "FT_SetRts",            (void*)ft_zero_ok_stub },
    { "FT_ClrRts",            (void*)ft_zero_ok_stub },
    { "FT_GetModemStatus",    (void*)ft_zero_ok_stub },
    { "FT_GetQueueStatus",    (void*)ft_zero_ok_stub },
    { "FT_GetDeviceInfo",     (void*)ft_zero_ok_stub },
    { "FT_GetDeviceInfoList", (void*)ft_zero_ok_stub },
    { "FT_CreateDeviceInfoList", (void*)ft_zero_ok_stub },
    { NULL, NULL },
};

void *ftdi_shim_lookup(const char *symbol)
{
    if (!symbol) return NULL;
    if (!InterlockedCompareExchange(&g_initialised, 0, 0)) ftdi_shim_init();
    for (SymEntry *e = g_udev_syms;   e->name; e++)
        if (strcmp(e->name, symbol) == 0) return e->fn;
    for (SymEntry *e = g_ftd2xx_syms; e->name; e++)
        if (strcmp(e->name, symbol) == 0) return e->fn;
    return NULL;
}

/* ==================================================================== */
/* Registration into the loader's dynlink table                          */
/* ==================================================================== */

/* Called by dynlink_register_builtins() during stubs bring-up. Registers all
 * symbols so the ELF's PLT can bind them directly (bypassing dlopen entirely
 * for statically-linked builds that don't use dlopen). */
void ftdi_shim_register(void *ctx)
{
    LoaderCtx *lc = (LoaderCtx*)ctx;
    if (!lc) return;
    if (!InterlockedCompareExchange(&g_initialised, 0, 0)) ftdi_shim_init();
    for (SymEntry *e = g_udev_syms;   e->name; e++)
        sym_register(lc, e->name, (uintptr_t)e->fn);
    for (SymEntry *e = g_ftd2xx_syms; e->name; e++)
        sym_register(lc, e->name, (uintptr_t)e->fn);
    loader_log(LOG_INFO, "ftdi_shim: registered %d udev + %d D2XX symbols",
               (int)(sizeof(g_udev_syms)/sizeof(g_udev_syms[0]) - 1),
               (int)(sizeof(g_ftd2xx_syms)/sizeof(g_ftd2xx_syms[0]) - 1));
}
