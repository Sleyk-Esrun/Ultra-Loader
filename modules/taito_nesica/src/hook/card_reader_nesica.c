/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  card_reader_nesica.c -- NESiCA IC-card-reader presence stub (env-gated, OFF)
 * =============================================================================
 *
 *  Taito NESiCAxLive gates the start of play on an IC-card touch: a NESiCA card
 *  (FeliCa-class) over a serial/USB reader carries the player's profile/save/
 *  ranking, and some titles will not leave the attract loop until a card is
 *  presented (or explicitly skipped).
 *
 *  WHAT THIS FILE HONESTLY IS:
 *    A clean-room SCAFFOLD that, when env-gated on (ULTRA_CARD_READER=1), makes
 *    a NESiCA-title CreateFile call to the reader's SERIAL/USB device path see a
 *    STUB "no card / benign idle" reader so a title that polls the reader keeps
 *    running instead of erroring on a missing device. It can optionally present
 *    a synthetic card-ID (ULTRA_CARD_ID=<hex>) so a "touch a card to start"
 *    prompt proceeds.
 *
 *  WHAT IT IS NOT:
 *    It ships NO real card data, NO player profiles, NO network/e-amusement
 *    handshake, and is OFF by default. It does not and cannot stand in for
 *    NESYS / Taito's online account system -- it only keeps the LOCAL reader
 *    poll from blocking the game. The exact reader command framing is
 *    title/reader-specific and is pinned against a real dump; this is the
 *    transport seam + a benign default. ULTRA_NESICA_OFFLINE (consumed by the
 *    board's spawner, taito_nesica_main.c) neutralizes residual ALL.Net/NESYS
 *    network heartbeats the same way -- neither knob impersonates the online
 *    service, they only stop a standalone dump from hanging on it.
 *
 *  ARCHITECTURE NOTE (why this file does not install its own IAT patch): a
 *  naive port of the sibling native_x86 card_reader.c would call
 *  ultra_iat_patch_all("kernel32.dll", "CreateFileA", Hook_CreateFileA) here,
 *  AFTER jvs_serial_hook_nesica.c has already patched the same slot to ITS
 *  OWN Hook_CreateFileA. Because ultra_iat_patch_all matches by import NAME
 *  (not by "whatever is currently installed"), that second patch would win the
 *  slot outright and every OTHER CreateFileA call this hook cares about (the
 *  JVS fake-COM open, the dongle bypass, the D: redirect) would silently stop
 *  routing through jvs_serial_hook_nesica.c -- because this stub's own
 *  passthrough calls the TRUE original CreateFileA, not the JVS hook. NESiCA
 *  is exactly the board where the card reader AND the JVS I/O need to work at
 *  the same time, so that landmine is unacceptable here. Instead this file
 *  exposes ultra_card_reader_try_open(), and jvs_serial_hook_nesica.c's single
 *  Hook_CreateFileA calls it first (see that file) -- one hook chain, one
 *  owner, no silent stacking order dependency.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the public Win32 device API + public
 *  descriptions of IC-card reader systems. No third-party loader source, no
 *  card dump.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

extern void ultra_nesica_log(const char* fmt, ...);

static int g_enabled = 0;

/* Optional synthetic card id (hex string the user supplies via ULTRA_CARD_ID).
 * When empty, the stub presents "no card" (the common idle the game tolerates). */
static unsigned char g_card_id[16];
static int           g_card_id_len = 0;

/* The reader's device path substring. Defaults to NESiCA-style names;
 * overridable so a reader on a different COM/USB path is matched. */
static char g_dev[64] = "card";

static int is_card_dev(const char* n) {
    if (!n) return 0;
    return (g_dev[0] && strstr(n, g_dev)) ||
            strstr(n, "nesica") || strstr(n, "NESICA") ||
            strstr(n, "aime")   || strstr(n, "AIME")   ||
            strstr(n, "felica") || strstr(n, "FeliCa");
}

typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static pCreateFileA Real_CreateFileA = NULL;

#define CARD_SENTINEL ((HANDLE)(ULONG_PTR)0xCA5D0001)

/* Called from jvs_serial_hook_nesica.c's Hook_CreateFileA/W BEFORE the fake-COM/
 * dongle/D-drive checks. Returns 1 and fills *out if this stub is claiming the
 * path (either the real reader if one happens to be present, or the synthetic
 * "no card / no reader" idle); returns 0 to defer to the rest of the chain. */
int ultra_card_reader_try_open(const char* name, HANDLE* out) {
    if (!g_enabled || !is_card_dev(name)) return 0;
    HANDLE real = Real_CreateFileA
        ? Real_CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL)
        : INVALID_HANDLE_VALUE;
    if (real != INVALID_HANDLE_VALUE) { *out = real; return 1; }   /* real reader present */
    ultra_nesica_log("[card] synthesizing card-reader presence (no physical reader)");
    *out = CARD_SENTINEL;
    return 1;
}

/* The reader's response surface. Given a command frame the game wrote, produce
 * the benign reply. We answer only the non-secret transport classes: a "reader
 * OK / no card" idle, or (if ULTRA_CARD_ID is set) a "card present" with the
 * synthetic id. Anything that needs the online account system is left
 * unanswered (the game's NESYS/e-amusement handshake is out of clean-room
 * scope -- it needs the network service, not a local reader reply). Returns
 * response length, 0 if it defers. Not yet wired to a transport (no NESiCA
 * reader dump has pinned the exact command framing); kept as the documented
 * next seam for when one is available. */
int ultra_card_reader_answer(const unsigned char* req, int req_len,
                             unsigned char* resp, int resp_cap) {
    if (!g_enabled || !req || req_len < 1 || !resp || resp_cap < 2) return 0;
    switch (req[0]) {
        case 0x00:                          /* status poll -> reader OK */
            resp[0] = 0x00; resp[1] = 0x00;
            return 2;
        case 0x01:                          /* read card */
            if (g_card_id_len > 0 && resp_cap >= 2 + g_card_id_len) {
                resp[0] = 0x01;
                resp[1] = (unsigned char)g_card_id_len;
                memcpy(resp + 2, g_card_id, g_card_id_len);
                return 2 + g_card_id_len;
            }
            resp[0] = 0x00; resp[1] = 0x00;  /* no card present (benign idle) */
            return 2;
        default:
            return 0;                       /* defer everything account/online */
    }
}

void ultra_card_reader_install(void) {
    const char* e = getenv("ULTRA_CARD_READER");
    g_enabled = e && e[0] == '1';
    if (!g_enabled) return;

    const char* dv = getenv("ULTRA_CARD_DEV");
    if (dv && dv[0]) { strncpy(g_dev, dv, sizeof(g_dev)-1); g_dev[sizeof(g_dev)-1] = 0; }

    const char* id = getenv("ULTRA_CARD_ID");
    g_card_id_len = 0;
    if (id && id[0]) {
        int n = 0;
        for (const char* p = id; p[0] && p[1] && n < 16; p += 2) {
            char b[3] = { p[0], p[1], 0 };
            g_card_id[n++] = (unsigned char)strtoul(b, NULL, 16);
        }
        g_card_id_len = n;
    }

    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (k) Real_CreateFileA = (pCreateFileA)(void*)GetProcAddress(k, "CreateFileA");

    ultra_nesica_log("[card] IC-card-reader stub armed (dev~='%s', %s). NOTE: presents only a "
         "local reader idle; the online account/e-amusement handshake is out of "
         "clean-room scope.", g_dev, g_card_id_len ? "synthetic card id set" : "no-card idle");
}

int ultra_card_reader_enabled(void) { return g_enabled; }

#endif /* _WIN32 */
