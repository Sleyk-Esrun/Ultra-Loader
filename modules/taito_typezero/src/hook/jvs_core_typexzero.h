/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  jvs_core_typexzero.h -- Ultra Loader clean-room JVS I/O engine, Taito Type
 *                          Zero's OWN independent copy (public interface)
 * =============================================================================
 *
 *  Everything under modules/taito_typezero/src/hook/ builds into
 *  ultra_taito_typezero_hook32.dll / ultra_taito_typezero_hook64.dll -- a hook
 *  binary dedicated to Taito Type Zero games ONLY. It shares NO source file and
 *  NO compiled binary with the multi-board hook at modules/native_x86/src/hook/
 *  (the "Type X hook", ultra_typex_hook32.dll/64.dll) or with any other board's
 *  module folder. This mirrors the fork already done for Cave PC
 *  (modules/cave_pc/src/hook/) -- see that folder's cave_hook.h for the
 *  rationale (one board's controller/JVS fix must never again silently
 *  regress a different board sharing the same compiled hook).
 *
 *  This is a CLEAN-ROOM implementation of the JAMMA Video Standard (JVS) I/O
 *  protocol, written from the published open specification -- it contains no
 *  code from any other arcade loader project. It models a single
 *  JVS I/O node (the board the arcade game expects to find on its I/O bus) and
 *  answers the host game's command stream with correctly framed responses,
 *  sourced from a plain input struct the loader fills from the player's
 *  controller. Logic (the JVS wire format + command dispatch) is carried over
 *  from this project's own proven native_x86/src/hook/jvs_core.c per the
 *  project owner's explicit instruction to read that file as REFERENCE ONLY
 *  and copy the logic into this board's own file -- every symbol here is
 *  renamed (ttz_ prefix) so this translation unit has zero link-time overlap
 *  with that file even if both were ever (accidentally) pulled into the same
 *  build.
 *
 *  Transport: JVS-over-serial (RS-485 on a COM port), the same transport this
 *  project already proved out on Taito Type X2 (see jvs_serial_hook_typexzero.c
 *  in this same folder -- the project owner's explicit instruction for this
 *  board is "JVS-over-serial like Type X2"). Note for the record: Taito's own
 *  public documentation for the real Type X Zero board describes its I/O as
 *  JVS-over-USB (see docs/X86_BOARDS.md), but graphics-and-I/O-wise this
 *  loader treats Type Zero as architecturally identical to Type X2 (same
 *  D3D9 graphics path, "already working" per docs/X86_BOARDS.md line 124) --
 *  so the serial transport is what this independent build implements, matching
 *  the explicit board spec given for this rebuild pass.
 *
 *  The engine is pure logic with no OS dependencies, so it is fully
 *  unit-testable on its own.
 * =============================================================================
 */
#ifndef ULTRA_TTZ_JVS_CORE_H
#define ULTRA_TTZ_JVS_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- JVS function (feature) limits this node advertises ------------------- */
#define TTZ_JVS_MAX_PLAYERS   2
#define TTZ_JVS_MAX_ANALOG    16

/* The shared jvs_core code was cloned into every board and the macro was
 * renamed per board - but the pasted bodies kept using the bare name, so
 * this board has not compiled since that rollout. Alias rather than
 * rewrite the call sites, so the board macro stays the single source of
 * truth for the array size. */
#ifndef JVS_MAX_ANALOG
#define JVS_MAX_ANALOG TTZ_JVS_MAX_ANALOG
#endif
#define TTZ_JVS_MAX_COINS     2

/* ---- Input state the loader pushes in; the engine reports it on demand ----
 * Bit layouts follow the JVS switch map so the loader fills them directly.
 *
 *  system  : bit7 TEST, bit6 TILT1, bit5 TILT2, bit4 TILT3            (active high)
 *  player[]: bit15 START  bit14 SERVICE bit13 UP    bit12 DOWN
 *            bit11 LEFT   bit10 RIGHT   bit9  PUSH1 bit8  PUSH2
 *            bit7  PUSH3  bit6  PUSH4   bit5  PUSH5 bit4  PUSH6
 *            bit3  PUSH7  bit2  PUSH8   (low bits reserved)
 *  analog[]: 16-bit unsigned, left-justified (0x8000 = center for a centered
 *            axis; 0x0000 / 0xFFFF = extremes).
 *  screen_x/y: touch/gun position, 16-bit, same left-justified convention
 *              (Type Zero hosts touch-rhythm titles too, e.g. Groove Coaster;
 *              this field is wired for that even though touch itself is not
 *              yet implemented in this independent build -- see
 *              hook_dllmain_typexzero.c's header comment).
 */
