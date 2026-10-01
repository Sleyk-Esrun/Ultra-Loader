/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  jvs_core_unico.h -- Ultra Loader clean-room JVS I/O engine, UNICO (Korea) PC
 *                      board's OWN independent copy (public interface).
 * =============================================================================
 *
 *  This is a CLEAN-ROOM implementation of the JAMMA Video Standard (JVS) I/O
 *  protocol, written from the published open specification. It contains no
 *  third-party loader code. It is a board-pinned copy that belongs ONLY to
 *  modules/unico_pc/src/hook/ -- it is not #included, linked, or shared at
 *  build or run time with modules/native_x86/src/hook/'s jvs_core.c/.h (that
 *  file was read as a reference only, to see a working, protocol-correct
 *  implementation; the logic below is this board's own compiled copy, per the
 *  project directive that every board get a fully independent, self-contained
 *  hook DLL with no dependency on native_x86/src/hook/'s compiled output).
 *
 *  UNICO PC's board profile (board_profile.c: BP_GFX_MIXED, BP_IO_JVS_SERIAL |
 *  BP_IO_ANALOG_WHEEL, BP_PROT_USB_DONGLE) is a Korean PC racing/redemption
 *  board family that talks generic JVS-over-serial for its cabinet I/O
 *  (buttons/coin + analog wheel/pedal channels). This engine answers that
 *  traffic; jvs_serial_hook_unico.c is the transport that feeds it.
 *
 *  The engine is pure logic with no OS dependencies, so it is unit-testable
 *  in isolation from the Win32 hook machinery around it.
 * =============================================================================
 */
#ifndef ULTRA_JVS_CORE_UNICO_H
#define ULTRA_JVS_CORE_UNICO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- JVS function (feature) limits this node advertises ------------------- */
#define JVS_UNICO_MAX_PLAYERS   2
/* UNICO racing/redemption cabinets poll wheel + pedal(s) + spares; size the
 * node's analog array generously so every polled channel gets a real
 * (centered) value -- an unmapped channel reporting 0 would read as full
 * deflection, jamming a wheel hard-over or a pedal fully down. */
#define JVS_UNICO_MAX_ANALOG    16

/* The shared jvs_core code was cloned into every board and the macro was
 * renamed per board - but the pasted bodies kept using the bare name, so
 * this board has not compiled since that rollout. Alias rather than
 * rewrite the call sites, so the board macro stays the single source of
 * truth for the array size. */
#ifndef JVS_MAX_ANALOG
#define JVS_MAX_ANALOG JVS_UNICO_MAX_ANALOG
#endif
#define JVS_UNICO_MAX_COINS     2

/* ---- Input state the loader pushes in; the engine reports it on demand ----
 * Bit layouts follow the JVS switch map so the loader fills them directly.
 *
 *  system  : bit7 TEST, bit6 TILT1, bit5 TILT2, bit4 TILT3            (active high)
 *  player[]: bit15 START  bit14 SERVICE bit13 UP    bit12 DOWN
 *            bit11 LEFT   bit10 RIGHT   bit9  PUSH1 bit8  PUSH2
 *            bit7  PUSH3  bit6  PUSH4   bit5  PUSH5 bit4  PUSH6
 *            bit3  PUSH7  bit2  PUSH8   (low bits reserved)
 *  analog[]: 16-bit unsigned, left-justified (0x8000 = center for a centered
 *            axis; 0x0000 / 0xFFFF = extremes). Channel 0 = wheel, 1 = accel,
 *            2 = brake by this board's own convention (see
 *            hook_dllmain_unico.c's standalone input source).
 *  screen_x/y: light-gun position, kept for public-spec completeness even
 *            though UNICO's catalog is driving/redemption, not gun games.
 */
typedef struct ultra_jvs_input_unico {
    uint8_t  system;
    uint16_t player[JVS_UNICO_MAX_PLAYERS];
    uint16_t analog[JVS_UNICO_MAX_ANALOG];
    uint16_t screen_x, screen_y;
} ultra_jvs_input_unico;

/* Opaque node. */
typedef struct ultra_jvs_node_unico ultra_jvs_node_unico;

