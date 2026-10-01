/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  test_namco_io.c -- unit tests for the clean-room Namco JVS-over-USB I/O +
 *                     amauthd LOCAL presence unit (namco_io.c).
 *
 *  Two halves, mirroring test_interop_io.c / test_jvs_core.c:
 *
 *   (1) JVS-USB BRIDGE round-trip: drive ultra_namco_io_bridge() with REAL JVS
 *       request frames (RESET, SET_ADDRESS, READ_SWITCH) and assert the queued
 *       reply (drained via ultra_namco_io_drain) is a correctly-framed JVS
 *       response carrying the stub input -- proving the USB wrapper bridges
 *       through the SAME clean-room jvs_core node the serial path uses. The SENSE
 *       seam is checked to flip with addressing.
 *
 *   (2) amauthd LOCAL presence answer surface: presence ping / local status /
 *       version / region / play-count are ANSWERED; every other class (online
 *       auth, billing, KEY, CONTENT) is DEFERRED (0). This pins the ethical line.
 *
 *  The test links namco_io.c + the real jvs_core.c and STUBS the three hook
 *  externs (logger, IAT patcher, node+pull) so it builds standalone. The stub node
 *  is a real jvs_core node fed a deterministic input.
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

/* ---- hook-provided externs namco_io.c calls; stub them for the test. -------- */
void ultra_d3d8_log(const char* f, ...) { (void)f; }
void ultra_iat_patch_all(const char* a, const char* b, void* c) { (void)a; (void)b; (void)c; }

/* one real jvs_core node, fed a deterministic input, shared by the bridge. */
static ultra_jvs_node* g_test_node = NULL;
static ultra_jvs_input g_test_in;
ultra_jvs_node* ultra_hook_node(void) { return g_test_node; }
void ultra_hook_pull_input(ultra_jvs_node* n) { if (n) jvs_set_input(n, &g_test_in); }

/* ---- units under test (defined in namco_io.c) ------------------------------ */
#if defined(_WIN32)
void          ultra_namco_io_install(void);
int           ultra_namco_io_enabled(void);
int           ultra_namco_io_bridge(const unsigned char* req, int req_len);
int           ultra_namco_io_drain(unsigned char* buf, int cap);
int           ultra_namco_io_queued(void);
unsigned char ultra_namco_io_sense(void);
int           ultra_namco_io_auth_answer(const unsigned char* req, int req_len,
                                         unsigned char* resp, int resp_cap);
#endif

/* Build a raw (un-escaped) JVS host->node packet: E0 dst len <cmd...> sum. The
 * jvs_core engine accepts the on-wire framing; none of our test command bytes hit
 * an escape value, so a literal frame is fine. */
static int jvs_pack(unsigned char dst, const unsigned char* body, int blen,
                    unsigned char* out, int cap) {
    if (blen + 5 > cap) return 0;
    int sum = 0, o = 0;
    out[o++] = JVS_SYNC;                 /* E0 */
    out[o++] = dst;                      /* destination node addr */
    out[o++] = (unsigned char)(blen + 1);/* byte count = body + checksum */
    sum += dst + (blen + 1);
    for (int i = 0; i < blen; ++i) { out[o++] = body[i]; sum += body[i]; }
    out[o++] = (unsigned char)(sum & 0xFF);
    return o;
}

