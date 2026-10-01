/* SPDX-License-Identifier: MIT
 * NOTE: independent namco_es3 board copy -- adapted from native_x86/src/hook/konami_io.c (reference only; no build-time or runtime dependency on native_x86 remains).
 * =============================================================================
 *  konami_io_es3.c -- Konami Bemani-PC arcade-I/O (ACIO bus) scaffold (env-gated, OFF).
 * =============================================================================
 *
 *  The Konami Bemani PC I/O PCBs (P3IO, P4IO, BIO2/BI2A, KFCA, USB-IO2) bridge a
 *  USB endpoint to an internal "ACIO" serial bus (RS-232/485, multi-node). ACIO is
 *  NOT JVS -- it has its own framing -- so the existing clean-room JVS engine cannot
 *  answer it; it needs its own frame codec + node model. Per-game inputs (IIDX
 *  turntable + 7 keys, pop'n buttons, SDVX slider/knobs, DDR pads, cabinet lamps)
 *  ride ACIO nodes as game-specific bit layouts.
 *
 *  PUBLIC PROTOCOL FACTS this scaffold is built from (no loader source read):
 *    - Frame sync byte 0xAA (frame delimiter).
 *    - Byte-stuffing: a literal 0xAA or 0xFF in the body is escaped as 0xFF followed
 *      by the bitwise-NOT of the original byte.
 *    - Header = node/dest address + command word + payload length; then payload;
 *      then a 1-byte additive checksum (sum of the body bytes) & 0xFF.
 *    - Per-node identity is an ACIO node ID (e.g. KFCA = 0x09060000); ICCA card
 *      readers / light nodes are sibling IDs.
 *    - Bring-up: the host streams repeated 0xAA until enough echo, then enumerates.
 *
 *  WHAT THIS FILE HONESTLY IS:
 *    A clean-room SCAFFOLD: the ACIO frame codec (sync/escape/unescape/checksum) +
 *    a node model that answers ENUMERATE / GET-VERSION / node-ID, and answers
 *    READ-INPUT / SET-LIGHTS from the EXISTING loader<->hook input struct
 *    (ultra_jvs_input). The per-game node ID + the input bit-map are env-knob-
 *    configured against the user's dump (ULTRA_KONAMI_NODE_*), NOT baked per title
 *    -- exactly the "I/O contract pinned against a dump" stance the Fast-I/O offsets
 *    and IO4 layout use.
 *
 *  WHAT IT IS NOT:
 *    It ships NO game data, NO keys. The iKey-2032-class security dongle PRESENCE
 *    is handled by dongle_bypass.c (presence only). e-amusement / Paseli is a LIVE
 *    online service -> OUT of scope, never impersonated. RSA/AES game-data keys are
 *    content-key-derivation -> OUT of scope.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the public ACIO protocol facts + the
 *  EXISTING ultra_jvs_input block. No third-party loader source, profile, or I/O
 *  binary was read or copied.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#endif

#include <stdint.h>
#include <string.h>

#include "jvs_core.h"

#if defined(_WIN32)
extern void ultra_d3d8_log(const char* fmt, ...);
#define klog ultra_d3d8_log
extern void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);
#endif
extern void ultra_hook_current_input(ultra_jvs_input* out);

/* ---- ACIO protocol constants (public facts) ------------------------------ */
#define ACIO_SYNC    0xAA
#define ACIO_ESCAPE  0xFF

/* ACIO commands a node answers (the common, non-secret surface). Values are the
 * well-documented Konami ACIO opcodes. */
#define ACIO_CMD_ASSIGN_ADDR  0x0100   /* host assigns / enumerates node addresses */
#define ACIO_CMD_GET_VERSION  0x0002   /* node firmware/version + node ID          */
#define ACIO_CMD_STARTUP      0x0001   /* node init / clear                        */
#define ACIO_CMD_POLL         0x0152   /* read input (KFCA-class poll)             */
#define ACIO_CMD_SET_LIGHTS   0x0153   /* set lamp/LED state                       */

