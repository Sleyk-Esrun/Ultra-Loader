/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  sega_io4_nu2.c -- Sega Nu2-native IO4-USB (837-15257) I/O-board emulation.
 * =============================================================================
 *
 *  Sega Nu2's board profile marks it BP_IO_IO4_USB -- the ONLY I/O class this
 *  board uses (no JVS-serial). It is a USB vendor/HID-INOUT device the game
 *  opens by VID/PID and talks fixed reports to -- NOT a plain HID joystick.
 *  The game parses a capability/ident string, then polls a 64-byte IN report
 *  (analog + switches + coin counters) and writes a 63-byte OUT report
 *  (general outputs/lamps).
 *
 *  PUBLIC HARDWARE FACTS this file is built from:
 *    - VID 0x0CA3, PID 0x0021.
 *    - Ident/capability string the game matches on:
 *        "I/O CONTROL BD;15257;01;90;1831;6679A;00;"
 *        "GOUT=14_ADIN=8,E_ROTIN=4_COININ=2_SWIN=2,E_UQ1=41,6;"
 *      => GOUT=14 general outputs, ADIN=8 analog, ROTIN=4 rotary, COININ=2
 *         coin counters, SWIN=2 switch banks.
 *    - IN  report id 0x01: up to 14 x 16-bit analog (first 8 = ADIN, range
 *      0..0xFFFE, 0x8000 = center), a switch bitfield (system + per-player),
 *      and the COININ running coin counters.
 *    - OUT report id 0x10: the GOUT general outputs (lamps/solenoids).
 *
 *  WHAT THIS FILE HONESTLY IS:
 *    A clean-room scaffold that, when env-gated on (ULTRA_IO4=1, set
 *    unconditionally by sega_nu2_main.c for this board), SYNTHESISES the IO4
 *    device when no physical board is present and services it from a LOCAL
 *    keyboard + XInput reader owned entirely by this file. This DLL owns its
 *    entire input chain end to end -- no cross-board dependency, no shared
 *    JVS input block, no include/link against native_x86 or sega_nu.
 *
 *    This is purely an I/O re-packing of live cabinet/controller state into
 *    the IO4 wire format. It owns NO security/keychip/crypto; those live in
 *    sega_keychip_nu2.c (presence only) and are user/operator-gated.
 *
 *  WHAT IT IS NOT:
 *    It ships NO game data, NO keys. It is OFF unless armed. The exact
 *    bRequest values for vendor control transfers (coin-counter reset,
 *    watchdog) are title/firmware-specific and are left generic here -- never
 *    baked in.
 *
 *  Env gate: ULTRA_IO4=1 (or ULTRA_SEGA_IO4=1), set unconditionally by
 *  sega_nu2_main.c for this board (byte-safe globally -- other boards don't
 *  set either flag).
 *  Default key map:
 *    TEST=F2  P1 START=1  P1 SERVICE=F1  P1 COIN=5  P2 START=2  P2 COIN=6
 *    P1 UP/DOWN/LEFT/RIGHT = arrow keys   P1 buttons 1-6 = A S D F Z X
 *    P2 UP/DOWN/LEFT/RIGHT = numpad 8/2/4/6   P2 buttons 1-3 = J K L
 *  Both XInput pads (index 0 -> P1, index 1 -> P2) are polled and OR'd on top
 *  of the keyboard state; left stick X -> steering, right/left trigger ->
 *  accel/brake (channels 0/1/2), matching the driving-cabinet-heavy Nu family.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the public Win32 device/HID API,
 *  the public 837-15257 descriptor facts, and the public XInput/Win32 input
 *  APIs. No third-party loader source, profile, or descriptor binary was read
 *  or copied.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <xinput.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sega_nu2_hook.h"

static int g_enabled = 0;