/* ---- Lifecycle ----------------------------------------------------------- */
ultra_jvs_node_unico* jvs_unico_create(void);
void                  jvs_unico_destroy(ultra_jvs_node_unico* n);

/* ---- Configuration (call before/while running; all optional) -------------- */
void jvs_unico_set_id(ultra_jvs_node_unico* n, const char* id);
void jvs_unico_set_features(ultra_jvs_node_unico* n,
                            uint8_t players, uint8_t switches_per_player,
                            uint8_t coin_slots, uint8_t analog_channels,
                            uint8_t analog_bits, uint8_t gpo_slots,
                            uint8_t has_screen_pos);

/* ---- Live input ---------------------------------------------------------- */
void jvs_unico_set_input(ultra_jvs_node_unico* n, const ultra_jvs_input_unico* in);
void jvs_unico_get_input(const ultra_jvs_node_unico* n, ultra_jvs_input_unico* out);
void jvs_unico_insert_coin(ultra_jvs_node_unico* n, int slot, int count);

/* ---- The transport entry point ------------------------------------------- *
 * Feed raw host->node bytes (a partial or whole stream; the engine buffers
 * until a full packet is present). Any node->host response bytes (already
 * framed + escaped, ready to hand back to the game) are written to `out` up to
 * `out_cap`. Returns the number of response bytes produced (0 if the stream so
 * far holds no complete, addressed-to-us packet that warrants a reply). */
size_t jvs_unico_host_bytes(ultra_jvs_node_unico* n,
                            const uint8_t* in, size_t in_len,
                            uint8_t* out, size_t out_cap);

/* ---- Introspection --------------------------------------------------------*/
uint8_t  jvs_unico_address(const ultra_jvs_node_unico* n);
uint32_t jvs_unico_coin_count(const ultra_jvs_node_unico* n, int slot);

/* ---- Protocol constants (public JVS spec) ---------------------------------*/
#define JVS_UNICO_SYNC        0xE0
#define JVS_UNICO_ESCAPE      0xD0
#define JVS_UNICO_BROADCAST   0xFF
#define JVS_UNICO_HOST_ADDR   0x00

#define JVS_UNICO_CMD_RESET        0xF0
#define JVS_UNICO_CMD_SET_ADDR     0xF1
#define JVS_UNICO_CMD_SET_COMMS    0xF2
#define JVS_UNICO_CMD_IOIDENT      0x10
#define JVS_UNICO_CMD_CMD_REV      0x11
#define JVS_UNICO_CMD_JVS_REV      0x12
#define JVS_UNICO_CMD_COMM_VER     0x13
#define JVS_UNICO_CMD_FEATCHK      0x14
#define JVS_UNICO_CMD_MAIN_ID      0x15
#define JVS_UNICO_CMD_READ_SWITCH  0x20
#define JVS_UNICO_CMD_READ_COIN    0x21
#define JVS_UNICO_CMD_READ_ANALOG  0x22
#define JVS_UNICO_CMD_READ_ROTARY  0x23
#define JVS_UNICO_CMD_READ_KEYCODE 0x24
#define JVS_UNICO_CMD_READ_SCRPOS  0x25
#define JVS_UNICO_CMD_READ_MISC    0x26
#define JVS_UNICO_CMD_RETRANSMIT   0x2F
#define JVS_UNICO_CMD_COIN_DEC     0x30
#define JVS_UNICO_CMD_GPO          0x32
#define JVS_UNICO_CMD_ANALOG_OUT   0x33
#define JVS_UNICO_CMD_CHAR_OUT     0x34
#define JVS_UNICO_CMD_COIN_INC     0x35
#define JVS_UNICO_CMD_GPO_BYTE     0x38

#define JVS_UNICO_STATUS_NORMAL    0x01
#define JVS_UNICO_STATUS_UNKNOWN   0x02
#define JVS_UNICO_STATUS_SUM_ERR   0x03
#define JVS_UNICO_STATUS_OVERFLOW  0x04
#define JVS_UNICO_REPORT_NORMAL    0x01
#define JVS_UNICO_REPORT_PARAM_ERR 0x02
#define JVS_UNICO_REPORT_DATA_ERR  0x03
#define JVS_UNICO_REPORT_BUSY      0x04

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* ULTRA_JVS_CORE_UNICO_H */