static int g_enabled = 0;

/* The synthetic node's ACIO node ID (e.g. KFCA 0x09060000). Per-dump-overridable
 * via ULTRA_KONAMI_NODE_ID so the game's enumerate matches whatever node its I/O
 * profile expects. Default is a widely-seen KFCA-class id. */
static uint32_t g_node_id      = 0x09060000u;
static uint8_t  g_node_version = 0x01;
static int      g_num_nodes    = 1;

/* Where, in the node poll payload, each input field lands. ACIO input layouts are
 * per-game (turntable byte, key bits, slider, knobs), so these are env knobs the
 * user's dump pins. Defaults place a JVS-shaped digital + analog block. */
static int g_off_p1     = 0;   /* player-1 switch word (2 bytes, JVS wire order)  */
static int g_off_p2     = 2;   /* player-2 switch word                           */
static int g_off_system = 4;   /* system/coin byte                               */
static int g_off_analog = 5;   /* analog block (turntable/slider/knobs), 16-bit   */
static int g_analog_n   = 4;

static void load_config(void) {
#if defined(_WIN32)
    const char* e;
    if ((e = getenv("ULTRA_KONAMI_NODE_ID"))  && *e) g_node_id      = (uint32_t)strtoul(e, NULL, 0);
    if ((e = getenv("ULTRA_KONAMI_NODE_VER")) && *e) g_node_version = (uint8_t)strtoul(e, NULL, 0);
    if ((e = getenv("ULTRA_KONAMI_NUM_NODES"))&& *e) { int v = atoi(e); if (v >= 1 && v <= 16) g_num_nodes = v; }
    if ((e = getenv("ULTRA_KONAMI_OFF_P1"))     && *e) g_off_p1     = atoi(e);
    if ((e = getenv("ULTRA_KONAMI_OFF_P2"))     && *e) g_off_p2     = atoi(e);
    if ((e = getenv("ULTRA_KONAMI_OFF_SYSTEM")) && *e) g_off_system = atoi(e);
    if ((e = getenv("ULTRA_KONAMI_OFF_ANALOG")) && *e) g_off_analog = atoi(e);
    if ((e = getenv("ULTRA_KONAMI_ANALOG_N"))   && *e) { int v = atoi(e); if (v >= 0 && v <= 16) g_analog_n = v; }
#endif
}

/* ===========================================================================
 *  ACIO frame codec -- the clean-room core (fully unit-testable, no OS deps).
 *
 *  Encode: SYNC, then for each body byte b: if b is 0xAA or 0xFF, emit 0xFF then
 *  (~b & 0xFF); else emit b. The body is [addr, cmd_hi, cmd_lo, len, payload...,
 *  checksum]; checksum = sum(addr..last payload byte) & 0xFF. SYNC is NOT escaped
 *  (it is the unambiguous frame start).
 * =========================================================================== */

/* Append one body byte to `out`, escaping per ACIO rules. Returns bytes written. */
static size_t acio_emit(uint8_t* out, size_t cap, size_t p, uint8_t b) {
    if (b == ACIO_SYNC || b == ACIO_ESCAPE) {
        if (p + 1 < cap) { out[p++] = ACIO_ESCAPE; out[p++] = (uint8_t)(~b); }
    } else {
        if (p < cap) out[p++] = b;
    }
    return p;
}

/* Build a full ACIO frame from a logical body (addr,cmd16,payload...). Computes
 * len + checksum, escapes, prepends SYNC. Returns total wire bytes (0 if it would
 * overflow `out`). */