/* ---- IO4 identity (public 837-15257 facts) -------------------------------- */
#define IO4_VID 0x0CA3
#define IO4_PID 0x0021
static const char g_ident[] =
    "I/O CONTROL BD;15257;01;90;1831;6679A;00;"
    "GOUT=14_ADIN=8,E_ROTIN=4_COININ=2_SWIN=2,E_UQ1=41,6;";

#define IO4_REPORT_IN   0x01   /* device->host poll response           */
#define IO4_REPORT_OUT  0x10   /* host->device general outputs (lamps) */
#define IO4_REPORT_SIZE 64     /* 1 report-id byte + 63 data bytes     */

#define IO4_ADIN    8          /* analog input channels (16-bit)       */
#define IO4_ROTIN   4          /* rotary/encoder channels (16-bit)     */
#define IO4_COININ  2          /* coin counters                        */
#define IO4_ANALOG_N (IO4_ADIN + IO4_ROTIN)

/* Layout (little-endian 16-bit fields), the common published IO4 layout:
 *   [0]        report id (0x01)
 *   [1..24]    12 x 16-bit analog LE  (ch0..7 = ADIN, ch8..11 = ROTIN)
 *   [29..34]   switch bitfield: byte29 = system, byte30/31 = P1, byte32/33 = P2
 *   [35..38]   coin counters: 2 x 16-bit LE
 *   [39..63]   reserved / padding
 */
#define IO4_OFF_ANALOG 1
#define IO4_OFF_SWITCH 29
#define IO4_OFF_COIN   35

/* ---- local cabinet-input snapshot (owned entirely by this file) ---------- */
#define NU2_MAX_PLAYERS 2
typedef struct {
    uint8_t  system;                     /* bit7 TEST */
    uint16_t player[NU2_MAX_PLAYERS];    /* JVS-style switch word, see header */
    uint16_t analog[IO4_ANALOG_N];       /* 0x8000 = center                   */
} sn2_cab_input;

static uint16_t g_coin[IO4_COININ] = { 0, 0 };

/* ---- XInput: dynamically resolved, never link-time linked ---------------- */
typedef DWORD (WINAPI *pXInputGetState)(DWORD, XINPUT_STATE*);
static pXInputGetState Real_XInputGetState = NULL;
static int             g_xinput_tried = 0;

static void xinput_init(void) {
    if (g_xinput_tried) return;
    g_xinput_tried = 1;
    static const char* const versions[] = {
        "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll",
        "xinput1_2.dll", "xinput1_1.dll", NULL
    };
    for (int i = 0; versions[i]; ++i) {
        HMODULE m = LoadLibraryA(versions[i]);
        if (!m) continue;
        Real_XInputGetState = (pXInputGetState)(void*)GetProcAddress(m, "XInputGetState");
        if (Real_XInputGetState) {
            ul_sn2_log("io4: XInput resolved via %s\n", versions[i]);
            break;
        }
    }
    if (!Real_XInputGetState) ul_sn2_log("io4: no XInput DLL found -- keyboard only\n");
}

/* OR XInput pad `idx` state into *sw (switch word) and, if non-NULL, write the
 * steering/accel/brake analog channels. Never clears bits the keyboard already
 * set. */
