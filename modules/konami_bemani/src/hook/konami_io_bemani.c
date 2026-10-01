/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  konami_io_bemani.c -- Konami Bemani-PC arcade-I/O (ACIO bus) scaffold
 *                        (env-gated, OFF) -- konami_bemani board's OWN copy.
 * =============================================================================
 *
 *  The Konami Bemani PC I/O PCBs (P3IO, P4IO, BIO2/BI2A, KFCA, USB-IO2) bridge a
 *  USB endpoint to an internal "ACIO" serial bus (RS-232/485, multi-node). ACIO is
 *  NOT JVS -- it has its own framing -- so the generic JVS engine in this same
 *  tree (jvs_core_bemani.c) cannot answer it; it needs its own frame codec + node
 *  model. Per-game inputs (IIDX turntable + 7 keys, pop'n buttons, SDVX
 *  slider/knobs, DDR pads, cabinet lamps) ride ACIO nodes as game-specific bit
 *  layouts.
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
 *    (ultra_kb_jvs_input, jvs_core_bemani.h). The per-game node ID + the input
 *    bit-map are env-knob-configured against the user's dump (ULTRA_KONAMI_NODE_*),
 *    NOT baked per title -- exactly the "I/O contract pinned against a dump"
 *    stance the rest of this project's per-board I/O scaffolds use.
 *
 *  WHAT IT IS NOT (the ethical / legal boundary this file preserves EXACTLY,
 *  unchanged from the native_x86 reference this was re-derived against):
 *    It ships NO game data, NO keys. The iKey-2032-class security dongle
 *    PRESENCE is out of scope for this scaffold (this board has no dump to pin
 *    a real bypass against). e-amusement / Paseli is a LIVE online service ->
 *    OUT of scope, never impersonated. RSA/AES game-data content keys are
 *    content-key-derivation -> OUT of scope, never implemented. This file
 *    answers ONLY the generic I/O bus (enumerate/version/poll-input/lights);
 *    it never touches, derives, or fakes possession of any cryptographic key
 *    material for game content.
 *
 *  CLEAN-ROOM: this is the konami_bemani module's OWN independent copy of the
 *  ACIO scaffold pattern (native_x86/src/hook/konami_io.c was read only as
 *  reference while writing this file; the logic is re-expressed here board-
 *  locally, over the public ACIO protocol facts + this tree's OWN
 *  ultra_kb_jvs_input block, per project policy that every board module be
 *  fully self-contained -- no #include, no link, no runtime dependency on the
 *  native_x86 module's compiled output). No third-party loader source,
 *  profile, or I/O binary was read or copied.
 *
 *  This file ALSO owns the shared IAT-patch machinery every hook TU in this
 *  board's OWN tree uses (ultra_kb_iat_patch_all) and the GetProcAddress
 *  interception that catches a packed game's dynamic resolve of
 *  Direct3DCreate9 -- the same pattern every other board module in this
 *  project uses, re-expressed here as this board's own independent copy.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#endif

#include <stdint.h>
#include <string.h>

#include "jvs_core_bemani.h"

/* ---- IS THIS MODULE STILL MAPPED? -----------------------------------------
 * EnumProcessModules hands back a SNAPSHOT. A DLL freed between that call and
 * our read of its PE headers leaves an unmapped base, and reading it is an
 * unhandled access violation that kills the game -- measured on Caladrius AC,
 * where the faulting address was exactly the base of a d3dcompiler_42.dll that
 * D3DX9 had just unloaded. mingw32 has no __try, so ask the memory manager.
 * This narrows the race rather than closing it, and that is acceptable because
 * it can only SUBTRACT: it refuses memory we could not have read anyway. */
static const unsigned char* g_ulpe_lo;
static const unsigned char* g_ulpe_hi;

static int ul_pe_readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    const unsigned char* a = (const unsigned char*)p;
    const unsigned char* end;
    if (!a) return 0;
    end = a + n;
    if (a >= g_ulpe_lo && end <= g_ulpe_hi) return 1;      /* cached fast path */
    while (a < end) {
        if (!VirtualQuery(a, &mbi, sizeof mbi))   return 0;
        if (mbi.State != MEM_COMMIT)              return 0;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        if (mbi.Protect & PAGE_EXECUTE)           return 0;  /* execute-only */
        g_ulpe_lo = (const unsigned char*)mbi.BaseAddress;
        g_ulpe_hi = g_ulpe_lo + mbi.RegionSize;
        a = g_ulpe_hi;
    }
    return 1;
}

