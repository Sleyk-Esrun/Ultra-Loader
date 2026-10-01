/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  sega_io4_allsux.c -- Sega ALLS UX board's OWN Sega IO4-USB (837-15257)
 *                       I/O-board emulation.
 * =============================================================================
 *
 *  Every modern Sega x86 board (RingEdge/RingWide/Nu/Nu2/ALLS) carries its
 *  cabinet inputs over ONE USB I/O board: the "IO4" (837/835-15257). It is a
 *  USB vendor/HID-INOUT device the game opens by VID/PID and talks fixed
 *  reports to -- NOT a plain HID joystick. The game parses a capability/ident
 *  string and then polls a 64-byte IN report (analog + switches + coin
 *  counters) and writes a 63-byte OUT report (general outputs / lamps).
 *
 *  PUBLIC HARDWARE FACTS this file is built from (no loader source read):
 *    - VID 0x0CA3, PID 0x0021.
 *    - Ident/capability string the game matches on:
 *        "I/O CONTROL BD;15257;01;90;1831;6679A;00;"
 *        "GOUT=14_ADIN=8,E_ROTIN=4_COININ=2_SWIN=2,E_UQ1=41,6;"
 *      => GOUT=14 general outputs, ADIN=8 analog, ROTIN=4 rotary, COININ=2
 *         coin counters, SWIN=2 switch banks.
 *    - IN  report id 0x01: up to 14 x 16-bit analog (first 8 = ADIN), a
 *      switch bitfield (system + per-player), and the COININ running coin
 *      counters.
 *    - OUT report id 0x10: the GOUT general outputs (lamps/solenoids).
 *
 *  WHAT THIS FILE HONESTLY IS: a clean-room SCAFFOLD that SYNTHESISES the IO4
 *  device (no physical board present) and services it FROM this board's own
 *  cabinet-input source (ul_ax_current_input(), fed by hook_dllmain_allsux.c's
 *  XInput poll thread). It owns NO security/keychip/crypto -- that lives in
 *  sega_keychip_allsux.c (presence only, DEFER'd on any secret). It ships NO
 *  game data, NO keys. The exact bRequest values for vendor control transfers
 *  (coin-counter reset, watchdog) are title/firmware-specific and are left as
 *  ULTRA_IO4_* env knobs, pinned against a real dump -- never baked in.
 *
 *  CLEAN-ROOM, fully independent: this is the sega_alls_ux module's own copy,
 *  written over the public Win32 device API + the public 837-15257 descriptor
 *  facts + this board's own ultra_jvs_input block. No third-party loader
 *  source, and no dependency (source or binary) on modules/native_x86.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "allsux_hook.h"

static int g_enabled = 0;

/* ---- IO4 identity (public 837-15257 facts) ------------------------------- */
#define IO4_VID 0x0CA3
#define IO4_PID 0x0021
static char g_ident[160] =
    "I/O CONTROL BD;15257;01;90;1831;6679A;00;"
    "GOUT=14_ADIN=8,E_ROTIN=4_COININ=2_SWIN=2,E_UQ1=41,6;";

#define IO4_REPORT_IN   0x01
#define IO4_REPORT_OUT  0x10
#define IO4_REPORT_SIZE 64

#define IO4_ADIN   8
#define IO4_ROTIN  4
#define IO4_COININ 2

static unsigned short g_coin[IO4_COININ] = { 0, 0 };

/* ---------------------------------------------------------------------------
 * Build the 64-byte IN report from the live cabinet input. Layout (from the
 * public capability contract):
 *   [0]        report id (0x01)
 *   [1..28]    14 x 16-bit analog LE  (ch0..7 = ADIN, ch8..11 = ROTIN)
 *   [29..34]   switch bitfield: byte29 = system, byte30/31 = P1, byte32/33 = P2
 *   [35..38]   coin counters: 2 x 16-bit LE
 *   [39..63]   reserved / padding
 * A title whose firmware packs them differently can re-point offsets via
 * ULTRA_IO4_OFF_* (pinned to its dump); default matches the published layout.
 * ------------------------------------------------------------------------- */
static int g_off_analog = 1;
static int g_off_switch = 29;
static int g_off_coin   = 35;
static int g_analog_n   = IO4_ADIN + IO4_ROTIN;

static void load_offsets(void) {
    const char* e;
    if ((e = ul_ax_env("ULTRA_IO4_OFF_ANALOG")) && *e) g_off_analog = atoi(e);
    if ((e = ul_ax_env("ULTRA_IO4_OFF_SWITCH")) && *e) g_off_switch = atoi(e);
    if ((e = ul_ax_env("ULTRA_IO4_OFF_COIN"))   && *e) g_off_coin   = atoi(e);
    if ((e = ul_ax_env("ULTRA_IO4_ANALOG_N"))   && *e) { int v = atoi(e); if (v >= 0 && v <= 14) g_analog_n = v; }
    if ((e = ul_ax_env("ULTRA_IO4_IDENT"))      && *e) { strncpy(g_ident, e, sizeof(g_ident)-1); g_ident[sizeof(g_ident)-1] = 0; }
}

#define IO4_PUT16_LE(b, cap, off, val) do { int _o=(off); if (_o>=0 && _o+1 < (cap)) { \
    (b)[_o] = (unsigned char)((val) & 0xFF); (b)[_o+1] = (unsigned char)(((val) >> 8) & 0xFF); } } while (0)

