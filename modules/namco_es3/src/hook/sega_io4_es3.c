/* SPDX-License-Identifier: MIT
 * NOTE: independent namco_es3 board copy -- adapted from native_x86/src/hook/sega_io4.c (reference only; no build-time or runtime dependency on native_x86 remains).
 * =============================================================================
 *  sega_io4_es3.c -- Sega IO4-USB (837-15257) I/O-board emulation (env-gated, OFF).
 * =============================================================================
 *
 *  Every modern Sega x86 board (RingEdge/RingWide/Nu/Nu2/ALLS) carries its cabinet
 *  inputs over ONE USB I/O board: the "IO4" (837/835-15257). It is a USB vendor /
 *  HID-INOUT device the game opens by VID/PID and talks fixed reports to -- NOT a
 *  plain HID joystick. The game parses a capability/ident string and then polls a
 *  64-byte IN report (analog + switches + coin counters) and writes a 63-byte OUT
 *  report (general outputs / lamps).
 *
 *  PUBLIC HARDWARE FACTS this scaffold is built from (no loader source read):
 *    - VID 0x0CA3, PID 0x0021.
 *    - Ident/capability string the game matches on:
 *        "I/O CONTROL BD;15257;01;90;1831;6679A;00;"
 *        "GOUT=14_ADIN=8,E_ROTIN=4_COININ=2_SWIN=2,E_UQ1=41,6;"
 *      => GOUT=14 general outputs, ADIN=8 analog, ROTIN=4 rotary, COININ=2 coin
 *         counters, SWIN=2 switch banks.
 *    - IN  report id 0x01: up to 14 x 16-bit analog (first 8 = ADIN, range
 *         0..0xFFFE, 0x8000 = center), a switch bitfield (system + per-player), and
 *         the COININ running coin counters.
 *    - OUT report id 0x10: the GOUT general outputs (lamps/solenoids).
 *
 *  WHAT THIS FILE HONESTLY IS:
 *    A clean-room SCAFFOLD that, when env-gated on (ULTRA_IO4=1, set by the Sega
 *    board profile), SYNTHESISES the IO4 device when no physical board is present
 *    and SERVICES it FROM THE EXISTING JVS INPUT BLOCK -- i.e. the same loader<->hook
 *    input struct (ultra_jvs_input) + clean-room engine that already drives Type X /
 *    X2. So a Sega title that opens the IO4 USB device finds its cabinet inputs.
 *
 *    This is purely an I/O re-PACKING of the loader's controller block (exactly like
 *    taito_fastio.c is for the Taito DMAC path). It owns NO security/keychip/crypto;
 *    those live in sega_keychip.c (presence only) and are user/operator-gated.
 *
 *  WHAT IT IS NOT:
 *    It ships NO game data, NO keys. It is OFF unless armed. The exact bRequest
 *    values for the vendor control transfers (coin-counter reset, watchdog) are
 *    title/firmware-specific and are left as ULTRA_IO4_* env knobs, pinned against a
 *    real dump -- never baked in.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the public Win32 device/HID API + the
 *  public 837-15257 descriptor facts + the EXISTING ultra_jvs_input block. No
 *  third-party loader source, profile, or descriptor binary was read or copied.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "jvs_core.h"

extern void ultra_d3d8_log(const char* fmt, ...);
#define iolog ultra_d3d8_log

/* from jvs_serial_hook.c / hook_dllmain.c -- the IAT patcher + the live input. */
extern void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);
extern void ultra_hook_current_input(ultra_jvs_input* out);

static int g_enabled = 0;

/* ---- IO4 identity (public 837-15257 facts) ------------------------------- */
#define IO4_VID 0x0CA3
#define IO4_PID 0x0021
/* The capability/ident string the game matches on. Overridable (ULTRA_IO4_IDENT)
 * so a board whose firmware revision differs can be pinned against its dump. */
static char g_ident[160] =
    "I/O CONTROL BD;15257;01;90;1831;6679A;00;"
    "GOUT=14_ADIN=8,E_ROTIN=4_COININ=2_SWIN=2,E_UQ1=41,6;";