/* Shared diagnostic log (hook_dllmain_bemani.c); gated on ULTRA_JVS_LOG. */
void ultra_kb_log(const char* fmt, ...);
/* Live controller snapshot (hook_dllmain_bemani.c). */
void ultra_kb_hook_current_input(ultra_kb_jvs_input* out);

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

static int g_kb_enabled = 0;

/* The synthetic node's ACIO node ID (e.g. KFCA 0x09060000). Per-dump-overridable
 * via ULTRA_KONAMI_NODE_ID so the game's enumerate matches whatever node its I/O
 * profile expects. Default is a widely-seen KFCA-class id. */
static uint32_t g_kb_node_id      = 0x09060000u;
static uint8_t  g_kb_node_version = 0x01;
static int      g_kb_num_nodes    = 1;

/* Where, in the node poll payload, each input field lands. ACIO input layouts are
 * per-game (turntable byte, key bits, slider, knobs), so these are env knobs the
 * user's dump pins. Defaults place a JVS-shaped digital + analog block. */
static int g_kb_off_p1     = 0;   /* player-1 switch word (2 bytes, JVS wire order)  */
static int g_kb_off_p2     = 2;   /* player-2 switch word                           */
static int g_kb_off_system = 4;   /* system/coin byte                               */
static int g_kb_off_analog = 5;   /* analog block (turntable/slider/knobs), 16-bit   */
static int g_kb_analog_n   = 4;