static void xinput_overlay(DWORD idx, uint16_t* sw,
                            uint16_t* steer, uint16_t* accel, uint16_t* brake) {
    xinput_init();
    if (!Real_XInputGetState) return;
    XINPUT_STATE st; memset(&st, 0, sizeof st);
    if (Real_XInputGetState(idx, &st) != ERROR_SUCCESS) return;

    WORD b = st.Gamepad.wButtons;
    uint16_t add = 0;
    if (b & XINPUT_GAMEPAD_START)          add |= 0x8000;   /* START   */
    if (b & XINPUT_GAMEPAD_BACK)           add |= 0x4000;   /* SERVICE */
    if (b & XINPUT_GAMEPAD_DPAD_UP)        add |= 0x2000;   /* UP      */
    if (b & XINPUT_GAMEPAD_DPAD_DOWN)      add |= 0x1000;   /* DOWN    */
    if (b & XINPUT_GAMEPAD_DPAD_LEFT)      add |= 0x0800;   /* LEFT    */
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT)     add |= 0x0400;   /* RIGHT   */
    if (b & XINPUT_GAMEPAD_A)              add |= 0x0200;   /* PUSH1   */
    if (b & XINPUT_GAMEPAD_B)              add |= 0x0100;   /* PUSH2   */
    if (b & XINPUT_GAMEPAD_X)              add |= 0x0080;   /* PUSH3   */
    if (b & XINPUT_GAMEPAD_Y)              add |= 0x0040;   /* PUSH4   */
    if (b & XINPUT_GAMEPAD_LEFT_SHOULDER)  add |= 0x0020;   /* PUSH5   */
    if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) add |= 0x0010;   /* PUSH6   */
    if (sw) *sw |= add;

    /* left stick X -> steering (signed -32768..32767 -> 0x0000..0xFFFF,
     * 0x8000 = center); triggers -> accel/brake (0..255 -> 0..0xFFFF). */
    if (steer) *steer = (uint16_t)((int)st.Gamepad.sThumbLX + 32768);
    if (accel) *accel = (uint16_t)((unsigned)st.Gamepad.bRightTrigger * 257u);
    if (brake) *brake = (uint16_t)((unsigned)st.Gamepad.bLeftTrigger  * 257u);
}

/* ---- keyboard + XInput -> cabinet snapshot -------------------------------- */
static int down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void read_cab_input(sn2_cab_input* out) {
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < IO4_ANALOG_N; ++i) out->analog[i] = 0x8000;

    out->system = down(VK_F2) ? 0x80 : 0x00;   /* TEST */

    uint16_t p1 = 0;
    if (down('1'))        p1 |= 0x8000;   /* START   */
    if (down(VK_F1))       p1 |= 0x4000;   /* SERVICE */
    if (down(VK_UP))        p1 |= 0x2000;
    if (down(VK_DOWN))      p1 |= 0x1000;
    if (down(VK_LEFT))      p1 |= 0x0800;
    if (down(VK_RIGHT))     p1 |= 0x0400;
    if (down('A'))         p1 |= 0x0200;   /* PUSH1 */
    if (down('S'))         p1 |= 0x0100;   /* PUSH2 */
    if (down('D'))         p1 |= 0x0080;   /* PUSH3 */
    if (down('F'))         p1 |= 0x0040;   /* PUSH4 */
    if (down('Z'))         p1 |= 0x0020;   /* PUSH5 */
    if (down('X'))         p1 |= 0x0010;   /* PUSH6 */

    uint16_t p2 = 0;
    if (down('2'))            p2 |= 0x8000;   /* START */
    if (down(VK_NUMPAD8))     p2 |= 0x2000;
    if (down(VK_NUMPAD2))     p2 |= 0x1000;
    if (down(VK_NUMPAD4))     p2 |= 0x0800;
    if (down(VK_NUMPAD6))     p2 |= 0x0400;
    if (down('J'))            p2 |= 0x0200;   /* PUSH1 */
    if (down('K'))            p2 |= 0x0100;   /* PUSH2 */
    if (down('L'))            p2 |= 0x0080;   /* PUSH3 */

    /* coin: '5' P1 / '6' P2, edge-triggered so a held key doesn't flood the
     * counter every poll. */
    static int prev5 = 0, prev6 = 0;
    int cur5 = down('5'), cur6 = down('6');
    if (cur5 && !prev5) g_coin[0]++;
    if (cur6 && !prev6) g_coin[1]++;
    prev5 = cur5; prev6 = cur6;

    /* XInput overlay: pad 0 -> P1 (+ steering/pedals on channels 0/1/2),
     * pad 1 -> P2 (digital only). */
    xinput_overlay(0, &p1, &out->analog[0], &out->analog[1], &out->analog[2]);
    xinput_overlay(1, &p2, NULL, NULL, NULL);

    out->player[0] = p1;
    out->player[1] = p2;
}

