/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  sega_io4_nuc.c -- Sega Nu Compact board's OWN copy of the IO4-USB
 *                    (837-15257) I/O-board emulation (env-gated, OFF).
 * =============================================================================
 *
 *  Every modern Sega x86 board (RingEdge/RingWide/Nu/Nu2/ALLS, including this
 *  Nu-family "Compact" variant) carries its cabinet inputs over ONE USB I/O
 *  board: the "IO4" (837/835-15257). It is a USB vendor / HID-INOUT device the
 *  game opens by VID/PID and talks fixed reports to -- NOT a plain HID
 *  joystick. The game parses a capability/ident string and then polls a
 *  64-byte IN report (analog + switches + coin counters) and writes a 63-byte
 *  OUT report (general outputs / lamps).
 *
 *  PUBLIC HARDWARE FACTS this scaffold is built from (no loader source read):
 *    - VID 0x0CA3, PID 0x0021.
 *    - Ident/capability string the game matches on:
 *        "I/O CONTROL BD;15257;01;90;1831;6679A;00;"
 *        "GOUT=14_ADIN=8,E_ROTIN=4_COININ=2_SWIN=2,E_UQ1=41,6;"
 *      => GOUT=14 general outputs, ADIN=8 analog, ROTIN=4 rotary, COININ=2 coin
 *         counters, SWIN=2 switch banks.
 *    - IN  report id 0x01: up to 14 x 16-bit analog (first 8 = ADIN), a switch
 *      bitfield (system + per-player), and the COININ running coin counters.
 *    - OUT report id 0x10: the GOUT general outputs (lamps/solenoids).
 *
 *  WHAT THIS FILE HONESTLY IS:
 *    A clean-room SCAFFOLD that, when env-gated on (ULTRA_IO4=1), SYNTHESISES
 *    the IO4 device when no physical board is present and SERVICES it FROM THE
 *    EXISTING JVS INPUT BLOCK -- i.e. the same hook<->input struct
 *    (ultra_jvs_input) this module's own jvs_core_nuc.c defines. So a Sega Nu
 *    title that opens the IO4 USB device finds its cabinet inputs.
 *
 *    This is purely an I/O re-PACKING of the hook's controller block. It owns
 *    NO security/keychip/crypto; that lives in sega_keychip_nuc.c (presence
 *    only) and is user/operator-gated.
 *
 *  WHAT IT IS NOT:
 *    It ships NO game data, NO keys. It is OFF unless armed. The exact
 *    bRequest values for the vendor control transfers (coin-counter reset,
 *    watchdog) are title/firmware-specific and are left as ULTRA_IO4_* env
 *    knobs, pinned against a real dump -- never baked in.
 *
 *  TRANSPORT NOTE (own-module design): unlike the native_x86 reference this was
 *  derived from (which has each board-specific file IAT-patch CreateFileA/W/
 *  ReadFile/WriteFile itself), this module centralizes the CreateFileA/W/
 *  ReadFile/WriteFile hooking in hook_dllmain_nuc.c so this board's OWN
 *  keychip presence-shim and this OWN IO4 shim share the SAME patched IAT slot
 *  without one silently overwriting the other's patch. This file only exposes
 *  the pure device-match predicate + report fill/consume logic; hook_dllmain_
 *  nuc.c owns the actual CreateFile/ReadFile/WriteFile hook functions and
 *  dispatches into these.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the public Win32 device/HID API +
 *  the public 837-15257 descriptor facts + this module's own ultra_jvs_input
 *  block. No third-party loader source, profile, or descriptor binary was
 *  read or copied. sega_io4.c in modules/native_x86/src/hook was read for
 *  REFERENCE ONLY per this board's build directive; nothing was linked from
 *  it, and this is an independent copy.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "jvs_core_nuc.h"

/* logger + current-input source, both owned by hook_dllmain_nuc.c */
extern void ultra_d3d8_log(const char* fmt, ...);
#define iolog ultra_d3d8_log
extern void ultra_hook_current_input(ultra_jvs_input* out);

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
 * Build the 64-byte IN report from the live cabinet input. Layout (LE 16-bit):
 *   [0]        report id (0x01)
 *   [1..28]    14 x 16-bit analog LE  (ch0..7 = ADIN, ch8..11 = ROTIN, rest 0)
 *   [29..34]   switch bitfield: byte29 = system, byte30/31 = P1, byte32/33 = P2
 *   [35..38]   coin counters: 2 x 16-bit LE
 *   [39..63]   reserved / padding
 * A title whose firmware packs them differently can re-point via
 * ULTRA_IO4_OFF_* (pinned to its dump). Defaults to the published layout.
 * ------------------------------------------------------------------------- */
