/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  sega_io4_alls.c -- Sega IO4-USB (837-15257) I/O-board emulation for ALLS
 *                     (env-gated, OFF by default). Board-owned, standalone copy.
 * =============================================================================
 *
 *  Sega ALLS (HX/UX/UX2/MX2) carries its cabinet inputs over the same IO4
 *  (837/835-15257) USB vendor/HID-INOUT device every modern Sega x86 board
 *  uses (RingEdge/RingWide/Nu/Nu2/ALLS). It is NOT a plain HID joystick: the
 *  game opens it by VID/PID, reads a capability/ident string, then polls a
 *  64-byte IN report (analog + switches + coin counters) and writes a 63-byte
 *  OUT report (general outputs / lamps).
 *
 *  PUBLIC HARDWARE FACTS this scaffold is built from (no third-party loader
 *  source read):
 *    - VID 0x0CA3, PID 0x0021.
 *    - Ident/capability string the game matches on:
 *        "I/O CONTROL BD;15257;01;90;1831;6679A;00;"
 *        "GOUT=14_ADIN=8,E_ROTIN=4_COININ=2_SWIN=2,E_UQ1=41,6;"
 *    - IN  report id 0x01: up to 14 x 16-bit analog (first 8 = ADIN, range
 *      0..0xFFFE, 0x8000 = center), a switch bitfield, and the COININ running
 *      coin counters.
 *    - OUT report id 0x10: the GOUT general outputs (lamps/solenoids).
 *
 *  WHAT THIS FILE HONESTLY IS: a clean-room SCAFFOLD that, when env-gated on
 *  (ULTRA_IO4=1, set by sega_alls_main.c), SYNTHESISES the IO4 device when no
 *  physical board is present and services it from the live cabinet input
 *  (ultra_jvs_input, jvs_core_alls.h) -- exactly like the analogous scaffolds
 *  on every other Sega IO4 board, re-derived here standalone for ALLS.
 *
 *  WHAT IT IS NOT: it ships NO game data, NO keys, and owns NO security/
 *  keychip/crypto (that lives in sega_keychip_alls.c, presence-only). It is
 *  OFF unless armed.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "jvs_core_alls.h"
#include "sega_alls_hook.h"

static int g_enabled = 0;

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

static int g_off_analog = 1;
static int g_off_switch = 29;
static int g_off_coin   = 35;
static int g_analog_n   = IO4_ADIN + IO4_ROTIN;

static void load_offsets(void) {
    const char* e;
    if ((e = ul_alls_env("ULTRA_IO4_OFF_ANALOG")) && *e) g_off_analog = atoi(e);
    if ((e = ul_alls_env("ULTRA_IO4_OFF_SWITCH")) && *e) g_off_switch = atoi(e);
    if ((e = ul_alls_env("ULTRA_IO4_OFF_COIN"))   && *e) g_off_coin   = atoi(e);
    if ((e = ul_alls_env("ULTRA_IO4_ANALOG_N"))   && *e) { int v = atoi(e); if (v >= 0 && v <= 14) g_analog_n = v; }
    if ((e = ul_alls_env("ULTRA_IO4_IDENT"))      && *e) { strncpy(g_ident, e, sizeof(g_ident)-1); g_ident[sizeof(g_ident)-1] = 0; }
}

#define IO4_PUT16_LE(b, cap, off, val) do { int _o=(off); if (_o>=0 && _o+1 < (cap)) { \
    (b)[_o] = (unsigned char)((val) & 0xFF); (b)[_o+1] = (unsigned char)(((val) >> 8) & 0xFF); } } while (0)

