/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  jvs_core_unico.c  --  UNICO (Korea) PC board's OWN clean-room JVS engine
 * =============================================================================
 *  Clean-room from the published JAMMA Video Standard. No third-party loader
 *  code, and no dependency on modules/native_x86/src/hook/'s jvs_core.c (that
 *  file was read as a reference only). This is UNICO PC's own compiled copy
 *  of the same public-spec logic, under its own file name and symbols, so a
 *  future rebuild of the shared native_x86 hook -- or any other board's
 *  module -- can never affect this one.
 *
 *  Wire format (one packet):
 *      E0 | NODE | NUM | DATA... | SUM
 *  - E0 (SYNC) is literal and never escaped; it only ever marks a packet start.
 *  - Every other field byte equal to E0 or D0 is escaped on the wire as
 *    D0 followed by (byte-1). NUM counts the *logical* (unescaped) bytes that
 *    follow it, including SUM. SUM = (NODE + NUM + sum(DATA)) & 0xFF.
 *  - Host address is 0x00; nodes take 0x01..0x1F; 0xFF is broadcast.
 * =============================================================================
 */
#include "jvs_core_unico.h"

#define ULTRA_JVS_BOARD_ID "unico_pc"
#include "jvs_ident_db.h"
#include <stdlib.h>
#include <string.h>

#define JVS_UNICO_RX_CAP   4096u

struct ultra_jvs_node_unico {
    /* config */
    char     id[128];
    uint8_t  players, switches_pp, coin_slots, analog_ch, analog_bits, gpo_slots;
    uint8_t  has_screen_pos;
    /* runtime */
    uint8_t  address;                        /* 0 = unassigned */
    uint32_t coin[JVS_UNICO_MAX_COINS];       /* 14-bit counters the game reads/debits */
    ultra_jvs_input_unico in;
    /* receive accumulator (raw wire bytes) */
    uint8_t  rx[JVS_UNICO_RX_CAP];
    size_t   rx_len;
};

/* -------------------------------------------------------------------------- */
/*  Lifecycle / config                                                         */
/* -------------------------------------------------------------------------- */