static void kb_load_config(void) {
#if defined(_WIN32)
    const char* e;
    if ((e = getenv("ULTRA_KONAMI_NODE_ID"))  && *e) g_kb_node_id      = (uint32_t)strtoul(e, NULL, 0);
    if ((e = getenv("ULTRA_KONAMI_NODE_VER")) && *e) g_kb_node_version = (uint8_t)strtoul(e, NULL, 0);
    if ((e = getenv("ULTRA_KONAMI_NUM_NODES"))&& *e) { int v = atoi(e); if (v >= 1 && v <= 16) g_kb_num_nodes = v; }
    if ((e = getenv("ULTRA_KONAMI_OFF_P1"))     && *e) g_kb_off_p1     = atoi(e);
    if ((e = getenv("ULTRA_KONAMI_OFF_P2"))     && *e) g_kb_off_p2     = atoi(e);
    if ((e = getenv("ULTRA_KONAMI_OFF_SYSTEM")) && *e) g_kb_off_system = atoi(e);
    if ((e = getenv("ULTRA_KONAMI_OFF_ANALOG")) && *e) g_kb_off_analog = atoi(e);
    if ((e = getenv("ULTRA_KONAMI_ANALOG_N"))   && *e) { int v = atoi(e); if (v >= 0 && v <= 16) g_kb_analog_n = v; }
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
static size_t kb_acio_emit(uint8_t* out, size_t cap, size_t p, uint8_t b) {
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
size_t kb_acio_encode(uint8_t addr, uint16_t cmd,
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
        size_t np = kb_acio_emit(out, cap, p, body[i]);
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
size_t kb_acio_decode(const uint8_t* in, size_t in_len,
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
 *  (there is no secret on the ACIO input bus, but unknown commands are
 *  deferred, never guessed at).
 * =========================================================================== */

/* Build the input poll payload from the live cabinet input (JVS-shaped + analog).*/
static size_t kb_build_poll_payload(uint8_t* p, size_t cap) {
    if (cap < 1) return 0;
    memset(p, 0, cap);
    ultra_kb_jvs_input in;
    ultra_kb_hook_current_input(&in);

    #define KPUT16(off, val) do { int o=(off); if (o>=0 && (size_t)(o+1) < cap) { \
        p[o] = (uint8_t)((val) >> 8); p[o+1] = (uint8_t)((val) & 0xFF); } } while (0)
    KPUT16(g_kb_off_p1, in.player[0]);
    if (JVS_MAX_PLAYERS > 1) KPUT16(g_kb_off_p2, in.player[1]);
    if (g_kb_off_system >= 0 && (size_t)g_kb_off_system < cap) p[g_kb_off_system] = in.system;
    for (int a = 0; a < g_kb_analog_n && a < JVS_MAX_ANALOG; ++a)
        KPUT16(g_kb_off_analog + a*2, in.analog[a]);
    #undef KPUT16

    /* payload length = highest field used + 2, capped. */
    size_t used = (size_t)(g_kb_off_analog + g_kb_analog_n*2);
    if (used > cap) used = cap;
    if (used < 6) used = 6;
    return used;
}

size_t kb_acio_answer(uint8_t addr, uint16_t cmd,
                      const uint8_t* req_payload, size_t req_plen,
                      uint8_t* out, size_t out_cap) {
    (void)req_payload; (void)req_plen;
    uint8_t pl[64];
    switch (cmd) {
        case ACIO_CMD_ASSIGN_ADDR: {
            /* enumerate: report the number of nodes downstream. */
            pl[0] = (uint8_t)g_kb_num_nodes;
            return kb_acio_encode(addr, cmd, pl, 1, out, out_cap);
        }
        case ACIO_CMD_GET_VERSION: {
            /* node ID (4 bytes, big-endian) + version. */
            pl[0] = (uint8_t)(g_kb_node_id >> 24);
            pl[1] = (uint8_t)(g_kb_node_id >> 16);
            pl[2] = (uint8_t)(g_kb_node_id >> 8);
            pl[3] = (uint8_t)(g_kb_node_id & 0xFF);
            pl[4] = g_kb_node_version;
            return kb_acio_encode(addr, cmd, pl, 5, out, out_cap);
        }
        case ACIO_CMD_STARTUP: {
            pl[0] = 0x00;   /* ok */
            return kb_acio_encode(addr, cmd, pl, 1, out, out_cap);
        }
        case ACIO_CMD_POLL: {
            size_t n = kb_build_poll_payload(pl, sizeof pl);
            return kb_acio_encode(addr, cmd, pl, n, out, out_cap);
        }
        case ACIO_CMD_SET_LIGHTS: {
            /* accept lamp state (nothing to physically drive) and ack. */
            pl[0] = 0x00;
            return kb_acio_encode(addr, cmd, pl, 1, out, out_cap);
        }
        default:
            /* unknown / game-specific command we cannot answer generically -> defer
             * (let a real node / per-dump profile handle it). */
            return 0;
    }
}

/* High-level: feed raw host->bus bytes, produce any response bytes. */
size_t kb_konami_io_host_bytes(const uint8_t* in, size_t in_len,
                               uint8_t* out, size_t out_cap) {
    if (!g_kb_enabled || !in || in_len == 0 || !out || out_cap == 0) return 0;
    uint8_t addr; uint16_t cmd; uint8_t payload[64]; size_t plen = 0;
    size_t consumed = kb_acio_decode(in, in_len, &addr, &cmd, payload, sizeof payload, &plen);
    if (consumed == 0) return 0;
    return kb_acio_answer(addr, cmd, payload, plen, out, out_cap);
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
typedef FARPROC (WINAPI *pGetProcAddress)(HMODULE,LPCSTR);
static pGetProcAddress Real_GetProcAddress = NULL;

#define KONAMI_SENTINEL ((HANDLE)(ULONG_PTR)0xC04A0001)

static char g_kb_dev[64] = "";   /* extra device-path substring the user can set */

/* pending response bytes produced by the last WriteFile, served by ReadFile. */
static uint8_t g_kb_resp[512];
static size_t  g_kb_resp_len = 0;
static size_t  g_kb_resp_pos = 0;

static int kb_path_matches(const char* n) {
    if (!n) return 0;
    char low[260]; int i = 0;
    for (; n[i] && i < 259; ++i) { char c = n[i]; low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
    low[i] = 0;
    if (g_kb_dev[0] && strstr(low, g_kb_dev)) return 1;
    return strstr(low, "p3io") || strstr(low, "p4io") || strstr(low, "acio") ||
           strstr(low, "bio2") || strstr(low, "kfca");
}

static HANDLE WINAPI Hook_kb_CreateFileA(LPCSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_kb_enabled && kb_path_matches(name)) {
        HANDLE real = Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                                       : INVALID_HANDLE_VALUE;
        if (real != INVALID_HANDLE_VALUE) return real;
        ultra_kb_log("[konami_bemani] synthesizing ACIO/P3IO presence (no physical I/O PCB)\n");
        return KONAMI_SENTINEL;
    }
    return Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}

static BOOL WINAPI Hook_kb_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote, LPOVERLAPPED ov) {
    if (g_kb_enabled && h == KONAMI_SENTINEL) {
        g_kb_resp_len = kb_konami_io_host_bytes((const uint8_t*)buf, (size_t)n, g_kb_resp, sizeof g_kb_resp);
        g_kb_resp_pos = 0;
        if (wrote) *wrote = n;
        return TRUE;
    }
    return Real_WriteFile ? Real_WriteFile(h, buf, n, wrote, ov) : FALSE;
}
static BOOL WINAPI Hook_kb_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    if (g_kb_enabled && h == KONAMI_SENTINEL) {
        size_t avail = g_kb_resp_len - g_kb_resp_pos;
        size_t cp = ((size_t)n < avail) ? (size_t)n : avail;
        if (cp) memcpy(buf, &g_kb_resp[g_kb_resp_pos], cp);
        g_kb_resp_pos += cp;
        if (got) *got = (DWORD)cp;
        return TRUE;
    }
    return Real_ReadFile ? Real_ReadFile(h, buf, n, got, ov) : FALSE;
}

/* -------------------------------------------------------------------------- */
/*  IAT patching across all loaded modules -- shared by every hook TU in this  */
/*  board's OWN tree (audio_hook_bemani.c, d3d9_throttle_bemani.c,             */
/*  dinput8_emu_bemani.c). This board's own independent copy of the pattern    */
/*  every board module in this project uses (native_x86/src/hook/             */
/*  jvs_serial_hook.c was read only as reference).                             */
/* -------------------------------------------------------------------------- */
static int kb_rva_ok(DWORD rva, DWORD isize) { return rva != 0 && rva < isize; }

static void kb_iat_patch_module(HMODULE mod, const char* impdll,
                                const char* impfn, void* newfn, void* realfn) {
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (!ul_pe_readable(dos, sizeof *dos)) return;   /* module unloaded under us */
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (!ul_pe_readable(nt, sizeof *nt)) return;
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    DWORD isize = nt->OptionalHeader.SizeOfImage;
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!kb_rva_ok(dir.VirtualAddress, isize)) return;
    IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    for (; kb_rva_ok(desc->Name, isize); ++desc) {
        const char* dllname = (const char*)(base + desc->Name);
        if (_stricmp(dllname, impdll) != 0) continue;
        DWORD orig_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        IMAGE_THUNK_DATA* orig = kb_rva_ok(orig_rva, isize) ? (IMAGE_THUNK_DATA*)(base + orig_rva) : NULL;
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);
        /* Bound EACH thunk read against the mapped image: a malformed/packed import
         * whose IAT array is not NULL-terminated within the image would otherwise
         * walk off the end and fault (no SEH guard under mingw). */
        for (; (size_t)((BYTE*)iat - base) + sizeof(*iat) <= (size_t)isize
               && (!orig || (size_t)((BYTE*)orig - base) + sizeof(*orig) <= (size_t)isize)
               && iat->u1.Function; ++iat, orig = orig ? orig + 1 : NULL) {
            int match = 0;
            if (orig && !(orig->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                && kb_rva_ok((DWORD)orig->u1.AddressOfData, isize)) {
                IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + orig->u1.AddressOfData);
                if (strcmp((const char*)ibn->Name, impfn) == 0) match = 1;
            }
            if (!match && realfn && (void*)(uintptr_t)iat->u1.Function == realfn) match = 1;
            if (match) {
                DWORD old;
                /* ALREADY OURS -> DO NOTHING. This early-out is not an
                 * optimization, it is a correctness fix.
                 *
                 * The re-patch worker calls this every second for 30 seconds
                 * over every loaded module. Without this check each pass
                 * re-ran VirtualProtect(PAGE_READWRITE) -> store -> restore on
                 * slots that already held our hook. VirtualProtect works at
                 * PAGE granularity and PAGE_READWRITE strips EXECUTE from the
                 * entire page, so any thread executing code sharing a page with
                 * a patched slot during that window takes a DEP fault.
                 *
                 * Measured on Mario Kart Arcade GP DX, 2026-09-10, 8 runs:
                 * with the periodic re-patch running, 3 of 4 launches died with
                 * EXECUTE/DEP at a valid .text address and every stack frame in
                 * the game; with it disabled, 0 of 4. Skipping the no-op writes
                 * removes the window while still catching a DLL the game loads
                 * late, which is the only reason the worker exists. */
                if ((void*)(uintptr_t)iat->u1.Function == newfn) continue;
                if (VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                    iat->u1.Function = (ULONG_PTR)(uintptr_t)newfn;
                    VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
                }
            }
        }
    }
}

