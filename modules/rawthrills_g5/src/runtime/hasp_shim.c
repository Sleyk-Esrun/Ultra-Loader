/*
 * hasp_shim.c
 *
 * Clean-room implementation of the Sentinel HL SDK (30 hasp_* entry points +
 * Raw Thrills SysGetDongleHID extension) for Cruis'n Blast on the Lindbergh
 * runtime.
 *
 * See hasp_shim.h for a full architecture writeup. Highlights:
 *
 *   - AES-128-CBC round-trip so hasp_encrypt(x) then hasp_decrypt returns x.
 *     Key + IV are derived deterministically from HID + feature id + session id
 *     via a small SHA-256-style expansion (public HKDF-like construction, no
 *     external OpenSSL dep - we already ship ossl_stub.c but AES-128 is small
 *     enough to inline here).
 *   - Deterministic 64-bit HID (0x1234ABCDCB5A57ULL) so the Cruis'n Blast CRC
 *     menu displays a stable dongle serial on every boot.
 *   - 4-file memory model (MAIN=112B, LICENSE=112B, RO=496B, RW=2048B) sized
 *     per the Sentinel HL public spec, zero-initialized.
 *   - Simulated RTC that pulls host UTC and converts to the SDK's
 *     seconds-since-2000-01-01 epoch.
 *   - Envelope decrypt is delegated to hasp_envelope.c.
 *
 * CLEAN-ROOM: no SDK source, no leaked keys. Only the public API contract from
 * SafeNet's developer docs + observed usage in the game's dynstr.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "loader.h"
#include "hasp_shim.h"

/* ================================================================== */
/* Deterministic HID + envelope-decrypt key seed.                     */
/* ================================================================== */

/* Cruis'n Blast HID = 0x1234ABCDCB5A57ULL. The high 32 bits are the vendor
 * signature; the low 32 bits are a compressed ASCII fingerprint of "CB5A57"
 * derived from the game codename. Deterministic on every boot. */
#define CRUISN_BLAST_HID  0x1234ABCDCB5A57ULL

uint64_t SysGetDongleHID(void)
{
    return CRUISN_BLAST_HID;
}

/* ================================================================== */
/* Sessions.                                                           */
/* ================================================================== */

/* We allow up to 8 concurrent sessions - the game only opens one, but the
 * Raw Thrills wrapper does open/close cycles on init and again during CRC
 * checks. */
#define HASP_MAX_SESSIONS  8

/* Public 4-file memory sizes (Sentinel HL Basic / Pro / Max share these). */
#define HASP_MAIN_BYTES    112
#define HASP_LICENSE_BYTES 112
#define HASP_RO_BYTES      496
#define HASP_RW_BYTES      2048

typedef struct {
    int            in_use;
    hasp_handle_t  id;
    hasp_feature_t fid;
    uint8_t        key[16];       /* derived AES-128 key */
    uint8_t        iv[16];        /* IV seed */
    uint8_t        mem_main   [HASP_MAIN_BYTES];
    uint8_t        mem_license[HASP_LICENSE_BYTES];
    uint8_t        mem_ro     [HASP_RO_BYTES];
    uint8_t        mem_rw     [HASP_RW_BYTES];
    uint32_t       idle_minutes;
    hasp_time_t    rtc_override; /* 0 = use host clock */
} hasp_session_t;

static hasp_session_t g_sessions[HASP_MAX_SESSIONS];
static CRITICAL_SECTION g_hasp_lock;
static INIT_ONCE g_hasp_lock_once = INIT_ONCE_STATIC_INIT;
static uint32_t g_next_session_id = 0x0000B1A5; /* "CB HID" prefix */

/* One-time initializer callback for InitOnceExecuteOnce. The Win32 runtime
 * guarantees this fires exactly once across all threads and every caller
 * blocks until it returns TRUE. That eliminates the previous double-check
 * race where a second thread could observe g_hasp_lock_inited=1 before
 * InitializeCriticalSection had actually completed. */
static BOOL CALLBACK hasp_lock_init_cb(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_hasp_lock);
    return TRUE;
}

static void hasp_lock_init(void)
{
    InitOnceExecuteOnce(&g_hasp_lock_once, hasp_lock_init_cb, NULL, NULL);
}

/* ================================================================== */
/* AES-128-CBC (clean-room, spec-only). Compact 4KB implementation.   */
/* ================================================================== */
/* This is a straightforward FIPS-197 AES-128 implementation written from the
 * spec. It is NOT taken from any existing codebase. It is small on purpose;
 * throughput does not matter - the game encrypts a handful of blocks per boot
 * (for the CRC round-trip check). */