/* Register a coin insert on a slot so the next IN report carries it (the
 * game reads + debits the counter itself). */
void ultra_sega_io4_insert_coin(int slot, int count) {
    if (slot < 0 || slot >= IO4_COININ || count <= 0) return;
    g_coin[slot] = (unsigned short)(g_coin[slot] + count);
}

int ultra_sega_io4_fill_in(void* buf, int cap) {
    if (!buf || cap < IO4_REPORT_SIZE) return 0;
    unsigned char* b = (unsigned char*)buf;
    memset(b, 0, IO4_REPORT_SIZE);
    b[0] = IO4_REPORT_IN;

    /* drain any coin edges the gamepad thread detected since our last poll */
    unsigned coin_delta = ul_ax_drain_coin();
    if (coin_delta) ultra_sega_io4_insert_coin(0, (int)coin_delta);

    ultra_jvs_input in;
    ul_ax_current_input(&in);

    for (int a = 0; a < g_analog_n && a < JVS_MAX_ANALOG; ++a)
        IO4_PUT16_LE(b, IO4_REPORT_SIZE, g_off_analog + a*2, in.analog[a]);

    if (g_off_switch >= 0 && g_off_switch < IO4_REPORT_SIZE) b[g_off_switch] = in.system;
    if (JVS_MAX_PLAYERS >= 1) {
        IO4_PUT16_LE(b, IO4_REPORT_SIZE, g_off_switch + 1,
                     (unsigned short)((in.player[0] >> 8) | ((in.player[0] & 0xFF) << 8)));
    }
    if (JVS_MAX_PLAYERS >= 2) {
        IO4_PUT16_LE(b, IO4_REPORT_SIZE, g_off_switch + 3,
                     (unsigned short)((in.player[1] >> 8) | ((in.player[1] & 0xFF) << 8)));
    }

    for (int c = 0; c < IO4_COININ; ++c)
        IO4_PUT16_LE(b, IO4_REPORT_SIZE, g_off_coin + c*2, g_coin[c]);

    return IO4_REPORT_SIZE;
}

int ultra_sega_io4_consume_out(const void* buf, int len) {
    (void)buf; (void)len;
    return 1;
}

int ultra_sega_io4_ident(char* buf, int cap) {
    if (!buf || cap <= 0) return 0;
    int n = (int)strlen(g_ident);
    if (n + 1 > cap) n = cap - 1;
    memcpy(buf, g_ident, n);
    buf[n] = 0;
    return n;
}

/* ---------------------------------------------------------------------------
 * Transport seam: intercept the device OPEN (CreateFile on a path whose
 * VID/PID match the IO4) and hand back a synthetic handle our read/write
 * shims service. This lets a board with NO physical IO4 pass the device-
 * present gate and feed inputs.
 * ------------------------------------------------------------------------- */
typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static pCreateFileA Real_CreateFileA = NULL;
static pCreateFileW Real_CreateFileW = NULL;

#define IO4_SENTINEL ((HANDLE)(ULONG_PTR)0x10040001)

static int io4_path_matches(const char* n) {
    if (!n) return 0;
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
        if (real != INVALID_HANDLE_VALUE) return real;
        ul_ax_log("io4: synthesizing IO4-USB presence (no physical 837-15257)\n");
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
        ul_ax_log("io4: synthesizing IO4-USB presence (W) (no physical 837-15257)\n");
        return IO4_SENTINEL;
    }
    return Real_CreateFileW ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}

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
 * Install. Armed unconditionally (this DLL only ever loads for sega_alls_ux),
 * unless the user explicitly opts out with ULTRA_IO4=0. Idempotent.
 * ------------------------------------------------------------------------- */
static int g_installed = 0;

void ul_ax_install_io4(void) {
    if (g_installed) return;
    const char* e = ul_ax_env("ULTRA_IO4");
    g_enabled = !(e && e[0] == '0');   /* default ON; ULTRA_IO4=0 opts out */
    if (!g_enabled) { ul_ax_log("io4: skipped (ULTRA_IO4=0)\n"); return; }
    g_installed = 1;
    load_offsets();

    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (k) {
        Real_CreateFileA = (pCreateFileA)(void*)GetProcAddress(k, "CreateFileA");
        Real_CreateFileW = (pCreateFileW)(void*)GetProcAddress(k, "CreateFileW");
        Real_ReadFile    = (pReadFile)(void*)GetProcAddress(k, "ReadFile");
        Real_WriteFile   = (pWriteFile)(void*)GetProcAddress(k, "WriteFile");
    }
    ul_ax_iat_patch_all("kernel32.dll", "CreateFileA", (void*)Hook_CreateFileA);
    ul_ax_iat_patch_all("kernel32.dll", "CreateFileW", (void*)Hook_CreateFileW);
    ul_ax_iat_patch_all("kernel32.dll", "ReadFile",    (void*)Hook_ReadFile);
    ul_ax_iat_patch_all("kernel32.dll", "WriteFile",   (void*)Hook_WriteFile);

    ul_ax_log("io4: Sega IO4-USB I/O emulation armed (VID %04X/PID %04X, analog@%d x%d, "
              "switch@%d, coin@%d)\n",
              IO4_VID, IO4_PID, g_off_analog, g_analog_n, g_off_switch, g_off_coin);
}

int ul_ax_io4_enabled(void) { return g_enabled; }

#endif /* _WIN32 */