static int g_off_analog = 1;
static int g_off_switch = 29;
static int g_off_coin   = 35;
static int g_analog_n   = IO4_ADIN + IO4_ROTIN;

static void load_offsets(void) {
    const char* e;
    if ((e = getenv("ULTRA_IO4_OFF_ANALOG")) && *e) g_off_analog = atoi(e);
    if ((e = getenv("ULTRA_IO4_OFF_SWITCH")) && *e) g_off_switch = atoi(e);
    if ((e = getenv("ULTRA_IO4_OFF_COIN"))   && *e) g_off_coin   = atoi(e);
    if ((e = getenv("ULTRA_IO4_ANALOG_N"))   && *e) { int v = atoi(e); if (v >= 0 && v <= 14) g_analog_n = v; }
    if ((e = getenv("ULTRA_IO4_IDENT"))      && *e) { strncpy(g_ident, e, sizeof(g_ident)-1); g_ident[sizeof(g_ident)-1] = 0; }
}

#define IO4_PUT16_LE(b, cap, off, val) do { int _o=(off); if (_o>=0 && _o+1 < (cap)) { \
    (b)[_o] = (unsigned char)((val) & 0xFF); (b)[_o+1] = (unsigned char)(((val) >> 8) & 0xFF); } } while (0)

int ultra_sega_io4_fill_in(void* buf, int cap) {
    if (!buf || cap < IO4_REPORT_SIZE) return 0;
    unsigned char* b = (unsigned char*)buf;
    memset(b, 0, IO4_REPORT_SIZE);
    b[0] = IO4_REPORT_IN;

    ultra_jvs_input in;
    ultra_hook_current_input(&in);

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

void ultra_sega_io4_insert_coin(int slot, int count) {
    if (slot < 0 || slot >= IO4_COININ || count <= 0) return;
    g_coin[slot] = (unsigned short)(g_coin[slot] + count);
}

int ultra_sega_io4_consume_out(const void* buf, int len) {
    (void)buf; (void)len;
    /* GOUT lamp/solenoid state -- nothing to physically drive; accept silently. */
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
 * Device-open match predicates (pure logic; hook_dllmain_nuc.c owns the actual
 * CreateFileA/W hooks and calls these to decide routing). A device path names
 * the IO4 when it carries the VID/PID (the Windows HID/USB device-interface
 * path embeds "vid_0ca3" / "pid_0021"), case-insensitively.
 * ------------------------------------------------------------------------- */
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

int ultra_sega_io4_matches_a(const char* name) {
    return g_enabled && io4_path_matches(name);
}
int ultra_sega_io4_matches_w(const wchar_t* name) {
    if (!g_enabled || !name) return 0;
    char buf[260]; int i = 0;
    for (; name[i] && i < 259; ++i) buf[i] = (char)name[i];
    buf[i] = 0;
    return io4_path_matches(buf);
}
HANDLE ultra_sega_io4_sentinel(void) { return IO4_SENTINEL; }

/* ---------------------------------------------------------------------------
 * Install. Env-gated (ULTRA_IO4=1, set by the sega_nuc board profile / launcher
 * env block). Default OFF so an unconfigured run touches nothing. Idempotent.
 * Only reads env + resets internal offsets; does NOT patch any IAT slot itself
 * (see the transport note above -- hook_dllmain_nuc.c owns that).
 * ------------------------------------------------------------------------- */
void ultra_sega_io4_install(void) {
    const char* e = getenv("ULTRA_IO4");
    g_enabled = e && e[0] == '1';
    if (!g_enabled) return;
    load_offsets();
    iolog("[io4_nuc] Sega IO4-USB I/O emulation armed (VID %04X/PID %04X, analog@%d x%d, "
          "switch@%d, coin@%d). JVS-backed inputs; lamps accepted/ignored. NOTE: vendor "
          "control bRequest values are title-specific -> ULTRA_IO4_* knobs.",
          IO4_VID, IO4_PID, g_off_analog, g_analog_n, g_off_switch, g_off_coin);
}

int ultra_sega_io4_enabled(void) { return g_enabled; }

#endif /* _WIN32 */