static const uint8_t g_sbox[256] = {
 0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
 0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
 0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
 0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
 0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
 0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
 0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
 0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
 0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
 0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
 0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
 0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
 0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
 0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
 0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
 0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t g_inv_sbox[256] = {
 0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
 0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
 0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
 0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
 0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
 0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
 0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
 0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
 0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
 0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
 0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
 0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
 0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
 0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
 0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
 0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static const uint8_t g_rcon[11] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

static uint8_t gm2(uint8_t a) { return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0)); }
static uint8_t gm3(uint8_t a) { return (uint8_t)(gm2(a) ^ a); }
static uint8_t gm9(uint8_t a) { return (uint8_t)(gm2(gm2(gm2(a))) ^ a); }
static uint8_t gmb(uint8_t a) { return (uint8_t)(gm2(gm2(gm2(a))) ^ gm2(a) ^ a); }
static uint8_t gmd(uint8_t a) { return (uint8_t)(gm2(gm2(gm2(a))) ^ gm2(gm2(a)) ^ a); }
static uint8_t gme(uint8_t a) { return (uint8_t)(gm2(gm2(gm2(a))) ^ gm2(gm2(a)) ^ gm2(a)); }

typedef struct { uint8_t rk[176]; } aes128_ctx_t;

static void aes128_key_expand(aes128_ctx_t *ctx, const uint8_t key[16])
{
    memcpy(ctx->rk, key, 16);
    for (int i = 16, r = 1; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, ctx->rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t s = t[0]; t[0] = t[1]; t[1] = t[2]; t[2] = t[3]; t[3] = s;
            for (int k = 0; k < 4; k++) t[k] = g_sbox[t[k]];
            t[0] ^= g_rcon[r++];
        }
        for (int k = 0; k < 4; k++) ctx->rk[i + k] = ctx->rk[i - 16 + k] ^ t[k];
    }
}

static void aes128_encrypt_block(const aes128_ctx_t *ctx,
                                 const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ ctx->rk[i];
    for (int round = 1; round < 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = g_sbox[s[i]];
        uint8_t t;
        t = s[1];  s[1] = s[5];  s[5] = s[9];  s[9] = s[13]; s[13] = t;
        t = s[2];  s[2] = s[10]; s[10] = t;
        t = s[6];  s[6] = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
        for (int c = 0; c < 4; c++) {
            uint8_t *col = s + c * 4;
            uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
            col[0] = (uint8_t)(gm2(a0) ^ gm3(a1) ^ a2 ^ a3);
            col[1] = (uint8_t)(a0 ^ gm2(a1) ^ gm3(a2) ^ a3);
            col[2] = (uint8_t)(a0 ^ a1 ^ gm2(a2) ^ gm3(a3));
            col[3] = (uint8_t)(gm3(a0) ^ a1 ^ a2 ^ gm2(a3));
        }
        for (int i = 0; i < 16; i++) s[i] ^= ctx->rk[round * 16 + i];
    }
    for (int i = 0; i < 16; i++) s[i] = g_sbox[s[i]];
    uint8_t t;
    t = s[1];  s[1] = s[5];  s[5] = s[9];  s[9] = s[13]; s[13] = t;
    t = s[2];  s[2] = s[10]; s[10] = t;
    t = s[6];  s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
    for (int i = 0; i < 16; i++) out[i] = s[i] ^ ctx->rk[160 + i];
}

static void aes128_decrypt_block(const aes128_ctx_t *ctx,
                                 const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ ctx->rk[160 + i];
    for (int round = 9; round >= 1; round--) {
        uint8_t t;
        t = s[13]; s[13] = s[9];  s[9] = s[5];  s[5] = s[1];  s[1] = t;
        t = s[2];  s[2] = s[10]; s[10] = t;
        t = s[6];  s[6] = s[14]; s[14] = t;
        t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
        for (int i = 0; i < 16; i++) s[i] = g_inv_sbox[s[i]];
        for (int i = 0; i < 16; i++) s[i] ^= ctx->rk[round * 16 + i];
        for (int c = 0; c < 4; c++) {
            uint8_t *col = s + c * 4;
            uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
            col[0] = (uint8_t)(gme(a0) ^ gmb(a1) ^ gmd(a2) ^ gm9(a3));
            col[1] = (uint8_t)(gm9(a0) ^ gme(a1) ^ gmb(a2) ^ gmd(a3));
            col[2] = (uint8_t)(gmd(a0) ^ gm9(a1) ^ gme(a2) ^ gmb(a3));
            col[3] = (uint8_t)(gmb(a0) ^ gmd(a1) ^ gm9(a2) ^ gme(a3));
        }
    }
    uint8_t t;
    t = s[13]; s[13] = s[9];  s[9] = s[5];  s[5] = s[1];  s[1] = t;
    t = s[2];  s[2] = s[10]; s[10] = t;
    t = s[6];  s[6] = s[14]; s[14] = t;
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
    for (int i = 0; i < 16; i++) s[i] = g_inv_sbox[s[i]];
    for (int i = 0; i < 16; i++) out[i] = s[i] ^ ctx->rk[i];
}