/* little-endian 16-bit store, bounds-checked against the report buffer. */
#define IO4_PUT16_LE(b, cap, off, val) do { int _o=(off); if (_o>=0 && _o+1 < (cap)) { \
    (b)[_o] = (unsigned char)((val) & 0xFF); (b)[_o+1] = (unsigned char)(((val) >> 8) & 0xFF); } } while (0)

/* Fill a caller buffer (>= IO4_REPORT_SIZE) with the current IN report.
 * Returns the number of bytes written (IO4_REPORT_SIZE on success, 0 on a bad
 * buffer). */
static int io4_fill_in(void* buf, int cap) {
    if (!buf || cap < IO4_REPORT_SIZE) return 0;
    unsigned char* b = (unsigned char*)buf;
    memset(b, 0, IO4_REPORT_SIZE);
    b[0] = IO4_REPORT_IN;

    sn2_cab_input in;
    read_cab_input(&in);

    for (int a = 0; a < IO4_ANALOG_N; ++a)
        IO4_PUT16_LE(b, IO4_REPORT_SIZE, IO4_OFF_ANALOG + a*2, in.analog[a]);

    b[IO4_OFF_SWITCH] = in.system;
    IO4_PUT16_LE(b, IO4_REPORT_SIZE, IO4_OFF_SWITCH + 1,
                 (unsigned short)((in.player[0] >> 8) | ((in.player[0] & 0xFF) << 8)));
    IO4_PUT16_LE(b, IO4_REPORT_SIZE, IO4_OFF_SWITCH + 3,
                 (unsigned short)((in.player[1] >> 8) | ((in.player[1] & 0xFF) << 8)));

    for (int c = 0; c < IO4_COININ; ++c)
        IO4_PUT16_LE(b, IO4_REPORT_SIZE, IO4_OFF_COIN + c*2, g_coin[c]);

    return IO4_REPORT_SIZE;
}

/* Consume a host->device OUT report (general outputs/lamps). We accept + ACK;
 * never error on it. */
static int io4_consume_out(const void* buf, int len) { (void)buf; (void)len; return 1; }

/* ---------------------------------------------------------------------------
 * Transport seam: intercept CreateFile on a device path whose Windows
 * HID/USB device-interface string carries the IO4's VID/PID, hand back a
 * synthetic handle our read/write shims service. Lets a board with no
 * physical IO4 pass the device-present gate and feed real inputs.
 * ------------------------------------------------------------------------- */
typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef BOOL   (WINAPI *pReadFile)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL   (WINAPI *pWriteFile)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL   (WINAPI *pCloseHandle)(HANDLE);

static pCreateFileA  Real_CreateFileA  = NULL;
static pCreateFileW  Real_CreateFileW  = NULL;
static pReadFile     Real_ReadFile     = NULL;
static pWriteFile    Real_WriteFile    = NULL;
static pCloseHandle  Real_CloseHandle  = NULL;

#define IO4_SENTINEL ((HANDLE)(ULONG_PTR)0x5E6B0003)   /* Nu2's own sentinel */

static int io4_path_matches(const char* n) {
    if (!n) return 0;
    char low[260]; int i = 0;
    for (; n[i] && i < 259; ++i) { char c = n[i]; low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
    low[i] = 0;
    return strstr(low, "vid_0ca3") != NULL && strstr(low, "pid_0021") != NULL;
}
static int io4_path_matches_w(LPCWSTR n) {
    if (!n) return 0;
    char buf[260]; int i = 0;
    for (; n[i] && i < 259; ++i) buf[i] = (char)n[i];
    buf[i] = 0;
    return io4_path_matches(buf);
}

static HANDLE WINAPI Hook_io4_CreateFileA(LPCSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && io4_path_matches(name)) {
        HANDLE real = Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                                       : INVALID_HANDLE_VALUE;
        if (real != INVALID_HANDLE_VALUE) return real;   /* real IO4 present -- prefer it */
        ul_sn2_log("io4: synthesizing IO4-USB presence for %s\n", name);
        return IO4_SENTINEL;
    }
    return Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}