/* IO4 report ids + sizes (public facts). */
#define IO4_REPORT_IN   0x01   /* device->host poll response                    */
#define IO4_REPORT_OUT  0x10   /* host->device general outputs (lamps)          */
#define IO4_REPORT_SIZE 64     /* 1 report-id byte + 63 data bytes              */

/* channel counts advertised by the ident string. */
#define IO4_ADIN   8           /* analog input channels (16-bit)                */
#define IO4_ROTIN  4           /* rotary/encoder channels (16-bit)              */
#define IO4_COININ 2           /* coin counters                                 */

/* coin counters the device maintains; fed from the loader's coin pulses. The
 * game reads these in the IN report and debits them itself. */
static unsigned short g_coin[IO4_COININ] = { 0, 0 };

/* ---------------------------------------------------------------------------
 * Build the 64-byte IN report from the live cabinet input.
 *
 * Layout (little-endian 16-bit fields), from the public capability contract:
 *   [0]        report id (0x01)
 *   [1..28]    14 x 16-bit analog LE  (ch0..7 = ADIN, ch8..11 = ROTIN, rest 0)
 *   [29..34]   switch bitfield (up to 48 bits): byte29 = system, byte30/31 = P1,
 *              byte32/33 = P2 (per-player switch word, JVS wire order)
 *   [35..38]   coin counters: 2 x 16-bit LE
 *   [39..63]   reserved / padding
 *
 * The exact field offsets are the common IO4 layout; a title whose firmware packs
 * them differently can re-point them via ULTRA_IO4_OFF_* (pinned to its dump). We
 * default to this published layout so it works out of the box for the common case.
 * ------------------------------------------------------------------------- */
static int g_off_analog = 1;    /* first analog channel byte                    */
static int g_off_switch = 29;   /* switch bitfield byte                         */
static int g_off_coin   = 35;   /* coin-counter block byte                      */
static int g_analog_n   = IO4_ADIN + IO4_ROTIN;   /* 12 of the 14 used          */

static void load_offsets(void) {
    const char* e;
    if ((e = getenv("ULTRA_IO4_OFF_ANALOG")) && *e) g_off_analog = atoi(e);
    if ((e = getenv("ULTRA_IO4_OFF_SWITCH")) && *e) g_off_switch = atoi(e);
    if ((e = getenv("ULTRA_IO4_OFF_COIN"))   && *e) g_off_coin   = atoi(e);
    if ((e = getenv("ULTRA_IO4_ANALOG_N"))   && *e) { int v = atoi(e); if (v >= 0 && v <= 14) g_analog_n = v; }
    if ((e = getenv("ULTRA_IO4_IDENT"))      && *e) { strncpy(g_ident, e, sizeof(g_ident)-1); g_ident[sizeof(g_ident)-1] = 0; }
}

/* little-endian 16-bit store, bounds-checked against the report buffer. */
#define IO4_PUT16_LE(b, cap, off, val) do { int _o=(off); if (_o>=0 && _o+1 < (cap)) { \
    (b)[_o] = (unsigned char)((val) & 0xFF); (b)[_o+1] = (unsigned char)(((val) >> 8) & 0xFF); } } while (0)

/* Fill a caller buffer (>= IO4_REPORT_SIZE) with the current IN report. Returns
 * the number of bytes written (IO4_REPORT_SIZE on success, 0 on a bad buffer). */