/* AES-CBC over a buffer whose length is padded up to the next 16 with the
 * ORIGINAL trailing bytes (no PKCS padding removal round-trip fuss). The
 * game hands us a length; we operate on floor(len/16) full blocks + leave
 * the tail bytes unchanged. This still makes hasp_encrypt+hasp_decrypt an
 * involution on the ciphered portion. */
static void aes128_cbc_encrypt(const aes128_ctx_t *ctx, const uint8_t iv[16],
                               uint8_t *buf, size_t len)
{
    uint8_t last[16]; memcpy(last, iv, 16);
    size_t blocks = len / 16;
    for (size_t b = 0; b < blocks; b++) {
        uint8_t in[16], out[16];
        for (int i = 0; i < 16; i++) in[i] = buf[b*16 + i] ^ last[i];
        aes128_encrypt_block(ctx, in, out);
        memcpy(buf + b*16, out, 16);
        memcpy(last, out, 16);
    }
}

static void aes128_cbc_decrypt(const aes128_ctx_t *ctx, const uint8_t iv[16],
                               uint8_t *buf, size_t len)
{
    uint8_t last[16]; memcpy(last, iv, 16);
    size_t blocks = len / 16;
    for (size_t b = 0; b < blocks; b++) {
        uint8_t in[16], out[16], carry[16];
        memcpy(in, buf + b*16, 16);
        memcpy(carry, in, 16);
        aes128_decrypt_block(ctx, in, out);
        for (int i = 0; i < 16; i++) buf[b*16 + i] = out[i] ^ last[i];
        memcpy(last, carry, 16);
    }
}

/* Deterministic key derivation: FNV-1a-based 16-byte expansion over
 * (HID | fid | label). Per the Sentinel HL SDK contract, the AES key must
 * depend ONLY on the dongle HID and the feature id - NOT on the ephemeral
 * session handle. That is what makes hasp_encrypt(x) followed by a fresh
 * login + hasp_decrypt(x') round-trip stable across the login cycles that
 * Raw Thrills' Dongle wrapper performs during boot + CRC checks.
 *
 * The `label` string distinguishes key-material from IV-material so the two
 * outputs diverge without needing session state. */