static void set_default_id(ultra_jvs_node_unico* n) {
    /* WHICH I/O BOARD WE CLAIM TO BE.
     *
     * Not cosmetic: titles that match the IOIDENT (0x10) reply against their
     * own accept-table refuse to boot on a board they do not recognize, and
     * the self-branded identity that used to live here was on no game's list
     * anywhere. ultra_jvs_ident_autoselect() reads the GAME'S OWN executable
     * for the identities it accepts. See jvs_ident_db.h.
     * ULTRA_JVS_ID overrides everything. */
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

ultra_jvs_node_unico* jvs_unico_create(void) {
    ultra_jvs_node_unico* n = (ultra_jvs_node_unico*)calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->players        = 2;
    n->switches_pp    = 13;
    n->coin_slots     = 2;
    n->analog_ch      = 8;
    n->analog_bits    = 16;
    n->gpo_slots      = 2;
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

void jvs_unico_destroy(ultra_jvs_node_unico* n) { free(n); }

void jvs_unico_set_id(ultra_jvs_node_unico* n, const char* id) {
    if (!n) return;
    if (!id || !*id) { set_default_id(n); return; }
    size_t k = strlen(id);
    if (k >= sizeof(n->id)) k = sizeof(n->id) - 1;
    memcpy(n->id, id, k);
    n->id[k] = '\0';
}

void jvs_unico_set_features(ultra_jvs_node_unico* n,
                            uint8_t players, uint8_t switches_per_player,
                            uint8_t coin_slots, uint8_t analog_channels,
                            uint8_t analog_bits, uint8_t gpo_slots,
                            uint8_t has_screen_pos) {
    if (!n) return;
    if (players)             n->players     = players > JVS_UNICO_MAX_PLAYERS ? JVS_UNICO_MAX_PLAYERS : players;
    if (switches_per_player) n->switches_pp = switches_per_player;
    if (coin_slots)          n->coin_slots  = coin_slots > JVS_UNICO_MAX_COINS ? JVS_UNICO_MAX_COINS : coin_slots;
    if (analog_channels)     n->analog_ch   = analog_channels > JVS_UNICO_MAX_ANALOG ? JVS_UNICO_MAX_ANALOG : analog_channels;
    if (analog_bits)         n->analog_bits = analog_bits;
    if (gpo_slots)           n->gpo_slots   = gpo_slots;
    n->has_screen_pos = has_screen_pos ? 1 : 0;
}

void jvs_unico_set_input(ultra_jvs_node_unico* n, const ultra_jvs_input_unico* in) {
    if (n && in) n->in = *in;
}
void jvs_unico_get_input(const ultra_jvs_node_unico* n, ultra_jvs_input_unico* out) {
    if (n && out) *out = n->in; else if (out) memset(out, 0, sizeof(*out));
}

void jvs_unico_insert_coin(ultra_jvs_node_unico* n, int slot, int count) {
    if (!n || slot < 0 || slot >= JVS_UNICO_MAX_COINS || count <= 0) return;
    n->coin[slot] += (uint32_t)count;
    if (n->coin[slot] > 0x3FFF) n->coin[slot] = 0x3FFF;   /* 14-bit field */
}

uint8_t  jvs_unico_address(const ultra_jvs_node_unico* n)           { return n ? n->address : 0; }
uint32_t jvs_unico_coin_count(const ultra_jvs_node_unico* n, int s) {
    return (n && s >= 0 && s < JVS_UNICO_MAX_COINS) ? n->coin[s] : 0;
}

/* -------------------------------------------------------------------------- */
/*  Wire helpers                                                                */
/* -------------------------------------------------------------------------- */

static int rd(const uint8_t* w, size_t len, size_t* wp, uint8_t* out) {
    if (*wp >= len) return 0;
    uint8_t b = w[*wp];
    if (b == JVS_UNICO_ESCAPE) {
        if (*wp + 1 >= len) return 0;
        *out = (uint8_t)(w[*wp + 1] + 1);
        *wp += 2;
    } else {
        *out = b;
        *wp += 1;
    }
    return 1;
}

static void wr(uint8_t* out, size_t* op, size_t cap, uint8_t b) {
    uint8_t seq[2]; size_t k = 0;
    if (b == JVS_UNICO_SYNC || b == JVS_UNICO_ESCAPE) { seq[k++] = JVS_UNICO_ESCAPE; seq[k++] = (uint8_t)(b - 1); }
    else                                              { seq[k++] = b; }
    for (size_t j = 0; j < k; ++j) { if (*op < cap) out[*op] = seq[j]; (*op)++; }
}

/* -------------------------------------------------------------------------- */
/*  Command dispatch -- build the logical response data (status + reports)     */
/* -------------------------------------------------------------------------- */

static size_t handle_packet(ultra_jvs_node_unico* n, uint8_t dest,
                            const uint8_t* d, size_t dlen,
                            uint8_t* rsp, size_t rsp_cap) {
    /* Some PC-arcade JVS libraries poll a host-addressed 0x04 right after
     * switching to the run state and block on the reply; ACK it so the I/O
     * stays healthy (harmless for a title that never sends it). */
    const int host_cmd04 = (dest == JVS_UNICO_HOST_ADDR && dlen >= 1 && d[0] == 0x04);
    if (dest == JVS_UNICO_HOST_ADDR && !host_cmd04) return 0;
    if (!host_cmd04 && n->address != 0) {
        if (dest == JVS_UNICO_BROADCAST) {
            if (!(dlen >= 1 && d[0] == JVS_UNICO_CMD_RESET)) return 0;
        } else if (dest != n->address) {
            return 0;
        }
    }

    size_t r = 0;
    uint8_t status = JVS_UNICO_STATUS_NORMAL;
    int no_response = 0, overflow = 0;

    const size_t cap = (rsp_cap < 254) ? rsp_cap : 254;
    #define PUT(b) do { if (1 + r < cap) { rsp[1 + r] = (uint8_t)(b); r++; } else { overflow = 1; } } while (0)

    size_t i = 0;
    while (i < dlen) {
        uint8_t cmd = d[i++];
        switch (cmd) {

        case JVS_UNICO_CMD_RESET:
            if (i < dlen) i++;
            n->address = 0;
            no_response = 1;
            i = dlen;
            break;

        case JVS_UNICO_CMD_SET_ADDR:
            if (i >= dlen) { status = JVS_UNICO_STATUS_UNKNOWN; i = dlen; break; }
            if (n->address == 0) {
                n->address = d[i++];
                PUT(JVS_UNICO_REPORT_NORMAL);
            } else {
                i++;
                no_response = 1;
                i = dlen;
            }
            break;

        case JVS_UNICO_CMD_SET_COMMS:
            if (i < dlen) i++;
            PUT(JVS_UNICO_REPORT_NORMAL);
            break;

        case 0x04:
            PUT(JVS_UNICO_REPORT_NORMAL);
            break;

        case JVS_UNICO_CMD_IOIDENT: {
            PUT(JVS_UNICO_REPORT_NORMAL);
            const char* s = n->id;
            while (*s) PUT((uint8_t)*s++);
            PUT(0x00);
            break;
        }
        case JVS_UNICO_CMD_CMD_REV:  PUT(JVS_UNICO_REPORT_NORMAL); PUT(0x13); break;
        case JVS_UNICO_CMD_JVS_REV:  PUT(JVS_UNICO_REPORT_NORMAL); PUT(0x30); break;
        case JVS_UNICO_CMD_COMM_VER: PUT(JVS_UNICO_REPORT_NORMAL); PUT(0x10); break;

        case JVS_UNICO_CMD_FEATCHK: {
            PUT(JVS_UNICO_REPORT_NORMAL);
            PUT(0x01); PUT(n->players); PUT(n->switches_pp); PUT(0x00);
            PUT(0x02); PUT(n->coin_slots); PUT(0x00); PUT(0x00);
            PUT(0x03); PUT(n->analog_ch); PUT(n->analog_bits); PUT(0x00);
            if (n->has_screen_pos) { PUT(0x06); PUT(0x10); PUT(0x10); PUT(0x01); }
            PUT(0x12); PUT(n->gpo_slots); PUT(0x00); PUT(0x00);
            PUT(0x00);
            break;
        }

        case JVS_UNICO_CMD_MAIN_ID:
            while (i < dlen && d[i] != 0x00) i++;
            if (i < dlen) i++;
            PUT(JVS_UNICO_REPORT_NORMAL);
            break;

        case JVS_UNICO_CMD_READ_SWITCH: {
            if (i + 1 >= dlen) { status = JVS_UNICO_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t np = d[i++], nb = d[i++];
            PUT(JVS_UNICO_REPORT_NORMAL);
            PUT(n->in.system);
            for (uint8_t p = 0; p < np; ++p) {
                uint16_t pw = (p < JVS_UNICO_MAX_PLAYERS) ? n->in.player[p] : 0;
                for (uint8_t b = 0; b < nb; ++b) {
                    uint8_t v = (b == 0) ? (uint8_t)(pw >> 8)
                              : (b == 1) ? (uint8_t)(pw & 0xff)
                              : 0;
                    PUT(v);
                }
            }
            break;
        }

        case JVS_UNICO_CMD_READ_COIN: {
            if (i >= dlen) { status = JVS_UNICO_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t ns = d[i++];
            PUT(JVS_UNICO_REPORT_NORMAL);
            for (uint8_t s = 0; s < ns; ++s) {
                uint32_t c = (s < JVS_UNICO_MAX_COINS) ? n->coin[s] : 0;
                if (c > 0x3FFF) c = 0x3FFF;
                PUT((uint8_t)((c >> 8) & 0x3F));
                PUT((uint8_t)(c & 0xFF));
            }
            break;
        }

        case JVS_UNICO_CMD_READ_ANALOG: {
            if (i >= dlen) { status = JVS_UNICO_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nc = d[i++];
            PUT(JVS_UNICO_REPORT_NORMAL);
            for (uint8_t c = 0; c < nc; ++c) {
                /* Center for any unmapped channel -- a 0 here would jam a
                 * wheel/pedal axis hard-over. */
                uint16_t v = (c < JVS_UNICO_MAX_ANALOG) ? n->in.analog[c] : 0x8000;
                PUT((uint8_t)(v >> 8)); PUT((uint8_t)(v & 0xff));
            }
            break;
        }

        case JVS_UNICO_CMD_READ_ROTARY: {
            if (i >= dlen) { status = JVS_UNICO_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nc = d[i++];
            PUT(JVS_UNICO_REPORT_NORMAL);
            for (uint8_t c = 0; c < nc; ++c) { PUT(0); PUT(0); }
            break;
        }

        case JVS_UNICO_CMD_READ_SCRPOS: {
            if (i >= dlen) { status = JVS_UNICO_STATUS_UNKNOWN; i = dlen; break; }
            i++;
            PUT(JVS_UNICO_REPORT_NORMAL);
            PUT((uint8_t)(n->in.screen_x >> 8)); PUT((uint8_t)(n->in.screen_x & 0xff));
            PUT((uint8_t)(n->in.screen_y >> 8)); PUT((uint8_t)(n->in.screen_y & 0xff));
            break;
        }

        case JVS_UNICO_CMD_READ_MISC: {
            if (i >= dlen) { status = JVS_UNICO_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nbytes = d[i++];
            PUT(JVS_UNICO_REPORT_NORMAL);
            for (uint8_t b = 0; b < nbytes; ++b) PUT(0);
            break;
        }

        case JVS_UNICO_CMD_COIN_DEC:
        case JVS_UNICO_CMD_COIN_INC: {
            if (i + 2 >= dlen) { status = JVS_UNICO_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t slot = d[i++];
            uint16_t amt = (uint16_t)((d[i] << 8) | d[i + 1]); i += 2;
            /* COIN_DECREASE/INCREASE numbers slots 1-based; map to our 0-based
             * array so a debit hits the same counter READ_COIN just reported. */
            uint8_t idx = (slot >= 1) ? (uint8_t)(slot - 1) : 0;
            if (idx < JVS_UNICO_MAX_COINS) {
                if (cmd == JVS_UNICO_CMD_COIN_DEC) {
                    n->coin[idx] = (n->coin[idx] >= amt) ? n->coin[idx] - amt : 0;
                } else {
                    n->coin[idx] += amt;
                    if (n->coin[idx] > 0x3FFF) n->coin[idx] = 0x3FFF;
                }
            }
            PUT(JVS_UNICO_REPORT_NORMAL);
            break;
        }

        case JVS_UNICO_CMD_GPO: {
            if (i >= dlen) { status = JVS_UNICO_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nbytes = d[i++];
            if (nbytes > (uint8_t)(dlen - i)) { status = JVS_UNICO_STATUS_UNKNOWN; i = dlen; break; }
            i += nbytes;
            PUT(JVS_UNICO_REPORT_NORMAL);
            break;
        }

        case JVS_UNICO_CMD_ANALOG_OUT: {
            /* Driving games drive wheel force-feedback via this command, often
             * bundled with READ_SWITCH/READ_COIN/READ_ANALOG in the same
             * packet -- consume + ACK and KEEP PARSING so the rest of the
             * packet still gets answered (no physical motor to drive here). */
            if (i >= dlen) { status = JVS_UNICO_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nch = d[i++];
            if ((uint16_t)nch * 2u > (uint16_t)(dlen - i)) { status = JVS_UNICO_STATUS_UNKNOWN; i = dlen; break; }
            i += (uint16_t)nch * 2u;
            PUT(JVS_UNICO_REPORT_NORMAL);
            break;
        }

        case JVS_UNICO_CMD_CHAR_OUT: {
            if (i >= dlen) { status = JVS_UNICO_STATUS_UNKNOWN; i = dlen; break; }
            uint8_t nbytes = d[i++];
            if (nbytes > (uint8_t)(dlen - i)) { status = JVS_UNICO_STATUS_UNKNOWN; i = dlen; break; }
            i += nbytes;
            PUT(JVS_UNICO_REPORT_NORMAL);
            break;
        }

        case JVS_UNICO_CMD_GPO_BYTE:
            if (i + 1 >= dlen) { status = JVS_UNICO_STATUS_UNKNOWN; i = dlen; break; }
            i += 2;
            PUT(JVS_UNICO_REPORT_NORMAL);
            break;

        case JVS_UNICO_CMD_RETRANSMIT:
            PUT(JVS_UNICO_REPORT_NORMAL);
            break;

        case JVS_UNICO_CMD_READ_KEYCODE:
            PUT(JVS_UNICO_REPORT_NORMAL);
            break;

        default:
            status = JVS_UNICO_STATUS_UNKNOWN;
            i = dlen;
            break;
        }
    }
    #undef PUT

    if (no_response) return 0;
    rsp[0] = overflow ? JVS_UNICO_STATUS_OVERFLOW : status;
    return r + 1;
}

/* -------------------------------------------------------------------------- */
/*  Transport entry: feed wire bytes, get framed responses                     */
/* -------------------------------------------------------------------------- */

size_t jvs_unico_host_bytes(ultra_jvs_node_unico* n,
                            const uint8_t* in, size_t in_len,
                            uint8_t* out, size_t out_cap) {
    if (!n) return 0;

    if (in && in_len) {
        if (n->rx_len + in_len > JVS_UNICO_RX_CAP) n->rx_len = 0;
        if (in_len <= JVS_UNICO_RX_CAP) {
            memcpy(n->rx + n->rx_len, in, in_len);
            n->rx_len += in_len;
        }
    }

    size_t op = 0;

    for (;;) {
        size_t start = 0;
        while (start < n->rx_len && n->rx[start] != JVS_UNICO_SYNC) start++;
        if (start) {
            memmove(n->rx, n->rx + start, n->rx_len - start);
            n->rx_len -= start;
        }
        if (n->rx_len < 2) break;

        size_t wp = 1;
        uint8_t node, num;
        if (!rd(n->rx, n->rx_len, &wp, &node)) break;
        if (!rd(n->rx, n->rx_len, &wp, &num))  break;
        if (num == 0) {
            memmove(n->rx, n->rx + 1, n->rx_len - 1); n->rx_len -= 1; continue;
        }

        uint8_t data[256];
        size_t got = 0; int incomplete = 0;
        for (; got < num; ++got) {
            if (!rd(n->rx, n->rx_len, &wp, &data[got])) { incomplete = 1; break; }
        }
        if (incomplete) break;

        uint8_t sum_rx = data[num - 1];
        size_t  dlen   = (size_t)num - 1;
        uint8_t sum = (uint8_t)(node + num);
        for (size_t k = 0; k < dlen; ++k) sum = (uint8_t)(sum + data[k]);

        size_t consumed = wp;

        if (sum != sum_rx) {
            memmove(n->rx, n->rx + 1, n->rx_len - 1); n->rx_len -= 1; continue;
        }

        uint8_t rsp[640];
        size_t rlen = handle_packet(n, node, data, dlen, rsp, sizeof(rsp));
        if (rlen > sizeof(rsp)) rlen = sizeof(rsp);

        if (rlen) {
            uint8_t nodb = JVS_UNICO_HOST_ADDR;
            uint8_t numb = (uint8_t)(rlen + 1);
            uint8_t csum = (uint8_t)(nodb + numb);
            for (size_t k = 0; k < rlen; ++k) csum = (uint8_t)(csum + rsp[k]);

            if (op < out_cap) out[op] = JVS_UNICO_SYNC;
            op++;
            wr(out, &op, out_cap, nodb);
            wr(out, &op, out_cap, numb);
            for (size_t k = 0; k < rlen; ++k) wr(out, &op, out_cap, rsp[k]);
            wr(out, &op, out_cap, csum);
        }

        memmove(n->rx, n->rx + consumed, n->rx_len - consumed);
        n->rx_len -= consumed;
        if (n->rx_len == 0) break;
    }

    return op;
}