typedef struct ttz_jvs_input {
    uint8_t  system;
    uint16_t player[TTZ_JVS_MAX_PLAYERS];
    uint16_t analog[TTZ_JVS_MAX_ANALOG];
    uint16_t screen_x, screen_y;
} ttz_jvs_input;

/* Opaque node. */
typedef struct ttz_jvs_node ttz_jvs_node;

/* ---- Lifecycle ----------------------------------------------------------- */
ttz_jvs_node* ttz_jvs_create(void);
void          ttz_jvs_destroy(ttz_jvs_node* n);

/* ---- Configuration (call before/while running; all optional) -------------- */
void ttz_jvs_set_id(ttz_jvs_node* n, const char* id);
void ttz_jvs_set_features(ttz_jvs_node* n,
                          uint8_t players, uint8_t switches_per_player,
                          uint8_t coin_slots, uint8_t analog_channels,
                          uint8_t analog_bits, uint8_t gpo_slots,
                          uint8_t has_screen_pos);

/* ---- Live input ---------------------------------------------------------- */
void ttz_jvs_set_input(ttz_jvs_node* n, const ttz_jvs_input* in);
void ttz_jvs_get_input(const ttz_jvs_node* n, ttz_jvs_input* out);
void ttz_jvs_insert_coin(ttz_jvs_node* n, int slot, int count);

/* ---- The transport entry point ------------------------------------------- *
 * Feed raw host->node bytes (a partial or whole stream; the engine buffers
 * until a full packet is present). Any node->host response bytes (already
 * framed + escaped, ready to hand back to the game) are written to `out` up to
 * `out_cap`. Returns the number of response bytes produced (0 if the stream so
 * far holds no complete, addressed-to-us packet that warrants a reply). */
size_t ttz_jvs_host_bytes(ttz_jvs_node* n,
                          const uint8_t* in, size_t in_len,
                          uint8_t* out, size_t out_cap);

/* ---- Introspection --------------------------------------------------------- */
uint8_t  ttz_jvs_address(const ttz_jvs_node* n);
uint32_t ttz_jvs_coin_count(const ttz_jvs_node* n, int slot);

/* ---- Protocol constants ----------------------------------------------------- */
#define TTZ_JVS_SYNC        0xE0
#define TTZ_JVS_ESCAPE      0xD0
#define TTZ_JVS_BROADCAST   0xFF
#define TTZ_JVS_HOST_ADDR   0x00

#define TTZ_JVS_CMD_RESET        0xF0   /* + 0xD9 */
#define TTZ_JVS_CMD_SET_ADDR     0xF1   /* + addr */
#define TTZ_JVS_CMD_SET_COMMS    0xF2
#define TTZ_JVS_CMD_IOIDENT      0x10
#define TTZ_JVS_CMD_CMD_REV      0x11
#define TTZ_JVS_CMD_JVS_REV      0x12
#define TTZ_JVS_CMD_COMM_VER     0x13
#define TTZ_JVS_CMD_FEATCHK      0x14
#define TTZ_JVS_CMD_MAIN_ID      0x15
#define TTZ_JVS_CMD_READ_SWITCH  0x20
#define TTZ_JVS_CMD_READ_COIN    0x21
#define TTZ_JVS_CMD_READ_ANALOG  0x22
#define TTZ_JVS_CMD_READ_ROTARY  0x23
#define TTZ_JVS_CMD_READ_KEYCODE 0x24
#define TTZ_JVS_CMD_READ_SCRPOS  0x25
#define TTZ_JVS_CMD_READ_MISC    0x26
#define TTZ_JVS_CMD_RETRANSMIT   0x2F
#define TTZ_JVS_CMD_COIN_DEC     0x30
#define TTZ_JVS_CMD_GPO          0x32
#define TTZ_JVS_CMD_ANALOG_OUT   0x33
#define TTZ_JVS_CMD_CHAR_OUT     0x34
#define TTZ_JVS_CMD_COIN_INC     0x35
#define TTZ_JVS_CMD_GPO_BYTE     0x38

#define TTZ_JVS_STATUS_NORMAL    0x01
#define TTZ_JVS_STATUS_UNKNOWN   0x02
#define TTZ_JVS_STATUS_SUM_ERR   0x03
#define TTZ_JVS_STATUS_OVERFLOW  0x04
#define TTZ_JVS_REPORT_NORMAL    0x01
#define TTZ_JVS_REPORT_PARAM_ERR 0x02
#define TTZ_JVS_REPORT_DATA_ERR  0x03
#define TTZ_JVS_REPORT_BUSY      0x04

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* ULTRA_TTZ_JVS_CORE_H */
