/* SPDX-License-Identifier: MIT
 * test_jvs_core.c -- clean-room JVS engine unit tests (no game, pure protocol).
 * Build:  gcc -std=c11 -Wall -Wextra -I../src/hook ../src/hook/jvs_core.c \
 *             test_jvs_core.c -o test_jvs_core && ./test_jvs_core
 */
#include "jvs_core.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(c, ...) do { if (c) { g_pass++; } else { \
    g_fail++; printf("FAIL: " __VA_ARGS__); printf("  (%s:%d)\n", __FILE__, __LINE__); } } while (0)

/* ---- wire builders/parsers (test-side, independent of the engine) --------- */

/* escape one logical byte into buf */
static void esc(unsigned char* buf, size_t* p, unsigned char b) {
    if (b == JVS_SYNC || b == JVS_ESCAPE) { buf[(*p)++] = JVS_ESCAPE; buf[(*p)++] = (unsigned char)(b - 1); }
    else buf[(*p)++] = b;
}
/* build a host->node packet onto buf, return length */
static size_t build(unsigned char* buf, unsigned char node,
                    const unsigned char* data, size_t dlen) {
    size_t p = 0;
    buf[p++] = JVS_SYNC;
    unsigned char num = (unsigned char)(dlen + 1);
    unsigned char sum = (unsigned char)(node + num);
    for (size_t i = 0; i < dlen; ++i) sum = (unsigned char)(sum + data[i]);
    esc(buf, &p, node);
    esc(buf, &p, num);
    for (size_t i = 0; i < dlen; ++i) esc(buf, &p, data[i]);
    esc(buf, &p, sum);
    return p;
}
/* unescape-read one logical byte */
static int urd(const unsigned char* w, size_t len, size_t* wp, unsigned char* o) {
    if (*wp >= len) return 0;
    unsigned char b = w[*wp];
    if (b == JVS_ESCAPE) { if (*wp + 1 >= len) return 0; *o = (unsigned char)(w[*wp+1]+1); *wp += 2; }
    else { *o = b; *wp += 1; }
    return 1;
}
/* parse a node->host response; fill data (status+reports), return 1 on ok */
static int parse_resp(const unsigned char* o, size_t olen,
                      unsigned char* node, unsigned char* data, size_t* dlen) {
    if (olen < 3 || o[0] != JVS_SYNC) return 0;
    size_t wp = 1; unsigned char num;
    if (!urd(o, olen, &wp, node)) return 0;
    if (!urd(o, olen, &wp, &num)) return 0;
    unsigned char tmp[640]; size_t got = 0;
    for (; got < num; ++got) if (!urd(o, olen, &wp, &tmp[got])) return 0;
    unsigned char sum = (unsigned char)(*node + num);
    for (size_t i = 0; i + 1 < num; ++i) sum = (unsigned char)(sum + tmp[i]);
    if (sum != tmp[num - 1]) return 0;          /* checksum */
    *dlen = num - 1;
    memcpy(data, tmp, *dlen);
    return 1;
}

/* ---- tests --------------------------------------------------------------- */

