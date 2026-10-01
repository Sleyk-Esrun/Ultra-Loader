/* SPDX-License-Identifier: MIT
 * NOTE: independent namco_es3 board copy -- adapted from native_x86/src/hook/card_reader.c (reference only; no build-time or runtime dependency on native_x86 remains).
 * =============================================================================
 *  card_reader_es3.c -- NESiCA / Aime IC-card-reader presence stub (env-gated, OFF).
 * =============================================================================
 *
 *  Several PC arcade boards gate the start of play on an IC-card touch:
 *    - Taito NESiCAxLive: a NESiCA card (FeliCa-class) over a serial/USB reader
 *      carries the player's profile/save/ranking; some titles will not leave the
 *      attract loop until a card is presented (or explicitly skipped).
 *    - Sega ALL.Net / Aime, Namco/Konami card systems are analogous serial readers.
 *
 *  WHAT THIS FILE HONESTLY IS:
 *    A clean-room SCAFFOLD that, when env-gated on (ULTRA_CARD_READER=1, set by the
 *    NESiCA / ALLS / Bemani board profile), intercepts the card-reader SERIAL
 *    transport (CreateFile on the reader's COM/device path) and presents a STUB
 *    "no card / benign idle" reader so a title that polls the reader keeps running
 *    instead of erroring on a missing device. It can optionally present a synthetic
 *    card-ID (ULTRA_CARD_ID=<hex>) so a "touch a card to start" prompt proceeds.
 *
 *  WHAT IT IS NOT:
 *    It ships NO real card data, NO player profiles, NO network/e-amusement
 *    handshake, and is OFF by default. It does not and cannot stand in for the
 *    online account system -- it only keeps the LOCAL reader poll from blocking the
 *    game. The exact reader command framing is title/reader-specific and is pinned
 *    against a real dump; this is the transport seam + a benign default.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the public Win32 device API + public
 *  descriptions of these card systems. No third-party loader source, no card dump.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

extern void ultra_d3d8_log(const char* fmt, ...);
#define clog ultra_d3d8_log

extern void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

static int g_enabled = 0;

/* Optional synthetic card id (hex string the user supplies via ULTRA_CARD_ID).
 * When empty, the stub presents "no card" (the common idle the game tolerates). */
static unsigned char g_card_id[16];
static int           g_card_id_len = 0;

/* The reader's serial device path. Defaults to a NESiCA-style name; overridable so
 * a board whose reader sits on a different COM/USB path is matched. */
static char g_dev[64] = "card";   /* substring match (e.g. COMx aliased "card") */

static int is_card_dev_a(LPCSTR n) {
    if (!n) return 0;
    /* match the user's configured device substring OR the common reader names */
    return (g_dev[0] && strstr(n, g_dev)) ||
            strstr(n, "nesica") || strstr(n, "NESICA") ||
            strstr(n, "aime")   || strstr(n, "AIME")   ||
            strstr(n, "felica") || strstr(n, "FeliCa");
}

typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static pCreateFileA Real_CreateFileA = NULL;

#define CARD_SENTINEL ((HANDLE)(ULONG_PTR)0xCA5D0001)

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && is_card_dev_a(name)) {
        HANDLE real = Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                                       : INVALID_HANDLE_VALUE;
        if (real != INVALID_HANDLE_VALUE) return real;   /* real reader present */
        clog("[card] synthesizing card-reader presence (no physical reader)\n");
        return CARD_SENTINEL;
    }
    return Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}

/* The reader's response surface. Given a command frame the game wrote, produce the
 * benign reply. We answer only the non-secret transport classes: a "reader OK /
 * no card" idle, or (if ULTRA_CARD_ID is set) a "card present" with the synthetic
 * id. Anything that needs the online account system is left unanswered (the game's
 * NESYS/e-amusement handshake is out of clean-room scope -- it needs the network
 * service, not a local reader reply). Returns response length, 0 if it defers. */
int ultra_card_reader_answer(const unsigned char* req, int req_len,
                             unsigned char* resp, int resp_cap) {
    if (!g_enabled || !req || req_len < 1 || !resp || resp_cap < 2) return 0;
    /* Generic framing: first byte = command class. 0x00 = poll/status (reader OK),
     * 0x01 = read card (present id or "no card"). These are the non-secret idles. */
    switch (req[0]) {
        case 0x00:                          /* status poll -> reader OK */
            resp[0] = 0x00;                 /* ok */
            resp[1] = 0x00;                 /* no error */
            return 2;
        case 0x01:                          /* read card */
            if (g_card_id_len > 0 && resp_cap >= 2 + g_card_id_len) {
                resp[0] = 0x01;             /* card present */
                resp[1] = (unsigned char)g_card_id_len;
                memcpy(resp + 2, g_card_id, g_card_id_len);
                return 2 + g_card_id_len;
            }
            resp[0] = 0x00;                 /* no card present (benign idle) */
            resp[1] = 0x00;
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
        /* parse a hex string into up to 16 bytes */
        int n = 0;
        for (const char* p = id; p[0] && p[1] && n < 16; p += 2) {
            char b[3] = { p[0], p[1], 0 };
            g_card_id[n++] = (unsigned char)strtoul(b, NULL, 16);
        }
        g_card_id_len = n;
    }

    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (k) Real_CreateFileA = (pCreateFileA)(void*)GetProcAddress(k, "CreateFileA");
    ultra_iat_patch_all("kernel32.dll", "CreateFileA", (void*)Hook_CreateFileA);

    clog("[card] IC-card-reader stub armed (dev~='%s', %s). NOTE: presents only a "
         "local reader idle; the online account/e-amusement handshake is out of "
         "clean-room scope.\n", g_dev, g_card_id_len ? "synthetic card id set" : "no-card idle");
}

int ultra_card_reader_enabled(void) { return g_enabled; }

#endif /* _WIN32 */