size_t konami_acio_encode(uint8_t addr, uint16_t cmd,
                          const uint8_t* payload, size_t plen,
                          uint8_t* out, size_t cap) {
    if (!out || cap < 2) return 0;
    /* logical body: addr, cmd_hi, cmd_lo, len, payload..., checksum */
    uint8_t body[260];
    size_t  bn = 0;
    if (plen > 250) return 0;
    body[bn++] = addr;
    body[bn++] = (uint8_t)(cmd >> 8);
    body[bn++] = (uint8_t)(cmd & 0xFF);
    body[bn++] = (uint8_t)plen;
    for (size_t i = 0; i < plen; ++i) body[bn++] = payload ? payload[i] : 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < bn; ++i) sum = (uint8_t)(sum + body[i]);
    body[bn++] = sum;

    size_t p = 0;
    out[p++] = ACIO_SYNC;
    for (size_t i = 0; i < bn; ++i) {
        size_t np = acio_emit(out, cap, p, body[i]);
        if (np == p && !(body[i] != ACIO_SYNC && body[i] != ACIO_ESCAPE && p < cap)) {
            /* overflow */
            if (np >= cap) return 0;
        }
        p = np;
    }
    if (p > cap) return 0;
    return p;
}

/* Decode one ACIO frame from `in`. On success fills addr/cmd/payload (payload is
 * the bytes between len and checksum) and verifies the additive checksum.
 * Returns the number of input bytes consumed (the frame length on the wire), or 0
 * if no complete, valid frame is present at the start of `in`. */
size_t konami_acio_decode(const uint8_t* in, size_t in_len,
                          uint8_t* addr, uint16_t* cmd,
                          uint8_t* payload, size_t pcap, size_t* plen) {
    if (!in || in_len < 6) return 0;
    /* find SYNC */
    size_t start = 0;
    while (start < in_len && in[start] != ACIO_SYNC) ++start;
    if (start >= in_len) return 0;
    size_t wp = start + 1;

    /* unescape the body into a scratch buffer until we have enough for header+len. */
    uint8_t body[260]; size_t bn = 0;
    /* We need at least addr,cmdhi,cmdlo,len before we know the total length. */
    while (wp < in_len && bn < sizeof(body)) {
        uint8_t b = in[wp];
        if (b == ACIO_SYNC) break;              /* next frame start; this one ended */
        if (b == ACIO_ESCAPE) {
            if (wp + 1 >= in_len) return 0;      /* incomplete escape -> wait */
            body[bn++] = (uint8_t)(~in[wp+1]);
            wp += 2;
        } else {
            body[bn++] = b;
            wp += 1;
        }
        /* once we have the len field, we know the full body size = 4 + len + 1. */
        if (bn >= 4) {
            size_t need = 4 + (size_t)body[3] + 1;   /* hdr(4) + payload + checksum */
            if (bn >= need) { bn = need; break; }
        }
    }
    if (bn < 5) return 0;                         /* not a complete frame yet */
    size_t need = 4 + (size_t)body[3] + 1;
    if (bn < need) return 0;

    /* verify additive checksum over body[0..need-2]. */
    uint8_t sum = 0;
    for (size_t i = 0; i + 1 < need; ++i) sum = (uint8_t)(sum + body[i]);
    if (sum != body[need - 1]) return 0;          /* bad checksum */

    if (addr) *addr = body[0];
    if (cmd)  *cmd  = (uint16_t)((body[1] << 8) | body[2]);
    size_t pl = body[3];
    if (plen) *plen = pl;
    if (payload && pcap > 0) {
        size_t cp = pl < pcap ? pl : pcap;
        memcpy(payload, &body[4], cp);
    }
    return wp - start;   /* bytes consumed from `in` (from SYNC to end of frame) */
}

/* ===========================================================================
 *  Node model -- answer a decoded request from the live input.
 *
 *  Returns the number of wire bytes written to `out` (a full encoded response
 *  frame), or 0 if the command needs something we deliberately do not provide
 *  (e.g. a secret -- there are none on the ACIO input bus, but we defer unknowns).
 * =========================================================================== */

