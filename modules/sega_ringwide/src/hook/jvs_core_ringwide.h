/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  jvs_core_ringwide.h  --  Ultra Loader clean-room JVS I/O engine (Sega RingWide)
 * =============================================================================
 *
 *  This is a CLEAN-ROOM implementation of the JAMMA Video Standard (JVS) I/O
 *  protocol, written from the published open specification. It contains NO code
 *  from any other arcade loader project -- it is Ultra Loader's own
 *  work. It models a single JVS I/O node (the board the arcade game expects to
 *  find on its I/O bus) and answers the host game's command stream with correctly
 *  framed responses, sourced from a plain input struct that this board's hook
 *  fills from the player's controller.
 *
 *  THIS IS THE sega_ringwide BOARD'S OWN, SELF-CONTAINED COPY of Ultra Loader's
 *  JVS engine. It is built ONLY into ultra_sega_ringwide_hook{32,64}.dll and does
 *  not link against, or get linked by, modules/native_x86 or any other board's
 *  hook DLL. (The wire protocol logic mirrors the reference implementation kept
 *  under modules/native_x86/src/hook/jvs_core.c for cross-board consistency --
 *  that file was read as REFERENCE ONLY; nothing here #includes or links it.)
 *
 *  Transport: Sega RingWide (and the RingEdge family it shares a keychip lineage
 *  with) talks JVS over a serial RS-485 link; jvs_serial_hook_ringwide.c
 *  intercepts the game's COM-port reads/writes and pipes the raw bytes through
 *  jvs_host_bytes() below.
 *
 *  The engine is pure logic with no OS dependencies, so it is independently
 *  testable.
 * =============================================================================
 */
#ifndef ULTRA_RINGWIDE_JVS_CORE_H
#define ULTRA_RINGWIDE_JVS_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- JVS function (feature) limits this node advertises ------------------- */
#define JVS_MAX_PLAYERS   2
#define JVS_MAX_ANALOG    16
#define JVS_MAX_COINS     2

/* ---- Input state the hook pushes in; the engine reports it on demand ------
 * Bit layouts follow the JVS switch map so the hook fills them directly.
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
typedef struct ultra_jvs_input {
    uint8_t  system;
    uint16_t player[JVS_MAX_PLAYERS];
    uint16_t analog[JVS_MAX_ANALOG];
    uint16_t screen_x, screen_y;
} ultra_jvs_input;

/* Opaque node. */
typedef struct ultra_jvs_node ultra_jvs_node;

/* ---- Lifecycle ----------------------------------------------------------- */
ultra_jvs_node* jvs_create(void);
void            jvs_destroy(ultra_jvs_node* n);

/* ---- Configuration (call before/while running; all optional) -------------- */
void jvs_set_id(ultra_jvs_node* n, const char* id);
void jvs_set_features(ultra_jvs_node* n,
                      uint8_t players, uint8_t switches_per_player,
                      uint8_t coin_slots, uint8_t analog_channels,
                      uint8_t analog_bits, uint8_t gpo_slots,
                      uint8_t has_screen_pos);

/* ---- Live input ---------------------------------------------------------- */
void jvs_set_input(ultra_jvs_node* n, const ultra_jvs_input* in);
void jvs_get_input(const ultra_jvs_node* n, ultra_jvs_input* out);
void jvs_insert_coin(ultra_jvs_node* n, int slot, int count);

/* ---- The transport entry point ------------------------------------------- */
size_t jvs_host_bytes(ultra_jvs_node* n,
                      const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t out_cap);

/* ---- Introspection --------------------------------------------------------- */
uint8_t  jvs_address(const ultra_jvs_node* n);
uint32_t jvs_coin_count(const ultra_jvs_node* n, int slot);

/* ---- Protocol constants ---------------------------------------------------- */
#define JVS_SYNC        0xE0
#define JVS_ESCAPE      0xD0
#define JVS_BROADCAST   0xFF
#define JVS_HOST_ADDR   0x00

#define JVS_CMD_RESET        0xF0
#define JVS_CMD_SET_ADDR     0xF1
#define JVS_CMD_SET_COMMS    0xF2
#define JVS_CMD_IOIDENT      0x10
#define JVS_CMD_CMD_REV      0x11
#define JVS_CMD_JVS_REV      0x12
#define JVS_CMD_COMM_VER     0x13
#define JVS_CMD_FEATCHK      0x14
#define JVS_CMD_MAIN_ID      0x15
#define JVS_CMD_READ_SWITCH  0x20
#define JVS_CMD_READ_COIN    0x21
#define JVS_CMD_READ_ANALOG  0x22
#define JVS_CMD_READ_ROTARY  0x23
#define JVS_CMD_READ_KEYCODE 0x24
#define JVS_CMD_READ_SCRPOS  0x25
#define JVS_CMD_READ_MISC    0x26
#define JVS_CMD_RETRANSMIT   0x2F
#define JVS_CMD_COIN_DEC     0x30
#define JVS_CMD_GPO          0x32
#define JVS_CMD_ANALOG_OUT   0x33
#define JVS_CMD_CHAR_OUT     0x34
#define JVS_CMD_COIN_INC     0x35
#define JVS_CMD_GPO_BYTE     0x38

#define JVS_STATUS_NORMAL    0x01
#define JVS_STATUS_UNKNOWN   0x02
#define JVS_STATUS_SUM_ERR   0x03
#define JVS_STATUS_OVERFLOW  0x04
#define JVS_REPORT_NORMAL    0x01
#define JVS_REPORT_PARAM_ERR 0x02
#define JVS_REPORT_DATA_ERR  0x03
#define JVS_REPORT_BUSY      0x04

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* ULTRA_RINGWIDE_JVS_CORE_H */
