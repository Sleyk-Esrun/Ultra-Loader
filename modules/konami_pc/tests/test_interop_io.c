/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  test_interop_io.c -- unit tests for the new interop I/O + presence layer:
 *    - Konami ACIO frame codec: escape/unescape + additive-checksum round-trip,
 *      node-model answers (enumerate / version / poll / lights).
 *    - Sega IO4-USB: 64-byte IN report field map (ident, analog, switch, coin).
 *    - Sega keychip Nu/ALLS + RingEdge presence vs. key-derivation SPLIT
 *      (presence answered; GkeyEnc/Dec/seed/decrypt/ds.compute DEFERRED).
 *    - Namco amdaemon LOCAL presence ack + online-defer.
 *
 *  Pure protocol -- no game, no device, no network. Mirrors test_jvs_core.c /
 *  test_board_profile.c structure. The hook-provided externs (logger, IAT patcher,
 *  current-input) are stubbed so the test links standalone; the stub input is
 *  all-centered + a known player word so the field map is deterministic.
 * =============================================================================
 */
#include "jvs_core.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) { g_pass++; } else { g_fail++; \
    printf("FAIL: " __VA_ARGS__); printf("  (%s:%d)\n", __FILE__, __LINE__); } } while (0)

/* ---- hook-provided externs the interop units call; stub them for the test. --- */
void ultra_d3d8_log(const char* f, ...) { (void)f; }
void ultra_iat_patch_all(const char* a, const char* b, void* c) { (void)a; (void)b; (void)c; }

/* deterministic stub input: P1 word = 0x8001 (START + PUSH...), all analog 0x8000. */
static ultra_jvs_input g_stub_in;
void ultra_hook_current_input(ultra_jvs_input* o) { *o = g_stub_in; }

/* ---- units under test (declared here; defined in the .c files we link) ------ */
/* Konami ACIO codec (cross-platform core). */
size_t konami_acio_encode(unsigned char addr, unsigned short cmd,
                          const unsigned char* payload, size_t plen,
                          unsigned char* out, size_t cap);
size_t konami_acio_decode(const unsigned char* in, size_t in_len,
                          unsigned char* addr, unsigned short* cmd,
                          unsigned char* payload, size_t pcap, size_t* plen);
size_t konami_acio_answer(unsigned char addr, unsigned short cmd,
                          const unsigned char* req_payload, size_t req_plen,
                          unsigned char* out, size_t out_cap);

#if defined(_WIN32)
/* Sega IO4. */
int  ultra_sega_io4_fill_in(void* buf, int cap);
void ultra_sega_io4_insert_coin(int slot, int count);
int  ultra_sega_io4_consume_out(const void* buf, int len);
int  ultra_sega_io4_ident(char* buf, int cap);
/* Sega keychip presence/auth. */
void ultra_sega_keychip_install(void);
int  ultra_sega_keychip_nu_cmd(int cmd, const unsigned char* req, int req_len,
                               unsigned char* resp, int resp_cap);
int  ultra_sega_keychip_mx_cmd(const char* cmd, char* resp, int resp_cap);
/* Namco amdaemon presence. */
void ultra_namco_amdaemon_install(void);
int  ultra_namco_amdaemon_answer(const unsigned char* req, int req_len,
                                 unsigned char* resp, int resp_cap);
#endif

/* helper: unescape an ACIO wire stream back to a logical body (test-side, to prove
 * the encoder really applied the 0xFF-NOT byte-stuffing). */
static size_t acio_unescape(const unsigned char* w, size_t len, unsigned char* out, size_t cap) {
    size_t p = 0, o = 0;
    if (len < 1 || w[0] != 0xAA) return 0;
    p = 1;
    while (p < len && o < cap) {
        unsigned char b = w[p];
        if (b == 0xFF) { if (p + 1 >= len) break; out[o++] = (unsigned char)(~w[p+1]); p += 2; }
        else { out[o++] = b; p += 1; }
    }
    return o;
}