void ultra_kb_iat_patch_all(const char* impdll, const char* impfn, void* newfn) {
    void* realfn = NULL;
    HMODULE imp = GetModuleHandleA(impdll);
    if (imp) realfn = (void*)GetProcAddress(imp, impfn);   /* for the value-based pass */

    HMODULE mods[512]; DWORD need = 0;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ultra_kb_iat_patch_all, &self);
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        DWORD count = need / sizeof(HMODULE);
        if (count > 512) count = 512;
        for (DWORD i = 0; i < count; ++i)
            if (mods[i] != self)                 /* don't patch our own imports */
                kb_iat_patch_module(mods[i], impdll, impfn, newfn, realfn);
    }
}

/* GetProcAddress interception: catches a packed game's DYNAMIC resolve of
 * Direct3DCreate9 (no static IAT slot exists for the IAT patch to find in that
 * case) -- the canonical packed-game present-hook entry point every board
 * module in this project uses. Also hands back our own ACIO shims for
 * dynamically-resolved kernel32 I/O calls. */
static FARPROC WINAPI Hook_kb_GetProcAddress(HMODULE mod, LPCSTR name) {
    if ((((uintptr_t)name) >> 16) != 0) {   /* by name, not ordinal */
        if (strcmp(name, "CreateFileA") == 0) return (FARPROC)Hook_kb_CreateFileA;
        if (strcmp(name, "ReadFile") == 0)    return (FARPROC)Hook_kb_ReadFile;
        if (strcmp(name, "WriteFile") == 0)   return (FARPROC)Hook_kb_WriteFile;
        if (strcmp(name, "GetProcAddress") == 0) return (FARPROC)Hook_kb_GetProcAddress;
        if (strcmp(name, "Direct3DCreate9") == 0) {
            extern void* __stdcall ultra_kb_hook_direct3dcreate9(UINT);
            ultra_kb_log("[konami_bemani] D3D9: GetProcAddress(\"Direct3DCreate9\") intercepted -> hook armed (packed path)\n");
            return (FARPROC)ultra_kb_hook_direct3dcreate9;
        }
    }
    return Real_GetProcAddress ? Real_GetProcAddress(mod, name) : NULL;
}

