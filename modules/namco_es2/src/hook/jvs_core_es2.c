/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  jvs_core_es2.c -- Ultra Loader clean-room JVS I/O engine (Namco System ES2's own
 *                    independent copy -- implementation)
 * =============================================================================
 *  Clean-room from the published JAMMA Video Standard. No third-party loader
 *  code. Models one JVS I/O node answering a host arcade game's command stream.
 *  Board-pinned: compiled only into modules/namco_es2's hook DLL, never shared
 *  with any other board's build.
 *
 *  Wire format (one packet):
 *      E0 | NODE | NUM | DATA... | SUM
 *  - E0 (SYNC) is literal and never escaped; it only ever marks a packet start.
 *  - Every other field byte equal to E0 or D0 is escaped on the wire as
 *    D0 followed by (byte-1). NUM counts the *logical* (unescaped) bytes that
 *    follow it, including SUM. SUM = (NODE + NUM + sum(DATA)) & 0xFF.
 *  - Host address is 0x00; nodes take 0x01..0x1F; 0xFF is broadcast.
 *
 *  No board-specific RE-derived quirks are baked in here (unlike some other
 *  boards' copies of this engine) -- there is no real Namco System ES2 game dump on
 *  hand yet (see modules/namco_es2/UNTESTED.txt), so this follows the plain
 *  spec: answer packets addressed to us (or unaddressed setup broadcasts), stay
 *  silent otherwise so the host's chain-walk completes correctly.
 * =============================================================================
 */
#include "jvs_core_es2.h"

#define ULTRA_JVS_BOARD_ID "namco_es2"
#include "jvs_ident_db.h"
#include <stdlib.h>
#include <string.h>

#define JVS_ES2_RX_CAP   4096u

struct ultra_jvs_node_es2 {
    /* config */
    char     id[128];
    uint8_t  players, switches_pp, coin_slots, analog_ch, analog_bits, gpo_slots;
    uint8_t  has_screen_pos;
    /* runtime */
    uint8_t  address;                     /* 0 = unassigned */
    uint32_t coin[JVS_ES2_MAX_COINS];     /* 14-bit counters the game reads/debits */
    ultra_jvs_input_es2 in;
    /* receive accumulator (raw wire bytes) */
    uint8_t  rx[JVS_ES2_RX_CAP];
    size_t   rx_len;
};

/* -------------------------------------------------------------------------- */
/*  Lifecycle / config                                                         */
/* -------------------------------------------------------------------------- */

static void set_default_id(ultra_jvs_node_es2* n) {
    /* Generic, widely-accepted-format I/O identity. Branded as ours but in the
     * conventional "maker;board;version;date" shape games expect. Overridable. */
    /* WHICH I/O BOARD WE CLAIM TO BE.
     *
     * This is not cosmetic. Titles that match the IOIDENT (0x10) reply against
     * their own accept-table refuse to boot on a board they do not recognize,
     * and the self-branded identity that used to live here was on no game's
     * list anywhere -- Mario Kart Arcade GP DX stopped dead on it with
     * "E03-02  I/O board type is abnormal".
     *
     * ultra_jvs_ident_autoselect() reads the GAME'S OWN executable for the
     * identities it accepts and answers with one of those, falling back to the
     * catalog. See jvs_ident_db.h. ULTRA_JVS_ID overrides everything. */
    {
        const char *sel = ultra_jvs_ident_autoselect(ULTRA_JVS_BOARD_ID);
        if (sel && *sel) {
            size_t k = strlen(sel);
            if (k >= sizeof n->id) k = sizeof n->id - 1;
            memcpy(n->id, sel, k);
            n->id[k] = '\0';
            return;
        }
    }
}

ultra_jvs_node_es2* jvs_es2_create(void) {
    ultra_jvs_node_es2* n = (ultra_jvs_node_es2*)calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->players      = 2;
    n->switches_pp  = 13;
    n->coin_slots   = 2;
    n->analog_ch    = 8;
    n->analog_bits  = 16;
    n->gpo_slots    = 2;
    n->has_screen_pos = 0;
    /* Start every axis CENTERED, not at zero.
     *
     * calloc leaves analog[] at 0x0000, and this header's own convention (see
     * jvs_core.h) is that 0x0000 is an EXTREME -- full lock -- while 0x8000 is
     * center. So between jvs_create() and the hook's first jvs_set_input() the
     * node was telling the game that every wheel, pedal and stick was jammed
     * hard against its stop. A driving game that runs a steering check during
     * boot reads that state before any input has been published. */
    {
        int a;
        for (a = 0; a < JVS_MAX_ANALOG; ++a) n->in.analog[a] = 0x8000;
        n->in.screen_x = n->in.screen_y = 0x8000;
    }
    set_default_id(n);
    return n;
}

void jvs_es2_destroy(ultra_jvs_node_es2* n) { free(n); }

void jvs_es2_set_id(ultra_jvs_node_es2* n, const char* id) {
    if (!n) return;
    if (!id || !*id) { set_default_id(n); return; }
    size_t k = strlen(id);
    if (k >= sizeof(n->id)) k = sizeof(n->id) - 1;
    memcpy(n->id, id, k);
    n->id[k] = '\0';
}

void jvs_es2_set_features(ultra_jvs_node_es2* n,
                          uint8_t players, uint8_t switches_per_player,
                          uint8_t coin_slots, uint8_t analog_channels,
                          uint8_t analog_bits, uint8_t gpo_slots,
                          uint8_t has_screen_pos) {
    if (!n) return;
    if (players)             n->players     = players > JVS_ES2_MAX_PLAYERS ? JVS_ES2_MAX_PLAYERS : players;
    if (switches_per_player) n->switches_pp = switches_per_player;
    if (coin_slots)          n->coin_slots  = coin_slots > JVS_ES2_MAX_COINS ? JVS_ES2_MAX_COINS : coin_slots;
    if (analog_channels)     n->analog_ch   = analog_channels > JVS_ES2_MAX_ANALOG ? JVS_ES2_MAX_ANALOG : analog_channels;
    if (analog_bits)         n->analog_bits = analog_bits;
    if (gpo_slots)           n->gpo_slots   = gpo_slots;
    n->has_screen_pos = has_screen_pos ? 1 : 0;
}

void jvs_es2_set_input(ultra_jvs_node_es2* n, const ultra_jvs_input_es2* in) {
    if (n && in) n->in = *in;
}
void jvs_es2_get_input(const ultra_jvs_node_es2* n, ultra_jvs_input_es2* out) {
    if (n && out) *out = n->in; else if (out) memset(out, 0, sizeof(*out));
}

void jvs_es2_insert_coin(ultra_jvs_node_es2* n, int slot, int count) {
    if (!n || slot < 0 || slot >= JVS_ES2_MAX_COINS || count <= 0) return;
    n->coin[slot] += (uint32_t)count;
    if (n->coin[slot] > 0x3FFF) n->coin[slot] = 0x3FFF;   /* 14-bit field */
}

uint8_t  jvs_es2_address(const ultra_jvs_node_es2* n)            { return n ? n->address : 0; }
uint32_t jvs_es2_coin_count(const ultra_jvs_node_es2* n, int s)  {
    return (n && s >= 0 && s < JVS_ES2_MAX_COINS) ? n->coin[s] : 0;
}

/* -------------------------------------------------------------------------- */
/*  Wire helpers                                                                */
/* -------------------------------------------------------------------------- */

/* Read one logical byte from wire[*wp..len), unescaping. Returns 1 on success
 * (advances *wp), 0 if not enough wire bytes are present yet. */
static int rd(const uint8_t* w, size_t len, size_t* wp, uint8_t* out) {
    if (*wp >= len) return 0;
    uint8_t b = w[*wp];
    if (b == JVS_ES2_ESCAPE) {
        if (*wp + 1 >= len) return 0;             /* escape seen, payload pending */
        *out = (uint8_t)(w[*wp + 1] + 1);
        *wp += 2;
    } else {
        *out = b;
        *wp += 1;
    }
    return 1;
}

/* Append one logical byte to out[*op..cap), escaping E0/D0. *op always advances
 * by the number of wire bytes the byte occupies; the store is skipped past cap. */
static void wr(uint8_t* out, size_t* op, size_t cap, uint8_t b) {
    uint8_t seq[2]; size_t k = 0;
    if (b == JVS_ES2_SYNC || b == JVS_ES2_ESCAPE) { seq[k++] = JVS_ES2_ESCAPE; seq[k++] = (uint8_t)(b - 1); }
    else                                          { seq[k++] = b; }
    for (size_t j = 0; j < k; ++j) { if (*op < cap) out[*op] = seq[j]; (*op)++; }
}

/* -------------------------------------------------------------------------- */
/*  Command dispatch -- build the logical response data (status + reports)     */
/* -------------------------------------------------------------------------- */

/* Returns logical response length in `rsp`, or 0 for "no response" (RESET). */
static size_t handle_packet(ultra_jvs_node_es2* n, uint8_t dest,
                            const uint8_t* d, size_t dlen,
                            uint8_t* rsp, size_t rsp_cap) {
    /* Plain-spec addressing filter (no board-specific host-address quirks --
     * no real dump exists yet to justify one; see file header):
     *  - node->host traffic (dest==HOST_ADDR) is never ours to answer.
     *  - unaddressed (addr 0): accept everything (setup phase: RESET / SET_ADDR).
     *  - addressed: answer only packets to OUR address; for broadcast accept
     *    ONLY RESET. Staying silent to a broadcast node-probe once addressed is
     *    how the host learns the chain is fully addressed and stops walking
     *    SET_ADDRESS. */
    if (dest == JVS_ES2_HOST_ADDR) return 0;
    if (n->address != 0) {
        if (dest == JVS_ES2_BROADCAST) {
            if (!(dlen >= 1 && d[0] == JVS_ES2_CMD_RESET)) return 0;
        } else if (dest != n->address) {
            return 0;
        }
    }

    size_t r = 0;
    uint8_t status = JVS_ES2_STATUS_NORMAL;
    int no_response = 0, overflow = 0;

    /* A JVS packet's NUM is one byte, so the logical response (status + reports)
     * is at most 254 bytes (NUM = data + 1 <= 255). Bound to that AND the
     * caller's buffer. PUT stores only within bound and otherwise flags overflow
     * and stops growing -- this stops a large READ_SWITCH/ANALOG count from
     * running r past rsp[] (the critical OOB read). */
    const size_t cap = (rsp_cap < 254) ? rsp_cap : 254;
    /* reserve rsp[0] for status; fill reports after it */
    #define PUT(b) do { if (1 + r < cap) { rsp[1 + r] = (uint8_t)(b); r++; } else { overflow = 1; } } while (0)

    size_t i = 0;
    while (i < dlen) {
        uint8_t cmd = d[i++];
        switch (cmd) {

        case JVS_ES2_CMD_RESET:                    /* F0 D9 -- no response */
            if (i < dlen) i++;                     /* consume 0xD9 */
            n->address = 0;
            no_response = 1;
            i = dlen;                              /* RESET is terminal */
            break;

        case JVS_ES2_CMD_SET_ADDR:                 /* F1 addr */
            if (i >= dlen) { status = JVS_ES2_STATUS_UNKNOWN; i = dlen; break; }
            /* Only an UNaddressed node accepts an address (JVS daisy-chain). Once
             * addressed we ignore further broadcast SET_ADDRESS with NO response,
             * so the host sees no node beyond us and stops walking the chain. */
            if (n->address == 0) {
                n->address = d[i++];
                PUT(JVS_ES2_REPORT_NORMAL);
            } else {
                i++;                 /* consume addr byte; stay silent */
                no_response = 1;
                i = dlen;
            }
            break;

        case JVS_ES2_CMD_SET_COMMS:                /* F2 method */
            if (i < dlen) i++;
            PUT(JVS_ES2_REPORT_NORMAL);
            break;

        case JVS_ES2_CMD_IOIDENT: {                /* 10 -> report + id\0 */
            PUT(JVS_ES2_REPORT_NORMAL);
            const char* s = n->id;
            while (*s) PUT((uint8_t)*s++);
            PUT(0x00);
            break;
        }
        case JVS_ES2_CMD_CMD_REV:  PUT(JVS_ES2_REPORT_NORMAL); PUT(0x13); break; /* 1.3 */
        case JVS_ES2_CMD_JVS_REV:  PUT(JVS_ES2_REPORT_NORMAL); PUT(0x30); break; /* 3.0 */
        case JVS_ES2_CMD_COMM_VER: PUT(JVS_ES2_REPORT_NORMAL); PUT(0x10); break; /* 1.0 */

        case JVS_ES2_CMD_FEATCHK: {                 /* 14 -> report + features + 0x00 */
            PUT(JVS_ES2_REPORT_NORMAL);
            /* switch input: players, switches/player */
            PUT(0x01); PUT(n->players); PUT(n->switches_pp); PUT(0x00);
            /* coin input: slots */
            PUT(0x02); PUT(n->coin_slots); PUT(0x00); PUT(0x00);
            /* analog input: channels, bits */
            PUT(0x03); PUT(n->analog_ch); PUT(n->analog_bits); PUT(0x00);
            if (n->has_screen_pos) {                /* screen position: Xbits,Ybits,#ch */
                PUT(0x06); PUT(0x10); PUT(0x10); PUT(0x01);
            }
            /* general purpose output: slots(bits) */
            PUT(0x12); PUT(n->gpo_slots); PUT(0x00); PUT(0x00);
            PUT(0x00);                              /* end of features */
            break;
        }

        case JVS_ES2_CMD_MAIN_ID:                   /* 15 + string\0 */
            while (i < dlen && d[i] != 0x00) i++;
            if (i < dlen) i++;                      /* consume terminator */
            PUT(JVS_ES2_REPORT_NORMAL);
            break;

        case JVS_ES2_CMD_READ_SWITCH: {             /* 20 nplayers nbytes */
            if (i + 1 >= dlen) { status = JVS_ES2_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t np = d[i++], nb = d[i++];
            PUT(JVS_ES2_REPORT_NORMAL);
            PUT(n->in.system);                      /* system (TEST/TILT) */
            for (uint8_t p = 0; p < np; ++p) {
                uint16_t pw = (p < JVS_ES2_MAX_PLAYERS) ? n->in.player[p] : 0;
                for (uint8_t b = 0; b < nb; ++b) {
                    uint8_t v = (b == 0) ? (uint8_t)(pw >> 8)
                              : (b == 1) ? (uint8_t)(pw & 0xff)
                              : 0;
                    PUT(v);
                }
            }
            break;
        }

        case JVS_ES2_CMD_READ_COIN: {               /* 21 nslots -> 2 bytes/slot */
            if (i >= dlen) { status = JVS_ES2_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t ns = d[i++];
            PUT(JVS_ES2_REPORT_NORMAL);
            for (uint8_t s = 0; s < ns; ++s) {
                uint32_t c = (s < JVS_ES2_MAX_COINS) ? n->coin[s] : 0;
                if (c > 0x3FFF) c = 0x3FFF;
                PUT((uint8_t)((c >> 8) & 0x3F));    /* cond=00 (normal) | count hi */
                PUT((uint8_t)(c & 0xFF));           /* count lo */
            }
            break;
        }

        case JVS_ES2_CMD_READ_ANALOG: {             /* 22 nch -> 2 bytes/ch (BE) */
            if (i >= dlen) { status = JVS_ES2_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nc = d[i++];
            PUT(JVS_ES2_REPORT_NORMAL);
            for (uint8_t c = 0; c < nc; ++c) {
                /* Center (0x8000) for any channel with no live data: a 0 here
                 * would read as full-negative deflection on an analog axis. */
                uint16_t v = (c < JVS_ES2_MAX_ANALOG) ? n->in.analog[c] : 0x8000;
                PUT((uint8_t)(v >> 8)); PUT((uint8_t)(v & 0xff));
            }
            break;
        }

        case JVS_ES2_CMD_READ_ROTARY: {             /* 23 nch -> 2 bytes/ch (0) */
            if (i >= dlen) { status = JVS_ES2_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nc = d[i++];
            PUT(JVS_ES2_REPORT_NORMAL);
            for (uint8_t c = 0; c < nc; ++c) { PUT(0); PUT(0); }
            break;
        }

        case JVS_ES2_CMD_READ_SCRPOS: {             /* 25 which -> X hi/lo, Y hi/lo */
            if (i >= dlen) { status = JVS_ES2_STATUS_UNKNOWN; i = dlen; break; }
            i++;                                     /* channel index (single gun) */
            PUT(JVS_ES2_REPORT_NORMAL);
            PUT((uint8_t)(n->in.screen_x >> 8)); PUT((uint8_t)(n->in.screen_x & 0xff));
            PUT((uint8_t)(n->in.screen_y >> 8)); PUT((uint8_t)(n->in.screen_y & 0xff));
            break;
        }

        case JVS_ES2_CMD_READ_MISC: {               /* 26 nbytes -> zeros */
            if (i >= dlen) { status = JVS_ES2_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nbytes = d[i++];
            PUT(JVS_ES2_REPORT_NORMAL);
            for (uint8_t b = 0; b < nbytes; ++b) PUT(0);
            break;
        }

        case JVS_ES2_CMD_COIN_DEC:                   /* 30 slot hi lo */
        case JVS_ES2_CMD_COIN_INC: {                 /* 35 slot hi lo */
            if (i + 2 >= dlen) { status = JVS_ES2_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t slot = d[i++];
            uint16_t amt = (uint16_t)((d[i] << 8) | d[i + 1]); i += 2;
            /* JVS numbers COIN_DECREASE/INCREASE slots 1-based (slot 1 = the
             * first chute); map to our 0-based array so a debit hits the same
             * counter READ_COIN just reported (a naive 0-based read of the wire
             * slot would debit an empty neighboring counter). */
            uint8_t idx = (slot >= 1) ? (uint8_t)(slot - 1) : 0;
            if (idx < JVS_ES2_MAX_COINS) {
                if (cmd == JVS_ES2_CMD_COIN_DEC) {
                    n->coin[idx] = (n->coin[idx] >= amt) ? n->coin[idx] - amt : 0;
                } else {
                    n->coin[idx] += amt;
                    if (n->coin[idx] > 0x3FFF) n->coin[idx] = 0x3FFF;
                }
            }
            PUT(JVS_ES2_REPORT_NORMAL);
            break;
        }

        case JVS_ES2_CMD_GPO: {                      /* 32 nbytes data... */
            if (i >= dlen) { status = JVS_ES2_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nbytes = d[i++];
            /* A declared output length that runs past the packet end is a
             * malformed frame: flag param error rather than silently truncating
             * + ACKing NORMAL (which would drop any bundled command after it). */
            if (nbytes > (uint8_t)(dlen - i)) { status = JVS_ES2_STATUS_UNKNOWN; i = dlen; break; }
            i += nbytes;                                            /* consume outputs */
            PUT(JVS_ES2_REPORT_NORMAL);
            break;
        }

        case JVS_ES2_CMD_ANALOG_OUT: {                /* 33 nch + 2 bytes/ch (force-fb) */
            if (i >= dlen) { status = JVS_ES2_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nch = d[i++];
            if ((uint16_t)nch * 2u > (uint16_t)(dlen - i)) { status = JVS_ES2_STATUS_UNKNOWN; i = dlen; break; }
            i += (uint16_t)nch * 2u;                                /* consume DAC words */
            PUT(JVS_ES2_REPORT_NORMAL);
            break;
        }

        case JVS_ES2_CMD_CHAR_OUT: {                  /* 34 nbytes + display chars */
            if (i >= dlen) { status = JVS_ES2_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nbytes = d[i++];
            if (nbytes > (uint8_t)(dlen - i)) { status = JVS_ES2_STATUS_UNKNOWN; i = dlen; break; }
            i += nbytes;
            PUT(JVS_ES2_REPORT_NORMAL);
            break;
        }

        case JVS_ES2_CMD_GPO_BYTE:                    /* 38 index value */
            if (i + 1 >= dlen) { status = JVS_ES2_STATUS_UNKNOWN; i = dlen; break; }
            i += 2;
            PUT(JVS_ES2_REPORT_NORMAL);
            break;

        case JVS_ES2_CMD_RETRANSMIT:                  /* 2F -- ack only */
            PUT(JVS_ES2_REPORT_NORMAL);
            break;

        case JVS_ES2_CMD_READ_KEYCODE:                /* 24 -- no keypad: ack, no keys */
            PUT(JVS_ES2_REPORT_NORMAL);
            break;

        default:
            /* Unknown command: per spec, flag the whole packet and stop, since
             * we cannot know this command's parameter length to skip it. Reports
             * for valid commands earlier in the packet remain in rsp[]. */
            status = JVS_ES2_STATUS_UNKNOWN;
            i = dlen;
            break;
        }
    }
    #undef PUT

    if (no_response) return 0;
    /* If we hit the response-size ceiling, report ACK overflow rather than a
     * length-wrapped frame; r is already bounded to <= cap-1 (<= 253). */
    rsp[0] = overflow ? JVS_ES2_STATUS_OVERFLOW : status;
    return r + 1;     /* +1 for the status byte (<= 254, so NUM <= 255) */
}

/* -------------------------------------------------------------------------- */
/*  Transport entry: feed wire bytes, get framed responses                     */
/* -------------------------------------------------------------------------- */

size_t jvs_es2_host_bytes(ultra_jvs_node_es2* n,
                          const uint8_t* in, size_t in_len,
                          uint8_t* out, size_t out_cap) {
    if (!n) return 0;

    /* accumulate */
    if (in && in_len) {
        if (n->rx_len + in_len > JVS_ES2_RX_CAP) n->rx_len = 0;   /* overflow: resync */
        if (in_len <= JVS_ES2_RX_CAP) {
            memcpy(n->rx + n->rx_len, in, in_len);
            n->rx_len += in_len;
        }
    }

    size_t op = 0;

    for (;;) {
        /* find SYNC at the head */
        size_t start = 0;
        while (start < n->rx_len && n->rx[start] != JVS_ES2_SYNC) start++;
        if (start) {                       /* drop noise before SYNC */
            memmove(n->rx, n->rx + start, n->rx_len - start);
            n->rx_len -= start;
        }
        if (n->rx_len < 2) break;          /* need at least SYNC + something */

        size_t wp = 1;                     /* past SYNC */
        uint8_t node, num;
        if (!rd(n->rx, n->rx_len, &wp, &node)) break;   /* need more */
        if (!rd(n->rx, n->rx_len, &wp, &num))  break;
        if (num == 0) {                    /* impossible (needs >=1 for sum): resync */
            memmove(n->rx, n->rx + 1, n->rx_len - 1); n->rx_len -= 1; continue;
        }

        /* read num logical bytes: (num-1) data + 1 checksum. num is one byte
         * (<=255), so a 256-byte buffer always holds a whole packet's data. */
        uint8_t data[256];
        size_t got = 0; int incomplete = 0;
        for (; got < num; ++got) {
            if (!rd(n->rx, n->rx_len, &wp, &data[got])) { incomplete = 1; break; }
        }
        if (incomplete) break;             /* whole packet not on the wire yet */

        uint8_t sum_rx = data[num - 1];
        size_t  dlen   = (size_t)num - 1;  /* data excluding checksum */
        uint8_t sum = (uint8_t)(node + num);
        for (size_t k = 0; k < dlen; ++k) sum = (uint8_t)(sum + data[k]);

        /* consume this packet's wire bytes regardless of checksum outcome */
        size_t consumed = wp;

        if (sum != sum_rx) {
            /* checksum mismatch: drop the SYNC and resync (do not consume rest) */
            memmove(n->rx, n->rx + 1, n->rx_len - 1); n->rx_len -= 1; continue;
        }

        /* dispatch */
        uint8_t rsp[640];
        size_t rlen = handle_packet(n, node, data, dlen, rsp, sizeof(rsp));
        if (rlen > sizeof(rsp)) rlen = sizeof(rsp);   /* defensive: never read past rsp */

        if (rlen) {
            /* frame: SYNC, NODE(=host 0x00), NUM(=rlen+1), data..., SUM */
            uint8_t nodb = JVS_ES2_HOST_ADDR;
            uint8_t numb = (uint8_t)(rlen + 1);
            uint8_t csum = (uint8_t)(nodb + numb);
            for (size_t k = 0; k < rlen; ++k) csum = (uint8_t)(csum + rsp[k]);

            if (op < out_cap) out[op] = JVS_ES2_SYNC;   /* SYNC: literal, never escaped */
            op++;
            wr(out, &op, out_cap, nodb);
            wr(out, &op, out_cap, numb);
            for (size_t k = 0; k < rlen; ++k) wr(out, &op, out_cap, rsp[k]);
            wr(out, &op, out_cap, csum);
        }

        /* remove consumed bytes */
        memmove(n->rx, n->rx + consumed, n->rx_len - consumed);
        n->rx_len -= consumed;
        if (n->rx_len == 0) break;
    }

    return op;
}
