/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  jvs_core_it.h  --  Incredible Technologies (Golden Tee PC "nowboard") board's
 *                     OWN clean-room JVS I/O engine (public interface).
 * =============================================================================
 *
 *  This is the incredible_technologies module's fully independent copy of
 *  Ultra Loader's clean-room JAMMA Video Standard (JVS) I/O engine, written
 *  from the published open specification. Per the per-board independence
 *  directive there is NO link-time or source dependency on
 *  modules/native_x86/src/hook/jvs_core.h (or any other board's copy) --
 *  every byte in this file + jvs_core_it.c was (re)written into this board's
 *  own tree. native_x86/src/hook/jvs_core.c was read for research only.
 *
 *  IT's cabinets ("nowboard" I/O, generic JVS) are trackball-driven (Golden
 *  Tee Golf, Silver Strike Bowling, Target Toss Pro): per this module's own
 *  UNTESTED.txt research note, the trackball enumerates as a PS/2 or USB HID
 *  mouse. This engine's ONE deliberate deviation from the shared reference
 *  engine is JVS_CMD_READ_ROTARY (0x23): the shared copy always reports two
 *  zero bytes for rotary channels (never wired to real data). Here the
 *  `rotary[]` field is a genuine free-running 16-bit counter (wraps mod
 *  65536, exactly like a quadrature encoder) that hook_dllmain_it.c's mouse
 *  poll thread advances from the trackball's relative motion -- the host game
 *  computes velocity itself by diffing two consecutive reads, which is the
 *  correct JVS ROTARY semantics.
 * =============================================================================
 */
#ifndef ULTRA_IT_JVS_CORE_H
#define ULTRA_IT_JVS_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- JVS function (feature) limits this node advertises ------------------- */
#define JVS_MAX_PLAYERS   2
#define JVS_MAX_ANALOG    8
#define JVS_MAX_ROTARY    2     /* trackball X, Y */
#define JVS_MAX_COINS     2

/* ---- Input state the hook's mouse/keyboard poll thread pushes in ----------
 *  system  : bit7 TEST, bit6 TILT1, bit5 TILT2, bit4 TILT3            (active high)
 *  player[]: bit15 START  bit14 SERVICE bit13 UP    bit12 DOWN
 *            bit11 LEFT   bit10 RIGHT   bit9  PUSH1 bit8  PUSH2
 *            bit7  PUSH3  bit6  PUSH4   bit5  PUSH5 bit4  PUSH6
 *            bit3  PUSH7  bit2  PUSH8   (low bits reserved)
 *            -> high byte is JVS switch byte 1, low byte is switch byte 2.
 *            PUSH1 = shot/swing trigger (mouse LEFT), PUSH2 = view/club
 *            select (mouse RIGHT) -- the two buttons a Golden Tee cabinet's
 *            trackball housing actually has, per this module's own research.
 *  analog[]: 16-bit unsigned, left-justified (0x8000 = center). Unused by the
 *            trackball itself; kept so a title that also polls READ_ANALOG
 *            (e.g. a pedal/spinner variant cabinet) still sees a centered,
 *            never-hard-over value instead of 0.
 *  rotary[] : free-running 16-bit counters (trackball X, Y). NOT centered --
 *            these wrap like a real quadrature counter; the game diffs two
 *            reads to get velocity, exactly like real trackball hardware.
 *  screen_x/y: unused by this board's known titles; kept centered for any
 *            title that happens to also poll READ_SCRPOS. */
typedef struct ultra_jvs_input {
    uint8_t  system;
    uint16_t player[JVS_MAX_PLAYERS];
    uint16_t analog[JVS_MAX_ANALOG];
    uint16_t rotary[JVS_MAX_ROTARY];
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
                      uint8_t analog_bits, uint8_t rotary_channels,
                      uint8_t gpo_slots, uint8_t has_screen_pos);

/* ---- Live input ---------------------------------------------------------- */
void jvs_set_input(ultra_jvs_node* n, const ultra_jvs_input* in);
void jvs_get_input(const ultra_jvs_node* n, ultra_jvs_input* out);
void jvs_insert_coin(ultra_jvs_node* n, int slot, int count);

/* ---- The transport entry point (fed by jvs_serial_hook_it.c) -------------- */
size_t jvs_host_bytes(ultra_jvs_node* n,
                      const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t out_cap);

/* ---- Introspection --------------------------------------------------------- */
uint8_t  jvs_address(const ultra_jvs_node* n);
uint32_t jvs_coin_count(const ultra_jvs_node* n, int slot);

/* ---- Protocol constants (exposed for the wire transport) ------------------- */
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
#endif /* ULTRA_IT_JVS_CORE_H */
