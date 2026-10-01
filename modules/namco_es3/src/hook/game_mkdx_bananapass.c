/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  game_mkdx_bananapass.c -- Mario Kart Arcade GP DX "Bananapass" IC-card
 *                             VENDOR (dispenser) unit, COM2
 * =============================================================================
 *
 *  HISTORY OF THIS FILE
 *  ---------------------------------------------------------------------------
 *  v1 (2026-09-24): PASSIVE OBSERVER -- logs every byte the game sends/reads
 *    on COM2; queues NO replies; game hits E23-02 vendor init error; purpose
 *    was to capture the wire protocol since zero prior documentation existed.
 *
 *  v2 (2026-09-25): PN533 RESPONDER -- promoted after the v1 capture confirmed
 *    the card vendor speaks the NXP PN533 UART frame format (standard
 *    preamble/checksum/direction-byte wrapper; see PROTOCOL section below).
 *    Now ACKs every command, sends confirmed replies for known PN533 commands,
 *    zero-length ack-only for anything unknown. Game passes init (no E23-02)
 *    but sees "no card present" on every InListPassiveTarget poll.
 *
 *  v3 (2026-09-27): CARD EMULATOR -- adds ULTRA_BANANAPASS_CARD support.
 *    When set to a 16-digit Bananapass card number, InListPassiveTarget
 *    returns one ISO14443-A (Mifare Classic 1K) target whose UID is derived
 *    from the card number. InDataExchange auth/read/write commands are
 *    handled: auth always succeeds; reads return zeroed blocks for sector
 *    data whose layout is not yet confirmed (all unconfirmed reads are logged
 *    verbosely so a real run captures the card data format for a future pass).
 *    Without ULTRA_BANANAPASS_CARD the behavior is exactly v2.
 *
 *  v4 (2026-09-29): PROTOCOL CORRECTIONS -- every real capture of this game
 *    (v1 through v3) died silently right after command 0x08, every single
 *    time. Root cause: 0x08 was guessed as PN533's WriteRegister and given a
 *    zero-length reply; cross-checked against github.com/whowechina/aic_pico
 *    (bana.c, a real, community-tested BANA reader implementation), 0x08 is
 *    actually "RF field off" and MUST reply with exactly one data byte
 *    (0x00). Fixed, and added several other commands bana.c confirms this
 *    class of reader answers (0x06, 0x0C, 0x42, 0x44, 0x52, 0x54) that this
 *    file previously had no case for at all. Also: 0x02/0x04 no longer get
 *    this file's own earlier guessed PN533-textbook replies -- bana.c's
 *    command table has no case for either, meaning real BANA hardware
 *    doesn't answer them specially, so unrecognized commands (including
 *    those two) now get ACK-only, matching bana.c's own default, instead of
 *    a fabricated response frame. 0x10/0x12/0x14/0x16/0x18/0x32 are
 *    untouched -- confirmed by three separate real captures to already need
 *    exactly the no-data reply they already get, even though bana.c itself
 *    has no case for some of them (its silence means its test hardware was
 *    never asked, not that no real game ever sends it).
 *
 *  WHAT THIS SUBSYSTEM IS
 *  ---------------------------------------------------------------------------
 *  namco_es3_main.c documents (and golden.ini's ULTRA_JVS_COM=2,3,4 confirms)
 *  that Mario Kart Arcade GP DX opens THREE serial devices:
 *      COM3  JVS I/O board          (jvs_core_es3.c handles this)
 *      COM4  drive / steering board (unclaimed / passthrough)
 *      COM2  IC card VENDOR (the physical Bananapass card dispenser/reader)
 *  The IC card reader is QR-web-independent: it directly reads/writes the
 *  player's physical Bananapass card to store their save progression.
 *
 *  Because ULTRA_JVS_COM lists "2", jvs_serial_hook_es3.c's generic
 *  we_claim_port() used to hand COM2 a fake JVS handle, and every byte the
 *  game wrote to it fell into jvs_host_bytes() -- the JVS coin/switch engine.
 *  That is wrong: a card vendor is not a JVS peripheral. This file's
 *  on_create_file callback fires BEFORE the JVS dispatcher sees the path
 *  (board_io_ops is checked first), pulling COM2 cleanly out of JVS.
 *
 *  PROTOCOL (confirmed by 2026-09-25 capture)
 *  ---------------------------------------------------------------------------
 *  CardVenderManager speaks the NXP PN533 UART frame format:
 *
 *      wake-up: 0x55 (one or more, no reply -- UART bit-sync warm-up)
 *      ACK    : 00 00 FF 00 FF 00              (6 bytes, no data)
 *      frame  : 00 00 FF LEN LCS TFI PD0..PDn DCS 00
 *                 LEN = 1 + n  (TFI + n payload bytes)
 *                 LCS = (0x100 - LEN) & 0xFF
 *                 TFI = 0xD4 host->reader, 0xD5 reader->host
 *                 DCS = (0x100 - (TFI + PD0 + ... + PDn)) & 0xFF
 *
 *  Confirmed frames from the capture:
 *      #0    TX 00 00 FF 03 FD D4 10 02 1A 00  CC=0x10 SetSerialBaudRate(2)
 *      #2580 TX 55                             wake-up (after unanswered reads)
 *      #2581 TX 00 00 FF 00 FF 00              host ACK
 *      #2582 TX 00 00 FF 03 FD D4 18 01 13 00  CC=0x18 RFRegulationTest(1)
 *
 *  GATING
 *  ---------------------------------------------------------------------------
 *  1. Exe identity: exact file size 6,819,840 bytes (MK_AGP3_FINAL.exe as
 *     shipped for this project), OR the filename itself, OR ULTRA_MKDX_ID=1
 *     escape hatch for a differently-packaged copy.
 *  2. ULTRA_BANANAPASS_VENDOR=1: all callbacks are hard no-ops unless set --
 *     so COM2 keeps its current behavior (fake JVS node) until explicitly
 *     opted in, and this file cannot affect Pokken or any other namco_es3
 *     title that passes the identity check.
 *  3. ULTRA_BANANAPASS_CARD=<16-digit card number>: enables card presence.
 *     Leave unset (or set to anything other than a 16-digit all-decimal
 *     string) to run in "reader present, no card inserted" mode (v2 behavior).
 * =============================================================================
 */
#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

#include "board_io_ops.h"

/* jvs_serial_hook_es3.c -- exported so game_*.c files can land a handle
 * in ITS g_slots table, giving SetCommState/WaitCommEvent/... shims for free
 * without duplicating that plumbing here. */
extern HANDLE open_fake_com(int port);

/* jvs_serial_hook_es3.c's generic IAT patcher -- same one every other per-game
 * file in this module uses (card_reader_es3.c, konami_io_es3.c, ...). Used
 * below for the GetRTTAndHopCount stub, unrelated to COM2/serial at all. */
extern void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* ---------------------------------------------------------------------------
 *  Diagnostic logging -- same ULTRA_JVS_LOG convention as every other file.
 *  Every line is tagged "BANAPASS" for easy grep separation.
 * -------------------------------------------------------------------------- */
static ULONGLONG bp_t0(void) {
    static ULONGLONG t0 = 0;
    if (!t0) t0 = GetTickCount64();
    return t0;
}
static LONG g_bp_seq = 0;

static void bp_log(const char* fmt, ...) {
    /* GetEnvironmentVariableA, not getenv: this file's constructor can run
     * very early in this DLL's load (before the CRT's own getenv() table is
     * necessarily populated), and the one-shot cache below means an early
     * miss would have stuck forever. GetEnvironmentVariableA reads the OS's
     * own PEB environment block, available from process creation onward. */
    static char path[MAX_PATH]; static int checked = 0;
    if (!checked) {
        DWORD n = GetEnvironmentVariableA("ULTRA_JVS_LOG", path, sizeof path);
        if (!(n > 0 && n < sizeof path)) path[0] = 0;
        checked = 1;
    }
    if (!path[0]) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    ULONGLONG dt = GetTickCount64() - bp_t0();
    fprintf(f, "[BANAPASS +%6llu.%03llums] ", dt / 1000ULL, dt % 1000ULL);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}
static void bp_log_hex(const char* tag, const unsigned char* b, size_t n) {
    /* No pre-check here anymore -- bp_log() below does its own correct
     * GetEnvironmentVariableA check; a redundant plain-getenv() pre-check
     * here could skip a working bp_log() call for no reason. */
    long seq = InterlockedIncrement(&g_bp_seq) - 1;
    char line[700]; size_t o = 0;
    o += (size_t)snprintf(line, sizeof(line), "#%ld %s [%zu bytes]:", seq, tag, n);
    for (size_t i = 0; i < n && i < 128 && o + 4 < sizeof(line); ++i)
        o += (size_t)snprintf(line + o, sizeof(line) - o, " %02X", b[i]);
    if (n > 128) o += (size_t)snprintf(line + o, sizeof(line) - o, " ...(+%zu more)", n - 128);
    bp_log("%s", line);
}

/* ---------------------------------------------------------------------------
 *  Feature gates.
 *  Use GetEnvironmentVariableA, not getenv: this DLL is injected into an
 *  already-running process; the CRT's getenv snapshot predates the loader's
 *  SetEnvironmentVariableA calls (same trap already found in jvs_core_es3.c).
 * -------------------------------------------------------------------------- */
static int bp_enabled(void) {
    char eb[8];
    DWORD en = GetEnvironmentVariableA("ULTRA_BANANAPASS_VENDOR", eb, sizeof eb);
    return (en > 0 && en < sizeof eb && eb[0] == '1');
}

/* Returns the 16-digit card number from ULTRA_BANANAPASS_CARD, or 0 if unset
 * or not exactly 16 decimal digits. */
static unsigned long long bp_card_number(void) {
    char cb[24];
    DWORD cn = GetEnvironmentVariableA("ULTRA_BANANAPASS_CARD", cb, sizeof cb);
    if (cn != 16) return 0;
    for (DWORD i = 0; i < 16; ++i) if (!isdigit((unsigned char)cb[i])) return 0;
    return (unsigned long long)_atoi64(cb);
}

/* ---------------------------------------------------------------------------
 *  COM2 handle table + output queue.
 *  Private to this file; jvs_serial_hook_es3.c's g_slots table ALSO tracks
 *  these handles for the generic comm-config shims.
 * -------------------------------------------------------------------------- */
#define BP_MAX_HANDLES 4
#define BP_OUTQ_CAP    512   /* enough for one full PN533 frame + ACK */

static HANDLE        g_bp_handles[BP_MAX_HANDLES];
static unsigned char g_bp_outq[BP_MAX_HANDLES][BP_OUTQ_CAP];
static int           g_bp_outq_len[BP_MAX_HANDLES];
static int           g_bp_outq_pos[BP_MAX_HANDLES];

static int bp_slot_of(HANDLE h) {
    for (int i = 0; i < BP_MAX_HANDLES; ++i)
        if (g_bp_handles[i] == h) return i;
    return -1;
}
static int  bp_is_ours(HANDLE h) { return bp_slot_of(h) >= 0; }
static void bp_remember(HANDLE h) {
    for (int i = 0; i < BP_MAX_HANDLES; ++i)
        if (!g_bp_handles[i]) { g_bp_handles[i] = h; g_bp_outq_len[i] = 0; g_bp_outq_pos[i] = 0; return; }
    g_bp_handles[0] = h; g_bp_outq_len[0] = 0; g_bp_outq_pos[0] = 0;
}
static void bp_forget(HANDLE h) {
    for (int i = 0; i < BP_MAX_HANDLES; ++i)
        if (g_bp_handles[i] == h) { g_bp_handles[i] = NULL; g_bp_outq_len[i] = 0; g_bp_outq_pos[i] = 0; return; }
}

/* ---------------------------------------------------------------------------
 *  PN533 frame building helpers
 * -------------------------------------------------------------------------- */
static void bp_queue(int slot, const unsigned char* b, int n) {
    int room = BP_OUTQ_CAP - g_bp_outq_len[slot];
    if (n > room) n = room;
    memcpy(g_bp_outq[slot] + g_bp_outq_len[slot], b, (size_t)n);
    g_bp_outq_len[slot] += n;
}
static void bp_queue_ack(int slot) {
    static const unsigned char ack[6] = { 0x00,0x00,0xFF,0x00,0xFF,0x00 };
    bp_queue(slot, ack, 6);
}

/* Build and queue a PN533 response frame: TFI=0xD5, resp_cc, then data[dlen]. */
static void bp_queue_response(int slot, unsigned char resp_cc,
                               const unsigned char* data, int dlen) {
    unsigned char f[6 + 2 + 256];
    int len = 2 + dlen;
    if (len > 255) len = 255;
    int o = 0;
    f[o++] = 0x00; f[o++] = 0x00; f[o++] = 0xFF;
    f[o++] = (unsigned char)len;
    f[o++] = (unsigned char)((0x100 - len) & 0xFF);
    f[o++] = 0xD5;
    f[o++] = resp_cc;
    unsigned int sum = 0xD5 + resp_cc;
    int n = len - 2; if (n > dlen) n = dlen;
    for (int i = 0; i < n; ++i) { f[o++] = data[i]; sum += data[i]; }
    f[o++] = (unsigned char)((0x100 - (sum & 0xFF)) & 0xFF);
    f[o++] = 0x00;
    bp_queue(slot, f, o);
    bp_log_hex("QUEUED response", f, (size_t)o);
}

/* ---------------------------------------------------------------------------
 *  Persistent card data (Mifare Classic 1K = 64 blocks × 16 bytes = 1024 B).
 *
 *  Stored at %LOCALAPPDATA%\BananapassService\card_data\<card_number>.bin.
 *  The game writes block data via InDataExchange(Mifare Write), we update the
 *  buffer and flush immediately so the virtual card survives across sessions.
 *  On first run the file doesn't exist and all blocks read as zeros -- the game
 *  initialises its own new-player data on first write.
 * -------------------------------------------------------------------------- */
#define BP_MIFARE_BLOCKS 64
#define BP_MIFARE_BLOCK_SIZE 16
#define BP_CARD_DATA_BYTES (BP_MIFARE_BLOCKS * BP_MIFARE_BLOCK_SIZE)   /* 1024 */

static unsigned char s_card_data[BP_MIFARE_BLOCKS][BP_MIFARE_BLOCK_SIZE];
static int           s_card_loaded = 0;

static void bp_card_data_path(unsigned long long card_num, char *out, size_t cap) {
    char lad[MAX_PATH];
    DWORD ln = GetEnvironmentVariableA("LOCALAPPDATA", lad, sizeof lad);
    if (ln == 0 || ln >= sizeof lad) { out[0] = 0; return; }
    snprintf(out, cap, "%s\\BananapassService\\card_data\\%llu.bin", lad,
             (unsigned long long)card_num);
}

static void bp_mkdirs_for_file(const char *path) {
    char buf[MAX_PATH]; strncpy(buf, path, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (char *p = buf + 1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char save = *p; *p = 0;
            CreateDirectoryA(buf, NULL);
            *p = save;
        }
    }
}

static void bp_load_card_data(unsigned long long card_num) {
    if (s_card_loaded) return;
    memset(s_card_data, 0, sizeof s_card_data);
    s_card_loaded = 1;

    char path[MAX_PATH * 2];
    bp_card_data_path(card_num, path, sizeof path);
    if (!path[0]) { bp_log("card data: could not build path (LOCALAPPDATA unset)"); return; }

    FILE *f = fopen(path, "rb");
    if (!f) {
        bp_log("card data: no saved data at %s -- starting blank (all-zero blocks)", path);
        return;
    }
    size_t n = fread(s_card_data, 1, BP_CARD_DATA_BYTES, f);
    fclose(f);
    bp_log("card data: loaded %lu bytes from %s", (unsigned long)n, path);
}

static void bp_save_card_data(unsigned long long card_num) {
    char path[MAX_PATH * 2];
    bp_card_data_path(card_num, path, sizeof path);
    if (!path[0]) return;
    bp_mkdirs_for_file(path);
    FILE *f = fopen(path, "wb");
    if (!f) { bp_log("card data: could not save to %s", path); return; }
    fwrite(s_card_data, 1, BP_CARD_DATA_BYTES, f);
    fclose(f);
    bp_log("card data: saved to %s", path);
}

/* ---------------------------------------------------------------------------
 *  Card UID derivation.
 *  Takes the 16-digit card number and folds it into a stable 4-byte ISO14443-A
 *  UID. Any deterministic, collision-resistant function works; we use simple
 *  byte folding because the UID only needs to be unique per card number within
 *  a single machine session -- it is never transmitted off-device.
 * -------------------------------------------------------------------------- */
static void bp_card_uid(unsigned long long card_num, unsigned char uid[4]) {
    /* Split into two 32-bit halves, XOR them, then bit-mix to spread. */
    unsigned int lo = (unsigned int)(card_num & 0xFFFFFFFFULL);
    unsigned int hi = (unsigned int)(card_num >> 32);
    unsigned int v  = lo ^ hi;
    /* Finalise: shift-and-add like Jenkins one-at-a-time, one pass. */
    v += (v << 12); v ^= (v >> 22);
    v += (v << 4);  v ^= (v >> 9);
    v += (v << 10); v ^= (v >> 2);
    v += (v << 7);  v ^= (v >> 12);
    uid[0] = (unsigned char)((v >> 24) & 0xFF);
    uid[1] = (unsigned char)((v >> 16) & 0xFF);
    uid[2] = (unsigned char)((v >>  8) & 0xFF);
    uid[3] = (unsigned char)( v        & 0xFF);
    /* Force bit0 of uid[0] = 0: ISO14443-A spec forbids a UID byte 0 with
     * the cascade-tag value (0x88), and the single-size-UID bit must be
     * clear. Clearing LSB of uid[0] costs one bit of uniqueness but avoids
     * a protocol corner case. */
    uid[0] &= 0xFE;
}

/* ---------------------------------------------------------------------------
 *  Command handlers
 * -------------------------------------------------------------------------- */

/* InListPassiveTarget (CC=0x4A): the game's card poll.
 * When ULTRA_BANANAPASS_CARD is set, return one ISO14443-A (Mifare Classic 1K)
 * target. Without it, return 0 targets (no card present). */
static void bp_handle_in_list_passive_target(int slot,
                                              const unsigned char* pd, int pdlen) {
    unsigned long long card_num = bp_card_number();
    if (!card_num) {
        /* No card: confirmed via bana.c as a fixed 3-byte all-zero reply
         * (card_present=0, num=0, atqa[0]=0), not just a 1-byte NbTg=0. */
        static const unsigned char no_card[3] = { 0x00, 0x00, 0x00 };
        bp_queue_response(slot, 0x4B, no_card, 3);
        bp_log("InListPassiveTarget: no card configured (ULTRA_BANANAPASS_CARD unset)");
        return;
    }

    /* BrTy (baud/modulation) byte is pd[1] when it's present; we respond the
     * same regardless -- the frame format doesn't change between 106/212/424kbps
     * for an emulated Mifare Classic. Log it anyway for the capture record. */
    if (pdlen >= 2)
        bp_log("InListPassiveTarget BrTy=0x%02X MaxTg=0x%02X", pd[1], pd[0]);

    unsigned char uid[4];
    bp_card_uid(card_num, uid);

    /* Load persistent block data on first card-present so reads are ready. */
    bp_load_card_data(card_num);

    /* ISO14443-A Mifare Classic 1K response (PN533 spec Table 136):
     *   NbTg     = 0x01
     *   Tg       = 0x01
     *   ATQA[0]  = 0x00, ATQA[1] = 0x04   (1K)
     *   SAK      = 0x08                    (ISO14443-3 compliant, not DESFire)
     *   NfcIdLen = 0x04
     *   NfcId[0..3] = uid                  */
    unsigned char resp[10];
    resp[0] = 0x01;         /* NbTg  */
    resp[1] = 0x01;         /* Tg    */
    resp[2] = 0x00;         /* ATQA[0] */
    resp[3] = 0x04;         /* ATQA[1] */
    resp[4] = 0x08;         /* SAK   */
    resp[5] = 0x04;         /* NfcIdLen */
    resp[6] = uid[0]; resp[7] = uid[1]; resp[8] = uid[2]; resp[9] = uid[3];
    bp_queue_response(slot, 0x4B, resp, 10);
    bp_log("InListPassiveTarget: card present, UID %02X %02X %02X %02X "
           "(card_num=%I64u)", uid[0], uid[1], uid[2], uid[3], card_num);
}

/* InDataExchange (CC=0x40): wraps raw Mifare / ISO14443 commands.
 * Data layout: pd[0]=Tg, pd[1..]=raw Mifare command bytes.
 * Response layout: ERR byte (0x00=ok) + optional data bytes.
 *
 * Confirmed: auth always succeeds (game can reach read); read returns
 * 16 zeroed bytes (placeholder -- card data layout not yet confirmed;
 * every unconfirmed read is logged so a live run captures the format).
 * Write: pretend success, log the data written. */
static void bp_handle_in_data_exchange(int slot,
                                        const unsigned char* pd, int pdlen) {
    /* pd[0] = Tg (always 0x01 for our single target),
     * pd[1] = Mifare command byte */
    if (pdlen < 2) {
        bp_log("InDataExchange: too short (pdlen=%d) -- generic ack", pdlen);
        static const unsigned char err_ok[1] = { 0x00 };
        bp_queue_response(slot, 0x41, err_ok, 1);
        return;
    }
    unsigned char mifare_cmd = pd[1];

    switch (mifare_cmd) {
        case 0x60:   /* MIFARE Authenticate KeyA */
        case 0x61: { /* MIFARE Authenticate KeyB */
            /* Pretend auth always succeeds. Block number is pd[2]; key is pd[3..8]. */
            unsigned char block = (pdlen >= 3) ? pd[2] : 0;
            bp_log("InDataExchange: Mifare Auth Key%c block=%u -- reporting success",
                   (mifare_cmd == 0x60) ? 'A' : 'B', block);
            static const unsigned char ok[1] = { 0x00 };
            bp_queue_response(slot, 0x41, ok, 1);
            break;
        }
        case 0x30: { /* MIFARE Read (16 bytes from block) */
            unsigned char block = (pdlen >= 3) ? pd[2] : 0;
            unsigned char read_resp[17];
            read_resp[0] = 0x00;   /* ERR = success */
            if (block < BP_MIFARE_BLOCKS)
                memcpy(read_resp + 1, s_card_data[block], BP_MIFARE_BLOCK_SIZE);
            else
                memset(read_resp + 1, 0, BP_MIFARE_BLOCK_SIZE);
            bp_log("InDataExchange: Mifare Read block=%u -> %02X %02X %02X %02X ...",
                   block, read_resp[1], read_resp[2], read_resp[3], read_resp[4]);
            bp_queue_response(slot, 0x41, read_resp, 17);
            break;
        }
        case 0xA0: { /* MIFARE Write (16 bytes to block) */
            /* pd: [0]=Tg [1]=0xA0 [2]=block [3..18]=16 data bytes */
            unsigned char block = (pdlen >= 3) ? pd[2] : 0;
            if (block < BP_MIFARE_BLOCKS && pdlen >= 19) {
                memcpy(s_card_data[block], pd + 3, BP_MIFARE_BLOCK_SIZE);
                bp_save_card_data(bp_card_number());
                bp_log("InDataExchange: Mifare Write block=%u -- saved", block);
            } else {
                bp_log("InDataExchange: Mifare Write block=%u pdlen=%d -- "
                       "too short or out of range, ignoring", block, pdlen);
            }
            static const unsigned char wok[1] = { 0x00 };
            bp_queue_response(slot, 0x41, wok, 1);
            break;
        }
        case 0xC0: case 0xC1: case 0xC2: case 0xB0: {
            /* MIFARE Decrement / Increment / Restore / Transfer -- value-block ops.
             * Acknowledge success; actual value-block arithmetic is not yet
             * implemented (confirm with a capture whether MK uses these). */
            bp_log("InDataExchange: Mifare value-block cmd 0x%02X -- ack only "
                   "(not yet implemented; log this block if the game behaves oddly)",
                   mifare_cmd);
            static const unsigned char vok[1] = { 0x00 };
            bp_queue_response(slot, 0x41, vok, 1);
            break;
        }
        default:
            bp_log("InDataExchange: unhandled Mifare command 0x%02X pdlen=%d -- "
                   "generic ok reply", mifare_cmd, pdlen);
            static const unsigned char dok[1] = { 0x00 };
            bp_queue_response(slot, 0x41, dok, 1);
            break;
    }
}

/* Parse one WriteFile worth of bytes (one complete PN533 unit) and queue the
 * appropriate reply. Called by mkdx_on_write_file. */
static void bp_respond(int slot, const unsigned char* b, DWORD len) {
    if (len == 1 && b[0] == 0x55) {
        bp_log("wake-up byte (0x55) -- no reply");
        return;
    }
    if (len == 6 && b[0]==0x00 && b[1]==0x00 && b[2]==0xFF &&
                    b[3]==0x00 && b[4]==0xFF && b[5]==0x00) {
        bp_log("host ACK -- no reply");
        return;
    }
    if (len >= 7 && b[0]==0x00 && b[1]==0x00 && b[2]==0xFF) {
        unsigned char frame_len = b[3];
        unsigned char lcs       = b[4];
        if ((unsigned char)((frame_len + lcs) & 0xFF) != 0) {
            bp_log("bad length checksum (LEN=%u LCS=%u) -- ignored", frame_len, lcs);
            return;
        }
        if (b[5] != 0xD4) {
            bp_log("TFI=0x%02X is not host-direction (0xD4) -- ignored", b[5]);
            return;
        }
        if ((DWORD)(6 + (frame_len - 1) + 2) > len) {
            bp_log("frame claims LEN=%u but write was only %lu bytes -- ignored",
                   frame_len, (unsigned long)len);
            return;
        }
        unsigned char cc = b[6];
        /* pd = payload bytes after CC (b[7..]) */
        const unsigned char* pd = b + 6;   /* pd[0]=CC, pd[1..]=params */
        int pdlen = (int)frame_len;        /* TFI + CC + params */

        /* ACK every well-formed command first, then queue the response. */
        bp_queue_ack(slot);

        switch (cc) {
            case 0x4A:     /* InListPassiveTarget (poll for card) */
                bp_handle_in_list_passive_target(slot, pd, pdlen);
                break;
            case 0x40:     /* InDataExchange */
                bp_handle_in_data_exchange(slot, pd + 1, pdlen - 1);
                break;
            case 0x0E:     /* WriteGPIO -- confirmed needed by MK_AGP3_FINAL.exe */
            case 0x10:     /* SetSerialBaudRate */
            case 0x12:     /* SetParameters */
            case 0x14:     /* SAMConfiguration */
            case 0x16:     /* PowerDown */
            case 0x18:     /* RFRegulationTest */
            case 0x32:     /* RFConfiguration */
                /* Confirmed by real capture: MK_AGP3_FINAL.exe needs a real
                 * no-data reply to proceed past every one of these, even
                 * where the aic_pico/bana.c reference below has no case at
                 * all for some of them (e.g. 0x10) -- a reference's silence
                 * on a command means the real hardware it was tested against
                 * was never asked it, not that no game ever sends it. */
                bp_queue_response(slot, (unsigned char)(cc + 1), NULL, 0);
                break;
            case 0x08: {
                /* RF field off. NOT PN533's WriteRegister (this file's
                 * earlier guess) -- confirmed via the real, community-tested
                 * github.com/whowechina/aic_pico (bana.c): the reply MUST
                 * carry exactly one data byte (0x00), not zero. Every real
                 * capture of this game before this fix went silent forever
                 * right after this exact command -- a zero-length reply
                 * where a 1-byte reply was expected is exactly why. */
                static const unsigned char d[1] = { 0x00 };
                bp_queue_response(slot, (unsigned char)(cc + 1), d, 1);
                break;
            }
            case 0x06: {
                /* Confirmed via bana.c: two fixed constant byte blobs keyed
                 * on the second parameter byte (our pd[2]) -- NOT a
                 * per-address ReadRegister (this file's earlier guess). */
                static const unsigned char d_1c[8]  = { 0xFF,0x3F,0x0E,0xF1,0xFF,0x3F,0x0E,0xF1 };
                static const unsigned char d_oth[11]= { 0xDC,0xF4,0x3F,0x11,0x4D,0x85,0x61,0xF1,0x26,0x6A,0x87 };
                if (pdlen >= 3 && pd[2] == 0x1C)
                    bp_queue_response(slot, (unsigned char)(cc + 1), d_1c, 8);
                else
                    bp_queue_response(slot, (unsigned char)(cc + 1), d_oth, 11);
                break;
            }
            case 0x0C: {   /* Confirmed via bana.c: fixed 3-byte reply. */
                static const unsigned char d[3] = { 0x00, 0x06, 0x00 };
                bp_queue_response(slot, (unsigned char)(cc + 1), d, 3);
                break;
            }
            case 0x42: {   /* CommThru. Confirmed via bana.c. */
                static const unsigned char d[1] = { 0x01 };
                bp_queue_response(slot, (unsigned char)(cc + 1), d, 1);
                break;
            }
            case 0x44:     /* Deselect. */
            case 0x52: {   /* Release. Confirmed via bana.c: same 2-byte reply. */
                static const unsigned char d[2] = { 0x01, 0x00 };
                bp_queue_response(slot, (unsigned char)(cc + 1), d, 2);
                break;
            }
            case 0x54: {   /* Select. Confirmed via bana.c. */
                static const unsigned char d[1] = { 0x00 };
                bp_queue_response(slot, (unsigned char)(cc + 1), d, 1);
                break;
            }
            default:
                /* Confirmed via bana.c: a genuinely unrecognized command
                 * gets ONLY the bare ACK already queued above, never a
                 * fabricated response frame. This file used to fabricate a
                 * zero-length response here (and for 0x02/0x04 specifically,
                 * a guessed PN533-textbook reply) -- bana.c's own command
                 * table has no case for 0x02/0x04 at all, meaning real BANA
                 * hardware doesn't answer them specially either, so both now
                 * fall here instead of shipping an unconfirmed guess. */
                bp_log("command 0x%02X: no confirmed reply -- ACK only "
                       "(matches aic_pico/bana.c's own default); check "
                       "capture for what the game does next", cc);
                break;
        }
        return;
    }
    bp_log("unrecognized byte sequence on COM2 -- no reply queued");
}

/* ---------------------------------------------------------------------------
 *  Parse COM port number from a CreateFile path
 * -------------------------------------------------------------------------- */
static int bp_com_number(const char* p) {
    if (!p) return -1;
    const char* s = p;
    if (s[0]=='\\' && s[1]=='\\' && s[2]=='.' && s[3]=='\\') s += 4;
    if ((s[0]=='C'||s[0]=='c') && (s[1]=='O'||s[1]=='o') && (s[2]=='M'||s[2]=='m')
        && isdigit((unsigned char)s[3])) {
        char* end = NULL;
        long n = strtol(s + 3, &end, 10);
        if (end && (*end == 0 || *end == ':')) return (int)n;
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 *  board_io_ops callbacks
 * -------------------------------------------------------------------------- */
static int mkdx_on_create_file(const char* path, HANDLE* out_handle) {
    if (!bp_enabled()) return 0;
    if (bp_com_number(path) != 2) return 0;

    HANDLE h = open_fake_com(2);
    if (h == INVALID_HANDLE_VALUE) {
        bp_log("open_fake_com(2) FAILED for '%s' -- deferring", path);
        return 0;
    }
    bp_remember(h);
    bp_log("OPEN COM2 (%s) -> handle %p (PN533 responder armed)", path, (void*)h);
    *out_handle = h;
    return 1;
}

static int mkdx_on_write_file(HANDLE h, const void* buf, DWORD len) {
    int slot = bp_slot_of(h);
    if (!bp_enabled() || slot < 0) return 0;
    bp_log_hex("TX COM2", (const unsigned char*)buf, len);
    bp_respond(slot, (const unsigned char*)buf, len);
    return 1;
}

static int mkdx_on_read_file(HANDLE h, void* buf, DWORD want, DWORD* out_read) {
    int slot = bp_slot_of(h);
    if (!bp_enabled() || slot < 0) return 0;
    int avail = g_bp_outq_len[slot] - g_bp_outq_pos[slot];
    DWORD give = (avail > 0) ? (DWORD)avail : 0;
    if (give > want) give = want;
    if (give > 0 && buf) {
        memcpy(buf, g_bp_outq[slot] + g_bp_outq_pos[slot], give);
        g_bp_outq_pos[slot] += (int)give;
        if (g_bp_outq_pos[slot] >= g_bp_outq_len[slot]) {
            g_bp_outq_pos[slot] = 0; g_bp_outq_len[slot] = 0;
        }
    }
    long seq = InterlockedIncrement(&g_bp_seq) - 1;
    bp_log("#%ld RX COM2: asked %lu, giving %lu", seq, (unsigned long)want, (unsigned long)give);
    if (out_read) *out_read = give;
    return 1;
}

static void mkdx_on_close_handle(HANDLE h) {
    if (!bp_is_ours(h)) return;
    bp_forget(h);
    bp_log("CLOSE COM2 (handle %p)", (void*)h);
}

/* ---------------------------------------------------------------------------
 *  "Local network check" self-test row -- confirmed unrelated to COM2/the
 *  card vendor at all. MK_AGP3_FINAL.exe calls iphlpapi.dll's
 *  GetRTTAndHopCount (confirmed present in this exe's import table) to
 *  measure round-trip time/hop count to some local address as part of its
 *  own network self-test; with no real router/LAN master on the wire to
 *  answer that, the real Windows implementation legitimately fails or times
 *  out. Stubbing this exact API is a known fix for games on the same
 *  underlying Namco ES3/MUCHA middleware family; it is applied here for
 *  MKDX specifically. Unconditional (not gated on ULTRA_BANANAPASS_VENDOR): this has
 *  nothing to do with the card vendor and should apply to every MKDX launch. */
static DWORD WINAPI Hook_GetRTTAndHopCount(ULONG DestIpAddress, PULONG HopCount,
                                            ULONG MaxHops, PULONG RTT) {
    if (HopCount) *HopCount = 1;
    if (RTT) *RTT = 1;
    bp_log("GetRTTAndHopCount(dest=0x%08lX, maxhops=%lu) stubbed -> success",
           (unsigned long)DestIpAddress, (unsigned long)MaxHops);
    return TRUE;
}

static void mkdx_on_install(void) {
    ultra_iat_patch_all("iphlpapi.dll", "GetRTTAndHopCount", (void*)Hook_GetRTTAndHopCount);

    if (!bp_enabled()) return;
    unsigned long long card_num = bp_card_number();
    if (card_num) {
        unsigned char uid[4]; bp_card_uid(card_num, uid);
        bp_log("Bananapass card emulator armed: card %llu, UID %02X %02X %02X %02X. "
               "Card data persists in %%LOCALAPPDATA%%\\BananapassService\\card_data\\. "
               "grep '[BANAPASS' on %%ULTRA_JVS_LOG%% to capture block layout.",
               (unsigned long long)card_num, uid[0], uid[1], uid[2], uid[3]);
    } else {
        bp_log("Bananapass PN533 responder armed (ULTRA_BANANAPASS_VENDOR=1, "
               "no ULTRA_BANANAPASS_CARD). Game will initialize normally but see "
               "no card present. Set ULTRA_BANANAPASS_CARD=<16-digit card number> "
               "to emulate a card.");
    }
}

static const board_io_ops s_mkdx_bananapass_ops = {
    .name                       = "Mario Kart Arcade GP DX - Bananapass vendor COM2 (PN533 card emulator)",
    .on_install                 = mkdx_on_install,
    .on_create_file             = mkdx_on_create_file,
    .on_read_file               = mkdx_on_read_file,
    .on_write_file              = mkdx_on_write_file,
    .on_close_handle            = mkdx_on_close_handle,
    .on_wait_comm_event_pending = NULL,
    .on_load_library_redirect   = NULL,
    .autopilot_tick             = NULL,
};

/* ---------------------------------------------------------------------------
 *  Auto-registration
 *  Identity: MK_AGP3_FINAL.exe, exact size 6,819,840 bytes, OR the filename,
 *  OR ULTRA_MKDX_ID=1 escape hatch.
 * -------------------------------------------------------------------------- */
static void __attribute__((constructor)) game_mkdx_bananapass_autoregister(void) {
    int is_mkdx = 0;
    const char* e = getenv("ULTRA_MKDX_ID");
    if (e && e[0] == '1') is_mkdx = 1;
    if (!is_mkdx) {
        char p[MAX_PATH];
        GetModuleFileNameA(NULL, p, MAX_PATH);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA(p, GetFileExInfoStandard, &fa) &&
            fa.nFileSizeHigh == 0 && fa.nFileSizeLow == 6819840)
            is_mkdx = 1;
        if (!is_mkdx) {
            const char* base = strrchr(p, '\\');
            base = base ? base + 1 : p;
            if (_stricmp(base, "MK_AGP3_FINAL.exe") == 0) is_mkdx = 1;
        }
    }
    if (!is_mkdx) return;
    ultra_board_ops_register(&s_mkdx_bananapass_ops);
}

#endif /* _WIN32 */