int main(void) {
    memset(&g_stub_in, 0, sizeof g_stub_in);
    for (int a = 0; a < JVS_MAX_ANALOG; ++a) g_stub_in.analog[a] = 0x8000;
    g_stub_in.player[0] = 0x8001;
    g_stub_in.player[1] = 0x4002;
    g_stub_in.system    = 0x80;   /* TEST */
    g_stub_in.screen_x = g_stub_in.screen_y = 0x8000;

    /* ===================== Konami ACIO codec ============================ */

    /* (1) encode/decode round-trip with a payload containing both escape bytes. */
    {
        unsigned char payload[] = { 0x01, 0xAA, 0xFF, 0x55, 0xAA };  /* forces stuffing */
        unsigned char wire[64];
        size_t wn = konami_acio_encode(0x00, 0x0152, payload, sizeof payload, wire, sizeof wire);
        CHECK(wn > 0, "ACIO encode produced a frame\n");
        CHECK(wire[0] == 0xAA, "ACIO frame starts with SYNC 0xAA\n");

        /* the literal 0xAA/0xFF in the body must NOT appear unescaped after byte 0. */
        int raw_aa = 0;
        for (size_t i = 1; i < wn; ++i) if (wire[i] == 0xAA) raw_aa++;
        CHECK(raw_aa == 0, "no raw 0xAA in the escaped body (byte-stuffing applied)\n");

        unsigned char addr; unsigned short cmd; unsigned char pout[64]; size_t pl;
        size_t consumed = konami_acio_decode(wire, wn, &addr, &cmd, pout, sizeof pout, &pl);
        CHECK(consumed == wn, "ACIO decode consumed the whole frame (%zu==%zu)\n", consumed, wn);
        CHECK(addr == 0x00 && cmd == 0x0152, "ACIO decode recovered addr+cmd\n");
        CHECK(pl == sizeof payload && memcmp(pout, payload, pl) == 0,
              "ACIO decode recovered the exact payload through the escaping\n");
    }

    /* (2) a corrupted checksum is rejected. */
    {
        unsigned char payload[] = { 0x10, 0x20 };
        unsigned char wire[32];
        size_t wn = konami_acio_encode(0x01, 0x0002, payload, sizeof payload, wire, sizeof wire);
        CHECK(wn > 0, "ACIO encode (2) ok\n");
        wire[wn - 1] ^= 0xFF;   /* corrupt the last (checksum) byte */
        unsigned char addr; unsigned short cmd; unsigned char pout[32]; size_t pl;
        size_t consumed = konami_acio_decode(wire, wn, &addr, &cmd, pout, sizeof pout, &pl);
        CHECK(consumed == 0, "ACIO decode rejects a bad checksum\n");
    }

    /* (3) the encoder really uses the documented escape: 0xAA -> FF 55, 0xFF -> FF 00. */
    {
        unsigned char payload[] = { 0xAA, 0xFF };
        unsigned char wire[32];
        size_t wn = konami_acio_encode(0x00, 0x0000, payload, sizeof payload, wire, sizeof wire);
        unsigned char body[32];
        size_t bn = acio_unescape(wire, wn, body, sizeof body);
        /* body = addr,cmdhi,cmdlo,len, payload..., checksum */
        CHECK(bn >= 6, "ACIO unescape recovered a full body\n");
        CHECK(body[4] == 0xAA && body[5] == 0xFF,
              "0xAA/0xFF survive the escape round-trip in-body\n");
    }

    /* (4) node-model answers: enumerate, version, poll, lights. */
    {
        unsigned char out[64]; unsigned char addr; unsigned short cmd;
        unsigned char pout[64]; size_t pl;

        /* GET_VERSION -> node ID (4 bytes) + version. */
        size_t n = konami_acio_answer(0x00, 0x0002, NULL, 0, out, sizeof out);
        CHECK(n > 0, "ACIO answer GET_VERSION produced a frame\n");
        CHECK(konami_acio_decode(out, n, &addr, &cmd, pout, sizeof pout, &pl) == n &&
              cmd == 0x0002 && pl == 5, "GET_VERSION reply = node-id(4)+ver(1)\n");

        /* POLL -> input payload reflecting the stub P1 word (0x8001) at offset 0. */
        n = konami_acio_answer(0x00, 0x0152, NULL, 0, out, sizeof out);
        CHECK(n > 0, "ACIO answer POLL produced a frame\n");
        CHECK(konami_acio_decode(out, n, &addr, &cmd, pout, sizeof pout, &pl) == n &&
              cmd == 0x0152, "POLL reply decodes\n");
        CHECK(pout[0] == 0x80 && pout[1] == 0x01, "POLL payload carries P1 switch word 0x8001\n");

        /* SET_LIGHTS -> ack. */
        n = konami_acio_answer(0x00, 0x0153, NULL, 0, out, sizeof out);
        CHECK(n > 0 && konami_acio_decode(out, n, &addr, &cmd, pout, sizeof pout, &pl) == n,
              "SET_LIGHTS acked\n");

        /* unknown command -> deferred (0 bytes). */
        n = konami_acio_answer(0x00, 0x7FFF, NULL, 0, out, sizeof out);
        CHECK(n == 0, "unknown ACIO command is deferred (0)\n");
    }

#if defined(_WIN32)
    /* ===================== Sega IO4-USB ================================= */
    {
        _putenv("ULTRA_IO4=1");
        /* the install reads getenv but for the report fill we call fill_in directly. */
        unsigned char rep[64];
        memset(rep, 0xCC, sizeof rep);
        int w = ultra_sega_io4_fill_in(rep, sizeof rep);
        CHECK(w == 64, "IO4 IN report is 64 bytes\n");
        CHECK(rep[0] == 0x01, "IO4 IN report id = 0x01\n");
        /* analog ch0 at offset 1, LE, 0x8000 center -> bytes 0x00,0x80. */
        CHECK(rep[1] == 0x00 && rep[2] == 0x80, "IO4 analog ch0 = 0x8000 center (LE)\n");
        /* system byte at offset 29 = stub TEST 0x80. */
        CHECK(rep[29] == 0x80, "IO4 switch block byte0 = system (TEST 0x80)\n");
        /* coin: insert 2 on slot 0, refill, read LE counter at offset 35. */
        ultra_sega_io4_insert_coin(0, 2);
        ultra_sega_io4_fill_in(rep, sizeof rep);
        CHECK(rep[35] == 0x02 && rep[36] == 0x00, "IO4 coin counter slot0 = 2 (LE)\n");
        /* OUT (lamp) report is accepted, never errors. */
        unsigned char out[63] = {0};
        CHECK(ultra_sega_io4_consume_out(out, sizeof out) == 1, "IO4 OUT lamp report accepted\n");
        /* ident string carries the VID/cap contract. */
        char id[200];
        int idn = ultra_sega_io4_ident(id, sizeof id);
        CHECK(idn > 0 && strstr(id, "I/O CONTROL BD") && strstr(id, "ADIN=8"),
              "IO4 ident string reports the capability contract\n");
    }

    /* ===================== Sega keychip presence vs. key-derivation ===== */
    {
        _putenv("ULTRA_SEGA_KEYCHIP=1");
        _putenv("ULTRA_SEGA_GAME_ID=SBXX");
        ultra_sega_keychip_install();
        unsigned char resp[96];

        /* PRESENCE/IDENTITY commands are answered. */
        CHECK(ultra_sega_keychip_nu_cmd(0, NULL, 0, resp, sizeof resp) == 1 && resp[0] == 0,
              "Nu keychip cmd0 SessionOpen acked\n");
        CHECK(ultra_sega_keychip_nu_cmd(5, NULL, 0, resp, sizeof resp) >= 1,
              "Nu keychip cmd5 LvGet answered\n");
        CHECK(ultra_sega_keychip_nu_cmd(7, NULL, 0, resp, sizeof resp) >= 1,
              "Nu keychip cmd7 VerGet answered\n");
        int n9 = ultra_sega_keychip_nu_cmd(9, NULL, 0, resp, sizeof resp);
        CHECK(n9 >= 16 && memcmp(&resp[4], "SBXX", 4) == 0,
              "Nu keychip cmd9 KeychipInfoRead reports game id (no secret)\n");
        CHECK(ultra_sega_keychip_nu_cmd(34, NULL, 0, resp, sizeof resp) == 1,
              "Nu keychip cmd34 ALStart (local framing) acked\n");

        /* KEY-DERIVATION commands are DEFERRED (the forbidden line). */
        CHECK(ultra_sega_keychip_nu_cmd(10, NULL, 0, resp, sizeof resp) == 0,
              "Nu keychip cmd10 GkeyWrite DEFERRED (secret)\n");
        CHECK(ultra_sega_keychip_nu_cmd(11, NULL, 0, resp, sizeof resp) == 0,
              "Nu keychip cmd11 GkeyEnc DEFERRED (content key)\n");
        CHECK(ultra_sega_keychip_nu_cmd(12, NULL, 0, resp, sizeof resp) == 0,
              "Nu keychip cmd12 GkeyDec DEFERRED (content key)\n");
        CHECK(ultra_sega_keychip_nu_cmd(32, NULL, 0, resp, sizeof resp) == 0,
              "Nu keychip cmd32 ALKey DEFERRED (key material)\n");

        /* RingEdge mxkeychip text commands: presence answered, content-key deferred. */
        char r[64];
        CHECK(ultra_sega_keychip_mx_cmd("keychip.status", r, sizeof r) > 0,
              "mxkeychip status answered\n");
        CHECK(ultra_sega_keychip_mx_cmd("keychip.appboot.gameid", r, sizeof r) > 0 &&
              strcmp(r, "SBXX") == 0, "mxkeychip gameid answered (no secret)\n");
        CHECK(ultra_sega_keychip_mx_cmd("keychip.region", r, sizeof r) > 0,
              "mxkeychip region answered\n");
        CHECK(ultra_sega_keychip_mx_cmd("keychip.decrypt", r, sizeof r) == 0,
              "mxkeychip DECRYPT DEFERRED (content key)\n");
        CHECK(ultra_sega_keychip_mx_cmd("keychip.appboot.seed", r, sizeof r) == 0,
              "mxkeychip appboot.seed DEFERRED (keyfile derivation)\n");
        CHECK(ultra_sega_keychip_mx_cmd("keychip.ds.compute", r, sizeof r) == 0,
              "mxkeychip ds.compute DEFERRED (challenge math)\n");
        CHECK(ultra_sega_keychip_mx_cmd("keychip.ssd.proof", r, sizeof r) == 0,
              "mxkeychip ssd.proof DEFERRED (secret)\n");
    }

    /* ===================== Namco amdaemon LOCAL presence ================ */
    {
        _putenv("ULTRA_AMDAEMON=1");
        ultra_namco_amdaemon_install();
        unsigned char req[4] = {0}, resp[16] = {0};
        req[0] = 0x00;   /* presence ping */
        CHECK(ultra_namco_amdaemon_answer(req, 1, resp, sizeof resp) == 2 && resp[0] == 0,
              "amdaemon presence ping acked\n");
        req[0] = 0x01;   /* LOCAL status query -> offline OK */
        CHECK(ultra_namco_amdaemon_answer(req, 1, resp, sizeof resp) == 2 && resp[0] == 0,
              "amdaemon local status = OK (offline)\n");
        req[0] = 0x77;   /* online auth/billing -> DEFERRED (not impersonated) */
        CHECK(ultra_namco_amdaemon_answer(req, 1, resp, sizeof resp) == 0,
              "amdaemon online/unknown class DEFERRED (no live-service impersonation)\n");
    }
#endif

    printf("==== interop I/O + presence: %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