int main(void) {
#if defined(_WIN32)
    _putenv("ULTRA_NAMCO_IO=1");
    ultra_namco_io_install();
    CHECK(ultra_namco_io_enabled() == 1, "namco_io arms under ULTRA_NAMCO_IO=1\n");

    /* deterministic input: P1 switch word 0x8001 (START + a push), all analog
     * centered, system TEST. */
    g_test_node = jvs_create();
    CHECK(g_test_node != NULL, "jvs_core node created for the bridge\n");
    memset(&g_test_in, 0, sizeof g_test_in);
    for (int a = 0; a < JVS_MAX_ANALOG; ++a) g_test_in.analog[a] = 0x8000;
    g_test_in.player[0] = 0x8001;
    g_test_in.system    = 0x80;
    g_test_in.screen_x  = g_test_in.screen_y = 0x8000;

    unsigned char frame[64], reply[256];

    /* SENSE before addressing: unaddressed -> "address me" (default 0). */
    CHECK(ultra_namco_io_sense() == 0x00, "SENSE pre-address = 0 (address me)\n");

    /* (1) RESET broadcast (F0 D9). No reply expected, but it must not crash/queue. */
    {
        unsigned char body[] = { JVS_CMD_RESET, 0xD9 };
        int n = jvs_pack(JVS_BROADCAST, body, sizeof body, frame, sizeof frame);
        ultra_namco_io_bridge(frame, n);
        (void)ultra_namco_io_drain(reply, sizeof reply);   /* clear any reply */
    }

    /* (2) SET_ADDRESS node 1 (broadcast F1 01) -> node takes address 1 + ACKs. */
    {
        unsigned char body[] = { JVS_CMD_SET_ADDR, 0x01 };
        int n = jvs_pack(JVS_BROADCAST, body, sizeof body, frame, sizeof frame);
        int q = ultra_namco_io_bridge(frame, n);
        CHECK(q > 0, "SET_ADDRESS produced a queued reply\n");
        int g = ultra_namco_io_drain(reply, sizeof reply);
        CHECK(g == q, "drain returns the queued length\n");
        CHECK(g >= 2 && reply[0] == JVS_SYNC, "SET_ADDRESS reply is JVS-framed (E0 ...)\n");
        CHECK(jvs_address(g_test_node) == 0x01, "node took address 1 over the USB bridge\n");
    }

    /* SENSE after addressing flips to "done" (default 1). */
    CHECK(ultra_namco_io_sense() == 0x01, "SENSE post-address = 1 (chain done)\n");

    /* (3) READ_SWITCH (addressed to node 1): cmd 20 players=2 bytes/player=2.
     * Reply must decode to a NORMAL status and carry the P1 switch word 0x8001. */
    {
        unsigned char body[] = { JVS_CMD_READ_SWITCH, 0x02, 0x02 };
        int n = jvs_pack(0x01, body, sizeof body, frame, sizeof frame);
        int q = ultra_namco_io_bridge(frame, n);
        CHECK(q > 0, "READ_SWITCH produced a queued reply\n");
        int g = ultra_namco_io_drain(reply, sizeof reply);
        CHECK(g >= 5 && reply[0] == JVS_SYNC, "READ_SWITCH reply is JVS-framed\n");
        /* layout: E0 00 len status report <sys> <p1hi> <p1lo> ... ; find the P1 word.
         * status@3=NORMAL(01), report@4=NORMAL(01), system byte@5, P1 hi@6 lo@7. */
        CHECK(reply[3] == JVS_STATUS_NORMAL, "READ_SWITCH packet status = NORMAL\n");
        CHECK(reply[6] == 0x80 && reply[7] == 0x01,
              "READ_SWITCH reply carries P1 switch word 0x8001 from the stub input\n");
    }

    /* queue is now drained. */
    CHECK(ultra_namco_io_queued() == 0, "reply queue drained to empty\n");

    /* ===================== amauthd LOCAL presence ======================= */
    {
        unsigned char req[8] = {0}, resp[16] = {0};

        req[0] = 0x00;   /* presence ping */
        CHECK(ultra_namco_io_auth_answer(req, 1, resp, sizeof resp) == 2 && resp[0] == 0,
              "amauthd presence ping acked\n");

        req[0] = 0x01;   /* local status -> offline OK */
        CHECK(ultra_namco_io_auth_answer(req, 1, resp, sizeof resp) == 2 && resp[0] == 0,
              "amauthd local status = OK (offline)\n");

        req[0] = 0x02;   /* version (AUTH-STATE metadata) */
        CHECK(ultra_namco_io_auth_answer(req, 1, resp, sizeof resp) == 4 && resp[0] == 0,
              "amauthd version query answered (metadata only)\n");

        req[0] = 0x03;   /* region (IDENTITY metadata) -> default 0 = any */
        CHECK(ultra_namco_io_auth_answer(req, 1, resp, sizeof resp) == 2 && resp[0] == 0 && resp[1] == 0,
              "amauthd region query answered (default any)\n");

        req[0] = 0x04;   /* play-count (LOCAL, no billing) */
        CHECK(ultra_namco_io_auth_answer(req, 1, resp, sizeof resp) == 3 && resp[0] == 0,
              "amauthd play-count query answered (local 0)\n");

        /* THE FORBIDDEN LINE: every other class DEFERS (0). Sample a spread of
         * "online auth / billing / key / content" opcodes -- all must return 0. */
        unsigned char forbidden[] = { 0x05, 0x10, 0x11, 0x12, 0x20, 0x40, 0x77, 0x80, 0xA0, 0xFF };
        int all_deferred = 1;
        for (size_t i = 0; i < sizeof forbidden; ++i) {
            req[0] = forbidden[i];
            if (ultra_namco_io_auth_answer(req, 1, resp, sizeof resp) != 0) all_deferred = 0;
        }
        CHECK(all_deferred,
              "amauthd DEFERS every online/key/content class (no impersonation, no key op)\n");
    }

    /* region knob override is honored (still presence/identity only). */
    {
        _putenv("ULTRA_NAMCO_IO_REGION=2");
        unsigned char req[1] = { 0x03 }, resp[4] = {0};
        int n = ultra_namco_io_auth_answer(req, 1, resp, sizeof resp);
        CHECK(n == 2 && resp[1] == 0x02, "amauthd region honors ULTRA_NAMCO_IO_REGION\n");
        _putenv("ULTRA_NAMCO_IO_REGION=");
    }

    jvs_destroy(g_test_node);
#endif

    printf("==== namco_io JVS-USB + amauthd: %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