int ultra_alls_io4_fill_in(void* buf, int cap) {
    if (!buf || cap < IO4_REPORT_SIZE) return 0;
    unsigned char* b = (unsigned char*)buf;
    memset(b, 0, IO4_REPORT_SIZE);
    b[0] = IO4_REPORT_IN;

    ultra_jvs_input in;
    ultra_alls_current_input(&in);

    for (int a = 0; a < g_analog_n && a < JVS_ALLS_MAX_ANALOG; ++a)
        IO4_PUT16_LE(b, IO4_REPORT_SIZE, g_off_analog + a*2, in.analog[a]);

    if (g_off_switch >= 0 && g_off_switch < IO4_REPORT_SIZE) b[g_off_switch] = in.system;
    if (JVS_ALLS_MAX_PLAYERS >= 1) {
        IO4_PUT16_LE(b, IO4_REPORT_SIZE, g_off_switch + 1,
                     (unsigned short)((in.player[0] >> 8) | ((in.player[0] & 0xFF) << 8)));
    }
    if (JVS_ALLS_MAX_PLAYERS >= 2) {
        IO4_PUT16_LE(b, IO4_REPORT_SIZE, g_off_switch + 3,
                     (unsigned short)((in.player[1] >> 8) | ((in.player[1] & 0xFF) << 8)));
    }

    for (int c = 0; c < IO4_COININ; ++c)
        IO4_PUT16_LE(b, IO4_REPORT_SIZE, g_off_coin + c*2, g_coin[c]);

    return IO4_REPORT_SIZE;
}

void ul_alls_io4_insert_coin(int slot, int count) {
    if (slot < 0 || slot >= IO4_COININ || count <= 0) return;
    g_coin[slot] = (unsigned short)(g_coin[slot] + count);
}

int ultra_alls_io4_consume_out(const void* buf, int len) {
    (void)buf; (void)len;
    return 1;
}

int ultra_alls_io4_ident(char* buf, int cap) {
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
 * shims service.
 * ------------------------------------------------------------------------- */
typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static pCreateFileA Real_CreateFileA = NULL;
static pCreateFileW Real_CreateFileW = NULL;

#define IO4_SENTINEL ((HANDLE)(ULONG_PTR)0x10040002)

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
        ul_alls_log("[io4] synthesizing IO4-USB presence (no physical 837-15257)\n");
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
        ul_alls_log("[io4] synthesizing IO4-USB presence (W) (no physical 837-15257)\n");
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
        int w = ultra_alls_io4_fill_in(buf, (int)n);
        if (got) *got = (DWORD)(w > 0 ? w : 0);
        return w > 0 ? TRUE : FALSE;
    }
    return Real_ReadFile ? Real_ReadFile(h, buf, n, got, ov) : FALSE;
}
static BOOL WINAPI Hook_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote, LPOVERLAPPED ov) {
    if (g_enabled && h == IO4_SENTINEL) {
        ultra_alls_io4_consume_out(buf, (int)n);
        if (wrote) *wrote = n;
        return TRUE;
    }
    return Real_WriteFile ? Real_WriteFile(h, buf, n, wrote, ov) : FALSE;
}

/* ---------------------------------------------------------------------------
 * Install. Env-gated (ULTRA_IO4=1, set by sega_alls_main.c). Idempotent.
 * ------------------------------------------------------------------------- */
void ul_alls_install_io4(void) {
    if (!ul_alls_env_on("ULTRA_IO4")) return;
    if (g_enabled) return;   /* idempotent */
    g_enabled = 1;
    load_offsets();

    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (k) {
        if (!Real_CreateFileA) Real_CreateFileA = (pCreateFileA)(void*)GetProcAddress(k, "CreateFileA");
        if (!Real_CreateFileW) Real_CreateFileW = (pCreateFileW)(void*)GetProcAddress(k, "CreateFileW");
        if (!Real_ReadFile)    Real_ReadFile    = (pReadFile)(void*)GetProcAddress(k, "ReadFile");
        if (!Real_WriteFile)   Real_WriteFile   = (pWriteFile)(void*)GetProcAddress(k, "WriteFile");
    }
    ul_alls_iat_patch_all("kernel32.dll", "CreateFileA", (void*)Hook_CreateFileA);
    ul_alls_iat_patch_all("kernel32.dll", "CreateFileW", (void*)Hook_CreateFileW);
    ul_alls_iat_patch_all("kernel32.dll", "ReadFile",    (void*)Hook_ReadFile);
    ul_alls_iat_patch_all("kernel32.dll", "WriteFile",   (void*)Hook_WriteFile);

    ul_alls_log("[io4] Sega ALLS IO4-USB I/O emulation armed (VID %04X/PID %04X, analog@%d x%d, "
                "switch@%d, coin@%d)\n",
                IO4_VID, IO4_PID, g_off_analog, g_analog_n, g_off_switch, g_off_coin);
}

int ul_alls_io4_enabled(void) { return g_enabled; }

#endif /* _WIN32 */