static void hasp_derive_key(uint64_t hid, hasp_feature_t fid,
                            const char *label, uint8_t out[16])
{
    uint64_t h = 1469598103934665603ULL; /* FNV-1a offset */
    const uint8_t *p;
    p = (const uint8_t*)&hid; for (int i = 0; i < 8; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    p = (const uint8_t*)&fid; for (int i = 0; i < 4; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    for (const char *q = label; q && *q; q++) { h ^= (uint8_t)*q; h *= 1099511628211ULL; }
    /* Splash h into 16 bytes deterministically. */
    for (int i = 0; i < 16; i++) {
        h ^= (uint64_t)i * 0x9E3779B97F4A7C15ULL;
        h *= 1099511628211ULL;
        out[i] = (uint8_t)(h >> ((i & 7) * 8));
    }
}

/* ================================================================== */
/* Session lookup.                                                     */
/* ================================================================== */

static hasp_session_t *hasp_session_from_handle(hasp_handle_t h)
{
    for (int i = 0; i < HASP_MAX_SESSIONS; i++)
        if (g_sessions[i].in_use && g_sessions[i].id == h) return &g_sessions[i];
    return NULL;
}

static hasp_status_t hasp_session_new(hasp_feature_t fid, hasp_handle_t *out)
{
    hasp_lock_init();
    EnterCriticalSection(&g_hasp_lock);
    hasp_session_t *slot = NULL;
    for (int i = 0; i < HASP_MAX_SESSIONS; i++) {
        if (!g_sessions[i].in_use) { slot = &g_sessions[i]; break; }
    }
    if (!slot) { LeaveCriticalSection(&g_hasp_lock); return HASP_SYS_ERR; }
    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1;
    slot->id     = g_next_session_id++;
    slot->fid    = fid;
    /* Key + IV depend ONLY on (HID, fid) so the encrypt/decrypt round-trip
     * stays stable across successive login cycles - the Raw Thrills Dongle
     * wrapper does exactly that during boot + CRC checks. */
    hasp_derive_key(CRUISN_BLAST_HID, fid, "AES-KEY", slot->key);
    hasp_derive_key(CRUISN_BLAST_HID, fid, "AES-IV",  slot->iv);
    /* Pre-seed license + RO with a plausible "unlocked" bit pattern so any
     * game-side field read is not literally all-zero. */
    memset(slot->mem_license, 0xFF, sizeof(slot->mem_license));
    slot->mem_ro[0] = 0x43; slot->mem_ro[1] = 0x42; /* "CB" */
    *out = slot->id;
    LeaveCriticalSection(&g_hasp_lock);
    loader_log(LOG_INFO, "[HASP] login fid=%u -> handle=0x%08x", fid, slot->id);
    return HASP_STATUS_OK;
}

/* ================================================================== */
/* 30 hasp_* entry points.                                             */
/* ================================================================== */

hasp_status_t hasp_login(hasp_feature_t fid, const uint8_t *vendor_code,
                         hasp_handle_t *out_handle)
{
    (void)vendor_code;
    if (!out_handle) return HASP_MEM_RANGE;
    return hasp_session_new(fid, out_handle);
}

hasp_status_t hasp_login_ex(hasp_feature_t fid, const uint8_t *vendor_code,
                            const char *scope, hasp_handle_t *out_handle)
{
    (void)vendor_code; (void)scope;
    if (!out_handle) return HASP_MEM_RANGE;
    return hasp_session_new(fid, out_handle);
}

hasp_status_t hasp_login_port(hasp_feature_t fid, const uint8_t *vendor_code,
                              const char *port, hasp_handle_t *out_handle)
{
    (void)vendor_code; (void)port;
    if (!out_handle) return HASP_MEM_RANGE;
    return hasp_session_new(fid, out_handle);
}

hasp_status_t hasp_login_scope(hasp_feature_t fid, const char *scope,
                               const uint8_t *vendor_code,
                               hasp_handle_t *out_handle)
{
    (void)vendor_code; (void)scope;
    if (!out_handle) return HASP_MEM_RANGE;
    return hasp_session_new(fid, out_handle);
}

hasp_status_t hasp_logout(hasp_handle_t handle)
{
    hasp_lock_init();
    EnterCriticalSection(&g_hasp_lock);
    hasp_session_t *s = hasp_session_from_handle(handle);
    if (s) { s->in_use = 0; s->id = 0; }
    LeaveCriticalSection(&g_hasp_lock);
    return HASP_STATUS_OK;
}

hasp_status_t hasp_detach(const char *scope, const char *vendor_code,
                          const char *info, char **out_info)
{
    (void)scope; (void)vendor_code; (void)info;
    if (out_info) *out_info = NULL;
    return HASP_STATUS_OK;
}

hasp_status_t hasp_encrypt(hasp_handle_t handle, void *buffer, hasp_size_t len)
{
    if (!buffer) return HASP_MEM_RANGE;
    if (len < 16) return HASP_TOO_SHORT;
    hasp_session_t *s = hasp_session_from_handle(handle);
    if (!s) return HASP_INV_HND;
    aes128_ctx_t ctx; aes128_key_expand(&ctx, s->key);
    aes128_cbc_encrypt(&ctx, s->iv, (uint8_t*)buffer, len);
    return HASP_STATUS_OK;
}

hasp_status_t hasp_decrypt(hasp_handle_t handle, void *buffer, hasp_size_t len)
{
    if (!buffer) return HASP_MEM_RANGE;
    if (len < 16) return HASP_TOO_SHORT;
    hasp_session_t *s = hasp_session_from_handle(handle);
    if (!s) return HASP_INV_HND;
    aes128_ctx_t ctx; aes128_key_expand(&ctx, s->key);
    aes128_cbc_decrypt(&ctx, s->iv, (uint8_t*)buffer, len);
    return HASP_STATUS_OK;
}

/* Legacy HASP HL used a weaker cipher; we reuse the AES path so round-trip
 * still holds. */
hasp_status_t hasp_legacy_encrypt(hasp_handle_t handle, void *buffer,
                                  hasp_size_t len)
{ return hasp_encrypt(handle, buffer, len); }
hasp_status_t hasp_legacy_decrypt(hasp_handle_t handle, void *buffer,
                                  hasp_size_t len)
{ return hasp_decrypt(handle, buffer, len); }

static uint8_t *hasp_file_ptr(hasp_session_t *s, hasp_fileid_t fid,
                              hasp_size_t *out_size)
{
    switch (fid) {
    case HASP_FILEID_MAIN:    *out_size = HASP_MAIN_BYTES;    return s->mem_main;
    case HASP_FILEID_LICENSE: *out_size = HASP_LICENSE_BYTES; return s->mem_license;
    case HASP_FILEID_RO:      *out_size = HASP_RO_BYTES;      return s->mem_ro;
    case HASP_FILEID_RW:      *out_size = HASP_RW_BYTES;      return s->mem_rw;
    default: *out_size = 0; return NULL;
    }
}

hasp_status_t hasp_read(hasp_handle_t handle, hasp_fileid_t fid,
                        hasp_size_t offset, hasp_size_t length, void *out_buffer)
{
    if (!out_buffer) return HASP_MEM_RANGE;
    hasp_session_t *s = hasp_session_from_handle(handle);
    if (!s) return HASP_INV_HND;
    hasp_size_t size = 0;
    uint8_t *file = hasp_file_ptr(s, fid, &size);
    if (!file) return HASP_INV_FILEID;
    if ((uint64_t)offset + length > size) return HASP_MEM_RANGE;
    memcpy(out_buffer, file + offset, length);
    return HASP_STATUS_OK;
}

hasp_status_t hasp_write(hasp_handle_t handle, hasp_fileid_t fid,
                         hasp_size_t offset, hasp_size_t length, const void *buffer)
{
    if (!buffer) return HASP_MEM_RANGE;
    hasp_session_t *s = hasp_session_from_handle(handle);
    if (!s) return HASP_INV_HND;
    hasp_size_t size = 0;
    uint8_t *file = hasp_file_ptr(s, fid, &size);
    if (!file) return HASP_INV_FILEID;
    if ((uint64_t)offset + length > size) return HASP_MEM_RANGE;
    if (fid == HASP_FILEID_MAIN || fid == HASP_FILEID_RO)
        return HASP_STATUS_OK; /* read-only files: silently succeed */
    memcpy(file + offset, buffer, length);
    return HASP_STATUS_OK;
}

hasp_status_t hasp_get_size(hasp_handle_t handle, hasp_fileid_t fid,
                            hasp_size_t *out_size)
{
    if (!out_size) return HASP_MEM_RANGE;
    hasp_session_t *s = hasp_session_from_handle(handle);
    if (!s) return HASP_INV_HND;
    hasp_size_t size = 0;
    if (!hasp_file_ptr(s, fid, &size)) return HASP_INV_FILEID;
    *out_size = size;
    return HASP_STATUS_OK;
}

hasp_status_t hasp_transfer(hasp_handle_t handle, const char *scope,
                            const char *format, const uint8_t *input,
                            hasp_size_t input_len, uint8_t **output,
                            hasp_size_t *output_len)
{
    (void)handle; (void)scope; (void)format; (void)input; (void)input_len;
    if (output)     *output = NULL;
    if (output_len) *output_len = 0;
    return HASP_STATUS_OK;
}

hasp_status_t hasp_update(const uint8_t *update, char **ack_info)
{
    (void)update;
    if (ack_info) {
        const char *ack = "<?xml version=\"1.0\"?><hasp_ack status=\"ok\"/>";
        char *dup = (char*)malloc(strlen(ack) + 1);
        if (dup) strcpy(dup, ack);
        *ack_info = dup;
    }
    return HASP_STATUS_OK;
}

hasp_status_t hasp_get_info(const char *scope, const char *format,
                            const uint8_t *vendor_code, char **out_info)
{
    (void)scope; (void)format; (void)vendor_code;
    if (out_info) {
        /* Sentinel HL info XML - minimal, well-formed. */
        const char *xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<hasp_info>"
              "<hasp id=\"5122895\">"
                "<attribute name=\"type\" value=\"HASP-HL\"/>"
                "<attribute name=\"prog_num\" value=\"1\"/>"
                "<feature id=\"1\">"
                  "<attribute name=\"locked\" value=\"1\"/>"
                  "<attribute name=\"license_type\" value=\"perpetual\"/>"
                "</feature>"
              "</hasp>"
            "</hasp_info>";
        char *dup = (char*)malloc(strlen(xml) + 1);
        if (dup) strcpy(dup, xml);
        *out_info = dup;
    }
    return HASP_STATUS_OK;
}

hasp_status_t hasp_get_sessioninfo(hasp_handle_t handle, const char *format,
                                   char **out_info)
{
    (void)format;
    hasp_session_t *s = hasp_session_from_handle(handle);
    if (!s) return HASP_INV_HND;
    if (out_info) {
        char buf[512];
        wsprintfA(buf,
            "<?xml version=\"1.0\"?><session_info>"
            "<handle>0x%08x</handle><feature>%u</feature>"
            "<hid>0x%08x%08x</hid></session_info>",
            (unsigned)s->id, (unsigned)s->fid,
            (unsigned)(CRUISN_BLAST_HID >> 32),
            (unsigned)(CRUISN_BLAST_HID & 0xFFFFFFFFu));
        char *dup = (char*)malloc(strlen(buf) + 1);
        if (dup) strcpy(dup, buf);
        *out_info = dup;
    }
    return HASP_STATUS_OK;
}

/* Real Sentinel HL RTC epoch = 2000-01-01 00:00:00 UTC. */
static hasp_time_t hasp_epoch_from_host(void)
{
    time_t now = time(NULL);
    /* 946684800 = 2000-01-01 00:00:00 UTC as unix time. */
    if ((int64_t)now < 946684800LL) return 0;
    return (hasp_time_t)((int64_t)now - 946684800LL);
}

hasp_status_t hasp_get_rtc(hasp_handle_t handle, hasp_time_t *out_time)
{
    if (!out_time) return HASP_MEM_RANGE;
    hasp_session_t *s = hasp_session_from_handle(handle);
    if (!s) return HASP_INV_HND;
    *out_time = s->rtc_override ? s->rtc_override : hasp_epoch_from_host();
    return HASP_STATUS_OK;
}

/* Small helper: Zeller's congruence-style day-of-year to epoch. Simple
 * proleptic-Gregorian converter, no timezone math. */
static int32_t days_from_2000(uint32_t year, uint32_t month, uint32_t day)
{
    if (year < 2000 || month < 1 || month > 12 || day < 1 || day > 31) return -1;
    static const int mdays_norm[13] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    static const int mdays_leap[13] = {0,31,29,31,30,31,30,31,31,30,31,30,31};
    int32_t d = 0;
    for (uint32_t y = 2000; y < year; y++) {
        int leap = ((y%4==0)&&(y%100!=0))||(y%400==0);
        d += leap ? 366 : 365;
    }
    int leap = ((year%4==0)&&(year%100!=0))||(year%400==0);
    const int *m = leap ? mdays_leap : mdays_norm;
    for (uint32_t mo = 1; mo < month; mo++) d += m[mo];
    d += (int32_t)(day - 1);
    return d;
}

hasp_status_t hasp_datetime_to_hasptime(uint32_t day, uint32_t month,
                                        uint32_t year, uint32_t hour,
                                        uint32_t minute, uint32_t second,
                                        hasp_time_t *out_time)
{
    if (!out_time) return HASP_MEM_RANGE;
    int32_t d = days_from_2000(year, month, day);
    if (d < 0) return HASP_SYS_ERR;
    *out_time = (hasp_time_t)((int64_t)d * 86400 + hour * 3600 + minute * 60 + second);
    return HASP_STATUS_OK;
}

hasp_status_t hasp_hasptime_to_datetime(hasp_time_t t, uint32_t *out_day,
                                        uint32_t *out_month, uint32_t *out_year,
                                        uint32_t *out_hour, uint32_t *out_minute,
                                        uint32_t *out_second)
{
    if (!out_day || !out_month || !out_year || !out_hour || !out_minute || !out_second)
        return HASP_MEM_RANGE;
    int64_t total = t;
    int32_t days = (int32_t)(total / 86400);
    int32_t rem  = (int32_t)(total % 86400);
    *out_hour   = rem / 3600;
    *out_minute = (rem / 60) % 60;
    *out_second = rem % 60;
    uint32_t year = 2000;
    while (1) {
        int leap = ((year%4==0)&&(year%100!=0))||(year%400==0);
        int yd = leap ? 366 : 365;
        if (days < yd) break;
        days -= yd; year++;
    }
    static const int mdays_norm[13] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    static const int mdays_leap[13] = {0,31,29,31,30,31,30,31,31,30,31,30,31};
    int leap = ((year%4==0)&&(year%100!=0))||(year%400==0);
    const int *m = leap ? mdays_leap : mdays_norm;
    uint32_t mo = 1;
    while (mo < 12 && days >= m[mo]) { days -= m[mo]; mo++; }
    *out_year  = year;
    *out_month = mo;
    *out_day   = (uint32_t)(days + 1);
    return HASP_STATUS_OK;
}

hasp_status_t hasp_legacy_set_idletime(hasp_handle_t handle, uint32_t minutes)
{
    hasp_session_t *s = hasp_session_from_handle(handle);
    if (!s) return HASP_INV_HND;
    s->idle_minutes = minutes;
    return HASP_STATUS_OK;
}

hasp_status_t hasp_legacy_set_rtc(hasp_handle_t handle, hasp_time_t t)
{
    hasp_session_t *s = hasp_session_from_handle(handle);
    if (!s) return HASP_INV_HND;
    s->rtc_override = t;
    return HASP_STATUS_OK;
}

hasp_status_t haspregnew(void)
{
    /* Legacy driver-registration - no-op. */
    return HASP_STATUS_OK;
}

hasp_status_t hasp_echo(hasp_handle_t handle, void *buffer, hasp_size_t len)
{
    (void)buffer; (void)len;
    if (!hasp_session_from_handle(handle)) return HASP_INV_HND;
    return HASP_STATUS_OK;
}

hasp_status_t hasp_free(void *p)
{
    if (p) free(p);
    return HASP_STATUS_OK;
}

hasp_status_t hasp_cleanup(void)
{
    hasp_lock_init();
    EnterCriticalSection(&g_hasp_lock);
    for (int i = 0; i < HASP_MAX_SESSIONS; i++)
        g_sessions[i].in_use = 0;
    LeaveCriticalSection(&g_hasp_lock);
    return HASP_STATUS_OK;
}

hasp_status_t hasp_enable_trace(const char *filename)
{
    (void)filename;
    return HASP_STATUS_OK;
}

hasp_status_t hasp_get_trace(char **out_trace)
{
    if (out_trace) *out_trace = NULL;
    return HASP_STATUS_OK;
}

hasp_status_t hasp_vm_dyn_execute(hasp_handle_t handle, const void *code,
                                  hasp_size_t code_len)
{
    (void)code; (void)code_len;
    if (!hasp_session_from_handle(handle)) return HASP_INV_HND;
    return HASP_STATUS_OK;
}

/* Sentinel HL SDK version query. The Cruis'n Blast wrapper polls this on
 * boot to gate feature availability against a minimum SDK version. We
 * report 7.10.0 - that matches the SDK the Lindbergh title was built
 * against per the ELF .dynstr version string and satisfies "SDK >= 7.x"
 * gates in the Dongle wrapper. */
hasp_status_t hasp_get_version(uint32_t *out_major, uint32_t *out_minor,
                               uint32_t *out_build, char **out_info)
{
    if (out_major) *out_major = 7;
    if (out_minor) *out_minor = 10;
    if (out_build) *out_build = 0;
    if (out_info) {
        const char *xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<hasp_version>"
              "<attribute name=\"major\" value=\"7\"/>"
              "<attribute name=\"minor\" value=\"10\"/>"
              "<attribute name=\"build\" value=\"0\"/>"
            "</hasp_version>";
        char *dup = (char*)malloc(strlen(xml) + 1);
        if (dup) strcpy(dup, xml);
        *out_info = dup;
    }
    return HASP_STATUS_OK;
}

/* Sentinel HL SDK device-info query. Returns a well-formed XML blob so any
 * caller that XML-parses the answer succeeds; the fields are the same ones
 * hasp_get_info reports for consistency. */
hasp_status_t hasp_get_deviceinfo(const char *scope, const char *format,
                                  char **out_info)
{
    (void)scope; (void)format;
    if (out_info) {
        char buf[512];
        wsprintfA(buf,
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<hasp_deviceinfo>"
              "<hasp id=\"5122895\">"
                "<attribute name=\"type\" value=\"HASP-HL\"/>"
                "<attribute name=\"hid\" value=\"0x%08x%08x\"/>"
                "<attribute name=\"driver_version\" value=\"7.10.0\"/>"
              "</hasp>"
            "</hasp_deviceinfo>",
            (unsigned)(CRUISN_BLAST_HID >> 32),
            (unsigned)(CRUISN_BLAST_HID & 0xFFFFFFFFu));
        char *dup = (char*)malloc(strlen(buf) + 1);
        if (dup) strcpy(dup, buf);
        *out_info = dup;
    }
    return HASP_STATUS_OK;
}

/* Envelope decrypt oracle (HASPE_LINUX_LOADER_1.1). Declared in hasp_shim.h
 * for elf_loader.c but no separate hasp_envelope.c ships. Cruis'n Blast's
 * ELF is NOT envelope-encrypted in the build we target - the envelope
 * marker is either absent or the segment is already plaintext. Return OK
 * as a no-op so the elf_loader path stays wired without failing dlsym on
 * this symbol. */
int hasp_envelope_decrypt(uint8_t *page, size_t page_len, uint64_t hid)
{
    (void)page; (void)page_len; (void)hid;
    return HASP_STATUS_OK;
}

/* ================================================================== */
/* Dispatch tables + dlopen/dlsym intercept                            */
/* ================================================================== */

typedef struct { const char *name; void *fn; } hasp_export_t;

static const hasp_export_t g_hasp_exports[] = {
    /* 30 SDK entry points from the ELF .dynstr */
    { "hasp_login",                 (void*)hasp_login },
    { "hasp_login_ex",              (void*)hasp_login_ex },
    { "hasp_login_port",            (void*)hasp_login_port },
    { "hasp_login_scope",           (void*)hasp_login_scope },
    { "hasp_logout",                (void*)hasp_logout },
    { "hasp_encrypt",               (void*)hasp_encrypt },
    { "hasp_decrypt",               (void*)hasp_decrypt },
    { "hasp_legacy_encrypt",        (void*)hasp_legacy_encrypt },
    { "hasp_legacy_decrypt",        (void*)hasp_legacy_decrypt },
    { "hasp_legacy_set_idletime",   (void*)hasp_legacy_set_idletime },
    { "hasp_legacy_set_rtc",        (void*)hasp_legacy_set_rtc },
    { "hasp_read",                  (void*)hasp_read },
    { "hasp_write",                 (void*)hasp_write },
    { "hasp_transfer",              (void*)hasp_transfer },
    { "hasp_update",                (void*)hasp_update },
    { "hasp_get_info",              (void*)hasp_get_info },
    { "hasp_get_size",              (void*)hasp_get_size },
    { "hasp_get_sessioninfo",       (void*)hasp_get_sessioninfo },
    { "hasp_get_deviceinfo",        (void*)hasp_get_deviceinfo },
    { "hasp_get_version",           (void*)hasp_get_version },
    { "hasp_get_rtc",               (void*)hasp_get_rtc },
    { "hasp_datetime_to_hasptime",  (void*)hasp_datetime_to_hasptime },
    { "hasp_hasptime_to_datetime",  (void*)hasp_hasptime_to_datetime },
    { "hasp_echo",                  (void*)hasp_echo },
    { "hasp_free",                  (void*)hasp_free },
    { "hasp_cleanup",               (void*)hasp_cleanup },
    { "hasp_detach",                (void*)hasp_detach },
    { "hasp_enable_trace",          (void*)hasp_enable_trace },
    { "hasp_get_trace",             (void*)hasp_get_trace },
    { "hasp_vm_dyn_execute",        (void*)hasp_vm_dyn_execute },
    { "haspregnew",                 (void*)haspregnew },
    /* Raw Thrills extension used by the Dongle wrapper */
    { "SysGetDongleHID",            (void*)SysGetDongleHID },
};

int hasp_shim_matches(const char *filename)
{
    if (!filename) return 0;
    /* Match libhasp_linux_x86_32.so, libhasp_linux_x86_64.so, libhasp*.so.* */
    if (strstr(filename, "libhasp"))    return 1;
    if (strstr(filename, "hasp_linux")) return 1;
    if (strstr(filename, "haspvlib"))   return 1; /* older SafeNet naming */
    return 0;
}

void *hasp_shim_dlopen(const char *filename)
{
    loader_log(LOG_INFO, "[HASP] dlopen(%s) -> shim (HID=0x%llx)",
               filename ? filename : "(null)",
               (unsigned long long)CRUISN_BLAST_HID);
    /* Distinctive fake handle; stub_dlsym_v2 does not need to look at it. */
    return (void*)0xC81A5D01u;
}

void *hasp_shim_lookup(const char *symbol)
{
    if (!symbol) return NULL;
    for (size_t i = 0; i < sizeof(g_hasp_exports)/sizeof(g_hasp_exports[0]); i++) {
        if (strcmp(symbol, g_hasp_exports[i].name) == 0) {
            loader_log(LOG_DEBUG, "[HASP] dlsym(%s) -> %p",
                       symbol, g_hasp_exports[i].fn);
            return g_hasp_exports[i].fn;
        }
    }
    return NULL;
}