int main(void) {
    ultra_jvs_node* n = jvs_create();
    CHECK(n != NULL, "create\n");

    unsigned char pkt[600], out[1024], rd[640], node; size_t rlen;

    /* (a) RESET broadcast -> no response, address cleared */
    {
        unsigned char d[] = { JVS_CMD_RESET, 0xD9 };
        size_t L = build(pkt, JVS_BROADCAST, d, sizeof d);
        size_t o = jvs_host_bytes(n, pkt, L, out, sizeof out);
        CHECK(o == 0, "RESET should produce no response (got %zu)\n", o);
        CHECK(jvs_address(n) == 0, "address 0 after reset\n");
    }

    /* (b) SET_ADDRESS=1 -> status normal, report normal; address adopted */
    {
        unsigned char d[] = { JVS_CMD_SET_ADDR, 0x01 };
        size_t L = build(pkt, JVS_BROADCAST, d, sizeof d);
        size_t o = jvs_host_bytes(n, pkt, L, out, sizeof out);
        CHECK(o > 0 && parse_resp(out, o, &node, rd, &rlen), "SET_ADDR parse\n");
        CHECK(node == JVS_HOST_ADDR, "resp addressed to host\n");
        CHECK(rlen == 2 && rd[0] == JVS_STATUS_NORMAL && rd[1] == JVS_REPORT_NORMAL,
              "SET_ADDR status/report (rlen=%zu s=%02x r=%02x)\n", rlen, rd[0], rd[1]);
        CHECK(jvs_address(n) == 1, "address==1 (got %u)\n", jvs_address(n));
    }

    /* (c) IOIDENT -> id string, null-terminated */
    {
        jvs_set_id(n, "ULTRA;TESTIO;ver1.00;x");
        unsigned char d[] = { JVS_CMD_IOIDENT };
        size_t L = build(pkt, 0x01, d, sizeof d);
        size_t o = jvs_host_bytes(n, pkt, L, out, sizeof out);
        CHECK(parse_resp(out, o, &node, rd, &rlen), "IOIDENT parse\n");
        CHECK(rd[0] == JVS_STATUS_NORMAL && rd[1] == JVS_REPORT_NORMAL, "IOIDENT report\n");
        CHECK(strcmp((char*)&rd[2], "ULTRA;TESTIO;ver1.00;x") == 0, "IOIDENT id string\n");
    }

    /* (d) revision bytes */
    {
        unsigned char d[] = { JVS_CMD_CMD_REV, JVS_CMD_JVS_REV, JVS_CMD_COMM_VER };
        size_t L = build(pkt, 0x01, d, sizeof d);
        size_t o = jvs_host_bytes(n, pkt, L, out, sizeof out);
        CHECK(parse_resp(out, o, &node, rd, &rlen), "REV parse\n");
        /* status, (rep 0x13), (rep 0x30), (rep 0x10) */
        CHECK(rlen == 7 && rd[0] == 1 && rd[2] == 0x13 && rd[4] == 0x30 && rd[6] == 0x10,
              "rev bytes (rlen=%zu)\n", rlen);
    }

    /* (e) FEATURE CHECK -> feature list */
    {
        jvs_set_features(n, 2, 13, 2, 8, 16, 2, 0);
        unsigned char d[] = { JVS_CMD_FEATCHK };
        size_t L = build(pkt, 0x01, d, sizeof d);
        size_t o = jvs_host_bytes(n, pkt, L, out, sizeof out);
        CHECK(parse_resp(out, o, &node, rd, &rlen), "FEATCHK parse\n");
        /* rd: [status][report][01 02 0d 00][02 02 00 00][03 08 10 00][12 02 00 00][00] */
        CHECK(rd[0] == 1 && rd[1] == 1, "FEATCHK report\n");
        CHECK(rd[2]==0x01 && rd[3]==2 && rd[4]==13, "switch feature\n");
        CHECK(rd[6]==0x02 && rd[7]==2, "coin feature\n");
        CHECK(rd[10]==0x03 && rd[11]==8 && rd[12]==16, "analog feature\n");
        CHECK(rd[14]==0x12 && rd[15]==2, "gpo feature\n");
        CHECK(rd[18]==0x00, "feature terminator\n");
    }

    /* (f) READ_SWITCH with inputs */
    {
        ultra_jvs_input in; memset(&in, 0, sizeof in);
        in.system   = 0x80;                       /* TEST */
        in.player[0] = (uint16_t)((1u<<15) | (1u<<9));   /* START + PUSH1 in byte1 */
        in.player[1] = (uint16_t)(1u<<11);               /* LEFT in byte1 */
        jvs_set_input(n, &in);
        unsigned char d[] = { JVS_CMD_READ_SWITCH, 0x02, 0x02 };  /* 2 players, 2 bytes */
        size_t L = build(pkt, 0x01, d, sizeof d);
        size_t o = jvs_host_bytes(n, pkt, L, out, sizeof out);
        CHECK(parse_resp(out, o, &node, rd, &rlen), "READ_SWITCH parse\n");
        /* [status][report][system][p0b1][p0b2][p1b1][p1b2] */
        CHECK(rd[2] == 0x80, "system TEST (got %02x)\n", rd[2]);
        CHECK(rd[3] == (unsigned char)((1<<7)|(1<<1)), "p0 byte1 START|PUSH1 (got %02x)\n", rd[3]);
        CHECK(rd[5] == (unsigned char)(1<<3), "p1 byte1 LEFT (got %02x)\n", rd[5]);
    }

    /* (g) READ_ANALOG with a value that forces response escaping (0xE000) */
    {
        ultra_jvs_input in; memset(&in, 0, sizeof in);
        in.analog[0] = 0xE000;   /* high byte 0xE0 -> must be escaped on the wire */
        in.analog[1] = 0x1234;
        jvs_set_input(n, &in);
        unsigned char d[] = { JVS_CMD_READ_ANALOG, 0x02 };
        size_t L = build(pkt, 0x01, d, sizeof d);
        size_t o = jvs_host_bytes(n, pkt, L, out, sizeof out);
        /* the raw response must contain an escape sequence for 0xE0 */
        int has_escape = 0;
        for (size_t k = 1; k < o; ++k) if (out[k] == JVS_ESCAPE) has_escape = 1;
        CHECK(has_escape, "analog 0xE0 escaped on wire\n");
        CHECK(parse_resp(out, o, &node, rd, &rlen), "READ_ANALOG parse\n");
        CHECK(rd[2]==0xE0 && rd[3]==0x00, "analog0 = E000 (got %02x%02x)\n", rd[2], rd[3]);
        CHECK(rd[4]==0x12 && rd[5]==0x34, "analog1 = 1234 (got %02x%02x)\n", rd[4], rd[5]);
    }

    /* (h) coin insert + READ_COIN */
    {
        jvs_insert_coin(n, 0, 2);
        unsigned char d[] = { JVS_CMD_READ_COIN, 0x01 };
        size_t L = build(pkt, 0x01, d, sizeof d);
        size_t o = jvs_host_bytes(n, pkt, L, out, sizeof out);
        CHECK(parse_resp(out, o, &node, rd, &rlen), "READ_COIN parse\n");
        unsigned cnt = ((rd[2] & 0x3F) << 8) | rd[3];
        CHECK((rd[2] & 0xC0) == 0, "coin condition normal\n");
        CHECK(cnt == 2, "coin count 2 (got %u)\n", cnt);
    }

    /* (i) COIN_DECREASE with an escaped 0xE0 in the request, then verify */
    {
        jvs_insert_coin(n, 0, 0x3000);    /* now 0x3002 (clamped 0x3FFF? 0x3002 ok) */
        /* decrement by 0xE001 -> but coin is only 0x3002, clamps to 0 */
        unsigned char d[] = { JVS_CMD_COIN_DEC, 0x00, 0xE0, 0x01 };  /* 0xE0 escaped in req */
        size_t L = build(pkt, 0x01, d, sizeof d);
        size_t o = jvs_host_bytes(n, pkt, L, out, sizeof out);
        CHECK(parse_resp(out, o, &node, rd, &rlen), "COIN_DEC parse\n");
        CHECK(rd[0]==1 && rd[1]==1, "COIN_DEC report\n");
        CHECK(jvs_coin_count(n, 0) == 0, "coin debited to 0 (got %u)\n", jvs_coin_count(n,0));
    }

    /* (i2) COIN_DECREASE uses 1-BASED slot numbering: a debit on wire slot 1 must
     *      hit coin[0] (the slot READ_COIN ns=1 exposes). This is the Wacky Race
     *      fix -- the game reads credits on slot 0 but debits "slot 1". */
    {
        jvs_insert_coin(n, 0, 3);                 /* coin[0] = 3 */
        unsigned char d[] = { JVS_CMD_COIN_DEC, 0x01, 0x00, 0x02 };  /* slot 1 -1, amt 2 */
        size_t L = build(pkt, 0x01, d, sizeof d);
        size_t o = jvs_host_bytes(n, pkt, L, out, sizeof out);
        CHECK(parse_resp(out, o, &node, rd, &rlen), "1-based COIN_DEC parse\n");
        CHECK(jvs_coin_count(n, 0) == 1, "wire slot 1 debits coin[0]: 3-2=1 (got %u)\n", jvs_coin_count(n,0));
    }

    /* (j) streaming: split one packet across two feeds */
    {
        unsigned char d[] = { JVS_CMD_JVS_REV };
        size_t L = build(pkt, 0x01, d, sizeof d);
        size_t o1 = jvs_host_bytes(n, pkt, 2, out, sizeof out);          /* first 2 bytes */
        CHECK(o1 == 0, "no response on partial feed (got %zu)\n", o1);
        size_t o2 = jvs_host_bytes(n, pkt + 2, L - 2, out, sizeof out);  /* rest */
        CHECK(parse_resp(out, o2, &node, rd, &rlen) && rd[2] == 0x30,
              "streamed packet completes (rlen=%zu)\n", rlen);
    }

    /* (k) bad checksum -> resync, no response */
    {
        unsigned char d[] = { JVS_CMD_JVS_REV };
        size_t L = build(pkt, 0x01, d, sizeof d);
        pkt[L - 1] ^= 0xFF;                         /* corrupt checksum */
        size_t o = jvs_host_bytes(n, pkt, L, out, sizeof out);
        CHECK(o == 0, "bad checksum -> no response (got %zu)\n", o);
        /* a following good packet must still parse (buffer recovered) */
        unsigned char d2[] = { JVS_CMD_COMM_VER };
        size_t L2 = build(pkt, 0x01, d2, sizeof d2);
        size_t o2 = jvs_host_bytes(n, pkt, L2, out, sizeof out);
        CHECK(parse_resp(out, o2, &node, rd, &rlen) && rd[2] == 0x10,
              "recover after bad checksum\n");
    }

    /* (l) unknown command -> status UNKNOWN */
    {
        unsigned char d[] = { 0x99 };
        size_t L = build(pkt, 0x01, d, sizeof d);
        size_t o = jvs_host_bytes(n, pkt, L, out, sizeof out);
        CHECK(parse_resp(out, o, &node, rd, &rlen), "unknown cmd parse\n");
        CHECK(rd[0] == JVS_STATUS_UNKNOWN, "unknown -> status 0x02 (got %02x)\n", rd[0]);
    }

    /* (m) REGRESSION: huge READ_SWITCH counts must NOT overrun rsp[] (was a
     *     critical OOB stack read). Response must stay bounded + report overflow. */
    {
        unsigned char d[] = { JVS_CMD_READ_SWITCH, 0xFF, 0xFF };  /* 255 players x 255 bytes */
        size_t L = build(pkt, 0x01, d, sizeof d);
        size_t o = jvs_host_bytes(n, pkt, L, out, sizeof out);
        CHECK(o > 0 && o < 600, "huge READ_SWITCH bounded on wire (got %zu)\n", o);
        CHECK(parse_resp(out, o, &node, rd, &rlen), "huge READ_SWITCH parses\n");
        CHECK(rlen <= 254, "response data <= 254 (got %zu)\n", rlen);
        CHECK(rd[0] == JVS_STATUS_OVERFLOW, "overflow status 0x04 (got %02x)\n", rd[0]);
    }

    /* (n) address filter: once assigned addr 5, ignore packets to a different
     *     node, still answer our own address and broadcast. */
    {
        /* a node only accepts SET_ADDRESS when unaddressed -> RESET first */
        unsigned char rst[] = { JVS_CMD_RESET, 0xD9 };
        jvs_host_bytes(n, pkt, build(pkt, JVS_BROADCAST, rst, sizeof rst), out, sizeof out);
        unsigned char sa[] = { JVS_CMD_SET_ADDR, 0x05 };
        jvs_host_bytes(n, pkt, build(pkt, JVS_BROADCAST, sa, sizeof sa), out, sizeof out);
        CHECK(jvs_address(n) == 5, "address==5\n");
        unsigned char q[] = { JVS_CMD_JVS_REV };
        size_t oo = jvs_host_bytes(n, pkt, build(pkt, 0x07, q, sizeof q), out, sizeof out);
        CHECK(oo == 0, "no reply to other node addr 7 (got %zu)\n", oo);
        size_t ok = jvs_host_bytes(n, pkt, build(pkt, 0x05, q, sizeof q), out, sizeof out);
        CHECK(ok > 0 && parse_resp(out, ok, &node, rd, &rlen) && rd[2] == 0x30,
              "replies to our addr 5\n");
        /* an ADDRESSED node ignores broadcast non-RESET probes (e.g. 0x2F) */
        size_t ob = jvs_host_bytes(n, pkt, build(pkt, JVS_BROADCAST, q, sizeof q), out, sizeof out);
        CHECK(ob == 0, "addressed node ignores broadcast non-RESET (got %zu)\n", ob);
    }

    /* (o) REGRESSION: a driving game's BUNDLED per-frame poll. Wacky Race (Type X2)
     *     sends, in ONE packet: READ_SWITCH + GPO(0x32) + ANALOG_OUT(0x33,force-fb)
     *     + READ_COIN + READ_ANALOG. The 0x33 output command used to hit default ->
     *     status UNKNOWN -> the rest of the packet (coin + analog) went UNANSWERED,
     *     so the game saw a dead coin slot and a dead wheel. Verify status NORMAL
     *     and that the coin + analog reports are present and correct. */
    {
        /* fresh node: re-address it to 1 so the packet is accepted */
        unsigned char rst[] = { JVS_CMD_RESET, 0xD9 };
        jvs_host_bytes(n, pkt, build(pkt, JVS_BROADCAST, rst, sizeof rst), out, sizeof out);
        unsigned char sa[] = { JVS_CMD_SET_ADDR, 0x01 };
        jvs_host_bytes(n, pkt, build(pkt, JVS_BROADCAST, sa, sizeof sa), out, sizeof out);
        CHECK(jvs_address(n) == 1, "re-address to 1 for bundled poll\n");

        ultra_jvs_input in; memset(&in, 0, sizeof in);
        in.player[0] = 0x8000;          /* START held (byte1 bit7) */
        for (int a = 0; a < JVS_MAX_ANALOG; ++a) in.analog[a] = 0x8000;
        in.analog[0] = 0xC000;          /* steering off-center */
        in.analog[1] = 0xFFFF;          /* accel full */
        jvs_set_input(n, &in);
        /* zero coin[0] (earlier tests may have left credit), then insert exactly 1 */
        { unsigned char dz[] = { JVS_CMD_COIN_DEC, 0x01, 0x3F, 0xFF };
          jvs_host_bytes(n, pkt, build(pkt, 0x01, dz, sizeof dz), out, sizeof out); }
        jvs_insert_coin(n, 0, 1);       /* one credit pending */

        /* the exact command stream Wacky Race sends each frame */
        unsigned char d[] = {
            JVS_CMD_READ_SWITCH, 0x01, 0x03,                 /* 1 player x 3 bytes */
            JVS_CMD_GPO, 0x03, 0x00, 0x18, 0x00,             /* lamps */
            JVS_CMD_ANALOG_OUT, 0x06, 0xFF,0x00, 0x00,0x00,  /* force-fb 6 ch */
                                     0xFF,0x00, 0x00,0x00,
                                     0xFF,0x00, 0x00,0x00,
            JVS_CMD_READ_COIN, 0x01,                          /* 1 coin slot */
            JVS_CMD_READ_ANALOG, 0x0A,                        /* 10 channels */
        };
        size_t L = build(pkt, 0x01, d, sizeof d);
        size_t o = jvs_host_bytes(n, pkt, L, out, sizeof out);
        CHECK(parse_resp(out, o, &node, rd, &rlen), "bundled poll parses\n");
        CHECK(rd[0] == JVS_STATUS_NORMAL, "bundled poll status NORMAL (got %02x)\n", rd[0]);
        /* walk reports: SWITCH(report + sys + 3 sw) GPO(report) ANALOGOUT(report)
         * COIN(report + 2) ANALOG(report + 20). Find the coin: after
         * status[0], sw=rd[1..4](report,sys,3sw? actually report+sys+3=5), ... we
         * just assert the tail analog block is present + first channel == 0xC000. */
        /* reports layout: [0]=status
         *   READ_SWITCH : report(1)+sys(1)+3 = rd[1..5]
         *   GPO         : report(1)         = rd[6]
         *   ANALOG_OUT  : report(1)         = rd[7]
         *   READ_COIN   : report(1)+2       = rd[8..10]
         *   READ_ANALOG : report(1)+20      = rd[11..31] */
        CHECK(rlen >= 31, "bundled poll full reports present (rlen=%zu)\n", rlen);
        unsigned coin = ((rd[9] & 0x3F) << 8) | rd[10];
        CHECK(coin == 1, "coin reported in bundled poll (got %u)\n", coin);
        CHECK(rd[12] == 0xC0 && rd[13] == 0x00, "analog ch0 = C000 in bundled poll (got %02x%02x)\n", rd[12], rd[13]);
        CHECK(rd[14] == 0xFF && rd[15] == 0xFF, "analog ch1 = FFFF in bundled poll (got %02x%02x)\n", rd[14], rd[15]);
    }

    /* (o2) REGRESSION (jvs-1): a length-prefixed output command whose declared
     *      length runs PAST the packet end is malformed -- it must flag status
     *      UNKNOWN (param error), NOT silently truncate + ACK NORMAL (which would
     *      drop any READ_COIN/READ_ANALOG bundled after it). Covers GPO(0x32),
     *      ANALOG_OUT(0x33) and CHAR_OUT(0x34). */
    {
        /* re-address to 1 (prior test left it at 1, but be explicit) */
        unsigned char rst[] = { JVS_CMD_RESET, 0xD9 };
        jvs_host_bytes(n, pkt, build(pkt, JVS_BROADCAST, rst, sizeof rst), out, sizeof out);
        unsigned char sa[] = { JVS_CMD_SET_ADDR, 0x01 };
        jvs_host_bytes(n, pkt, build(pkt, JVS_BROADCAST, sa, sizeof sa), out, sizeof out);

        /* GPO claims 5 output bytes but only 2 follow -> UNKNOWN */
        unsigned char dg[] = { JVS_CMD_GPO, 0x05, 0xAA, 0xBB };
        size_t Lg = build(pkt, 0x01, dg, sizeof dg);
        size_t og = jvs_host_bytes(n, pkt, Lg, out, sizeof out);
        CHECK(parse_resp(out, og, &node, rd, &rlen), "truncated GPO parses\n");
        CHECK(rd[0] == JVS_STATUS_UNKNOWN, "over-declared GPO -> UNKNOWN (got %02x)\n", rd[0]);

        /* ANALOG_OUT claims 4 channels (8 bytes) but only 2 bytes follow -> UNKNOWN.
         * A trailing READ_COIN that would otherwise be dropped must NOT be ACKed
         * NORMAL: the packet is flagged param-error as a whole. */
        unsigned char da[] = { JVS_CMD_ANALOG_OUT, 0x04, 0xFF, 0x00,
                               JVS_CMD_READ_COIN, 0x01 };
        size_t La = build(pkt, 0x01, da, sizeof da);
        size_t oa = jvs_host_bytes(n, pkt, La, out, sizeof out);
        CHECK(parse_resp(out, oa, &node, rd, &rlen), "truncated ANALOG_OUT parses\n");
        CHECK(rd[0] == JVS_STATUS_UNKNOWN, "over-declared ANALOG_OUT -> UNKNOWN (got %02x)\n", rd[0]);

        /* CHAR_OUT claims 9 display bytes but only 1 follows -> UNKNOWN */
        unsigned char dc[] = { JVS_CMD_CHAR_OUT, 0x09, 0x41 };
        size_t Lc = build(pkt, 0x01, dc, sizeof dc);
        size_t oc = jvs_host_bytes(n, pkt, Lc, out, sizeof out);
        CHECK(parse_resp(out, oc, &node, rd, &rlen), "truncated CHAR_OUT parses\n");
        CHECK(rd[0] == JVS_STATUS_UNKNOWN, "over-declared CHAR_OUT -> UNKNOWN (got %02x)\n", rd[0]);

        /* a WELL-FORMED ANALOG_OUT (declared length exactly fits) must still ACK
         * NORMAL -- the guard must not reject valid frames (no Wacky Race regress). */
        unsigned char dok[] = { JVS_CMD_ANALOG_OUT, 0x02, 0xFF, 0x00, 0x00, 0x00 };
        size_t Lok = build(pkt, 0x01, dok, sizeof dok);
        size_t ook = jvs_host_bytes(n, pkt, Lok, out, sizeof out);
        CHECK(parse_resp(out, ook, &node, rd, &rlen), "exact-fit ANALOG_OUT parses\n");
        CHECK(rd[0] == JVS_STATUS_NORMAL, "exact-fit ANALOG_OUT -> NORMAL (got %02x)\n", rd[0]);
    }

    /* (p) READ_ANALOG for channels beyond our data must center (0x8000), NOT 0
     *     (a 0 reads as full-negative deflection -> jammed wheel/pedal). */
    {
        ultra_jvs_input in; memset(&in, 0, sizeof in);
        for (int a = 0; a < JVS_MAX_ANALOG; ++a) in.analog[a] = 0x8000;
        jvs_set_input(n, &in);
        unsigned char d[] = { JVS_CMD_READ_ANALOG, 0x0A };  /* 10 ch (game polls 10) */
        size_t L = build(pkt, 0x01, d, sizeof d);
        size_t o = jvs_host_bytes(n, pkt, L, out, sizeof out);
        CHECK(parse_resp(out, o, &node, rd, &rlen), "10-ch READ_ANALOG parses\n");
        /* every channel should be 0x8000 (center) */
        int all_center = 1;
        for (int c = 0; c < 10; ++c) if (rd[1 + 1 + c*2] != 0x80 || rd[1 + 2 + c*2] != 0x00) all_center = 0;
        CHECK(all_center, "all 10 analog channels centered at 0x8000\n");
    }

    jvs_destroy(n);
    printf("\n==== JVS core: %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
