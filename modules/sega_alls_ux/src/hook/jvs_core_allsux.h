/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  jvs_core_allsux.h  --  Sega ALLS UX board's OWN clean-room JVS/cabinet-input
 *                         engine (public interface).
 * =============================================================================
 *
 *  This is the sega_alls_ux module's independent copy of Ultra Loader's
 *  clean-room JAMMA Video Standard (JVS) I/O engine. It is written from the
 *  published open specification -- no third-party loader code, and (per the
 *  per-board independence directive) NO link-time or source dependency on
 *  modules/native_x86/src/hook/jvs_core.h. It is fully self-contained inside
 *  this board's own hook DLL.
 *
 *  The struct below (ultra_jvs_input) is the single shared "what the cabinet's
 *  controls currently read" block every subsystem in this board's hook uses:
 *  sega_io4_allsux.c re-packs it into the Sega IO4-USB report the ALLS titles
 *  actually poll, and dinput8_emu_allsux.c reports the same state through a
 *  synthetic DirectInput8 device for titles that read input that way instead.
 *
 *  The wire-level JVS node (jvs_create/jvs_host_bytes) is kept here too even
 *  though ALLS's primary transport is the IO4-USB report (not JVS-over-serial):
 *  it costs nothing to carry, is independently unit-provable, and covers a
 *  title that tunnels genuine JVS packets over a byte-stream transport instead
 *  of the flat 64-byte IO4 report.
 * =============================================================================
 */
#ifndef ULTRA_ALLSUX_JVS_CORE_H
#define ULTRA_ALLSUX_JVS_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- JVS function (feature) limits this node advertises ------------------- */
#define JVS_MAX_PLAYERS   2
/* Sized generously (16) so every channel an ALLS IO4 ident string can advertise
 * (8 ADIN + 4 ROTIN + spares) gets a real, centered value -- a channel read as
 * 0 would look like a hard-over deflection on an analog axis. */
#define JVS_MAX_ANALOG    16
#define JVS_MAX_COINS     2

/* ---- Input state the loader/gamepad thread pushes in ---------------------
 *  system  : bit7 TEST, bit6 TILT1, bit5 TILT2, bit4 TILT3            (active high)
 *  player[]: bit15 START  bit14 SERVICE bit13 UP    bit12 DOWN
 *            bit11 LEFT   bit10 RIGHT   bit9  PUSH1 bit8  PUSH2
 *            bit7  PUSH3  bit6  PUSH4   bit5  PUSH5 bit4  PUSH6
 *            bit3  PUSH7  bit2  PUSH8   (low bits reserved)
 *            -> high byte is JVS switch byte 1, low byte is switch byte 2.
 *  analog[]: 16-bit unsigned, left-justified (0x8000 = center).
 *  screen_x/y: light-gun/touch position, 16-bit, same convention (ALLS UX
 *              wires a lightgun bridge on gun cabinets per sega_board.c). */
typedef struct ultra_jvs_input {
    uint8_t  system;
    uint16_t player[JVS_MAX_PLAYERS];
    uint16_t analog[JVS_MAX_ANALOG];
    uint16_t screen_x, screen_y;
} ultra_jvs_input;

/* Opaque node (only used by the optional wire-level JVS path). */
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

/* ---- The transport entry point (optional wire-level JVS path) ------------- */
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
#endif /* ULTRA_ALLSUX_JVS_CORE_H */