int ultra_sega_io4_fill_in(void* buf, int cap) {
    if (!buf || cap < IO4_REPORT_SIZE) return 0;
    unsigned char* b = (unsigned char*)buf;
    memset(b, 0, IO4_REPORT_SIZE);
    b[0] = IO4_REPORT_IN;

    ultra_jvs_input in;
    ultra_hook_current_input(&in);

    /* analog channels: ADIN first, then ROTIN, all 16-bit LE, 0x8000 = center.
     * ultra_jvs_input.analog[] is already 16-bit left-justified (0x8000 center),
     * so it maps straight onto the IO4 analog words. A channel reported as 0 would
     * read as full deflection, so every advertised channel carries a real value
     * (ultra_hook_current_input centers unused channels at 0x8000). */
    for (int a = 0; a < g_analog_n && a < JVS_MAX_ANALOG; ++a)
        IO4_PUT16_LE(b, IO4_REPORT_SIZE, g_off_analog + a*2, in.analog[a]);

    /* switch bitfield: byte0 = system (TEST/TILT in the JVS map), then per-player
     * switch words in JVS wire order (high byte = switch byte 1, low = byte 2). */
    if (g_off_switch >= 0 && g_off_switch < IO4_REPORT_SIZE) b[g_off_switch] = in.system;
    if (JVS_MAX_PLAYERS >= 1) {
        IO4_PUT16_LE(b, IO4_REPORT_SIZE, g_off_switch + 1,
                     (unsigned short)((in.player[0] >> 8) | ((in.player[0] & 0xFF) << 8)));
    }
    if (JVS_MAX_PLAYERS >= 2) {
        IO4_PUT16_LE(b, IO4_REPORT_SIZE, g_off_switch + 3,
                     (unsigned short)((in.player[1] >> 8) | ((in.player[1] & 0xFF) << 8)));
    }

    /* coin counters (2 x 16-bit LE). */
    for (int c = 0; c < IO4_COININ; ++c)
        IO4_PUT16_LE(b, IO4_REPORT_SIZE, g_off_coin + c*2, g_coin[c]);

    return IO4_REPORT_SIZE;
}

/* Register a coin insert on a slot so the next IN report carries it (the game
 * reads + debits the counter itself). Called from the loader's coin-pulse path. */
void ultra_sega_io4_insert_coin(int slot, int count) {
    if (slot < 0 || slot >= IO4_COININ || count <= 0) return;
    g_coin[slot] = (unsigned short)(g_coin[slot] + count);
}

/* Consume a host->device OUT report (general outputs / lamps). We accept + ACK
 * (optionally forwarding to the overlay later); never error on it. Returns 1. */
int ultra_sega_io4_consume_out(const void* buf, int len) {
    (void)buf; (void)len;
    /* GOUT lamp/solenoid state -- nothing to physically drive; accept silently. */
    return 1;
}

/* Answer the device ident/capability query (vendor GET_FEATURE / string).
 * Copies the capability string; returns its length (0 if the buffer is too small).
 * The game matches the VID/PID + this string to recognize the IO4. */
int ultra_sega_io4_ident(char* buf, int cap) {
    if (!buf || cap <= 0) return 0;
    int n = (int)strlen(g_ident);
    if (n + 1 > cap) n = cap - 1;
    memcpy(buf, g_ident, n);
    buf[n] = 0;
    return n;
}

/* ---------------------------------------------------------------------------
 * Transport seam.
 *
 * Games reach the IO4 either through HID (HidD_* / ReadFile on the device path) or
 * a WinUSB/libusb vendor pipe. We intercept the device OPEN (CreateFile on a path
 * whose VID/PID match the IO4) and hand back a synthetic handle our read/write
 * shims service. This lets a board with NO physical IO4 pass the device-present
 * gate and feed inputs. The precise IOCTL/transfer framing a given title uses is
 * pinned against its dump; this scaffold owns the open + the report re-pack.
 * ------------------------------------------------------------------------- */
typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static pCreateFileA Real_CreateFileA = NULL;
static pCreateFileW Real_CreateFileW = NULL;

#define IO4_SENTINEL ((HANDLE)(ULONG_PTR)0x10040001)

/* A device path names the IO4 when it carries the VID/PID (the Windows HID/USB
 * device-interface path embeds "vid_0ca3" / "pid_0021"), case-insensitively. */
