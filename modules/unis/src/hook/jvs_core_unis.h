/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  jvs_core_unis.h -- Ultra Loader clean-room JVS I/O engine, UNIS Arcade board's
 *                    OWN independent copy (public interface).
 * =============================================================================
 *
 *  This is a CLEAN-ROOM implementation of the JAMMA Video Standard (JVS) I/O
 *  protocol, written from the published open specification. It contains no
 *  third-party loader code. It is a board-pinned copy that belongs ONLY to
 *  modules/unis/src/hook/ -- it is not #included, linked, or shared at
 *  build or run time with modules/native_x86/src/hook/'s jvs_core.c/.h (that
 *  file was read as a reference only, to see a working, protocol-correct
 *  implementation; the logic below is this board's own compiled copy).
 *
 *  UNIS Arcade is a generic-JVS Chinese PC arcade board: no real game dump is on
 *  hand for this board family (see modules/unis/UNTESTED.txt), so this
 *  engine implements the JVS wire protocol straight off the public spec with no
 *  board-specific quirks/RE-derived hacks -- those would need a real capture to
 *  justify. When a real dump surfaces and needs a divergent behavior, it lands
 *  HERE, isolated to this board, never in the shared native_x86 tree.
 *
 *  Two transports can feed this one engine:
 *    - JVS-over-serial (RS-485 on COM2/COM3): jvs_serial_hook_unis.c intercepts
 *      the game's COM-port reads/writes and pipes the raw bytes through
 *      jvs_unis_host_bytes().
 *    - Any other framed transport a future UNIS Arcade dump turns out to need.
 *
 *  The engine is pure logic with no OS dependencies.
 * =============================================================================
 */
#ifndef ULTRA_JVS_CORE_UNIS_H
#define ULTRA_JVS_CORE_UNIS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- JVS function (feature) limits this node advertises ------------------- */
#define JVS_UNIS_MAX_PLAYERS   2
/* Generously sized so any UNIS title's READ_ANALOG poll (wheel/pedals/whatever a
 * future dump turns out to use) always gets a real (centered) value -- a
 * channel reported as 0 reads as full deflection on an analog axis. */
#define JVS_UNIS_MAX_ANALOG    16

/* The shared jvs_core code was cloned into every board and the macro was
 * renamed per board - but the pasted bodies kept using the bare name, so
 * this board has not compiled since that rollout. Alias rather than
 * rewrite the call sites, so the board macro stays the single source of
 * truth for the array size. */
#ifndef JVS_MAX_ANALOG
#define JVS_MAX_ANALOG JVS_UNIS_MAX_ANALOG
#endif
#define JVS_UNIS_MAX_COINS     2

/* ---- Input state the loader/hook pushes in; the engine reports it on demand
 * Bit layouts follow the JVS switch map exactly so the caller fills them
 * directly.
 *
 *  system  : bit7 TEST, bit6 TILT1, bit5 TILT2, bit4 TILT3            (active high)
 *  player[]: bit15 START  bit14 SERVICE bit13 UP    bit12 DOWN
 *            bit11 LEFT   bit10 RIGHT   bit9  PUSH1 bit8  PUSH2
 *            bit7  PUSH3  bit6  PUSH4   bit5  PUSH5 bit4  PUSH6
 *            bit3  PUSH7  bit2  PUSH8   (low bits reserved)
 *            -> high byte is "switch byte 1", low byte is "switch byte 2",
 *               which is exactly the JVS READ_SWITCH per-player wire order.
 *  analog[]: 16-bit unsigned, left-justified (0x8000 = center for a centered
 *            axis; 0x0000 / 0xFFFF = extremes).
 *  screen_x/y: light-gun position, 16-bit, same left-justified convention.
 */
typedef struct ultra_jvs_input_unis {
    uint8_t  system;
    uint16_t player[JVS_UNIS_MAX_PLAYERS];
    uint16_t analog[JVS_UNIS_MAX_ANALOG];
    uint16_t screen_x, screen_y;
} ultra_jvs_input_unis;

/* Opaque node. */
typedef struct ultra_jvs_node_unis ultra_jvs_node_unis;

/* ---- Lifecycle ----------------------------------------------------------- */
ultra_jvs_node_unis* jvs_unis_create(void);
void                 jvs_unis_destroy(ultra_jvs_node_unis* n);

/* ---- Configuration (call before/while running; all optional) -------------- */
/* The node identity string returned by IOIDENT (0x10). Some games match on it.
 * Defaults to a widely-compatible generic I/O id. NULL/empty restores default. */
