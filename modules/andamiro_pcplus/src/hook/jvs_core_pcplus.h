/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  jvs_core_pcplus.h  --  andamiro_pcplus module's OWN clean-room JVS I/O
 *                         engine (public interface)
 * =============================================================================
 *
 *  This is a CLEAN-ROOM implementation of the JAMMA Video Standard (JVS) I/O
 *  protocol, written from the published open specification. It contains NO
 *  code from any other arcade loader project -- it is Ultra
 *  Loader's own work. It models a single JVS I/O node (the board the game
 *  expects to find on its I/O bus) and answers the host game's command stream
 *  with correctly framed responses, sourced from a plain input struct the
 *  loader fills from the player's controller (or the dance-pad panels, once a
 *  real cabinet capture confirms the exact panel-to-switch mapping).
 *
 *  Pump It Up on Andamiro PC+ hardware has never been captured on real
 *  hardware by this project (see UNTESTED.txt / STOCK_TAKE.md: framework-only,
 *  no dump), so per the standing instruction this engine treats the cabinet's
 *  I/O as GENERIC JVS switches/pads: 2 players, the standard 13-switch JVS
 *  per-player field (UP/DOWN/LEFT/RIGHT + 8 push buttons + START/SERVICE),
 *  2 coin slots. A dance pad's panels (Pump It Up uses a 5-panel "X" layout:
 *  up-left/up-right/center/down-left/down-right, unlike DDR's 4-way cross)
 *  land on these same generic switch bits -- exactly which physical panel
 *  maps to which bit is a controls/config concern for whoever wires up the
 *  physical pad, not a protocol decision this engine needs to make.
 *
 *  Transport: jvs_serial_hook_pcplus.c intercepts the game's COM-port
 *  reads/writes (JVS-over-serial, the common wire transport on PC-class
 *  arcade I/O boards) and pipes the raw bytes through jvs_pcplus_host_bytes().
 *
 *  The engine is pure logic with no OS dependencies.
 *
 *  This file is the andamiro_pcplus module's OWN independent copy of the JVS
 *  engine (native_x86/src/hook/jvs_core.c was read only as reference, per the
 *  project directive that every board gets its own fully independent hook --
 *  no shared runtime binary, no dependency on native_x86's compiled output).
 * =============================================================================
 */
#ifndef ULTRA_PCPLUS_JVS_CORE_H
#define ULTRA_PCPLUS_JVS_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- JVS function (feature) limits this node advertises ------------------- */
#define JVS_MAX_PLAYERS   2
#define JVS_MAX_ANALOG    16
#define JVS_MAX_COINS     2

/* ---- Input state the loader pushes in; the engine reports it on demand ----
 * Bit layouts follow the JVS switch map so the loader fills them directly.
 *
 *  system  : bit7 TEST, bit6 TILT1, bit5 TILT2, bit4 TILT3            (active high)
 *  player[]: bit15 START  bit14 SERVICE bit13 UP    bit12 DOWN
 *            bit11 LEFT   bit10 RIGHT   bit9  PUSH1 bit8  PUSH2
 *            bit7  PUSH3  bit6  PUSH4   bit5  PUSH5 bit4  PUSH6
 *            bit3  PUSH7  bit2  PUSH8   (low bits reserved)
 *            -> high byte is "switch byte 1", low byte is "switch byte 2",
 *               which is exactly the JVS READ_SWITCH per-player wire order.
 *  analog[]: 16-bit unsigned, left-justified (0x8000 = center). Not used by a
 *            dance pad, but harmless to advertise/answer.
 *  screen_x/y: light-gun position (unused by this board; kept for engine
 *            symmetry with every other Ultra Loader JVS node).
 */
typedef struct ultra_pcplus_jvs_input {
    uint8_t  system;
    uint16_t player[JVS_MAX_PLAYERS];
    uint16_t analog[JVS_MAX_ANALOG];
    uint16_t screen_x, screen_y;
} ultra_pcplus_jvs_input;

/* Opaque node. */
typedef struct ultra_pcplus_jvs_node ultra_pcplus_jvs_node;

/* ---- Lifecycle ----------------------------------------------------------- */
ultra_pcplus_jvs_node* jvs_pcplus_create(void);
void                   jvs_pcplus_destroy(ultra_pcplus_jvs_node* n);

/* ---- Configuration (call before/while running; all optional) -------------- */
/* The node identity string returned by IOIDENT (0x10). Some games match on it.
 * Defaults to a widely-compatible generic I/O id. NULL/empty restores default. */
void jvs_pcplus_set_id(ultra_pcplus_jvs_node* n, const char* id);
/* Advertised feature counts (defaults: 2 players x 13 switches, 2 coins,
 * 8 analog channels @16-bit, 2 GPO). Pass 0 on any field to keep its default. */
void jvs_pcplus_set_features(ultra_pcplus_jvs_node* n,
                      uint8_t players, uint8_t switches_per_player,
                      uint8_t coin_slots, uint8_t analog_channels,
                      uint8_t analog_bits, uint8_t gpo_slots,
                      uint8_t has_screen_pos);

/* ---- Live input ---------------------------------------------------------- */
void jvs_pcplus_set_input(ultra_pcplus_jvs_node* n, const ultra_pcplus_jvs_input* in);
void jvs_pcplus_get_input(const ultra_pcplus_jvs_node* n, ultra_pcplus_jvs_input* out);
/* Register a physical coin insert on a slot (increments the coin counter the
 * game reads with READ_COIN; the game later debits it with COIN_DECREASE). */
void jvs_pcplus_insert_coin(ultra_pcplus_jvs_node* n, int slot, int count);

/* ---- The transport entry point ------------------------------------------- *
 * Feed raw host->node bytes (a partial or whole stream; the engine buffers
 * until a full packet is present). Any node->host response bytes (already
 * framed + escaped, ready to hand back to the game) are written to `out` up to
 * `out_cap`. Returns the number of response bytes produced (0 if the stream so
 * far holds no complete, addressed-to-us packet that warrants a reply). */
size_t jvs_pcplus_host_bytes(ultra_pcplus_jvs_node* n,
                      const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t out_cap);

/* ---- Introspection (for the loader UI / diagnostics) --------------------- */
uint8_t  jvs_pcplus_address(const ultra_pcplus_jvs_node* n);   /* assigned addr, 0 = unassigned */
uint32_t jvs_pcplus_coin_count(const ultra_pcplus_jvs_node* n, int slot);

/* ---- Protocol constants (exposed for the transport) ------------------------ */
#define JVS_SYNC        0xE0
#define JVS_ESCAPE      0xD0
#define JVS_BROADCAST   0xFF
#define JVS_HOST_ADDR   0x00

/* Broadcast / setup commands */
#define JVS_CMD_RESET        0xF0   /* + 0xD9 */
#define JVS_CMD_SET_ADDR     0xF1   /* + addr */
#define JVS_CMD_SET_COMMS    0xF2
/* Identification commands */
#define JVS_CMD_IOIDENT      0x10
#define JVS_CMD_CMD_REV      0x11
#define JVS_CMD_JVS_REV      0x12
#define JVS_CMD_COMM_VER     0x13
#define JVS_CMD_FEATCHK      0x14
#define JVS_CMD_MAIN_ID      0x15
/* Input commands */
#define JVS_CMD_READ_SWITCH  0x20
#define JVS_CMD_READ_COIN    0x21
#define JVS_CMD_READ_ANALOG  0x22
#define JVS_CMD_READ_ROTARY  0x23
#define JVS_CMD_READ_KEYCODE 0x24
#define JVS_CMD_READ_SCRPOS  0x25
#define JVS_CMD_READ_MISC    0x26
/* Output commands */
#define JVS_CMD_RETRANSMIT   0x2F
#define JVS_CMD_COIN_DEC     0x30
#define JVS_CMD_GPO          0x32
#define JVS_CMD_ANALOG_OUT   0x33
#define JVS_CMD_CHAR_OUT     0x34
#define JVS_CMD_COIN_INC     0x35
#define JVS_CMD_GPO_BYTE     0x38

/* Status byte (whole packet) */
#define JVS_STATUS_NORMAL    0x01
#define JVS_STATUS_UNKNOWN   0x02
#define JVS_STATUS_SUM_ERR   0x03
#define JVS_STATUS_OVERFLOW  0x04
/* Report byte (per command) */
#define JVS_REPORT_NORMAL    0x01
#define JVS_REPORT_PARAM_ERR 0x02
#define JVS_REPORT_DATA_ERR  0x03
#define JVS_REPORT_BUSY      0x04

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* ULTRA_PCPLUS_JVS_CORE_H */