static HANDLE WINAPI Hook_io4_CreateFileW(LPCWSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && io4_path_matches_w(name)) {
        HANDLE real = Real_CreateFileW ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
                                       : INVALID_HANDLE_VALUE;
        if (real != INVALID_HANDLE_VALUE) return real;
        ul_sn2_log("io4: synthesizing IO4-USB presence (W)\n");
        return IO4_SENTINEL;
    }
    return Real_CreateFileW ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}
static BOOL WINAPI Hook_io4_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    if (h == IO4_SENTINEL) {
        int w = io4_fill_in(buf, (int)n);
        if (got) *got = (DWORD)(w > 0 ? w : 0);
        (void)ov;
        return w > 0 ? TRUE : FALSE;
    }
    return Real_ReadFile ? Real_ReadFile(h, buf, n, got, ov) : FALSE;
}
static BOOL WINAPI Hook_io4_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote, LPOVERLAPPED ov) {
    if (h == IO4_SENTINEL) {
        io4_consume_out(buf, (int)n);
        if (wrote) *wrote = n;
        (void)ov;
        return TRUE;
    }
    return Real_WriteFile ? Real_WriteFile(h, buf, n, wrote, ov) : FALSE;
}
static BOOL WINAPI Hook_io4_CloseHandle(HANDLE h) {
    if (h == IO4_SENTINEL) return TRUE;
    return Real_CloseHandle ? Real_CloseHandle(h) : FALSE;
}

void ul_sn2_install_io4(void) {
    if (!ul_sn2_env_on("ULTRA_IO4") && !ul_sn2_env_on("ULTRA_SEGA_IO4")) {
        ul_sn2_log("io4: skipped (ULTRA_IO4!=1)\n");
        return;
    }
    if (g_enabled) return;
    g_enabled = 1;

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
        Real_CreateFileA = (pCreateFileA)(void*)GetProcAddress(k32, "CreateFileA");
        Real_CreateFileW = (pCreateFileW)(void*)GetProcAddress(k32, "CreateFileW");
        Real_ReadFile    = (pReadFile)   (void*)GetProcAddress(k32, "ReadFile");
        Real_WriteFile   = (pWriteFile)  (void*)GetProcAddress(k32, "WriteFile");
        Real_CloseHandle = (pCloseHandle)(void*)GetProcAddress(k32, "CloseHandle");
    }
    ul_sn2_iat_patch_all("kernel32.dll", "CreateFileA", (void*)Hook_io4_CreateFileA);
    ul_sn2_iat_patch_all("kernel32.dll", "CreateFileW", (void*)Hook_io4_CreateFileW);
    ul_sn2_iat_patch_all("kernel32.dll", "ReadFile",    (void*)Hook_io4_ReadFile);
    ul_sn2_iat_patch_all("kernel32.dll", "WriteFile",   (void*)Hook_io4_WriteFile);
    ul_sn2_iat_patch_all("kernel32.dll", "CloseHandle", (void*)Hook_io4_CloseHandle);
    xinput_init();
    (void)g_ident; (void)IO4_VID; (void)IO4_PID;   /* referenced by ident/VID-PID doc + matcher above */
    ul_sn2_log("io4: armed (VID %04X/PID %04X, analog x%d, switch@%d, coin@%d); "
              "keyboard+XInput backed (F2=TEST 1/2=START 5/6=COIN)\n",
              IO4_VID, IO4_PID, IO4_ANALOG_N, IO4_OFF_SWITCH, IO4_OFF_COIN);
}

#endif /* _WIN32 */