void ultra_kb_konami_io_install(void) {
    const char* e = getenv("ULTRA_KONAMI_IO");
    g_kb_enabled = e && e[0] == '1';
    if (g_kb_enabled) {
        kb_load_config();
        const char* dv = getenv("ULTRA_KONAMI_DEV");
        if (dv && dv[0]) { strncpy(g_kb_dev, dv, sizeof(g_kb_dev)-1); g_kb_dev[sizeof(g_kb_dev)-1] = 0;
            for (char* q = g_kb_dev; *q; ++q) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32); }
    }

    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (k) {
        if (!Real_CreateFileA)    Real_CreateFileA    = (pCreateFileA)(void*)GetProcAddress(k, "CreateFileA");
        if (!Real_ReadFile)       Real_ReadFile       = (pReadFile)(void*)GetProcAddress(k, "ReadFile");
        if (!Real_WriteFile)      Real_WriteFile      = (pWriteFile)(void*)GetProcAddress(k, "WriteFile");
        if (!Real_GetProcAddress) Real_GetProcAddress = (pGetProcAddress)(void*)GetProcAddress(k, "GetProcAddress");
    }
    /* ACIO transport hooks are only USEFUL when the scaffold is armed, but we
     * install the IAT patches unconditionally alongside the D3D9/D3D9-packed
     * hook below so a single install call arms everything this board's own
     * hook DLL needs; Hook_kb_CreateFileA/ReadFile/WriteFile themselves check
     * g_kb_enabled and pass through untouched when the scaffold is OFF. */
    ultra_kb_iat_patch_all("kernel32.dll", "CreateFileA",     (void*)Hook_kb_CreateFileA);
    ultra_kb_iat_patch_all("kernel32.dll", "ReadFile",        (void*)Hook_kb_ReadFile);
    ultra_kb_iat_patch_all("kernel32.dll", "WriteFile",       (void*)Hook_kb_WriteFile);
    ultra_kb_iat_patch_all("kernel32.dll", "GetProcAddress",  (void*)Hook_kb_GetProcAddress);

    /* D3D9 present hook (this board's graphics API). A no-op if the game
     * doesn't statically import Direct3DCreate9 (the GetProcAddress
     * interception above covers the packed/dynamic-resolve case). */
    {
        extern void* __stdcall ultra_kb_hook_direct3dcreate9(UINT);
        ultra_kb_iat_patch_all("d3d9.dll", "Direct3DCreate9", (void*)ultra_kb_hook_direct3dcreate9);
    }

    if (g_kb_enabled)
        ultra_kb_log("[konami_bemani] ACIO I/O scaffold armed (node ID 0x%08lX, %d node(s)). JVS-shaped "
             "inputs; lamps accepted. NOTE: per-game node ID + input bit-map are pinned via "
             "ULTRA_KONAMI_NODE_*; iKey dongle / e-amusement / Paseli / game-data RSA-AES keys "
             "are ALL out of scope -- presence-only, never derived or faked.\n",
             (unsigned long)g_kb_node_id, g_kb_num_nodes);
}

int ultra_kb_konami_io_enabled(void) { return g_kb_enabled; }
#endif /* _WIN32 */