/* Build the input poll payload from the live cabinet input (JVS-shaped + analog).*/
static size_t konami_build_poll_payload(uint8_t* p, size_t cap) {
    if (cap < 1) return 0;
    memset(p, 0, cap);
    ultra_jvs_input in;
    ultra_hook_current_input(&in);

    #define KPUT16(off, val) do { int o=(off); if (o>=0 && (size_t)(o+1) < cap) { \
        p[o] = (uint8_t)((val) >> 8); p[o+1] = (uint8_t)((val) & 0xFF); } } while (0)
    KPUT16(g_off_p1, in.player[0]);
    if (JVS_MAX_PLAYERS > 1) KPUT16(g_off_p2, in.player[1]);
    if (g_off_system >= 0 && (size_t)g_off_system < cap) p[g_off_system] = in.system;
    for (int a = 0; a < g_analog_n && a < JVS_MAX_ANALOG; ++a)
        KPUT16(g_off_analog + a*2, in.analog[a]);
    #undef KPUT16

    /* payload length = highest field used + 2, capped. */
    size_t used = (size_t)(g_off_analog + g_analog_n*2);
    if (used > cap) used = cap;
    if (used < 6) used = 6;
    return used;
}

size_t konami_acio_answer(uint8_t addr, uint16_t cmd,
                          const uint8_t* req_payload, size_t req_plen,
                          uint8_t* out, size_t out_cap) {
    (void)req_payload; (void)req_plen;
    uint8_t pl[64];
    switch (cmd) {
        case ACIO_CMD_ASSIGN_ADDR: {
            /* enumerate: report the number of nodes downstream. */
            pl[0] = (uint8_t)g_num_nodes;
            return konami_acio_encode(addr, cmd, pl, 1, out, out_cap);
        }
        case ACIO_CMD_GET_VERSION: {
            /* node ID (4 bytes, big-endian) + version. */
            pl[0] = (uint8_t)(g_node_id >> 24);
            pl[1] = (uint8_t)(g_node_id >> 16);
            pl[2] = (uint8_t)(g_node_id >> 8);
            pl[3] = (uint8_t)(g_node_id & 0xFF);
            pl[4] = g_node_version;
            return konami_acio_encode(addr, cmd, pl, 5, out, out_cap);
        }
        case ACIO_CMD_STARTUP: {
            pl[0] = 0x00;   /* ok */
            return konami_acio_encode(addr, cmd, pl, 1, out, out_cap);
        }
        case ACIO_CMD_POLL: {
            size_t n = konami_build_poll_payload(pl, sizeof pl);
            return konami_acio_encode(addr, cmd, pl, n, out, out_cap);
        }
        case ACIO_CMD_SET_LIGHTS: {
            /* accept lamp state (nothing to physically drive) and ack. */
            pl[0] = 0x00;
            return konami_acio_encode(addr, cmd, pl, 1, out, out_cap);
        }
        default:
            /* unknown / game-specific command we cannot answer generically -> defer
             * (let a real node / per-dump profile handle it). */
            return 0;
    }
}

/* High-level: feed raw host->bus bytes, produce any response bytes. Mirrors the
 * jvs_host_bytes() shape so a transport seam can pipe a COM/USB stream through it. */
size_t konami_io_host_bytes(const uint8_t* in, size_t in_len,
                            uint8_t* out, size_t out_cap) {
    if (!g_enabled || !in || in_len == 0 || !out || out_cap == 0) return 0;
    uint8_t addr; uint16_t cmd; uint8_t payload[64]; size_t plen = 0;
    size_t consumed = konami_acio_decode(in, in_len, &addr, &cmd, payload, sizeof payload, &plen);
    if (consumed == 0) return 0;
    return konami_acio_answer(addr, cmd, payload, plen, out, out_cap);
}

#if defined(_WIN32)
/* ---------------------------------------------------------------------------
 * Transport seam. P3IO presents as a USB device; the ACIO sub-bus also appears on
 * a COM alias. We intercept the device OPEN (CreateFile on the P3IO/ACIO path) and
 * hand back a synthetic handle whose ReadFile/WriteFile route through the codec.
 * The precise device path is per-board; ULTRA_KONAMI_DEV pins a substring.
 * ------------------------------------------------------------------------- */
typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static pCreateFileA Real_CreateFileA = NULL;
typedef BOOL (WINAPI *pReadFile)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL (WINAPI *pWriteFile)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
static pReadFile  Real_ReadFile  = NULL;
static pWriteFile Real_WriteFile = NULL;