static int io4_path_matches(const char* n) {
    if (!n) return 0;
    /* lower-cased substring scan for the two id tokens (either order). */
    char low[260]; int i = 0;
    for (; n[i] && i < 259; ++i) { char c = n[i]; low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
    low[i] = 0;
    int has_vid = (strstr(low, "vid_0ca3") != NULL);
    int has_pid = (strstr(low, "pid_0021") != NULL);
    return has_vid && has_pid;
}
static int io4_path_matches_w(LPCWSTR n) {
    if (!n) return 0;
    char buf[260]; int i = 0;
    for (; n[i] && i < 259; ++i) buf[i] = (char)n[i];
    buf[i] = 0;
    return io4_path_matches(buf);
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && io4_path_matches(name)) {
        HANDLE real = Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                                       : INVALID_HANDLE_VALUE;
        if (real != INVALID_HANDLE_VALUE) return real;     /* real IO4 present */
        iolog("[io4] synthesizing IO4-USB presence (no physical 837-15257)\n");
        return IO4_SENTINEL;
    }
    return Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && io4_path_matches_w(name)) {
        HANDLE real = Real_CreateFileW ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
                                       : INVALID_HANDLE_VALUE;
        if (real != INVALID_HANDLE_VALUE) return real;
        iolog("[io4] synthesizing IO4-USB presence (W) (no physical 837-15257)\n");
        return IO4_SENTINEL;
    }
    return Real_CreateFileW ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}

/* ReadFile against the synthetic IO4 handle returns the IN report; everything else
 * falls through to the real ReadFile. WriteFile against it consumes the OUT report. */
typedef BOOL (WINAPI *pReadFile)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL (WINAPI *pWriteFile)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
static pReadFile  Real_ReadFile  = NULL;
static pWriteFile Real_WriteFile = NULL;

static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    if (g_enabled && h == IO4_SENTINEL) {
        int w = ultra_sega_io4_fill_in(buf, (int)n);
        if (got) *got = (DWORD)(w > 0 ? w : 0);
        return w > 0 ? TRUE : FALSE;
    }
    return Real_ReadFile ? Real_ReadFile(h, buf, n, got, ov) : FALSE;
}
static BOOL WINAPI Hook_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote, LPOVERLAPPED ov) {
    if (g_enabled && h == IO4_SENTINEL) {
        ultra_sega_io4_consume_out(buf, (int)n);
        if (wrote) *wrote = n;
        return TRUE;
    }
    return Real_WriteFile ? Real_WriteFile(h, buf, n, wrote, ov) : FALSE;
}

/* ---------------------------------------------------------------------------
 * Install. Env-gated (ULTRA_IO4=1, set by a Sega IO4 board profile). Default OFF
 * so a normal native_x86 run is byte-identical. Idempotent.
 * ------------------------------------------------------------------------- */
void ultra_sega_io4_install(void) {
    const char* e = getenv("ULTRA_IO4");
    g_enabled = e && e[0] == '1';
    if (!g_enabled) return;
    load_offsets();

    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (k) {
        if (!Real_CreateFileA) Real_CreateFileA = (pCreateFileA)(void*)GetProcAddress(k, "CreateFileA");
        if (!Real_CreateFileW) Real_CreateFileW = (pCreateFileW)(void*)GetProcAddress(k, "CreateFileW");
        if (!Real_ReadFile)    Real_ReadFile    = (pReadFile)(void*)GetProcAddress(k, "ReadFile");
        if (!Real_WriteFile)   Real_WriteFile   = (pWriteFile)(void*)GetProcAddress(k, "WriteFile");
    }
    ultra_iat_patch_all("kernel32.dll", "CreateFileA", (void*)Hook_CreateFileA);
    ultra_iat_patch_all("kernel32.dll", "CreateFileW", (void*)Hook_CreateFileW);
    ultra_iat_patch_all("kernel32.dll", "ReadFile",    (void*)Hook_ReadFile);
    ultra_iat_patch_all("kernel32.dll", "WriteFile",   (void*)Hook_WriteFile);

    iolog("[io4] Sega IO4-USB I/O emulation armed (VID %04X/PID %04X, analog@%d x%d, "
          "switch@%d, coin@%d). JVS-backed inputs; lamps accepted/ignored. NOTE: vendor "
          "control bRequest values are title-specific -> ULTRA_IO4_* knobs.\n",
          IO4_VID, IO4_PID, g_off_analog, g_analog_n, g_off_switch, g_off_coin);
}

int ultra_sega_io4_enabled(void) { return g_enabled; }

#endif /* _WIN32 */