void jvs_unis_set_id(ultra_jvs_node_unis* n, const char* id);
/* Advertised feature counts (defaults: 2 players x 13 switches, 2 coins,
 * 8 analog channels @16-bit, 2 GPO). Pass 0 on any field to keep its default. */
void jvs_unis_set_features(ultra_jvs_node_unis* n,
                          uint8_t players, uint8_t switches_per_player,
                          uint8_t coin_slots, uint8_t analog_channels,
                          uint8_t analog_bits, uint8_t gpo_slots,
                          uint8_t has_screen_pos);

/* ---- Live input ---------------------------------------------------------- */
void jvs_unis_set_input(ultra_jvs_node_unis* n, const ultra_jvs_input_unis* in);
void jvs_unis_get_input(const ultra_jvs_node_unis* n, ultra_jvs_input_unis* out);
/* Register a physical coin insert on a slot (increments the coin counter the
 * game reads with READ_COIN; the game later debits it with COIN_DECREASE). */
void jvs_unis_insert_coin(ultra_jvs_node_unis* n, int slot, int count);

/* ---- The transport entry point ------------------------------------------- *
 * Feed raw host->node bytes (a partial or whole stream; the engine buffers
 * until a full packet is present). Any node->host response bytes (already
 * framed + escaped, ready to hand back to the game) are written to `out` up to
 * `out_cap`. Returns the number of response bytes produced (0 if the stream so
 * far holds no complete, addressed-to-us packet that warrants a reply). */
size_t jvs_unis_host_bytes(ultra_jvs_node_unis* n,
                          const uint8_t* in, size_t in_len,
                          uint8_t* out, size_t out_cap);

/* ---- Introspection (for diagnostics) -------------------------------------- */
uint8_t  jvs_unis_address(const ultra_jvs_node_unis* n);   /* assigned addr, 0 = unassigned */
uint32_t jvs_unis_coin_count(const ultra_jvs_node_unis* n, int slot);

/* ---- Protocol constants (board-pinned copy of the public JVS spec) -------- */
#define JVS_UNIS_SYNC        0xE0
#define JVS_UNIS_ESCAPE      0xD0
#define JVS_UNIS_BROADCAST   0xFF
#define JVS_UNIS_HOST_ADDR   0x00

/* Broadcast / setup commands */
#define JVS_UNIS_CMD_RESET        0xF0   /* + 0xD9 */
#define JVS_UNIS_CMD_SET_ADDR     0xF1   /* + addr */
#define JVS_UNIS_CMD_SET_COMMS    0xF2
/* Identification commands */
#define JVS_UNIS_CMD_IOIDENT      0x10
#define JVS_UNIS_CMD_CMD_REV      0x11
#define JVS_UNIS_CMD_JVS_REV      0x12
#define JVS_UNIS_CMD_COMM_VER     0x13
#define JVS_UNIS_CMD_FEATCHK      0x14
#define JVS_UNIS_CMD_MAIN_ID      0x15
/* Input commands */
#define JVS_UNIS_CMD_READ_SWITCH  0x20
#define JVS_UNIS_CMD_READ_COIN    0x21
#define JVS_UNIS_CMD_READ_ANALOG  0x22
#define JVS_UNIS_CMD_READ_ROTARY  0x23
#define JVS_UNIS_CMD_READ_KEYCODE 0x24
#define JVS_UNIS_CMD_READ_SCRPOS  0x25
#define JVS_UNIS_CMD_READ_MISC    0x26
/* Output commands */
#define JVS_UNIS_CMD_RETRANSMIT   0x2F
#define JVS_UNIS_CMD_COIN_DEC     0x30
#define JVS_UNIS_CMD_GPO          0x32
#define JVS_UNIS_CMD_ANALOG_OUT   0x33
#define JVS_UNIS_CMD_CHAR_OUT     0x34
#define JVS_UNIS_CMD_COIN_INC     0x35
#define JVS_UNIS_CMD_GPO_BYTE     0x38

/* Status byte (whole packet) */
#define JVS_UNIS_STATUS_NORMAL    0x01
#define JVS_UNIS_STATUS_UNKNOWN   0x02
#define JVS_UNIS_STATUS_SUM_ERR   0x03
#define JVS_UNIS_STATUS_OVERFLOW  0x04
/* Report byte (per command) */
#define JVS_UNIS_REPORT_NORMAL    0x01
#define JVS_UNIS_REPORT_PARAM_ERR 0x02
#define JVS_UNIS_REPORT_DATA_ERR  0x03
#define JVS_UNIS_REPORT_BUSY      0x04

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* ULTRA_JVS_CORE_UNIS_H */