#define KONAMI_SENTINEL ((HANDLE)(ULONG_PTR)0xC04A0001)

static char g_dev[64] = "";   /* extra device-path substring the user can set     */

/* pending response bytes produced by the last WriteFile, served by ReadFile. */
static uint8_t g_resp[512];
static size_t  g_resp_len = 0;
static size_t  g_resp_pos = 0;

static int konami_path_matches(const char* n) {
    if (!n) return 0;
    char low[260]; int i = 0;
    for (; n[i] && i < 259; ++i) { char c = n[i]; low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
    low[i] = 0;
    if (g_dev[0] && strstr(low, g_dev)) return 1;
    return strstr(low, "p3io") || strstr(low, "p4io") || strstr(low, "acio") ||
           strstr(low, "bio2") || strstr(low, "kfca");
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && konami_path_matches(name)) {
        HANDLE real = Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                                       : INVALID_HANDLE_VALUE;
        if (real != INVALID_HANDLE_VALUE) return real;
        klog("[konami] synthesizing ACIO/P3IO presence (no physical I/O PCB)\n");
        return KONAMI_SENTINEL;
    }
    return Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}

static BOOL WINAPI Hook_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote, LPOVERLAPPED ov) {
    if (g_enabled && h == KONAMI_SENTINEL) {
        g_resp_len = konami_io_host_bytes((const uint8_t*)buf, (size_t)n, g_resp, sizeof g_resp);
        g_resp_pos = 0;
        if (wrote) *wrote = n;
        return TRUE;
    }
    return Real_WriteFile ? Real_WriteFile(h, buf, n, wrote, ov) : FALSE;
}
static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    if (g_enabled && h == KONAMI_SENTINEL) {
        size_t avail = g_resp_len - g_resp_pos;
        size_t cp = ((size_t)n < avail) ? (size_t)n : avail;
        if (cp) memcpy(buf, &g_resp[g_resp_pos], cp);
        g_resp_pos += cp;
        if (got) *got = (DWORD)cp;
        return TRUE;
    }
    return Real_ReadFile ? Real_ReadFile(h, buf, n, got, ov) : FALSE;
}

void ultra_konami_io_install(void) {
    const char* e = getenv("ULTRA_KONAMI_IO");
    g_enabled = e && e[0] == '1';
    if (!g_enabled) return;
    load_config();
    const char* dv = getenv("ULTRA_KONAMI_DEV");
    if (dv && dv[0]) { strncpy(g_dev, dv, sizeof(g_dev)-1); g_dev[sizeof(g_dev)-1] = 0;
        for (char* q = g_dev; *q; ++q) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32); }

    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (k) {
        if (!Real_CreateFileA) Real_CreateFileA = (pCreateFileA)(void*)GetProcAddress(k, "CreateFileA");
        if (!Real_ReadFile)    Real_ReadFile    = (pReadFile)(void*)GetProcAddress(k, "ReadFile");
        if (!Real_WriteFile)   Real_WriteFile   = (pWriteFile)(void*)GetProcAddress(k, "WriteFile");
    }
    ultra_iat_patch_all("kernel32.dll", "CreateFileA", (void*)Hook_CreateFileA);
    ultra_iat_patch_all("kernel32.dll", "ReadFile",    (void*)Hook_ReadFile);
    ultra_iat_patch_all("kernel32.dll", "WriteFile",   (void*)Hook_WriteFile);

    klog("[konami] ACIO I/O scaffold armed (node ID 0x%08lX, %d node(s)). JVS-backed "
         "inputs; lamps accepted. NOTE: per-game node ID + input bit-map are pinned via "
         "ULTRA_KONAMI_NODE_*; iKey dongle presence = dongle_bypass.c; e-amusement/Paseli "
         "online = OUT of scope.\n", (unsigned long)g_node_id, g_num_nodes);
}

int ultra_konami_io_enabled(void) { return g_enabled; }
#endif /* _WIN32 */
